/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen_netcore/pch.h"
#include "oxygen_netcore/network/LobbyDirectory.h"

#include <chrono>

namespace
{
	const float SLOW_HELLO_SECONDS = 8.0f;		// While the server is unreachable, it is asked again this often (so that it works as soon as it is back)
}


LobbyDirectory::~LobbyDirectory()
{
	stop();
}

double LobbyDirectory::now()
{
	return (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
}

void LobbyDirectory::start(const SocketAddress& serverAddress)
{
	if (mStarted)
		stop();
	if (!mSocket.bindToAnyPort())
	{
		mStatus = Status::UNREACHABLE;
		return;
	}
	mServerAddress = serverAddress;
	mSocket.startRelay(UDPSocket::RelayRole::DIRECTORY, serverAddress, 0, 0);
	mStarted = true;
	mStatus = Status::CONNECTING;
	mCookie = 0;
	mCookieAge = 0.0f;
	mHelloPending = true;
	mHelloTimer = 0.0f;
	mHelloTries = 0;
	mSecondsStarted = 0.0f;
	mSlowHelloTimer = 0.0f;
	mSecondsNotReady = 0.0f;
	mTransportWasOpen = false;
	mPingMs = -1;
}

void LobbyDirectory::stop()
{
	if (mStarted && mPublishing && mPublishedOk)
		stopPublishing(true);
	mPublishing = false;
	mPublishedOk = false;
	mPublishedCode.clear();
	mStarted = false;
	mStatus = Status::OFF;
	// A request that was waiting ends as failed (never as "idle", which nobody waits for: the menu would hang until its own timeout)
	if (mListState == RequestState::PENDING)
		mListState = RequestState::FAILED;
	if (mResolveState == RequestState::PENDING)
		mResolveState = RequestState::FAILED;
	mList.clear();
	mListBuilding.clear();
	mListTotal = 0;
	mListAge = 1.0e9f;
	mCookie = 0;
	mSocket.stopRelay();
	mSocket.close();
}

void LobbyDirectory::send(const std::vector<uint8>& data)
{
	if (!mStarted)
		return;
	if (mSocket.sendData(data, mServerAddress))
		++mSent;
}

void LobbyDirectory::sendHello()
{
	std::vector<uint8> data;
	DirectoryProtocol::buildHello(data);
	mHelloSentAt = now();
	send(data);
}

void LobbyDirectory::sendListPage()
{
	std::vector<uint8> data;
	DirectoryProtocol::buildList(data, mCookie, mListCompatibleOnly, (uint8)std::min(mListOffset, 255), mListCompat);
	send(data);
}

void LobbyDirectory::sendResolve()
{
	std::vector<uint8> data;
	DirectoryProtocol::buildResolve(data, mCookie, mResolveCode);
	send(data);
}

void LobbyDirectory::sendPublish()
{
	std::vector<uint8> data;
	DirectoryProtocol::buildPublish(data, mPublishInfo);
	send(data);
}

void LobbyDirectory::requestList(bool compatibleOnly, const std::string& compat)
{
	mListCompatibleOnly = compatibleOnly;
	mListCompat = compat;
	mListBuilding.clear();
	mListOffset = 0;
	mListTries = 0;
	mListTimer = 0.0f;
	mListState = RequestState::PENDING;
}

void LobbyDirectory::requestResolve(const std::string& code)
{
	mResolveCode = code;
	mResolveTries = 0;
	mResolveTimer = 0.0f;
	mResolveReply = DirectoryProtocol::ResolveReply();
	mResolveState = RequestState::PENDING;
}

void LobbyDirectory::startPublishing(const DirectoryProtocol::PublishInfo& info)
{
	mPublishInfo = info;
	mPublishing = true;
	mPublishedOk = false;
	mPublishedCode.clear();
	mPublishStatus = -1;
	mPublishTimer = 0.0f;
	mSecondsSincePublishReply = 1000.0f;
	mPublishDirty = false;
}

void LobbyDirectory::updatePublishInfo(const DirectoryProtocol::PublishInfo& info)
{
	if (!mPublishing)
		return;
	const bool changed = (info.mPlayers != mPublishInfo.mPlayers || info.mMaxPlayers != mPublishInfo.mMaxPlayers || info.mInGame != mPublishInfo.mInGame
						  || info.mPublic != mPublishInfo.mPublic || info.mName != mPublishInfo.mName || info.mHostName != mPublishInfo.mHostName);
	mPublishInfo = info;
	if (changed)
		mPublishDirty = true;
}

void LobbyDirectory::stopPublishing(bool tellServer)
{
	if (mStarted && mPublishing && tellServer && mPublishInfo.mRelayLobby != 0)
	{
		std::vector<uint8> data;
		DirectoryProtocol::buildUnpublish(data, mPublishInfo.mRelayLobby, mPublishInfo.mRelayKey, mPublishInfo.mHostSecret);
		send(data);
	}
	mPublishing = false;
	mPublishedOk = false;
	mPublishedCode.clear();
}

void LobbyDirectory::update(float deltaSeconds)
{
	if (!mStarted)
		return;

	mSocket.updateRelay();		// (browser build: reconnects the WebSocket)
	mSecondsStarted += deltaSeconds;
	mSecondsNotReady = (mStatus == Status::READY) ? 0.0f : (mSecondsNotReady + deltaSeconds);
	mCookieAge += deltaSeconds;

	// A connection that came up (or came back, in the browser build) is a new chance for the server to answer: ask at once, and give it its full number of tries again
	{
		const bool open = mSocket.isTransportOpen();
		if (open && !mTransportWasOpen)
		{
			mHelloTimer = 0.0f;
			mHelloTries = 0;
			if (mStatus == Status::UNREACHABLE)
				mStatus = Status::CONNECTING;
		}
		mTransportWasOpen = open;
	}
	mListAge += deltaSeconds;
	mSecondsSincePublishReply += deltaSeconds;

	// Answers
	for (int i = 0; i < 32; ++i)
	{
		UDPSocket::ReceiveResult received;
		if (!mSocket.receiveNonBlocking(received) || received.mBuffer.empty())
			break;
		if (!(received.mSenderAddress == mServerAddress))
			continue;
		++mReceived;
		handle(received.mBuffer);
	}

	// Hello: the first answer makes the server READY; later ones renew the cookie
	const bool needHello = (mStatus == Status::CONNECTING || mStatus == Status::UNREACHABLE || (mStatus == Status::READY && mCookieAge > COOKIE_REFRESH_SECONDS));
	if (needHello)
	{
		mHelloTimer -= deltaSeconds;
		if (mHelloTimer <= 0.0f)
		{
			if (mStatus == Status::CONNECTING && mHelloTries >= MAX_TRIES)
			{
				mStatus = Status::UNREACHABLE;
				mHelloTimer = SLOW_HELLO_SECONDS;
				mHelloTries = 0;
			}
			else
			{
				sendHello();
				++mHelloTries;
				mHelloTimer = (mStatus == Status::UNREACHABLE) ? SLOW_HELLO_SECONDS : RETRY_SECONDS;
			}
		}
	}

	// A request cannot work while the server is unreachable: it fails at once, so that the menu can say so
	// (with patience, a request waits for a server that may be asleep, and is sent by itself when the server answers)
	if (mStatus == Status::UNREACHABLE && !isWaitingForServer())
	{
		if (mListState == RequestState::PENDING)
			mListState = RequestState::FAILED;
		if (mResolveState == RequestState::PENDING)
			mResolveState = RequestState::FAILED;
	}

	if (mStatus == Status::READY && mCookie != 0)
	{
		if (mListState == RequestState::PENDING)
		{
			mListTimer -= deltaSeconds;
			if (mListTimer <= 0.0f)
			{
				if (mListTries >= MAX_TRIES)
				{
					mListState = RequestState::FAILED;
				}
				else
				{
					sendListPage();
					++mListTries;
					mListTimer = RETRY_SECONDS;
				}
			}
		}
		if (mResolveState == RequestState::PENDING)
		{
			mResolveTimer -= deltaSeconds;
			if (mResolveTimer <= 0.0f)
			{
				if (mResolveTries >= MAX_TRIES)
				{
					mResolveState = RequestState::FAILED;
				}
				else
				{
					sendResolve();
					++mResolveTries;
					mResolveTimer = RETRY_SECONDS;
				}
			}
		}
	}

	// Publishing needs no cookie: the relay lobby's key and secret prove who we are
	if (mPublishing)
	{
		mPublishTimer -= deltaSeconds;
		if (mPublishTimer <= 0.0f || (mPublishDirty && mPublishTimer < PUBLISH_SECONDS - 1.0f))
		{
			mPublishDirty = false;
			sendPublish();
			mPublishTimer = PUBLISH_SECONDS;
		}
	}
}

void LobbyDirectory::handle(const std::vector<uint8>& data)
{
	uint8 type = 0;
	if (!RelayProtocol::parseHeader(data.data(), data.size(), type))
		return;

	switch (type)
	{
		case DirectoryProtocol::HELLO_REPLY:
		{
			DirectoryProtocol::HelloReply reply;
			if (!DirectoryProtocol::parseHelloReply(data.data(), data.size(), reply))
				return;
			mCookie = reply.mCookie;
			mCookieAge = 0.0f;
			mVersion = reply.mVersion;
			mCodeDigits = reply.mCodeDigits;
			mServerLobbies = reply.mLobbies;
			mPingMs = (int)((now() - mHelloSentAt) * 1000.0 + 0.5);
			mStatus = Status::READY;
			mHelloTries = 0;
			mHelloTimer = 0.0f;
			break;
		}

		case DirectoryProtocol::LIST_REPLY:
		{
			DirectoryProtocol::ListReply reply;
			if (!DirectoryProtocol::parseListReply(data.data(), data.size(), reply) || mListState != RequestState::PENDING)
				return;
			if (reply.mStatus == DirectoryProtocol::LIST_BAD_COOKIE)
			{
				// The cookie ran out: ask for a new one, then the request goes on
				mCookie = 0;
				mStatus = Status::CONNECTING;
				mHelloTries = 0;
				mHelloTimer = 0.0f;
				mListTries = 0;
				return;
			}
			if (reply.mOffset != (uint8)std::min(mListOffset, 255))
				return;		// not the page that was asked for (a late answer to a repeated request)
			mListBuilding.insert(mListBuilding.end(), reply.mEntries.begin(), reply.mEntries.end());
			mListOffset += (int)reply.mEntries.size();
			if (mListOffset < (int)reply.mTotal && !reply.mEntries.empty() && mListOffset < MAX_LIST_ENTRIES)
			{
				mListTries = 0;
				mListTimer = 0.0f;
			}
			else
			{
				mList = mListBuilding;
				mListTotal = reply.mTotal;
				mListAge = 0.0f;
				mListState = RequestState::DONE;
			}
			break;
		}

		case DirectoryProtocol::RESOLVE_REPLY:
		{
			DirectoryProtocol::ResolveReply reply;
			if (!DirectoryProtocol::parseResolveReply(data.data(), data.size(), reply) || mResolveState != RequestState::PENDING)
				return;
			if (reply.mStatus == DirectoryProtocol::RESOLVE_BAD_COOKIE)
			{
				mCookie = 0;
				mStatus = Status::CONNECTING;
				mHelloTries = 0;
				mHelloTimer = 0.0f;
				mResolveTries = 0;
				return;
			}
			mResolveReply = reply;
			mResolveState = RequestState::DONE;
			break;
		}

		case DirectoryProtocol::PUBLISH_REPLY:
		{
			DirectoryProtocol::PublishReply reply;
			if (!DirectoryProtocol::parsePublishReply(data.data(), data.size(), reply) || !mPublishing)
				return;
			mPublishStatus = reply.mStatus;
			if (reply.mStatus == DirectoryProtocol::PUBLISH_OK)
			{
				mPublishedOk = true;
				mPublishedCode = reply.mCode;
				mSecondsSincePublishReply = 0.0f;
			}
			else
			{
				mPublishedOk = false;
				if (reply.mStatus == DirectoryProtocol::PUBLISH_NOT_REGISTERED)
					mPublishTimer = 1.0f;		// the relay does not know the lobby yet: soon again
			}
			break;
		}

		default:
			break;
	}
}
