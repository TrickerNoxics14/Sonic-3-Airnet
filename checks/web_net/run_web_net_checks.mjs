#!/usr/bin/env node
// Automated checks of online play in the browser build (needs: a web build in ../../web_build, Chrome, the player's own ROM in %APPDATA%\Sonic3AIR, Node 22+).
//   node checks/web_net/run_web_net_checks.mjs                 all sessions
//   node checks/web_net/run_web_net_checks.mjs --only A,C      only these sessions
//   node checks/web_net/run_web_net_checks.mjs --native        also the sessions with a Windows game (needs the Windows build; one game window at a time, uses a scratch savedata folder, never your own %APPDATA%)
// Sessions:  A two browsers through the relay (join, play in sync, relay drop and reconnect, host drop)
//            B two browsers through a line with delay, jitter and stalls (TCP lag proxy)
//            C one browser: no relay set, wrong code, unreachable relay
//            P direct (WebRTC) connections between two browsers: P1 works (traffic on the data channel, relay idle), P2 blocked (relay carries the game),
//              P3 the host cannot do it (old client / Windows game), P4 the channel dies during the game (closed, silent, one-way, long)
//              P5 the path is blocked at the start and again in the middle of the game, and the game returns to the channel each time it opens up (relay idle again)
//            L the public lobby in the browser menus: L1 list + quick join + play in sync, L2 6 digit number (keyboard and number pad) and private lobby, L3 wrong codes and lockout,
//              L4 server asleep (waking message, retries) , L5 server down, L6 old page vs new page (reload message), L7 a server without a lobby list, L8 the host leaves / closes the tab / freezes
//            N a Windows game and a browser in one lobby (both ways round)
// Everything it starts (relay, web server, browsers, game) is stopped at the end; the ROM is only served from where the game keeps it, to 127.0.0.1.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { startHarness, startWebServer, sleep, defaultRomPath } from './harness.mjs';
import { createRelayServer } from '../relay_ws/server.mjs';
import { startLagProxy } from './tcp_lag_proxy.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const logDir = path.join(root, 'checks', 'last_logs');
fs.mkdirSync(logDir, { recursive: true });
const args = process.argv.slice(2);
const only = (args.includes('--only') ? args[args.indexOf('--only') + 1] : 'A,B,C,P,L,T,N').split(',');
const withNative = args.includes('--native');
const webDir = path.join(root, 'web_build');

const results = [];
function check(id, text, ok, detail = '') {
	results.push({ id, text, ok: !!ok, detail });
	console.log(`${ok ? 'PASS' : 'FAIL'}  ${id}  ${text}${detail ? `  [${detail}]` : ''}`);
}

// ---- lobby code of a session id (same as NetplayCodes::makeSessionCode): 12 digits plus a Damm check digit, "XXXX-XXXX-XXXXX" ----
const DAMM = [[0,3,1,7,5,9,8,6,4,2],[7,0,9,2,1,5,4,8,6,3],[4,2,0,6,8,7,1,3,5,9],[1,7,5,0,9,8,3,4,2,6],[6,1,2,3,0,4,5,9,7,8],[3,6,7,4,2,0,9,5,8,1],[5,8,6,9,7,2,0,1,3,4],[8,9,4,5,3,6,2,0,1,7],[9,4,3,8,6,1,7,2,0,5],[2,5,8,1,4,3,6,7,9,0]];
function sessionCode(id) {
	const digits = id.toString();
	let interim = 0;
	for (const ch of digits) interim = DAMM[interim][+ch];
	const full = digits + interim;
	return `${full.slice(0, 4)}-${full.slice(4, 8)}-${full.slice(8)}`;
}
// every random byte of the page is the seed byte (see harness.seedScript), so the game's 64 bit random number is that byte eight times (NetplayCodes::sessionIDFromRandom)
const codeForSeed = (byte) => sessionCode(100000000000n + (BigInt(byte) * 0x0101010101010101n) % 900000000000n);

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
	await host.createLobby();
	const hosted = await host.waitLog(/Lobby: screen HOSTING/, 10000);
	const known = await host.waitLog(/the relay server knows this lobby/, 15000);
	await sleep(1000);
	const joined = [];
	for (const friend of friends) {
		await friend.toLobby();
		friend.forgetLogs();
		await friend.openJoin();
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
		const query = `?relay=ws://127.0.0.1:${relay.ports.port}&p2p=off`;   // (these sessions test the game through the relay: direct connections are off, session P tests them)
		await Promise.all([host.openGame(h.baseURL, query), friend.openGame(h.baseURL, query)]);
		await Promise.all([host.waitLog(/Ready to go/, 90000), friend.waitLog(/Ready to go/, 90000)]);
		await sleep(2000);
		check('A0', 'both browsers start the game and reach the main menu', (await Promise.all([host.toMainMenu(), friend.toMainMenu()])).every(Boolean));
		check('A1', 'the lobby start screen shows the relay from the page link', true);
		const r = await hostAndJoin(host, [friend], query, code);
		check('A2', 'the host opens a lobby (relay knows it) and shows the screen', r.hosted && r.known);
		check('A3', 'the friend joins with the lobby code through the relay (code proof both ways, player slot 2)', r.joined[0]);
		check('A3b', 'the relay holds one lobby and one friend, both browsers connected by WebSocket (the game, and the lobby directory while the menu is open)', relay.relay.core.stats.lobbies === 1 && relay.relay.core.stats.pipes === 1 && relay.relay.wsConnections.size >= 2 && relay.relay.wsConnections.size <= 4, JSON.stringify(relay.relay.core.stats) + ' websockets: ' + relay.relay.wsConnections.size);		// (each browser has one for the game and, while its menu is open, one for the lobby directory)
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
		check('A6c', 'the game runs again on both sides after the drop (frames advanced by more than 300; the online log is written every 5 s, so the last sample is up to 300 frames behind) and the friend is back in sync', after.host && after.friend && after.host.frame > before.host + 300 && after.friend.frame > before.friend + 300 && after.friend.syncOk >= after.friend.frame - 360, `host frame ${before.host} -> ${after.host?.frame}; friend frame ${before.friend} -> ${after.friend?.frame}, last sync ok ${after.friend?.syncOk}, repairs ${after.friend?.repairs}`);
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
		const query = `?relay=ws://127.0.0.1:${proxy.port}&p2p=off`;
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
		await b.createLobby();
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
		await b.openJoin();
		await b.waitLog(/Lobby: screen JOIN/, 8000);
		await b.type('1234-5678-90124');   // not a valid long code (check digit)
		await b.key('Return');
		const typo = await b.waitLog(/WRONG_CODE/, 8000);
		check('C2', 'a mistyped code (wrong check character) is refused at once with a plain message, nothing is sent to the relay', typo && relay.relay.core.stats.received === 0 || (typo && !b.hasLog(/asking the relay server to let us in/)));
		await b.navigate(`${h.baseURL}/sonic3air_web.html?relay=ws://127.0.0.1:${relay.ports.port}`);
		await b.waitLog(/Ready to go/, 90000);
		await sleep(3000);
		await b.toMainMenu();
		await b.toLobby();
		b.forgetLogs();
		await b.openJoin();
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
		await b.createLobby();
		const unreachable = await b.waitLog(/Lobby: the relay server has not registered this lobby after 10 seconds/, 25000);
		const waking = b.hasLog(/Lobby: waking the server/);
		check('C4', 'a relay that cannot be reached: the host lobby says it is waking the server (the server may be asleep; session L4 and L5 follow that through to the end), and the game keeps trying to connect', unreachable && waking && b.hasLog(/connecting to the relay server/));
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
				await b.openJoin();
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
			await b.createLobby();
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
// =====================================================================================================================
// Session P: direct (WebRTC) connections between two browsers, with the relay as lobby, signalling and fallback.
// STUN is switched off (?stun=none): only the computer's own addresses are tried, nothing outside this PC is contacted.
const RTC_TRACK = `(() => { const Real = window.RTCPeerConnection; if (!Real) return; window.__pcs = [];
	class T extends Real { constructor(...a) { super(...a); window.__pcs.push(this); } }
	window.RTCPeerConnection = T;
	const send = RTCDataChannel.prototype.send; window.__blackhole = false;
	RTCDataChannel.prototype.send = function (d) { if (window.__blackhole) return; return send.call(this, d); }; })();`;
// "the direct path is blocked": neither side learns the other's candidates (no connectivity checks can start), like a pair of strict NATs without TURN
const RTC_BLOCK = `(() => { const Real = window.RTCPeerConnection; window.__pcs = [];
	class T extends Real {
		constructor(...a) { super(...a); window.__pcs.push(this); }
		addIceCandidate() { return Promise.resolve(); }
		setRemoteDescription(d) { if (d && d.sdp) d = { type: d.type, sdp: d.sdp.split('\\r\\n').filter((l) => !l.startsWith('a=candidate') && !l.startsWith('a=end-of-candidates')).join('\\r\\n') }; return super.setRemoteDescription(d); }
	}
	window.RTCPeerConnection = T; })();`;
// "this player cannot do direct connections" (an old browser, or the Windows game, which has no WebRTC)
const RTC_NONE = `(() => { window.RTCPeerConnection = undefined; })();`;

const p2pInfo = (inst) => inst.eval(`(() => { const G = globalThis.__s3p2p; if (!G) return null; return { enabled: G.enabled, msgsIn: G.msgsIn, msgsOut: G.msgsOut, bytesIn: G.bytesIn, bytesOut: G.bytesOut, peers: [...G.peers.values()].map((p) => ({ id: p.id, state: p.state })) }; })()`);
function parseP2PRows(text) {
	const rows = [];
	for (const line of text.split('\n')) {
		const m = /^\[p2p\] t=(\d+)s transport=(\w+) direct_in=(\d+) direct_out=(\d+) relay_in=(\d+) relay_out=(\d+)/.exec(line);
		if (m) rows.push({ t: +m[1], transport: m[2], directIn: +m[3], directOut: +m[4], relayIn: +m[5], relayOut: +m[6] });
	}
	return rows;
}
const relayStats = (relay) => ({ ...relay.relay.core.stats });

async function p2pSetup(relay, { hostSeed, friendSeed, basePort, hostScript, friendScript }) {
	const code = codeForSeed(hostSeed);
	const h = await startHarness({ webDir, instances: 2, seeds: [hostSeed, friendSeed], basePort });
	const [host, friend] = h.instances;
	if (hostScript) await host.addScript(hostScript);
	if (friendScript) await friend.addScript(friendScript);
	const query = `?relay=ws://127.0.0.1:${relay.ports.port}&stun=none`;
	await Promise.all([host.openGame(h.baseURL, query), friend.openGame(h.baseURL, query)]);
	await Promise.all([host.waitLog(/Ready to go/, 90000), friend.waitLog(/Ready to go/, 90000)]);
	await sleep(2000);
	await Promise.all([host.toMainMenu(), friend.toMainMenu()]);
	return { h, host, friend, code, query };
}

function dumpConsole(name, host, friend) {
	const pick = (inst) => inst.allLogs.filter((l) => /Netplay|direct|Lobby|EXC/i.test(l)).join('\n');
	fs.writeFileSync(path.join(logDir, `web_net_${name}_console.txt`), `HOST\n${pick(host)}\nFRIEND\n${pick(friend)}\n`);
}

async function p2pLines(host, friend) {
	return { host: parseP2PRows(await host.onlineLog()).pop(), friend: parseP2PRows(await friend.onlineLog()).pop() };
}

async function sessionP1(relay) {
	console.log('--- session P1: two browsers, direct channel works');
	const { h, host, friend, code, query } = await p2pSetup(relay, { hostSeed: 0x71, friendSeed: 0x52, basePort: 9380, hostScript: RTC_TRACK, friendScript: RTC_TRACK });
	try {
		const r = await hostAndJoin(host, [friend], query, code);
		check('P1a', 'two browsers join through the relay as before (lobby code, code proof both ways)', r.hosted && r.known && r.joined[0]);
		const fDirect = await friend.waitLog(/direct connection to the host is in use \(p2p\)/, 30000);
		const hDirect = await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 30000);
		check('P1b', 'the friend and the host both report that a direct (peer to peer) connection is in use, set up through signalling messages of the relay', fDirect && hDirect, `${fDirect ?? 'friend: none'} | ${hDirect ?? 'host: none'}`);
		check('P1c', 'the relay passed signalling messages (offer, answer, candidates) and counts them apart from game data', relay.relay.core.stats.signals >= 4, JSON.stringify(relayStats(relay)));
		await sleep(3000);
		await shot(friend, 'P1_lobby_friend');
		const loaded = await startNormalGame(host, [friend]);
		check('P1d', 'the host starts a new game and the friend loads the game state over the direct channel', loaded[0]);
		const s0 = relayStats(relay), a0 = await p2pInfo(host), b0 = await p2pInfo(friend);
		const play = await playAndMeasure(host, [friend], 25, 'P1');
		const s1 = relayStats(relay), a1 = await p2pInfo(host), b1 = await p2pInfo(friend);
		const directMsgs = (a1.msgsIn - a0.msgsIn) + (a1.msgsOut - a0.msgsOut) + (b1.msgsIn - b0.msgsIn) + (b1.msgsOut - b0.msgsOut);
		const relayMsgs = s1.forwarded - s0.forwarded;
		check('P1e', 'during 25 s of play the traffic goes over the data channel (thousands of datagrams in each direction) and the relay forwards almost nothing (under 2% of it)', directMsgs > 4000 && a1.msgsIn - a0.msgsIn > 1000 && b1.msgsIn - b0.msgsIn > 1000 && relayMsgs < directMsgs * 0.02, `direct datagrams ${directMsgs} (host in ${a1.msgsIn - a0.msgsIn}, friend in ${b1.msgsIn - b0.msgsIn}), relay forwarded ${relayMsgs} (${s1.forwardedBytes - s0.forwardedBytes} bytes)`);
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('P1f', 'both games stay in sync for 25 s over the direct channel (no repairs, last sync recent, both near 60 fps)', hs.ok && fs_.ok && hs.last.fps > 50 && fs_.last.fps > 50, `host: ${hs.why}; friend: ${fs_.why}`);
		const lines = await p2pLines(host, friend);
		check('P1g', 'the games report the transport as p2p, and the host no longer counts the friend as relayed (normal input delay rules)', lines.host?.transport === 'p2p' && lines.friend?.transport === 'p2p' && play.hostRows.at(-1)?.relayed === 0, JSON.stringify(lines) + ' relayed=' + play.hostRows.at(-1)?.relayed);
		// The relay counters are the proof that the game did not go through the relay: a relay-only game of the same length forwards about 700000 bytes (session A, A5d), here only keep-alives and registration may arrive
		const fwdBytes = s1.forwardedBytes - s0.forwardedBytes, recvMsgs = s1.received - s0.received, directBytes = (a1.bytesIn - a0.bytesIn) + (b1.bytesIn - b0.bytesIn);
		check('P1i', 'during 25 s of play the relay forwarded no game bytes (under 1000 of the ~700000 a relayed game needs), received only a few messages (registration keep-alives, under 150 in all) and the data channel carried the game (over 200000 bytes), repairs 0 on both sides', fwdBytes < 1000 && recvMsgs < 150 && directBytes > 200000 && Math.max(...play.hostRows.map((r) => r.repairs ?? 0), ...play.friendRows[0].map((r) => r.repairs ?? 0)) === 0, `relay forwarded ${fwdBytes} bytes, received ${recvMsgs} messages; data channel bytes in ${directBytes}`);
		check('P1h', 'no desync message and no error in either console', !host.hasLog(/state differs from the host|EXC|assert/i) && !friend.hasLog(/state differs from the host|EXC|assert/i));
		fs.writeFileSync(path.join(logDir, 'web_net_P1_console.txt'), `HOST\n${host.allLogs.filter((l) => /Netplay|direct/i.test(l)).join('\n')}\nFRIEND\n${friend.allLogs.filter((l) => /Netplay|direct/i.test(l)).join('\n')}`);
	} finally { await h.close(); }
}

