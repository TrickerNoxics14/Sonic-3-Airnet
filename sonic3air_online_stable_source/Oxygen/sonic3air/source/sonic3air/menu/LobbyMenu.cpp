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
#include "sonic3air/ConfigurationImpl.h"
#include "sonic3air/menu/entries/GeneralMenuEntries.h"
#include "sonic3air/audio/AudioOut.h"

#include "oxygen/application/EngineMain.h"
#include "oxygen/application/Application.h"
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
	const int LIST_ROWS_VISIBLE = 5;

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
		ROOM_OPTIONS,
		LIST_ROW = 1000,		// + index of the row in the list
		PAD_KEY = 2000			// + 0 to 9 for the digits, 10 = delete, 11 = join
	};

	// The names of the screens in the log (the automated checks wait for them; START is the first screen)
	static const char* const SCREEN_NAMES[] = { "START", "LIST", "JOIN", "CREATE", "LAN", "SETTINGS", "SEARCHING", "HOSTING", "JOINED", "TEXT", "OPTIONS" };

	static const int LIST_QUICK_JOIN_SELECTED = -2;		// mListFooterSelected: QUICK JOIN, the first row of the list, was selected

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
	mHookScreenIndex = 0;
	mHookScreenDone = false;
	mHookTypeDone = false;
	mHookTypeTimer = 0.0f;

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
	// A key that types a character is also a controller button for letters A, S, D... of the default keyboard layout. Its key-down event arrives in the same frame
	// as that button press (the typed character may come a frame later, in a browser), so this is where a frame is marked as typing (see update)
	const bool printableKey = (ev.key >= 32 && ev.key < 127);
	if (mScreen == Screen::TEXT && mMenuEntries.hasSelected())
	{
		if (ev.state && (printableKey || ev.key == SDLK_BACKSPACE || ev.key == SDLK_DELETE) && mMenuEntries.selected().is<InputFieldMenuEntry>())
			mTextInputThisFrame = true;
		mMenuEntries.selected().keyboard(ev);
	}
	else if (mScreen == Screen::JOIN && ev.state)
	{
		if (printableKey)
			mTextInputThisFrame = true;		// (Backspace is not marked: on this screen it is the button that takes a digit away)
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
		// A typed letter is also a controller button (A, S, D... in the default keyboard layout): this frame it must only type (see update)
		mTextInputThisFrame = true;
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
	// Whatever is going on in the network decides what is shown (the lobby's OPTIONS stay open while there is a lobby)
	const bool inOptions = (mScreen == Screen::OPTIONS);
	if (nullptr != netplay().getNetplayHost())
		mScreen = inOptions ? Screen::OPTIONS : Screen::HOSTING;
	else if (nullptr != netplay().getNetplayClient())
		mScreen = inOptions ? Screen::OPTIONS : Screen::JOINED;
	else if (isRoomScreen() || mScreen == Screen::TEXT || (mScreen == Screen::SEARCHING && mFlow == Flow::NONE))
		mScreen = Screen::HOME;
	rebuildEntries();
}

bool LobbyMenu::isRoomScreen() const
{
	return (mScreen == Screen::HOSTING || mScreen == Screen::JOINED || mScreen == Screen::OPTIONS);
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
		mListFooterSelected = -1;
		mListScroll = 0;
		refreshList();
	}
	rebuildEntries();
}

void LobbyMenu::rebuildEntries()
{
	{
		RMX_LOG_INFO("Lobby: screen " << SCREEN_NAMES[(int)mScreen]);		// (what the automated checks wait for; START is the first screen)
	}

	mMenuEntries.clear();
	switch (mScreen)
	{
		case Screen::HOME:
			if (mFailure != NetplayFailure::Reason::NONE && NetplayFailure::isRetryable(mFailure) && (mLastWasHosting || !mLastJoinCode.empty()))
				mMenuEntries.addEntry("RETRY", RETRY);
			mMenuEntries.addEntry("FIND A LOBBY", PUBLIC_LOBBIES);
			mMenuEntries.addEntry("CREATE LOBBY", CREATE_LOBBY);
			mMenuEntries.addEntry("JOIN WITH CODE", JOIN_LOBBY);
			if (!NetplayManager::isBrowserBuild())		// (a browser cannot find games on the network: no UDP)
				mMenuEntries.addEntry("LAN GAME", LAN_GAME);
			mMenuEntries.addEntry("SETTINGS", SETTINGS);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::LIST:
			mMenuEntries.addEntry("QUICK JOIN", QUICK_JOIN);		// (the first row: the best open lobby of the list, chosen by the lobby server)
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
			mMenuEntries.addEntry("CHAT", CHAT_QUICK);
			mMenuEntries.addEntry("OPTIONS", ROOM_OPTIONS);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::JOINED:
			mMenuEntries.addEntry("READY", READY);
			mMenuEntries.addEntry("CHARACTER", CHARACTER);
			mMenuEntries.addEntry("CHAT", CHAT_QUICK);
			mMenuEntries.addEntry("OPTIONS", ROOM_OPTIONS);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::OPTIONS:
			// The host's input delay and kick (only while there is somebody to kick), everybody's name, the host's code
			mKickAvailable = false;
			if (nullptr != netplay().getNetplayHost())
			{
				mMenuEntries.addEntry("INPUT DELAY", INPUT_DELAY);
				mKickAvailable = (findKickTarget(0, 1) != 0);
				if (mKickAvailable)
					mMenuEntries.addEntry("KICK", KICK);
			}
			mMenuEntries.addEntry("NAME", EDIT_NAME);
			if (nullptr != netplay().getNetplayHost() && PlatformFunctions::hasClipboardSupport())
				mMenuEntries.addEntry("COPY CODE", COPY_CODE);
			mMenuEntries.addEntry("BACK", BACK);
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
		// The row that was selected stays selected when the list changes; else the first lobby that can be joined; else QUICK JOIN
		// (entry 0 is QUICK JOIN, the lobbies are the entries 1 to "rows", then REFRESH, SHOW and BACK)
		const int rows = (int)mShownList.size();
		int select = 0;
		if (mListFooterSelected == LIST_QUICK_JOIN_SELECTED)
		{
			select = 0;
		}
		else if (mListFooterSelected >= 0)
		{
			select = rows + 1 + std::min(mListFooterSelected, 2);
		}
		else if (!mListSelectedCode.empty())
		{
			for (int i = 0; i < rows; ++i)
			{
				if (mShownList[(size_t)i].mCode == mListSelectedCode)
				{
					select = i + 1;
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
					select = i + 1;
					break;
				}
			}
		}
		mMenuEntries.mSelectedEntryIndex = std::min(select, (int)mMenuEntries.size() - 1);
	}
	for (size_t i = 0; i < mMenuEntries.size(); ++i)
		mMenuEntries[i].mAnimation.mHighlight = 0.0f;
	updateEntryTexts();
	mLoggedSelection.clear();		// (logged again after each "Lobby: screen" line, even if it is the same entry)
	logSelection();
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
			if (NetplayManager::isBrowserBuild())
				return "No server is set, so online play is off in this page.\nChoose SETTINGS, then RELAY SERVER, and type the address of a server.";
			return "No lobby server is set yet, so there are no public lobbies.\nYou can still play: LAN GAME, or JOIN WITH CODE with a long code.\nA lobby server can be added in SETTINGS.";
		case NetplayManager::LobbyServerState::BAD_ADDRESS:
			return "The lobby server address does not look right.\nCheck it in SETTINGS. You can still use LAN GAME.";
		case NetplayManager::LobbyServerState::NO_LIST:
			return "This server has no list of public lobbies (it may be an older version).\nYou can still join with a long code of 13 digits.";
		default:
			if (NetplayManager::isBrowserBuild())
				return "The lobby server does not answer. It may be down, or you may be\noffline. Try again in a few minutes.";
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
		// (The developer test switch S3AIR_NETPLAY_MAXCLIENTS, read by the host itself, wins over the menu: the checks of a full lobby rely on it)
		if (nullptr != manager.getNetplayHost() && nullptr == TestHooks::getEnv("S3AIR_NETPLAY_MAXCLIENTS"))
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
			mMessage = "This long code has a typing mistake.\nCheck the digits one by one.";
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
	mSearchReturnScreen = (mScreen == Screen::LIST) ? Screen::LIST : Screen::HOME;		// (a quick join that finds nothing goes back to where it was chosen)
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
	if (serverState == NetplayManager::LobbyServerState::NOT_SET || serverState == NetplayManager::LobbyServerState::BAD_ADDRESS || serverState == NetplayManager::LobbyServerState::NO_LIST)
	{
		finishFlow(getNoServerMessage(), NetplayFailure::Reason::NONE);
		return;
	}
	// (A WebSocket server - the browser's, or a ws:// / wss:// one in the Windows game - that does not answer may be asleep. The directory keeps the request and the menu waits
	// for up to SERVER_WAKE_SECONDS, then this limit holds)
	const float flowLimit = manager.usesWebSocketServer() ? (NetplayManager::SERVER_WAKE_SECONDS + 20.0f) : 15.0f;

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
			const Screen returnScreen = mSearchReturnScreen;
			beginResolve(best->mCode, best->mName);
			mSearchReturnScreen = returnScreen;
			return;
		}
		if (directory.getListState() == LobbyDirectory::RequestState::FAILED || mFlowSeconds > flowLimit)
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
					manager.startJoinViaLobbyServer(reply.mSessionID, reply.isListed());
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
		if (directory.getResolveState() == LobbyDirectory::RequestState::FAILED || mFlowSeconds > flowLimit)
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
		mMessage = NetplayManager::isBrowserBuild() ? "This lobby plays another version of the game.\nReload the page: a new version may be out. Then try again."
													 : "This lobby plays another version of the game, or has other mods.\nYou cannot join it.";
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

