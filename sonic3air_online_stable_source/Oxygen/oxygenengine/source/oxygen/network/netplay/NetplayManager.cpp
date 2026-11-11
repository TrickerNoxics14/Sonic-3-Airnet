/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/network/netplay/NetplayManager.h"
#include "oxygen/network/netplay/NetplayClient.h"
#include "oxygen/network/netplay/NetplayHost.h"
#include "oxygen/network/netplay/NetplayModFingerprint.h"
#include "oxygen/network/netplay/NetplayAuth.h"
#include "oxygen/network/EngineServerClient.h"
#include "oxygen/application/Application.h"
#include "oxygen/application/modding/ModManager.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/Simulation.h"

#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen_netcore/serverclient/Packets.h"
#include "oxygen_netcore/serverclient/ProtocolVersion.h"


#define USE_IPv6 false


NetplayManager::NetplayManager() :
	mConnectionManager(&mUDPSocket, nullptr, *this, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE),
	mExternalAddressQuery(mConnectionManager, USE_IPv6)
{
	mUseIPv6 = USE_IPv6;

#if defined(DEBUG) && 0
	// Just for testing / debugging
	mConnectionManager.mDebugSettings.mSendingPacketLoss = 0.0f;
	mConnectionManager.mDebugSettings.mReceivingPacketLoss = 0.0f;
	mConnectionManager.mDebugSettings.mReceivingDelayAverage = 0.0f;
	mConnectionManager.mDebugSettings.mReceivingDelayVariance = 0.0f;
#endif
}

NetplayManager::~NetplayManager()
{
	closeConnections();
}

bool NetplayManager::setupAsHost(bool registerSessionAtServer, uint16 port, bool directSecret)
{
	if (!restartConnection(true, port) || nullptr == mNetplayHost)
		return false;
	clearFailure();
	mTestCloseTimer = 0.0f;

	// Everybody who connects has to prove that they know the code: the secret comes either from the join code (direct IP connections) or from the session code
	mHostSecret = directSecret ? NetplayAuth::makeRandomSecret() : NetplayAuth::secretFromSession(mSessionID);

	// A friend in the same network finds us directly with the code, without the game server
	//  -> Only a one way hash of the session ID goes over the network, so that somebody who merely listens to the broadcast does not learn the code
	if (!directSecret)
		mUDPSocket.setLanHostSession(NetplayAuth::lanTagFromSession(mSessionID));

	if (registerSessionAtServer)
	{
		mNetplayHost->registerAtServer();
	}
	return true;
}

bool NetplayManager::setupAsHostWithSecret(uint16 port, uint32 secret)
{
	if (!setupAsHost(false, port, true))
		return false;
	mHostSecret = secret;
	return true;
}

void NetplayManager::startJoinViaServer()
{
	if (!restartConnection(false) || nullptr == mNetplayClient)
		return;
	clearFailure();

	mNetplayClient->setAuthSecret(NetplayAuth::secretFromSession(mSessionID));
	mUDPSocket.setLanSearchSession(NetplayAuth::lanTagFromSession(mSessionID));
	mLanSearchTimer = 0.0f;
	mNetplayClient->joinViaServer();
}

void NetplayManager::startJoinDirect(std::string_view ip, uint16 port, uint32 secret)
{
	if (!restartConnection(false) || nullptr == mNetplayClient)
		return;
	clearFailure();

	mNetplayClient->setAuthSecret(secret);
	mNetplayClient->connectDirectlyToHost(ip, port);
}

void NetplayManager::reportFailure(NetplayFailure::Reason reason, const std::string& detail)
{
	mLastFailure = reason;
	mLastErrorMessage = NetplayFailure::getMessage(reason);
	if (!detail.empty())
		mLastErrorMessage += std::string("\n") + detail;
	RMX_LOG_INFO("Netplay failure: " << NetplayFailure::getName(reason) << (detail.empty() ? "" : " (") << detail << (detail.empty() ? "" : ")"));
}