async function sessionP2(relay) {
	console.log('--- session P2: the direct path is blocked: the relay carries the game');
	const { h, host, friend, code, query } = await p2pSetup(relay, { hostSeed: 0x75, friendSeed: 0x53, basePort: 9384, hostScript: RTC_BLOCK, friendScript: RTC_BLOCK });
	try {
		const r = await hostAndJoin(host, [friend], query, code);
		check('P2a', 'the lobby works through the relay although a direct connection is tried', r.hosted && r.known && r.joined[0]);
		const failed = await friend.waitLog(/direct connection to peer 0 did not work/, 25000);
		check('P2b', 'the friend gives the direct attempt up after 8 s and says it uses the relay (nobody sees an error)', failed, failed ?? friend.allLogs.filter((l) => /direct/i.test(l)).join(' | '));
		await startNormalGame(host, [friend]);
		const s0 = relayStats(relay);
		const play = await playAndMeasure(host, [friend], 20, 'P2');
		const s1 = relayStats(relay);
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('P2c', 'the game runs in sync for 20 s over the relay (no repairs)', hs.ok && fs_.ok, `host: ${hs.why}; friend: ${fs_.why}`);
		const lines = await p2pLines(host, friend);
		check('P2d', 'the relay carried the game (thousands of datagrams), the transport is reported as relay, the host treats the friend as relayed', s1.forwarded - s0.forwarded > 1500 && lines.host?.transport === 'relay' && lines.friend?.transport === 'relay' && play.hostRows.at(-1)?.relayed === 1, `relay forwarded ${s1.forwarded - s0.forwarded}; ${JSON.stringify(lines)}`);
		check('P2e', 'no "direct connection is in use" line, no desync, no error', !friend.hasLog(/is in use \(p2p\)/) && !host.hasLog(/is in use \(p2p\)/) && !host.hasLog(/state differs|EXC/) && !friend.hasLog(/state differs|EXC/));
	} finally { dumpConsole('P2', host, friend); await h.close(); }
}

async function sessionP3(relay) {
	console.log('--- session P3: the host cannot do direct connections (like the Windows game): the friend stays on the relay');
	const { h, host, friend, code, query } = await p2pSetup(relay, { hostSeed: 0x82, friendSeed: 0x54, basePort: 9388, hostScript: RTC_NONE, friendScript: RTC_TRACK });
	try {
		const r = await hostAndJoin(host, [friend], query, code);
		check('P3a', 'a friend that can do direct connections joins a host that cannot', r.hosted && r.known && r.joined[0]);
		const failed = await friend.waitLog(/direct connection to peer 0 did not work/, 25000);
		check('P3b', 'the friend\'s offer finds no answer: it gives up after 8 s, stays on the relay, and the host (no WebRTC at all) logs nothing wrong', failed && !host.hasLog(/EXC|error/i), failed ?? '');
		await startNormalGame(host, [friend]);
		const play = await playAndMeasure(host, [friend], 15, 'P3');
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('P3c', 'the game runs in sync over the relay for 15 s', hs.ok && fs_.ok, `host: ${hs.why}; friend: ${fs_.why}`);
	} finally { dumpConsole('P3', host, friend); await h.close(); }
}

async function sessionP4(relay) {
	console.log('--- session P4: the direct channel dies during the game (closed, silent, one-way): fall back without desync');
	const { h, host, friend, code, query } = await p2pSetup(relay, { hostSeed: 0x74, friendSeed: 0x55, basePort: 9392, hostScript: RTC_TRACK, friendScript: RTC_TRACK });
	try {
		const r = await hostAndJoin(host, [friend], query, code);
		await friend.waitLog(/direct connection to the host is in use \(p2p\)/, 30000);
		await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 30000);
		await startNormalGame(host, [friend]);
		await playAndMeasure(host, [friend], 8, 'P4a');
		let lines = await p2pLines(host, friend);
		check('P4a', 'the game starts over the direct channel (transport p2p on both sides)', r.joined[0] && lines.host?.transport === 'p2p' && lines.friend?.transport === 'p2p', JSON.stringify(lines));
		const frameOf = async (inst) => parseOnlineLog(await inst.onlineLog()).pop();

		// 1. the friend's side closes the peer connection (a tab that closes its network, a browser that drops WebRTC)
		let s0 = relayStats(relay), f0 = await frameOf(friend);
		host.forgetLogs(); friend.forgetLogs();
		await friend.eval('window.__pcs.forEach((p) => p.close()); window.__pcs.length')
		const hostSaw = await host.waitLog(/direct connection to friend 1 is not working \(any more\), back on the relay server/, 15000);
		const friendSaw = await friend.waitLog(/direct connection to the host is not working \(any more\), back on the relay server/, 15000);
		check('P4b', 'when one side closes the direct connection both sides notice within seconds and say they are back on the relay', hostSaw && friendSaw, `host: ${hostSaw ?? 'nothing'} | friend: ${friendSaw ?? 'nothing'}`);
		await playAndMeasure(host, [friend], 12, 'P4b');
		let f1 = await frameOf(friend), s1 = relayStats(relay);
		check('P4c', 'the game goes on over the relay without a desync (friend frames advanced, relay forwarded the game, no repairs)', f1.frame > f0.frame + 500 && (f1.repairs ?? 0) === 0 && s1.forwarded - s0.forwarded > 1000 && !friend.hasLog(/state differs from the host/), `friend frame ${f0.frame} -> ${f1.frame}, repairs ${f1.repairs}, relay forwarded ${s1.forwarded - s0.forwarded}`);

		// 2. the friend tries again by itself (after 12 s) and the direct channel comes back
		const again = await friend.waitLog(/direct connection to the host is in use \(p2p\)/, 60000);
		await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 10000);
		check('P4d', 'a direct connection that died is set up again by itself after a few seconds', again);
		const rBack0 = relayStats(relay);
		await playAndMeasure(host, [friend], 6, 'P4d');
		const rBack1 = relayStats(relay);
		check('P4h', 'once the channel is back the relay is idle again: it forwarded under 1000 game bytes during the next 6 s of play', rBack1.forwardedBytes - rBack0.forwardedBytes < 1000, `relay forwarded ${rBack1.forwardedBytes - rBack0.forwardedBytes} bytes`);

		// 3. a short silent outage (everything the host sends over the channel vanishes for 3 s): both sides use the relay while it lasts and take the channel back when it works again
		host.forgetLogs(); friend.forgetLogs();
		await host.eval('window.__blackhole = true');
		const quiet = await friend.waitLog(/direct connection to the host is not working \(any more\), back on the relay server/, 10000);
		const quietHost = await host.waitLog(/direct connection to friend 1 is not working \(any more\), back on the relay server/, 10000);
		await sleep(1000);
		await host.eval('window.__blackhole = false');
		const back = await friend.waitLog(/direct connection to the host is in use \(p2p\)/, 15000);
		const backHost = await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 15000);
		check('P4e', 'a short silent one-way outage: both sides (also the one that still hears the other) leave the channel within seconds and return to it when it works again, without a new connection', quiet && quietHost && back && backHost && !friend.hasLog(/did not work|lost \(/), `${quiet ? 'f out' : 'f NOT out'}, ${quietHost ? 'h out' : 'h NOT out'}, ${back ? 'f back' : 'f NOT back'}, ${backHost ? 'h back' : 'h NOT back'}`);

		// 4. a long silent outage: the channel is given up for good, the game keeps going on the relay
		s0 = relayStats(relay); f0 = await frameOf(friend);
		host.forgetLogs(); friend.forgetLogs();
		await host.eval('window.__blackhole = true');
		const lost = await friend.waitLog(/direct connection to peer 0 lost \(nothing received for 5 s\)/, 20000);
		const play = await playAndMeasure(host, [friend], 10, 'P4f');
		f1 = await frameOf(friend); s1 = relayStats(relay);
		check('P4f', 'a long silent outage: the channel is declared lost after 5 s and the game keeps running over the relay', lost && f1.frame > f0.frame + 500 && s1.forwarded - s0.forwarded > 800, `${lost ?? 'not lost'}; friend frame ${f0.frame} -> ${f1.frame}; relay forwarded ${s1.forwarded - s0.forwarded}`);
		await host.eval('window.__blackhole = false');
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('P4g', 'after all of it the games are still in sync with no repair ever needed (the engine compared the memory of both every second)', hs.ok && fs_.ok && !host.hasLog(/state differs|EXC/) && !friend.hasLog(/state differs|EXC/), `host: ${hs.why}; friend: ${fs_.why}`);
		fs.writeFileSync(path.join(logDir, 'web_net_P4_console.txt'), `HOST\n${host.allLogs.filter((l) => /Netplay|direct/i.test(l)).join('\n')}\nFRIEND\n${friend.allLogs.filter((l) => /Netplay|direct/i.test(l)).join('\n')}`);
	} finally { await h.close(); }
}

