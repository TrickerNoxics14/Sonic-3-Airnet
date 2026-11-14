// The logic of the relay, without any sockets (port of oxygenserver/source/relay/RelayCore.h).
// It gets the received datagrams, the sender's endpoint key and the current time, and calls send(endpointKey, buffer).
// An endpoint key is any string that identifies one peer connection: "udp:1.2.3.4:5678" or "ws:17". The sender's IP address
// (used for rate limits and the wrong-key block) is passed separately. Nothing here ever touches a file or the console.
import { randomBytes } from 'node:crypto';
import * as P from './protocol.mjs';

export const DEFAULT_LIMITS = Object.freeze({
	maxLobbies: 512,
	maxPipesPerLobby: 6,              // the game has 3 friends at most; the rest is room for friends that reconnect
	maxLobbiesPerIP: 4,
	hostTimeoutSeconds: 45.0,         // a lobby disappears when its host was silent for this long (the game repeats its registration every 5 seconds)
	pipeTimeoutSeconds: 60.0,         // a friend that sent nothing for this long is forgotten
	controlPerSecond: 8.0,            // registration and join requests per second and sender IP address
	controlBurst: 24.0,
	maxBadKeys: 8,                    // wrong keys from one address before it is ignored ...
	blockSeconds: 60.0,               // ... for this long
	dataPacketsPerSecond: 3000.0,     // per friend and direction
	dataPacketBurst: 6000.0,
	dataBytesPerSecond: 3000000.0,
	dataByteBurst: 6000000.0,
	maxSources: 8192,                 // IP addresses that are remembered for the rate limits
});

class Bucket {
	constructor() { this.tokens = -1; this.last = 0; }
	take(cost, perSecond, burst, now) {
		if (this.tokens < 0) { this.tokens = burst; this.last = now; }
		this.tokens = Math.min(burst, this.tokens + Math.max(0, now - this.last) * perSecond);
		this.last = now;
		if (this.tokens < cost) return false;
		this.tokens -= cost;
		return true;
	}
}

function randomToken() {
	let token = 0;
	while (token === 0) token = randomBytes(4).readUInt32BE(0);   // OS random numbers: a token that cannot be guessed is what stops forged data
	return token;
}

export class RelayCore {
	constructor(send, limits = {}) {
		this.send = send;
		this.limits = { ...DEFAULT_LIMITS, ...limits };
		this.stats = {
			received: 0, forwarded: 0, forwardedBytes: 0, droppedInvalid: 0, droppedRate: 0, droppedBlocked: 0,
			wrongKeys: 0, refused: 0, lobbiesCreated: 0, lobbies: 0, pipes: 0,
		};
		this.lobbies = new Map();          // lobby id (BigInt) -> lobby
		this.hostByEndpoint = new Map();   // endpoint key -> lobby id
		this.pipeByEndpoint = new Map();   // endpoint key -> { lobbyID, pipeID }
		this.sources = new Map();          // ip -> source
		this.lastPrune = 0;
	}

	hasLobby(lobby) { return this.lobbies.has(BigInt.asUintN(64, lobby)); }
	get sourceCount() { return this.sources.size; }

	/** One received datagram. */
	onDatagram(from, ip, data, now) {
		const s = this.stats;
		++s.received;
		const type = (data.length > P.MAX_DATAGRAM_SIZE) ? -1 : P.parseHeader(data);
		if (type < 0) { ++s.droppedInvalid; return; }

		switch (type) {
			case P.Type.HOST_REGISTER:
			case P.Type.CLIENT_JOIN: {
				const wanted = (type === P.Type.HOST_REGISTER) ? P.HOST_HELLO_SIZE : P.HELLO_SIZE;
				if (data.length !== wanted) { ++s.droppedInvalid; return; }
				const source = this.#getSource(ip, now);
				if (!source) { ++s.droppedRate; return; }
				if (now < source.blockedUntil) { ++s.droppedBlocked; return; }
				if (!source.control.take(1, this.limits.controlPerSecond, this.limits.controlBurst, now)) { ++s.droppedRate; return; }
				const lobby = data.readBigUInt64BE(P.HEADER_SIZE);
				const key = data.readBigUInt64BE(P.HEADER_SIZE + 8);
				const secret = (type === P.Type.HOST_REGISTER) ? data.readBigUInt64BE(P.HELLO_SIZE) : 0n;
				if (lobby === 0n || (type === P.Type.HOST_REGISTER && secret === 0n)) { ++s.droppedInvalid; return; }
				if (type === P.Type.HOST_REGISTER) this.#hostRegister(from, ip, source, lobby, key, secret, now);
				else this.#clientJoin(from, source, lobby, key, now);
				break;
			}
			case P.Type.DATA_C2R: this.#clientData(from, data, now); break;
			case P.Type.DATA_H2R: this.#hostData(from, data, now); break;
			default: ++s.droppedInvalid; break;
		}
	}

