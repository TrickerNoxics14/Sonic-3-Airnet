// Wire format of the Sonic 3 AirNet relay. Identical to oxygen_netcore/network/RelayProtocol.h (version 2):
// the same datagram is one UDP packet for native games and one binary WebSocket message for browser games.
// All numbers are big endian.

export const MAGIC = Buffer.from('S3RL', 'latin1');
export const VERSION = 2;
export const DEFAULT_PORT = 28842;
export const HEADER_SIZE = 6;                     // magic, version, type
export const HELLO_SIZE = HEADER_SIZE + 16;       // lobby (8), key (8)
export const HOST_HELLO_SIZE = HELLO_SIZE + 8;    // + host secret (8)
export const MAX_DATAGRAM_SIZE = 4096;
export const MAX_PAYLOAD_SIZE = MAX_DATAGRAM_SIZE - 16;

export const Type = Object.freeze({
	HOST_REGISTER: 1,   // host -> relay: lobby, key, host secret
	HOST_REGISTERED: 2, // relay -> host: lobby, host token
	CLIENT_JOIN: 3,     // friend -> relay: lobby, key
	CLIENT_JOINED: 4,   // relay -> friend: token
	CLIENT_REFUSED: 5,  // relay -> friend: reason
	DATA_C2R: 6,        // friend -> relay: token, payload
	DATA_R2C: 7,        // relay -> friend: token, payload
	DATA_H2R: 8,        // host -> relay: host token, friend id, payload
	DATA_R2H: 9,        // relay -> host: host token, friend id, payload
	// Direct-connection signalling (WebRTC offer / answer / ICE candidates): same layout as the data messages, but counted and rate limited
	// separately and never mixed into the game's traffic. Relays and games that do not know these types drop them as unknown ones.
	SIGNAL_C2R: 10,     // friend -> relay: token, payload
	SIGNAL_R2C: 11,     // relay -> friend: token, payload
	SIGNAL_H2R: 12,     // host -> relay: host token, friend id, payload
	SIGNAL_R2H: 13,     // relay -> host: host token, friend id, payload
});
export const MAX_SIGNAL_PAYLOAD_SIZE = 3000;

export const Refusal = Object.freeze({ NONE: 0, NO_HOST: 1, FULL: 2 });

/** Returns the type byte, or -1 if the datagram does not start like a relay datagram. */
export function parseHeader(buf) {
	if (buf.length < HEADER_SIZE || buf.compare(MAGIC, 0, 4, 0, 4) !== 0 || buf[4] !== VERSION)
		return -1;
	return buf[5];
}

function header(type, extra) {
	const out = Buffer.alloc(HEADER_SIZE + extra);
	MAGIC.copy(out, 0);
	out[4] = VERSION;
	out[5] = type;
	return out;
}

export function buildHello(type, lobby, key) {
	const out = header(type, 16);
	out.writeBigUInt64BE(BigInt.asUintN(64, lobby), HEADER_SIZE);
	out.writeBigUInt64BE(BigInt.asUintN(64, key), HEADER_SIZE + 8);
	return out;
}

export function buildHostHello(lobby, key, hostSecret) {
	const out = header(Type.HOST_REGISTER, 24);
	out.writeBigUInt64BE(BigInt.asUintN(64, lobby), HEADER_SIZE);
	out.writeBigUInt64BE(BigInt.asUintN(64, key), HEADER_SIZE + 8);
	out.writeBigUInt64BE(BigInt.asUintN(64, hostSecret), HEADER_SIZE + 16);
	return out;
}

export function buildHostRegistered(lobby, hostToken) {
	const out = header(Type.HOST_REGISTERED, 12);
	out.writeBigUInt64BE(lobby, HEADER_SIZE);
	out.writeUInt32BE(hostToken, HEADER_SIZE + 8);
	return out;
}

export function buildClientJoined(token) {
	const out = header(Type.CLIENT_JOINED, 4);
	out.writeUInt32BE(token, HEADER_SIZE);
	return out;
}

export function buildClientRefused(reason) {
	const out = header(Type.CLIENT_REFUSED, 1);
	out[HEADER_SIZE] = reason;
	return out;
}

/** Friend data: DATA_C2R or DATA_R2C (token, payload). */
export function buildClientData(type, token, payload) {
	const out = header(type, 4 + payload.length);
	out.writeUInt32BE(token, HEADER_SIZE);
	payload.copy(out, HEADER_SIZE + 4);
	return out;
}

