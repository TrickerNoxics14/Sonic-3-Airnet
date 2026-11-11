/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "sonic3air/client/SelfUpdateCore.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>


// In-game updater of the online fork. It replaces the stock "check for updates" of Sonic 3 A.I.R., which asks sonic3air.org about stock releases.
//  - Nothing happens without a click in the options menu (or, for the automated checks only, a developer test switch).
//  - Check: one request to the public GitHub REST API (releases/latest of the repository in settings.json: Online / UpdateRepo).
//  - Update: download the zip, compare its SHA-256 with the manifest of the release, unpack it into a staging folder (zip-slip protection),
//    and only after a second click close the game and let update_helper.ps1 (shipped with the game) copy the files over, with a backup and a rollback.
//  - Never executed: anything from the download. The helper script is the one that already lies in the game folder.
class SelfUpdater
{
public:
	enum class State
	{
		IDLE,
		DISABLED,				// no version information next to the executable (development build) or unsupported platform
		CHECKING,
		UP_TO_DATE,
		UPDATE_AVAILABLE,
		OLDER_THAN_CURRENT,
		DOWNLOADING,
		VERIFYING,
		STAGING,
		READY_TO_INSTALL,
		INSTALLING,
		FAILED,
		CANCELLED
	};

	struct Snapshot
	{
		State mState = State::IDLE;
		std::string mCurrentVersion;	// empty if unknown
		std::string mLatestVersion;
		std::string mNotes;
		std::string mMessage;			// the error or the explanation for the state
		uint64_t mBytesDone = 0;
		uint64_t mBytesTotal = 0;
		std::string mLastResult;		// result of the last installation (written by the helper), empty if none
	};

public:
	static SelfUpdater& instance();
	~SelfUpdater();

	// Called every frame by the game
	void update();

	Snapshot getSnapshot();

	// What the menu button does in the current state: check, start the update, cancel, install
	void onMenuButton();
	const char* getButtonText();

	void startCheck();
	void startUpdate();
	void cancel();
	void installNow();

private:
	SelfUpdater();

	void setState(State state, const std::string& message = std::string());
	void fail(const std::string& message);
	void log(const std::string& text);
	void joinWorker();
	bool isBusy() const;

	void runCheck();
	void runUpdate();
	bool stagePackage(const std::filesystem::path& zipPath, const std::filesystem::path& stagingDir, const selfupdate::ReleaseInfo& release, std::string& outError);

private:
	std::mutex mMutex;
	std::atomic<int> mState { (int)State::IDLE };
	std::atomic<bool> mCancel { false };
	std::atomic<uint64_t> mBytesDone { 0 };
	std::atomic<uint64_t> mBytesTotal { 0 };
	std::thread mWorker;

	// Guarded by mMutex
	std::string mMessage;
	std::string mCurrentVersionText;
	selfupdate::Version mCurrentVersion;
	selfupdate::ReleaseInfo mRelease;
	bool mHasRelease = false;

	// Settings (read once at start)
	std::string mRepo;
	selfupdate::Endpoints mEndpoints;
	std::wstring mExeDirectory;
	std::wstring mExeName;
	std::wstring mAppDataDirectory;
	std::string mLastResult;

	// Developer test switch (only with the test marker file): drive the updater without a menu
	std::string mAutoMode;
	bool mAutoCheckStarted = false;
	bool mAutoUpdateStarted = false;
	bool mAutoInstallStarted = false;
	bool mAutoCancelled = false;
};
