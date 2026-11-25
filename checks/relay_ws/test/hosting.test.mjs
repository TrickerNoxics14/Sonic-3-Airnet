// Checks of the relay the way a free web host runs it: the real program as its own process, started with the settings of HOSTING.md
// (UDP off, behind the platform's proxy, only the game page's origin allowed), and a page of another origin as the player.
// Run with:  node --test test/hosting.test.mjs     (it is part of "npm test" in this folder)
// Everything listens on 127.0.0.1 only. The test starts and stops its own processes and leaves nothing running.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import dgram from 'node:dgram';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as P from '../protocol.mjs';
import * as server from '../server.mjs';
import { RawWsPeer, registerHost, sleep, LOBBY, KEY, SECRET } from './helpers.mjs';
import { checkImage, importedFiles, imageFiles } from './docker_guard.mjs';

const relayDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const { createRelayServer } = server;
const playerAddress = (...args) => server.playerAddress(...args);
const GAME_PAGE = 'https://trickernoxics14.github.io';   // what a browser sends as Origin from a GitHub Pages site (scheme and host only, lower case)

// ---- the program as a free host starts it ----

const SETTINGS = ['PORT', 'UDP_PORT', 'BIND', 'TRUST_PROXY', 'ALLOWED_ORIGINS', 'MAX_LOBBIES', 'MAX_LOBBIES_PER_IP', 'MAX_LISTINGS_PER_IP', 'MAX_CONNECTIONS_PER_IP', 'DIRECTORY', 'STATS_SECONDS'];

function freePort() {
	return new Promise((resolve) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => resolve(port)); }); });
}

