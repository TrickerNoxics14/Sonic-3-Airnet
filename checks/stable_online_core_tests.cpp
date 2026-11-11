#include "oxygen/network/netplay/NetplayCodes.h"
#include "oxygen/network/netplay/NetplayModFingerprint.h"
#include "oxygen/network/netplay/NetplayAuth.h"
#include "oxygen/network/netplay/NetplayFailure.h"
#include "oxygen/network/netplay/NetplayPackets.h"
#include "oxygen_netcore/network/Sockets.h"
#include "oxygen_netcore/network/ConnectionListener.h"
#include "oxygen_netcore/network/ConnectionManager.h"
#include "oxygen_netcore/network/NetConnection.h"
#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen_netcore/serverclient/ProtocolVersion.h"

#include <algorithm>
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

namespace
{
	// A trivial packet, to get a connection going between two connection managers on this computer
	struct TestPacket : public highlevel::PacketBase
	{
		HIGHLEVEL_PACKET_DEFINE_PACKET_TYPE("CoreTestPacket");
		uint32 mValue = 0;
		virtual void serializeContent(VectorBinarySerializer& serializer, uint8 protocolVersion) override  { serializer.serialize(mValue); }
	};

	struct TestListener : public ConnectionListenerInterface
	{
		std::vector<NetConnection*> mCreated;
		int mReceivedTestPackets = 0;

		virtual NetConnection* createNetConnection(ConnectionManager&, const SocketAddress&) override
		{
			mCreated.push_back(new NetConnection());
			return mCreated.back();
		}
		virtual void destroyNetConnection(NetConnection& connection) override
		{
			mCreated.erase(std::remove(mCreated.begin(), mCreated.end(), &connection), mCreated.end());
			delete &connection;
		}
		virtual bool onReceivedPacket(ReceivedPacketEvaluation& evaluation) override
		{
			if (evaluation.mPacketType != TestPacket::PACKET_TYPE)
				return false;
			TestPacket packet;
			if (evaluation.readPacket(packet))
				++mReceivedTestPackets;
			return true;
		}
		void deleteAll()
		{
			for (NetConnection* connection : mCreated)
				delete connection;
			mCreated.clear();
		}
	};

