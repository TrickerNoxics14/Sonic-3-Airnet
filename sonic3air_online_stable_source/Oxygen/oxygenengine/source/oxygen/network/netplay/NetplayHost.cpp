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
#include "oxygen/network/netplay/NetplayAuth.h"
#include "oxygen/network/EngineServerClient.h"
#include "oxygen/application/Application.h"
#include "oxygen/application/input/ControlsIn.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/simulation/Simulation.h"
#include "oxygen/simulation/SimulationState.h"
#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen_netcore/network/RelayProtocol.h"
#include <algorithm>
#include <cmath>

namespace
{
	// True if any friend is connected and authenticated (the first one may have left while others are still waiting)
	template<typename T>
	bool anyPlayerConnected(const std::vector<T*>& connections)
	{
		for (const T* connection : connections)
		{
			if (nullptr != connection && connection->isActive())
				return true;
		}
		return false;
	}
}


NetplayHost::NetplayHost(ConnectionManager& connectionManager, NetplayManager& netplayManager) :
	mConnectionManager(connectionManager),
	mNetplayManager(netplayManager)
{
	// Developer test: fewer free slots, to see what a friend gets told when the lobby is full
	if (const char* maxClients = TestHooks::getEnv("S3AIR_NETPLAY_MAXCLIENTS"))
		mMaxClients = std::max(0, std::min(MAX_PLAYERS - 1, atoi(maxClients)));

	mDelaySetting = NetplayLobby::clampDelaySetting(mNetplayManager.getInputDelaySetting());
	mDelayLogEnabled = (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DELAYLOG"));
}

NetplayHost::~NetplayHost()
{
	// TODO: Call this only if netplay game was actually active
	EngineMain::getDelegate().onStopNetplayGame(false);

	// Destroy all connections to clients
	for (size_t k = 0; k < mPlayerConnections.size(); ++k)
	{
		// Tell the friend that the host is gone (so the lobby can say so right away instead of waiting for a timeout)
		mPlayerConnections[k]->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
		delete mPlayerConnections[k];
	}
	mPlayerConnections.clear();
	for (PlayerConnection* connection : mRetiredConnections)
		delete connection;
	mRetiredConnections.clear();
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
	if (RelayProtocol::isVirtualIP(senderAddress.getIP()))
		RMX_LOG_INFO("Netplay host: a friend connects through the relay server");
	if (mRestrictToKnownPeers)
	{
		bool known = std::find(mKnownPeerIPs.begin(), mKnownPeerIPs.end(), senderAddress.getIP()) != mKnownPeerIPs.end();
		for (size_t k = 0; k < mConnectingPlayers.size() && !known; ++k)
			known = (mConnectingPlayers[k].mRemoteAddress.getIP() == senderAddress.getIP());
		if (!known)
			known = mNetplayManager.isLanRequester(senderAddress.getIP());		// A friend on the same network found this host with the code
		if (!known && RelayProtocol::isVirtualIP(senderAddress.getIP()))
		{
			// A friend through the relay server: the relay only passes on friends who know the lobby's key, and the code check (handshake) still follows
			known = true;
		}
		if (!known)
		{
			RMX_LOG_INFO("Netplay host: refused a connection from an address the matchmaking server did not introduce");
			return nullptr;
		}
		if (std::find(mKnownPeerIPs.begin(), mKnownPeerIPs.end(), senderAddress.getIP()) == mKnownPeerIPs.end())
			mKnownPeerIPs.push_back(senderAddress.getIP());
	}

	// Connections that ended are removed right away (a dropped friend's slot must be free even while the simulation is paused)
	const auto oldConnections = mPlayerConnections;
	for (PlayerConnection* connection : oldConnections)
		if (connection->getState() == NetConnection::State::DISCONNECTED || connection->getState() == NetConnection::State::EMPTY)
			retireConnection(connection);

	// Strangers who never prove anything must not be able to pile up connections (the player slot is only assigned after the proof), and must not
	// be able to keep real friends out either: one address holds only a few unproven connections, and a full table gives way to a newcomer
	{
		std::vector<NetplayLobby::ConnectionInfo> infos;
		for (const PlayerConnection* existing : mPlayerConnections)
		{
			NetplayLobby::ConnectionInfo info;
			info.mIP = existing->getRemoteAddress().getIP();
			info.mAuthenticated = existing->mAuthenticated;
			info.mPendingSeconds = existing->mPendingSeconds;
			infos.push_back(info);
		}
		const int victim = NetplayLobby::chooseConnectionToReplace(infos, senderAddress.getIP(), (size_t)(MAX_PLAYERS * 2));
		if (victim == NetplayLobby::REFUSE_NEW_CONNECTION)
			return nullptr;
		if (victim >= 0)
		{
			PlayerConnection* old = mPlayerConnections[(size_t)victim];
			RMX_LOG_INFO("Netplay host: dropped an unproven connection to make room for a new one");
			old->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			retireConnection(old);
		}
	}

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
	connection->mAuthenticated = false;		// A player slot is assigned once the friend proved that it knows the code, see "handleJoinAuth"

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

void NetplayHost::retireConnection(PlayerConnection* connection)
{
	if (nullptr == connection || std::find(mPlayerConnections.begin(), mPlayerConnections.end(), connection) == mPlayerConnections.end())
		return;
	if (mJoiningConnection == connection)
	{
		mJoiningConnection = nullptr;
		mJoinPhase = JoinPhase::NONE;
	}

	// A friend whose connection just broke (timeout) keeps its slot for a while, for its own address; whoever left on purpose or was kicked does not
	if (connection->mAuthenticated && !connection->mLeaving && connection->mPlayerIndex >= 1 && connection->mPlayerIndex < MAX_PLAYERS)
	{
		const NetConnection::DisconnectReason reason = connection->getDisconnectReason();
		if (reason == NetConnection::DisconnectReason::TIMEOUT || reason == NetConnection::DisconnectReason::STALE)
		{
			mReservations.add(connection->mPlayerIndex, identityOf(*connection), connection->mName, connection->mCharacter);
			RMX_LOG_INFO("Netplay host: player " << (int)(connection->mPlayerIndex + 1) << " (" << connection->mName << ") lost the connection, the slot is kept for a minute");
		}
	}

	connection->clear();
	vectorRemoveAll(mPlayerConnections, connection);
	mRetiredConnections.push_back(connection);		// Deleted in the next update: packets that are still queued may refer to it
}

bool NetplayHost::friendsHaveAnnounced() const
{
	// The game starts for a friend once it has said its name, readiness and character (its first lobby message comes right after the code check), or after three seconds at most.
	// Without this, a friend who joins without the lobby screen could be given the game before the host heard which character it wants.
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && !connection->mLobbyAnnounced && connection->mAuthSeconds < 3.0f)
			return false;
	return true;
}

int NetplayHost::getAuthenticatedCount() const
{
	int count = 0;
	for (const PlayerConnection* connection : mPlayerConnections)
		count += (nullptr != connection && connection->isActive()) ? 1 : 0;
	return count;
}

std::string NetplayHost::identityOf(const PlayerConnection& connection)
{
	// The token is chosen by the friend's game, so it only tells the same running game apart from another one. Without a token the exact address (with port) is used:
	// two friends behind the same network address must not be taken for one player (slot takeover).
	return (connection.mClientToken != 0) ? ("t" + std::to_string(connection.mClientToken)) : ("ip" + connection.getRemoteAddress().getIP() + ":" + std::to_string(connection.getRemoteAddress().getPort()));
}


void NetplayHost::refuseConnection(PlayerConnection& connection, uint8 result)
{
	JoinReplyPacket reply;
	reply.mResult = result;
	connection.sendPacket(reply, NetConnection::SendFlags::NONE);
	connection.mRefuseTimer = 0.0f;		// Disconnected a moment later, so that the answer can arrive
}

void NetplayHost::handleJoinAuth(PlayerConnection& connection, const JoinAuthPacket& packet)
{
	if (connection.mAuthenticated || !connection.mChallengeSent || connection.mRefuseTimer >= 0.0f)
		return;

	const std::string ip = connection.getRemoteAddress().getIP();
	if (mFailedAuth.isBlocked(ip))
	{
		// Somebody keeps guessing: no more answers for a minute (the block ends by itself, see AuthFailureTracker)
		connection.mRefuseTimer = 1000.0f;
		return;
	}

	if (!NetplayAuth::verifyFriendProof(mNetplayManager.getHostSecret(), connection.mHostNonce, packet.mClientNonce, packet.mProof))
	{
		const int failures = mFailedAuth.recordFailure(ip);
		RMX_LOG_INFO("Netplay host: refused a friend, the code was wrong (" << failures << ". wrong attempt from " << ip << ")");
		refuseConnection(connection, (uint8)NetplayAuth::JoinResult::WRONG_CODE);
		return;
	}
	mFailedAuth.recordSuccess(ip);

	// A friend whose lobby messages are of another version (before the character choice, or a later one) cannot play with this host: it gets told so, through the normal
	// "different version" path, instead of messages that one side cannot read
	if (packet.mLobbyProtocol != NetplayLobby::LOBBY_PROTOCOL)
	{
		RMX_LOG_INFO("Netplay host: refused a friend that speaks another version of the lobby messages (" << (int)packet.mLobbyProtocol << " instead of " << (int)NetplayLobby::LOBBY_PROTOCOL << ")");
		refuseConnection(connection, (uint8)NetplayAuth::JoinResult::VERSION_MISMATCH);
		return;
	}

	connection.mClientToken = packet.mClientToken;
	const std::string identity = identityOf(connection);
	if (mKickBlocked.isBlocked(identity))
	{
		RMX_LOG_INFO("Netplay host: refused a player from " << ip << ", the host removed this player a moment ago");
		refuseConnection(connection, (uint8)NetplayAuth::JoinResult::KICKED);
		return;
	}

	// Pick the player slot: the one the friend had before a connection loss if that is free (or still held by the old connection of the same address), else the lowest free one
	auto holderOf = [this, &connection](int slot) -> PlayerConnection*
	{
		for (PlayerConnection* other : mPlayerConnections)
			if (other != &connection && other->mAuthenticated && other->mPlayerIndex == slot && other->getState() != NetConnection::State::DISCONNECTED && other->getState() != NetConnection::State::EMPTY)
				return other;		// (a connection that already ended does not hold its slot any more)
		return nullptr;
	};
	uint16 takenOverCharacter = NetplayLobby::CHARACTER_NONE;		// the pick of the old connection of the same friend, if its slot is taken over below
	int slot = 0;
	// A slot that was kept for this address after a lost connection comes first (also for a friend who comes back by pressing Retry, without asking for it)
	int preferred = packet.mPreferredSlot;
	if (const int reserved = mReservations.findSlotFor(identity, preferred))
	{
		if (reserved <= mMaxClients && nullptr == holderOf(reserved))
		{
			preferred = reserved;
			RMX_LOG_INFO("Netplay host: friend came back, getting its kept player slot " << (int)(reserved + 1));
		}
	}
	if (preferred >= 1 && preferred <= mMaxClients)
	{
		PlayerConnection* holder = holderOf(preferred);
		if (nullptr == holder)
		{
			if (!mReservations.isReservedForOther(preferred, identity))
				slot = preferred;
		}
		else if (identityOf(*holder) == identity)
		{
			// The old connection of this friend has not timed out yet: it is the same friend coming back
			RMX_LOG_INFO("Netplay host: friend came back, taking over its old player slot " << (int)(preferred + 1));
			takenOverCharacter = holder->mCharacter;
			holder->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			retireConnection(holder);
			slot = preferred;
		}
	}
	for (int candidate = 1; slot == 0 && candidate <= mMaxClients; ++candidate)
		if (nullptr == holderOf(candidate) && !mReservations.isReservedForOther(candidate, identity))
			slot = candidate;

	if (slot == 0)
	{
		RMX_LOG_INFO("Netplay host: refused a friend, the lobby is full");
		refuseConnection(connection, (uint8)NetplayAuth::JoinResult::LOBBY_FULL);
		return;
	}
	uint16 reservedCharacter = mReservations.findCharacterFor(slot, identity);
	if (reservedCharacter == NetplayLobby::CHARACTER_NONE)
		reservedCharacter = takenOverCharacter;		// a friend who comes back keeps what it had picked
	mReservations.remove(slot);
	if (packet.mPreferredSlot >= 1 && slot == packet.mPreferredSlot)
		RMX_LOG_INFO("Netplay host: friend came back to its player slot " << (slot + 1));

	connection.mAuthenticated = true;
	connection.mPlayerIndex = (uint8)slot;
	connection.mName = NetplayLobby::DEFAULT_NAME;		// The real name comes with the next lobby message of the friend
	connection.mPing = NetplayLobby::PingTracker();
	connection.mCharacterNotice = false;
	connection.mCharacterLimiter = NetplayLobby::CharacterRateLimiter();
	{
		// Its character: what the session has fixed if the game already started, else what it had before a connection loss, else the default of the slot
		const NetplayLobby::CharacterList& list = mNetplayManager.getAvailableCharacters();
		const int configured = mNetplayManager.getConfiguredPartyExtraSlot(slot);
		if (mPicksLocked)
			connection.mCharacter = mSessionPicks[slot];
		else if (reservedCharacter != NetplayLobby::CHARACTER_NONE && NetplayLobby::isCharacterAllowedForSlot(slot, reservedCharacter, list, configured))
			connection.mCharacter = reservedCharacter;
		else
			connection.mCharacter = NetplayLobby::defaultCharacterForSlot(slot, list, configured);
		if (reservedCharacter != NetplayLobby::CHARACTER_NONE && connection.mCharacter == reservedCharacter)
			RMX_LOG_INFO("Netplay host: player " << (slot + 1) << " keeps its character " << NetplayLobby::getCharacterName(reservedCharacter));
	}
	JoinReplyPacket reply;
	reply.mResult = (uint8)NetplayAuth::JoinResult::ACCEPTED;
	reply.mLobbyProtocol = NetplayLobby::LOBBY_PROTOCOL;
	reply.mSlot = (uint8)slot;
	reply.mProof = NetplayAuth::computeProof('H', mNetplayManager.getHostSecret(), connection.mHostNonce, packet.mClientNonce);
	connection.sendPacket(reply, NetConnection::SendFlags::NONE);
	RMX_LOG_INFO("Netplay host: a friend proved the code and is player " << (slot + 1));
}

void NetplayHost::updateHandshakes(float deltaSeconds)
{
	const std::vector<PlayerConnection*> connections = mPlayerConnections;
	for (PlayerConnection* connection : connections)
	{
		const NetConnection::State state = connection->getState();
		if (state == NetConnection::State::DISCONNECTED || state == NetConnection::State::EMPTY)
		{
			retireConnection(connection);
			continue;
		}
		if (connection->mLeaving)
		{
			// Sent away: the message needs a moment to arrive, then the connection ends
			connection->mLeaveTimer += deltaSeconds;
			if (connection->mLeaveTimer > 0.7f)
			{
				connection->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
				retireConnection(connection);
			}
			continue;
		}
		if (connection->mAuthenticated)
		{
			connection->mAuthSeconds += deltaSeconds;
			continue;
		}

		connection->mPendingSeconds += deltaSeconds;
		if (connection->mRefuseTimer >= 0.0f)
		{
			connection->mRefuseTimer += deltaSeconds;
			if (connection->mRefuseTimer > 1.5f)
			{
				connection->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
				retireConnection(connection);
			}
			continue;
		}

		if (state == NetConnection::State::CONNECTED && !connection->mChallengeSent)
		{
			JoinChallengePacket challenge;
			connection->mHostNonce = NetplayAuth::randomNonce();
			challenge.mHostNonce = connection->mHostNonce;
			connection->sendPacket(challenge, NetConnection::SendFlags::NONE);
			connection->mChallengeSent = true;
		}
		if (connection->mPendingSeconds > 10.0f)
		{
			RMX_LOG_INFO("Netplay host: dropped a connection that did not prove the code in time");
			connection->disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			retireConnection(connection);
		}
	}
}

void NetplayHost::updateConnection(float deltaSeconds)
{
	EngineServerClient& engineServerClient = EngineServerClient::instance();

	for (PlayerConnection* connection : mRetiredConnections)
		delete connection;
	mRetiredConnections.clear();
	updateHandshakes(deltaSeconds);
	mReservations.update(deltaSeconds);
	mKickBlocked.update(deltaSeconds);
	mFailedAuth.update(deltaSeconds);
	updatePings(deltaSeconds);
	updateAutoDelay(deltaSeconds, false);

	// Do not wait forever for the matchmaking server
	if (mHostState != mTimedHostState)
	{
		mTimedHostState = mHostState;
		mHostStateTimer = 0.0f;
	}
	mHostStateTimer += deltaSeconds;
	if ((mHostState == HostState::CONNECT_TO_SERVER || (mHostState == HostState::REGISTERED && !mRegistrationRequest.hasResponse())) && mHostStateTimer > 25.0f)
	{
		mHostState = HostState::FAILED;
		mNetplayManager.reportFailure(NetplayFailure::Reason::SERVER_UNREACHABLE);
	}

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
			if (anyPlayerConnected(mPlayerConnections) && Application::instance().getSimulation().isRunning() && friendsHaveAnnounced())
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
					mNetplayManager.reportFailure(NetplayFailure::Reason::SERVER_UNREACHABLE, "The server did not accept this lobby code. Create a new lobby.");
					break;
				}
			}

			// Same as for direct connections: the friend waits while the host is in a menu, and gets the game once the host started one
			if (anyPlayerConnected(mPlayerConnections) && Application::instance().getSimulation().isRunning() && friendsHaveAnnounced())
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
			if (connection->isActive() && !connection->mReadyForGame)
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

	// From now on the characters of the players are fixed for this session (they go to everybody in the start message)
	lockCharacters();

	// Is a game already running? Then the client joins that game, which needs the current game state to be sent over
	PlayerConnection* firstActive = nullptr;
	for (PlayerConnection* connection : mPlayerConnections)
	{
		if (connection->isActive())
		{
			firstActive = connection;
			break;
		}
	}
	if (Application::instance().getSimulation().isRunning() && nullptr != firstActive)
	{
		RMX_LOG_INFO("Netplay host: a game is already running, the client will join it");
		mJoiningConnection = firstActive;
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
		for (int k = 0; k < MAX_PLAYERS; ++k)
			packet.mCharacterPicks[k] = mSessionPicks[k];
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
		if (!connection->mAuthenticated)
			continue;
		connection->mReadyForGame = true;
		packet.mPlayerIndex = connection->mPlayerIndex;
		connection->sendPacket(packet, NetConnection::SendFlags::NONE, &connection->mStartGamePacketID);
	}

	// Setup input checksum tracking
	mInputChecksum = rmx::startFNV1a_32();
	mRegularInputChecksum = mInputChecksum;
	mRegularChecksumFrameNumber = 0;

	mHostState = HostState::GAME_RUNNING;
	mDelayLine.clear();
	updateAutoDelay(0.0f, true);

	EngineMain::getDelegate().onStartNetplayGame(true);
}