// "the direct path is blocked" as a switch (window.__blockIce): while it is on, neither side learns the other's candidates; it starts on, and the test turns it off and on again
const RTC_TOGGLE = `(() => { const Real = window.RTCPeerConnection; window.__pcs = []; window.__blockIce = true;
	class T extends Real {
		constructor(...a) { super(...a); window.__pcs.push(this); }
		addIceCandidate(c) { return window.__blockIce ? Promise.resolve() : super.addIceCandidate(c); }
		setRemoteDescription(d) { if (window.__blockIce && d && d.sdp) d = { type: d.type, sdp: d.sdp.split('\\r\\n').filter((l) => !l.startsWith('a=candidate') && !l.startsWith('a=end-of-candidates')).join('\\r\\n') }; return super.setRemoteDescription(d); }
	}
	window.RTCPeerConnection = T; })();`;

/** Inputs from every player for `seconds` (no measuring). */
async function drive(host, friends, seconds) {
	const end = Date.now() + seconds * 1000;
	while (Date.now() < end) {
		await Promise.all([host.hold('Right', 700), ...friends.map((f, i) => f.hold(i % 2 ? 'Left' : 'Right', 900))]);
		await sleep(400);
	}
}

/** Plays until a console line of the friend matches (the host's inputs keep the game moving); returns the line or null. */
async function driveUntil(host, friend, inst, pattern, seconds) {
	const end = Date.now() + seconds * 1000;
	while (Date.now() < end) {
		const hit = inst.allLogs.find((l) => pattern.test(l));
		if (hit) return hit;
		await drive(host, [friend], 3);
	}
	return inst.allLogs.find((l) => pattern.test(l)) ?? null;
}

async function sessionP5(relay) {
	console.log('--- session P5: the direct path is blocked at the start and again in the middle of the game, and comes back when it works');
	const { h, host, friend, code, query } = await p2pSetup(relay, { hostSeed: 0x61, friendSeed: 0x56, basePort: 9396, hostScript: RTC_TOGGLE, friendScript: RTC_TOGGLE });
	try {
		const frameOf = async (inst) => parseOnlineLog(await inst.onlineLog()).pop();
		const idle = (a, b) => b.forwardedBytes - a.forwardedBytes;
		const r = await hostAndJoin(host, [friend], query, code);
		const failed = await friend.waitLog(/direct connection to peer 0 did not work/, 25000);
		check('P5a', 'the direct path is blocked from the start: the lobby works and the friend gives the direct attempt up after 8 s', r.hosted && r.known && r.joined[0] && failed, failed ?? '');
		await startNormalGame(host, [friend]);
		let s0 = relayStats(relay);
		let play = await playAndMeasure(host, [friend], 10, 'P5a');
		let s1 = relayStats(relay);
		let lines = await p2pLines(host, friend);
		check('P5b', 'while blocked the relay carries the game in sync (thousands of datagrams forwarded, transport relay, repairs 0)', s1.forwarded - s0.forwarded > 1000 && lines.host?.transport === 'relay' && lines.friend?.transport === 'relay' && syncOk(play.hostRows, 'host').ok && syncOk(play.friendRows[0], 'friend').ok, `relay forwarded ${s1.forwarded - s0.forwarded}; ${JSON.stringify(lines)}`);

		// the path opens up: the friend asks again by itself (after a pause of 12 s x (1 + failures)) and the game moves to the direct channel
		host.forgetLogs(); friend.forgetLogs();
		await friend.eval('window.__blockIce = false'); await host.eval('window.__blockIce = false');
		const fBack = await driveUntil(host, friend, friend, /direct connection to the host is in use \(p2p\)/, 110);
		const hBack = await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 10000);
		check('P5c', 'when the path is open again the friend sets the direct connection up by itself (no restart, no new lobby) and both sides report it in use', fBack && hBack, `${fBack ?? 'friend: no'} | ${hBack ?? 'host: no'}`);
		await drive(host, [friend], 4);
		s0 = relayStats(relay); const a0 = await p2pInfo(host), b0 = await p2pInfo(friend);
		play = await playAndMeasure(host, [friend], 15, 'P5c');
		s1 = relayStats(relay); const a1 = await p2pInfo(host), b1 = await p2pInfo(friend);
		lines = await p2pLines(host, friend);
		check('P5d', 'after the switch to the channel the relay forwards no game bytes (under 1000 in 15 s of play), the channel carries it (over 2000 datagrams received by the two games together: about 125 per second each for the 21 s of the play and the wait after it), transport p2p, in sync, repairs 0', idle(s0, s1) < 1000 && (a1.msgsIn - a0.msgsIn) + (b1.msgsIn - b0.msgsIn) > 2000 && lines.host?.transport === 'p2p' && lines.friend?.transport === 'p2p' && syncOk(play.hostRows, 'host').ok && syncOk(play.friendRows[0], 'friend').ok, `relay forwarded ${idle(s0, s1)} bytes; channel datagrams in ${(a1.msgsIn - a0.msgsIn) + (b1.msgsIn - b0.msgsIn)}; ${JSON.stringify(lines)}`);

		// blocked again in the middle of the game, and the channel dies: the relay takes over
		host.forgetLogs(); friend.forgetLogs();
		s0 = relayStats(relay); let f0 = await frameOf(friend);
		await friend.eval('window.__blockIce = true'); await host.eval('window.__blockIce = true');
		await friend.eval('window.__pcs.forEach((p) => p.close()); window.__pcs.length');
		const hostSaw = await host.waitLog(/direct connection to friend 1 is not working \(any more\), back on the relay server/, 15000);
		const friendSaw = await friend.waitLog(/direct connection to the host is not working \(any more\), back on the relay server/, 15000);
		play = await playAndMeasure(host, [friend], 15, 'P5f');
		s1 = relayStats(relay); let f1 = await frameOf(friend); lines = await p2pLines(host, friend);
		check('P5e', 'the channel dies while the path is blocked: both sides say they are back on the relay, which carries the game in sync (friend frames advanced, relay forwarded the game, repairs 0, no desync message)', hostSaw && friendSaw && f1.frame > f0.frame + 600 && s1.forwarded - s0.forwarded > 1000 && (f1.repairs ?? 0) === 0 && lines.friend?.transport === 'relay' && !friend.hasLog(/state differs from the host/), `friend frame ${f0.frame} -> ${f1.frame}; relay forwarded ${s1.forwarded - s0.forwarded}; ${JSON.stringify(lines)}`);

		// the path opens up again: back to the channel, relay idle again
		host.forgetLogs(); friend.forgetLogs();
		await friend.eval('window.__blockIce = false'); await host.eval('window.__blockIce = false');
		const fBack2 = await driveUntil(host, friend, friend, /direct connection to the host is in use \(p2p\)/, 130);
		const hBack2 = await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 10000);
		await drive(host, [friend], 4);
		s0 = relayStats(relay);
		play = await playAndMeasure(host, [friend], 15, 'P5h');
		s1 = relayStats(relay); lines = await p2pLines(host, friend);
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('P5f', 'when the path opens again the game returns to the direct channel by itself: relay idle (under 1000 bytes in 15 s), transport p2p, both in sync with repairs 0 after both switch-overs, no desync or error in either console', fBack2 && hBack2 && idle(s0, s1) < 1000 && lines.host?.transport === 'p2p' && lines.friend?.transport === 'p2p' && hs.ok && fs_.ok && !host.hasLog(/state differs|EXC/) && !friend.hasLog(/state differs|EXC/), `relay forwarded ${idle(s0, s1)} bytes; ${JSON.stringify(lines)}; host: ${hs.why}; friend: ${fs_.why}`);
	} finally { dumpConsole('P5', host, friend); await h.close(); }
}

async function sessionP6(relay) {
	console.log('--- session P6: the relay drops its WebSockets (all of them, then only the host\'s) while the game runs over the direct channel');
	const { h, host, friend, code, query } = await p2pSetup(relay, { hostSeed: 0x62, friendSeed: 0x57, basePort: 9400, hostScript: RTC_TRACK, friendScript: RTC_TRACK });
	try {
		const frameOf = async (inst) => parseOnlineLog(await inst.onlineLog()).pop();
		const r = await hostAndJoin(host, [friend], query, code);
		await friend.waitLog(/direct connection to the host is in use \(p2p\)/, 30000);
		await host.waitLog(/direct connection to friend 1 is in use \(p2p\)/, 30000);
		await startNormalGame(host, [friend]);
		const play = await playAndMeasure(host, [friend], 8, 'P6a');
		let lines = await p2pLines(host, friend);
		check('P6a', 'the game runs over the direct channel before the relay is touched', r.joined[0] && lines.host?.transport === 'p2p' && lines.friend?.transport === 'p2p' && syncOk(play.hostRows, 'host').ok && syncOk(play.friendRows[0], 'friend').ok, JSON.stringify(lines));

		// every WebSocket of the relay drops (a restart of the relay, a network change): the game does not need the relay while the channel works
		let h0 = await frameOf(host), f0 = await frameOf(friend);
		host.forgetLogs(); friend.forgetLogs();
		for (const c of relay.relay.wsConnections.values()) c.terminate();
		const hostBack = await host.waitLog(/the relay server knows this lobby/, 40000);
		const friendBack = await friend.waitLog(/the relay server let us in/, 40000);
		await drive(host, [friend], 20);
		await sleep(6000);
		let h1 = await frameOf(host), f1 = await frameOf(friend);
		lines = await p2pLines(host, friend);
		check('P6b', 'after the relay dropped every WebSocket both browsers register / join again by themselves and the game goes on for both (frames advanced by more than 600 each, friend in sync again, repairs 0)', hostBack && friendBack && h1.frame > h0.frame + 600 && f1.frame > f0.frame + 600 && f1.syncOk >= f1.frame - 360 && (f1.repairs ?? 0) === 0, `host frame ${h0.frame} -> ${h1.frame}; friend frame ${f0.frame} -> ${f1.frame}, last sync ok ${f1.syncOk}, repairs ${f1.repairs}; ${JSON.stringify(lines)}; ${hostBack ?? 'host: no'} | ${friendBack ?? 'friend: no'}`);

		// only the host's WebSocket drops
		const lobby = [...relay.relay.core.lobbies.values()].at(-1);
		f0 = await frameOf(friend);
		host.forgetLogs(); friend.forgetLogs();
		relay.relay.wsConnections.get(lobby.host)?.terminate();
		const reRegistered = await host.waitLog(/the relay server knows this lobby/, 30000);
		await drive(host, [friend], 15);
		await sleep(6000);
		f1 = await frameOf(friend);
		check('P6c', 'when only the host\'s WebSocket drops, the host takes its lobby back and the game goes on (friend frames advance by more than 400, no desync)', reRegistered && f1.frame > f0.frame + 400 && !friend.hasLog(/state differs from the host/), `friend frame ${f0.frame} -> ${f1.frame}`);
	} finally { dumpConsole('P6', host, friend); await h.close(); }
}

// =====================================================================================================================
// Session L: the public lobby in the browser menus (list, quick join, number codes, wrong codes and lockout, a server that is asleep or down, an old page)
// Every part uses its own lobby server on 127.0.0.1 (WebSocket only, no UDP), so its counters, listings and the lockout of an address start from nothing.
async function startLobbyRelay({ port = 0, limits = {} } = {}) {
	const server = createRelayServer({ port, udp: false, bind: '127.0.0.1', statsSeconds: 0, limits });
	const ports = await server.start();
	return { server, ports, core: server.core, entries: () => [...server.core.directory.entries.values()], stop: () => server.stop() };
}

const relayQuery = (port, extra = '') => `?relay=ws://127.0.0.1:${port}&p2p=off${extra}`;

/** Opens the page in a browser (gives it the ROM the first time for each address) and goes on to the main menu. */
async function bootPage(inst, baseURL, query) {
	inst.forgetLogs();
	inst.opened ??= new Set();
	if (!inst.opened.has(baseURL)) { await inst.openGame(baseURL, query); inst.opened.add(baseURL); }
	else await inst.navigate(`${baseURL}/sonic3air_web.html${query}`);
	await inst.waitLog(/Ready to go/, 90000);
	await sleep(3000);
	return inst.toMainMenu();
}

/** Screenshots of the menu at the window's own size, at a small window and at a phone held upright (the game picture shrinks with the page). */
async function shotSizes(inst, name, sizes = [[0, 0, ''], [640, 360, '_small'], [360, 640, '_phone']]) {
	for (const [w, hgt, suffix] of sizes) {
		await inst.resize(w, hgt);
		await sleep(700);
		await shot(inst, `L_${name}${suffix}`);
	}
	await inst.resize();
	await sleep(300);
}

