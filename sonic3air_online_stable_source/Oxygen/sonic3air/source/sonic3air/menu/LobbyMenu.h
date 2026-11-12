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

class MenuBackground;


// Online lobby: one player creates a lobby and shares its code, friends join with the code and press Ready.
// When everybody is ready, the host starts the game as usual (Normal Game, Act Select, ...) and the friends follow.
// In the lobby: player names with their ping, quick chat messages, the host's input delay setting, kicking a player, and a ready check with a time limit.
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
		START,		// Create a lobby or join one
		JOIN,		// Typing the code
		HOSTING,	// In a lobby as the host
		JOINED,		// In a lobby as a friend
		TEXT		// Typing a player name or a chat line
	};

	enum class TextMode
	{
		NAME,
		CHAT
	};

private:
	void chooseScreen();
	void rebuildEntries();
	void createLobby(bool viaServer = true);
	void joinLobby();
	bool joinWithCode(const std::string& code);
	void leaveLobby();
	void startGame(bool force = false);
	void pressStart();
	void goBack();
	void getSlotStates(uint8 outStates[4], bool& outConnected) const;
	void showFailureAndReturnToStart();
	void retry();
	void openTextScreen(TextMode mode);
	void sendQuickChat();
	void updateEntryTexts();
	void updateTestHooks(float timeElapsed);
	std::string getPlayerNameOfSlot(int slot) const;
	int getPingOfSlot(int slot) const;
	int findKickTarget(int startSlot, int step) const;
	int getMySlot() const;
	uint16 getCharacterOfSlot(int slot);
	uint16 getMyCharacter();
	bool areCharactersLocked();
	void changeCharacter(int step);

private:
	MenuBackground* mMenuBackground = nullptr;
	State mState = State::INACTIVE;
	Screen mScreen = Screen::START;
	TextMode mTextMode = TextMode::NAME;
	GameMenuEntries mMenuEntries;
	std::string mMessage;		// Shown to the player, e.g. why joining failed
	bool mShowCopied = false;
	int mQuickIndex = 1;		// Quick chat message 1 to 12, or 13 for "write a message"
	int mKickSlot = 0;			// The friend the Kick entry points at (0 = nobody)

	// What the player tried last, so that "Retry" can do the same again after a failure
	std::string mLastJoinCode;
	bool mLastWasHosting = false;
	float mAutoRetryTimer = 0.0f;		// Developer test switch S3AIR_LOBBY_AUTORETRY
	bool mLastHostViaServer = true;
	NetplayFailure::Reason mFailure = NetplayFailure::Reason::NONE;

	// Developer test switches (only with the test hooks enabled): chat, kick, start and input delay without key presses
	float mHookLobbyTimer = 0.0f;		// Seconds since somebody was in the lobby
	bool mHookChatDone = false;
	bool mHookSpamDone = false;
	bool mHookKickDone = false;
	bool mHookStartDone = false;
	bool mHookDelayDone = false;
	int mLastWarnedSlot = -1;			// The friend the host was last warned about (high ping), for the log
	float mHostingMessageSeconds = 0.0f;	// A hint that was set while hosting (nobody joined yet, ready check asked) is shown for a few seconds only: it must not hide the warnings and the ready check status for good
	std::string mLastDrawnStatus;		// What the bottom line of the lobby screen showed last (written to the log when it changes, so that the checks see what was really drawn)
	float mCharacterNoticeSeconds = 0.0f;	// A note that the host changed this player's character is shown for a few seconds
	std::string mCharacterNoticeText;
	bool mHookGameDone = false;
	bool mHostCharacterNoticeSeen = false;
};