	void pump(ConnectionManager& a, ConnectionManager& b, int milliseconds)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		while (std::chrono::steady_clock::now() < deadline)
		{
			a.updateConnectionManager();
			b.updateConnectionManager();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	std::vector<uint8> buildErrorPacket(uint16 claimedSenderID, uint16 receiverConnectionID)
	{
		std::vector<uint8> bytes;
		VectorBinarySerializer serializer(false, bytes);
		serializer.write<uint16>(lowlevel::ErrorPacket::SIGNATURE);
		serializer.write<uint16>(claimedSenderID);
		serializer.write<uint16>(receiverConnectionID);
		serializer.write<uint8>((uint8)lowlevel::ErrorPacket::ErrorCode::CONNECTION_INVALID);
		return bytes;
	}

	bool bindFreePort(UDPSocket& socket, uint16& outPort, uint16 first)
	{
		for (uint16 candidate = first; candidate < first + 100; ++candidate)
		{
			if (socket.bindToPort(candidate))
			{
				outPort = candidate;
				return true;
			}
		}
		return false;
	}

	void runConnectionTests()
	{
		UDPSocket hostSocket, clientSocket, attackerSocket;
		uint16 hostPort = 0, clientPort = 0, attackerPort = 0;
		if (!bindFreePort(hostSocket, hostPort, 40200) || !bindFreePort(clientSocket, clientPort, 40300) || !bindFreePort(attackerSocket, attackerPort, 40400))
		{
			report("connection tests could bind their sockets", false);
			return;
		}

		TestListener hostListener, clientListener;
		ConnectionManager hostManager(&hostSocket, nullptr, hostListener, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE);
		ConnectionManager clientManager(&clientSocket, nullptr, clientListener, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE);
		NetConnection clientConnection;

		clientConnection.startConnectTo(clientManager, SocketAddress("127.0.0.1", hostPort));
		pump(hostManager, clientManager, 400);
		TestPacket hello;
		clientConnection.sendPacket(hello);
		pump(hostManager, clientManager, 300);
		const bool connected = (clientConnection.getState() == NetConnection::State::CONNECTED) && (hostListener.mCreated.size() == 1) && (hostListener.mCreated[0]->getState() == NetConnection::State::CONNECTED);
		report("two connection managers connect over the loopback and exchange a packet", connected && hostListener.mReceivedTestPackets == 1);
		if (!connected)
		{
			clientConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			hostManager.terminateAllConnections();
			hostListener.deleteAll();
			return;
		}

		// F3: an error packet that claims to end the connection, sent by somebody else than the peer, must not disconnect it
		const std::vector<uint8> spoofed = buildErrorPacket((uint16)(clientConnection.getRemoteConnectionID() ^ 0x5a5a), clientConnection.getLocalConnectionID());
		attackerSocket.sendData(spoofed, SocketAddress("127.0.0.1", clientPort));
		pump(hostManager, clientManager, 300);
		report("a connection ignores an end-of-connection packet from an address that is not the peer's", clientConnection.getState() == NetConnection::State::CONNECTED);

		// F6: junk LAN discovery packets in front of a real packet must not hold the real packet back
		{
			std::vector<uint8> junk(17, 0);
			memcpy(&junk[0], "S3AIRFIND", 9);
			for (int i = 0; i < 12; ++i)
				attackerSocket.sendData(junk, SocketAddress("127.0.0.1", hostPort));
			TestPacket second;
			second.mValue = 2;
			clientConnection.sendPacket(second);
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			const int before = hostListener.mReceivedTestPackets;
			hostManager.updateConnectionManager();		// One single update
			report("junk LAN discovery packets do not hold back a real packet in the same update", hostListener.mReceivedTestPackets == before + 1);
		}

		// Control: the same kind of packet from the real peer's address does end the connection (so the check above can fail)
		{
			const std::vector<uint8> real = buildErrorPacket((uint16)(clientConnection.getRemoteConnectionID() ^ 0x5a5a), clientConnection.getLocalConnectionID());
			hostSocket.sendData(real, SocketAddress("127.0.0.1", clientPort));
			pump(hostManager, clientManager, 300);
			report("control: the same packet from the peer's own address ends the connection", clientConnection.getState() == NetConnection::State::DISCONNECTED);
		}

		clientConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
		hostManager.terminateAllConnections();
		hostListener.deleteAll();
		clientListener.deleteAll();
	}

	void runAuthTests()
	{
		using namespace NetplayAuth;
		const uint32 secret = secretFromSession(0x123456789aull);
		const uint32 otherSecret = secretFromSession(0x123456789bull);
		const uint32 hostNonce = 0x11112222u;
		const uint32 clientNonce = 0x33334444u;

		report("auth: the right secret is accepted", verifyFriendProof(secret, hostNonce, clientNonce, computeProof('C', secret, hostNonce, clientNonce)));
		report("auth: a wrong secret is refused", !verifyFriendProof(secret, hostNonce, clientNonce, computeProof('C', otherSecret, hostNonce, clientNonce)));
		report("auth: a recorded proof does not work with a new host challenge (replay)", !verifyFriendProof(secret, hostNonce + 1, clientNonce, computeProof('C', secret, hostNonce, clientNonce)));
		report("auth: the friend's proof cannot be played back as the host's proof", !verifyHostProof(secret, hostNonce, clientNonce, computeProof('C', secret, hostNonce, clientNonce)));
		report("auth: a host that does not know the code cannot prove it", !verifyHostProof(secret, hostNonce, clientNonce, computeProof('H', otherSecret, hostNonce, clientNonce)) && verifyHostProof(secret, hostNonce, clientNonce, computeProof('H', secret, hostNonce, clientNonce)));
		report("auth: the secret derived from a session code is stable and differs between sessions", secret == secretFromSession(0x123456789aull) && secret != otherSecret);
		{
			const uint64 session = 0x123456789aull;
			const uint64 tag = lanTagFromSession(session);
			report("auth: the LAN broadcast tag reveals neither the session ID nor the secret", tag != session && tag != (uint64)secretFromSession(session) && tag == lanTagFromSession(session) && tag != lanTagFromSession(session + 1));
		}
		{
			bool inRange = true, varies = false;
			uint32 first = makeRandomSecret();
			for (int i = 0; i < 64; ++i)
			{
				const uint32 s = makeRandomSecret();
				inRange &= (s != 0 && s <= 0x3fffffffu);
				varies |= (s != first);
			}
			report("auth: random join code secrets are 30 bit, never zero, and not repeated", inRange && varies);
		}
		{
			// The proofs really depend on every input
			const uint64 base = computeProof('C', secret, hostNonce, clientNonce);
			report("auth: the proof changes with the secret, both nonces and the role", base != computeProof('C', secret + 1, hostNonce, clientNonce) && base != computeProof('C', secret, hostNonce + 1, clientNonce) && base != computeProof('C', secret, hostNonce, clientNonce + 1) && base != computeProof('H', secret, hostNonce, clientNonce));
		}
		{
			// A code with the wrong secret yields a different proof than the host expects (join code level)
			const std::string code = NetplayCodes::makeJoinCode("192.168.0.5", 28840, 0x1234567u);
			const std::string wrongCode = NetplayCodes::makeJoinCode("192.168.0.5", 28840, 0x1234568u);
			std::string ip, wrongIp;
			uint16 port = 0, wrongPort = 0;
			uint32 readSecret = 0, readWrongSecret = 0;
			const bool ok = NetplayCodes::readJoinCode(code.c_str(), ip, port, readSecret) && NetplayCodes::readJoinCode(wrongCode.c_str(), wrongIp, wrongPort, readWrongSecret);
			report("auth: a join code carries its secret, and a code for the same host with another secret does not pass", ok && readSecret == 0x1234567u && !verifyFriendProof(readSecret, hostNonce, clientNonce, computeProof('C', readWrongSecret, hostNonce, clientNonce)));
		}
	}

	void runBroadcastTests()
	{
		// F20: the discovery request also goes out as a subnet broadcast of each network adapter (VPN adapters are missed by the limited broadcast)
		const std::vector<std::string> addresses = UDPSocket::getSubnetBroadcastAddresses();
		bool valid = true;
		for (const std::string& address : addresses)
		{
			uint32 value = 0;
			valid &= NetplayCodes::parseIPv4(address, value) && (value & 0xff) != 0 && (address.compare(0, 4, "127.") != 0);
		}
		const std::string name = "LAN discovery: subnet broadcast addresses of the adapters are valid (" + std::to_string(addresses.size()) + " found on this computer)";
		report(name.c_str(), valid);
	}

	void runFailureTests()
	{
		using namespace NetplayFailure;
		bool allDescribed = true;
		std::string seen;
		for (int i = 1; i < (int)Reason::COUNT; ++i)
		{
			const Reason reason = (Reason)i;
			const std::string message = getMessage(reason);
			const std::string name = getName(reason);
			allDescribed &= !message.empty() && name != "NONE" && isRetryable(reason) && (seen.find("|" + name + "|") == std::string::npos) && (seen.find("|" + message + "|") == std::string::npos);
			seen += "|" + name + "|" + message + "|";
		}
		report("failure reasons: every reason has its own name and plain-language message, and offers a retry", allDescribed && std::string(getMessage(Reason::NONE)).empty() && !isRetryable(Reason::NONE));
	}

	void runPacketLimitTests()
	{
		{
			// F4: a packet that announces a gigantic blob is refused before anything gets allocated
			StartGamePacket good;
			std::vector<uint8> bytes;
			{
				VectorBinarySerializer serializer(false, bytes);
				good.serializePacket(serializer, 1);
			}
			// The last four bytes are the length of the mod info blob (empty): announce four gigabytes instead
			bool refused = false;
			size_t capacity = 0;
			if (bytes.size() >= 4)
			{
				bytes[bytes.size() - 4] = 0xf0; bytes[bytes.size() - 3] = 0xff; bytes[bytes.size() - 2] = 0xff; bytes[bytes.size() - 1] = 0xff;
				StartGamePacket bad;
				VectorBinarySerializer serializer(true, bytes);
				refused = !bad.serializePacket(serializer, 1);
				capacity = bad.mActiveModsInfo.capacity();
			}
			report("packets: a start game packet with an absurd blob size is refused without allocating it", refused && capacity < 0x100000);
		}
		{
			// F4: a zlib stream that expands a thousandfold stops at the announced size
			std::vector<uint8> zeros(64 * 1024 * 1024, 0), packed;
			const bool encoded = ZlibDeflate::encode(packed, zeros.data(), zeros.size(), 6);
			std::vector<uint8> out;
			const bool bombRefused = encoded && !ZlibDeflate::decode(out, packed.data(), packed.size(), 4096);
			report("packets: decompression stops at the announced size (zip bomb)", bombRefused && out.size() < 0x20000);
			std::vector<uint8> small(5000, 7), smallPacked, back;
			const bool roundTrip = ZlibDeflate::encode(smallPacked, small.data(), small.size(), 6) && ZlibDeflate::decode(back, smallPacked.data(), smallPacked.size(), small.size()) && back == small;
			report("packets: decompression with the right announced size still works", roundTrip);
		}
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
			const uint64 sessionID = NetplayAuth::lanTagFromSession(0x123456789aull);
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
	runConnectionTests();
	Sockets::shutdownSockets();
	runAuthTests();
	runFailureTests();
	runBroadcastTests();
	runPacketLimitTests();
	report("test hooks are off by default (no marker file next to this test program)", !TestHooks::isEnabled() && TestHooks::getEnv("PATH") == nullptr);
	report("LAN discovery finds a host with the matching session code", lanFound);
	report("LAN discovery rejects a different session code", lanMismatchRejected);

	std::cout << (failures == 0 ? "ONLINE CORE TESTS PASS" : "ONLINE CORE TESTS FAIL") << " (" << failures << " failures)\n";
	return failures == 0 ? 0 : 1;
}
