/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <string>
#include <string_view>
#include <vector>


// Small, self-contained rules of the online lobby: player names, chat, ping, input delay, slot reservations.
// Header only (no engine dependencies), so that the developer test program checks exactly the code that the game uses.
//
// How the online game works (for the input delay below): the host runs the game and decides which input every player has on every frame.
// A friend sends its input to the host, the host puts it into the frame it is just running and sends the finished frames back; the friend's game
// runs exactly the frames it got from the host. There is no rollback: a friend sees its own button press only after one round trip.
// The input delay holds back *every* player's input (the host's own too) by the same number of frames before it enters that stream.
// That does not make anything faster; it makes the host's own controls feel as late as the friends' and keeps the timing of presses even.
namespace NetplayLobby
{
	static const size_t MAX_NAME_LENGTH = 12;
	static const size_t MAX_CHAT_LENGTH = 40;
	static const int HIGH_PING_MS = 150;			// The host is warned about friends above this round trip time
	static const int MAX_INPUT_DELAY = 6;			// Frames
	static const int INPUT_DELAY_AUTO = -1;
	static const int NUM_QUICK_MESSAGES = 12;
	static const uint32 CHAT_MIN_INTERVAL_MS = 1200;
	static const float READY_CHECK_SECONDS = 20.0f;
	static const float SLOT_RESERVATION_SECONDS = 60.0f;
	static const float KICK_BLOCK_SECONDS = 60.0f;
	static const char* const DEFAULT_NAME = "Player";


	// A character that may appear in names and chat lines: printable ASCII without anything that could confuse a log or a menu
	inline bool isAllowedChar(unsigned char c)
	{
		if (c >= '0' && c <= '9')	return true;
		if (c >= 'a' && c <= 'z')	return true;
		if (c >= 'A' && c <= 'Z')	return true;
		switch (c)
		{
			case ' ': case '_': case '-': case '.': case '!': case '?': case '+': case '#': case '*': case ',': case ':': case '(': case ')': case '\'':
				return true;
			default:
				return false;
		}
	}

	inline std::string sanitizeText(std::string_view raw, size_t maxLength)
	{
		std::string result;
		bool lastWasSpace = true;		// Also removes leading spaces
		for (const char ch : raw)
		{
			const unsigned char c = (unsigned char)ch;
			if (!isAllowedChar(c))
				continue;
			if (c == ' ')
			{
				if (lastWasSpace)
					continue;
				lastWasSpace = true;
			}
			else
			{
				lastWasSpace = false;
			}
			result += (char)c;
			if (result.size() >= maxLength)
				break;
		}
		while (!result.empty() && result.back() == ' ')
			result.pop_back();
		return result;
	}

	// A player name: only plain characters, at most 12 of them, never empty (the default is just "Player", not the Windows user name)
	inline std::string sanitizeName(std::string_view raw)
	{
		const std::string name = sanitizeText(raw, MAX_NAME_LENGTH);
		return name.empty() ? std::string(DEFAULT_NAME) : name;
	}

	// A chat line: plain characters, at most 40 of them (may be empty, then it is not sent)
	inline std::string sanitizeChat(std::string_view raw)
	{
		return sanitizeText(raw, MAX_CHAT_LENGTH);
	}


	// The quick messages: a fixed list, so that a friend can say something without typing and without any text coming over the network
	inline const char* getQuickMessage(int id)
	{
		static const char* const MESSAGES[NUM_QUICK_MESSAGES] =
		{
			"Hello!", "Ready!", "One moment...", "Let's go!", "Good luck!", "Nice one!",
			"Thanks!", "Sorry!", "Wait for me!", "Go ahead!", "Help!", "Good game!"
		};
		return (id >= 1 && id <= NUM_QUICK_MESSAGES) ? MESSAGES[id - 1] : nullptr;
	}

	// The text of a chat line as shown: a quick message by its number, or the sanitised free text
	inline std::string buildChatText(int quickId, std::string_view freeText)
	{
		if (const char* quick = getQuickMessage(quickId))
			return quick;
		return sanitizeChat(freeText);
	}

	// At most one chat line per 1.2 seconds per player; the host drops the rest
	struct ChatRateLimiter
	{
		uint64 mLastAcceptedMs = 0;
		bool mHasAccepted = false;
		int mDropped = 0;

