// A real browser against the relay started the way a free host starts it (UDP off, one proxy trusted, only the game page's origin allowed).
// Not part of "npm test" (it needs Chrome and, for the exact GitHub Pages origin, openssl): run it by hand
//     node test/browser_origin_check.mjs
// What it does, in this order, one headless Chrome at a time, everything on 127.0.0.1 and removed afterwards:
//   1. starts `node server.mjs` with UDP_PORT=0 TRUST_PROXY=1 ALLOWED_ORIGINS=https://trickernoxics14.github.io (a free port, bound to 127.0.0.1)
//   2. serves a test page whose address in the browser is exactly https://trickernoxics14.github.io (a throw-away self-signed certificate and
//      Chrome's --host-resolver-rules; without openssl a plain http page on another 127.0.0.1 port is used and that origin is allowed instead)
//   3. that page opens two WebSockets to the relay (another origin than the relay's own), a host publishes a lobby, a friend lists it, resolves its
//      code, joins, and both send signalling and data; it also reads /health from the other origin
//   4. a second page on another origin (http://localhost:<port>) tries the same WebSocket: the relay must refuse it
// It prints one line per check and exits with 1 if one failed. It never touches the game, the game's ROM or any settings.
import { spawn, execFileSync } from 'node:child_process';
import http from 'node:http';
import https from 'node:https';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { findChrome } from '../../web_net/harness.mjs';

const relayDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const freePort = () => new Promise((resolve) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => resolve(port)); }); });
const GITHUB_ORIGIN = 'https://trickernoxics14.github.io';