void LobbyMenu::refreshList(bool userRequest)
{
	NetplayManager& manager = netplay();
	manager.setLobbyDirectoryInUse(true);
	if (!manager.isLobbyServerConfigured())
		return;
	manager.getLobbyDirectory().requestList(mListCompatibleOnly, manager.getCompatibilityKey(), userRequest);
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

		case Screen::OPTIONS:
			mScreen = Screen::HOSTING;		// (chooseScreen picks the lobby screen of a host or a friend, or the first screen if the lobby is gone)
			chooseScreen();
			break;

		case Screen::TEXT:
			if (mTextReturnScreen == Screen::HOSTING || mTextReturnScreen == Screen::JOINED || mTextReturnScreen == Screen::OPTIONS)
			{
				mScreen = mTextReturnScreen;
				chooseScreen();
			}
			else
			{
				showScreen(mTextReturnScreen);
			}
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
		entry->mText = std::string("CHAT: ") + ((nullptr != quick) ? quick : "Write a message...");
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(KICK))
	{
		if (mKickSlot == 0 || getPlayerNameOfSlot(mKickSlot).empty())
			mKickSlot = findKickTarget(0, 1);
		entry->mText = (mKickSlot == 0) ? std::string("KICK") : ("KICK: " + shorten(getPlayerNameOfSlot(mKickSlot), 12));
	}
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(CHARACTER))
	{
		const uint16 mine = getMyCharacter();
		const std::string name = (mine == NetplayLobby::CHARACTER_NONE) ? std::string("...") : ((mine == NetplayLobby::CHARACTER_MENU) ? std::string("CHOOSE IN GAME") : NetplayLobby::getCharacterName(mine));
		entry->mText = "CHARACTER: " + name + (areCharactersLocked() ? " (FIXED)" : "");
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

// The selected entry as the automated checks name it: its text up to the ':' ("CHARACTER: SONIC" is "CHARACTER"), "ROW <lobby name>" for a row of the list,
// "PAD <key>" for a key of the number pad, "TEXT FIELD" for the text field
std::string LobbyMenu::getSelectionLabel()
{
	if (!mMenuEntries.hasSelected())
		return "(nothing)";
	GameMenuEntry& entry = mMenuEntries.selected();
	if (entry.is<InputFieldMenuEntry>())
		return "TEXT FIELD";
	if (entry.mData >= PAD_KEY)
	{
		const int value = (int)(entry.mData - PAD_KEY);
		return std::string("PAD ") + ((value == PAD_DELETE) ? "DEL" : ((value == PAD_JOIN) ? "JOIN" : std::string(1, (char)('0' + value))));
	}
	if (entry.mData >= LIST_ROW)
		return "ROW " + entry.mText;
	std::string label = entry.mText.substr(0, entry.mText.find(':'));
	while (!label.empty() && label.back() == ' ')
		label.pop_back();
	return label;
}

void LobbyMenu::logSelection()
{
	const std::string label = getSelectionLabel();
	const std::string key = std::string(SCREEN_NAMES[(int)mScreen]) + "|" + label;
	if (key != mLoggedSelection)
	{
		mLoggedSelection = key;
		RMX_LOG_INFO("Lobby: selected " << label);		// (the automated checks pick entries by this label instead of counting key presses)
	}
}

void LobbyMenu::updateJoinScreenInput(const InputManager::ControllerScheme& keys)
{
	if (mCodeDigits.size() != mLoggedCodeDigits)
	{
		mLoggedCodeDigits = mCodeDigits.size();
		RMX_LOG_INFO("Lobby: the code screen holds " << mLoggedCodeDigits << " digits");		// (the automated checks read this)
	}

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
		// Backspace and B take the last digit away; with no digit left B leaves the screen, Backspace does not (a typist presses it once too often), Escape always leaves
		const bool backspaceKey = FTX::keyState(SDLK_BACKSPACE) || FTX::keyState(SDLK_DELETE);
		if (FTX::keyState(SDLK_ESCAPE) || (mCodeDigits.empty() && !backspaceKey))
		{
			playMenuSound(0xad);
			goBackOneLevel();
		}
		else if (!mCodeDigits.empty())
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

	// WebSocket server (browser, or ws:// / wss:// in the Windows game): how long the host has been waiting for the relay server to know its lobby (after a while the lobby
	// screen says that it cannot be reached)
	if (manager.usesWebSocketServer() && mScreen == Screen::HOSTING && !manager.isRelayHostRegistered())
	{
		const float before = mRelayWaitSeconds;
		mRelayWaitSeconds += timeElapsed;
		if (before <= 10.0f && mRelayWaitSeconds > 10.0f)
			RMX_LOG_INFO("Lobby: the relay server has not registered this lobby after 10 seconds (the lobby screen says what is going on, and the connection keeps being retried)");
	}
	else
	{
		mRelayWaitSeconds = 0.0f;
	}

	// Browser build: the lobby server does not answer yet (it may be asleep): the screens say so, and the requests wait and retry
	{
		const bool waking = (manager.getLobbyServerState() == NetplayManager::LobbyServerState::WAKING);
		if (waking && !mWasWaking)
		{
			RMX_LOG_INFO("Lobby: waking the server (retrying for up to " << (int)NetplayManager::SERVER_WAKE_SECONDS << " seconds)");
		}
		else if (!waking && mWasWaking)
		{
			RMX_LOG_INFO("Lobby: the server is no longer waking (" << ((manager.getLobbyServerState() == NetplayManager::LobbyServerState::READY) ? "it answered" : "it did not answer in time") << ")");
		}
		mWasWaking = waking;
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
		else if (mScreen == Screen::OPTIONS && !hosting && !joined)
		{
			if (!manager.mLastErrorMessage.empty())
			{
				mMessage = manager.mLastErrorMessage;
				mFailure = manager.mLastFailure;
			}
			chooseScreen();
		}
		else if (!isRoomScreen() && (hosting || joined))
		{
			chooseScreen();
		}
	}

	// A friend whose connection failed, or a host whose lobby could not be opened: say why, and offer to try again
	if (mState == State::SHOW && (mScreen == Screen::JOINED || mScreen == Screen::OPTIONS || mScreen == Screen::TEXT) && nullptr != manager.getNetplayClient() && manager.getNetplayClient()->getState() == NetplayClient::State::FAILED)
		showFailureAndReturnToStart();
	else if (mState == State::SHOW && (mScreen == Screen::HOSTING || mScreen == Screen::OPTIONS || mScreen == Screen::TEXT) && nullptr != manager.getNetplayHost() && manager.getNetplayHost()->getHostState() == NetplayHost::HostState::FAILED)
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

	// Developer test switch: open the screens one after the other, 6 seconds each (for screenshots): S3AIR_LOBBY_SCREEN=home,settings,create,lan,join,list,quick
	// (digits for the code screen come from S3AIR_LOBBY_JOINDIGITS)
	if (mState == State::SHOW && !mHookScreenDone && !isRoomScreen())
	{
		if (const char* hookScreens = TestHooks::getEnv("S3AIR_LOBBY_SCREEN"))
		{
			mHookScreenTimer += timeElapsed;
			if (mHookScreenTimer > ((mHookScreenIndex == 0) ? 1.0f : 6.0f))
			{
				mHookScreenTimer = 0.0f;
				// the name with number "mHookScreenIndex" in the list
				std::string name;
				{
					int index = 0;
					size_t begin = 0;
					const std::string all = hookScreens;
					while (begin <= all.size())
					{
						size_t end = all.find(',', begin);
						if (end == std::string::npos)
							end = all.size();
						if (index == mHookScreenIndex)
						{
							name = all.substr(begin, end - begin);
							break;
						}
						++index;
						begin = end + 1;
					}
				}
				++mHookScreenIndex;
				if (name.empty())
				{
					mHookScreenDone = true;
				}
				else
				{
					RMX_LOG_INFO("Lobby: (test) showing the screen " << name);
					mFlow = Flow::NONE;
					if (name == "home")
						showScreen(Screen::HOME);
					else if (name == "list")
						showScreen(Screen::LIST);
					else if (name == "join")
					{
						mJoinLanOnly = false;
						mCodeDigits = NetplayCodes::keepDigitsOnly((nullptr != TestHooks::getEnv("S3AIR_LOBBY_JOINDIGITS")) ? TestHooks::getEnv("S3AIR_LOBBY_JOINDIGITS") : "", (size_t)MAX_CODE_DIGITS);
						showScreen(Screen::JOIN);
					}
					else if (name == "create")
						showScreen(Screen::CREATE);
					else if (name == "lan")
						showScreen(Screen::LAN);
					else if (name == "settings")
						showScreen(Screen::SETTINGS);
					else if (name == "quick")
						beginQuickJoin();
				}
			}
		}
	}

	// Developer test switch: S3AIR_LOBBY_TYPE types text on the code screen as if it came from the keyboard (letters in it must be ignored)
	if (mState == State::SHOW && mScreen == Screen::JOIN && !mHookTypeDone)
	{
		if (const char* typed = TestHooks::getEnv("S3AIR_LOBBY_TYPE"))
		{
			mHookTypeTimer += timeElapsed;
			if (mHookTypeTimer > 2.0f)
			{
				mHookTypeDone = true;
				for (const char* p = typed; *p != 0; ++p)
				{
					rmx::TextInputEvent ev;
					ev.text = std::wstring(1, (wchar_t)*p);
					textinput(ev);
				}
				RMX_LOG_INFO("Lobby: (test) typed on the code screen, the field now holds " << mCodeDigits.size() << " digits");
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
			if (mMenuEntries.hasSelected() && rows > 0)		// (nothing was there to choose from before: the first lobby that can be joined gets selected)
			{
				const int index = mMenuEntries.mSelectedEntryIndex;
				if (index == 0)
					mListFooterSelected = LIST_QUICK_JOIN_SELECTED;
				else if (index <= rows)
					mListSelectedCode = mShownList[(size_t)(index - 1)].mCode;
				else
					mListFooterSelected = index - rows - 1;
			}
			mShownList = directory.getList();
			RMX_LOG_INFO("Lobby: the list shows " << mShownList.size() << ((mShownList.size() == 1) ? " public lobby" : " public lobbies"));		// (the automated checks read this)
			rebuildEntries();
		}
		mLastListAge = directory.getListAgeSeconds();
		mListRefreshTimer -= timeElapsed;
		if (mListRefreshTimer <= 0.0f && directory.getListState() != LobbyDirectory::RequestState::PENDING)
			refreshList(false);		// (the regular refresh: not a request of the player, see LobbyDirectory::requestList)
	}

	// A ready check that ended: the host goes on to pick the game, without the players who were left out
	if (mState == State::SHOW && isRoomScreen() && nullptr != manager.getNetplayHost() && manager.getNetplayHost()->takeReadyCheckFinished())
		startGame(true);

	// A hint of the host ("Nobody has joined yet", "Asked everybody to get ready") goes away after a few seconds, so that the ready check countdown, the relay and
	// high ping warnings and the status line below can show (they are not drawn while a message is set)
	if (isRoomScreen() && mHostingMessageSeconds > 0.0f)
	{
		mHostingMessageSeconds -= timeElapsed;
		if (mHostingMessageSeconds <= 0.0f)
		{
			mHostingMessageSeconds = 0.0f;
			mMessage.clear();
		}
	}

	// The host is warned about a friend with a high ping (the text is drawn in "render"; the log says when it first appeared)
	if (mState == State::SHOW && isRoomScreen() && nullptr != manager.getNetplayHost())
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

	if (mScreen == Screen::OPTIONS && nullptr != manager.getNetplayHost() && (findKickTarget(0, 1) != 0) != mKickAvailable)
	{
		const uint32 selectedData = mMenuEntries.hasSelected() ? mMenuEntries.selected().mData : 0;
		rebuildEntries();
		for (size_t i = 0; i < mMenuEntries.size(); ++i)
		{
			if (mMenuEntries[i].mData == selectedData)
				mMenuEntries.mSelectedEntryIndex = (int)i;
		}
	}

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
			// In the text field the keyboard edits text: a key that typed a letter (S is the B button) or Backspace / Delete (Backspace is the Back button) must not
			// also act as a menu button. A controller's B still leaves the field (to the buttons below), Enter (Start) still saves.
			const bool keyboardEditing = typing && (mTextInputThisFrame || mTextInputLastFrame || FTX::keyState(SDLK_BACKSPACE) || FTX::keyState(SDLK_DELETE));
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

					case ROOM_OPTIONS:
						playMenuSound(0x63);
						showScreen(Screen::OPTIONS);
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
						if (mTextReturnScreen == Screen::HOSTING || mTextReturnScreen == Screen::JOINED || mTextReturnScreen == Screen::OPTIONS || mTextMode == TextMode::CHAT)
						{
							mScreen = mTextReturnScreen;
							chooseScreen();
						}
						else
						{
							showScreen(mTextReturnScreen);
						}
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
			else if (back && typing && !keyboardEditing && mMenuEntries.size() > 1)
			{
				// Back in the text field: go to the buttons below it
				mMenuEntries.mSelectedEntryIndex = (int)mMenuEntries.size() - 1;
			}
		}
	}

	if (mState == State::SHOW)
		logSelection();

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
			return NetplayManager::isBrowserBuild() ? "No server is set. Choose SETTINGS, then RELAY SERVER." : "No lobby server is set. LAN and long codes still work.";
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
		case NetplayManager::LobbyServerState::WAKING:
			outColorKind = 2;
			return "Waking the server up, retrying (" + std::to_string((int)manager.getLobbyDirectory().getSecondsNotReady()) + " s)...";
		case NetplayManager::LobbyServerState::NO_LIST:
			outColorKind = 2;
			return "The server " + name + " has no lobby list (an older version?).";
		case NetplayManager::LobbyServerState::UNREACHABLE:
			outColorKind = 3;
			return "The lobby server " + name + " does not answer.";
	}
	outColorKind = 0;
	return std::string();
}

