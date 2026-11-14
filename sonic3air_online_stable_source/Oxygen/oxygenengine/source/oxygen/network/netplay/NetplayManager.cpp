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
#include "oxygen/application/Configuration.h"
#include "oxygen/network/EngineServerClient.h"
#include "oxygen/application/Application.h"
#include "oxygen/application/modding/ModManager.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/Simulation.h"
#include "oxygen/simulation/CodeExec.h"
#include "oxygen/simulation/LemonScriptRuntime.h"

#ifndef _WIN32
	// The MSVC names of the case-insensitive string compares (used below) do not exist on other platforms, e.g. the web build
	#include <strings.h>
	#define _stricmp strcasecmp
	#define _strnicmp strncasecmp
#endif

#ifdef PLATFORM_WEB
	#include <emscripten.h>
#endif

#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen_netcore/network/RelayProtocol.h"
#include "oxygen_netcore/serverclient/Packets.h"
#include "oxygen_netcore/serverclient/ProtocolVersion.h"


#define USE_IPv6 false

// The same text in every platform's build of the same game version, see getActiveModsInfo
static const char* const NETPLAY_COMPATIBILITY_ID = "airnet-1";


NetplayManager::NetplayManager() :
	mConnectionManager(&mUDPSocket, nullptr, *this, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE),
	mExternalAddressQuery(mConnectionManager, USE_IPv6)
{
	mUseIPv6 = USE_IPv6;

	// Developer test: the host's input delay ("auto" or a number of frames)
	if (const char* delay = TestHooks::getEnv("S3AIR_NETPLAY_DELAY"))
		mInputDelaySetting = (_stricmp(delay, "auto") == 0) ? NetplayLobby::INPUT_DELAY_AUTO : NetplayLobby::clampDelaySetting(atoi(delay));

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
	mNetplayHost->setInputDelaySetting(mInputDelaySetting);
	if (const uint16 testPick = getCharacterPickFromEnvironment(); testPick != NetplayLobby::CHARACTER_NONE)
		setHostCharacterPick(testPick);		// Developer test switch: the host picks without a key press
	clearFailure();
	mTestCloseTimer = 0.0f;

	// Everybody who connects has to prove that they know the code: the secret comes either from the join code (direct IP connections) or from the session code
	mHostSecret = directSecret ? NetplayAuth::makeRandomSecret() : NetplayAuth::secretFromSession(mSessionID);
	NetplayAuth::prepareSecret(mHostSecret);		// The slow key stretching happens now, once, not when the first friend connects

	// A friend in the same network finds us directly with the code, without the game server
	//  -> Only a one way hash of the session ID goes over the network, so that somebody who merely listens to the broadcast does not learn the code
	if (!directSecret)
		mUDPSocket.setLanHostSession(NetplayAuth::lanTagFromSession(mSessionID));

	// A relay server is only used if the player set one up: friends who cannot reach this host directly can then still join through it
	if (!directSecret && isRelayConfigured())
		startRelay(UDPSocket::RelayRole::HOST);

	if (registerSessionAtServer)
	{
		mNetplayHost->registerAtServer();
	}
	return true;
}

std::string NetplayManager::getRelayServerSetting() const
{
	// Developer test: a relay without touching the settings file
	if (const char* testRelay = TestHooks::getEnv("S3AIR_RELAY"))
		return testRelay;
#ifdef PLATFORM_WEB
	// Browser: the address in the page's link (...?relay=wss://relay.example.org) wins, then the one saved in the settings (typed in the lobby),
	// then the default that the web page's owner put into relay_config.js (S3AIR_RELAY_DEFAULT)
	static uint32 sCacheTicks = 0;
	static std::string sCache;
	static bool sCacheValid = false;
	if (sCacheValid && SDL_GetTicks() - sCacheTicks < 500)
		return sCache;		// (asking the page runs a bit of JavaScript: not for every frame)

	auto askPage = [](const char* script) -> std::string
	{
		const char* result = emscripten_run_script_string(script);
		return (nullptr != result) ? std::string(result) : std::string();
	};
	std::string result = askPage("(function(){try{return new URLSearchParams(location.search).get('relay')||'';}catch(e){return '';}})()");
	if (result.empty())
		result = Configuration::instance().mOnlineRelayServer;
	if (result.empty())
		result = askPage("(function(){try{return (typeof S3AIR_RELAY_DEFAULT==='string')?S3AIR_RELAY_DEFAULT:'';}catch(e){return '';}})()");
	sCache = result;
	sCacheTicks = SDL_GetTicks();
	sCacheValid = true;
	return result;
#else
	return Configuration::instance().mOnlineRelayServer;
#endif
}