bool NetplayHost::canBeginNextFrame(uint32 frameNumber)
{
	// The frame number goes back to the start when a new game gets started (e.g. the host picked a save in the menu): the clients need the new game then
	if (frameNumber < mLastSeenFrameNumber)
	{
		for (PlayerConnection* connection : mPlayerConnections)
		{
			if (connection->isActive() && connection->mReadyForGame && mJoinPhase == JoinPhase::NONE)
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
		if (connection->isActive() && connection->mReadyForGame && (uint64)frameNumber > (uint64)connection->mLastReceivedFrameNumber + MAX_LEAD)
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
	for (int k = 0; k < MAX_PLAYERS; ++k)
		packet.mCharacterPicks[k] = mSessionPicks[k];
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
			if (playerConnection->isActive() && playerConnection->mReadyForGame)
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
				retireConnection(connection);
			}
	}

	// Add new frame to the history
	{
		mInputHistory.emplace_back();
		InputFrame& newInputFrame = mInputHistory.back();

		// The inputs of this moment: the host's local input (local player 1 only) and the last received input from each client
		NetplayLobby::InputDelayLine::Frame rawInputs = { 0, 0, 0, 0 };
		rawInputs[0] = controlsIn.getInputFromController(0);
		for (PlayerConnection* playerConnection : activeConnections)
		{
			if (playerConnection->mPlayerIndex >= 0 && playerConnection->mPlayerIndex < MAX_PLAYERS)
				rawInputs[playerConnection->mPlayerIndex] = playerConnection->mLastReceivedInput;
		}

		// Input delay: every player's input enters the game a few frames later, all by the same number (0 = no delay at all)
		const NetplayLobby::InputDelayLine::Frame usedInputs = mDelayLine.process(rawInputs);
		for (int k = 0; k < MAX_PLAYERS; ++k)
			newInputFrame.mInputsByPlayer[k] = usedInputs[k];
		if (mDelayLogEnabled)
		{
			for (int k = 1; k < MAX_PLAYERS; ++k)
			{
				if (rawInputs[k] != mDelayLogRaw[k])
				{
					mDelayLogRaw[k] = rawInputs[k];
					RMX_LOG_INFO("Input delay log: frame " << frameNumber << " player " << (k + 1) << " raw " << rawInputs[k] << " delay " << mEffectiveDelay);
				}
				if (usedInputs[k] != mDelayLogUsed[k])
				{
					mDelayLogUsed[k] = usedInputs[k];
					RMX_LOG_INFO("Input delay log: frame " << frameNumber << " player " << (k + 1) << " used " << usedInputs[k] << " delay " << mEffectiveDelay);
				}
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
		if (nullptr == connection || !connection->isActive())
			continue;
		if (connection->mPlayerIndex >= 1 && connection->mPlayerIndex < MAX_PLAYERS)
			outStates[connection->mPlayerIndex] = connection->mLobbyReady ? 3 : 2;
	}
}

int NetplayHost::getConnectedClientCount() const
{
	int count = 0;
	return getAuthenticatedCount();
}

bool NetplayHost::areAllClientsReady() const
{
	int count = 0;
	for (const PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr == connection || !connection->isActive())
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

	// The installed characters may have changed (a mod switched on or off): every pick is checked again then
	mNetplayManager.getAvailableCharacters();
	if (mNetplayManager.takeCharacterListChanged() && !mPicksLocked)
		revalidateCharacters();

	// What a friend sees (without the ping, which changes all the time): only a change in here is sent right away
	const int readySeconds = getReadyCheckSecondsLeft();
	std::string key = mLobbyHostStarting ? "S" : "-";
	for (int k = 0; k < MAX_PLAYERS; ++k)
		key += std::to_string((int)states[k]) + "|" + getPlayerName(k) + "|";
	key += std::to_string(mDelaySetting) + "|" + std::to_string(mEffectiveDelay) + "|" + std::to_string(readySeconds);
	uint8 noticeMask = 0;
	for (int k = 0; k < MAX_PLAYERS; ++k)
	{
		key += "|c" + std::to_string((int)getCharacterOfSlot(k));
		noticeMask |= (uint8)((hasCharacterNotice(k) ? 1 : 0) << k);
	}
	key += std::string("|n") + std::to_string((int)noticeMask) + (mPicksLocked ? "L" : "-");

	const bool allReady = areAllClientsReady();
	if (allReady != mLobbyWasAllReady)
	{
		mLobbyWasAllReady = allReady;
		RMX_LOG_INFO("Netplay host: lobby: " << (allReady ? "all players are ready" : "not everybody is ready"));
	}

	// A ready check ends when everybody is ready, or when its time ran out
	if (mReadyCheckActive)
	{
		mReadyCheckRemaining -= deltaSeconds;
		if (allReady)
			finishReadyCheck(false);
		else if (mReadyCheckRemaining <= 0.0f)
			finishReadyCheck(true);
	}

	// Repeated once per second while the lobby is open (a lost packet must not leave a friend with an old picture); once the game runs, only changes are sent
	mLobbyBroadcastTimer += deltaSeconds;
	const bool repeat = (mHostState != HostState::GAME_RUNNING) && (mLobbyBroadcastTimer >= 1.0f);
	if (key == mLobbyLastKey && !repeat)
		return;
	mLobbyLastKey = key;
	mLobbyBroadcastTimer = 0.0f;

	for (PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr == connection || !connection->isActive())
			continue;
		LobbyStatePacket packet;
		packet.mYourSlot = connection->mPlayerIndex;
		for (int k = 0; k < MAX_PLAYERS; ++k)
		{
			packet.mSlotState[k] = states[k];
			packet.mNames[k] = getPlayerName(k);
			const int ping = getPlayerPing(k);
			packet.mPingMs[k] = (ping < 0) ? 0xffff : (uint16)std::min(ping, 9999);
		}
		packet.mHostStarting = mLobbyHostStarting ? 1 : 0;
		packet.mDelaySetting = (mDelaySetting < 0) ? 0xff : (uint8)mDelaySetting;
		packet.mEffectiveDelay = (uint8)mEffectiveDelay;
		packet.mReadyCheckSeconds = (uint8)readySeconds;
		for (int k = 0; k < MAX_PLAYERS; ++k)
			packet.mCharacters[k] = getCharacterOfSlot(k);
		packet.mCharacterNotice = noticeMask;
		packet.mCharactersLocked = mPicksLocked ? 1 : 0;
		connection->sendPacket(packet, NetConnection::SendFlags::NONE);
	}
}

std::string NetplayHost::getPlayerName(int slot) const
{
	if (slot == 0)
		return mNetplayManager.getLocalPlayerName();
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && connection->mPlayerIndex == slot)
			return connection->mName;
	return std::string();
}

