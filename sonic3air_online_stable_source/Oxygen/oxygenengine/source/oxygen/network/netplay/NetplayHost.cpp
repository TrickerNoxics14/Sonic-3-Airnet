/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/network/netplay/NetplayHost.h"
#include "oxygen/network/netplay/NetplayManager.h"
#include "oxygen/network/EngineServerClient.h"
#include "oxygen/application/Application.h"
#include "oxygen/application/input/ControlsIn.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/simulation/Simulation.h"
#include "oxygen/simulation/SimulationState.h"
#include <algorithm>

namespace
{
	// True if any player connection is established (slot 0 may have left while others are still waiting)
	template<typename T>
	bool anyPlayerConnected(const std::vector<T*>& connections)
	{
		for (const T* connection : connections)
		{
			if (nullptr != connection && connection->getState() == NetConnection::State::CONNECTED)
				return true;
		}
		return false;
	}
}


NetplayHost::NetplayHost(ConnectionManager& connectionManager, NetplayManager& netplayManager) :
	mConnectionManager(connectionManager),
	mNetplayManager(netplayManager)
{
}

NetplayHost::~NetplayHost()
{
	// TODO: Call this only if netplay game was actually active
	EngineMain::getDelegate().onStopNetplayGame(false);

	// Destroy all connections to clients
	for (size_t k = 0; k < mPlayerConnections.size(); ++k)
	{
		// TODO: This just destroys the connection, but doesn't tell the client about it
		mPlayerConnections[k]->clear();
		delete mPlayerConnections[k];
	}
	mPlayerConnections.clear();
}

void NetplayHost::registerAtServer()
{
	// Request game server connection
	EngineServerClient::instance().connectToServer();
	mHostState = HostState::CONNECT_TO_SERVER;
	mRestrictToKnownPeers = true;
}

NetConnection* NetplayHost::createNetConnection(const SocketAddress& senderAddress)
{
	if (mRestrictToKnownPeers)
	{
		bool known = std::find(mKnownPeerIPs.begin(), mKnownPeerIPs.end(), senderAddress.getIP()) != mKnownPeerIPs.end();
		for (size_t k = 0; k < mConnectingPlayers.size() && !known; ++k)
			known = (mConnectingPlayers[k].mRemoteAddress.getIP() == senderAddress.getIP());
		if (!known)
			known = mNetplayManager.isLanRequester(senderAddress.getIP());		// A friend on the same network found this host with the code
		if (!known)
		{
			RMX_LOG_INFO("Netplay host: refused a connection from an address the matchmaking server did not introduce");
			return nullptr;
		}
		if (std::find(mKnownPeerIPs.begin(), mKnownPeerIPs.end(), senderAddress.getIP()) == mKnownPeerIPs.end())
			mKnownPeerIPs.push_back(senderAddress.getIP());
	}

	// A dropped friend's slot must be free even while simulation is paused.
	const auto oldConnections = mPlayerConnections;
	for (PlayerConnection* connection : oldConnections)
		if (connection->getState() == NetConnection::State::DISCONNECTED || connection->getState() == NetConnection::State::EMPTY)
			destroyNetConnection(*connection);
	uint8 freeIndex = 1;
	for (; freeIndex < MAX_PLAYERS; ++freeIndex)
	{
		bool used = false;
		for (const PlayerConnection* connection : mPlayerConnections)
			used |= connection->mPlayerIndex == freeIndex;
		if (!used)
			break;
	}
	if (freeIndex == MAX_PLAYERS)
		return nullptr;
	// Remove from connecting players
	for (size_t k = 0; k < mConnectingPlayers.size(); ++k)
	{
		const ConnectingPlayer& player = mConnectingPlayers[k];
		if (player.mRemoteAddress == senderAddress)
		{
			mConnectingPlayers.erase(mConnectingPlayers.begin() + k);
			break;
		}
	}

	// Create new connection
	PlayerConnection* connection = new PlayerConnection();
	connection->mReadyForGame = false;		// Gets set once the player received the current game, see "startGame" and "updateJoinTransfer"

	connection->mPlayerIndex = freeIndex;

	mPlayerConnections.push_back(connection);
	return connection;
}