		inline bool allow(uint64 nowMs)
		{
			if (mHasAccepted && nowMs - mLastAcceptedMs < CHAT_MIN_INTERVAL_MS)
			{
				++mDropped;
				return false;
			}
			mHasAccepted = true;
			mLastAcceptedMs = nowMs;
			return true;
		}
	};


	// Round trip time: a smoothed value over the last measurements
	struct PingTracker
	{
		bool mHasValue = false;
		float mSmoothed = 0.0f;
		float mDeviation = 0.0f;		// Smoothed distance of the samples from their average: how unsteady the line is (a TCP / WebSocket detour through a relay is much worse at that than UDP)

		inline void addSample(int milliseconds)
		{
			const float sample = (float)std::max(0, std::min(milliseconds, 10000));
			if (!mHasValue)
			{
				mSmoothed = sample;
				mDeviation = 0.0f;
				mHasValue = true;
			}
			else
			{
				const float distance = (sample > mSmoothed) ? (sample - mSmoothed) : (mSmoothed - sample);
				mDeviation += (distance - mDeviation) * 0.2f;
				mSmoothed += (sample - mSmoothed) * 0.3f;
			}
		}
		inline int get() const  { return mHasValue ? (int)(mSmoothed + 0.5f) : -1; }
		inline int getDeviation() const  { return mHasValue ? (int)(mDeviation + 0.5f) : 0; }
	};

	inline bool isHighPing(int pingMs)
	{
		return pingMs > HIGH_PING_MS;
	}


	// Input delay: -1 = Auto, 0 to 6 = a fixed number of frames
	inline int clampDelaySetting(int setting)
	{
		if (setting < 0)
			return INPUT_DELAY_AUTO;
		return std::min(setting, MAX_INPUT_DELAY);
	}

	// What "Auto" picks for a measured round trip time (of the slowest friend): about one frame per 33 ms, 0 to 6 frames.
	// The measured value always contains about 20 ms of the game's own frame timing (both games only look at the network once per frame; on one
	// computer the measurement shows 16 to 25 ms), so that part does not count: a good connection gets no delay at all.
	// Unknown ping (nothing measured yet) means no delay.
	static const int PING_FLOOR_MS = 20;
	inline int autoInputDelay(int worstPingMs)
	{
		if (worstPingMs < 0)
			return 0;
		const float networkPing = (float)std::max(0, worstPingMs - PING_FLOOR_MS);
		return std::max(0, std::min(MAX_INPUT_DELAY, (int)(networkPing / 33.3f + 0.5f)));
	}

	// Through a relay server (and in particular through a browser's WebSocket, which is TCP: one lost packet holds back everything behind it until it is sent again),
	// the line is much less steady than a direct UDP connection. Auto then plans for the unsteadiness too: the ping plus twice its usual deviation, and never less than
	// RELAY_MIN_DELAY frames. A fixed setting is never changed.
	static const int RELAY_MIN_DELAY = 1;
	inline int autoInputDelayRelayed(int worstPingMs, int worstDeviationMs)
	{
		if (worstPingMs < 0)
			return RELAY_MIN_DELAY;
		const float planned = (float)std::max(0, worstPingMs - PING_FLOOR_MS) + 2.0f * (float)std::max(0, worstDeviationMs);
		return std::max(RELAY_MIN_DELAY, std::min(MAX_INPUT_DELAY, (int)(planned / 33.3f + 0.5f)));
	}

	inline int effectiveInputDelay(int setting, int worstPingMs, bool relayed = false, int worstDeviationMs = 0)
	{
		const int clamped = clampDelaySetting(setting);
		if (clamped != INPUT_DELAY_AUTO)
			return clamped;
		return relayed ? autoInputDelayRelayed(worstPingMs, worstDeviationMs) : autoInputDelay(worstPingMs);
	}

	// Text for the lobby, e.g. "AUTO (3 frames)", "2 frames" or "OFF"
	inline std::string describeInputDelay(int setting, int effective)
	{
		const std::string frames = (effective == 0) ? std::string("0 frames") : (std::to_string(effective) + ((effective == 1) ? " frame" : " frames"));
		if (clampDelaySetting(setting) == INPUT_DELAY_AUTO)
			return "AUTO (" + frames + ")";
		return (effective == 0) ? std::string("OFF") : frames;
	}

