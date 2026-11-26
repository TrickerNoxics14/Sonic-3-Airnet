/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// A WebSocket client for the Windows game, so that it can use a relay server that only takes web traffic (a free web host has no UDP): the same relay datagrams
// (RelayProtocol.h, DirectoryProtocol.h) travel in binary WebSocket messages, exactly like the browser build's. That puts Windows and browser players in one lobby.
// Included by Sockets.cpp only. It uses WinHTTP, which is part of Windows (TLS for wss:// included, nothing to install).
//
// Two threads per connection: one connects and then receives (a WinHTTP WebSocket call blocks), one sends from a queue (a send can block while the network stalls,
// and the game's frame must never wait for it). The game's thread only touches the queues. close() never waits for the threads: it tells them to stop, closes the
// handles (which ends a blocked call) and lets them finish on their own; they keep the shared state alive until then.

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include <winhttp.h>
#pragma comment (lib, "winhttp.lib")

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NativeWebSocket
{
public:
	enum class State { CLOSED, CONNECTING, OPEN };
	static const constexpr size_t MAX_MESSAGE_SIZE = 0x8000;	// The relay never sends more than a datagram (UDPSocket::MAX_DATAGRAM_SIZE)

	~NativeWebSocket()  { close(); }

	// Splits "ws://host[:port][/path]" or "wss://..." (already checked by NetplayLobby::normalizeRelayWebSocketURL)
	static bool splitURL(const std::string& url, bool& outSecure, std::wstring& outHost, uint16& outPort, std::wstring& outPath)
	{
		size_t start = 0;
		if (url.compare(0, 6, "wss://") == 0)		{ outSecure = true;  start = 6; }
		else if (url.compare(0, 5, "ws://") == 0)	{ outSecure = false; start = 5; }
		else
			return false;
		const size_t pathStart = url.find_first_of("/?", start);
		const std::string hostPort = url.substr(start, (pathStart == std::string::npos) ? std::string::npos : pathStart - start);
		std::string path = (pathStart == std::string::npos) ? std::string("/") : url.substr(pathStart);
		if (!path.empty() && path[0] == '?')
			path = "/" + path;
		const size_t colon = hostPort.find(':');
		const std::string host = hostPort.substr(0, colon);
		int port = outSecure ? 443 : 80;
		if (colon != std::string::npos)
			port = std::atoi(hostPort.c_str() + colon + 1);
		if (host.empty() || port < 1 || port > 65535)
			return false;
		outHost.assign(host.begin(), host.end());		// (only plain ASCII gets through the normalizer)
		outPath.assign(path.begin(), path.end());
		outPort = (uint16)port;
		return true;
	}

	// Starts connecting in the background (an old connection is closed first)
	void open(const std::string& url)
	{
		close();
		mShared = std::make_shared<Shared>();
		mShared->mState = State::CONNECTING;
		std::shared_ptr<Shared> shared = mShared;
		std::thread([shared, url]() { runConnection(shared, url); }).detach();
	}

	void close()
	{
		if (!mShared)
			return;
		std::shared_ptr<Shared> shared = std::move(mShared);
		shared->mStop = true;
		shared->mState = State::CLOSED;
		shared->mWakeSender.notify_all();
		closeHandles(*shared);
	}

	inline State getState() const  { return mShared ? mShared->mState.load() : State::CLOSED; }

	bool takeJustOpened()
	{
		return mShared && mShared->mJustOpened.exchange(false);
	}

	// Queues one message (false while not open, or when the queue is full because the connection stalls)
	bool send(const uint8* data, size_t length)
	{
		if (!mShared || mShared->mState != State::OPEN || length == 0 || length > MAX_MESSAGE_SIZE)
			return false;
		{
			std::lock_guard<std::mutex> lock(mShared->mMutex);
			if (mShared->mOutbox.size() >= 1024)
				return false;
			mShared->mOutbox.emplace_back(data, data + length);
		}
		mShared->mWakeSender.notify_one();
		++mMessagesOut;
		return true;
	}

	// The next message that came in (false if none)
	bool receive(std::vector<uint8>& outData)
	{
		if (!mShared)
			return false;
		std::lock_guard<std::mutex> lock(mShared->mMutex);
		if (mShared->mInbox.empty())
			return false;
		outData = std::move(mShared->mInbox.front());
		mShared->mInbox.pop_front();
		return true;
	}

	// What the threads want to say in the log (the game's thread writes it: the log is not made for other threads)
	bool takeLogLine(std::string& outLine)
	{
		if (!mShared)
			return false;
		std::lock_guard<std::mutex> lock(mShared->mMutex);
		if (mShared->mLogLines.empty())
			return false;
		outLine = std::move(mShared->mLogLines.front());
		mShared->mLogLines.pop_front();
		return true;
	}

	inline uint64 getMessagesOut() const  { return mMessagesOut; }

private:
	struct Shared
	{
		std::mutex mMutex;
		std::condition_variable mWakeSender;
		std::atomic<State> mState { State::CLOSED };
		std::atomic<bool> mStop { false };
		std::atomic<bool> mJustOpened { false };
		HINTERNET mSession = nullptr;
		HINTERNET mConnect = nullptr;
		HINTERNET mRequest = nullptr;
		HINTERNET mWebSocket = nullptr;
		std::deque<std::vector<uint8>> mInbox;
		std::deque<std::vector<uint8>> mOutbox;
		std::deque<std::string> mLogLines;
	};

	static void log(Shared& shared, const std::string& line)
	{
		std::lock_guard<std::mutex> lock(shared.mMutex);
		if (shared.mLogLines.size() < 32)
			shared.mLogLines.push_back(line);
	}

	// Closes every handle that is still open (closing one ends a WinHTTP call that blocks on it in another thread)
	static void closeHandles(Shared& shared)
	{
		HINTERNET handles[4];
		{
			std::lock_guard<std::mutex> lock(shared.mMutex);
			handles[0] = shared.mWebSocket;  shared.mWebSocket = nullptr;
			handles[1] = shared.mRequest;    shared.mRequest = nullptr;
			handles[2] = shared.mConnect;    shared.mConnect = nullptr;
			handles[3] = shared.mSession;    shared.mSession = nullptr;
		}
		for (HINTERNET handle : handles)
		{
			if (nullptr != handle)
				WinHttpCloseHandle(handle);
		}
	}

	static void runConnection(std::shared_ptr<Shared> shared, std::string url)
	{
		bool secure = false;
		std::wstring host, path;
		uint16 port = 0;
		if (!splitURL(url, secure, host, port, path))
		{
			log(*shared, "Netplay (WebSocket): the relay server address is not usable");
			shared->mState = State::CLOSED;
			return;
		}
		log(*shared, "Netplay (WebSocket): connecting to the relay server");

		// The connection: proxy settings of Windows, generous waits (a free web host that sleeps needs about a minute before it answers the first request)
		HINTERNET session = WinHttpOpen(L"Sonic3AirNet", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (nullptr == session)
			session = WinHttpOpen(L"Sonic3AirNet", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (nullptr != session)
			WinHttpSetTimeouts(session, 15000, 15000, 15000, 90000);
		HINTERNET connect = (nullptr != session) ? WinHttpConnect(session, host.c_str(), port, 0) : nullptr;
		HINTERNET request = (nullptr != connect) ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
		{
			std::lock_guard<std::mutex> lock(shared->mMutex);
			shared->mSession = session;
			shared->mConnect = connect;
			shared->mRequest = request;
		}
		bool ok = (nullptr != request) && !shared->mStop
			&& WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
			&& WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			&& WinHttpReceiveResponse(request, nullptr);
		DWORD status = 0;
		if (ok)
		{
			DWORD size = sizeof(status);
			ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 101;
		}
		HINTERNET webSocket = ok ? WinHttpWebSocketCompleteUpgrade(request, 0) : nullptr;
		bool ownRequest = false;
		bool storedWebSocket = false;
		{
			// Each handle is closed by exactly one side: whoever takes it out of the shared state (close() may have taken them already)
			std::lock_guard<std::mutex> lock(shared->mMutex);
			ownRequest = (nullptr != request && shared->mRequest == request);
			if (ownRequest)
				shared->mRequest = nullptr;
			if (nullptr != webSocket && !shared->mStop)
			{
				shared->mWebSocket = webSocket;
				storedWebSocket = true;
			}
		}
		if (ownRequest)
			WinHttpCloseHandle(request);		// (the WebSocket handle lives on its own)
		if (!storedWebSocket)
		{
			if (nullptr != webSocket)
				WinHttpCloseHandle(webSocket);
			if (!shared->mStop)
			{
				const DWORD error = GetLastError();
				log(*shared, (status != 0 && status != 101) ? ("Netplay (WebSocket): the relay server refused the connection (HTTP " + std::to_string(status) + ")")
															: ("Netplay (WebSocket): could not connect to the relay server (error " + std::to_string(error) + ")"));
			}
			shared->mState = State::CLOSED;
			closeHandles(*shared);
			return;
		}

		// No time limit for waiting on a message of the open connection (a quiet lobby is no error); a dead connection shows up as a failed send instead,
		// which closes the handles and so ends the waiting receive
		DWORD noTimeout = 0;
		WinHttpSetOption(webSocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &noTimeout, sizeof(noTimeout));

		shared->mState = State::OPEN;
		shared->mJustOpened = true;
		log(*shared, "Netplay (WebSocket): connected to the relay server");

		// The sender: one message after the other, in the order the game queued them
		std::thread([shared, webSocket]()
		{
			std::vector<uint8> message;
			while (true)
			{
				{
					std::unique_lock<std::mutex> lock(shared->mMutex);
					shared->mWakeSender.wait(lock, [&shared]() { return shared->mStop || shared->mState != State::OPEN || !shared->mOutbox.empty(); });
					if (shared->mStop || shared->mState != State::OPEN || shared->mWebSocket != webSocket)
						return;
					message = std::move(shared->mOutbox.front());
					shared->mOutbox.pop_front();
				}
				if (WinHttpWebSocketSend(webSocket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, message.data(), (DWORD)message.size()) != NO_ERROR)
				{
					// A broken connection: closing the handles ends the receiver's wait, which then reports it and marks the connection closed (the game opens a new one)
					closeHandles(*shared);
					return;
				}
			}
		}).detach();

		// The receiver: binary messages go to the inbox (a message may arrive in fragments); the relay's pings are answered by WinHTTP itself
		std::vector<uint8> buffer(MAX_MESSAGE_SIZE + 1024);
		size_t filled = 0;
		std::string why = "closed by the server";
		while (!shared->mStop)
		{
			DWORD bytesRead = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
			const DWORD error = WinHttpWebSocketReceive(webSocket, buffer.data() + filled, (DWORD)(buffer.size() - filled), &bytesRead, &type);
			if (error == ERROR_WINHTTP_TIMEOUT)
				continue;
			if (error != NO_ERROR)
			{
				why = "error " + std::to_string(error);
				break;
			}
			if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
				break;
			filled += bytesRead;
			if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
			{
				if (filled >= buffer.size())
					filled = 0;		// (too long for a datagram: dropped, like the browser build drops it)
				continue;
			}
			if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && filled > 0 && filled <= MAX_MESSAGE_SIZE)
			{
				std::lock_guard<std::mutex> lock(shared->mMutex);
				if (shared->mInbox.size() >= 4096)
					shared->mInbox.pop_front();		// a stalled game must not collect messages without end
				shared->mInbox.emplace_back(buffer.data(), buffer.data() + filled);
			}
			filled = 0;
		}
		if (!shared->mStop)
			log(*shared, "Netplay (WebSocket): the connection to the relay server closed (" + why + ")");
		shared->mState = State::CLOSED;
		shared->mWakeSender.notify_all();
		closeHandles(*shared);
	}

private:
	std::shared_ptr<Shared> mShared;
	uint64 mMessagesOut = 0;
};

#endif
