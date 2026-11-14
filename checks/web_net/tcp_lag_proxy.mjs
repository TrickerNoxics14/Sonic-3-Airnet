// A TCP proxy on 127.0.0.1 that makes the connection behave like a bad internet line, to test the browser's WebSocket path (which is TCP):
//   - every chunk is delayed by `baseMs` plus a random 0..`jitterMs`, but always delivered IN ORDER (TCP never reorders),
//   - every `stallEveryMs` the whole line freezes for `stallMs` (a lost packet that has to be sent again holds everything behind it back), then the held data arrives in a burst.
// Used between the browsers and the relay. Both directions get the same treatment, so the round trip is about twice the one-way delay.
import net from 'node:net';

export function startLagProxy({ targetPort, baseMs = 40, jitterMs = 30, stallEveryMs = 0, stallMs = 0 }) {
	const profile = { baseMs, jitterMs, stallEveryMs, stallMs };
	const started = Date.now();
	const sockets = new Set();
	const server = net.createServer((client) => {
		const upstream = net.connect(targetPort, '127.0.0.1');
		sockets.add(client); sockets.add(upstream);
		for (const socket of [client, upstream]) { socket.setNoDelay(true); socket.on('error', () => {}); }
		const pipe = (from, to) => {
			let lastDelivery = 0;
			from.on('data', (chunk) => {
				const now = Date.now();
				let due = now + profile.baseMs + Math.random() * profile.jitterMs;
				if (profile.stallEveryMs > 0 && profile.stallMs > 0) {
					const phase = (now - started) % profile.stallEveryMs;
					if (phase < profile.stallMs) due = Math.max(due, now + (profile.stallMs - phase));   // inside a stall: everything waits for its end
				}
				due = Math.max(due, lastDelivery);   // in order
				lastDelivery = due;
				setTimeout(() => { if (!to.destroyed) to.write(chunk); }, Math.max(0, due - now));
			});
			from.on('close', () => setTimeout(() => to.destroy(), Math.max(0, lastDelivery - Date.now()) + 5));
		};
		pipe(client, upstream);
		pipe(upstream, client);
	});
	return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({
		port: server.address().port,
		profile,
		close: () => new Promise((done) => { for (const s of sockets) s.destroy(); server.close(done); }),
	})));
}