void NetplayHost::destroyNetConnection(NetConnection& connection)
{
	if (mJoiningConnection == &connection)
	{
		mJoiningConnection = nullptr;
		mJoinPhase = JoinPhase::NONE;
	}
	vectorRemoveAll(mPlayerConnections, &connection);
}

void NetplayHost::updateConnection(float deltaSeconds)
{
	EngineServerClient& engineServerClient = EngineServerClient::instance();

	switch (mHostState)
	{
		case HostState::NONE:
		{
			// Direct connections: start the game as soon as the first client is connected
			static int lastLoggedState = -1;
			if (!mPlayerConnections.empty() && (int)mPlayerConnections[0]->getState() != lastLoggedState)
			{
				lastLoggedState = (int)mPlayerConnections[0]->getState();
				RMX_LOG_INFO("Netplay host: connection state is now " << lastLoggedState);
			}
			// As long as the host is still in a menu, the friend waits; once the host started a game (e.g. picked a save file), the friend gets that game
			if (anyPlayerConnected(mPlayerConnections) && Application::instance().getSimulation().isRunning())
			{
				startGame();
			}
			break;
		}

		case HostState::CONNECT_TO_SERVER:
		{
			// Request game server connection
			engineServerClient.connectToServer();

			// Wait until the server connection is established, server features were queried, and the game socket's external address was retrieved
			if (!engineServerClient.hasReceivedServerFeatures() || mNetplayManager.getExternalAddressQuery().mOwnExternalIP.empty())
				break;

			// TODO: Why not check the server features?

			// Register at game server
			mRegistrationRequest = network::RegisterForNetplayRequest();
			mRegistrationRequest.mQuery.mIsHost = true;
			mRegistrationRequest.mQuery.mSessionID = mNetplayManager.mSessionID;
			mRegistrationRequest.mQuery.mGameSocketExternalIP = mNetplayManager.getExternalAddressQuery().mOwnExternalIP;
			mRegistrationRequest.mQuery.mGameSocketExternalPort = mNetplayManager.getExternalAddressQuery().mOwnExternalPort;
			engineServerClient.getServerConnection().sendRequest(mRegistrationRequest);

			mHostState = HostState::REGISTERED;
			break;
		}

		case HostState::REGISTERED:
		{
			// Check registration
			if (mRegistrationRequest.hasResponse())
			{
				static bool logged = false;
				if (!logged)
				{
					logged = true;
					RMX_LOG_INFO("Netplay host: game server answered, success = " << (mRegistrationRequest.mResponse.mSuccess ? 1 : 0) << ", session " << mRegistrationRequest.mResponse.mSessionID);
				}
				if (!mRegistrationRequest.mResponse.mSuccess)
				{
					mHostState = HostState::FAILED;
					break;
				}
			}

			// Same as for direct connections: the friend waits while the host is in a menu, and gets the game once the host started one
			if (anyPlayerConnected(mPlayerConnections) && Application::instance().getSimulation().isRunning())
			{
				startGame();
			}
			break;
		}

		default:
			break;
	}

	// A further player connected while the game is already running
	if (mHostState == HostState::GAME_RUNNING && mJoinPhase == JoinPhase::NONE && Application::instance().getSimulation().isRunning())
	{
		for (PlayerConnection* connection : mPlayerConnections)
		{
			if (connection->getState() == NetConnection::State::CONNECTED && !connection->mReadyForGame)
			{
				RMX_LOG_INFO("Netplay host: another player joins the running game");
				mJoiningConnection = connection;
				mJoinPhase = JoinPhase::WAITING_FOR_FRAME_BOUNDARY;
				mJoinTimer = 0.0f;
				break;
			}
		}
	}

	// Joining a game that is already running
	updateJoinTransfer(deltaSeconds);

	// Lobby
	updateLobby(deltaSeconds);

	// Update connecting players
	for (size_t k = 0; k < mConnectingPlayers.size(); ++k)
	{
		ConnectingPlayer& player = mConnectingPlayers[k];
		player.mSendTimer += deltaSeconds;
		if (player.mSendTimer < 0.1f)
			continue;

		if (player.mSentPackets < 100)
		{
			sendPunchthroughPacket(player);
		}
		else
		{
			mConnectingPlayers.erase(mConnectingPlayers.begin() + k);
			--k;
		}
	}
}

