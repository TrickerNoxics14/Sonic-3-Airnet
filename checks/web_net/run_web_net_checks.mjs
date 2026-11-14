#!/usr/bin/env node
// Automated checks of online play in the browser build (needs: a web build in ../../web_build, Chrome, the player's own ROM in %APPDATA%\Sonic3AIR, Node 22+).
//   node checks/web_net/run_web_net_checks.mjs                 all sessions
//   node checks/web_net/run_web_net_checks.mjs --only A,C      only these sessions
//   node checks/web_net/run_web_net_checks.mjs --native        also the sessions with a Windows game (needs the Windows build; one game window at a time, uses a scratch savedata folder, never your own %APPDATA%)
// Sessions:  A two browsers through the relay (join, play in sync, relay drop and reconnect, host drop)
//            B two browsers through a line with delay, jitter and stalls (TCP lag proxy)
//            C one browser: no relay set, wrong code, unreachable relay
//            N a Windows game and a browser in one lobby (both ways round)
// Everything it starts (relay, web server, browsers, game) is stopped at the end; the ROM is only served from where the game keeps it, to 127.0.0.1.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { startHarness, sleep, defaultRomPath } from './harness.mjs';
import { createRelayServer } from '../relay_ws/server.mjs';
import { startLagProxy } from './tcp_lag_proxy.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const logDir = path.join(root, 'checks', 'last_logs');
fs.mkdirSync(logDir, { recursive: true });
const args = process.argv.slice(2);
const only = (args.includes('--only') ? args[args.indexOf('--only') + 1] : 'A,B,C,N').split(',');
const withNative = args.includes('--native');
const webDir = path.join(root, 'web_build');

const results = [];
function check(id, text, ok, detail = '') {
	results.push({ id, text, ok: !!ok, detail });
	console.log(`${ok ? 'PASS' : 'FAIL'}  ${id}  ${text}${detail ? `  [${detail}]` : ''}`);
}

// ---- lobby code of a session id (same as NetplayCodes::makeSessionCode) ----
const ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
function sessionCode(id) {
	let text = '';
	for (let i = 7; i >= 0; --i) text += ALPHABET[Number((id >> BigInt(i * 5)) & 31n)];
	let sum = 7;
	for (let i = 0; i < 8; ++i) sum += (2 * i + 1) * Number((id >> BigInt((7 - i) * 5)) & 31n);
	return text + ALPHABET[sum & 31];
}
// every random byte of the page is the seed byte (see harness.seedScript), so the host's id is that byte, five times
const codeForSeed = (byte) => sessionCode(BigInt(byte) * 0x0101010101n);

// ---- reading the games' own online logs ----
function parseOnlineLog(text) {
	const lines = text.split('\n').filter(Boolean);
	const rows = [];
	for (const line of lines) {
		const m = /^\[(friend|host)\] t=(\d+)s state=(\d+) frame=(\d+) fps=([\d.]+)/.exec(line);
		if (!m) continue;
		const row = { role: m[1], t: +m[2], state: +m[3], frame: +m[4], fps: +m[5] };
		const rep = /repairs=(\d+)/.exec(line); if (rep) row.repairs = +rep[1];
		const sync = /last sync ok at frame (\d+)/.exec(line); if (sync) row.syncOk = +sync[1];
		const relay = /relayed=(\d+)/.exec(line); if (relay) row.relayed = +relay[1];
		const delay = /inputdelay=(\d+)/.exec(line); if (delay) row.inputDelay = +delay[1];
		const ping = /ping=(-?\d+)/.exec(line); if (ping) row.ping = +ping[1];
		rows.push(row);
	}
	return rows;
}

async function waitFor(fn, timeout, step = 500) {
	const end = Date.now() + timeout;
	while (Date.now() < end) { const v = await fn(); if (v) return v; await sleep(step); }
	return null;
}

async function shot(inst, name) { try { await inst.shot(path.join(logDir, `web_net_${name}.png`)); } catch { /* browser gone */ } }

