/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen_netcore/network/HighLevelPacketBase.h"
#include "oxygen/network/netplay/NetplayLobbyLogic.h"


// Sent from host to clients
struct StartGamePacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("StartGamePacket");

	static const uint8 PACKET_VERSION = 5;
	static const size_t MAX_BLOB_SIZE = 0x100000;		// Reading refuses larger sizes before anything gets allocated

	uint8 mPacketVersion = PACKET_VERSION;
	uint32 mGameBuildVersion = 0;
	uint8 mTransferMode = 0;
	uint8 mGameMode = 0;
	uint8 mPlayerIndex = 1;
	uint32 mFirstFrameNumber = 0;
	uint64 mRNGState[4] = { 0 };
	uint16 mCharacterPicks[4] = { NetplayLobby::CHARACTER_MENU, NetplayLobby::CHARACTER_TAILS, NetplayLobby::CHARACTER_KNUCKLES, NetplayLobby::CHARACTER_SONIC };		// The character of each player slot: fixed for this session

	std::vector<uint8> mSerializedGameSettings;
	std::vector<uint8> mActiveModsInfo;		// Text with the active mods (and versions), to check that host and client use the same ones
	// TODO: Add more, like persistent data, list of active mods incl. versions

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mPacketVersion);
		if (serializer.isReading() && mPacketVersion != PACKET_VERSION)
		{
			serializer.setError();
			return;
		}

		serializer.serialize(mGameBuildVersion);
		serializer.serialize(mTransferMode);
		serializer.serialize(mGameMode);
		serializer.serialize(mPlayerIndex);
		serializer.serialize(mFirstFrameNumber);

		for (int k = 0; k < 4; ++k)
			serializer.serialize(mRNGState[k]);
		for (int k = 0; k < 4; ++k)
			serializer.serialize(mCharacterPicks[k]);

		serializer.serializeData(mSerializedGameSettings, MAX_BLOB_SIZE);
		serializer.serializeData(mActiveModsInfo, MAX_BLOB_SIZE);
	}
};


// Sent from host to a client that joins a game which is already running: one chunk of the (compressed) complete game state
struct JoinStateChunkPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("JoinStateChunkPacket");

	uint32 mFrameNumber = 0;		// Frame number the state belongs to
	uint32 mCompressedSize = 0;
	uint32 mUncompressedSize = 0;
	uint32 mOffset = 0;				// Offset of this chunk inside the compressed data
	uint16 mLastInputs[4] = { 0 };	// Input state of the last frame, as the game compares it to the next frame's input
	uint16 mScreenWidth = 0;		// Render resolution of the host (the game scripts depend on it)
	uint16 mScreenHeight = 0;
	static const size_t MAX_CHUNK_SIZE = 0xffff;		// A chunk carries 1000 bytes; larger sizes are refused before anything gets allocated
	std::vector<uint8> mData;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mFrameNumber);
		serializer.serialize(mCompressedSize);
		serializer.serialize(mUncompressedSize);
		serializer.serialize(mOffset);
		for (int k = 0; k < 4; ++k)
			serializer.serialize(mLastInputs[k]);
		serializer.serialize(mScreenWidth);
		serializer.serialize(mScreenHeight);
		serializer.serializeData(mData, MAX_CHUNK_SIZE);
	}
};