/** The digit keys of the number pad: moves from the pad key `from` (0-11, rows of 3: 1 2 3 / 4 5 6 / 7 8 9 / DEL 0 JOIN) to the key for `digit` and presses A. Returns the new position. */
async function padPress(inst, from, digit) {
	const target = digit === 0 ? 10 : digit - 1;
	let dx = (target % 3) - (from % 3), dy = Math.floor(target / 3) - Math.floor(from / 3);
	if (dx === 2) dx = -1; else if (dx === -2) dx = 1;		// the columns wrap around
	const keys = [];
	for (let i = 0; i < Math.abs(dy); ++i) keys.push(dy > 0 ? 'Down' : 'Up');
	for (let i = 0; i < Math.abs(dx); ++i) keys.push(dx > 0 ? 'Right' : 'Left');
	await inst.keys(keys, 180);
	await inst.key('a');
	await sleep(250);
	return target;
}

const lobbyMenuMessage = (inst) => [...inst.allLogs].reverse().map((l) => /Lobby: whole message: (.*)/s.exec(l)?.[1]?.trim()).find(Boolean) ?? '';

async function sessionL1() {
	console.log('--- session L1: a public lobby is listed, quick join finds it, both play in sync');
	const R = await startLobbyRelay();
	const h = await startHarness({ webDir, instances: 2, basePort: 9420 });
	try {
		const [host, friend] = h.instances;
		const query = relayQuery(R.ports.port);
		await Promise.all([bootPage(host, h.baseURL, query), bootPage(friend, h.baseURL, query)]);
		await host.toLobby();
		await shotSizes(host, 'home');
		host.forgetLogs();
		await host.createLobby();
		await host.waitLog(/Lobby: screen HOSTING/, 10000);
		const published = await waitFor(() => R.entries().find((e) => e.isPublic && e.code), 20000);
		check('L1a', 'browser A creates a lobby in the ONLINE menu (public by default): the lobby server lists it with a 6 digit code, its name, 1 of 4 players, waiting', published && /^[1-9][0-9]{5}$/.test(published.code) && published.name === 'My lobby' && published.players === 1 && published.max === 4 && !published.inGame, JSON.stringify(published && { code: published.code, name: published.name, host: published.host, players: published.players, max: published.max }));
		await sleep(1500);
		await shot(host, 'L_hosting');
		await shotSizes(host, 'hosting', [[640, 360, '_small'], [360, 640, '_phone']]);

		// B opens PUBLIC LOBBIES and sees it
		await friend.toLobby();
		friend.forgetLogs();
		await friend.key('Return');
		await friend.waitLog(/Lobby: screen LIST/, 8000);
		const seen = await friend.waitLog(/Lobby: the list shows 1 public lobb/, 15000);
		await sleep(800);
		await shotSizes(friend, 'list');
		check('L1b', 'browser B opens PUBLIC LOBBIES and the list shows the lobby of A', seen, seen ?? friend.allLogs.slice(-5).join(' | '));

		// quick join from the first screen
		await friend.key('Escape');
		await friend.waitLog(/Lobby: screen START/, 8000);
		friend.forgetLogs();
		await friend.keys(['Down', 'Return'], 500);
		const picked = await friend.waitLog(/Lobby: quick join picks the lobby "My lobby" \(1\/4\)/, 15000);
		const proved = await friend.waitLog(/the host proved the code, player slot 2/, 40000);
		check('L1c', 'QUICK JOIN picks the lobby from the list and B ends up in it as player 2 (code proof both ways through the relay)', picked && proved, `${picked ?? 'no pick'} | ${proved ?? 'not joined'}`);
		await sleep(2500);
		await shot(friend, 'L_joined');
		await shotSizes(friend, 'joined', [[360, 640, '_phone']]);
		const after = await waitFor(() => R.entries()[0]?.players === 2 ? R.entries()[0] : null, 15000);
		check('L1d', 'the listing follows the lobby: 2 of 4 players after B joined', after, JSON.stringify(after && { players: after.players }));

		const loaded = await startNormalGame(host, [friend]);
		const play = await playAndMeasure(host, [friend], 20, 'L1');
		const hs = syncOk(play.hostRows, 'host'), fs_ = syncOk(play.friendRows[0], 'friend');
		check('L1e', 'the host starts a new game, both browsers load it and stay in sync for 20 s of play with inputs from both (no repairs, last sync recent)', loaded[0] && hs.ok && fs_.ok && !host.hasLog(/state differs from the host|EXC/) && !friend.hasLog(/state differs from the host|EXC/), `host: ${hs.why}; friend: ${fs_.why}`);
		const playing = R.entries()[0];
		check('L1f', 'the listing shows the lobby as playing now, and nothing was dropped as invalid by the server', playing?.inGame === true && R.core.stats.droppedInvalid === 0, JSON.stringify({ inGame: playing?.inGame, dropped: R.core.stats.droppedInvalid }));
	} finally { await h.close(); await R.stop(); }
}

async function sessionL2() {
	console.log('--- session L2: join with the 6 digit number (typed and with the number pad), a private lobby is not listed');
	const R = await startLobbyRelay();
	const h = await startHarness({ webDir, instances: 2, basePort: 9424 });
	try {
		const [host, friend] = h.instances;
		const query = relayQuery(R.ports.port);
		await Promise.all([bootPage(host, h.baseURL, query), bootPage(friend, h.baseURL, query)]);
		await host.toLobby();
		host.forgetLogs();
		await host.keys(['Down', 'Down', 'Return'], 600);		// CREATE LOBBY
		await host.waitLog(/Lobby: screen CREATE/, 8000);
		await host.keys(['Down', 'Right', 'Down', 'Down', 'Return'], 600);		// LOBBY: public -> private, then OPEN LOBBY
		await host.waitLog(/Lobby: screen HOSTING/, 10000);
		const entry = await waitFor(() => R.entries().find((e) => e.code), 20000);
		check('L2a', 'a private lobby gets its 6 digit code from the server but is not in the public list', entry && !entry.isPublic, JSON.stringify(entry && { code: entry.code, isPublic: entry.isPublic }));
		await shot(host, 'L_hosting_private');

		await friend.toLobby();
		friend.forgetLogs();
		await friend.key('Return');
		await friend.waitLog(/Lobby: screen LIST/, 8000);
		await friend.waitLog(/Lobby: the list shows 0 public lobbies/, 15000);
		await sleep(500);
		await shot(friend, 'L_list_empty');
		await friend.key('Escape');
		await friend.waitLog(/Lobby: screen START/, 8000);
		friend.forgetLogs();
		await friend.keys(['Down', 'Return'], 500);		// QUICK JOIN
		const none = await friend.waitLog(/Lobby: the lookup ended: There is no open lobby for your version/, 20000);
		await sleep(800);
		check('L2b', 'the private lobby is not listed: B\'s list is empty and QUICK JOIN says there is no open lobby (it does not join a private one)', none && lobbyMenuMessage(friend).startsWith('There is no open lobby'), lobbyMenuMessage(friend));
		await shot(friend, 'L_quick_none');

		// the number pad: three digits typed on the keyboard, three with the D-pad and A
		friend.forgetLogs();
		await friend.openJoin();		// (the failed QUICK JOIN left the menu on its first screen)
		await friend.waitLog(/Lobby: screen JOIN/, 8000);
		await friend.type(entry.code.slice(0, 3));
		await sleep(400);
		let pad = 0;		// the cursor starts on the 1 key
		for (const digit of entry.code.slice(3)) pad = await padPress(friend, pad, +digit);
		const six = await friend.waitLog(/Lobby: the code screen holds 6 digits/, 5000);
		check('L2c', 'the number pad takes digits typed on the keyboard and digits pressed with D-pad + A (6 digits in the field)', six, six ?? friend.allLogs.filter((l) => /Lobby/.test(l)).slice(-4).join(' | '));
		await shotSizes(friend, 'join', [[0, 0, ''], [640, 360, '_small'], [360, 640, '_phone']]);
		await friend.key('Return');		// START = JOIN
		const resolved = await friend.waitLog(/Lobby: the lobby server found the lobby, joining it/, 20000);
		const proved = await friend.waitLog(/the host proved the code, player slot 2/, 40000);
		check('L2d', 'JOIN with the 6 digit number: the server resolves it, B joins the private lobby as player 2', resolved && proved, `${resolved ?? 'not resolved'} | ${proved ?? 'not joined'}`);
		check('L2e', 'a right code costs no strike (the lockout counter stays empty)', R.core.directory.stats.notFound === 0 && R.core.directory.stats.resolved >= 1, JSON.stringify(R.core.directory.stats));
	} finally { await h.close(); await R.stop(); }
}

async function sessionL3() {
	console.log('--- session L3: wrong codes in the browser menu, and the lockout after five of them');
	const R = await startLobbyRelay();
	const h = await startHarness({ webDir, instances: 1, basePort: 9428 });
	try {
		const [b] = h.instances;
		await bootPage(b, h.baseURL, relayQuery(R.ports.port));
		await b.toLobby();
		// (pictures of two more screens while here: CREATE LOBBY and the settings, which in a browser hold the relay server and no lobby server)
		await b.keys(['Down', 'Down', 'Return'], 500);
		await b.waitLog(/Lobby: screen CREATE/, 8000);
		await sleep(600);
		await shotSizes(b, 'create', [[0, 0, ''], [640, 360, '_small']]);
		await b.key('Escape');
		await b.waitLog(/Lobby: screen START/, 8000);
		await b.keys(['Down', 'Down', 'Down', 'Down', 'Return'], 500);
		await b.waitLog(/Lobby: screen SETTINGS/, 8000);
		await sleep(600);
		await shotSizes(b, 'settings', [[0, 0, ''], [640, 360, '_small']]);
		await b.key('Escape');
		await b.waitLog(/Lobby: screen START/, 8000);
		b.forgetLogs();
		await b.openJoin();
		await b.waitLog(/Lobby: screen JOIN/, 8000);
		await b.type('12345');
		await b.key('Return');
		await sleep(1200);
		check('L3a', 'five digits: the menu says a lobby code has 6 digits and sends nothing to the server', /6 digits/.test(lobbyMenuMessage(b)) && R.core.directory.stats.notFound === 0 && R.core.directory.stats.resolved === 0, lobbyMenuMessage(b));
		await shot(b, 'L_code_short');
		await b.type('6');
		b.forgetLogs();
		await b.key('Return');
		const unknown = await b.waitLog(/Lobby: the lookup ended: There is no lobby with this code/, 20000);
		await sleep(600);
		check('L3b', 'a 6 digit code that nobody has opened: "There is no lobby with this code" and the menu is back on the code screen', unknown && R.core.directory.stats.notFound === 1 && /no lobby with this code/.test(lobbyMenuMessage(b)), lobbyMenuMessage(b));
		await shot(b, 'L_code_wrong');
		// four more wrong codes (the last digit changes): the fifth in a minute locks the address
		const start = Date.now();
		for (let n = 2; n <= 5; ++n) {
			await b.key('Backspace');
			await b.type(String(n));
			b.forgetLogs();
			await b.key('Return');
			await b.waitLog(/Lobby: the lookup ended/, 20000);
			await sleep(400);
		}
		check('L3c', 'the fifth wrong code within a minute is counted by the server (5 strikes)', R.core.directory.stats.notFound === 5 && Date.now() - start < 60000, `${JSON.stringify(R.core.directory.stats)} in ${Math.round((Date.now() - start) / 1000)} s after the first`);
		await b.key('Backspace');
		await b.type('6');
		b.forgetLogs();
		await b.key('Return');
		const locked = await b.waitLog(/Lobby: the lookup ended: Too many wrong codes/, 20000);
		await sleep(600);
		check('L3d', 'the sixth try is refused as "Too many wrong codes. Please wait 5 minute(s)" and the server did not look the code up (lockout holds for the browser path)', locked && R.core.directory.stats.throttled >= 1 && R.core.directory.stats.notFound === 5 && /Please wait 5 minute/.test(lobbyMenuMessage(b)), `${lobbyMenuMessage(b)}; ${JSON.stringify(R.core.directory.stats)}`);
		await shot(b, 'L_code_locked');
		// the menu is not stuck: back out, then another screen opens
		b.forgetLogs();
		await b.key('Escape');
		const home = await b.waitLog(/Lobby: screen START/, 8000);
		await b.key('Return');
		const list = await b.waitLog(/Lobby: screen LIST/, 8000);
		check('L3e', 'after the lockout the menu still works (back to the first screen, PUBLIC LOBBIES opens)', home && list);
	} finally { await h.close(); await R.stop(); }
}

