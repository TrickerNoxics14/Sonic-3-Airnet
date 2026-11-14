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
#include <cstring>
#include <cstdlib>
#include <algorithm>


// Short codes that players type in instead of IP addresses
// Header only, so that the developer self test (see "runSelfTest") tests exactly the code that the game uses
namespace NetplayCodes
{
	const char* const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";	// No I, L, O, U, as they are easy to mix up

	// Keeps only the characters that matter, normalizing case and the characters that look alike
	inline std::string cleanCode(const char* text)
	{
		std::string clean;
		for (const char* p = text; *p != 0; ++p)
		{
			char ch = *p;
			if (ch == '-' || ch == ' ')
				continue;
			if (ch >= 'a' && ch <= 'z')
				ch -= 32;
			if (ch == 'O')
				ch = '0';
			else if (ch == 'I' || ch == 'L')
				ch = '1';
			clean += ch;
		}
		return clean;
	}

	inline int alphabetIndex(char ch)
	{
		const char* pos = strchr(ALPHABET, ch);
		return (ch != 0 && nullptr != pos) ? (int)(pos - ALPHABET) : -1;
	}

	// Dotted IPv4 address to a 32 bit value, without any C library function that differs between platforms
	inline bool parseIPv4(const std::string& text, uint32& outValue)
	{
		uint32 value = 0;
		uint32 current = 0;
		int parts = 0;
		bool hasDigit = false;
		for (char ch : text)
		{
			if (ch >= '0' && ch <= '9')
			{
				current = current * 10 + (uint32)(ch - '0');
				if (current > 255)
					return false;
				hasDigit = true;
			}
			else if (ch == '.')
			{
				if (!hasDigit)
					return false;
				value = (value << 8) | current;
				current = 0;
				hasDigit = false;
				++parts;
			}
			else
			{
				return false;
			}
		}
		if (!hasDigit || parts != 3)
			return false;
		outValue = (value << 8) | current;
		return true;
	}

	// 12 bit checksum (CRC-12, polynomial 0x80F) over the data bits of a join code.
	// A single mistyped character changes at most five neighbouring bits, and a CRC-12 notices every error burst of up to 12 bits.
	inline uint32 joinCodeChecksum(const uint8* bits, int numBits)
	{
		uint32 crc = 0x5a5;
		for (int i = 0; i < numBits; ++i)
		{
			const uint32 top = ((crc >> 11) & 1) ^ bits[i];
			crc = (crc << 1) & 0xfff;
			if (top)
				crc ^= 0x80f;
		}
		return crc;
	}

	// Join code: the host's address and port (48 bits), a random secret (30 bits) and a checksum (12 bits), in 18 characters, written as "XXXXXX-XXXXXX-XXXXXX".
	// The secret is what keeps strangers out: the host only lets in friends who prove that they know it (see NetplayAuth.h).
	static const int JOIN_CODE_DATA_BITS = 78;
	static const int JOIN_CODE_CHARS = 18;

	inline std::string makeJoinCode(const std::string& ip, uint16 port, uint32 secret)
	{
		uint32 address = 0;
		if (!parseIPv4(ip, address) || secret > 0x3fffffffu)
			return std::string();

		uint8 bits[JOIN_CODE_CHARS * 5] = { 0 };
		int count = 0;
		auto put = [&](uint64 value, int numBits)
		{
			for (int i = numBits - 1; i >= 0; --i)
				bits[count++] = (uint8)((value >> i) & 1);
		};
		put(address, 32);
		put(port, 16);
		put(secret, 30);
		put(joinCodeChecksum(bits, JOIN_CODE_DATA_BITS), 12);

		std::string result;
		for (int c = 0; c < JOIN_CODE_CHARS; ++c)
		{
			if (c > 0 && (c % 6) == 0)
				result += '-';
			int index = 0;
			for (int b = 0; b < 5; ++b)
				index = (index << 1) | bits[c * 5 + b];
			result += ALPHABET[index];
		}
		return result;
	}

