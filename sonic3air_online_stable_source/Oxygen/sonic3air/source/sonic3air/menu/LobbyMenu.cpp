/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "oxygen_netcore/base/TestHooks.h"
#include "sonic3air/menu/LobbyMenu.h"
#include "sonic3air/menu/MenuBackground.h"
#include "sonic3air/menu/GameApp.h"
#include "sonic3air/Game.h"
#include "sonic3air/menu/SharedResources.h"
#include "sonic3air/menu/entries/GeneralMenuEntries.h"
#include "sonic3air/audio/AudioOut.h"

#include "oxygen/application/EngineMain.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/input/InputManager.h"
#include "oxygen/network/netplay/NetplayManager.h"
#include "oxygen/network/netplay/NetplayHost.h"
#include "oxygen/network/netplay/NetplayClient.h"
#include "oxygen/network/netplay/NetplayCodes.h"
#include "oxygen/network/netplay/NetplayLobbyLogic.h"
#include "oxygen/platform/PlatformFunctions.h"

#include <random>


namespace
{
	const float HOSTING_MESSAGE_SECONDS = 4.0f;
	const float LIST_REFRESH_SECONDS = 8.0f;
	const int MAX_CODE_DIGITS = 13;
	const int LIST_ROWS_VISIBLE = 6;

	enum Action
	{
		CREATE_LOBBY = 1,
		JOIN_LOBBY,
		JOIN_NOW,
		READY,
		START_GAME,
		RETRY,
		COPY_CODE,
		LEAVE,
		BACK,
		INPUT_DELAY,
		CHAT_QUICK,
		KICK,
		EDIT_NAME,
		CHARACTER,
		TEXT_OK,
		EDIT_RELAY,
		PUBLIC_LOBBIES,
		QUICK_JOIN,
		LAN_GAME,
		SETTINGS,
		EDIT_LOBBY_SERVER,
		EDIT_LOBBY_NAME,
		TOGGLE_PUBLIC,
		MAX_PLAYERS,
		OPEN_LOBBY,
		LAN_HOST,
		LAN_JOIN,
		REFRESH,
		FILTER,
		CANCEL,
		PASTE_CODE,
		LIST_ROW = 1000,		// + index of the row in the list
		PAD_KEY = 2000			// + 0 to 9 for the digits, 10 = delete, 11 = join
	};

	static const int WRITE_MESSAGE_INDEX = NetplayLobby::NUM_QUICK_MESSAGES + 1;
	static const int PAD_DELETE = 10;
	static const int PAD_JOIN = 11;
	static const int PAD_KEY_COUNT = 12;
	// The number pad: 1 2 3 / 4 5 6 / 7 8 9 / delete 0 join
	static const int PAD_LAYOUT[PAD_KEY_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, PAD_DELETE, 0, PAD_JOIN };

	NetplayManager& netplay()
	{
		return NetplayManager::instance();
	}

	std::wstring widen(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	std::string shorten(const std::string& text, size_t maxLength)
	{
		return (text.size() > maxLength) ? (text.substr(0, maxLength - 3) + "...") : text;
	}

	// "123 456" for a lobby code, "1234-5678-90123" for the long code, while typing: whatever is there so far
	std::string groupDigits(const std::string& digits)
	{
		if (digits.size() <= (size_t)NetplayCodes::LOBBY_CODE_DIGITS)
			return (digits.size() > 3) ? (digits.substr(0, 3) + " " + digits.substr(3)) : digits;
		std::string text = digits.substr(0, 4);
		if (digits.size() > 4)
			text += "-" + digits.substr(4, 4);
		if (digits.size() > 8)
			text += "-" + digits.substr(8);
		return text;
	}

	std::string cleanLobbyName(const std::string& raw)
	{
		// Plain letters, digits and a few marks, at most 20 characters (the lobby server cleans it once more)
		std::string name;
		for (const char c : raw)
		{
			const bool ok = std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '!' || c == '?' || c == '\'' || c == '#' || c == '+' || c == '&' || c == '(' || c == ')';
			if (ok && !(c == ' ' && (name.empty() || name.back() == ' ')))
				name += c;
		}
		while (!name.empty() && name.back() == ' ')
			name.pop_back();
		if (name.size() > 20)
			name.resize(20);
		return name.empty() ? std::string("My lobby") : name;
	}
}


LobbyMenu::LobbyMenu(MenuBackground& menuBackground) :
	mMenuBackground(&menuBackground)
{
	mMenuEntries.reserve(10);
}

LobbyMenu::~LobbyMenu()
{
}

GameMenuBase::BaseState LobbyMenu::getBaseState() const
{
	switch (mState)
	{
		case State::APPEAR:		   return BaseState::FADE_IN;
		case State::SHOW:		   return BaseState::SHOW;
		case State::FADE_TO_MENU:  return BaseState::FADE_OUT;
		default:				   return BaseState::INACTIVE;
	}
}

void LobbyMenu::setBaseState(BaseState baseState)
{
	switch (baseState)
	{
		case BaseState::INACTIVE: mState = State::INACTIVE;  break;
		case BaseState::FADE_IN:  mState = State::APPEAR;  break;
		case BaseState::SHOW:	  mState = State::SHOW;  break;
		case BaseState::FADE_OUT: mState = State::FADE_TO_MENU;  break;
	}
}

void LobbyMenu::onFadeIn()
{
	mState = State::APPEAR;
	mShowCopied = false;

	mMenuBackground->showPreview(false, false);
	mMenuBackground->startTransition(MenuBackground::Target::BLUE);

	NetplayManager& manager = netplay();

	// Back in the lobby: a host who comes back from the main menu is not picking a game any more
	if (nullptr != manager.getNetplayHost())
		manager.getNetplayHost()->setLobbyHostStarting(false);

	// The menu keeps a connection to the lobby server while it is open (it is closed again when the menu is left, unless a lobby is published)
	manager.setLobbyDirectoryInUse(true);
	mHookScreenTimer = 0.0f;

	// What was chosen last time
	{
		const Configuration& configuration = Configuration::instance();
		mLobbyName = cleanLobbyName(configuration.mOnlineLobbyName);
		mLobbyPublic = configuration.mOnlineLobbyPublic;
		mLobbyMaxPlayers = clamp(configuration.mOnlineLobbyMaxPlayers, 2, 4);
		mListCompatibleOnly = configuration.mOnlineListCompatibleOnly;
		if (const char* testName = TestHooks::getEnv("S3AIR_LOBBY_NAME"))
			mLobbyName = cleanLobbyName(testName);
		if (const char* testMax = TestHooks::getEnv("S3AIR_LOBBY_MAX"))
			mLobbyMaxPlayers = clamp(atoi(testMax), 2, 4);
	}

	chooseScreen();
	AudioOut::instance().setMenuMusic(0x2f);

	// Test hook for the automated checks: host or join a lobby without any key presses
	static bool testHookDone = false;
	if (!testHookDone)
	{
		const char* mode = TestHooks::getEnv("S3AIR_LOBBY_OPEN");
		if (nullptr != mode)
		{
			testHookDone = true;
			if (strcmp(mode, "host") == 0)
				createLobby(true, false, mLobbyMaxPlayers, mLobbyName);
			else if (strcmp(mode, "hostlan") == 0)
				createLobby(false, false, mLobbyMaxPlayers, mLobbyName);
			else if (strcmp(mode, "hostpublic") == 0)
				createLobby(false, true, mLobbyMaxPlayers, mLobbyName);
			else if (strcmp(mode, "hostprivate") == 0)
				createLobby(false, false, mLobbyMaxPlayers, mLobbyName);
			else if (strcmp(mode, "join") == 0 && nullptr != TestHooks::getEnv("S3AIR_LOBBY_CODE"))
				joinWithCode(TestHooks::getEnv("S3AIR_LOBBY_CODE"));
			else if (strcmp(mode, "list") == 0)
				showScreen(Screen::LIST);
			else if (strcmp(mode, "quick") == 0)
				beginQuickJoin();
		}
	}
}

bool LobbyMenu::canBeRemoved()
{
	return (mState == State::INACTIVE && mVisibility <= 0.0f);
}

void LobbyMenu::initialize()
{
}

void LobbyMenu::deinitialize()
{
}

void LobbyMenu::keyboard(const rmx::KeyboardEvent& ev)
{
	if (mScreen == Screen::TEXT && mMenuEntries.hasSelected())
	{
		mMenuEntries.selected().keyboard(ev);
	}
	else if (mScreen == Screen::JOIN && ev.state)
	{
		// Paste (Ctrl+V): only the digits of what is on the clipboard are used, so "Code: 123 456" works
		if (ev.key == 'v' && (SDL_GetModState() & KMOD_CTRL) != 0 && PlatformFunctions::hasClipboardSupport())
		{
			std::wstring text;
			if (PlatformFunctions::pasteFromClipboard(text))
			{
				std::string narrow;
				for (const wchar_t c : text)
					narrow += (c >= '0' && c <= '9') ? (char)c : ' ';
				mCodeDigits = NetplayCodes::keepDigitsOnly(mCodeDigits + narrow, (size_t)MAX_CODE_DIGITS);
				mMessage.clear();
			}
			mTextInputThisFrame = true;		// (the V key is not a button press)
		}
		else if (ev.key == SDLK_KP_ENTER)
		{
			if (mState == State::SHOW)
				joinWithCode(mCodeDigits);
		}
	}
}

void LobbyMenu::textinput(const rmx::TextInputEvent& ev)
{
	if (mScreen == Screen::TEXT && mMenuEntries.hasSelected())
	{
		mMenuEntries.selected().textinput(ev);
	}
	else if (mScreen == Screen::JOIN)
	{
		// Typed text: only digits are taken (the number keys and the number pad both end up here); letters do nothing, and must not press the pad's buttons either
		mTextInputThisFrame = true;
		std::string narrow;
		for (const wchar_t c : std::wstring(ev.text))
			narrow += (c >= '0' && c <= '9') ? (char)c : ' ';
		const std::string digits = NetplayCodes::keepDigitsOnly(narrow, (size_t)MAX_CODE_DIGITS - mCodeDigits.size());
		if (!digits.empty())
		{
			mCodeDigits += digits;
			mMessage.clear();
		}
	}
}

void LobbyMenu::chooseScreen()
{
	// Whatever is going on in the network decides what is shown
	if (nullptr != netplay().getNetplayHost())
		mScreen = Screen::HOSTING;
	else if (nullptr != netplay().getNetplayClient())
		mScreen = Screen::JOINED;
	else if (mScreen == Screen::HOSTING || mScreen == Screen::JOINED || mScreen == Screen::TEXT || (mScreen == Screen::SEARCHING && mFlow == Flow::NONE))
		mScreen = Screen::HOME;
	rebuildEntries();
}

void LobbyMenu::showScreen(Screen screen)
{
	mScreen = screen;
	mMessage.clear();
	if (screen == Screen::LIST)
	{
		mShownList = netplay().getLobbyDirectory().getList();
		mLastListAge = netplay().getLobbyDirectory().getListAgeSeconds();
		mListSelectedCode.clear();
		mListScroll = 0;
		refreshList();
	}
	rebuildEntries();
}

void LobbyMenu::rebuildEntries()
{
	{
		static const char* const SCREEN_NAMES[] = { "START", "LIST", "JOIN", "CREATE", "LAN", "SETTINGS", "SEARCHING", "HOSTING", "JOINED", "TEXT" };
		RMX_LOG_INFO("Lobby: screen " << SCREEN_NAMES[(int)mScreen]);		// (what the automated checks wait for; START is the first screen)
	}

	mMenuEntries.clear();
	switch (mScreen)
	{
		case Screen::HOME:
			if (mFailure != NetplayFailure::Reason::NONE && NetplayFailure::isRetryable(mFailure) && (mLastWasHosting || !mLastJoinCode.empty()))
				mMenuEntries.addEntry("RETRY", RETRY);
			mMenuEntries.addEntry("PUBLIC LOBBIES", PUBLIC_LOBBIES);
			mMenuEntries.addEntry("QUICK JOIN", QUICK_JOIN);
			mMenuEntries.addEntry("CREATE LOBBY", CREATE_LOBBY);
			mMenuEntries.addEntry("JOIN WITH CODE", JOIN_LOBBY);
			mMenuEntries.addEntry("LAN GAME", LAN_GAME);
			mMenuEntries.addEntry("SETTINGS", SETTINGS);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::LIST:
			for (size_t i = 0; i < mShownList.size(); ++i)
				mMenuEntries.addEntry(mShownList[i].mName, LIST_ROW + (uint32)i);
			mMenuEntries.addEntry("REFRESH", REFRESH);
			mMenuEntries.addEntry("SHOW", FILTER);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::JOIN:
			for (int key = 0; key < PAD_KEY_COUNT; ++key)
				mMenuEntries.addEntry("", PAD_KEY + (uint32)PAD_LAYOUT[key]);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::CREATE:
			mMenuEntries.addEntry("NAME", EDIT_LOBBY_NAME);
			mMenuEntries.addEntry("LOBBY", TOGGLE_PUBLIC);
			mMenuEntries.addEntry("PLAYERS", MAX_PLAYERS);
			mMenuEntries.addEntry("OPEN LOBBY", OPEN_LOBBY);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::LAN:
			mMenuEntries.addEntry("HOST A LAN GAME", LAN_HOST);
			mMenuEntries.addEntry("JOIN A LAN GAME", LAN_JOIN);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::SETTINGS:
			mMenuEntries.addEntry("NAME", EDIT_NAME);
			if (!NetplayManager::isBrowserBuild())
				mMenuEntries.addEntry("LOBBY SERVER", EDIT_LOBBY_SERVER);
			mMenuEntries.addEntry("RELAY SERVER", EDIT_RELAY);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::SEARCHING:
			mMenuEntries.addEntry("CANCEL", CANCEL);
			break;

		case Screen::HOSTING:
			mMenuEntries.addEntry("START GAME", START_GAME);
			mMenuEntries.addEntry("CHARACTER", CHARACTER);
			mMenuEntries.addEntry("INPUT DELAY", INPUT_DELAY);
			mMenuEntries.addEntry("QUICK CHAT", CHAT_QUICK);
			mMenuEntries.addEntry("KICK", KICK);
			mMenuEntries.addEntry("NAME", EDIT_NAME);
			mMenuEntries.addEntry("COPY CODE", COPY_CODE);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::JOINED:
			mMenuEntries.addEntry("READY", READY);
			mMenuEntries.addEntry("CHARACTER", CHARACTER);
			mMenuEntries.addEntry("QUICK CHAT", CHAT_QUICK);
			mMenuEntries.addEntry("NAME", EDIT_NAME);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::TEXT:
			if (mTextMode == TextMode::NAME)
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), widen(netplay().getLocalPlayerName()), L"Type your name...");
			else if (mTextMode == TextMode::RELAY)
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), widen(netplay().getRelayServerSetting()), L"e.g. relay.example.org");
			else if (mTextMode == TextMode::LOBBY_SERVER)
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), widen(netplay().getLobbyServerSetting()), L"e.g. lobbies.example.org");
			else if (mTextMode == TextMode::LOBBY_NAME)
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), widen(mLobbyName), L"Name of your lobby...");
			else
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), L"", L"Type a short message...");
			mMenuEntries.addEntry((mTextMode == TextMode::CHAT) ? "SEND" : "SAVE", TEXT_OK);
			mMenuEntries.addEntry("BACK", BACK);
			break;
	}

	mMenuEntries.mSelectedEntryIndex = 0;
	if (mScreen == Screen::LIST)
	{
		// The row that was selected stays selected when the list changes; else the first lobby that can be joined; else REFRESH
		const int rows = (int)mShownList.size();
		int select = rows;
		if (mListFooterSelected >= 0)
		{
			select = rows + std::min(mListFooterSelected, 2);
		}
		else if (!mListSelectedCode.empty())
		{
			for (int i = 0; i < rows; ++i)
			{
				if (mShownList[(size_t)i].mCode == mListSelectedCode)
				{
					select = i;
					break;
				}
			}
		}
		else
		{
			for (int i = 0; i < rows; ++i)
			{
				const DirectoryProtocol::ListEntry& entry = mShownList[(size_t)i];
				if (entry.isCompatible() && !entry.isFull() && !entry.isInGame())
				{
					select = i;
					break;
				}
			}
		}
		mMenuEntries.mSelectedEntryIndex = std::min(select, (int)mMenuEntries.size() - 1);
	}
	for (size_t i = 0; i < mMenuEntries.size(); ++i)
		mMenuEntries[i].mAnimation.mHighlight = 0.0f;
	updateEntryTexts();
}

