// Pictures of every screen of the online menu in the browser build (1280x720), with the longest names the game allows (12 letters for a player, 20 for a lobby,
// all "W", the widest letter), to look at by eye: nothing may run into the next column or off the screen. Needs the web build (checks\build_web.ps1) and a ROM
// where the web_net checks find it.
//   node checks/web_net/lobby_screens_tour.mjs [output folder]     (default: checks/last_logs/lobby_tour)
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { startHarness } from './harness.mjs';
import { createRelayServer } from '../relay_ws/server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const out = path.resolve(process.argv[2] ?? path.join(root, 'checks', 'last_logs', 'lobby_tour'));
fs.mkdirSync(out, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const relay = createRelayServer({ port: 0, udp: false, bind: '127.0.0.1', statsSeconds: 0 });
const ports = await relay.start();
const h = await startHarness({ webDir: path.join(root, 'web_build'), instances: 2, seeds: [0x61, 0x62], basePort: 9680 });
const shot = async (inst, name) => { await sleep(700); await inst.shot(path.join(out, `${name}.png`)); console.log('picture', name); };
const setText = async (inst, text, name) => {
	await sleep(500);
	await inst.keys(Array(24).fill('Backspace'), 60);
	await inst.type(text, 60);
	await sleep(300);
	await shot(inst, name);
	await inst.key('Return');
	await sleep(800);
};
try {
	const [a, b] = h.instances;
	for (const p of [a, b]) {
		await p.send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 720, deviceScaleFactor: 1, mobile: false });
		await p.openGame(h.baseURL, `?relay=ws://127.0.0.1:${ports.port}&p2p=off`);
	}
	for (const p of [a, b]) await p.waitLog(/Ready to go/, 90000);
	await sleep(3000);
	for (const p of [a, b]) await p.toMainMenu();

	// A: the longest player name and lobby name, then a public lobby
	await a.toLobby();
	await shot(a, 'home');
	await a.openSettings();
	await shot(a, 'settings');
	await a.choose('NAME', { screen: 'SETTINGS' });
	await setText(a, 'WWWWWWWWWWWW', 'text_player_name');
	await a.toStart();
	await a.choose('CREATE LOBBY', { screen: 'START' });
	await shot(a, 'create');
	await a.choose('NAME', { screen: 'CREATE' });
	await setText(a, 'WWWWWWWWWWWWWWWWWWWW', 'text_lobby_name');
	await a.choose('OPEN LOBBY', { screen: 'CREATE' });
	await a.waitLog(/Lobby: screen HOSTING/, 10000);
	await sleep(4000);
	await shot(a, 'room_host_alone');

	// B: the code screen, the list with that lobby, then quick join
	await b.toLobby();
	await b.choose('JOIN WITH CODE', { screen: 'START' });
	await b.type('123');
	await shot(b, 'join');
	await b.key('Escape');
	await b.openList();
	await b.waitLog(/Lobby: the list shows 1 public lobb/, 15000);
	await shot(b, 'list');
	await b.choose('QUICK JOIN', { screen: 'LIST', direction: 'Up' });
	await b.waitLog(/the host proved the code, player slot 2/, 40000);
	await sleep(3000);
	await b.choose('CHAT', { screen: 'JOINED' });		// (sends the quick message)
	await sleep(1500);
	await shot(b, 'room_friend');
	await shot(a, 'room_host_two');
	await a.choose('OPTIONS', { screen: 'HOSTING' });
	await shot(a, 'options_host');
	await b.choose('OPTIONS', { screen: 'JOINED' });
	await shot(b, 'options_friend');
	await b.key('Escape');
	await b.choose('READY', { screen: 'JOINED' });
	await sleep(1500);
	await a.key('Escape');
	await shot(a, 'room_host_ready');
	console.log(`pictures in ${out}`);
} finally { await h.close(); await relay.stop(); }