// Lobby (before the game starts): the host tells every client who is in the lobby, who is ready, and what the input delay is
struct LobbyStatePacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyStatePacket");

	static const size_t MAX_NAME_BYTES = 16;

	uint8 mYourSlot = 0;				// The slot (1 to 3) of the client who receives this packet
	uint8 mSlotState[4] = { 0 };		// Per slot: 0 = empty, 1 = host, 2 = joined, 3 = ready
	uint8 mHostStarting = 0;			// 1 once the host leaves the lobby screen to pick the game
	std::string mNames[4];				// Player names (already cleaned up by the host)
	uint16 mPingMs[4] = { 0xffff, 0xffff, 0xffff, 0xffff };		// Round trip time per slot as measured by the host, 0xffff = unknown
	uint8 mDelaySetting = 0xff;			// Input delay chosen by the host: 0xff = Auto, otherwise the number of frames
	uint8 mEffectiveDelay = 0;			// The input delay that is in use right now
	uint8 mReadyCheckSeconds = 0;		// Seconds left of a running ready check (0 = none)

	// Character choice (see NetplayLobby::CHARACTER_*): the pick of each player slot as the host accepted it. This part is optional on the wire
	// (a message without it leaves the defaults), and the join handshake refuses peers of another NetplayLobby::LOBBY_PROTOCOL anyway.
	uint16 mCharacters[4] = { NetplayLobby::CHARACTER_NONE, NetplayLobby::CHARACTER_NONE, NetplayLobby::CHARACTER_NONE, NetplayLobby::CHARACTER_NONE };
	uint8 mCharacterNotice = 0;			// Bit k: the host changed the request of player slot k (not allowed or not installed), the player is told
	uint8 mCharactersLocked = 0;		// 1 once the game has started: the picks cannot change any more

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mYourSlot);
		for (int k = 0; k < 4; ++k)
			serializer.serialize(mSlotState[k]);
		serializer.serialize(mHostStarting);
		for (int k = 0; k < 4; ++k)
			serializer.serialize(mNames[k], MAX_NAME_BYTES);
		for (int k = 0; k < 4; ++k)
			serializer.serialize(mPingMs[k]);
		serializer.serialize(mDelaySetting);
		serializer.serialize(mEffectiveDelay);
		serializer.serialize(mReadyCheckSeconds);

		if (!serializer.isReading() || serializer.getRemaining() >= 10)
		{
			for (int k = 0; k < 4; ++k)
				serializer.serialize(mCharacters[k]);
			serializer.serialize(mCharacterNotice);
			serializer.serialize(mCharactersLocked);
		}
	}
};


// Sent from a client to the host: the player pressed Ready (or took it back); also sent every two seconds, together with the player's name
struct LobbyReadyPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyReadyPacket");

	uint8 mReady = 0;
	std::string mName;		// The host cleans it up again, nothing the friend sends is trusted
	uint16 mCharacter = NetplayLobby::CHARACTER_NONE;		// The character the player wants (NetplayLobby::CHARACTER_*), or CHARACTER_NONE for no request; the host validates it

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mReady);
		serializer.serialize(mName, LobbyStatePacket::MAX_NAME_BYTES);
		if (!serializer.isReading() || serializer.getRemaining() >= 2)
			serializer.serialize(mCharacter);
		else
			mCharacter = NetplayLobby::CHARACTER_NONE;
	}
};


// A chat line from a client to the host (a quick message by number, or a short free text); the host checks it and passes it on
struct LobbyChatPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyChatPacket");

	static const size_t MAX_TEXT_BYTES = 64;

	uint8 mQuickMessage = 0;		// 1 to 12, or 0 for free text
	std::string mText;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mQuickMessage);
		serializer.serialize(mText, MAX_TEXT_BYTES);
	}
};


// A chat line that the host passes on to everybody, with the slot of the player who said it
struct LobbyChatBroadcastPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyChatBroadcastPacket");

	uint8 mSlot = 0;
	std::string mText;		// Already the final text (the quick message, or the cleaned up free text)

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mSlot);
		serializer.serialize(mText, LobbyChatPacket::MAX_TEXT_BYTES);
	}
};


// Ping measurement: the host sends a token, the friend sends it straight back (both unreliable, a lost one is just no measurement)
struct LobbyPingPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyPingPacket");

	uint32 mToken = 0;

	virtual bool isReliablePacket() const override  { return false; }
	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mToken);
	}
};

struct LobbyPongPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyPongPacket");

	uint32 mToken = 0;

	virtual bool isReliablePacket() const override  { return false; }
	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mToken);
	}
};


// The host sends a friend away (kicked, or not ready when the game started)
struct LobbyLeavePacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyLeavePacket");

	uint8 mReason = 0;		// A NetplayLobby::LeaveReason

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mReason);
	}
};