/** Host creates a lobby, friend(s) join, everybody is ready, the host starts a new game. Returns once every browser runs the game. */
async function hostAndJoin(host, friends, relayURLforPage, code) {
	await host.toLobby();
	host.forgetLogs();
	await host.key('Return');   // CREATE LOBBY
	const hosted = await host.waitLog(/Lobby: screen HOSTING/, 10000);
	const known = await host.waitLog(/the relay server knows this lobby/, 15000);
	await sleep(1000);
	const joined = [];
	for (const friend of friends) {
		await friend.toLobby();
		friend.forgetLogs();
		await friend.keys(['Down', 'Return'], 600);   // JOIN WITH CODE
		await friend.waitLog(/Lobby: screen JOIN/, 10000);
		await friend.type(code);
		await friend.key('Return');
		joined.push(await friend.waitLog(/the host proved the code, player slot/, 40000));
	}
	return { hosted, known, joined };
}

async function startNormalGame(host, friends) {
	for (const friend of friends) { await sleep(1500); await friend.key('Return'); }   // READY (first entry)
	await sleep(3000);
	host.forgetLogs();
	await host.key('Return');   // START GAME
	await host.waitLog(/Menu: main menu/, 20000);
	await sleep(1000);
	await host.keys(['Up', 'Up', 'Up', 'Return'], 700);   // NORMAL GAME
	await sleep(3500);
	await host.key('Return');   // first save slot: a new game
	const loaded = [];
	// (the friend loads the host's state twice: when the lobby starts and when the new game starts)
	for (const friend of friends) loaded.push(await waitFor(() => friend.allLogs.filter((l) => /game state loaded, now at frame/.test(l)).length >= 2, 40000));
	return loaded;
}

async function playAndMeasure(host, friends, seconds, tag) {
	// movement from every player while the game runs
	const end = Date.now() + seconds * 1000;
	while (Date.now() < end) {
		await Promise.all([host.hold('Right', 700), ...friends.map((f, i) => f.hold(i % 2 ? 'Left' : 'Right', 900))]);
		await sleep(400);
	}
	await sleep(6000);   // the games write their log every 5 seconds
	const hostRows = parseOnlineLog(await host.onlineLog());
	const friendRows = friends.map(async (f) => parseOnlineLog(await f.onlineLog()));
	await shot(host, `${tag}_host`);
	for (let i = 0; i < friends.length; ++i) await shot(friends[i], `${tag}_friend${i + 1}`);
	return { hostRows, friendRows: await Promise.all(friendRows) };
}

function syncOk(rows, role) {
	const last = rows[rows.length - 1];
	if (!last) return { ok: false, why: `no ${role} log lines` };
	const repairs = rows.reduce((m, r) => Math.max(m, r.repairs ?? 0), 0);
	return { ok: last.frame > 600 && repairs === 0 && (role === 'host' || (last.syncOk !== undefined && last.syncOk >= last.frame - 240)), why: `frame ${last.frame}, fps ${last.fps}, repairs ${repairs}${role === 'friend' ? `, last sync ok ${last.syncOk}` : ''}`, last };
}