	// The next choice when the host cycles through the settings: Auto, 0, 1, ... 6, Auto
	inline int nextDelaySetting(int setting, int step)
	{
		int index = clampDelaySetting(setting) + 1;		// 0 = Auto, 1 = 0 frames, ...
		const int count = MAX_INPUT_DELAY + 2;
		index = ((index + step) % count + count) % count;
		return index - 1;
	}


	// Holds back the inputs of all players by the same number of frames. The result for frame n is the input of frame n - delay;
	// until that many frames exist, the first input is repeated (no button gets released for a moment, and nothing is invented).
	// Changing the delay while it runs repeats or skips a frame once, for all players at the same time.
	class InputDelayLine
	{
	public:
		static const int NUM_PLAYERS = 4;
		typedef std::array<uint16, NUM_PLAYERS> Frame;

		inline void setDelay(int frames)
		{
			mDelay = std::max(0, std::min(frames, MAX_INPUT_DELAY));
			while ((int)mHistory.size() > mDelay + 1)
				mHistory.pop_front();
		}
		inline int getDelay() const  { return mDelay; }
		inline void clear()  { mHistory.clear(); }

		inline Frame process(const Frame& input)
		{
			mHistory.push_back(input);
			while ((int)mHistory.size() > mDelay + 1)
				mHistory.pop_front();
			return mHistory.front();
		}

	private:
		int mDelay = 0;
		std::deque<Frame> mHistory;
	};


	// Player slots of friends whose connection dropped: held for a while for the same player, so that the friend gets the same slot back
	// (instead of a lobby that filled up in the meantime). A friend who left on purpose or was kicked does not reserve anything.
	class SlotReservations
	{
	public:
		struct Item
		{
			int mSlot = 0;
			std::string mKey;
			std::string mName;
			uint16 mCharacter = 0xffff;		// The character the player had picked (0xffff = none), so that it keeps it when it comes back
			float mRemaining = 0.0f;
		};

		inline void add(int slot, const std::string& key, const std::string& name, uint16 character = 0xffff)
		{
			remove(slot);
			Item item;
			item.mSlot = slot;
			item.mKey = key;
			item.mName = name;
			item.mCharacter = character;
			item.mRemaining = SLOT_RESERVATION_SECONDS;
			mItems.push_back(item);
		}
		// The character that was picked by the player (identified by key) in this slot, or 0xffff if nothing is held for it
		inline uint16 findCharacterFor(int slot, const std::string& key) const
		{
			for (const Item& item : mItems)
				if (item.mSlot == slot && item.mKey == key)
					return item.mCharacter;
			return 0xffff;
		}
		inline void remove(int slot)
		{
			mItems.erase(std::remove_if(mItems.begin(), mItems.end(), [slot](const Item& item) { return item.mSlot == slot; }), mItems.end());
		}
		inline void update(float deltaSeconds)
		{
			for (Item& item : mItems)
				item.mRemaining -= deltaSeconds;
			mItems.erase(std::remove_if(mItems.begin(), mItems.end(), [](const Item& item) { return item.mRemaining <= 0.0f; }), mItems.end());
		}
		// Is this slot held for somebody else? (the key identifies a player: see NetplayHost::identityOf)
		inline bool isReservedForOther(int slot, const std::string& key) const
		{
			for (const Item& item : mItems)
				if (item.mSlot == slot && item.mKey != key)
					return true;
			return false;
		}
		// The slot held for this player (the preferred one if it has several), or 0
		inline int findSlotFor(const std::string& key, int preferredSlot) const
		{
			int found = 0;
			for (const Item& item : mItems)
			{
				if (item.mKey != key)
					continue;
				if (item.mSlot == preferredSlot)
					return item.mSlot;
				if (found == 0)
					found = item.mSlot;
			}
			return found;
		}
		inline const std::vector<Item>& getItems() const  { return mItems; }

	private:
		std::vector<Item> mItems;
	};