void NetplayManager::setRelayServerSetting(const std::string& text)
{
	Configuration& configuration = Configuration::instance();
	if (configuration.mOnlineRelayServer == text)
		return;
	configuration.mOnlineRelayServer = text;
	configuration.saveSettings();
	RMX_LOG_INFO("Netplay: the relay server setting was changed" << (text.empty() ? " (cleared)" : ""));
}

std::string NetplayManager::getRelayDisplayName() const
{
	// Short text for the lobby: the host name only (never anything that looks like a password or a path)
	std::string text = getRelayServerSetting();
#ifdef PLATFORM_WEB
	std::string url;
	if (NetplayLobby::normalizeRelayWebSocketURL(text, url))
	{
		const size_t start = url.find("://") + 3;
		const size_t end = url.find_first_of("/?", start);
		return url.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
	}
	return std::string();
#else
	return text;
#endif
}

bool NetplayManager::resolveRelayAddress(SocketAddress& outAddress) const
{
#ifdef PLATFORM_WEB
	// Browser: the relay is reached by a WebSocket URL, there is nothing to resolve (and no address that could be used for anything else)
	std::string url;
	if (!NetplayLobby::normalizeRelayWebSocketURL(getRelayServerSetting(), url))
		return false;
	UDPSocket::setRelayWebSocketURL(url);
	outAddress.set("websocket.relay", 1);
	return true;
#endif

	// "host" or "host:port" (a name or an IPv4 address)
	std::string text = getRelayServerSetting();
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
		text.pop_back();
	size_t start = 0;
	while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
		++start;
	text = text.substr(start);
	if (text.empty())
		return false;

	std::string host = text;
	int port = RelayProtocol::DEFAULT_PORT;
	const size_t colon = text.rfind(':');
	if (colon != std::string::npos)
	{
		host = text.substr(0, colon);
		port = atoi(text.c_str() + colon + 1);
	}
	if (host.empty() || port <= 0 || port > 65535)
		return false;
	for (const char c : host)
	{
		if (!(isalnum((unsigned char)c) || c == '.' || c == '-'))
			return false;
	}

	std::string ip;
	if (!Sockets::resolveToIP(host, ip, false))
		return false;
	outAddress.set(ip, (uint16)port);
	return true;
}

bool NetplayManager::isRelayConfigured() const
{
	return !getRelayServerSetting().empty();
}

bool NetplayManager::startRelay(UDPSocket::RelayRole role)
{
	SocketAddress address;
	if (!resolveRelayAddress(address))
	{
		RMX_LOG_INFO("Netplay: the relay server setting is not usable (not a valid name or address), no relay is used");
		return false;
	}
	mRelayReadyLogged = false;
	mUDPSocket.startRelay(role, address, NetplayAuth::relayLobbyFromSession(mSessionID), NetplayAuth::relayKeyFromSession(mSessionID));
	RMX_LOG_INFO("Netplay: " << ((role == UDPSocket::RelayRole::HOST) ? "registering this lobby at the relay server" : "asking the relay server to let us in"));
	return true;
}

bool NetplayManager::beginRelayAsClient()
{
	return canUseRelayAsClient() && startRelay(UDPSocket::RelayRole::CLIENT);
}

bool NetplayManager::setupAsHostWithSecret(uint16 port, uint64 secret)
{
	if (!setupAsHost(false, port, true))
		return false;
	mHostSecret = secret;
	NetplayAuth::prepareSecret(mHostSecret);
	return true;
}

