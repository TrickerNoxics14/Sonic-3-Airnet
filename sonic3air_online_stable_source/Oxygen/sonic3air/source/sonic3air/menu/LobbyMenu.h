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
		JOINED		// In a lobby as a friend
	};

private:
	void chooseScreen();
	void rebuildEntries();
	void createLobby(bool viaServer = true);
	void joinLobby();
	bool joinWithCode(const std::string& code);
	void leaveLobby();
	void startGame();
	void goBack();
	void getSlotStates(uint8 outStates[4], bool& outConnected) const;
	void showFailureAndReturnToStart();
	void retry();

private:
	MenuBackground* mMenuBackground = nullptr;
	State mState = State::INACTIVE;
	Screen mScreen = Screen::START;
	GameMenuEntries mMenuEntries;
	std::string mMessage;		// Shown to the player, e.g. why joining failed
	bool mShowCopied = false;

	// What the player tried last, so that "Retry" can do the same again after a failure
	std::string mLastJoinCode;
	bool mLastWasHosting = false;
	float mAutoRetryTimer = 0.0f;		// Developer test switch S3AIR_LOBBY_AUTORETRY
	bool mLastHostViaServer = true;
	NetplayFailure::Reason mFailure = NetplayFailure::Reason::NONE;
};
