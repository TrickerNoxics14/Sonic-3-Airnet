/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/audio/RemasteredMusicDownload.h"
#include "sonic3air/audio/AudioOut.h"

#include "oxygen/download/DownloadManager.h"


RemasteredMusicDownload::RemasteredMusicDownload() :
	mDownloadID(rmx::constMurmur2_64("RemasteredSoundtrack"))
{
}

#if defined(PLATFORM_WEB)
	#include <emscripten.h>

	// In the browser the page downloads the soundtrack (window.airnetMusic in sonic3air_web.html): it is published in parts, and is written straight into the
	// browser's storage instead of being held in the game's memory first. airnetMusic.state: 0 = not there, 1 = downloading, 2 = there, 3 = failed; -1 = no such page
	namespace
	{
		int getPageMusicState()
		{
			return EM_ASM_INT({ return (typeof airnetMusic === 'object' && airnetMusic) ? airnetMusic.state : -1; });
		}
	}
#endif

RemasteredMusicDownload::State RemasteredMusicDownload::getState()
{
#if defined(PLATFORM_WEB)
	switch (getPageMusicState())
	{
		case 0:  return State::READY_FOR_DOWNLOAD;
		case 1:  return State::DOWNLOAD_RUNNING;
		case 2:  return State::DOWNLOAD_DONE;
		case 3:  return State::DOWNLOAD_FAILED;
		default: return State::UNSUPPORTED;
	}
#endif

	if (!Downloader::isDownloaderSupported())
		return State::UNSUPPORTED;

	mCachedDownload = DownloadManager::instance().getDownloadByKey(mDownloadID);
	if (nullptr != mCachedDownload)
	{
		switch (mCachedDownload->getState())
		{
			case Downloader::State::NONE:		return State::DOWNLOAD_PENDING;
			case Downloader::State::RUNNING:	return State::DOWNLOAD_RUNNING;
			case Downloader::State::DONE:		return State::DOWNLOAD_DONE;
			case Downloader::State::FAILED:		return State::DOWNLOAD_FAILED;
		}
	}
	return State::READY_FOR_DOWNLOAD;
}

uint64 RemasteredMusicDownload::getBytesDownloaded() const
{
#if defined(PLATFORM_WEB)
	return (uint64)EM_ASM_DOUBLE({ return (typeof airnetMusic === 'object' && airnetMusic) ? airnetMusic.bytes : 0; });
#else
	return (nullptr != mCachedDownload) ? mCachedDownload->getBytesDownloaded() : 0;
#endif
}

void RemasteredMusicDownload::startDownload()
{
#if defined(PLATFORM_WEB)
	EM_ASM({ if (typeof airnetMusic === 'object' && airnetMusic) airnetMusic.start(); });
#else
	DownloadManager::instance().startDownload(mDownloadID, "https://sonic3air.org/download/audioremaster.bin", L"data/audioremaster.bin");
#endif
}

void RemasteredMusicDownload::removeDownload()
{
#if defined(PLATFORM_WEB)
	EM_ASM({ if (typeof airnetMusic === 'object' && airnetMusic) airnetMusic.cancel(); });
#else
	DownloadManager::instance().removeDownload(mDownloadID);
#endif
}

void RemasteredMusicDownload::applyAfterDownload()
{
	// Load remastered music package
	EngineMain::instance().reloadFilePackage(L"audioremaster.bin", false);
	AudioOut::instance().reloadRemasteredSoundtrack();
}
