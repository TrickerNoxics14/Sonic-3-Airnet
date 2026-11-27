// Test harness for the browser build: a static web server for web_build, headless Chrome instances driven over the DevTools protocol, and key input.
// The player's own ROM is served from where the game normally keeps it (%APPDATA%\Sonic3AIR) through a route on 127.0.0.1 only, so it is never copied anywhere.
// Nothing here stays running: close() stops the browsers (and deletes their temporary profiles) and the web server.
import { spawn } from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json', '.png': 'image/png', '.svg': 'image/svg+xml', '.data': 'application/octet-stream' };

export function findChrome() {
	const candidates = [process.env.CHROME, 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe', 'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
		'/usr/bin/google-chrome', '/usr/bin/chromium', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
	return candidates.find((p) => p && fs.existsSync(p));
}

export function defaultRomPath() {
	if (process.env.S3AIR_ROM) return process.env.S3AIR_ROM;
	const base = process.env.APPDATA ? path.join(process.env.APPDATA, 'Sonic3AIR') : path.join(os.homedir(), '.config', 'Sonic3AIR');
	return path.join(base, 'Sonic_Knuckles_wSonic3.bin');
}

// patchFile(name, data) may return other bytes for a file (used to serve an "old version" of the page: the same build with another netplay compatibility text)
export function startWebServer(webDir, romPath, port = 0, patchFile = null) {
	const server = http.createServer((request, response) => {
		const url = new URL(request.url, 'http://x');
		response.setHeader('Cache-Control', 'no-store');
		if (url.pathname === '/__rom.bin' && romPath) {
			const data = fs.readFileSync(romPath);
			response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': data.length });
			response.end(data);
			return;
		}
		let file = path.normalize(path.join(webDir, decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname)));
		if (!file.startsWith(path.normalize(webDir)) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { response.writeHead(404); response.end('not found'); return; }
		let data = fs.readFileSync(file);
		if (patchFile) data = patchFile(path.basename(file), data) ?? data;
		response.writeHead(200, { 'Content-Type': MIME[path.extname(file)] ?? 'application/octet-stream', 'Content-Length': data.length });
		response.end(data);
	});
	return new Promise((resolve) => server.listen(port, '127.0.0.1', () => resolve({ server, port: server.address().port })));
}

const KEYS = {
	Return: [13, 'Enter', 'Enter'], Up: [38, 'ArrowUp', 'ArrowUp'], Down: [40, 'ArrowDown', 'ArrowDown'], Left: [37, 'ArrowLeft', 'ArrowLeft'], Right: [39, 'ArrowRight', 'ArrowRight'],
	s: [83, 's', 'KeyS'], a: [65, 'a', 'KeyA'], d: [68, 'd', 'KeyD'], Space: [32, ' ', 'Space'], Escape: [27, 'Escape', 'Escape'], Backspace: [8, 'Backspace', 'Backspace'],
};

// Makes "random" numbers of the page predictable: every byte is `fill` (so the host's lobby code is known in advance) plus an offset per call for anything beyond the first ones.
export const seedScript = (fill) => `(() => { const real = crypto.getRandomValues.bind(crypto); crypto.getRandomValues = function (a) { if (a instanceof Uint8Array || a instanceof Uint32Array) { const bytes = new Uint8Array(a.buffer, a.byteOffset, a.byteLength); bytes.fill(${fill}); return a; } return real(a); }; })();`;

export class Instance {
	constructor(index, harness) {
		this.index = index;
		this.harness = harness;
		this.logs = [];
		this.allLogs = [];
		this.pending = new Map();
		this.nextID = 0;
	}

	async launch(chrome, debugPort, profileDir, extraSeedFill, throttled = false) {
		this.debugPort = debugPort;
		this.profileDir = profileDir;
		this.process = spawn(chrome, ['--headless=new', `--remote-debugging-port=${debugPort}`, `--user-data-dir=${profileDir}`, '--window-size=1280,720', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
			'--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required', ...(throttled ? ['--disable-backgrounding-occluded-windows'] : ['--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows']),
			'--disable-features=WebRtcHideLocalIpsWithMdns', '--no-first-run', '--no-default-browser-check', 'about:blank'], { stdio: 'ignore' });
		let list;
		for (let i = 0; i < 100 && !list; ++i) {
			try { const targets = await (await fetch(`http://127.0.0.1:${debugPort}/json`)).json(); if (targets.find((t) => t.type === 'page')) list = targets; } catch { /* not up yet */ }
			if (!list) await sleep(200);
		}
		if (!list) throw new Error('Chrome did not start');
		const page = list.find((t) => t.type === 'page');
		this.pageID = page.id;
		this.ws = new WebSocket(page.webSocketDebuggerUrl);
		await new Promise((resolve) => { this.ws.onopen = resolve; });
		this.ws.onmessage = (event) => this.#onMessage(JSON.parse(event.data));
		await this.send('Runtime.enable');
		await this.send('Page.enable');
		if (extraSeedFill !== undefined) await this.send('Page.addScriptToEvaluateOnNewDocument', { source: seedScript(extraSeedFill) });
	}

	/**
	 * Puts this tab in the background the way a player does: a second tab opens in the same browser window and takes the front, so the browser itself makes the game tab hidden
	 * (document.visibilityState 'hidden', no animation frames, timers about once a second). Only real if the browser was started with `throttled: true` (without the flags
	 * that switch background throttling off; only the occlusion flag stays, because two overlapping browser windows on one screen made one of them 'hidden' by occlusion in a run, which a tab switch is not). Returns once the page reports 'hidden'.
	 */
	async hide() {
		const r = await fetch(`http://127.0.0.1:${this.debugPort}/json/new?about:blank`, { method: 'PUT' });
		this.otherTabID = (await r.json()).id;
		for (let i = 0; i < 50; ++i) { if (await this.eval('document.visibilityState') === 'hidden') return true; await sleep(100); }
		return false;
	}

	/** Brings the game tab back to the front (and closes the tab that was opened to hide it). */
	async show() {
		await (await fetch(`http://127.0.0.1:${this.debugPort}/json/activate/${this.pageID}`)).text();
		for (let i = 0; i < 50; ++i) { if (await this.eval('document.visibilityState') === 'visible') break; await sleep(100); }
		if (this.otherTabID) { try { await (await fetch(`http://127.0.0.1:${this.debugPort}/json/close/${this.otherTabID}`)).text(); } catch { /* gone */ } this.otherTabID = null; }
		return (await this.eval('document.visibilityState')) === 'visible';
	}

	/** Runs a script in every page this browser loads from now on, before the page's own scripts (test hooks, e.g. to break WebRTC). */
	async addScript(source) { await this.send('Page.addScriptToEvaluateOnNewDocument', { source }); }

	#onMessage(m) {
		if (m.id && this.pending.has(m.id)) { this.pending.get(m.id)(m); this.pending.delete(m.id); }
		else if (m.method === 'Runtime.consoleAPICalled') { const line = `${m.params.type} ${m.params.args.map((a) => a.value ?? a.description).join(' ')}`; this.logs.push(line); this.allLogs.push(line); if (this.allLogs.length > 20000) this.allLogs.splice(0, 10000); this.#trackSelection(line); }
		else if (m.method === 'Runtime.exceptionThrown') this.logs.push(`EXC ${m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text}`);
	}

	// The lobby menu logs "Lobby: screen X" when a screen is built, then "Lobby: selected LABEL" for the entry under the cursor (and again whenever it changes)
	#trackSelection(line) {
		if (/Lobby: screen \w+/.test(line)) { this.lastSelected = null; this.lastScreen = /Lobby: screen (\w+)/.exec(line)[1]; }
		const m = /Lobby: selected ([^\r\n]*)/.exec(line);		// (a console line can end with a line break)
		if (m) { this.lastSelected = m[1].trim(); this.selectionCount = (this.selectionCount ?? 0) + 1; }
	}

	/** In the lobby menu: moves the cursor (Down, or `direction`) until the entry `label` is selected, then presses Return (unless `press` is false).
	 *  `screen` waits for that screen first. A label is the entry's text up to its ':' (e.g. "CHARACTER"), "ROW <lobby name>" for a list row, "PAD 7" for a pad key. */
	async choose(label, { screen = null, press = true, maxSteps = 16, direction = 'Down' } = {}) {
		if (screen) {
			const end = Date.now() + 10000;
			while (Date.now() < end && this.lastScreen !== screen) await sleep(100);
			if (this.lastScreen !== screen) throw new Error(`choose: the screen ${screen} did not open (on ${this.lastScreen})`);
		}
		const until = Date.now() + 5000;
		while (Date.now() < until && this.lastSelected == null) await sleep(100);
		if (this.lastSelected == null) throw new Error(`choose: the lobby menu does not say which entry is selected (on ${this.lastScreen}, looking for "${label}")`);
		for (let step = 0; step <= maxSteps; ++step) {
			if (this.lastSelected != null && (label instanceof RegExp ? label.test(this.lastSelected) : this.lastSelected === label)) {
				if (press) { await sleep(150); await this.key('Return'); await sleep(400); }
				return true;
			}
			if (step === maxSteps) break;
			const before = this.selectionCount ?? 0;
			await this.key(direction);
			const end = Date.now() + 1500;
			while (Date.now() < end && (this.selectionCount ?? 0) === before) await sleep(50);
		}
		throw new Error(`choose: "${label}" not found on ${this.lastScreen} (the cursor is on "${this.lastSelected}")`);
	}

	send(method, params = {}) {
		return new Promise((resolve) => { const id = ++this.nextID; this.pending.set(id, resolve); this.ws.send(JSON.stringify({ id, method, params })); });
	}

	async eval(expression) {
		const r = await this.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
		if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description ?? 'evaluation failed');
		return r.result.result.value;
	}

	async navigate(url) { await this.send('Page.navigate', { url }); }

	async key(name, hold = 90) {
		const [vk, key, code] = KEYS[name];
		const base = { key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk };
		await this.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base });
		await sleep(hold);
		await this.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
	}

	async keys(names, gap = 250, hold = 90) { for (const name of names) { await this.key(name, hold); await sleep(gap); } }

	/** Holds a key for `ms` (for movement in the game). */
	async hold(name, ms) {
		const [vk, key, code] = KEYS[name];
		const base = { key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk };
		await this.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base });
		await sleep(ms);
		await this.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
	}

	/** Types text into a game text field: one key press per character. */
	async type(text, gap = 120) {
		for (const ch of text) {
			const upper = ch.toUpperCase();
			const code = /[A-Z]/.test(upper) ? `Key${upper}` : (/[0-9]/.test(ch) ? `Digit${ch}` : '');
			const vk = upper.charCodeAt(0);
			const base = { key: ch, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk };
			await this.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', ...base });
			await this.send('Input.dispatchKeyEvent', { type: 'char', ...base, text: ch, unmodifiedText: ch });
			await sleep(30);
			await this.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
			await sleep(gap);
		}
	}

	/** A mouse click at page coordinates (x, y), 'left' or 'right' (the right button opens the game's own context menu). The pointer moves there first, as a real mouse does. */
	async click(x, y, button = 'left') {
		const mask = button === 'right' ? 2 : 1;
		const event = (type, extra = {}) => this.send('Input.dispatchMouseEvent', { type, x, y, button: 'none', buttons: 0, ...extra });
		await event('mouseMoved');
		await sleep(120);
		await event('mousePressed', { button, buttons: mask, clickCount: 1 });
		await sleep(80);
		await event('mouseReleased', { button, buttons: 0, clickCount: 1 });
		await sleep(400);
	}

	/** Makes the page area this big (a phone, a small window); without arguments it goes back to the window's own size. */
	async resize(width, height) {
		if (width) await this.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
		else await this.send('Emulation.clearDeviceMetricsOverride');
	}

	async shot(file) {
		const r = await this.send('Page.captureScreenshot', { format: 'png' });
		fs.writeFileSync(file, Buffer.from(r.result.data, 'base64'));
	}

	takeLogs() { return this.logs.splice(0); }

	/** True if a console line since the start (or since the last forgetLogs) matches. */
	hasLog(pattern) { return this.allLogs.some((line) => pattern.test(line)); }
	forgetLogs() { this.allLogs.length = 0; }

	/** Waits until a console line matches; returns the line, or null after `timeout` ms. */
	async waitLog(pattern, timeout = 30000) {
		const end = Date.now() + timeout;
		while (Date.now() < end) {
			const hit = this.allLogs.find((line) => pattern.test(line));
			if (hit) return hit;
			await sleep(200);
		}
		return null;
	}

	/** From a freshly started game to the main menu (the cursor is on NORMAL GAME afterwards). */
	async toMainMenu() {
		const count = () => this.allLogs.filter((l) => /Menu: main menu/.test(l)).length;
		for (let i = 0; i < 40; ++i) {
			const before = count();
			await this.key('Escape');
			const end = Date.now() + 2500;
			while (Date.now() < end && count() === before) await sleep(200);
			if (count() > before) { await sleep(600); return true; }
			await this.key('Return');
			await sleep(2500);
			if (count() > before) { await sleep(600); return true; }
		}
		return false;
	}

	/** From the main menu (cursor on NORMAL GAME) to the lobby's first screen. */
	async toLobby() {
		this.forgetLogs();
		await this.keys(['Down', 'Down', 'Down', 'Return'], 600);
		return !!(await this.waitLog(/Lobby: screen START/, 10000));
	}

	/** From the ONLINE menu's first screen: CREATE LOBBY, then OPEN LOBBY with the defaults (public), or as a private lobby (not on the public list: the only kind
	 *  that makes direct connections). */
	async createLobby({ privateLobby = false } = {}) {
		await this.toStart();
		await this.choose('CREATE LOBBY', { screen: 'START' });
		if (privateLobby) {
			await this.choose('LOBBY', { screen: 'CREATE', press: false });
			await this.key('Right');		// (public -> private)
			await sleep(400);
		}
		await this.choose('OPEN LOBBY', { screen: 'CREATE' });
	}

	/** Back to the ONLINE menu's first screen from one of its sub screens (a failed QUICK JOIN, for one, stays on the list it was started from). */
	async toStart() {
		if (['LIST', 'CREATE', 'SETTINGS', 'LAN', 'JOIN'].includes(this.lastScreen)) {
			await this.key('Escape');
			const end = Date.now() + 8000;
			while (Date.now() < end && this.lastScreen !== 'START') await sleep(100);
			await sleep(300);
		}
	}

	/** From the ONLINE menu's first screen to the code screen (JOIN WITH CODE). */
	async openJoin() {
		await this.toStart();
		await this.choose('JOIN WITH CODE', { screen: 'START' });
	}

	/** From the ONLINE menu's first screen to the list of public lobbies (FIND A LOBBY); from the list itself it goes back and opens it again. */
	async openList() {
		await this.toStart();
		await this.choose('FIND A LOBBY', { screen: 'START' });
	}

	/** QUICK JOIN: the first row of the list (FIND A LOBBY). */
	async quickJoin() {
		const shows = () => this.allLogs.filter((l) => /Lobby: the list shows/.test(l)).length;
		const before = shows();
		await this.openList();
		// (the first list that arrives puts the cursor on the first lobby that can be joined: wait for it, a few seconds at most, a server that is down sends none)
		const end = Date.now() + 4000;
		while (Date.now() < end && shows() === before) await sleep(100);
		await this.choose('QUICK JOIN', { screen: 'LIST', direction: 'Up' });
	}

	/** From the ONLINE menu's first screen to the online settings. */
	async openSettings() {
		await this.toStart();
		await this.choose('SETTINGS', { screen: 'START' });
	}

	/** In a lobby (host or friend): LEAVE LOBBY. */
	async leaveLobby() {
		await this.choose('LEAVE LOBBY', { direction: 'Up' });
	}

	/** Opens the page, hands the player's ROM to the page like the file picker does, and starts the game. */
	async openGame(baseURL, query = '') {
		await this.navigate(`${baseURL}/sonic3air_web.html${query}`);
		for (let i = 0; i < 120; ++i) {
			const state = await this.eval(`(typeof Module !== 'undefined' && Module.calledRun ? 'run' : 'wait') + ':' + (document.getElementById('s3kPromptMessage') ? 'prompt' : 'noprompt') + ':' + (typeof FS !== 'undefined' && FS.analyzePath('/sonic3air/savedata').exists)`).catch(() => 'wait');
			if (/^run:prompt:true/.test(state)) break;
			await sleep(250);
		}
		const result = await this.eval(`(async () => {
			let step = 'fetch the ROM';
			try {
				const data = new Uint8Array(await (await fetch('/__rom.bin')).arrayBuffer());
				if (data.length !== 4194304) return 'rom size ' + data.length;
				step = 'create the ROM file'; if (!FS.analyzePath('/sonic3air/savedata').exists) FS.mkdir('/sonic3air/savedata'); Module['FS_createDataFile']('/sonic3air/savedata/', 'Sonic_Knuckles_wSonic3.bin', data, true, true, true);
				step = 'sync the file system'; await new Promise((resolve) => FS.syncfs(resolve));
				step = 'hide the page UI'; hidePageUI();
				step = 'make the internal folder'; if (!FS.analyzePath('/sonic3air/___internal/').exists) FS.mkdir('/sonic3air/___internal/');
				step = 'start the game'; startGame();
				return 'started';
			} catch (e) { return 'failed at "' + step + '": ' + (e && (e.message || e.name)) + ' errno=' + (e && e.errno); }
		})()`);
		if (result !== 'started') throw new Error(`could not start the game: ${result}`);
	}

	/** Contents of the game's own online log (written every 5 seconds while a session runs), or ''. */
	async onlineLog() {
		return this.eval(`(() => { for (const p of ['/sonic3air/savedata/online_log.txt']) { try { return FS.readFile(p, { encoding: 'utf8' }); } catch (e) {} } return ''; })()`);
	}

	async close() {
		try { this.ws?.close(); } catch { /* closed */ }
		if (this.process && !this.process.killed) {
			this.process.kill();
			await sleep(400);
		}
		for (let i = 0; i < 5; ++i) {
			try { fs.rmSync(this.profileDir, { recursive: true, force: true }); break; } catch { await sleep(500); }
		}
	}
}