	// Players the host removed: they cannot come back right away (this is not a ban: leaving the game and starting it again gets a new identity, and the block ends after a minute)
	class KickBlockList
	{
	public:
		inline void add(const std::string& key)
		{
			for (auto& pair : mBlocked)
			{
				if (pair.first == key)
				{
					pair.second = KICK_BLOCK_SECONDS;
					return;
				}
			}
			mBlocked.emplace_back(key, KICK_BLOCK_SECONDS);
		}
		inline void update(float deltaSeconds)
		{
			for (auto& pair : mBlocked)
				pair.second -= deltaSeconds;
			mBlocked.erase(std::remove_if(mBlocked.begin(), mBlocked.end(), [](const std::pair<std::string, float>& pair) { return pair.second <= 0.0f; }), mBlocked.end());
		}
		inline bool isBlocked(const std::string& key) const
		{
			for (const auto& pair : mBlocked)
				if (pair.first == key)
					return true;
			return false;
		}

	private:
		std::vector<std::pair<std::string, float>> mBlocked;
	};


	// Wrong codes per address. After MAX_AUTH_FAILURES wrong proofs an address is ignored, but only for a while (the count restarts after
	// AUTH_FAILURE_WINDOW_SECONDS without another failure; a friend behind the same network address as a guesser, or one who mistyped, is not locked out for the whole session),
	// and the list cannot grow without bound (the entry that expires first is dropped when it is full).
	static const int MAX_AUTH_FAILURES = 8;
	static const float AUTH_FAILURE_WINDOW_SECONDS = 60.0f;
	static const size_t MAX_AUTH_FAILURE_ENTRIES = 256;

	class AuthFailureTracker
	{
	public:
		inline bool isBlocked(const std::string& ip) const
		{
			for (const Item& item : mItems)
				if (item.mIP == ip)
					return item.mCount >= MAX_AUTH_FAILURES;
			return false;
		}
		inline int recordFailure(const std::string& ip)
		{
			for (Item& item : mItems)
			{
				if (item.mIP == ip)
				{
					item.mRemaining = AUTH_FAILURE_WINDOW_SECONDS;
					return ++item.mCount;
				}
			}
			if (mItems.size() >= MAX_AUTH_FAILURE_ENTRIES)
			{
				auto oldest = std::min_element(mItems.begin(), mItems.end(), [](const Item& a, const Item& b) { return a.mRemaining < b.mRemaining; });
				mItems.erase(oldest);
			}
			Item item;
			item.mIP = ip;
			item.mCount = 1;
			item.mRemaining = AUTH_FAILURE_WINDOW_SECONDS;
			mItems.push_back(item);
			return 1;
		}
		// A friend that proved the code starts over
		inline void recordSuccess(const std::string& ip)
		{
			mItems.erase(std::remove_if(mItems.begin(), mItems.end(), [&ip](const Item& item) { return item.mIP == ip && item.mCount < MAX_AUTH_FAILURES; }), mItems.end());
		}
		inline void update(float deltaSeconds)
		{
			for (Item& item : mItems)
				item.mRemaining -= deltaSeconds;
			mItems.erase(std::remove_if(mItems.begin(), mItems.end(), [](const Item& item) { return item.mRemaining <= 0.0f; }), mItems.end());
		}
		inline size_t size() const  { return mItems.size(); }

	private:
		struct Item
		{
			std::string mIP;
			int mCount = 0;
			float mRemaining = 0.0f;
		};
		std::vector<Item> mItems;
	};


	// Connections that did not prove the code yet must not be able to lock real friends out: a stranger needs no secret to open one.
	// Each address may hold only a few of them, and when the table is full the oldest one that had its chance is replaced by the newcomer.
	static const int MAX_PENDING_PER_ADDRESS = 2;
	static const float MIN_PENDING_AGE_TO_REPLACE = 3.0f;
	static const int REFUSE_NEW_CONNECTION = -2;
	static const int ACCEPT_NEW_CONNECTION = -1;

	struct ConnectionInfo
	{
		std::string mIP;
		bool mAuthenticated = false;
		float mPendingSeconds = 0.0f;
	};

