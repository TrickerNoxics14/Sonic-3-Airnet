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
// Header only, so that the developer test program tests exactly the code that the game uses.
namespace NetplayAuth
{
	enum class JoinResult : uint8
	{
		ACCEPTED		= 0,
		WRONG_CODE		= 1,	// The proof did not match: the friend typed a code for a different host
		LOBBY_FULL		= 2,	// All player slots are taken
		TOO_MANY_TRIES	= 3,	// Too many wrong attempts from the same address
		KICKED			= 4		// The host removed this address a moment ago
	};

	namespace detail
	{
		inline uint64 hashToUint64(const std::string& text)
		{
			uint32 words[5] = { 0 };
			Crypto::buildSHA1(text, words);
			return ((uint64)words[0] << 32) | (uint64)words[1];
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
	inline uint32 secretFromSession(uint64 sessionID)
	{
		return (uint32)detail::hashToUint64("S3AIRSECRET1|" + std::to_string(sessionID));
	}

	// What gets broadcast on the local network instead of the session ID: it is enough to recognize the right host, but not to derive the secret
	inline uint64 lanTagFromSession(uint64 sessionID)
	{
		uint64 tag = detail::hashToUint64("S3AIRLANTAG1|" + std::to_string(sessionID));
		return (tag == 0) ? 1 : tag;
	}

	// role: 'C' for the friend's proof, 'H' for the host's proof
	inline uint64 computeProof(char role, uint32 secret, uint32 hostNonce, uint32 clientNonce)
	{
		return detail::hashToUint64(std::string("S3AIRAUTH1|") + role + "|" + std::to_string(secret) + "|" + std::to_string(hostNonce) + "|" + std::to_string(clientNonce));
	}

	inline bool verifyFriendProof(uint32 secret, uint32 hostNonce, uint32 clientNonce, uint64 proof)
	{
		return proof == computeProof('C', secret, hostNonce, clientNonce);
	}

	inline bool verifyHostProof(uint32 secret, uint32 hostNonce, uint32 clientNonce, uint64 proof)
	{
		return proof == computeProof('H', secret, hostNonce, clientNonce);
	}
}
