/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen_netcore/network/ConnectionListener.h"
#include "oxygen_netcore/network/NetConnection.h"
#include "oxygen_netcore/serverclient/NetplaySetupPackets.h"

#include "oxygen/network/netplay/NetplayPackets.h"

class ControlsIn;
class NetplayManager;


class NetplayHost
{
public:
	static const int MAX_PLAYERS = 4;

	enum class HostState
	{
		NONE,				// Idle, but reacting to direct connections
		CONNECT_TO_SERVER,	// Waiting for a game server connection
		REGISTERED,			// Sent registration to game server, now waiting for a client to join with a "ConnectToNetplayPacket"
		GAME_RUNNING,		// Game is running
		FAILED
	};

	struct ConnectingPlayer
	{
		SocketAddress mRemoteAddress;
		uint64 mSessionID = 0;
		int mSentPackets = 0;
		float mSendTimer = 0.0f;
	};

	enum class JoinPhase
	{
		NONE,
		WAITING_FOR_FRAME_BOUNDARY,	// A client joins a game that is already running; the game state gets saved at the beginning of the next frame
		TRANSFERRING				// Sending the game state to the client, the game is paused meanwhile
	};

	struct PlayerConnection : public NetConnection
	{
		bool mReadyForGame = true;	// False while a client is still receiving the game state of an already running game
		bool mLobbyReady = false;	// The player pressed Ready in the lobby
		bool mAuthenticated = false;	// The friend proved that it knows the code (see NetplayAuth.h); until then it is not a player and all its packets are ignored
		bool mChallengeSent = false;
		uint32 mHostNonce = 0;
		float mPendingSeconds = 0.0f;	// Time spent without being authenticated
		float mRefuseTimer = -1.0f;		// 0 or more: the friend got a refusal and gets disconnected soon
		uint32 mLastResyncTicks = 0;
		uint8 mPlayerIndex = 0xff;		// Assigned once the friend is authenticated
		inline bool isActive() const  { return mAuthenticated && getState() == NetConnection::State::CONNECTED; }
		uint32 mStartGamePacketID = 0;
		uint32 mLastReceivedFrameNumber = 0;
		uint16 mLastReceivedInput = 0;
		int mCurrentLatency = 0;
	};

	struct InputFrame
	{
		uint16 mInputsByPlayer[MAX_PLAYERS] = { 0 };
	};

public:
	NetplayHost(ConnectionManager& connectionManager, NetplayManager& netplayManager);
	~NetplayHost();

	inline HostState getHostState() const  { return mHostState; }
	inline const std::vector<PlayerConnection*>& getPlayerConnections() const  { return mPlayerConnections; }
	int getAuthenticatedCount() const;		// Friends who are connected and proved that they know the code

	void registerAtServer();

	NetConnection* createNetConnection(const SocketAddress& senderAddress);
	void destroyNetConnection(NetConnection& connection);

	void updateConnection(float deltaSeconds);
	bool onReceivedGameServerPacket(ReceivedPacketEvaluation& evaluation);

	void startGame();

	inline int getResyncCount() const  { return mResyncCount; }
	inline bool isRegisteredAtServer() const  { return mRegistrationRequest.hasResponse() && mRegistrationRequest.mResponse.mSuccess; }

	bool canBeginNextFrame(uint32 frameNumber);
	void onFrameUpdate(ControlsIn& controlsIn, uint32 frameNumber);
	bool onReceivedPacket(ReceivedPacketEvaluation& evaluation);

	uint32 getRegularInputChecksum(int& outFrameNumber) const;

	// Lobby: players gather (and press Ready) before the host starts the game
	void getLobbySlots(uint8 outStates[MAX_PLAYERS]) const;		// 0 = empty, 1 = host, 2 = joined, 3 = ready
	int getConnectedClientCount() const;
	bool areAllClientsReady() const;							// At least one client, and every connected client is ready
	void setLobbyHostStarting(bool starting);

private:
	void sendPunchthroughPacket(ConnectingPlayer& player);
	void handleJoinAuth(PlayerConnection& connection, const JoinAuthPacket& packet);
	void updateHandshakes(float deltaSeconds);
	void retireConnection(PlayerConnection* connection);	// Takes it out of the list; it gets deleted a bit later, when no queued packet can refer to it any more
	void refuseConnection(PlayerConnection& connection, uint8 result);
	void beginJoinTransfer(uint32 frameNumber);
	void updateJoinTransfer(float deltaSeconds);
	void updateLobby(float deltaSeconds);
	void abortJoinTransfer();

private:
	ConnectionManager& mConnectionManager;
	NetplayManager& mNetplayManager;
	HostState mHostState = HostState::NONE;

	network::RegisterForNetplayRequest mRegistrationRequest;

	std::vector<ConnectingPlayer> mConnectingPlayers;

	// When hosting through the matchmaking server, only addresses the server introduced (and addresses that already connected before) may connect.
	// Without this, anyone who can reach the UDP port could take a player slot and receive the game state.
	bool mRestrictToKnownPeers = false;
	std::vector<std::string> mKnownPeerIPs;
	std::vector<PlayerConnection*> mPlayerConnections;
	std::vector<PlayerConnection*> mRetiredConnections;
	std::map<std::string, int> mFailedAuthByIP;		// Wrong codes per address: after a few, that address gets ignored
	int mMaxClients = MAX_PLAYERS - 1;
	float mHostStateTimer = 0.0f;
	HostState mTimedHostState = HostState::NONE;
	float mLobbyBroadcastTimer = 0.0f;
	uint32 mLobbyLastSignature = 0xffffffff;
	bool mLobbyHostStarting = false;
	bool mLobbyWasAllReady = false;
	std::deque<InputFrame> mInputHistory;

	uint32 mInputChecksum = 0;
	uint32 mRegularInputChecksum = 0;
	int mRegularChecksumFrameNumber = 0;

	uint32 mLastSeenFrameNumber = 0;
	int mResyncCount = 0;				// How often a client got sent a fresh game state because its game drifted away
	uint32 mStallStartTicks = 0;		// While waiting for a client that lags behind

	// Joining a game that is already running
	JoinPhase mJoinPhase = JoinPhase::NONE;
	PlayerConnection* mJoiningConnection = nullptr;
	std::vector<uint8> mJoinCompressedState;
	uint32 mJoinUncompressedSize = 0;
	uint32 mJoinFrameNumber = 0;
	uint32 mJoinSentOffset = 0;
	uint16 mJoinLastInputs[4] = { 0 };
	float mJoinTimer = 0.0f;
};