	// Result: ACCEPT_NEW_CONNECTION, REFUSE_NEW_CONNECTION, or the index of the existing connection to remove first
	inline int chooseConnectionToReplace(const std::vector<ConnectionInfo>& existing, const std::string& newIP, size_t maxTotal)
	{
		int sameAddressCount = 0, oldestSameAddress = -1, oldestAny = -1;
		for (size_t k = 0; k < existing.size(); ++k)
		{
			const ConnectionInfo& info = existing[k];
			if (info.mAuthenticated)
				continue;
			if (info.mIP == newIP)
			{
				++sameAddressCount;
				if (oldestSameAddress < 0 || info.mPendingSeconds > existing[oldestSameAddress].mPendingSeconds)
					oldestSameAddress = (int)k;
			}
			if (info.mPendingSeconds >= MIN_PENDING_AGE_TO_REPLACE && (oldestAny < 0 || info.mPendingSeconds > existing[oldestAny].mPendingSeconds))
				oldestAny = (int)k;
		}
		if (sameAddressCount >= MAX_PENDING_PER_ADDRESS)
			return oldestSameAddress;
		if (existing.size() >= maxTotal)
			return (oldestAny >= 0) ? oldestAny : REFUSE_NEW_CONNECTION;
		return ACCEPT_NEW_CONNECTION;
	}


	// ---------------------------------------------------------------------------------------------------------------
	// Character choice: every player picks the character they play with, in the lobby.
	//
	// What the game can really run (see DropInCoop/scripts/charpick.lemon, which carries a pick out):
	//  - Player 1 (the host) plays the main character of the game: Sonic or Knuckles, each with Tails as the second character (the game's own
	//    "Sonic & Tails" and "Knuckles & Tails"). "Game menu" keeps whatever the save slot / game menu says, as before this feature existed.
	//  - Player 2 is the game's second character, which is Tails. If player 2 picks somebody else, that body is turned into the picked character.
	//  - Players 3 and 4 are the extra characters of the mod ("party" actors): Sonic, Knuckles, or a character of an installed extra-slot mod.
	//  - Tails exists once in the game (it is the second character), so only player 2 can be Tails.
	// An extra character is a character of an "extra slot" mod (Extra Slots Unlimited and the mods built on it): it has a slot number N (1 to 255),
	// and its mod provides the script function "ExtraChar.startup_charN". The list of installed extra characters is read from the game's scripts at
	// run time, it is not fixed in the program. Without such a mod the list is just Sonic, Tails and Knuckles.
	// Everybody must have the same mods (the join check enforces that), so everybody's list is the same, but the host checks every pick anyway.
	static const uint16 CHARACTER_SONIC = 0;
	static const uint16 CHARACTER_TAILS = 1;
	static const uint16 CHARACTER_KNUCKLES = 2;
	static const uint16 CHARACTER_EXTRA_BASE = 0x100;		// The extra slot N (1 to 255) is the id 0x100 + N
	static const uint16 CHARACTER_MENU = 0xfffe;			// Player 1 only: the character chosen in the game's own menus (save slot, act select)
	static const uint16 CHARACTER_NONE = 0xffff;			// On the wire: "no request", also "unknown"
	static const int MAX_EXTRA_CHARACTERS = 24;				// More than this many extra characters are not offered (the list stays small and bounded)
	static const uint8 LOBBY_PROTOCOL = 2;					// Version of the lobby messages: 2 = with character choice. A peer with another number is refused as a different version.

	inline bool isExtraCharacter(uint16 id)
	{
		return id > CHARACTER_EXTRA_BASE && id <= CHARACTER_EXTRA_BASE + 255;
	}

	inline int extraSlotOf(uint16 id)
	{
		return isExtraCharacter(id) ? (int)(id - CHARACTER_EXTRA_BASE) : 0;
	}

	inline uint16 makeExtraCharacter(int slot)
	{
		return (slot >= 1 && slot <= 255) ? (uint16)(CHARACTER_EXTRA_BASE + slot) : CHARACTER_NONE;
	}