void LobbyMenu::openTextScreen(TextMode mode, Screen returnTo)
{
	mTextMode = mode;
	mTextReturnScreen = returnTo;
	mScreen = Screen::TEXT;
	mMessage.clear();
	rebuildEntries();
}

std::string LobbyMenu::getNoServerMessage() const
{
	NetplayManager& manager = netplay();
	switch (manager.getLobbyServerState())
	{
		case NetplayManager::LobbyServerState::NOT_SET:
			return "No lobby server is set yet, so there are no public lobbies.\nYou can still play: LAN GAME, or JOIN WITH CODE with a long code.\nA lobby server can be added in SETTINGS.";
		case NetplayManager::LobbyServerState::BAD_ADDRESS:
			return "The lobby server address does not look right.\nCheck it in SETTINGS. You can still use LAN GAME.";
		default:
			return "The lobby server does not answer. It may be down, or you may be\noffline. Try again later, or use LAN GAME with a friend.";
	}
}

void LobbyMenu::createLobby(bool viaServer, bool isPublic, int maxPlayers, const std::string& lobbyName, bool lanOnly)
{
	NetplayManager& manager = netplay();
	manager.clearFailure();
	mMessage.clear();
	mFailure = NetplayFailure::Reason::NONE;
	mLastWasHosting = true;
	if (NetplayManager::isBrowserBuild())
	{
		// A browser has no UDP: the lobby lives at the relay server, not at the game server
		viaServer = false;
		std::string relayURL;
		if (!NetplayLobby::normalizeRelayWebSocketURL(manager.getRelayServerSetting(), relayURL))
		{
			RMX_LOG_INFO("Lobby: no relay server set, the browser version cannot open a lobby");
			mMessage = "Online play in the browser needs a lobby server (or relay server).\nChoose SETTINGS, then RELAY SERVER, and enter its address first.";
			mFailure = NetplayFailure::Reason::SERVER_UNREACHABLE;
			mLastWasHosting = false;
			showScreen(Screen::HOME);
			return;
		}
	}
	mLastHostViaServer = viaServer;
	mLastHostPublic = isPublic;
	mLastHostLanOnly = lanOnly;
	mLastHostMaxPlayers = maxPlayers;
	mLastHostName = lobbyName;

	// A fresh session code: 12 random digits, which the friends use to find this lobby (a lobby server turns it into a 6 digit lobby code)
	std::random_device randomDevice;
	uint64 sessionID = NetplayCodes::sessionIDFromRandom(((uint64)randomDevice() << 32) | randomDevice());
	if (const char* fixedCode = TestHooks::getEnv("S3AIR_LOBBY_CODE"))
	{
		uint64 fixedID = 0;
		if (NetplayCodes::readSessionCode(fixedCode, fixedID))
			sessionID = fixedID;
	}
	manager.mSessionID = sessionID;

	// A lobby of a lobby server does not need the matchmaking server (which is not ours and may be down)
	// (A LAN game uses neither: the friends find it on the network with the long code)
	const bool onLobbyServer = !lanOnly && manager.isLobbyServerConfigured();
	if (manager.setupAsHost(viaServer && !onLobbyServer && !lanOnly, NetplayManager::DEFAULT_HOST_PORT, false, !lanOnly))
	{
		if (nullptr != manager.getNetplayHost())
			manager.getNetplayHost()->setMaxPlayers(maxPlayers);
		if (onLobbyServer)
			manager.startPublishingLobby(isPublic, cleanLobbyName(lobbyName));
		mScreen = Screen::HOSTING;
		rebuildEntries();
	}
	else
	{
		mMessage = manager.mLastErrorMessage.empty() ? std::string("Could not open the lobby.\nThe network port (UDP 28840) may be in use by another copy of the game.") : manager.mLastErrorMessage;
		mFailure = NetplayFailure::Reason::BLOCKED_OR_TIMEOUT;
		showScreen(Screen::HOME);
	}
}

bool LobbyMenu::joinWithSessionCode(uint64 sessionID)
{
	NetplayManager& manager = netplay();
	manager.mSessionID = sessionID;
	if (NetplayManager::isBrowserBuild())
	{
		// Only a code works in the browser, and only with a relay server
		std::string relayURL;
		if (!NetplayLobby::normalizeRelayWebSocketURL(manager.getRelayServerSetting(), relayURL))
		{
			RMX_LOG_INFO("Lobby: no relay server set, the browser version cannot join a lobby");
			mMessage = "Online play in the browser needs a lobby server (or relay server).\nChoose SETTINGS, then RELAY SERVER, and enter its address first.";
			mFailure = NetplayFailure::Reason::SERVER_UNREACHABLE;
			showScreen(Screen::HOME);
			return false;
		}
		manager.startJoinViaRelayOnly();
	}
	else
	{
		manager.startJoinViaServer();
	}
	mScreen = Screen::JOINED;
	rebuildEntries();
	return true;
}

bool LobbyMenu::joinWithCode(const std::string& code)
{
	NetplayManager& manager = netplay();
	manager.clearFailure();
	mMessage.clear();
	mFailure = NetplayFailure::Reason::NONE;
	mLastWasHosting = false;
	mLastJoinCode = code;

	// What kind of code is it? Typing mistakes are noticed here, before anything is sent
	std::string digits;
	switch (NetplayCodes::classifyCode(code.c_str(), digits))
	{
		case NetplayCodes::CodeKind::EMPTY:
			mMessage = "Please type the code first.";
			mScreen = Screen::JOIN;
			rebuildEntries();
			return false;

		case NetplayCodes::CodeKind::HAS_LETTERS:
			mMessage = "Codes are numbers only.\nCheck what your friend sent you.";
			mScreen = Screen::JOIN;
			rebuildEntries();
			return false;

		case NetplayCodes::CodeKind::WRONG_LENGTH:
			mMessage = "A lobby code has 6 digits, a long code has 13.\nCheck for a missing or extra digit.";
			mScreen = Screen::JOIN;
			rebuildEntries();
			return false;

		case NetplayCodes::CodeKind::BAD_CHECK:
			manager.reportFailure(NetplayFailure::Reason::WRONG_CODE, "This long code has a typing mistake. Check the digits.");
			mMessage = manager.mLastErrorMessage;
			mFailure = NetplayFailure::Reason::NONE;
			mScreen = Screen::JOIN;
			rebuildEntries();
			return false;

		case NetplayCodes::CodeKind::LOBBY:
			if (mJoinLanOnly)
			{
				mMessage = "That is a lobby code from a lobby server.\nUse JOIN WITH CODE in the ONLINE menu.";
				mScreen = Screen::JOIN;
				rebuildEntries();
				return false;
			}
			if (!manager.isLobbyServerConfigured())
			{
				mMessage = getNoServerMessage();
				mScreen = Screen::JOIN;
				rebuildEntries();
				return false;
			}
			mFlowJoinedFromList = false;
			beginResolve(digits, std::string());
			return true;

		case NetplayCodes::CodeKind::SESSION:
		{
			uint64 sessionID = 0;
			if (!NetplayCodes::readSessionCode(digits.c_str(), sessionID))
				return false;
			return joinWithSessionCode(sessionID);
		}
	}
	return false;
}

