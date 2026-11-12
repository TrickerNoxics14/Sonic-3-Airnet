#include "oxygen/network/netplay/NetplayCodes.h"
#include "oxygen/network/netplay/NetplayModFingerprint.h"
#include "oxygen/network/netplay/NetplayAuth.h"
#include "oxygen/network/netplay/NetplayFailure.h"
#include "oxygen/network/netplay/NetplayLobbyLogic.h"
#include "oxygen/network/netplay/NetplayPackets.h"
#include "oxygen_netcore/network/Sockets.h"
#include "oxygen_netcore/network/ConnectionListener.h"
#include "oxygen_netcore/network/ConnectionManager.h"
#include "oxygen_netcore/network/NetConnection.h"
#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen_netcore/serverclient/ProtocolVersion.h"
#include "oxygen_netcore/network/RelayProtocol.h"
#include "RelayCore.h"
#include "rmxbase.h"
#include "sonic3air/client/SelfUpdateCore.h"

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
		const uint64 secret = secretFromSession(0x123456789aull);
		const uint64 otherSecret = secretFromSession(0x123456789bull);
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
			report("auth: the LAN broadcast tag reveals neither the session ID nor the secret", tag != session && tag != secretFromSession(session) && tag == lanTagFromSession(session) && tag != lanTagFromSession(session + 1));
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
			// Review finding: the tag, the relay lobby name and the proof were single hashes of a 40 bit / 30 bit secret, so every guess cost the attacker one hash
			const uint64 session = 0x0123456789ull;
			const auto start = std::chrono::steady_clock::now();
			const uint64 firstTag = lanTagFromSession(session);		// the slow part runs here
			const auto middle = std::chrono::steady_clock::now();
			const uint64 secondTag = lanTagFromSession(session);
			const uint64 lobby = relayLobbyFromSession(session), key = relayKeyFromSession(session), derivedSecret = secretFromSession(session);
			const auto end = std::chrono::steady_clock::now();
			const double firstMs = std::chrono::duration<double, std::milli>(middle - start).count();
			const double repeatMs = std::chrono::duration<double, std::milli>(end - middle).count();
			std::cout << "  (info: first derivation from a session ID " << firstMs << " ms, the next four " << repeatMs << " ms, " << STRETCH_ITERATIONS << " chained hashes)\n";
			report("auth: values derived from a code go through key stretching (at least 100000 chained hashes per guess), computed once and then cached", STRETCH_ITERATIONS >= 100000 && firstTag == secondTag && firstMs > 5.0 * repeatMs && repeatMs < 50.0);
			report("auth: tag, relay lobby, relay key and secret of one session are four different values", firstTag != lobby && firstTag != key && firstTag != derivedSecret && lobby != key && lobby != derivedSecret && key != derivedSecret);
			// The old single-hash derivation (version 1) must no longer match: an attacker's table of single hashes is useless
			report("auth: a plain single SHA-1 of the session ID no longer gives the LAN tag", firstTag != detail::hashToUint64("S3AIRLANTAG1|" + std::to_string(session)) && firstTag != detail::hashToUint64("S3AIRLANTAG2|" + std::to_string(session)));
			const uint64 directSecret = 0x2aaaaaaaull;
			const auto proofStart = std::chrono::steady_clock::now();
			const uint64 proofA = computeProof('C', directSecret, 1, 2);
			const double proofFirstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - proofStart).count();
			const auto proofStart2 = std::chrono::steady_clock::now();
			const uint64 proofB = computeProof('C', directSecret, 1, 2);
			const double proofRepeatMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - proofStart2).count();
			report("auth: a proof for a join code secret also needs the slow stretching once, and a repeated proof (every handshake after the first) is cheap", proofA == proofB && proofFirstMs > 5.0 * proofRepeatMs && proofRepeatMs < 20.0);
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
			allDescribed &= !message.empty() && name != "NONE" && (isRetryable(reason) == (reason != Reason::KICKED)) && (seen.find("|" + name + "|") == std::string::npos) && (seen.find("|" + message + "|") == std::string::npos);
			seen += "|" + name + "|" + message + "|";
		}
		report("failure reasons: every reason has its own name and plain-language message, and offers a retry (except being kicked: the host removed you)", allDescribed && std::string(getMessage(Reason::NONE)).empty() && !isRetryable(Reason::NONE));
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

	void runLobbyLogicTests()
	{
		using namespace NetplayLobby;

		// Names
		report("lobby: an empty name becomes the default Player (not a user name)", sanitizeName("") == "Player" && sanitizeName("   ") == "Player" && sanitizeName("<>|&%$") == "Player");
		report("lobby: a plain name stays as typed", sanitizeName("Tails fan 7") == "Tails fan 7");
		report("lobby: names are cut to 12 characters", sanitizeName("ABCDEFGHIJKLMNOPQRST") == "ABCDEFGHIJKL" && sanitizeName("ABCDEFGHIJKLMNOPQRST").size() == MAX_NAME_LENGTH);
		report("lobby: control characters, markup, newlines and non-ASCII are removed from names", sanitizeName("A\nB\tC\x01<b>D|E\xC3\xA9") == "ABCbDE");
		report("lobby: spaces are tidied (no leading, trailing or repeated spaces)", sanitizeName("   a    b   ") == "a b");
		report("lobby: a very long input with a huge tail does not blow up", sanitizeName(std::string(100000, 'x')).size() == MAX_NAME_LENGTH);

		// Chat
		report("lobby: a chat line is cut to 40 characters and cleaned", sanitizeChat(std::string(100, 'a')).size() == MAX_CHAT_LENGTH && sanitizeChat("hi\r\nthere\x07") == "hithere");
		report("lobby: an empty chat line stays empty (it is not sent)", sanitizeChat("").empty() && sanitizeChat("<<<>>>").empty() && buildChatText(0, "").empty());
		bool quickOk = true;
		std::string seenQuick;
		for (int id = 1; id <= NUM_QUICK_MESSAGES; ++id)
		{
			const char* text = getQuickMessage(id);
			quickOk &= (text != nullptr) && sanitizeChat(text) == text && seenQuick.find(std::string("|") + text + "|") == std::string::npos;
			seenQuick += std::string("|") + (text ? text : "") + "|";
		}
		report("lobby: the 12 quick messages exist, are different, and are clean text", quickOk && getQuickMessage(0) == nullptr && getQuickMessage(NUM_QUICK_MESSAGES + 1) == nullptr);
		report("lobby: a quick message wins over any free text that comes with it", buildChatText(3, "something else") == getQuickMessage(3) && buildChatText(0, "free text") == "free text");
		{
			ChatRateLimiter limiter;
			int accepted = 0;
			for (int k = 0; k < 6; ++k)
				accepted += limiter.allow(1000 + k * 100) ? 1 : 0;
			const bool laterAccepted = limiter.allow(1000 + 5 * 100 + CHAT_MIN_INTERVAL_MS + 1);
			report("lobby: chat is rate limited (6 lines in half a second: 1 passes, 5 are dropped), and works again later", accepted == 1 && limiter.mDropped == 5 && laterAccepted);
		}

		// Ping
		{
			PingTracker ping;
			const bool unknownFirst = (ping.get() == -1);
			ping.addSample(100);
			const bool first = (ping.get() == 100);
			for (int k = 0; k < 40; ++k)
				ping.addSample(200);
			const bool converges = (ping.get() >= 199 && ping.get() <= 200);
			ping.addSample(20000);	// a wild value is capped
			report("lobby: ping is unknown until measured, then smoothed and capped", unknownFirst && first && converges && ping.get() <= 10000);
			report("lobby: only a ping above 150 ms is a warning", !isHighPing(150) && isHighPing(151) && !isHighPing(-1) && !isHighPing(0));
		}

		// Input delay
		{
			bool monotonic = true;
			int last = 0;
			for (int ping = 0; ping <= 600; ping += 5)
			{
				const int frames = autoInputDelay(ping);
				monotonic &= (frames >= last) && (frames <= MAX_INPUT_DELAY);
				last = frames;
			}
			report("lobby: Auto input delay grows with the ping, from 0 to at most 6 frames", monotonic && autoInputDelay(-1) == 0 && autoInputDelay(10) == 0 && autoInputDelay(22) == 0 && autoInputDelay(36) == 0 && autoInputDelay(40) == 1 && autoInputDelay(60) == 1 && autoInputDelay(100) == 2 && autoInputDelay(200) == 5 && autoInputDelay(250) == 6 && autoInputDelay(5000) == 6);
			report("lobby: a fixed input delay ignores the ping, Auto follows it", effectiveInputDelay(2, 400) == 2 && effectiveInputDelay(0, 400) == 0 && effectiveInputDelay(-1, 100) == 2 && effectiveInputDelay(99, 0) == 6);
			report("lobby: the input delay is described in words", describeInputDelay(-1, 3) == "AUTO (3 frames)" && describeInputDelay(-1, 0) == "AUTO (0 frames)" && describeInputDelay(-1, 1) == "AUTO (1 frame)" && describeInputDelay(2, 2) == "2 frames" && describeInputDelay(0, 0) == "OFF");
			int setting = INPUT_DELAY_AUTO;
			std::string visited;
			for (int k = 0; k < MAX_INPUT_DELAY + 2; ++k)
			{
				visited += std::to_string(setting) + ",";
				setting = nextDelaySetting(setting, 1);
			}
			report("lobby: cycling the input delay setting visits Auto and 0 to 6 once and comes back", visited == "-1,0,1,2,3,4,5,6," && setting == INPUT_DELAY_AUTO && nextDelaySetting(INPUT_DELAY_AUTO, -1) == MAX_INPUT_DELAY);
		}
		{
			// The delay line: every player's input comes out exactly N frames later, all players alike
			bool exact = true;
			for (int delay = 0; delay <= MAX_INPUT_DELAY; ++delay)
			{
				InputDelayLine line;
				line.setDelay(delay);
				std::vector<InputDelayLine::Frame> inputs;
				for (int n = 0; n < 40; ++n)
				{
					InputDelayLine::Frame in = { (uint16)(100 + n), (uint16)(200 + n), (uint16)(300 + n), (uint16)(400 + n) };
					inputs.push_back(in);
					const InputDelayLine::Frame out = line.process(in);
					const InputDelayLine::Frame& expected = inputs[std::max(0, n - delay)];
					exact &= (out == expected);
				}
			}
			report("lobby: the input delay line delays all four players by exactly N frames (the first input is repeated at the start)", exact);

			InputDelayLine line;
			line.setDelay(3);
			InputDelayLine::Frame out = {};
			for (int n = 0; n < 10; ++n)
			{
				InputDelayLine::Frame in = { (uint16)n, 0, 0, 0 };
				out = line.process(in);
			}
			line.setDelay(1);		// Lowering it skips frames, once
			InputDelayLine::Frame in10 = { 10, 0, 0, 0 };
			const InputDelayLine::Frame afterLower = line.process(in10);
			line.setDelay(4);		// Raising it repeats frames, once
			InputDelayLine::Frame in11 = { 11, 0, 0, 0 };
			const InputDelayLine::Frame afterRaise = line.process(in11);
			report("lobby: changing the delay while playing only skips or repeats a frame (nothing is invented)", out[0] == 6 && afterLower[0] == 9 && afterRaise[0] == 9);
		}

		// Reconnecting
		{
			SlotReservations reservations;
			reservations.add(2, "10.0.0.5", "Knux");
			const bool reserved = reservations.isReservedForOther(2, "10.0.0.9") && !reservations.isReservedForOther(2, "10.0.0.5") && !reservations.isReservedForOther(1, "10.0.0.9");
			const bool found = reservations.findSlotFor("10.0.0.5", 0) == 2 && reservations.findSlotFor("10.0.0.9", 0) == 0;
			reservations.add(3, "10.0.0.5", "Knux2");
			const bool preferred = reservations.findSlotFor("10.0.0.5", 3) == 3 && reservations.findSlotFor("10.0.0.5", 2) == 2;
			reservations.update(SLOT_RESERVATION_SECONDS - 1.0f);
			const bool stillThere = reservations.isReservedForOther(2, "10.0.0.9");
			reservations.update(2.0f);
			const bool expired = !reservations.isReservedForOther(2, "10.0.0.9") && reservations.getItems().empty();
			reservations.add(1, "a", "x");
			reservations.remove(1);
			report("lobby: a dropped friend's slot is kept for its own address for a minute, then free again", reserved && found && preferred && stillThere && expired && reservations.getItems().empty());
		}
		{
			KickBlockList blocked;
			blocked.add("1.2.3.4");
			const bool now = blocked.isBlocked("1.2.3.4") && !blocked.isBlocked("1.2.3.5");
			blocked.update(KICK_BLOCK_SECONDS - 1.0f);
			const bool almost = blocked.isBlocked("1.2.3.4");
			blocked.update(2.0f);
			report("lobby: a kicked address cannot come back for a minute, other addresses are not affected", now && almost && !blocked.isBlocked("1.2.3.4"));
		}
		{
			// Review findings: wrong-code counter that never decayed, connection table that strangers could fill, LAN requester list that never made room
			AuthFailureTracker failures;
			bool blockedAfterEight = false;
			for (int i = 0; i < MAX_AUTH_FAILURES; ++i)
				failures.recordFailure("9.9.9.9");
			blockedAfterEight = failures.isBlocked("9.9.9.9") && !failures.isBlocked("9.9.9.8");
			failures.update(AUTH_FAILURE_WINDOW_SECONDS - 1.0f);
			const bool stillBlocked = failures.isBlocked("9.9.9.9");
			failures.update(2.0f);
			report("lobby: an address that guessed wrong codes is ignored only for a minute, then it may try again (the counter decays)", blockedAfterEight && stillBlocked && !failures.isBlocked("9.9.9.9") && failures.size() == 0);

			AuthFailureTracker some;
			some.recordFailure("1.1.1.1"); some.recordFailure("1.1.1.1");
			some.recordSuccess("1.1.1.1");
			report("lobby: a friend who proves the code after a typo starts over", some.size() == 0);

			AuthFailureTracker many;
			for (size_t i = 0; i < MAX_AUTH_FAILURE_ENTRIES * 3; ++i)
				many.recordFailure("10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256));
			report("lobby: the list of wrong-code addresses stays bounded", many.size() <= MAX_AUTH_FAILURE_ENTRIES);

			std::vector<ConnectionInfo> table;
			auto add = [&table](const std::string& ip, bool authenticated, float seconds) { ConnectionInfo info; info.mIP = ip; info.mAuthenticated = authenticated; info.mPendingSeconds = seconds; table.push_back(info); };
			report("lobby: a free connection table accepts a newcomer", chooseConnectionToReplace(table, "5.5.5.5", 8) == ACCEPT_NEW_CONNECTION);
			add("6.6.6.6", false, 1.0f); add("6.6.6.6", false, 4.0f);
			const int victim = chooseConnectionToReplace(table, "6.6.6.6", 8);
			report("lobby: one address cannot hold more than two unproven connections (its oldest one gives way)", victim == 1 && chooseConnectionToReplace(table, "5.5.5.5", 8) == ACCEPT_NEW_CONNECTION);
			table.clear();
			for (int i = 0; i < 8; ++i)
				add("7.7.7." + std::to_string(i), false, 0.5f + (float)i * 0.1f);
			const int refused = chooseConnectionToReplace(table, "5.5.5.5", 8);
			table[3].mPendingSeconds = 9.0f;
			const int replaced = chooseConnectionToReplace(table, "5.5.5.5", 8);
			report("lobby: a full table of fresh strangers refuses, but once one of them had its chance a real friend replaces the oldest", refused == REFUSE_NEW_CONNECTION && replaced == 3);
			table.clear();
			for (int i = 0; i < 8; ++i)
				add("8.8.8." + std::to_string(i), true, 20.0f);
			report("lobby: connections that proved the code are never replaced by a newcomer", chooseConnectionToReplace(table, "5.5.5.5", 8) == REFUSE_NEW_CONNECTION);

			std::vector<std::string> recent;
			for (int i = 0; i < 16; ++i)
				UDPSocket::rememberRecentAddress(recent, "192.168.0." + std::to_string(i), 16);
			UDPSocket::rememberRecentAddress(recent, "192.168.0.99", 16);
			const bool oldestGone = std::find(recent.begin(), recent.end(), "192.168.0.0") == recent.end();
			UDPSocket::rememberRecentAddress(recent, "192.168.0.5", 16);
			report("lobby: the LAN requester list makes room for a new friend by forgetting the oldest entry, and does not grow",
				recent.size() == 16 && oldestGone && std::find(recent.begin(), recent.end(), "192.168.0.99") != recent.end() && recent.back() == "192.168.0.5");
		}

		// Packets
		{
			LobbyStatePacket packet;
			packet.mYourSlot = 2;
			packet.mSlotState[0] = 1; packet.mSlotState[1] = 3; packet.mSlotState[2] = 2;
			packet.mNames[0] = "Hosty"; packet.mNames[1] = "Friend One"; packet.mNames[2] = "Knux";
			packet.mPingMs[1] = 212; packet.mPingMs[2] = 40;
			packet.mDelaySetting = 0xff; packet.mEffectiveDelay = 3; packet.mReadyCheckSeconds = 17;
			std::vector<uint8> bytes;
			{
				VectorBinarySerializer serializer(false, bytes);
				packet.serializePacket(serializer, 1);
			}
			LobbyStatePacket back;
			VectorBinarySerializer reader(true, bytes);
			const bool ok = back.serializePacket(reader, 1);
			report("lobby: the lobby state packet carries names, pings, input delay and ready check", ok && back.mNames[0] == "Hosty" && back.mNames[1] == "Friend One" && back.mPingMs[1] == 212 && back.mPingMs[3] == 0xffff && back.mEffectiveDelay == 3 && back.mDelaySetting == 0xff && back.mReadyCheckSeconds == 17 && back.mYourSlot == 2);
		}
		{
			// An absurdly long name or chat text on the wire is refused, not allocated
			LobbyChatPacket tooLong;
			tooLong.mQuickMessage = 0;
			tooLong.mText = std::string(1000, 'a');
			std::vector<uint8> bytes;
			{
				VectorBinarySerializer serializer(false, bytes);
				tooLong.serializePacket(serializer, 1);
			}
			LobbyChatPacket back;
			VectorBinarySerializer reader(true, bytes);
			const bool refused = !back.serializePacket(reader, 1) || back.mText.size() <= LobbyChatPacket::MAX_TEXT_BYTES;
			LobbyReadyPacket readyPacket;
			readyPacket.mName = std::string(5000, 'b');
			std::vector<uint8> readyBytes;
			{
				VectorBinarySerializer serializer(false, readyBytes);
				readyPacket.serializePacket(serializer, 1);
			}
			LobbyReadyPacket readyBack;
			VectorBinarySerializer readyReader(true, readyBytes);
			const bool readyRefused = !readyBack.serializePacket(readyReader, 1) || readyBack.mName.size() <= LobbyStatePacket::MAX_NAME_BYTES;
			report("lobby: over-long names and chat lines on the wire are refused", refused && readyRefused);
		}
	}

}


