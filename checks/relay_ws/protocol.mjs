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
});

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