/** Host data: DATA_H2R or DATA_R2H (host token, friend id, payload). */
export function buildHostData(type, hostToken, friendID, payload) {
	const out = header(type, 6 + payload.length);
	out.writeUInt32BE(hostToken, HEADER_SIZE);
	out.writeUInt16BE(friendID, HEADER_SIZE + 4);
	payload.copy(out, HEADER_SIZE + 6);
	return out;
}

// ---- Public lobby directory (added in 1.3, directory protocol version 1) ----
// The directory uses the same transport and the same S3RL header (VERSION stays 2, so old games and old relays are not affected:
// an old relay drops these types as unknown, an old game never sends them). Every directory message carries its own version byte
// as the first payload byte. Strings are one length byte plus ASCII bytes. Numbers are big endian.
// Identical to oxygen_netcore/network/DirectoryProtocol.h.
export const DIR_VERSION = 1;                     // highest directory protocol version this server speaks

export const DirType = Object.freeze({
	HELLO: 16,           // client -> server: [ver] + padding (the reply must not be bigger than the request)
	HELLO_REPLY: 17,     // server -> client: [ver][flags][codeDigits][pageSize][lobbies u16][cookie u64]
	PUBLISH: 18,         // host -> server: [ver][lobby u64][key u64][secret u64][sessionID u64][flags][players][max][compat s][name s][host s]
	PUBLISH_REPLY: 19,   // server -> host: [ver][status][code s]
	UNPUBLISH: 20,       // host -> server: [ver][lobby u64][key u64][secret u64]
	LIST: 21,            // client -> server: [ver][cookie u64][flags][offset][compat s]
	LIST_REPLY: 22,      // server -> client: [ver][status][total u16][offset][count] + entries
	RESOLVE: 23,         // client -> server: [ver][cookie u64][code s]
	RESOLVE_REPLY: 24,   // server -> client: [ver][status] (+ [sessionID u64][players][max][flags] when OK, + [retryAfter u16] when THROTTLED)
});

export const PublishFlags = Object.freeze({ PUBLIC: 1, IN_GAME: 2 });
export const ListFlags = Object.freeze({ COMPATIBLE_ONLY: 1 });
export const EntryFlags = Object.freeze({ IN_GAME: 1, COMPATIBLE: 2, FULL: 4 });
export const PublishStatus = Object.freeze({ OK: 0, NOT_REGISTERED: 1, LIMIT: 2, BAD_REQUEST: 3, CLOSED: 4 });
export const ListStatus = Object.freeze({ OK: 0, BAD_COOKIE: 1 });
export const ResolveStatus = Object.freeze({ OK: 0, NOT_FOUND: 1, BAD_COOKIE: 2, THROTTLED: 3 });
export const HelloFlags = Object.freeze({ DIRECTORY: 1 });
export const HELLO_MIN_PAYLOAD = 16;

export function isDirectoryType(type) { return type >= DirType.HELLO && type <= DirType.RESOLVE_REPLY; }

export function dirMessage(type, payload) {
	const out = Buffer.alloc(HEADER_SIZE + payload.length);
	MAGIC.copy(out, 0);
	out[4] = VERSION;
	out[5] = type;
	payload.copy(out, HEADER_SIZE);
	return out;
}

/** Small writer for directory payloads. */
export class Writer {
	constructor() { this.parts = []; }
	u8(v) { this.parts.push(Buffer.from([v & 0xff])); return this; }
	u16(v) { const b = Buffer.alloc(2); b.writeUInt16BE(v); this.parts.push(b); return this; }
	u64(v) { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt.asUintN(64, BigInt(v))); this.parts.push(b); return this; }
	str(s) { const b = Buffer.from(String(s), 'latin1').subarray(0, 255); this.parts.push(Buffer.from([b.length]), b); return this; }
	pad(n) { this.parts.push(Buffer.alloc(n)); return this; }
	done() { return Buffer.concat(this.parts); }
}

/** Small reader; every read past the end sets `bad` instead of throwing. */
export class Reader {
	constructor(buf, offset = HEADER_SIZE) { this.buf = buf; this.pos = offset; this.bad = false; }
	#need(n) { if (this.pos + n > this.buf.length) { this.bad = true; return false; } return true; }
	u8() { if (!this.#need(1)) return 0; return this.buf[this.pos++]; }
	u16() { if (!this.#need(2)) return 0; const v = this.buf.readUInt16BE(this.pos); this.pos += 2; return v; }
	u64() { if (!this.#need(8)) return 0n; const v = this.buf.readBigUInt64BE(this.pos); this.pos += 8; return v; }
	str() { const n = this.u8(); if (!this.#need(n)) return ''; const s = this.buf.toString('latin1', this.pos, this.pos + n); this.pos += n; return s; }
}
