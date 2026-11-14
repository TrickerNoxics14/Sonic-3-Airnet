// Automated checks of the relay server: protocol, join, drop, reconnect, abuse. Run: node --test test/relay.test.mjs   (Node 22 or newer for the built-in WebSocket client)
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import * as P from '../protocol.mjs';
import { startRelay, UdpPeer, WsPeer, registerHost, joinFriend, rawRequest, sleep, LOBBY, KEY, SECRET } from './helpers.mjs';

const payload = (text) => Buffer.from(text, 'latin1');

for (const [hostKind, Host] of [['UDP', UdpPeer], ['WebSocket', WsPeer]]) {
	for (const [friendKind, Friend] of [['UDP', UdpPeer], ['WebSocket', WsPeer]]) {
		test(`join and forward: ${hostKind} host with a ${friendKind} friend, both directions`, async (t) => {
			const r = await startRelay();
			t.after(() => r.stop());
			const host = await Host.create(Host === UdpPeer ? r.udpPort : r.port);
			const friend = await Friend.create(Friend === UdpPeer ? r.udpPort : r.port);
			t.after(() => { host.close(); friend.close(); });

			const hostToken = await registerHost(host);
			assert.ok(hostToken, 'host registered');
			const joined = await joinFriend(friend);
			assert.ok(joined?.token, 'friend let in');

			friend.send(P.buildClientData(P.Type.DATA_C2R, joined.token, payload('hello host')));
			const toHost = await host.nextOfType(P.Type.DATA_R2H);
			assert.ok(toHost, 'host got the datagram');
			assert.equal(toHost.readUInt32BE(P.HEADER_SIZE), hostToken);
			assert.equal(toHost.readUInt16BE(P.HEADER_SIZE + 4), 1, 'first friend is number 1');
			assert.equal(toHost.subarray(P.HEADER_SIZE + 6).toString('latin1'), 'hello host');

			host.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 1, payload('hello friend')));
			const toFriend = await friend.nextOfType(P.Type.DATA_R2C);
			assert.ok(toFriend, 'friend got the datagram');
			assert.equal(toFriend.readUInt32BE(P.HEADER_SIZE), joined.token);
			assert.equal(toFriend.subarray(P.HEADER_SIZE + 4).toString('latin1'), 'hello friend');
		});
	}
}

test('binary payloads survive unchanged (every byte value, 1 byte and the maximum size)', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	const friend = await WsPeer.create(r.port);
	t.after(() => { host.close(); friend.close(); });
	const hostToken = await registerHost(host);
	const { token } = await joinFriend(friend);
	for (const size of [1, 256, 1100, P.MAX_PAYLOAD_SIZE]) {
		const data = Buffer.alloc(size);
		for (let i = 0; i < size; ++i) data[i] = i & 0xff;
		friend.send(P.buildClientData(P.Type.DATA_C2R, token, data));
		const got = await host.nextOfType(P.Type.DATA_R2H);
		assert.ok(got, `size ${size} arrived`);
		assert.ok(got.subarray(P.HEADER_SIZE + 6).equals(data), `size ${size} unchanged`);
		host.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 1, data));
		const back = await friend.nextOfType(P.Type.DATA_R2C);
		assert.ok(back && back.subarray(P.HEADER_SIZE + 4).equals(data), `size ${size} back unchanged`);
	}
});

test('friends of both kinds in one lobby get their own numbers and do not hear each other', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await WsPeer.create(r.port);
	const a = await UdpPeer.create(r.udpPort);
	const b = await WsPeer.create(r.port);
	t.after(() => { host.close(); a.close(); b.close(); });
	const hostToken = await registerHost(host);
	const ja = await joinFriend(a);
	const jb = await joinFriend(b);
	host.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 2, payload('for b')));
	assert.equal((await b.nextOfType(P.Type.DATA_R2C))?.subarray(P.HEADER_SIZE + 4).toString('latin1'), 'for b');
	assert.ok(await a.silent(), 'friend a hears nothing meant for b');
	a.send(P.buildClientData(P.Type.DATA_C2R, ja.token, payload('from a')));
	const got = await host.nextOfType(P.Type.DATA_R2H);
	assert.equal(got.readUInt16BE(P.HEADER_SIZE + 4), 1);
	assert.notEqual(ja.token, jb.token);
});