void NetplayManager::startJoinViaServer()
{
	if (!restartConnection(false) || nullptr == mNetplayClient)
		return;
	clearFailure();

	mNetplayClient->setAuthSecret(NetplayAuth::secretFromSession(mSessionID));
	NetplayAuth::prepareSecret(NetplayAuth::secretFromSession(mSessionID));		// The slow key stretching happens now, once, not in the middle of the handshake
	mUDPSocket.setLanSearchSession(NetplayAuth::lanTagFromSession(mSessionID));
	mLanSearchTimer = 0.0f;
	mRelaySessionKnown = true;
	mNetplayClient->joinViaServer();
}

void NetplayManager::startJoinViaRelayOnly()
{
	if (!restartConnection(false) || nullptr == mNetplayClient)
		return;
	clearFailure();

	mNetplayClient->setAuthSecret(NetplayAuth::secretFromSession(mSessionID));
	NetplayAuth::prepareSecret(NetplayAuth::secretFromSession(mSessionID));		// The slow key stretching happens now, once, not in the middle of the handshake
	mRelaySessionKnown = true;
	mNetplayClient->joinViaRelayOnly();
}

void NetplayManager::startJoinIntroduced(uint64 sessionID, std::string_view ip, uint16 port)
{
	mSessionID = sessionID;
	if (!restartConnection(false) || nullptr == mNetplayClient)
		return;
	clearFailure();

	mNetplayClient->setAuthSecret(NetplayAuth::secretFromSession(mSessionID));
	NetplayAuth::prepareSecret(NetplayAuth::secretFromSession(mSessionID));		// The slow key stretching happens now, once, not in the middle of the handshake
	mRelaySessionKnown = true;
	mNetplayClient->connectDirectlyToHost(ip, port);
}

void NetplayManager::startJoinDirect(std::string_view ip, uint16 port, uint64 secret)
{
	if (!restartConnection(false) || nullptr == mNetplayClient)
		return;
	clearFailure();

	mNetplayClient->setAuthSecret(secret);
	NetplayAuth::prepareSecret(secret);		// The slow key stretching happens now, once, not in the middle of the handshake
	mNetplayClient->connectDirectlyToHost(ip, port);
}

std::string NetplayManager::getLocalPlayerName() const
{
	// Developer test: a name without touching the settings file
	if (const char* testName = TestHooks::getEnv("S3AIR_PLAYER_NAME"))
		return NetplayLobby::sanitizeName(testName);
	return NetplayLobby::sanitizeName(Configuration::instance().mOnlinePlayerName);
}

void NetplayManager::setLocalPlayerName(const std::string& name)
{
	Configuration& configuration = Configuration::instance();
	const std::string clean = NetplayLobby::sanitizeName(name);
	if (configuration.mOnlinePlayerName == clean)
		return;
	configuration.mOnlinePlayerName = clean;
	configuration.saveSettings();
	RMX_LOG_INFO("Netplay: the player name is now \"" << clean << "\"");

	// A friend tells the host right away, the host's lobby message goes out with its next update
	if (nullptr != mNetplayClient)
		mNetplayClient->sendLobbyReady(mNetplayClient->isLobbyReady());
}

void NetplayManager::setInputDelaySetting(int setting)
{
	mInputDelaySetting = NetplayLobby::clampDelaySetting(setting);
	if (nullptr != mNetplayHost)
		mNetplayHost->setInputDelaySetting(mInputDelaySetting);
}

std::string NetplayManager::getPlayerName(int slot) const
{
	if (nullptr != mNetplayHost && mNetplayHost->getHostState() != NetplayHost::HostState::FAILED)
		return mNetplayHost->getPlayerName(slot);
	if (nullptr != mNetplayClient && mNetplayClient->hasLobbyState())
		return mNetplayClient->getLobbyName(slot);
	return std::string();
}

int NetplayManager::getPlayerPingMs(int slot) const
{
	if (nullptr != mNetplayHost)
		return mNetplayHost->getPlayerPing(slot);
	if (nullptr != mNetplayClient && mNetplayClient->hasLobbyState())
		return mNetplayClient->getLobbyPing(slot);
	return -1;
}

