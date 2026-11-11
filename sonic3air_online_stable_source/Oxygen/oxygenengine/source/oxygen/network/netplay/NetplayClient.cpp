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

	// TODO: This just destroys the connection, but doesn't tell the host about it
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
	mHostConnection.startConnectTo(mConnectionManager, SocketAddress(ip, port));
	mState = State::CONNECT_TO_HOST;
}

void NetplayClient::updateConnection(float deltaSeconds)
{
	EngineServerClient& engineServerClient = EngineServerClient::instance();
	if (!mReconnectTestTriggered && nullptr != getenv("S3AIR_NETPLAY_RECONNECT_TEST") && mState == State::GAME_RUNNING && !mReceivingJoinState && mStateTimer > 12.0f)
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
		mHostConnection.startConnectTo(mConnectionManager, mReconnectAddress);
		mState = State::CONNECT_TO_HOST;
		mTimedState = State::NONE;
		mStateTimer = 0.0f;
		mNetplayManager.mLastErrorMessage = "Connection lost. Reconnecting to your game...";
		RMX_LOG_INFO("Netplay client: reconnecting to the existing session");
	}
	if (mReconnectRemaining > 0.0f)
	{
		mReconnectRemaining -= deltaSeconds;
		if (mReconnectRemaining <= 0.0f)
		{
			mReconnectRemaining = 0.0f;
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			mState = State::FAILED;
			mNetplayManager.mLastErrorMessage = "Could not reconnect within 90 seconds. Join again with the host's code.";
			RMX_LOG_INFO("Netplay client: reconnect timed out");
			return;
		}
		if (mState == State::CONNECT_TO_HOST && (mStateTimer > 5.0f || mHostConnection.getState() == NetConnection::State::DISCONNECTED))
		{
			mHostConnection.clear();
			mHostConnection.startConnectTo(mConnectionManager, mReconnectAddress);
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
	const char* timeoutError = nullptr;
	if (mState == State::CONNECT_TO_SERVER && mStateTimer > 15.0f)
		timeoutError = "Could not reach the matchmaking server.\nCheck your internet connection, or use the VPN option.";
	else if (mState == State::REGISTERED && mStateTimer > 30.0f)
		timeoutError = "Nobody is hosting with this code.\nCheck the code, and that the host pressed Host.";
	else if (mState == State::CONNECT_TO_HOST && mStateTimer > 20.0f && mReconnectRemaining == 0.0f)
		timeoutError = "Found the host, but could not connect.\nOne of your networks blocks direct connections: try the VPN option.";
	if (nullptr != timeoutError)
	{
		RMX_LOG_INFO("Netplay client: giving up, " << timeoutError);
		mNetplayManager.mLastErrorMessage = timeoutError;
		mState = State::FAILED;
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
					RMX_LOG_INFO("Netplay client: giving up, nobody is hosting with this code (the game server does not know the session)");
					mNetplayManager.mLastErrorMessage = "Nobody is hosting with this code.\nCheck the code, and that the host pressed Host.";
					mState = State::FAILED;
					break;
				}
			}
			break;
		}

		case State::CONNECT_TO_HOST:
		{
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

				// Test hook: press Ready right away
				if (nullptr != getenv("S3AIR_LOBBY_AUTOREADY"))
					sendLobbyReady(true);
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
			mHostConnection.startConnectTo(mConnectionManager, SocketAddress(packet.mConnectToIP, packet.mConnectToPort));
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
	if (mHostConnection.getState() != NetConnection::State::CONNECTED)
		return;

	// Compare our game state with what the host reported for the same frame
	if (mState == State::GAME_RUNNING && !mReceivingJoinState && (frameNumber % 60) == 0)
	{
		const uint32 ownHash = NetplayManager::computeStateHash(frameNumber);

		static const bool corruptForTest = (nullptr != getenv("S3AIR_NETPLAY_CORRUPT"));
		static bool corrupted = false;
		if (corruptForTest && !corrupted && frameNumber >= mJoinFrameNumber + 300 && mJoinFrameNumber != 0)
		{
			// Developer test: make our game drift away on purpose
			corrupted = true;
			Application::instance().getSimulation().getEmulatorInterface().getRam()[0xb010] ^= 0x40;
			RMX_LOG_INFO("Netplay client: (test) changed the game state on purpose at frame " << frameNumber);
		}

		if (nullptr != getenv("S3AIR_NETPLAY_DUMP"))
		{
			FILE* f = fopen((std::string(getenv("APPDATA")) + "/Sonic3AIR/client_compare.txt").c_str(), "a");
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
		if (nullptr != getenv("S3AIR_CHECKS"))
			if (const char* testInput = getenv("S3AIR_NETPLAY_TEST_INPUT"))
				input = (uint16)strtoul(testInput, nullptr, 0);
		packet.mInputs.push_back(input);
		{
			static uint16 lastSent = 0;
			if (packet.mInputs.back() != lastSent && nullptr != getenv("S3AIR_NETPLAY_DEBUG"))
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
	LobbyReadyPacket packet;
	packet.mReady = ready ? 1 : 0;
	mHostConnection.sendPacket(packet, NetConnection::SendFlags::NONE);
}

bool NetplayClient::onReceivedPacket(ReceivedPacketEvaluation& evaluation)
{
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
			if (changed)
				RMX_LOG_INFO("Netplay client: lobby: you are slot " << (int)(mLobbyYourSlot + 1) << ", players " << (int)mLobbySlotState[0] << (int)mLobbySlotState[1] << (int)mLobbySlotState[2] << (int)mLobbySlotState[3] << (mLobbyHostStarting ? ", the host is picking the game" : ""));
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

void NetplayClient::startGame(const StartGamePacket& packet)
{
	RMX_LOG_INFO("Netplay client: received start game packet");
	if (packet.mPlayerIndex == 0 || packet.mPlayerIndex >= MAX_PLAYERS)
	{
		mState = State::FAILED;
		mNetplayManager.mLastErrorMessage = "The host assigned an invalid player slot.";
		return;
	}
	mPlayerIndex = packet.mPlayerIndex;
	RMX_LOG_INFO("Netplay client: assigned player " << (int)(mPlayerIndex + 1));

	// Both players need the same game version and the same mods, or the two games would not stay identical
	{
		const std::string hostMods(packet.mActiveModsInfo.begin(), packet.mActiveModsInfo.end());
		const std::string ownMods = NetplayManager::getActiveModsInfo();
		std::string error;
		if (packet.mGameBuildVersion != EngineMain::getDelegate().getAppMetaData().mBuildVersionNumber)
			error = "The host uses a different game version.";
		else if (hostMods != ownMods)
			error = "The host uses different mods, a different mod order, or a different build of the game.\nHost: " + (hostMods.empty() ? std::string("none") : hostMods) + "\nYou: " + (ownMods.empty() ? std::string("none") : ownMods);

		if (!error.empty())
		{
			RMX_LOG_INFO("Netplay client: cannot join, " << error);
			mNetplayManager.mLastErrorMessage = error;
			mState = State::FAILED;
			mHostConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			return;
		}
	}
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
		if (packet.mCompressedSize == 0 || packet.mCompressedSize > 32 * 1024 * 1024 || packet.mUncompressedSize > 128 * 1024 * 1024)
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
	if (!ZlibDeflate::decode(state, mJoinCompressedState.data(), mJoinCompressedState.size()) || state.size() != mJoinUncompressedSize)
	{
		RMX_ERROR("Netplay client: received game state is invalid", );
		mReceivingJoinState = false;
		return;
	}

	Simulation& simulation = Application::instance().getSimulation();
	if (!simulation.loadStateFromBuffer(state, mJoinFrameNumber))
	{
		RMX_ERROR("Netplay client: failed to load the game state from the host", );
		mReceivingJoinState = false;
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