namespace
{
	const Color LOBBY_WHITE(1.0f, 1.0f, 1.0f);
	const Color LOBBY_YELLOW(1.0f, 1.0f, 0.0f);
	const Color LOBBY_GREEN(0.4f, 1.0f, 0.4f);
	const Color LOBBY_GREY(0.7f, 0.7f, 0.7f);
	const Color LOBBY_DARK(0.5f, 0.5f, 0.5f);
	const Color LOBBY_ORANGE(1.0f, 0.75f, 0.5f);
	const Color LOBBY_RED(1.0f, 0.45f, 0.4f);
	const Color LOBBY_HEADING(0.6f, 0.8f, 1.0f);		// (the "* TITLE *" lines of the Options menu)

	Color withAlpha(const Color& color, float alpha)
	{
		return Color(color.r, color.g, color.b, color.a * alpha);
	}

	// The A.I.R. menus: the arrows next to a selected entry move a little, back and forth
	int arrowWobble()
	{
		const int offset = (int)std::fmod(FTX::getTime() * 6.0f, 6.0f);
		return (offset > 3) ? (6 - offset) : offset;
	}

	// "CHARACTER: SONIC" is shown as the label "CHARACTER" with the value "SONIC"
	void splitEntryText(const std::string& text, std::string& outLabel, std::string& outValue)
	{
		const size_t colon = text.find(": ");
		outLabel = (colon == std::string::npos) ? text : text.substr(0, colon);
		outValue = (colon == std::string::npos) ? std::string() : text.substr(colon + 2);
	}