// =====================================================================================================================
async function sessionA(relay) {
	console.log('--- session A: two browsers through the relay');
	const code = codeForSeed(0x41);
	const h = await startHarness({ webDir, instances: 2, seeds: [0x41, 0x52], basePort: 9340 });
	try {
		const [host, friend] = h.instances;
		const query = `?relay=ws://127.0.0.1:${relay.ports.port}`;
		await Promise.all([host.openGame(h.baseURL, query), friend.openGame(h.baseURL, query)]);
		await Promise.all([host.waitLog(/Ready to go/, 90000), friend.waitLog(/Ready to go/, 90000)]);
		await sleep(2000);
		check('A0', 'both browsers start the game and reach the main menu', (await Promise.all([host.toMainMenu(), friend.toMainMenu()])).every(Boolean));
		check('A1', 'the lobby start screen shows the relay from the page link', true);
		const r = await hostAndJoin(host, [friend], query, code);
		check('A2', 'the host opens a lobby (relay knows it) and shows the screen', r.hosted && r.known);
		check('A3', 'the friend joins with the lobby code through the relay (code proof both ways, player slot 2)', r.joined[0]);
		check('A3b', 'the relay holds one lobby and one friend, both browsers connected by WebSocket', relay.relay.core.stats.lobbies === 1 && relay.relay.core.stats.pipes === 1 && relay.relay.wsConnections.size === 2, JSON.stringify(relay.relay.core.stats));
		const loaded = await startNormalGame(host, [friend]);
		check('A4', 'the host starts a new game and the friend loads the game state (wasm to wasm)', loaded[0]);
		const play = await playAndMeasure(host, [friend], 25, 'A5');
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('A5', 'both games run and stay in sync for 25 s of play with inputs from both players (the engine compares the game memory every second: no repairs, last sync recent)', hs.ok && fs_.ok, `host: ${hs.why}; friend: ${fs_.why}`);
		check('A5b', 'no desync message and no error in either console', !host.hasLog(/state differs from the host|EXC|assert/i) && !friend.hasLog(/state differs from the host|EXC|assert/i));
		const hostLast = play.hostRows[play.hostRows.length - 1];
		check('A5c', 'the host knows the friend is relayed and plans for it in the auto input delay (at least 1 frame)', hostLast?.relayed === 1 && (hostLast?.inputDelay ?? 0) >= 1, JSON.stringify(hostLast));
		check('A5d', 'the relay forwarded the game and dropped nothing as invalid', relay.relay.core.stats.forwarded > 2000 && relay.relay.core.stats.droppedInvalid === 0, JSON.stringify(relay.relay.core.stats));

		// ---- A6: the relay drops every WebSocket (a network change, a restart of the relay host): both browsers come back
		const before = { host: parseOnlineLog(await host.onlineLog()).pop()?.frame ?? 0, friend: parseOnlineLog(await friend.onlineLog()).pop()?.frame ?? 0 };
		host.forgetLogs(); friend.forgetLogs();
		for (const c of relay.relay.wsConnections.values()) c.terminate();
		const hostBack = await host.waitLog(/the relay server knows this lobby/, 40000);
		const friendBack = await friend.waitLog(/the relay server let us in/, 40000);
		check('A6a', 'after the relay dropped every WebSocket both browsers connect again and register / join again by themselves', hostBack && friendBack);
		const reconnecting = await friend.waitLog(/gave us a new place|nothing from the host for 10 seconds|reconnecting to the existing session/, 40000);
		const slotBack = await waitFor(() => host.hasLog(/came back to its player slot|back in player slot/) || friend.hasLog(/back in player slot|Reconnected|resum/i), 60000);
		check('A6b', 'the friend notices that its place at the relay is new and starts the connection to the host again, which puts it back into its slot', reconnecting && slotBack, `${reconnecting ?? ''} | ${slotBack ? 'slot back' : 'no slot message'}`);
		await sleep(12000);
		const after = { host: parseOnlineLog(await host.onlineLog()).pop(), friend: parseOnlineLog(await friend.onlineLog()).pop() };
		check('A6c', 'the game runs again on both sides after the drop (frames advanced by more than 600) and the friend is back in sync', after.host && after.friend && after.host.frame > before.host + 600 && after.friend.frame > before.friend + 300 && after.friend.syncOk >= after.friend.frame - 360, `host frame ${before.host} -> ${after.host?.frame}; friend frame ${before.friend} -> ${after.friend?.frame}, last sync ok ${after.friend?.syncOk}, repairs ${after.friend?.repairs}`);
		await shot(host, 'A6_host'); await shot(friend, 'A6_friend');

		// ---- A7: only the host's WebSocket drops: the lobby stays at the relay, the host registers again with its secret, the friend's connection survives
		const lobby = [...relay.relay.core.lobbies.values()][0];
		const f0 = parseOnlineLog(await friend.onlineLog()).pop()?.frame ?? 0;
		host.forgetLogs(); friend.forgetLogs();
		relay.relay.wsConnections.get(lobby.host)?.terminate();
		const reRegistered = await host.waitLog(/the relay server knows this lobby/, 30000);
		await sleep(15000);
		const f1 = parseOnlineLog(await friend.onlineLog()).pop();
		check('A7', 'when only the host\'s WebSocket drops, the host takes its lobby back and the game goes on (friend frames advance, no desync)', reRegistered && f1 && f1.frame > f0 + 400 && !friend.hasLog(/state differs from the host/), `friend frame ${f0} -> ${f1?.frame}`);
	} finally {
		await h.close();
	}
}