bool NetplayHost::onReceivedGameServerPacket(ReceivedPacketEvaluation& evaluation)
{
	switch (evaluation.mPacketType)
	{
		case network::ConnectToNetplayPacket::PACKET_TYPE:
		{
			network::ConnectToNetplayPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			RMX_LOG_INFO("Netplay host: game server sent a friend, type " << (int)packet.mConnectionType);

			switch (packet.mConnectionType)
			{
				case network::NetplayConnectionType::PUNCHTHROUGH:
				{
					ConnectingPlayer& player = vectorAdd(mConnectingPlayers);
					player.mRemoteAddress = SocketAddress(packet.mConnectToIP, packet.mConnectToPort);
					player.mSessionID = packet.mSessionID;

					sendPunchthroughPacket(player);
					break;
				}

				default:
					break;
			}

			return true;
		}
	}

	return false;
}

void NetplayHost::startGame()
{
	RMX_LOG_INFO("Netplay host: starting game");

	// Is a game already running? Then the client joins that game, which needs the current game state to be sent over
	if (Application::instance().getSimulation().isRunning() && !mPlayerConnections.empty())
	{
		RMX_LOG_INFO("Netplay host: a game is already running, the client will join it");
		mJoiningConnection = mPlayerConnections[0];
		mJoiningConnection->mReadyForGame = false;
		mJoinPhase = JoinPhase::WAITING_FOR_FRAME_BOUNDARY;
		mJoinTimer = 0.0f;

		mInputChecksum = rmx::startFNV1a_32();
		mRegularInputChecksum = mInputChecksum;
		mRegularChecksumFrameNumber = 0;
		mHostState = HostState::GAME_RUNNING;
		return;
	}
	// Fill in start game packet
	StartGamePacket packet;
	{
		packet.mGameBuildVersion = EngineMain::getDelegate().getAppMetaData().mBuildVersionNumber;
		packet.mTransferMode = 0;
		packet.mGameMode = 0;
		packet.mFirstFrameNumber = 0;

		// Write RNG state
		const uint64* rngState = Application::instance().getSimulation().getSimulationState().getRandomNumberGenerator().accessState();
		for (int k = 0; k < 4; ++k)
			packet.mRNGState[k] = rngState[k];

		// Save game settings
		VectorBinarySerializer serializer(false, packet.mSerializedGameSettings);
		EngineMain::getDelegate().serializeGameSettings(serializer);

		const std::string modsInfo = NetplayManager::getActiveModsInfo();
		packet.mActiveModsInfo.assign(modsInfo.begin(), modsInfo.end());
	}

	// Send to players
	for (PlayerConnection* connection : mPlayerConnections)
	{
		connection->mReadyForGame = true;
		packet.mPlayerIndex = connection->mPlayerIndex;
		connection->sendPacket(packet, NetConnection::SendFlags::NONE, &connection->mStartGamePacketID);
	}

	// Setup input checksum tracking
	mInputChecksum = rmx::startFNV1a_32();
	mRegularInputChecksum = mInputChecksum;
	mRegularChecksumFrameNumber = 0;

	mHostState = HostState::GAME_RUNNING;

	EngineMain::getDelegate().onStartNetplayGame(true);
}

