/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>
#include "oxygen_netcore/network/internal/CryptoFunctions.h"

#include <map>
#include <random>
#include <string>


// Proof that both sides of a connection know the secret from the join / session code, without sending the secret itself.
//
// Handshake (after the transport connection is established):
//   host   -> friend : challenge (random host nonce)
//   friend -> host   : client nonce + proof("C", secret, host nonce, client nonce)
//   host   -> friend : result + (if accepted) proof("H", secret, host nonce, client nonce)
// The host refuses a friend whose proof is wrong, and the friend refuses a host whose proof is wrong, so neither a stranger who can reach the port
// nor a fake host on the same network (that only saw the broadcast) gets any further.
//
// Where the secret comes from:
//  - Join code (direct connection by IP): a random 30 bit number that is part of the code.
//  - Session code (lobby, matchmaking server, same network): derived from the 40 bit session ID, which only the people who were given the code know.
//    The broadcast on the local network only contains a one way hash of the session ID, never the ID itself.
//
// Honest limit: 40 bits (30 bits for a join code) are a small secret. Anybody who sees a proof, a LAN tag or a relay lobby name can try every
// possible code offline. Every value that is derived from such a code therefore goes through a deliberately slow key stretching
// (STRETCH_ITERATIONS chained hashes, computed once per hosting / joining and cached), which makes each guess that many times more expensive
// for the attacker. This is a cost, not a proof of impossibility: a determined attacker with a lot of GPU time can still win against a 30 bit join code.
// Header only, so that the developer test program tests exactly the code that the game uses.
namespace NetplayAuth
{
	// Chained hashes for key stretching: about 150 ms for each of the two calculations per hosting or joining (measured: 0.36 microseconds per chained hash).
	// Changing this changes every derived value (all friends must use the same number).
	static const int STRETCH_ITERATIONS = 400000;

	enum class JoinResult : uint8
	{
		ACCEPTED		= 0,
		WRONG_CODE		= 1,	// The proof did not match: the friend typed a code for a different host
		LOBBY_FULL		= 2,	// All player slots are taken
		TOO_MANY_TRIES	= 3,	// Too many wrong attempts from the same address
		KICKED			= 4,	// The host removed this address a moment ago
		VERSION_MISMATCH = 5	// The friend speaks another version of the lobby messages (see NetplayLobby::LOBBY_PROTOCOL): both need the same version
	};

	namespace detail
	{
		inline uint64 hashToUint64(const std::string& text)
		{
			uint32 words[5] = { 0 };
			Crypto::buildSHA1(text, words);
			return ((uint64)words[0] << 32) | (uint64)words[1];
		}

		// Chained hashing: every step needs the result of the one before, so the work for one guess cannot be shortcut
		inline std::string stretch(const std::string& domain, const std::string& value)
		{
			std::string state = domain + "|" + value;
			uint32 words[5] = { 0 };
			for (int i = 0; i < STRETCH_ITERATIONS; ++i)
			{
				Crypto::buildSHA1(state, words);
				state.assign((const char*)words, sizeof(words));
				state += domain;
			}
			return state;
		}

		// The stretching result for a code value, cached (a handshake must not repeat the slow part)
		inline const std::string& stretchCached(const std::string& domain, const std::string& value)
		{
			static thread_local std::map<std::string, std::string> cache;
			const std::string key = domain + "|" + value;
			auto it = cache.find(key);
			if (it == cache.end())
			{
				if (cache.size() >= 16)
					cache.clear();
				it = cache.emplace(key, stretch(domain, value)).first;
			}
			return it->second;
		}

		// Everything that is derived from a session ID starts from this one slow value
		inline const std::string& sessionMaster(uint64 sessionID)
		{
			return stretchCached("S3AIRSESSION2", std::to_string(sessionID));
		}
	}

	inline uint32 randomNonce()
	{
		std::random_device randomDevice;
		return (uint32)randomDevice() ^ ((uint32)randomDevice() << 7);
	}

	// A fresh random secret for a direct (IP) join code: 30 bits, never zero
	inline uint32 makeRandomSecret()
	{
		std::random_device randomDevice;
		uint32 secret = 0;
		while (secret == 0)
			secret = ((uint32)randomDevice() ^ ((uint32)randomDevice() << 11)) & 0x3fffffffu;
		return secret;
	}

	// The secret for a session code
	inline uint64 secretFromSession(uint64 sessionID)
	{
		return detail::hashToUint64("S3AIRSECRET2|" + detail::sessionMaster(sessionID));
	}

	// What gets broadcast on the local network instead of the session ID: it is enough to recognize the right host, but not to derive the secret
	inline uint64 lanTagFromSession(uint64 sessionID)
	{
		uint64 tag = detail::hashToUint64("S3AIRLANTAG2|" + detail::sessionMaster(sessionID));
		return (tag == 0) ? 1 : tag;
	}

	// What a relay server gets to see instead of the session ID (one way hashes, see RelayProtocol.h): a lobby name and a key that only people with the code can compute.
	// The relay operator could still try all 2^40 session IDs offline, so a relay should be run by somebody the players trust, like any server that carries their traffic.
	inline uint64 relayLobbyFromSession(uint64 sessionID)
	{
		uint64 id = detail::hashToUint64("S3AIRRELAYLOBBY2|" + detail::sessionMaster(sessionID));
		return (id == 0) ? 1 : id;
	}

	inline uint64 relayKeyFromSession(uint64 sessionID)
	{
		return detail::hashToUint64("S3AIRRELAYKEY2|" + detail::sessionMaster(sessionID));
	}

	// role: 'C' for the friend's proof, 'H' for the host's proof
	inline uint64 computeProof(char role, uint64 secret, uint32 hostNonce, uint32 clientNonce)
	{
		return detail::hashToUint64(std::string("S3AIRAUTH2|") + role + "|" + detail::stretchCached("S3AIRPROOFKEY2", std::to_string(secret)) + "|" + std::to_string(hostNonce) + "|" + std::to_string(clientNonce));
	}

	// Does the slow part of the proof calculation for this secret now (it is cached afterwards), so that the first handshake does not stall the game
	inline void prepareSecret(uint64 secret)
	{
		computeProof('C', secret, 0, 0);
	}

	inline bool verifyFriendProof(uint64 secret, uint32 hostNonce, uint32 clientNonce, uint64 proof)
	{
		return proof == computeProof('C', secret, hostNonce, clientNonce);
	}

	inline bool verifyHostProof(uint64 secret, uint32 hostNonce, uint32 clientNonce, uint64 proof)
	{
		return proof == computeProof('H', secret, hostNonce, clientNonce);
	}
}
