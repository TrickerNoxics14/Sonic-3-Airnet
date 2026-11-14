/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// The wire format between a game and the public lobby directory of a relay server (checks/relay_ws/directory.mjs, "protocol.mjs" there is the same format).
// Standard C++ only: the game and the developer tests include this one file.
//
// The directory speaks over the same transport as the relay (one UDP datagram for native games, one binary WebSocket message for browsers) with the same
// S3RL header (RelayProtocol::VERSION stays 2, so old games and old relay servers are not affected: an old relay drops these messages, an old game never sends them).
// Every message carries its own directory version as the first payload byte. Strings are one length byte plus ASCII bytes. Numbers are big endian.
//
//   client -> server : HELLO (version, padding)                  server -> client : HELLO_REPLY (version, flags, code digits, page size, lobbies, cookie)
//   host   -> server : PUBLISH (relay lobby, key, host secret, session ID, flags, players, max, compat, name, host name), repeated every 5 seconds
//                                                                server -> host   : PUBLISH_REPLY (version, status, code)
//   host   -> server : UNPUBLISH (relay lobby, key, host secret) server -> host   : PUBLISH_REPLY (status CLOSED)
//   client -> server : LIST (cookie, flags, offset, compat)      server -> client : LIST_REPLY (status, total, offset, count, entries)
//   client -> server : RESOLVE (cookie, code)                    server -> client : RESOLVE_REPLY (status, session ID, players, max, flags)
//
// The cookie comes from a HELLO and belongs to the sender's address: it is what stops somebody from making the server send a big reply to a forged address.
#include "oxygen_netcore/network/RelayProtocol.h"

namespace DirectoryProtocol
{
	static const uint8_t VERSION = 1;					// The highest directory version this game speaks
	static const size_t HELLO_MIN_PAYLOAD = 16;			// A HELLO is padded to this many payload bytes (a reply is never bigger than its request)
	static const size_t MAX_STRING = 255;

	enum Type : uint8_t
	{
		HELLO			= 16,
		HELLO_REPLY		= 17,
		PUBLISH			= 18,
		PUBLISH_REPLY	= 19,
		UNPUBLISH		= 20,
		LIST			= 21,
		LIST_REPLY		= 22,
		RESOLVE			= 23,
		RESOLVE_REPLY	= 24
	};

	enum PublishFlags : uint8_t { PUBLISH_PUBLIC = 1, PUBLISH_IN_GAME = 2 };
	enum ListFlags : uint8_t { LIST_COMPATIBLE_ONLY = 1 };
	enum EntryFlags : uint8_t { ENTRY_IN_GAME = 1, ENTRY_COMPATIBLE = 2, ENTRY_FULL = 4 };
	enum PublishStatus : uint8_t { PUBLISH_OK = 0, PUBLISH_NOT_REGISTERED = 1, PUBLISH_LIMIT = 2, PUBLISH_BAD_REQUEST = 3, PUBLISH_CLOSED = 4 };
	enum ListStatus : uint8_t { LIST_OK = 0, LIST_BAD_COOKIE = 1 };
	enum ResolveStatus : uint8_t { RESOLVE_OK = 0, RESOLVE_NOT_FOUND = 1, RESOLVE_BAD_COOKIE = 2, RESOLVE_THROTTLED = 3 };

	inline bool isDirectoryType(uint8_t type)  { return type >= HELLO && type <= RESOLVE_REPLY; }

	inline void putString(std::vector<uint8_t>& out, const std::string& text)
	{
		const size_t length = (text.size() > MAX_STRING) ? MAX_STRING : text.size();
		out.push_back((uint8_t)length);
		out.insert(out.end(), text.begin(), text.begin() + length);
	}

	inline void putHeader(std::vector<uint8_t>& out, Type type)
	{
		out.clear();
		out.insert(out.end(), RelayProtocol::MAGIC, RelayProtocol::MAGIC + 4);
		out.push_back(RelayProtocol::VERSION);
		out.push_back((uint8_t)type);
	}