	inline bool readJoinCode(const char* text, std::string& outIp, uint16& outPort, uint32& outSecret)
	{
		const std::string clean = cleanCode(text);
		if ((int)clean.size() != JOIN_CODE_CHARS)
			return false;

		uint8 bits[JOIN_CODE_CHARS * 5] = { 0 };
		for (int c = 0; c < JOIN_CODE_CHARS; ++c)
		{
			const int index = alphabetIndex(clean[c]);
			if (index < 0)
				return false;
			for (int b = 0; b < 5; ++b)
				bits[c * 5 + b] = (uint8)((index >> (4 - b)) & 1);
		}

		auto get = [&](int offset, int numBits)
		{
			uint64 value = 0;
			for (int i = 0; i < numBits; ++i)
				value = (value << 1) | bits[offset + i];
			return value;
		};
		if ((uint32)get(JOIN_CODE_DATA_BITS, 12) != joinCodeChecksum(bits, JOIN_CODE_DATA_BITS))
			return false;

		const uint32 address = (uint32)get(0, 32);
		outIp = std::to_string((address >> 24) & 0xff) + "." + std::to_string((address >> 16) & 0xff) + "." + std::to_string((address >> 8) & 0xff) + "." + std::to_string(address & 0xff);
		outPort = (uint16)get(32, 16);
		outSecret = (uint32)get(48, 30);
		return true;
	}

	// ---- Numeric codes (since 1.3): the codes that players type are numbers only ----
	//
	// Lobby code: 6 digits, handed out by the lobby server (relay_ws) for one lobby ("123 456"). It is only a way to find the lobby: the server answers with the
	// session ID below, and everything else (the secret, the relay lobby, the LAN tag) is derived from that ID, so nothing but the 6 digits is ever typed.
	// Session code: the 40 bit class secret behind a lobby as 12 digits plus one check digit (13 digits, "1234-5678-90123"). It works without any server of ours:
	// LAN discovery, the matchmaking server and a relay all find the host from it. A single wrong digit or two swapped neighbours are always noticed (Damm check digit).
	// Neither code ever contains a letter, so they can be typed on a number pad, a controller or a phone's number keyboard.
	static const int LOBBY_CODE_DIGITS = 6;
	static const int SESSION_CODE_DIGITS = 13;
	static const uint64 SESSION_ID_MIN = 100000000000ull;		// 10^11: always 12 digits, so a code never starts with 0
	static const uint64 SESSION_ID_LIMIT = 1000000000000ull;	// 10^12 (exclusive)

	// Damm algorithm: a check digit that detects every single wrong digit and every swap of two neighbouring digits
	inline int dammCheckDigit(const std::string& digits)
	{
		static const uint8 TABLE[10][10] =
		{
			{ 0, 3, 1, 7, 5, 9, 8, 6, 4, 2 },
			{ 7, 0, 9, 2, 1, 5, 4, 8, 6, 3 },
			{ 4, 2, 0, 6, 8, 7, 1, 3, 5, 9 },
			{ 1, 7, 5, 0, 9, 8, 3, 4, 2, 6 },
			{ 6, 1, 2, 3, 0, 4, 5, 9, 7, 8 },
			{ 3, 6, 7, 4, 2, 0, 9, 5, 8, 1 },
			{ 5, 8, 6, 9, 7, 2, 0, 1, 3, 4 },
			{ 8, 9, 4, 5, 3, 6, 2, 0, 1, 7 },
			{ 9, 4, 3, 8, 6, 1, 7, 2, 0, 5 },
			{ 2, 5, 8, 1, 4, 3, 6, 7, 9, 0 }
		};
		int interim = 0;
		for (char ch : digits)
			interim = TABLE[interim][(ch >= '0' && ch <= '9') ? (ch - '0') : 0];
		return interim;
	}

	// Keeps the separators out and leaves everything else (so that a letter is rejected, not silently dropped)
	inline std::string stripSeparators(const char* text)
	{
		std::string clean;
		for (const char* p = (nullptr != text) ? text : ""; *p != 0; ++p)
		{
			if (*p == '-' || *p == ' ' || *p == '.' || *p == '\t')
				continue;
			clean += *p;
		}
		return clean;
	}

