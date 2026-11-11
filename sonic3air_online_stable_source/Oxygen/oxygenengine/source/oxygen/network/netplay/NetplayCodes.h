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

	// Session code: a random 40 bit number that identifies a game at the game server, plus one check character, written as "XXXX-XXXXX".
	// The check character notices every single mistyped character. The session ID also derives the secret that is checked when connecting.
	inline int sessionCodeCheckIndex(uint64 sessionID)
	{
		int sum = 7;
		for (int i = 0; i < 8; ++i)
			sum += (2 * i + 1) * (int)((sessionID >> ((7 - i) * 5)) & 31);
		return sum & 31;
	}

	inline std::string makeSessionCode(uint64 sessionID)
	{
		std::string result;
		for (int i = 7; i >= 0; --i)
		{
			result += ALPHABET[(sessionID >> (i * 5)) & 31];
			if (i == 4)
				result += '-';
		}
		result += ALPHABET[sessionCodeCheckIndex(sessionID)];
		return result;
	}

	// Session IDs below 0x10000000 are never generated (and an empty or mistyped field looks like "0000-00000")
	inline bool readSessionCode(const char* text, uint64& outSessionID)
	{
		const std::string clean = cleanCode(text);
		if (clean.size() != 9)
			return false;

		uint64 value = 0;
		for (int i = 0; i < 8; ++i)
		{
			const int index = alphabetIndex(clean[i]);
			if (index < 0)
				return false;
			value = (value << 5) | (uint64)index;
		}
		const int check = alphabetIndex(clean[8]);
		if (check < 0 || check != sessionCodeCheckIndex(value) || value < 0x10000000ull)
			return false;
		outSessionID = value;
		return true;
	}

	// Developer self test: one line per check, starting with "PASS" or "FAIL"
	inline std::vector<std::string> runSelfTest()
	{
		std::vector<std::string> results;
		auto report = [&results](const std::string& name, bool passed, const std::string& detail)
		{
			results.push_back(std::string(passed ? "PASS " : "FAIL ") + name + (detail.empty() ? std::string() : ": " + detail));
		};

		const uint64 sessionSamples[] = { 0x10000000ull, 0xffffffffffull, 0x123456789aull, 0x1000000001ull, 0x8000000000ull };
		{
			bool passed = true;
			std::string failures;
			for (uint64 sample : sessionSamples)
			{
				const std::string code = makeSessionCode(sample);
				uint64 back = 0;
				if (code.size() != 10 || !readSessionCode(code.c_str(), back) || back != sample)
				{
					passed = false;
					failures += code + " ";
				}
			}
			report("session code round trip", passed, failures);
		}
		{
			uint64 back = 0;
			const std::string code = makeSessionCode(0x123456789aull);
			std::string lowerNoDash;
			for (char ch : code)
				if (ch != '-')
					lowerNoDash += (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
			report("session code accepts lower case without dash", readSessionCode(lowerNoDash.c_str(), back) && back == 0x123456789aull, "");
		}
		{
			// Every single mistyped character of a session code is noticed
			int accepted = 0;
			const std::string code = makeSessionCode(0x123456789aull);
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
					uint64 back = 0;
					if (readSessionCode(mutated.c_str(), back))
						++accepted;
				}
			}
			report("session code notices every single typing mistake", accepted == 0, std::to_string(accepted) + " accepted");
			uint64 back = 0;
			report("session code rejects the empty id and short input", !readSessionCode("0000-00000", back) && !readSessionCode("ABCD-123", back), "");
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