const NetplayLobby::CharacterList& NetplayManager::getAvailableCharacters()
{
	// The extra characters are found by asking the game's script program which "ExtraChar.startup_charN" functions exist (nothing gets executed). The answer only
	// changes when the active mods change, so it is kept for a second at most.
	const uint32 now = SDL_GetTicks();
	if (!mCharacterListKnown || now - mCharacterListTicks > 1000)
	{
		mCharacterListKnown = true;
		mCharacterListTicks = now;
		std::vector<int> extras;
		LemonScriptRuntime& runtime = Application::instance().getSimulation().getCodeExec().getLemonScriptRuntime();
		if (runtime.hasValidProgram())
		{
			for (int slot = 1; slot <= 255; ++slot)
			{
				if (runtime.hasFunctionByName(lemon::FlyweightString("ExtraChar.startup_char" + std::to_string(slot))))
					extras.push_back(slot);
			}
		}
		// Developer test switch: after this many seconds the extra characters count as gone (as if their mod had been switched off), to see what the lobby does then
		if (const char* dropAfter = TestHooks::getEnv("S3AIR_CHARACTER_LIST_DROP"))
		{
			if ((double)now / 1000.0 > atof(dropAfter))
				extras.clear();
		}
		NetplayLobby::CharacterList list;
		list.setExtraSlots(extras);
		if (list != mCharacterList || !mCharacterListLogged)
		{
			std::string text;
			for (const int slot : list.getExtraSlots())
				text += " " + std::to_string(slot);
			RMX_LOG_INFO("Netplay: characters that can be picked: Sonic, Tails, Knuckles" << (list.getExtraSlots().empty() ? "" : ", extra characters in the slots:") << text);
			mCharacterList = list;
			mCharacterListLogged = true;
			mCharacterListChanged = true;
		}
	}
	return mCharacterList;
}

void NetplayManager::invalidateCharacterList()
{
	mCharacterListKnown = false;
}

int NetplayManager::getConfiguredPartyExtraSlot(int slot) const
{
	// The setting of the Drop-In Co-op mod that names the extra character of player 3 or 4 (it is still the default of that player)
	if (slot != 2 && slot != 3)
		return 0;
	const char* identifier = (slot == 2) ? "party_slot3" : "party_slot4";
	for (const Mod* mod : ModManager::instance().getActiveMods())
	{
		if (mod->mUniqueID != "dropin-coop")
			continue;
		for (const Mod::SettingCategory& category : mod->mSettingCategories)
			for (const Mod::Setting& setting : category.mSettings)
				if (setting.mIdentifier == identifier)
					return (int)setting.mCurrentValue;
	}
	return 0;
}

uint16 NetplayManager::getCharacterPickFromEnvironment() const
{
	// Developer test switch: "sonic", "tails", "knuckles", "menu", or "extraN" for the extra slot N
	const char* text = TestHooks::getEnv("S3AIR_CHARACTER");
	if (nullptr == text)
		return NetplayLobby::CHARACTER_NONE;
	if (_stricmp(text, "sonic") == 0)		return NetplayLobby::CHARACTER_SONIC;
	if (_stricmp(text, "tails") == 0)		return NetplayLobby::CHARACTER_TAILS;
	if (_stricmp(text, "knuckles") == 0)	return NetplayLobby::CHARACTER_KNUCKLES;
	if (_stricmp(text, "menu") == 0)		return NetplayLobby::CHARACTER_MENU;
	if (_strnicmp(text, "extra", 5) == 0)	return NetplayLobby::makeExtraCharacter(atoi(text + 5));
	if (_stricmp(text, "bad") == 0)			return 0x7777;		// (a pick that nobody may have, to test that the host refuses it)
	return NetplayLobby::CHARACTER_NONE;
}

void NetplayManager::setHostCharacterPick(uint16 pick)
{
	if (nullptr != mNetplayHost && mNetplayHost->arePicksLocked())
		return;
	bool adjusted = false;
	const uint16 accepted = NetplayLobby::sanitizeCharacterPick(0, pick, getAvailableCharacters(), mHostCharacterPick, &adjusted);
	if (accepted != mHostCharacterPick)
	{
		mHostCharacterPick = accepted;
		RMX_LOG_INFO("Netplay: the host plays as " << NetplayLobby::getCharacterName(accepted));
	}
	if (nullptr != mNetplayHost)
		mNetplayHost->onHostCharacterChanged();
}