	inline bool isAllDigits(const std::string& text)
	{
		if (text.empty())
			return false;
		for (char ch : text)
			if (ch < '0' || ch > '9')
				return false;
		return true;
	}

	// For typing and pasting into a code field: only the digits stay (a code that was pasted with spaces, dashes or some words around it still works)
	inline std::string keepDigitsOnly(const std::string& text, size_t maxDigits = (size_t)SESSION_CODE_DIGITS)
	{
		std::string digits;
		for (char ch : text)
		{
			if (ch >= '0' && ch <= '9' && digits.size() < maxDigits)
				digits += ch;
		}
		return digits;
	}

	inline std::string makeSessionCode(uint64 sessionID)
	{
		if (sessionID < SESSION_ID_MIN || sessionID >= SESSION_ID_LIMIT)
			return std::string();
		const std::string digits = std::to_string(sessionID);
		const std::string full = digits + (char)('0' + dammCheckDigit(digits));
		return full.substr(0, 4) + "-" + full.substr(4, 4) + "-" + full.substr(8);
	}

	inline bool readSessionCode(const char* text, uint64& outSessionID)
	{
		const std::string clean = stripSeparators(text);
		if ((int)clean.size() != SESSION_CODE_DIGITS || !isAllDigits(clean) || dammCheckDigit(clean) != 0)
			return false;
		const uint64 value = strtoull(clean.substr(0, SESSION_CODE_DIGITS - 1).c_str(), nullptr, 10);
		if (value < SESSION_ID_MIN || value >= SESSION_ID_LIMIT)
			return false;
		outSessionID = value;
		return true;
	}

	// The session ID that a new lobby gets, from a random 64 bit value: uniform enough, always in range
	inline uint64 sessionIDFromRandom(uint64 random)
	{
		return SESSION_ID_MIN + random % (SESSION_ID_LIMIT - SESSION_ID_MIN);
	}

	inline std::string formatLobbyCode(const std::string& digits)
	{
		return (digits.size() == (size_t)LOBBY_CODE_DIGITS) ? (digits.substr(0, 3) + " " + digits.substr(3)) : digits;
	}

	inline bool readLobbyCode(const char* text, std::string& outDigits)
	{
		const std::string clean = stripSeparators(text);
		if ((int)clean.size() != LOBBY_CODE_DIGITS || !isAllDigits(clean) || clean[0] == '0')
			return false;
		outDigits = clean;
		return true;
	}

	// What a typed code is: the menu decides what to do from this
	enum class CodeKind : uint8 { EMPTY, LOBBY, SESSION, HAS_LETTERS, WRONG_LENGTH, BAD_CHECK };
	inline CodeKind classifyCode(const char* text, std::string& outDigits)
	{
		const std::string clean = stripSeparators(text);
		if (clean.empty())
			return CodeKind::EMPTY;
		if (!isAllDigits(clean))
			return CodeKind::HAS_LETTERS;
		outDigits = clean;
		if ((int)clean.size() == LOBBY_CODE_DIGITS)
			return (clean[0] == '0') ? CodeKind::WRONG_LENGTH : CodeKind::LOBBY;
		if ((int)clean.size() == SESSION_CODE_DIGITS)
		{
			uint64 ignored = 0;
			return readSessionCode(clean.c_str(), ignored) ? CodeKind::SESSION : CodeKind::BAD_CHECK;
		}
		return CodeKind::WRONG_LENGTH;
	}