bool NetplayHost::canBeginNextFrame(uint32 frameNumber)
{
	// The frame number goes back to the start when a new game gets started (e.g. the host picked a save in the menu): the clients need the new game then
	if (frameNumber < mLastSeenFrameNumber)
	{
		for (PlayerConnection* connection : mPlayerConnections)
		{
			if (connection->getState() == NetConnection::State::CONNECTED && connection->mReadyForGame && mJoinPhase == JoinPhase::NONE)
			{
				RMX_LOG_INFO("Netplay host: a new game was started, sending it to the client");
				connection->mReadyForGame = false;
			}
		}
	}
	mLastSeenFrameNumber = frameNumber;

	switch (mJoinPhase)
	{
		case JoinPhase::WAITING_FOR_FRAME_BOUNDARY:
			// This is the moment between two frames: the game state can be saved consistently now
			beginJoinTransfer(frameNumber);
			return false;

		case JoinPhase::TRANSFERRING:
			// The game stays paused until the client confirmed that it loaded the state
			return false;

		default:
			break;
	}

	// Don't run too far ahead of a client that lags behind: the game would drift apart if the client misses too many frames
	const uint32 MAX_LEAD = 22;	// Must stay below the 30 input frames that each packet carries
	bool mustWait = false;
	for (const PlayerConnection* connection : mPlayerConnections)
	{
		if (connection->getState() == NetConnection::State::CONNECTED && connection->mReadyForGame && frameNumber > connection->mLastReceivedFrameNumber + MAX_LEAD)
			mustWait = true;
	}
	if (!mustWait)
	{
		mStallStartTicks = 0;
		return true;
	}

	// Give up waiting after a while (a repair gets triggered by the fingerprint check if the client came back out of sync)
	if (mStallStartTicks == 0)
		mStallStartTicks = std::max<uint32>(SDL_GetTicks(), 1);
	if (SDL_GetTicks() - mStallStartTicks > 8000)
		return true;
	return false;
}

void NetplayHost::beginJoinTransfer(uint32 frameNumber)
{
	Simulation& simulation = Application::instance().getSimulation();
	ControlsIn& controlsIn = ControlsIn::instance();

	std::vector<uint8> state;
	if (!simulation.saveStateToBuffer(state) || !ZlibDeflate::encode(mJoinCompressedState, state.data(), state.size(), 6))
	{
		RMX_LOG_INFO("Netplay host: failed to save the game state for the joining player");
		abortJoinTransfer();
		return;
	}

	mJoinUncompressedSize = (uint32)state.size();
	mJoinFrameNumber = frameNumber;
	mJoinSentOffset = 0;
	mJoinTimer = 0.0f;
	for (int k = 0; k < 4; ++k)
		mJoinLastInputs[k] = controlsIn.getGamepad(k).mCurrentInput;

	// Fill in start game packet; the actual state follows in chunks
	StartGamePacket packet;
	packet.mGameBuildVersion = EngineMain::getDelegate().getAppMetaData().mBuildVersionNumber;
	packet.mTransferMode = 1;
	packet.mGameMode = 0;
	packet.mFirstFrameNumber = frameNumber;
	const uint64* rngState = simulation.getSimulationState().getRandomNumberGenerator().accessState();
	for (int k = 0; k < 4; ++k)
		packet.mRNGState[k] = rngState[k];
	VectorBinarySerializer serializer(false, packet.mSerializedGameSettings);
	EngineMain::getDelegate().serializeGameSettings(serializer);
	const std::string modsInfo = NetplayManager::getActiveModsInfo();
	packet.mActiveModsInfo.assign(modsInfo.begin(), modsInfo.end());

	packet.mPlayerIndex = mJoiningConnection->mPlayerIndex;
	mJoiningConnection->sendPacket(packet, NetConnection::SendFlags::NONE, &mJoiningConnection->mStartGamePacketID);

	mJoinPhase = JoinPhase::TRANSFERRING;
	RMX_LOG_INFO("Netplay host: sending game state at frame " << frameNumber << " (" << mJoinUncompressedSize << " bytes, " << mJoinCompressedState.size() << " compressed)");
}

