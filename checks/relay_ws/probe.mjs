#!/usr/bin/env node
// A small command line tool to look at a lobby server, and to put a fake lobby into its list (for checks and for trying the game's lobby list by hand).
//   node probe.mjs 127.0.0.1:28842 list [compat]            prints the public lobbies as JSON
//   node probe.mjs 127.0.0.1:28842 resolve 123456            prints what the lobby code resolves to
//   node probe.mjs 127.0.0.1:28842 fake "Name" 60 [compat] [players] [max] [inGame] [host]
//                                                            registers a fake lobby for 60 seconds (as a game would) and keeps it listed; prints its code
// It talks UDP, like the Windows game. Nothing is written anywhere.
import dgram from 'node:dgram';
import { randomBytes } from 'node:crypto';
import * as P from './protocol.mjs';

const [address, command, ...args] = process.argv.slice(2);
if (!address || !command) {
	console.error('Usage: node probe.mjs host:port list|resolve|fake ...  (see the top of this file)');
	process.exit(2);
}
const [host, portText] = address.split(':');
const port = Number(portText || P.DEFAULT_PORT);
const socket = dgram.createSocket('udp4');
const inbox = [];
let waiter = null;
socket.on('message', (data) => { if (waiter) { const w = waiter; waiter = null; w(Buffer.from(data)); } else inbox.push(Buffer.from(data)); });
const send = (buffer) => socket.send(buffer, port, host);
const next = (timeout = 2000) => new Promise((resolve) => {
	if (inbox.length) { resolve(inbox.shift()); return; }
	const timer = setTimeout(() => { waiter = null; resolve(null); }, timeout);
	waiter = (b) => { clearTimeout(timer); resolve(b); };
});
const hello = () => P.dirMessage(P.DirType.HELLO, new P.Writer().u8(1).pad(P.HELLO_MIN_PAYLOAD - 1).done());

async function getCookie() {
	send(hello());
	const reply = await next();
	if (!reply || P.parseHeader(reply) !== P.DirType.HELLO_REPLY) throw new Error('the server does not answer (no lobby directory there, or it is down)');
	const r = new P.Reader(reply);
	r.u8(); r.u8(); r.u8(); r.u8(); r.u16();
	return r.u64();
}

try {
	if (command === 'list') {
		const compat = args[0] ?? '';
		const cookie = await getCookie();
		send(P.dirMessage(P.DirType.LIST, new P.Writer().u8(1).u64(cookie).u8(0).u8(0).str(compat).done()));
		const reply = await next();
		const r = new P.Reader(reply);
		r.u8(); const status = r.u8();
		const out = { status, total: 0, lobbies: [] };
		if (status === 0) {
			out.total = r.u16(); r.u8(); const count = r.u8();
			for (let i = 0; i < count; ++i) out.lobbies.push({ code: r.str(), name: r.str(), host: r.str(), compat: r.str(), players: r.u8(), max: r.u8(), flags: r.u8(), age: r.u16() });
		}
		console.log(JSON.stringify(out));
	} else if (command === 'resolve') {
		const cookie = await getCookie();
		send(P.dirMessage(P.DirType.RESOLVE, new P.Writer().u8(1).u64(cookie).str(args[0] ?? '').done()));
		const reply = await next();
		const r = new P.Reader(reply);
		r.u8(); const status = r.u8();
		console.log(JSON.stringify(status === 0 ? { status, sessionID: r.u64().toString(), players: r.u8(), max: r.u8(), flags: r.u8() } : { status }));
	} else if (command === 'fake') {
		const [name, seconds = '60', compat = 'fake0000', players = '1', max = '4', inGame = '0', hostName = 'Fake'] = args;
		const lobby = randomBytes(8).readBigUInt64BE() | 1n, key = randomBytes(8).readBigUInt64BE(), secret = randomBytes(8).readBigUInt64BE() | 1n;
		const publish = () => send(P.dirMessage(P.DirType.PUBLISH, new P.Writer().u8(1).u64(lobby).u64(key).u64(secret).u64(100000000000n + BigInt(Math.floor(Math.random() * 1e9))).u8(P.PublishFlags.PUBLIC | (inGame === '1' ? P.PublishFlags.IN_GAME : 0))
			.u8(Number(players)).u8(Number(max)).str(compat).str(name ?? 'Fake').str(hostName).done()));
		const register = () => send(P.buildHostHello(lobby, key, secret));
		register();
		await next();
		publish();
		const reply = await next();
		const r = new P.Reader(reply ?? Buffer.alloc(P.HEADER_SIZE));
		r.u8(); const status = r.u8(); const code = r.str();
		console.log(JSON.stringify({ status, code }));
		const end = Date.now() + Number(seconds) * 1000;
		while (Date.now() < end) {
			await new Promise((resolve) => setTimeout(resolve, 5000));
			register(); publish();
			while (inbox.length) inbox.shift();
		}
	} else {
		console.error('unknown command');
		process.exit(2);
	}
} catch (error) {
	console.error(String(error.message ?? error));
	process.exitCode = 1;
}
socket.close();