int NetplayHost::getPlayerPing(int slot) const
{
	if (slot == 0)
		return 0;
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && connection->mPlayerIndex == slot)
			return connection->mPing.get();
	return -1;
}

// ---------------------------------------------------------------------------------------------------------------
// Character choice

uint16 NetplayHost::getCharacterOfSlot(int slot)
{
	if (slot < 0 || slot >= MAX_PLAYERS)
		return NetplayLobby::CHARACTER_NONE;
	if (mPicksLocked)
		return mSessionPicks[slot];
	if (slot == 0)
		return mNetplayManager.getHostCharacterPick();
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && connection->mPlayerIndex == slot && connection->mCharacter != NetplayLobby::CHARACTER_NONE)
			return connection->mCharacter;
	return NetplayLobby::defaultCharacterForSlot(slot, mNetplayManager.getAvailableCharacters(), mNetplayManager.getConfiguredPartyExtraSlot(slot));
}

bool NetplayHost::hasCharacterNotice(int slot) const
{
	if (slot == 0)
		return mHostCharacterNotice;
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && connection->mPlayerIndex == slot)
			return connection->mCharacterNotice;
	return false;
}

void NetplayHost::onHostCharacterChanged()
{
	mHostCharacterNotice = false;		// the host chose it itself
	mLobbyLastKey.clear();				// tell everybody right away
}