	// Reads the fields of a message in order; a read past the end sets "bad" (and returns zeros)
	struct Reader
	{
		const uint8_t* mData = nullptr;
		size_t mSize = 0;
		size_t mPos = 0;
		bool mBad = false;

		Reader(const uint8_t* data, size_t size) : mData(data), mSize(size), mPos(RelayProtocol::HEADER_SIZE) {}
		bool need(size_t n)  { if (mPos + n > mSize) { mBad = true; return false; } return true; }
		uint8_t u8()    { if (!need(1)) return 0; return mData[mPos++]; }
		uint16_t u16()  { if (!need(2)) return 0; const uint16_t v = RelayProtocol::get16(mData + mPos); mPos += 2; return v; }
		uint64_t u64()  { if (!need(8)) return 0; const uint64_t v = RelayProtocol::get64(mData + mPos); mPos += 8; return v; }
		std::string str()
		{
			const size_t n = u8();
			if (!need(n))
				return std::string();
			std::string s((const char*)mData + mPos, n);
			mPos += n;
			return s;
		}
	};

	// ---- requests ----

	inline void buildHello(std::vector<uint8_t>& out)
	{
		putHeader(out, HELLO);
		out.push_back(VERSION);
		out.insert(out.end(), HELLO_MIN_PAYLOAD - 1, 0);
	}

	struct PublishInfo
	{
		uint64_t mRelayLobby = 0;
		uint64_t mRelayKey = 0;
		uint64_t mHostSecret = 0;
		uint64_t mSessionID = 0;
		bool mPublic = true;
		bool mInGame = false;
		uint8_t mPlayers = 1;
		uint8_t mMaxPlayers = 4;
		std::string mCompat;		// Which games can play together (a short hash of the version and the mods)
		std::string mName;			// The lobby's name (the server cleans it again)
		std::string mHostName;
	};

	inline void buildPublish(std::vector<uint8_t>& out, const PublishInfo& info)
	{
		putHeader(out, PUBLISH);
		out.push_back(VERSION);
		RelayProtocol::put64(out, info.mRelayLobby);
		RelayProtocol::put64(out, info.mRelayKey);
		RelayProtocol::put64(out, info.mHostSecret);
		RelayProtocol::put64(out, info.mSessionID);
		out.push_back((uint8_t)((info.mPublic ? PUBLISH_PUBLIC : 0) | (info.mInGame ? PUBLISH_IN_GAME : 0)));
		out.push_back(info.mPlayers);
		out.push_back(info.mMaxPlayers);
		putString(out, info.mCompat);
		putString(out, info.mName);
		putString(out, info.mHostName);
	}

	inline void buildUnpublish(std::vector<uint8_t>& out, uint64_t relayLobby, uint64_t relayKey, uint64_t hostSecret)
	{
		putHeader(out, UNPUBLISH);
		out.push_back(VERSION);
		RelayProtocol::put64(out, relayLobby);
		RelayProtocol::put64(out, relayKey);
		RelayProtocol::put64(out, hostSecret);
	}

	inline void buildList(std::vector<uint8_t>& out, uint64_t cookie, bool compatibleOnly, uint8_t offset, const std::string& compat)
	{
		putHeader(out, LIST);
		out.push_back(VERSION);
		RelayProtocol::put64(out, cookie);
		out.push_back(compatibleOnly ? LIST_COMPATIBLE_ONLY : 0);
		out.push_back(offset);
		putString(out, compat);
	}

	inline void buildResolve(std::vector<uint8_t>& out, uint64_t cookie, const std::string& code)
	{
		putHeader(out, RESOLVE);
		out.push_back(VERSION);
		RelayProtocol::put64(out, cookie);
		putString(out, code);
	}

	// ---- replies ----

