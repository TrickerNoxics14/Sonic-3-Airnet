/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/client/SelfUpdater.h"
#include "oxygen_netcore/base/TestHooks.h"
#include "oxygen/application/Configuration.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#if defined(PLATFORM_WINDOWS)

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <windows.h>

#define CURL_STATICLIB
#include <curl/curl.h>
#pragma comment(lib, "libcurl.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wldap32.lib")
#pragma comment(lib, "crypt32.lib")

#pragma comment(lib, "minizip.lib")
#include "unzip.h"
#include "iowin32.h"


namespace fs = std::filesystem;
using namespace selfupdate;

namespace
{
	const char* DEFAULT_REPO = "TrickerNoxics14/Sonic-3-Airnet";
	const char* const HELPER_SCRIPT_NAME = "update_helper.ps1";
	const uint64_t MAX_TOTAL_UNPACKED = 1600ull * 1024 * 1024;
	const size_t MAX_ENTRIES = 20000;
	const uint64_t MAX_API_BODY = 1024 * 1024;

	std::string wideToUtf8(const std::wstring& wide)
	{
		if (wide.empty())
			return std::string();
		const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
		std::string result((size_t)std::max(size, 0), '\0');
		if (size > 0)
			WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &result[0], size, nullptr, nullptr);
		return result;
	}

	bool utf8ToWide(const std::string& utf8, std::wstring& outWide)
	{
		outWide.clear();
		if (utf8.empty())
			return true;
		const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), (int)utf8.size(), nullptr, 0);
		if (size <= 0)
			return false;
		outWide.assign((size_t)size, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), (int)utf8.size(), &outWide[0], size);
		return true;
	}

	std::string formatMegabytes(uint64_t bytes)
	{
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
		return buffer;
	}

	bool endsWith(const std::string& text, const std::string& suffix)
	{
		return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// HTTP(S) through libcurl, with the redirects followed by hand: GitHub answers a release download with a 302 to its CDN, and only
	// https addresses of github.com or githubusercontent.com are followed (the loopback test server only in test mode).
	// ---------------------------------------------------------------------------------------------------------------------
	struct Sink
	{
		// Either memory or a file
		std::string* mMemory = nullptr;
		FILE* mFile = nullptr;
		Sha256* mSha = nullptr;
		uint64_t mLimit = 0;				// maximum number of body bytes
		uint64_t mBytes = 0;
		bool mOverLimit = false;
		bool mWriteFailed = false;

		// Filled by the header callback
		long mStatus = 0;
		std::string mLocation;
		bool mRateLimitExhausted = false;

		std::atomic<bool>* mCancel = nullptr;
		std::atomic<uint64_t>* mProgress = nullptr;
		std::atomic<uint64_t>* mTotal = nullptr;
	};

	size_t headerCallback(char* buffer, size_t size, size_t count, void* userData)
	{
		Sink& sink = *(Sink*)userData;
		const size_t length = size * count;
		std::string line(buffer, length);
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();

		if (line.compare(0, 5, "HTTP/") == 0)
		{
			// A new response (also after a redirect): forget what the last one said
			const size_t space = line.find(' ');
			sink.mStatus = (space == std::string::npos) ? 0 : atol(line.c_str() + space + 1);
			sink.mLocation.clear();
			sink.mBytes = 0;
		}
		else
		{
			const size_t colon = line.find(':');
			if (colon != std::string::npos)
			{
				const std::string name = toLowerAscii(line.substr(0, colon));
				std::string value = line.substr(colon + 1);
				while (!value.empty() && value.front() == ' ')
					value.erase(0, 1);
				if (name == "location")
					sink.mLocation = value;
				else if (name == "x-ratelimit-remaining" && value == "0")
					sink.mRateLimitExhausted = true;
			}
		}
		return length;
	}

	size_t writeCallback(char* data, size_t size, size_t count, void* userData)
	{
		Sink& sink = *(Sink*)userData;
		const size_t length = size * count;
		if (sink.mStatus != 200)
			return length;		// error pages and redirect bodies are not kept

		if (sink.mBytes + length > sink.mLimit)
		{
			sink.mOverLimit = true;
			return 0;
		}
		if (nullptr != sink.mMemory)
		{
			sink.mMemory->append(data, length);
		}
		else if (nullptr != sink.mFile)
		{
			if (fwrite(data, 1, length, sink.mFile) != length)
			{
				sink.mWriteFailed = true;
				return 0;
			}
			if (nullptr != sink.mSha)
				sink.mSha->update(data, length);
		}
		sink.mBytes += length;
		if (nullptr != sink.mProgress)
			sink.mProgress->store(sink.mBytes);
		return length;
	}

	int progressCallback(void* userData, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
	{
		Sink& sink = *(Sink*)userData;
		if (nullptr != sink.mCancel && sink.mCancel->load())
			return 1;
		if (nullptr != sink.mTotal && total > 0 && sink.mStatus == 200)
			sink.mTotal->store((uint64_t)total);
		return 0;
	}

	bool isAcceptedRedirect(const std::string& location, const Endpoints& endpoints)
	{
		ParsedUrl url;
		if (!parseUrl(location, url))
			return false;
		if (url.mScheme == "https" && url.mPort.empty() && (url.mHost == "github.com" || endsWith(url.mHost, ".githubusercontent.com") || url.mHost == "githubusercontent.com"))
			return true;
		if (endpoints.mTestMode && url.mScheme == "http" && url.mHost == "127.0.0.1")
		{
			ParsedUrl base;
			return parseUrl(endpoints.mApiBase, base) && base.mPort == url.mPort;
		}
		return false;
	}

	struct FetchResult
	{
		bool mOk = false;			// HTTP 200 and the body is complete
		bool mCancelled = false;
		long mStatus = 0;
		std::string mError;			// plain language, if not ok
	};

	FetchResult fetch(const std::string& startUrl, const Endpoints& endpoints, Sink& sink, bool isApi, const std::string& userAgent)
	{
		FetchResult result;
		std::string url = startUrl;
		for (int hop = 0; hop < 6; ++hop)
		{
			if (hop > 0 && !isAcceptedRedirect(url, endpoints))
			{
				result.mError = "The server redirected the download to an address that is not accepted.";
				return result;
			}

			CURL* curl = curl_easy_init();
			if (nullptr == curl)
			{
				result.mError = "The network library could not be started.";
				return result;
			}
			sink.mStatus = 0;
			sink.mLocation.clear();
			sink.mBytes = 0;
			sink.mOverLimit = false;
			if (nullptr != sink.mMemory)
				sink.mMemory->clear();

			curl_slist* headers = nullptr;
			headers = curl_slist_append(headers, isApi ? "Accept: application/vnd.github+json" : "Accept: application/octet-stream");
			if (isApi)
				headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");

			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
			curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, endpoints.mTestMode ? "http,https" : "https");
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
			curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);		// less than 1 KB/s for 30 s counts as stalled
			curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
			if (isApi)
				curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &headerCallback);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sink);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &progressCallback);
			curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);

			const CURLcode code = curl_easy_perform(curl);
			long status = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			result.mStatus = status;

			if (code == CURLE_ABORTED_BY_CALLBACK && sink.mCancel != nullptr && sink.mCancel->load())
			{
				result.mCancelled = true;
				result.mError = "Cancelled.";
				return result;
			}
			if (sink.mOverLimit)
			{
				result.mError = "The download is larger than allowed and was stopped.";
				return result;
			}
			if (sink.mWriteFailed)
			{
				result.mError = "The downloaded data could not be written to the disk (is it full?).";
				return result;
			}
			if (code != CURLE_OK)
			{
				switch (code)
				{
					case CURLE_COULDNT_RESOLVE_HOST:
					case CURLE_COULDNT_RESOLVE_PROXY:
					case CURLE_COULDNT_CONNECT:
						result.mError = "No connection to the update server. Are you offline?";
						break;
					case CURLE_OPERATION_TIMEDOUT:
						result.mError = "The update server did not answer in time (or the download stalled).";
						break;
					case CURLE_PARTIAL_FILE:
					case CURLE_RECV_ERROR:
					case CURLE_GOT_NOTHING:
						result.mError = "The download was cut off before it was complete (partial download).";
						break;
					case CURLE_SSL_CONNECT_ERROR:
					case CURLE_PEER_FAILED_VERIFICATION:
					case CURLE_SSL_CACERT_BADFILE:
						result.mError = "The secure connection to the update server failed.";
						break;
					default:
						result.mError = std::string("The network request failed (") + curl_easy_strerror(code) + ").";
						break;
				}
				return result;
			}

			if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
			{
				if (sink.mLocation.empty())
				{
					result.mError = "The server sent a redirect without a target.";
					return result;
				}
				url = sink.mLocation;
				continue;
			}
			if (status != 200)
			{
				result.mError = describeHttpStatus(status, sink.mRateLimitExhausted);
				return result;
			}
			result.mOk = true;
			return result;
		}
		result.mError = "Too many redirects.";
		return result;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	void removeTreeChecked(const fs::path& path, const wchar_t* requiredLeaf)
	{
		// Only ever removes the updater's own working folders (the leaf name is checked)
		std::error_code ec;
		if (path.filename() != requiredLeaf)
			return;
		fs::remove_all(path, ec);
	}

	std::string readSmallTextFile(const fs::path& path, size_t maxSize = 4096)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return std::string();
		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (text.size() > maxSize)
			text.resize(maxSize);
		return text;
	}

	std::string firstLine(const std::string& text)
	{
		const size_t end = text.find_first_of("\r\n");
		return (end == std::string::npos) ? text : text.substr(0, end);
	}

	struct StagedFile
	{
		std::string mPath;
		std::string mSha256;
		uint64_t mSize = 0;
	};

	struct ZipEntryInfo
	{
		std::string mRawName;
		std::string mPath;		// normalized, without the package folder
		uint64_t mSize = 0;
		bool mIsDirectory = false;
	};

	std::string quotePath(const std::wstring& path)
	{
		std::wstring text = path;
		std::replace(text.begin(), text.end(), L'/', L'\\');
		while (!text.empty() && (text.back() == L'\\' || text.back() == L'/'))
			text.pop_back();
		return "\"" + wideToUtf8(text) + "\"";
	}
}


