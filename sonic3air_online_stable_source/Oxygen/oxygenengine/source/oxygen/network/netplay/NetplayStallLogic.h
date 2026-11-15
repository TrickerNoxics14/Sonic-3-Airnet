/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>
#include <string>
#include <vector>


// The rules for a player whose game stands still (in the browser build: the tab went into the background, where browsers stop the animation frames and slow the
// timers down to about one per second). Pure logic without any engine dependency, so that the developer test program can check it.
//  - the player's own browser announces "my tab is hidden" at once (PlayerAwayPacket), so the others do not have to wait for a timeout to know why
//  - without such a message (an older game, a frozen page) a silent player is noticed after NOTICE_AFTER_SECONDS
//  - the host plays on without a player that lags behind (NetplayHost::canBeginNextFrame gives up after 8 seconds), and drops a player that stays silent for DROP_AFTER_SECONDS
//  - a friend does not treat a host that announced "my tab is hidden" as lost for HOST_AWAY_GIVE_UP_SECONDS (the host normally comes back), and then says so
namespace NetplayStall
{
	static const constexpr float NOTICE_AFTER_SECONDS = 2.5f;			// Silence that is worth a notice when nobody said why
	static const constexpr float AWAY_NOTICE_AFTER_SECONDS = 0.75f;		// Silence that is worth a notice from a player who said that the tab is hidden
	static const constexpr float DROP_AFTER_SECONDS = 25.0f;			// The host drops a friend that was silent this long (the slot is kept for a minute for a return)
	static const constexpr float HOST_AWAY_GIVE_UP_SECONDS = 60.0f;		// A friend gives up on a silent host that said its tab is hidden after this long
	static const constexpr float AWAY_RESEND_SECONDS = 1.0f;			// A hidden tab that still runs repeats its "away" message this often
	static const constexpr float AWAY_EXPIRE_SECONDS = 5.0f;			// An "away" message that is older than this counts only for a player who is silent

	// What one player says about its own tab, as the others know it
	struct AwayTracker
	{
		bool mAway = false;
		float mSecondsSinceMessage = 0.0f;		// Since the last message that said "away"

		inline void onMessage(bool away)	{ mAway = away; mSecondsSinceMessage = 0.0f; }
		inline void update(float deltaSeconds)  { mSecondsSinceMessage += deltaSeconds; }
		inline void reset()					{ mAway = false; mSecondsSinceMessage = 0.0f; }

		// "away" counts while the player is silent, or while the message is fresh (a player that keeps playing in a hidden tab is not stalling anybody)
		inline bool isAway(float silentSeconds) const  { return mAway && (silentSeconds >= AWAY_NOTICE_AFTER_SECONDS || mSecondsSinceMessage < AWAY_EXPIRE_SECONDS); }
	};

	inline bool isStalled(bool away, float silentSeconds)
	{
		return silentSeconds >= (away ? AWAY_NOTICE_AFTER_SECONDS : NOTICE_AFTER_SECONDS);
	}

	inline bool shouldDropFriend(float silentSeconds)
	{
		return silentSeconds >= DROP_AFTER_SECONDS;
	}

	inline bool shouldGiveUpOnHost(bool hostAway, float silentSeconds)
	{
		return hostAway && silentSeconds >= HOST_AWAY_GIVE_UP_SECONDS;
	}

	struct Waiting
	{
		std::string mName;
		bool mAway = false;
		int mSilentSeconds = 0;
	};

	// The text on the screen of the players who wait ("Waiting for Sam: their tab is in the background (12 s)"); empty if nobody waits
	inline std::string describeWaiting(const std::vector<Waiting>& waiting)
	{
		if (waiting.empty())
			return std::string();
		std::string text = "Waiting for ";
		bool allAway = true;
		int longest = 0;
		for (size_t k = 0; k < waiting.size(); ++k)
		{
			if (k > 0)
				text += (k + 1 == waiting.size()) ? " and " : ", ";
			text += waiting[k].mName.empty() ? std::string("a player") : waiting[k].mName;
			allAway = allAway && waiting[k].mAway;
			longest = (waiting[k].mSilentSeconds > longest) ? waiting[k].mSilentSeconds : longest;
		}
		if (allAway)
			text += (waiting.size() == 1) ? ": their tab is in the background" : ": their tabs are in the background";
		else
			text += ": no answer";
		text += " (" + std::to_string(longest) + " s)";
		return text;
	}

	inline std::string describeDropped(const std::string& name, bool away)
	{
		return (name.empty() ? std::string("A player") : name) + " was dropped: " + (away ? "the tab stayed in the background too long" : "no answer for too long") + ". The slot is kept for a minute.";
	}

	// What the player sees on the title of its own hidden tab (it cannot see the game, but it sees the tab strip)
	static const constexpr char* RETURN_TO_TAB_TITLE = "Return to the tab - the game waits for you";
}
