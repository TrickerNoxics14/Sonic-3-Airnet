/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/menu/options/OptionsMenuEntries.h"
#include "sonic3air/menu/options/OptionsEntry.h"
#include "sonic3air/menu/options/OptionsMenu.h"
#include "sonic3air/menu/SharedResources.h"
#include "sonic3air/audio/AudioOut.h"
#include "sonic3air/client/GameClient.h"
#include "sonic3air/client/SelfUpdater.h"
#include "sonic3air/ConfigurationImpl.h"
#include "sonic3air/Game.h"
#include "sonic3air/version.inc"

#include "oxygen/application/Application.h"
#include "oxygen/application/modding/Mod.h"
#include "oxygen/download/Downloader.h"


namespace
{
	const String& getVersionString(uint32 buildNumber)
	{
		static uint32 cachedBuildNumber = 0xffffffff;
		static String cachedBuildString;
		if (cachedBuildNumber != buildNumber || cachedBuildNumber == 0xffffffff)
		{
			cachedBuildNumber = buildNumber;
			cachedBuildString.format("v%02x.%02x.%02x.%x", (buildNumber >> 24) & 0xff, (buildNumber >> 16) & 0xff, (buildNumber >> 8) & 0xff, buildNumber & 0xff);
		}
		return cachedBuildString;
	}

	float moveTowards(float value, float target, float maxStep)
	{
		if (value != target)
		{
			if (value < target)
				return std::min(value + maxStep, target);
			else
				return std::max(value - maxStep, target);
		}
		return value;
	}
}


TitleMenuEntry::TitleMenuEntry()
{
	mMenuEntryType = MENU_ENTRY_TYPE;
	setInteractable(false);
}

TitleMenuEntry& TitleMenuEntry::initEntry(const std::string& text)
{
	mText = text;
	return *this;
}

void TitleMenuEntry::renderEntry(RenderContext& renderContext_)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	Drawer& drawer = *renderContext.mDrawer;
	const int baseX = renderContext.mCurrentPosition.x;
	int& py = renderContext.mCurrentPosition.y;

	py += 15;
	drawer.printText(global::mSonicFontB, Recti(baseX, py, 0, 10), ("* " + mText + " *"), 5, Color(0.6f, 0.8f, 1.0f, renderContext.mTabAlpha));
	py += 2;
}


ModTitleMenuEntry::ModTitleMenuEntry()
{
	mMenuEntryType = MENU_ENTRY_TYPE;
}

ModTitleMenuEntry& ModTitleMenuEntry::initEntry(const Mod& mod)
{
	mMod = &mod;
	mText = mod.mDisplayName;
	return *this;
}

void ModTitleMenuEntry::renderEntry(RenderContext& renderContext_)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	Drawer& drawer = *renderContext.mDrawer;
	const int baseX = renderContext.mCurrentPosition.x;
	int& py = renderContext.mCurrentPosition.y;
	const float alpha = renderContext.mTabAlpha;

	const bool isSelected = renderContext.mIsSelected;
	const bool isActive = (mSelectedIndex > 0);
	const Color color = isSelected ? Color(1.0f, 1.0f, 0.0f, alpha) : Color(0.7f, 1.0f, 0.9f, alpha);

	const float targetIndent = isActive ? 20.0f : 30.0f;
	if (mIndent == 0.0f)
		mIndent = targetIndent;
	mIndent = moveTowards(mIndent, targetIndent, renderContext.mDeltaSeconds * 250.0f);
	const int indent = roundToInt(mIndent);

	py += 14;
	const int textWidth = global::mOxyfontRegular.getWidth(mText);
	drawer.printText(global::mOxyfontRegular, Recti(baseX - 180 + indent, py, 0, 10), mText, 4, color);
	drawer.drawRect(Recti(baseX - 185, py + 4, indent, 1), color);
	drawer.drawRect(Recti(baseX - 184, py + 5, indent, 1), Color(0.0f, 0.0f, 0.0f, alpha * 0.75f));
	drawer.drawRect(Recti(baseX - 175 + textWidth + indent, py + 4, 360 - textWidth - indent, 1), color);
	drawer.drawRect(Recti(baseX - 174 + textWidth + indent, py + 5, 360 - textWidth - indent, 1), Color(0.0f, 0.0f, 0.0f, alpha * 0.75f));
	py += 7;
}


