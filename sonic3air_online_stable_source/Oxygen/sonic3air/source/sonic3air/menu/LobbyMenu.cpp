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
#include "sonic3air/menu/SharedResources.h"
#include "sonic3air/menu/entries/GeneralMenuEntries.h"
#include "sonic3air/audio/AudioOut.h"

#include "oxygen/application/EngineMain.h"
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
		TEXT_OK
	};

	static const int WRITE_MESSAGE_INDEX = NetplayLobby::NUM_QUICK_MESSAGES + 1;

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

	// Back in the lobby: a host who comes back from the main menu is not picking a game any more
	if (nullptr != netplay().getNetplayHost())
		netplay().getNetplayHost()->setLobbyHostStarting(false);

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
				createLobby();
			else if (strcmp(mode, "hostlan") == 0)
				createLobby(false);
			else if (strcmp(mode, "join") == 0 && nullptr != TestHooks::getEnv("S3AIR_LOBBY_CODE"))
				joinWithCode(TestHooks::getEnv("S3AIR_LOBBY_CODE"));
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
	if ((mScreen == Screen::JOIN || mScreen == Screen::TEXT) && mMenuEntries.hasSelected())
		mMenuEntries.selected().keyboard(ev);
}

void LobbyMenu::textinput(const rmx::TextInputEvent& ev)
{
	if ((mScreen == Screen::JOIN || mScreen == Screen::TEXT) && mMenuEntries.hasSelected())
		mMenuEntries.selected().textinput(ev);
}

void LobbyMenu::chooseScreen()
{
	// Whatever is going on in the network decides what is shown
	if (nullptr != netplay().getNetplayHost())
		mScreen = Screen::HOSTING;
	else if (nullptr != netplay().getNetplayClient())
		mScreen = Screen::JOINED;
	else
		mScreen = Screen::START;
	rebuildEntries();
}

void LobbyMenu::rebuildEntries()
{
	mMenuEntries.clear();
	switch (mScreen)
	{
		case Screen::START:
			if (mFailure != NetplayFailure::Reason::NONE && NetplayFailure::isRetryable(mFailure) && (mLastWasHosting || !mLastJoinCode.empty()))
				mMenuEntries.addEntry("RETRY", RETRY);
			mMenuEntries.addEntry("CREATE LOBBY", CREATE_LOBBY);
			mMenuEntries.addEntry("JOIN WITH CODE", JOIN_LOBBY);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::JOIN:
			mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), L"", L"Type or paste the code...");
			mMenuEntries.addEntry("JOIN", JOIN_NOW);
			mMenuEntries.addEntry("BACK", BACK);
			break;

		case Screen::HOSTING:
			mMenuEntries.addEntry("START GAME", START_GAME);
			mMenuEntries.addEntry("INPUT DELAY", INPUT_DELAY);
			mMenuEntries.addEntry("QUICK CHAT", CHAT_QUICK);
			mMenuEntries.addEntry("KICK", KICK);
			mMenuEntries.addEntry("NAME", EDIT_NAME);
			mMenuEntries.addEntry("COPY CODE", COPY_CODE);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::JOINED:
			mMenuEntries.addEntry("READY", READY);
			mMenuEntries.addEntry("QUICK CHAT", CHAT_QUICK);
			mMenuEntries.addEntry("NAME", EDIT_NAME);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::TEXT:
			if (mTextMode == TextMode::NAME)
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), widen(netplay().getLocalPlayerName()), L"Type your name...");
			else
				mMenuEntries.addEntry<InputFieldMenuEntry>().initEntry(Vec2i(200, 15), L"", L"Type a short message...");
			mMenuEntries.addEntry((mTextMode == TextMode::NAME) ? "SAVE NAME" : "SEND", TEXT_OK);
			mMenuEntries.addEntry("BACK", BACK);
			break;
	}
	mMenuEntries.mSelectedEntryIndex = 0;
	for (size_t i = 0; i < mMenuEntries.size(); ++i)
		mMenuEntries[i].mAnimation.mHighlight = 0.0f;
	updateEntryTexts();
}

