/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen_netcore/network/HighLevelPacketBase.h"


// Sent from host to clients
struct StartGamePacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("StartGamePacket");

	static const uint8 PACKET_VERSION = 3;

	uint8 mPacketVersion = PACKET_VERSION;
	uint32 mGameBuildVersion = 0;
	uint8 mTransferMode = 0;
	uint8 mGameMode = 0;
	uint8 mPlayerIndex = 1;
	uint32 mFirstFrameNumber = 0;
	uint64 mRNGState[4] = { 0 };

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

		serializer.serializeData(mSerializedGameSettings);
		serializer.serializeData(mActiveModsInfo);
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
		serializer.serializeData(mData);
	}
};


// Lobby (before the game starts): the host tells every client who is in the lobby and who is ready
struct LobbyStatePacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyStatePacket");

	uint8 mYourSlot = 0;				// The slot (1 to 3) of the client who receives this packet
	uint8 mSlotState[4] = { 0 };		// Per slot: 0 = empty, 1 = host, 2 = joined, 3 = ready
	uint8 mHostStarting = 0;			// 1 once the host leaves the lobby screen to pick the game

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mYourSlot);
		for (int k = 0; k < 4; ++k)
			serializer.serialize(mSlotState[k]);
		serializer.serialize(mHostStarting);
	}
};


// Sent from a client to the host: the player pressed Ready (or took it back)
struct LobbyReadyPacket : public highlevel::PacketBase
{
	HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("LobbyReadyPacket");

	uint8 mReady = 0;

	virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override
	{
		serializer.serialize(mReady);
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