/**
 * @param {object} options { webDir, romPath, instances, seeds: [fill byte per instance] }
 */
export async function startHarness({ webDir, romPath = defaultRomPath(), instances = 1, seeds = [], basePort = 9340, throttled = false }) {
	const chrome = findChrome();
	if (!chrome) throw new Error('No Chrome found (set CHROME)');
	if (!fs.existsSync(path.join(webDir, 'sonic3air_web.html'))) throw new Error(`web build not found in ${webDir}`);
	if (!fs.existsSync(romPath)) throw new Error('ROM not found (set S3AIR_ROM)');
	// The checks never use the published page's relay (relay_config.js names the live server): every session gives its own local one in the link (?relay=...)
	const noPublishedRelay = (name, data) => name === 'relay_config.js' ? Buffer.from(data.toString('utf8').replace(/window\.S3AIR_RELAY_DEFAULT\s*=\s*'[^']*'/, "window.S3AIR_RELAY_DEFAULT = ''")) : null;
	const { server, port } = await startWebServer(webDir, romPath, 0, noPublishedRelay);
	const harness = { server, port, baseURL: `http://127.0.0.1:${port}`, instances: [] };
	for (let i = 0; i < instances; ++i) {
		const instance = new Instance(i, harness);
		const profile = fs.mkdtempSync(path.join(os.tmpdir(), `s3air-web-${i}-`));
		await instance.launch(chrome, basePort + i, profile, seeds[i], throttled);
		harness.instances.push(instance);
	}
	harness.close = async () => {
		for (const instance of harness.instances) await instance.close();
		await new Promise((resolve) => { server.closeAllConnections?.(); server.close(resolve); });
	};
	return harness;
}