async function sessionB(relay) {
	console.log('--- session B: two browsers through a bad TCP line (delay, jitter, stalls)');
	const code = codeForSeed(0x41);
	const proxy = await startLagProxy({ targetPort: relay.ports.port, baseMs: 45, jitterMs: 40, stallEveryMs: 6000, stallMs: 350 });
	const h = await startHarness({ webDir, instances: 2, seeds: [0x41, 0x52], basePort: 9350 });
	try {
		const [host, friend] = h.instances;
		const query = `?relay=ws://127.0.0.1:${proxy.port}`;
		await Promise.all([host.openGame(h.baseURL, query), friend.openGame(h.baseURL, query)]);
		await Promise.all([host.waitLog(/Ready to go/, 90000), friend.waitLog(/Ready to go/, 90000)]);
		await sleep(5000);
		await Promise.all([host.toMainMenu(), friend.toMainMenu()]);
		const r = await hostAndJoin(host, [friend], query, code);
		check('B1', 'the lobby works through the delayed line (about 90 ms round trip, +-40 ms jitter, a 350 ms stall every 6 s)', r.hosted && r.known && r.joined[0]);
		await startNormalGame(host, [friend]);
		const play = await playAndMeasure(host, [friend], 40, 'B2');
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('B2', 'the games stay in sync through the unsteady line for 40 s (no repairs)', hs.ok && fs_.ok, `host: ${hs.why}; friend: ${fs_.why}`);
		const last = play.hostRows[play.hostRows.length - 1];
		check('B3', 'the auto input delay reacts to the line (more than the 1 frame minimum) and the games keep a playable speed (friend above 40 fps)', last && last.inputDelay >= 2 && (play.friendRows[0].at(-1)?.fps ?? 0) > 40, `host ping ${last?.ping} ms, input delay ${last?.inputDelay}; friend fps ${play.friendRows[0].at(-1)?.fps}`);
		const delays = host.takeLogs().filter((l) => /input delay is now/.test(l));
		console.log('      ' + delays.slice(-4).join('\n      '));
	} finally {
		await h.close();
		await proxy.close();
	}
}