void LobbyMenu::openTextScreen(TextMode mode)
{
	mTextMode = mode;
	mScreen = Screen::TEXT;
	rebuildEntries();
}

void LobbyMenu::createLobby(bool viaServer)
{
	NetplayManager& manager = netplay();
	manager.clearFailure();
	mMessage.clear();
	mFailure = NetplayFailure::Reason::NONE;
	mLastWasHosting = true;
	mLastHostViaServer = viaServer;

	// A fresh session code, which the friends use to find this lobby through the game server
	std::random_device randomDevice;
	uint64 sessionID = (((uint64)randomDevice() << 32) | randomDevice()) & 0xffffffffffull;
	if (sessionID < 0x10000000ull)
		sessionID += 0x10000000ull;
	if (const char* fixedCode = TestHooks::getEnv("S3AIR_LOBBY_CODE"))
	{
		uint64 fixedID = 0;
		if (NetplayCodes::readSessionCode(fixedCode, fixedID))
			sessionID = fixedID;
	}
	manager.mSessionID = sessionID;

	if (manager.setupAsHost(viaServer, NetplayManager::DEFAULT_HOST_PORT))
	{
		mScreen = Screen::HOSTING;
		rebuildEntries();
	}
	else
	{
		mMessage = manager.mLastErrorMessage.empty() ? std::string("Could not open the lobby.\nThe network port (UDP 28840) may be in use by another copy of the game.") : manager.mLastErrorMessage;
		mFailure = NetplayFailure::Reason::BLOCKED_OR_TIMEOUT;
		rebuildEntries();
	}
}

void LobbyMenu::joinLobby()
{
	mScreen = Screen::JOIN;
	rebuildEntries();
	mMessage.clear();
}

bool LobbyMenu::joinWithCode(const std::string& code)
{
	NetplayManager& manager = netplay();
	manager.clearFailure();
	mMessage.clear();
	mFailure = NetplayFailure::Reason::NONE;
	mLastWasHosting = false;
	mLastJoinCode = code;
	uint64 sessionID = 0;
	std::string ip;
	uint16 port = NetplayManager::DEFAULT_HOST_PORT;
	uint32 secret = 0;
	if (NetplayCodes::readSessionCode(code.c_str(), sessionID))
	{
		manager.mSessionID = sessionID;
		manager.startJoinViaServer();
	}
	else if (NetplayCodes::readJoinCode(code.c_str(), ip, port, secret))
	{
		manager.startJoinDirect(ip, port, secret);
	}
	else if (!code.empty())
	{
		// Typing mistakes are noticed by the code's check character, before anything is sent
		manager.reportFailure(NetplayFailure::Reason::WRONG_CODE, "This does not look like a lobby code. Check for typing mistakes.");
		mMessage = manager.mLastErrorMessage;
		mFailure = manager.mLastFailure;
		mScreen = Screen::START;
		rebuildEntries();
		return false;
	}
	else
	{
		mMessage = "Please type the lobby code first.";
		return false;
	}
	mScreen = Screen::JOINED;
	rebuildEntries();
	return true;
}

void LobbyMenu::showFailureAndReturnToStart()
{
	NetplayManager& manager = netplay();
	mMessage = manager.mLastErrorMessage.empty() ? "Could not connect to the lobby." : manager.mLastErrorMessage;
	mFailure = (manager.mLastFailure != NetplayFailure::Reason::NONE) ? manager.mLastFailure : NetplayFailure::Reason::BLOCKED_OR_TIMEOUT;
	manager.closeConnections();
	mScreen = Screen::START;
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
		createLobby(viaServer);
	}
	else if (joinWithCode(code))
	{
		mScreen = Screen::JOINED;
		rebuildEntries();
	}
}

void LobbyMenu::leaveLobby()
{
	netplay().closeConnections();
	netplay().clearFailure();
	mFailure = NetplayFailure::Reason::NONE;
	mMessage.clear();
	mScreen = Screen::START;
	rebuildEntries();
}