void NetplayHost::handleCharacterRequest(PlayerConnection& connection, uint16 requested)
{
	const int slot = connection.mPlayerIndex;
	if (slot < 1 || slot >= MAX_PLAYERS || requested == NetplayLobby::CHARACTER_NONE)
		return;
	if (mPicksLocked)
		return;		// the game has started: every pick is fixed for this session
	if (requested == connection.mCharacter)
		return;		// the same request that is repeated every two seconds together with the name
	if (!connection.mCharacterLimiter.allow(SDL_GetTicks()))
	{
		if (connection.mCharacterLimiter.mDropped == 1 || (connection.mCharacterLimiter.mDropped % 50) == 0)
			RMX_LOG_INFO("Netplay host: character requests of player " << (slot + 1) << " dropped, too fast (" << connection.mCharacterLimiter.mDropped << " dropped so far)");
		return;
	}

	// What a friend asks for is never trusted: it must be a character that this player can have and that is installed here
	const NetplayLobby::CharacterList& list = mNetplayManager.getAvailableCharacters();
	const int configured = mNetplayManager.getConfiguredPartyExtraSlot(slot);
	bool adjusted = false;
	const uint16 accepted = NetplayLobby::sanitizeCharacterPick(slot, requested, list, connection.mCharacter, &adjusted, configured);
	if (adjusted)
	{
		RMX_LOG_INFO("Netplay host: player " << (slot + 1) << " (" << connection.mName << ") asked for a character that is not possible (" << (int)requested << "), it stays " << NetplayLobby::getCharacterName(accepted));
		connection.mCharacterNotice = true;
	}
	else
	{
		connection.mCharacterNotice = false;
	}
	if (accepted != connection.mCharacter)
	{
		connection.mCharacter = accepted;
		RMX_LOG_INFO("Netplay host: lobby: player " << (slot + 1) << " (" << connection.mName << ") plays as " << NetplayLobby::getCharacterName(accepted));
		mLobbyLastKey.clear();
	}
}