/** Starts `node server.mjs` in `dir` with the free-host settings (a value of undefined removes a setting). Resolves when it printed its start line or ended. */
async function startProgram(overrides = {}, dir = relayDir) {
	const env = { ...process.env };
	for (const name of SETTINGS) delete env[name];
	const port = await freePort();
	Object.assign(env, { PORT: String(port), BIND: '127.0.0.1', UDP_PORT: '0', TRUST_PROXY: '1', ALLOWED_ORIGINS: GAME_PAGE, STATS_SECONDS: '0' }, overrides);
	for (const name of Object.keys(env)) if (env[name] === undefined) delete env[name];
	const child = spawn(process.execPath, ['server.mjs'], { env, cwd: dir, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
	const program = { port, child, text: '', exitCode: null, started: Date.now() };
	child.stdout.on('data', (c) => { program.text += c; });
	child.stderr.on('data', (c) => { program.text += c; });
	child.on('exit', (code) => { program.exitCode = code ?? -1; });
	program.stop = () => new Promise((resolve) => {
		if (program.exitCode !== null) { resolve(); return; }
		child.once('exit', () => resolve());
		child.kill();
		setTimeout(resolve, 3000).unref();
	});
	for (let i = 0; i < 200 && program.exitCode === null && !/s3air_relay: WebSocket/.test(program.text); ++i) await sleep(25);
	program.startSeconds = (Date.now() - program.started) / 1000;
	return program;
}

const healthOf = async (program) => (await fetch(`http://127.0.0.1:${program.port}/health`)).json();

// ---- what a game sends to the directory (same builders as test/directory.test.mjs) ----
const hello = () => P.dirMessage(P.DirType.HELLO, new P.Writer().u8(1).pad(P.HELLO_MIN_PAYLOAD - 1).done());
const publishMsg = () => P.dirMessage(P.DirType.PUBLISH, new P.Writer().u8(1).u64(LOBBY).u64(KEY).u64(SECRET).u64(0x1234567890n).u8(P.PublishFlags.PUBLIC).u8(1).u8(4).str('abcd1234').str('Browser Room').str('Host').done());
const listMsg = (cookie) => P.dirMessage(P.DirType.LIST, new P.Writer().u8(1).u64(cookie).u8(0).u8(0).str('abcd1234').done());
const resolveMsg = (cookie, code) => P.dirMessage(P.DirType.RESOLVE, new P.Writer().u8(1).u64(cookie).str(code).done());

// ---- 1. UDP off ----

test('free host: UDP_PORT=0 opens no UDP socket at all (a UDP socket on the same number can still be opened), and with UDP on it cannot', async (t) => {
	const off = await startProgram();
	t.after(() => off.stop());
	assert.match(off.text, /TCP \d+, UDP off/, 'the start line says that UDP is off');
	const tryUdp = (port) => new Promise((resolve) => {
		const socket = dgram.createSocket('udp4');
		socket.once('error', (error) => { try { socket.close(); } catch { /* not open */ } resolve(error.code); });
		socket.bind(port, '127.0.0.1', () => socket.close(() => resolve('free')));
	});
	assert.equal(await tryUdp(off.port), 'free', 'nobody holds UDP on the relay port');

	// the same probe against a relay that does use UDP (same number as TCP, the default): the probe must see it
	const on = await startProgram({ UDP_PORT: undefined });
	t.after(() => on.stop());
	assert.match(on.text, /UDP on \d+/);
	assert.equal(await tryUdp(on.port), 'EADDRINUSE', 'the probe does notice a UDP socket');
});

test('free host: starts at once (a sleeping host wakes the program on the first request) and the start line holds no address or setting values', async (t) => {
	const program = await startProgram();
	t.after(() => program.stop());
	assert.ok(program.startSeconds < 3, `started in ${program.startSeconds} s`);
	assert.equal(program.text.includes('127.0.0.1'), false);
	assert.equal(program.text.includes('github.io'), false, 'the allowed origin is not printed');
});

// ---- 2. a page of another origin: list, publish, resolve, signal ----

test('game page origin: connects over WebSocket, publishes a lobby, a second page lists and resolves it, joins, signalling and data both ways (real process, UDP off)', async (t) => {
	const program = await startProgram();
	t.after(() => program.stop());
	const hostPage = await RawWsPeer.open(program.port, { Origin: GAME_PAGE, 'X-Forwarded-For': '203.0.113.10' });
	const friendPage = await RawWsPeer.open(program.port, { Origin: GAME_PAGE, 'X-Forwarded-For': '203.0.113.11' });
	t.after(() => { hostPage.close(); friendPage.close(); });
	assert.equal(hostPage.status, 101);
	assert.equal(friendPage.status, 101);

	const hostToken = await registerHost(hostPage);
	assert.ok(hostToken, 'the host page registered its lobby');
	hostPage.send(publishMsg());
	const published = new P.Reader(await hostPage.nextOfType(P.DirType.PUBLISH_REPLY));
	assert.equal(published.u8(), 1);
	assert.equal(published.u8(), P.PublishStatus.OK);
	const code = published.str();
	assert.match(code, /^[1-9][0-9]{5}$/);

	friendPage.send(hello());
	const helloReply = new P.Reader(await friendPage.nextOfType(P.DirType.HELLO_REPLY));
	helloReply.u8(); helloReply.u8(); helloReply.u8(); helloReply.u8(); helloReply.u16();
	const cookie = helloReply.u64();

	friendPage.send(listMsg(cookie));
	const list = new P.Reader(await friendPage.nextOfType(P.DirType.LIST_REPLY));
	assert.equal(list.u8(), 1);
	assert.equal(list.u8(), P.ListStatus.OK);
	assert.equal(list.u16(), 1, 'one public lobby');
	list.u8(); list.u8();
	assert.equal(list.str(), code);
	assert.equal(list.str(), 'Browser Room');

	friendPage.send(resolveMsg(cookie, code));
	const resolved = new P.Reader(await friendPage.nextOfType(P.DirType.RESOLVE_REPLY));
	assert.equal(resolved.u8(), 1);
	assert.equal(resolved.u8(), P.ResolveStatus.OK);
	assert.equal(resolved.u64(), 0x1234567890n);

	friendPage.send(P.buildHello(P.Type.CLIENT_JOIN, LOBBY, KEY));
	const joined = await friendPage.nextOfType(P.Type.CLIENT_JOINED);
	assert.ok(joined, 'the friend page joined the relay lobby');
	const token = joined.readUInt32BE(P.HEADER_SIZE);

	// signalling (offer / answer / candidates) and data, both directions
	friendPage.send(P.buildClientData(P.Type.SIGNAL_C2R, token, Buffer.from('offer from the friend')));
	const toHost = await hostPage.nextOfType(P.Type.SIGNAL_R2H);
	assert.equal(toHost?.subarray(P.HEADER_SIZE + 6).toString(), 'offer from the friend');
	hostPage.send(P.buildHostData(P.Type.SIGNAL_H2R, hostToken, 1, Buffer.from('answer from the host')));
	const toFriend = await friendPage.nextOfType(P.Type.SIGNAL_R2C);
	assert.equal(toFriend?.subarray(P.HEADER_SIZE + 4).toString(), 'answer from the host');
	friendPage.send(P.buildClientData(P.Type.DATA_C2R, token, Buffer.from('input')));
	assert.equal((await hostPage.nextOfType(P.Type.DATA_R2H))?.subarray(P.HEADER_SIZE + 6).toString(), 'input');
	hostPage.send(P.buildHostData(P.Type.DATA_H2R, hostToken, 1, Buffer.from('state')));
	assert.equal((await friendPage.nextOfType(P.Type.DATA_R2C))?.subarray(P.HEADER_SIZE + 4).toString(), 'state');

	const health = await healthOf(program);
	assert.equal(health.lobbies, 1);
	assert.equal(health.listed, 1);
	assert.equal(health.websocketClients, 2);
	assert.equal(health.signals, 2);
});

test('start line: says what TRUST_PROXY means, so that a changed meaning shows in the log of the host', async (t) => {
	const lines = { 0: /TRUST_PROXY=0: no proxy trusted, X-Forwarded-For is ignored/, 1: /TRUST_PROXY=1: one proxy trusted, the last X-Forwarded-For entry is the player's address/, 2: /TRUST_PROXY=2: 2 proxies trusted, entry number 2 counted from the end of X-Forwarded-For is the player's address/, 3: /TRUST_PROXY=3: 3 proxies trusted, entry number 3 counted from the end/ };
	for (const [n, pattern] of Object.entries(lines)) {
		const program = await startProgram({ TRUST_PROXY: n });
		t.after(() => program.stop());
		assert.match(program.text, pattern, `TRUST_PROXY=${n}`);
		assert.match(program.text, /fewer entries counts as the address of the connection|every player counts as the address of the connection/, 'and what a missing or short header means');
	}
	const unset = await startProgram({ TRUST_PROXY: undefined });
	t.after(() => unset.stop());
	assert.match(unset.text, /TRUST_PROXY=0: no proxy trusted/, 'unset means 0');
});

// ---- 3. other origins ----

test('origins: another page is refused (403), so are look-alikes; a client without Origin (a native game) is let in; the browser spelling of the allowed origin matches', async (t) => {
	const program = await startProgram();
	t.after(() => program.stop());
	const status = async (headers) => { const peer = await RawWsPeer.open(program.port, headers); peer.close(); return peer.status; };
	assert.equal(await status({ Origin: 'https://example.org' }), 403);
	assert.equal(await status({ Origin: `${GAME_PAGE}.evil.example` }), 403, 'the origin must match completely, not start with the allowed one');
	assert.equal(await status({ Origin: 'http://trickernoxics14.github.io' }), 403, 'another scheme');
	assert.equal(await status({ Origin: 'https://trickernoxics14.github.io:8443' }), 403, 'another port');
	assert.equal(await status({ Origin: 'https://sub.trickernoxics14.github.io' }), 403, 'a sub domain');
	assert.equal(await status({ Origin: 'null' }), 403, 'a sandboxed frame or a local file');
	assert.equal(await status({ Origin: GAME_PAGE }), 101);
	// no Origin header: native games (and any script, which can write any Origin it likes: the list protects against other web pages, it is not a login)
	assert.equal(await status({}), 101);
	// an Origin header that is there but empty is no origin either: refused while a list is set (a browser never sends one)
	assert.equal(await status({ Origin: '' }), 403, 'an empty Origin header');
	// the process still answers after the refusals
	assert.equal((await healthOf(program)).ok, true);
});

test('origins: without a list every page is let in, an empty Origin header too', async (t) => {
	const program = await startProgram({ ALLOWED_ORIGINS: undefined });
	t.after(() => program.stop());
	const status = async (headers) => { const peer = await RawWsPeer.open(program.port, headers); peer.close(); return peer.status; };
	assert.equal(await status({ Origin: 'https://example.org' }), 101);
	assert.equal(await status({ Origin: '' }), 101);
	assert.equal(await status({}), 101);
});

test('origins: the setting may be written the way people copy it (capital letters, trailing slash, a page path, spaces, several entries); a value that is no origin stops the program at the start', async (t) => {
	const program = await startProgram({ ALLOWED_ORIGINS: ' https://TrickerNoxics14.github.io/Sonic-3-Airnet/ , https://other.example/ ' });
	t.after(() => program.stop());
	const status = async (origin) => { const peer = await RawWsPeer.open(program.port, { Origin: origin }); peer.close(); return peer.status; };
	assert.equal(await status(GAME_PAGE), 101, 'the browser sends the lower case origin without a path');
	assert.equal(await status('https://other.example'), 101);
	assert.equal(await status('https://example.org'), 403);

	// an entry that no browser's Origin can ever equal must stop the program: it would lock every page out without a word (or, dropped, open the list)
	const never = ['trickernoxics14.github.io', '*', 'https://', 'https://*.github.io', 'https://*', 'https://user@x.github.io', 'ftp://x.github.io', 'https://x.github.io?a=1', 'https://x.github.io#top',
		'https://x.github.io:99999', 'https://x.github.io:abc', 'https://x github.io', 'https://.github.io', 'javascript:alert(1)', 'https://x.github.io, *'];
	for (const bad of never) {
		const stopped = await startProgram({ ALLOWED_ORIGINS: bad });
		t.after(() => stopped.stop());
		for (let i = 0; i < 100 && stopped.exitCode === null; ++i) await sleep(25);
		assert.equal(stopped.exitCode, 1, `"${bad}" stops the program`);
		assert.match(stopped.text, /ALLOWED_ORIGINS/);
		assert.match(stopped.text, /scheme and host/, 'the message says how to write it');
	}
	// the forms that do match stay accepted (default ports are dropped like a browser does, an IP address and a port are fine)
	for (const good of ['https://x.github.io', 'http://localhost:8080', 'https://x.github.io:443', 'http://127.0.0.1:5500', 'https://xn--bcher-kva.example', 'https://[::1]:8443', 'HTTPS://X.GitHub.IO/']) {
		const started = await startProgram({ ALLOWED_ORIGINS: good });
		t.after(() => started.stop());
		assert.equal(started.exitCode, null, `"${good}" is accepted: ${started.text}`);
	}
});

// ---- 4. the health page ----

test('health page: answers GET and HEAD, shows counters only (no address, code, name or secret), and is not cached', async (t) => {
	const program = await startProgram();
	t.after(() => program.stop());
	const page = await RawWsPeer.open(program.port, { Origin: GAME_PAGE, 'X-Forwarded-For': '203.0.113.77' });
	t.after(() => page.close());
	assert.ok(await registerHost(page), 'registered');
	page.send(publishMsg());
	assert.ok(await page.nextOfType(P.DirType.PUBLISH_REPLY));

	const response = await fetch(`http://127.0.0.1:${program.port}/health`, { headers: { 'X-Forwarded-For': '203.0.113.78' } });
	assert.equal(response.status, 200);
	assert.match(response.headers.get('content-type'), /application\/json/);
	assert.equal(response.headers.get('cache-control'), 'no-store');
	const text = await response.text();
	const health = JSON.parse(text);
	assert.deepEqual(Object.keys(health).sort(), ['forwarded', 'friends', 'listed', 'lobbies', 'ok', 'protocol', 'signals', 'websocketAddresses', 'websocketClients']);
	assert.equal(health.ok, true);
	assert.equal(health.protocol, 2);
	for (const [key, value] of Object.entries(health)) if (key !== 'ok') assert.equal(typeof value, 'number', `${key} is a counter`);
	for (const secret of ['127.0.0.1', '203.0.113', 'Browser Room', 'Host', GAME_PAGE, '1234567890', '11223344', 'deadbeef']) assert.equal(text.includes(secret), false, `no "${secret}" on the health page`);
	assert.equal(health.listed, 1);
	assert.equal(health.websocketAddresses, 1);

	const head = await fetch(`http://127.0.0.1:${program.port}/health`, { method: 'HEAD' });
	assert.equal(head.status, 200);
	assert.equal(await head.text(), '');
	assert.equal((await fetch(`http://127.0.0.1:${program.port}/health?check=1`)).status, 200);
	const root = await fetch(`http://127.0.0.1:${program.port}/`);
	assert.equal(root.status, 200);
	assert.equal((await root.text()).includes('203.0.113'), false);
	assert.equal(program.text.includes('203.0.113'), false, 'no player address in the program output');
});

// ---- 5. the player's address behind the platform's proxy ----

test('playerAddress: the address that the limits use (no proxy, one proxy, two proxies, a faked first entry, garbage)', () => {
	assert.equal(playerAddress('::ffff:10.0.0.1', '1.2.3.4', 0), '10.0.0.1', 'without proxy the header is ignored');
	assert.equal(playerAddress('10.0.0.1', '1.2.3.4', 0), '10.0.0.1');
	assert.equal(playerAddress('10.0.0.1', '1.2.3.4', 1), '1.2.3.4', 'one proxy that wrote the only entry');
	assert.equal(playerAddress('10.0.0.1', '6.6.6.6, 1.2.3.4', 1), '1.2.3.4', 'a proxy that appends: the first entry came from the player');
	assert.equal(playerAddress('10.0.0.1', '6.6.6.6, 1.2.3.4, 10.9.9.9', 2), '1.2.3.4', 'two proxies');
	// a header with fewer entries than proxies is not the chain that was promised: the connection's own address (the platform's edge) is used, never an entry from the front
	assert.equal(playerAddress('10.0.0.1', '1.2.3.4', 2), '10.0.0.1', 'one entry, two proxies');
	assert.equal(playerAddress('10.0.0.1', '6.6.6.6, 1.2.3.4', 3), '10.0.0.1', 'two entries, three proxies');
	assert.equal(playerAddress('10.0.0.1', '6.6.6.6, 1.2.3.4, 10.9.9.9', 3), '6.6.6.6', 'exactly as many entries as proxies: the first one is the player');
	assert.equal(playerAddress('10.0.0.1', undefined, 3), '10.0.0.1');
	assert.equal(playerAddress('10.0.0.1', '', 2), '10.0.0.1');
	assert.equal(playerAddress('10.0.0.1', undefined, 1), '10.0.0.1', 'no header: the address of the connection');
	assert.equal(playerAddress('10.0.0.1', '', 1), '10.0.0.1');
	assert.equal(playerAddress('10.0.0.1', 'not an address', 1), '10.0.0.1', 'garbage is never used as an address');
	assert.equal(playerAddress('10.0.0.1', '1.2.3.4, <script>', 1), '10.0.0.1');
	assert.equal(playerAddress('10.0.0.1', '1.2.3.4:5555', 1), '10.0.0.1', 'with a port it is no address either: not guessed at');
	assert.equal(playerAddress('10.0.0.1', '2001:db8::1', 1), '2001:db8::1');
	assert.equal(playerAddress('10.0.0.1', '::ffff:1.2.3.4', 1), '1.2.3.4');
});

test('TRUST_PROXY=1: the per-address connection limit counts the address the proxy saw, a faked first entry does not help; TRUST_PROXY=0 ignores the header', async (t) => {
	const behind = await startProgram({ MAX_CONNECTIONS_PER_IP: '2', ALLOWED_ORIGINS: undefined });
	t.after(() => behind.stop());
	const open = async (program, forwarded, keep) => { const peer = await RawWsPeer.open(program.port, forwarded === null ? {} : { 'X-Forwarded-For': forwarded }); keep.push(peer); return peer.status; };
	const kept = [];
	t.after(() => kept.forEach((peer) => peer.close()));
	// two players behind the proxy: each has its own allowance of two
	assert.deepEqual([await open(behind, '203.0.113.1', kept), await open(behind, '203.0.113.1', kept), await open(behind, '203.0.113.1', kept)], [101, 101, 429]);
	assert.deepEqual([await open(behind, '203.0.113.2', kept), await open(behind, '203.0.113.2', kept)], [101, 101], 'another player is not held back by the first');
	// a player who writes its own X-Forwarded-For: the proxy appends the real address behind it, so the real address is what counts
	assert.deepEqual([
		await open(behind, '198.51.100.1, 203.0.113.50', kept), await open(behind, '198.51.100.2, 203.0.113.50', kept), await open(behind, '198.51.100.3, 203.0.113.50', kept),
	], [101, 101, 429], 'three different faked first entries are still one player');
	// the same through two separate header lines (the program sees them joined)
	assert.deepEqual([await open(behind, ['198.51.100.4', '203.0.113.60'], kept), await open(behind, ['198.51.100.5', '203.0.113.60'], kept), await open(behind, ['198.51.100.6', '203.0.113.60'], kept)], [101, 101, 429]);
	// text that is no address cannot be used as a fresh identity: it counts as the connection's own address
	assert.deepEqual([await open(behind, 'aaa', kept), await open(behind, 'bbb', kept), await open(behind, 'ccc', kept)], [101, 101, 429]);
	// no header at all (a platform check, a direct visit): the connection's own address
	assert.equal(await open(behind, null, kept), 429, 'the garbage entries above used up the address of the connection itself');

	const direct = await startProgram({ MAX_CONNECTIONS_PER_IP: '2', TRUST_PROXY: '0', ALLOWED_ORIGINS: undefined });
	t.after(() => direct.stop());
	assert.deepEqual([await open(direct, '203.0.113.1', kept), await open(direct, '203.0.113.2', kept), await open(direct, '203.0.113.3', kept)], [101, 101, 429], 'the header is ignored when no proxy is trusted (nobody can fake an address)');
});

test('health page: websocketAddresses counts the distinct player addresses behind the proxy (how an operator sees that every player looks the same, or that a faked address makes no new one)', async (t) => {
	const program = await startProgram({ ALLOWED_ORIGINS: undefined });
	t.after(() => program.stop());
	const kept = [];
	t.after(() => kept.forEach((peer) => peer.close()));
	const open = async (forwarded) => { kept.push(await RawWsPeer.open(program.port, { 'X-Forwarded-For': forwarded })); };
	await open('203.0.113.1'); await open('203.0.113.2'); await open('203.0.113.2');
	assert.deepEqual([(await healthOf(program)).websocketClients, (await healthOf(program)).websocketAddresses], [3, 2]);
	await open('1.1.1.1, 203.0.113.2'); await open('1.1.1.2, 203.0.113.2');   // faked first entries behind a known address: no new address
	assert.deepEqual([(await healthOf(program)).websocketClients, (await healthOf(program)).websocketAddresses], [5, 2]);
	kept[0].closeWithFrame();
	for (let i = 0; i < 40 && (await healthOf(program)).websocketAddresses !== 1; ++i) await sleep(50);
	assert.equal((await healthOf(program)).websocketAddresses, 1, 'an address is forgotten when its last connection closes');
});

test('TRUST_PROXY=N: a header with fewer entries than N counts as the address of the connection itself (nobody gets an identity of its own invention), here N=3 and N=2', async (t) => {
	const kept = [];
	t.after(() => kept.forEach((peer) => peer.close()));
	for (const [n, headers] of [['3', ['6.6.6.1, 1.2.3.4', '6.6.6.2, 1.2.3.4', '6.6.6.3, 1.2.3.4']], ['2', ['6.6.6.1', '6.6.6.2', '6.6.6.3']]]) {
		const program = await startProgram({ MAX_CONNECTIONS_PER_IP: '2', TRUST_PROXY: n, ALLOWED_ORIGINS: undefined });
		t.after(() => program.stop());
		const statuses = [];
		for (const header of headers) { const peer = await RawWsPeer.open(program.port, { 'X-Forwarded-For': header }); kept.push(peer); statuses.push(peer.status); }
		assert.deepEqual(statuses, [101, 101, 429], `TRUST_PROXY=${n}: three different faked entries are one address (the connection's own)`);
	}
});

test('TRUST_PROXY=2: two proxies in front, the address in front of both is the player', async (t) => {
	const program = await startProgram({ MAX_CONNECTIONS_PER_IP: '1', TRUST_PROXY: '2', ALLOWED_ORIGINS: undefined });
	t.after(() => program.stop());
	const kept = [];
	t.after(() => kept.forEach((peer) => peer.close()));
	const open = async (forwarded) => { const peer = await RawWsPeer.open(program.port, { 'X-Forwarded-For': forwarded }); kept.push(peer); return peer.status; };
	assert.deepEqual([await open('9.9.9.1, 203.0.113.5, 10.1.1.1'), await open('9.9.9.2, 203.0.113.5, 10.1.1.1'), await open('9.9.9.3, 203.0.113.6, 10.1.1.1')], [101, 429, 101]);
});

test('TRUST_PROXY: only whole numbers are accepted, anything else stops the program with a message (a typo must not silently pool every player behind the proxy)', async (t) => {
	for (const bad of ['yes', 'true', '1.5', '-1', '100']) {
		const program = await startProgram({ TRUST_PROXY: bad });
		t.after(() => program.stop());
		for (let i = 0; i < 100 && program.exitCode === null; ++i) await sleep(25);
		assert.equal(program.exitCode, 1, `"${bad}" stops the program`);
		assert.match(program.text, /TRUST_PROXY/);
	}
	for (const good of ['0', '1', '2', ' 1 ']) {
		const program = await startProgram({ TRUST_PROXY: good });
		t.after(() => program.stop());
		assert.equal(program.exitCode, null, `"${good}" is accepted`);
	}
});

test('in-process relay: a boolean trustProxy still means one proxy (older callers)', async (t) => {
	const relay = createRelayServer({ port: 0, udp: false, bind: '127.0.0.1', trustProxy: true, maxConnectionsPerIP: 1 });
	const { port } = await relay.start();
	t.after(() => relay.stop());
	const kept = [];
	t.after(() => kept.forEach((peer) => peer.close()));
	const open = async (forwarded) => { const peer = await RawWsPeer.open(port, { 'X-Forwarded-For': forwarded }); kept.push(peer); return peer.status; };
	assert.deepEqual([await open('203.0.113.1'), await open('203.0.113.1'), await open('203.0.113.2')], [101, 429, 101]);
});

const runProxyCheck = (args) => new Promise((resolve) => {
	const child = spawn(process.execPath, ['proxy_check.mjs', ...args], { cwd: relayDir, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
	let text = '';
	child.stdout.on('data', (c) => { text += c; });
	child.stderr.on('data', (c) => { text += c; });
	child.on('exit', (code) => resolve({ code, text }));
});

/**
 * A proxy in front of `targetPort` that handles X-Forwarded-For the way the platforms do:
 *   append    adds the address it saw to the end of what the player sent (nginx, Cloudflare, a platform router)
 *   pass      adds nothing: the header arrives as the player wrote it
 *   overwrite throws away what the player sent and writes the address it saw (Caddy's default)
 * `seen` is the address this proxy believes its peer has. Everything else of the request and the whole connection afterwards goes through untouched.
 */
function startProxy(targetPort, mode, seen) {
	const server = net.createServer((client) => {
		const upstream = net.connect(targetPort, '127.0.0.1');
		let pending = Buffer.alloc(0);
		let tunnel = false;   // after an upgrade request the connection is just a pipe
		client.on('data', (chunk) => {
			if (tunnel) { upstream.write(chunk); return; }
			pending = Buffer.concat([pending, chunk]);
			// one connection can carry several requests (a keep-alive connection of the client that is reused for the upgrade): every request head is rewritten, like a real proxy does
			for (let end = pending.indexOf('\r\n\r\n'); end >= 0 && !tunnel; end = pending.indexOf('\r\n\r\n')) {
				const lines = pending.toString('latin1', 0, end).split('\r\n');
				const sent = lines.filter((l) => /^x-forwarded-for:/i.test(l)).map((l) => l.slice(l.indexOf(':') + 1).trim());
				const others = lines.filter((l) => !/^x-forwarded-for:/i.test(l));
				if (mode === 'pass') others.push(...sent.map((v) => `X-Forwarded-For: ${v}`));
				else if (mode === 'append') others.push(`X-Forwarded-For: ${[...sent, seen].join(', ')}`);
				else others.push(`X-Forwarded-For: ${seen}`);
				upstream.write(Buffer.from(others.join('\r\n') + '\r\n\r\n', 'latin1'));
				tunnel = lines.some((l) => /^upgrade:/i.test(l));
				pending = pending.subarray(end + 4);   // requests here have no body (GET, HEAD, an upgrade)
			}
			if (tunnel && pending.length) { upstream.write(pending); pending = Buffer.alloc(0); }
		});
		upstream.on('data', (chunk) => client.write(chunk));
		for (const side of [client, upstream]) { side.on('error', () => {}); side.on('close', () => { client.destroy(); upstream.destroy(); }); }
	});
	return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ port: server.address().port, stop: () => new Promise((done) => { server.close(done); server.closeAllConnections?.(); }) })));
}