SelfUpdater& SelfUpdater::instance()
{
	static SelfUpdater instance;
	return instance;
}

SelfUpdater::SelfUpdater()
{
	Configuration& config = Configuration::instance();
	mRepo = config.mOnlineUpdateRepo.empty() ? std::string(DEFAULT_REPO) : config.mOnlineUpdateRepo;
	mAppDataDirectory = config.mAppDataPath;

	// Developer test switches (honoured only with the test marker file next to the executable)
	if (const char* api = TestHooks::getEnv("S3AIR_UPDATE_API"))
	{
		mEndpoints.mApiBase = api;
		mEndpoints.mTestMode = true;
	}
	if (const char* repo = TestHooks::getEnv("S3AIR_UPDATE_REPO"))
		mRepo = repo;
	if (const char* mode = TestHooks::getEnv("S3AIR_UPDATE_AUTO"))
		mAutoMode = mode;

	wchar_t exePath[MAX_PATH * 2] = {};
	GetModuleFileNameW(nullptr, exePath, (DWORD)(sizeof(exePath) / sizeof(exePath[0]) - 1));
	const fs::path exe(exePath);
	mExeDirectory = exe.parent_path().wstring();
	mExeName = exe.filename().wstring();

	// The version of this copy of the game lies next to the executable (written into the package by release.ps1)
	const std::string versionText = readSmallTextFile(fs::path(mExeDirectory) / L"version.txt", 64);
	if (parseVersion(firstLine(versionText), mCurrentVersion))
		mCurrentVersionText = versionToString(mCurrentVersion);

	if (!mCurrentVersion.mValid)
	{
		mState = (int)State::DISABLED;
		mMessage = "This copy of the game has no version information (a development build), so updates are switched off.";
	}
	else if (_wcsicmp(mExeName.c_str(), L"Sonic3AIR.exe") != 0)
	{
		// The update package contains "Sonic3AIR.exe" and the install helper looks for the program under the running name: a renamed program would fail after the whole download
		mState = (int)State::DISABLED;
		mMessage = "Updates only work when the game program is named Sonic3AIR.exe (it is named " + WString(mExeName).toStdString() + "). Rename it back to update.";
	}
	else if (!isValidRepoName(mRepo))
	{
		mState = (int)State::DISABLED;
		mMessage = "The setting Online / UpdateRepo in settings.json is not a valid repository name (owner/name).";
	}
	else if (!isAcceptedApiBase(mEndpoints))
	{
		mState = (int)State::DISABLED;
		mMessage = "The update address is not accepted.";
	}

	mLastResult = firstLine(readSmallTextFile(fs::path(mAppDataDirectory) / L"update" / L"last_result.txt"));
}