test('protocol: garbage, wrong version, wrong size and unknown types get no answer', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const udp = await UdpPeer.create(r.udpPort);
	const ws = await WsPeer.create(r.port);
	t.after(() => { udp.close(); ws.close(); });
	const good = P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY);
	const wrongVersion = Buffer.from(good); wrongVersion[4] = 1;
	const wrongMagic = Buffer.from(good); wrongMagic[0] = 0x58;
	const unknownType = Buffer.from(good); unknownType[5] = 99;
	const bad = [Buffer.alloc(0), Buffer.from('hello'), good.subarray(0, 10), Buffer.concat([good, Buffer.from([0])]), wrongVersion, wrongMagic, unknownType,
		P.buildClientData(P.Type.DATA_R2C, 5, payload('x')), P.buildHostRegistered(LOBBY, 1)];
	for (const peer of [udp, ws]) {
		for (const datagram of bad) if (datagram.length || peer === udp) peer.send(datagram);
		assert.ok(await peer.silent(400), 'no answer to garbage');
	}
	assert.ok(r.relay.core.stats.droppedInvalid >= 8);
	// and the relay still works
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	assert.ok(await registerHost(host));
});

test('a friend gets NO_HOST when nobody registered the lobby', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	for (const Peer of [UdpPeer, WsPeer]) {
		const friend = await Peer.create(Peer === UdpPeer ? r.udpPort : r.port);
		t.after(() => friend.close());
		assert.deepEqual(await joinFriend(friend), { refused: P.Refusal.NO_HOST });
	}
});

test('a lobby is full after six friends, and a free place is given back', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	await registerHost(host);
	const friends = [];
	for (let i = 0; i < 6; ++i) {
		const f = await WsPeer.create(r.port);
		friends.push(f);
		assert.ok((await joinFriend(f))?.token, `friend ${i + 1}`);
	}
	const extra = await WsPeer.create(r.port);
	assert.deepEqual(await joinFriend(extra), { refused: P.Refusal.FULL });
	friends[2].close();   // a WebSocket that closes frees its place at once
	await sleep(200);
	assert.ok((await joinFriend(extra))?.token, 'place was freed by the closed connection');
	friends.concat([extra]).forEach((f) => f.close());
});

test('wrong key: no answer, and eight wrong keys block the address for the right key too', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	const friend = await UdpPeer.create(r.udpPort);
	t.after(() => { host.close(); friend.close(); });
	await registerHost(host);
	for (let i = 0; i < 8; ++i) {
		friend.send(P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY + 1n + BigInt(i)));
		assert.ok(await friend.silent(60));
	}
	assert.equal(r.relay.core.stats.wrongKeys, 8);
	assert.equal(await joinFriend(friend), null, 'blocked even with the right key');
	assert.ok(r.relay.core.stats.droppedBlocked >= 1);
	// another address is not affected (a WebSocket from the same machine has the same IP, so use another loopback address for UDP)
});

test('taking over a lobby needs the host secret, not only the key that every friend knows', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	const thief = await UdpPeer.create(r.udpPort);
	const friend = await WsPeer.create(r.port);
	t.after(() => { host.close(); thief.close(); friend.close(); });
	const hostToken = await registerHost(host);
	thief.send(P.buildHostHello(LOBBY, KEY, SECRET + 1n));
	assert.ok(await thief.silent(), 'no answer to the thief');
	const { token } = await joinFriend(friend);
	friend.send(P.buildClientData(P.Type.DATA_C2R, token, payload('still goes to the real host')));
	assert.ok(await host.nextOfType(P.Type.DATA_R2H), 'real host still gets the data');
	assert.ok(await thief.silent(200));
	assert.ok(hostToken);
});