void LobbyMenu::startGame(bool force)
{
	// The host picks the game as usual; everybody in the lobby follows as soon as it starts
	NetplayHost* host = netplay().getNetplayHost();
	if (nullptr == host || (!force && !host->areAllClientsReady()))
		return;
	host->setLobbyHostStarting(true);
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
	}
	else if (!host->isReadyCheckActive())
	{
		// Not everybody is ready: the ones who are not get a prompt and a time limit, then the game starts without them
		playMenuSound(0x63);
		host->beginReadyCheck();
		mMessage = "Asked everybody to get ready.\nPress START GAME again to start now without waiting.";
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
	mMenuBackground->openMainMenu();
	mState = State::FADE_TO_MENU;
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
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(EDIT_NAME))
		entry->mText = "NAME: " + manager.getLocalPlayerName();
	if (GameMenuEntry* entry = mMenuEntries.getEntryByData(READY))
		entry->mText = (nullptr != client && client->isLobbyReady()) ? "NOT READY" : "READY";
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
		else if ((mScreen == Screen::START || mScreen == Screen::JOIN) && (hosting || joined))
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
	if (mState == State::SHOW && mScreen == Screen::START && mFailure != NetplayFailure::Reason::NONE && nullptr != TestHooks::getEnv("S3AIR_LOBBY_AUTORETRY"))
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

	// A ready check that ended: the host goes on to pick the game, without the players who were left out
	if (mState == State::SHOW && mScreen == Screen::HOSTING && nullptr != manager.getNetplayHost() && manager.getNetplayHost()->takeReadyCheckFinished())
		startGame(true);

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

	updateEntryTexts();

	if (mState == State::SHOW)
	{
		const InputManager::ControllerScheme& keys = InputManager::instance().getController(0);

		const GameMenuEntries::UpdateResult result = mMenuEntries.update();
		if (result != GameMenuEntries::UpdateResult::NONE)
			playMenuSound(0x5b);

		const bool accept = (keys.Start.justPressed() || keys.A.justPressed() || keys.X.justPressed());
		const bool back = (keys.Back.justPressed() || keys.B.justPressed());
		const bool typing = ((mScreen == Screen::JOIN || mScreen == Screen::TEXT) && mMenuEntries.hasSelected() && mMenuEntries.selected().is<InputFieldMenuEntry>());
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
			}
		}

		// While typing, only Start sends it (letters must not trigger menu buttons)
		if ((accept && !typing && mMenuEntries.hasSelected()) || (typing && keys.Start.justPressed()))
		{
			uint32 action = mMenuEntries.selected().mData;
			if (typing)
				action = (mScreen == Screen::TEXT) ? (uint32)TEXT_OK : (uint32)JOIN_NOW;

			switch (action)
			{
				case CREATE_LOBBY:
					playMenuSound(0x63);
					createLobby();
					break;

				case JOIN_LOBBY:
					playMenuSound(0x63);
					joinLobby();
					break;

				case RETRY:
					playMenuSound(0x63);
					retry();
					break;

				case JOIN_NOW:
				{
					// The code field is the first entry
					std::string code;
					if (mMenuEntries.size() > 0 && mMenuEntries[0].is<InputFieldMenuEntry>())
						code = mMenuEntries[0].as<InputFieldMenuEntry>().getTextUTF8();

					if (!joinWithCode(code))
						break;
					playMenuSound(0x63);
					mScreen = Screen::JOINED;
					rebuildEntries();
					break;
				}

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
						openTextScreen(TextMode::CHAT);
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

				case EDIT_NAME:
					playMenuSound(0x63);
					openTextScreen(TextMode::NAME);
					break;

				case TEXT_OK:
				{
					std::string text;
					if (mMenuEntries.size() > 0 && mMenuEntries[0].is<InputFieldMenuEntry>())
						text = mMenuEntries[0].as<InputFieldMenuEntry>().getTextUTF8();
					playMenuSound(0x63);
					if (mTextMode == TextMode::NAME)
					{
						manager.setLocalPlayerName(text);
					}
					else if (nullptr != manager.getNetplayHost())
					{
						manager.getNetplayHost()->sendHostChat(0, text);
					}
					else if (nullptr != manager.getNetplayClient())
					{
						manager.getNetplayClient()->sendChat(0, text);
					}
					chooseScreen();
					break;
				}

				case COPY_CODE:
					if (PlatformFunctions::hasClipboardSupport())
					{
						PlatformFunctions::copyToClipboard(NetplayCodes::makeSessionCode(manager.mSessionID));
						mShowCopied = true;
					}
					break;

				case LEAVE:
					playMenuSound(0xad);
					leaveLobby();
					break;

				case BACK:
					if (mScreen == Screen::TEXT)
						chooseScreen();
					else
						goBack();
					break;
			}
		}
		else if (back && !typing)
		{
			if (mScreen == Screen::JOIN)
			{
				mScreen = Screen::START;
				rebuildEntries();
			}
			else if (mScreen == Screen::TEXT)
			{
				chooseScreen();
			}
			else
			{
				// In a lobby, Back goes to the main menu and keeps the lobby (use "Leave Lobby" to leave it)
				goBack();
			}
		}
		else if (back && typing && mMenuEntries.size() > 1)
		{
			// Back in the text field: go to the buttons below it
			mMenuEntries.mSelectedEntryIndex = (int)mMenuEntries.size() - 1;
		}
	}

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

	int py = inLobbyScreen ? 4 : 18;
	drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), "ONLINE LOBBY", 5, white);
	py += inLobbyScreen ? 18 : 22;

	// Code of the lobby
	if (mScreen == Screen::HOSTING)
	{
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Your friends join with this code:", 5, grey);
		py += 10;
		drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), NetplayCodes::makeSessionCode(manager.mSessionID), 5, yellow);
		py += 16;
		if (mShowCopied)
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Copied!", 5, green);
		}
		else if (nullptr != host && host->getHostState() == NetplayHost::HostState::CONNECT_TO_SERVER)
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, py), "Contacting the game server...", 5, orange);
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
	else if (mScreen == Screen::JOIN)
	{
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Enter the code your friend gave you:", 5, grey);
		py += 18;
	}
	else if (mScreen == Screen::TEXT)
	{
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), (mTextMode == TextMode::NAME) ? "Your name (12 letters at most):" : "Your message (40 characters at most):", 5, grey);
		py += 18;
	}
	else
	{
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Play with up to three friends. Create a lobby", 5, grey);
		py += 12;
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "and share the code, or join a lobby with a code.", 5, grey);
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

			// Ping (round trip, as measured by the host)
			if (slot != 0 && states[slot] >= 2)
			{
				const int ping = getPingOfSlot(slot);
				if (ping >= 0)
				{
					const Color pingColor = NetplayLobby::isHighPing(ping) ? red : ((ping > 80) ? yellow : green);
					drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 40, py), std::to_string(ping) + " ms", 6, pingColor);
				}
				else
				{
					drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 40, py), "... ms", 6, grey);
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
	if (!mMessage.empty())
	{
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
	else if (mScreen == Screen::JOINED && nullptr != client && client->getLobbyReadyCheckSeconds() > 0 && !client->isLobbyReady())
	{
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
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "WARNING: " + shorten(host->getPlayerName(relayedSlot), 12) + " plays through the relay server.", 4, red);
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY + 10), "That adds delay for them. Input delay evens out the lag.", 4, grey);
		}
		else if (host->isReadyCheckActive())
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "Waiting for players to get ready... " + std::to_string(host->getReadyCheckSecondsLeft()) + " s", 4, yellow);
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY + 10), "Press START GAME again to start without them.", 4, grey);
		}
		else if (highSlot > 0)
		{
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), "WARNING: " + shorten(host->getPlayerName(highSlot), 12) + " has a high ping (" + std::to_string(highPing) + " ms).", 4, red);
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY + 10), "The game may feel slow for them. Input delay evens out the lag.", 4, grey);
		}
		else
		{
			const int count = host->getConnectedClientCount();
			const char* hint = (count == 0) ? "Waiting for friends to join..." : (host->areAllClientsReady() ? "Everybody is ready!" : "Waiting for everybody to be ready...");
			drawer.printText(global::mOxyfontSmall, Vec2i(leftX, bottomY), hint, 4, grey);
		}
	}
}