SelfUpdater::~SelfUpdater()
{
	mCancel = true;
	joinWorker();
}

void SelfUpdater::joinWorker()
{
	if (mWorker.joinable())
		mWorker.join();
}

bool SelfUpdater::isBusy() const
{
	const State state = (State)mState.load();
	return state == State::CHECKING || state == State::DOWNLOADING || state == State::VERIFYING || state == State::STAGING;
}

void SelfUpdater::setState(State state, const std::string& message)
{
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mMessage = message;
	}
	mState = (int)state;
}

void SelfUpdater::fail(const std::string& message)
{
	log("FAILED: " + message);
	setState(State::FAILED, message);
}

void SelfUpdater::log(const std::string& text)
{
	RMX_LOG_INFO("Update: " << text);
	try
	{
		const fs::path directory = fs::path(mAppDataDirectory) / L"update";
		std::error_code ec;
		fs::create_directories(directory, ec);
		const fs::path logPath = directory / L"update_log.txt";
		static bool first = true;
		std::ios::openmode mode = std::ios::app;
		if (first)
		{
			first = false;
			if (fs::exists(logPath, ec) && fs::file_size(logPath, ec) > 200 * 1024)
				mode = std::ios::trunc;
		}
		std::ofstream file(logPath, mode);
		const time_t now = time(nullptr);
		char stamp[32] = {};
		tm local = {};
		localtime_s(&local, &now);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
		file << stamp << "  " << text << "\n";
	}
	catch (...) {}
}

