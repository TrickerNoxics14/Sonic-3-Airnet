/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>


class Sockets
{
public:
	enum class ProtocolFamily
	{
		IPv4,
		IPv6,
		DualStack	// IPv6 socket that also supports IPv4
	};

public:
	static void startupSockets();
	static void shutdownSockets();

	static bool resolveToIP(const std::string& hostName, std::string& outIP, bool useIPv6);

public:
	static inline rmx::ErrorHandling::LoggerInterface* mLogger = nullptr;

private:
	static inline bool mIsInitialized = false;
};


struct SocketAddress
{
public:
	static inline bool mPreventIPLogging = false;

public:
	inline SocketAddress() :
		mHasSockAddr(false),
		mHasIpPort(false),
		mPort(0)
	{}

	inline SocketAddress(std::string_view ip, uint16 port) :
		mHasSockAddr(false),
		mHasIpPort(true),
		mIP(ip),
		mPort(port)
	{}

	inline const uint8* getSockAddr() const  { assureSockAddr();  return mSockAddr; }
	inline uint8* accessSockAddr() const	 { return mSockAddr; }

	inline const std::string& getIP() const	 { assureIpPort();  return mIP; }
	inline uint16 getPort() const			 { assureIpPort();  return mPort; }
	inline std::string toString() const		 { assureIpPort();  return mIP + ':' + std::to_string(mPort); }
	std::string toLoggedString() const;

	inline bool isValid() const  { return (mHasSockAddr || mHasIpPort); }

	inline void clear()
	{
		mHasSockAddr = false;
		mHasIpPort = false;
	}

	inline void set(std::string_view ip, uint16 port)
	{
		mHasSockAddr = false;
		mHasIpPort = true;
		mIP = ip;
	#ifdef _MSC_VER
		mIP = ip;	// Two assignments because there seems to be a weird compiler error that makes the first assignment not work
	#endif
		mPort = port;
	}

	inline void set(const uint8* sockAddr)
	{
		memcpy(mSockAddr, sockAddr, 128);
		mHasSockAddr = true;
		mHasIpPort = false;
	}

	inline void onSockAddrSet()
	{
		mHasSockAddr = true;
		mHasIpPort = false;
	}

	inline void writeTo(std::string& outIP, uint16& outPort) const
	{
		assureIpPort();
		outIP = mIP;
		outPort = mPort;
	}

	uint64 getHash() const;

	void assureSockAddr() const;
	void assureIpPort() const;

	bool operator==(const SocketAddress& other) const;
	inline bool operator!=(const SocketAddress& other) const { return !operator==(other); }

private:
	// Yes, everything is mutable here indeed...
	mutable bool mHasSockAddr = false;
	mutable bool mHasIpPort = false;
	mutable uint8 mSockAddr[128];	// Size of sockaddr_storage (pretty large, huh?)
	mutable std::string mIP;		// Usually an IPv4 as string, e.g. "123.45.67.89"
	mutable uint16 mPort = 0;
};


class TCPSocket
{
public:
	struct ReceiveResult
	{
		std::vector<uint8> mBuffer;
	};

public:
	TCPSocket();
	~TCPSocket();

	bool isValid() const;
	void close();

	const SocketAddress& getRemoteAddress();
	void swapWith(TCPSocket& other);

	bool setupServer(uint16 serverPort, Sockets::ProtocolFamily protocolFamily = Sockets::ProtocolFamily::IPv4);
	bool acceptConnection(TCPSocket& outSocket);

	bool connectTo(const std::string& serverAddress, uint16 serverPort, Sockets::ProtocolFamily protocolFamily = Sockets::ProtocolFamily::IPv4);

	bool sendData(const uint8* data, size_t length);
	bool sendData(const std::vector<uint8>& data);

	bool receiveBlocking(ReceiveResult& outReceiveResult);
	bool receiveNonBlocking(ReceiveResult& outReceiveResult);


private:
	bool receiveInternal(ReceiveResult& outReceiveResult);

private:
	struct Internal;
	Internal* mInternal = nullptr;
};



class UDPSocket
{
public:
	static const constexpr size_t MAX_DATAGRAM_SIZE = 0x8000;	// That's 32 KB (the actual limit is somewhat close to 64 KB, but let's play safe here)

	struct ReceiveResult
	{
		std::vector<uint8> mBuffer;
		SocketAddress mSenderAddress;
	};

public:
	~UDPSocket();

	bool isValid() const;
	void close();

	bool bindToPort(uint16 port, Sockets::ProtocolFamily protocolFamily = Sockets::ProtocolFamily::IPv4);
	bool bindToAnyPort(Sockets::ProtocolFamily protocolFamily = Sockets::ProtocolFamily::IPv4);