	// The name that the lobby shows (capital letters like the rest of the lobby); an extra character is called after its slot unless it is a well known one
	inline std::string getCharacterName(uint16 id)
	{
		switch (id)
		{
			case CHARACTER_SONIC:		return "SONIC";
			case CHARACTER_TAILS:		return "TAILS";
			case CHARACTER_KNUCKLES:	return "KNUCKLES";
			case CHARACTER_MENU:		return "GAME MENU";
			default: break;
		}
		if (isExtraCharacter(id))
		{
			switch (extraSlotOf(id))
			{
				case 35:	return "MODERN SONIC";
				case 90:	return "RETO";
				default:	return "EXTRA " + std::to_string(extraSlotOf(id));
			}
		}
		return "?";
	}

	// The characters that are installed: the three stock ones, plus the extra slots whose script function was found (sorted, no duplicates, bounded)
	class CharacterList
	{
	public:
		inline void setExtraSlots(const std::vector<int>& slots)
		{
			mExtras.clear();
			std::vector<int> sorted(slots);
			std::sort(sorted.begin(), sorted.end());
			for (const int slot : sorted)
			{
				if (slot < 1 || slot > 255 || (!mExtras.empty() && mExtras.back() == slot) || (int)mExtras.size() >= MAX_EXTRA_CHARACTERS)
					continue;
				mExtras.push_back(slot);
			}
		}
		inline bool hasExtra(int slot) const  { return std::find(mExtras.begin(), mExtras.end(), slot) != mExtras.end(); }
		inline const std::vector<int>& getExtraSlots() const  { return mExtras; }
		inline bool operator==(const CharacterList& other) const  { return mExtras == other.mExtras; }
		inline bool operator!=(const CharacterList& other) const  { return mExtras != other.mExtras; }
	private:
		std::vector<int> mExtras;
	};

	// The characters player "slot" (0 = host, 1 to 3 = friends) may pick, in the order in which the lobby cycles through them; the first one is the default.
	// "configuredExtraSlot" is the extra slot that the mod's older setting "Player 3/4 Extra Character Slot" names for this player (0 = none): it is the default
	// then, so that a host who set it up that way keeps what he had.
	inline std::vector<uint16> getChoicesForSlot(int slot, const CharacterList& available, int configuredExtraSlot = 0)
	{
		std::vector<uint16> choices;
		switch (slot)
		{
			case 0:  choices = { CHARACTER_MENU, CHARACTER_SONIC, CHARACTER_KNUCKLES };  break;
			case 1:  choices = { CHARACTER_TAILS, CHARACTER_SONIC, CHARACTER_KNUCKLES };  break;
			case 2:  choices = { CHARACTER_KNUCKLES, CHARACTER_SONIC };  break;
			case 3:  choices = { CHARACTER_SONIC, CHARACTER_KNUCKLES };  break;
			default: return choices;
		}
		if (slot >= 1)		// (the host's character is the game's main character: an extra character is picked in the game's own menu there, when the mod supports it)
		{
			for (const int extra : available.getExtraSlots())
				choices.push_back(makeExtraCharacter(extra));
			if (slot >= 2 && configuredExtraSlot > 0 && available.hasExtra(configuredExtraSlot))
			{
				// The mod setting names the default: that character moves to the front
				const uint16 configured = makeExtraCharacter(configuredExtraSlot);
				choices.erase(std::remove(choices.begin(), choices.end(), configured), choices.end());
				choices.insert(choices.begin(), configured);
			}
		}
		return choices;
	}

	inline uint16 defaultCharacterForSlot(int slot, const CharacterList& available, int configuredExtraSlot = 0)
	{
		const std::vector<uint16> choices = getChoicesForSlot(slot, available, configuredExtraSlot);
		return choices.empty() ? CHARACTER_SONIC : choices.front();
	}

	inline bool isCharacterAllowedForSlot(int slot, uint16 id, const CharacterList& available, int configuredExtraSlot = 0)
	{
		const std::vector<uint16> choices = getChoicesForSlot(slot, available, configuredExtraSlot);
		return std::find(choices.begin(), choices.end(), id) != choices.end();
	}

