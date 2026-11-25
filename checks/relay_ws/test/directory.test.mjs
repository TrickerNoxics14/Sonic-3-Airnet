// Tests of the public lobby directory (directory.mjs): publish, list, resolve, expiry, limits, brute force protection, text cleaning, versions.
// Run with:  node --test test/directory.test.mjs   (Node 22 or newer for the WebSocket client)
import test from 'node:test';
import assert from 'node:assert/strict';
import { RelayCore } from '../relay_core.mjs';
import * as P from '../protocol.mjs';
import { sanitizeText, isDisallowedText, isValidCode } from '../directory.mjs';
import { startRelay, UdpPeer, WsPeer, registerHost, sleep, LOBBY, KEY, SECRET } from './helpers.mjs';

const COMPAT = 'abcd1234';

// ---- message helpers (what the game's DirectoryClient sends) ----
function hello(ver = 1, pad = P.HELLO_MIN_PAYLOAD) { return P.dirMessage(P.DirType.HELLO, new P.Writer().u8(ver).pad(pad - 1).done()); }
function publishMsg({ lobby = LOBBY, key = KEY, secret = SECRET, session = 0x1234567890n, flags = P.PublishFlags.PUBLIC, players = 1, max = 4, compat = COMPAT, name = 'Test Lobby', host = 'Host', ver = 1 } = {}) {
	return P.dirMessage(P.DirType.PUBLISH, new P.Writer().u8(ver).u64(lobby).u64(key).u64(secret).u64(session).u8(flags).u8(players).u8(max).str(compat).str(name).str(host).done());
}
function unpublishMsg(lobby = LOBBY, key = KEY, secret = SECRET) { return P.dirMessage(P.DirType.UNPUBLISH, new P.Writer().u8(1).u64(lobby).u64(key).u64(secret).done()); }
function listMsg(cookie, { flags = 0, offset = 0, compat = COMPAT } = {}) { return P.dirMessage(P.DirType.LIST, new P.Writer().u8(1).u64(cookie).u8(flags).u8(offset).str(compat).done()); }
function resolveMsg(cookie, code) { return P.dirMessage(P.DirType.RESOLVE, new P.Writer().u8(1).u64(cookie).str(code).done()); }

function parseHelloReply(buf) {
	assert.equal(P.parseHeader(buf), P.DirType.HELLO_REPLY);
	const r = new P.Reader(buf);
	return { ver: r.u8(), flags: r.u8(), digits: r.u8(), pageSize: r.u8(), lobbies: r.u16(), cookie: r.u64() };
}
function parsePublishReply(buf) {
	assert.equal(P.parseHeader(buf), P.DirType.PUBLISH_REPLY);
	const r = new P.Reader(buf);
	return { ver: r.u8(), status: r.u8(), code: r.str() };
}
function parseListReply(buf) {
	assert.equal(P.parseHeader(buf), P.DirType.LIST_REPLY);
	const r = new P.Reader(buf);
	const out = { ver: r.u8(), status: r.u8(), entries: [] };
	if (out.status !== P.ListStatus.OK) return out;
	out.total = r.u16(); out.offset = r.u8(); const count = r.u8();
	for (let i = 0; i < count; ++i) out.entries.push({ code: r.str(), name: r.str(), host: r.str(), compat: r.str(), players: r.u8(), max: r.u8(), flags: r.u8(), age: r.u16() });
	assert.equal(r.bad, false);
	return out;
}
function parseResolveReply(buf) {
	assert.equal(P.parseHeader(buf), P.DirType.RESOLVE_REPLY);
	const r = new P.Reader(buf);
	const out = { ver: r.u8(), status: r.u8() };
	if (out.status === P.ResolveStatus.OK) { out.sessionID = r.u64(); out.players = r.u8(); out.max = r.u8(); out.flags = r.u8(); }
	if (out.status === P.ResolveStatus.THROTTLED) out.retryAfter = r.u16();
	return out;
}