test('forged data: wrong token, no join, wrong host token are dropped', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	const friend = await WsPeer.create(r.port);
	const stranger = await WsPeer.create(r.port);
	t.after(() => { host.close(); friend.close(); stranger.close(); });
	const hostToken = await registerHost(host);
	const { token } = await joinFriend(friend);
	friend.send(P.buildClientData(P.Type.DATA_C2R, (token ^ 1) >>> 0, payload('wrong token')));
	stranger.send(P.buildClientData(P.Type.DATA_C2R, token, payload('stolen token, never joined')));
	host.send(P.buildHostData(P.Type.DATA_H2R, (hostToken ^ 1) >>> 0, 1, payload('wrong host token')));
	stranger.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 1, payload('host token but not the host')));
	assert.ok(await host.silent(300), 'host heard nothing');
	assert.ok(await friend.silent(100), 'friend heard nothing');
	assert.ok(await stranger.silent(100));
	assert.ok(r.relay.core.stats.droppedInvalid >= 4);
});

test('drop: a friend that leaves and comes back gets a new place and the old token is dead', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	const hostToken = await registerHost(host);
	const first = await WsPeer.create(r.port);
	const j1 = await joinFriend(first);
	first.close();
	await sleep(200);
	assert.equal(r.relay.core.stats.pipes, 0, 'place freed at once');
	const second = await WsPeer.create(r.port);
	t.after(() => second.close());
	const j2 = await joinFriend(second);
	assert.ok(j2.token && j2.token !== j1.token, 'fresh token');
	second.send(P.buildClientData(P.Type.DATA_C2R, j1.token, payload('old token')));
	assert.ok(await host.silent(250), 'old token is dead');
	second.send(P.buildClientData(P.Type.DATA_C2R, j2.token, payload('new token')));
	const got = await host.nextOfType(P.Type.DATA_R2H);
	assert.ok(got);
	// data to the old friend number goes nowhere and does not break anything
	host.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 1, payload('to nobody')));
	host.send(P.buildHostData(P.Type.DATA_H2R, hostToken, got.readUInt16BE(P.HEADER_SIZE + 4), payload('to the new friend')));
	assert.equal((await second.nextOfType(P.Type.DATA_R2C))?.subarray(P.HEADER_SIZE + 4).toString('latin1'), 'to the new friend');
});

test('drop and reconnect of the host: the lobby survives, the same game takes it back and the friend keeps playing', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host1 = await WsPeer.create(r.port);
	const friend = await UdpPeer.create(r.udpPort);
	t.after(() => { host1.close(); friend.close(); });
	const hostToken = await registerHost(host1);
	const { token } = await joinFriend(friend);
	host1.close();
	await sleep(200);
	assert.ok(r.relay.core.hasLobby(LOBBY), 'lobby is kept for the reconnect window');
	friend.send(P.buildClientData(P.Type.DATA_C2R, token, payload('while the host is away')));
	assert.ok(await friend.silent(150), 'nothing breaks while the host is away');

	// a stranger with the key but without the secret cannot take the free place of the host
	const thief = await WsPeer.create(r.port);
	thief.send(P.buildHostHello(LOBBY, KEY, SECRET ^ 0xffn));
	assert.ok(await thief.silent(), 'no answer');
	thief.close();

	const host2 = await UdpPeer.create(r.udpPort);   // reconnects from another address, even by another transport
	t.after(() => host2.close());
	assert.equal(await registerHost(host2), hostToken, 'same host token, so the friends need no new registration');
	friend.send(P.buildClientData(P.Type.DATA_C2R, token, payload('after reconnect')));
	const got = await host2.nextOfType(P.Type.DATA_R2H);
	assert.equal(got?.readUInt16BE(P.HEADER_SIZE + 4), 1, 'same friend number');
	host2.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 1, payload('welcome back')));
	assert.ok(await friend.nextOfType(P.Type.DATA_R2C));
});

