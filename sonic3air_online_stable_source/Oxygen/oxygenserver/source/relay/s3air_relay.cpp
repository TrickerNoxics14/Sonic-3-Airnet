/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

// A small UDP relay for Sonic 3 A.I.R. online play. Players whose connection cannot be made directly (strict NAT, firewalls) can play through it.
// One file of sockets code around RelayCore.h. Builds with Visual Studio (cl) on Windows and with g++ / clang on Linux, no other libraries:
//
//   Windows:  cl /nologo /EHsc /O2 /std:c++17 /I <folder with RelayProtocol.h> s3air_relay.cpp /Fe:s3air_relay.exe ws2_32.lib
//   Linux:    g++ -O2 -std=c++17 -I <folder with RelayProtocol.h> s3air_relay.cpp -o s3air_relay
//
// Usage:  s3air_relay [--port 28842] [--bind 0.0.0.0] [--stats-seconds 60] [--stats-file path] [--run-seconds N] [--max-lobbies N] [--quiet]
//   --run-seconds ends the program by itself after that time (for tests). --stats-file rewrites a file with the counters once per second.
//
// It prints only counters (no addresses, no lobby codes, no player data). See DEPLOY_RELAY.txt.
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "RelayCore.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef SOCKET SocketHandle;
	typedef int SockLen;
	static const SocketHandle INVALID_HANDLE = INVALID_SOCKET;
	static void closeSocket(SocketHandle s)  { closesocket(s); }
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <unistd.h>
	typedef int SocketHandle;
	typedef socklen_t SockLen;
	static const SocketHandle INVALID_HANDLE = -1;
	static void closeSocket(SocketHandle s)  { close(s); }
#endif

static volatile bool gStop = false;
static void onSignal(int)  { gStop = true; }

static double nowSeconds()
{
	return (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
}

static void writeStats(const char* path, const Relay::Stats& s, double uptime)
{
	if (nullptr == path || path[0] == 0)
		return;
	FILE* file = fopen(path, "w");
	if (nullptr == file)
		return;
	fprintf(file, "uptime_s=%.0f\nreceived=%llu\nforwarded=%llu\nforwarded_bytes=%llu\ndropped_invalid=%llu\ndropped_rate=%llu\ndropped_blocked=%llu\nwrong_keys=%llu\nrefused=%llu\nlobbies_created=%llu\nlobbies=%u\npipes=%u\n",
		uptime, (unsigned long long)s.mReceived, (unsigned long long)s.mForwarded, (unsigned long long)s.mForwardedBytes, (unsigned long long)s.mDroppedInvalid,
		(unsigned long long)s.mDroppedRate, (unsigned long long)s.mDroppedBlocked, (unsigned long long)s.mWrongKeys, (unsigned long long)s.mRefused,
		(unsigned long long)s.mLobbiesCreated, (unsigned)s.mLobbies, (unsigned)s.mPipes);
	fclose(file);
}

int main(int argc, char** argv)
{
	int port = RelayProtocol::DEFAULT_PORT;
	std::string bindAddress = "0.0.0.0";
	double statsSeconds = 60.0;
	double runSeconds = 0.0;
	const char* statsFile = nullptr;
	bool quiet = false;
	Relay::Limits limits;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		const bool hasValue = (i + 1 < argc);
		if (arg == "--port" && hasValue)				port = atoi(argv[++i]);
		else if (arg == "--bind" && hasValue)			bindAddress = argv[++i];
		else if (arg == "--stats-seconds" && hasValue)	statsSeconds = atof(argv[++i]);
		else if (arg == "--stats-file" && hasValue)		statsFile = argv[++i];
		else if (arg == "--run-seconds" && hasValue)	runSeconds = atof(argv[++i]);
		else if (arg == "--max-lobbies" && hasValue)	limits.mMaxLobbies = (size_t)std::max(1, atoi(argv[++i]));
		else if (arg == "--quiet")						quiet = true;
		else
		{
			printf("Usage: s3air_relay [--port 28842] [--bind 0.0.0.0] [--stats-seconds 60] [--stats-file path] [--run-seconds N] [--max-lobbies N] [--quiet]\n");
			return (arg == "--help" || arg == "-h") ? 0 : 2;
		}
	}
	if (port <= 0 || port > 65535)
	{
		printf("Invalid port\n");
		return 2;
	}

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		printf("Could not start the network library\n");
		return 1;
	}
