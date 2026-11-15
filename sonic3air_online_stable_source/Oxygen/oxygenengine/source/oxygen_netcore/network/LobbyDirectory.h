/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen_netcore/network/Sockets.h"
#include "oxygen_netcore/network/DirectoryProtocol.h"


// Client of the public lobby directory of a relay server (see DirectoryProtocol.h): lists public lobbies, turns a 6 digit lobby code into what is needed to join,
// and publishes the lobby of this game. It has a UDP socket of its own (a WebSocket in the browser build) and never blocks: everything is a request that is
// polled while update() is called every frame, and every request ends with a result or a failure after a few seconds, never with a hang.
class LobbyDirectory
{
public:
	enum class Status
	{
		OFF,			// No lobby server is set
		CONNECTING,		// Asking the server whether it has a directory
		READY,			// The server answered: lists and codes work
		UNREACHABLE		// No answer (yet): wrong address, server down, no connection. It keeps trying, so READY can follow.
	};

	enum class RequestState
	{
		IDLE,
		PENDING,
		DONE,
		FAILED			// No answer from the server
	};

	// A request that is not answered within this long is tried again, up to "MAX_TRIES" times
	static const constexpr float RETRY_SECONDS = 1.5f;
	static const constexpr int MAX_TRIES = 4;
	static const constexpr float COOKIE_REFRESH_SECONDS = 60.0f;
	static const constexpr float PUBLISH_SECONDS = 5.0f;
	static const constexpr int MAX_LIST_ENTRIES = 48;

public:
	~LobbyDirectory();

	// The server's address (already resolved), or nothing to switch the directory off
	void start(const SocketAddress& serverAddress);
	void stop();
	inline bool isStarted() const  { return mStarted; }
	void update(float deltaSeconds);

	inline Status getStatus() const  { return mStatus; }
	inline int getPingMs() const  { return mPingMs; }			// Round trip to the server, -1 until measured
	inline int getListedLobbyCount() const  { return mServerLobbies; }
	inline int getServerCodeDigits() const  { return mCodeDigits; }
	inline int getDirectoryVersion() const  { return mVersion; }

	// Patience with a server that does not answer (0 = none, the default: requests fail as soon as the server counts as unreachable).
	// The browser build sets it: a free web host puts a server to sleep when nobody uses it and needs up to a minute to wake it, so the connection is retried
	// (the WebSocket backs off by itself) and a request waits, instead of failing at once.
	inline void setPatienceSeconds(float seconds)  { mPatienceSeconds = seconds; }
	inline float getSecondsNotReady() const  { return mSecondsNotReady; }	// Seconds since the server last answered (since the start), 0 while it is ready
	// Only with patience: the server does not answer, and the connection to it is not up (or is only just being made): it may be asleep. The seconds run out after the patience.
	inline bool isWaitingForServer() const  { return mPatienceSeconds > 0.0f && mStarted && mStatus != Status::READY && !isServerWithoutDirectory() && mSecondsNotReady < mPatienceSeconds; }
	// Only with patience: the connection to the server is up and it relays, but it never answers the directory (a server older than the lobby list)
	inline bool isServerWithoutDirectory() const  { return mPatienceSeconds > 0.0f && mStarted && mStatus == Status::UNREACHABLE && mSocket.isTransportOpen(); }

	// The list of public lobbies (best first, as the server sorts them). The previous list stays until the new one is complete.
	void requestList(bool compatibleOnly, const std::string& compat);
	inline RequestState getListState() const  { return mListState; }
	inline const std::vector<DirectoryProtocol::ListEntry>& getList() const  { return mList; }
	inline int getListTotal() const  { return mListTotal; }
	inline float getListAgeSeconds() const  { return mListAge; }	// Seconds since the list was complete (a big number if never)

	// A lobby code from the list or from the player: the answer holds the session ID of the lobby
	void requestResolve(const std::string& code);
	inline RequestState getResolveState() const  { return mResolveState; }
	inline const DirectoryProtocol::ResolveReply& getResolveReply() const  { return mResolveReply; }
	inline void clearResolve()  { mResolveState = RequestState::IDLE; }

	// Publishing the lobby of this game: the info is sent now and then every few seconds until stopPublishing()
	void startPublishing(const DirectoryProtocol::PublishInfo& info);
	void updatePublishInfo(const DirectoryProtocol::PublishInfo& info);
	void stopPublishing(bool tellServer);
	inline bool isPublishing() const  { return mPublishing; }
	inline bool isPublished() const  { return mPublishing && mPublishedOk && mSecondsSincePublishReply < 15.0f; }	// The server confirmed the listing recently
	inline const std::string& getPublishedCode() const  { return mPublishedCode; }
	inline int getPublishStatus() const  { return mPublishStatus; }		// The last answer's status (DirectoryProtocol::PublishStatus), -1 = none yet

	// Developer test: how many datagrams went out and came in
	inline uint32 getSentCount() const  { return mSent; }
	inline uint32 getReceivedCount() const  { return mReceived; }

private:
	void send(const std::vector<uint8>& data);
	void sendHello();
	void sendListPage();
	void sendResolve();
	void sendPublish();
	void handle(const std::vector<uint8>& data);
	static double now();

private:
	UDPSocket mSocket;
	SocketAddress mServerAddress;
	bool mStarted = false;
	Status mStatus = Status::OFF;
	uint8 mVersion = 1;
	int mCodeDigits = 6;
	int mServerLobbies = 0;
	int mPingMs = -1;

	// Cookie
	uint64 mCookie = 0;
	float mCookieAge = 0.0f;
	bool mHelloPending = false;
	double mHelloSentAt = 0.0;
	float mHelloTimer = 0.0f;
	int mHelloTries = 0;
	float mSecondsStarted = 0.0f;
	float mSlowHelloTimer = 0.0f;
	float mPatienceSeconds = 0.0f;
	float mSecondsNotReady = 0.0f;
	bool mTransportWasOpen = false;

	// List
	RequestState mListState = RequestState::IDLE;
	bool mListCompatibleOnly = false;
	std::string mListCompat;
	std::vector<DirectoryProtocol::ListEntry> mList;
	std::vector<DirectoryProtocol::ListEntry> mListBuilding;
	int mListTotal = 0;
	float mListAge = 1.0e9f;
	float mListTimer = 0.0f;
	int mListTries = 0;
	int mListOffset = 0;

	// Resolve
	RequestState mResolveState = RequestState::IDLE;
	std::string mResolveCode;
	DirectoryProtocol::ResolveReply mResolveReply;
	float mResolveTimer = 0.0f;
	int mResolveTries = 0;

	// Publish
	bool mPublishing = false;
	bool mPublishedOk = false;
	DirectoryProtocol::PublishInfo mPublishInfo;
	std::string mPublishedCode;
	int mPublishStatus = -1;
	float mPublishTimer = 0.0f;
	float mSecondsSincePublishReply = 1000.0f;
	bool mPublishDirty = false;

	uint32 mSent = 0;
	uint32 mReceived = 0;
};