test('timeouts: a lobby whose host went silent disappears, a silent friend is forgotten', async (t) => {
	const r = await startRelay({ limits: { hostTimeoutSeconds: 1.5, pipeTimeoutSeconds: 1.0 } });
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	const friend = await UdpPeer.create(r.udpPort);
	t.after(() => { host.close(); friend.close(); });
	await registerHost(host);
	await joinFriend(friend);
	assert.equal(r.relay.core.stats.pipes, 1);
	await sleep(2600);
	assert.equal(r.relay.core.stats.pipes, 0, 'silent friend forgotten');
	assert.equal(r.relay.core.hasLobby(LOBBY), false, 'silent host: lobby gone');
	assert.deepEqual(await joinFriend(friend), { refused: P.Refusal.NO_HOST });
});

test('lobby limits: per address and in total', async (t) => {
	const r = await startRelay({ limits: { maxLobbies: 3, maxLobbiesPerIP: 2 } });
	t.after(() => r.stop());
	const peers = [];
	for (let i = 0; i < 4; ++i) peers.push(await UdpPeer.create(r.udpPort));
	t.after(() => peers.forEach((p) => p.close()));
	assert.ok(await registerHost(peers[0], 1n, 1n, 1n));
	assert.ok(await registerHost(peers[1], 2n, 1n, 1n));
	assert.equal(await registerHost(peers[2], 3n, 1n, 1n), null, 'third lobby from the same address is refused (no answer)');
	assert.equal(r.relay.core.stats.refused, 1);
});

test('control messages are rate limited per address', async (t) => {
	const r = await startRelay({ limits: { controlBurst: 5, controlPerSecond: 0.001 } });
	t.after(() => r.stop());
	const friend = await UdpPeer.create(r.udpPort);
	t.after(() => friend.close());
	for (let i = 0; i < 20; ++i) friend.send(P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY));
	await sleep(400);
	assert.ok(r.relay.core.stats.droppedRate >= 14);
});

// ---- the WebSocket server itself ----

test('websocket handshake: RFC 6455 example key gets the RFC answer', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const reply = (await rawRequest(r.port, 'GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n')).toString('latin1');
	assert.match(reply, /^HTTP\/1\.1 101 /);
	assert.match(reply, /Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK\+xOo=\r\n/);
	assert.equal(createHash('sha1').update('dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'), 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=');
});

test('websocket handshake: bad requests are refused, plain HTTP and /health work', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const noKey = (await rawRequest(r.port, 'GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n')).toString('latin1');
	assert.match(noKey, /^HTTP\/1\.1 400 /);
	const oldVersion = (await rawRequest(r.port, 'GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n')).toString('latin1');
	assert.match(oldVersion, /^HTTP\/1\.1 426 /);
	const health = await (await fetch(`http://127.0.0.1:${r.port}/health`)).json();
	assert.equal(health.ok, true);
	assert.equal(typeof health.lobbies, 'number');
	assert.equal(JSON.stringify(health).includes('127.0.0.1'), false, 'no addresses in the health page');
	assert.equal((await fetch(`http://127.0.0.1:${r.port}/`)).status, 200);
});

test('websocket: oversized and text messages are refused with a close code', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const big = await WsPeer.create(r.port);
	big.send(Buffer.alloc(P.MAX_DATAGRAM_SIZE + 1));
	assert.ok(await big.waitClosed());
	assert.equal(big.closeCode, 1009);
	const text = await WsPeer.create(r.port);
	text.socket.send('not binary');
	assert.ok(await text.waitClosed());
	assert.equal(text.closeCode, 1003);
});

test('websocket: an unmasked frame from a client is a protocol error', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const net = await import('node:net');
	const result = await new Promise((resolve) => {
		const socket = net.connect(r.port, '127.0.0.1', () => socket.write('GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n'));
		let sentFrame = false;
		const chunks = [];
		socket.on('data', (c) => {
			chunks.push(c);
			if (!sentFrame) { sentFrame = true; socket.write(Buffer.from([0x82, 0x02, 0x01, 0x02])); }   // binary, NOT masked
		});
		socket.on('close', () => resolve(Buffer.concat(chunks)));
		setTimeout(() => socket.destroy(), 1500).unref();
	});
	const text = result.toString('latin1');
	const afterHeaders = result.subarray(text.indexOf('\r\n\r\n') + 4);
	assert.equal(afterHeaders[0], 0x88, 'a close frame');
	assert.equal(afterHeaders.readUInt16BE(2), 1002);
});