	/**
	 * The connection of an endpoint ended (a WebSocket closed). A friend's place is freed at once; a host's lobby stays
	 * until its timeout, so that the same game can reconnect and register again with its secret.
	 */
	endpointClosed(endpoint) {
		const pipe = this.pipeByEndpoint.get(endpoint);
		if (pipe) {
			const lobby = this.lobbies.get(pipe.lobbyID);
			if (lobby) lobby.pipes = lobby.pipes.filter((p) => p.id !== pipe.pipeID);
			this.pipeByEndpoint.delete(endpoint);
		}
		this.hostByEndpoint.delete(endpoint);
		this.#refreshCounts();
	}

	/** Call about once per second: forgets lobbies, friends and addresses that were silent for too long. */
	update(now) {
		for (const [id, lobby] of [...this.lobbies]) {
			if (now - lobby.hostSeen > this.limits.hostTimeoutSeconds) { this.#removeLobby(id); continue; }
			lobby.pipes = lobby.pipes.filter((p) => {
				if (now - p.lastSeen > this.limits.pipeTimeoutSeconds) { this.pipeByEndpoint.delete(p.endpoint); return false; }
				return true;
			});
		}
		this.#pruneSources(now, 120);
		this.#refreshCounts();
	}

	// ---- internals ----

	#getSource(ip, now) {
		let source = this.sources.get(ip);
		if (!source) {
			if (this.sources.size >= this.limits.maxSources && now - this.lastPrune >= 1.0) {
				this.#pruneSources(now, 120);
				if (this.sources.size >= this.limits.maxSources) this.#pruneSources(now, 10);
			}
			if (this.sources.size >= this.limits.maxSources) return null;   // memory stays bounded
			source = { control: new Bucket(), badKeys: 0, blockedUntil: 0, lastSeen: 0 };
			this.sources.set(ip, source);
		}
		source.lastSeen = now;
		return source;
	}

