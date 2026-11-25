#!/usr/bin/env node
// Is TRUST_PROXY right for this host?  Run it from your own PC against the relay you deployed (Node 22 or newer):
//     node proxy_check.mjs wss://your-relay.example.org
// It opens three WebSocket connections that each claim another player address in X-Forwarded-For (198.51.100.x, a range reserved for examples),
// then reads /health. With the right setting the relay ignores what a player claims and sees one address for all three. It sends no game data,
// joins no lobby and closes the connections at the end. (It sends no Origin, like a native game, so a list of allowed pages does not stop it.)
//
//   OK       the three claims made no new address: nobody can fake an address (also true when TRUST_PROXY=0)
//   PROBLEM  every claim became its own address: the relay believes whatever a player writes. Raise TRUST_PROXY by one and run this again
//            (or set it to 0 when no proxy of the host adds its own address to the header).
// It cannot tell whether TRUST_PROXY is too LOW (then every player shows up with the platform's own address): look at /health while two friends on
// different networks are connected: websocketClients 2 should come with websocketAddresses 2.
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
	const socket = new WebSocket(target, { headers: { 'X-Forwarded-For': `198.51.100.${i}` } });
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
if (newAddresses >= 2) { console.log('PROBLEM: the relay takes the address a player claims in X-Forwarded-For. Raise TRUST_PROXY by one (see HOSTING.md) and run this again.'); process.exit(1); }
console.log('OK: claimed addresses made no new player address.');
process.exit(0);