void NetplayHost::updateJoinTransfer(float deltaSeconds)
{
	if (mJoinPhase == JoinPhase::NONE)
		return;

	mJoinTimer += deltaSeconds;
	if (mJoinTimer > 30.0f || nullptr == mJoiningConnection || mJoiningConnection->getState() != NetConnection::State::CONNECTED)
	{
		RMX_LOG_INFO("Netplay host: joining player did not receive the game state, aborting");
		abortJoinTransfer();
		return;
	}

	if (mJoinPhase != JoinPhase::TRANSFERRING)
		return;

	// Send some more chunks (not all at once, to not flood the connection)
	const uint32 CHUNK_SIZE = 1000;
	for (int count = 0; count < 24 && mJoinSentOffset < mJoinCompressedState.size(); ++count)
	{
		const uint32 size = std::min<uint32>(CHUNK_SIZE, (uint32)mJoinCompressedState.size() - mJoinSentOffset);

		JoinStateChunkPacket chunk;
		chunk.mFrameNumber = mJoinFrameNumber;
		chunk.mCompressedSize = (uint32)mJoinCompressedState.size();
		chunk.mUncompressedSize = mJoinUncompressedSize;
		chunk.mOffset = mJoinSentOffset;
		for (int k = 0; k < 4; ++k)
			chunk.mLastInputs[k] = mJoinLastInputs[k];
		chunk.mScreenWidth = (uint16)VideoOut::instance().getScreenWidth();
		chunk.mScreenHeight = (uint16)VideoOut::instance().getScreenHeight();
		chunk.mData.assign(mJoinCompressedState.begin() + mJoinSentOffset, mJoinCompressedState.begin() + mJoinSentOffset + size);

		mJoiningConnection->sendPacket(chunk, NetConnection::SendFlags::NONE);
		mJoinSentOffset += size;
	}
}

void NetplayHost::abortJoinTransfer()
{
	if (nullptr != mJoiningConnection)
	{
		PlayerConnection* connection = mJoiningConnection;
		mJoiningConnection = nullptr;
		connection->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
	}
	mJoinPhase = JoinPhase::NONE;
	mJoinCompressedState.clear();
	mJoinCompressedState.shrink_to_fit();
}

void NetplayHost::onFrameUpdate(ControlsIn& controlsIn, uint32 frameNumber)
{
	if (mHostState != HostState::GAME_RUNNING)
		return;

	std::vector<PlayerConnection*> activeConnections;
	{
		// First filter out connections that became invalid
		std::vector<PlayerConnection*> connectionsToRemove;
		for (PlayerConnection* playerConnection : mPlayerConnections)
		{
			if (playerConnection->getState() == NetConnection::State::CONNECTED && playerConnection->mReadyForGame)
			{
				activeConnections.push_back(playerConnection);
			}
			else if (playerConnection->getState() == NetConnection::State::DISCONNECTED || playerConnection->getState() == NetConnection::State::EMPTY)
			{
				connectionsToRemove.push_back(playerConnection);
			}
		}
		for (PlayerConnection* connection : connectionsToRemove)
		{
			destroyNetConnection(*connection);
		}
	}

	// Add new frame to the history
	{
		mInputHistory.emplace_back();
		InputFrame& newInputFrame = mInputHistory.back();

		// Apply host's local input (local player 1 only)
		newInputFrame.mInputsByPlayer[0] = controlsIn.getInputFromController(0);

		// Apply last received input from the clients
		for (PlayerConnection* playerConnection : activeConnections)
		{
			if (playerConnection->mPlayerIndex >= 0 && playerConnection->mPlayerIndex < MAX_PLAYERS)
			{
				const uint16 input = playerConnection->mLastReceivedInput;
				newInputFrame.mInputsByPlayer[playerConnection->mPlayerIndex] = input;
			}
		}

		// Checksum for debugging
		mInputChecksum = rmx::addToFNV1a_32(mInputChecksum, reinterpret_cast<uint8*>(newInputFrame.mInputsByPlayer), sizeof(newInputFrame.mInputsByPlayer));
		if (frameNumber % 200 == 0)
		{
			mRegularInputChecksum = mInputChecksum;
			mRegularChecksumFrameNumber = frameNumber;
		}

		// Limit inputs history to a reasonable number of frames
		while (mInputHistory.size() > 30)
			mInputHistory.pop_front();
	}

	// Apply input locally
	{
		const InputFrame& inputFrame = mInputHistory.back();
		controlsIn.injectInputs(inputFrame.mInputsByPlayer, MAX_PLAYERS);
	}

	if (activeConnections.empty())
		return;

	// Regularly tell the clients what the game state looks like, so they notice when their game drifted away
	if ((frameNumber % 60) == 0 && mJoinPhase == JoinPhase::NONE)
	{
		SyncCheckPacket syncPacket;
		syncPacket.mFrameNumber = frameNumber;
		syncPacket.mHash = NetplayManager::computeStateHash(frameNumber);
		for (PlayerConnection* playerConnection : activeConnections)
			playerConnection->sendPacket(syncPacket, NetConnection::SendFlags::NONE);
	}

	// Preserve slot numbers when a middle player disconnects; compacting the
	// packet would discard player four's input after player two/three leaves.
	const size_t numPlayers = MAX_PLAYERS;
	const size_t numFrames = std::min<size_t>(mInputHistory.size(), 30);

	// Build packet to send to the clients
	GameStateIncrementPacket packet;
	packet.mGameplayState = 1;			// TODO: Set this to some other value during paused game
	packet.mNumPlayers = (uint8)numPlayers;
	packet.mNumFrames = (uint8)numFrames;
	packet.mFrameNumber = frameNumber;
	packet.mInputs.resize(packet.mNumPlayers * packet.mNumFrames);

	// Copy input data from players
	{
		int reverseFrameCount = 1;
		for (auto it = mInputHistory.rbegin(); it != mInputHistory.rend(); ++it)
		{
			uint16* input = &packet.mInputs[(numFrames - reverseFrameCount) * numPlayers];
			for (int playerIndex = 0; playerIndex < (int)numPlayers; ++playerIndex)
			{
				*input = it->mInputsByPlayer[playerIndex];
				++input;
			}
			++reverseFrameCount;
		}
	}

	for (PlayerConnection* playerConnection : activeConnections)
	{
		packet.mLastClientReceivedFrame = playerConnection->mLastReceivedFrameNumber;
		playerConnection->mCurrentLatency = frameNumber - playerConnection->mLastReceivedFrameNumber;

		playerConnection->sendPacket(packet, NetConnection::SendFlags::UNRELIABLE);
	}
}