void NetplayManager::clearFailure()
{
	mLastFailure = NetplayFailure::Reason::NONE;
	mLastErrorMessage.clear();
}

void NetplayManager::closeConnections()
{
	mUDPSocket.clearLan();
	SAFE_DELETE(mNetplayHost);
	SAFE_DELETE(mNetplayClient);
	mExternalAddressQuery.reset();
	mUDPSocket.close();
}


// A short log of the online session, to find out what went wrong when something did (it is written next to the other game logs)
void NetplayManager::writeOnlineLog(float deltaSeconds)
{
	if (nullptr == mNetplayHost && nullptr == mNetplayClient)
		return;

	mOnlineLogTimer += deltaSeconds;
	if (mOnlineLogTimer < 5.0f)
		return;
	const float elapsed = mOnlineLogTimer;
	mOnlineLogTimer = 0.0f;

	const char* appdata = getenv("APPDATA");
	// Same-PC tests run two processes. Give them the same role suffix already
	// used by their main logs; otherwise each process truncates/overwrites the
	// other process's buffered stdio writes. Normal players keep online_log.txt.
	// (The suffix is a test switch: only honoured when the test hooks are enabled, and only plain file name characters.)
	std::string logName;
	if (const char* suffix = TestHooks::getEnv("S3AIR_LOGNAME"))
	{
		for (const char* p = suffix; *p != 0; ++p)
			if (isalnum((unsigned char)*p) || *p == '_' || *p == '-')
				logName += *p;
	}
	const std::string path = std::string(appdata ? appdata : ".") + "/Sonic3AIR/online_log" + logName + ".txt";
	FILE* file = fopen(path.c_str(), mOnlineLogStarted ? "a" : "w");
	if (nullptr == file)
		return;
	mOnlineLogStarted = true;

	const uint32 frame = Application::instance().getSimulation().getFrameNumber();
	const float fps = (float)(frame - mOnlineLogLastFrame) / elapsed;
	mOnlineLogLastFrame = frame;

	if (nullptr != mNetplayClient)
	{
		fprintf(file, "[friend] t=%us state=%d frame=%u fps=%.1f delay=%d frames, repairs=%d, last sync ok at frame %u, receiving game=%d\n",
			(unsigned)(SDL_GetTicks() / 1000), (int)mNetplayClient->getState(), (unsigned)frame, fps, mNetplayClient->getCurrentLatency(),
			mNetplayClient->getDesyncCount(), (unsigned)mNetplayClient->getLastSyncOkFrame(), mNetplayClient->isReceivingGameState() ? 1 : 0);
	}
	else
	{
		int latency = 0;
		int connected = 0;
		for (const auto* player : mNetplayHost->getPlayerConnections())
		{
			if (player->getState() == NetConnection::State::CONNECTED)
			{
				++connected;
				latency = std::max(latency, player->mCurrentLatency);
			}
		}
		fprintf(file, "[host] t=%us state=%d frame=%u fps=%.1f friends=%d, delay=%d frames, repairs=%d\n",
			(unsigned)(SDL_GetTicks() / 1000), (int)mNetplayHost->getHostState(), (unsigned)frame, fps, connected, latency, mNetplayHost->getResyncCount());
	}
	fclose(file);
}

