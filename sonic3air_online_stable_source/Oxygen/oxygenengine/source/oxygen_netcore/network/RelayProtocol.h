/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// The wire format between a game and a relay server (the optional fallback for players whose connection cannot be made directly).
// Standard C++ only: the game, the relay program and the developer tests all include this one file.
//
// How it works: both the host and the friends send UDP datagrams to the relay server's address. The relay forwards the payload of each
// datagram to the other side. The payload is an ordinary datagram of the game's own protocol, which is still checked end to end
// (join code challenge, see NetplayAuth.h). The relay does not read the payload.
//
//   host   -> relay : HOST_REGISTER (lobby, key, host secret), repeated every few seconds; the relay answers HOST_REGISTERED (token)
//   friend -> relay : CLIENT_JOIN (lobby, key);                              the relay answers CLIENT_JOINED (token) or CLIENT_REFUSED (reason)
//   friend -> relay : DATA_C2R (token, payload)            -> relay -> host   : DATA_R2H (host token, friend id, payload)
//   host   -> relay : DATA_H2R (host token, friend id, payload) -> relay -> friend : DATA_R2C (token, payload)
//
// "lobby" and "key" are one way hashes of the session code (see NetplayAuth::relayLobbyFromSession / relayKeyFromSession): the relay never sees
// the code. The first host that registers a lobby sets its key and its host secret (a random number that only this game process knows, NOT derived from the code,
// because every friend knows the code); whoever does not know the key, or tries to take over a lobby without its host secret, gets no answer at all.
// Every answer is smaller than the request that caused it, so the relay cannot be used to amplify traffic.
//
// Inside the game the relayed peers look like ordinary peers with a made-up address ("240.0.x.y", a range that is never routed on the internet):
// the host sees friend number N as 240.0.(N / 256).(N % 256), a friend sees the host as 240.0.0.0.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace RelayProtocol
{
	static const uint8_t MAGIC[4] = { 'S', '3', 'R', 'L' };
	static const uint8_t VERSION = 2;					// 2: HOST_REGISTER carries a host secret (only the registering game knows it)
	static const uint16_t DEFAULT_PORT = 28842;
	static const size_t HEADER_SIZE = 6;				// magic, version, type
	static const size_t HELLO_SIZE = HEADER_SIZE + 16;	// lobby (8 bytes), key (8 bytes)
	static const size_t HOST_HELLO_SIZE = HELLO_SIZE + 8;	// lobby, key, host secret (8 bytes)
	static const size_t MAX_DATAGRAM_SIZE = 4096;		// What the relay accepts; the game's own datagrams are about 1.1 KB at most
	static const size_t MAX_PAYLOAD_SIZE = MAX_DATAGRAM_SIZE - 16;
	static const uint16_t VIRTUAL_PORT = 1;

	enum Type : uint8_t
	{
		HOST_REGISTER	= 1,	// host -> relay: lobby, key, host secret
		HOST_REGISTERED	= 2,	// relay -> host: lobby, host token
		CLIENT_JOIN		= 3,	// friend -> relay: lobby, key
		CLIENT_JOINED	= 4,	// relay -> friend: token
		CLIENT_REFUSED	= 5,	// relay -> friend: reason
		DATA_C2R		= 6,	// friend -> relay: token, payload
		DATA_R2C		= 7,	// relay -> friend: token, payload
		DATA_H2R		= 8,	// host -> relay: host token, friend id, payload
		DATA_R2H		= 9		// relay -> host: host token, friend id, payload
	};

	enum Refusal : uint8_t
	{
		NONE		= 0,
		NO_HOST		= 1,	// No host is registered for this lobby (yet)
		FULL		= 2		// The lobby has no room for another friend
	};

	inline void put16(std::vector<uint8_t>& out, uint16_t v)  { out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v); }
	inline void put32(std::vector<uint8_t>& out, uint32_t v)  { put16(out, (uint16_t)(v >> 16)); put16(out, (uint16_t)v); }
	inline void put64(std::vector<uint8_t>& out, uint64_t v)  { put32(out, (uint32_t)(v >> 32)); put32(out, (uint32_t)v); }
	inline uint16_t get16(const uint8_t* p)  { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
	inline uint32_t get32(const uint8_t* p)  { return ((uint32_t)get16(p) << 16) | get16(p + 2); }
	inline uint64_t get64(const uint8_t* p)  { return ((uint64_t)get32(p) << 32) | get32(p + 4); }

	inline void putHeader(std::vector<uint8_t>& out, Type type)
	{
		out.insert(out.end(), MAGIC, MAGIC + 4);
		out.push_back(VERSION);
		out.push_back((uint8_t)type);
	}

	// True if the datagram starts like a relay datagram (right magic and version); outType gets its type
	inline bool parseHeader(const uint8_t* data, size_t size, uint8_t& outType)
	{
		if (size < HEADER_SIZE || memcmp(data, MAGIC, 4) != 0 || data[4] != VERSION)
			return false;
		outType = data[5];
		return true;
	}

	inline void buildHello(std::vector<uint8_t>& out, Type type, uint64_t lobby, uint64_t key)
	{
		out.clear();
		putHeader(out, type);
		put64(out, lobby);
		put64(out, key);
	}


	// Registration message of the host: like buildHello, plus the host secret
	inline void buildHostHello(std::vector<uint8_t>& out, uint64_t lobby, uint64_t key, uint64_t hostSecret)
	{
		buildHello(out, HOST_REGISTER, lobby, key);
		put64(out, hostSecret);
	}
	// Data datagram of a friend (token, payload)
	inline void buildClientData(std::vector<uint8_t>& out, Type type, uint32_t token, const uint8_t* payload, size_t payloadSize)
	{
		out.clear();
		putHeader(out, type);
		put32(out, token);
		out.insert(out.end(), payload, payload + payloadSize);
	}

	// Data datagram of the host (host token, friend id, payload)
	inline void buildHostData(std::vector<uint8_t>& out, Type type, uint32_t hostToken, uint16_t friendID, const uint8_t* payload, size_t payloadSize)
	{
		out.clear();
		putHeader(out, type);
		put32(out, hostToken);
		put16(out, friendID);
		out.insert(out.end(), payload, payload + payloadSize);
	}

	// The made-up peer addresses inside the game
	inline std::string virtualIP(uint16_t friendID)
	{
		return "240.0." + std::to_string((unsigned)(friendID >> 8)) + "." + std::to_string((unsigned)(friendID & 0xff));
	}
	inline std::string hostVirtualIP()  { return virtualIP(0); }

	inline bool isVirtualIP(const std::string& ip)
	{
		return ip.compare(0, 6, "240.0.") == 0;
	}

	inline bool parseVirtualIP(const std::string& ip, uint16_t& outFriendID)
	{
		// "240.0.<high>.<low>"
		if (!isVirtualIP(ip))
			return false;
		const size_t dot = ip.find('.', 6);
		if (dot == std::string::npos || dot == 6 || dot + 1 >= ip.size() || dot - 6 > 3 || ip.size() - dot - 1 > 3)
			return false;
		unsigned values[2] = { 0, 0 };
		for (int part = 0; part < 2; ++part)
		{
			const size_t begin = (part == 0) ? 6 : dot + 1;
			const size_t end = (part == 0) ? dot : ip.size();
			for (size_t i = begin; i < end; ++i)
			{
				if (ip[i] < '0' || ip[i] > '9')
					return false;
				values[part] = values[part] * 10 + (unsigned)(ip[i] - '0');
			}
		}
		if (values[0] > 255 || values[1] > 255)
			return false;
		outFriendID = (uint16_t)((values[0] << 8) | values[1]);
		return true;
	}
}