void LobbyMenu::beginResolve(const std::string& lobbyCode, const std::string& lobbyName)
{
	NetplayManager& manager = netplay();
	manager.setLobbyDirectoryInUse(true);
	manager.getLobbyDirectory().requestResolve(lobbyCode);
	mFlow = Flow::RESOLVE;
	mFlowSeconds = 0.0f;
	mFlowCode = lobbyCode;
	mFlowLobbyName = lobbyName;
	mLastJoinCode = lobbyCode;
	mSearchReturnScreen = mFlowJoinedFromList ? Screen::LIST : Screen::JOIN;
	mMessage.clear();
	mScreen = Screen::SEARCHING;
	rebuildEntries();
}

void LobbyMenu::beginQuickJoin()
{
	NetplayManager& manager = netplay();
	mMessage.clear();
	if (!manager.isLobbyServerConfigured() || !manager.isLobbyServerAddressUsable())
	{
		mMessage = getNoServerMessage();
		mScreen = Screen::HOME;
		rebuildEntries();
		return;
	}
	manager.setLobbyDirectoryInUse(true);
	manager.getLobbyDirectory().requestList(true, manager.getCompatibilityKey());
	mFlow = Flow::QUICK;
	mFlowSeconds = 0.0f;
	mFlowLobbyName.clear();
	mSearchReturnScreen = Screen::HOME;
	mScreen = Screen::SEARCHING;
	rebuildEntries();
}

void LobbyMenu::finishFlow(const std::string& message, NetplayFailure::Reason failure)
{
	mFlow = Flow::NONE;
	mMessage = message;
	mFailure = failure;
	RMX_LOG_INFO("Lobby: the lookup ended: " << (message.empty() ? std::string("(no message)") : message.substr(0, message.find('\n'))));
	mScreen = mSearchReturnScreen;
	if (mScreen == Screen::LIST)
	{
		mShownList = netplay().getLobbyDirectory().getList();
		mListSelectedCode.clear();
	}
	rebuildEntries();
}

void LobbyMenu::updateFlow()
{
	NetplayManager& manager = netplay();
	LobbyDirectory& directory = manager.getLobbyDirectory();
	const NetplayManager::LobbyServerState serverState = manager.getLobbyServerState();
	if (serverState == NetplayManager::LobbyServerState::NOT_SET || serverState == NetplayManager::LobbyServerState::BAD_ADDRESS)
	{
		finishFlow(getNoServerMessage(), NetplayFailure::Reason::NONE);
		return;
	}

	if (mFlow == Flow::QUICK)
	{
		if (directory.getListState() == LobbyDirectory::RequestState::DONE)
		{
			// The server sorts the best lobby first (same version, still waiting for players, room for us, the fullest one first)
			const DirectoryProtocol::ListEntry* best = nullptr;
			for (const DirectoryProtocol::ListEntry& entry : directory.getList())
			{
				if (entry.isCompatible() && !entry.isInGame() && !entry.isFull())
				{
					best = &entry;
					break;
				}
			}
			if (nullptr == best)
			{
				finishFlow("There is no open lobby for your version right now.\nCreate one, or try again in a moment.", NetplayFailure::Reason::NONE);
				return;
			}
			mFlowLobbyName = best->mName;
			mFlowCode = best->mCode;
			mFlowJoinedFromList = false;
			RMX_LOG_INFO("Lobby: quick join picks the lobby \"" << best->mName << "\" (" << (int)best->mPlayers << "/" << (int)best->mMaxPlayers << ")");
			beginResolve(best->mCode, best->mName);
			mSearchReturnScreen = Screen::HOME;
			return;
		}
		if (directory.getListState() == LobbyDirectory::RequestState::FAILED || mFlowSeconds > 15.0f)
		{
			finishFlow(getNoServerMessage(), NetplayFailure::Reason::SERVER_UNREACHABLE);
			return;
		}
	}
	else if (mFlow == Flow::RESOLVE)
	{
		if (directory.getResolveState() == LobbyDirectory::RequestState::DONE)
		{
			const DirectoryProtocol::ResolveReply reply = directory.getResolveReply();
			directory.clearResolve();
			switch (reply.mStatus)
			{
				case DirectoryProtocol::RESOLVE_OK:
				{
					if (reply.isInGame())
					{
						finishFlow("This lobby has already started its game.\nTry another one, or ask the host to wait for you.", NetplayFailure::Reason::NONE);
						return;
					}
					if (reply.isFull())
					{
						finishFlow("This lobby is full.", NetplayFailure::Reason::LOBBY_FULL);
						mFailure = NetplayFailure::Reason::NONE;
						return;
					}
					RMX_LOG_INFO("Lobby: the lobby server found the lobby, joining it");
					mFlow = Flow::NONE;
					manager.clearFailure();
					manager.startJoinViaLobbyServer(reply.mSessionID);
					mScreen = Screen::JOINED;
					rebuildEntries();
					return;
				}
				case DirectoryProtocol::RESOLVE_THROTTLED:
					finishFlow("Too many wrong codes. Please wait " + std::to_string(std::max(1, (int)((reply.mRetryAfterSeconds + 59) / 60))) + " minute(s) and try again.", NetplayFailure::Reason::NONE);
					return;
				default:
					finishFlow("There is no lobby with this code (any more).\nCheck the digits, or ask your friend for the current code.", NetplayFailure::Reason::NONE);
					return;
			}
		}
		if (directory.getResolveState() == LobbyDirectory::RequestState::FAILED || mFlowSeconds > 15.0f)
		{
			finishFlow(getNoServerMessage(), NetplayFailure::Reason::SERVER_UNREACHABLE);
			return;
		}
	}
}

void LobbyMenu::joinListEntry(int index)
{
	if (index < 0 || index >= (int)mShownList.size())
		return;
	const DirectoryProtocol::ListEntry& entry = mShownList[(size_t)index];
	mMessage.clear();
	if (!entry.isCompatible())
	{
		playMenuSound(0xad);
		mMessage = "This lobby plays another version of the game, or has other mods.\nYou cannot join it.";
		return;
	}
	if (entry.isInGame())
	{
		playMenuSound(0xad);
		mMessage = "This lobby has already started its game.";
		return;
	}
	if (entry.isFull())
	{
		playMenuSound(0xad);
		mMessage = "This lobby is full.";
		return;
	}
	playMenuSound(0x63);
	mFlowJoinedFromList = true;
	beginResolve(entry.mCode, entry.mName);
}

void LobbyMenu::refreshList()
{
	NetplayManager& manager = netplay();
	manager.setLobbyDirectoryInUse(true);
	if (!manager.isLobbyServerConfigured())
		return;
	manager.getLobbyDirectory().requestList(mListCompatibleOnly, manager.getCompatibilityKey());
	mListRefreshTimer = LIST_REFRESH_SECONDS;
}

void LobbyMenu::showFailureAndReturnToStart()
{
	NetplayManager& manager = netplay();
	mMessage = manager.mLastErrorMessage.empty() ? "Could not connect to the lobby." : manager.mLastErrorMessage;
	mFailure = (manager.mLastFailure != NetplayFailure::Reason::NONE) ? manager.mLastFailure : NetplayFailure::Reason::BLOCKED_OR_TIMEOUT;
	manager.closeConnections();
	mScreen = Screen::HOME;
	rebuildEntries();
	RMX_LOG_INFO("Lobby: showing the problem to the player (" << NetplayFailure::getName(mFailure) << ")" << (NetplayFailure::isRetryable(mFailure) ? " with a Retry button" : " without a Retry button"));
}

void LobbyMenu::retry()
{
	RMX_LOG_INFO("Lobby: the player pressed Retry");
	const bool wasHosting = mLastWasHosting;
	const bool viaServer = mLastHostViaServer;
	const std::string code = mLastJoinCode;
	netplay().closeConnections();
	if (wasHosting)
	{
		createLobby(viaServer, mLastHostPublic, mLastHostMaxPlayers, mLastHostName, mLastHostLanOnly);
	}
	else
	{
		mJoinLanOnly = false;
		joinWithCode(code);
	}
}

void LobbyMenu::leaveLobby()
{
	netplay().closeConnections();
	netplay().clearFailure();
	mFailure = NetplayFailure::Reason::NONE;
	mMessage.clear();
	mScreen = Screen::HOME;
	rebuildEntries();
}

void LobbyMenu::startGame(bool force)
{
	// The host picks the game as usual; everybody in the lobby follows as soon as it starts
	NetplayHost* host = netplay().getNetplayHost();
	if (nullptr == host || (!force && !host->areAllClientsReady()))
		return;
	host->setLobbyHostStarting(true);

	// Developer test switch (only with the test hooks enabled): the host starts a level by itself, as if it had picked it in Act Select, so that a check can play
	// a game with the characters that were picked in the lobby without any key press. The number is the zone and act, e.g. 0x0100.
	if (const char* autoGame = TestHooks::getEnv("S3AIR_LOBBY_AUTOGAME"))
	{
		RMX_LOG_INFO("Lobby: (test) the host starts the level " << autoGame << " by itself");
		Game::instance().startIntoLevel(Game::Mode::UNDEFINED, 0, (uint16)strtoul(autoGame, nullptr, 0), 0);
		GameApp::instance().onStartGame();
		mMenuBackground->setGameStartedMenu();
		mState = State::INACTIVE;
		return;
	}
	goBack();
}

void LobbyMenu::pressStart()
{
	NetplayHost* host = netplay().getNetplayHost();
	if (nullptr == host)
		return;

	if (host->areAllClientsReady())
	{
		playMenuSound(0x63);
		startGame();
	}
	else if (host->getConnectedClientCount() == 0)
	{
		playMenuSound(0xad);
		mMessage = "Nobody has joined yet.";
		mHostingMessageSeconds = HOSTING_MESSAGE_SECONDS;
	}
	else if (!host->isReadyCheckActive())
	{
		// Not everybody is ready: the ones who are not get a prompt and a time limit, then the game starts without them
		playMenuSound(0x63);
		host->beginReadyCheck();
		mMessage = "Asked everybody to get ready.\nPress START GAME again to start now without waiting.";
		mHostingMessageSeconds = HOSTING_MESSAGE_SECONDS;
	}
	else
	{
		// The host does not want to wait any longer
		playMenuSound(0x63);
		host->finishReadyCheckNow();
	}
}

void LobbyMenu::goBack()
{
	playMenuSound(0xad);
	// Leaving the menu: the lobby server connection is only kept while a lobby is published (the manager decides)
	mFlow = Flow::NONE;
	netplay().setLobbyDirectoryInUse(false);
	mMenuBackground->openMainMenu();
	mState = State::FADE_TO_MENU;
}

// Back (Escape, the B button): one step towards the main menu, never a dead end
void LobbyMenu::goBackOneLevel()
{
	switch (mScreen)
	{
		case Screen::HOME:
		case Screen::HOSTING:
		case Screen::JOINED:
			goBack();		// (in a lobby, Back goes to the main menu and keeps the lobby: "Leave Lobby" leaves it)
			break;

		case Screen::JOIN:
			showScreen(mJoinLanOnly ? Screen::LAN : Screen::HOME);
			break;

		case Screen::SEARCHING:
			netplay().getLobbyDirectory().clearResolve();
			mFlow = Flow::NONE;
			showScreen(mSearchReturnScreen);
			break;

		case Screen::TEXT:
			if (mTextReturnScreen == Screen::HOSTING || mTextReturnScreen == Screen::JOINED)
				chooseScreen();
			else
				showScreen(mTextReturnScreen);
			break;

		default:
			showScreen(Screen::HOME);
			break;
	}
}

std::string LobbyMenu::getPlayerNameOfSlot(int slot) const
{
	if (nullptr != netplay().getNetplayHost())
		return netplay().getNetplayHost()->getPlayerName(slot);
	if (nullptr != netplay().getNetplayClient() && netplay().getNetplayClient()->hasLobbyState())
		return netplay().getNetplayClient()->getLobbyName(slot);
	return std::string();
}