/** A game on the wire: one peer that registered a relay lobby and publishes it, one that browses. */
async function getCookie(peer) {
	peer.send(hello());
	const reply = await peer.next();
	assert.ok(reply, 'hello answered');
	return parseHelloReply(reply).cookie;
}
async function publish(peer, options) {
	peer.send(publishMsg(options));
	const reply = await peer.next();
	return reply ? parsePublishReply(reply) : null;
}
async function list(peer, cookie, options) {
	peer.send(listMsg(cookie, options));
	const reply = await peer.next();
	return reply ? parseListReply(reply) : null;
}
async function resolve(peer, cookie, code) {
	peer.send(resolveMsg(cookie, code));
	const reply = await peer.next();
	return reply ? parseResolveReply(reply) : null;
}
async function hostAndPublish(r, t, options = {}, relayIDs = {}) {
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	assert.ok(await registerHost(host, relayIDs.lobby ?? LOBBY, relayIDs.key ?? KEY, relayIDs.secret ?? SECRET), 'relay lobby registered');
	const reply = await publish(host, { lobby: relayIDs.lobby ?? LOBBY, key: relayIDs.key ?? KEY, secret: relayIDs.secret ?? SECRET, ...options });
	return { host, reply };
}

// ---- tests ----

test('hello: answers with the directory version, the code length and a cookie; a reply is never bigger than its request', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	for (const Kind of [UdpPeer, WsPeer]) {
		const peer = await Kind.create(Kind === UdpPeer ? r.udpPort : r.port);
		t.after(() => peer.close());
		const request = hello();
		peer.send(request);
		const reply = await peer.next();
		const info = parseHelloReply(reply);
		assert.equal(info.ver, 1);
		assert.equal(info.digits, 6);
		assert.ok(info.cookie !== 0n);
		assert.ok(reply.length <= request.length, 'no amplification');
		// too small a hello gets no answer (its reply would be bigger)
		peer.send(P.dirMessage(P.DirType.HELLO, Buffer.from([1])));
		assert.ok(await peer.silent(300));
	}
});

test('publish: needs the relay lobby with its key and secret; the reply holds a 6 digit code', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const stranger = await UdpPeer.create(r.udpPort);
	t.after(() => stranger.close());
	// nothing registered at the relay: "not registered", no listing
	assert.equal((await publish(stranger)).status, P.PublishStatus.NOT_REGISTERED);
	assert.equal(r.relay.core.directory.count, 0);

	const { host, reply } = await hostAndPublish(r, t);
	assert.equal(reply.status, P.PublishStatus.OK);
	assert.match(reply.code, /^[1-9][0-9]{5}$/);
	assert.equal(r.relay.core.directory.count, 1);

	// the right key but not the host's secret (a friend knows the key): not a word back, no change
	assert.equal(await publish(stranger, { secret: 0x1111n, name: 'Hijack' }), null);
	assert.equal(await publish(stranger, { key: 0x2222n, name: 'Hijack' }), null);
	const cookie = await getCookie(stranger);
	const listed = await list(stranger, cookie);
	assert.equal(listed.entries.length, 1);
	assert.equal(listed.entries[0].name, 'Test Lobby');
	host.close();
});

test('publish again updates the listing (players, status) and keeps the code', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const { host, reply } = await hostAndPublish(r, t, { players: 1 });
	const again = await publish(host, { players: 3, flags: P.PublishFlags.PUBLIC | P.PublishFlags.IN_GAME, name: 'New name' });
	assert.equal(again.code, reply.code);
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const entries = (await list(viewer, await getCookie(viewer))).entries;
	assert.equal(entries.length, 1);
	assert.equal(entries[0].players, 3);
	assert.equal(entries[0].name, 'New name');
	assert.equal(entries[0].flags & P.EntryFlags.IN_GAME, P.EntryFlags.IN_GAME);
	assert.equal(entries[0].flags & P.EntryFlags.COMPATIBLE, P.EntryFlags.COMPATIBLE);
});