void NetplayManager::updateConnections(float deltaSeconds)
{
	writeOnlineLog(deltaSeconds);
	if (!mUDPSocket.isValid())
		return;

	mConnectionManager.updateConnectionManager();

	mExternalAddressQuery.updateQuery(deltaSeconds);

	if (nullptr != mNetplayHost)
	{
		mNetplayHost->updateConnection(deltaSeconds);

		// Developer test: the host closes its lobby a few seconds after the first friend got in, to see how the friend reacts
		if (const char* closeAfter = TestHooks::getEnv("S3AIR_NETPLAY_CLOSE_AFTER"))
		{
			if (mNetplayHost->getAuthenticatedCount() > 0)
			{
				mTestCloseTimer += deltaSeconds;
				if (mTestCloseTimer > (float)atoi(closeAfter))
				{
					RMX_LOG_INFO("Netplay host: (test) closing the lobby on purpose");
					closeConnections();
					return;
				}
			}
		}
	}
	// Joining with a code: first look for the host in the same network (that is the one case where the game server cannot help)
	if (nullptr != mNetplayClient && (mNetplayClient->getState() == NetplayClient::State::CONNECT_TO_SERVER || mNetplayClient->getState() == NetplayClient::State::REGISTERED))
	{
		SocketAddress found;
		if (mUDPSocket.takeLanFoundAddress(found))
		{
			RMX_LOG_INFO("Netplay client: found the host in the same network");
			mNetplayClient->connectDirectlyToHost(found.getIP(), found.getPort());
		}
		else
		{
			mLanSearchTimer -= deltaSeconds;
			if (mLanSearchTimer <= 0.0f)
			{
				mLanSearchTimer = 0.7f;
				mUDPSocket.sendLanSearchRequest(DEFAULT_HOST_PORT);
			}
		}
	}

	if (nullptr != mNetplayClient)
	{
		mNetplayClient->updateConnection(deltaSeconds);
	}
}

bool NetplayManager::onReceivedGameServerPacket(ReceivedPacketEvaluation& evaluation)
{
	if (nullptr != mNetplayHost)
	{
		if (mNetplayHost->onReceivedGameServerPacket(evaluation))
			return true;
	}
	if (nullptr != mNetplayClient)
	{
		if (mNetplayClient->onReceivedGameServerPacket(evaluation))
			return true;
	}
	return false;
}

bool NetplayManager::canBeginNextFrame(uint32 frameNumber)
{
	if (nullptr != mNetplayHost)
	{
		return mNetplayHost->canBeginNextFrame(frameNumber);
	}
	if (nullptr != mNetplayClient)
	{
		return mNetplayClient->canBeginNextFrame(frameNumber);
	}
	return true;
}