	struct HelloReply
	{
		uint8_t mVersion = 0;
		uint8_t mFlags = 0;
		uint8_t mCodeDigits = 0;
		uint8_t mPageSize = 0;
		uint16_t mLobbies = 0;
		uint64_t mCookie = 0;
	};

	inline bool parseHelloReply(const uint8_t* data, size_t size, HelloReply& out)
	{
		Reader r(data, size);
		out.mVersion = r.u8();  out.mFlags = r.u8();  out.mCodeDigits = r.u8();  out.mPageSize = r.u8();  out.mLobbies = r.u16();  out.mCookie = r.u64();
		return !r.mBad && out.mVersion >= 1 && out.mCookie != 0;
	}

	struct PublishReply
	{
		uint8_t mStatus = 0;
		std::string mCode;
	};

	inline bool parsePublishReply(const uint8_t* data, size_t size, PublishReply& out)
	{
		Reader r(data, size);
		const uint8_t version = r.u8();
		out.mStatus = r.u8();
		out.mCode = r.str();
		return !r.mBad && version >= 1;
	}

	struct ListEntry
	{
		std::string mCode;
		std::string mName;
		std::string mHostName;
		std::string mCompat;
		uint8_t mPlayers = 0;
		uint8_t mMaxPlayers = 0;
		uint8_t mFlags = 0;
		uint16_t mAgeSeconds = 0;
		inline bool isInGame() const	 { return (mFlags & ENTRY_IN_GAME) != 0; }
		inline bool isCompatible() const { return (mFlags & ENTRY_COMPATIBLE) != 0; }
		inline bool isFull() const		 { return (mFlags & ENTRY_FULL) != 0; }
	};

	struct ListReply
	{
		uint8_t mStatus = 0;
		uint16_t mTotal = 0;
		uint8_t mOffset = 0;
		std::vector<ListEntry> mEntries;
	};

	inline bool parseListReply(const uint8_t* data, size_t size, ListReply& out)
	{
		Reader r(data, size);
		const uint8_t version = r.u8();
		out.mStatus = r.u8();
		out.mEntries.clear();
		if (r.mBad || version < 1)
			return false;
		if (out.mStatus != LIST_OK)
			return true;
		out.mTotal = r.u16();
		out.mOffset = r.u8();
		const int count = r.u8();
		for (int i = 0; i < count && !r.mBad; ++i)
		{
			ListEntry entry;
			entry.mCode = r.str();  entry.mName = r.str();  entry.mHostName = r.str();  entry.mCompat = r.str();
			entry.mPlayers = r.u8();  entry.mMaxPlayers = r.u8();  entry.mFlags = r.u8();  entry.mAgeSeconds = r.u16();
			out.mEntries.push_back(entry);
		}
		return !r.mBad;
	}

	struct ResolveReply
	{
		uint8_t mStatus = 0;
		uint64_t mSessionID = 0;
		uint8_t mPlayers = 0;
		uint8_t mMaxPlayers = 0;
		uint8_t mFlags = 0;
		uint16_t mRetryAfterSeconds = 0;
		inline bool isInGame() const  { return (mFlags & ENTRY_IN_GAME) != 0; }
		inline bool isFull() const	  { return (mFlags & ENTRY_FULL) != 0; }
	};

	inline bool parseResolveReply(const uint8_t* data, size_t size, ResolveReply& out)
	{
		Reader r(data, size);
		const uint8_t version = r.u8();
		out.mStatus = r.u8();
		if (r.mBad || version < 1)
			return false;
		if (out.mStatus == RESOLVE_OK)
		{
			out.mSessionID = r.u64();  out.mPlayers = r.u8();  out.mMaxPlayers = r.u8();  out.mFlags = r.u8();
		}
		else if (out.mStatus == RESOLVE_THROTTLED)
		{
			out.mRetryAfterSeconds = r.u16();
		}
		return !r.mBad;
	}
}