int LobbyMenu::getPingOfSlot(int slot) const
{
	if (nullptr != netplay().getNetplayHost())
		return netplay().getNetplayHost()->getPlayerPing(slot);
	if (nullptr != netplay().getNetplayClient() && netplay().getNetplayClient()->hasLobbyState())
		return netplay().getNetplayClient()->getLobbyPing(slot);
	return -1;
}

void LobbyMenu::getSlotStates(uint8 outStates[4], bool& outConnected) const
{
	for (int k = 0; k < 4; ++k)
		outStates[k] = 0;
	outConnected = false;

	if (nullptr != netplay().getNetplayHost())
	{
		netplay().getNetplayHost()->getLobbySlots(outStates);
		outConnected = true;
	}
	else if (nullptr != netplay().getNetplayClient() && netplay().getNetplayClient()->hasLobbyState())
	{
		for (int k = 0; k < 4; ++k)
			outStates[k] = netplay().getNetplayClient()->getLobbySlotState(k);
		outConnected = true;
	}
}

int LobbyMenu::getMySlot() const
{
	if (nullptr != netplay().getNetplayHost())
		return 0;
	if (nullptr != netplay().getNetplayClient() && netplay().getNetplayClient()->hasLobbyState())
		return (int)netplay().getNetplayClient()->getLobbyYourSlot();
	return -1;
}

uint16 LobbyMenu::getCharacterOfSlot(int slot)
{
	if (nullptr != netplay().getNetplayHost())
		return netplay().getNetplayHost()->getCharacterOfSlot(slot);
	if (nullptr != netplay().getNetplayClient() && netplay().getNetplayClient()->hasLobbyState())
		return netplay().getNetplayClient()->getLobbyCharacter(slot);
	return NetplayLobby::CHARACTER_NONE;
}

uint16 LobbyMenu::getMyCharacter()
{
	const int slot = getMySlot();
	return (slot < 0) ? NetplayLobby::CHARACTER_NONE : getCharacterOfSlot(slot);
}

bool LobbyMenu::areCharactersLocked()
{
	if (nullptr != netplay().getNetplayHost())
		return netplay().getNetplayHost()->arePicksLocked();
	if (nullptr != netplay().getNetplayClient())
		return netplay().getNetplayClient()->areLobbyCharactersLocked();
	return false;
}

// Left and right (or the accept button) go through the characters this player may pick; the host decides what is accepted
void LobbyMenu::changeCharacter(int step)
{
	NetplayManager& manager = netplay();
	const int slot = getMySlot();
	if (slot < 0 || areCharactersLocked())
	{
		playMenuSound(0xad);
		return;
	}
	const NetplayLobby::CharacterList& list = manager.getAvailableCharacters();
	const uint16 next = NetplayLobby::nextCharacterChoice(slot, getMyCharacter(), step, list, manager.getConfiguredPartyExtraSlot(slot));
	playMenuSound(0x5b);
	if (nullptr != manager.getNetplayHost())
		manager.setHostCharacterPick(next);
	else if (nullptr != manager.getNetplayClient())
		manager.getNetplayClient()->setMyCharacter(next);
}

// The next connected friend (slot 1 to 3) after "startSlot" in the direction "step", or 0 if nobody is there
int LobbyMenu::findKickTarget(int startSlot, int step) const
{
	uint8 states[4];
	bool connected = false;
	getSlotStates(states, connected);
	for (int i = 1; i <= 3; ++i)
	{
		const int slot = ((startSlot - 1 + step * i) % 3 + 3) % 3 + 1;
		if (states[slot] >= 2)
			return slot;
	}
	return 0;
}

void LobbyMenu::sendQuickChat()
{
	NetplayManager& manager = netplay();
	if (nullptr != manager.getNetplayHost())
		manager.getNetplayHost()->sendHostChat(mQuickIndex, std::string());
	else if (nullptr != manager.getNetplayClient())
		manager.getNetplayClient()->sendChat(mQuickIndex, std::string());
}

void LobbyMenu::updateEntryTexts()
{
	NetplayManager& manager = netplay();
	NetplayHost* host = manager.getNetplayHost();
	NetplayClient* client = manager.getNetplayClient();

	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(INPUT_DELAY))
	{
		const int setting = (nullptr != host) ? host->getInputDelaySetting() : manager.getInputDelaySetting();
		const int effective = (nullptr != host) ? host->getEffectiveInputDelay() : 0;
		entry->mText = "INPUT DELAY: " + NetplayLobby::describeInputDelay(setting, effective);
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(CHAT_QUICK))
	{
		const char* quick = NetplayLobby::getQuickMessage(mQuickIndex);
		entry->mText = std::string("SAY: ") + ((nullptr != quick) ? quick : "(WRITE A MESSAGE...)");
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(KICK))
	{
		if (mKickSlot == 0 || getPlayerNameOfSlot(mKickSlot).empty())
			mKickSlot = findKickTarget(0, 1);
		entry->mText = (mKickSlot == 0) ? std::string("KICK: (NOBODY)") : ("KICK: " + shorten(getPlayerNameOfSlot(mKickSlot), 12));
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(CHARACTER))
	{
		const uint16 mine = getMyCharacter();
		entry->mText = "CHARACTER: " + ((mine == NetplayLobby::CHARACTER_NONE) ? std::string("...") : NetplayLobby::getCharacterName(mine)) + (areCharactersLocked() ? "  (FIXED)" : "");
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(EDIT_NAME))
		entry->mText = "NAME: " + manager.getLocalPlayerName();
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(EDIT_LOBBY_NAME))
		entry->mText = "NAME: " + shorten(mLobbyName, 20);
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(TOGGLE_PUBLIC))
		entry->mText = std::string("LOBBY: ") + (mLobbyPublic ? "PUBLIC" : "PRIVATE (CODE ONLY)");
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(MAX_PLAYERS))
		entry->mText = "PLAYERS: " + std::to_string(mLobbyMaxPlayers);
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(FILTER))
		entry->mText = std::string("SHOW: ") + (mListCompatibleOnly ? "MY VERSION ONLY" : "ALL LOBBIES");
	if (mScreen == Screen::SETTINGS)
	{
		const std::string lobbyServer = manager.getLobbyServerSetting();
		const std::string relay = manager.getRelayDisplayName();
		if (GameMenuEntry* entry = mMenuEntries.getEntryByData(EDIT_LOBBY_SERVER))
			entry->mText = "LOBBY SERVER: " + (lobbyServer.empty() ? std::string("NOT SET") : shorten(lobbyServer, 28));
		if (GameMenuEntry* entry = mMenuEntries.getEntryByData(EDIT_RELAY))
			entry->mText = "RELAY SERVER: " + (manager.getRelayServerSetting().empty() ? std::string("NOT SET") : (manager.getExplicitRelayIsSet() ? shorten(relay, 26) : std::string("SAME AS LOBBY SERVER")));
		if (GameMenuEntry* entry = mMenuEntries.getEntryByData(EDIT_NAME))
			entry->mText = "NAME: " + manager.getLocalPlayerName();
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(READY))
		entry->mText = (nullptr != client && client->isLobbyReady()) ? "NOT READY" : "READY";
}

void LobbyMenu::updateJoinScreenInput(const InputManager::ControllerScheme& keys)
{
	// The number pad: 3 columns and 4 rows (1 2 3 / 4 5 6 / 7 8 9 / delete 0 join), then BACK. The D-pad moves, A presses.
	// Typed digits (number keys and the number pad of a keyboard) arrive through "textinput"; a letter key is also a controller button (A, S, D...), which must not press the pad.
	const bool typed = (mTextInputThisFrame || mTextInputLastFrame);
	int& selected = mMenuEntries.mSelectedEntryIndex;
	selected = clamp(selected, 0, PAD_KEY_COUNT);

	const int dx = (keys.Right.justPressedOrRepeat() ? 1 : 0) - (keys.Left.justPressedOrRepeat() ? 1 : 0);
	const int dy = (keys.Down.justPressedOrRepeat() ? 1 : 0) - (keys.Up.justPressedOrRepeat() ? 1 : 0);
	if (dx != 0 || dy != 0)
	{
		playMenuSound(0x5b);
		if (selected == PAD_KEY_COUNT)
		{
			selected = (dy < 0) ? (PAD_KEY_COUNT - 2) : 1;		// from BACK up to the 0 key, or down to the 2 key
		}
		else
		{
			int row = selected / 3;
			int column = selected % 3;
			column = (column + dx + 3) % 3;
			row += dy;
			selected = (row > 3) ? PAD_KEY_COUNT : ((row < 0) ? PAD_KEY_COUNT : (row * 3 + column));
		}
	}

	auto pressKey = [&](int key)
	{
		if (key == PAD_JOIN)
		{
			playMenuSound(0x63);
			joinWithCode(mCodeDigits);
		}
		else if (key == PAD_DELETE)
		{
			playMenuSound(0xad);
			if (!mCodeDigits.empty())
				mCodeDigits.pop_back();
			mMessage.clear();
		}
		else if ((int)mCodeDigits.size() < MAX_CODE_DIGITS)
		{
			playMenuSound(0x5b);
			mCodeDigits += (char)('0' + key);
			mMessage.clear();
		}
	};

	const bool start = keys.Start.justPressed();
	const bool accept = (keys.A.justPressed() || keys.X.justPressed()) && !typed;
	const bool back = (keys.Back.justPressed() || keys.B.justPressed()) && !typed;
	if (start)
	{
		pressKey(PAD_JOIN);
	}
	else if (accept)
	{
		if (selected == PAD_KEY_COUNT)
		{
			playMenuSound(0xad);
			goBackOneLevel();
		}
		else
		{
			pressKey(PAD_LAYOUT[selected]);
		}
	}
	else if (back)
	{
		// Backspace and B take the last digit away, and leave the screen when there is none (Escape always leaves)
		if (FTX::keyState(SDLK_ESCAPE) || mCodeDigits.empty())
		{
			playMenuSound(0xad);
			goBackOneLevel();
		}
		else
		{
			playMenuSound(0xad);
			mCodeDigits.pop_back();
			mMessage.clear();
		}
	}
}

void LobbyMenu::updateTestHooks(float timeElapsed)
{
	NetplayManager& manager = netplay();
	NetplayHost* host = manager.getNetplayHost();
	NetplayClient* client = manager.getNetplayClient();

	const bool inLobby = (nullptr != host && host->getConnectedClientCount() > 0) || (nullptr != client && client->hasLobbyState());
	if (!inLobby)
		return;
	mHookLobbyTimer += timeElapsed;

	// One chat line, "number|text" (number 1 to 12 for a quick message, 0 for the free text)
	if (!mHookChatDone && mHookLobbyTimer > 3.0f)
	{
		if (const char* chat = TestHooks::getEnv("S3AIR_LOBBY_CHAT"))
		{
			mHookChatDone = true;
			const int quick = atoi(chat);
			const char* bar = strchr(chat, '|');
			const std::string text = (nullptr != bar) ? std::string(bar + 1) : std::string();
			if (nullptr != host)
				host->sendHostChat(quick, text);
			else if (nullptr != client)
				client->sendChat(quick, text);
		}
	}

	// Six chat lines at once: the host has to drop most of them
	if (!mHookSpamDone && mHookLobbyTimer > 5.0f && nullptr != client && nullptr != TestHooks::getEnv("S3AIR_LOBBY_CHAT_SPAM"))
	{
		mHookSpamDone = true;
		for (int k = 0; k < 6; ++k)
			client->sendChat(1 + k, std::string());
	}

	// The host changes the input delay: "frames@seconds"
	if (!mHookDelayDone && nullptr != host)
	{
		if (const char* delay = TestHooks::getEnv("S3AIR_LOBBY_AUTODELAY"))
		{
			const char* at = strchr(delay, '@');
			if (mHookLobbyTimer > ((nullptr != at) ? (float)atof(at + 1) : 5.0f))
			{
				mHookDelayDone = true;
				manager.setInputDelaySetting(atoi(delay));
			}
		}
	}

	// The host kicks a player: "slot@seconds"
	if (!mHookKickDone && nullptr != host)
	{
		if (const char* kick = TestHooks::getEnv("S3AIR_LOBBY_AUTOKICK"))
		{
			const char* at = strchr(kick, '@');
			if (mHookLobbyTimer > ((nullptr != at) ? (float)atof(at + 1) : 5.0f))
			{
				mHookKickDone = true;
				host->kickPlayer(atoi(kick));
			}
		}
	}

	// The host presses START GAME after some seconds
	if (!mHookStartDone && nullptr != host)
	{
		if (const char* start = TestHooks::getEnv("S3AIR_LOBBY_AUTOSTART"))
		{
			if (mHookLobbyTimer > (float)atof(start))
			{
				mHookStartDone = true;
				pressStart();
			}
		}
	}
}