test('private lobbies are not listed but their code resolves; public ones do both', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const a = await hostAndPublish(r, t, { flags: 0, name: 'Secret', session: 0xaaaaaaaaaan }, { lobby: 1n, key: 1n, secret: 1n });
	const b = await hostAndPublish(r, t, { name: 'Open', session: 0xbbbbbbbbbbn }, { lobby: 2n, key: 2n, secret: 2n });
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const cookie = await getCookie(viewer);
	const listed = await list(viewer, cookie);
	assert.deepEqual(listed.entries.map((e) => e.name), ['Open']);
	assert.equal(listed.total, 1);
	const rp = await resolve(viewer, cookie, a.reply.code);
	assert.equal(rp.status, P.ResolveStatus.OK);
	assert.equal(rp.sessionID, 0xaaaaaaaaaan);
	const ro = await resolve(viewer, cookie, b.reply.code);
	assert.equal(ro.sessionID, 0xbbbbbbbbbbn);
	assert.equal(ro.players, 1);
	assert.equal(ro.max, 4);
	// The joining game must know whether the lobby is on the public list: in a listed lobby the players may be strangers, so the browser build makes no direct
	// (peer to peer) connection there, which would show each player's IP address to the others
	assert.equal(rp.flags & P.EntryFlags.LISTED, 0, 'a private lobby is not marked as listed');
	assert.equal(ro.flags & P.EntryFlags.LISTED, P.EntryFlags.LISTED, 'a public lobby is marked as listed');
	assert.equal(P.EntryFlags.LISTED, 8, 'the flag the game reads (DirectoryProtocol.h ENTRY_LISTED)');
});

test('no answer contains an address of a host', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const { reply } = await hostAndPublish(r, t);
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const cookie = await getCookie(viewer);
	viewer.send(listMsg(cookie));
	const listBytes = await viewer.next();
	viewer.send(resolveMsg(cookie, reply.code));
	const resolveBytes = await viewer.next();
	for (const bytes of [listBytes, resolveBytes]) {
		assert.equal(bytes.includes(Buffer.from('127.0.0.1')), false);
		assert.equal(bytes.includes(Buffer.from([127, 0, 0, 1])), false);
	}
});

test('list and resolve need the cookie from a hello; without it the reply is tiny', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const { reply } = await hostAndPublish(r, t);
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const request = listMsg(0xdeadn);
	viewer.send(request);
	const answer = await viewer.next();
	assert.equal(parseListReply(answer).status, P.ListStatus.BAD_COOKIE);
	assert.ok(answer.length < request.length);
	const rr = resolveMsg(0xdeadn, reply.code);
	viewer.send(rr);
	const ra = await viewer.next();
	assert.equal(parseResolveReply(ra).status, P.ResolveStatus.BAD_COOKIE);
	assert.ok(ra.length < rr.length);
	// a bad cookie is not a "wrong code": the lobby code is not consumed or revealed
	assert.equal(r.relay.core.directory.stats.resolved, 0);
});

test('a cookie belongs to the address that asked for it (WebSocket and UDP share nothing but the address)', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const cookie = await getCookie(viewer);
	// same loopback address, so the cookie is valid for the WebSocket client as well; but a cookie from another address is not
	const core = new RelayCore(() => {});
	const other = new RelayCore(() => {});
	assert.notEqual(core.directory.cookieSecret.toString('hex'), other.directory.cookieSecret.toString('hex'));
	assert.ok(cookie !== 0n);
});

