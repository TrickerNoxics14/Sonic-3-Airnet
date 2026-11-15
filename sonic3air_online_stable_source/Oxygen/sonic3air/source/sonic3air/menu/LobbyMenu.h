/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "sonic3air/menu/GameMenuBase.h"
#include "oxygen/network/netplay/NetplayFailure.h"
#include "oxygen_netcore/network/DirectoryProtocol.h"
#include "oxygen/application/input/InputManager.h"

class MenuBackground;


// The online menu. Players can
//  - browse the public lobbies of the lobby server and join one (or press QUICK JOIN for the best one),
//  - create a lobby (public, or private and joined only with its code),
//  - join with a code: 6 digits (a lobby of the lobby server) or 13 digits (a long code, which also works without any server of ours, on the same network),
//  - play on the same network without any server (LAN GAME),
//  - set the player name, the lobby server and the relay server.
// In a lobby: player names with their ping and character, ready state, quick chat, the host's input delay setting, kicking a player, and a ready check with a time limit.
// When everybody is ready, the host starts the game as usual (Normal Game, Act Select, ...) and the friends follow.
class LobbyMenu : public GameMenuBase
{
public:
	LobbyMenu(MenuBackground& menuBackground);
	~LobbyMenu();

	virtual BaseState getBaseState() const override;
	virtual void setBaseState(BaseState baseState) override;
	virtual void onFadeIn() override;
	virtual bool canBeRemoved() override;

	virtual void initialize() override;
	virtual void deinitialize() override;
	virtual void keyboard(const rmx::KeyboardEvent& ev) override;
	virtual void textinput(const rmx::TextInputEvent& ev) override;
	virtual void update(float timeElapsed) override;
	virtual void render() override;

private:
	enum class State
	{
		INACTIVE,
		APPEAR,
		SHOW,
		FADE_TO_MENU
	};

	enum class Screen
	{
		HOME,		// The first screen: public lobbies, quick join, create, join with a code, LAN, settings
		LIST,		// The public lobbies
		JOIN,		// Typing a code (number pad)
		CREATE,		// Name, public or private, number of players
		LAN,		// Play on the same network, without any server
		SETTINGS,	// Player name, lobby server, relay server
		SEARCHING,	// Waiting for the lobby server (quick join, looking up a code)
		HOSTING,	// In a lobby as the host
		JOINED,		// In a lobby as a friend
		TEXT		// Typing a player name, a lobby name, an address or a chat line
	};

	enum class TextMode
	{
		NAME,
		CHAT,
		RELAY,			// The address of the relay server
		LOBBY_SERVER,	// The address of the lobby server
		LOBBY_NAME		// The name of a new lobby
	};

	enum class Flow
	{
		NONE,
		QUICK,			// Quick join: get the list, take the best lobby, look it up, join
		RESOLVE			// A lobby code (typed or from the list) is being looked up
	};

private:
	void chooseScreen();
	void showScreen(Screen screen);
	void rebuildEntries();
	void createLobby(bool viaServer, bool isPublic, int maxPlayers, const std::string& lobbyName, bool lanOnly = false);
	bool joinWithCode(const std::string& code);
	bool joinWithSessionCode(uint64 sessionID);
	void beginResolve(const std::string& lobbyCode, const std::string& lobbyName);
	void beginQuickJoin();
	void updateFlow();
	void finishFlow(const std::string& message, NetplayFailure::Reason failure);
	void joinListEntry(int index);
	void leaveLobby();
	void startGame(bool force = false);
	void pressStart();
	void goBack();
	void goBackOneLevel();
	void getSlotStates(uint8 outStates[4], bool& outConnected) const;
	void showFailureAndReturnToStart();
	void retry();
	void openTextScreen(TextMode mode, Screen returnTo);
	void sendQuickChat();
	void updateEntryTexts();
	void updateTestHooks(float timeElapsed);
	void updateBrowserInput();
	void updateJoinScreenInput(const InputManager::ControllerScheme& keys);
	void refreshList();
	std::string getPlayerNameOfSlot(int slot) const;
	int getPingOfSlot(int slot) const;
	int findKickTarget(int startSlot, int step) const;
	int getMySlot() const;
	uint16 getCharacterOfSlot(int slot);
	uint16 getMyCharacter();
	bool areCharactersLocked();
	void changeCharacter(int step);
	std::string getServerStatusLine(uint32& outColorKind) const;
	std::string getNoServerMessage() const;
	void renderBrowserScreen(Drawer& drawer, int anchorX, float alpha);
	void renderCodeKeypad(Drawer& drawer, int anchorX, float alpha, int top);

private:
	MenuBackground* mMenuBackground = nullptr;
	State mState = State::INACTIVE;
	Screen mScreen = Screen::HOME;
	Screen mTextReturnScreen = Screen::HOME;
	Screen mSearchReturnScreen = Screen::HOME;
	TextMode mTextMode = TextMode::NAME;
	GameMenuEntries mMenuEntries;
	float mRelayWaitSeconds = 0.0f;		// Browser build: how long the host has been waiting for the relay server to know its lobby
	bool mWasWaking = false;			// Browser build: the lobby server did not answer and is being retried (it may be asleep), as of the last frame
	size_t mLoggedCodeDigits = 0;		// What the log says about the code screen (the automated checks read it)
	std::string mMessage;		// Shown to the player, e.g. why joining failed
	bool mShowCopied = false;
	int mQuickIndex = 1;		// Quick chat message 1 to 12, or 13 for "write a message"
	int mKickSlot = 0;			// The friend the Kick entry points at (0 = nobody)