// ---------------------------------------------------------------------------------------------------------------
// Character choice in the lobby: the rules of what each player may pick, the host's checks, the packets, and the picks that are fixed for a session
namespace
{
	std::vector<uint8> writePacket(highlevel::PacketBase& packet)
	{
		std::vector<uint8> bytes;
		VectorBinarySerializer serializer(false, bytes);
		packet.serializePacket(serializer, 1);
		return bytes;
	}

	bool readPacketBytes(highlevel::PacketBase& packet, const std::vector<uint8>& bytes)
	{
		VectorBinarySerializer reader(true, bytes);
		return packet.serializePacket(reader, 1);
	}

	void runCharacterTests()
	{
		using namespace NetplayLobby;

		CharacterList stockOnly;
		CharacterList withExtras;
		withExtras.setExtraSlots({ 90, 35, 201, 35, 0, 256, -4 });		// unsorted, a duplicate, and numbers that are no slots

		// The list of installed characters
		{
			const std::vector<int> expected = { 35, 90, 201 };
			report("characters: extra slots are sorted, no duplicates, numbers outside 1 to 255 are dropped", withExtras.getExtraSlots() == expected && stockOnly.getExtraSlots().empty());
			std::vector<int> many;
			for (int k = 1; k <= 200; ++k)
				many.push_back(k);
			CharacterList big;
			big.setExtraSlots(many);
			report("characters: the list of extra characters stays small (at most 24)", (int)big.getExtraSlots().size() == MAX_EXTRA_CHARACTERS && big.getExtraSlots().front() == 1);
			report("characters: names are plain text, known extra slots have their own name, the others are called after the slot",
				getCharacterName(CHARACTER_SONIC) == "SONIC" && getCharacterName(CHARACTER_TAILS) == "TAILS" && getCharacterName(CHARACTER_KNUCKLES) == "KNUCKLES" &&
				getCharacterName(makeExtraCharacter(35)) == "MODERN SONIC" && getCharacterName(makeExtraCharacter(90)) == "RETO" && getCharacterName(makeExtraCharacter(201)) == "EXTRA 201" && getCharacterName(0x7777) == "?");
		}

		// What each player may pick: with no extra-slot mod, only the three stock characters can be picked
		{
			bool onlyStock = true;
			for (int slot = 0; slot < 4; ++slot)
				for (const uint16 id : getChoicesForSlot(slot, stockOnly))
					onlyStock = onlyStock && (id == CHARACTER_SONIC || id == CHARACTER_TAILS || id == CHARACTER_KNUCKLES || id == CHARACTER_MENU);
			report("characters: without an extra-slot mod nothing but Sonic, Tails and Knuckles (and the host menu choice) is offered", onlyStock);
			report("characters: the defaults are the old roles (host: the game menu, player 2 Tails, player 3 Knuckles, player 4 Sonic)",
				defaultCharacterForSlot(0, stockOnly) == CHARACTER_MENU && defaultCharacterForSlot(1, stockOnly) == CHARACTER_TAILS && defaultCharacterForSlot(2, stockOnly) == CHARACTER_KNUCKLES && defaultCharacterForSlot(3, stockOnly) == CHARACTER_SONIC);
			report("characters: Tails exists once (it is the second character of the game): only player 2 can be Tails, and only the host can say game menu",
				!isCharacterAllowedForSlot(0, CHARACTER_TAILS, stockOnly) && isCharacterAllowedForSlot(1, CHARACTER_TAILS, stockOnly) && !isCharacterAllowedForSlot(2, CHARACTER_TAILS, stockOnly) && !isCharacterAllowedForSlot(3, CHARACTER_TAILS, stockOnly) &&
				isCharacterAllowedForSlot(0, CHARACTER_MENU, stockOnly) && !isCharacterAllowedForSlot(1, CHARACTER_MENU, stockOnly) && !isCharacterAllowedForSlot(2, CHARACTER_MENU, stockOnly));
			report("characters: two players may pick the same character (Sonic and Sonic, Knuckles and Knuckles)",
				isCharacterAllowedForSlot(0, CHARACTER_SONIC, stockOnly) && isCharacterAllowedForSlot(1, CHARACTER_SONIC, stockOnly) && isCharacterAllowedForSlot(2, CHARACTER_SONIC, stockOnly) && isCharacterAllowedForSlot(3, CHARACTER_SONIC, stockOnly) &&
				isCharacterAllowedForSlot(2, CHARACTER_KNUCKLES, stockOnly) && isCharacterAllowedForSlot(3, CHARACTER_KNUCKLES, stockOnly));
		}

		// With an extra-slot mod: its characters are offered to the friends, but only if they are installed
		{
			report("characters: installed extra characters can be picked by the friends (the host picks one in the game menu)",
				isCharacterAllowedForSlot(1, makeExtraCharacter(201), withExtras) && isCharacterAllowedForSlot(2, makeExtraCharacter(35), withExtras) && isCharacterAllowedForSlot(3, makeExtraCharacter(90), withExtras) &&
				!isCharacterAllowedForSlot(0, makeExtraCharacter(201), withExtras));
			report("characters: an extra character that is not installed is refused, as are numbers that are no character at all",
				!isCharacterAllowedForSlot(2, makeExtraCharacter(202), withExtras) && !isCharacterAllowedForSlot(2, makeExtraCharacter(201), stockOnly) &&
				!isCharacterAllowedForSlot(2, 3, withExtras) && !isCharacterAllowedForSlot(2, 0x100, withExtras) && !isCharacterAllowedForSlot(2, 0x200, withExtras) && !isCharacterAllowedForSlot(2, CHARACTER_NONE, withExtras) && !isCharacterAllowedForSlot(2, 0x7fff, withExtras) &&
				!isCharacterAllowedForSlot(4, CHARACTER_SONIC, withExtras) && !isCharacterAllowedForSlot(-1, CHARACTER_SONIC, withExtras));
			report("characters: the setting of the mod for player 3 or 4 stays their default (nothing changes for somebody who set it up)",
				defaultCharacterForSlot(2, withExtras, 90) == makeExtraCharacter(90) && defaultCharacterForSlot(3, withExtras, 35) == makeExtraCharacter(35) &&
				defaultCharacterForSlot(2, stockOnly, 90) == CHARACTER_KNUCKLES && defaultCharacterForSlot(3, withExtras, 0) == CHARACTER_SONIC);
		}

		// The host checks of a request
		{
			bool adjusted = true;
			uint16 result = sanitizeCharacterPick(2, CHARACTER_SONIC, withExtras, CHARACTER_KNUCKLES, &adjusted);
			report("characters: a possible request is accepted as it is", result == CHARACTER_SONIC && !adjusted);
			result = sanitizeCharacterPick(2, CHARACTER_TAILS, withExtras, CHARACTER_SONIC, &adjusted);
			report("characters: an impossible request keeps what the player had, and says that it was changed", result == CHARACTER_SONIC && adjusted);
			result = sanitizeCharacterPick(2, 0xbeef, withExtras, 0x1234, &adjusted);
			report("characters: when even what the player had is not possible any more the default of the slot is used", result == CHARACTER_KNUCKLES && adjusted);
			result = sanitizeCharacterPick(3, makeExtraCharacter(77), stockOnly, makeExtraCharacter(77), &adjusted);
			report("characters: an extra character whose mod was switched off becomes the default of the slot", result == CHARACTER_SONIC && adjusted);
			bool all = true;
			for (uint32 value = 0; value <= 0xffff; value += 1)
			{
				for (int slot = 0; slot < 4; ++slot)
				{
					const uint16 got = sanitizeCharacterPick(slot, (uint16)value, withExtras, defaultCharacterForSlot(slot, withExtras));
					all = all && isCharacterAllowedForSlot(slot, got, withExtras);
				}
			}
			report("characters: whatever number a friend sends (all 65536 of them, every slot) the result is always a character that player can have", all);
		}

		// Cycling through the list in the lobby
		{
			const std::vector<uint16> choices = getChoicesForSlot(2, withExtras);
			uint16 current = defaultCharacterForSlot(2, withExtras);
			std::vector<uint16> visited;
			for (size_t k = 0; k < choices.size(); ++k)
			{
				visited.push_back(current);
				current = nextCharacterChoice(2, current, 1, withExtras);
			}
			std::vector<uint16> sortedVisited(visited), sortedChoices(choices);
			std::sort(sortedVisited.begin(), sortedVisited.end());
			std::sort(sortedChoices.begin(), sortedChoices.end());
			report("characters: cycling visits every choice once and comes back to the start", sortedVisited == sortedChoices && current == defaultCharacterForSlot(2, withExtras) && choices.size() == 2 + 3);
			report("characters: cycling backwards from the default goes to the last choice; an unknown pick starts at the default",
				nextCharacterChoice(2, CHARACTER_KNUCKLES, -1, withExtras) == makeExtraCharacter(201) && nextCharacterChoice(2, 0x7777, 1, withExtras) == choices[1] &&
				nextCharacterChoice(9, CHARACTER_SONIC, 1, withExtras) == CHARACTER_SONIC);
		}

		// Rate limit: cycling is fine, a flood is dropped, and it works again later
		{
			CharacterRateLimiter limiter;
			int accepted = 0;
			for (int k = 0; k < 100; ++k)
				accepted += limiter.allow(1000 + (uint64)k) ? 1 : 0;		// 100 requests in 100 ms
			const bool burstLimited = (accepted >= 7 && accepted <= 9) && limiter.mDropped > 80;
			const bool laterOk = limiter.allow(1000 + 100 + 2000);			// two seconds later
			CharacterRateLimiter normal;
			int normalAccepted = 0;
			for (int k = 0; k < 20; ++k)
				normalAccepted += normal.allow(5000 + (uint64)k * 300) ? 1 : 0;		// a player who presses a key three times a second is never dropped
			CharacterRateLimiter backwards;
			backwards.allow(50000);
			const bool clockBack = backwards.allow(10);						// a clock that goes backwards must not break it
			report("characters: requests are rate limited (a flood of 100 in 100 ms: about 8 pass), a normal player is never dropped, and it works again later", burstLimited && laterOk && normalAccepted == 20 && clockBack);
		}

		// Reconnecting: the slot keeps its pick for a minute
		{
			SlotReservations reservations;
			reservations.add(2, "tokenA", "Knux", makeExtraCharacter(90));
			reservations.add(3, "tokenB", "Sonic");
			const bool kept = reservations.findCharacterFor(2, "tokenA") == makeExtraCharacter(90) && reservations.findCharacterFor(2, "tokenB") == CHARACTER_NONE &&
							  reservations.findCharacterFor(3, "tokenB") == CHARACTER_NONE && reservations.findCharacterFor(1, "tokenA") == CHARACTER_NONE;
			reservations.update(SLOT_RESERVATION_SECONDS + 1.0f);
			report("characters: a friend whose connection broke keeps its pick for the reservation of the slot (a minute), nobody else gets it", kept && reservations.findCharacterFor(2, "tokenA") == CHARACTER_NONE);
		}

		// The session picks that are fixed at the start
		{
			SessionCharacters session;
			const bool defaultsOk = session.isValid(stockOnly);
			session.mPicks[1] = CHARACTER_SONIC;
			session.mPicks[2] = makeExtraCharacter(201);
			session.mPicks[3] = makeExtraCharacter(90);
			const bool validWithMods = session.isValid(withExtras);
			const bool invalidWithoutMods = !session.isValid(stockOnly);
			report("characters: the picks of a session are valid with the mods that were there, and not valid when a mod is gone", defaultsOk && validWithMods && invalidWithoutMods);
		}

		// A friend uses the host's picks exactly as sent, or refuses the start (finding: a pick replaced on one computer only makes the computers run other characters)
		{
			SessionCharacters session;
			session.mPicks[1] = CHARACTER_SONIC;
			session.mPicks[2] = makeExtraCharacter(90);
			session.mPicks[3] = CHARACTER_KNUCKLES;
			const int configured[4] = { 0, 0, 0, 0 };
			CharacterList gone;										// the same mods, but the list could not be read right now (script program reloading)
			const int usable = findUnusableSessionPick(session.mPicks, withExtras, configured);
			const int missingExtra = findUnusableSessionPick(session.mPicks, stockOnly, configured);
			const int missingEmpty = findUnusableSessionPick(session.mPicks, gone, configured);
			report("characters: a friend finds out whether it can use the host's picks exactly as sent; an extra character that it does not have is reported (slot 3), never replaced", usable == -1 && missingExtra == 2 && missingEmpty == 2);
			uint16 tailsForThree[4] = { CHARACTER_MENU, CHARACTER_TAILS, CHARACTER_TAILS, CHARACTER_SONIC };		// Tails exists once: slot 3 may not have it
			report("characters: picks that no slot may have (Tails for player 3) are reported too, so a friend refuses a start message that could not come from a real host", findUnusableSessionPick(tailsForThree, withExtras, configured) == 2 && findUnusableSessionPick(tailsForThree, withExtras, nullptr) == 2);
		}

		// A friend's new request is not undone by lobby states that are older than the host's answer
		{
			CharacterRequestTracker tracker;
			tracker.request(CHARACTER_SONIC);
			// an old state: the host still shows Knuckles and the notice bit from an earlier refusal is still set
			const bool oldStateTakenOver = tracker.onHostState(CHARACTER_KNUCKLES, true, false);
			tracker.update(1.0f);
			const bool stillOld = tracker.onHostState(CHARACTER_KNUCKLES, true, false);
			const bool accepted = tracker.onHostState(CHARACTER_SONIC, false, false);		// the host's answer: accepted
			const bool afterAccepted = tracker.onHostState(CHARACTER_SONIC, true, false);
			report("characters: an old lobby state (old pick, notice bit still set) does not undo a request that was just made; once the host accepted it, nothing is taken over", !oldStateTakenOver && !stillOld && !accepted && !afterAccepted);

			CharacterRequestTracker refused;
			refused.request(makeExtraCharacter(201));
			const bool early = refused.onHostState(CHARACTER_KNUCKLES, true, false);
			refused.update(CHARACTER_REQUEST_SETTLE_SECONDS + 0.1f);
			const bool late = refused.onHostState(CHARACTER_KNUCKLES, true, false);
			const bool noNotice = CharacterRequestTracker().onHostState(CHARACTER_KNUCKLES, false, false);
			report("characters: a request that the host really refuses is taken over once the host had time to answer (and a plain different pick without notice is not taken over)", !early && late && !noNotice);

			CharacterRequestTracker changedByHost;		// the host changed the pick of somebody who is not asking for anything (a mod went away): taken over at once
			const bool atOnce = changedByHost.onHostState(CHARACTER_KNUCKLES, true, false);
			CharacterRequestTracker fixedStart;
			fixedStart.request(CHARACTER_SONIC);
			const bool atStart = fixedStart.onHostState(CHARACTER_KNUCKLES, false, true);		// the game started: what is fixed counts, whatever was asked
			report("characters: a pick that the host changed by itself and the picks fixed at the start are taken over at once", atOnce && atStart);
		}

		// The host waits for the friends' first lobby message long enough
		{
			report("characters: the host waits for a silent friend's first lobby message for ten seconds (not three), never for one that has announced itself",
				mustWaitForAnnouncement(false, 0.0f) && mustWaitForAnnouncement(false, 3.5f) && mustWaitForAnnouncement(false, 9.9f) && !mustWaitForAnnouncement(false, 10.1f) && !mustWaitForAnnouncement(true, 0.0f));
		}

		// Packets
		{
			LobbyReadyPacket ready;
			ready.mReady = 1;
			ready.mName = "Knux";
			ready.mCharacter = makeExtraCharacter(201);
			const std::vector<uint8> bytes = writePacket(ready);
			LobbyReadyPacket back;
			const bool ok = readPacketBytes(back, bytes);
			report("characters: the ready message carries the character that the player wants", ok && back.mCharacter == makeExtraCharacter(201) && back.mReady == 1 && back.mName == "Knux");
			// A message without the character (an older version) reads as no request, not as an error and not as garbage
			std::vector<uint8> shortBytes(bytes.begin(), bytes.end() - 2);
			LobbyReadyPacket oldStyle;
			const bool oldOk = readPacketBytes(oldStyle, shortBytes);
			report("characters: a ready message without the character part is read as no request (nothing breaks)", oldOk && oldStyle.mCharacter == CHARACTER_NONE && oldStyle.mName == "Knux");
			// Truncated in the middle of the character: no crash, the result is a refusal or no request
			std::vector<uint8> oneByteTooShort(bytes.begin(), bytes.end() - 1);
			LobbyReadyPacket cut;
			const bool cutOk = readPacketBytes(cut, oneByteTooShort);
			report("characters: a ready message cut in the middle of the character is refused or ignored, never half read", !cutOk || cut.mCharacter == CHARACTER_NONE);
			std::vector<uint8> empty;
			LobbyReadyPacket nothing;
			report("characters: an empty ready message is refused without a crash", !readPacketBytes(nothing, empty));
			// Forged numbers in the two character bytes
			bool garbageSafe = true;
			for (int value = 0; value < 0x10000 && garbageSafe; value += 257)
			{
				std::vector<uint8> forged(bytes);
				forged[forged.size() - 2] = (uint8)(value & 0xff);
				forged[forged.size() - 1] = (uint8)(value >> 8);
				LobbyReadyPacket forgedPacket;
				garbageSafe = readPacketBytes(forgedPacket, forged) && forgedPacket.mCharacter == (uint16)value;
				const uint16 checked = sanitizeCharacterPick(2, forgedPacket.mCharacter, withExtras, CHARACTER_KNUCKLES);
				garbageSafe = garbageSafe && isCharacterAllowedForSlot(2, checked, withExtras);
			}
			report("characters: forged character numbers in the ready message are read, and then refused by the check of the host", garbageSafe);
		}
		{
			LobbyStatePacket state;
			state.mYourSlot = 2;
			state.mSlotState[0] = 1; state.mSlotState[1] = 3; state.mSlotState[2] = 2;
			state.mNames[0] = "Hosty"; state.mNames[1] = "One"; state.mNames[2] = "Two";
			state.mCharacters[0] = CHARACTER_MENU; state.mCharacters[1] = CHARACTER_TAILS; state.mCharacters[2] = makeExtraCharacter(201); state.mCharacters[3] = CHARACTER_SONIC;
			state.mCharacterNotice = 4;
			state.mCharactersLocked = 1;
			const std::vector<uint8> bytes = writePacket(state);
			LobbyStatePacket back;
			const bool ok = readPacketBytes(back, bytes);
			report("characters: the lobby state shows everybody's pick, the notice for a changed pick, and whether the picks are fixed",
				ok && back.mCharacters[0] == CHARACTER_MENU && back.mCharacters[1] == CHARACTER_TAILS && back.mCharacters[2] == makeExtraCharacter(201) && back.mCharacters[3] == CHARACTER_SONIC && back.mCharacterNotice == 4 && back.mCharactersLocked == 1 && back.mNames[1] == "One");
			// An older lobby state (without the picks) reads fine and leaves unknown
			std::vector<uint8> oldBytes(bytes.begin(), bytes.end() - 10);
			LobbyStatePacket oldBack;
			const bool oldOk = readPacketBytes(oldBack, oldBytes);
			report("characters: a lobby state without the picks is read as unknown picks (nothing breaks)", oldOk && oldBack.mCharacters[2] == CHARACTER_NONE && oldBack.mNames[2] == "Two" && oldBack.mCharactersLocked == 0);
			std::vector<uint8> cut(bytes.begin(), bytes.end() - 3);
			LobbyStatePacket cutBack;
			const bool cutOk = readPacketBytes(cutBack, cut);
			report("characters: a lobby state cut inside the picks is read as unknown picks (never half read)", cutOk && cutBack.mCharacters[2] == CHARACTER_NONE && cutBack.mCharactersLocked == 0);
		}
		{
			StartGamePacket start;
			start.mPlayerIndex = 2;
			start.mCharacterPicks[0] = CHARACTER_KNUCKLES;
			start.mCharacterPicks[1] = CHARACTER_SONIC;
			start.mCharacterPicks[2] = makeExtraCharacter(90);
			start.mCharacterPicks[3] = CHARACTER_SONIC;
			start.mActiveModsInfo = { 'a', 'b' };
			const std::vector<uint8> bytes = writePacket(start);
			StartGamePacket back;
			const bool ok = readPacketBytes(back, bytes);
			report("characters: the start message carries the fixed picks of the four players", ok && back.mCharacterPicks[0] == CHARACTER_KNUCKLES && back.mCharacterPicks[1] == CHARACTER_SONIC && back.mCharacterPicks[2] == makeExtraCharacter(90) && back.mCharacterPicks[3] == CHARACTER_SONIC && back.mPlayerIndex == 2 && back.mActiveModsInfo.size() == 2);
			std::vector<uint8> cut(bytes.begin(), bytes.begin() + 20);
			StartGamePacket cutBack;
			report("characters: a start message that is cut short is refused", !readPacketBytes(cutBack, cut));
			std::vector<uint8> otherVersion(bytes);
			otherVersion[0] = (uint8)(StartGamePacket::PACKET_VERSION - 1);
			StartGamePacket versionBack;
			report("characters: a start message of the older version is refused cleanly", !readPacketBytes(versionBack, otherVersion));
		}

		// Old and new versions: the join handshake tells the version of the lobby messages, a peer without it is refused as another version
		{
			JoinAuthPacket auth;
			auth.mClientNonce = 7; auth.mProof = 9; auth.mPreferredSlot = 2; auth.mClientToken = 77; auth.mLobbyProtocol = LOBBY_PROTOCOL;
			const std::vector<uint8> bytes = writePacket(auth);
			JoinAuthPacket back;
			const bool ok = readPacketBytes(back, bytes);
			std::vector<uint8> oldBytes(bytes.begin(), bytes.end() - 1);
			JoinAuthPacket oldBack;
			const bool oldOk = readPacketBytes(oldBack, oldBytes);
			report("compatibility: the join message carries the version of the lobby messages, and one without it reads as version 0 (the host then refuses it as another version)",
				ok && back.mLobbyProtocol == LOBBY_PROTOCOL && back.mClientToken == 77 && oldOk && oldBack.mLobbyProtocol == 0 && oldBack.mClientToken == 77 && LOBBY_PROTOCOL != 0);
			JoinReplyPacket reply;
			reply.mResult = (uint8)NetplayAuth::JoinResult::VERSION_MISMATCH;
			reply.mLobbyProtocol = LOBBY_PROTOCOL;
			const std::vector<uint8> replyBytes = writePacket(reply);
			JoinReplyPacket replyBack;
			const bool replyOk = readPacketBytes(replyBack, replyBytes);
			std::vector<uint8> oldReply(replyBytes.begin(), replyBytes.end() - 1);
			JoinReplyPacket oldReplyBack;
			const bool oldReplyOk = readPacketBytes(oldReplyBack, oldReply);
			report("compatibility: the answer of the host carries its version of the lobby messages too (an older host answer reads as 0, the friend then says different version), and a version mismatch is a join result of its own",
				replyOk && replyBack.mResult == (uint8)NetplayAuth::JoinResult::VERSION_MISMATCH && replyBack.mLobbyProtocol == LOBBY_PROTOCOL && oldReplyOk && oldReplyBack.mLobbyProtocol == 0 &&
				(uint8)NetplayAuth::JoinResult::VERSION_MISMATCH != (uint8)NetplayAuth::JoinResult::ACCEPTED && NetplayFailure::isRetryable(NetplayFailure::Reason::VERSION_MISMATCH) && std::string(NetplayFailure::getMessage(NetplayFailure::Reason::VERSION_MISMATCH)).size() > 10);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Relay server: the protocol, the relay logic (with forged datagrams and a fake clock) and the game's socket going through a real relay on the loopback
namespace
{
	struct SentDatagram
	{
		Relay::Endpoint mTo;
		std::vector<uint8_t> mData;
	};

	struct RelayFixture
	{
		std::vector<SentDatagram> mSent;
		Relay::Core mCore;
		double mNow = 1000.0;
		uint64_t mHostSecret = 0x5ec2e75ec2e75ec2ull;		// What the registering host game sends along (see RelayProtocol.h)

		explicit RelayFixture(const Relay::Limits& limits = Relay::Limits()) :
			mCore(limits, [this](const Relay::Endpoint& to, const uint8_t* data, size_t size) { mSent.push_back({ to, std::vector<uint8_t>(data, data + size) }); })
		{}

		static Relay::Endpoint ep(uint32_t ip, uint16_t port)  { Relay::Endpoint e; e.mIP = ip; e.mPort = port; return e; }

		void send(const Relay::Endpoint& from, const std::vector<uint8_t>& datagram)  { mCore.onDatagram(from, datagram.data(), datagram.size(), mNow); }
		void hello(const Relay::Endpoint& from, RelayProtocol::Type type, uint64_t lobby, uint64_t key)
		{
			std::vector<uint8_t> bytes;
			if (type == RelayProtocol::HOST_REGISTER)
				RelayProtocol::buildHostHello(bytes, lobby, key, mHostSecret);
			else
				RelayProtocol::buildHello(bytes, type, lobby, key);
			send(from, bytes);
		}
		void hostHelloWithSecret(const Relay::Endpoint& from, uint64_t lobby, uint64_t key, uint64_t secret)
		{
			std::vector<uint8_t> bytes;
			RelayProtocol::buildHostHello(bytes, lobby, key, secret);
			send(from, bytes);
		}
		const SentDatagram* lastTo(const Relay::Endpoint& to) const
		{
			for (size_t i = mSent.size(); i > 0; --i)
				if (mSent[i - 1].mTo == to)
					return &mSent[i - 1];
			return nullptr;
		}
		uint32_t hostToken(const Relay::Endpoint& host) const
		{
			const SentDatagram* reply = lastTo(host);
			return (reply != nullptr && reply->mData.size() == RelayProtocol::HEADER_SIZE + 12 && reply->mData[5] == RelayProtocol::HOST_REGISTERED) ? RelayProtocol::get32(&reply->mData[RelayProtocol::HEADER_SIZE + 8]) : 0;
		}
		uint32_t clientToken(const Relay::Endpoint& client) const
		{
			const SentDatagram* reply = lastTo(client);
			return (reply != nullptr && reply->mData.size() == RelayProtocol::HEADER_SIZE + 4 && reply->mData[5] == RelayProtocol::CLIENT_JOINED) ? RelayProtocol::get32(&reply->mData[RelayProtocol::HEADER_SIZE]) : 0;
		}
	};

	Relay::Endpoint relayEndpointFromAddress(const SocketAddress& address)
	{
		unsigned a = 0, b = 0, c = 0, d = 0;
		Relay::Endpoint result;
		const std::string ip = address.getIP();
		if (sscanf_s(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
			result.mIP = (a << 24) | (b << 16) | (c << 8) | d;
		result.mPort = address.getPort();
		return result;
	}

	std::string relayIPText(uint32_t ip)
	{
		return std::to_string((ip >> 24) & 0xff) + "." + std::to_string((ip >> 16) & 0xff) + "." + std::to_string((ip >> 8) & 0xff) + "." + std::to_string(ip & 0xff);
	}

	// The relay logic behind a real UDP socket (what s3air_relay.cpp does)
	struct SocketRelay
	{
		UDPSocket mSocket;
		Relay::Core mCore;
		uint16 mPort = 0;
		SocketRelay() : mCore(Relay::Limits(), [this](const Relay::Endpoint& to, const uint8_t* data, size_t size) { mSocket.sendData(data, size, SocketAddress(relayIPText(to.mIP), to.mPort)); }) {}
		void pump()
		{
			const double now = (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
			for (int i = 0; i < 200; ++i)
			{
				UDPSocket::ReceiveResult received;
				if (!mSocket.receiveNonBlocking(received) || received.mBuffer.empty())
					break;
				mCore.onDatagram(relayEndpointFromAddress(received.mSenderAddress), received.mBuffer.data(), received.mBuffer.size(), now);
			}
		}
	};

	void runRelayTests()
	{
		// --- Protocol
		{
			std::vector<uint8_t> hello;
			RelayProtocol::buildHello(hello, RelayProtocol::CLIENT_JOIN, 0x1122334455667788ull, 0x99aabbccddeeff00ull);
			uint8_t type = 0;
			report("relay protocol: a hello has the expected size and parses back",
				hello.size() == RelayProtocol::HELLO_SIZE && RelayProtocol::parseHeader(hello.data(), hello.size(), type) && type == RelayProtocol::CLIENT_JOIN &&
				RelayProtocol::get64(&hello[6]) == 0x1122334455667788ull && RelayProtocol::get64(&hello[14]) == 0x99aabbccddeeff00ull);
			std::vector<uint8_t> wrongMagic = hello;
			wrongMagic[0] = 'X';
			std::vector<uint8_t> wrongVersion = hello;
			wrongVersion[4] = 99;
			report("relay protocol: wrong magic or version is not a relay datagram", !RelayProtocol::parseHeader(wrongMagic.data(), wrongMagic.size(), type) && !RelayProtocol::parseHeader(wrongVersion.data(), wrongVersion.size(), type) && !RelayProtocol::parseHeader(hello.data(), 3, type));
			uint16_t id = 0;
			bool roundTrip = true;
			for (uint32_t n : { 1u, 2u, 255u, 256u, 4660u, 65535u })
				roundTrip = roundTrip && RelayProtocol::parseVirtualIP(RelayProtocol::virtualIP((uint16_t)n), id) && id == n;
			report("relay protocol: made-up peer addresses round trip and are recognized", roundTrip && RelayProtocol::isVirtualIP("240.0.0.1") && !RelayProtocol::isVirtualIP("127.0.0.1") && !RelayProtocol::isVirtualIP("240.1.0.1") && !RelayProtocol::isVirtualIP("8.8.8.8"));
			report("relay protocol: malformed made-up addresses are refused", !RelayProtocol::parseVirtualIP("240.0.256.1", id) && !RelayProtocol::parseVirtualIP("240.0.1", id) && !RelayProtocol::parseVirtualIP("240.0.a.1", id) && !RelayProtocol::parseVirtualIP("240.0.1.2.3", id) && !RelayProtocol::parseVirtualIP("240.0..1", id));
			report("relay protocol: the relay's lobby name and key do not reveal the session ID and differ from each other",
				NetplayAuth::relayLobbyFromSession(0x123456789aull) != 0x123456789aull && NetplayAuth::relayKeyFromSession(0x123456789aull) != NetplayAuth::relayLobbyFromSession(0x123456789aull) &&
				NetplayAuth::relayKeyFromSession(1) != NetplayAuth::relayKeyFromSession(2) && NetplayAuth::relayLobbyFromSession(7) == NetplayAuth::relayLobbyFromSession(7));
		}

		const Relay::Endpoint hostEp = RelayFixture::ep(0x0a000001, 5000), friendEp = RelayFixture::ep(0x0a000002, 6000), strangerEp = RelayFixture::ep(0x0a000003, 7000);
		const uint64_t lobby = 0xabcdef0123ull, key = 0x1234567890abcdefull;

		// --- Forwarding both ways
		{
			RelayFixture fixture;
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			const uint32_t hostToken = fixture.hostToken(hostEp);
			fixture.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t friendToken = fixture.clientToken(friendEp);
			report("relay: a host registers a lobby and a friend with the key joins it (both get a token)", hostToken != 0 && friendToken != 0 && fixture.mCore.getStats().mLobbies == 1 && fixture.mCore.getStats().mPipes == 1);

			const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
			std::vector<uint8_t> up;
			RelayProtocol::buildClientData(up, RelayProtocol::DATA_C2R, friendToken, payload, sizeof(payload));
			fixture.mSent.clear();
			fixture.send(friendEp, up);
			const bool toHost = fixture.mSent.size() == 1 && fixture.mSent[0].mTo == hostEp && fixture.mSent[0].mData.size() == RelayProtocol::HEADER_SIZE + 6 + 5 &&
				fixture.mSent[0].mData[5] == RelayProtocol::DATA_R2H && RelayProtocol::get32(&fixture.mSent[0].mData[6]) == hostToken && RelayProtocol::get16(&fixture.mSent[0].mData[10]) == 1 &&
				memcmp(&fixture.mSent[0].mData[12], payload, 5) == 0;
			uint16_t friendID = (toHost) ? RelayProtocol::get16(&fixture.mSent[0].mData[10]) : 0;
			report("relay: a friend's datagram reaches the host with the friend's number, the payload unchanged", toHost);

			std::vector<uint8_t> down;
			RelayProtocol::buildHostData(down, RelayProtocol::DATA_H2R, hostToken, friendID, payload, sizeof(payload));
			fixture.mSent.clear();
			fixture.send(hostEp, down);
			report("relay: the host's datagram reaches that friend, the payload unchanged",
				fixture.mSent.size() == 1 && fixture.mSent[0].mTo == friendEp && fixture.mSent[0].mData[5] == RelayProtocol::DATA_R2C && RelayProtocol::get32(&fixture.mSent[0].mData[6]) == friendToken &&
				fixture.mSent[0].mData.size() == RelayProtocol::HEADER_SIZE + 4 + 5 && memcmp(&fixture.mSent[0].mData[10], payload, 5) == 0);

			// A second friend gets its own number, and the datagrams do not get mixed up
			const Relay::Endpoint friend2 = RelayFixture::ep(0x0a000004, 6001);
			fixture.hello(friend2, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t token2 = fixture.clientToken(friend2);
			std::vector<uint8_t> down2;
			RelayProtocol::buildHostData(down2, RelayProtocol::DATA_H2R, hostToken, 2, payload, sizeof(payload));
			fixture.mSent.clear();
			fixture.send(hostEp, down2);
			report("relay: two friends are kept apart (the host addresses each by number)", token2 != 0 && token2 != friendToken && fixture.mSent.size() == 1 && fixture.mSent[0].mTo == friend2);

			// Repeated hello (keepalive) gives the same token and not a second place
			fixture.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			report("relay: a repeated join of the same friend keeps its token and does not use a second place", fixture.clientToken(friendEp) == friendToken && fixture.mCore.getStats().mPipes == 2);
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			report("relay: a repeated registration of the host keeps the host token", fixture.hostToken(hostEp) == hostToken && fixture.mCore.getStats().mLobbies == 1);
		}

		// --- Abuse: wrong key, forged data, stranger
		{
			RelayFixture fixture;
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			const uint32_t hostToken = fixture.hostToken(hostEp);
			fixture.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t friendToken = fixture.clientToken(friendEp);
			const uint8_t payload[3] = { 9, 9, 9 };

			// Wrong key: not one byte comes back
			const size_t before = fixture.mSent.size();
			fixture.hello(strangerEp, RelayProtocol::CLIENT_JOIN, lobby, key ^ 1);
			fixture.hello(strangerEp, RelayProtocol::HOST_REGISTER, lobby, key ^ 1);
			report("relay abuse: a join or registration with the wrong key gets no answer and no place", fixture.mSent.size() == before && fixture.mCore.getStats().mWrongKeys == 2 && fixture.mCore.getStats().mPipes == 1);

			// The stranger tries to take over the lobby: the real host still works
			std::vector<uint8_t> down;
			RelayProtocol::buildHostData(down, RelayProtocol::DATA_H2R, hostToken, 1, payload, sizeof(payload));
			fixture.mSent.clear();
			fixture.send(hostEp, down);
			report("relay abuse: a wrong-key registration does not take the lobby from the real host", fixture.mSent.size() == 1 && fixture.mSent[0].mTo == friendEp);

			// Data without having joined, with a wrong token, from a forged address of the host (wrong token), to a friend that does not exist
			std::vector<uint8_t> forged;
			fixture.mSent.clear();
			RelayProtocol::buildClientData(forged, RelayProtocol::DATA_C2R, 0x1234, payload, sizeof(payload));
			fixture.send(strangerEp, forged);
			RelayProtocol::buildClientData(forged, RelayProtocol::DATA_C2R, friendToken ^ 1, payload, sizeof(payload));
			fixture.send(friendEp, forged);
			RelayProtocol::buildHostData(forged, RelayProtocol::DATA_H2R, hostToken ^ 1, 1, payload, sizeof(payload));
			fixture.send(hostEp, forged);
			RelayProtocol::buildHostData(forged, RelayProtocol::DATA_H2R, hostToken, 1, payload, sizeof(payload));
			fixture.send(strangerEp, forged);		// Right token, but from an address that is not the host's
			RelayProtocol::buildHostData(forged, RelayProtocol::DATA_H2R, hostToken, 77, payload, sizeof(payload));
			fixture.send(hostEp, forged);			// Right host, but no such friend
			report("relay abuse: data with no join, a wrong token, from a forged host address or to a missing friend is dropped", fixture.mSent.empty() && fixture.mCore.getStats().mDroppedInvalid >= 5);

			// Garbage
			fixture.send(strangerEp, std::vector<uint8_t>(100, 0xaa));
			fixture.send(strangerEp, std::vector<uint8_t>(3, 0));
			fixture.send(strangerEp, std::vector<uint8_t>(RelayProtocol::MAX_DATAGRAM_SIZE + 1, 0));
			std::vector<uint8_t> bigData;
			RelayProtocol::buildClientData(bigData, RelayProtocol::DATA_C2R, friendToken, std::vector<uint8_t>(RelayProtocol::MAX_DATAGRAM_SIZE, 1).data(), RelayProtocol::MAX_DATAGRAM_SIZE);
			fixture.send(friendEp, bigData);
			std::vector<uint8_t> shortHello;
			RelayProtocol::buildHello(shortHello, RelayProtocol::CLIENT_JOIN, lobby, key);
			shortHello.pop_back();
			fixture.send(strangerEp, shortHello);
			std::vector<uint8_t> unknownType = shortHello;
			unknownType[5] = 200;
			fixture.send(strangerEp, unknownType);
			fixture.hello(strangerEp, RelayProtocol::CLIENT_JOIN, 0, key);
			report("relay abuse: garbage, truncated, oversized and unknown-type datagrams and lobby 0 are dropped without any answer", fixture.mSent.empty());

			// Unknown lobby: only a small refusal
			fixture.hello(strangerEp, RelayProtocol::CLIENT_JOIN, 0x777, key);
			report("relay: joining a lobby that nobody registered is answered with NO_HOST", fixture.mSent.size() == 1 && fixture.mSent[0].mData.size() == RelayProtocol::HEADER_SIZE + 1 && fixture.mSent[0].mData[5] == RelayProtocol::CLIENT_REFUSED && fixture.mSent[0].mData[6] == RelayProtocol::NO_HOST);
		}

		// --- Abuse: guessing the key is cut off
		{
			RelayFixture fixture;
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			for (int i = 0; i < 8; ++i)
			{
				fixture.mNow += 1.0;
				fixture.hello(strangerEp, RelayProtocol::CLIENT_JOIN, lobby, 0x5000 + i);
			}
			const size_t sentBefore = fixture.mSent.size();
			fixture.mNow += 1.0;
			fixture.hello(strangerEp, RelayProtocol::CLIENT_JOIN, lobby, key);		// Now even the RIGHT key from this address is ignored for a while
			const bool blocked = fixture.mSent.size() == sentBefore && fixture.mCore.getStats().mDroppedBlocked == 1;
			const Relay::Endpoint otherEp = RelayFixture::ep(0x0a000009, 6100);
			fixture.hello(otherEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const bool othersFine = fixture.clientToken(otherEp) != 0;
			fixture.mNow += 61.0;
			fixture.mCore.update(fixture.mNow);
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			fixture.hello(strangerEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			report("relay abuse: after 8 wrong keys an address is ignored for a minute (others are not affected), then it may try again", blocked && othersFine && fixture.clientToken(strangerEp) != 0);
		}

		// --- Abuse: floods
		{
			RelayFixture fixture;
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			const uint32_t hostToken = fixture.hostToken(hostEp);
			fixture.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t friendToken = fixture.clientToken(friendEp);
			const uint8_t payload[100] = { 0 };
			std::vector<uint8_t> up;
			RelayProtocol::buildClientData(up, RelayProtocol::DATA_C2R, friendToken, payload, sizeof(payload));
			fixture.mSent.clear();
			for (int i = 0; i < 50000; ++i)
				fixture.send(friendEp, up);		// All at the same instant
			const uint64_t forwarded = fixture.mCore.getStats().mForwarded;
			report("relay abuse: a data flood from one friend is capped at the burst limit and the rest is dropped", forwarded <= 6100 && forwarded >= 5000 && fixture.mCore.getStats().mDroppedRate >= 40000);

			// While that friend floods, the other direction and other friends are not hurt
			const Relay::Endpoint friend2 = RelayFixture::ep(0x0a000005, 6200);
			fixture.hello(friend2, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t token2 = fixture.clientToken(friend2);
			std::vector<uint8_t> up2;
			RelayProtocol::buildClientData(up2, RelayProtocol::DATA_C2R, token2, payload, sizeof(payload));
			const uint64_t before = fixture.mCore.getStats().mForwarded;
			fixture.send(friend2, up2);
			std::vector<uint8_t> down;
			RelayProtocol::buildHostData(down, RelayProtocol::DATA_H2R, hostToken, 1, payload, sizeof(payload));
			fixture.send(hostEp, down);
			report("relay abuse: another friend and the host's own traffic still get through while one friend floods", fixture.mCore.getStats().mForwarded == before + 2);

			// The flood stops being dropped when time passes (the limit is a rate, not a ban)
			fixture.mNow += 2.0;
			const uint64_t before2 = fixture.mCore.getStats().mForwarded;
			fixture.send(friendEp, up);
			report("relay abuse: the flooding friend can send again after a short pause", fixture.mCore.getStats().mForwarded == before2 + 1);

			// Byte limit: big datagrams are limited by bytes even when few
			RelayFixture bytes;
			bytes.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			bytes.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t bytesToken = bytes.clientToken(friendEp);
			std::vector<uint8_t> big;
			RelayProtocol::buildClientData(big, RelayProtocol::DATA_C2R, bytesToken, std::vector<uint8_t>(4000, 7).data(), 4000);
			for (int i = 0; i < 3000; ++i)
				bytes.send(friendEp, big);
			report("relay abuse: a flood of big datagrams is capped by bytes (about 6 MB burst)", bytes.mCore.getStats().mForwardedBytes <= 6100000 && bytes.mCore.getStats().mForwardedBytes >= 5000000);

			// Hello flood from one address: only a handful are served
			RelayFixture hellos;
			hellos.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			hellos.mSent.clear();
			for (int i = 0; i < 2000; ++i)
				hellos.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			report("relay abuse: a flood of join requests from one address is answered only a few times", hellos.mSent.size() <= (size_t)(hellos.mCore.getLimits().mControlBurst + 8) && hellos.mCore.getStats().mDroppedRate >= 1900);
		}

		// --- Limits: lobbies, friends, memory
		{
			RelayFixture fixture;
			for (int i = 0; i < 10; ++i)
				fixture.hello(RelayFixture::ep(0x0b000001, (uint16_t)(4000 + i)), RelayProtocol::HOST_REGISTER, 100 + i, key);
			report("relay limits: one address can open only a few lobbies", fixture.mCore.getStats().mLobbies <= 4);

			RelayFixture many;
			for (int i = 0; i < 2000; ++i)
				many.hello(RelayFixture::ep(0x0c000000 + (uint32_t)i, 5000), RelayProtocol::HOST_REGISTER, 1000 + i, key);
			report("relay limits: the number of lobbies is capped", many.mCore.getStats().mLobbies <= many.mCore.getLimits().mMaxLobbies && many.mCore.getStats().mLobbies >= 500);

			RelayFixture full;
			full.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			bool allJoined = true;
			for (int i = 0; i < 6; ++i)
			{
				const Relay::Endpoint e = RelayFixture::ep(0x0d000000 + (uint32_t)i, 6000);
				full.hello(e, RelayProtocol::CLIENT_JOIN, lobby, key);
				allJoined = allJoined && full.clientToken(e) != 0;
			}
			const Relay::Endpoint seventh = RelayFixture::ep(0x0d0000ff, 6000);
			full.hello(seventh, RelayProtocol::CLIENT_JOIN, lobby, key);
			const SentDatagram* reply = full.lastTo(seventh);
			report("relay limits: a lobby has room for a few friends only; the next one is told FULL", allJoined && reply != nullptr && reply->mData[5] == RelayProtocol::CLIENT_REFUSED && reply->mData[6] == RelayProtocol::FULL);

			RelayFixture sources;
			for (int i = 0; i < 30000; ++i)
				sources.hello(RelayFixture::ep(0x0e000000 + (uint32_t)i, 9000), RelayProtocol::CLIENT_JOIN, 5, key);
			report("relay limits: memory for remembered addresses stays bounded during a flood from many addresses", sources.mCore.getSourceCount() <= sources.mCore.getLimits().mMaxSources);

			// Review findings: taking over a lobby with the shared key, and rotating the source port
			{
				RelayFixture hijack;
				hijack.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
				const uint32_t realHostToken = hijack.hostToken(hostEp);
				hijack.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
				const uint32_t friendToken = hijack.clientToken(friendEp);
				const size_t sentBefore = hijack.mSent.size();
				hijack.hostHelloWithSecret(friendEp, lobby, key, 0x1111222233334444ull);		// A friend knows the lobby key (it follows from the code) but not the host's secret
				report("relay abuse: a friend who knows the lobby key but not the host secret cannot take the lobby over (no answer, counted as a wrong key)", hijack.mSent.size() == sentBefore && hijack.mCore.getStats().mWrongKeys == 1);
				const uint8_t payload[2] = { 5, 6 };
				std::vector<uint8_t> up;
				RelayProtocol::buildClientData(up, RelayProtocol::DATA_C2R, friendToken, payload, sizeof(payload));
				hijack.mSent.clear();
				hijack.send(friendEp, up);
				std::vector<uint8_t> down;
				RelayProtocol::buildHostData(down, RelayProtocol::DATA_H2R, realHostToken, 1, payload, sizeof(payload));
				hijack.send(hostEp, down);
				report("relay abuse: after the takeover attempt the friend's data still goes to the real host and the real host's data still reaches the friend",
					hijack.mSent.size() == 2 && hijack.mSent[0].mTo == hostEp && hijack.mSent[1].mTo == friendEp);

				// The real host's address changes (router rebinding): the same secret re-registers it
				const Relay::Endpoint movedHost = RelayFixture::ep(0x0a000001, 5001);
				hijack.hello(movedHost, RelayProtocol::HOST_REGISTER, lobby, key);
				report("relay: the real host can re-register from a new address with its secret and keeps its token", hijack.hostToken(movedHost) == realHostToken);
				hijack.hostHelloWithSecret(movedHost, lobby, key, 0);
				report("relay protocol: a registration with host secret 0 is refused", hijack.mCore.getStats().mDroppedInvalid >= 1);
			}
			{
				// One address that changes its source port for every datagram is still one sender
				RelayFixture rotate;
				rotate.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
				for (int i = 0; i < 9000; ++i)
					rotate.hello(RelayFixture::ep(0x0f000001, (uint16_t)(1000 + (i % 60000))), RelayProtocol::CLIENT_JOIN, lobby, key ^ 0x77);
				const size_t sentBefore = rotate.mSent.size();
				rotate.hello(RelayFixture::ep(0x0f000001, 61000), RelayProtocol::CLIENT_JOIN, lobby, key);		// Right key, yet another port
				const Relay::Endpoint otherIP = RelayFixture::ep(0x0f000002, 1000);
				rotate.mNow += 1.0;
				rotate.hello(otherIP, RelayProtocol::CLIENT_JOIN, lobby, key);
				report("relay limits: rotating the source port does not give a fresh allowance (one remembered address, wrong keys counted per IP, blocked for all its ports)",
					rotate.mCore.getSourceCount() <= 3 && rotate.mCore.getStats().mWrongKeys == (uint64_t)rotate.mCore.getLimits().mMaxBadKeys && rotate.mSent.size() == sentBefore + 1 && rotate.clientToken(otherIP) != 0);

				// A flood from thousands of different addresses must not shut real newcomers out once it is over
				RelayFixture full;
				for (int i = 0; i < 9000; ++i)
					full.hello(RelayFixture::ep(0x10000000 + (uint32_t)i, 9000), RelayProtocol::CLIENT_JOIN, 5, key);
				const bool filled = full.mCore.getSourceCount() >= full.mCore.getLimits().mMaxSources;
				full.mNow += 11.0;
				const Relay::Endpoint newcomer = RelayFixture::ep(0x20000001, 9000);
				full.hello(newcomer, RelayProtocol::HOST_REGISTER, lobby, key);
				report("relay limits: after a flood from thousands of addresses filled the table, a real newcomer is served again a few seconds later", filled && full.hostToken(newcomer) != 0);
			}

			// Amplification: no answer is ever larger than the request
			RelayFixture amp;
			bool smaller = true;
			std::vector<uint8_t> request;
			RelayProtocol::buildHostHello(request, lobby, key, 0x1234567812345678ull);
			amp.send(hostEp, request);
			smaller = smaller && !amp.mSent.empty() && amp.mSent.back().mData.size() <= request.size();
			RelayProtocol::buildHello(request, RelayProtocol::CLIENT_JOIN, lobby, key);
			amp.send(friendEp, request);
			smaller = smaller && amp.mSent.back().mData.size() <= request.size();
			RelayProtocol::buildHello(request, RelayProtocol::CLIENT_JOIN, 0x999, key);
			amp.send(strangerEp, request);
			smaller = smaller && amp.mSent.back().mData.size() <= request.size();
			report("relay abuse: every answer is smaller than the request (the relay cannot amplify traffic)", smaller);
		}

		// --- Expiry
		{
			RelayFixture fixture;
			fixture.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			fixture.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const uint32_t friendToken = fixture.clientToken(friendEp);
			fixture.mNow += 50.0;		// The host stopped registering
			fixture.mCore.update(fixture.mNow);
			std::vector<uint8_t> up;
			const uint8_t payload[2] = { 1, 2 };
			RelayProtocol::buildClientData(up, RelayProtocol::DATA_C2R, friendToken, payload, sizeof(payload));
			fixture.mSent.clear();
			fixture.send(friendEp, up);
			fixture.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			const SentDatagram* reply = fixture.lastTo(friendEp);
			report("relay: a lobby whose host went silent disappears, and nothing is forwarded to it", !fixture.mCore.hasLobby(lobby) && fixture.mCore.getStats().mLobbies == 0 && reply != nullptr && reply->mData[5] == RelayProtocol::CLIENT_REFUSED);

			RelayFixture idle;
			idle.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
			idle.hello(friendEp, RelayProtocol::CLIENT_JOIN, lobby, key);
			for (int i = 0; i < 13; ++i)
			{
				idle.mNow += 5.0;
				idle.hello(hostEp, RelayProtocol::HOST_REGISTER, lobby, key);
				idle.mCore.update(idle.mNow);
			}
			report("relay: a friend that sent nothing for a minute loses its place, the lobby stays", idle.mCore.getStats().mPipes == 0 && idle.mCore.hasLobby(lobby));
		}

		// --- The game's UDP socket through a real relay on the loopback
		{
			SocketRelay relay;
			UDPSocket hostSocket, friendSocket, attacker;
			uint16 hostPort = 0, friendPort = 0, attackerPort = 0;
			if (!bindFreePort(relay.mSocket, relay.mPort, 40500) || !bindFreePort(hostSocket, hostPort, 40600) || !bindFreePort(friendSocket, friendPort, 40700) || !bindFreePort(attacker, attackerPort, 40800))
			{
				report("relay socket tests could bind their sockets", false);
				return;
			}
			const SocketAddress relayAddress("127.0.0.1", relay.mPort);
			const uint64 lobbyID = NetplayAuth::relayLobbyFromSession(0x1122334455ull), relayKey = NetplayAuth::relayKeyFromSession(0x1122334455ull);
			hostSocket.startRelay(UDPSocket::RelayRole::HOST, relayAddress, lobbyID, relayKey);
			friendSocket.startRelay(UDPSocket::RelayRole::CLIENT, relayAddress, lobbyID, relayKey);

			TestListener hostListener, friendListener;
			ConnectionManager hostManager(&hostSocket, nullptr, hostListener, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE);
			ConnectionManager friendManager(&friendSocket, nullptr, friendListener, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE);
			auto pumpAll = [&](int milliseconds)
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
				while (std::chrono::steady_clock::now() < deadline)
				{
					hostSocket.updateRelay();
					friendSocket.updateRelay();
					relay.pump();
					hostManager.updateConnectionManager();
					friendManager.updateConnectionManager();
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			};
			pumpAll(600);
			report("relay sockets: the host is registered and the friend let in", hostSocket.isRelayReady() && friendSocket.isRelayReady());

			// A real connection (handshake, packets in both directions) over the relay, to the made-up address of the host
			NetConnection friendConnection;
			friendConnection.startConnectTo(friendManager, SocketAddress(RelayProtocol::hostVirtualIP(), RelayProtocol::VIRTUAL_PORT));
			pumpAll(500);
			TestPacket hello;
			friendConnection.sendPacket(hello);
			pumpAll(400);
			const bool connected = friendConnection.getState() == NetConnection::State::CONNECTED && hostListener.mCreated.size() == 1 && hostListener.mCreated[0]->getState() == NetConnection::State::CONNECTED;
			report("relay sockets: two connection managers connect through the relay and exchange a packet", connected && hostListener.mReceivedTestPackets == 1);
			bool hostSeesVirtual = connected && RelayProtocol::isVirtualIP(hostListener.mCreated[0]->getRemoteAddress().getIP());
			report("relay sockets: the host sees the relayed friend under a made-up address, the friend sees the host under one too", hostSeesVirtual && friendConnection.getRemoteAddress().getIP() == RelayProtocol::hostVirtualIP());
			if (connected)
			{
				TestPacket back;
				hostListener.mCreated[0]->sendPacket(back);
				pumpAll(400);
				report("relay sockets: the host's packet reaches the friend through the relay", friendListener.mReceivedTestPackets == 1 && relay.mCore.getStats().mForwarded > 4);
			}

			// A forged "relay" datagram from somebody else is not unwrapped: it reaches the game as an ordinary datagram from its real sender
			{
				std::vector<uint8_t> forged;
				const uint8_t payload[4] = { 1, 2, 3, 4 };
				RelayProtocol::buildHostData(forged, RelayProtocol::DATA_R2H, 0x1111, 1, payload, sizeof(payload));
				attacker.sendData(forged, SocketAddress("127.0.0.1", hostPort));
				bool sawForged = false, sawVirtual = false;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
				while (std::chrono::steady_clock::now() < deadline)
				{
					UDPSocket::ReceiveResult received;
					hostSocket.receiveNonBlocking(received);
					if (received.mBuffer == forged)
					{
						sawForged = true;
						sawVirtual = RelayProtocol::isVirtualIP(received.mSenderAddress.getIP());
					}
					else if (!hostSocket.consumedLanPacket())
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				report("relay sockets: a datagram that looks like relay data but comes from another address is not treated as relayed", sawForged && !sawVirtual);
			}

			friendConnection.disconnect(NetConnection::DisconnectReason::MANUAL_LOCAL);
			hostManager.terminateAllConnections();
			hostListener.deleteAll();

			// A friend with the wrong key never gets in
			UDPSocket wrongFriend;
			uint16 wrongPort = 0;
			if (bindFreePort(wrongFriend, wrongPort, 40900))
			{
				wrongFriend.startRelay(UDPSocket::RelayRole::CLIENT, relayAddress, lobbyID, relayKey ^ 0xff);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
				while (std::chrono::steady_clock::now() < deadline)
				{
					wrongFriend.updateRelay();
					relay.pump();
					UDPSocket::ReceiveResult received;
					wrongFriend.receiveNonBlocking(received);
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				report("relay sockets: a friend with the wrong key is not let in", !wrongFriend.isRelayReady() && relay.mCore.getStats().mWrongKeys >= 1);
			}
		}
	}
}

namespace
{
	Json::Value parseJson(const std::string& text)
	{
		std::string errors;
		return rmx::JsonHelper::loadFromMemory(std::vector<uint8>(text.begin(), text.end()), &errors);
	}

	// Self-update (the in-game updater): pure logic from SelfUpdateCore.h
	void runSelfUpdateTests()
	{
		using namespace selfupdate;

		// --- Versions ---
		Version v;
		report("update version: 1.2.3 is read", parseVersion("1.2.3", v) && v.mPart[0] == 1 && v.mPart[1] == 2 && v.mPart[2] == 3);
		report("update version: a leading v and a trailing line break are allowed", parseVersion("v10.0.7\r\n", v) && v.mPart[0] == 10 && v.mPart[2] == 7);
		bool allBadRejected = true;
		for (const char* bad : { "", " ", "1", "1.2", "1.2.3.4", "1.2.3-beta", "1.2.3+5", "a.b.c", "1..3", ".1.2", "1.2.", "-1.2.3", "1.2.-3", "100000.0.0", "1.2.3 4", "v", "vv1.2.3", "1,2,3", "0x1.2.3" })
		{
			if (parseVersion(bad, v))
				allBadRejected = false;
		}
		report("update version: malformed version strings are rejected", allBadRejected);
		Version a, b;
		parseVersion("1.2.9", a); parseVersion("1.2.10", b);
		report("update version: 1.2.10 is newer than 1.2.9 (numbers, not text)", compareVersions(b, a) > 0 && compareVersions(a, b) < 0);
		parseVersion("1.9.9", a); parseVersion("1.10.0", b);
		report("update version: 1.10.0 is newer than 1.9.9", compareVersions(b, a) > 0);
		parseVersion("2.0.0", a); parseVersion("1.99.99", b);
		report("update version: 2.0.0 is newer than 1.99.99", compareVersions(a, b) > 0);
		parseVersion("1.2.3", a); parseVersion("v1.2.3", b);
		report("update version: equal versions compare equal", compareVersions(a, b) == 0);
		Version older, same, newer, invalid;
		parseVersion("1.0.0", older); parseVersion("1.1.0", same); parseVersion("1.2.0", newer);
		report("update decision: newer release is offered", decide(same, newer) == Decision::UPDATE_AVAILABLE);
		report("update decision: the same version is up to date", decide(same, same) == Decision::UP_TO_DATE);
		report("update decision: an older release is never offered (no downgrade)", decide(same, older) == Decision::OLDER_THAN_CURRENT);
		report("update decision: an unreadable version is invalid", decide(same, invalid) == Decision::INVALID && decide(invalid, same) == Decision::INVALID);

		// --- SHA-256 ---
		report("update sha256: empty input", Sha256::hashHex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
		report("update sha256: abc", Sha256::hashHex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		{
			const std::string text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
			report("update sha256: 56 byte message (padding edge)", Sha256::hashHex(text.data(), text.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
			Sha256 sha;
			const std::string chunk(1000, 'a');
			for (int i = 0; i < 1000; ++i)
				sha.update(chunk.data(), chunk.size());
			report("update sha256: one million 'a' in chunks", sha.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
			Sha256 streamed;
			for (size_t i = 0; i < text.size(); i += 7)
				streamed.update(text.data() + i, std::min<size_t>(7, text.size() - i));
			report("update sha256: odd chunk sizes give the same hash as one piece", streamed.finishHex() == Sha256::hashHex(text.data(), text.size()));
		}

		// --- Zip-slip protection: names of an update package ---
		std::string path;
		bool isDirectory = false;
		report("update path: a plain relative path is accepted", normalizeEntryPath("data/sprites/a.png", path, isDirectory) && path == "data/sprites/a.png" && !isDirectory);
		report("update path: backslashes become slashes", normalizeEntryPath("data\\scripts\\a.lemon", path, isDirectory) && path == "data/scripts/a.lemon");
		report("update path: a trailing slash marks a directory", normalizeEntryPath("Sonic3Airnet/data/", path, isDirectory) && isDirectory && path == "Sonic3Airnet/data");
		bool allUnsafeRefused = true;
		std::string firstAccepted;
		const std::vector<std::string> unsafeNames = { "../evil.txt", "a/../../b", "..\\..\\evil", "a/..\\b", "..", "/abs/path", "\\abs\\path", "C:\\Windows\\x", "C:/x", "c:x", "D:", "a/b:stream", "a//b", "./a", "a/./b", "con", "data/NUL.txt", "COM1", "lpt9.bin", "a/b.", "a/b ", "a/ ", "", "/", std::string("a\x01") + "b", "x*y", "x?y", "x<y", "a|b", "//server/share/x", "....//x", "a/../b" };
		for (const std::string& bad : unsafeNames)
		{
			if (normalizeEntryPath(bad, path, isDirectory))
			{
				allUnsafeRefused = false;
				if (firstAccepted.empty())
					firstAccepted = bad;
			}
		}
		report("update path: absolute paths, drive letters, .., stream markers, device names and bad characters are all refused", allUnsafeRefused);
		if (!allUnsafeRefused)
			std::cout << "     (accepted by mistake: " << firstAccepted << ")\n";
		report("update path: an over-long name is refused", !normalizeEntryPath(std::string(300, 'a'), path, isDirectory));

		// --- Things an update must never carry ---
		report("update forbidden: saves, states, savedata and mods folders", isForbiddenPackagePath("saves/save1.sav", 10) && isForbiddenPackagePath("data/States/x.state", 10) && isForbiddenPackagePath("savedata/a", 1) && isForbiddenPackagePath("mods/MyMod/mod.json", 10));
		report("update forbidden: user settings and active mods", isForbiddenPackagePath("settings.json", 10) && isForbiddenPackagePath("Settings_global.json", 10) && isForbiddenPackagePath("active-mods.json", 10));
		report("update forbidden: the test marker and test fixtures", isForbiddenPackagePath("testhooks.enabled", 1) && isForbiddenPackagePath("harness/ChecksHarness/mod.json", 1) && isForbiddenPackagePath("x/PartyHarness/a", 1));
		report("update forbidden: ROM-like files", isForbiddenPackagePath("Sonic_Knuckles_wSonic3.bin", 10) && isForbiddenPackagePath("rom.gen", 1) && isForbiddenPackagePath("rom.SMD", 1) && isForbiddenPackagePath("a.md", 1) && isForbiddenPackagePath("data/big.bin", 600 * 1024));
		report("update allowed: the game's own files", !isForbiddenPackagePath("Sonic3AIR.exe", 100000000) && !isForbiddenPackagePath("data/audio.bin", 1000) && !isForbiddenPackagePath("bundled_mods/DropInCoop/mod.json", 100) && !isForbiddenPackagePath("scripts/main_gameapp.lemon", 100) && !isForbiddenPackagePath("README_ONLINE.txt", 10) && !isForbiddenPackagePath("config.json", 10) && !isForbiddenPackagePath("version.txt", 5));
		report("update: config.json is kept if it exists, nothing else is", isKeepIfExistsPath("config.json") && isKeepIfExistsPath("Config.JSON") && !isKeepIfExistsPath("data/config.json") && !isKeepIfExistsPath("Sonic3AIR.exe"));
		report("update: the single top folder of a package is stripped", stripPackageFolder("Sonic3Airnet/data/a.txt", true) == "data/a.txt" && stripPackageFolder("other/a.txt", true).empty() && stripPackageFolder("data/a.txt", false) == "data/a.txt");

		// --- Addresses ---
		Endpoints production;
		Endpoints test;
		test.mApiBase = "http://127.0.0.1:34567";
		test.mTestMode = true;
		Endpoints testWithoutFlag;
		testWithoutFlag.mApiBase = "http://127.0.0.1:34567";
		Endpoints evil;
		evil.mApiBase = "https://api.github.com.evil.example";
		report("update address: the public GitHub API is accepted", isAcceptedApiBase(production));
		report("update address: a loopback test server is accepted only in test mode", isAcceptedApiBase(test) && !isAcceptedApiBase(testWithoutFlag) && !isAcceptedApiBase(evil));
		const std::string repo = "owner/repo";
		report("update address: the release download of the repository is accepted", isAcceptedAssetUrl("https://github.com/owner/repo/releases/download/v1.2.3/Sonic3Airnet.zip", repo, production));
		bool badUrlsRefused = true;
		const std::vector<std::string> badUrls = { "http://github.com/owner/repo/releases/download/v1/x.zip", "https://github.com/other/repo/releases/download/v1/x.zip", "https://github.com.evil.com/owner/repo/releases/download/v1/x.zip", "https://github.com@evil.com/owner/repo/releases/download/v1/x.zip", "https://evil.com/owner/repo/releases/download/v1/x.zip", "https://github.com/owner/repo/releases/download/../../x.zip", "https://github.com/owner/repo/archive/main.zip", "https://github.com:8443/owner/repo/releases/download/v1/x.zip", "https://objects.githubusercontent.com/x.zip", "ftp://github.com/owner/repo/releases/download/v1/x.zip", "https://github.com/owner/repo/releases/download/v1/x y.zip", "https://github.com\\@evil.com/owner/repo/releases/download/x", "file:///c:/x" };
		for (const std::string& bad : badUrls)
		{
			if (isAcceptedAssetUrl(bad, repo, production))
				badUrlsRefused = false;
		}
		report("update address: other hosts, http, user info, other repositories and path tricks are refused", badUrlsRefused);
		report("update address: a loopback asset is accepted only in test mode and only on the same port", isAcceptedAssetUrl("http://127.0.0.1:34567/dl/a.zip", repo, test) && !isAcceptedAssetUrl("http://127.0.0.1:34567/dl/a.zip", repo, production) && !isAcceptedAssetUrl("http://127.0.0.1:34568/dl/a.zip", repo, test) && !isAcceptedAssetUrl("http://127.0.0.1:34567/dl/a.zip", repo, testWithoutFlag));
		report("update address: repository names", isValidRepoName("TrickerNoxics14/Sonic-3-Airnet") && !isValidRepoName("") && !isValidRepoName("owner") && !isValidRepoName("a/b/c") && !isValidRepoName("a/../b") && !isValidRepoName("a b/c") && !isValidRepoName("/b") && !isValidRepoName("a/"));

		// --- Release information ---
		const std::string goodUrl = "https://github.com/owner/repo/releases/download/v1.2.0/";
		const std::string hash64(64, 'a');
		auto releaseJson = [&](const std::string& tag, const std::string& assets, const std::string& extra = "")
		{
			return parseJson("{\"tag_name\":\"" + tag + "\",\"body\":\"Notes here\"" + extra + ",\"assets\":" + assets + "}");
		};
		const std::string goodAssets = "[{\"name\":\"Sonic3Airnet.zip\",\"size\":1000,\"browser_download_url\":\"" + goodUrl + "Sonic3Airnet.zip\",\"digest\":\"sha256:" + hash64 + "\"},"
			"{\"name\":\"Sonic3Airnet.manifest.json\",\"size\":200,\"browser_download_url\":\"" + goodUrl + "Sonic3Airnet.manifest.json\"}]";
		ReleaseInfo release;
		std::string error;
		report("update release: a good release is read (version, package, digest, manifest)",
			parseRelease(releaseJson("v1.2.0", goodAssets), repo, production, release, error) && release.mVersion.mPart[1] == 2 && release.mPackageSize == 1000 && release.mPackageDigest == hash64 && release.mHasManifest && release.mNotes == "Notes here");
		report("update release: a tag that is not a version is refused", !parseRelease(releaseJson("latest", goodAssets), repo, production, release, error) && !error.empty());
		report("update release: a pre-release is refused", !parseRelease(releaseJson("v1.2.0", goodAssets, ",\"prerelease\":true"), repo, production, release, error));
		report("update release: a release without the package is refused", !parseRelease(releaseJson("v1.2.0", "[]"), repo, production, release, error));
		report("update release: a package from another host is refused",
			!parseRelease(releaseJson("v1.2.0", "[{\"name\":\"Sonic3Airnet.zip\",\"size\":1,\"browser_download_url\":\"https://evil.example/Sonic3Airnet.zip\"}]"), repo, production, release, error));
		report("update release: a package that is too large is refused",
			!parseRelease(releaseJson("v1.2.0", "[{\"name\":\"Sonic3Airnet.zip\",\"size\":999999999999,\"browser_download_url\":\"" + goodUrl + "Sonic3Airnet.zip\"}]"), repo, production, release, error));
		report("update release: garbage instead of an object is refused", !parseRelease(parseJson("[1,2,3]"), repo, production, release, error) && !parseRelease(parseJson("not json"), repo, production, release, error));
		report("update release: notes are plain ASCII without markup noise", sanitizeNotes("## Title\r\n**bold** `code` caf\xc3\xa9\n\n\n\n\nend") == " Title\nbold code caf??\n\nend");
		report("update release: very long notes are shortened", sanitizeNotes(std::string(5000, 'x')).size() < 800);

		// --- Which hash must the package have ---
		Manifest manifest;
		ExpectedPackage expected;
		const std::string manifestOk = "{\"version\":\"1.2.0\",\"asset\":\"Sonic3Airnet.zip\",\"size\":1000,\"sha256\":\"" + hash64 + "\"}";
		report("update manifest: a good manifest is read", parseManifest(parseJson(manifestOk), manifest, error) && manifest.mSize == 1000 && manifest.mSha256 == hash64);
		report("update manifest: a hash that is not 64 hex digits is refused", !parseManifest(parseJson("{\"version\":\"1.2.0\",\"asset\":\"x\",\"size\":1,\"sha256\":\"abc\"}"), manifest, error) && !parseManifest(parseJson("{\"version\":\"1.2.0\"}"), manifest, error));
		parseRelease(releaseJson("v1.2.0", goodAssets), repo, production, release, error);
		parseManifest(parseJson(manifestOk), manifest, error);
		report("update verify: manifest and digest agree, the hash to check is known", resolveExpectedPackage(release, &manifest, expected, error) && expected.mSha256 == hash64 && expected.mSize == 1000);
		Manifest other = manifest;
		other.mSha256 = std::string(64, 'b');
		report("update verify: manifest and GitHub digest that disagree are refused", !resolveExpectedPackage(release, &other, expected, error));
		other = manifest; other.mVersion = "1.1.0";
		report("update verify: a manifest of another version is refused", !resolveExpectedPackage(release, &other, expected, error));
		other = manifest; other.mAsset = "Other.zip";
		report("update verify: a manifest for another file is refused", !resolveExpectedPackage(release, &other, expected, error));
		other = manifest; other.mSize = 999;
		report("update verify: a manifest with another size than the release is refused", !resolveExpectedPackage(release, &other, expected, error));
		ReleaseInfo noDigest = release;
		noDigest.mPackageDigest.clear();
		report("update verify: the manifest alone is enough", resolveExpectedPackage(noDigest, &manifest, expected, error) && expected.mSha256 == hash64);
		report("update verify: the GitHub digest alone is enough", resolveExpectedPackage(release, nullptr, expected, error) && expected.mSha256 == hash64);
		report("update verify: without manifest and without digest nothing is installed", !resolveExpectedPackage(noDigest, nullptr, expected, error) && expected.mSha256.empty());
		report("update http: status texts", describeHttpStatus(404, false).find("No release") != std::string::npos && describeHttpStatus(403, true).find("too many") != std::string::npos && describeHttpStatus(503, false).find("Try again") != std::string::npos);
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
	runLobbyLogicTests();
	runCharacterTests();
	Sockets::startupSockets();
	runRelayTests();
	Sockets::shutdownSockets();
	runSelfUpdateTests();
	report("test hooks are off by default (no marker file next to this test program)", !TestHooks::isEnabled() && TestHooks::getEnv("PATH") == nullptr);
	report("LAN discovery finds a host with the matching session code", lanFound);
	report("LAN discovery rejects a different session code", lanMismatchRejected);

	std::cout << (failures == 0 ? "ONLINE CORE TESTS PASS" : "ONLINE CORE TESTS FAIL") << " (" << failures << " failures)\n";
	return failures == 0 ? 0 : 1;
}
