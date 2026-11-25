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
	// the process still answers after the refusals
	assert.equal((await healthOf(program)).ok, true);
});

test('origins: the setting may be written the way people copy it (capital letters, trailing slash, a page path, spaces, several entries); a value that is no origin stops the program at the start', async (t) => {
	const program = await startProgram({ ALLOWED_ORIGINS: ' https://TrickerNoxics14.github.io/Sonic-3-Airnet/ , https://other.example/ ' });
	t.after(() => program.stop());
	const status = async (origin) => { const peer = await RawWsPeer.open(program.port, { Origin: origin }); peer.close(); return peer.status; };
	assert.equal(await status(GAME_PAGE), 101, 'the browser sends the lower case origin without a path');
	assert.equal(await status('https://other.example'), 101);
	assert.equal(await status('https://example.org'), 403);

	// a host name without the scheme can never match: it must not silently turn the list off (every page allowed) either
	for (const bad of ['trickernoxics14.github.io', '*', 'https://']) {
		const stopped = await startProgram({ ALLOWED_ORIGINS: bad });
		t.after(() => stopped.stop());
		for (let i = 0; i < 100 && stopped.exitCode === null; ++i) await sleep(25);
		assert.equal(stopped.exitCode, 1, `"${bad}" stops the program`);
		assert.match(stopped.text, /ALLOWED_ORIGINS/);
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
	assert.equal(playerAddress('10.0.0.1', '1.2.3.4', 2), '1.2.3.4', 'fewer entries than proxies: the first one');
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

test('proxy_check.mjs (run by the operator against the deployed relay): OK when claimed addresses make no new address, PROBLEM when the relay believes them, usage error otherwise', async (t) => {
	const run = (args) => new Promise((resolve) => {
		const child = spawn(process.execPath, ['proxy_check.mjs', ...args], { cwd: relayDir, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
		let text = '';
		child.stdout.on('data', (c) => { text += c; });
		child.stderr.on('data', (c) => { text += c; });
		child.on('exit', (code) => resolve({ code, text }));
	});
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
	assert.equal((await run([])).code, 2);
	assert.equal((await run(['https://example.org'])).code, 2);
});

// ---- 6. the container image ----

/** The local .mjs files that server.mjs needs, found the way Node finds them (import ... from './x.mjs'). */
function importedFiles(start = 'server.mjs', seen = new Set()) {
	if (seen.has(start)) return seen;
	seen.add(start);
	const source = fs.readFileSync(path.join(relayDir, start), 'utf8');
	for (const match of source.matchAll(/(?:from|import)\s*\(?\s*'\.\/([\w.-]+\.mjs)'/g)) importedFiles(match[1], seen);
	return seen;
}

/** What the Dockerfile copies into the image: the sources of its COPY lines (a * stands for any characters). */
function dockerCopies() {
	const lines = fs.readFileSync(path.join(relayDir, 'Dockerfile'), 'utf8').split(/\r?\n/).filter((line) => /^COPY\s/i.test(line.trim()));
	return lines.flatMap((line) => line.trim().split(/\s+/).slice(1, -1));
}

test('container image: the Dockerfile copies every file that server.mjs needs, and a folder with exactly those files starts and answers', async (t) => {
	const wanted = [...importedFiles()];
	assert.ok(wanted.includes('directory.mjs') && wanted.includes('relay_core.mjs') && wanted.includes('ws.mjs') && wanted.includes('protocol.mjs'), `found ${wanted}`);
	const patterns = dockerCopies().map((p) => new RegExp(`^${p.replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '[^/]*')}$`));
	const inImage = fs.readdirSync(relayDir).filter((name) => fs.statSync(path.join(relayDir, name)).isFile() && patterns.some((re) => re.test(name)));
	for (const file of wanted) assert.ok(inImage.includes(file), `the Dockerfile does not copy ${file}, so the image cannot start`);
	assert.ok(inImage.includes('package.json'), 'package.json (the "type": "module" setting) is in the image');

	const image = fs.mkdtempSync(path.join(os.tmpdir(), 's3air-image-'));
	for (const file of inImage) fs.copyFileSync(path.join(relayDir, file), path.join(image, file));
	const program = await startProgram({ ALLOWED_ORIGINS: undefined }, image);
	t.after(async () => { await program.stop(); fs.rmSync(image, { recursive: true, force: true }); });   // the folder cannot be removed while the program runs in it
	assert.equal(program.exitCode, null, `the program in the image ended: ${program.text}`);
	assert.equal((await healthOf(program)).ok, true);
});
