#!/usr/bin/env node
// Sonic 3 AirNet relay server: forwards the game's online datagrams between players.
//   - native (Windows) games talk UDP to it,
//   - browser games talk WebSocket to it (a browser has no UDP),
//   and both kinds of players can be in the same lobby. The payload is never read; the game checks its peers end to end.
// No dependencies, Node.js 18 or newer. Run:  node server.mjs        (settings below, all optional)
//
//   PORT=28842            TCP port for WebSocket + the /health page (hosting platforms set this themselves)
//   UDP_PORT=28842        UDP port for native games; UDP_PORT=0 turns UDP off (for hosts that only offer HTTP)
//   BIND=0.0.0.0          address to listen on
//   TRUST_PROXY=0         1 = the server is behind a reverse proxy / platform router that sets X-Forwarded-For: use it as the player's address
//   ALLOWED_ORIGINS=      comma separated list of web page origins that may connect (empty = every page); native games send no Origin
//   MAX_LOBBIES=512       lobbies at the same time
//   MAX_CONNECTIONS_PER_IP=32   WebSocket connections per player address
//   STATS_SECONDS=60      print a counter line this often (0 = never). Only counters are printed, never addresses, codes or payloads.
import http from 'node:http';
import dgram from 'node:dgram';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { RelayCore } from './relay_core.mjs';
import { acceptUpgrade } from './ws.mjs';
import { DEFAULT_PORT, MAX_DATAGRAM_SIZE } from './protocol.mjs';

const nowSeconds = () => performance.now() / 1000;
const cleanIP = (ip) => (ip ?? '').replace(/^::ffff:/, '');