	#pruneSources(now, quietSeconds) {
		this.lastPrune = now;
		for (const [ip, source] of this.sources)
			if (now - source.lastSeen > quietSeconds && now >= source.blockedUntil) this.sources.delete(ip);
	}

	#refreshCounts() {
		this.stats.lobbies = this.lobbies.size;
		let pipes = 0;
		for (const lobby of this.lobbies.values()) pipes += lobby.pipes.length;
		this.stats.pipes = pipes;
	}

	#badKey(source, now) {
		++this.stats.wrongKeys;
		if (++source.badKeys >= this.limits.maxBadKeys) { source.badKeys = 0; source.blockedUntil = now + this.limits.blockSeconds; }
	}

	#removeLobby(id) {
		const lobby = this.lobbies.get(id);
		if (!lobby) return;
		for (const pipe of lobby.pipes) this.pipeByEndpoint.delete(pipe.endpoint);
		if (lobby.host !== null && this.hostByEndpoint.get(lobby.host) === id) this.hostByEndpoint.delete(lobby.host);
		this.lobbies.delete(id);
	}

	#hostRegister(from, ip, source, lobbyID, key, secret, now) {
		let lobby = this.lobbies.get(lobbyID);
		if (!lobby) {
			let fromSameIP = 0;
			for (const l of this.lobbies.values()) fromSameIP += (l.hostIP === ip) ? 1 : 0;
			if (this.lobbies.size >= this.limits.maxLobbies || fromSameIP >= this.limits.maxLobbiesPerIP) { ++this.stats.refused; return; }
			lobby = { key, hostSecret: secret, host: null, hostIP: ip, hostToken: randomToken(), hostSeen: now, nextID: 1, pipes: [] };
			this.lobbies.set(lobbyID, lobby);
			++this.stats.lobbiesCreated;
		} else if (lobby.key !== key || lobby.hostSecret !== secret) {
			// Wrong key, or the right key (every friend knows it) without the secret of the game that registered the lobby:
			// not a word back, so that nobody learns anything.
			this.#badKey(source, now);
			return;
		}

		// The same endpoint was host of another lobby: that one is over
		const previous = this.hostByEndpoint.get(from);
		if (previous !== undefined && previous !== lobbyID) this.#removeLobby(previous);
		lobby = this.lobbies.get(lobbyID);
		if (lobby.host !== from) {
			if (lobby.host !== null && this.hostByEndpoint.get(lobby.host) === lobbyID) this.hostByEndpoint.delete(lobby.host);
			lobby.host = from;
		}
		lobby.hostIP = ip;
		lobby.hostSeen = now;
		this.hostByEndpoint.set(from, lobbyID);
		this.send(from, P.buildHostRegistered(lobbyID, lobby.hostToken));
		this.#refreshCounts();
	}

	#refuse(to, reason) {
		++this.stats.refused;
		this.send(to, P.buildClientRefused(reason));
	}

	#clientJoin(from, source, lobbyID, key, now) {
		const lobby = this.lobbies.get(lobbyID);
		if (!lobby) { this.#refuse(from, P.Refusal.NO_HOST); return; }
		if (lobby.key !== key) { this.#badKey(source, now); return; }
		if (now - lobby.hostSeen > this.limits.hostTimeoutSeconds) { this.#refuse(from, P.Refusal.NO_HOST); return; }

		let pipe = null;
		const existing = this.pipeByEndpoint.get(from);
		if (existing) {
			if (existing.lobbyID === lobbyID) {
				pipe = lobby.pipes.find((p) => p.id === existing.pipeID) ?? null;
			} else {
				// this friend moved on to another lobby
				const old = this.lobbies.get(existing.lobbyID);
				if (old) old.pipes = old.pipes.filter((p) => p.id !== existing.pipeID);
				this.pipeByEndpoint.delete(from);
			}
		}
		if (!pipe) {
			if (lobby.pipes.length >= this.limits.maxPipesPerLobby) { this.#refuse(from, P.Refusal.FULL); return; }
			let id = 0;
			for (let tries = 0; tries < 70000; ++tries) {   // a free friend number (never 0: that is the host)
				if (lobby.nextID === 0 || lobby.nextID > 0xffff) lobby.nextID = 1;
				const candidate = lobby.nextID++;
				if (!lobby.pipes.some((p) => p.id === candidate)) { id = candidate; break; }
			}
			pipe = {
				endpoint: from, token: randomToken(), id, lastSeen: now,
				up: { packets: new Bucket(), bytes: new Bucket() }, down: { packets: new Bucket(), bytes: new Bucket() },
			};
			lobby.pipes.push(pipe);
			this.pipeByEndpoint.set(from, { lobbyID, pipeID: id });
		}
		pipe.lastSeen = now;
		this.send(from, P.buildClientJoined(pipe.token));
		this.#refreshCounts();
	}

	#clientData(from, data, now) {
		const s = this.stats;
		if (data.length < P.HEADER_SIZE + 4) { ++s.droppedInvalid; return; }
		const entry = this.pipeByEndpoint.get(from);
		if (!entry) { ++s.droppedInvalid; return; }
		const lobby = this.lobbies.get(entry.lobbyID);
		const pipe = lobby?.pipes.find((p) => p.id === entry.pipeID);
		if (!pipe || pipe.token !== data.readUInt32BE(P.HEADER_SIZE)) { ++s.droppedInvalid; return; }
		if (now - lobby.hostSeen > this.limits.hostTimeoutSeconds || lobby.host === null) { ++s.droppedInvalid; return; }
		const payload = data.subarray(P.HEADER_SIZE + 4);
		const L = this.limits;
		if (!pipe.up.packets.take(1, L.dataPacketsPerSecond, L.dataPacketBurst, now) || !pipe.up.bytes.take(payload.length, L.dataBytesPerSecond, L.dataByteBurst, now)) { ++s.droppedRate; return; }
		pipe.lastSeen = now;
		++s.forwarded;
		s.forwardedBytes += payload.length;
		this.send(lobby.host, P.buildHostData(P.Type.DATA_R2H, lobby.hostToken, pipe.id, payload));
	}

	#hostData(from, data, now) {
		const s = this.stats;
		if (data.length < P.HEADER_SIZE + 6) { ++s.droppedInvalid; return; }
		const id = this.hostByEndpoint.get(from);
		const lobby = (id === undefined) ? undefined : this.lobbies.get(id);
		if (!lobby || lobby.hostToken !== data.readUInt32BE(P.HEADER_SIZE)) { ++s.droppedInvalid; return; }
		lobby.hostSeen = now;   // a host that keeps sending data is alive, with or without registration messages
		const pipe = lobby.pipes.find((p) => p.id === data.readUInt16BE(P.HEADER_SIZE + 4));
		if (!pipe) { ++s.droppedInvalid; return; }
		const payload = data.subarray(P.HEADER_SIZE + 6);
		const L = this.limits;
		if (!pipe.down.packets.take(1, L.dataPacketsPerSecond, L.dataPacketBurst, now) || !pipe.down.bytes.take(payload.length, L.dataBytesPerSecond, L.dataByteBurst, now)) { ++s.droppedRate; return; }
		++s.forwarded;
		s.forwardedBytes += payload.length;
		this.send(pipe.endpoint, P.buildClientData(P.Type.DATA_R2C, pipe.token, payload));
	}
}