LabelMenuEntry::LabelMenuEntry()
{
	setInteractable(false);
}

LabelMenuEntry& LabelMenuEntry::initEntry(const std::string& text, const Color& color)
{
	mText = text;
	mColor = color;
	return *this;
}

void LabelMenuEntry::renderEntry(RenderContext& renderContext_)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	Drawer& drawer = *renderContext.mDrawer;
	const int baseX = renderContext.mCurrentPosition.x;
	int& py = renderContext.mCurrentPosition.y;

	py -= 1;
	const Vec2i boxSize = global::mOxyfontTiny.getTextBoxSize(mText);
	drawer.printText(global::mOxyfontTiny, Recti(baseX, py, 0, 10), mText, 5, Color(mColor.r, mColor.g, mColor.b, mColor.a * renderContext.mTabAlpha));
	py += boxSize.y - 4;
}


OptionsMenuEntry& OptionsMenuEntry::setUseSmallFont(bool useSmallFont)
{
	mUseSmallFont = useSmallFont;
	return *this;
}

void OptionsMenuEntry::renderEntry(RenderContext& renderContext_)
{
	renderInternal(renderContext_, Color::WHITE, Color::YELLOW);
}

void OptionsMenuEntry::renderInternal(RenderContext& renderContext_, const Color& normalColor, const Color& selectedColor)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	Drawer& drawer = *renderContext.mDrawer;
	const int baseX = renderContext.mCurrentPosition.x;
	int& py = renderContext.mCurrentPosition.y;
	Font& font = (mUseSmallFont || renderContext.mIsModsTab) ? global::mOxyfontSmall : global::mOxyfontRegular;

	const bool isSelected = renderContext.mIsSelected;
	const bool isDisabled = !isInteractable();

	Color color = isSelected ? selectedColor : isDisabled ? Color(0.4f, 0.4f, 0.4f) : normalColor;
	color.a *= renderContext.mTabAlpha;

	if (mOptions.empty())
	{
		// Used for selectable entries, like "Back"
		if (mData == option::_BACK)
		{
			py += 16;
		}

		if (mData == option::CONTROLLER_SETUP)
		{
			drawer.printText(font, Recti(baseX, py, 0, 10), Application::instance().hasKeyboard() ? "Setup Keyboard & Game Controllers..." : "Setup Game Controllers...", 5, color);
		}
		else
		{
			drawer.printText(font, Recti(baseX, py, 0, 10), mText, 5, color);
		}

		if (isSelected)
		{
			// Draw arrows
			const int halfTextWidth = font.getWidth(mText) / 2;
			const int offset = (int)std::fmod(FTX::getTime() * 6.0f, 6.0f);
			const int arrowDistance = 16 + ((offset > 3) ? (6 - offset) : offset);
			drawer.printText(font, Recti(baseX - halfTextWidth - arrowDistance, py, 0, 10), ">>", 5, color);
			drawer.printText(font, Recti(baseX + halfTextWidth + arrowDistance, py, 0, 10), "<<", 5, color);
		}

		if (mData == option::CONTROLLER_SETUP)
			py += 4;
	}
	else
	{
		// It's an actual options entry, with multiple options to choose from
		const bool canGoLeft  = !isDisabled && (mSelectedIndex > 0);
		const bool canGoRight = !isDisabled && (mSelectedIndex < mOptions.size() - 1);

		const int center = mText.empty() ? baseX : (baseX + 88);
		int arrowDistance = 75;
		if (isSelected)
		{
			const int offset = (int)std::fmod(FTX::getTime() * 6.0f, 6.0f);
			arrowDistance += ((offset > 3) ? (6 - offset) : offset);
		}

		// Description
		if (!mText.empty())
		{
			drawer.printText(font, Recti(baseX - 40, py, 0, 10), mText, 6, color);
		}

		// Value text
		const AudioCollection::AudioDefinition* audioDefinition = nullptr;
		{
			static const std::string TEXT_NOT_AVAILABLE = "not available";
			const std::string* text = (isDisabled && mData != option::RENDERER) ? &TEXT_NOT_AVAILABLE : &mOptions[mSelectedIndex].mText;
			if (mData == option::SOUND_TEST)
			{
				audioDefinition = renderContext.mOptionsMenu->getSoundTestAudioDefinition(selected().mValue);
				if (nullptr != audioDefinition && AudioOut::instance().getAudioKeyType(audioDefinition->mKeyId) == AudioOutBase::AudioKeyType::MODDED)
				{
					static std::string combinedText;
					combinedText = *text + " (modded)";
					text = &combinedText;
				}
			}
			drawer.printText(font, Recti(center - 80, py, 160, 10), *text, 5, color);
		}

		if (canGoLeft)
			drawer.printText(font, Recti(center - arrowDistance, py, 0, 10), "<", 5, color);
		if (canGoRight)
			drawer.printText(font, Recti(center + arrowDistance, py, 0, 10), ">", 5, color);

		// Additional text for sound test
		if (mData == option::SOUND_TEST && nullptr != audioDefinition)
		{
			py += 13;
			drawer.printText(global::mOxyfontTiny, Recti(center - 80, py, 160, 10), audioDefinition->mDisplayName, 5, color);
		}
	}
}