SelfUpdater::Snapshot SelfUpdater::getSnapshot()
{
	Snapshot snapshot;
	snapshot.mState = (State)mState.load();
	snapshot.mBytesDone = mBytesDone.load();
	snapshot.mBytesTotal = mBytesTotal.load();
	std::lock_guard<std::mutex> lock(mMutex);
	snapshot.mMessage = mMessage;
	snapshot.mCurrentVersion = mCurrentVersionText;
	if (mHasRelease)
	{
		snapshot.mLatestVersion = versionToString(mRelease.mVersion);
		snapshot.mNotes = mRelease.mNotes;
	}
	snapshot.mLastResult = mLastResult;
	return snapshot;
}

const char* SelfUpdater::getButtonText()
{
	switch ((State)mState.load())
	{
		case State::UPDATE_AVAILABLE:	return "Update now";
		case State::DOWNLOADING:
		case State::VERIFYING:
		case State::STAGING:			return "Cancel";
		case State::READY_TO_INSTALL:	return "Close the game and install";
		case State::CHECKING:
		case State::INSTALLING:			return "Please wait...";
		case State::DISABLED:			return "Updates are not available";
		case State::UP_TO_DATE:
		case State::OLDER_THAN_CURRENT:
		case State::FAILED:
		case State::CANCELLED:			return "Check again";
		default:						return "Check for updates";
	}
}

void SelfUpdater::onMenuButton()
{
	switch ((State)mState.load())
	{
		case State::IDLE:
		case State::UP_TO_DATE:
		case State::OLDER_THAN_CURRENT:
		case State::FAILED:
		case State::CANCELLED:			startCheck(); break;
		case State::UPDATE_AVAILABLE:	startUpdate(); break;
		case State::DOWNLOADING:
		case State::VERIFYING:
		case State::STAGING:			cancel(); break;
		case State::READY_TO_INSTALL:	installNow(); break;
		default:						break;
	}
}

void SelfUpdater::startCheck()
{
	const State state = (State)mState.load();
	if (state == State::DISABLED)
	{
		std::string message;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			message = mMessage;
		}
		log("updates are switched off: " + message);
		return;
	}
	if (isBusy() || state == State::INSTALLING || state == State::READY_TO_INSTALL)
		return;
	joinWorker();
	mCancel = false;
	setState(State::CHECKING);
	mWorker = std::thread(&SelfUpdater::runCheck, this);
}

void SelfUpdater::startUpdate()
{
	if ((State)mState.load() != State::UPDATE_AVAILABLE)
		return;
	joinWorker();
	mCancel = false;
	mBytesDone = 0;
	mBytesTotal = 0;
	setState(State::DOWNLOADING);
	mWorker = std::thread(&SelfUpdater::runUpdate, this);
}