	// What the player tried last, so that "Retry" can do the same again after a failure
	std::string mLastJoinCode;
	bool mLastWasHosting = false;
	float mAutoRetryTimer = 0.0f;		// Developer test switch S3AIR_LOBBY_AUTORETRY
	bool mLastHostViaServer = true;
	bool mLastHostPublic = true;
	bool mLastHostLanOnly = false;
	int mLastHostMaxPlayers = 4;
	std::string mLastHostName;
	NetplayFailure::Reason mFailure = NetplayFailure::Reason::NONE;

	// Code typing (number pad)
	std::string mCodeDigits;
	bool mTextInputThisFrame = false;		// Text arrived from the keyboard in this frame or the one before: keys that are letters must not press the pad
	bool mTextInputLastFrame = false;
	bool mJoinLanOnly = false;				// The code screen was opened from LAN GAME

	// Public lobbies
	bool mListCompatibleOnly = false;
	std::vector<DirectoryProtocol::ListEntry> mShownList;
	float mLastListAge = 1.0e9f;
	float mListRefreshTimer = 0.0f;
	int mListScroll = 0;
	std::string mListSelectedCode;			// The row that was selected when the list was rebuilt
	int mListFooterSelected = -1;			// Or the button below the rows that was selected (0 = refresh), -1 if it was a row

	// Creating a lobby
	std::string mLobbyName;
	bool mLobbyPublic = true;
	int mLobbyMaxPlayers = 4;

	// Waiting for the lobby server
	Flow mFlow = Flow::NONE;
	float mFlowSeconds = 0.0f;
	std::string mFlowLobbyName;
	std::string mFlowCode;
	bool mFlowJoinedFromList = false;

	// Developer test switches (only with the test hooks enabled): chat, kick, start and input delay without key presses
	float mHookLobbyTimer = 0.0f;		// Seconds since somebody was in the lobby
	bool mHookChatDone = false;
	bool mHookSpamDone = false;
	bool mHookKickDone = false;
	bool mHookStartDone = false;
	bool mHookDelayDone = false;
	float mHookScreenTimer = 0.0f;		// S3AIR_LOBBY_SCREEN: seconds since the menu appeared
	bool mHookScreenDone = false;
	int mHookScreenIndex = 0;
	bool mHookTypeDone = false;
	float mHookTypeTimer = 0.0f;
	bool mHookQuickDone = false;
	int mLastWarnedSlot = -1;			// The friend the host was last warned about (high ping), for the log
	float mHostingMessageSeconds = 0.0f;	// A hint that was set while hosting (nobody joined yet, ready check asked) is shown for a few seconds only: it must not hide the warnings and the ready check status for good
	std::string mLastDrawnStatus;		// What the bottom line of the lobby screen showed last (written to the log when it changes, so that the checks see what was really drawn)
	float mCharacterNoticeSeconds = 0.0f;	// A note that the host changed this player's character is shown for a few seconds
	std::string mCharacterNoticeText;
	bool mHookGameDone = false;
	bool mHostCharacterNoticeSeen = false;
};
