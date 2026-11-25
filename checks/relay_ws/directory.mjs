// The public lobby directory of the relay server (directory protocol version 1, see protocol.mjs).
// A host that is registered at the relay publishes its lobby here (name, host name, players, status). The directory hands out a short numeric
// lobby code. Players ask for the list of public lobbies, or resolve a code (public or private lobby) to what they need to join through the relay.
// Without sockets, like relay_core.mjs: the core passes datagrams in and gets replies through send().
//
// Security notes (the long version is in the handoff / README):
//  - A listing can only be made, changed or removed by the game that holds the relay lobby: the publish message must carry the relay lobby id, key and
//    host secret and the relay must have exactly that lobby registered right now. Anything else gets no answer (or only "not registered").
//  - Codes are random (crypto), 6 digits, never reused for a while after a lobby closed. A wrong code costs the asking address (and its subnet) a "strike";
//    5 strikes in a minute block the address for 5 minutes, so guessing a code is limited to about one try per minute and address.
//  - Not found, expired, closed and never existed look the same. Nothing in an answer contains an address of a host.
//  - List and resolve need a cookie from a hello (bound to the sender's address): a forged source address cannot get a big reply (no amplification),
//    and every reply to a request without a valid cookie is smaller than the request.
import { createHmac, randomBytes, randomInt } from 'node:crypto';
import * as P from './protocol.mjs';

export const DIRECTORY_LIMITS = Object.freeze({
	directoryEnabled: true,
	codeDigits: 6,
	maxListings: 256,
	maxListingsPerIP: 2,
	listingTimeoutSeconds: 20.0,      // a listing disappears when its host did not publish for this long (the game publishes every 5 seconds)
	codeCooldownSeconds: 300.0,       // a released code is not given out again for this long
	pageSize: 16,
	cookieWindowSeconds: 120.0,       // a cookie is valid for the window it was issued in and the next one (2 to 4 minutes)
	dirPerSecond: 4.0,                // hello, list and resolve requests per second and address
	dirBurst: 12.0,
	maxResolveStrikes: 5,             // wrong codes from one address within 60 seconds ...
	resolveBlockSeconds: 300.0,       // ... block resolving for this long
	maxSubnetStrikes: 25,             // the same for a whole /24 (IPv4) or /64 (IPv6)
	maxLobbyNameLength: 20,
	maxHostNameLength: 12,
});