	// Entries whose value is changed with Left and Right
	bool isChangeableEntry(uint32 data)
	{
		switch (data)
		{
			case INPUT_DELAY:
			case CHAT_QUICK:
			case KICK:
			case CHARACTER:
			case TOGGLE_PUBLIC:
			case MAX_PLAYERS:
			case FILTER:
				return true;
			default:
				return false;
		}
	}

	// The small picture of a character from the game's own menus (0 for a character that has none, like an extra character of a mod)
	uint64 getCharacterSpriteKey(uint16 character)
	{
		switch (character)
		{
			case NetplayLobby::CHARACTER_SONIC:		return rmx::constMurmur2_64("menu_characters_sonic");
			case NetplayLobby::CHARACTER_TAILS:		return rmx::constMurmur2_64("menu_characters_tails");
			case NetplayLobby::CHARACTER_KNUCKLES:	return rmx::constMurmur2_64("menu_characters_knuckles");
			default:								return 0;
		}
	}

	// The text, cut so that it fits into "maxWidth" pixels (with "..." if it was cut; the large Sonic fonts have no dot)
	std::string fitText(Font& font, const std::string& text, int maxWidth, bool dots = true)
	{
		if (font.getWidth(text) <= maxWidth)
			return text;
		std::string result = text;
		while (!result.empty() && font.getWidth(dots ? (result + "...") : result) > maxWidth)
			result.pop_back();
		return dots ? (result + "...") : result;
	}

	void printLines(Drawer& drawer, Font& font, int centerX, int y, int step, const std::string& text, int maxLines, size_t maxLength, const Color& color)
	{
		size_t begin = 0;
		for (int line = 0; line < maxLines && begin <= text.size(); ++line)
		{
			size_t end = text.find('\n', begin);
			if (end == std::string::npos)
				end = text.size();
			drawer.printText(font, Recti(centerX, y, 0, 10), shorten(text.substr(begin, end - begin), maxLength), 5, color);
			y += step;
			begin = end + 1;
		}
	}
}

bool LobbyMenu::isEntryEnabled(uint32 data) const
{
	NetplayManager& manager = netplay();
	switch (data)
	{
		case START_GAME:
			return (nullptr != manager.getNetplayHost() && manager.getNetplayHost()->areAllClientsReady());
		case KICK:
			return (mKickSlot != 0);
		case PUBLIC_LOBBIES:
		case QUICK_JOIN:
			return (manager.getLobbyServerState() != NetplayManager::LobbyServerState::NOT_SET);
		default:
			return true;
	}
}

// The band at the top, as in the Options and Extras menus: the title, and on the right an optional text (the code of the lobby)
void LobbyMenu::drawHeader(Drawer& drawer, int anchorX, float alpha, const std::string& title, const std::string& rightText)
{
	drawer.drawRect(Recti(anchorX - 200, -6, 400, 48), global::mOptionsTopBar, Color(1.0f, 1.0f, 1.0f, alpha));
	if (rightText.empty())
	{
		drawer.printText(global::mSonicFontC, Recti(anchorX - 180, 4, 360, 20), title, 5, withAlpha(LOBBY_WHITE, alpha));
	}
	else
	{
		drawer.printText(global::mSonicFontC, Recti(anchorX - 188, 4, 0, 20), title, 4, withAlpha(LOBBY_WHITE, alpha));
		drawer.printText(global::mSonicFontB, Recti(anchorX + 188, 4, 0, 20), rightText, 6, withAlpha(LOBBY_YELLOW, alpha));
	}
}

// A dark box behind a list (the player rows, the lobbies), with the line on top that the Options menu draws above its messages
void LobbyMenu::drawPanel(Drawer& drawer, const Recti& rect, float alpha)
{
	drawer.drawRect(rect, Color(0.0f, 0.0f, 0.0f, 0.45f * alpha));
	drawer.drawRect(Recti(rect.x, rect.y - 1, rect.width, 1), Color(0.4f, 0.2f, 0.0f, alpha * 0.95f));
	drawer.drawRect(Recti(rect.x, rect.y - 2, rect.width, 1), Color(0.9f, 0.9f, 0.9f, alpha * 0.9f));
	drawer.drawRect(Recti(rect.x, rect.y - 3, rect.width, 1), Color(0.9f, 0.9f, 0.9f, alpha * 0.6f));
	drawer.drawRect(Recti(rect.x, rect.y - 4, rect.width, 1), Color(0.9f, 0.9f, 0.9f, alpha * 0.3f));
}

// An entry as in the Options menu: centered, yellow when selected, with ">>" and "<<" next to it
void LobbyMenu::drawRow(Drawer& drawer, Font& font, int centerX, int y, const std::string& text, bool selected, bool enabled, float alpha)
{
	const Color color = withAlpha(selected ? (enabled ? LOBBY_YELLOW : Color(0.8f, 0.8f, 0.4f)) : (enabled ? LOBBY_WHITE : LOBBY_DARK), alpha);
	drawer.printText(font, Recti(centerX, y, 0, 10), text, 5, color);
	if (selected)
	{
		const int halfWidth = font.getWidth(text) / 2;
		const int distance = 16 + arrowWobble();
		drawer.printText(global::mOxyfontRegular, Recti(centerX - halfWidth - distance, y, 0, 10), ">>", 5, color);
		drawer.printText(global::mOxyfontRegular, Recti(centerX + halfWidth + distance, y, 0, 10), "<<", 5, color);
	}
}

// An entry with a value, as in the Options menu: the label on the left of the middle, the value on the right, with "<" and ">" when it can be changed
void LobbyMenu::drawValueRow(Drawer& drawer, int centerX, int y, const std::string& label, const std::string& value, bool selected, bool changeable, bool enabled, float alpha)
{
	Font& font = global::mOxyfontRegular;
	const Color color = withAlpha(selected ? (enabled ? LOBBY_YELLOW : Color(0.8f, 0.8f, 0.4f)) : (enabled ? LOBBY_WHITE : LOBBY_DARK), alpha);
	drawer.printText(font, Recti(centerX - 32, y, 0, 10), label, 6, color);
	const int valueCenter = centerX + 70;
	drawer.printText(font, Recti(valueCenter, y, 0, 10), fitText(font, value, 120), 5, color);
	if (selected && changeable && enabled)
	{
		const int distance = 68 + arrowWobble();
		drawer.printText(font, Recti(valueCenter - distance, y, 0, 10), "<", 5, color);
		drawer.printText(font, Recti(valueCenter + distance, y, 0, 10), ">", 5, color);
	}
	else if (selected)
	{
		const int distance = 12 + arrowWobble();
		drawer.printText(font, Recti(centerX - 32 - font.getWidth(label) - distance, y, 0, 10), ">>", 6, color);
	}
}

// The entries from "first" to "last" (not included) below each other; returns the height below them
int LobbyMenu::drawEntryRows(Drawer& drawer, int centerX, int y, int step, size_t first, size_t last, float alpha)
{
	for (size_t i = first; i < last && i < mMenuEntries.size(); ++i)
	{
		GameMenuEntry& entry = mMenuEntries[i];
		const bool selected = ((int)i == mMenuEntries.mSelectedEntryIndex);
		if (entry.mData == BACK || entry.mData == CANCEL)
			y += 6;		// (a little room before Back, as in the Options menu)
		std::string label, value;
		splitEntryText(entry.mText, label, value);
		if (value.empty())
			drawRow(drawer, global::mOxyfontRegular, centerX, y, entry.mText, selected, isEntryEnabled(entry.mData), alpha);
		else
			drawValueRow(drawer, centerX, y, label, value, selected, isChangeableEntry(entry.mData), isEntryEnabled(entry.mData), alpha);
		y += step;
	}
	return y;
}

