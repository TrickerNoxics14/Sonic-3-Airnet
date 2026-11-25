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
//   TRUST_PROXY=0         number of proxies / platform routers in front of the server that each add the address they saw to X-Forwarded-For
//                         (0 = none: the header is ignored; 1 = one proxy: the LAST entry is the player; 2 = the second from the end, ...).
//                         The entries a proxy of yours wrote are at the end; the ones before them came from the player and can be faked.
//                         A header with FEWER entries than this counts as the address of the connection itself (players behind it are pooled, never faked).
//                         A number HIGHER than the real number of proxies is the one unsafe value: the entry then counted was written by the player.
//                         node proxy_check.mjs wss://your-relay finds that; if it says PROBLEM, lower the number.
//   ALLOWED_ORIGINS=      comma separated list of web page origins that may connect, e.g. https://name.github.io (empty = every page);
//                         scheme and host (and port), nothing else; capital letters, a trailing slash or a page path are ignored; native games send no Origin.
//                         An Origin header that is there but empty or not an origin is refused while a list is set. An entry that no browser could ever send
//                         (a * wildcard, no scheme, a user name, a query, a bad host or port) stops the program.
//   MAX_LOBBIES=512       lobbies at the same time
//   MAX_LOBBIES_PER_IP=4  relay lobbies per player address; MAX_LISTINGS_PER_IP=2  public list entries per address (raise both when many people share one address, or for tests)
//   DIRECTORY=0           turn the public lobby directory off (the relay then works as before 1.3)
//   MAX_CONNECTIONS_PER_IP=32   WebSocket connections per player address
//   STATS_SECONDS=60      print a counter line this often (0 = never). Only counters are printed, never addresses, codes or payloads.
import http from 'node:http';
import dgram from 'node:dgram';
import net from 'node:net';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { RelayCore } from './relay_core.mjs';
import { acceptUpgrade } from './ws.mjs';
import { DEFAULT_PORT, MAX_DATAGRAM_SIZE } from './protocol.mjs';

const nowSeconds = () => performance.now() / 1000;
const cleanIP = (ip) => (ip ?? '').replace(/^::ffff:/, '');

/** "https://Name.github.io/page/" -> "https://name.github.io" (what a browser sends as Origin); null if it is no web page origin. */
export function normalizeOrigin(text) {
	try {
		const url = new URL(String(text).trim());
		return (url.protocol === 'https:' || url.protocol === 'http:') && url.hostname ? url.origin : null;
	} catch { return null; }
}

/**
 * An ALLOWED_ORIGINS entry: scheme, host and perhaps a port (https://name.github.io, http://localhost:8080). Returns the origin the way a browser sends it
 * (lower case, no default port, no path), or null if no browser could ever send it: a wildcard, no scheme, a user name, a query, a bad host or port.
 * A trailing slash or a page path is accepted and dropped (it is what people copy from the address bar, and the origin it stands for is clear).
 */