/** A relay with TRUST_PROXY = `n` behind a chain of proxies (first = the one the player talks to); returns the address the operator would give proxy_check. */
async function relayBehind(chain, n) {
	const relay = createRelayServer({ port: 0, udp: false, bind: '127.0.0.1', trustProxy: n });
	const { port } = await relay.start();
	const proxies = [];
	let target = port;
	for (let i = chain.length - 1; i >= 0; --i) {
		const proxy = await startProxy(target, chain[i], i === 0 ? '127.0.0.1' : `10.0.0.${i}`);   // the player's own address, then the address of the proxy in front
		proxies.unshift(proxy);
		target = proxy.port;
	}
	return { url: `ws://127.0.0.1:${target}`, stop: async () => { for (const p of proxies) await p.stop(); await relay.stop(); } };
}

test('proxy_check.mjs against real chains of proxies: PROBLEM appears exactly when TRUST_PROXY is higher than the number of proxies that add an address, and lowering it fixes it', async (t) => {
	// exit code of proxy_check for TRUST_PROXY = 0, 1, 2, 3.   0 = OK, 1 = PROBLEM
	const cases = [
		{ name: 'one proxy that appends (nginx, Cloudflare, a platform router)', chain: ['append'], expected: [0, 0, 1, 1] },
		{ name: 'a proxy that adds nothing: the header is only what the player wrote', chain: ['pass'], expected: [0, 1, 1, 1] },
		{ name: 'a proxy that overwrites the header (Caddy): whatever the player sends is gone', chain: ['overwrite'], expected: [0, 0, 0, 0] },
		{ name: 'two proxies that append', chain: ['append', 'append'], expected: [0, 0, 0, 1] },
		{ name: 'three proxies that append', chain: ['append', 'append', 'append'], expected: [0, 0, 0, 0] },
		{ name: 'an overwriting proxy behind an appending one', chain: ['append', 'overwrite'], expected: [0, 0, 0, 0] },
	];
	for (const { name, chain, expected } of cases) {
		const runs = await Promise.all([0, 1, 2, 3].map(async (n) => {
			const setup = await relayBehind(chain, n);
			t.after(() => setup.stop());
			return runProxyCheck([setup.url]);
		}));
		assert.deepEqual(runs.map((r) => r.code), expected, `${name}: exit codes for TRUST_PROXY 0, 1, 2, 3\n${runs.map((r, n) => `  N=${n}: ${r.text.trim().split('\n').pop()}`).join('\n')}`);
		runs.forEach((r, n) => { if (r.code === 1) { assert.match(r.text, /^PROBLEM: .*too HIGH/m, `N=${n}`); assert.match(r.text, /Lower TRUST_PROXY by one/, `N=${n}`); assert.doesNotMatch(r.text, /\braise\b/i, `N=${n}: the message never says raise`); assert.match(r.text, /run (it|this) again/i, `N=${n}: players may have joined during the check`); } });
	}
	// following the printed advice from too high converges: start at 3 with one appending proxy, lower while PROBLEM
	let n = 3;
	let steps = 0;
	for (; n >= 0; --n) {
		const setup = await relayBehind(['append'], n);
		t.after(() => setup.stop());
		const result = await runProxyCheck([setup.url]);
		++steps;
		if (result.code === 0) break;
	}
	assert.deepEqual([n, steps], [1, 3], 'from 3 to 2 to 1: OK at the number of proxies (one)');
});

