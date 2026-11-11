/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
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
		COPY_CODE,
		LEAVE,
		BACK
	};

	NetplayManager& netplay()
	{
		return NetplayManager::instance();
	}
}


LobbyMenu::LobbyMenu(MenuBackground& menuBackground) :
	mMenuBackground(&menuBackground)
{
	mMenuEntries.reserve(8);
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
		const char* mode = getenv("S3AIR_LOBBY_OPEN");
		if (nullptr != mode)
		{
			testHookDone = true;
			if (strcmp(mode, "host") == 0)
				createLobby();
			else if (strcmp(mode, "hostlan") == 0)
				createLobby(false);
			else if (strcmp(mode, "join") == 0 && nullptr != getenv("S3AIR_LOBBY_CODE"))
				joinWithCode(getenv("S3AIR_LOBBY_CODE"));
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
	if (mScreen == Screen::JOIN && mMenuEntries.hasSelected())
		mMenuEntries.selected().keyboard(ev);
}

void LobbyMenu::textinput(const rmx::TextInputEvent& ev)
{
	if (mScreen == Screen::JOIN && mMenuEntries.hasSelected())
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
			mMenuEntries.addEntry("COPY CODE", COPY_CODE);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;

		case Screen::JOINED:
			mMenuEntries.addEntry("READY", READY);
			mMenuEntries.addEntry("LEAVE LOBBY", LEAVE);
			break;
	}
	mMenuEntries.mSelectedEntryIndex = 0;
	for (size_t i = 0; i < mMenuEntries.size(); ++i)
		mMenuEntries[i].mAnimation.mHighlight = 0.0f;
}