	// What the host makes of a request: the request if it is allowed for that player (and installed), otherwise "fallback" (the pick the player had; if that is
	// not allowed any more either, the slot's default). "outAdjusted" tells that the request was not accepted as asked.
	inline uint16 sanitizeCharacterPick(int slot, uint16 requested, const CharacterList& available, uint16 fallback, bool* outAdjusted = nullptr, int configuredExtraSlot = 0)
	{
		if (isCharacterAllowedForSlot(slot, requested, available, configuredExtraSlot))
		{
			if (nullptr != outAdjusted)
				*outAdjusted = false;
			return requested;
		}
		if (nullptr != outAdjusted)
			*outAdjusted = true;
		return isCharacterAllowedForSlot(slot, fallback, available, configuredExtraSlot) ? fallback : defaultCharacterForSlot(slot, available, configuredExtraSlot);
	}

	// The next choice when a player cycles through the list (left and right); wraps around. A pick that is not in the list starts at the default.
	inline uint16 nextCharacterChoice(int slot, uint16 current, int step, const CharacterList& available, int configuredExtraSlot = 0)
	{
		const std::vector<uint16> choices = getChoicesForSlot(slot, available, configuredExtraSlot);
		if (choices.empty())
			return CHARACTER_SONIC;
		const auto it = std::find(choices.begin(), choices.end(), current);
		const int index = (it == choices.end()) ? 0 : (int)(it - choices.begin());
		const int count = (int)choices.size();
		return choices[(size_t)(((index + step) % count + count) % count)];
	}

	// A player may change the pick a few times per second (cycling through a list), but must not flood the host: a small token bucket per player
	struct CharacterRateLimiter
	{
		float mTokens = 8.0f;
		uint64 mLastMs = 0;
		bool mStarted = false;
		int mDropped = 0;

		inline bool allow(uint64 nowMs)
		{
			if (!mStarted)
			{
				mStarted = true;
				mLastMs = nowMs;
			}
			const uint64 passed = (nowMs >= mLastMs) ? (nowMs - mLastMs) : 0;
			mLastMs = nowMs;
			mTokens = std::min(8.0f, mTokens + (float)passed * 0.004f);		// 4 changes per second on average, a burst of 8
			if (mTokens < 1.0f)
			{
				++mDropped;
				return false;
			}
			mTokens -= 1.0f;
			return true;
		}
	};

	// The picks that are fixed when the game starts (identical on all computers): one id per player slot
	struct SessionCharacters
	{
		uint16 mPicks[4] = { CHARACTER_MENU, CHARACTER_TAILS, CHARACTER_KNUCKLES, CHARACTER_SONIC };
		inline bool isValid(const CharacterList& available) const
		{
			for (int slot = 0; slot < 4; ++slot)
				if (!isCharacterAllowedForSlot(slot, mPicks[slot], available))
					return false;
			return true;
		}
	};


	// A friend's start message carries the picks that the host fixed. They are used exactly as sent or not at all: a pick that cannot be used here (an extra
	// character whose mod this computer does not have, or a character that slot may never have) must not be replaced by something else on this computer only,
	// because then the computers would run different characters. Returns the first slot whose pick cannot be used as it is, or -1 if all can.
	inline int findUnusableSessionPick(const uint16 picks[4], const CharacterList& available, const int configuredExtraSlots[4])
	{
		for (int slot = 0; slot < 4; ++slot)
			if (!isCharacterAllowedForSlot(slot, picks[slot], available, (nullptr != configuredExtraSlots) ? configuredExtraSlots[slot] : 0))
				return slot;
		return -1;
	}

	// The host waits this long for a friend's first lobby message (name, ready, character) before it starts the game and fixes the picks without it. Messages are
	// sent reliably (resent until confirmed), so a friend that is still silent after this long has a connection that is about to time out anyway.
	static const float ANNOUNCE_WAIT_SECONDS = 10.0f;
	inline bool mustWaitForAnnouncement(bool announced, float secondsSinceAuthenticated)
	{
		return !announced && secondsSinceAuthenticated < ANNOUNCE_WAIT_SECONDS;
	}

	// A friend who just asked for another character keeps what it asked for until the host has answered that request. Lobby states that the host sent BEFORE it read the
	// request (they are rebroadcast about once a second and may be on their way for a while) still show the old pick, with the "changed by the host" bit still set from an
	// earlier refusal; taking those over would undo the friend's choice, and the next heartbeat would then ask the host for the old pick again.
	static const float CHARACTER_REQUEST_SETTLE_SECONDS = 3.0f;
	struct CharacterRequestTracker
	{
		uint16 mRequested = CHARACTER_NONE;
		float mAge = 0.0f;
		bool mPending = false;