void SelfUpdater::cancel()
{
	if (isBusy())
	{
		log("the player cancelled");
		mCancel = true;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
void SelfUpdater::runCheck()
{
	const std::string userAgent = "Sonic3AIR-Online-Updater/" + mCurrentVersionText;
	log("checking for updates: this version " + mCurrentVersionText + ", repository " + mRepo);

	std::string body;
	Sink sink;
	sink.mMemory = &body;
	sink.mLimit = MAX_API_BODY;
	sink.mCancel = &mCancel;
	const std::string url = mEndpoints.mApiBase + "/repos/" + mRepo + "/releases/latest";
	const FetchResult result = fetch(url, mEndpoints, sink, true, userAgent);
	if (!result.mOk)
	{
		if (result.mCancelled)
		{
			setState(State::CANCELLED, "Cancelled.");
			return;
		}
		fail(result.mError);
		return;
	}

	std::string errors;
	const Json::Value root = rmx::JsonHelper::loadFromMemory(std::vector<uint8>(body.begin(), body.end()), &errors);
	ReleaseInfo release;
	std::string error;
	if (!parseRelease(root, mRepo, mEndpoints, release, error))
	{
		fail(error);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mRelease = release;
		mHasRelease = true;
	}

	const std::string latestText = versionToString(release.mVersion);
	switch (decide(mCurrentVersion, release.mVersion))
	{
		case Decision::UP_TO_DATE:
			log("up to date (" + latestText + ")");
			setState(State::UP_TO_DATE, "You have the latest version.");
			break;
		case Decision::UPDATE_AVAILABLE:
			log("update available: " + latestText);
			setState(State::UPDATE_AVAILABLE, "");
			break;
		case Decision::OLDER_THAN_CURRENT:
			log("the latest release " + latestText + " is older than this version " + mCurrentVersionText + ": no downgrade");
			setState(State::OLDER_THAN_CURRENT, "The latest release is older than your version. The game never goes back to an older version.");
			break;
		default:
			fail("The versions could not be compared.");
			break;
	}
}

void SelfUpdater::runUpdate()
{
	const std::string userAgent = "Sonic3AIR-Online-Updater/" + mCurrentVersionText;
	ReleaseInfo release;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		release = mRelease;
	}
	const std::string versionText = versionToString(release.mVersion);
	log("update to " + versionText + " started");

	std::error_code ec;
	if (!fs::exists(fs::path(mExeDirectory) / HELPER_SCRIPT_NAME, ec))
	{
		fail(std::string(HELPER_SCRIPT_NAME) + " is missing from the game folder, so an update could not be installed. Download the game again.");
		return;
	}

	const fs::path updateDir = fs::path(mAppDataDirectory) / L"update";
	const fs::path downloadDir = updateDir / L"download";
	const fs::path stagingDir = updateDir / L"staging";
	removeTreeChecked(downloadDir, L"download");
	removeTreeChecked(stagingDir, L"staging");
	fs::remove(updateDir / L"staging_manifest.txt", ec);
	fs::create_directories(downloadDir, ec);
	fs::create_directories(stagingDir, ec);
	if (ec)
	{
		fail("The update folder could not be created in the game's data folder.");
		return;
	}
	auto cleanUp = [&]()
	{
		removeTreeChecked(downloadDir, L"download");
		removeTreeChecked(stagingDir, L"staging");
	};

	// The game folder must be writable, otherwise the helper could not copy anything (for example a game in Program Files)
	{
		const fs::path probe = fs::path(mExeDirectory) / L"update_write_probe.tmp";
		std::ofstream probeFile(probe, std::ios::binary);
		const bool writable = probeFile.good();
		probeFile.close();
		fs::remove(probe, ec);
		if (!writable)
		{
			fail("The game folder is not writable for this user, so the update could not be installed there. Move the game to a folder you own.");
			cleanUp();
			return;
		}
	}

	// 1. What must the package be? (manifest of the release, or GitHub's digest)
	Manifest manifest;
	bool hasManifest = false;
	if (release.mHasManifest)
	{
		std::string manifestBody;
		Sink sink;
		sink.mMemory = &manifestBody;
		sink.mLimit = MAX_MANIFEST_SIZE;
		sink.mCancel = &mCancel;
		const FetchResult result = fetch(release.mManifestUrl, mEndpoints, sink, false, userAgent);
		if (!result.mOk)
		{
			if (result.mCancelled) { setState(State::CANCELLED, "Cancelled."); cleanUp(); return; }
			fail("The manifest of the release could not be fetched: " + result.mError);
			cleanUp();
			return;
		}
		std::string errors;
		const Json::Value manifestJson = rmx::JsonHelper::loadFromMemory(std::vector<uint8>(manifestBody.begin(), manifestBody.end()), &errors);
		std::string error;
		if (!parseManifest(manifestJson, manifest, error))
		{
			fail(error);
			cleanUp();
			return;
		}
		hasManifest = true;
	}
	ExpectedPackage expected;
	{
		std::string error;
		if (!resolveExpectedPackage(release, hasManifest ? &manifest : nullptr, expected, error))
		{
			fail(error);
			cleanUp();
			return;
		}
	}
	mBytesTotal = expected.mSize;

	// 2. Is there room? (the zip, the unpacked files, and the backup)
	if (expected.mSize != 0)
	{
		const fs::space_info space = fs::space(updateDir, ec);
		if (!ec && space.available < expected.mSize * 3)
		{
			fail("There is not enough free disk space for the update (about " + formatMegabytes(expected.mSize * 3) + " needed).");
			cleanUp();
			return;
		}
	}

	// 3. Download
	const fs::path partPath = downloadDir / L"package.zip.part";
	const fs::path zipPath = downloadDir / L"package.zip";
	std::string downloadedHash;
	{
		FILE* file = _wfopen(partPath.c_str(), L"wb");
		if (nullptr == file)
		{
			fail("The download file could not be created.");
			cleanUp();
			return;
		}
		Sha256 sha;
		Sink sink;
		sink.mFile = file;
		sink.mSha = &sha;
		sink.mLimit = (expected.mSize != 0) ? expected.mSize : MAX_PACKAGE_SIZE;
		sink.mCancel = &mCancel;
		sink.mProgress = &mBytesDone;
		sink.mTotal = &mBytesTotal;
		const FetchResult result = fetch(release.mPackageUrl, mEndpoints, sink, false, userAgent);
		fclose(file);
		if (!result.mOk)
		{
			if (result.mCancelled)
			{
				setState(State::CANCELLED, "Cancelled. Nothing was changed.");
				log("cancelled during the download");
			}
			else
			{
				fail(result.mError + " Nothing was changed.");
			}
			cleanUp();
			return;
		}
		if (expected.mSize != 0 && sink.mBytes != expected.mSize)
		{
			fail("The download is incomplete (" + std::to_string(sink.mBytes) + " of " + std::to_string(expected.mSize) + " bytes). Nothing was changed.");
			cleanUp();
			return;
		}
		downloadedHash = sha.finishHex();
	}

	// 4. Verify
	setState(State::VERIFYING);
	if (downloadedHash != expected.mSha256)
	{
		fail("The downloaded update does not match the SHA-256 of the release, so it was thrown away and not installed. Nothing was changed.");
		cleanUp();
		return;
	}
	fs::rename(partPath, zipPath, ec);
	if (ec)
	{
		fail("The downloaded file could not be moved into place.");
		cleanUp();
		return;
	}
	log("download verified (" + formatMegabytes(expected.mSize) + ", SHA-256 matches)");

	// 5. Unpack into the staging folder, with zip-slip protection
	setState(State::STAGING);
	std::string stagingError;
	if (!stagePackage(zipPath, stagingDir, release, stagingError))
	{
		if (mCancel.load())
		{
			setState(State::CANCELLED, "Cancelled. Nothing was changed.");
			log("cancelled while unpacking");
		}
		else
		{
			fail(stagingError + " Nothing was changed.");
		}
		cleanUp();
		return;
	}
	fs::remove(zipPath, ec);
	removeTreeChecked(downloadDir, L"download");

	log("update " + versionText + " is unpacked and checked, ready to install");
	setState(State::READY_TO_INSTALL, "The update is downloaded and checked. Installing closes the game and starts it again.");
}

bool SelfUpdater::stagePackage(const fs::path& zipPath, const fs::path& stagingDir, const ReleaseInfo& release, std::string& outError)
{
	zlib_filefunc64_def fileFunctions;
	fill_win32_filefunc64W(&fileFunctions);
	unzFile zip = unzOpen2_64(zipPath.c_str(), &fileFunctions);
	if (nullptr == zip)
	{
		outError = "The downloaded file is not a valid zip file.";
		return false;
	}
	struct ZipCloser { unzFile mZip; ~ZipCloser() { unzClose(mZip); } } closer { zip };

	// Pass 1: look at every entry before anything is written
	std::vector<ZipEntryInfo> entries;
	std::vector<std::string> rawNames;
	{
		unz_global_info64 globalInfo;
		if (unzGetGlobalInfo64(zip, &globalInfo) != UNZ_OK || globalInfo.number_entry == 0 || globalInfo.number_entry > MAX_ENTRIES)
		{
			outError = "The update package has an unusable number of files.";
			return false;
		}

		int result = unzGoToFirstFile(zip);
		while (result == UNZ_OK)
		{
			char name[1024];
			unz_file_info64 info;
			if (unzGetCurrentFileInfo64(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK || info.size_filename >= sizeof(name) - 1)
			{
				outError = "The update package contains an unreadable or over-long file name.";
				return false;
			}
			ZipEntryInfo entry;
			entry.mRawName = name;
			entry.mSize = info.uncompressed_size;

			std::wstring ignored;
			if (!utf8ToWide(entry.mRawName, ignored))
			{
				outError = "The update package contains a file name that is not valid text.";
				return false;
			}
			std::string reason;
			if (!normalizeEntryPath(entry.mRawName, entry.mPath, entry.mIsDirectory, &reason))
			{
				outError = "The update package was refused: unsafe file name \"" + sanitizeNotes(entry.mRawName, 80) + "\" (" + reason + ").";
				return false;
			}
			if (info.flag & 1)
			{
				outError = "The update package contains encrypted files.";
				return false;
			}
			const uint32_t unixMode = (uint32_t)(info.external_fa >> 16);
			if ((unixMode & 0170000) == 0120000)
			{
				outError = "The update package contains a symbolic link.";
				return false;
			}
			entries.push_back(entry);
			result = unzGoToNextFile(zip);
		}
		if (result != UNZ_END_OF_LIST_OF_FILE)
		{
			outError = "The update package is damaged.";
			return false;
		}
	}

	// A single top folder "Sonic3Airnet/" (how Compress-Archive packs it) is stripped
	bool hasTopFolder = !entries.empty();
	for (const ZipEntryInfo& entry : entries)
	{
		if (entry.mPath.compare(0, 13, "Sonic3Airnet/") != 0 && entry.mPath != "Sonic3Airnet")
			hasTopFolder = false;
	}
	std::vector<ZipEntryInfo> files;
	std::set<std::string> seen;
	uint64_t totalSize = 0;
	bool hasExe = false, hasVersion = false;
	for (ZipEntryInfo& entry : entries)
	{
		if (entry.mIsDirectory)
			continue;
		entry.mPath = stripPackageFolder(entry.mPath, hasTopFolder);
		if (entry.mPath.empty())
		{
			outError = "The update package has files outside its folder.";
			return false;
		}
		std::string reason;
		if (isForbiddenPackagePath(entry.mPath, entry.mSize, &reason))
		{
			outError = "The update package was refused: it contains " + reason + " (\"" + sanitizeNotes(entry.mPath, 80) + "\").";
			return false;
		}
		if (!seen.insert(toLowerAscii(entry.mPath)).second)
		{
			outError = "The update package was refused: the file \"" + sanitizeNotes(entry.mPath, 80) + "\" appears twice.";
			return false;
		}
		totalSize += entry.mSize;
		if (totalSize > MAX_TOTAL_UNPACKED)
		{
			outError = "The update package is larger than allowed when unpacked.";
			return false;
		}
		hasExe = hasExe || (entry.mPath == "Sonic3AIR.exe");
		hasVersion = hasVersion || (entry.mPath == "version.txt");
		files.push_back(entry);
	}
	if (!hasExe || !hasVersion)
	{
		outError = "The update package is not a game package (Sonic3AIR.exe or version.txt is missing).";
		return false;
	}

	// Pass 2: unpack. Every target is checked once more to lie inside the staging folder.
	const fs::path stagingRoot = fs::weakly_canonical(stagingDir);
	std::vector<StagedFile> staged;
	std::vector<char> buffer(64 * 1024);
	int result = unzGoToFirstFile(zip);
	while (result == UNZ_OK)
	{
		char name[1024];
		unz_file_info64 info;
		unzGetCurrentFileInfo64(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0);
		std::string path;
		bool isDirectory = false;
		normalizeEntryPath(name, path, isDirectory);
		if (!isDirectory)
		{
			path = stripPackageFolder(path, hasTopFolder);
			std::wstring widePath;
			utf8ToWide(path, widePath);
			const fs::path target = (stagingRoot / fs::path(widePath)).lexically_normal();
			const std::string targetText = wideToUtf8(target.wstring());
			const std::string rootText = wideToUtf8(stagingRoot.wstring());
			if (targetText.size() <= rootText.size() + 1 || toLowerAscii(targetText.substr(0, rootText.size())) != toLowerAscii(rootText))
			{
				outError = "The update package was refused: a file would land outside the staging folder.";
				return false;
			}
			std::error_code ec;
			fs::create_directories(target.parent_path(), ec);

			if (unzOpenCurrentFile(zip) != UNZ_OK)
			{
				outError = "A file of the update package could not be opened.";
				return false;
			}
			FILE* out = _wfopen(target.c_str(), L"wb");
			if (nullptr == out)
			{
				unzCloseCurrentFile(zip);
				outError = "A file of the update could not be written to the staging folder.";
				return false;
			}
			Sha256 sha;
			uint64_t written = 0;
			bool ok = true;
			while (true)
			{
				if (mCancel.load())
				{
					ok = false;
					outError = "Cancelled.";
					break;
				}
				const int bytes = unzReadCurrentFile(zip, buffer.data(), (unsigned)buffer.size());
				if (bytes < 0)
				{
					ok = false;
					outError = "A file in the update package is damaged.";
					break;
				}
				if (bytes == 0)
					break;
				written += (uint64_t)bytes;
				if (written > info.uncompressed_size)
				{
					ok = false;
					outError = "A file in the update package is larger than it says (refused).";
					break;
				}
				if (fwrite(buffer.data(), 1, (size_t)bytes, out) != (size_t)bytes)
				{
					ok = false;
					outError = "The update could not be written to the disk (is it full?).";
					break;
				}
				sha.update(buffer.data(), (size_t)bytes);
			}
			fclose(out);
			const int closeResult = unzCloseCurrentFile(zip);
			if (ok && (closeResult != UNZ_OK || written != info.uncompressed_size))
			{
				ok = false;
				outError = "A file in the update package is damaged (checksum).";
			}
			if (!ok)
				return false;

			StagedFile stagedFile;
			stagedFile.mPath = path;
			stagedFile.mSha256 = sha.finishHex();
			stagedFile.mSize = written;
			staged.push_back(stagedFile);
		}
		result = unzGoToNextFile(zip);
	}
	if (staged.size() != files.size())
	{
		outError = "The update package could not be unpacked completely.";
		return false;
	}

	// The package must be the version the release says it is
	{
		Version packaged;
		const std::string packagedText = firstLine(readSmallTextFile(stagingRoot / L"version.txt", 64));
		if (!parseVersion(packagedText, packaged) || compareVersions(packaged, release.mVersion) != 0)
		{
			outError = "The version inside the update package does not match the release.";
			return false;
		}
		std::error_code ec;
		if (fs::file_size(stagingRoot / L"Sonic3AIR.exe", ec) < 100 * 1024)
		{
			outError = "The game program inside the update package looks wrong.";
			return false;
		}
	}

	// The list the helper checks the staging folder against ("sha256  size  path"), then installs from
	{
		std::ofstream list(stagingRoot.parent_path() / L"staging_manifest.txt", std::ios::binary | std::ios::trunc);
		for (const StagedFile& file : staged)
			list << file.mSha256 << "  " << file.mSize << "  " << file.mPath << "\n";
		if (!list.good())
		{
			outError = "The staging list could not be written.";
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
void SelfUpdater::installNow()
{
	if ((State)mState.load() != State::READY_TO_INSTALL)
		return;

	const fs::path updateDir = fs::path(mAppDataDirectory) / L"update";
	std::error_code ec;
	const fs::path helperSource = fs::path(mExeDirectory) / HELPER_SCRIPT_NAME;
	const fs::path helperCopy = updateDir / L"helper.ps1";
	fs::copy_file(helperSource, helperCopy, fs::copy_options::overwrite_existing, ec);
	if (ec)
	{
		fail(std::string("The install helper could not be prepared (") + ec.message() + ").");
		return;
	}

	wchar_t systemDir[MAX_PATH] = {};
	GetSystemDirectoryW(systemDir, MAX_PATH);
	const std::wstring powershell = std::wstring(systemDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

	std::string versionTo;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		versionTo = versionToString(mRelease.mVersion);
	}
	std::ostringstream command;
	command << "\"" << wideToUtf8(powershell) << "\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File " << quotePath(helperCopy.wstring())
			<< " -GameDir " << quotePath(mExeDirectory)
			<< " -StagingDir " << quotePath((updateDir / L"staging").wstring())
			<< " -UpdateDir " << quotePath(updateDir.wstring())
			<< " -ModsDir " << quotePath((fs::path(mAppDataDirectory) / L"mods").wstring())
			<< " -ExeName " << quotePath(mExeName)
			<< " -ProcessId " << GetCurrentProcessId()
			<< " -FromVersion " << mCurrentVersionText
			<< " -ToVersion " << versionTo;
	log("starting the install helper: " + command.str());
	std::wstring commandWide;
	utf8ToWide(command.str(), commandWide);
	std::vector<wchar_t> commandLine(commandWide.begin(), commandWide.end());
	commandLine.push_back(L'\0');

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(powershell.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, mExeDirectory.c_str(), &startup, &process))
	{
		fail("The install helper (PowerShell) could not be started. Nothing was changed.");
		return;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);

	log("install helper started, closing the game");
	setState(State::INSTALLING, "The game closes now, installs the update and starts again.");
	FTX::System->quit();
}

void SelfUpdater::update()
{
	if (mAutoMode.empty())
		return;

	// Developer test switch: drive the updater without anybody pressing a key
	const State state = (State)mState.load();
	if (!mAutoCheckStarted)
	{
		mAutoCheckStarted = true;
		log("test mode: " + mAutoMode);
		startCheck();
		return;
	}
	if (mAutoMode == "check")
		return;
	if (state == State::UPDATE_AVAILABLE && !mAutoUpdateStarted)
	{
		mAutoUpdateStarted = true;
		startUpdate();
	}
	else if (state == State::DOWNLOADING && mAutoMode == "cancel" && !mAutoCancelled && mBytesDone.load() > 0)
	{
		mAutoCancelled = true;
		cancel();
	}
	else if (state == State::READY_TO_INSTALL && mAutoMode == "apply" && !mAutoInstallStarted)
	{
		mAutoInstallStarted = true;
		installNow();
	}
}


// ---------------------------------------------------------------------------------------------------------------------
// Other platforms: no updater
#else

SelfUpdater& SelfUpdater::instance()  { static SelfUpdater instance; return instance; }
SelfUpdater::SelfUpdater()  { mState = (int)State::DISABLED; mMessage = "Updates are only available on Windows. In the browser version, reload the page to get the newest version."; }
SelfUpdater::~SelfUpdater() {}
void SelfUpdater::update() {}
SelfUpdater::Snapshot SelfUpdater::getSnapshot()  { Snapshot s; s.mState = State::DISABLED; s.mMessage = mMessage; return s; }
void SelfUpdater::onMenuButton() {}
const char* SelfUpdater::getButtonText()  { return "Updates are not available"; }
void SelfUpdater::startCheck() {}
void SelfUpdater::startUpdate() {}
void SelfUpdater::cancel() {}
void SelfUpdater::installNow() {}

#endif