// The black bar at the bottom with the buttons, as in the Options menu
void LobbyMenu::renderControlsDisplay(Drawer& drawer, float alpha)
{
	if (!ConfigurationImpl::instance().mShowControlsDisplay)
		return;
	mControlsDisplay.clear();
	const bool inRoom = (mScreen == Screen::HOSTING || mScreen == Screen::JOINED);
	if (mScreen == Screen::JOIN)
	{
		mControlsDisplay.addControl("Press key", false, "@input_icon_button_A");
		mControlsDisplay.addControl("Join", false, "@input_icon_button_start");
	}
	else if (mScreen == Screen::TEXT)
	{
		mControlsDisplay.addControl((mTextMode == TextMode::CHAT) ? "Send" : "Save", false, "@input_icon_button_start");
	}
	else if (mMenuEntries.hasSelected())
	{
		if (isChangeableEntry(mMenuEntries.selected().mData))
			mControlsDisplay.addControl("Change", false, "@input_icon_button_left", "@input_icon_button_right");
		mControlsDisplay.addControl("Select", false, "@input_icon_button_A");
	}
	mControlsDisplay.addControl(inRoom ? "Main menu (stays in the lobby)" : "Back", true, "@input_icon_button_B");
	mControlsDisplay.render(drawer, alpha);
}

void LobbyMenu::renderCodeKeypad(Drawer& drawer, int anchorX, float alpha, int top)
{
	for (int key = 0; key < PAD_KEY_COUNT; ++key)
	{
		const int row = key / 3;
		const int column = key % 3;
		const bool isSelected = (mMenuEntries.mSelectedEntryIndex == key);
		const int value = PAD_LAYOUT[key];
		const std::string label = (value == PAD_DELETE) ? "DEL" : ((value == PAD_JOIN) ? "JOIN" : std::string(1, (char)('0' + value)));
		const Vec2i position(anchorX + (column - 1) * 44, top + row * 15);
		if (isSelected)
			drawer.drawRect(Recti(position.x - 18, position.y - 1, 36, 13), Color(1.0f, 1.0f, 0.0f, 0.18f * alpha));
		const bool dim = (value == PAD_JOIN && mCodeDigits.empty());
		drawer.printText(global::mOxyfontRegular, Recti(position.x, position.y, 0, 10), label, 5, withAlpha(isSelected ? LOBBY_YELLOW : (dim ? LOBBY_GREY : LOBBY_WHITE), alpha));
	}
}