void LobbyMenu::update(float timeElapsed)
{
	GameMenuBase::update(timeElapsed);

	NetplayManager& manager = netplay();

	// Browser build: how long the host has been waiting for the relay server to know its lobby (after a while the lobby screen says that it cannot be reached)
	if (NetplayManager::isBrowserBuild() && mScreen == Screen::HOSTING && !manager.isRelayHostRegistered())
	{
		const float before = mRelayWaitSeconds;
		mRelayWaitSeconds += timeElapsed;
		if (before <= 10.0f && mRelayWaitSeconds > 10.0f)
			RMX_LOG_INFO("Lobby: cannot reach the relay server (the lobby screen says so)");
	}
	else
	{
		mRelayWaitSeconds = 0.0f;
	}

	// The network may have changed behind our back (connection lost, joined through F9, ...)
	if (mState == State::SHOW && mScreen != Screen::TEXT)
	{
		const bool hosting = (nullptr != manager.getNetplayHost());
		const bool joined = (nullptr != manager.getNetplayClient());
		if (mScreen == Screen::HOSTING && !hosting)
		{
			chooseScreen();
		}
		else if (mScreen == Screen::JOINED && !joined)
		{
			if (!manager.mLastErrorMessage.empty())
			{
				mMessage = manager.mLastErrorMessage;
				mFailure = manager.mLastFailure;
			}
			chooseScreen();
		}
		else if (mScreen != Screen::HOSTING && mScreen != Screen::JOINED && (hosting || joined))
		{
			chooseScreen();
		}
	}

	// A friend whose connection failed, or a host whose lobby could not be opened: say why, and offer to try again
	if (mState == State::SHOW && (mScreen == Screen::JOINED || mScreen == Screen::TEXT) && nullptr != manager.getNetplayClient() && manager.getNetplayClient()->getState() == NetplayClient::State::FAILED)
		showFailureAndReturnToStart();
	else if (mState == State::SHOW && (mScreen == Screen::HOSTING || mScreen == Screen::TEXT) && nullptr != manager.getNetplayHost() && manager.getNetplayHost()->getHostState() == NetplayHost::HostState::FAILED)
		showFailureAndReturnToStart();

	// Developer test switch: press Retry by itself, eight seconds after a failure was shown (only with the test hooks enabled)
	if (mState == State::SHOW && mScreen == Screen::HOME && mFailure != NetplayFailure::Reason::NONE && nullptr != TestHooks::getEnv("S3AIR_LOBBY_AUTORETRY"))
	{
		mAutoRetryTimer += timeElapsed;
		if (mAutoRetryTimer > 8.0f)
		{
			mAutoRetryTimer = 0.0f;
			retry();
		}
	}

	if (mState == State::SHOW)
		updateTestHooks(timeElapsed);

	// Developer test switch: open one of the screens by itself (for screenshots): home, list, join (digits from S3AIR_LOBBY_JOINDIGITS), create, lan, settings, quick
	if (mState == State::SHOW && !mHookScreenDone && mScreen == Screen::HOME)
	{
		if (const char* hookScreen = TestHooks::getEnv("S3AIR_LOBBY_SCREEN"))
		{
			mHookScreenTimer += timeElapsed;
			if (mHookScreenTimer > 1.0f)
			{
				mHookScreenDone = true;
				if (strcmp(hookScreen, "list") == 0)
					showScreen(Screen::LIST);
				else if (strcmp(hookScreen, "join") == 0)
				{
					mCodeDigits = NetplayCodes::keepDigitsOnly((nullptr != TestHooks::getEnv("S3AIR_LOBBY_JOINDIGITS")) ? TestHooks::getEnv("S3AIR_LOBBY_JOINDIGITS") : "", (size_t)MAX_CODE_DIGITS);
					showScreen(Screen::JOIN);
				}
				else if (strcmp(hookScreen, "create") == 0)
					showScreen(Screen::CREATE);
				else if (strcmp(hookScreen, "lan") == 0)
					showScreen(Screen::LAN);
				else if (strcmp(hookScreen, "settings") == 0)
					showScreen(Screen::SETTINGS);
				else if (strcmp(hookScreen, "quick") == 0)
					beginQuickJoin();
			}
		}
	}

	// Waiting for the lobby server (quick join, looking up a code)
	if (mState == State::SHOW && mScreen == Screen::SEARCHING && mFlow != Flow::NONE)
	{
		mFlowSeconds += timeElapsed;
		updateFlow();
	}

	// The list of public lobbies: shown as it arrives, and asked for again every few seconds
	if (mState == State::SHOW && mScreen == Screen::LIST)
	{
		LobbyDirectory& directory = manager.getLobbyDirectory();
		if (directory.getListState() == LobbyDirectory::RequestState::DONE && directory.getListAgeSeconds() < mLastListAge)
		{
			mListSelectedCode.clear();
			mListFooterSelected = -1;
			const int rows = (int)mShownList.size();
			if (mMenuEntries.hasSelected())
			{
				const int index = mMenuEntries.mSelectedEntryIndex;
				if (index < rows)
					mListSelectedCode = mShownList[(size_t)index].mCode;
				else
					mListFooterSelected = index - rows;
			}
			mShownList = directory.getList();
			rebuildEntries();
		}
		mLastListAge = directory.getListAgeSeconds();
		mListRefreshTimer -= timeElapsed;
		if (mListRefreshTimer <= 0.0f && directory.getListState() != LobbyDirectory::RequestState::PENDING)
			refreshList();
	}

	// A ready check that ended: the host goes on to pick the game, without the players who were left out
	if (mState == State::SHOW && mScreen == Screen::HOSTING && nullptr != manager.getNetplayHost() && manager.getNetplayHost()->takeReadyCheckFinished())
		startGame(true);

	// A hint of the host ("Nobody has joined yet", "Asked everybody to get ready") goes away after a few seconds, so that the ready check countdown, the relay and
	// high ping warnings and the status line below can show (they are not drawn while a message is set)
	if (mScreen == Screen::HOSTING && mHostingMessageSeconds > 0.0f)
	{
		mHostingMessageSeconds -= timeElapsed;
		if (mHostingMessageSeconds <= 0.0f)
		{
			mHostingMessageSeconds = 0.0f;
			mMessage.clear();
		}
	}

	// The host is warned about a friend with a high ping (the text is drawn in "render"; the log says when it first appeared)
	if (mState == State::SHOW && mScreen == Screen::HOSTING && nullptr != manager.getNetplayHost())
	{
		int pingMs = 0;
		const int warnedSlot = manager.getNetplayHost()->getHighPingSlot(pingMs);
		if (warnedSlot != mLastWarnedSlot)
		{
			mLastWarnedSlot = warnedSlot;
			if (warnedSlot > 0)
				RMX_LOG_INFO("Lobby: showing the high ping warning for player " << (warnedSlot + 1) << " (" << pingMs << " ms)");
		}
	}

	// The host changed this player's character because it was not possible (or not installed any more): say so for a few seconds
	if (nullptr != manager.getNetplayClient() && manager.getNetplayClient()->takeCharacterNotice())
	{
		mCharacterNoticeText = "Your character is not available. Now: " + NetplayLobby::getCharacterName(getMyCharacter());
		mCharacterNoticeSeconds = 6.0f;
	}
	if (nullptr != manager.getNetplayHost())
	{
		const bool hostNotice = manager.getNetplayHost()->hasCharacterNotice(0);
		if (hostNotice && !mHostCharacterNoticeSeen)
		{
			mCharacterNoticeText = "Your character is not available any more. Now: " + NetplayLobby::getCharacterName(getMyCharacter());
			mCharacterNoticeSeconds = 6.0f;
		}
		mHostCharacterNoticeSeen = hostNotice;
	}
	if (mCharacterNoticeSeconds > 0.0f)
		mCharacterNoticeSeconds = std::max(0.0f, mCharacterNoticeSeconds - timeElapsed);

	updateEntryTexts();

	if (mState == State::SHOW)
	{
		const InputManager::ControllerScheme& keys = InputManager::instance().getController(0);

		if (mScreen == Screen::JOIN)
		{
			updateJoinScreenInput(keys);
		}
		else
		{
			const GameMenuEntries::UpdateResult result = mMenuEntries.update();
			if (result != GameMenuEntries::UpdateResult::NONE)
				playMenuSound(0x5b);

			const bool accept = (keys.Start.justPressed() || keys.A.justPressed() || keys.X.justPressed());
			const bool back = (keys.Back.justPressed() || keys.B.justPressed());
			const bool typing = (mScreen == Screen::TEXT && mMenuEntries.hasSelected() && mMenuEntries.selected().is<InputFieldMenuEntry>());
			const int leftRight = (keys.Right.justPressed() ? 1 : 0) - (keys.Left.justPressed() ? 1 : 0);

			// Entries that are changed with Left and Right
			if (leftRight != 0 && !typing && mMenuEntries.hasSelected())
			{
				switch (mMenuEntries.selected().mData)
				{
					case INPUT_DELAY:
						playMenuSound(0x5b);
						manager.setInputDelaySetting(NetplayLobby::nextDelaySetting(manager.getInputDelaySetting(), leftRight));
						break;

					case CHAT_QUICK:
						playMenuSound(0x5b);
						mQuickIndex = (mQuickIndex - 1 + leftRight + WRITE_MESSAGE_INDEX) % WRITE_MESSAGE_INDEX + 1;
						break;

					case KICK:
						playMenuSound(0x5b);
						mKickSlot = findKickTarget(mKickSlot == 0 ? 0 : mKickSlot, leftRight);
						break;

					case CHARACTER:
						changeCharacter(leftRight);
						break;

					case TOGGLE_PUBLIC:
						playMenuSound(0x5b);
						mLobbyPublic = !mLobbyPublic;
						Configuration::instance().mOnlineLobbyPublic = mLobbyPublic;
						Configuration::instance().saveSettings();
						break;

					case MAX_PLAYERS:
						playMenuSound(0x5b);
						mLobbyMaxPlayers = (mLobbyMaxPlayers - 2 + leftRight + 3) % 3 + 2;
						Configuration::instance().mOnlineLobbyMaxPlayers = mLobbyMaxPlayers;
						Configuration::instance().saveSettings();
						break;

					case FILTER:
						playMenuSound(0x5b);
						mListCompatibleOnly = !mListCompatibleOnly;
						Configuration::instance().mOnlineListCompatibleOnly = mListCompatibleOnly;
						Configuration::instance().saveSettings();
						refreshList();
						break;
				}
			}

			// While typing, only Start sends it (letters must not trigger menu buttons)
			if ((accept && !typing && mMenuEntries.hasSelected()) || (typing && keys.Start.justPressed()))
			{
				uint32 action = mMenuEntries.selected().mData;
				if (typing)
					action = TEXT_OK;

				if (action >= LIST_ROW && action < PAD_KEY)
					action = LIST_ROW;

				switch (action)
				{
					case LIST_ROW:
						joinListEntry((int)(mMenuEntries.selected().mData - LIST_ROW));
						break;

					case PUBLIC_LOBBIES:
						playMenuSound(0x63);
						showScreen(Screen::LIST);
						break;

					case QUICK_JOIN:
						playMenuSound(0x63);
						beginQuickJoin();
						break;

					case CREATE_LOBBY:
						playMenuSound(0x63);
						showScreen(Screen::CREATE);
						break;

					case JOIN_LOBBY:
						playMenuSound(0x63);
						mJoinLanOnly = false;
						mCodeDigits.clear();
						showScreen(Screen::JOIN);
						break;

					case LAN_GAME:
						playMenuSound(0x63);
						showScreen(Screen::LAN);
						break;

					case LAN_HOST:
						playMenuSound(0x63);
						createLobby(false, false, 4, mLobbyName, true);
						break;

					case LAN_JOIN:
						playMenuSound(0x63);
						mJoinLanOnly = true;
						mCodeDigits.clear();
						showScreen(Screen::JOIN);
						break;

					case SETTINGS:
						playMenuSound(0x63);
						showScreen(Screen::SETTINGS);
						break;

					case EDIT_LOBBY_SERVER:
						playMenuSound(0x63);
						openTextScreen(TextMode::LOBBY_SERVER, Screen::SETTINGS);
						break;

					case EDIT_LOBBY_NAME:
						playMenuSound(0x63);
						openTextScreen(TextMode::LOBBY_NAME, Screen::CREATE);
						break;

					case TOGGLE_PUBLIC:
						playMenuSound(0x5b);
						mLobbyPublic = !mLobbyPublic;
						Configuration::instance().mOnlineLobbyPublic = mLobbyPublic;
						Configuration::instance().saveSettings();
						break;

					case MAX_PLAYERS:
						playMenuSound(0x5b);
						mLobbyMaxPlayers = (mLobbyMaxPlayers - 2 + 1) % 3 + 2;
						Configuration::instance().mOnlineLobbyMaxPlayers = mLobbyMaxPlayers;
						Configuration::instance().saveSettings();
						break;

					case OPEN_LOBBY:
						playMenuSound(0x63);
						createLobby(true, mLobbyPublic, mLobbyMaxPlayers, mLobbyName);
						break;

					case REFRESH:
						playMenuSound(0x63);
						mMessage.clear();
						refreshList();
						break;

					case FILTER:
						playMenuSound(0x5b);
						mListCompatibleOnly = !mListCompatibleOnly;
						Configuration::instance().mOnlineListCompatibleOnly = mListCompatibleOnly;
						Configuration::instance().saveSettings();
						refreshList();
						break;

					case CANCEL:
						playMenuSound(0xad);
						goBackOneLevel();
						break;

					case RETRY:
						playMenuSound(0x63);
						retry();
						break;

					case READY:
						if (nullptr != manager.getNetplayClient())
						{
							playMenuSound(0x63);
							manager.getNetplayClient()->sendLobbyReady(!manager.getNetplayClient()->isLobbyReady());
						}
						break;

					case START_GAME:
						pressStart();
						break;

					case INPUT_DELAY:
						playMenuSound(0x5b);
						manager.setInputDelaySetting(NetplayLobby::nextDelaySetting(manager.getInputDelaySetting(), 1));
						break;

					case CHAT_QUICK:
						if (mQuickIndex == WRITE_MESSAGE_INDEX)
						{
							playMenuSound(0x63);
							openTextScreen(TextMode::CHAT, mScreen);
						}
						else
						{
							playMenuSound(0x63);
							sendQuickChat();
						}
						break;

					case KICK:
						if (mKickSlot != 0 && nullptr != manager.getNetplayHost())
						{
							playMenuSound(0x63);
							manager.getNetplayHost()->kickPlayer(mKickSlot);
							mKickSlot = findKickTarget(mKickSlot, 1);
						}
						else
						{
							playMenuSound(0xad);
						}
						break;

					case CHARACTER:
						changeCharacter(1);
						break;

					case EDIT_NAME:
						playMenuSound(0x63);
						openTextScreen(TextMode::NAME, mScreen);
						break;

					case EDIT_RELAY:
						playMenuSound(0x63);
						openTextScreen(TextMode::RELAY, Screen::SETTINGS);
						break;

					case TEXT_OK:
					{
						std::string text;
						if (mMenuEntries.size() > 0 && mMenuEntries[0].is<InputFieldMenuEntry>())
							text = mMenuEntries[0].as<InputFieldMenuEntry>().getTextUTF8();
						playMenuSound(0x63);
						auto addressUsable = [](const std::string& address)
						{
							std::string url;
							return NetplayManager::isBrowserBuild() ? NetplayLobby::normalizeRelayWebSocketURL(address, url) : NetplayLobby::isPlausibleServerAddress(address);
						};
						if (mTextMode == TextMode::NAME)
						{
							manager.setLocalPlayerName(text);
						}
						else if (mTextMode == TextMode::LOBBY_NAME)
						{
							mLobbyName = cleanLobbyName(text);
							Configuration::instance().mOnlineLobbyName = mLobbyName;
							Configuration::instance().saveSettings();
						}
						else if (mTextMode == TextMode::RELAY || mTextMode == TextMode::LOBBY_SERVER)
						{
							if (!text.empty() && !addressUsable(text))
							{
								mMessage = "That is not a usable address.\nUse a name like lobbies.example.org (or lobbies.example.org:28842).";
								mFailure = NetplayFailure::Reason::NONE;
								playMenuSound(0xad);
								break;		// (stay in the text field)
							}
							if (mTextMode == TextMode::RELAY)
								manager.setRelayServerSetting(text);
							else
								manager.setLobbyServerSetting(text);
							mMessage.clear();
							mFailure = NetplayFailure::Reason::NONE;
						}
						else if (nullptr != manager.getNetplayHost())
						{
							manager.getNetplayHost()->sendHostChat(0, text);
						}
						else if (nullptr != manager.getNetplayClient())
						{
							manager.getNetplayClient()->sendChat(0, text);
						}
						if (mTextReturnScreen == Screen::HOSTING || mTextReturnScreen == Screen::JOINED || mTextMode == TextMode::CHAT)
							chooseScreen();
						else
							showScreen(mTextReturnScreen);
						break;
					}

					case COPY_CODE:
						if (PlatformFunctions::hasClipboardSupport())
						{
							const std::string shortCode = manager.getPublishedLobbyCode();
							PlatformFunctions::copyToClipboard(shortCode.empty() ? NetplayCodes::makeSessionCode(manager.mSessionID) : shortCode);
							mShowCopied = true;
						}
						break;

					case LEAVE:
						playMenuSound(0xad);
						leaveLobby();
						break;

					case BACK:
						playMenuSound(0xad);
						goBackOneLevel();
						break;
				}
			}
			else if (back && !typing)
			{
				goBackOneLevel();
			}
			else if (back && typing && mMenuEntries.size() > 1)
			{
				// Back in the text field: go to the buttons below it
				mMenuEntries.mSelectedEntryIndex = (int)mMenuEntries.size() - 1;
			}
		}
	}

	mTextInputLastFrame = mTextInputThisFrame;
	mTextInputThisFrame = false;

	// Animation
	const float maxStep = timeElapsed * 10.0f;
	for (size_t i = 0; i < mMenuEntries.size(); ++i)
	{
		const float target = ((int)i == mMenuEntries.mSelectedEntryIndex) ? 1.0f : 0.0f;
		mMenuEntries[i].mAnimation.mHighlight += clamp(target - mMenuEntries[i].mAnimation.mHighlight, -maxStep, maxStep);
	}

	// Fade in and out
	if (mState == State::APPEAR)
	{
		if (updateFadeIn(timeElapsed * 4.0f))
			mState = State::SHOW;
	}
	else if (mState > State::SHOW)
	{
		if (updateFadeOut(timeElapsed * 4.0f))
			mState = State::INACTIVE;
	}
}

