/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/network/netplay/NetplayClient.h"
#include "oxygen/network/netplay/NetplayManager.h"
#include "oxygen/network/netplay/NetplayPackets.h"
#include "oxygen/network/netplay/NetplayAuth.h"
#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen_netcore/network/RelayProtocol.h"
#include "oxygen/network/EngineServerClient.h"
#include "oxygen/application/Application.h"
#include "oxygen/application/input/ControlsIn.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/Simulation.h"
#include "oxygen/simulation/SimulationState.h"

#include "oxygen_netcore/serverclient/NetplaySetupPackets.h"
#include "oxygen_netcore/serverclient/Packets.h"


NetplayClient::NetplayClient(ConnectionManager& connectionManager, NetplayManager& netplayManager) :
	mConnectionManager(connectionManager),
	mNetplayManager(netplayManager)
{
}

NetplayClient::~NetplayClient()
{
	// TODO: Call this only if netplay game was actually active
	EngineMain::getDelegate().onStopNetplayGame(false);

	// Tell the host that we are leaving (so that the slot is free at once), then drop the connection
	mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
	mHostConnection.clear();
}

void NetplayClient::joinViaServer()
{
	// Request game server connection
	EngineServerClient::instance().connectToServer();
	mState = State::CONNECT_TO_SERVER;
}

void NetplayClient::connectDirectlyToHost(std::string_view ip, uint16 port)
{
	beginConnectTo(SocketAddress(ip, port));
	mState = State::CONNECT_TO_HOST;
}

void NetplayClient::beginConnectTo(const SocketAddress& address)
{
	// Every new connection starts with the code check
	mHostVerified = false;
	mAuthSent = false;
	mHeartbeatTimer = 0.0f;
	mHostConnection.startConnectTo(mConnectionManager, address);
}

void NetplayClient::fail(NetplayFailure::Reason reason, const std::string& detail)
{
	RMX_LOG_INFO("Netplay client: giving up, " << NetplayFailure::getName(reason) << (detail.empty() ? "" : ": ") << detail);
	mNetplayManager.reportFailure(reason, detail);
	mState = State::FAILED;
}

bool NetplayClient::startRelayFallback(const char* why)
{
	// Only if the player set up a relay server (and joined with a session code, which the relay needs), and only once
	if (mUsingRelay || !mNetplayManager.canUseRelayAsClient())
		return false;
	if (!mNetplayManager.beginRelayAsClient())
		return false;

	RMX_LOG_INFO("Netplay client: " << why << ", connecting through the relay server instead");
	mUsingRelay = true;
	mRelayJoinPending = true;
	mHostConnection.clear();		// The direct attempt is over
	mHostVerified = false;
	mAuthSent = false;
	mState = State::CONNECT_TO_HOST;
	mTimedState = State::NONE;		// Restarts the timeout: the relay gets its own time
	mStateTimer = 0.0f;
	return true;
}