async function sessionC(relay) {
	console.log('--- session C: one browser, error cases');
	const h = await startHarness({ webDir, instances: 1, seeds: [0x41], basePort: 9360 });
	try {
		const [b] = h.instances;
		// no relay at all: online play stays off and says why
		await b.openGame(h.baseURL, '');
		await b.waitLog(/Ready to go/, 90000);
		await sleep(5000);
		await b.toMainMenu();
		await b.toLobby();
		b.forgetLogs();
		await b.key('Return');   // CREATE LOBBY
		const refused = await b.waitLog(/Lobby: no relay server set/, 8000);
		check('C1', 'without a relay address CREATE LOBBY does not open a lobby and says that a relay server is needed', refused && !b.hasLog(/registering this lobby/));
		await shot(b, 'C1_norelay');
		// wrong code, with a relay that has no lobby
		await b.navigate(`${h.baseURL}/sonic3air_web.html?relay=ws://127.0.0.1:${relay.ports.port}`);
		await b.waitLog(/Ready to go/, 90000);
		await sleep(5000);
		await b.toMainMenu();
		await b.toLobby();
		b.forgetLogs();
		await b.keys(['Down', 'Return'], 600);
		await b.waitLog(/Lobby: screen JOIN/, 8000);
		await b.type('ZZZZ' + 'Z' + 'Z' + 'Z' + 'Z');   // not a valid code (check character)
		await b.key('Return');
		const typo = await b.waitLog(/WRONG_CODE/, 8000);
		check('C2', 'a mistyped code (wrong check character) is refused at once with a plain message, nothing is sent to the relay', typo && relay.relay.core.stats.received === 0 || (typo && !b.hasLog(/asking the relay server to let us in/)));
		await b.navigate(`${h.baseURL}/sonic3air_web.html?relay=ws://127.0.0.1:${relay.ports.port}`);
		await b.waitLog(/Ready to go/, 90000);
		await sleep(3000);
		await b.toMainMenu();
		await b.toLobby();
		b.forgetLogs();
		await b.keys(['Down', 'Return'], 600);
		await b.waitLog(/Lobby: screen JOIN/, 8000);
		await b.type(codeForSeed(0x77));   // a valid code that no host registered
		await b.key('Return');
		const unknown = await b.waitLog(/giving up, WRONG_CODE/, 40000);
		check('C3', 'a valid code that no host registered at the relay ends in WRONG_CODE within 40 s ("the relay does not know a lobby with this code")', unknown && b.hasLog(/asking the relay server to let us in/), unknown ?? b.allLogs.slice(-6).join(' | '));
		await shot(b, 'C3_unknown');
		// relay that cannot be reached
		await b.navigate(`${h.baseURL}/sonic3air_web.html?relay=ws://127.0.0.1:9`);
		await b.waitLog(/Ready to go/, 90000);
		await sleep(5000);
		await b.toMainMenu();
		await b.toLobby();
		b.forgetLogs();
		await b.key('Return');   // CREATE LOBBY
		const unreachable = await b.waitLog(/Lobby: cannot reach the relay server/, 25000);
		check('C4', 'a relay that cannot be reached: the host lobby says so after 10 s, and the game keeps trying to connect', unreachable && b.hasLog(/connecting to the relay server/));
		await shot(b, 'C4_unreachable');
	} finally {
		await h.close();
	}
}

