/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// The logic of the relay server, without any sockets: it gets the received datagrams and the current time, and calls a function to send datagrams.
// That is what makes it testable (the developer tests feed it forged datagrams and a fake clock) and keeps the real program (s3air_relay.cpp) tiny.
//
// Privacy: the relay keeps addresses only in memory, only for as long as a lobby or friend is active, and never writes them anywhere.
// It does not read the payload of the data it forwards. The statistics are counters only.
#include "RelayProtocol.h"

#include <algorithm>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace Relay
{
	struct Endpoint
	{
		uint32_t mIP = 0;		// IPv4, host byte order
		uint16_t mPort = 0;
		inline bool operator==(const Endpoint& other) const  { return mIP == other.mIP && mPort == other.mPort; }
		inline bool operator!=(const Endpoint& other) const  { return !operator==(other); }
		inline bool operator<(const Endpoint& other) const   { return (mIP != other.mIP) ? (mIP < other.mIP) : (mPort < other.mPort); }
	};

	struct Limits
	{
		size_t mMaxLobbies = 512;
		int mMaxPipesPerLobby = 6;				// The game has 3 friends at most; the rest is room for friends that reconnect
		int mMaxLobbiesPerIP = 4;
		double mHostTimeoutSeconds = 45.0;		// A lobby disappears when its host was silent for this long (the game repeats its registration every 5 seconds)
		double mPipeTimeoutSeconds = 60.0;		// A friend that sent nothing for this long is forgotten
		double mControlPerSecond = 8.0;			// Registration and join requests per second and sender IP address (friends and a host behind the same router share it)
		double mControlBurst = 24.0;
		int mMaxBadKeys = 8;					// Wrong keys from one address before it is ignored ...
		double mBlockSeconds = 60.0;			// ... for this long
		double mDataPacketsPerSecond = 3000.0;	// Per friend and direction (a state transfer to a joining friend peaks at about 1500 datagrams per second)
		double mDataPacketBurst = 6000.0;
		double mDataBytesPerSecond = 3000000.0;
		double mDataByteBurst = 6000000.0;
		size_t mMaxSources = 8192;				// IP addresses that are remembered for the rate limits
	};

	struct Stats
	{
		uint64_t mReceived = 0;
		uint64_t mForwarded = 0;
		uint64_t mForwardedBytes = 0;
		uint64_t mDroppedInvalid = 0;		// Not a relay datagram, wrong size, unknown friend, wrong token
		uint64_t mDroppedRate = 0;			// Over a rate limit
		uint64_t mDroppedBlocked = 0;		// From an address that sent too many wrong keys
		uint64_t mWrongKeys = 0;
		uint64_t mRefused = 0;				// Valid requests that were turned down (no host, lobby full, too many lobbies)
		uint64_t mLobbiesCreated = 0;
		uint32_t mLobbies = 0;
		uint32_t mPipes = 0;
	};

	class Core
	{
	public:
		typedef std::function<void(const Endpoint& to, const uint8_t* data, size_t size)> SendFunction;

		Core(const Limits& limits, SendFunction sendFunction) : mLimits(limits), mSend(std::move(sendFunction)) {}

		inline const Stats& getStats() const  { return mStats; }
		inline const Limits& getLimits() const  { return mLimits; }

		// One received datagram
		void onDatagram(const Endpoint& from, const uint8_t* data, size_t size, double now)
		{
			++mStats.mReceived;
			uint8_t type = 0;
			if (size > RelayProtocol::MAX_DATAGRAM_SIZE || !RelayProtocol::parseHeader(data, size, type))
			{
				++mStats.mDroppedInvalid;
				return;
			}

			switch (type)
			{
				case RelayProtocol::HOST_REGISTER:
				case RelayProtocol::CLIENT_JOIN:
				{
					if (size != ((type == RelayProtocol::HOST_REGISTER) ? RelayProtocol::HOST_HELLO_SIZE : RelayProtocol::HELLO_SIZE))
					{
						++mStats.mDroppedInvalid;
						return;
					}
					Source* source = getSource(from.mIP, now);
					if (nullptr == source)
					{
						++mStats.mDroppedRate;
						return;
					}
					if (now < source->mBlockedUntil)
					{
						++mStats.mDroppedBlocked;
						return;
					}
					if (!source->mControl.take(1.0, mLimits.mControlPerSecond, mLimits.mControlBurst, now))
					{
						++mStats.mDroppedRate;
						return;
					}
					const uint64_t lobby = RelayProtocol::get64(data + RelayProtocol::HEADER_SIZE);
					const uint64_t key = RelayProtocol::get64(data + RelayProtocol::HEADER_SIZE + 8);
					if (lobby == 0 || (type == RelayProtocol::HOST_REGISTER && RelayProtocol::get64(data + RelayProtocol::HELLO_SIZE) == 0))
					{
						++mStats.mDroppedInvalid;
						return;
					}
					if (type == RelayProtocol::HOST_REGISTER)
						handleHostRegister(from, *source, lobby, key, RelayProtocol::get64(data + RelayProtocol::HELLO_SIZE), now);
					else
						handleClientJoin(from, *source, lobby, key, now);
					break;
				}

				case RelayProtocol::DATA_C2R:
					handleClientData(from, data, size, now);
					break;

				case RelayProtocol::DATA_H2R:
					handleHostData(from, data, size, now);
					break;

				default:
					++mStats.mDroppedInvalid;
					break;
			}
		}

		// Call about once per second: forgets lobbies, friends and addresses that were silent for too long
		void update(double now)
		{
			for (auto it = mLobbies.begin(); it != mLobbies.end(); )
			{
				Lobby& lobby = it->second;
				if (now - lobby.mHostSeen > mLimits.mHostTimeoutSeconds)
				{
					const uint64_t id = it->first;
					++it;
					removeLobby(id);
					continue;
				}
				for (size_t k = 0; k < lobby.mPipes.size(); )
				{
					if (now - lobby.mPipes[k].mLastSeen > mLimits.mPipeTimeoutSeconds)
					{
						mPipeByEndpoint.erase(lobby.mPipes[k].mClient);
						lobby.mPipes.erase(lobby.mPipes.begin() + k);
					}
					else
						++k;
				}
				++it;
			}
			pruneSources(now);
			refreshCounts();
		}

		bool hasLobby(uint64_t lobby) const  { return mLobbies.find(lobby) != mLobbies.end(); }
		size_t getSourceCount() const  { return mSources.size(); }

	private:
		struct Bucket
		{
			double mTokens = -1.0;		// Negative = not started yet (starts full)
			double mLast = 0.0;
			bool take(double cost, double perSecond, double burst, double now)
			{
				if (mTokens < 0.0)
				{
					mTokens = burst;
					mLast = now;
				}
				mTokens = std::min(burst, mTokens + std::max(0.0, now - mLast) * perSecond);
				mLast = now;
				if (mTokens < cost)
					return false;
				mTokens -= cost;
				return true;
			}
		};

		struct Direction
		{
			Bucket mPackets;
			Bucket mBytes;
		};

		struct Pipe
		{
			Endpoint mClient;
			uint32_t mToken = 0;
			uint16_t mID = 0;
			double mLastSeen = 0.0;
			Direction mUp;		// friend -> host
			Direction mDown;	// host -> friend
		};

		struct Lobby
		{
			uint64_t mKey = 0;
			uint64_t mHostSecret = 0;		// Random number of the registering game, not derivable from the code: only the real host can re-register or take over the lobby
			Endpoint mHost;
			uint32_t mHostToken = 0;
			double mHostSeen = 0.0;
			uint16_t mNextID = 1;
			std::vector<Pipe> mPipes;
		};

		struct Source
		{
			Bucket mControl;
			int mBadKeys = 0;
			double mBlockedUntil = 0.0;
			double mLastSeen = 0.0;
		};

		static inline uint32_t randomToken()
		{
			std::random_device device;		// The operating system's random numbers: a token that cannot be guessed is what stops forged data datagrams
			uint32_t token = 0;
			while (token == 0)
				token = (uint32_t)device() ^ ((uint32_t)device() << 16);
			return token;
		}

		Source* getSource(uint32_t ip, double now)
		{
			auto it = mSources.find(ip);
			if (it == mSources.end())
			{
				if (mSources.size() >= mLimits.mMaxSources && now - mLastPrune >= 1.0)		// (not for every datagram: a flood from many addresses must not make each datagram expensive)
				{
					pruneSources(now, 120.0);
					if (mSources.size() >= mLimits.mMaxSources)
						pruneSources(now, 10.0);	// Still full: a flood from thousands of addresses. Forget the ones that have been quiet a little while so that real newcomers are still served
				}
				if (mSources.size() >= mLimits.mMaxSources)
					return nullptr;		// Memory stays bounded; only a flood that really comes from thousands of different IP addresses gets here
				it = mSources.emplace(ip, Source()).first;
			}
			it->second.mLastSeen = now;
			return &it->second;
		}

		void pruneSources(double now, double quietSeconds = 120.0)
		{
			mLastPrune = now;
			for (auto it = mSources.begin(); it != mSources.end(); )
			{
				if (now - it->second.mLastSeen > quietSeconds && now >= it->second.mBlockedUntil)
					it = mSources.erase(it);
				else
					++it;
			}
		}

		void refreshCounts()
		{
			mStats.mLobbies = (uint32_t)mLobbies.size();
			uint32_t pipes = 0;
			for (const auto& entry : mLobbies)
				pipes += (uint32_t)entry.second.mPipes.size();
			mStats.mPipes = pipes;
		}

		void sendTo(const Endpoint& to, const std::vector<uint8_t>& data)
		{
			mSend(to, data.data(), data.size());
		}

		void badKey(Source& source, double now)
		{
			++mStats.mWrongKeys;
			if (++source.mBadKeys >= mLimits.mMaxBadKeys)
			{
				source.mBadKeys = 0;
				source.mBlockedUntil = now + mLimits.mBlockSeconds;
			}
		}

		void removeLobby(uint64_t id)
		{
			auto it = mLobbies.find(id);
			if (it == mLobbies.end())
				return;
			for (const Pipe& pipe : it->second.mPipes)
				mPipeByEndpoint.erase(pipe.mClient);
			auto hostIt = mHostByEndpoint.find(it->second.mHost);
			if (hostIt != mHostByEndpoint.end() && hostIt->second == id)
				mHostByEndpoint.erase(hostIt);
			mLobbies.erase(it);
		}

		void handleHostRegister(const Endpoint& from, Source& source, uint64_t lobbyID, uint64_t key, uint64_t hostSecret, double now)
		{
			auto it = mLobbies.find(lobbyID);
			if (it == mLobbies.end())
			{
				int fromSameIP = 0;
				for (const auto& entry : mLobbies)
					fromSameIP += (entry.second.mHost.mIP == from.mIP) ? 1 : 0;
				if (mLobbies.size() >= mLimits.mMaxLobbies || fromSameIP >= mLimits.mMaxLobbiesPerIP)
				{
					++mStats.mRefused;
					return;
				}
				Lobby lobby;
				lobby.mKey = key;
				lobby.mHostSecret = hostSecret;
				lobby.mHostToken = randomToken();
				it = mLobbies.emplace(lobbyID, std::move(lobby)).first;
				++mStats.mLobbiesCreated;
			}
			else if (it->second.mKey != key || it->second.mHostSecret != hostSecret)
			{
				// Wrong key, or the right key (every friend knows it) without the secret of the game that registered the lobby: somebody who is not the host
				// must not be able to take the lobby over. Not a word back, so that nobody learns anything.
				badKey(source, now);
				return;
			}

			// The same address was host of another lobby: that one is over
			auto previous = mHostByEndpoint.find(from);
			if (previous != mHostByEndpoint.end() && previous->second != lobbyID)
			{
				const uint64_t oldLobby = previous->second;
				removeLobby(oldLobby);
			}
			it = mLobbies.find(lobbyID);
			Lobby& lobby = it->second;
			if (lobby.mHost != from)
			{
				auto old = mHostByEndpoint.find(lobby.mHost);
				if (old != mHostByEndpoint.end() && old->second == lobbyID)
					mHostByEndpoint.erase(old);
				lobby.mHost = from;
			}
			lobby.mHostSeen = now;
			mHostByEndpoint[from] = lobbyID;

			std::vector<uint8_t> reply;
			RelayProtocol::putHeader(reply, RelayProtocol::HOST_REGISTERED);
			RelayProtocol::put64(reply, lobbyID);
			RelayProtocol::put32(reply, lobby.mHostToken);
			sendTo(from, reply);
			refreshCounts();
		}

		void refuse(const Endpoint& to, RelayProtocol::Refusal reason)
		{
			++mStats.mRefused;
			std::vector<uint8_t> reply;
			RelayProtocol::putHeader(reply, RelayProtocol::CLIENT_REFUSED);
			reply.push_back((uint8_t)reason);
			sendTo(to, reply);
		}

		void handleClientJoin(const Endpoint& from, Source& source, uint64_t lobbyID, uint64_t key, double now)
		{
			auto it = mLobbies.find(lobbyID);
			if (it == mLobbies.end())
			{
				refuse(from, RelayProtocol::NO_HOST);
				return;
			}
			Lobby& lobby = it->second;
			if (lobby.mKey != key)
			{
				badKey(source, now);
				return;
			}
			if (now - lobby.mHostSeen > mLimits.mHostTimeoutSeconds)
			{
				refuse(from, RelayProtocol::NO_HOST);
				return;
			}

			Pipe* pipe = nullptr;
			auto existing = mPipeByEndpoint.find(from);
			if (existing != mPipeByEndpoint.end())
			{
				if (existing->second.first == lobbyID)
				{
					pipe = findPipe(lobby, existing->second.second);
				}
				else
				{
					// This friend moved on to another lobby
					auto oldLobby = mLobbies.find(existing->second.first);
					if (oldLobby != mLobbies.end())
					{
						const uint16_t oldID = existing->second.second;
						auto& pipes = oldLobby->second.mPipes;
						pipes.erase(std::remove_if(pipes.begin(), pipes.end(), [oldID](const Pipe& p) { return p.mID == oldID; }), pipes.end());
					}
					mPipeByEndpoint.erase(existing);
				}
			}
			if (nullptr == pipe)
			{
				if ((int)lobby.mPipes.size() >= mLimits.mMaxPipesPerLobby)
				{
					refuse(from, RelayProtocol::FULL);
					return;
				}
				Pipe fresh;
				fresh.mClient = from;
				fresh.mToken = randomToken();
				for (int tries = 0; tries < 70000; ++tries)		// A free friend number (never 0: that is the host)
				{
					if (lobby.mNextID == 0)
						lobby.mNextID = 1;
					const uint16_t candidate = lobby.mNextID++;
					if (findPipe(lobby, candidate) == nullptr)
					{
						fresh.mID = candidate;
						break;
					}
				}
				lobby.mPipes.push_back(fresh);
				pipe = &lobby.mPipes.back();
				mPipeByEndpoint[from] = std::make_pair(lobbyID, pipe->mID);
			}
			pipe->mLastSeen = now;

			std::vector<uint8_t> reply;
			RelayProtocol::putHeader(reply, RelayProtocol::CLIENT_JOINED);
			RelayProtocol::put32(reply, pipe->mToken);
			sendTo(from, reply);
			refreshCounts();
		}

		Pipe* findPipe(Lobby& lobby, uint16_t id)
		{
			for (Pipe& pipe : lobby.mPipes)
				if (pipe.mID == id)
					return &pipe;
			return nullptr;
		}

		void handleClientData(const Endpoint& from, const uint8_t* data, size_t size, double now)
		{
			if (size < RelayProtocol::HEADER_SIZE + 4)
			{
				++mStats.mDroppedInvalid;
				return;
			}
			auto entry = mPipeByEndpoint.find(from);
			if (entry == mPipeByEndpoint.end())
			{
				++mStats.mDroppedInvalid;
				return;
			}
			auto lobbyIt = mLobbies.find(entry->second.first);
			Pipe* pipe = (lobbyIt == mLobbies.end()) ? nullptr : findPipe(lobbyIt->second, entry->second.second);
			if (nullptr == pipe || pipe->mToken != RelayProtocol::get32(data + RelayProtocol::HEADER_SIZE))
			{
				++mStats.mDroppedInvalid;
				return;
			}
			Lobby& lobby = lobbyIt->second;
			if (now - lobby.mHostSeen > mLimits.mHostTimeoutSeconds)
			{
				++mStats.mDroppedInvalid;
				return;
			}
			const uint8_t* payload = data + RelayProtocol::HEADER_SIZE + 4;
			const size_t payloadSize = size - (RelayProtocol::HEADER_SIZE + 4);
			if (!pipe->mUp.mPackets.take(1.0, mLimits.mDataPacketsPerSecond, mLimits.mDataPacketBurst, now) ||
				!pipe->mUp.mBytes.take((double)payloadSize, mLimits.mDataBytesPerSecond, mLimits.mDataByteBurst, now))
			{
				++mStats.mDroppedRate;
				return;
			}
			pipe->mLastSeen = now;

			RelayProtocol::buildHostData(mScratch, RelayProtocol::DATA_R2H, lobby.mHostToken, pipe->mID, payload, payloadSize);
			++mStats.mForwarded;
			mStats.mForwardedBytes += payloadSize;
			sendTo(lobby.mHost, mScratch);
		}

		void handleHostData(const Endpoint& from, const uint8_t* data, size_t size, double now)
		{
			if (size < RelayProtocol::HEADER_SIZE + 6)
			{
				++mStats.mDroppedInvalid;
				return;
			}
			auto entry = mHostByEndpoint.find(from);
			auto lobbyIt = (entry == mHostByEndpoint.end()) ? mLobbies.end() : mLobbies.find(entry->second);
			if (lobbyIt == mLobbies.end() || lobbyIt->second.mHostToken != RelayProtocol::get32(data + RelayProtocol::HEADER_SIZE))
			{
				++mStats.mDroppedInvalid;
				return;
			}
			Lobby& lobby = lobbyIt->second;
			lobby.mHostSeen = now;		// A host that keeps sending data is alive, with or without registration messages
			Pipe* pipe = findPipe(lobby, RelayProtocol::get16(data + RelayProtocol::HEADER_SIZE + 4));
			if (nullptr == pipe)
			{
				++mStats.mDroppedInvalid;
				return;
			}
			const uint8_t* payload = data + RelayProtocol::HEADER_SIZE + 6;
			const size_t payloadSize = size - (RelayProtocol::HEADER_SIZE + 6);
			if (!pipe->mDown.mPackets.take(1.0, mLimits.mDataPacketsPerSecond, mLimits.mDataPacketBurst, now) ||
				!pipe->mDown.mBytes.take((double)payloadSize, mLimits.mDataBytesPerSecond, mLimits.mDataByteBurst, now))
			{
				++mStats.mDroppedRate;
				return;
			}

			RelayProtocol::buildClientData(mScratch, RelayProtocol::DATA_R2C, pipe->mToken, payload, payloadSize);
			++mStats.mForwarded;
			mStats.mForwardedBytes += payloadSize;
			sendTo(pipe->mClient, mScratch);
		}

	private:
		Limits mLimits;
		SendFunction mSend;
		Stats mStats;
		std::map<uint64_t, Lobby> mLobbies;
		std::map<Endpoint, uint64_t> mHostByEndpoint;
		std::map<Endpoint, std::pair<uint64_t, uint16_t>> mPipeByEndpoint;
		std::map<uint32_t, Source> mSources;		// By IP address, not by IP and port: changing the source port must not give a sender a fresh rate limit or a fresh bad-key count
		double mLastPrune = 0.0;
		std::vector<uint8_t> mScratch;
	};
}