void NetplayHost::revalidateCharacters()
{
	// The installed characters can change while the lobby is open (a mod was switched on or off): everybody whose character is gone gets the default of their slot, and is told
	const NetplayLobby::CharacterList& list = mNetplayManager.getAvailableCharacters();
	for (PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr == connection || !connection->isActive() || connection->mPlayerIndex < 1 || connection->mPlayerIndex >= MAX_PLAYERS)
			continue;
		const int slot = connection->mPlayerIndex;
		const int configured = mNetplayManager.getConfiguredPartyExtraSlot(slot);
		if (connection->mCharacter == NetplayLobby::CHARACTER_NONE)
		{
			connection->mCharacter = NetplayLobby::defaultCharacterForSlot(slot, list, configured);
			continue;
		}
		bool adjusted = false;
		const uint16 fixedPick = NetplayLobby::sanitizeCharacterPick(slot, connection->mCharacter, list, NetplayLobby::defaultCharacterForSlot(slot, list, configured), &adjusted, configured);
		if (adjusted)
		{
			RMX_LOG_INFO("Netplay host: the character of player " << (slot + 1) << " (" << NetplayLobby::getCharacterName(connection->mCharacter) << ") is not available any more, using " << NetplayLobby::getCharacterName(fixedPick));
			connection->mCharacter = fixedPick;
			connection->mCharacterNotice = true;
			mLobbyLastKey.clear();
		}
	}
	bool hostAdjusted = false;
	const uint16 hostPick = NetplayLobby::sanitizeCharacterPick(0, mNetplayManager.getHostCharacterPick(), list, NetplayLobby::CHARACTER_MENU, &hostAdjusted);
	if (hostAdjusted)
	{
		RMX_LOG_INFO("Netplay host: the host's own character is not available any more, using " << NetplayLobby::getCharacterName(hostPick));
		mNetplayManager.setHostCharacterPick(hostPick);
		mHostCharacterNotice = true;
		mLobbyLastKey.clear();
	}
}