#endif

	SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_HANDLE)
	{
		printf("Could not create the socket\n");
		return 1;
	}
	int bufferSize = 4 * 1024 * 1024;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferSize, sizeof(bufferSize));
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufferSize, sizeof(bufferSize));
#ifdef _WIN32
	{
		// Without this, an ICMP "port unreachable" answer to a datagram that was sent to a closed port makes the next receive fail
		DWORD bytesReturned = 0;
		BOOL newBehavior = FALSE;
		WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12), &newBehavior, sizeof(newBehavior), nullptr, 0, &bytesReturned, nullptr, nullptr);
	}
#endif

	sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, bindAddress.c_str(), &local.sin_addr) != 1)
	{
		printf("Invalid bind address\n");
		return 2;
	}
	if (bind(sock, (sockaddr*)&local, sizeof(local)) != 0)
	{
		printf("Could not bind to UDP port %d (is it in use?)\n", port);
		return 1;
	}

	Relay::Core core(limits, [&](const Relay::Endpoint& to, const uint8_t* data, size_t size)
	{
		sockaddr_in destination;
		memset(&destination, 0, sizeof(destination));
		destination.sin_family = AF_INET;
		destination.sin_addr.s_addr = htonl(to.mIP);
		destination.sin_port = htons(to.mPort);
		sendto(sock, (const char*)data, (int)size, 0, (const sockaddr*)&destination, sizeof(destination));
	});

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	const double startTime = nowSeconds();
	double nextUpdate = startTime + 1.0;
	double nextPrint = startTime + statsSeconds;
	if (!quiet)
		printf("s3air_relay: listening on UDP port %d (no addresses or player data are logged)\n", port);
	fflush(stdout);

	static uint8_t buffer[RelayProtocol::MAX_DATAGRAM_SIZE + 64];
	while (!gStop)
	{
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(sock, &readSet);
		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 50000;
		const int ready = select((int)sock + 1, &readSet, nullptr, nullptr, &timeout);

		// Read what is waiting (a bounded number per round, so that the clean-up below always gets its turn)
		for (int count = 0; ready > 0 && count < 2000; ++count)
		{
			sockaddr_in from;
			SockLen fromSize = sizeof(from);
			const int received = recvfrom(sock, (char*)buffer, (int)sizeof(buffer), 0, (sockaddr*)&from, &fromSize);
			if (received < 0)
				break;
			Relay::Endpoint sender;
			sender.mIP = ntohl(from.sin_addr.s_addr);
			sender.mPort = ntohs(from.sin_port);
			core.onDatagram(sender, buffer, (size_t)received, nowSeconds());

			// Anything else waiting? (the loop stops at the first empty read)
			fd_set again;
			FD_ZERO(&again);
			FD_SET(sock, &again);
			timeval zero = { 0, 0 };
			if (select((int)sock + 1, &again, nullptr, nullptr, &zero) <= 0)
				break;
		}

		const double now = nowSeconds();
		if (now >= nextUpdate)
		{
			nextUpdate = now + 1.0;
			core.update(now);
			writeStats(statsFile, core.getStats(), now - startTime);
		}
		if (statsSeconds > 0.0 && now >= nextPrint)
		{
			nextPrint = now + statsSeconds;
			const Relay::Stats& s = core.getStats();
			if (!quiet)
			{
				printf("s3air_relay: lobbies=%u friends=%u forwarded=%llu dropped(invalid=%llu rate=%llu blocked=%llu) wrong_keys=%llu\n", (unsigned)s.mLobbies, (unsigned)s.mPipes,
					(unsigned long long)s.mForwarded, (unsigned long long)s.mDroppedInvalid, (unsigned long long)s.mDroppedRate, (unsigned long long)s.mDroppedBlocked, (unsigned long long)s.mWrongKeys);
				fflush(stdout);
			}
		}
		if (runSeconds > 0.0 && now - startTime >= runSeconds)
			break;
	}

	core.update(nowSeconds());
	writeStats(statsFile, core.getStats(), nowSeconds() - startTime);
	closeSocket(sock);
#ifdef _WIN32
	WSACleanup();
#endif
	if (!quiet)
		printf("s3air_relay: stopped\n");
	return 0;
}