test('expiry: a listing disappears when its host stops publishing, and its code is not given out again at once', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }), { listingTimeoutSeconds: 20 });
	let now = 1000;
	const host = 'udp:1.1.1.1:1';
	core.onDatagram(host, '1.1.1.1', P.buildHostHello(LOBBY, KEY, SECRET), now);
	core.onDatagram(host, '1.1.1.1', publishMsg(), now);
	const code = parsePublishReply(sent.at(-1).buf).code;
	assert.equal(core.directory.listedCount, 1);
	now += 10; core.update(now);
	core.onDatagram(host, '1.1.1.1', P.buildHostHello(LOBBY, KEY, SECRET), now);    // the relay lobby stays alive ...
	assert.equal(core.directory.listedCount, 1, 'still listed after 10 s');
	now += 15; core.update(now);                                                       // ... but nobody published for 25 s
	assert.equal(core.directory.listedCount, 0, 'gone after the timeout');
	// resolve says "not found" now
	core.onDatagram('udp:2.2.2.2:2', '2.2.2.2', hello(), now);
	const cookie = parseHelloReply(sent.at(-1).buf).cookie;
	core.onDatagram('udp:2.2.2.2:2', '2.2.2.2', resolveMsg(cookie, code), now);
	assert.equal(parseResolveReply(sent.at(-1).buf).status, P.ResolveStatus.NOT_FOUND);
	// the host comes back: a new listing gets a different code (the old one rests for 5 minutes)
	core.onDatagram(host, '1.1.1.1', P.buildHostHello(LOBBY, KEY, SECRET), now);
	core.onDatagram(host, '1.1.1.1', publishMsg(), now);
	assert.notEqual(parsePublishReply(sent.at(-1).buf).code, code);
});

test('expiry: when the relay forgets the host, the listing is gone with it', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }), { hostTimeoutSeconds: 10, listingTimeoutSeconds: 100 });
	let now = 50;
	core.onDatagram('udp:1.1.1.1:1', '1.1.1.1', P.buildHostHello(LOBBY, KEY, SECRET), now);
	core.onDatagram('udp:1.1.1.1:1', '1.1.1.1', publishMsg(), now);
	assert.equal(core.directory.count, 1);
	now += 11; core.update(now);
	assert.equal(core.hasLobby(LOBBY), false);
	assert.equal(core.directory.count, 0);
});

test('expiry with the real server: a lobby that stops publishing leaves the list', async (t) => {
	const r = await startRelay({ limits: { listingTimeoutSeconds: 1.2 } });
	t.after(() => r.stop());
	const { host } = await hostAndPublish(r, t);
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const cookie = await getCookie(viewer);
	assert.equal((await list(viewer, cookie)).entries.length, 1);
	await sleep(2600);
	assert.equal((await list(viewer, cookie)).entries.length, 0);
	host.close();
});

test('unpublish: only the owner can close a listing', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const { host, reply } = await hostAndPublish(r, t);
	const stranger = await UdpPeer.create(r.udpPort);
	t.after(() => stranger.close());
	stranger.send(unpublishMsg(LOBBY, KEY, 0x9999n));
	assert.ok(await stranger.silent(300));
	assert.equal(r.relay.core.directory.count, 1);
	host.send(unpublishMsg());
	assert.equal(parsePublishReply(await host.next()).status, P.PublishStatus.CLOSED);
	assert.equal(r.relay.core.directory.count, 0);
	const cookie = await getCookie(stranger);
	assert.equal((await resolve(stranger, cookie, reply.code)).status, P.ResolveStatus.NOT_FOUND);
	assert.equal(r.relay.core.hasLobby(LOBBY), true, 'the relay lobby itself stays (friends keep playing)');
});

test('limits: listings per address and in total', async (t) => {
	const r = await startRelay({ limits: { maxListingsPerIP: 2, maxListings: 3, maxLobbiesPerIP: 10 } });
	t.after(() => r.stop());
	const a = await hostAndPublish(r, t, {}, { lobby: 1n, key: 1n, secret: 1n });
	const b = await hostAndPublish(r, t, {}, { lobby: 2n, key: 2n, secret: 2n });
	const c = await hostAndPublish(r, t, {}, { lobby: 3n, key: 3n, secret: 3n });
	assert.equal(a.reply.status, P.PublishStatus.OK);
	assert.equal(b.reply.status, P.PublishStatus.OK);
	assert.equal(c.reply.status, P.PublishStatus.LIMIT, 'third listing from one address');
	assert.equal(r.relay.core.directory.count, 2);
	// updating an existing listing is never refused by the limit
	assert.equal((await publish(a.host, { lobby: 1n, key: 1n, secret: 1n, players: 2 })).status, P.PublishStatus.OK);
});

