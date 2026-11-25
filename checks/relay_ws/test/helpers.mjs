import dgram from 'node:dgram';
import net from 'node:net';
import { createRelayServer } from '../server.mjs';
import * as P from '../protocol.mjs';

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export async function startRelay(options = {}) {
	const relay = createRelayServer({ port: 0, udpPort: 0, bind: '127.0.0.1', ...options });
	const ports = await relay.start();
	return { relay, ...ports, async stop() { await relay.stop(); } };
}

/** A peer that talks to the relay: same interface for UDP and WebSocket. */
class Peer {
	constructor() { this.queue = []; this.waiters = []; }
	push(buffer) {
		const waiter = this.waiters.shift();
		if (waiter) waiter(buffer); else this.queue.push(buffer);
	}
	/** Next datagram from the relay, or null if none arrives within `timeout` ms. */
	next(timeout = 1500) {
		if (this.queue.length) return Promise.resolve(this.queue.shift());
		return new Promise((resolve) => {
			const waiter = (buffer) => { clearTimeout(timer); resolve(buffer); };
			const timer = setTimeout(() => { this.waiters = this.waiters.filter((w) => w !== waiter); resolve(null); }, timeout);
			this.waiters.push(waiter);
		});
	}
	/** Asserts-by-return: true if nothing arrives within `timeout` ms. */
	async silent(timeout = 300) { return (await this.next(timeout)) === null; }
	async nextOfType(type, timeout = 1500) {
		const buffer = await this.next(timeout);
		return (buffer && P.parseHeader(buffer) === type) ? buffer : null;
	}
}

export class UdpPeer extends Peer {
	static async create(port) {
		const peer = new UdpPeer();
		peer.port = port;
		peer.socket = dgram.createSocket('udp4');
		peer.socket.on('message', (data) => peer.push(Buffer.from(data)));
		await new Promise((resolve) => peer.socket.bind(0, '127.0.0.1', resolve));
		return peer;
	}
	send(buffer) { this.socket.send(buffer, this.port, '127.0.0.1'); }
	close() { try { this.socket.close(); } catch { /* closed */ } }
}

export class WsPeer extends Peer {
	static async create(port) {
		const peer = new WsPeer();
		peer.closedEvent = false;
		peer.socket = new WebSocket(`ws://127.0.0.1:${port}/`);
		peer.socket.binaryType = 'arraybuffer';
		peer.socket.onmessage = (event) => peer.push(Buffer.from(event.data));
		peer.socket.onclose = (event) => { peer.closedEvent = true; peer.closeCode = event.code; };
		await new Promise((resolve, reject) => { peer.socket.onopen = resolve; peer.socket.onerror = () => reject(new Error('websocket error')); });
		return peer;
	}
	send(buffer) { this.socket.send(buffer); }
	close() { try { this.socket.close(); } catch { /* closed */ } }
	async waitClosed(timeout = 1500) {
		const end = Date.now() + timeout;
		while (!this.closedEvent && Date.now() < end) await sleep(20);
		return this.closedEvent;
	}
}

/**
 * A WebSocket client written on a raw TCP socket, so that a test chooses the handshake headers itself (Origin, X-Forwarded-For): the
 * built-in WebSocket client of Node cannot send either. `status` is the HTTP status of the handshake (101 = let in, 0 = no answer).
 * `headers` is an object; an array value sends the header once per entry.
 */