void LobbyMenu::renderBrowserScreen(Drawer& drawer, int anchorX, float alpha)
{
	NetplayManager& manager = netplay();
	const Color white = withAlpha(LOBBY_WHITE, alpha);
	const Color yellow = withAlpha(LOBBY_YELLOW, alpha);
	const Color grey = withAlpha(LOBBY_GREY, alpha);
	const Color dark = withAlpha(LOBBY_DARK, alpha);
	const Color orange = withAlpha(LOBBY_ORANGE, alpha);
	const Color red = withAlpha(LOBBY_RED, alpha);
	const Color green = withAlpha(LOBBY_GREEN, alpha);
	const Color kindColors[4] = { grey, green, orange, red };
	const uint32 selectedData = mMenuEntries.hasSelected() ? mMenuEntries.selected().mData : 0;

	std::string title = "ONLINE";
	switch (mScreen)
	{
		case Screen::LIST:		title = "FIND A LOBBY";  break;
		case Screen::JOIN:		title = mJoinLanOnly ? "JOIN A LAN GAME" : "JOIN WITH CODE";  break;
		case Screen::CREATE:	title = "CREATE LOBBY";  break;
		case Screen::LAN:		title = "LAN GAME";  break;
		case Screen::SETTINGS:	title = "ONLINE SETTINGS";  break;
		case Screen::SEARCHING:	title = (mFlow == Flow::QUICK) ? "QUICK JOIN" : "JOINING";  break;
		case Screen::TEXT:
			switch (mTextMode)
			{
				case TextMode::NAME:			title = "YOUR NAME";  break;
				case TextMode::RELAY:			title = "RELAY SERVER";  break;
				case TextMode::LOBBY_SERVER:	title = "LOBBY SERVER";  break;
				case TextMode::LOBBY_NAME:		title = "LOBBY NAME";  break;
				default:						title = "CHAT";  break;
			}
			break;
		default: break;
	}
	drawHeader(drawer, anchorX, alpha, title, std::string());

	auto statusLine = [&](int y)
	{
		uint32 kind = 0;
		const std::string line = getServerStatusLine(kind);
		drawer.printText(global::mOxyfontSmall, Recti(anchorX, y, 0, 10), fitText(global::mOxyfontSmall, line, 380), 5, kindColors[kind]);
	};
	auto hint = [&](int y, const std::string& text, const Color& color)
	{
		printLines(drawer, global::mOxyfontSmall, anchorX, y, 10, text, 2, 72, color);
	};

	switch (mScreen)
	{
		case Screen::HOME:
		{
			// The big choices first (Retry, Find a lobby, Create lobby, Join with code), then the small ones (LAN game, Settings, Back)
			const bool hasRetry = (mMenuEntries.size() > 0 && mMenuEntries[0].mData == RETRY);
			int y = hasRetry ? 50 : 56;
			bool bigBefore = true;
			for (size_t i = 0; i < mMenuEntries.size(); ++i)
			{
				GameMenuEntry& entry = mMenuEntries[i];
				const bool big = (entry.mData == RETRY || entry.mData == PUBLIC_LOBBIES || entry.mData == CREATE_LOBBY || entry.mData == JOIN_LOBBY);
				if (!big && bigBefore)
					y += hasRetry ? 6 : 10;
				if (entry.mData == BACK)
					y += 4;
				bigBefore = big;
				drawRow(drawer, big ? global::mSonicFontC : global::mOxyfontRegular, anchorX, y, entry.mText, (int)i == mMenuEntries.mSelectedEntryIndex, isEntryEnabled(entry.mData), alpha);
				y += big ? (hasRetry ? 20 : 24) : 14;
			}

			// What the selected choice does
			std::string text;
			switch (selectedData)
			{
				case RETRY:				text = "Try the last lobby again.";  break;
				case PUBLIC_LOBBIES:	text = isEntryEnabled(PUBLIC_LOBBIES) ? "Join a public lobby, or let Quick Join pick one for you." : "No server is set. Add one in SETTINGS first.";  break;
				case CREATE_LOBBY:		text = "Open your own lobby and invite friends with its code.";  break;
				case JOIN_LOBBY:		text = "Type the code a friend gave you.";  break;
				case LAN_GAME:			text = "Play with friends on the same network, no server needed.";  break;
				case SETTINGS:			text = "Your name, and the servers used for online play.";  break;
				case BACK:				text = "Back to the main menu.";  break;
				default: break;
			}
			if (mMessage.empty())
				hint(178, text, grey);		// (a message, below, matters more)
			if (mMessage.empty())
				drawer.printText(global::mOxyfontSmall, Recti(anchorX, 190, 0, 10), "Playing as " + shorten(manager.getLocalPlayerName(), 16), 5, dark);
			break;
		}

		case Screen::LIST:
		{
			const int rows = (int)mShownList.size();
			drawRow(drawer, global::mSonicFontC, anchorX, 46, "QUICK JOIN", mMenuEntries.mSelectedEntryIndex == 0, isEntryEnabled(QUICK_JOIN), alpha);

			// The table of lobbies
			const int tableTop = 66;
			const int tableLeft = anchorX - 176;
			drawPanel(drawer, Recti(anchorX - 186, tableTop, 372, 14 + LIST_ROWS_VISIBLE * 11), alpha);
			drawer.printText(global::mOxyfontSmall, Recti(tableLeft, tableTop + 2, 0, 10), "LOBBY", 4, dark);
			drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 112, tableTop + 2, 0, 10), "HOST", 4, dark);
			drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 178, tableTop + 2, 0, 10), "PLAYERS", 4, dark);
			drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 234, tableTop + 2, 0, 10), "STATUS", 4, dark);
			drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 292, tableTop + 2, 0, 10), "VERSION", 4, dark);
			const int rowsTop = tableTop + 13;

			const LobbyDirectory& directory = manager.getLobbyDirectory();
			if (rows == 0)
			{
				const NetplayManager::LobbyServerState serverState = manager.getLobbyServerState();
				std::string line1, line2;
				Color color = grey;
				if (serverState == NetplayManager::LobbyServerState::NOT_SET)
				{
					line1 = "No lobby server is set, so there are no public lobbies.";
					line2 = NetplayManager::isBrowserBuild() ? "Choose SETTINGS, then RELAY SERVER, to add one." : "Add one in SETTINGS, or use LAN GAME and long codes.";
					color = orange;
				}
				else if (serverState == NetplayManager::LobbyServerState::WAKING)
				{
					line1 = "The server is asleep and is being woken up. Retrying...";
					line2 = "This can take up to a minute. The list shows by itself.";
					color = orange;
				}
				else if (serverState == NetplayManager::LobbyServerState::NO_LIST)
				{
					line1 = "This server cannot list lobbies (it may be an older version).";
					line2 = "You can still join with a long code of 13 digits.";
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
				drawer.printText(global::mOxyfontSmall, Recti(anchorX, rowsTop + 12, 0, 10), line1, 5, color);
				if (!line2.empty())
					drawer.printText(global::mOxyfontSmall, Recti(anchorX, rowsTop + 24, 0, 10), line2, 5, color);
			}

			// (entry 0 is QUICK JOIN, the lobbies are the entries 1 to "rows")
			const int selectedRow = (mMenuEntries.mSelectedEntryIndex >= 1 && mMenuEntries.mSelectedEntryIndex <= rows) ? (mMenuEntries.mSelectedEntryIndex - 1) : -1;
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
				const int y = rowsTop + (i - first) * 11;
				if (isSelected)
					drawer.drawRect(Recti(tableLeft - 6, y - 1, 364, 11), Color(1.0f, 1.0f, 0.0f, 0.15f * alpha));
				drawer.printText(global::mOxyfontSmall, Recti(tableLeft, y, 0, 10), fitText(global::mOxyfontSmall, entry.mName, 106), 4, base);
				drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 112, y, 0, 10), fitText(global::mOxyfontSmall, entry.mHostName, 60), 4, base);
				drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 178, y, 0, 10), std::to_string((int)entry.mPlayers) + "/" + std::to_string((int)entry.mMaxPlayers), 4, base);
				const char* status = entry.isInGame() ? "PLAYING" : (entry.isFull() ? "FULL" : "OPEN");
				drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 234, y, 0, 10), status, 4, (entry.isInGame() || entry.isFull()) ? (isSelected ? yellow : orange) : (isSelected ? yellow : (joinable ? green : dark)));
				drawer.printText(global::mOxyfontSmall, Recti(tableLeft + 292, y, 0, 10), entry.isCompatible() ? "OK" : "OTHER", 4, entry.isCompatible() ? (isSelected ? yellow : green) : (isSelected ? yellow : red));
			}
			if (first > 0)
				drawer.printText(global::mOxyfontSmall, Recti(anchorX + 180, rowsTop, 0, 10), "^", 5, grey);
			if (first + LIST_ROWS_VISIBLE < rows)
				drawer.printText(global::mOxyfontSmall, Recti(anchorX + 180, rowsTop + (LIST_ROWS_VISIBLE - 1) * 11, 0, 10), "v", 5, grey);

			// REFRESH, SHOW and BACK below the table
			drawEntryRows(drawer, anchorX, tableTop + 14 + LIST_ROWS_VISIBLE * 11 + 6, 12, (size_t)rows + 1, mMenuEntries.size(), alpha);
			break;
		}

		case Screen::JOIN:
		{
		#ifndef PLATFORM_ANDROID
			Application::instance().requestActiveTextInput();		// typed digits arrive as text (on a phone the number pad on the screen is used instead of the keyboard)
		#endif
			drawer.printText(global::mOxyfontRegular, Recti(anchorX, 48, 0, 10), mJoinLanOnly ? "Type the long code of the LAN game:" : "Type or paste the number your friend gave you:", 5, grey);
			// The digits typed so far, with a cursor
			std::string shown = groupDigits(mCodeDigits);
			const bool blink = (std::fmod(FTX::getTime(), 1.0f) < 0.6f);
			if ((int)mCodeDigits.size() < MAX_CODE_DIGITS && blink)
				shown += "_";
			drawer.printText(global::mSonicFontB, Recti(anchorX, 62, 0, 10), mCodeDigits.empty() ? std::string(blink ? "_" : " ") : shown, 5, yellow);
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, 80, 0, 10), mJoinLanOnly ? "13 digits. Your friend can copy it from their lobby screen." : (NetplayManager::isBrowserBuild() ? "6 digits: a lobby code.  13 digits: a long code." : "6 digits: a lobby code.  13 digits: a long code (LAN, no server)."), 5, grey);
			const int padTop = 100;
			drawPanel(drawer, Recti(anchorX - 76, padTop - 6, 152, 4 * 15 + 8), alpha);
			renderCodeKeypad(drawer, anchorX, alpha, padTop);
			drawRow(drawer, global::mOxyfontRegular, anchorX, padTop + 4 * 15 + 10, "BACK", mMenuEntries.mSelectedEntryIndex == PAD_KEY_COUNT, true, alpha);
			break;
		}

		case Screen::CREATE:
		{
			drawEntryRows(drawer, anchorX, 56, 16, 0, mMenuEntries.size(), alpha);
			if (!manager.isLobbyServerConfigured())
			{
				hint(150, std::string("Without a lobby server the lobby is not listed: it is private.\n") + (NetplayManager::isBrowserBuild() ? "Online play needs a server: choose SETTINGS, then RELAY SERVER." : "Friends join with the long code (the matchmaking server, or LAN)."), orange);
			}
			else
			{
				std::string text;
				switch (selectedData)
				{
					case EDIT_LOBBY_NAME:	text = "The name that others see in FIND A LOBBY.";  break;
					case TOGGLE_PUBLIC:		text = mLobbyPublic ? "Public: listed in FIND A LOBBY, anybody can join." : "Private: not listed. Friends join with its code.";  break;
					case MAX_PLAYERS:		text = "Up to " + std::to_string(mLobbyMaxPlayers) + " players, you included.";  break;
					case OPEN_LOBBY:		text = "Opens the lobby. Its code is shown at the top.";  break;
					default: break;
				}
				hint(150, text, grey);
			}
			break;
		}

		case Screen::LAN:
		{
			drawer.printText(global::mOxyfontRegular, Recti(anchorX, 50, 0, 10), "Play with friends on the same network.", 5, grey);
			drawer.printText(global::mOxyfontRegular, Recti(anchorX, 61, 0, 10), "No server is needed.", 5, grey);
			drawEntryRows(drawer, anchorX, 86, 16, 0, mMenuEntries.size(), alpha);
			hint(150, "Host a game and tell your friends the long code,\nor type the long code that they give you.", grey);
			break;
		}

		case Screen::SETTINGS:
		{
			drawEntryRows(drawer, anchorX, 56, 16, 0, mMenuEntries.size(), alpha);
			statusLine(136);
			hint(152, "The lobby server lists public lobbies and gives the 6 digit codes.\nThe relay helps players whose direct connection fails.", grey);
			break;
		}

		case Screen::SEARCHING:
		{
			statusLine(52);
			const int dots = (int)(std::fmod(FTX::getTime(), 1.2f) / 0.3f);
			std::string what;
			if (mFlow == Flow::QUICK)
				what = "Looking for the best open lobby";
			else if (!mFlowLobbyName.empty())
				what = "Joining \"" + fitText(global::mOxyfontRegular, mFlowLobbyName, 150) + "\"";
			else
				what = "Looking up the lobby code";
			drawer.printText(global::mOxyfontRegular, Recti(anchorX - 80, 84, 0, 10), what + std::string(dots, '.'), 4, white);
			if (manager.getLobbyServerState() == NetplayManager::LobbyServerState::WAKING)
				hint(102, "The server was asleep and is being woken up.\nThis can take up to a minute. Please wait.", orange);
			drawEntryRows(drawer, anchorX, 134, 16, 0, mMenuEntries.size(), alpha);
			break;
		}

		case Screen::TEXT:
		{
			const char* description = "Your message (40 characters at most):";
			switch (mTextMode)
			{
				case TextMode::NAME:			description = "Your name (12 letters at most):";  break;
				case TextMode::RELAY:			description = "Address of the relay server:";  break;
				case TextMode::LOBBY_SERVER:	description = "Address of the lobby server (empty = none):";  break;
				case TextMode::LOBBY_NAME:		description = "Name of your lobby (20 characters at most):";  break;
				default: break;
			}
			drawer.printText(global::mOxyfontRegular, Recti(anchorX, 52, 0, 10), description, 5, grey);
			int y = 72;
			for (size_t i = 0; i < mMenuEntries.size(); ++i)
			{
				GameMenuEntry& entry = mMenuEntries[i];
				const bool isSelected = ((int)i == mMenuEntries.mSelectedEntryIndex);
				if (entry.is<InputFieldMenuEntry>())
				{
					GameMenuEntry::RenderContext context;
					context.mDrawer = &drawer;
					context.mCurrentPosition = Vec2i(anchorX - 100, y);
					context.mIsSelected = isSelected;
					context.mDeltaSeconds = 0.0f;
					entry.performRenderEntry(context);
					y += 30;
				}
				else
				{
					if (entry.mData == BACK)
						y += 4;
					drawRow(drawer, global::mOxyfontRegular, anchorX, y, entry.mText, isSelected, true, alpha);
					y += 16;
				}
			}
			break;
		}

		default:
			break;
	}

	// Above the bar at the bottom: what the player needs to know right now (a message can have several lines)
	if (!mMessage.empty())
	{
		printLines(drawer, global::mOxyfontSmall, anchorX, 184, 10, mMessage, 3, 72, orange);
		if (mLastDrawnStatus != "message: " + mMessage)
		{
			mLastDrawnStatus = "message: " + mMessage;
			RMX_LOG_INFO("Lobby: on screen: message " << mMessage.substr(0, mMessage.find('\n')));
			{
				std::string flat = mMessage;
				std::replace(flat.begin(), flat.end(), '\n', '|');
				RMX_LOG_INFO("Lobby: whole message: " << flat);		// (the automated checks read this)
			}
		}
	}
	else
	{
		mLastDrawnStatus.clear();
	}
}