AdvancedOptionMenuEntry::AdvancedOptionMenuEntry()
{
	setUseSmallFont(true);
}

void AdvancedOptionMenuEntry::renderEntry(RenderContext& renderContext_)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	const Color normalColor = (mOptions[mSelectedIndex].mValue == mDefaultValue) ? Color::WHITE : Color(1.0f, 0.5f, 0.5f);
	const Color selectedColor = (mOptions[mSelectedIndex].mValue == mDefaultValue) ? Color::YELLOW : Color(1.0f, 0.75f, 0.0f);
	renderInternal(renderContext_, normalColor, selectedColor);
	renderContext.mCurrentPosition.y -= 1;
}


void UpdateCheckMenuEntry::renderEntry(RenderContext& renderContext_)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	Drawer& drawer = *renderContext.mDrawer;
	const int baseX = renderContext.mCurrentPosition.x;
	int& py = renderContext.mCurrentPosition.y;
	const float alpha = renderContext.mTabAlpha;

	SelfUpdater& updater = SelfUpdater::instance();
	const SelfUpdater::Snapshot snapshot = updater.getSnapshot();
	const Color white(1.0f, 1.0f, 1.0f, alpha);
	const Color green(0.8f, 1.0f, 0.8f, alpha);
	const Color yellow(1.0f, 1.0f, 0.6f, alpha);
	const Color red(1.0f, 0.4f, 0.4f, alpha);

	// Wraps a message into lines of at most 46 characters and prints them centered
	auto printWrapped = [&](const std::string& text, const Color& color, int maxLines)
	{
		int lines = 0;
		size_t pos = 0;
		while (pos < text.size() && lines < maxLines)
		{
			size_t lineEnd = text.find('\n', pos);
			if (lineEnd == std::string::npos)
				lineEnd = text.size();
			std::string line = text.substr(pos, lineEnd - pos);
			size_t next = lineEnd + 1;
			if (line.size() > 46)
			{
				size_t cut = line.rfind(' ', 46);
				if (cut == std::string::npos || cut < 20)
					cut = 46;
				next = pos + cut + (line[cut] == ' ' ? 1 : 0);
				line = line.substr(0, cut);
			}
			if (!line.empty())
			{
				drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), line, 5, color);
				py += 10;
				++lines;
			}
			pos = next;
		}
	};

	drawer.printText(global::mOxyfontSmall, Recti(baseX - 100, py, 0, 10), "Your Game Version:", 4, white);
	drawer.printText(global::mOxyfontSmall, Recti(baseX + 100, py, 0, 10), snapshot.mCurrentVersion.empty() ? std::string("unknown (development build)") : ("v" + snapshot.mCurrentVersion), 6, green);
	py += 12;

	switch (snapshot.mState)
	{
		case SelfUpdater::State::CHECKING:
			drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "Asking GitHub for the latest release...", 5, white);
			py += 10;
			break;

		case SelfUpdater::State::UP_TO_DATE:
			drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "You're using the latest version", 5, green);
			py += 10;
			break;

		case SelfUpdater::State::UPDATE_AVAILABLE:
		case SelfUpdater::State::DOWNLOADING:
		case SelfUpdater::State::VERIFYING:
		case SelfUpdater::State::STAGING:
		case SelfUpdater::State::READY_TO_INSTALL:
		case SelfUpdater::State::INSTALLING:
		{
			drawer.printText(global::mOxyfontSmall, Recti(baseX - 100, py, 0, 10), "Update available:", 4, white);
			drawer.printText(global::mOxyfontSmall, Recti(baseX + 100, py, 0, 10), "v" + snapshot.mLatestVersion, 6, yellow);
			py += 12;
			if (snapshot.mState == SelfUpdater::State::DOWNLOADING)
			{
				char text[96];
				const double done = (double)snapshot.mBytesDone / (1024.0 * 1024.0);
				if (snapshot.mBytesTotal > 0)
					snprintf(text, sizeof(text), "Downloading... %.1f of %.1f MB (%d%%)", done, (double)snapshot.mBytesTotal / (1024.0 * 1024.0), (int)(100.0 * snapshot.mBytesDone / snapshot.mBytesTotal));
				else
					snprintf(text, sizeof(text), "Downloading... %.1f MB", done);
				drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), text, 5, white);
				py += 10;
			}
			else if (snapshot.mState == SelfUpdater::State::VERIFYING)
			{
				drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "Checking the download (SHA-256)...", 5, white);
				py += 10;
			}
			else if (snapshot.mState == SelfUpdater::State::STAGING)
			{
				drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "Unpacking the update...", 5, white);
				py += 10;
			}
			else if (snapshot.mState == SelfUpdater::State::READY_TO_INSTALL)
			{
				printWrapped("Downloaded and checked. Installing closes the game and starts it again.", green, 2);
			}
			else if (snapshot.mState == SelfUpdater::State::INSTALLING)
			{
				drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "Closing the game...", 5, white);
				py += 10;
			}
			if (!snapshot.mNotes.empty() && snapshot.mState == SelfUpdater::State::UPDATE_AVAILABLE)
			{
				py += 2;
				drawer.printText(global::mOxyfontSmall, Recti(baseX - 100, py, 0, 10), "What's new:", 4, white);
				py += 11;
				printWrapped(snapshot.mNotes, Color(0.85f, 0.85f, 1.0f, alpha), 6);
			}
			break;
		}

		case SelfUpdater::State::FAILED:
		case SelfUpdater::State::OLDER_THAN_CURRENT:
		case SelfUpdater::State::CANCELLED:
		case SelfUpdater::State::DISABLED:
			printWrapped(snapshot.mMessage, (snapshot.mState == SelfUpdater::State::FAILED) ? red : white, 5);
			break;

		default:
			drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "Ready to check for updates", 5, Color(0.8f, 0.8f, 0.8f, alpha));
			py += 10;
			break;
	}

	if (!snapshot.mLastResult.empty() && snapshot.mState != SelfUpdater::State::DOWNLOADING)
	{
		printWrapped("Last update: " + snapshot.mLastResult, Color(0.7f, 0.7f, 0.7f, alpha), 2);
	}
	py += 10;

	mText = updater.getButtonText();
	OptionsMenuEntry::renderEntry(renderContext);
}