export class RawWsPeer extends Peer {
	static async open(port, headers = {}, host = '127.0.0.1') {
		const peer = new RawWsPeer();
		peer.status = 0;
		peer.closedEvent = false;
		peer.pending = Buffer.alloc(0);
		peer.socket = net.connect(port, host);
		const all = { Host: 'relay.test', Upgrade: 'websocket', Connection: 'Upgrade', 'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==', 'Sec-WebSocket-Version': '13', ...headers };
		let text = 'GET / HTTP/1.1\r\n';
		for (const [name, value] of Object.entries(all)) for (const v of [].concat(value)) text += `${name}: ${v}\r\n`;
		peer.socket.on('connect', () => peer.socket.write(`${text}\r\n`));
		peer.socket.on('error', () => {});
		peer.socket.on('close', () => { peer.closedEvent = true; peer.onHead?.(); });
		await new Promise((resolve) => {
			peer.onHead = resolve;
			let head = Buffer.alloc(0);
			peer.socket.on('data', (chunk) => {
				if (peer.status) { peer.#onFrames(chunk); return; }
				head = Buffer.concat([head, chunk]);
				const end = head.indexOf('\r\n\r\n');
				if (end < 0) return;
				peer.status = Number(head.toString('latin1', 0, end).split(' ')[1]);
				peer.head = head.toString('latin1', 0, end);
				resolve();
				if (peer.status === 101) peer.#onFrames(head.subarray(end + 4));
			});
			setTimeout(resolve, 2000).unref();
		});
		return peer;
	}
	#onFrames(chunk) {
		this.pending = Buffer.concat([this.pending, chunk]);
		for (;;) {
			const b = this.pending;
			if (b.length < 2) return;
			let length = b[1] & 0x7f;
			let offset = 2;
			if (length === 126) { if (b.length < 4) return; length = b.readUInt16BE(2); offset = 4; }
			if (b.length < offset + length) return;
			const payload = Buffer.from(b.subarray(offset, offset + length));
			this.pending = b.subarray(offset + length);
			const opcode = b[0] & 0x0f;
			if (opcode === 0x2) this.push(payload);
			else if (opcode === 0x8) { this.closeCode = payload.length >= 2 ? payload.readUInt16BE(0) : 1005; }
			// a ping from the relay needs no answer in a test that lasts seconds
		}
	}
	send(data) {
		const mask = Buffer.from([5, 6, 7, 8]);
		const head = data.length < 126 ? Buffer.from([0x82, 0x80 | data.length]) : Buffer.from([0x82, 0x80 | 126, data.length >> 8, data.length & 0xff]);
		const body = Buffer.alloc(data.length);
		for (let i = 0; i < data.length; ++i) body[i] = data[i] ^ mask[i & 3];
		this.socket.write(Buffer.concat([head, mask, body]));
	}
	close() { try { this.socket.destroy(); } catch { /* closed */ } }
	/** Says goodbye the way a browser does (a close frame): the relay forgets the connection at once. close() just drops the TCP connection, which the relay notices at its next ping. */
	closeWithFrame() {
		const mask = Buffer.from([1, 2, 3, 4]);
		const code = Buffer.from([0x03 ^ mask[0], 0xe8 ^ mask[1]]);   // 1000
		this.socket.write(Buffer.concat([Buffer.from([0x88, 0x82]), mask, code]));
	}
	async waitClosed(timeout = 1500) {
		const end = Date.now() + timeout;
		while (!this.closedEvent && Date.now() < end) await sleep(20);
		return this.closedEvent;
	}
}

export const LOBBY = 0x1122334455667788n;
export const KEY = 0x0102030405060708n;
export const SECRET = 0xdeadbeefcafef00dn;

/** Host registration; returns the host token. */
export async function registerHost(peer, lobby = LOBBY, key = KEY, secret = SECRET) {
	peer.send(P.buildHostHello(lobby, key, secret));
	const reply = await peer.nextOfType(P.Type.HOST_REGISTERED);
	if (!reply) return null;
	return reply.readUInt32BE(P.HEADER_SIZE + 8);
}

/** Friend join; returns { token } or { refused: reason } or null (no answer). */
export async function joinFriend(peer, lobby = LOBBY, key = KEY) {
	peer.send(P.buildHello(P.Type.CLIENT_JOIN, lobby, key));
	const reply = await peer.next();
	if (!reply) return null;
	const type = P.parseHeader(reply);
	if (type === P.Type.CLIENT_JOINED) return { token: reply.readUInt32BE(P.HEADER_SIZE) };
	if (type === P.Type.CLIENT_REFUSED) return { refused: reply[P.HEADER_SIZE] };
	return null;
}

/** Raw TCP client for handshake level tests. */
export function rawRequest(port, text) {
	return new Promise((resolve) => {
		const socket = net.connect(port, '127.0.0.1', () => socket.write(text));
		const chunks = [];
		socket.on('data', (c) => chunks.push(c));
		socket.on('close', () => resolve(Buffer.concat(chunks)));
		socket.on('error', () => resolve(Buffer.concat(chunks)));
		setTimeout(() => socket.destroy(), 1500).unref();
	});
}