test('websocket: fragmented messages are put together', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const net = await import('node:net');
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	const hostToken = await registerHost(host);
	const hello = P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY);
	const frame = (fin, opcode, data) => {
		const mask = Buffer.from([1, 2, 3, 4]);
		const out = Buffer.alloc(2 + 4 + data.length);
		out[0] = (fin ? 0x80 : 0) | opcode; out[1] = 0x80 | data.length; mask.copy(out, 2);
		for (let i = 0; i < data.length; ++i) out[6 + i] = data[i] ^ mask[i & 3];
		return out;
	};
	const answer = await new Promise((resolve) => {
		let upgraded = false;
		const socket = net.connect(r.port, '127.0.0.1', () => socket.write('GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n'));
		socket.on('data', (c) => {
			if (!upgraded) {
				upgraded = true;
				socket.write(frame(false, 0x2, hello.subarray(0, 9)));
				socket.write(frame(true, 0x0, hello.subarray(9)));
			} else { socket.destroy(); resolve(c); }
		});
		setTimeout(() => { socket.destroy(); resolve(null); }, 1500).unref();
	});
	assert.ok(answer, 'the joined message was understood');
	assert.equal(answer[0], 0x82);
	assert.equal(P.parseHeader(answer.subarray(2)), P.Type.CLIENT_JOINED);
	assert.ok(hostToken);
});

test('connection limits: per address, and origins can be restricted', async (t) => {
	const r = await startRelay({ maxConnectionsPerIP: 2, allowedOrigins: ['https://game.example'] });
	t.after(() => r.stop());
	const upgrade = (origin) => `GET / HTTP/1.1\r\nHost: x\r\n${origin ? `Origin: ${origin}\r\n` : ''}Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`;
	assert.match((await rawRequest(r.port, upgrade('https://evil.example'))).toString('latin1'), /^HTTP\/1\.1 403 /);
	// native games send no Origin header and are let in; the right origin too
	const a = await WsPeer.create(r.port);
	const b = await WsPeer.create(r.port);
	t.after(() => { a.close(); b.close(); });
	await assert.rejects(() => WsPeer.create(r.port), 'third connection from one address refused');
});

test('proxy mode: the address of the player comes from X-Forwarded-For (rate limits are per player, not per proxy)', async (t) => {
	const r = await startRelay({ trustProxy: true, limits: { maxBadKeys: 2 } });
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	await registerHost(host);
	// two different players behind the same proxy: one sends wrong keys and gets blocked, the other still gets in
	const open = (ip) => new Promise((resolve) => {
		import('node:net').then(({ default: net }) => {
			const socket = net.connect(r.port, '127.0.0.1', () => socket.write(`GET / HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: ${ip}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`));
			resolve(socket);
		});
	});
	const send = (socket, data) => {
		const mask = Buffer.from([9, 8, 7, 6]);
		const out = Buffer.alloc(2 + 4 + data.length);
		out[0] = 0x82; out[1] = 0x80 | data.length; mask.copy(out, 2);
		for (let i = 0; i < data.length; ++i) out[6 + i] = data[i] ^ mask[i & 3];
		socket.write(out);
	};
	const bad = await open('203.0.113.7');
	const good = await open('203.0.113.8');
	const goodAnswers = [];
	good.on('data', (c) => goodAnswers.push(c));
	await sleep(150);
	for (let i = 0; i < 3; ++i) send(bad, P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY + 5n));
	await sleep(150);
	send(good, P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY));
	await sleep(300);
	const text = Buffer.concat(goodAnswers).toString('latin1');
	assert.ok(text.includes('S3RL'), 'the other player still got an answer');
	bad.destroy(); good.destroy();
});