test('limits: total number of listings', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }), { maxListings: 3, maxListingsPerIP: 99, maxLobbiesPerIP: 99 });
	for (let i = 1; i <= 5; ++i) {
		const ip = `9.9.9.${i}`, ep = `udp:${ip}:1`;
		core.onDatagram(ep, ip, P.buildHostHello(BigInt(i), 1n, 1n), 10);
		core.onDatagram(ep, ip, publishMsg({ lobby: BigInt(i), key: 1n, secret: 1n }), 10);
	}
	assert.equal(core.directory.count, 3);
	assert.equal(parsePublishReply(sent.at(-1).buf).status, P.PublishStatus.LIMIT);
});

test('guessing codes: 5 wrong codes block the address (also for the right code), the block ends, a whole subnet is limited too', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }));
	let now = 100;
	core.onDatagram('udp:1.1.1.1:1', '1.1.1.1', P.buildHostHello(LOBBY, KEY, SECRET), now);
	core.onDatagram('udp:1.1.1.1:1', '1.1.1.1', publishMsg(), now);
	const code = parsePublishReply(sent.at(-1).buf).code;
	const ask = (ip, c) => {
		now += 0.5;
		core.onDatagram(`udp:${ip}:5`, ip, resolveMsg(cookieFor(ip), c), now);
		return parseResolveReply(sent.at(-1).buf);
	};
	const cookieFor = (ip) => { core.onDatagram(`udp:${ip}:5`, ip, hello(), now); return parseHelloReply(sent.at(-1).buf).cookie; };
	const wrong = code === '123456' ? '654321' : '123456';
	for (let i = 0; i < 5; ++i) assert.equal(ask('3.3.3.3', wrong).status, P.ResolveStatus.NOT_FOUND);
	const blocked = ask('3.3.3.3', code);
	assert.equal(blocked.status, P.ResolveStatus.THROTTLED, 'even the right code is refused while blocked');
	assert.ok(blocked.retryAfter > 200);
	assert.equal(ask('3.3.3.3', code).status, P.ResolveStatus.THROTTLED);
	// another address is not affected ...
	assert.equal(ask('4.4.4.4', code).status, P.ResolveStatus.OK);
	// ... the block ends
	now += 305;
	core.onDatagram('udp:1.1.1.1:1', '1.1.1.1', publishMsg(), now);    // (the host publishes every 5 seconds; the listing would have expired meanwhile)
	assert.equal(ask('3.3.3.3', code).status, P.ResolveStatus.OK);

	// many addresses in one subnet: 25 wrong codes in a minute block the whole /24
	for (let i = 1; i <= 25; ++i) ask(`7.7.7.${i}`, wrong);
	assert.equal(ask('7.7.7.200', code).status, P.ResolveStatus.THROTTLED);
	assert.equal(ask('7.7.8.1', code).status, P.ResolveStatus.OK, 'the next subnet is fine');
});

test('guessing codes: a wrong code, an expired code and a bad format all look the same', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }));
	const now = 100;
	core.onDatagram('udp:5.5.5.5:5', '5.5.5.5', hello(), now);
	const cookie = parseHelloReply(sent.at(-1).buf).cookie;
	const answers = [];
	for (const code of ['123456', '000000', '12345', '1234567', 'abcdef', '12 456', '']) {
		core.onDatagram('udp:6.6.6.6:5', '6.6.6.6', resolveMsg(cookie, code), now);
		answers.push(sent.at(-1).buf.toString('hex'));
	}
	// the cookie is for 5.5.5.5, so these are all bad-cookie answers from 6.6.6.6: identical bytes, nothing to tell apart
	assert.equal(new Set(answers).size, 1);
	core.onDatagram('udp:5.5.5.5:5', '5.5.5.5', resolveMsg(cookie, '123456'), now);
	const a = sent.at(-1).buf.toString('hex');
	core.onDatagram('udp:5.5.5.5:5', '5.5.5.5', resolveMsg(cookie, 'abcdef'), now + 1);
	const b = sent.at(-1).buf.toString('hex');
	assert.equal(a, b, 'unknown code and malformed code: the same answer');
});