export function createRelayServer(options = {}) {
	const {
		port = DEFAULT_PORT, udpPort = port, bind = '0.0.0.0', trustProxy = false, allowedOrigins = [],
		maxConnectionsPerIP = 32, pingSeconds = 20, statsSeconds = 0, quiet = true, limits = {},
	} = options;

	const wsConnections = new Map();      // endpoint key -> WsConnection
	const connectionsPerIP = new Map();
	let nextWsID = 1;
	let udp = null;

	const core = new RelayCore((endpoint, buffer) => {
		if (endpoint.startsWith('ws:')) {
			wsConnections.get(endpoint)?.send(buffer);
		} else if (udp) {
			const split = endpoint.lastIndexOf(':');
			udp.send(buffer, Number(endpoint.slice(split + 1)), endpoint.slice(4, split));
		}
	}, limits);

	const server = http.createServer((request, response) => {
		response.setHeader('Access-Control-Allow-Origin', '*');
		response.setHeader('Cache-Control', 'no-store');
		if (request.url === '/health' || request.url?.startsWith('/health?')) {
			const s = core.stats;
			response.writeHead(200, { 'Content-Type': 'application/json' });
			response.end(JSON.stringify({ ok: true, protocol: 2, lobbies: s.lobbies, friends: s.pipes, websocketClients: wsConnections.size, forwarded: s.forwarded }));
			return;
		}
		response.writeHead(200, { 'Content-Type': 'text/plain' });
		response.end('Sonic 3 AirNet relay. Games connect with a WebSocket (or UDP). Health: /health\n');
	});

	server.on('upgrade', (request, socket) => {
		socket.on('error', () => {});
		const origin = request.headers.origin;
		if (allowedOrigins.length && origin && !allowedOrigins.includes(origin)) { socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n'); return; }
		let ip = cleanIP(socket.remoteAddress);
		if (trustProxy) {
			const forwarded = String(request.headers['x-forwarded-for'] ?? '').split(',')[0].trim();
			if (forwarded) ip = cleanIP(forwarded);
		}
		if ((connectionsPerIP.get(ip) ?? 0) >= maxConnectionsPerIP) { socket.end('HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nContent-Length: 0\r\n\r\n'); return; }
		const connection = acceptUpgrade(request, socket, MAX_DATAGRAM_SIZE);
		if (!connection) return;
		const endpoint = `ws:${nextWsID++}`;
		wsConnections.set(endpoint, connection);
		connectionsPerIP.set(ip, (connectionsPerIP.get(ip) ?? 0) + 1);
		connection.on('message', (data) => core.onDatagram(endpoint, ip, data, nowSeconds()));
		connection.on('close', () => {
			wsConnections.delete(endpoint);
			const left = (connectionsPerIP.get(ip) ?? 1) - 1;
			if (left > 0) connectionsPerIP.set(ip, left); else connectionsPerIP.delete(ip);
			core.endpointClosed(endpoint);
		});
	});

	let tick = null;
	let keepAlive = null;
	let statsTimer = null;

	function start() {
		return new Promise((resolve, reject) => {
			const finish = () => {
				tick = setInterval(() => core.update(nowSeconds()), 1000);
				// a browser tab that vanished without a close message is noticed here
				keepAlive = setInterval(() => {
					const limit = Date.now() - pingSeconds * 3000;
					for (const connection of wsConnections.values()) {
						if (connection.lastActivity < limit) connection.terminate(); else connection.ping();
					}
				}, pingSeconds * 1000);
				if (statsSeconds > 0) {
					statsTimer = setInterval(() => {
						const s = core.stats;
						if (!quiet) console.log(`s3air_relay: lobbies=${s.lobbies} friends=${s.pipes} ws=${wsConnections.size} forwarded=${s.forwarded} dropped(invalid=${s.droppedInvalid} rate=${s.droppedRate} blocked=${s.droppedBlocked}) wrong_keys=${s.wrongKeys}`);
					}, statsSeconds * 1000);
				}
				resolve({ port: server.address().port, udpPort: udp ? udp.address().port : 0 });
			};
			server.once('error', reject);
			server.listen(port, bind, () => {
				server.off('error', reject);
				if (options.udp === false) { finish(); return; }
				udp = dgram.createSocket({ type: 'udp4', recvBufferSize: 4 << 20, sendBufferSize: 4 << 20 });
				udp.on('error', () => {});
				udp.on('message', (data, rinfo) => core.onDatagram(`udp:${rinfo.address}:${rinfo.port}`, rinfo.address, data, nowSeconds()));
				udp.once('error', reject);
				udp.bind(udpPort, bind, () => { udp.off('error', reject); udp.on('error', () => {}); finish(); });
			});
		});
	}

	function stop() {
		clearInterval(tick); clearInterval(keepAlive); clearInterval(statsTimer);
		for (const connection of wsConnections.values()) connection.terminate();
		server.closeAllConnections?.();
		return new Promise((resolve) => {
			try { udp?.close(); } catch { /* already closed */ }
			server.close(() => resolve());
		});
	}

	return { start, stop, core, server, wsConnections };
}

// ---- command line / environment ----
const isMain = process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1];
if (isMain) {
	const env = process.env;
	const args = process.argv.slice(2);
	if (args.includes('--help') || args.includes('-h')) {
		console.log('Usage: node server.mjs   (settings through environment variables, see the top of this file)');
		process.exit(0);
	}
	const port = Number(env.PORT ?? DEFAULT_PORT);
	const udpEnv = env.UDP_PORT === undefined ? port : Number(env.UDP_PORT);
	const relay = createRelayServer({
		port,
		udp: udpEnv !== 0,
		udpPort: udpEnv || port,
		bind: env.BIND ?? '0.0.0.0',
		trustProxy: env.TRUST_PROXY === '1',
		allowedOrigins: (env.ALLOWED_ORIGINS ?? '').split(',').map((x) => x.trim()).filter(Boolean),
		maxConnectionsPerIP: Number(env.MAX_CONNECTIONS_PER_IP ?? 32),
		statsSeconds: Number(env.STATS_SECONDS ?? 60),
		quiet: false,
		limits: env.MAX_LOBBIES ? { maxLobbies: Number(env.MAX_LOBBIES) } : {},
	});
	relay.start().then(({ port: p, udpPort: u }) => {
		console.log(`s3air_relay: WebSocket/HTTP on TCP ${p}${u ? `, UDP on ${u}` : ', UDP off'} (no addresses or player data are logged)`);
	}).catch((error) => { console.error(`s3air_relay: could not start: ${error.message}`); process.exit(1); });
	const shutdown = () => relay.stop().then(() => process.exit(0));
	process.on('SIGINT', shutdown);
	process.on('SIGTERM', shutdown);
}