std::string LobbyMenu::getServerStatusLine(uint32& outColorKind) const
{
	// outColorKind: 0 = grey, 1 = green, 2 = orange, 3 = red
	NetplayManager& manager = netplay();
	const std::string name = shorten(manager.getLobbyServerDisplayName(), 30);
	switch (manager.getLobbyServerState())
	{
		case NetplayManager::LobbyServerState::NOT_SET:
			outColorKind = 2;
			return "No lobby server is set. LAN and long codes still work.";
		case NetplayManager::LobbyServerState::BAD_ADDRESS:
			outColorKind = 3;
			return "The lobby server address is not usable. Check SETTINGS.";
		case NetplayManager::LobbyServerState::CONNECTING:
			outColorKind = 2;
			return "Connecting to the lobby server " + name + "...";
		case NetplayManager::LobbyServerState::READY:
		{
			outColorKind = 1;
			const LobbyDirectory& directory = manager.getLobbyDirectory();
			std::string line = "Lobby server: " + name;
			if (directory.getPingMs() >= 0)
				line += "  -  " + std::to_string(directory.getPingMs()) + " ms";
			line += "  -  " + std::to_string(directory.getListedLobbyCount()) + (directory.getListedLobbyCount() == 1 ? " public lobby" : " public lobbies");
			return line;
		}
		case NetplayManager::LobbyServerState::UNREACHABLE:
			outColorKind = 3;
			return "The lobby server " + name + " does not answer.";
	}
	outColorKind = 0;
	return std::string();
}

void LobbyMenu::renderCodeKeypad(Drawer& drawer, int anchorX, float alpha, int top)
{
	const Color white(1.0f, 1.0f, 1.0f, alpha);
	const Color yellow(1.0f, 1.0f, 0.0f, alpha);
	const Color grey(0.7f, 0.7f, 0.7f, alpha);
	for (int key = 0; key < PAD_KEY_COUNT; ++key)
	{
		const int row = key / 3;
		const int column = key % 3;
		const bool isSelected = (mMenuEntries.mSelectedEntryIndex == key);
		const int value = PAD_LAYOUT[key];
		std::string label = (value == PAD_DELETE) ? "DEL" : ((value == PAD_JOIN) ? "JOIN" : std::string(1, (char)('0' + value)));
		const Vec2i position(anchorX + (column - 1) * 44, top + row * 15);
		if (isSelected)
			drawer.drawRect(Recti(position.x - 18, position.y - 1, 36, 13), Color(1.0f, 1.0f, 0.0f, 0.18f * alpha));
		const bool dim = (value == PAD_JOIN && mCodeDigits.empty());
		drawer.printText(global::mOxyfontRegular, Recti(position.x, position.y, 0, 10), label, 5, isSelected ? yellow : (dim ? grey : white));
	}
}