void NetplayHost::lockCharacters()
{
	if (mPicksLocked)
		return;
	revalidateCharacters();		// the characters that are installed right now decide (a mod may have been switched off since the pick)
	uint16 picks[MAX_PLAYERS];
	for (int slot = 0; slot < MAX_PLAYERS; ++slot)
		picks[slot] = getCharacterOfSlot(slot);
	for (int slot = 0; slot < MAX_PLAYERS; ++slot)
		mSessionPicks[slot] = picks[slot];
	mPicksLocked = true;
	mNetplayManager.setSessionCharacters(mSessionPicks);
	mLobbyLastKey.clear();
}

int NetplayHost::getRelayedSlot() const
{
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && connection->mPlayerIndex >= 1 && RelayProtocol::isVirtualIP(connection->getRemoteAddress().getIP()))
			return connection->mPlayerIndex;
	return -1;
}

int NetplayHost::getRelayedFriendCount() const
{
	int count = 0;
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive() && RelayProtocol::isVirtualIP(connection->getRemoteAddress().getIP()))
			++count;
	return count;
}

int NetplayHost::getWorstPing() const
{
	int worst = -1;
	for (const PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive())
			worst = std::max(worst, connection->mPing.get());
	return worst;
}

int NetplayHost::getHighPingSlot(int& outPingMs) const
{
	for (const PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr != connection && connection->isActive() && NetplayLobby::isHighPing(connection->mPing.get()))
		{
			outPingMs = connection->mPing.get();
			return connection->mPlayerIndex;
		}
	}
	return -1;
}