// The lobby itself (host or friend) and its options: the code at the top, the buttons on the left, the players and the chat on the right
void LobbyMenu::renderRoom(Drawer& drawer, int anchorX, float alpha)
{
	NetplayManager& manager = netplay();
	NetplayHost* host = manager.getNetplayHost();
	NetplayClient* client = manager.getNetplayClient();
	const Color white = withAlpha(LOBBY_WHITE, alpha);
	const Color yellow = withAlpha(LOBBY_YELLOW, alpha);
	const Color green = withAlpha(LOBBY_GREEN, alpha);
	const Color grey = withAlpha(LOBBY_GREY, alpha);
	const Color dark = withAlpha(LOBBY_DARK, alpha);
	const Color orange = withAlpha(LOBBY_ORANGE, alpha);
	const Color red = withAlpha(LOBBY_RED, alpha);

	// The header: the name of the lobby, and its code (only the short code of a lobby server; the long code only where there is nothing else, LAN or no lobby server)
	{
		const std::string lobbyCode = manager.getPublishedLobbyCode();
		const bool onLobbyServer = manager.isPublishingLobby();
		std::string codeText;
		const char* codeKind = "none";
		if (nullptr != host)
		{
			if (!lobbyCode.empty())
			{
				codeText = "CODE " + NetplayCodes::formatLobbyCode(lobbyCode);
				codeKind = "short";
			}
			else if (!onLobbyServer)
			{
				codeText = NetplayCodes::makeSessionCode(manager.mSessionID);
				codeKind = "long";
			}
		}
		else if (mLastJoinCode.size() == (size_t)NetplayCodes::LOBBY_CODE_DIGITS)
		{
			codeText = "CODE " + NetplayCodes::formatLobbyCode(mLastJoinCode);
			codeKind = "short";
		}
		if (mLoggedHeaderCode != codeKind)
		{
			mLoggedHeaderCode = codeKind;
			RMX_LOG_INFO("Lobby: header code " << codeKind);		// (the automated checks read this: a public lobby never shows its long code)
		}

		std::string title;
		if (mScreen == Screen::OPTIONS)
			title = "LOBBY OPTIONS";
		else if (nullptr != host)
			title = mLastHostLanOnly ? std::string("LAN GAME") : fitText(global::mSonicFontC, mLobbyName, 220, false);
		else
			title = mFlowLobbyName.empty() ? std::string("LOBBY") : fitText(global::mSonicFontC, mFlowLobbyName, 220, false);
		drawHeader(drawer, anchorX, alpha, title, (mScreen == Screen::OPTIONS) ? std::string() : codeText);
	}

	// One line under the header: the state of the lobby
	{
		std::string line;
		Color color = grey;
		if (nullptr != host)
		{
			const std::string lobbyCode = manager.getPublishedLobbyCode();
			const bool onLobbyServer = manager.isPublishingLobby();
			if (mShowCopied)
			{
				line = "The code was copied.";
				color = green;
			}
			else if (host->getHostState() == NetplayHost::HostState::CONNECT_TO_SERVER)
			{
				line = "Contacting the game server...";
				color = orange;
			}
			else if (onLobbyServer && lobbyCode.empty())
			{
				const NetplayManager::LobbyServerState serverState = manager.getLobbyServerState();
				color = orange;
				if (serverState == NetplayManager::LobbyServerState::WAKING)
					line = "Waking the server up, retrying (" + std::to_string((int)manager.getLobbyDirectory().getSecondsNotReady()) + " s). Please wait...";
				else if (serverState == NetplayManager::LobbyServerState::NO_LIST)
					line = "This server has no lobby list. Friends join with the long code.";
				else if (serverState == NetplayManager::LobbyServerState::UNREACHABLE || serverState == NetplayManager::LobbyServerState::BAD_ADDRESS)
				{
					line = manager.usesWebSocketServer() ? "The server does not answer. Still trying in the background." : "The lobby server does not answer. Use the long code, or LAN.";
					color = red;
				}
				else
					line = "Asking the lobby server for a lobby code...";
			}
			else if (manager.usesWebSocketServer() && !manager.isRelayHostRegistered() && lobbyCode.empty())
			{
				line = (mRelayWaitSeconds > 10.0f) ? "Cannot reach the relay server. Friends cannot join yet." : "Connecting to the relay server...";
				color = (mRelayWaitSeconds > 10.0f) ? red : orange;
			}
			else if (!lobbyCode.empty())
			{
				line = mLobbyPublic ? "Public lobby: listed in FIND A LOBBY, or join with the code." : "Private lobby: friends join with the code.";
			}
			else
			{
				line = "Friends join with the long code above.";
			}
		}
		else if (nullptr != client)
		{
			const bool inLobby = client->hasLobbyState();
			line = inLobby ? (client->isLobbyHostStarting() ? "The host is picking a game..." : "Press READY when you are set.") : "Connecting to the lobby...";
			color = inLobby ? white : orange;
			if (client->isRejoiningLobby())
			{
				line = "Connection lost. Coming back to your slot...";
				color = orange;
			}
			if (client->isRelayConnecting())
			{
				line = "Direct connection failed. Connecting through the relay...";
				color = orange;
			}
			else if (inLobby && client->isUsingRelay() && manager.getDirectPeerCount() == 0 && !client->isLobbyHostStarting())
			{
				line = "Playing through a relay server: expect more lag.";
				color = orange;
			}
		}
		drawer.printText(global::mOxyfontSmall, Recti(anchorX, 45, 0, 10), line, 5, color);
	}

	if (mScreen == Screen::OPTIONS)
	{
		const int rowsHeight = (int)mMenuEntries.size() * 16 + 6 + ((nullptr == host) ? 18 : 0);
		drawPanel(drawer, Recti(anchorX - 150, 62, 300, rowsHeight + 10), alpha);
		int y = 68;
		if (nullptr == host)
		{
			const int delaySetting = (nullptr != client) ? client->getLobbyDelaySetting() : -1;
			const int delayEffective = (nullptr != client) ? client->getLobbyEffectiveDelay() : 0;
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, y, 0, 10), "Input delay (the host sets it): " + NetplayLobby::describeInputDelay(delaySetting, delayEffective), 5, grey);
			y += 18;
		}
		drawEntryRows(drawer, anchorX, y, 16, 0, mMenuEntries.size(), alpha);
	}
	else
	{
		// The buttons, on the left
		drawPanel(drawer, Recti(anchorX - 192, 62, 122, 124), alpha);
		{
			const int centerX = anchorX - 131;
			int y = 68;
			for (size_t i = 0; i < mMenuEntries.size(); ++i)
			{
				GameMenuEntry& entry = mMenuEntries[i];
				const bool isSelected = ((int)i == mMenuEntries.mSelectedEntryIndex);
				std::string label, value;
				splitEntryText(entry.mText, label, value);
				if (entry.mData == LEAVE)
					y += 4;
				drawRow(drawer, global::mOxyfontRegular, centerX, y, label, isSelected, isEntryEnabled(entry.mData), alpha);
				y += 13;
				if (!value.empty())
				{
					// The value under the button: the character, the chat message
					const bool canChange = isSelected && !(entry.mData == CHARACTER && areCharactersLocked());
					const std::string shown = canChange ? ("< " + fitText(global::mOxyfontSmall, value, 96) + " >") : fitText(global::mOxyfontSmall, value, 116);
					drawer.printText(global::mOxyfontSmall, Recti(centerX, y, 0, 10), shown, 5, isSelected ? yellow : grey);
					y += 11;
				}
				y += 3;
			}
		}

		// The players and the chat, on the right
		const int panelX = anchorX - 62;
		drawPanel(drawer, Recti(panelX, 62, 254, 124), alpha);
		{
			uint8 states[4];
			bool connected = false;
			getSlotStates(states, connected);
			const int mySlot = getMySlot();
			for (int slot = 0; slot < 4; ++slot)
			{
				const int y = 66 + slot * 20;
				const int px = panelX + 6;
				if (states[slot] == 0)
				{
					drawer.printText(global::mOxyfontRegular, Recti(px + 22, y + 3, 0, 10), connected ? "Open" : "...", 4, dark);
					continue;
				}
				const uint16 character = getCharacterOfSlot(slot);
				const uint64 spriteKey = getCharacterSpriteKey(character);
				if (spriteKey != 0)
					drawer.drawSprite(Vec2i(px + 8, y + 8), spriteKey, Color(1.0f, 1.0f, 1.0f, alpha), Vec2f(0.4f, 0.4f));
				std::string name = getPlayerNameOfSlot(slot);
				if (name.empty())
					name = "Player " + std::to_string(slot + 1);
				drawer.printText(global::mOxyfontRegular, Recti(px + 22, y, 0, 10), fitText(global::mOxyfontRegular, name, 112), 4, (slot == mySlot) ? yellow : white);
				if (character != NetplayLobby::CHARACTER_NONE)
					drawer.printText(global::mOxyfontTiny, Recti(px + 22, y + 10, 0, 10), NetplayLobby::getCharacterLabel(character), 4, grey);

				if (slot == 0)
				{
					drawer.printText(global::mOxyfontSmall, Recti(px + 240, y, 0, 10), "HOST", 6, white);
				}
				else
				{
					// Ping (round trip, as measured by the host)
					const int ping = getPingOfSlot(slot);
					if (states[slot] >= 2)
					{
						const Color pingColor = (ping < 0) ? grey : (NetplayLobby::isHighPing(ping) ? red : ((ping > 80) ? yellow : green));
						drawer.printText(global::mOxyfontSmall, Recti(px + 176, y, 0, 10), (ping >= 0) ? (std::to_string(ping) + " ms") : std::string("... ms"), 6, pingColor);
					}
					const bool ready = (states[slot] == 3);
					drawer.printText(global::mOxyfontSmall, Recti(px + 240, y, 0, 10), ready ? "READY" : "NOT READY", 6, ready ? green : orange);
				}
			}

			// The chat, below the players
			drawer.drawRect(Recti(panelX + 4, 147, 246, 1), Color(1.0f, 1.0f, 1.0f, 0.15f * alpha));
			std::vector<std::pair<int, std::string>> lines;
			if (nullptr != host)
			{
				for (const NetplayHost::ChatLine& line : host->getChatLog())
					lines.emplace_back(line.mSlot, line.mText);
			}
			else if (nullptr != client)
			{
				for (const NetplayClient::ChatLine& line : client->getChatLog())
					lines.emplace_back(line.mSlot, line.mText);
			}
			if (lines.empty())
				drawer.printText(global::mOxyfontSmall, Recti(panelX + 8, 151, 0, 10), "No messages yet. Use CHAT to say hello.", 4, dark);
			const size_t first = (lines.size() > 4) ? (lines.size() - 4) : 0;
			int chatY = 151;
			for (size_t i = first; i < lines.size(); ++i)
			{
				const std::string who = shorten(getPlayerNameOfSlot(lines[i].first).empty() ? ("Player " + std::to_string(lines[i].first + 1)) : getPlayerNameOfSlot(lines[i].first), 10);
				drawer.printText(global::mOxyfontSmall, Recti(panelX + 8, chatY, 0, 10), fitText(global::mOxyfontSmall, who + ": " + lines[i].second, 238), 4, white);
				chatY += 9;
			}
		}
	}

	// Above the bar at the bottom: what the player needs to know right now
	const int bottomY = 192;
	std::string drawnStatus = "nothing";
	if (!mMessage.empty())
	{
		drawnStatus = "message";
		printLines(drawer, global::mOxyfontSmall, anchorX, bottomY - 4, 10, mMessage, 2, 72, orange);
	}
	else if (mCharacterNoticeSeconds > 0.0f)
	{
		drawnStatus = "character notice";
		drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY, 0, 10), shorten(mCharacterNoticeText, 70), 5, orange);
	}
	else if (nullptr != client && client->getLobbyReadyCheckSeconds() > 0 && !client->isLobbyReady())
	{
		drawnStatus = "friend ready prompt";
		// The host wants to start: whoever is not ready is asked to be
		const bool blink = (std::fmod(FTX::getTime(), 0.8f) < 0.5f);
		drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY, 0, 10), "THE HOST WANTS TO START! PRESS READY (" + std::to_string(client->getLobbyReadyCheckSeconds()) + ")", 5, blink ? yellow : orange);
	}
	else if (nullptr != host)
	{
		int highPing = 0;
		const int highSlot = host->getHighPingSlot(highPing);
		const int relayedSlot = host->getRelayedSlot();
		if (relayedSlot > 0 && !host->isReadyCheckActive())
		{
			drawnStatus = "relay warning";
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY - 4, 0, 10), shorten(host->getPlayerName(relayedSlot), 12) + " plays through the relay server: more delay for them.", 5, red);
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY + 6, 0, 10), "Input delay (in OPTIONS) evens out the lag.", 5, grey);
		}
		else if (host->isReadyCheckActive())
		{
			drawnStatus = "ready check countdown";
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY - 4, 0, 10), "Waiting for players to get ready... " + std::to_string(host->getReadyCheckSecondsLeft()) + " s", 5, yellow);
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY + 6, 0, 10), "Press START GAME again to start without them.", 5, grey);
		}
		else if (highSlot > 0)
		{
			drawnStatus = "high ping warning";
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY - 4, 0, 10), shorten(host->getPlayerName(highSlot), 12) + " has a high ping (" + std::to_string(highPing) + " ms): the game may feel slow.", 5, red);
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY + 6, 0, 10), "Input delay (in OPTIONS) evens out the lag.", 5, grey);
		}
		else
		{
			const int count = host->getConnectedClientCount();
			const char* hint = (count == 0) ? "Waiting for friends to join..." : (host->areAllClientsReady() ? "Everybody is ready!" : "Waiting for everybody to be ready...");
			drawnStatus = (count == 0) ? "waiting for friends hint" : (host->areAllClientsReady() ? "everybody ready hint" : "waiting for ready hint");
			drawer.printText(global::mOxyfontSmall, Recti(anchorX, bottomY, 0, 10), hint, 5, host->areAllClientsReady() && count > 0 ? green : grey);
		}
	}

	// Written to the log whenever the bottom line changes: the checks read it to know what the lobby screen really showed (not only what the logic decided to show)
	if (drawnStatus != mLastDrawnStatus)
	{
		mLastDrawnStatus = drawnStatus;
		RMX_LOG_INFO("Lobby: on screen: " << drawnStatus);
	}
}

void LobbyMenu::render()
{
	GuiBase::render();

	Drawer& drawer = EngineMain::instance().getDrawer();

	int anchorX = 200;
	float alpha = 1.0f;
	if (mState != State::SHOW)
	{
		anchorX += roundToInt((1.0f - mVisibility) * 300.0f);
		alpha = mVisibility;
	}

	if (isRoomScreen())
		renderRoom(drawer, anchorX, alpha);
	else
		renderBrowserScreen(drawer, anchorX, alpha);
	renderControlsDisplay(drawer, alpha);
}
