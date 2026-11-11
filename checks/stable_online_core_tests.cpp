#include "oxygen/network/netplay/NetplayCodes.h"
#include "oxygen/network/netplay/NetplayModFingerprint.h"
#include "oxygen_netcore/network/Sockets.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int failures = 0;

	void report(const char* name, bool passed)
	{
		std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
		if (!passed)
			++failures;
	}

	std::string modSignature(const char* uniqueId, const char* version, const std::vector<NetplayModFingerprint::File>& files)
	{
		return std::string(uniqueId) + "@" + version + "#" + std::to_string(NetplayModFingerprint::hashFiles(files));
	}
}

int main()
{
	for (const std::string& line : NetplayCodes::runSelfTest())
	{
		std::cout << line << '\n';
		if (line.compare(0, 5, "PASS ") != 0)
			++failures;
	}

	using NetplayModFingerprint::File;
	const std::vector<File> files = {
		{ "scripts/main.lemon", { 0x11, 0x22, 0x33 }, true },
		{ "mod.json", { 0x44, 0x55 }, true }
	};
	const std::vector<File> reversed = { files[1], files[0] };
	report("mod fingerprint ignores file enumeration order", NetplayModFingerprint::hashFiles(files) == NetplayModFingerprint::hashFiles(reversed));
	report("same id/version with changed file bytes has a different signature",
		modSignature("example.coop", "1.0", files) != modSignature("example.coop", "1.0", { { "scripts/main.lemon", { 0x11, 0x22, 0x34 }, true }, files[1] }));
	report("same bytes at a different relative path has a different fingerprint",
		NetplayModFingerprint::hashFiles(files) != NetplayModFingerprint::hashFiles({ { "scripts/renamed.lemon", files[0].mContent, true }, files[1] }));
	report("unreadable and empty files have different fingerprints",
		NetplayModFingerprint::hashFiles({ { "data.bin", {}, false } }) != NetplayModFingerprint::hashFiles({ { "data.bin", {}, true } }));

	bool lanFound = false;
	bool lanMismatchRejected = false;
	Sockets::startupSockets();
	{
		UDPSocket host;
		UDPSocket friendSocket;
		uint16 hostPort = 0;
		for (uint16 candidate = 40000; candidate < 40100; ++candidate)
		{
			if (host.bindToPort(candidate))
			{
				hostPort = candidate;
				break;
			}
		}
		const bool socketsReady = (hostPort != 0 && friendSocket.bindToAnyPort());
		if (socketsReady)
		{
			const uint64 sessionID = 0x123456789aull;
			host.setLanHostSession(sessionID);
			friendSocket.setLanSearchSession(sessionID);
			friendSocket.sendLanSearchRequest(hostPort);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < deadline && !lanFound)
			{
				UDPSocket::ReceiveResult received;
				host.receiveNonBlocking(received);
				friendSocket.receiveNonBlocking(received);
				SocketAddress foundAddress;
				lanFound = friendSocket.takeLanFoundAddress(foundAddress);
				if (!lanFound)
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}

			UDPSocket wrongCodeSocket;
			if (wrongCodeSocket.bindToAnyPort())
			{
				wrongCodeSocket.setLanSearchSession(sessionID + 1);
				wrongCodeSocket.sendLanSearchRequest(hostPort);
				const auto mismatchDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
				bool wrongCodeFound = false;
				while (std::chrono::steady_clock::now() < mismatchDeadline)
				{
					UDPSocket::ReceiveResult received;
					host.receiveNonBlocking(received);
					wrongCodeSocket.receiveNonBlocking(received);
					SocketAddress ignored;
					if (wrongCodeSocket.takeLanFoundAddress(ignored))
					{
						wrongCodeFound = true;
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				SocketAddress ignored;
				lanMismatchRejected = !wrongCodeFound && !wrongCodeSocket.takeLanFoundAddress(ignored);
			}
		}
	}
	Sockets::shutdownSockets();
	report("LAN discovery finds a host with the matching session code", lanFound);
	report("LAN discovery rejects a different session code", lanMismatchRejected);

	std::cout << (failures == 0 ? "ONLINE CORE TESTS PASS" : "ONLINE CORE TESTS FAIL") << " (" << failures << " failures)\n";
	return failures == 0 ? 0 : 1;
}