// ---- the page's script: the game's relay protocol in a few lines (see protocol.mjs) ----
const PAGE_SCRIPT = `
const T = { HOST_REGISTER: 1, HOST_REGISTERED: 2, CLIENT_JOIN: 3, CLIENT_JOINED: 4, DATA_C2R: 6, DATA_R2C: 7, DATA_H2R: 8, DATA_R2H: 9, SIGNAL_C2R: 10, SIGNAL_R2C: 11, SIGNAL_H2R: 12, SIGNAL_R2H: 13,
	HELLO: 16, HELLO_REPLY: 17, PUBLISH: 18, PUBLISH_REPLY: 19, LIST: 21, LIST_REPLY: 22, RESOLVE: 23, RESOLVE_REPLY: 24 };
const enc = new TextEncoder(), dec = new TextDecoder();
const concat = (...parts) => { const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0)); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } return out; };
const u8 = (v) => Uint8Array.of(v), u16 = (v) => Uint8Array.of(v >> 8, v & 255);
const u32 = (v) => Uint8Array.of(v >>> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
const u64 = (v) => { const b = new Uint8Array(8); new DataView(b.buffer).setBigUint64(0, BigInt(v)); return b; };
const str = (s) => concat(u8(s.length), enc.encode(s));
const msg = (type, ...payload) => concat(Uint8Array.of(0x53, 0x33, 0x52, 0x4c, 2, type), ...payload);
const view = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);

class Client {
	constructor(url) { this.url = url; this.queue = []; this.waiters = []; }
	open() {
		return new Promise((resolve) => {
			this.ws = new WebSocket(this.url);
			this.ws.binaryType = 'arraybuffer';
			this.ws.onmessage = (e) => { const b = new Uint8Array(e.data); const w = this.waiters.shift(); if (w) w(b); else this.queue.push(b); };
			this.ws.onclose = (e) => { this.closeCode = e.code; if (!this.everOpened) resolve({ opened: false, closeCode: e.code }); };
			this.ws.onopen = () => { this.everOpened = true; resolve({ opened: true }); };
		});
	}
	send(b) { this.ws.send(b); }
	next(type, ms = 3000) {
		return new Promise((resolve) => {
			const take = (b) => (b[5] === type ? resolve(b) : this.next(type, ms).then(resolve));
			if (this.queue.length) { take(this.queue.shift()); return; }
			const w = (b) => { clearTimeout(t); take(b); };
			const t = setTimeout(() => { this.waiters = this.waiters.filter((x) => x !== w); resolve(null); }, ms);
			this.waiters.push(w);
		});
	}
}

window.runOther = async (relay) => { const c = new Client(relay); const r = await c.open(); try { c.ws.close(); } catch (e) { /* already closed */ } return r; };

window.runAll = async (relay, healthURL) => {
	const out = {};
	const host = new Client(relay), friend = new Client(relay);
	out.hostOpen = (await host.open()).opened; out.friendOpen = (await friend.open()).opened;
	if (!out.hostOpen || !out.friendOpen) return out;
	const LOBBY = 0x1122334455667788n, KEY = 0x0102030405060708n, SECRET = 0xdeadbeefcafef00dn;
	host.send(msg(T.HOST_REGISTER, u64(LOBBY), u64(KEY), u64(SECRET)));
	const reg = await host.next(T.HOST_REGISTERED); out.registered = !!reg; const hostToken = reg ? view(reg).getUint32(6 + 8) : 0;
	host.send(msg(T.PUBLISH, u8(1), u64(LOBBY), u64(KEY), u64(SECRET), u64(0xabcdef12345n), u8(1), u8(1), u8(4), str('abcd1234'), str('Browser Room'), str('Host')));
	const pub = await host.next(T.PUBLISH_REPLY); out.publishStatus = pub ? pub[7] : -1; const code = pub ? dec.decode(pub.subarray(9, 9 + pub[8])) : ''; out.code = code;
	friend.send(msg(T.HELLO, u8(1), new Uint8Array(15)));
	const hello = await friend.next(T.HELLO_REPLY); const cookie = hello ? view(hello).getBigUint64(6 + 6) : 0n; out.helloOk = !!hello;
	friend.send(msg(T.LIST, u8(1), u64(cookie), u8(0), u8(0), str('abcd1234')));
	const list = await friend.next(T.LIST_REPLY); out.listStatus = list ? list[7] : -1; out.listed = list ? view(list).getUint16(8) : -1;
	friend.send(msg(T.RESOLVE, u8(1), u64(cookie), str(code)));
	const res = await friend.next(T.RESOLVE_REPLY); out.resolveStatus = res ? res[7] : -1; out.session = res ? view(res).getBigUint64(8).toString(16) : '';
	friend.send(msg(T.CLIENT_JOIN, u64(LOBBY), u64(KEY)));
	const joined = await friend.next(T.CLIENT_JOINED); const token = joined ? view(joined).getUint32(6) : 0; out.joined = !!joined;
	friend.send(msg(T.SIGNAL_C2R, u32(token), enc.encode('offer')));
	const s1 = await host.next(T.SIGNAL_R2H); out.signalToHost = s1 ? dec.decode(s1.subarray(6 + 6)) : null;
	host.send(msg(T.SIGNAL_H2R, u32(hostToken), u16(1), enc.encode('answer')));
	const s2 = await friend.next(T.SIGNAL_R2C); out.signalToFriend = s2 ? dec.decode(s2.subarray(6 + 4)) : null;
	friend.send(msg(T.DATA_C2R, u32(token), enc.encode('input')));
	const d1 = await host.next(T.DATA_R2H); out.dataToHost = d1 ? dec.decode(d1.subarray(6 + 6)) : null;
	host.send(msg(T.DATA_H2R, u32(hostToken), u16(1), enc.encode('state')));
	const d2 = await friend.next(T.DATA_R2C); out.dataToFriend = d2 ? dec.decode(d2.subarray(6 + 4)) : null;
	try { const h = await fetch(healthURL); out.health = await h.json(); } catch (e) { out.healthError = String(e); }
	host.ws.close(); friend.ws.close();
	return out;
};
`;