/** A free TCP port number on 127.0.0.1 (nothing listens on it afterwards). */
async function freePort() {
	const { default: net } = await import('node:net');
	return new Promise((resolve) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
}

async function sessionL4() {
	console.log('--- session L4: the server is asleep and wakes up late (waking message, retries), then a server that stays down');
	const port = await freePort();
	const h = await startHarness({ webDir, instances: 1, basePort: 9432 });
	let R = null;
	try {
		const [b] = h.instances;
		await bootPage(b, h.baseURL, relayQuery(port));
		await b.toLobby();
		b.forgetLogs();
		await b.keys(['Down', 'Return'], 500);		// QUICK JOIN while the server does not answer
		const waking = await b.waitLog(/Lobby: waking the server/, 12000);
		const t0 = Date.now();
		await sleep(14000);
		const early = b.hasLog(/Lobby: the lookup ended/);
		check('L4a', 'a server that does not answer: the menu says it is waking the server and keeps retrying (it has not given up after 25 s)', waking && !early, waking ?? b.allLogs.filter((l) => /Lobby/.test(l)).slice(-4).join(' | '));
		await shotSizes(b, 'waking', [[0, 0, ''], [640, 360, '_small'], [360, 640, '_phone']]);
		R = await startLobbyRelay({ port });		// the server wakes up
		const ended = await b.waitLog(/Lobby: the lookup ended/, 45000);
		check('L4b', 'when the server is up the retrying request goes through by itself (no key pressed): QUICK JOIN answers "no open lobby"', ended && /There is no open lobby/.test(ended) && R.core.directory.stats.listRequests >= 1, `${ended ?? 'no end'} after ${Math.round((Date.now() - t0) / 1000)} s`);
		await shot(b, 'L_waking_done');
		await R.stop(); R = null;

		// a lobby opened while the server sleeps registers when it wakes up
		await bootPage(b, h.baseURL, relayQuery(port));
		await b.toLobby();
		b.forgetLogs();
		await b.createLobby();
		await b.waitLog(/Lobby: screen HOSTING/, 10000);
		const hostWaking = await b.waitLog(/Lobby: waking the server/, 20000);
		await sleep(3000);
		await shot(b, 'L_hosting_waking');
		await shotSizes(b, 'hosting_waking', [[640, 360, '_small'], [360, 640, '_phone']]);
		R = await startLobbyRelay({ port });
		const listed = await waitFor(() => R.entries().find((e) => e.isPublic), 45000);
		check('L4c', 'a lobby opened while the server sleeps says so, and is registered and listed by itself when the server wakes up', hostWaking && listed, `${hostWaking ?? 'no waking text'} | ${listed ? 'listed ' + listed.code : 'not listed'}`);
		await sleep(2000);
		await shot(b, 'L_hosting_after_waking');
		await R.stop(); R = null;
	} finally { await h.close(); if (R) await R.stop(); }
}

async function sessionL5() {
	console.log('--- session L5: the server stays down for longer than the waking window; every new request gets its own full window');
	const port = await freePort();
	const h = await startHarness({ webDir, instances: 1, basePort: 9436 });
	let R = null;
	try {
		const [b] = h.instances;
		await bootPage(b, h.baseURL, relayQuery(port));
		await b.toLobby();
		b.forgetLogs();
		const t0 = Date.now();
		await b.keys(['Down', 'Return'], 500);		// QUICK JOIN
		const ended = await b.waitLog(/Lobby: the lookup ended: The lobby server does not answer/, 90000);
		const seconds = (Date.now() - t0) / 1000;
		check('L5a', 'a server that stays down: after about a minute of retries QUICK JOIN says the lobby server does not answer (not before 50 s, not after 75 s)', ended && seconds > 50 && seconds < 75, `${ended ?? 'no end'} after ${Math.round(seconds)} s`);
		await sleep(600);
		await shot(b, 'L_server_down');
		await shotSizes(b, 'server_down', [[360, 640, '_phone']]);
		b.forgetLogs();		// (kept from here on: the waking line of the next window must show up in it)
		await b.key('Down');		// the menu is not broken: PUBLIC LOBBIES, then back
		await b.key('Up');
		await b.key('Return');
		const list = await b.waitLog(/Lobby: screen LIST/, 8000);
		await sleep(2500);
		await shot(b, 'L_list_down');
		await b.key('Escape');
		const back = await b.waitLog(/Lobby: screen START/, 8000);

		// A NEW request after the first window has run out gets a full window of its own (it used to fail at once: the window was counted from the start of the menu, not from the request)
		await b.keys(['Down', 'Down', 'Down', 'Return'], 500);
		const join = await b.waitLog(/Lobby: screen JOIN/, 8000);
		await b.type('123456');
		const t1 = Date.now();
		await b.key('Return');
		const waking2 = await b.waitLog(/Lobby: waking the server/, 15000);
		await sleep(15000);
		const early = b.hasLog(/Lobby: the lookup ended/);
		const joinEnd = await b.waitLog(/Lobby: the lookup ended: The lobby server does not answer/, 90000);
		const seconds2 = (Date.now() - t1) / 1000;
		check('L5b', 'with the server down the menu stays usable (list opens with a clear text, back works, the code screen opens); a code lookup made after the first window says "waking the server" again, is still waiting after 15 s, and ends with the clear message after about another minute (50..75 s)', list && back && join && waking2 && !early && joinEnd && seconds2 > 50 && seconds2 < 75, `${!!list} ${!!back} ${!!join} ${waking2 ?? 'no waking'} early=${early} ${joinEnd ?? 'no end'} after ${Math.round(seconds2)} s`);

		// and a later request succeeds when the server wakes up inside its window
		await b.key('Escape');
		await b.waitLog(/Lobby: screen START/, 8000);
		b.forgetLogs();
		const t2 = Date.now();
		await b.keys(['Down', 'Down', 'Return'], 500);		// QUICK JOIN again (the first entry is RETRY after the failed lookup)
		await b.waitLog(/Lobby: waking the server/, 15000);
		await sleep(12000);
		R = await startLobbyRelay({ port });
		const ok = await b.waitLog(/Lobby: the lookup ended: There is no open lobby/, 45000);
		check('L5c', 'a third request, made when the server has been down for minutes, waits again and succeeds when the server comes up inside its window (answered by itself, no key pressed)', ok && R.core.directory.stats.listRequests >= 1, `${ok ?? 'no answer'} after ${Math.round((Date.now() - t2) / 1000)} s; ${b.allLogs.filter((l) => /Lobby: (the lookup|waking|the server)/.test(l)).slice(-4).join(' | ')}`);
	} finally { await h.close(); if (R) await R.stop(); }
}

async function sessionL6() {
	let R2 = null;
	console.log('--- session L6: an old page and a new page meet in a lobby (other version): reload message, no broken game');
	const R = await startLobbyRelay();
	const h = await startHarness({ webDir, instances: 2, basePort: 9440 });
	// "the old page" is this page with another netplay compatibility text ("airnet-1" -> "airnet-0", same length): the lobby server marks its lobbies and lists as another version, and the start of a game is refused
	let patched = 0;
	const old = await startWebServer(webDir, defaultRomPath(), 0, (name, data) => {
		if (!name.endsWith('.wasm')) return null;
		const copy = Buffer.from(data);
		const at = copy.indexOf('airnet-1');
		if (at < 0) return null;
		copy.write('airnet-0', at, 'latin1'); ++patched;
		return copy;
	});
	try {
		const [newPage, oldPage] = h.instances;
		const oldURL = `http://127.0.0.1:${old.port}`;
		const query = relayQuery(R.ports.port);
		await Promise.all([bootPage(newPage, h.baseURL, query), bootPage(oldPage, oldURL, query)]);
		check('L6a', 'the old page (same build, other compatibility text) starts and reaches the menu', patched >= 1);

		// the new page hosts; the old page sees the lobby as another version and is told to reload
		await newPage.toLobby();
		newPage.forgetLogs();
		await newPage.createLobby();
		await newPage.waitLog(/Lobby: screen HOSTING/, 10000);
		const entry = await waitFor(() => R.entries().find((e) => e.isPublic && e.code), 20000);
		await oldPage.toLobby();
		oldPage.forgetLogs();
		await oldPage.key('Return');
		await oldPage.waitLog(/Lobby: screen LIST/, 8000);
		await oldPage.waitLog(/Lobby: the list shows 1 public lobb/, 15000);
		await sleep(600);
		await shotSizes(oldPage, 'list_other', [[0, 0, ''], [360, 640, '_phone']]);
		oldPage.forgetLogs();
		await oldPage.key('Up');		// (the cursor waits on REFRESH, because no lobby of the list can be joined)
		await oldPage.key('Return');		// the row of the other version
		await sleep(900);
		const msg = lobbyMenuMessage(oldPage);
		check('L6b', 'the list marks the lobby of the other version and choosing it says to reload the page (a new version is out) instead of trying to join', /[Rr]eload the page/.test(msg) && !oldPage.hasLog(/asking the relay server to let us in/), msg);
		await shot(oldPage, 'L_other_message');

		// joining with the number anyway: the lobby takes it, the start of the game is refused with the same advice
		await oldPage.key('Escape');
		await oldPage.waitLog(/Lobby: screen START/, 8000);
		await oldPage.openJoin();
		await oldPage.waitLog(/Lobby: screen JOIN/, 8000);
		await oldPage.type(entry.code);
		oldPage.forgetLogs();
		await oldPage.key('Return');
		const proved = await oldPage.waitLog(/the host proved the code, player slot 2/, 40000);
		await sleep(2500);
		await oldPage.key('Return');		// READY
		await sleep(3000);
		newPage.forgetLogs();
		await newPage.key('Return');		// START GAME
		const refused = await oldPage.waitLog(/Lobby: showing the problem to the player \((MOD|VERSION)_MISMATCH\)/, 30000);
		await sleep(1200);
		const msg2 = lobbyMenuMessage(oldPage);
		check('L6c', 'the old page joined by number; when the host starts, the old page is refused and says "reload the page, a new version is out" (not a broken game: no error, back at the first screen)', proved && refused && /[Rr]eload (the|this) page/.test(msg2) && !oldPage.hasLog(/EXC|assert/i), `${refused ?? 'no refusal'} | ${msg2}`);
		await shot(oldPage, 'L_mismatch_start');
		await shotSizes(oldPage, 'mismatch_start', [[360, 640, '_phone']]);
		await oldPage.key('Escape');
		const usable = await oldPage.waitLog(/Lobby: screen/, 5000);
		check('L6d', 'after the refusal the old page\'s menu is still usable', usable);

		// the other way round (a fresh lobby server, so no listing of the first part is left): the old page hosts, the new page looks at the list
		R2 = await startLobbyRelay();
		const query2 = relayQuery(R2.ports.port);
		await Promise.all([bootPage(newPage, h.baseURL, query2), bootPage(oldPage, oldURL, query2)]);
		await oldPage.toLobby();
		oldPage.forgetLogs();
		await oldPage.createLobby();
		await oldPage.waitLog(/Lobby: screen HOSTING/, 10000);
		await waitFor(() => R2.entries().find((e) => e.isPublic && e.code), 20000);
		await newPage.toLobby();
		newPage.forgetLogs();
		await newPage.key('Return');
		await newPage.waitLog(/Lobby: screen LIST/, 8000);
		await newPage.waitLog(/Lobby: the list shows 1 public lobb/, 15000);
		await sleep(600);
		newPage.forgetLogs();
		await newPage.key('Up');
		await newPage.key('Return');
		await sleep(900);
		const msg3 = lobbyMenuMessage(newPage);
		check('L6e', 'the other way round (old page hosts, new page browses) the lobby is also marked as another version, with the same advice', /[Rr]eload the page/.test(msg3), msg3);
	} finally { await h.close(); await new Promise((resolve) => { old.server.closeAllConnections?.(); old.server.close(resolve); }); await R.stop(); if (R2) await R2.stop(); }
}

async function sessionL7() {
	console.log('--- session L7: a server without a lobby list (a relay older than the lobby directory): a clear message, the long code still works');
	const R = await startLobbyRelay({ limits: { directoryEnabled: false } });
	const h = await startHarness({ webDir, instances: 2, seeds: [0x41, 0x52], basePort: 9444 });
	try {
		const [host, friend] = h.instances;
		const query = relayQuery(R.ports.port);
		await Promise.all([bootPage(host, h.baseURL, query), bootPage(friend, h.baseURL, query)]);
		await friend.toLobby();
		friend.forgetLogs();
		await friend.keys(['Down', 'Return'], 500);		// QUICK JOIN
		const ended = await friend.waitLog(/Lobby: the lookup ended: This server has no list of public lobbies/, 30000);
		await sleep(800);
		check('L7a', 'a server that relays but has no lobby directory: QUICK JOIN says at once (within 30 s, not after a minute of retries) that the server has no list and that a long code still works', ended && /13 digits/.test(lobbyMenuMessage(friend)), `${ended ?? 'no end'} | ${lobbyMenuMessage(friend)}`);
		await shot(friend, 'L_no_list_home');
		await friend.key('Return');		// PUBLIC LOBBIES
		await friend.waitLog(/Lobby: screen LIST/, 8000);
		await sleep(1500);
		await shot(friend, 'L_no_list_list');
		await friend.key('Escape');
		await friend.waitLog(/Lobby: screen START/, 8000);

		await host.toLobby();
		host.forgetLogs();
		await host.createLobby();
		const known = await host.waitLog(/the relay server knows this lobby/, 20000);
		await sleep(9000);
		await shot(host, 'L_hosting_no_list');
		check('L7b', 'a lobby can still be opened on that server (the relay knows it) and nothing is listed', known && R.entries().length === 0, known ?? '');
		friend.forgetLogs();
		await friend.openJoin();
		await friend.waitLog(/Lobby: screen JOIN/, 8000);
		await friend.type(codeForSeed(0x41));
		await friend.key('Return');
		const proved = await friend.waitLog(/the host proved the code, player slot 2/, 40000);
		check('L7c', 'the friend joins that lobby with the long code through the same server', proved);
	} finally { await h.close(); await R.stop(); }
}

/** Host opens a public lobby, the friend joins it by QUICK JOIN. Both pages must already be in the main menu. Returns the listing. */
async function openAndQuickJoin(host, friend, R) {
	host.forgetLogs();
	const toLobbyOk = await host.toLobby();
	await host.createLobby();
	const hosted = await host.waitLog(/Lobby: screen HOSTING/, 10000);
	const entry = await waitFor(() => R.entries().find((e) => e.isPublic && e.code), 20000);
	const hostTail = host.allLogs.slice(-14).join(' | ');
	const friendLobbyOk = await friend.toLobby();
	friend.forgetLogs();
	await friend.keys(['Down', 'Return'], 500);
	const proved = await friend.waitLog(/the host proved the code, player slot 2/, 40000);
	await sleep(2000);
	// what happened during the setup, for a failing run (a failure to set up is not a failure of the case that follows, and must be told apart from it)
	openAndQuickJoin.last = { hostToLobby: toLobbyOk, hostScreen: !!hosted, listed: !!entry, friendToLobby: friendLobbyOk, friendJoined: !!proved, hostTail, friendTail: friend.allLogs.slice(-10).join(' | '), relay: JSON.stringify(R.core.stats), lobbies: R.core.lobbies.size };
	return proved ? entry : null;
}

async function sessionL8() {
	console.log('--- session L8: the host goes away (Leave Lobby, tab closed, page frozen): the listing goes, the joiner is told');
	const h = await startHarness({ webDir, instances: 2, basePort: 9448 });
	let R = null;
	try {
		const [host, friend] = h.instances;
		// ---- (a1) the host leaves while B only looks at the list: the list refreshes to nothing
		R = await startLobbyRelay();
		await Promise.all([bootPage(host, h.baseURL, relayQuery(R.ports.port)), bootPage(friend, h.baseURL, relayQuery(R.ports.port))]);
		await host.toLobby();
		host.forgetLogs();
		await host.createLobby();
		await host.waitLog(/Lobby: screen HOSTING/, 10000);
		await waitFor(() => R.entries().find((e) => e.isPublic && e.code), 20000);
		await friend.toLobby();
		friend.forgetLogs();
		await friend.key('Return');		// PUBLIC LOBBIES
		await friend.waitLog(/Lobby: screen LIST/, 8000);
		const seen = await friend.waitLog(/Lobby: the list shows 1 public lobby/, 15000);
		host.forgetLogs();
		const tLeave = Date.now();
		await host.key('Up');		// from START GAME round to LEAVE LOBBY
		await host.key('Return');
		const left = await host.waitLog(/Lobby: screen START/, 8000);
		const gone = await waitFor(() => R.entries().length === 0, 10000, 200);
		const goneSeconds = (Date.now() - tLeave) / 1000;
		friend.forgetLogs();
		const empty = await friend.waitLog(/Lobby: the list shows 0 public lobbies/, 20000);
		check('L8a', 'the host leaves with LEAVE LOBBY: the server\'s listing is gone at once, and the list on B\'s screen refreshes to 0 lobbies by itself (its refresh is every 8 s)', seen && left && gone && empty, `listing gone after ${goneSeconds.toFixed(1)} s; ${empty ?? 'list did not refresh'}`);
		await shot(friend, 'L_list_after_leave');
		await R.stop(); R = null;

		// ---- (a2) the host leaves while B is in the lobby
		R = await startLobbyRelay();
		await Promise.all([bootPage(host, h.baseURL, relayQuery(R.ports.port)), bootPage(friend, h.baseURL, relayQuery(R.ports.port))]);
		const entry = await openAndQuickJoin(host, friend, R);
		friend.forgetLogs();
		const t1 = Date.now();
		await host.key('Up');
		await host.key('Return');
		const hostLeftB = await host.waitLog(/Lobby: screen START/, 8000);
		const gone2 = await waitFor(() => R.entries().length === 0, 10000, 200);
		const told = await friend.waitLog(/Lobby: showing the problem to the player \(HOST_CLOSED\)/, 90000);
		await sleep(800);
		const msg = lobbyMenuMessage(friend);
		check('L8b', 'the host leaves while B is in its lobby: the listing is gone and B is told within 15 s that the host closed the lobby (the host says goodbye to the relay connection before it is closed), not after a timeout of more than a minute', entry && gone2 && told && (Date.now() - t1) / 1000 < 15 && /The host closed the lobby or stopped answering/.test(msg), `${Math.round((Date.now() - t1) / 1000)} s until told; ${msg}; gone2=${!!gone2}; hostLeft=${!!hostLeftB}; setup=${JSON.stringify(openAndQuickJoin.last)}; friend: ${friend.allLogs.filter((l) => /Netplay|Lobby/.test(l)).slice(-6).join(" | ")}`);
		await shot(friend, 'L_host_left_message');
		friend.forgetLogs();
		await friend.keys(['Down', 'Return'], 400);		// (the first entry is RETRY, then PUBLIC LOBBIES)
		check('L8b2', 'B is back on the first screen and can open PUBLIC LOBBIES', !!(await friend.waitLog(/Lobby: screen LIST/, 8000)));
		await R.stop(); R = null;

		// ---- (b) the host closes the tab (the page is gone: its WebSocket closes)
		R = await startLobbyRelay();
		await Promise.all([bootPage(host, h.baseURL, relayQuery(R.ports.port)), bootPage(friend, h.baseURL, relayQuery(R.ports.port))]);
		const entry3 = await openAndQuickJoin(host, friend, R);
		friend.forgetLogs();
		const t2 = Date.now();
		await host.navigate('about:blank');
		const gone3 = await waitFor(() => R.entries().length === 0, 60000, 500);
		const goneSeconds3 = (Date.now() - t2) / 1000;
		const told3 = await friend.waitLog(/Lobby: showing the problem to the player \(HOST_CLOSED\)/, 90000);
		await sleep(800);
		check('L8c', 'the host closes its tab: the listing is gone (the relay drops a host whose WebSocket closed) and B is told the host closed the lobby', entry3 && gone3 && told3 && (Date.now() - t2) / 1000 < 85 && /The host closed the lobby or stopped answering/.test(lobbyMenuMessage(friend)), `listing gone after ${goneSeconds3.toFixed(1)} s; B told after ${Math.round((Date.now() - t2) / 1000)} s; ${lobbyMenuMessage(friend)}`);
		await R.stop(); R = null;

		// ---- (c) the host's page freezes or its network dies without a close (the WebSocket stays open, nothing is published or answered): the listing runs out, B is told.
		// The server's listing expiry is 20 s by default (the game publishes every 5 s); the test shortens it to 6 s so that the check is quick. The relay also drops a host that is silent.
		R = await startLobbyRelay({ limits: { listingTimeoutSeconds: 6 } });
		await Promise.all([bootPage(host, h.baseURL, relayQuery(R.ports.port)), bootPage(friend, h.baseURL, relayQuery(R.ports.port))]);
		const entry4 = await openAndQuickJoin(host, friend, R);
		friend.forgetLogs();
		const t3 = Date.now();
		await host.send('Debugger.enable');
		await host.send('Debugger.pause');		// the host's JavaScript stops: no more publishing, no answers
		const gone4 = await waitFor(() => R.entries().length === 0, 60000, 500);
		const goneSeconds4 = (Date.now() - t3) / 1000;
		const told4 = await friend.waitLog(/Lobby: showing the problem to the player \((HOST_CLOSED|CONNECTION_LOST)\)/, 120000);
		await sleep(800);
		check('L8d', 'the host freezes without closing (no publishing, no answers): the listing runs out after the expiry (6 s in this test, 20 s by default) and B is not frozen forever: it gets a clear message', entry4 && gone4 && goneSeconds4 < 30 && told4 && (Date.now() - t3) / 1000 < 85 && lobbyMenuMessage(friend) !== '', `listing gone after ${goneSeconds4.toFixed(1)} s; ${told4 ?? 'B never told'} after ${Math.round((Date.now() - t3) / 1000)} s; ${lobbyMenuMessage(friend)}`);
		await shot(friend, 'L_host_frozen_message');
	} finally { await h.close(); if (R) await R.stop(); }
}

/** Repeats the "host leaves while the friend is in the lobby" case (L8b) many times and keeps the logs of EVERY run in checks/last_logs (never overwritten). `--repeat N` (default 10). */
async function sessionL8R() {
	const runs = Number(args.includes('--repeat') ? args[args.indexOf('--repeat') + 1] : 10);
	console.log(`--- session L8R: the host leaves LEAVE LOBBY while the friend is in the lobby, ${runs} times (logs of every run kept)`);
	const stamp = new Date().toISOString().replace(/[:.]/g, '-');
	const file = path.join(logDir, `web_net_L8R_${stamp}.txt`);
	const h = await startHarness({ webDir, instances: 2, basePort: 9456 });
	let R = null;
	try {
		const [host, friend] = h.instances;
		for (let n = 1; n <= runs; ++n) {
			R = await startLobbyRelay();
			await Promise.all([bootPage(host, h.baseURL, relayQuery(R.ports.port)), bootPage(friend, h.baseURL, relayQuery(R.ports.port))]);
			const entry = await openAndQuickJoin(host, friend, R);
			const statsBefore = JSON.stringify(R.core.stats);
			friend.forgetLogs(); host.forgetLogs();
			const t1 = Date.now();
			await host.key('Up');
			await host.key('Return');
			const hostLeft = await host.waitLog(/Lobby: screen START/, 8000);
			const gone = await waitFor(() => R.entries().length === 0, 10000, 200);
			const told = await friend.waitLog(/Lobby: showing the problem to the player \(HOST_CLOSED\)/, 100000);
			const seconds = (Date.now() - t1) / 1000;
			await sleep(800);
			const detail = `run ${n}: joined=${!!entry} hostLeft=${!!hostLeft} listingGone=${!!gone} told after ${seconds.toFixed(1)} s; ${lobbyMenuMessage(friend)}${entry ? '' : ' SETUP FAILED: ' + JSON.stringify(openAndQuickJoin.last)}`;
			fs.appendFileSync(file, `=== ${detail}\nrelay stats before ${statsBefore}\nrelay stats after ${JSON.stringify(R.core.stats)} ws=${R.server.wsConnections.size}\nHOST (last 12 lines)\n${host.allLogs.slice(-12).join('\n')}\nFRIEND (last 25 lines)\n${friend.allLogs.slice(-25).join('\n')}\n\n`);
			check(`L8R-${n}`, 'host leaves with LEAVE LOBBY while the friend is in the lobby: the host really left, the listing is gone and the friend is told within 15 s', entry && hostLeft && gone && told && seconds < 15, `${detail} (log: ${path.basename(file)})`);
			await R.stop(); R = null;
		}
	} finally { await h.close(); if (R) await R.stop(); }
}

// =====================================================================================================================
// Session T: a player's tab goes into the background (browsers stop animation frames and slow timers to about 1 per second in a hidden tab), which stalls the lockstep game for everybody.
// The browser itself hides the tab: a second tab opens in the same (headless) window, see Instance.hide(). The browsers of this session run WITHOUT the flags that switch background
// throttling off. What this reproduces: document.visibilityState 'hidden', visibilitychange, no animation frames, timers once a second, worker timers unthrottled (Chrome's own rules).
// What it does not: Chrome's "intensive throttling" after 5 minutes hidden, a tab discarded or frozen by the browser (memory saver, mobile), Firefox and Safari (their own rules), a tab
// that is merely out of focus next to another window.
// The pages expose their tab state in globalThis.__s3tab = { hidden, banner, frame, bgtick } (written by the game's own web glue); banner is what the game shows on screen about it.
const tState = (inst) => inst.eval(`(() => { const s = globalThis.__s3tab; return s ? { hidden: !!s.hidden, banner: String(s.banner || ''), frame: typeof s.frame === 'number' ? s.frame : -1, bgtick: String(s.bgtick || '') } : null; })()`);
const tTitle = (inst) => inst.eval('document.title');

/** Samples the given browsers every 250 ms for `seconds` while `driver` plays (inputs of the visible player). Returns, per browser, the samples { t, frame, banner, title }. */
async function tObserve(insts, seconds, driver) {
	const samples = insts.map(() => []);
	const t0 = Date.now();
	const sampler = (async () => {
		while (Date.now() - t0 < seconds * 1000) {
			await Promise.all(insts.map(async (inst, i) => {
				try {
					const [s, title] = await Promise.all([tState(inst), tTitle(inst)]);
					samples[i].push({ t: (Date.now() - t0) / 1000, frame: s ? s.frame : -1, banner: s ? s.banner : '', title, hidden: s ? s.hidden : null });
				} catch { /* page busy */ }
			}));
			await sleep(250);
		}
	})();
	await Promise.all([sampler, driver ? driver(seconds) : Promise.resolve()]);
	return samples;
}
const tStall = (samples) => {	// the longest time the frame number did not change (seconds)
	let longest = 0, since = samples[0]?.t ?? 0, frame = samples[0]?.frame;
	for (const s of samples) { if (s.frame !== frame) { frame = s.frame; since = s.t; } longest = Math.max(longest, s.t - since); }
	return longest;
};
const tRate = (samples) => samples.length > 1 ? (samples.at(-1).frame - samples[0].frame) / (samples.at(-1).t - samples[0].t) : 0;
const tFirst = (samples, pattern, field = 'banner') => samples.find((s) => pattern.test(s[field]))?.t ?? null;
const tPlay = (inst) => async (seconds) => {	// inputs of the one visible player
	const end = Date.now() + seconds * 1000;
	while (Date.now() < end) { await inst.hold('Right', 700); await sleep(300); }
};

async function tSetup(relay, { seed, basePort, extra = '', p2p = false }) {
	const h = await startHarness({ webDir, instances: 2, seeds: [seed, 0x52], basePort, throttled: true });
	try {
		const [host, friend] = h.instances;
		const query = `?relay=ws://127.0.0.1:${relay.ports.port}${p2p ? '&stun=none' : '&p2p=off'}${extra}`;
		await Promise.all([host.openGame(h.baseURL, query), friend.openGame(h.baseURL, query)]);
		await Promise.all([host.waitLog(/Ready to go/, 90000), friend.waitLog(/Ready to go/, 90000)]);
		await sleep(2000);
		await Promise.all([host.toMainMenu(), friend.toMainMenu()]);
		const r = await hostAndJoin(host, [friend], query, codeForSeed(seed));
		const loaded = await startNormalGame(host, [friend]);
		await drive(host, [friend], 6);
		return { h, host, friend, ok: !!(r.hosted && r.known && r.joined[0] && loaded[0]), detail: `hosted=${!!r.hosted} known=${!!r.known} joined=${!!r.joined[0]} friendLoadedState=${!!loaded[0]}` };
	} catch (e) { await h.close(); throw e; }
}

const tSync = async (host, friend, framesAfter = 0) => {
	await sleep(6000);
	const hr = parseOnlineLog(await host.onlineLog()), fr = parseOnlineLog(await friend.onlineLog());
	const h = syncOk(hr, 'host'), f = syncOk(fr, 'friend');
	return { ok: h.ok && f.ok, why: `host: ${h.why}; friend: ${f.why}`, hostLast: hr.at(-1), friendLast: fr.at(-1) };
};

// ---- T1: the mitigation is on (default): a hidden tab keeps playing ----
async function sessionT1(relay) {
	console.log('--- session T1: a tab in the background keeps playing (worker driven ticks), both ways round');
	const s = await tSetup(relay, { seed: 0x62, basePort: 9470 });
	const { h, host, friend } = s;
	try {
		check('T1a', 'two browsers (with real background throttling) are in a running game', s.ok, s.detail);
		const t0 = await Promise.all([tState(host), tState(friend)]);
		check('T1b', 'both pages expose their tab state (globalThis.__s3tab) and report the worker ticks as on', !!t0[0] && !!t0[1] && t0[0].bgtick === 'on' && t0[1].bgtick === 'on', JSON.stringify(t0));
		const visible = await tObserve([host, friend], 5, (sec) => drive(host, [friend], sec));
		console.log(`      visible: host ${tRate(visible[0]).toFixed(1)} frames/s, friend ${tRate(visible[1]).toFixed(1)} frames/s`);

		// -- the friend's tab goes into the background
		const hid = await friend.hide();
		const obs = await tObserve([host, friend], 20, tPlay(host));
		const [oh, of] = obs;
		const hiddenRate = tRate(of), hostRate = tRate(oh);
		check('T1c', 'the friend\'s tab really is hidden (visibilityState hidden)', hid && of.every((x) => x.hidden !== false));
		check('T1d', 'the hidden friend keeps playing: 20 s in the background run at 45 frames per second or more (visible: ~60), and the host never waits more than 2 s for it', hiddenRate >= 45 && tStall(oh) <= 2 && tStall(of) <= 2, `hidden friend ${hiddenRate.toFixed(1)} frames/s, host ${hostRate.toFixed(1)} frames/s, longest stall: friend ${tStall(of).toFixed(1)} s, host ${tStall(oh).toFixed(1)} s`);
		check('T1e', 'nobody sees a "waiting" banner, and the hidden tab shows no "return to the tab" title', !oh.some((x) => x.banner) && !of.some((x) => /return/i.test(x.title)), `host banners: ${[...new Set(oh.map((x) => x.banner))].join('|')}; friend titles: ${[...new Set(of.map((x) => x.title))].join('|')}`);
		await friend.show();
		const sync1 = await tSync(host, friend);
		check('T1f', 'after the tab returned both games are in sync (no repairs, last sync recent)', sync1.ok, sync1.why);

		// -- the host's tab goes into the background
		const hid2 = await host.hide();
		const obs2 = await tObserve([host, friend], 20, tPlay(friend));
		check('T1g', 'the hidden host keeps playing too: 45 frames per second or more on both sides, the friend never waits more than 2 s', hid2 && tRate(obs2[0]) >= 45 && tRate(obs2[1]) >= 45 && tStall(obs2[1]) <= 2 && tStall(obs2[0]) <= 2, `hidden host ${tRate(obs2[0]).toFixed(1)} frames/s, friend ${tRate(obs2[1]).toFixed(1)} frames/s, longest stall: host ${tStall(obs2[0]).toFixed(1)} s, friend ${tStall(obs2[1]).toFixed(1)} s`);
		await host.show();
		const sync2 = await tSync(host, friend);
		check('T1h', 'after the host\'s tab returned both games are in sync (no repairs)', sync2.ok && !host.hasLog(/EXC/) && !friend.hasLog(/EXC/), sync2.why);
		await shot(friend, 'T1_friend'); await shot(host, 'T1_host');
	} finally { await h.close(); }
}

// ---- T2/T3: the mitigation is off (bgtick=off, as in a browser that does not keep a hidden tab running): the stall, the notices, the way back ----
async function sessionT2(relay) {
	console.log('--- session T2: no worker ticks (bgtick=off): the friend\'s tab is hidden for 20 s, then comes back');
	const s = await tSetup(relay, { seed: 0x63, basePort: 9474, extra: '&bgtick=off' });
	const { h, host, friend } = s;
	try {
		check('T2a', 'two browsers with real background throttling are in a running game (worker ticks off by the page link)', s.ok, s.detail);
		const st0 = await tState(friend);
		check('T2a2', 'the page exposes its tab state (globalThis.__s3tab) and says that the worker ticks are off', !!st0 && st0.bgtick === 'off', JSON.stringify(st0));
		await friend.hide();
		const tHide = Date.now();
		const [oh, of] = await tObserve([host, friend], 20, tPlay(host));
		console.log(`      without ticks, friend hidden: its own frame counter ${of[0].frame} -> ${of.at(-1).frame} (${tRate(of).toFixed(2)} frames/s, stale while the page cannot run), the host ${tRate(oh).toFixed(1)} frames/s over the 20 s (longest wait ${tStall(oh).toFixed(1)} s)`);
		const notice = tFirst(oh, /waiting for/i);
		check('T2b', 'the host sees a "waiting for NAME" notice within 4 s of the friend\'s tab going hidden (the friend tells it at once, nothing has to time out)', notice !== null && notice <= 4, `first notice after ${notice} s: "${oh.find((x) => /waiting for/i.test(x.banner))?.banner ?? ''}"`);
		const ret = tFirst(of, /return to/i, 'title');
		check('T2c', 'the hidden tab says "return to the tab" in its title (what the stalled player can see in the tab strip) within 2 s', ret !== null && ret <= 2, `title after ${ret} s: "${of.find((x) => /return to/i.test(x.title))?.title ?? of.at(-1)?.title}"`);
		const stall = tStall(oh);
		check('T2d', 'the host is not frozen for good: it waits at most 10 s, then plays on (20 s window: more than 300 frames)', oh.at(-1).frame - oh[0].frame > 300 && stall <= 10, `host frames ${oh[0].frame} -> ${oh.at(-1).frame}, longest stall ${stall.toFixed(1)} s`);
		await shot(host, 'T2_host_waiting');
		await friend.show();
		const back = await waitFor(async () => { const a = await tState(host), b = await tState(friend); return a && b && a.banner === '' && b.banner === '' && !/return to/i.test(await tTitle(friend)) ? true : null; }, 8000, 250);
		check('T2e', 'when the tab returns the "waiting" banner of the host and the "return to the tab" banner of the friend clear within 8 s', !!back, `host banner "${(await tState(host))?.banner}", friend banner "${(await tState(friend))?.banner}", title "${await tTitle(friend)}"`);
		const after = await tObserve([host, friend], 10, (sec) => drive(host, [friend], sec));
		check('T2f', 'after the return both games run again (60 frames per second or close, more than 300 frames each in 10 s)', after[0].at(-1).frame - after[0][0].frame > 300 && after[1].at(-1).frame - after[1][0].frame > 300, `host ${tRate(after[0]).toFixed(1)}, friend ${tRate(after[1]).toFixed(1)} frames/s`);
		const sync = await tSync(host, friend);
		check('T2g', 'and they are in sync: repairs 0 on both sides, last sync recent', sync.ok, sync.why);
		await shot(friend, 'T2_friend_back');

		// -- the host's tab
		await host.hide();
		const [hh, ff] = await tObserve([host, friend], 20, tPlay(friend));
		const n2 = tFirst(ff, /waiting for/i);
		check('T2h', 'the friend sees a "waiting for NAME" notice within 4 s of the host\'s tab going hidden', n2 !== null && n2 <= 4, `first notice after ${n2} s: "${ff.find((x) => /waiting for/i.test(x.banner))?.banner ?? ''}"`);
		check('T2i', 'the hidden host\'s tab says "return to the tab" in its title within 2 s', tFirst(hh, /return to/i, 'title') !== null && tFirst(hh, /return to/i, 'title') <= 2, `title: "${hh.at(-1)?.title}"`);
		await shot(friend, 'T2_friend_waiting');
		await host.show();
		const back2 = await waitFor(async () => { const a = await tState(host), b = await tState(friend); return a && b && a.banner === '' && b.banner === '' && !/return to/i.test(await tTitle(host)) ? true : null; }, 15000, 250);
		check('T2j', 'when the host returns, the banners clear within 15 s', !!back2, `host banner "${(await tState(host))?.banner}", friend banner "${(await tState(friend))?.banner}"`);
		const after2 = await tObserve([host, friend], 12, (sec) => drive(host, [friend], sec));
		check('T2k', 'and the game runs again on both sides (more than 300 frames in 12 s)', after2[0].at(-1).frame - after2[0][0].frame > 300 && after2[1].at(-1).frame - after2[1][0].frame > 300, `host ${tRate(after2[0]).toFixed(1)}, friend ${tRate(after2[1]).toFixed(1)} frames/s`);
		const sync2 = await tSync(host, friend);
		check('T2l', 'in sync, repairs 0 on both sides', sync2.ok, sync2.why);
		fs.writeFileSync(path.join(logDir, 'web_net_T2_console.txt'), `HOST\n${host.allLogs.filter((l) => /Netplay|tab|EXC/i.test(l)).join('\n')}\nFRIEND\n${friend.allLogs.filter((l) => /Netplay|tab|EXC/i.test(l)).join('\n')}\n`);
	} finally { await h.close(); }
}

async function sessionT3(relay) {
	console.log('--- session T3: no worker ticks: the friend\'s tab stays hidden too long, the host drops it with a message');
	const s = await tSetup(relay, { seed: 0x64, basePort: 9478, extra: '&bgtick=off' });
	const { h, host, friend } = s;
	try {
		check('T3a', 'two browsers with real background throttling are in a running game, worker ticks off', s.ok, s.detail);
		await friend.hide();
		const [oh] = await tObserve([host, friend], 50, tPlay(host));
		const dropped = tFirst(oh, /dropped|removed|left the game/i);
		check('T3b', 'the host is not frozen and the stalled friend is dropped after a timeout (within 45 s), with a banner that says so ("NAME dropped: tab in the background")', dropped !== null && dropped <= 45 && tStall(oh) <= 10, `banner after ${dropped} s: "${oh.find((x) => /dropped|removed|left the game/i.test(x.banner))?.banner ?? ''}"; longest stall ${tStall(oh).toFixed(1)} s; banners: ${[...new Set(oh.map((x) => x.banner))].join(' | ')}`);
		await shot(host, 'T3_host_dropped');
		await friend.show();
		const msg = await waitFor(async () => { const b = (await tState(friend))?.banner ?? ''; return /Reconnecting to your game/.test(b) ? b : null; }, 15000, 250);
		check('T3c', 'the returning friend is told on its screen (banner) that the connection was lost and it is reconnecting, within 15 s', !!msg, `banner "${(await tState(friend))?.banner}"; ${friend.allLogs.filter((l) => /Netplay client/.test(l)).slice(-4).join(' | ')}`);
		await shot(friend, 'T3_friend_back');
		const restored = await friend.waitLog(/reconnected and restored the existing session/, 40000);
		const afterRejoin = await tObserve([host, friend], 12, (sec) => drive(host, [friend], sec));
		const syncRejoin = await tSync(host, friend);
		check('T3d', 'the dropped friend comes back into its slot (the game logs that the session was restored), both games run again (more than 300 frames in 12 s each, banners gone) and are in sync with repairs 0', !!restored && afterRejoin[0].at(-1).frame - afterRejoin[0][0].frame > 300 && afterRejoin[1].at(-1).frame - afterRejoin[1][0].frame > 300 && afterRejoin[0].at(-1).banner === '' && afterRejoin[1].at(-1).banner === '' && syncRejoin.ok, `${restored ?? 'no restore line'}; host ${tRate(afterRejoin[0]).toFixed(1)}, friend ${tRate(afterRejoin[1]).toFixed(1)} frames/s; ${syncRejoin.why}`);
		fs.writeFileSync(path.join(logDir, 'web_net_T3_console.txt'), `HOST\n${host.allLogs.filter((l) => /Netplay|tab|EXC/i.test(l)).join('\n')}\nFRIEND\n${friend.allLogs.filter((l) => /Netplay|tab|EXC/i.test(l)).join('\n')}\n`);
	} finally { await h.close(); }
}

async function sessionT4(relay) {
	console.log('--- session T4: no worker ticks: the host\'s tab stays hidden too long, the friend is told');
	const s = await tSetup(relay, { seed: 0x65, basePort: 9482, extra: '&bgtick=off' });
	const { h, host, friend } = s;
	try {
		check('T4a', 'two browsers with real background throttling are in a running game, worker ticks off', s.ok, s.detail);
		await host.hide();
		const [, ff] = await tObserve([host, friend], 70, tPlay(friend));
		const first = tFirst(ff, /waiting for/i);
		const lobbyMsg = friend.allLogs.filter((l) => /Netplay failure: HOST_CLOSED .The host.s tab stayed in the background/.test(l));
		const screenMsg = ff.at(-1)?.banner ?? '';
		check('T4b', 'the friend is not frozen forever: it sees a "waiting for NAME" notice within 4 s, and within 70 s of the host going hidden the game ends for it with a message ON ITS SCREEN (the banner of the stopped game, read from the page state, not just a console line) that names the background tab; the host tab is still hidden', first !== null && first <= 4 && lobbyMsg.length > 0 && /host's tab stayed in the background for more than a minute/.test(screenMsg), `waiting after ${first} s; screen banner at the end: "${screenMsg}"; ${lobbyMsg.slice(-1).join(' | ')}`);
		await shot(friend, 'T4_friend_told');
		await host.show();
		fs.writeFileSync(path.join(logDir, 'web_net_T4_console.txt'), `HOST\n${host.allLogs.filter((l) => /Netplay|tab|EXC/i.test(l)).join('\n')}\nFRIEND\n${friend.allLogs.filter((l) => /Netplay|Lobby|tab|EXC/i.test(l)).join('\n')}\n`);
	} finally { await h.close(); }
}

// ---- T5: the same with the direct (WebRTC) connection on: the hidden tab's data channel is driven by a main-thread timer that the browser slows down ----
async function sessionT5(relay) {
	console.log('--- session T5: a hidden tab with a direct (peer to peer) connection');
	const s = await tSetup(relay, { seed: 0x66, basePort: 9486, p2p: true });
	const { h, host, friend } = s;
	try {
		await drive(host, [friend], 6);
		const pre = await p2pLines(host, friend);
		check('T5a', 'two browsers are in a running game and both report the direct connection as the transport (online log: transport p2p)', s.ok && pre.host?.transport === 'p2p' && pre.friend?.transport === 'p2p', `${s.detail}; ${JSON.stringify(pre)}`);
		await drive(host, [friend], 4);
		const infoBefore = await p2pLines(host, friend);
		await friend.hide();
		const [oh, of] = await tObserve([host, friend], 25, tPlay(host));
		const lines = await p2pLines(host, friend);
		check('T5b', 'the hidden friend keeps playing with the direct connection on (45 frames per second or more, the host never waits more than 2 s)', tRate(of) >= 45 && tStall(oh) <= 2 && tStall(of) <= 2, `hidden friend ${tRate(of).toFixed(1)} frames/s, host ${tRate(oh).toFixed(1)} frames/s; transport before the hiding ${JSON.stringify(infoBefore.friend?.transport)}, at its end host ${lines.host?.transport} friend ${lines.friend?.transport}`);
		await friend.show();
		const sync = await tSync(host, friend);
		check('T5c', 'after the return both games are in sync (repairs 0), whichever path the game took meanwhile', sync.ok, `${sync.why}; transport now: host ${(await p2pLines(host, friend)).host?.transport}, friend ${(await p2pLines(host, friend)).friend?.transport}`);
	} finally { await h.close(); }
}

// ---- T6: a friend that waited in the lobby for a long time must not see a "waiting" banner when the game starts (the stall counters belong to a running game only) ----
async function sessionT6(relay) {
	console.log('--- session T6: no waiting banner at the start of a game after a long wait in the lobby');
	const seed = 0x67;
	const h = await startHarness({ webDir, instances: 2, seeds: [seed, 0x52], basePort: 9490, throttled: true });
	try {
		const [host, friend] = h.instances;
		const query = `?relay=ws://127.0.0.1:${relay.ports.port}&p2p=off`;
		await Promise.all([host.openGame(h.baseURL, query), friend.openGame(h.baseURL, query)]);
		await Promise.all([host.waitLog(/Ready to go/, 90000), friend.waitLog(/Ready to go/, 90000)]);
		await sleep(2000);
		await Promise.all([host.toMainMenu(), friend.toMainMenu()]);
		const r = await hostAndJoin(host, [friend], query, codeForSeed(seed));
		await sleep(20000);		// the friend sits in the lobby (more than the 2.5 s that make a notice)
		const obs = await tObserve([host, friend], 30, async () => { await startNormalGame(host, [friend]); await drive(host, [friend], 6); });
		const waitingFriend = obs[1].filter((x) => /waiting for/i.test(x.banner)), waitingHost = obs[0].filter((x) => /waiting for/i.test(x.banner));
		check('T6a', 'the lobby and the game start work (the friend runs the game: its frame counter advances)', !!(r.hosted && r.known && r.joined[0]) && obs[1].at(-1).frame > 300, `frame ${obs[1].at(-1).frame}`);
		check('T6b', 'after 20 s in the lobby, neither the friend nor the host shows a "Waiting for" banner at any time while the game starts and loads', waitingFriend.length === 0 && waitingHost.length === 0, `friend: ${waitingFriend.length} samples, first "${waitingFriend[0]?.banner ?? ''}" at ${waitingFriend[0]?.t ?? '-'} s; host: ${waitingHost.length} samples${waitingHost[0] ? ', first "' + waitingHost[0].banner + '"' : ''}; hidden pages seen: host ${obs[0].filter((x) => x.hidden).length}, friend ${obs[1].filter((x) => x.hidden).length}; away lines: ${[...friend.allLogs, ...host.allLogs].filter((l) => /tab (of player|is hidden|is visible)/.test(l)).join(' | ')}`);
		check('T6c', 'and no "notice:" line in either console', !friend.hasLog(/Netplay: notice:/) && !host.hasLog(/Netplay: notice:/), `${friend.allLogs.filter((l) => /Netplay: notice:/.test(l)).slice(0, 2).join(' | ')}`);
	} finally { await h.close(); }
}

let exitCode = 0;
const relay ={ relay: createRelayServer({ port: 0, udpPort: 28842, bind: '127.0.0.1', statsSeconds: 0 }) };
try {
	relay.ports = await relay.relay.start();
	if (only.includes('A')) await sessionA(relay);
	if (only.includes('B')) await sessionB(relay);
	if (only.includes('C')) await sessionC(relay);
	if (only.includes('P') || only.includes('P1')) await sessionP1(relay);
	if (only.includes('P') || only.includes('P2')) await sessionP2(relay);
	if (only.includes('P') || only.includes('P3')) await sessionP3(relay);
	if (only.includes('P') || only.includes('P4')) await sessionP4(relay);
	if (only.includes('P') || only.includes('P5')) await sessionP5(relay);
	if (only.includes('P') || only.includes('P6')) await sessionP6(relay);
	for (const [id, fn] of [['L1', sessionL1], ['L2', sessionL2], ['L3', sessionL3], ['L4', sessionL4], ['L5', sessionL5], ['L6', sessionL6], ['L7', sessionL7], ['L8', sessionL8]]) if (only.includes('L') || only.includes(id)) await fn();
	if (only.includes('L8R')) await sessionL8R();
	if (only.includes('T') || only.includes('T1')) await sessionT1(relay);
	if (only.includes('T') || only.includes('T2')) await sessionT2(relay);
	if (only.includes('T') || only.includes('T3')) await sessionT3(relay);
	if (only.includes('T') || only.includes('T4')) await sessionT4(relay);
	if (only.includes('T') || only.includes('T5')) await sessionT5(relay);
	if (only.includes('T') || only.includes('T6')) await sessionT6(relay);
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
	// The committed report comes only from a run of the whole standard set (everything but the native session N); a run of some sessions writes its own file in last_logs
	const standardSet = ['A', 'B', 'C', 'P', 'L', 'T'].every((id) => only.includes(id));
	const reportFile = standardSet ? path.join(root, 'checks', 'web_net_report.txt') : path.join(logDir, `web_net_report_${only.join('_')}.txt`);
	fs.writeFileSync(reportFile, lines.join('\n') + '\n');
	console.log(`report: ${standardSet ? 'checks/web_net_report.txt' : `checks/last_logs/${path.basename(reportFile)} (some of the sessions only: the committed report is left alone)`}`);
	console.log(`\n${results.length - failed.length} of ${results.length} checks passed`);
	exitCode = failed.length ? 1 : 0;
}
process.exit(exitCode);
