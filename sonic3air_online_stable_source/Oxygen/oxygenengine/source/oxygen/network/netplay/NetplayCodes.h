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

	// 12 bit checksum over address and port, so that most typing mistakes get noticed
	inline uint32 joinCodeChecksum(uint64 data)
	{
		uint32 check = 0x5a5;
		for (int i = 5; i >= 0; --i)
			check = (check * 31 + (uint32)((data >> (8 * i)) & 0xff)) & 0xfff;
		return check;
	}

	// Join code: the host's address and port (48 bits) plus a checksum (12 bits), in 12 characters, written as "XXXX-XXXX-XXXX"
	inline std::string makeJoinCode(const std::string& ip, uint16 port)
	{
		uint32 address = 0;
		if (!parseIPv4(ip, address))
			return std::string();

		const uint64 data = ((uint64)address << 16) | (uint64)port;
		uint64 value = (data << 12) | joinCodeChecksum(data);
		char chars[12] = { 0 };
		for (int i = 11; i >= 0; --i)
		{
			chars[i] = ALPHABET[value & 31];
			value >>= 5;
		}

		std::string result;
		for (int i = 0; i < 12; ++i)
		{
			if (i == 4 || i == 8)
				result += '-';
			result += chars[i];
		}
		return result;
	}

	inline bool readJoinCode(const char* text, std::string& outIp, uint16& outPort)
	{
		const std::string clean = cleanCode(text);
		if (clean.size() != 12)
			return false;

		uint64 value = 0;
		for (char ch : clean)
		{
			const int index = alphabetIndex(ch);
			if (index < 0)
				return false;
			value = (value << 5) | (uint64)index;
		}

		const uint64 data = value >> 12;
		if ((uint32)(value & 0xfff) != joinCodeChecksum(data))
			return false;

		outIp = std::to_string((data >> 40) & 0xff) + "." + std::to_string((data >> 32) & 0xff) + "." + std::to_string((data >> 24) & 0xff) + "." + std::to_string((data >> 16) & 0xff);
		outPort = (uint16)(data & 0xffff);
		return true;
	}

	// Session code: a random 40 bit number that identifies a game at the game server, written as "XXXX-XXXX"
	inline std::string makeSessionCode(uint64 sessionID)
	{
		std::string result;
		for (int i = 7; i >= 0; --i)
		{
			result += ALPHABET[(sessionID >> (i * 5)) & 31];
			if (i == 4)
				result += '-';
		}
		return result;
	}

	inline bool readSessionCode(const char* text, uint64& outSessionID)
	{
		const std::string clean = cleanCode(text);
		if (clean.size() != 8)
			return false;

		uint64 value = 0;
		for (char ch : clean)
		{
			const int index = alphabetIndex(ch);
			if (index < 0)
				return false;
			value = (value << 5) | (uint64)index;
		}
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

		const uint64 sessionSamples[] = { 0x10000000ull, 0xffffffffffull, 0x123456789aull, 0x1000000001ull, 0x1ull, 0x8000000000ull };
		{
			bool passed = true;
			std::string failures;
			for (uint64 sample : sessionSamples)
			{
				const std::string code = makeSessionCode(sample);
				uint64 back = 0;
				if (code.size() != 9 || !readSessionCode(code.c_str(), back) || back != sample)
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

		struct Sample { std::string ip; uint16 port; };
		const std::vector<Sample> joinSamples = { { "26.12.200.7", 28840 }, { "100.64.1.2", 28840 }, { "192.168.0.5", 28840 }, { "255.255.255.255", 65535 }, { "5.193.152.226", 28840 }, { "0.0.0.1", 1 } };
		{
			bool passed = true;
			std::string failures;
			for (const Sample& sample : joinSamples)
			{
				const std::string code = makeJoinCode(sample.ip, sample.port);
				std::string ip;
				uint16 port = 0;
				if (code.size() != 14 || !readJoinCode(code.c_str(), ip, port) || ip != sample.ip || port != sample.port)
				{
					passed = false;
					failures += sample.ip + " ";
				}
			}
			report("join code round trip", passed, failures);
		}
		report("join code rejects an invalid address", makeJoinCode("999.1.1.1", 28840).empty() && makeJoinCode("1.2.3", 28840).empty(), "");
		{
			const std::string code = makeJoinCode("26.12.200.7", 28840);
			std::string ip;
			uint16 port = 0;
			const std::string shorter = code.substr(0, code.size() - 1);
			const std::string longer = code + "7";
			report("join code rejects wrong length", !readJoinCode(shorter.c_str(), ip, port) && !readJoinCode(longer.c_str(), ip, port), "");

			std::string invalidChar = code;
			invalidChar[0] = 'U';
			report("join code rejects invalid characters", !readJoinCode(invalidChar.c_str(), ip, port), "");
		}
		{
			// Every single character typo in a code must be noticed, except for rare checksum coincidences
			int total = 0;
			int accepted = 0;
			for (const Sample& sample : joinSamples)
			{
				const std::string code = makeJoinCode(sample.ip, sample.port);
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
						++total;
						if (readJoinCode(mutated.c_str(), ip, port))
							++accepted;
					}
				}
			}
			report("join code notices typing mistakes", accepted * 1000 <= total * 5, std::to_string(accepted) + " of " + std::to_string(total) + " single character typos accepted (limit 0.5 percent)");
		}

		return results;
	}
}
