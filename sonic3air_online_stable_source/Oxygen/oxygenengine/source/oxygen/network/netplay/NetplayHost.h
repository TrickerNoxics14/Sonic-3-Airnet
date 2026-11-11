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
#include "oxygen/network/netplay/NetplayLobbyLogic.h"

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
		uint32 mClientToken = 0;		// From the join message, see JoinAuthPacket
		bool mLobbyAnnounced = false;	// The first lobby message was logged
		bool mLeaving = false;			// The host sent this friend away (kick, not ready): it gets disconnected a moment later
		float mLeaveTimer = 0.0f;
		std::string mName = NetplayLobby::DEFAULT_NAME;		// Cleaned up by the host, see NetplayLobby::sanitizeName
		NetplayLobby::PingTracker mPing;
		uint32 mPingToken = 0;			// The token of the ping that is waiting for its answer (0 = none)
		uint32 mPingSentTicks = 0;
		float mHighPingLogTimer = 0.0f;
		NetplayLobby::ChatRateLimiter mChatLimiter;
		inline bool isActive() const  { return mAuthenticated && !mLeaving && getState() == NetConnection::State::CONNECTED; }
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

	struct ChatLine
	{
		int mSlot = 0;
		std::string mText;
	};

	// Lobby: players gather (and press Ready) before the host starts the game
	void getLobbySlots(uint8 outStates[MAX_PLAYERS]) const;		// 0 = empty, 1 = host, 2 = joined, 3 = ready
	int getConnectedClientCount() const;
	bool areAllClientsReady() const;							// At least one client, and every connected client is ready
	void setLobbyHostStarting(bool starting);

	// Names and ping (slot 0 is the host)
	std::string getPlayerName(int slot) const;			// Empty for an empty slot
	int getPlayerPing(int slot) const;					// Round trip time in milliseconds, -1 = not measured (yet); the host itself has 0
	int getWorstPing() const;							// Of the connected friends, -1 if none is measured
	int getHighPingSlot(int& outPingMs) const;			// The first friend above NetplayLobby::HIGH_PING_MS, or -1

	// Host controls
	bool kickPlayer(int slot);							// Sends a friend away; its address cannot come back for a minute
	void sendHostChat(int quickMessage, const std::string& freeText);
	inline const std::deque<ChatLine>& getChatLog() const  { return mChatLog; }

	// Input delay: -1 = Auto, otherwise a number of frames (see NetplayLobbyLogic.h)
	void setInputDelaySetting(int setting);
	inline int getInputDelaySetting() const  { return mDelaySetting; }
	inline int getEffectiveInputDelay() const  { return mEffectiveDelay; }

	// Ready check: the host wants to start, friends who are not ready get a prompt and a time limit; whoever is not ready when it runs out is left out
	void beginReadyCheck();
	void finishReadyCheckNow();							// The host does not wait any longer
	inline bool isReadyCheckActive() const  { return mReadyCheckActive; }
	int getReadyCheckSecondsLeft() const;
	bool takeReadyCheckFinished();						// True once after a ready check ended: the host can leave the lobby to pick the game

private:
	void sendPunchthroughPacket(ConnectingPlayer& player);
	void handleJoinAuth(PlayerConnection& connection, const JoinAuthPacket& packet);
	static std::string identityOf(const PlayerConnection& connection);		// Who this player is: its token, or its address if it sent none
	void updateHandshakes(float deltaSeconds);
	void retireConnection(PlayerConnection* connection);	// Takes it out of the list; it gets deleted a bit later, when no queued packet can refer to it any more
	void refuseConnection(PlayerConnection& connection, uint8 result);
	void beginJoinTransfer(uint32 frameNumber);
	void updateJoinTransfer(float deltaSeconds);
	void updateLobby(float deltaSeconds);
	void updatePings(float deltaSeconds);
	void updateAutoDelay(float deltaSeconds, bool force);
	void sendLeave(PlayerConnection& connection, NetplayLobby::LeaveReason reason);
	void addChatLine(int slot, const std::string& text);
	void finishReadyCheck(bool timedOut);
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
	std::string mLobbyLastKey;
	std::deque<ChatLine> mChatLog;
	NetplayLobby::SlotReservations mReservations;
	NetplayLobby::KickBlockList mKickBlocked;

	// Ping, input delay
	float mPingTimer = 0.0f;
	float mPingLogTimer = 0.0f;
	uint32 mPingCounter = 0;
	int mDelaySetting = NetplayLobby::INPUT_DELAY_AUTO;
	int mEffectiveDelay = 0;
	float mAutoDelayTimer = 0.0f;
	NetplayLobby::InputDelayLine mDelayLine;
	bool mDelayLogEnabled = false;						// Developer test: log when a friend's input changes, before and after the delay (to measure the delay)
	uint16 mDelayLogRaw[MAX_PLAYERS] = { 0 };
	uint16 mDelayLogUsed[MAX_PLAYERS] = { 0 };

	// Ready check
	bool mReadyCheckActive = false;
	bool mReadyCheckFinished = false;
	float mReadyCheckRemaining = 0.0f;
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