export function parseConfiguredOrigin(entry) {
	const match = /^(https?):\/\/([^/\s\\@*?#]+)(\/[^\s\\@*?#]*)?$/i.exec(String(entry).trim());
	if (!match) return null;
	let url;
	try { url = new URL(`${match[1]}://${match[2]}`); } catch { return null; }
	const host = url.hostname;
	const plainName = /^[a-z0-9_-]+(\.[a-z0-9_-]+)*$/i.test(host);
	const ipv6 = host.startsWith('[') && host.endsWith(']') && net.isIPv6(host.slice(1, -1));
	return (plainName || ipv6) ? url.origin : null;
}

/**
 * The address of a player, which the rate limits and blocks are kept by. `trustedProxies` is how many proxies of the host's own sit in front
 * of the program; each one appends the address it saw to X-Forwarded-For, so the entries they wrote are the LAST ones and the entry that many places
 * from the end is the player. Whatever stands before it came from the player (or from something before the player) and may be invented, so it is never used
 * as long as the number is right. With 0 the header is ignored. The address of the connection itself (the platform's edge: everybody behind it is pooled,
 * which /health shows as websocketAddresses, but nobody can invent an address) is used when the header has fewer entries than there are proxies, or
 * when the entry is no IP address (invented text, an address with a port).
 * A number that is HIGHER than the real number of proxies cannot be made safe here: the entry that is counted is then one the player wrote (a player who
 * knows the number just adds entries in front). proxy_check.mjs finds that case.
 */
export function playerAddress(socketAddress, forwardedFor, trustedProxies) {
	const direct = cleanIP(socketAddress);
	if (!(trustedProxies > 0) || !forwardedFor) return direct;
	const entries = String(forwardedFor).split(',').map((entry) => cleanIP(entry.trim()));
	if (entries.length < trustedProxies) return direct;
	const chosen = entries[entries.length - trustedProxies];
	return net.isIP(chosen) ? chosen : direct;
}

/** One line for the start of the program's output: what TRUST_PROXY means right now (a changed meaning must be visible in the log of the host). */
export function describeTrustProxy(trustedProxies) {
	const tail = 'a header with fewer entries counts as the address of the connection';
	if (!(trustedProxies > 0)) return 'TRUST_PROXY=0: no proxy trusted, X-Forwarded-For is ignored (every player counts as the address of the connection)';
	if (trustedProxies === 1) return `TRUST_PROXY=1: one proxy trusted, the last X-Forwarded-For entry is the player's address (${tail})`;
	return `TRUST_PROXY=${trustedProxies}: ${trustedProxies} proxies trusted, entry number ${trustedProxies} counted from the end of X-Forwarded-For is the player's address (${tail})`;
}

export function createRelayServer(options = {}) {
	const {
		port = DEFAULT_PORT, udpPort = port, bind = '0.0.0.0', trustProxy = 0, allowedOrigins = [],
		maxConnectionsPerIP = 32, pingSeconds = 20, statsSeconds = 0, quiet = true, limits = {},
	} = options;
	const trustedProxies = (trustProxy === true) ? 1 : (Number(trustProxy) || 0);   // true (older callers) = one proxy
	// A page's origin is compared in the form a browser sends it. An entry that no browser could send throws: it could never match, and an
	// empty list would let every page in, so it must not be dropped silently.
	const origins = allowedOrigins.map((entry) => {
		const origin = parseConfiguredOrigin(entry);
		if (!origin) throw new Error(`ALLOWED_ORIGINS entry "${entry}" is not a web page origin: write only the scheme and host (and port), like https://name.github.io`);
		return origin;
	});

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
			response.end(JSON.stringify({ ok: true, protocol: 2, lobbies: s.lobbies, listed: core.directory.listedCount, friends: s.pipes, websocketClients: wsConnections.size, websocketAddresses: connectionsPerIP.size, forwarded: s.forwarded, signals: s.signals }));
			return;
		}
		response.writeHead(200, { 'Content-Type': 'text/plain' });
		response.end('Sonic 3 AirNet relay. Games connect with a WebSocket (or UDP). Health: /health\n');
	});

	server.on('upgrade', (request, socket) => {
		socket.on('error', () => {});
		const origin = request.headers.origin;   // none from a native game (it is no web page); a script can send any text, so this keeps other web pages out, it is not a login
		if (origins.length && origin !== undefined && !origins.includes(normalizeOrigin(origin))) { socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n'); return; }
		const ip = playerAddress(socket.remoteAddress, request.headers['x-forwarded-for'], trustedProxies);
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
	const trustProxy = String(env.TRUST_PROXY ?? '').trim() || '0';
	if (!/^\d{1,2}$/.test(trustProxy) || Number(trustProxy) > 10) {
		console.error(`s3air_relay: could not start: TRUST_PROXY must be the number of proxies in front of the program (0 to 10), not "${trustProxy}"`);
		process.exit(1);
	}
	let relay;
	try {
		relay = createRelayServer({
			port,
			udp: udpEnv !== 0,
			udpPort: udpEnv || port,
			bind: env.BIND ?? '0.0.0.0',
			trustProxy: Number(trustProxy),
			allowedOrigins: (env.ALLOWED_ORIGINS ?? '').split(',').map((x) => x.trim()).filter(Boolean),
			maxConnectionsPerIP: Number(env.MAX_CONNECTIONS_PER_IP ?? 32),
			statsSeconds: Number(env.STATS_SECONDS ?? 60),
			quiet: false,
			limits: {
				...(env.MAX_LOBBIES ? { maxLobbies: Number(env.MAX_LOBBIES) } : {}),
				...(env.MAX_LOBBIES_PER_IP ? { maxLobbiesPerIP: Number(env.MAX_LOBBIES_PER_IP) } : {}),
				...(env.MAX_LISTINGS_PER_IP ? { maxListingsPerIP: Number(env.MAX_LISTINGS_PER_IP) } : {}),
				...(env.DIRECTORY === '0' ? { directoryEnabled: false } : {}),
			},
		});
	} catch (error) { console.error(`s3air_relay: could not start: ${error.message}`); process.exit(1); }
	console.log(`s3air_relay: ${describeTrustProxy(Number(trustProxy))}`);
	relay.start().then(({ port: p, udpPort: u }) => {
		console.log(`s3air_relay: WebSocket/HTTP on TCP ${p}${u ? `, UDP on ${u}` : ', UDP off'} (no addresses or player data are logged)`);
	}).catch((error) => { console.error(`s3air_relay: could not start: ${error.message}`); process.exit(1); });
	const shutdown = () => relay.stop().then(() => process.exit(0));
	process.on('SIGINT', shutdown);
	process.on('SIGTERM', shutdown);
}