// Sent regularly from host to clients: fingerprint of the game state at the start of a frame, to detect when a client's game drifted away
struct SyncCheckPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("SyncCheckPacket");

	uint32 mFrameNumber = 0;
	uint32 mHash = 0;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mFrameNumber);
		serializer.serialize(mHash);
	}
};


// Sent from a client to the host when the fingerprints did not match; the host answers with a fresh copy of the game state
struct ResyncRequestPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("ResyncRequestPacket");

	uint32 mFrameNumber = 0;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mFrameNumber);
	}
};


// Sent from a client to the host when the game state was received and loaded completely
struct JoinStateLoadedPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("JoinStateLoadedPacket");

	uint32 mFrameNumber = 0;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mFrameNumber);
	}
};


// Sent from host to clients
struct GameStateIncrementPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("GameStateIncrementPacket");

	uint8 mGameplayState = 0;
	uint32 mFrameNumber = 0;		// Most recent frame number
	uint8 mNumFrames = 0;
	uint8 mNumPlayers = 0;
	std::vector<uint16> mInputs;	// One input per player and frame (player inputs are packed together, forming one frame)
	uint32 mLastClientReceivedFrame = 0;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mGameplayState);
		serializer.serialize(mFrameNumber);
		serializer.serialize(mNumFrames);
		serializer.serialize(mNumPlayers);

		if (serializer.isReading())
			mInputs.resize(mNumFrames * mNumPlayers);

		for (uint16& inputs : mInputs)
			serializer.serialize(inputs);

		serializer.serialize(mLastClientReceivedFrame);
	}
};


// Sent from clients to host
struct PlayerInputIncrementPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("PlayerInputIncrementPacket");

	uint32 mFrameNumber = 0;		// Most recent frame number
	uint8 mNumFrames = 0;
	std::vector<uint16> mInputs;	// One input per frame -- TODO: Only the last fraem is actually used at the moment

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mFrameNumber);
		serializer.serialize(mNumFrames);

		if (serializer.isReading())
			mInputs.resize(mNumFrames);

		for (uint16& inputs : mInputs)
			serializer.serialize(inputs);
	}
};


// Join handshake (see NetplayAuth.h). Sent from the host to a newly connected friend: "prove that you know the code"
struct JoinChallengePacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("JoinChallengePacket");

	uint32 mHostNonce = 0;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mHostNonce);
	}
};


// The friend's answer: a proof (not the secret itself) that is only correct for this host nonce
struct JoinAuthPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("JoinAuthPacket");

	uint32 mClientNonce = 0;
	uint64 mProof = 0;
	uint8 mPreferredSlot = 0;	// The player slot (1 to 3) this friend had before a connection loss, or 0
	uint32 mClientToken = 0;	// Random number chosen once per run of the game: tells the host that it is the same player again (even from another address)
	uint8 mLobbyProtocol = 0;	// NetplayLobby::LOBBY_PROTOCOL of this friend; 0 = a version without the field (the host refuses it as another version)

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mClientNonce);
		serializer.serialize(mProof);
		serializer.serialize(mPreferredSlot);
		serializer.serialize(mClientToken);
		if (!serializer.isReading() || serializer.getRemaining() >= 1)
			serializer.serialize(mLobbyProtocol);
		else
			mLobbyProtocol = 0;
	}
};


// The host's verdict. If it accepted the friend, it proves with its own proof that it knows the code as well
struct JoinReplyPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("JoinReplyPacket");

	uint8 mResult = 0;			// A NetplayAuth::JoinResult
	uint8 mSlot = 0;
	uint64 mProof = 0;
	uint8 mLobbyProtocol = 0;	// NetplayLobby::LOBBY_PROTOCOL of the host; 0 = a version without the field (the friend refuses it as another version)

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mResult);
		serializer.serialize(mSlot);
		serializer.serialize(mProof);
		if (!serializer.isReading() || serializer.getRemaining() >= 1)
			serializer.serialize(mLobbyProtocol);
		else
			mLobbyProtocol = 0;
	}
};