		inline void request(uint16 character)  { mRequested = character; mAge = 0.0f; mPending = true; }
		inline void update(float deltaSeconds)  { if (mPending) mAge += deltaSeconds; }

		// "hostPick" is what the lobby state says this player plays as. Returns true if the friend has to take over the host's pick as its own.
		inline bool onHostState(uint16 hostPick, bool notice, bool locked)
		{
			if (hostPick == CHARACTER_NONE)
				return false;
			if (locked)
			{
				mPending = false;
				return hostPick != mRequested;
			}
			if (mPending)
			{
				if (hostPick == mRequested)
				{
					mPending = false;		// the host accepted it
					return false;
				}
				if (mAge < CHARACTER_REQUEST_SETTLE_SECONDS)
					return false;			// maybe an old state: the host has not answered the request yet
			}
			if (hostPick != mRequested && notice)
			{
				mPending = false;
				return true;				// the host answered with another character and says that it changed the pick
			}
			return false;
		}
	};


	// Browser build: the relay server's address as a player or a web page writes it, turned into a WebSocket URL.
	//   "relay.example.org"            -> "wss://relay.example.org"       (secure by default: a page served over https may not use ws://)
	//   "relay.example.org:8443/play"  -> "wss://relay.example.org:8443/play"
	//   "127.0.0.1:28842", "localhost" -> "ws://127.0.0.1:28842"          (a relay on the same computer has no certificate)
	//   "https://..." / "http://..."   -> "wss://..." / "ws://..."
	// Returns false for anything that is not a plain host name or address with an optional port and path (no user names, no spaces, no odd characters).
	inline bool normalizeRelayWebSocketURL(const std::string& input, std::string& outURL)
	{
		size_t begin = 0, end = input.size();
		while (begin < end && (input[begin] == ' ' || input[begin] == '	'))
			++begin;
		while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '	'))
			--end;
		std::string text = input.substr(begin, end - begin);
		if (text.empty() || text.size() > 200)
			return false;

		auto startsWithNoCase = [&text](const char* prefix) -> size_t
		{
			size_t i = 0;
			for (; prefix[i] != 0; ++i)
				if (i >= text.size() || std::tolower((unsigned char)text[i]) != prefix[i])
					return 0;
			return i;
		};
		std::string scheme;
		if (size_t n = startsWithNoCase("wss://"))			{ scheme = "wss";  text = text.substr(n); }
		else if (size_t n = startsWithNoCase("ws://"))		{ scheme = "ws";   text = text.substr(n); }
		else if (size_t n = startsWithNoCase("https://"))	{ scheme = "wss";  text = text.substr(n); }
		else if (size_t n = startsWithNoCase("http://"))	{ scheme = "ws";   text = text.substr(n); }
		else if (text.find("://") != std::string::npos)		return false;

		for (const char c : text)
		{
			const bool ok = std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '/' || c == '~' || c == '%' || c == '=' || c == '&' || c == '?';
			if (!ok)
				return false;
		}
		const size_t hostEnd = text.find_first_of("/?");
		const std::string hostPort = text.substr(0, hostEnd);
		const std::string rest = (hostEnd == std::string::npos) ? std::string() : text.substr(hostEnd);
		const size_t colon = hostPort.find(':');
		const std::string host = hostPort.substr(0, colon);
		if (host.empty() || host.front() == '.' || host.front() == '-' || host.back() == '.')
			return false;
		if (colon != std::string::npos)
		{
			const std::string port = hostPort.substr(colon + 1);
			if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos || std::atoi(port.c_str()) < 1 || std::atoi(port.c_str()) > 65535)
				return false;
		}
		if (scheme.empty())
		{
			const bool local = (host == "localhost") || host.compare(0, 4, "127.") == 0;
			scheme = local ? "ws" : "wss";
		}
		outURL = scheme + "://" + hostPort + rest;
		return true;
	}


	// Why the host sends a friend away
	enum class LeaveReason : uint8
	{
		KICKED = 1,			// The host removed the friend
		NOT_READY = 2		// The ready check ran out and the friend was not ready: the game starts without it (it can join the running game)
	};
}