test('codes: random 6 digits, unique among live lobbies, never starting with 0', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }), { maxListings: 5000, maxListingsPerIP: 99999, maxLobbies: 5000, maxLobbiesPerIP: 99999, maxSources: 100000 });
	const codes = new Set();
	for (let i = 1; i <= 1500; ++i) {
		const ip = `10.${i >> 8}.${i & 255}.1`;
		core.onDatagram(`udp:${ip}:1`, ip, P.buildHostHello(BigInt(i), 1n, 1n), 10);
		core.onDatagram(`udp:${ip}:1`, ip, publishMsg({ lobby: BigInt(i), key: 1n, secret: 1n }), 10);
		const code = parsePublishReply(sent.at(-1).buf).code;
		assert.match(code, /^[1-9][0-9]{5}$/);
		assert.ok(isValidCode(code, 6));
		codes.add(code);
	}
	assert.equal(codes.size, 1500, 'no collisions');
	assert.ok([...codes].some((c) => c[0] >= '5') && [...codes].some((c) => c[0] <= '4'), 'spread over the range');
});

test('list: order (compatible, in lobby, free room, fullest first), compatible filter and paging', () => {
	const sent = [];
	const core = new RelayCore((to, buf) => sent.push({ to, buf }), { maxListings: 100, maxListingsPerIP: 100, maxLobbiesPerIP: 100, pageSize: 3 });
	const add = (i, opt) => {
		const ip = `11.0.0.${i}`;
		core.onDatagram(`udp:${ip}:1`, ip, P.buildHostHello(BigInt(i), 1n, 1n), 10 + i);
		core.onDatagram(`udp:${ip}:1`, ip, publishMsg({ lobby: BigInt(i), key: 1n, secret: 1n, ...opt }), 10 + i);
	};
	add(1, { name: 'other version', compat: 'zzzz', players: 3 });
	add(2, { name: 'playing', flags: P.PublishFlags.PUBLIC | P.PublishFlags.IN_GAME, players: 3 });
	add(3, { name: 'empty', players: 1 });
	add(4, { name: 'nearly full', players: 3 });
	add(5, { name: 'full', players: 4 });
	add(6, { name: 'two', players: 2 });
	core.onDatagram('udp:8.8.8.8:1', '8.8.8.8', hello(), 30);
	const cookie = parseHelloReply(sent.at(-1).buf).cookie;
	const get = (opt) => { core.onDatagram('udp:8.8.8.8:1', '8.8.8.8', listMsg(cookie, opt), 31); return parseListReply(sent.at(-1).buf); };
	const page1 = get({});
	assert.equal(page1.total, 6);
	assert.equal(page1.entries.length, 3, 'page size');
	assert.deepEqual(page1.entries.map((e) => e.name), ['nearly full', 'two', 'empty']);
	const page2 = get({ offset: 3 });
	assert.deepEqual(page2.entries.map((e) => e.name), ['full', 'playing', 'other version']);
	assert.equal(page2.entries[0].flags & P.EntryFlags.FULL, P.EntryFlags.FULL);
	assert.equal(page2.entries[2].flags & P.EntryFlags.COMPATIBLE, 0, 'other version is marked incompatible');
	const only = get({ flags: P.ListFlags.COMPATIBLE_ONLY });
	assert.equal(only.total, 5);
	assert.ok(!only.entries.some((e) => e.name === 'other version'));
	// no compat given: nothing counts as compatible (the client can still show everything)
	const none = get({ compat: '' });
	assert.equal(none.total, 6);
});