void LobbyMenu::renderBrowserScreen(Drawer& drawer, int anchorX, float alpha)
{
	NetplayManager& manager = netplay();
	const Color white(1.0f, 1.0f, 1.0f, alpha);
	const Color yellow(1.0f, 1.0f, 0.0f, alpha);
	const Color green(0.4f, 1.0f, 0.4f, alpha);
	const Color grey(0.7f, 0.7f, 0.7f, alpha);
	const Color dark(0.5f, 0.5f, 0.5f, alpha);
	const Color orange(1.0f, 0.75f, 0.5f, alpha);
	const Color red(1.0f, 0.45f, 0.4f, alpha);
	const Color kindColors[4] = { grey, green, orange, red };

	const char* title = "ONLINE";
	switch (mScreen)
	{
		case Screen::LIST:		title = "PUBLIC LOBBIES";  break;
		case Screen::JOIN:		title = mJoinLanOnly ? "JOIN A LAN GAME" : "JOIN WITH CODE";  break;
		case Screen::CREATE:	title = "CREATE LOBBY";  break;
		case Screen::LAN:		title = "LAN GAME";  break;
		case Screen::SETTINGS:	title = "ONLINE SETTINGS";  break;
		case Screen::SEARCHING:	title = (mFlow == Flow::QUICK) ? "QUICK JOIN" : "JOINING";  break;
		default: break;
	}
	const bool compactTitle = (mScreen == Screen::LIST || mScreen == Screen::JOIN);
	int py = compactTitle ? 6 : 14;
	drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), title, 5, white);
	py += compactTitle ? 20 : 26;

	auto statusLine = [&](int y)
	{
		uint32 kind = 0;
		const std::string line = getServerStatusLine(kind);
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, y), shorten(line, 70), 5, kindColors[kind]);
	};
	auto drawEntries = [&](int top, int step, bool smallLayout)
	{
		for (size_t i = 0; i < mMenuEntries.size(); ++i)
		{
			GameMenuEntry& entry = mMenuEntries[i];
			const bool isSelected = ((int)i == mMenuEntries.mSelectedEntryIndex);
			Color color = isSelected ? yellow : white;
			// Things that need a lobby server are dimmed while there is none (they still answer, with what to do)
			if ((entry.mData == PUBLIC_LOBBIES || entry.mData == QUICK_JOIN) && manager.getLobbyServerState() == NetplayManager::LobbyServerState::NOT_SET)
				color = isSelected ? Color(0.8f, 0.8f, 0.4f, alpha) : dark;
			if (entry.mData == OPEN_LOBBY && !mLobbyPublic)
				color = isSelected ? yellow : white;
			drawer.printText(smallLayout ? global::mOxyfontSmall : global::mOxyfontRegular, Recti(anchorX, top + (int)i * step, 0, 10), entry.mText, 5, color);
		}
	};

	switch (mScreen)
	{
		case Screen::HOME:
		{
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Play with up to three friends. Find a public lobby,", 5, grey);
			py += 11;
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "open one yourself, or join with a code.", 5, grey);
			py += 15;
			statusLine(py);
			py += 15;
			drawEntries(py, 14, false);
			break;
		}

		case Screen::CREATE:
		{
			statusLine(py);
			py += 13;
			if (manager.isLobbyServerConfigured())
			{
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), mLobbyPublic ? "A public lobby is shown to everybody in PUBLIC LOBBIES." : "A private lobby is not listed. Friends need its code.", 5, grey);
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 10), "The code of the lobby is shown when it is open.", 5, grey);
			}
			else
			{
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Without a lobby server the lobby is not listed: it is private.", 5, orange);
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 10), "Friends join with the long code (the matchmaking server, or LAN).", 5, grey);
			}
			py += 32;
			drawEntries(py, 16, false);
			break;
		}

		case Screen::LAN:
		{
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Play with friends on the same network.", 5, grey);
			py += 11;
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "No server is needed.", 5, grey);
			py += 18;
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Host a game and tell your friends the long code,", 5, grey);
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 10), "or type the long code that they give you.", 5, grey);
			py += 30;
			drawEntries(py, 16, false);
			break;
		}

		case Screen::SETTINGS:
		{
			statusLine(py);
			py += 14;
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "The lobby server lists public lobbies and gives out the 6 digit codes.", 5, grey);
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 10), "The relay connects players whose direct connection fails (and browsers).", 5, grey);
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 20), "Both can be the same server. Nothing is set by default.", 5, grey);
			py += 38;
			drawEntries(py, 16, false);
			break;
		}

		case Screen::SEARCHING:
		{
			statusLine(py);
			py += 24;
			const int dots = (int)(std::fmod(FTX::getTime(), 1.2f) / 0.3f);
			const std::string dotText(dots, '.');
			std::string what;
			if (mFlow == Flow::QUICK)
				what = "Looking for the best open lobby";
			else if (!mFlowLobbyName.empty())
				what = "Joining \"" + shorten(mFlowLobbyName, 20) + "\"";
			else
				what = "Looking up the lobby code";
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX - 80, py), what + dotText, 4, white);
			py += 30;
			drawEntries(py, 16, false);
			break;
		}

		case Screen::JOIN:
		{
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), mJoinLanOnly ? "Type the long code of the LAN game:" : "Type or paste the number your friend gave you:", 5, grey);
			py += 15;
			// The digits typed so far, with a cursor; before that, the shape of the code
			std::string shown = groupDigits(mCodeDigits);
			const bool blink = (std::fmod(FTX::getTime(), 1.0f) < 0.6f);
			if ((int)mCodeDigits.size() < MAX_CODE_DIGITS && blink)
				shown += "_";
			if (mCodeDigits.empty())
				drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), blink ? "_" : " ", 5, yellow);
			else
				drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), shown, 5, yellow);
			py += 20;
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), mJoinLanOnly ? "13 digits. Your friend can copy it from their lobby screen." : "6 digits: a lobby code.  13 digits: a long code (LAN, no server).", 5, grey);
			py += 14;
			renderCodeKeypad(drawer, anchorX, alpha, py);
			// BACK under the pad
			{
				const bool isSelected = (mMenuEntries.mSelectedEntryIndex == PAD_KEY_COUNT);
				drawer.printText(global::mOxyfontRegular, Recti(anchorX, py + 4 * 15 + 1, 0, 10), "BACK", 5, isSelected ? yellow : white);
			}
			break;
		}

		case Screen::LIST:
		{
			statusLine(py);
			py += 12;
			const int tableLeft = anchorX - 176;
			drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft, py), "LOBBY", 4, dark);
			drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 112, py), "HOST", 4, dark);
			drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 178, py), "PLAYERS", 4, dark);
			drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 226, py), "STATUS", 4, dark);
			drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 282, py), "VERSION", 4, dark);
			py += 11;
			const int rows = (int)mShownList.size();
			const LobbyDirectory& directory = manager.getLobbyDirectory();
			if (rows == 0)
			{
				const NetplayManager::LobbyServerState serverState = manager.getLobbyServerState();
				std::string line1, line2;
				Color color = grey;
				if (serverState == NetplayManager::LobbyServerState::NOT_SET)
				{
					line1 = "No lobby server is set, so there are no public lobbies.";
					line2 = "Add one in SETTINGS, or use LAN GAME and long codes.";
					color = orange;
				}
				else if (serverState == NetplayManager::LobbyServerState::BAD_ADDRESS || serverState == NetplayManager::LobbyServerState::UNREACHABLE || directory.getListState() == LobbyDirectory::RequestState::FAILED)
				{
					line1 = "Could not get the list from the lobby server.";
					line2 = "It may be down, or you may be offline. Try REFRESH later.";
					color = red;
				}
				else if (directory.getListState() == LobbyDirectory::RequestState::DONE)
				{
					line1 = mListCompatibleOnly ? "No public lobby for your version right now." : "No public lobbies right now.";
					line2 = "Be the first: go back and choose CREATE LOBBY.";
				}
				else
				{
					line1 = "Looking for lobbies...";
				}
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 8), line1, 5, color);
				if (!line2.empty())
					drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py + 20), line2, 5, color);
			}
			const int selectedRow = (mMenuEntries.mSelectedEntryIndex < rows) ? mMenuEntries.mSelectedEntryIndex : -1;
			int first = mListScroll;
			if (selectedRow >= 0)
			{
				if (selectedRow < first)
					first = selectedRow;
				if (selectedRow >= first + LIST_ROWS_VISIBLE)
					first = selectedRow - LIST_ROWS_VISIBLE + 1;
			}
			first = clamp(first, 0, std::max(0, rows - LIST_ROWS_VISIBLE));
			mListScroll = first;
			for (int i = first; i < rows && i < first + LIST_ROWS_VISIBLE; ++i)
			{
				const DirectoryProtocol::ListEntry& entry = mShownList[(size_t)i];
				const bool isSelected = (i == selectedRow);
				const bool joinable = entry.isCompatible() && !entry.isInGame() && !entry.isFull();
				const Color base = isSelected ? yellow : (joinable ? white : dark);
				const int y = py + (i - first) * 11;
				if (isSelected)
					drawer.drawRect(Recti(tableLeft - 4, y - 1, 360, 11), Color(1.0f, 1.0f, 0.0f, 0.15f * alpha));
				drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft, y), shorten(entry.mName, 19), 4, base);
				drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 112, y), shorten(entry.mHostName, 11), 4, base);
				drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 178, y), std::to_string((int)entry.mPlayers) + "/" + std::to_string((int)entry.mMaxPlayers), 4, base);
				const char* status = entry.isInGame() ? "PLAYING" : (entry.isFull() ? "FULL" : "OPEN");
				drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 226, y), status, 4, (entry.isInGame() || entry.isFull()) ? (isSelected ? yellow : orange) : (isSelected ? yellow : (joinable ? green : dark)));
				drawer.printText(global::mOxyfontSmall, Vec2i(tableLeft + 282, y), entry.isCompatible() ? "OK" : "OTHER", 4, entry.isCompatible() ? (isSelected ? yellow : green) : (isSelected ? yellow : red));
			}
			if (first > 0)
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 180, py), "^", 5, grey);
			if (first + LIST_ROWS_VISIBLE < rows)
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 180, py + (LIST_ROWS_VISIBLE - 1) * 11), "v", 5, grey);

			// REFRESH, SHOW and BACK below the rows
			const int footerTop = py + LIST_ROWS_VISIBLE * 11 + 6;
			for (int k = 0; k < 3; ++k)
			{
				const int index = rows + k;
				if (index >= (int)mMenuEntries.size())
					break;
				const bool isSelected = (mMenuEntries.mSelectedEntryIndex == index);
				drawer.printText(global::mOxyfontRegular, Recti(anchorX, footerTop + k * 13, 0, 10), mMenuEntries[(size_t)index].mText, 5, isSelected ? yellow : white);
			}
			break;
		}

		default:
			break;
	}

	// At the bottom: what the player needs to know right now (a message can have several lines)
	const int bottomY = 202;
	if (!mMessage.empty())
	{
		int lineY = bottomY;
		size_t begin = 0;
		for (int line = 0; line < 3 && begin <= mMessage.size(); ++line)
		{
			size_t end = mMessage.find('\n', begin);
			if (end == std::string::npos)
				end = mMessage.size();
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, lineY), shorten(mMessage.substr(begin, end - begin), 72), 5, orange);
			lineY += 10;
			begin = end + 1;
		}
		if (mLastDrawnStatus != "message: " + mMessage)
		{
			mLastDrawnStatus = "message: " + mMessage;
			RMX_LOG_INFO("Lobby: on screen: message " << mMessage.substr(0, mMessage.find('\n')));
		}
	}
	else if (mScreen == Screen::LIST || mScreen == Screen::HOME)
	{
		const char* hint = (mScreen == Screen::LIST) ? "Choose a lobby and press A to join it." : "";
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, bottomY + 6), hint, 5, dark);
		mLastDrawnStatus.clear();
	}
	else
	{
		mLastDrawnStatus.clear();
	}
}