void NetplayHost::getLobbySlots(uint8 outStates[MAX_PLAYERS]) const
{
	for (int k = 0; k < MAX_PLAYERS; ++k)
		outStates[k] = 0;
	outStates[0] = 1;
	for (const PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr == connection || connection->getState() != NetConnection::State::CONNECTED)
			continue;
		if (connection->mPlayerIndex >= 1 && connection->mPlayerIndex < MAX_PLAYERS)
			outStates[connection->mPlayerIndex] = connection->mLobbyReady ? 3 : 2;
	}
}

int NetplayHost::getConnectedClientCount() const
{
	int count = 0;
	for (const PlayerConnection* connection : mPlayerConnections)
		count += (nullptr != connection && connection->getState() == NetConnection::State::CONNECTED) ? 1 : 0;
	return count;
}

bool NetplayHost::areAllClientsReady() const
{
	int count = 0;
	for (const PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr == connection || connection->getState() != NetConnection::State::CONNECTED)
			continue;
		if (!connection->mLobbyReady)
			return false;
		++count;
	}
	return count > 0;
}

void NetplayHost::setLobbyHostStarting(bool starting)
{
	if (mLobbyHostStarting != starting)
	{
		mLobbyHostStarting = starting;
		mLobbyLastSignature = 0xffffffff;		// Tell everybody right away
	}
}

void NetplayHost::updateLobby(float deltaSeconds)
{
	uint8 states[MAX_PLAYERS];
	getLobbySlots(states);

	uint32 signature = mLobbyHostStarting ? 0x100u : 0u;
	for (int k = 0; k < MAX_PLAYERS; ++k)
		signature = signature * 4 + states[k];

	const bool allReady = areAllClientsReady();
	if (allReady != mLobbyWasAllReady)
	{
		mLobbyWasAllReady = allReady;
		RMX_LOG_INFO("Netplay host: lobby: " << (allReady ? "all players are ready" : "not everybody is ready"));
	}

	// Repeated once per second while the lobby is open (a lost packet must not leave a friend with an old picture); once the game runs, only changes are sent
	mLobbyBroadcastTimer += deltaSeconds;
	const bool repeat = (mHostState != HostState::GAME_RUNNING) && (mLobbyBroadcastTimer >= 1.0f);
	if (signature == mLobbyLastSignature && !repeat)
		return;
	mLobbyLastSignature = signature;
	mLobbyBroadcastTimer = 0.0f;

	for (PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr == connection || connection->getState() != NetConnection::State::CONNECTED)
			continue;
		LobbyStatePacket packet;
		packet.mYourSlot = connection->mPlayerIndex;
		for (int k = 0; k < MAX_PLAYERS; ++k)
			packet.mSlotState[k] = states[k];
		packet.mHostStarting = mLobbyHostStarting ? 1 : 0;
		connection->sendPacket(packet, NetConnection::SendFlags::NONE);
	}
}