void SoundtrackMenuEntry::renderEntry(RenderContext& renderContext_)
{
	renderInternal(renderContext_, Color::WHITE, Color::YELLOW);

	#if defined(PLATFORM_WEB)
		// Extra text for Web version if remastered soundtrack was not downloaded
		//  -> This could be obsolete if Web can implement the internal downloader functionality
		if (!Downloader::isDownloaderSupported() && !AudioOut::instance().hasLoadedRemasteredSoundtrack() && selected().mIndex == 1)
		{
			OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
			Drawer& drawer = *renderContext.mDrawer;
			const int baseX = renderContext.mCurrentPosition.x;
			int& py = renderContext.mCurrentPosition.y;
			const int center = mText.empty() ? baseX : (baseX + 88);

			py += 13;
			drawer.printText(global::mOxyfontTiny, Recti(center - 80, py, 160, 10), "Must be downloaded separately", 5, Color(1.0f, 0.9f, 0.8f, renderContext.mTabAlpha));
			py += 10;
			drawer.printText(global::mOxyfontTiny, Recti(center - 80, py, 160, 10), "(Exit and select \"Extra Downloads\")", 5, Color(1.0f, 0.9f, 0.8f, renderContext.mTabAlpha));
			++py;
		}
	#endif
}


void SoundtrackDownloadMenuEntry::renderEntry(RenderContext& renderContext_)
{
	RemasteredMusicDownload& download = Game::instance().getRemasteredMusicDownload();
	const RemasteredMusicDownload::State state = download.getState();
	if (state != RemasteredMusicDownload::State::LOADED)
	{
		OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
		Drawer& drawer = *renderContext.mDrawer;
		const int center = renderContext.mCurrentPosition.x;
		int& py = renderContext.mCurrentPosition.y;
		Color color = renderContext.mIsSelected ? Color::YELLOW : Color::WHITE;
		color.a *= renderContext.mTabAlpha;

		std::string text;
		switch (state)
		{
			case RemasteredMusicDownload::State::READY_FOR_DOWNLOAD:
				text = "Download remastered soundtrack now? (126 MB)";
				mText = "Start download";
				break;

			case RemasteredMusicDownload::State::DOWNLOAD_PENDING:
				text = "Waiting for download...";
				mText = "Stop download";
				break;

			case RemasteredMusicDownload::State::DOWNLOAD_RUNNING:
				text = "Downloading... " + std::to_string(download.getBytesDownloaded() / (1024*1024)) + " MB";
				mText = "Stop download";
				break;

			case RemasteredMusicDownload::State::DOWNLOAD_DONE:
				text = "Download complete";
				mText = "Load soundtrack";
				break;

			case RemasteredMusicDownload::State::DOWNLOAD_FAILED:
				text = "Download failed";
				mText = "Restart download";
				break;

			default:
				break;
		}

		drawer.printText(global::mOxyfontTiny, Recti(center - 80, py, 160, 10), text, 5, Color(0.8f, 1.0f, 0.9f, color.a));
		py += 12;

		mUseSmallFont = true;
		renderInternal(renderContext_, Color::WHITE, Color::YELLOW);
		py += 3;
	}
}

