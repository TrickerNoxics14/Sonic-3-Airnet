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

		inline void addSample(int milliseconds)
		{
			const float sample = (float)std::max(0, std::min(milliseconds, 10000));
			if (!mHasValue)
			{
				mSmoothed = sample;
				mHasValue = true;
			}
			else
			{
				mSmoothed += (sample - mSmoothed) * 0.3f;
			}
		}
		inline int get() const  { return mHasValue ? (int)(mSmoothed + 0.5f) : -1; }
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

	inline int effectiveInputDelay(int setting, int worstPingMs)
	{
		const int clamped = clampDelaySetting(setting);
		return (clamped == INPUT_DELAY_AUTO) ? autoInputDelay(worstPingMs) : clamped;
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
			float mRemaining = 0.0f;
		};

		inline void add(int slot, const std::string& key, const std::string& name)
		{
			remove(slot);
			Item item;
			item.mSlot = slot;
			item.mKey = key;
			item.mName = name;
			item.mRemaining = SLOT_RESERVATION_SECONDS;
			mItems.push_back(item);
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


	// Why the host sends a friend away
	enum class LeaveReason : uint8
	{
		KICKED = 1,			// The host removed the friend
		NOT_READY = 2		// The ready check ran out and the friend was not ready: the game starts without it (it can join the running game)
	};
}
