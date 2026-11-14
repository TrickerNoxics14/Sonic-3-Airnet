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

export function startWebServer(webDir, romPath, port = 0) {
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
		const data = fs.readFileSync(file);
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

	async launch(chrome, debugPort, profileDir, extraSeedFill) {
		this.profileDir = profileDir;
		this.process = spawn(chrome, ['--headless=new', `--remote-debugging-port=${debugPort}`, `--user-data-dir=${profileDir}`, '--window-size=1280,720', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
			'--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
			'--no-first-run', '--no-default-browser-check', 'about:blank'], { stdio: 'ignore' });
		let list;
		for (let i = 0; i < 100 && !list; ++i) {
			try { const targets = await (await fetch(`http://127.0.0.1:${debugPort}/json`)).json(); if (targets.find((t) => t.type === 'page')) list = targets; } catch { /* not up yet */ }
			if (!list) await sleep(200);
		}
		if (!list) throw new Error('Chrome did not start');
		const page = list.find((t) => t.type === 'page');
		this.ws = new WebSocket(page.webSocketDebuggerUrl);
		await new Promise((resolve) => { this.ws.onopen = resolve; });
		this.ws.onmessage = (event) => this.#onMessage(JSON.parse(event.data));
		await this.send('Runtime.enable');
		await this.send('Page.enable');
		if (extraSeedFill !== undefined) await this.send('Page.addScriptToEvaluateOnNewDocument', { source: seedScript(extraSeedFill) });
	}

	#onMessage(m) {
		if (m.id && this.pending.has(m.id)) { this.pending.get(m.id)(m); this.pending.delete(m.id); }
		else if (m.method === 'Runtime.consoleAPICalled') { const line = `${m.params.type} ${m.params.args.map((a) => a.value ?? a.description).join(' ')}`; this.logs.push(line); this.allLogs.push(line); if (this.allLogs.length > 20000) this.allLogs.splice(0, 10000); }
		else if (m.method === 'Runtime.exceptionThrown') this.logs.push(`EXC ${m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text}`);
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

	/** Opens the page, hands the player's ROM to the page like the file picker does, and starts the game. */
	async openGame(baseURL, query = '') {
		await this.navigate(`${baseURL}/sonic3air_web.html${query}`);
		for (let i = 0; i < 120; ++i) {
			const state = await this.eval(`(typeof Module !== 'undefined' && Module.calledRun ? 'run' : 'wait') + ':' + (document.getElementById('s3kPromptMessage') ? 'prompt' : 'noprompt') + ':' + (typeof FS !== 'undefined')`).catch(() => 'wait');
			if (/^run:prompt:true/.test(state)) break;
			await sleep(250);
		}
		const result = await this.eval(`(async () => {
			const data = new Uint8Array(await (await fetch('/__rom.bin')).arrayBuffer());
			if (data.length !== 4194304) return 'rom size ' + data.length;
			Module['FS_createDataFile']('/sonic3air/savedata/', 'Sonic_Knuckles_wSonic3.bin', data, true, true, true);
			await new Promise((resolve) => FS.syncfs(resolve));
			hidePageUI();
			if (!FS.analyzePath('/sonic3air/___internal/').exists) FS.mkdir('/sonic3air/___internal/');
			startGame();
			return 'started';
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
export async function startHarness({ webDir, romPath = defaultRomPath(), instances = 1, seeds = [], basePort = 9340 }) {
	const chrome = findChrome();
	if (!chrome) throw new Error('No Chrome found (set CHROME)');
	if (!fs.existsSync(path.join(webDir, 'sonic3air_web.html'))) throw new Error(`web build not found in ${webDir}`);
	if (!fs.existsSync(romPath)) throw new Error('ROM not found (set S3AIR_ROM)');
	const { server, port } = await startWebServer(webDir, romPath);
	const harness = { server, port, baseURL: `http://127.0.0.1:${port}`, instances: [] };
	for (let i = 0; i < instances; ++i) {
		const instance = new Instance(i, harness);
		const profile = fs.mkdtempSync(path.join(os.tmpdir(), `s3air-web-${i}-`));
		await instance.launch(chrome, basePort + i, profile, seeds[i]);
		harness.instances.push(instance);
	}
	harness.close = async () => {
		for (const instance of harness.instances) await instance.close();
		await new Promise((resolve) => { server.closeAllConnections?.(); server.close(resolve); });
	};
	return harness;
}