void LobbyMenu::createLobby(bool viaServer)
{
	NetplayManager& manager = netplay();
	manager.mLastErrorMessage.clear();
	mMessage.clear();

	// A fresh session code, which the friends use to find this lobby through the game server
	std::random_device randomDevice;
	uint64 sessionID = (((uint64)randomDevice() << 32) | randomDevice()) & 0xffffffffffull;
	if (sessionID < 0x10000000ull)
		sessionID += 0x10000000ull;
	if (const char* fixedCode = getenv("S3AIR_LOBBY_CODE"))
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
		mMessage = manager.mLastErrorMessage.empty() ? "Could not create the lobby." : manager.mLastErrorMessage;
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
	manager.mLastErrorMessage.clear();
	mMessage.clear();
	uint64 sessionID = 0;
	std::string ip;
	uint16 port = NetplayManager::DEFAULT_HOST_PORT;
	if (NetplayCodes::readSessionCode(code.c_str(), sessionID))
	{
		manager.mSessionID = sessionID;
		manager.startJoinViaServer();
	}
	else if (NetplayCodes::readJoinCode(code.c_str(), ip, port))
	{
		manager.startJoinDirect(ip, port);
	}
	else if (!code.empty())
	{
		manager.startJoinDirect(code, NetplayManager::DEFAULT_HOST_PORT);	// An address typed in directly works as well
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

void LobbyMenu::leaveLobby()
{
	netplay().closeConnections();
	mMessage.clear();
	mScreen = Screen::START;
	rebuildEntries();
}

void LobbyMenu::startGame()
{
	// The host picks the game as usual; everybody in the lobby follows as soon as it starts
	NetplayHost* host = netplay().getNetplayHost();
	if (nullptr == host || !host->areAllClientsReady())
		return;
	host->setLobbyHostStarting(true);
	goBack();
}

void LobbyMenu::goBack()
{
	playMenuSound(0xad);
	mMenuBackground->openMainMenu();
	mState = State::FADE_TO_MENU;
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

void LobbyMenu::update(float timeElapsed)
{
	GameMenuBase::update(timeElapsed);

	NetplayManager& manager = netplay();

	// The network may have changed behind our back (connection lost, joined through F9, ...)
	if (mState == State::SHOW)
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
				mMessage = manager.mLastErrorMessage;
			chooseScreen();
		}
		else if ((mScreen == Screen::START || mScreen == Screen::JOIN) && (hosting || joined))
		{
			chooseScreen();
		}
	}

	// A friend whose connection failed
	if (mState == State::SHOW && mScreen == Screen::JOINED && nullptr != manager.getNetplayClient() && manager.getNetplayClient()->getState() == NetplayClient::State::FAILED)
	{
		mMessage = manager.mLastErrorMessage.empty() ? "Could not connect to the lobby." : manager.mLastErrorMessage;
		manager.closeConnections();
		mScreen = Screen::START;
		rebuildEntries();
	}

	// The label of the Ready button shows what pressing it does
	if (mScreen == Screen::JOINED && nullptr != manager.getNetplayClient() && mMenuEntries.size() > 0)
		mMenuEntries[0].mText = manager.getNetplayClient()->isLobbyReady() ? "NOT READY" : "READY";

	if (mState == State::SHOW)
	{
		const InputManager::ControllerScheme& keys = InputManager::instance().getController(0);

		const GameMenuEntries::UpdateResult result = mMenuEntries.update();
		if (result != GameMenuEntries::UpdateResult::NONE)
			playMenuSound(0x5b);

		const bool accept = (keys.Start.justPressed() || keys.A.justPressed() || keys.X.justPressed());
		const bool back = (keys.Back.justPressed() || keys.B.justPressed());
		const bool typing = (mScreen == Screen::JOIN && mMenuEntries.hasSelected() && mMenuEntries.selected().is<InputFieldMenuEntry>());

		// While typing a code, only Start sends it (letters must not trigger menu buttons)
		if ((accept && !typing && mMenuEntries.hasSelected()) || (typing && keys.Start.justPressed()))
		{
			uint32 action = mMenuEntries.selected().mData;
			if (typing)
				action = JOIN_NOW;

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
				{
					NetplayHost* host = manager.getNetplayHost();
					if (nullptr != host && host->areAllClientsReady())
					{
						playMenuSound(0x63);
						startGame();
					}
					else
					{
						playMenuSound(0xad);
						mMessage = (nullptr != host && host->getConnectedClientCount() == 0) ? "Nobody has joined yet." : "Waiting for everybody to be ready.";
					}
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
			else
			{
				// In a lobby, Back goes to the main menu and keeps the lobby (use "Leave Lobby" to leave it)
				goBack();
			}
		}
		else if (back && typing && mMenuEntries.size() > 1)
		{
			// Back in the code field: go to the buttons below it
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

	const Color white(1.0f, 1.0f, 1.0f, alpha);
	const Color yellow(1.0f, 1.0f, 0.0f, alpha);
	const Color green(0.4f, 1.0f, 0.4f, alpha);
	const Color grey(0.7f, 0.7f, 0.7f, alpha);
	const Color orange(1.0f, 0.75f, 0.5f, alpha);

	int py = 18;
	drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), "ONLINE LOBBY", 5, white);
	py += 22;

	NetplayHost* host = manager.getNetplayHost();
	NetplayClient* client = manager.getNetplayClient();

	// Code of the lobby
	if (mScreen == Screen::HOSTING)
	{
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Your friends join with this code:", 5, grey);
		py += 14;
		drawer.printText(global::mSonicFontB, Vec2i(anchorX, py), NetplayCodes::makeSessionCode(manager.mSessionID), 5, yellow);
		py += 16;
		if (mShowCopied)
		{
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Copied!", 5, green);
		}
		else if (nullptr != host && host->getHostState() == NetplayHost::HostState::CONNECT_TO_SERVER)
		{
			drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Contacting the game server...", 5, orange);
		}
		py += 14;
	}
	else if (mScreen == Screen::JOINED)
	{
		const bool inLobby = (nullptr != client && client->hasLobbyState());
		const char* text = inLobby ? (client->isLobbyHostStarting() ? "The host is picking a game..." : "Press READY when you are set.") : "Connecting to the lobby...";
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), text, 5, inLobby ? white : orange);
		py += 20;
	}
	else if (mScreen == Screen::JOIN)
	{
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Enter the code your friend gave you:", 5, grey);
		py += 18;
	}
	else
	{
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "Play with up to three friends. Create a lobby", 5, grey);
		py += 12;
		drawer.printText(global::mOxyfontRegular, Vec2i(anchorX, py), "and share the code, or join a lobby with a code.", 5, grey);
		py += 18;
	}

	// The four players
	if (mScreen == Screen::HOSTING || mScreen == Screen::JOINED)
	{
		uint8 states[4];
		bool connected = false;
		getSlotStates(states, connected);
		const int mySlot = (mScreen == Screen::HOSTING) ? 0 : ((nullptr != client) ? (int)client->getLobbyYourSlot() : -1);

		for (int slot = 0; slot < 4; ++slot)
		{
			std::string name = "PLAYER " + std::to_string(slot + 1);
			if (slot == 0)
				name += " (HOST)";
			if (slot == mySlot)
				name += " - YOU";

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

			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX - 90, py), name, 4, (states[slot] != 0) ? white : grey);
			drawer.printText(global::mOxyfontSmall, Vec2i(anchorX + 90, py), status, 6, color);
			py += 13;
		}
		py += 10;
	}

	// Buttons
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
		drawer.printText(global::mOxyfontRegular, Recti(anchorX, py, 0, 10), entry.mText, 5, color);
		py += 16;
	}

	if (mScreen == Screen::HOSTING && nullptr != host && mMessage.empty())
	{
		const int count = host->getConnectedClientCount();
		const char* hint = (count == 0) ? "Waiting for friends to join..." : (host->areAllClientsReady() ? "Everybody is ready!" : "Waiting for everybody to be ready...");
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, 208), hint, 5, grey);
	}
	if (!mMessage.empty())
		drawer.printText(global::mOxyfontSmall, Vec2i(anchorX, 208), mMessage, 5, orange);
}