	bool sendData(const uint8* data, size_t length, const SocketAddress& destinationAddress);
	bool sendData(const std::vector<uint8>& data, const SocketAddress& destinationAddress);

	bool receiveBlocking(ReceiveResult& outReceiveResult);
	bool receiveNonBlocking(ReceiveResult& outReceiveResult);

	// Finding a friend in the same network: the host answers requests for its session, the friend sends requests to everybody nearby
	void setLanHostSession(uint64 sessionID);
	void setLanSearchSession(uint64 sessionID);
	void clearLan();
	bool sendLanSearchRequest(uint16 port);
	bool takeLanFoundAddress(SocketAddress& outAddress);
	bool hasLanRequester(const std::string& ip) const;
	// Remembers an address in a most-recently-seen list of at most "capacity" entries: a repeated address moves to the end, a full list drops its oldest entry
	// (so that a stream of other addresses cannot keep a real friend out for as long as the program runs)
	static void rememberRecentAddress(std::vector<std::string>& list, const std::string& ip, size_t capacity);
	inline bool consumedLanPacket()  { const bool result = mConsumedLanPacket;  mConsumedLanPacket = false;  return result; }	// The last receive only handled a LAN discovery packet inside the socket
	static std::vector<std::string> getSubnetBroadcastAddresses();		// Broadcast addresses of the network adapters (empty on platforms where this is not implemented)		// Somebody on the network asked for this host's session code (and got an answer)

	// Relay server (see RelayProtocol.h): all datagrams to and from the other side go through the relay, which makes a connection possible when a direct one is not.
	// The peers on the other side get made-up addresses (240.0.x.y); everything above this class works with them like with any other address.
	// DIRECTORY: a socket of its own that only talks to the lobby directory of the relay server (see DirectoryProtocol.h): nothing is registered or joined, the directory datagrams are
	// handed to the caller unchanged (sender = the relay's address)
	enum class RelayRole { NONE, HOST, CLIENT, DIRECTORY };
	void startRelay(RelayRole role, const SocketAddress& relayAddress, uint64 lobbyID, uint64 key);
	void stopRelay();
	void updateRelay();		// Call regularly: (re)sends the registration or join message
	inline RelayRole getRelayRole() const  { return mRelayRole; }
	inline uint64 getRelayLobby() const  { return mRelayLobby; }
	inline uint64 getRelayKey() const  { return mRelayKey; }
	inline uint64 getRelayHostSecret() const  { return mRelayHostSecret; }		// Host: lets the lobby directory know that the publishing game is the one that holds the relay lobby
	bool isTransportOpen() const;		// Browser build: the WebSocket to the relay is open. Elsewhere: always true while the socket is valid.
	inline bool isRelayReady() const  { return mRelayReady; }		// Host: the relay knows the lobby. Friend: the relay let us in.
	inline int getRelayRefusal() const  { return mRelayRefusal; }	// 0 = none, 1 = no host registered at the relay, 2 = lobby full
	inline uint32 getRelayRejoinCount() const  { return mRelayRejoinCount; }		// Friend: how often the relay gave us a new place (a different token than before: the old place was gone, e.g. after a lost WebSocket), so that the connection through it has to be started again
	inline uint64 getRelayDatagramsIn() const  { return mRelayDatagramsIn; }
	inline uint64 getRelayDatagramsOut() const  { return mRelayDatagramsOut; }

	// Direct connections between the players (WebRTC data channels, browser build only so far, see DirectLink.h): an optional shortcut around the relay.
	// Signalling goes through the relay; as soon as a direct channel works the datagrams to that peer use it, and when it fails they use the relay again.
	// Set before startRelay(). "iceServers" is a comma separated list of STUN URLs.
	static void setDirectConnectionConfig(bool enabled, const std::string& iceServers)  { mDirectEnabled = enabled;  mDirectIceServers = iceServers; }
	static inline bool isDirectConnectionEnabled()  { return mDirectEnabled; }
	// Direct connections in THIS lobby: switched off in a lobby on the public list, whose players may be strangers (a direct connection shows each player's IP address
	// to the other one; the relay shows it to nobody but its operator). Switching it off closes the direct channels at once and ignores the other side's signalling;
	// the switch holds until stopRelay(), so the next lobby starts allowed again.
	void setDirectAllowedInLobby(bool allowed);
	inline bool isDirectAllowedInLobby() const  { return mDirectAllowedInLobby; }
	bool isDirectTo(const SocketAddress& peerAddress) const;		// True while datagrams to this relayed peer go over a direct channel
	inline bool isDirectActive() const  { return mDirectSupported; }	// The platform can do direct connections and they are switched on
	inline uint64 getDirectDatagramsIn() const  { return mDirectDatagramsIn; }
	inline uint64 getDirectDatagramsOut() const  { return mDirectDatagramsOut; }
	int getDirectPeerCount() const;		// How many peers are reached directly right now