void NetplayHost::updatePings(float deltaSeconds)
{
	mPingTimer += deltaSeconds;
	mPingLogTimer += deltaSeconds;
	for (PlayerConnection* connection : mPlayerConnections)
		connection->mHighPingLogTimer += deltaSeconds;

	if (mPingTimer >= 1.0f)
	{
		mPingTimer = 0.0f;
		for (PlayerConnection* connection : mPlayerConnections)
		{
			// (Not while the game state is being sent to this friend: the line is busy then and the number would only say that)
			if (nullptr == connection || !connection->isActive() || (mJoinPhase == JoinPhase::TRANSFERRING && connection == mJoiningConnection))
				continue;
			LobbyPingPacket ping;
			ping.mToken = ++mPingCounter;
			if (ping.mToken == 0)
				ping.mToken = ++mPingCounter;
			connection->mPingToken = ping.mToken;
			connection->mPingSentTicks = SDL_GetTicks();
			connection->sendPacket(ping, NetConnection::SendFlags::UNRELIABLE);
		}
	}

	// The measurements go into the log, and a high ping is a warning for the host
	if (mPingLogTimer >= 5.0f)
	{
		mPingLogTimer = 0.0f;
		for (PlayerConnection* connection : mPlayerConnections)
		{
			if (nullptr == connection || !connection->isActive() || connection->mPing.get() < 0)
				continue;
			RMX_LOG_INFO("Netplay host: ping of player " << (int)(connection->mPlayerIndex + 1) << " (" << connection->mName << ") is " << connection->mPing.get() << " ms");
			if (NetplayLobby::isHighPing(connection->mPing.get()) && connection->mHighPingLogTimer >= 10.0f)
			{
				connection->mHighPingLogTimer = 0.0f;
				RMX_LOG_INFO("Netplay host: WARNING player " << (int)(connection->mPlayerIndex + 1) << " (" << connection->mName << ") has a high ping: " << connection->mPing.get() << " ms");
			}
		}
	}
}

void NetplayHost::setInputDelaySetting(int setting)
{
	mDelaySetting = NetplayLobby::clampDelaySetting(setting);
	RMX_LOG_INFO("Netplay host: input delay setting is now " << (mDelaySetting < 0 ? std::string("AUTO") : std::to_string(mDelaySetting) + " frames"));
	updateAutoDelay(0.0f, true);
}

void NetplayHost::updateAutoDelay(float deltaSeconds, bool force)
{
	// A fixed setting applies at once; Auto looks at the measured ping every few seconds (so that it does not jump around)
	mAutoDelayTimer += deltaSeconds;
	if (!force && mDelaySetting < 0 && mAutoDelayTimer < 3.0f)
		return;
	mAutoDelayTimer = 0.0f;

	const int worstPing = getWorstPing();
	int wanted = NetplayLobby::effectiveInputDelay(mDelaySetting, worstPing);
	// Auto does not flip back and forth when the ping sits between two steps: it goes up at once, but only comes down by two steps or more
	if (mDelaySetting < 0 && !force && wanted < mEffectiveDelay && wanted > mEffectiveDelay - 2)
		wanted = mEffectiveDelay;
	if (wanted != mEffectiveDelay)
	{
		RMX_LOG_INFO("Netplay host: input delay is now " << wanted << " frames (" << (mDelaySetting < 0 ? "auto, worst ping " + std::to_string(worstPing) + " ms" : std::string("fixed")) << ")");
		mEffectiveDelay = wanted;
	}
	mDelayLine.setDelay(mEffectiveDelay);
}

bool NetplayHost::kickPlayer(int slot)
{
	for (PlayerConnection* connection : mPlayerConnections)
	{
		if (nullptr != connection && connection->isActive() && connection->mPlayerIndex == slot)
		{
			RMX_LOG_INFO("Netplay host: kicked player " << (slot + 1) << " (" << connection->mName << ")");
			mKickBlocked.add(identityOf(*connection));
			sendLeave(*connection, NetplayLobby::LeaveReason::KICKED);
			mLobbyLastKey.clear();		// Tell the others right away
			return true;
		}
	}
	return false;
}

void NetplayHost::sendLeave(PlayerConnection& connection, NetplayLobby::LeaveReason reason)
{
	LobbyLeavePacket packet;
	packet.mReason = (uint8)reason;
	connection.sendPacket(packet, NetConnection::SendFlags::NONE);
	connection.mLeaving = true;
	connection.mLeaveTimer = 0.0f;
	connection.mLobbyReady = false;
}

void NetplayHost::addChatLine(int slot, const std::string& text)
{
	ChatLine line;
	line.mSlot = slot;
	line.mText = text;
	mChatLog.push_back(line);
	while (mChatLog.size() > 8)
		mChatLog.pop_front();

	LobbyChatBroadcastPacket packet;
	packet.mSlot = (uint8)slot;
	packet.mText = text;
	for (PlayerConnection* connection : mPlayerConnections)
		if (nullptr != connection && connection->isActive())
			connection->sendPacket(packet, NetConnection::SendFlags::NONE);
	RMX_LOG_INFO("Netplay host: chat, player " << (slot + 1) << " (" << getPlayerName(slot) << "): " << text);
}

void NetplayHost::sendHostChat(int quickMessage, const std::string& freeText)
{
	const std::string text = NetplayLobby::buildChatText(quickMessage, freeText);
	if (!text.empty())
		addChatLine(0, text);
}

void NetplayHost::beginReadyCheck()
{
	if (mReadyCheckActive || areAllClientsReady() || getAuthenticatedCount() == 0)
		return;
	float seconds = NetplayLobby::READY_CHECK_SECONDS;
	if (const char* testSeconds = TestHooks::getEnv("S3AIR_LOBBY_READY_TIMEOUT"))
		seconds = std::max(1.0f, (float)atof(testSeconds));
	mReadyCheckActive = true;
	mReadyCheckFinished = false;
	mReadyCheckRemaining = seconds;
	mLobbyLastKey.clear();
	RMX_LOG_INFO("Netplay host: ready check started, " << (int)seconds << " seconds");
}