bool NetplayHost::onReceivedPacket(ReceivedPacketEvaluation& evaluation)
{
	switch (evaluation.mPacketType)
	{
		case LobbyReadyPacket::PACKET_TYPE:
		{
			LobbyReadyPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			connection.mLobbyReady = (packet.mReady != 0);
			RMX_LOG_INFO("Netplay host: lobby: player " << (int)(connection.mPlayerIndex + 1) << (connection.mLobbyReady ? " is ready" : " is not ready"));
			return true;
		}

		case ResyncRequestPacket::PACKET_TYPE:
		{
			ResyncRequestPacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			// The client's game drifted away: send it the game state again (handled like a player that joins a running game)
			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			if (connection.mReadyForGame && mJoinPhase == JoinPhase::NONE)
			{
				RMX_LOG_INFO("Netplay host: client's game drifted away at frame " << packet.mFrameNumber << ", sending the game state again");
				connection.mReadyForGame = false;
				++mResyncCount;
			}
			return true;
		}

		case JoinStateLoadedPacket::PACKET_TYPE:
		{
			JoinStateLoadedPacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			if (mJoinPhase == JoinPhase::TRANSFERRING && &connection == mJoiningConnection && packet.mFrameNumber == mJoinFrameNumber)
			{
				RMX_LOG_INFO("Netplay host: client loaded the game state, resuming at frame " << mJoinFrameNumber);
				connection.mReadyForGame = true;
				connection.mLastReceivedFrameNumber = (mJoinFrameNumber > 0) ? (mJoinFrameNumber - 1) : 0;
				connection.mLastReceivedInput = 0;
				mJoiningConnection = nullptr;
				mJoinPhase = JoinPhase::NONE;
				mJoinCompressedState.clear();
				mJoinCompressedState.shrink_to_fit();

				// Don't let the game rush ahead to make up for the time it was paused
				Application::instance().getSimulation().resetTargetFrameToCurrent();
			}
			return true;
		}

		case PlayerInputIncrementPacket::PACKET_TYPE:
		{
			PlayerInputIncrementPacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			if (!packet.mInputs.empty())
			{
				PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);

				if (packet.mFrameNumber > connection.mLastReceivedFrameNumber)		// Ignore out-dated packets
				{
					connection.mLastReceivedFrameNumber = packet.mFrameNumber;
					if (connection.mLastReceivedInput != packet.mInputs.back() && nullptr != getenv("S3AIR_NETPLAY_DEBUG"))
						RMX_LOG_INFO("Netplay host: player " << (int)(connection.mPlayerIndex + 1) << " input changed to " << rmx::hexString(packet.mInputs.back(), 4) << " at frame " << packet.mFrameNumber);
					connection.mLastReceivedInput = packet.mInputs.back();
				}
			}

			return true;
		}
	}

	return false;
}

uint32 NetplayHost::getRegularInputChecksum(int& outFrameNumber) const
{
	outFrameNumber = mRegularChecksumFrameNumber;
	return mRegularInputChecksum;
}

void NetplayHost::sendPunchthroughPacket(ConnectingPlayer& player)
{
	network::PunchthroughConnectionlessPacket punchthroughPacket;
	punchthroughPacket.mQueryID = (uint32)player.mSessionID;
	punchthroughPacket.mSenderReceivedPackets = false;

	mConnectionManager.sendConnectionlessLowLevelPacket(punchthroughPacket, player.mRemoteAddress, 0, 0);

	++player.mSentPackets;
	player.mSendTimer = 0.0f;
}