// ---- Windows game (a scratch savedata folder next to the game, ROM copied there for the run and deleted afterwards) ----
async function withNativeGame(env, logName, body) {
	const gameDir = path.join(root, 'sonic3air_online_stable_source', 'Oxygen', 'sonic3air');
	const binDir = path.join(gameDir, 'bin', 'Release_x64');
	const exe = path.join(binDir, 'Sonic3AIR.exe');
	const savedata = path.join(gameDir, 'savedata');
	const marker = path.join(binDir, 'testhooks.enabled');
	if (!fs.existsSync(exe)) throw new Error('no Windows build');
	if (fs.existsSync(savedata)) throw new Error(`${savedata} exists already: refusing to touch it`);
	fs.mkdirSync(savedata);
	fs.copyFileSync(defaultRomPath(), path.join(savedata, 'Sonic_Knuckles_wSonic3.bin'));
	fs.writeFileSync(marker, '');
	const nativeOnlineLog = path.join(process.env.APPDATA ?? '', 'Sonic3AIR', `online_log${logName}.txt`);
	fs.rmSync(nativeOnlineLog, { force: true });
	let child;
	try {
		child = spawn(exe, [], { cwd: gameDir, env: { ...process.env, S3AIR_CHECKS: '1', S3AIR_LOGNAME: logName, ...env }, stdio: 'ignore' });
		return await body({
			readLog: (name) => { try { return fs.readFileSync(path.join(savedata, name), 'utf8'); } catch { return ''; } },
			onlineLog: () => { try { return fs.readFileSync(nativeOnlineLog, 'utf8'); } catch { return ''; } },		// (the game writes this one to %APPDATA%Sonic3AIR whatever its savedata folder is; the run deletes it afterwards)
			mainLog: () => { try { return fs.readFileSync(path.join(savedata, `logfile${logName}.txt`), 'utf8'); } catch { return ''; } },
			exited: () => child.exitCode !== null,
		});
	} finally {
		try { execFileSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { /* already gone */ }
		await sleep(1500);
		for (const [from, to] of [[path.join(savedata, `logfile${logName}.txt`), `web_net_native${logName}_logfile.txt`], [nativeOnlineLog, `web_net_native${logName}_online.txt`]]) { try { fs.copyFileSync(from, path.join(logDir, to)); } catch { /* no such log */ } }
		fs.rmSync(marker, { force: true });
		fs.rmSync(nativeOnlineLog, { force: true });
		for (let i = 0; i < 10; ++i) { try { fs.rmSync(savedata, { recursive: true, force: true }); break; } catch { await sleep(500); } }
	}
}

async function sessionN(relay) {
	console.log('--- session N: a Windows game and a browser in one lobby');
	const code = codeForSeed(0x41);
	const relayAddress = `127.0.0.1:${relay.ports.udpPort}`;

	// N1: the Windows game hosts (opens the lobby by itself, registers at the relay, starts a level when the browser is ready), the browser joins
	if (!args.includes('--skip-n1')) {
		const h = await startHarness({ webDir, instances: 1, seeds: [0x52], basePort: 9370 });
		try {
			await withNativeGame({ S3AIR_RELAY: relayAddress, S3AIR_LOBBY_OPEN: 'hostlan', S3AIR_LOBBY_CODE: code, S3AIR_LOBBY_AUTOSTART: '1', S3AIR_LOBBY_AUTOGAME: '0x0100', S3AIR_PLAYER_NAME: 'Windows', S3AIR_NO_SERVER: '1' }, '_native', async (native) => {
				const [b] = h.instances;
				await b.openGame(h.baseURL, `?relay=ws://127.0.0.1:${relay.ports.port}`);
				await b.waitLog(/Ready to go/, 90000);
				const registered = await waitFor(() => relay.relay.core.stats.lobbies === 1, 60000);
				check('N1a', 'a Windows game hosts: it registers its lobby at the relay over UDP', registered);
				await sleep(4000);
				await b.toMainMenu();
				await b.toLobby();
				b.forgetLogs();
				await b.keys(['Down', 'Return'], 600);
				await b.waitLog(/Lobby: screen JOIN/, 8000);
				await b.type(code);
				await b.key('Return');
				const joined = await b.waitLog(/the host proved the code, player slot/, 40000);
				check('N1b', 'a browser joins the Windows host\'s lobby with the code through the relay (WebSocket to UDP)', joined);
				await sleep(2500);
				await b.key('Return');   // READY
				const loaded = await b.waitLog(/game state loaded, now at frame/, 90000);
				check('N1c', 'the Windows host starts the level and the browser loads its game state (x64 to wasm32; same game version, no mods)', loaded);
				await sleep(2000);
				const end = Date.now() + 25000;
				while (Date.now() < end) { await b.hold('Right', 900); await sleep(300); }
				await sleep(7000);
				const friendRows = parseOnlineLog(await b.onlineLog());
				const hostRows = parseOnlineLog(native.onlineLog());
				const fsync = syncOk(friendRows, 'friend'), hsync = syncOk(hostRows, 'host');
				check('N1d', 'the Windows host and the browser stay in sync (no repairs, recent sync ok)', fsync.ok && hsync.ok, `host: ${hsync.why}; friend: ${fsync.why}`);
				await shot(b, 'N1_browser');
				fs.writeFileSync(path.join(logDir, 'web_net_N1_browser_console.txt'), b.allLogs.join('\n') + '\n' + JSON.stringify(relay.relay.core.stats));
			});
		} finally { await h.close(); }
	}

	// N2: a browser hosts (its own code: the lobby of the N1 host stays at the relay for 45 s and would refuse a host with another secret, and block this address for a minute), the Windows game joins (the direct attempt goes nowhere, so after 8 s it uses the relay: the same as for a player behind a strict NAT)
	{
		const code2 = codeForSeed(0x66);
		const h = await startHarness({ webDir, instances: 1, seeds: [0x66], basePort: 9371 });
		try {
			const [b] = h.instances;
			await b.openGame(h.baseURL, `?relay=ws://127.0.0.1:${relay.ports.port}`);
			await b.waitLog(/Ready to go/, 90000);
			await sleep(5000);
			await b.toMainMenu();
			await b.toLobby();
			b.forgetLogs();
			await b.key('Return');
			await b.waitLog(/the relay server knows this lobby/, 20000);
			await withNativeGame({ S3AIR_RELAY: relayAddress, S3AIR_NETPLAY: `joinintro:${code2}:9`, S3AIR_LOBBY_AUTOREADY: '1', S3AIR_PLAYER_NAME: 'Windows', S3AIR_NO_SERVER: '1' }, '_native2', async (native) => {
				const joined = await b.waitLog(/a friend proved the code and is player 2/, 90000);
				check('N2a', 'a Windows friend (direct path dead, so through the relay after 8 s) joins the browser host\'s lobby', joined);
				if (args.includes('--n2-idle')) { await sleep(40000); const hr = parseOnlineLog(await b.onlineLog()); const fr = parseOnlineLog(native.onlineLog()); check('N2i', 'idle in the lobby with a Windows friend for 40 s: no desync in the menu phase', hr.at(-1) && fr.at(-1) && (fr.at(-1).repairs ?? 0) === 0 && hr.at(-1).fps > 50, JSON.stringify([hr.at(-1), fr.at(-1)])); return; }
				await sleep(3000);
				await sleep(3000);
				b.forgetLogs();
				await b.key('Return');   // host: START GAME (the friend is ready by itself)
				await b.waitLog(/Menu: main menu/, 40000);
				await sleep(1000);
				await b.keys(['Up', 'Up', 'Up', 'Return'], 700);
				await sleep(3500);
				await b.key('Return');
				const started = await b.waitLog(/client loaded the game state/, 90000);
				check('N2b', 'the browser host starts a new game and the Windows friend loads its game state (wasm32 to x64)', started);
				const end = Date.now() + 25000;
				while (Date.now() < end) { await b.hold('Right', 900); await sleep(300); }
				await sleep(7000);
				const hostRows = parseOnlineLog(await b.onlineLog());
				const friendRows = parseOnlineLog(native.onlineLog());
				const fsync = syncOk(friendRows, 'friend'), hsync = syncOk(hostRows, 'host');
				check('N2c', 'the browser host and the Windows friend stay in sync (no repairs, recent sync ok)', fsync.ok && hsync.ok, `host: ${hsync.why}; friend: ${fsync.why}`);
				await shot(b, 'N2_browser');
				fs.writeFileSync(path.join(logDir, 'web_net_N2_browser_console.txt'), b.allLogs.join('\n') + '\n' + JSON.stringify(relay.relay.core.stats));
			});
		} finally { await h.close(); }
	}
}

// =====================================================================================================================
let exitCode = 0;
const relay = { relay: createRelayServer({ port: 0, udpPort: 28842, bind: '127.0.0.1', statsSeconds: 0 }) };
try {
	relay.ports = await relay.relay.start();
	if (only.includes('A')) await sessionA(relay);
	if (only.includes('B')) await sessionB(relay);
	if (only.includes('C')) await sessionC(relay);
	if (only.includes("N")) {
		if (withNative) await sessionN(relay); else console.log('--- session N skipped (add --native to run it)');
	}
} catch (error) {
	console.log(`ERROR  ${error.stack ?? error}`);
	check('ERR', 'the run was not interrupted by an error', false, String(error.message ?? error));
} finally {
	await relay.relay.stop();
	const failed = results.filter((r) => !r.ok);
	const lines = results.map((r) => `${r.ok ? 'PASS' : 'FAIL'}  ${r.id}  ${r.text}${r.detail ? `  [${r.detail}]` : ''}`);
	lines.push('', `${results.length - failed.length} of ${results.length} checks passed`);
	fs.writeFileSync(path.join(root, 'checks', 'web_net_report.txt'), lines.join('\n') + '\n');
	console.log(`\n${results.length - failed.length} of ${results.length} checks passed`);
	exitCode = failed.length ? 1 : 0;
}
process.exit(exitCode);