void NetplayHost::finishReadyCheckNow()
{
	if (mReadyCheckActive)
		finishReadyCheck(true);
}

void NetplayHost::finishReadyCheck(bool timedOut)
{
	mReadyCheckActive = false;
	int leftOut = 0;
	if (timedOut)
	{
		// The game starts without the players who are not ready (they can join the running game with the code)
		for (PlayerConnection* connection : mPlayerConnections)
		{
			if (nullptr != connection && connection->isActive() && !connection->mLobbyReady)
			{
				RMX_LOG_INFO("Netplay host: player " << (int)(connection->mPlayerIndex + 1) << " (" << connection->mName << ") was not ready and is left out");
				sendLeave(*connection, NetplayLobby::LeaveReason::NOT_READY);
				++leftOut;
			}
		}
		RMX_LOG_INFO("Netplay host: ready check timed out, starting without " << leftOut << " player(s)");
	}
	else
	{
		RMX_LOG_INFO("Netplay host: ready check passed, everybody is ready");
	}
	mReadyCheckFinished = true;
	mLobbyLastKey.clear();
}

int NetplayHost::getReadyCheckSecondsLeft() const
{
	return mReadyCheckActive ? std::max(1, (int)std::ceil(mReadyCheckRemaining)) : 0;
}

bool NetplayHost::takeReadyCheckFinished()
{
	const bool finished = mReadyCheckFinished;
	mReadyCheckFinished = false;
	return finished;
}

bool NetplayHost::onReceivedPacket(ReceivedPacketEvaluation& evaluation)
{
	// Nobody gets any further than the handshake before proving that they know the code
	if (evaluation.mPacketType == JoinAuthPacket::PACKET_TYPE)
	{
		JoinAuthPacket packet;
		if (!evaluation.readPacket(packet))
			return false;
		handleJoinAuth(static_cast<PlayerConnection&>(evaluation.mConnection), packet);
		return true;
	}
	if (!static_cast<PlayerConnection&>(evaluation.mConnection).mAuthenticated)
		return true;

	switch (evaluation.mPacketType)
	{
		case LobbyReadyPacket::PACKET_TYPE:
		{
			LobbyReadyPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			// What a friend sends is never trusted: the name is cleaned up here
			const std::string name = NetplayLobby::sanitizeName(packet.mName);
			if (name != connection.mName)
			{
				RMX_LOG_INFO("Netplay host: lobby: player " << (int)(connection.mPlayerIndex + 1) << " is called \"" << name << "\"");
				connection.mName = name;
			}
			handleCharacterRequest(connection, packet.mCharacter);
			const bool wasReady = connection.mLobbyReady;
			connection.mLobbyReady = (packet.mReady != 0);
			if (connection.mLobbyReady != wasReady || !connection.mLobbyAnnounced)
			{
				connection.mLobbyAnnounced = true;
				RMX_LOG_INFO("Netplay host: lobby: player " << (int)(connection.mPlayerIndex + 1) << (connection.mLobbyReady ? " is ready" : " is not ready"));
			}
			return true;
		}

		case LobbyChatPacket::PACKET_TYPE:
		{
			LobbyChatPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			const std::string text = NetplayLobby::buildChatText(packet.mQuickMessage, packet.mText);
			if (text.empty())
				return true;
			if (!connection.mChatLimiter.allow(SDL_GetTicks()))
			{
				RMX_LOG_INFO("Netplay host: chat of player " << (int)(connection.mPlayerIndex + 1) << " dropped, too fast (" << connection.mChatLimiter.mDropped << " dropped so far)");
				return true;
			}
			addChatLine(connection.mPlayerIndex, text);
			return true;
		}

		case LobbyPongPacket::PACKET_TYPE:
		{
			LobbyPongPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			if (packet.mToken != 0 && packet.mToken == connection.mPingToken)
			{
				connection.mPingToken = 0;
				connection.mPing.addSample((int)(SDL_GetTicks() - connection.mPingSentTicks));
			}
			return true;
		}

		case ResyncRequestPacket::PACKET_TYPE:
		{
			ResyncRequestPacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			// The client's game drifted away: send it the game state again (handled like a player that joins a running game)
			PlayerConnection& connection = static_cast<PlayerConnection&>(evaluation.mConnection);
			// A repair freezes the game for everybody, so one friend cannot ask again and again: at most one repair per ten seconds each
				const uint32 now = std::max<uint32>(SDL_GetTicks(), 1);
				const bool tooSoon = (connection.mLastResyncTicks != 0 && now - connection.mLastResyncTicks < 10000);
				if (connection.mReadyForGame && mJoinPhase == JoinPhase::NONE && !tooSoon)
				{
					RMX_LOG_INFO("Netplay host: client's game drifted away at frame " << packet.mFrameNumber << ", sending the game state again");
					connection.mReadyForGame = false;
					connection.mLastResyncTicks = now;
					++mResyncCount;
				}
				else if (tooSoon)
				{
					RMX_LOG_INFO("Netplay host: ignored a repair request that came too soon after the last one");
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

				// A friend cannot be ahead of the host's own frame (by more than a little): a frame number far in the future would freeze that slot's input
					const bool plausible = ((uint64)packet.mFrameNumber <= (uint64)mLastSeenFrameNumber + 300);
					if (packet.mFrameNumber > connection.mLastReceivedFrameNumber && plausible)		// Ignore out-dated packets
					{
					connection.mLastReceivedFrameNumber = packet.mFrameNumber;
					if (connection.mLastReceivedInput != packet.mInputs.back() && nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DEBUG"))
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