namespace
{
	// Developer debugging: with S3AIR_NETPLAY_DEBUG set, a fingerprint of the game memory gets written to a file every few frames
	//  -> Comparing the files of the host and the client shows whether (and when) their games stopped being identical
	void writeDebugFingerprint(bool isHost, uint32 frameNumber)
	{
		static const bool enabled = (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DEBUG"));
		if (!enabled || (frameNumber % 30) != 0)
			return;

		const uint8* ram = Application::instance().getSimulation().getEmulatorInterface().getRam();
		uint32 hash = rmx::startFNV1a_32();
		hash = rmx::addToFNV1a_32(hash, ram, 0x10000);

		if (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DUMP"))
		{
			const std::string dumpPath = std::string(getenv("APPDATA") ? getenv("APPDATA") : ".") + "/Sonic3AIR/dbg_" + (isHost ? "host_" : "client_") + std::to_string(frameNumber) + ".bin";
			FILE* dumpFile = fopen(dumpPath.c_str(), "wb");
			if (nullptr != dumpFile) { fwrite(ram, 1, 0x10000, dumpFile); fclose(dumpFile); }
		}

		static FILE* file = nullptr;
		static bool fileIsHost = false;
		if (nullptr == file || fileIsHost != isHost)
		{
			if (nullptr != file)
				fclose(file);
			const std::string path = std::string(getenv("APPDATA") ? getenv("APPDATA") : ".") + (isHost ? "\\Sonic3AIR\\netsync_host.txt" : "\\Sonic3AIR\\netsync_client.txt");
			file = fopen(path.c_str(), "a");
			fileIsHost = isHost;
		}
		if (nullptr != file)
		{
			fprintf(file, "%u %08x\n", frameNumber, hash);
			fflush(file);
		}
	}
}

void NetplayManager::onFrameUpdate(ControlsIn& controlsIn, uint32 frameNumber)
{
	// Tell the game scripts about the role of this computer: 0 = not playing online, 1 = host (shows player 1), 2 = friend (shows player 2)
	//  -> This is a place in the shared memory, so it is not part of the game state that gets compared between the computers
	{
		uint8 role = 0;
		if (nullptr != mNetplayClient && mNetplayClient->getState() == NetplayClient::State::GAME_RUNNING)
			role = mNetplayClient->getPlayerIndex() + 1;
		else if (nullptr != mNetplayHost && mNetplayHost->getHostState() == NetplayHost::HostState::GAME_RUNNING)
			role = 1;
		// Developer test switch: pretend to be online with a certain role
		static const char* forced = TestHooks::getEnv("S3AIR_FORCE_ROLE");
		if (nullptr != forced)
			role = (uint8)atoi(forced);
		Application::instance().getSimulation().getEmulatorInterface().writeMemory8(ROLE_ADDRESS, role);
	}

	if (nullptr != mNetplayHost || nullptr != mNetplayClient)
	{
		writeDebugFingerprint(nullptr != mNetplayHost, frameNumber);
	}

	if (nullptr != mNetplayHost)
	{
		mNetplayHost->onFrameUpdate(controlsIn, frameNumber);
	}
	if (nullptr != mNetplayClient)
	{
		mNetplayClient->onFrameUpdate(controlsIn, frameNumber);
	}
}

NetConnection* NetplayManager::createNetConnection(ConnectionManager& connectionManager, const SocketAddress& senderAddress)
{
	if (nullptr != mNetplayHost)
	{
		// Accept incoming connection
		return mNetplayHost->createNetConnection(senderAddress);
	}
	else
	{
		// Reject connection
		return nullptr;
	}
}

void NetplayManager::destroyNetConnection(NetConnection& connection)
{
	if (nullptr != mNetplayHost)
	{
		mNetplayHost->destroyNetConnection(connection);
	}

	delete &connection;
}

bool NetplayManager::onReceivedConnectionlessPacket(ConnectionlessPacketEvaluation& evaluation)
{
	if (mExternalAddressQuery.onReceivedConnectionlessPacket(evaluation))
		return true;

	if (nullptr != mNetplayClient)
	{
		mNetplayClient->onReceivedConnectionlessPacket(evaluation);
	}

	return false;
}

bool NetplayManager::onReceivedPacket(ReceivedPacketEvaluation& evaluation)
{
	if (nullptr != mNetplayHost)
	{
		return mNetplayHost->onReceivedPacket(evaluation);
	}
	if (nullptr != mNetplayClient)
	{
		return mNetplayClient->onReceivedPacket(evaluation);
	}
	return false;
}

bool NetplayManager::restartConnection(bool asHost, uint16 hostPort)
{
	closeConnections();
	Sockets::startupSockets();

	const Sockets::ProtocolFamily protocolFamily = mUseIPv6 ? Sockets::ProtocolFamily::IPv6 : Sockets::ProtocolFamily::IPv4;
	if (asHost && hostPort != 0)
	{
		if (!mUDPSocket.bindToPort(hostPort, protocolFamily))
		{
			RMX_ASSERT(false, "UDP socket bind to port " << hostPort << " failed");
			return false;
		}
	}
	else
	{
		if (!mUDPSocket.bindToAnyPort(protocolFamily))
		{
			RMX_ERROR("Socket bind to any port failed", );
			return false;
		}
	}

	if (asHost)
	{
		mNetplayHost = new NetplayHost(mConnectionManager, *this);
	}
	else
	{
		mNetplayClient = new NetplayClient(mConnectionManager, *this);
	}

	mExternalAddressQuery.startQuery();
	return true;
}


std::string NetplayManager::getActiveModsInfo()
{
	std::string result;
	for (const Mod* mod : ModManager::instance().getActiveMods())
	{
		// Mod versions are not always bumped when a mod is edited locally. Include
		// the active files and setting values so peers fail before the simulation
		// diverges when their character or level mod copies are different.
		std::vector<rmx::FileIO::FileEntry> files;
		FTX::FileSystem->listFilesByMask(mod->mFullPath + L"/*", true, files);
		std::sort(files.begin(), files.end(), [](const rmx::FileIO::FileEntry& a, const rmx::FileIO::FileEntry& b)
		{
			return (a.mPath + a.mFilename) < (b.mPath + b.mFilename);
		});

		std::vector<NetplayModFingerprint::File> fingerprintFiles;
		for (const rmx::FileIO::FileEntry& file : files)
		{
			std::wstring relativePath = file.mPath;
			if (relativePath.compare(0, mod->mFullPath.size(), mod->mFullPath) == 0)
				relativePath.erase(0, mod->mFullPath.size());
			relativePath += file.mFilename;
			const std::string relativePathUtf8 = WString(relativePath).toStdString();
			NetplayModFingerprint::File& fingerprintFile = fingerprintFiles.emplace_back();
			fingerprintFile.mRelativePath = relativePathUtf8;
			fingerprintFile.mReadable = FTX::FileSystem->readFile(file.mPath + file.mFilename, fingerprintFile.mContent);
		}

		const uint64 contentHash = NetplayModFingerprint::hashFiles(std::move(fingerprintFiles));
		result += mod->mUniqueID + "@" + mod->mModVersion + "#" + rmx::hexString(contentHash, 16);
		for (const Mod::SettingCategory& category : mod->mSettingCategories)
		{
			for (const Mod::Setting& setting : category.mSettings)
			{
				result += "/" + setting.mIdentifier + "=" + std::to_string(setting.mCurrentValue);
			}
		}
		result += ";";
	}
	// Two different builds of the game must not play together either
	result += std::string("build:") + __DATE__ + " " + __TIME__;
	return result;
}


uint32 NetplayManager::computeStateHash(uint32 frameNumber)
{
	const uint8* ram = Application::instance().getSimulation().getEmulatorInterface().getRam();
	static const bool dump = (nullptr != TestHooks::getEnv("S3AIR_NETPLAY_DUMP"));
	if (dump && frameNumber != 0)
	{
		// Developer debugging: write the game memory to a file, to find out which memory differs between host and client
		const char* appdata = getenv("APPDATA");
		const std::string path = std::string(appdata ? appdata : ".") + "/Sonic3AIR/ram_" + ((TestHooks::getEnv("S3AIR_NETPLAY") && 0 == strncmp(TestHooks::getEnv("S3AIR_NETPLAY"), "join", 4)) ? "client_" : "host_") + std::to_string(frameNumber) + ".bin";
		FILE* f = fopen(path.c_str(), "wb");
		if (nullptr != f) { fwrite(ram, 1, 0x10000, f); fclose(f); }
		uint32 nz = 0;
		for (int i = 0; i < 0x10000; ++i) nz += (ram[i] != 0);
		uint32 h2 = rmx::addToFNV1a_32(rmx::startFNV1a_32(), ram, 0x10000);
		FILE* g = fopen((std::string(appdata ? appdata : ".") + "/Sonic3AIR/hashlog_" + (TestHooks::getEnv("S3AIR_NETPLAY") && 0 == strncmp(TestHooks::getEnv("S3AIR_NETPLAY"), "join", 4) ? "client" : "host") + ".txt").c_str(), "a");
		if (nullptr != g) { fprintf(g, "frame %u nz %u hash %08x\n", frameNumber, nz, h2); fclose(g); }
	}
	uint32 hash = rmx::startFNV1a_32();
	return rmx::addToFNV1a_32(hash, ram, 0x10000);
}