void SoundtrackDownloadMenuEntry::triggerButton()
{
	RemasteredMusicDownload& download = Game::instance().getRemasteredMusicDownload();
	const RemasteredMusicDownload::State state = download.getState();
	switch (state)
	{
		case RemasteredMusicDownload::State::READY_FOR_DOWNLOAD:
		case RemasteredMusicDownload::State::DOWNLOAD_FAILED:
			download.startDownload();
			break;

		case RemasteredMusicDownload::State::DOWNLOAD_PENDING:
		case RemasteredMusicDownload::State::DOWNLOAD_RUNNING:
			download.removeDownload();
			break;

		case RemasteredMusicDownload::State::DOWNLOAD_DONE:
			download.applyAfterDownload();
			break;

		default:
			break;
	}
}

bool SoundtrackDownloadMenuEntry::shouldBeShown()
{
	return (ConfigurationImpl::instance().mActiveSoundtrack == 1 && Downloader::isDownloaderSupported() && !AudioOut::instance().hasLoadedRemasteredSoundtrack());
}


DevModeMenuEntry::DevModeMenuEntry()
{
	setUseSmallFont(true);
}

void DevModeMenuEntry::renderEntry(RenderContext& renderContext_)
{
	OptionsMenuRenderContext& renderContext = renderContext_.as<OptionsMenuRenderContext>();
	Drawer& drawer = *renderContext.mDrawer;

	renderInternal(renderContext_, Color::WHITE, Color::YELLOW);

	if ((mOptions[mSelectedIndex].mValue != 0) != Configuration::instance().mDevMode.mEnabled)
	{
		const int baseX = renderContext.mCurrentPosition.x;
		int& py = renderContext.mCurrentPosition.y;
		py += 14;
		drawer.printText(global::mOxyfontSmall, Recti(baseX, py, 0, 10), "App restart required to switch Dev Mode on or off!", 5, Color(1.0f, 0.8f, 0.6f, renderContext.mTabAlpha));
	}
}
