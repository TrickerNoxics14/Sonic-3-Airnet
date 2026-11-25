#!/usr/bin/env node
// Is TRUST_PROXY too HIGH for this host?  Run it from your own PC against the relay you deployed (Node 22 or newer):
//     node proxy_check.mjs wss://your-relay.example.org
// It opens three WebSocket connections that each write ten made-up addresses (198.51.100.x, a range reserved for examples) into X-Forwarded-For, then reads /health.
// The relay counts the entry that many places from the end of the header as the player, as many places as TRUST_PROXY says. If TRUST_PROXY is at most the number of
// proxies in front of the relay that add an address, that entry was written by one of them and the three connections are ONE address. If it is higher, the relay counts an
// entry that the player wrote, and the three connections become three addresses: anybody could do that to dodge the connection cap, the wrong-key block and the code guess limit.
// It sends no game data, joins no lobby and closes the connections at the end. (It sends no Origin, like a native game, so a list of allowed pages does not stop it.)
//
//   OK        the made-up addresses made no new address: nobody can invent an address with this setting (also true when TRUST_PROXY=0)
//   PROBLEM   TRUST_PROXY is too HIGH. LOWER it by one (to 0 if nothing in front of the relay adds an address) and run this again
//   INCONCLUSIVE  not all three connections were accepted: try again when nobody is playing
// OK does not say that TRUST_PROXY is high enough. If it is too LOW, every player shows up with the address of the platform (stricter limits, nobody can fake an address):
// look at /health while two friends on different networks are connected: websocketClients 2 should come with websocketAddresses 2. If it shows 1, raise TRUST_PROXY by one
// and run this again; if it then says PROBLEM, go back (the number before is the right one).
const target = process.argv[2];
if (!target || !/^wss?:\/\//.test(target)) { console.error('Usage: node proxy_check.mjs wss://your-relay.example.org   (or ws://127.0.0.1:28842)'); process.exit(2); }
if (typeof WebSocket === 'undefined') { console.error('This needs Node 22 or newer (built-in WebSocket).'); process.exit(2); }
const healthURL = new URL('/health', target.replace(/^ws/, 'http')).href;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const health = async () => (await fetch(healthURL, { signal: AbortSignal.timeout(90000) })).json();   // a sleeping host needs up to a minute to answer the first request

console.log(`Reading ${healthURL} (a host that sleeps may take a minute to wake up) ...`);
const before = await health();
if (before.websocketAddresses === undefined) { console.error('This relay does not report websocketAddresses: it is an older version of the program.'); process.exit(2); }
const sockets = [];
for (let i = 1; i <= 3; ++i) {
	// ten made-up entries, different for each connection: whichever of them the relay counts (TRUST_PROXY is at most 10), it is one of these when TRUST_PROXY is too high
	const claims = Array.from({ length: 10 }, (_, j) => `198.51.100.${(i - 1) * 10 + j + 1}`).join(', ');
	const socket = new WebSocket(target, { headers: { 'X-Forwarded-For': claims } });
	sockets.push(socket);
	await new Promise((resolve) => { socket.onopen = resolve; socket.onerror = resolve; setTimeout(resolve, 15000); });
}
await sleep(1000);
const after = await health();
const opened = sockets.filter((s) => s.readyState === 1).length;
sockets.forEach((s) => { try { s.close(); } catch { /* closed */ } });
const newClients = after.websocketClients - before.websocketClients;
const newAddresses = after.websocketAddresses - before.websocketAddresses;
console.log(`connections opened: ${opened} of 3 (relay counted ${newClients} new), new player addresses: ${newAddresses}`);
if (opened < 3 || newClients < 3) { console.log('INCONCLUSIVE: the relay did not accept all three connections (a limit, an origin list that refuses scripts, or other players connecting at the same time). Try again when nobody is playing.'); process.exit(2); }
if (newAddresses >= 2) {
	console.log('PROBLEM: the relay counted an address that the player wrote itself: TRUST_PROXY is too HIGH for the proxies in front of this relay (or the host adds no address to X-Forwarded-For). Lower TRUST_PROXY by one (to 0 if nothing in front of the relay adds an address) and run this again.');
	console.log('If real players connected during the second this takes, they can look like new addresses: run this again before you change anything.');
	process.exit(1);
}
console.log('OK: made-up addresses made no new player address.');
process.exit(0);