// ---- Chrome over the DevTools protocol ----
async function drive(chrome, debugPort, profile, extraArgs, steps) {
	const child = spawn(chrome, ['--headless=new', `--remote-debugging-port=${debugPort}`, `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', ...extraArgs, 'about:blank'], { stdio: 'ignore', windowsHide: true });
	try {
		let targets;
		for (let i = 0; i < 100 && !targets; ++i) {
			try { const list = await (await fetch(`http://127.0.0.1:${debugPort}/json`)).json(); if (list.find((t) => t.type === 'page')) targets = list; } catch { /* not up yet */ }
			if (!targets) await sleep(200);
		}
		if (!targets) throw new Error('Chrome did not start');
		const ws = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
		await new Promise((resolve) => { ws.onopen = resolve; });
		let nextID = 0;
		const pending = new Map();
		ws.onmessage = (event) => { const m = JSON.parse(event.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
		const send = (method, params = {}) => new Promise((resolve) => { const id = ++nextID; pending.set(id, resolve); ws.send(JSON.stringify({ id, method, params })); });
		const evaluate = async (expression) => {
			const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
			if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description ?? 'evaluation failed');
			return r.result.result.value;
		};
		await send('Page.enable');
		return await steps({ send, evaluate, navigate: async (url) => { await send('Page.navigate', { url }); await sleep(1500); } });
	} finally {
		child.kill();
		await sleep(500);
	}
}

// ---- main ----
const results = [];
const check = (name, ok, detail = '') => { results.push(ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`); };

const chrome = findChrome();
if (!chrome) { console.error('No Chrome found (set CHROME to its path)'); process.exit(2); }
const work = fs.mkdtempSync(path.join(os.tmpdir(), 's3air-relay-browser-'));
const stops = [];
try {
	// the certificate for the GitHub Pages name (throw-away; only this Chrome is told to accept it)
	let tls = null;
	try {
		execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-keyout', path.join(work, 'key.pem'), '-out', path.join(work, 'cert.pem'), '-subj', '/CN=trickernoxics14.github.io', '-addext', 'subjectAltName=DNS:trickernoxics14.github.io'], { stdio: 'ignore' });
		tls = { key: fs.readFileSync(path.join(work, 'key.pem')), cert: fs.readFileSync(path.join(work, 'cert.pem')) };
	} catch { console.log('openssl not found: the page will be served as http://127.0.0.1:<port> and that origin is allowed instead of the GitHub Pages one'); }

	const page = (request, response) => { response.writeHead(200, { 'Content-Type': 'text/html' }); response.end(`<!doctype html><meta charset="utf-8"><title>relay check</title><script>${PAGE_SCRIPT}</script>`); };
	const allowedServer = tls ? https.createServer(tls, page) : http.createServer(page);
	await new Promise((resolve) => allowedServer.listen(0, '127.0.0.1', resolve));
	stops.push(() => allowedServer.close());
	const allowedPort = allowedServer.address().port;
	const allowedOrigin = tls ? GITHUB_ORIGIN : `http://127.0.0.1:${allowedPort}`;
	const otherServer = http.createServer(page);   // the page of another origin: http://localhost:<port>
	await new Promise((resolve) => otherServer.listen(0, '127.0.0.1', resolve));
	stops.push(() => otherServer.close());
	const otherPort = otherServer.address().port;

	// the relay as a free host starts it (bound to the loopback address here so that nothing outside this PC can reach it)
	const port = await freePort();
	const env = { ...process.env, PORT: String(port), BIND: '127.0.0.1', UDP_PORT: '0', TRUST_PROXY: '1', ALLOWED_ORIGINS: allowedOrigin, STATS_SECONDS: '0' };
	const relay = spawn(process.execPath, ['server.mjs'], { cwd: relayDir, env, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
	let relayText = '';
	relay.stdout.on('data', (c) => { relayText += c; });
	relay.stderr.on('data', (c) => { relayText += c; });
	stops.push(() => relay.kill());
	for (let i = 0; i < 100 && !/s3air_relay: WebSocket/.test(relayText); ++i) await sleep(50);
	check('the relay started with UDP off', /UDP off/.test(relayText), relayText.trim());
	// control: the same relay without a list of origins lets every page in, so the refusal below comes from the list and not from the browser or the test page
	const openPort = await freePort();
	const openRelay = spawn(process.execPath, ['server.mjs'], { cwd: relayDir, env: { ...env, PORT: String(openPort), ALLOWED_ORIGINS: '' }, stdio: 'ignore', windowsHide: true });
	stops.push(() => openRelay.kill());
	await sleep(700);
	const relayURL = `ws://127.0.0.1:${port}`;
	const healthURL = `http://127.0.0.1:${port}/health`;

	// 1. the page of the allowed origin
	const hostRule = tls ? [`--host-resolver-rules=MAP trickernoxics14.github.io 127.0.0.1:${allowedPort}`, '--ignore-certificate-errors'] : [];
	const profile = fs.mkdtempSync(path.join(work, 'profile-'));
	const result = await drive(chrome, 9461, profile, hostRule, async ({ navigate, evaluate }) => {
		await navigate(tls ? `${GITHUB_ORIGIN}/` : `http://127.0.0.1:${allowedPort}/`);
		const origin = await evaluate('location.origin');
		const all = await evaluate(`window.runAll(${JSON.stringify(relayURL)}, ${JSON.stringify(healthURL)})`);
		// 2. the same browser, a page of another origin
		await navigate(`http://localhost:${otherPort}/`);
		const otherOrigin = await evaluate('location.origin');
		const other = await evaluate(`window.runOther(${JSON.stringify(relayURL)})`);
		const otherControl = await evaluate(`window.runOther(${JSON.stringify(`ws://127.0.0.1:${openPort}`)})`);
		return { origin, all, otherOrigin, other, otherControl };
	});
	check(`the test page really has the origin ${allowedOrigin}`, result.origin === allowedOrigin, result.origin);
	const a = result.all;
	check('WebSockets from that page connect', a.hostOpen && a.friendOpen);
	check('host registered, published, got a 6 digit code', a.registered && a.publishStatus === 0 && /^[1-9]\d{5}$/.test(a.code), `status ${a.publishStatus}, code ${a.code}`);
	check('friend: hello, list shows the lobby, resolve gives the session', a.helloOk && a.listStatus === 0 && a.listed === 1 && a.resolveStatus === 0 && a.session === 'abcdef12345', `list ${a.listed}, session ${a.session}`);
	check('friend joined the relay lobby', a.joined);
	check('signalling both ways', a.signalToHost === 'offer' && a.signalToFriend === 'answer', `${a.signalToHost} / ${a.signalToFriend}`);
	check('game data both ways', a.dataToHost === 'input' && a.dataToFriend === 'state', `${a.dataToHost} / ${a.dataToFriend}`);
	check('/health is readable from the page (another origin) and shows counters only', a.health?.ok === true && a.health.listed === 1 && a.health.signals === 2 && !JSON.stringify(a.health).includes('127.0.0.1'), JSON.stringify(a.health));
	check(`a page of another origin (${result.otherOrigin}) is refused`, result.other.opened === false && result.other.closeCode === 1006, JSON.stringify(result.other));
	check('control: a relay without a list of origins lets that same page in', result.otherControl.opened === true, JSON.stringify(result.otherControl));
	const after = await (await fetch(healthURL)).json();
	check('the relay is still serving after the refusal', after.ok === true && after.websocketClients <= 2, `websocketClients ${after.websocketClients}`);
	check('the relay printed no address or player data', !/127\.0\.0\.1|github\.io|localhost/.test(relayText.replace(/^s3air_relay: WebSocket.*$/m, '')), relayText.trim());
} finally {
	for (const stop of stops.reverse()) { try { stop(); } catch { /* gone */ } }
	await sleep(500);
	for (let i = 0; i < 5; ++i) { try { fs.rmSync(work, { recursive: true, force: true }); break; } catch { await sleep(500); } }
}
const failed = results.filter((ok) => !ok).length;
console.log(`${results.length - failed} of ${results.length} checks passed`);
process.exit(failed ? 1 : 0);