bool NetplayManager::setSessionCharacters(const uint16 picks[4], bool strict, std::string* outProblem)
{
	// Strict (a friend, from the start message of the host): the picks are used exactly as they are, or not at all. The host decided them, and a pick that this computer
	// replaced by something else would make this game run another character than the others (the sync check does not cover them). That is a mod mismatch: it is refused.
	// Not strict (the host): what it fixes is made valid for this computer first, and what is sent to the friends is what the host really uses.
	if (strict && mSessionCharactersValid && memcmp(mSessionCharacters.mPicks, picks, sizeof(mSessionCharacters.mPicks)) == 0)
		return true;		// the same picks again (the host repairs our game state while we play): nothing to check, nothing changes

	invalidateCharacterList();		// read the installed extra characters right now, not what was cached a moment ago
	const NetplayLobby::CharacterList& list = getAvailableCharacters();
	int configured[4];
	for (int slot = 0; slot < 4; ++slot)
		configured[slot] = getConfiguredPartyExtraSlot(slot);

	bool allUsable = true;
	if (strict)
	{
		const int bad = NetplayLobby::findUnusableSessionPick(picks, list, configured);
		if (bad >= 0)
		{
			const bool programReady = Application::instance().getSimulation().getCodeExec().getLemonScriptRuntime().hasValidProgram();
			std::string problem = "The host picked " + NetplayLobby::getCharacterName(picks[bad]) + " for player " + std::to_string(bad + 1) + ", which this game cannot use";
			problem += NetplayLobby::isExtraCharacter(picks[bad]) ? (programReady ? " (that extra character is not installed here)." : " (the mods are still loading).") : ".";
			RMX_LOG_INFO("Netplay: refusing the start: " << problem);
			if (nullptr != outProblem)
				*outProblem = problem;
			return false;
		}
		for (int slot = 0; slot < 4; ++slot)
			mSessionCharacters.mPicks[slot] = picks[slot];
	}
	else
	{
		for (int slot = 0; slot < 4; ++slot)
		{
			bool adjusted = false;
			mSessionCharacters.mPicks[slot] = NetplayLobby::sanitizeCharacterPick(slot, picks[slot], list, NetplayLobby::defaultCharacterForSlot(slot, list, configured[slot]), &adjusted, configured[slot]);
			if (adjusted)
			{
				allUsable = false;
				RMX_LOG_INFO("Netplay: the character of player " << (slot + 1) << " (" << NetplayLobby::getCharacterName(picks[slot]) << ") is not available here, using " << NetplayLobby::getCharacterName(mSessionCharacters.mPicks[slot]));
			}
		}
	}
	mSessionCharactersValid = true;
	RMX_LOG_INFO("Netplay: characters of this game: player 1 " << NetplayLobby::getCharacterName(mSessionCharacters.mPicks[0]) << ", player 2 " << NetplayLobby::getCharacterName(mSessionCharacters.mPicks[1]) << ", player 3 " << NetplayLobby::getCharacterName(mSessionCharacters.mPicks[2]) << ", player 4 " << NetplayLobby::getCharacterName(mSessionCharacters.mPicks[3]));
	return allUsable;
}