const ALLOWED = /[^A-Za-z0-9 _\-.!?'#+&()]/g;
const LEET = { '0': 'o', '1': 'i', '3': 'e', '4': 'a', '5': 's', '7': 't', '@': 'a', '$': 's' };
// A short, light list: whole words, and a few strong ones that are also caught inside a longer word
const BANNED_WORDS = new Set(['fuck', 'fucker', 'fucking', 'shit', 'cunt', 'bitch', 'whore', 'slut', 'rape', 'rapist', 'nazi', 'hitler', 'porn', 'pussy', 'dick', 'cock', 'kike', 'retard', 'fag', 'faggot', 'nigger', 'nigga']);
const BANNED_INSIDE = ['fuck', 'nigg', 'fagg'];

export function isDisallowedText(text) {
	const folded = String(text).toLowerCase().replace(/[013457@$]/g, (c) => LEET[c] ?? c);
	const words = folded.split(/[^a-z]+/).filter(Boolean);
	if (words.some((w) => BANNED_WORDS.has(w))) return true;
	const squashed = folded.replace(/[^a-z]/g, '');
	return BANNED_INSIDE.some((w) => squashed.includes(w));
}

/** Plain text only: letters, digits, space and a few punctuation marks, one space at a time, trimmed, at most maxLength characters. */
export function sanitizeText(text, maxLength, fallback) {
	let clean = String(text ?? '').replace(ALLOWED, '').replace(/ +/g, ' ').trim().slice(0, maxLength).trim();
	if (!clean || isDisallowedText(clean)) clean = fallback;
	return clean;
}

export function isValidCode(code, digits) {
	return typeof code === 'string' && code.length === digits && /^[1-9][0-9]*$/.test(code);
}

function subnetOf(ip) {
	if (ip.includes(':')) return 'v6:' + ip.split(':').slice(0, 4).join(':');
	const parts = ip.split('.');
	return parts.length === 4 ? 'v4:' + parts.slice(0, 3).join('.') : 'x:' + ip;
}

class Strikes {
	constructor() { this.times = []; this.blockedUntil = 0; }
	add(now, windowSeconds, maxStrikes, blockSeconds) {
		this.times = this.times.filter((t) => now - t < windowSeconds);
		this.times.push(now);
		if (this.times.length >= maxStrikes) { this.times = []; this.blockedUntil = now + blockSeconds; }
	}
}

export class Directory {
	/** core: the RelayCore (for the registered lobbies and the per-address state), send: (endpointKey, buffer) */
	constructor(core, send, limits = {}) {
		this.core = core;
		this.send = send;
		this.limits = { ...DIRECTORY_LIMITS, ...limits };
		this.cookieSecret = randomBytes(32);
		this.entries = new Map();        // relay lobby id -> entry
		this.byCode = new Map();         // code -> entry
		this.cooldown = new Map();       // code -> time it may be used again
		this.strikesByIP = new Map();
		this.strikesBySubnet = new Map();
		this.stats = { published: 0, expired: 0, listRequests: 0, resolved: 0, notFound: 0, throttled: 0, badCookie: 0, refusedLimit: 0 };
	}

	get listedCount() { let n = 0; for (const e of this.entries.values()) n += e.isPublic ? 1 : 0; return n; }
	get count() { return this.entries.size; }

	/** The relay dropped this lobby (host timed out, replaced): its listing goes too. */
	lobbyRemoved(lobbyID, now) { this.#remove(lobbyID, now); }

	update(now) {
		for (const [id, entry] of [...this.entries]) {
			const lobby = this.core.lobbies.get(id);
			if (!lobby || now - entry.lastSeen > this.limits.listingTimeoutSeconds) {
				this.#remove(id, now);
				++this.stats.expired;
			}
		}
		for (const [code, until] of [...this.cooldown]) if (now >= until) this.cooldown.delete(code);
		for (const map of [this.strikesByIP, this.strikesBySubnet])
			for (const [key, s] of [...map]) if (now >= s.blockedUntil && (s.times.length === 0 || now - s.times[s.times.length - 1] > 60)) map.delete(key);
	}

	onMessage(type, from, ip, data, now) {
		const L = this.limits;
		if (!L.directoryEnabled) return;
		const source = this.core.sourceFor(ip, now);
		if (!source) return;
		if (now < source.blockedUntil) return;
		const r = new P.Reader(data);
		const version = r.u8();
		if (version < 1 || version > P.DIR_VERSION) return;
		switch (type) {
			case P.DirType.HELLO: return this.#hello(from, source, version, data, now);
			case P.DirType.PUBLISH: return this.#publish(from, ip, source, r, now);
			case P.DirType.UNPUBLISH: return this.#unpublish(from, ip, source, r, now);
			case P.DirType.LIST: return this.#list(from, ip, source, r, now);
			case P.DirType.RESOLVE: return this.#resolve(from, ip, source, r, now);
			default: return;
		}
	}

	// ---- internals ----

	#cookie(ip, windowIndex) {
		return createHmac('sha256', this.cookieSecret).update(`${ip}|${windowIndex}`).digest().readBigUInt64BE(0);
	}

	#cookieValid(ip, cookie, now) {
		const w = Math.floor(now / this.limits.cookieWindowSeconds);
		return cookie === this.#cookie(ip, w) || cookie === this.#cookie(ip, w - 1);
	}

	#dirAllowed(source, now) {
		if (!source.dir) source.dir = { tokens: -1, last: 0 };
		const bucket = source.dir, L = this.limits;
		if (bucket.tokens < 0) { bucket.tokens = L.dirBurst; bucket.last = now; }
		bucket.tokens = Math.min(L.dirBurst, bucket.tokens + Math.max(0, now - bucket.last) * L.dirPerSecond);
		bucket.last = now;
		if (bucket.tokens < 1) return false;
		bucket.tokens -= 1;
		return true;
	}

	#reply(to, type, writer) { this.send(to, P.dirMessage(type, writer.done())); }

	#hello(from, source, version, data, now) {
		if (data.length < P.HEADER_SIZE + P.HELLO_MIN_PAYLOAD) return;      // too small: the reply would be bigger than the request
		if (!this.#dirAllowed(source, now)) return;
		const ip = source.ip;
		const w = new P.Writer().u8(Math.min(version, P.DIR_VERSION)).u8(P.HelloFlags.DIRECTORY).u8(this.limits.codeDigits).u8(this.limits.pageSize)
			.u16(Math.min(this.listedCount, 0xffff)).u64(this.#cookie(ip, Math.floor(now / this.limits.cookieWindowSeconds)));
		this.#reply(from, P.DirType.HELLO_REPLY, w);
	}

	#owner(ip, source, r, now) {
		const lobbyID = r.u64(), key = r.u64(), secret = r.u64();
		if (r.bad || lobbyID === 0n || secret === 0n) return { error: 'bad' };
		const lobby = this.core.lobbies.get(lobbyID);
		if (!lobby) return { error: 'none', lobbyID };
		if (lobby.key !== key || lobby.hostSecret !== secret) { this.core.noteBadKey(source, now); return { error: 'silent' }; }
		return { lobbyID, lobby };
	}

	#publish(from, ip, source, r, now) {
		const L = this.limits;
		if (!source.control.take(1, this.core.limits.controlPerSecond, this.core.limits.controlBurst, now)) return;
		const owner = this.#owner(ip, source, r, now);
		const status = (s, code = '') => this.#reply(from, P.DirType.PUBLISH_REPLY, new P.Writer().u8(P.DIR_VERSION).u8(s).str(code));
		if (owner.error === 'silent') return;
		if (owner.error === 'bad') return status(P.PublishStatus.BAD_REQUEST);
		if (owner.error === 'none') return status(P.PublishStatus.NOT_REGISTERED);
		const sessionID = r.u64(), flags = r.u8(), players = r.u8(), max = r.u8();
		const compat = r.str(), name = r.str(), host = r.str();
		if (r.bad || max < 1 || max > 16 || players > max || compat.length > 32 || !/^[A-Za-z0-9._\-]*$/.test(compat)) return status(P.PublishStatus.BAD_REQUEST);

		let entry = this.entries.get(owner.lobbyID);
		if (!entry) {
			let fromSameIP = 0;
			for (const e of this.entries.values()) fromSameIP += (e.ip === ip) ? 1 : 0;
			if (this.entries.size >= L.maxListings || fromSameIP >= L.maxListingsPerIP) { ++this.stats.refusedLimit; return status(P.PublishStatus.LIMIT); }
			const code = this.#newCode(now);
			if (!code) { ++this.stats.refusedLimit; return status(P.PublishStatus.LIMIT); }
			entry = { lobbyID: owner.lobbyID, code, ip, publishedAt: now };
			this.entries.set(owner.lobbyID, entry);
			this.byCode.set(code, entry);
			++this.stats.published;
		}
		entry.ip = ip;
		entry.lastSeen = now;
		entry.sessionID = sessionID;
		entry.isPublic = (flags & P.PublishFlags.PUBLIC) !== 0;
		entry.inGame = (flags & P.PublishFlags.IN_GAME) !== 0;
		entry.players = players;
		entry.max = max;
		entry.compat = compat;
		entry.name = sanitizeText(name, L.maxLobbyNameLength, 'Lobby');
		entry.host = sanitizeText(host, L.maxHostNameLength, 'Player');
		status(P.PublishStatus.OK, entry.code);
	}

	#unpublish(from, ip, source, r, now) {
		if (!source.control.take(1, this.core.limits.controlPerSecond, this.core.limits.controlBurst, now)) return;
		const owner = this.#owner(ip, source, r, now);
		if (owner.error) return;
		this.#remove(owner.lobbyID, now);
		this.#reply(from, P.DirType.PUBLISH_REPLY, new P.Writer().u8(P.DIR_VERSION).u8(P.PublishStatus.CLOSED).str(''));
	}

	#list(from, ip, source, r, now) {
		const cookie = r.u64(), flags = r.u8(), offset = r.u8(), compat = r.str();
		if (r.bad || !this.#dirAllowed(source, now)) return;
		if (!this.#cookieValid(ip, cookie, now)) {
			++this.stats.badCookie;
			return this.#reply(from, P.DirType.LIST_REPLY, new P.Writer().u8(P.DIR_VERSION).u8(P.ListStatus.BAD_COOKIE));
		}
		++this.stats.listRequests;
		const compatibleOnly = (flags & P.ListFlags.COMPATIBLE_ONLY) !== 0;
		const rows = [];
		for (const e of this.entries.values()) {
			if (!e.isPublic) continue;
			const compatible = compat !== '' && e.compat === compat;
			if (compatibleOnly && !compatible) continue;
			rows.push({ e, compatible });
		}
		// best first: compatible, still in the lobby, with room, the fullest first (so that games fill up), then the oldest
		rows.sort((a, b) => (b.compatible - a.compatible) || ((a.e.inGame ? 1 : 0) - (b.e.inGame ? 1 : 0)) || ((a.e.players >= a.e.max ? 1 : 0) - (b.e.players >= b.e.max ? 1 : 0))
			|| (b.e.players - a.e.players) || (a.e.publishedAt - b.e.publishedAt));
		const page = rows.slice(offset, offset + this.limits.pageSize);
		const w = new P.Writer().u8(P.DIR_VERSION).u8(P.ListStatus.OK).u16(Math.min(rows.length, 0xffff)).u8(offset).u8(page.length);
		for (const { e, compatible } of page) {
			const f = (e.inGame ? P.EntryFlags.IN_GAME : 0) | (compatible ? P.EntryFlags.COMPATIBLE : 0) | (e.players >= e.max ? P.EntryFlags.FULL : 0);
			w.str(e.code).str(e.name).str(e.host).str(e.compat).u8(e.players).u8(e.max).u8(f).u16(Math.min(Math.floor(now - e.publishedAt), 0xffff));
		}
		this.#reply(from, P.DirType.LIST_REPLY, w);
	}

	#strike(ip, now) {
		const L = this.limits;
		const byIP = this.strikesByIP.get(ip) ?? new Strikes();
		this.strikesByIP.set(ip, byIP);
		byIP.add(now, 60, L.maxResolveStrikes, L.resolveBlockSeconds);
		const net = subnetOf(ip);
		const bySubnet = this.strikesBySubnet.get(net) ?? new Strikes();
		this.strikesBySubnet.set(net, bySubnet);
		bySubnet.add(now, 60, L.maxSubnetStrikes, L.resolveBlockSeconds);
		if (this.strikesByIP.size > 20000) this.strikesByIP.clear();         // memory stays bounded
		if (this.strikesBySubnet.size > 20000) this.strikesBySubnet.clear();
	}

	#resolve(from, ip, source, r, now) {
		const cookie = r.u64(), code = r.str();
		if (r.bad || !this.#dirAllowed(source, now)) return;
		const reply = (status, extra) => this.#reply(from, P.DirType.RESOLVE_REPLY, extra ?? new P.Writer().u8(P.DIR_VERSION).u8(status));
		if (!this.#cookieValid(ip, cookie, now)) { ++this.stats.badCookie; return reply(P.ResolveStatus.BAD_COOKIE); }

		const blockedUntil = Math.max(this.strikesByIP.get(ip)?.blockedUntil ?? 0, this.strikesBySubnet.get(subnetOf(ip))?.blockedUntil ?? 0);
		if (now < blockedUntil) {
			++this.stats.throttled;
			return reply(0, new P.Writer().u8(P.DIR_VERSION).u8(P.ResolveStatus.THROTTLED).u16(Math.min(Math.ceil(blockedUntil - now), 0xffff)));
		}
		const entry = isValidCode(code, this.limits.codeDigits) ? this.byCode.get(code) : undefined;
		if (!entry || now - entry.lastSeen > this.limits.listingTimeoutSeconds) {
			++this.stats.notFound;
			this.#strike(ip, now);
			return reply(P.ResolveStatus.NOT_FOUND);
		}
		++this.stats.resolved;
		const f = (entry.inGame ? P.EntryFlags.IN_GAME : 0) | (entry.players >= entry.max ? P.EntryFlags.FULL : 0) | (entry.isPublic ? P.EntryFlags.LISTED : 0);
		reply(0, new P.Writer().u8(P.DIR_VERSION).u8(P.ResolveStatus.OK).u64(entry.sessionID).u8(entry.players).u8(entry.max).u8(f));
	}

	#newCode(now) {
		const digits = this.limits.codeDigits;
		const lo = 10 ** (digits - 1), hi = 10 ** digits;
		for (let tries = 0; tries < 200; ++tries) {
			const code = String(randomInt(lo, hi));
			if (!this.byCode.has(code) && !(this.cooldown.get(code) > now)) return code;
		}
		return null;
	}

	#remove(lobbyID, now) {
		const entry = this.entries.get(lobbyID);
		if (!entry) return;
		this.entries.delete(lobbyID);
		this.byCode.delete(entry.code);
		this.cooldown.set(entry.code, now + this.limits.codeCooldownSeconds);
	}
}