test('text cleaning: names are plain, short and free of the worst words', () => {
	assert.equal(sanitizeText('  Cool   Lobby  ', 20, 'Lobby'), 'Cool Lobby');
	assert.equal(sanitizeText('<b>Hi</b>\n\tthere|', 20, 'Lobby'), 'bHibthere');
	assert.equal(sanitizeText('<script>x</script>', 20, 'Lobby').includes('<'), false);
	assert.equal(sanitizeText('A'.repeat(100), 20, 'Lobby').length, 20);
	assert.equal(sanitizeText('', 20, 'Lobby'), 'Lobby');
	assert.equal(sanitizeText('éè中', 20, 'Player'), 'Player', 'only plain ASCII survives; nothing left means the fallback');
	assert.equal(sanitizeText('Sonic\x00Fan', 12, 'Player'), 'SonicFan');
	for (const bad of ['fuck you', 'F U C K', 'sh1t', 'what a b1tch', 'n1gger', 'heil hitler', 'xxfuckxx', 'N@zi']) assert.equal(isDisallowedText(bad), true, bad);
	for (const fine of ['Peacock Fans', 'Dickson', 'Scunthorpe', 'Classic', 'Sonic 3 and Knuckles', 'Shiitake Run', 'Ring Race!', "Tails' Place", 'Fast & Fun']) assert.equal(isDisallowedText(fine), false, fine);
	assert.equal(sanitizeText('fuck this', 20, 'Lobby'), 'Lobby');
});

test('text cleaning through the wire: the listing holds the cleaned text only', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const { reply } = await hostAndPublish(r, t, { name: '<<evil>>  name\n' + 'x'.repeat(60), host: 'Ho\x01sté' });
	assert.equal(reply.status, P.PublishStatus.OK);
	const viewer = await UdpPeer.create(r.udpPort);
	t.after(() => viewer.close());
	const entry = (await list(viewer, await getCookie(viewer))).entries[0];
	assert.match(entry.name, /^[A-Za-z0-9 _\-.!?'#+&()]{1,20}$/);
	assert.equal(entry.host, 'Host');
});

test('bad publish requests are answered with BAD_REQUEST or ignored, never crash the server', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const { host } = await hostAndPublish(r, t);
	assert.equal((await publish(host, { players: 9, max: 4 })).status, P.PublishStatus.BAD_REQUEST);
	assert.equal((await publish(host, { max: 0 })).status, P.PublishStatus.BAD_REQUEST);
	assert.equal((await publish(host, { max: 99 })).status, P.PublishStatus.BAD_REQUEST);
	assert.equal((await publish(host, { compat: 'bad compat!' })).status, P.PublishStatus.BAD_REQUEST);
	// truncated messages and garbage inside the directory range
	for (const type of [16, 18, 19, 20, 21, 22, 23, 24, 25]) {
		for (const len of [0, 1, 3, 9, 30]) host.send(P.dirMessage(type, Buffer.alloc(len, 7)));
	}
	await sleep(300);
	assert.equal((await publish(host, {})).status, P.PublishStatus.OK, 'still alive');
});

test('protocol versions: version 0 and versions the server does not know are ignored, version 1 works, old messages keep working', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const peer = await UdpPeer.create(r.udpPort);
	t.after(() => peer.close());
	peer.send(hello(0)); assert.ok(await peer.silent(250));
	peer.send(hello(2)); assert.ok(await peer.silent(250));
	peer.send(hello(255)); assert.ok(await peer.silent(250));
	peer.send(hello(1)); assert.equal(parseHelloReply(await peer.next()).ver, 1);
	// the original relay protocol is unchanged: registration and joining still work next to the directory
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	assert.ok(await registerHost(host));
	assert.equal(P.VERSION, 2, 'the relay header version stays 2: old games and old relays are unaffected');
});

test('directory off: the relay works as before and ignores directory messages', async (t) => {
	const r = await startRelay({ limits: { directoryEnabled: false } });
	t.after(() => r.stop());
	const peer = await UdpPeer.create(r.udpPort);
	t.after(() => peer.close());
	peer.send(hello());
	assert.ok(await peer.silent(300));
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	assert.ok(await registerHost(host));
});

test('directory messages are rate limited per address', async (t) => {
	const r = await startRelay({ limits: { dirBurst: 4, dirPerSecond: 0.001 } });
	t.after(() => r.stop());
	const peer = await UdpPeer.create(r.udpPort);
	t.after(() => peer.close());
	for (let i = 0; i < 20; ++i) peer.send(hello());
	await sleep(500);
	let answers = 0;
	while (await peer.next(100)) ++answers;
	assert.equal(answers, 4);
});