void NetplayClient::updateConnection(float deltaSeconds)
{
	EngineServerClient& engineServerClient = EngineServerClient::instance();
	if (!mReconnectTestTriggered && nullptr != TestHooks::getEnv("S3AIR_NETPLAY_RECONNECT_TEST") && mState == State::GAME_RUNNING && !mReceivingJoinState && mStateTimer > 12.0f)
	{
		mReconnectTestTriggered = true;
		RMX_LOG_INFO("Netplay client: automated check disconnecting the transport");
		mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
	}

	if (mState == State::GAME_RUNNING && mHostConnection.getState() == NetConnection::State::DISCONNECTED)
	{
		mReconnectAddress = mHostConnection.getRemoteAddress();
		if (!mResumeSession)
			mReconnectRemaining = 90.0f;
		mResumeSession = true;
		mReceivingJoinState = false;
		mNextFrameNumber = 0;
		mReceivedFrames.clear();
		mHostConnection.clear();
			beginConnectTo(mReconnectAddress);
			mState = State::CONNECT_TO_HOST;
			mTimedState = State::NONE;
		mStateTimer = 0.0f;
		mNetplayManager.mLastErrorMessage = "Connection lost. Reconnecting to your game...";
		RMX_LOG_INFO("Netplay client: reconnecting to the existing session");
	}

	// Developer test: the lobby connection breaks without a word (as if the network dropped), to see that the friend comes back to its slot
	static bool lobbyDropTestDone = false;		// (once per run of the game, not once per connection)
	if (!lobbyDropTestDone && nullptr != TestHooks::getEnv("S3AIR_NETPLAY_LOBBY_DROP_TEST") && mState == State::CONNECTED && mHostVerified && mLobbyKnown && mStateTimer > 6.0f)
	{
		lobbyDropTestDone = true;
		RMX_LOG_INFO("Netplay client: automated check dropping the lobby connection");
		mHostConnection.clear();
	}

	// The connection to the host broke while we were waiting in its lobby: try to come back to the same slot for a while
	//  -> Not if the host closed the lobby on purpose (it said so), that is a different message
	if (mState == State::CONNECTED && mHostVerified && mLobbyKnown && !mLobbyRejoin && !mResumeSession)
	{
		const NetConnection::State connectionState = mHostConnection.getState();
		const bool hostSaidGoodbye = (connectionState == NetConnection::State::DISCONNECTED && mHostConnection.getDisconnectReason() == NetConnection::DisconnectReason::MANUAL_REMOTE);
		if ((connectionState == NetConnection::State::DISCONNECTED || connectionState == NetConnection::State::EMPTY) && !hostSaidGoodbye)
		{
			mReconnectAddress = mHostConnection.getRemoteAddress();
			mLobbyRejoin = true;
			mReconnectRemaining = 40.0f;
			mHostConnection.clear();
			beginConnectTo(mReconnectAddress);
			mState = State::CONNECT_TO_HOST;
			mTimedState = State::NONE;
			mStateTimer = 0.0f;
			mNetplayManager.mLastErrorMessage = "Connection lost. Reconnecting to the lobby...";
			RMX_LOG_INFO("Netplay client: lost the lobby connection, trying to come back to player slot " << (int)(mLobbyYourSlot + 1));
		}
	}

	if (mReconnectRemaining > 0.0f)
	{
		mReconnectRemaining -= deltaSeconds;
		if (mReconnectRemaining <= 0.0f)
		{
			mReconnectRemaining = 0.0f;
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			if (mLobbyRejoin)
			{
				mLobbyRejoin = false;
				fail(NetplayFailure::Reason::HOST_CLOSED, "Could not reconnect to the lobby within 40 seconds.");
			}
			else
			{
				fail(NetplayFailure::Reason::CONNECTION_LOST, "Could not reconnect within 90 seconds.");
			}
			return;
		}
		if (mState == State::CONNECT_TO_HOST && (mStateTimer > 5.0f || mHostConnection.getState() == NetConnection::State::DISCONNECTED))
		{
			mHostConnection.clear();
				beginConnectTo(mReconnectAddress);
				mStateTimer = 0.0f;
		}
	}

	// Do not wait forever
	if (mState != mTimedState)
	{
		mTimedState = mState;
		mStateTimer = 0.0f;
	}
	mStateTimer += deltaSeconds;
	if (mState == State::CONNECT_TO_SERVER && mStateTimer > 15.0f)
	{
		if (startRelayFallback("the game server did not answer"))
			return;
		fail(NetplayFailure::Reason::SERVER_UNREACHABLE);
		return;
	}
	else if (mState == State::REGISTERED && mStateTimer > 30.0f)
	{
		if (startRelayFallback("the game server did not introduce the host"))
			return;
		fail(NetplayFailure::Reason::WRONG_CODE);
		return;
	}
	else if (mState == State::CONNECT_TO_HOST && !mUsingRelay && mStateTimer > NetplayManager::RELAY_FALLBACK_SECONDS && mReconnectRemaining == 0.0f && !mResumeSession && startRelayFallback("the direct connection did not work"))
	{
		return;
	}
	else if (mState == State::CONNECT_TO_HOST && mUsingRelay && mRelayJoinPending && mNetplayManager.getRelayRefusal() == 2)
	{
		mNetplayManager.reportFailure(NetplayFailure::Reason::LOBBY_FULL);
		mState = State::FAILED;
		return;
	}
	else if (mState == State::CONNECT_TO_HOST && mStateTimer > 20.0f && mReconnectRemaining == 0.0f)
	{
		if (mUsingRelay)
			fail(mServerKnewNoHost ? NetplayFailure::Reason::WRONG_CODE : NetplayFailure::Reason::BLOCKED_OR_TIMEOUT, mRelayJoinPending ? "The relay server did not answer, or the host is not registered there." : "Not even the relay server could connect you to the host.");
		else
			fail(NetplayFailure::Reason::BLOCKED_OR_TIMEOUT);
		return;
	}
	else if (mState == State::CONNECTED && !mHostVerified && mStateTimer > 10.0f)
	{
		// Connected, but the host never started the code check: it runs a different version of the game
		fail(NetplayFailure::Reason::VERSION_MISMATCH, "The host did not answer the code check.");
		return;
	}
	else if (mState == State::CONNECTED && mHostConnection.getState() == NetConnection::State::DISCONNECTED)
	{
		// The host left before the game started (or stopped answering)
		fail(NetplayFailure::Reason::HOST_CLOSED);
		return;
	}

	switch (mState)
	{
		case State::CONNECT_TO_SERVER:
		{
			// Keep requesting game server connection
			engineServerClient.connectToServer();

			// Wait until the server connection is established, server features were queried, and the game socket's external address was retrieved
			if (!engineServerClient.hasReceivedServerFeatures() || mNetplayManager.getExternalAddressQuery().mOwnExternalIP.empty())
				break;

			// TODO: Why not check the server features?

			// Register at game server
			mRegistrationRequest = network::RegisterForNetplayRequest();
			mRegistrationRequest.mQuery.mIsHost = false;
			mRegistrationRequest.mQuery.mSessionID = mNetplayManager.mSessionID;
			mRegistrationRequest.mQuery.mGameSocketExternalIP = mNetplayManager.getExternalAddressQuery().mOwnExternalIP;
			mRegistrationRequest.mQuery.mGameSocketExternalPort = mNetplayManager.getExternalAddressQuery().mOwnExternalPort;
			engineServerClient.getServerConnection().sendRequest(mRegistrationRequest);

			mState = State::REGISTERED;
			break;
		}

		case State::REGISTERED:
		{
			// Check registration
			if (mRegistrationRequest.hasResponse())
			{
				if (!mRegistrationRequest.mResponse.mSuccess)
					{
						mServerKnewNoHost = true;
						if (startRelayFallback("the game server does not know this code"))
							break;
						fail(NetplayFailure::Reason::WRONG_CODE, "The game server does not know this code.");
						break;
					}
			}
			break;
		}

		case State::CONNECT_TO_HOST:
		{
			if (mRelayJoinPending)
			{
				// Through the relay: first the relay has to let us in, then the normal connection to the host starts (with the host's made-up address)
				if (mNetplayManager.isRelayJoined())
				{
					mRelayJoinPending = false;
					RMX_LOG_INFO("Netplay client: the relay server let us in, connecting to the host through it");
					beginConnectTo(SocketAddress(RelayProtocol::hostVirtualIP(), RelayProtocol::VIRTUAL_PORT));
				}
				break;
			}
			if (mHostConnection.getState() == NetConnection::State::CONNECTED)
			{
				RMX_LOG_INFO("Netplay client: connected to host");
				mState = State::CONNECTED;

				// The host only considers the connection established once it received any packet from the client
				//  -> Without this, nothing would get sent as long as the client is not running a game yet (e.g. in a menu)
				PlayerInputIncrementPacket helloPacket;
				helloPacket.mFrameNumber = 0;
				helloPacket.mNumFrames = 1;
				helloPacket.mInputs.push_back(0);
				mHostConnection.sendPacket(helloPacket, NetConnection::SendFlags::NONE);

				}
				break;
			}

			case State::CONNECTED:
			{
				// While waiting in the lobby the host hears from us every two seconds (a silent connection would be dropped as stale, and a lost "Ready" gets repeated).
				//  -> These packets are also how we notice that the host is gone: they would stay unanswered.
				if (mHostVerified && mHostConnection.getState() == NetConnection::State::CONNECTED)
				{
					mHeartbeatTimer += deltaSeconds;
					if (mHeartbeatTimer >= 2.0f)
					{
						mHeartbeatTimer = 0.0f;
						sendLobbyReady(mLobbyMyReady);
					}
				}
				break;
			}

		default:
			break;
	}
}