void NetplayManager::clearSessionCharacters()
{
	mSessionCharactersValid = false;
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
	clearSessionCharacters();
	mHostCharacterPick = NetplayLobby::CHARACTER_MENU;
	mUDPSocket.stopRelay();
	mRelaySessionKnown = false;
	mRelayReadyLogged = false;
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
#ifdef PLATFORM_WEB
	const std::string path = "/sonic3air/savedata/online_log" + logName + ".txt";		// (the browser's save data folder; there is no %APPDATA%)
#else
	const std::string path = std::string(appdata ? appdata : ".") + "/Sonic3AIR/online_log" + logName + ".txt";
#endif
	FILE* file = fopen(path.c_str(), mOnlineLogStarted ? "a" : "w");
	if (nullptr == file)
		return;
	mOnlineLogStarted = true;

	const uint32 frame = Application::instance().getSimulation().getFrameNumber();
	const float fps = (float)(frame - mOnlineLogLastFrame) / elapsed;
	mOnlineLogLastFrame = frame;

	if (nullptr != mNetplayClient)
	{
		fprintf(file, "[friend] t=%us state=%d frame=%u fps=%.1f delay=%d frames, repairs=%d, last sync ok at frame %u, receiving game=%d, relay=%d\n",
			(unsigned)(SDL_GetTicks() / 1000), (int)mNetplayClient->getState(), (unsigned)frame, fps, mNetplayClient->getCurrentLatency(),
			mNetplayClient->getDesyncCount(), (unsigned)mNetplayClient->getLastSyncOkFrame(), mNetplayClient->isReceivingGameState() ? 1 : 0, mNetplayClient->isUsingRelay() ? 1 : 0);
	}
	else
	{
		int latency = 0;
		int connected = 0;
		const int worstPing = mNetplayHost->getWorstPing();
		for (const auto* player : mNetplayHost->getPlayerConnections())
		{
			if (player->getState() == NetConnection::State::CONNECTED)
			{
				++connected;
				latency = std::max(latency, player->mCurrentLatency);
			}
		}
		fprintf(file, "[host] t=%us state=%d frame=%u fps=%.1f friends=%d, delay=%d frames, repairs=%d, inputdelay=%d, ping=%d, relayed=%d\n",
			(unsigned)(SDL_GetTicks() / 1000), (int)mNetplayHost->getHostState(), (unsigned)frame, fps, connected, latency, mNetplayHost->getResyncCount(), mNetplayHost->getEffectiveInputDelay(), worstPing, mNetplayHost->getRelayedFriendCount());
	}
	fclose(file);
}

void NetplayManager::updateConnections(float deltaSeconds)
{
	writeOnlineLog(deltaSeconds);
	if (!mUDPSocket.isValid())
		return;

	mUDPSocket.updateRelay();
	mConnectionManager.updateConnectionManager();

	if (mUDPSocket.getRelayRole() != UDPSocket::RelayRole::NONE && mUDPSocket.isRelayReady() != mRelayReadyLogged)
	{
		mRelayReadyLogged = mUDPSocket.isRelayReady();
		RMX_LOG_INFO("Netplay: " << (mRelayReadyLogged ? ((mUDPSocket.getRelayRole() == UDPSocket::RelayRole::HOST) ? "the relay server knows this lobby" : "the relay server let us in") : "the relay server does not answer (any more)"));
	}

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

	// The characters of the four players, fixed when the game started, for the game scripts: also shared memory, so that every computer's scripts see the same
	// numbers in every frame (they came from the host in the start message). Not part of the compared game state; the scripts use them to set up the players.
	{
		EmulatorInterface& emulatorInterface = Application::instance().getSimulation().getEmulatorInterface();
		const bool gameRunning = (nullptr != mNetplayClient && mNetplayClient->getState() == NetplayClient::State::GAME_RUNNING) ||
								 (nullptr != mNetplayHost && mNetplayHost->getHostState() == NetplayHost::HostState::GAME_RUNNING);
		if (gameRunning && mSessionCharactersValid)
		{
			emulatorInterface.writeMemory8(PICKS_ADDRESS, 1);
			for (int slot = 0; slot < 4; ++slot)
			{
				emulatorInterface.writeMemory8(PICKS_ADDRESS + 2 + slot * 2, (uint8)(mSessionCharacters.mPicks[slot] & 0xff));
				emulatorInterface.writeMemory8(PICKS_ADDRESS + 3 + slot * 2, (uint8)(mSessionCharacters.mPicks[slot] >> 8));
			}
		}
		else
		{
			emulatorInterface.writeMemory8(PICKS_ADDRESS, 0);
		}
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

#ifndef PLATFORM_WEB
	// (the browser build never talks to the game server: it cannot send UDP, and the relay is its only way to other players)
	// Developer test: S3AIR_NO_SERVER keeps this game away from the game server completely (checks that need no matchmaking)
	if (nullptr == TestHooks::getEnv("S3AIR_NO_SERVER"))
		mExternalAddressQuery.startQuery();
#endif
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
	// Two builds with a different netplay revision must not play together. This used to be the compile date and time of the program, which made every
	// compile (and so the browser build and the Windows build, even of the same source) incompatible with every other. Now it is a fixed text: change
	// NETPLAY_COMPATIBILITY_ID whenever a change makes the simulation or the netplay protocol behave differently from earlier builds.
	result += std::string("build:") + NETPLAY_COMPATIBILITY_ID;
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