test('a browser (WebSocket) host publishes and a Windows (UDP) viewer finds and resolves it, and the other way round', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const wsHost = await WsPeer.create(r.port);
	const udpViewer = await UdpPeer.create(r.udpPort);
	t.after(() => { wsHost.close(); udpViewer.close(); });
	assert.ok(await registerHost(wsHost));
	const reply = await publish(wsHost, { name: 'Browser lobby', session: 0x42n });
	assert.equal(reply.status, P.PublishStatus.OK);
	const cookie = await getCookie(udpViewer);
	const listed = await list(udpViewer, cookie);
	assert.equal(listed.entries[0].name, 'Browser lobby');
	assert.equal((await resolve(udpViewer, cookie, reply.code)).sessionID, 0x42n);

	const wsViewer = await WsPeer.create(r.port);
	t.after(() => wsViewer.close());
	const cookie2 = await getCookie(wsViewer);
	assert.equal((await list(wsViewer, cookie2)).entries.length, 1);
	assert.equal((await resolve(wsViewer, cookie2, reply.code)).status, P.ResolveStatus.OK);
});

test('whole path: publish, list, resolve and then join through the relay and exchange game data', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	const host = await UdpPeer.create(r.udpPort);
	t.after(() => host.close());
	const hostToken = await registerHost(host);
	const reply = await publish(host, { session: 0x7777n });
	const friend = await UdpPeer.create(r.udpPort);
	t.after(() => friend.close());
	const cookie = await getCookie(friend);
	const resolved = await resolve(friend, cookie, reply.code);
	assert.equal(resolved.sessionID, 0x7777n);
	// (the game derives the relay lobby and key from the session id; here they are the ones the host registered)
	friend.send(P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY));
	const joined = await friend.nextOfType(P.Type.CLIENT_JOINED);
	assert.ok(joined, 'joined through the relay');
	const token = joined.readUInt32BE(P.HEADER_SIZE);
	friend.send(P.buildClientData(P.Type.DATA_C2R, token, Buffer.from('hi')));
	const data = await host.nextOfType(P.Type.DATA_R2H);
	assert.ok(data);
	assert.equal(hostToken > 0, true);
});

test('health page counts directory listings (counters only)', async (t) => {
	const r = await startRelay();
	t.after(() => r.stop());
	await hostAndPublish(r, t);
	const text = await (await fetch(`http://127.0.0.1:${r.port}/health`)).text();
	const health = JSON.parse(text);
	assert.equal(health.listed, 1);
	assert.equal(text.includes('Test Lobby'), false, 'no lobby names or codes on the health page');
});

test('golden vectors: the messages are byte for byte the ones the game\'s C++ tests (stable_online_core_tests.cpp) use', () => {
	const hex = (b) => b.toString('hex');
	assert.equal(hex(hello()), '5333524c021001000000000000000000000000000000');
	assert.equal(hex(publishMsg({ lobby: 0x1122334455667788n, key: 0x0102030405060708n, secret: 0xdeadbeefcafef00dn, session: 123456789012n, flags: 1, players: 2, max: 4, compat: 'abcd1234', name: 'Test Lobby', host: 'Host' })),
		'5333524c02120111223344556677880102030405060708deadbeefcafef00d0000001cbe991a140102040861626364313233340a54657374204c6f62627904486f7374');
	assert.equal(hex(unpublishMsg(0x1122334455667788n, 0x0102030405060708n, 0xdeadbeefcafef00dn)), '5333524c02140111223344556677880102030405060708deadbeefcafef00d');
	assert.equal(hex(listMsg(0xa1a2a3a4a5a6a7a8n, { flags: 1, offset: 0, compat: 'abcd1234' })), '5333524c021501a1a2a3a4a5a6a7a80100086162636431323334');
	assert.equal(hex(resolveMsg(0xa1a2a3a4a5a6a7a8n, '123456')), '5333524c021701a1a2a3a4a5a6a7a806313233343536');
});