void LobbyMenu::render()
{
	GuiBase::render();

	Drawer& drawer = EngineMain::instance().getDrawer();
	NetplayManager& manager = netplay();

	int anchorX = 200;
	float alpha = 1.0f;
	if (mState != State::SHOW)
	{
		anchorX += roundToInt((1.0f - mVisibility) * 300.0f);
		alpha = mVisibility;
	}
	const int leftX = anchorX - 174;		// Left edge of the lists

	const Color white(1.0f, 1.0f, 1.0f, alpha);
	const Color yellow(1.0f, 1.0f, 0.0f, alpha);
	const Color green(0.4f, 1.0f, 0.4f, alpha);
	const Color grey(0.7f, 0.7f, 0.7f, alpha);
	const Color orange(1.0f, 0.75f, 0.5f, alpha);
	const Color red(1.0f, 0.45f, 0.4f, alpha);

	NetplayHost* host = manager.getNetplayHost();
	NetplayClient* client = manager.getNetplayClient();
	const bool inLobbyScreen = (mScreen == Screen::HOSTING || mScreen == Screen::JOINED);
	if (!inLobbyScreen && mScreen != Screen::TEXT)
	{
		renderBrowserScreen(drawer, anchorX, alpha);
		return;
	}

	int py = inLobbyScreen ? 4 : 18;
	drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), "ONLINE", 5, white);
	py += inLobbyScreen ? 18 : 22;

	// Code of the lobby
	if (mScreen == Screen::HOSTING)
	{
		const std::string lobbyCode = manager.getPublishedLobbyCode();
		const std::string longCode = NetplayCodes::makeSessionCode(manager.mSessionID);
		const bool onLobbyServer = manager.isPublishingLobby();
		if (!lobbyCode.empty())
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), mLobbyPublic ? "Public lobby. Friends can find it, or join with this code:" : "Private lobby. Friends join with this code:", 5, grey);
			py += 10;
			drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), NetplayCodes::formatLobbyCode(lobbyCode), 5, yellow);
			py += 16;
		}
		else
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), onLobbyServer ? "Your friends join with this long code (the short code comes soon):" : "Your friends join with this code:", 5, grey);
			py += 10;
			drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), longCode, 5, yellow);
			py += 16;
		}
		if (mShowCopied)
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Copied!", 5, green);
		}
		else if (nullptr != host && host->getHostState() == NetplayHost::HostState::CONNECT_TO_SERVER)
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Contacting the game server...", 5, orange);
		}
		else if (onLobbyServer && lobbyCode.empty())
		{
			const NetplayManager::LobbyServerState serverState = manager.getLobbyServerState();
			if (serverState == NetplayManager::LobbyServerState::UNREACHABLE || serverState == NetplayManager::LobbyServerState::BAD_ADDRESS)
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "The lobby server does not answer. Use the long code, or LAN.", 5, red);
			else
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Asking the lobby server for a lobby code...", 5, orange);
		}
		else if (!lobbyCode.empty() && mLobbyPublic)
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Listed as \"" + shorten(mLobbyName, 20) + "\"   Long code: " + longCode, 5, green);
		}
		else if (!lobbyCode.empty())
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Long code (same network): " + longCode, 5, grey);
		}
		else if (NetplayManager::isBrowserBuild() && !manager.isRelayHostRegistered())
		{
			if (mRelayWaitSeconds > 10.0f)
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Cannot reach the relay server. Friends cannot join yet.", 5, red);
			else
				drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Connecting to the relay server...", 5, orange);
		}
		py += 12;
	}
	else if (mScreen == Screen::JOINED)
	{
		const bool inLobby = (nullptr != client && client->hasLobbyState());
		const char* text = inLobby ? (client->isLobbyHostStarting() ? "The host is picking a game..." : "Press READY when you are set.") : "Connecting to the lobby...";
		if (nullptr != client && client->isRejoiningLobby())
			text = "Connection lost. Coming back to your slot...";
		const bool relayConnecting = (nullptr != client && client->isRelayConnecting());
		if (relayConnecting)
			text = "Direct connection failed. Connecting through the relay...";
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), text, 5, (inLobby && !client->isRejoiningLobby() && !relayConnecting) ? white : orange);
		py += 14;
		if (nullptr != client && client->isUsingRelay() && inLobby)
		{
			// Everything goes through another computer: that adds delay, so say it
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py - 4), "Playing through a relay server: expect more lag.", 5, orange);
			py += 8;
		}
	}
	else if (mScreen == Screen::TEXT)
	{
		const char* title = "Your message (40 characters at most):";
		switch (mTextMode)
		{
			case TextMode::NAME:			title = "Your name (12 letters at most):";  break;
			case TextMode::RELAY:			title = "Address of the relay server:";  break;
			case TextMode::LOBBY_SERVER:	title = "Address of the lobby server (empty = none):";  break;
			case TextMode::LOBBY_NAME:		title = "Name of your lobby (20 characters at most):";  break;
			default: break;
		}
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), title, 5, grey);
		py += 18;
	}

	// The four players: name, ping, status
	if (inLobbyScreen)
	{
		uint8 states[4];
		bool connected = false;
		getSlotStates(states, connected);
		const int mySlot = (mScreen == Screen::HOSTING) ? 0 : ((nullptr != client) ? (int)client->getLobbyYourSlot() : -1);

		for (int slot = 0; slot < 4; ++slot)
		{
			std::string name = getPlayerNameOfSlot(slot);
			if (name.empty())
				name = (states[slot] != 0) ? "Player" : "waiting...";
			std::string label = std::to_string(slot + 1) + "  " + shorten(name, 12);
			if (slot == 0)
				label += "  (host)";
			if (slot == mySlot)
				label += "  - you";

			const char* status = "WAITING...";
			Color color = grey;
			switch (states[slot])
			{
				case 1: status = "HOST";		color = white;	break;
				case 2: status = "NOT READY";	color = orange;	break;
				case 3: status = "READY";		color = green;	break;
				default: break;
			}
			if (!connected && slot != 0)
				status = "";

			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, py), label, 4, (states[slot] != 0) ? white : grey);

			// The character this player has picked (as the host accepted it), next to the name
			if (states[slot] != 0)
			{
				const uint16 character = getCharacterOfSlot(slot);
				if (character != NetplayLobby::CHARACTER_NONE)
					drawer.printText(global::mOxyfontSmall, Vec2i(anchorX - 22, py), "- " + shorten(NetplayLobby::getCharacterName(character), 12), 4, (slot == mySlot) ? yellow : white);
			}

			// Ping (round trip, as measured by the host)
			if (slot != 0 && states[slot] >= 2)
			{
				const int ping = getPingOfSlot(slot);
				if (ping >= 0)
				{
					const Color pingColor = NetplayLobby::isHighPing(ping) ? red : ((ping > 80) ? yellow : green);
					drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 84, py), std::to_string(ping) + " ms", 6, pingColor);
				}
				else
				{
					drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 84, py), "... ms", 6, grey);
				}
			}
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 174, py), status, 6, color);
			py += 11;
		}
		py += 4;

		// Everybody sees the input delay that the host chose
		const int delaySetting = (nullptr != host) ? host->getInputDelaySetting() : ((nullptr != client) ? client->getLobbyDelaySetting() : -1);
		const int delayEffective = (nullptr != host) ? host->getEffectiveInputDelay() : ((nullptr != client) ? client->getLobbyEffectiveDelay() : 0);
		drawer.printText(global::mOxyfontSmall, Vec2i(leftX, py), "Input delay: " + NetplayLobby::describeInputDelay(delaySetting, delayEffective), 4, grey);
		py += 14;
	}

	// Buttons
	const int buttonsTop = py;
	for (size_t i = 0; i < mMenuEntries.size(); ++i)
	{
		GameMenuEntry& entry = mMenuEntries[i];
		const bool isSelected = ((int)i == mMenuEntries.mSelectedEntryIndex);

		if (entry.is<InputFieldMenuEntry>())
		{
			GameMenuEntry::RenderContext context;
			context.mDrawer = &drawer;
			context.mCurrentPosition = Vec2i(anchorX - 100, py);
			context.mIsSelected = isSelected;
			context.mDeltaSeconds = 0.0f;
			entry.performRenderEntry(context);
			py += 22;
			continue;
		}

		Color color = isSelected ? yellow : white;
		if (entry.mData == START_GAME)
		{
			const bool canStart = (nullptr != host && host->areAllClientsReady());
			if (!canStart)
				color = isSelected ? Color(0.8f, 0.8f, 0.4f, alpha) : grey;
		}
		else if (entry.mData == KICK && mKickSlot == 0)
		{
			color = isSelected ? Color(0.8f, 0.8f, 0.4f, alpha) : grey;
		}

		if (inLobbyScreen)
		{
			// A compact list on the left, so that there is room for the chat on the right
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, py), (isSelected ? "> " : "  ") + entry.mText, 4, color);
			py += 10;
		}
		else
		{
			drawer.printText(global::mOxyfontRegular, Recti(anchorX, py, 0, 10), entry.mText, 5, color);
			py += 16;
		}
	}

	// Chat, on the right of the buttons
	if (inLobbyScreen)
	{
		const std::deque<NetplayHost::ChatLine> hostLog = (nullptr != host) ? host->getChatLog() : std::deque<NetplayHost::ChatLine>();
		std::vector<std::pair<int, std::string>> lines;
		if (nullptr != host)
		{
			for (const NetplayHost::ChatLine& line : hostLog)
				lines.emplace_back(line.mSlot, line.mText);
		}
		else if (nullptr != client)
		{
			for (const NetplayClient::ChatLine& line : client->getChatLog())
				lines.emplace_back(line.mSlot, line.mText);
		}
		int chatY = buttonsTop;
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 8, chatY), "Chat", 4, grey);
		chatY += 10;
		const size_t first = (lines.size() > 5) ? (lines.size() - 5) : 0;
		for (size_t i = first; i < lines.size(); ++i)
		{
			const std::string who = shorten(getPlayerNameOfSlot(lines[i].first).empty() ? ("Player " + std::to_string(lines[i].first + 1)) : getPlayerNameOfSlot(lines[i].first), 8);
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 8, chatY), shorten(who + ": " + lines[i].second, 34), 4, white);
			chatY += 10;
		}
	}

	// At the bottom: what the player needs to know right now
	const int bottomY = 202;
	std::string drawnStatus = "nothing";
	if (!mMessage.empty())
	{
		drawnStatus = "message";
		// The message can have several lines (the first ones say what happened and what to do)
		int lineY = bottomY;
		size_t begin = 0;
		for (int line = 0; line < 3 && begin <= mMessage.size(); ++line)
		{
			size_t end = mMessage.find('\n', begin);
			if (end == std::string::npos)
				end = mMessage.size();
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, lineY), shorten(mMessage.substr(begin, end - begin), 58), 4, orange);
			lineY += 10;
			begin = end + 1;
		}
	}
	else if (mCharacterNoticeSeconds > 0.0f)
	{
		drawnStatus = "character notice";
		drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), shorten(mCharacterNoticeText, 58), 4, orange);
	}
	else if (mScreen == Screen::JOINED && nullptr != client && client->getLobbyReadyCheckSeconds() > 0 && !client->isLobbyReady())
	{
		drawnStatus = "friend ready prompt";
		// The host wants to start: whoever is not ready is asked to be
		const bool blink = (std::fmod(FTX::getTime(), 0.8f) < 0.5f);
		drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "THE HOST WANTS TO START! PRESS READY (" + std::to_string(client->getLobbyReadyCheckSeconds()) + ")", 4, blink ? yellow : orange);
	}
	else if (mScreen == Screen::HOSTING && nullptr != host)
	{
		int highPing = 0;
		const int highSlot = host->getHighPingSlot(highPing);
		const int relayedSlot = host->getRelayedSlot();
		if (relayedSlot > 0 && !host->isReadyCheckActive())
		{
			drawnStatus = "relay warning";
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "WARNING: " + shorten(host->getPlayerName(relayedSlot), 12) + " plays through the relay server.", 4, red);
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY + 10), "That adds delay for them. Input delay evens out the lag.", 4, grey);
		}
		else if (host->isReadyCheckActive())
		{
			drawnStatus = "ready check countdown";
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "Waiting for players to get ready... " + std::to_string(host->getReadyCheckSecondsLeft()) + " s", 4, yellow);
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY + 10), "Press START GAME again to start without them.", 4, grey);
		}
		else if (highSlot > 0)
		{
			drawnStatus = "high ping warning";
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "WARNING: " + shorten(host->getPlayerName(highSlot), 12) + " has a high ping (" + std::to_string(highPing) + " ms).", 4, red);
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY + 10), "The game may feel slow for them. Input delay evens out the lag.", 4, grey);
		}
		else
		{
			const int count = host->getConnectedClientCount();
			const char* hint = (count == 0) ? "Waiting for friends to join..." : (host->areAllClientsReady() ? "Everybody is ready!" : "Waiting for everybody to be ready...");
			drawnStatus = (count == 0) ? "waiting for friends hint" : (host->areAllClientsReady() ? "everybody ready hint" : "waiting for ready hint");
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), hint, 4, grey);
		}
	}

	// Written to the log whenever the bottom line changes: the checks read it to know what the lobby screen really showed (not only what the logic decided to show)
	if (drawnStatus != mLastDrawnStatus)
	{
		mLastDrawnStatus = drawnStatus;
		RMX_LOG_INFO("Lobby: on screen: " << drawnStatus);
	}
}
