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
#include "oxygen/network/netplay/NetplayFailure.h"

class ControlsIn;
class NetplayManager;
struct StartGamePacket;
struct JoinStateChunkPacket;


class NetplayClient
{
public:
	static inline const int MAX_PLAYERS = 4;	// Needs to be inline, or the Android build fails for some reason

	enum class State
	{
		NONE,				// Not started anything yet
		CONNECT_TO_SERVER,	// Waiting for a game server connection
		REGISTERED,			// Sent registration to game server, now waiting for a "ConnectToNetplayPacket"
		CONNECT_TO_HOST,	// Waiting for connection to host
		CONNECTED,			// Connection to host established
		GAME_RUNNING,		// Game running
		FAILED
	};

	struct HostConnection : public NetConnection
	{
		// Nothing here yet
	};

public:
	NetplayClient(ConnectionManager& connectionManager, NetplayManager& netplayManager);
	~NetplayClient();

	inline State getState() const											{ return mState; }
	inline uint8 getPlayerIndex() const { return mPlayerIndex; }
	inline const HostConnection& getHostConnection() const					{ return mHostConnection; }
	inline const SocketAddress& getReceivedPunchthroughPacketSender() const	{ return mReceivedPunchthroughPacketSender; }
	inline int getCurrentLatency() const									{ return mCurrentLatency; }
	inline int getDesyncCount() const										{ return mDesyncCount; }
	inline uint32 getLastSyncOkFrame() const								{ return mLastSyncOkFrame; }
	inline bool isReceivingGameState() const								{ return mReceivingJoinState; }

	// The secret from the code that the player typed (see NetplayAuth.h): without it the host does not let us in, and without the host's proof we do not trust it
	inline void setAuthSecret(uint32 secret)  { mAuthSecret = secret; }
	inline bool isHostVerified() const  { return mHostVerified; }

	void joinViaServer();
	void connectDirectlyToHost(std::string_view ip, uint16 port);

	void updateConnection(float deltaSeconds);
	bool onReceivedGameServerPacket(ReceivedPacketEvaluation& evaluation);

	bool canBeginNextFrame(uint32 frameNumber);
	void onFrameUpdate(ControlsIn& controlsIn, uint32 frameNumber);
	bool onReceivedPacket(ReceivedPacketEvaluation& evaluation);
	bool onReceivedConnectionlessPacket(ConnectionlessPacketEvaluation& evaluation);

	uint32 getRegularInputChecksum(int& outFrameNumber) const;

	// Lobby: what the host last told us about who is in the lobby
	inline bool hasLobbyState() const		{ return mLobbyKnown; }
	inline uint8 getLobbyYourSlot() const	{ return mLobbyYourSlot; }
	inline uint8 getLobbySlotState(int slot) const  { return (slot >= 0 && slot < 4) ? mLobbySlotState[slot] : 0; }
	inline bool isLobbyHostStarting() const	{ return mLobbyHostStarting; }
	inline bool isLobbyReady() const		{ return mLobbyMyReady; }
	void sendLobbyReady(bool ready);

private:
	struct ReceivedFrame
	{
		uint16 mInputsByPlayer[MAX_PLAYERS] = { 0 };
	};

private:
	void startGame(const StartGamePacket& packet);
	void beginConnectTo(const SocketAddress& address);
	void fail(NetplayFailure::Reason reason, const std::string& detail = std::string());
	void handleJoinChallenge(uint32 hostNonce);
	void handleJoinReply(const struct JoinReplyPacket& packet);
	void onReceivedJoinStateChunk(const JoinStateChunkPacket& packet);
	void finishJoinTransfer();

private:
	ConnectionManager& mConnectionManager;
	NetplayManager& mNetplayManager;
	HostConnection mHostConnection;
	State mState = State::NONE;
	State mTimedState = State::NONE;
	float mStateTimer = 0.0f;
	float mReconnectRemaining = 0.0f;
	SocketAddress mReconnectAddress;
	bool mResumeSession = false;
	bool mReconnectTestTriggered = false;
	uint8 mPlayerIndex = 1;

	// Handshake with the host
	uint32 mAuthSecret = 0;
	bool mHostVerified = false;		// The host answered the challenge correctly (and let us in): only then is anything it says believed
	bool mAuthSent = false;
	uint32 mHostNonce = 0;
	uint32 mClientNonce = 0;
	float mHeartbeatTimer = 0.0f;

	network::RegisterForNetplayRequest mRegistrationRequest;
	SocketAddress mReceivedPunchthroughPacketSender;

	std::deque<ReceivedFrame> mReceivedFrames;
	uint32 mNextFrameNumber = 0;
	int mCurrentLatency = 0;

	uint32 mInputChecksum = 0;
	uint32 mRegularInputChecksum = 0;
	int mRegularChecksumFrameNumber = 0;

	// Checking that our game is still identical to the host's game
	std::map<uint32, uint32> mHostStateHashes;
	std::map<uint32, uint32> mOwnStateHashes;
	bool mAwaitingRepair = false;
	uint8 mLobbyYourSlot = 0;
	uint8 mLobbySlotState[4] = { 0 };
	bool mLobbyHostStarting = false;
	bool mLobbyKnown = false;
	bool mLobbyMyReady = false;
	uint32 mRepairRequestedFrame = 0;
	int mDesyncCount = 0;
	uint32 mLastSyncOkFrame = 0;

	// Joining a game that is already running: receiving its game state
	bool mReceivingJoinState = false;
	std::vector<uint8> mJoinCompressedState;
	std::set<uint32> mJoinReceivedChunkOffsets;
	uint32 mJoinReceivedBytes = 0;
	uint32 mJoinUncompressedSize = 0;
	uint32 mJoinFrameNumber = 0;
	uint16 mJoinLastInputs[4] = { 0 };
	uint16 mJoinScreenWidth = 0;
	uint16 mJoinScreenHeight = 0;
};