	// Developer test: pretend to have a bad connection (environment variables S3AIR_NET_LAG in ms and S3AIR_NET_LOSS in percent)
	void flushDelayedPackets();

	// Browser build: a browser has no UDP sockets. The relay datagrams (see RelayProtocol.h) travel in binary WebSocket messages to the relay server instead
	// (the same relay program speaks UDP to native games and WebSocket to browsers, so both can be in one lobby), and nothing else can be sent or received.
	// The URL is "ws://host:port" or "wss://host:port"; it has to be set before startRelay(). Outside the browser build it is ignored.
	static void setRelayWebSocketURL(const std::string& url)  { mRelayWebSocketURL = url; }
	static inline bool isWebSocketTransport()
	{
	#ifdef __EMSCRIPTEN__
		return true;
	#else
		return false;
	#endif
	}

	// Windows game: a relay server written as ws://... or wss://... (NetplayLobby::isWebSocketAddress; a free web host takes no UDP) is reached by a WebSocket as well,
	// so that Windows and browser players share its lobbies. startRelay() is then given this made-up relay address, and the URL is the one of setRelayWebSocketURL() at
	// that moment (each socket keeps its own). LAN discovery and direct UDP connections are not touched.
	static constexpr const char* WEBSOCKET_RELAY_HOST = "websocket.relay";
	static bool isNativeWebSocketSupported();
	inline bool isRelayOverWebSocket() const  { return mRelayOverWebSocket; }

private:
	static inline std::string mRelayWebSocketURL;
	static inline bool mDirectEnabled = true;
	static inline std::string mDirectIceServers;
	void updateDirect();
	bool updateNativeWebSocket();		// Windows game, relay over a WebSocket: (re)connects; false while the connection is not open
	bool receiveFromNativeWebSocket(ReceiveResult& outReceiveResult);
	bool receiveInternal(ReceiveResult& outReceiveResult);
	bool handleLanPacket(ReceiveResult& outReceiveResult);
	bool sendDataNow(const uint8* data, size_t length, const SocketAddress& destinationAddress);
	bool sendViaRelay(const uint8* data, size_t length, const SocketAddress& destinationAddress);
	bool handleRelayPacket(ReceiveResult& outReceiveResult);

private:
	struct Internal;
	Internal* mInternal = nullptr;

	RelayRole mRelayRole = RelayRole::NONE;
	SocketAddress mRelayAddress;
	uint64 mRelayLobby = 0;
	uint64 mRelayKey = 0;
	uint64 mRelayHostSecret = 0;		// Host only: random per hosting, proves to the relay that this game is the one that registered the lobby (see RelayProtocol.h)
	uint32 mRelayRejoinCount = 0;
	uint32 mRelayToken = 0;			// Host: the host token from the relay. Friend: our token
	bool mRelayReady = false;
	int mRelayRefusal = 0;
	double mRelayLastHello = -1000.0;
	uint64 mRelayDatagramsIn = 0;
	uint64 mRelayDatagramsOut = 0;
	bool mDirectSupported = false;
	bool mDirectAllowedInLobby = true;
	bool mRelayOverWebSocket = false;		// Windows game: the relay of this socket is reached by a WebSocket (see WEBSOCKET_RELAY_HOST)
	std::string mRelayOverWebSocketURL;
	double mWebSocketNextConnect = 0.0;
	double mWebSocketBackoff = 0.5;
	bool mWebSocketWasStarted = false;
	uint64 mDirectDatagramsIn = 0;
	uint64 mDirectDatagramsOut = 0;
	uint32 mDirectRejoinSeen = 0;
	int mDirectFailures = 0;			// Consecutive failed attempts of the friend (slows the retries down)
	double mDirectLastStart = -1000.0;
	bool mDirectWorkedSinceStart = false;
	int mDirectLastStates[17] = {};	// Last observed DirectLink::State per peer number (for the log lines)

	uint64 mLanHostSession = 0;
	std::vector<std::string> mLanRequesterIPs;
	uint64 mLanSearchSession = 0;
	bool mLanFound = false;
	bool mConsumedLanPacket = false;
	double mLanReplyWindowStart = 0.0;
	int mLanRepliesInWindow = 0;
	SocketAddress mLanFoundAddress;

	struct DelayedPacket
	{
		double mSendTime = 0.0;
		std::vector<uint8> mData;
		SocketAddress mDestination;
	};
	std::vector<DelayedPacket> mDelayedPackets;
};
