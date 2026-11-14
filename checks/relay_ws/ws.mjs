// A very small WebSocket (RFC 6455) server side, so that the relay has no dependencies at all.
// It does what the relay needs and nothing else: the opening handshake, binary messages in both directions, ping/pong, close.
// No extensions (so no compression), no sub-protocols, text messages are not accepted.
import { createHash } from 'node:crypto';
import { EventEmitter } from 'node:events';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const MAX_BACKLOG = 1 << 20;   // bytes waiting to be sent to one slow peer; beyond that datagrams are dropped (the game protocol copes with loss)

export class WsConnection extends EventEmitter {
	constructor(socket, maxMessage) {
		super();
		this.socket = socket;
		this.maxMessage = maxMessage;
		this.buffer = Buffer.alloc(0);
		this.fragments = [];
		this.fragmentBytes = 0;
		this.closed = false;
		this.closeSent = false;
		this.lastActivity = Date.now();
		socket.setNoDelay(true);       // no Nagle delay: a game packet must leave at once
		socket.on('data', (chunk) => this.#onData(chunk));
		socket.on('close', () => this.#finish());
		socket.on('error', () => { /* followed by 'close' */ });
	}

	#finish() {
		if (this.closed) return;
		this.closed = true;
		this.emit('close');
	}

	#frame(opcode, payload) {
		const length = payload.length;
		let head;
		if (length < 126) { head = Buffer.alloc(2); head[1] = length; }
		else if (length <= 0xffff) { head = Buffer.alloc(4); head[1] = 126; head.writeUInt16BE(length, 2); }
		else { head = Buffer.alloc(10); head[1] = 127; head.writeBigUInt64BE(BigInt(length), 2); }
		head[0] = 0x80 | opcode;
		return Buffer.concat([head, payload]);
	}

	/** Sends one binary message. Returns false if it was dropped. */
	send(data) {
		if (this.closed || this.closeSent || this.socket.destroyed) return false;
		if (this.socket.writableLength > MAX_BACKLOG) return false;
		this.socket.write(this.#frame(0x2, data));
		return true;
	}

	ping() {
		if (!this.closed && !this.closeSent && !this.socket.destroyed) this.socket.write(this.#frame(0x9, Buffer.alloc(0)));
	}

	close(code = 1000) {
		if (this.closed || this.closeSent) return;
		this.closeSent = true;
		const payload = Buffer.alloc(2);
		payload.writeUInt16BE(code, 0);
		this.socket.write(this.#frame(0x8, payload), () => this.socket.end());
		setTimeout(() => this.socket.destroy(), 1000).unref();
	}

	terminate() { this.socket.destroy(); }

	#fail(code) { this.buffer = Buffer.alloc(0); this.close(code); }

	#onData(chunk) {
		this.lastActivity = Date.now();
		this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk;
		while (!this.closed && !this.closeSent) {
			const b = this.buffer;
			if (b.length < 2) return;
			const fin = (b[0] & 0x80) !== 0;
			const opcode = b[0] & 0x0f;
			const masked = (b[1] & 0x80) !== 0;
			let length = b[1] & 0x7f;
			let offset = 2;
			if ((b[0] & 0x70) !== 0 || !masked) { this.#fail(1002); return; }   // reserved bits (no extensions) / a client must mask
			if (length === 126) { if (b.length < 4) return; length = b.readUInt16BE(2); offset = 4; }
			else if (length === 127) {
				if (b.length < 10) return;
				const big = b.readBigUInt64BE(2);
				if (big > BigInt(this.maxMessage)) { this.#fail(1009); return; }
				length = Number(big); offset = 10;
			}
			if (length > this.maxMessage) { this.#fail(1009); return; }
			if (b.length < offset + 4 + length) return;
			const mask = b.subarray(offset, offset + 4);
			const payload = Buffer.from(b.subarray(offset + 4, offset + 4 + length));
			for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i & 3];
			this.buffer = b.subarray(offset + 4 + length);

			switch (opcode) {
				case 0x0: case 0x2: {    // continuation / binary
					if (opcode === 0x2 && this.fragments.length) { this.#fail(1002); return; }
					if (opcode === 0x0 && !this.fragments.length) { this.#fail(1002); return; }
					this.fragments.push(payload);
					this.fragmentBytes += payload.length;
					if (this.fragmentBytes > this.maxMessage) { this.#fail(1009); return; }
					if (fin) {
						const message = (this.fragments.length === 1) ? this.fragments[0] : Buffer.concat(this.fragments);
						this.fragments = []; this.fragmentBytes = 0;
						this.emit('message', message);
					}
					break;
				}
				case 0x1: this.#fail(1003); return;   // text is not part of the protocol
				case 0x8: this.close(1000); return;
				case 0x9: if (!this.socket.destroyed) this.socket.write(this.#frame(0xA, payload)); break;
				case 0xA: break;
				default: this.#fail(1002); return;
			}
		}
	}
}

/** Completes the handshake on an 'upgrade' event. Returns a WsConnection, or null after answering with an HTTP error. */
export function acceptUpgrade(request, socket, maxMessage) {
	const key = request.headers['sec-websocket-key'];
	const bad = (status, text) => { socket.end(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`); return null; };
	if (String(request.headers.upgrade ?? '').toLowerCase() !== 'websocket') return bad(400, 'Bad Request');
	if (request.headers['sec-websocket-version'] !== '13') return bad(426, 'Upgrade Required');
	if (typeof key !== 'string' || Buffer.from(key, 'base64').length !== 16) return bad(400, 'Bad Request');
	const accept = createHash('sha1').update(key + GUID).digest('base64');
	socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
	return new WsConnection(socket, maxMessage);
}