	// Developer self test: one line per check, starting with "PASS" or "FAIL"
	inline std::vector<std::string> runSelfTest()
	{
		std::vector<std::string> results;
		auto report = [&results](const std::string& name, bool passed, const std::string& detail)
		{
			results.push_back(std::string(passed ? "PASS " : "FAIL ") + name + (detail.empty() ? std::string() : ": " + detail));
		};

		const uint64 sessionSamples[] = { SESSION_ID_MIN, SESSION_ID_LIMIT - 1, 123456789012ull, 100000000001ull, 800000000000ull, 555555555555ull };
		{
			bool passed = true;
			std::string failures;
			for (uint64 sample : sessionSamples)
			{
				const std::string code = makeSessionCode(sample);
				uint64 back = 0;
				if (code.size() != 15 || !readSessionCode(code.c_str(), back) || back != sample)
				{
					passed = false;
					failures += code + " ";
				}
			}
			report("session code round trip (13 digits)", passed, failures);
		}
		{
			uint64 back = 0;
			const std::string code = makeSessionCode(123456789012ull);
			std::string noDash, spaced;
			for (char ch : code)
			{
				if (ch != '-')
					noDash += ch;
				spaced += (ch == '-') ? ' ' : ch;
			}
			report("session code accepts the digits with spaces, dashes or none", readSessionCode(noDash.c_str(), back) && back == 123456789012ull && readSessionCode(spaced.c_str(), back) && back == 123456789012ull, "");
		}
		{
			// Every single mistyped digit and every swap of two neighbouring digits of a session code is noticed
			int acceptedWrong = 0;
			int acceptedSwaps = 0;
			int total = 0;
			for (uint64 sample : sessionSamples)
			{
				std::string code = makeSessionCode(sample);
				code.erase(std::remove(code.begin(), code.end(), '-'), code.end());
				for (size_t i = 0; i < code.size(); ++i)
				{
					for (char digit = '0'; digit <= '9'; ++digit)
					{
						if (digit == code[i])
							continue;
						std::string mutated = code;
						mutated[i] = digit;
						uint64 back = 0;
						++total;
						if (readSessionCode(mutated.c_str(), back))
							++acceptedWrong;
					}
					if (i + 1 < code.size() && code[i] != code[i + 1])
					{
						std::string swapped = code;
						std::swap(swapped[i], swapped[i + 1]);
						uint64 back = 0;
						++total;
						if (readSessionCode(swapped.c_str(), back))
							++acceptedSwaps;
					}
				}
			}
			report("session code notices every single typing mistake and every swapped pair", acceptedWrong == 0 && acceptedSwaps == 0, std::to_string(acceptedWrong) + " wrong digits and " + std::to_string(acceptedSwaps) + " swaps accepted of " + std::to_string(total));
			uint64 back = 0;
			report("session code rejects short, long and out of range input", !readSessionCode("0000-0000-00000", back) && !readSessionCode("1234-5678", back) && !readSessionCode("1234-5678-90123-4", back) && !readSessionCode("", back) && makeSessionCode(5).empty() && makeSessionCode(SESSION_ID_LIMIT).empty(), "");
		}
		{
			// The guard: nothing a player is shown or may type contains a letter
			bool noLetters = true;
			for (uint64 sample : sessionSamples)
				for (char ch : makeSessionCode(sample))
					if (!((ch >= '0' && ch <= '9') || ch == '-'))
						noLetters = false;
			uint64 back = 0;
			std::string digits;
			const bool oldStyleRefused = !readSessionCode("ABCD-EFGH7", back) && !readSessionCode("4C2K-9X7MA", back)
				&& classifyCode("ABCD-EFGH7", digits) == CodeKind::HAS_LETTERS && classifyCode("12345A", digits) == CodeKind::HAS_LETTERS
				&& classifyCode("1234-5678-9O123", digits) == CodeKind::HAS_LETTERS;		// (a letter O is not read as a zero any more)
			report("codes contain only digits: shown codes have no letter, letters are refused, not corrected", noLetters && oldStyleRefused, "");
		}
		{
			// Lobby codes: 6 digits from the lobby server
			std::string digits;
			const bool read = readLobbyCode("123 456", digits) && digits == "123456" && readLobbyCode("123-456", digits) && readLobbyCode("123456", digits);
			const bool refused = !readLobbyCode("12345", digits) && !readLobbyCode("1234567", digits) && !readLobbyCode("012345", digits) && !readLobbyCode("12345a", digits) && !readLobbyCode("", digits) && !readLobbyCode("12 34 5x", digits);
			report("lobby code: 6 digits with optional space or dash, nothing else", read && refused && formatLobbyCode("123456") == "123 456", "");
			report("codes are told apart by what they are (lobby, session, wrong)", classifyCode("123 456", digits) == CodeKind::LOBBY && classifyCode(makeSessionCode(123456789012ull).c_str(), digits) == CodeKind::SESSION
				&& classifyCode("", digits) == CodeKind::EMPTY && classifyCode("1234-5678-90124", digits) == CodeKind::BAD_CHECK && classifyCode("1234567", digits) == CodeKind::WRONG_LENGTH && classifyCode("012345", digits) == CodeKind::WRONG_LENGTH, "");
			report("typed text keeps only digits (paste with words around the code works)", keepDigitsOnly("Code: 123 456!") == "123456" && keepDigitsOnly("ABC") == "" && keepDigitsOnly("1234-5678-90123-99") == "1234567890123", "");
		}
		{
			// New session IDs from random numbers always have 12 digits, so a code never starts with 0, and every one of them makes a valid code
			bool ok = true;
			for (uint64 random : { 0ull, 1ull, 0xffffffffffffffffull, 0x123456789abcdef0ull, 899999999999ull, 900000000000ull })
			{
				const uint64 id = sessionIDFromRandom(random);
				uint64 back = 0;
				ok &= (id >= SESSION_ID_MIN && id < SESSION_ID_LIMIT) && readSessionCode(makeSessionCode(id).c_str(), back) && back == id;
			}
			report("new session IDs are always in range", ok, "");
		}

		struct Sample { std::string ip; uint16 port; uint32 secret; };
		const std::vector<Sample> joinSamples = { { "26.12.200.7", 28840, 0x12345678u & 0x3fffffffu }, { "100.64.1.2", 28840, 1 }, { "192.168.0.5", 28840, 0x3fffffffu }, { "255.255.255.255", 65535, 0x2aaaaaaau }, { "5.193.152.226", 28840, 0x15555555u }, { "0.0.0.1", 1, 0x00ff00ffu } };
		{
			bool passed = true;
			std::string failures;
			for (const Sample& sample : joinSamples)
			{
				const std::string code = makeJoinCode(sample.ip, sample.port, sample.secret);
				std::string ip;
				uint16 port = 0;
				uint32 secret = 0;
				if (code.size() != 20 || !readJoinCode(code.c_str(), ip, port, secret) || ip != sample.ip || port != sample.port || secret != sample.secret)
				{
					passed = false;
					failures += sample.ip + " ";
				}
			}
			report("join code round trip (address, port and secret)", passed, failures);
		}
		report("join code rejects an invalid address or secret", makeJoinCode("999.1.1.1", 28840, 5).empty() && makeJoinCode("1.2.3", 28840, 5).empty() && makeJoinCode("1.2.3.4", 28840, 0x40000000u).empty(), "");
		{
			const std::string code = makeJoinCode("26.12.200.7", 28840, 0x2468aceu);
			std::string ip;
			uint16 port = 0;
			uint32 secret = 0;
			const std::string shorter = code.substr(0, code.size() - 1);
			const std::string longer = code + "7";
			report("join code rejects wrong length", !readJoinCode(shorter.c_str(), ip, port, secret) && !readJoinCode(longer.c_str(), ip, port, secret), "");

			std::string invalidChar = code;
			invalidChar[0] = 'U';
			report("join code rejects invalid characters", !readJoinCode(invalidChar.c_str(), ip, port, secret), "");
		}
		{
			// Every single character typo in a code must be noticed (CRC-12 detects all bursts of up to 12 bits)
			int total = 0;
			int accepted = 0;
			for (const Sample& sample : joinSamples)
			{
				const std::string code = makeJoinCode(sample.ip, sample.port, sample.secret);
				for (size_t i = 0; i < code.size(); ++i)
				{
					if (code[i] == '-')
						continue;
					for (const char* alternative = ALPHABET; *alternative != 0; ++alternative)
					{
						if (*alternative == code[i])
							continue;
						std::string mutated = code;
						mutated[i] = *alternative;
						std::string ip;
						uint16 port = 0;
						uint32 secret = 0;
						++total;
						if (readJoinCode(mutated.c_str(), ip, port, secret))
							++accepted;
					}
				}
			}
			report("join code notices every single typing mistake", accepted == 0, std::to_string(accepted) + " of " + std::to_string(total) + " single character typos accepted");
		}

		return results;
	}
}