test('proxy_check.mjs (run by the operator against the deployed relay): OK when claimed addresses make no new address, PROBLEM when the relay believes them, usage error otherwise', async (t) => {
	const run = runProxyCheck;
	// no proxy in front and none trusted: every claim is ignored, the three connections are one address
	const plain = await startProgram({ TRUST_PROXY: '0', ALLOWED_ORIGINS: undefined });
	t.after(() => plain.stop());
	const ok = await run(['ws://127.0.0.1:' + plain.port]);
	assert.equal(ok.code, 0, ok.text);
	assert.match(ok.text, /^OK: /m);
	// the relay trusts a header that nobody in front of it overwrites (a misconfiguration): the claims become addresses
	const trusting = await startProgram({ TRUST_PROXY: '1', ALLOWED_ORIGINS: undefined });
	t.after(() => trusting.stop());
	const bad = await run(['ws://127.0.0.1:' + trusting.port]);
	assert.equal(bad.code, 1, bad.text);
	assert.match(bad.text, /^PROBLEM: /m);
	assert.match(bad.text, /Lower TRUST_PROXY by one \(to 0 if/, 'with nothing in front the advice ends at 0');
	assert.equal((await run([])).code, 2);
	assert.equal((await run(['https://example.org'])).code, 2);
});

// ---- 6. the container image ----

const readRelay = (name) => { try { return fs.readFileSync(path.join(relayDir, ...name.split('/')), 'utf8'); } catch { return null; } };
const listFiles = (dir, prefix = '') => fs.readdirSync(path.join(relayDir, dir), { withFileTypes: true }).flatMap((e) => (e.name === 'node_modules' ? [] : e.isDirectory() ? listFiles(path.posix.join(dir, e.name), path.posix.join(prefix, e.name)) : [path.posix.join(prefix, e.name)]));

test('container image: the Dockerfile copies every file that server.mjs needs, and a folder with exactly the copied files starts and answers', async (t) => {
	const dockerfile = readRelay('Dockerfile');
	const dockerignore = readRelay('.dockerignore') ?? '';
	const check = checkImage({ entry: 'server.mjs', read: readRelay, dockerfile, dockerignore });
	assert.deepEqual(check.problems, [], 'what the guard found');
	for (const must of ['directory.mjs', 'relay_core.mjs', 'ws.mjs', 'protocol.mjs', 'server.mjs']) assert.ok(check.needed.includes(must), `the import walk found ${check.needed}`);

	const image = fs.mkdtempSync(path.join(os.tmpdir(), 's3air-image-'));
	const inImage = imageFiles(listFiles(''), check.sources, dockerignore);
	assert.ok(inImage.includes('package.json') && inImage.includes('server.mjs'));
	assert.equal(inImage.some((f) => f.startsWith('test/')), false, 'the tests are not in the image (.dockerignore)');
	for (const file of inImage) { fs.mkdirSync(path.dirname(path.join(image, file)), { recursive: true }); fs.copyFileSync(path.join(relayDir, file), path.join(image, file)); }
	const program = await startProgram({ ALLOWED_ORIGINS: undefined }, image);
	t.after(async () => { await program.stop(); fs.rmSync(image, { recursive: true, force: true }); });   // the folder cannot be removed while the program runs in it
	assert.equal(program.exitCode, null, `the program in the image ended: ${program.text}`);
	assert.equal((await healthOf(program)).ok, true);
});

test('container image guard: follows both quote styles, sub folders and re-exports, reports what it cannot follow, and sees what a COPY brings', () => {
	const files = (map) => (name) => (name in map ? map[name] : null);
	const copy = (line) => `FROM node:22\nWORKDIR /app\n${line}\nCMD ["node","server.mjs"]\n`;
	const run = (map, dockerfile, dockerignore = '') => checkImage({ entry: 'server.mjs', read: files(map), dockerfile, dockerignore });

	// the normal case: flat files, copied by a pattern
	assert.deepEqual(run({ 'server.mjs': "import './a.mjs';", 'a.mjs': '', 'package.json': '{}' }, copy('COPY package.json *.mjs ./')).problems, []);
	// a file that no COPY brings
	assert.match(run({ 'server.mjs': "import { a } from './a.mjs';", 'a.mjs': '', 'package.json': '{}' }, copy('COPY package.json server.mjs ./')).problems.join('\n'), /a\.mjs: needed by the program but not copied/);
	// double quotes, export-from, dynamic import with either quote
	const doubles = { 'server.mjs': 'import x from "./b.mjs"; export * from "./c.mjs"; const d = await import("./d.mjs"); const e = await import(\'./e.mjs\');', 'b.mjs': '', 'c.mjs': '', 'd.mjs': '', 'e.mjs': '', 'package.json': '{}' };
	assert.deepEqual(importedFiles('server.mjs', files(doubles)).files, ['b.mjs', 'c.mjs', 'd.mjs', 'e.mjs', 'server.mjs']);
	// a sub folder: not copied by *.mjs, copied by the folder or by '.'
	const lib = { 'server.mjs': "import './lib/x.mjs';", 'lib/x.mjs': "import '../y.mjs';", 'y.mjs': '', 'package.json': '{}' };
	assert.match(run(lib, copy('COPY package.json *.mjs ./')).problems.join('\n'), /lib\/x\.mjs: needed by the program but not copied/);
	assert.deepEqual(run(lib, copy('COPY package.json *.mjs ./\nCOPY lib ./lib')).problems, []);
	assert.deepEqual(run(lib, copy('COPY . .')).problems, []);
	assert.match(run(lib, copy('COPY . .'), 'lib\n').problems.join('\n'), /lib\/x\.mjs: needed by the program but not copied/, '.dockerignore removes it again');
	// forms it cannot follow are reported, never passed over
	const unknownCases = [
		['an import that leaves the folder', { 'server.mjs': "import '../outside.mjs';" }, /leaves the folder/],
		['an import without extension', { 'server.mjs': "import './a';", 'a.mjs': '' }, /no file extension/],
		['an import of a file that is not there (also what a comment looks like)', { 'server.mjs': "import './nope.mjs';" }, /is no file in the folder/],
		['import() of a variable', { 'server.mjs': 'await import(name);' }, /not a plain string/],
		['require', { 'server.mjs': "const x = require('./a.cjs');" }, /require/],
		['a file read through new URL', { 'server.mjs': "const u = new URL('./data.json', import.meta.url);" }, /read by URL/],
	];
	for (const [what, map, pattern] of unknownCases) assert.match(run({ ...map, 'package.json': '{}' }, copy('COPY package.json *.mjs ./')).problems.join('\n'), pattern, what);
	for (const [what, line, pattern] of [['COPY --from', 'COPY --from=build /out ./', /--from=/], ['the JSON form', 'COPY ["package.json", "server.mjs", "./"]', /JSON form/]]) {
		assert.match(run({ 'server.mjs': '', 'package.json': '{}' }, copy(line)).problems.join('\n'), pattern, what);
	}
	assert.match(run({ 'server.mjs': '', 'package.json': '{}' }, copy('COPY . .'), '*\n!server.mjs\n').problems.join('\n'), /"!" lines/);
	// flags and line continuations in a COPY are understood
	assert.deepEqual(run({ 'server.mjs': "import './a.mjs';", 'a.mjs': '', 'package.json': '{}' }, copy('COPY --chown=node:node package.json \\\n  server.mjs a.mjs ./')).problems, []);
});