bool NetplayClient::onReceivedGameServerPacket(ReceivedPacketEvaluation& evaluation)
{
	switch (evaluation.mPacketType)
	{
		case network::ConnectToNetplayPacket::PACKET_TYPE:
		{
			network::ConnectToNetplayPacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			if (mState != State::CONNECT_TO_SERVER && mState != State::REGISTERED)
				return true;	// Already connecting (e.g. found the host in the same network)

			// Connect to the given address
				beginConnectTo(SocketAddress(packet.mConnectToIP, packet.mConnectToPort));
			mState = State::CONNECT_TO_HOST;
			return true;
		}
	}

	return false;
}

bool NetplayClient::canBeginNextFrame(uint32 frameNumber)
{
	// Don't proceed beyond the latest received frame number
	if (mState != State::GAME_RUNNING || mReceivingJoinState || frameNumber >= mNextFrameNumber)
		return false;

	return true;
}

void NetplayClient::onFrameUpdate(ControlsIn& controlsIn, uint32 frameNumber)
{
	if (mHostConnection.getState() != NetConnection::State::CONNECTED || !mHostVerified)
		return;

	// Compare our game state with what the host reported for the same frame
	if (mState == State::GAME_RUNNING && !mReceivingJoinState && (frameNumber % 60) == 0)
	{
		const uint32 ownHash = NetplayManager::computeStateHash(frameNumber);

		static const bool corruptForTest = (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_CORRUPT"));
		static bool corrupted = false;
		if (corruptForTest && !corrupted && frameNumber >= mJoinFrameNumber + 300 && mJoinFrameNumber != 0)
		{
			// Developer test: make our game drift away on purpose
			corrupted = true;
			Application::instance().getSimulation().getEmulatorInterface().getRam()[0xb010] ^= 0x40;
			RMX_LOG_INFO("Netplay client: (test) changed the game state on purpose at frame " << frameNumber);
		}

		if (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DUMP"))
		{
			FILE* f = fopen((std::string(getenv("APPDATA") ? getenv("APPDATA") : ".") + "/Sonic3AIR/client_compare.txt").c_str(), "a");
			if (nullptr != f)
			{
				const auto hostIt = mHostStateHashes.find(frameNumber);
				fprintf(f, "frame %u own %08x host %s%08x\n", frameNumber, ownHash, (hostIt == mHostStateHashes.end()) ? "(none) " : "", (hostIt == mHostStateHashes.end()) ? 0 : hostIt->second);
				fclose(f);
			}
		}
		mOwnStateHashes[frameNumber] = ownHash;
		const auto it = mHostStateHashes.find(frameNumber);
		if (it != mHostStateHashes.end())
		{
			if (it->second == ownHash)
			{
				mLastSyncOkFrame = frameNumber;
			}
			else if (!mAwaitingRepair || frameNumber >= mRepairRequestedFrame + 600)	// a lost request is retried after about ten seconds
			{
				RMX_LOG_INFO("Netplay client: game state differs from the host at frame " << frameNumber << ", asking for a repair");
				++mDesyncCount;
				mAwaitingRepair = true;
				mRepairRequestedFrame = frameNumber;
				ResyncRequestPacket request;
				request.mFrameNumber = frameNumber;
				mHostConnection.sendPacket(request, NetConnection::SendFlags::NONE);
			}
			mHostStateHashes.erase(it);
		}
		while (mOwnStateHashes.size() > 8)
			mOwnStateHashes.erase(mOwnStateHashes.begin());
	}

	// Get current input state and send it to the host
	{
		PlayerInputIncrementPacket packet;
		packet.mFrameNumber = frameNumber;
		packet.mNumFrames = 1;
		uint16 input = controlsIn.getInputFromController(0);
		if (nullptr != TestHooks::getEnv("S3AIR_CHECKS"))
			if (const char* testInput = TestHooks::getEnv("S3AIR_NETPLAY_TEST_INPUT"))
			{
				input = (uint16)strtoul(testInput, nullptr, 0);
				// Developer test: the input switches on and off every two seconds, so that the delay between a change and its effect can be measured
				if (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_TEST_TOGGLE") && ((frameNumber / 120) % 2) == 0)
					input = 0;
			}
		packet.mInputs.push_back(input);
		{
			static uint16 lastSent = 0;
			if (packet.mInputs.back() != lastSent && nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DEBUG"))
				RMX_LOG_INFO("Netplay client: sending input " << rmx::hexString(packet.mInputs.back(), 4) << " at frame " << frameNumber);
			lastSent = packet.mInputs.back();
		}

		mHostConnection.sendPacket(packet, NetConnection::SendFlags::UNRELIABLE);
	}

	// Inject input from what we received from host
	if (!mReceivedFrames.empty())
	{
		const int indexFromBack = mNextFrameNumber - frameNumber - 1;
		if (indexFromBack >= 0 && indexFromBack < (int)mReceivedFrames.size())
		{
			auto it = mReceivedFrames.rbegin();
			for (int k = 0; k < indexFromBack; ++k)
				++it;

			controlsIn.injectInputs(it->mInputsByPlayer, MAX_PLAYERS);
		}
	}
}

void NetplayClient::sendLobbyReady(bool ready)
{
	mLobbyMyReady = ready;
	if (mHostConnection.getState() != NetConnection::State::CONNECTED)
		return;
	if (mMyCharacter == NetplayLobby::CHARACTER_NONE)
	{
		// Developer test switch: pick without a key press
		mMyCharacter = mNetplayManager.getCharacterPickFromEnvironment();
	}
	LobbyReadyPacket packet;
	packet.mReady = ready ? 1 : 0;
	packet.mName = mNetplayManager.getLocalPlayerName();
	packet.mCharacter = mMyCharacter;	// (CHARACTER_NONE = no request, the host keeps the default of the slot)
	mHostConnection.sendPacket(packet, NetConnection::SendFlags::NONE);
}

void NetplayClient::setMyCharacter(uint16 character)
{
	if (mLobbyCharactersLocked)
		return;		// the game has started: the pick is fixed for this session
	if (character == mMyCharacter)
		return;
	mMyCharacter = character;
	RMX_LOG_INFO("Netplay client: lobby: asking the host to play as " << NetplayLobby::getCharacterName(character));
	sendLobbyReady(mLobbyMyReady);
}

bool NetplayClient::takeCharacterNotice()
{
	const bool pending = mCharacterNoticePending;
	mCharacterNoticePending = false;
	return pending;
}

void NetplayClient::sendChat(int quickMessage, const std::string& freeText)
{
	if (mHostConnection.getState() != NetConnection::State::CONNECTED || !mHostVerified)
		return;
	LobbyChatPacket packet;
	if (NetplayLobby::getQuickMessage(quickMessage) != nullptr)
		packet.mQuickMessage = (uint8)quickMessage;
	else
		packet.mText = NetplayLobby::sanitizeChat(freeText);
	if (packet.mQuickMessage == 0 && packet.mText.empty())
		return;
	mHostConnection.sendPacket(packet, NetConnection::SendFlags::NONE);
}

bool NetplayClient::onReceivedPacket(ReceivedPacketEvaluation& evaluation)
{
	// The code check comes first: until the host proved that it knows the code, nothing it sends is believed
	//  -> This is what stops a fake host on the same network (it saw the broadcast, but does not know the code) from sending us a game
	if (evaluation.mPacketType == JoinChallengePacket::PACKET_TYPE)
	{
		JoinChallengePacket packet;
		if (!evaluation.readPacket(packet))
			return false;
		handleJoinChallenge(packet.mHostNonce);
		return true;
	}
	if (evaluation.mPacketType == JoinReplyPacket::PACKET_TYPE)
	{
		JoinReplyPacket packet;
		if (!evaluation.readPacket(packet))
			return false;
		handleJoinReply(packet);
		return true;
	}
	if (!mHostVerified)
		return true;

	switch (evaluation.mPacketType)
	{
		case LobbyStatePacket::PACKET_TYPE:
		{
			LobbyStatePacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			const bool changed = !mLobbyKnown || mLobbyYourSlot != packet.mYourSlot || mLobbyHostStarting != (packet.mHostStarting != 0)
				|| memcmp(mLobbySlotState, packet.mSlotState, sizeof(mLobbySlotState)) != 0;
			mLobbyKnown = true;
			mLobbyYourSlot = packet.mYourSlot;
			mLobbyHostStarting = (packet.mHostStarting != 0);
			memcpy(mLobbySlotState, packet.mSlotState, sizeof(mLobbySlotState));
			bool detailsChanged = (mLobbyDelaySetting != packet.mDelaySetting || mLobbyEffectiveDelay != packet.mEffectiveDelay);
			for (int k = 0; k < 4; ++k)
			{
				const std::string name = NetplayLobby::sanitizeText(packet.mNames[k], NetplayLobby::MAX_NAME_LENGTH);		// (the host cleaned them up already, but nothing is trusted)
				detailsChanged = detailsChanged || (name != mLobbyNames[k]);
				mLobbyNames[k] = name;
				mLobbyPingMs[k] = packet.mPingMs[k];
			}
			mLobbyDelaySetting = packet.mDelaySetting;
			mLobbyEffectiveDelay = packet.mEffectiveDelay;
			{
				// Characters: what the host accepted for everybody
				bool charactersChanged = false;
				for (int k = 0; k < 4; ++k)
				{
					// (nothing is trusted: only a pick that is possible for that slot is taken over)
					uint16 pick = packet.mCharacters[k];
					if (pick != NetplayLobby::CHARACTER_NONE && !NetplayLobby::isCharacterAllowedForSlot(k, pick, mNetplayManager.getAvailableCharacters(), mNetplayManager.getConfiguredPartyExtraSlot(k)) && !NetplayLobby::isExtraCharacter(pick))
						pick = NetplayLobby::CHARACTER_NONE;
					charactersChanged = charactersChanged || (mLobbyCharacters[k] != pick);
					mLobbyCharacters[k] = pick;
				}
				mLobbyCharactersLocked = (packet.mCharactersLocked != 0);
				const bool notice = (packet.mYourSlot < 4) && (((packet.mCharacterNotice >> packet.mYourSlot) & 1) != 0);
				const uint16 mine = (packet.mYourSlot < 4) ? mLobbyCharacters[packet.mYourSlot] : NetplayLobby::CHARACTER_NONE;
				if (mine != NetplayLobby::CHARACTER_NONE)
				{
					if (mMyCharacter == NetplayLobby::CHARACTER_NONE)
						mMyCharacter = mine;		// the default of this slot
					else if (mine != mMyCharacter && (notice || mLobbyCharactersLocked))
						mMyCharacter = mine;		// the host did not accept the request: take over what it says
				}
				if (notice && !mCharacterNoticeSeen)
				{
					mCharacterNoticePending = true;
					RMX_LOG_INFO("Netplay client: lobby: the host changed our character to " << NetplayLobby::getCharacterName(mine));
				}
				mCharacterNoticeSeen = notice;
				if (charactersChanged)
					RMX_LOG_INFO("Netplay client: lobby: characters " << NetplayLobby::getCharacterName(mLobbyCharacters[0]) << "," << NetplayLobby::getCharacterName(mLobbyCharacters[1]) << "," << NetplayLobby::getCharacterName(mLobbyCharacters[2]) << "," << NetplayLobby::getCharacterName(mLobbyCharacters[3]) << (mLobbyCharactersLocked ? " (fixed for this game)" : ""));
			}
			if (!mLobbyPingLogged && packet.mYourSlot < 4 && packet.mPingMs[packet.mYourSlot] != 0xffff)
			{
				mLobbyPingLogged = true;
				RMX_LOG_INFO("Netplay client: lobby: the host measured our ping: " << (int)packet.mPingMs[packet.mYourSlot] << " ms");
			}
			if (packet.mReadyCheckSeconds != 0 && mLobbyReadyCheckSeconds == 0)
				RMX_LOG_INFO("Netplay client: lobby: the host wants to start, ready check running (" << (int)packet.mReadyCheckSeconds << " seconds)");
			mLobbyReadyCheckSeconds = packet.mReadyCheckSeconds;
			if (detailsChanged)
				RMX_LOG_INFO("Netplay client: lobby: names " << mLobbyNames[0] << "," << mLobbyNames[1] << "," << mLobbyNames[2] << "," << mLobbyNames[3] << ", input delay " << NetplayLobby::describeInputDelay(getLobbyDelaySetting(), getLobbyEffectiveDelay()));
			if (changed)
				RMX_LOG_INFO("Netplay client: lobby: you are slot " << (int)(mLobbyYourSlot + 1) << ", players " << (int)mLobbySlotState[0] << (int)mLobbySlotState[1] << (int)mLobbySlotState[2] << (int)mLobbySlotState[3] << (mLobbyHostStarting ? ", the host is picking the game" : ""));
			return true;
		}

		case LobbyChatBroadcastPacket::PACKET_TYPE:
		{
			LobbyChatBroadcastPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			const std::string text = NetplayLobby::sanitizeChat(packet.mText);
			if (!text.empty() && packet.mSlot < 4)
			{
				ChatLine line;
				line.mSlot = packet.mSlot;
				line.mText = text;
				mChatLog.push_back(line);
				while (mChatLog.size() > 8)
					mChatLog.pop_front();
				RMX_LOG_INFO("Netplay client: chat, player " << (int)(packet.mSlot + 1) << " (" << mLobbyNames[packet.mSlot] << "): " << text);
			}
			return true;
		}

		case LobbyPingPacket::PACKET_TYPE:
		{
			LobbyPingPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			LobbyPongPacket pong;
			pong.mToken = packet.mToken;
			mHostConnection.sendPacket(pong, NetConnection::SendFlags::UNRELIABLE);
			return true;
		}

		case LobbyLeavePacket::PACKET_TYPE:
		{
			LobbyLeavePacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			const bool kicked = (packet.mReason == (uint8)NetplayLobby::LeaveReason::KICKED);
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			fail(kicked ? NetplayFailure::Reason::KICKED : NetplayFailure::Reason::LEFT_OUT);
			return true;
		}

		case StartGamePacket::PACKET_TYPE:
		{
			StartGamePacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			startGame(packet);
			return true;
		}

		case SyncCheckPacket::PACKET_TYPE:
		{
			SyncCheckPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			mHostStateHashes[packet.mFrameNumber] = packet.mHash;
			while (mHostStateHashes.size() > 16)
				mHostStateHashes.erase(mHostStateHashes.begin());
			return true;
		}

		case JoinStateChunkPacket::PACKET_TYPE:
		{
			JoinStateChunkPacket packet;
			if (!evaluation.readPacket(packet))
				return false;
			onReceivedJoinStateChunk(packet);
			return true;
		}

		case GameStateIncrementPacket::PACKET_TYPE:
		{
			GameStateIncrementPacket packet;
			if (!evaluation.readPacket(packet))
				return false;

			if (packet.mFrameNumber < mNextFrameNumber)
				return true;

			// Refuse nonsense: no players, no frames, or a jump of many minutes into the future would make every real packet look out of date
			if (packet.mNumPlayers == 0 || packet.mNumFrames == 0 || packet.mInputs.size() != (size_t)packet.mNumPlayers * packet.mNumFrames || packet.mFrameNumber > mNextFrameNumber + 36000)
				return true;

			// Evaluate the packet and store its input history for all players
			const size_t currentFramesGap = packet.mFrameNumber - mNextFrameNumber + 1;
			const size_t numFramesToCopy = std::min<size_t>(currentFramesGap, packet.mNumFrames);
			if (numFramesToCopy < currentFramesGap)
			{
				// Throw away old frames, to not create an actual gap between frames in the queue
				RMX_ERROR("Warning: Complete game state desync!", );
				mReceivedFrames.clear();
			}

			for (int k = 0; k < (int)numFramesToCopy; ++k)
			{
				mReceivedFrames.emplace_back();
				ReceivedFrame& newFrame = mReceivedFrames.back();
				const uint16* input = &packet.mInputs[packet.mNumPlayers * (packet.mNumFrames - numFramesToCopy + k)];
				for (int playerIndex = 0; playerIndex < std::min<int>(packet.mNumPlayers, MAX_PLAYERS); ++playerIndex)
				{
					newFrame.mInputsByPlayer[playerIndex] = *input;
					++input;
				}
				// Player inputs not included in the packet stay at 0

				// Checksum for debugging
				mInputChecksum = rmx::addToFNV1a_32(mInputChecksum, reinterpret_cast<uint8*>(newFrame.mInputsByPlayer), sizeof(newFrame.mInputsByPlayer));
				const int frameNumber = packet.mFrameNumber - (int)numFramesToCopy + k + 1;
				if (frameNumber % 200 == 0)
				{
					mRegularInputChecksum = mInputChecksum;
					mRegularChecksumFrameNumber = frameNumber;
				}
			}

			mNextFrameNumber = packet.mFrameNumber + 1;

			// Only the newest frames are ever needed (the game runs at most a few dozen frames behind the host)
			while (mReceivedFrames.size() > 600)
				mReceivedFrames.pop_front();

			// Update latency
			mCurrentLatency = packet.mFrameNumber - packet.mLastClientReceivedFrame;

			return true;
		}
	}

	return false;
}

bool NetplayClient::onReceivedConnectionlessPacket(ConnectionlessPacketEvaluation& evaluation)
{
	switch (evaluation.mLowLevelSignature)
	{
		case network::PunchthroughConnectionlessPacket::SIGNATURE:
		{
			network::PunchthroughConnectionlessPacket packet;
			if (!packet.serializePacket(evaluation.mSerializer, 1))
				return false;

			mReceivedPunchthroughPacketSender = evaluation.mSenderAddress;
			return true;
		}
	}

	return false;
}

uint32 NetplayClient::getRegularInputChecksum(int& outFrameNumber) const
{
	outFrameNumber = mRegularChecksumFrameNumber;
	return mRegularInputChecksum;
}

void NetplayClient::handleJoinChallenge(uint32 hostNonce)
{
	if (mAuthSent || mHostVerified)
		return;		// One answer per connection

	mHostNonce = hostNonce;
	mClientNonce = NetplayAuth::randomNonce();
	JoinAuthPacket answer;
	answer.mClientNonce = mClientNonce;
	answer.mProof = NetplayAuth::computeProof('C', mAuthSecret, mHostNonce, mClientNonce);
	static const uint32 processToken = std::max<uint32>(NetplayAuth::randomNonce(), 1);
	answer.mClientToken = processToken;
	answer.mPreferredSlot = mResumeSession ? mPlayerIndex : (mLobbyRejoin ? mLobbyYourSlot : 0);		// After a connection loss, ask for the same player slot again
	// Developer test switch: this friend speaks the lobby messages of the version before the character choice (the host must refuse it as another version)
	answer.mLobbyProtocol = (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_OLD_LOBBY_PROTOCOL")) ? (uint8)0 : NetplayLobby::LOBBY_PROTOCOL;
	mHostConnection.sendPacket(answer, NetConnection::SendFlags::NONE);
	mAuthSent = true;
}

void NetplayClient::handleJoinReply(const JoinReplyPacket& packet)
{
	if (!mAuthSent || mHostVerified)
		return;

	switch ((NetplayAuth::JoinResult)packet.mResult)
	{
		case NetplayAuth::JoinResult::ACCEPTED:
		{
			if (!NetplayAuth::verifyHostProof(mAuthSecret, mHostNonce, mClientNonce, packet.mProof))
			{
				// Somebody answered who does not know the code
				mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
				fail(NetplayFailure::Reason::WRONG_CODE, "The other side does not know your code.");
				return;
			}
			if (packet.mLobbyProtocol != NetplayLobby::LOBBY_PROTOCOL)
			{
				// A host of another version of the lobby messages (before the character choice, or a later one): both need the same version
				RMX_LOG_INFO("Netplay client: the host speaks another version of the lobby messages (" << (int)packet.mLobbyProtocol << " instead of " << (int)NetplayLobby::LOBBY_PROTOCOL << ")");
				mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
				fail(NetplayFailure::Reason::VERSION_MISMATCH);
				return;
			}
			mHostVerified = true;
			RMX_LOG_INFO("Netplay client: the host proved the code, player slot " << (int)(packet.mSlot + 1));
			sendLobbyReady(mLobbyMyReady);		// The host learns our name right away
			if (mLobbyRejoin)
			{
				mLobbyRejoin = false;
				mReconnectRemaining = 0.0f;
				mNetplayManager.mLastErrorMessage.clear();
				mLobbyYourSlot = packet.mSlot;
				RMX_LOG_INFO("Netplay client: reconnected to the lobby, back in player slot " << (int)(packet.mSlot + 1));
			}
			// Test hook: press Ready right away
			if (nullptr != TestHooks::getEnv("S3AIR_LOBBY_AUTOREADY"))
				sendLobbyReady(true);
			return;
		}

		case NetplayAuth::JoinResult::LOBBY_FULL:
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			fail(NetplayFailure::Reason::LOBBY_FULL);
			return;

		case NetplayAuth::JoinResult::KICKED:
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			fail(NetplayFailure::Reason::KICKED);
			return;

		case NetplayAuth::JoinResult::VERSION_MISMATCH:
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			fail(NetplayFailure::Reason::VERSION_MISMATCH);
			return;

		default:
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			fail(NetplayFailure::Reason::WRONG_CODE);
			return;
	}
}

void NetplayClient::startGame(const StartGamePacket& packet)
{
	RMX_LOG_INFO("Netplay client: received start game packet");
	if (packet.mPlayerIndex == 0 || packet.mPlayerIndex >= MAX_PLAYERS)
	{
		fail(NetplayFailure::Reason::BAD_GAME_DATA, "The host assigned an invalid player slot.");
		return;
	}
	mPlayerIndex = packet.mPlayerIndex;
	RMX_LOG_INFO("Netplay client: assigned player " << (int)(mPlayerIndex + 1));

	// Both players need the same game version and the same mods, or the two games would not stay identical
	{
		const std::string hostMods(packet.mActiveModsInfo.begin(), packet.mActiveModsInfo.end());
		const std::string ownMods = NetplayManager::getActiveModsInfo();
		// Developer test switches that make this friend look like a different version or mod setup
		const uint32 ownBuild = EngineMain::getDelegate().getAppMetaData().mBuildVersionNumber + ((nullptr != TestHooks::getEnv("S3AIR_NETPLAY_FAKE_BUILD")) ? 1 : 0);
		const std::string ownModsChecked = ownMods + ((nullptr != TestHooks::getEnv("S3AIR_NETPLAY_FAKE_MODS")) ? "+fake" : "");

		NetplayFailure::Reason reason = NetplayFailure::Reason::NONE;
		std::string detail;
		if (packet.mGameBuildVersion != ownBuild)
		{
			reason = NetplayFailure::Reason::VERSION_MISMATCH;
		}
		else if (hostMods != ownModsChecked)
		{
			reason = NetplayFailure::Reason::MOD_MISMATCH;
			detail = "Host: " + (hostMods.empty() ? std::string("none") : hostMods) + "\nYou: " + (ownModsChecked.empty() ? std::string("none") : ownModsChecked);
		}

		if (reason != NetplayFailure::Reason::NONE)
		{
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			fail(reason, detail);
			return;
		}
	}
	// The characters of the players are fixed for this session (the same on every computer; the scripts read them from the shared memory)
	mNetplayManager.setSessionCharacters(packet.mCharacterPicks);

	// Read RNG state from packet
	uint64* rngState = Application::instance().getSimulation().getSimulationState().getRandomNumberGenerator().accessState();
	for (int k = 0; k < 4; ++k)
		rngState[k] = packet.mRNGState[k];

	// Load game settings
	VectorBinarySerializer serializer(true, packet.mSerializedGameSettings);
	EngineMain::getDelegate().serializeGameSettings(serializer);

	// Setup input checksum tracking
	mInputChecksum = rmx::startFNV1a_32();
	mRegularInputChecksum = mInputChecksum;
	mRegularChecksumFrameNumber = 0;

	// A second start game packet while playing means that the host repairs our game state (after the games drifted apart): no restart of the game
	const bool isRepair = (mState == State::GAME_RUNNING || mResumeSession);
	mState = State::GAME_RUNNING;

	if (!isRepair)
		EngineMain::getDelegate().onStartNetplayGame(false);

	mHostStateHashes.clear();
	mOwnStateHashes.clear();
	mAwaitingRepair = false;

	if (packet.mTransferMode == 1)
	{
		// The host's game is already running: we get its game state in a moment, and the game stays paused until then
		//  -> That works because the next frame number is still zero, see "canBeginNextFrame"
		mReceivingJoinState = true;
		mJoinCompressedState.clear();
		mJoinReceivedChunkOffsets.clear();
		mJoinReceivedBytes = 0;
		mJoinFrameNumber = packet.mFirstFrameNumber;
		mNextFrameNumber = 0;
		mReceivedFrames.clear();
	}
}

void NetplayClient::onReceivedJoinStateChunk(const JoinStateChunkPacket& packet)
{
	if (!mReceivingJoinState || packet.mFrameNumber != mJoinFrameNumber)
		return;

	if (mJoinCompressedState.empty())
	{
		// A save state is a few megabytes at most; refuse sizes that would make us allocate gigabytes
		if (packet.mCompressedSize == 0 || packet.mUncompressedSize == 0 || packet.mCompressedSize > 32 * 1024 * 1024 || packet.mUncompressedSize > 128 * 1024 * 1024)
		{
			RMX_LOG_INFO("Netplay client: ignoring a game state with an implausible size");
			return;
		}
		mJoinCompressedState.resize(packet.mCompressedSize);
		mJoinUncompressedSize = packet.mUncompressedSize;
		for (int k = 0; k < 4; ++k)
			mJoinLastInputs[k] = packet.mLastInputs[k];
		mJoinScreenWidth = packet.mScreenWidth;
		mJoinScreenHeight = packet.mScreenHeight;
	}

	if (packet.mCompressedSize != mJoinCompressedState.size() || (size_t)packet.mOffset + packet.mData.size() > mJoinCompressedState.size())
		return;
	if (!mJoinReceivedChunkOffsets.insert(packet.mOffset).second)
		return;		// Duplicate

	memcpy(&mJoinCompressedState[packet.mOffset], packet.mData.data(), packet.mData.size());
	mJoinReceivedBytes += (uint32)packet.mData.size();

	if (mJoinReceivedBytes >= mJoinCompressedState.size())
		finishJoinTransfer();
}

void NetplayClient::finishJoinTransfer()
{
	std::vector<uint8> state;
	// The output size is known in advance: a damaged or malicious stream must not be able to expand into gigabytes
	if (!ZlibDeflate::decode(state, mJoinCompressedState.data(), mJoinCompressedState.size(), mJoinUncompressedSize) || state.size() != mJoinUncompressedSize)
	{
		RMX_LOG_INFO("Netplay client: received game state is invalid");
		mReceivingJoinState = false;
		mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
		fail(NetplayFailure::Reason::BAD_GAME_DATA);
		return;
	}

	Simulation& simulation = Application::instance().getSimulation();
	if (!simulation.loadStateFromBuffer(state, mJoinFrameNumber))
	{
		RMX_LOG_INFO("Netplay client: failed to load the game state from the host");
		mReceivingJoinState = false;
		mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
		fail(NetplayFailure::Reason::BAD_GAME_DATA);
		return;
	}

	// The game scripts depend on the render resolution, so it must be the same as on the host
	//  -> When coming from the menus, it would still be the menu resolution
	if (mJoinScreenWidth != 0 && mJoinScreenHeight != 0)
		VideoOut::instance().setScreenSize(mJoinScreenWidth, mJoinScreenHeight);

	// Make the input state of the previous frame the same as for the host
	ControlsIn& controlsIn = ControlsIn::instance();
	for (int k = 0; k < 4; ++k)
		controlsIn.injectInput(k, mJoinLastInputs[k]);

	// The host's inputs for the current frame arrive in the next packet
	mNextFrameNumber = mJoinFrameNumber;
	mReceivedFrames.clear();
	simulation.resetTargetFrameToCurrent();

	mReceivingJoinState = false;
	mJoinCompressedState.clear();
	mJoinCompressedState.shrink_to_fit();
	if (mResumeSession)
	{
		mResumeSession = false;
		mReconnectRemaining = 0.0f;
		mNetplayManager.mLastErrorMessage.clear();
		RMX_LOG_INFO("Netplay client: reconnected and restored the existing session");
	}
	RMX_LOG_INFO("Netplay client: game state loaded, now at frame " << mJoinFrameNumber);

	JoinStateLoadedPacket loadedPacket;
	loadedPacket.mFrameNumber = mJoinFrameNumber;
	mHostConnection.sendPacket(loadedPacket, NetConnection::SendFlags::NONE);
}
