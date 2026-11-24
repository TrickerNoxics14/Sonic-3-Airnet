/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/menu/context/ApplicationContextMenu.h"
#include "sonic3air/Game.h"
#include "sonic3air/menu/GameApp.h"
#include "sonic3air/menu/SharedResources.h"

#include "oxygen/application/Application.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/EngineMain.h"
#include "oxygen/platform/DirectoryOpenLogic.h"
#include "oxygen/platform/PlatformFunctions.h"
#include "oxygen/simulation/Simulation.h"


namespace
{
	DirectoryOpenAction getDirectoryOpenAction()
	{
		// Where no file manager can be started (web, Android, iOS), the game's own file browser stands in for it
		return chooseDirectoryOpenAction(PlatformFunctions::canOpenDirectoryExternal(), GameApp::instance().supportsFileBrowser());
	}

	bool isGameplayRunning()
	{
		// The same condition as Game::shouldPauseOnFocusLoss without the online exception: a level that plays on (the menu background does not count, nor does a paused game)
		return Application::instance().getSimulation().isRunning() && Application::instance().getSimulation().getSpeed() > 0.0f && !Game::instance().isInMainMenuMode();
	}

	bool isOpeningAllowedNow()
	{
		return isDirectoryOpenAllowedNow(getDirectoryOpenAction(), isGameplayRunning());
	}

	void openDirectory(const std::wstring& path, bool isModsDirectory)
	{
		if (!isOpeningAllowedNow())
			return;

		switch (getDirectoryOpenAction())
		{
			case DirectoryOpenAction::EXTERNAL:
				PlatformFunctions::openDirectoryExternal(path);
				break;

			case DirectoryOpenAction::FILE_BROWSER:
				GameApp::instance().openFileBrowser(isModsDirectory);
				break;

			case DirectoryOpenAction::NONE:
				break;
		}
	}
}


ApplicationContextMenu::ApplicationContextMenu()
{
}

ApplicationContextMenu::~ApplicationContextMenu()
{
}

void ApplicationContextMenu::initialize()
{
	GuiBase::initialize();

	// Without any way to open a folder (neither a file manager nor the game's own file browser) there is nothing to offer, and the menu stays closed
	if (mItems.empty() && getDirectoryOpenAction() != DirectoryOpenAction::NONE)
	{
		mItems.emplace_back(Item { "Open saved data " PLATFORM_DIRECTORY_STRING, Item::Function::OPEN_SAVED_DATA_DIRECTORY });
		mItems.emplace_back(Item { "Open mods " PLATFORM_DIRECTORY_STRING,		Item::Function::OPEN_MODS_DIRECTORY });
	#if 0
		// TODO: This does not work well on Windows
		mItems.emplace_back(Item { "Open log file",						Item::Function::OPEN_LOGFILE });
	#endif

		mBaseInnerRect.set(2, 24, 210, 24);
		mBaseOuterSize.set(mBaseInnerRect.width + 4, (int)(mItems.size()) * 24 + 28);
	}
}

void ApplicationContextMenu::mouse(const rmx::MouseEvent& ev)
{
	if (FTX::System->wasEventConsumed())
	{
		mActive = false;
		mContextMenuClick.set(-1, -1);
		return;
	}

	if (ev.state)
	{
		if (ev.button == rmx::MouseButton::Right && !mItems.empty() && isOpeningAllowedNow() && !FTX::keyState(SDLK_LALT) && !FTX::keyState(SDLK_RALT))
		{
			mActive = !mActive;
			if (mActive)
			{
				RMX_LOG_INFO("Menu: right-click menu opened");		// (what the automated browser checks wait for)

				// Determine a good position where to show the context menu, so it stays inside the game window
				mContextMenuPosition = ev.position;
				if (mContextMenuPosition.x + mBaseOuterSize.x > FTX::screenWidth())
				{
					mContextMenuPosition.x = std::max(mContextMenuPosition.x - mBaseOuterSize.x, 0);
				}
				if (mContextMenuPosition.y + mBaseOuterSize.y > FTX::screenHeight())
				{
					mContextMenuPosition.y = std::max(mContextMenuPosition.y - mBaseOuterSize.y, 0);
				}
			}
			mContextMenuClick.set(-1, -1);
			return;
		}
		else if (ev.button == rmx::MouseButton::Left)
		{
			if (mActive)
			{
				mContextMenuClick = ev.position;
				return;
			}
		}
	}

	GuiBase::mouse(ev);
}

void ApplicationContextMenu::update(float timeElapsed)
{
	GuiBase::update(timeElapsed);

	// A level that starts while the menu is open (only the game's own file browser would be left to open) closes the menu
	if (mActive && !isOpeningAllowedNow())
		mActive = false;
}

void ApplicationContextMenu::render()
{
	GuiBase::render();

	if (mActive)
	{
		Drawer& drawer = EngineMain::instance().getDrawer();

		Recti rect(mContextMenuPosition.x, mContextMenuPosition.y, mBaseOuterSize.x, mBaseOuterSize.y);
		drawer.drawRect(rect, Color(0.3f, 0.3f, 0.3f));

		rect.addPos(mBaseInnerRect.getPos());
		rect.setSize(mBaseInnerRect.getSize());

		{
			const Recti textRect(rect.x + 5, rect.y - 21, 0, rect.height);
			drawer.printText(global::mOxyfontRegular, textRect, "Sonic 3 A.I.R.", 4);
		}

		Item* clickedItem = nullptr;
		for (Item& item : mItems)
		{
			rect.y += 2;
			const bool selected = rect.contains(FTX::mousePos()) && !FTX::System->wasEventConsumed();
			if (mContextMenuClick.x >= 0 && rect.contains(mContextMenuClick))
			{
				clickedItem = &item;
			}

			drawer.drawRect(rect, selected ? Color(0.4f, 0.4f, 0.1f) : Color(0.1f, 0.1f, 0.1f));

			const Recti textRect(rect.x + 10, rect.y + 1, 0, rect.height);
			drawer.printText(global::mOxyfontRegular, textRect, item.mText, 4);
			rect.y += 22;
		}

		if (mContextMenuClick.x >= 0)
		{
			if (nullptr != clickedItem)
			{
				switch (clickedItem->mFunction)
				{
					case Item::Function::OPEN_SAVED_DATA_DIRECTORY:
						openDirectory(Configuration::instance().mAppDataPath, false);
						break;

					case Item::Function::OPEN_MODS_DIRECTORY:
						openDirectory(Configuration::instance().mAppDataPath + L"mods/", true);
						break;

					case Item::Function::OPEN_LOGFILE:
						PlatformFunctions::openFileExternal(Configuration::instance().mAppDataPath + L"logfile.txt");
						break;
				}
			}

			// Close context menu again in any case
			mActive = false;
		}

		drawer.performRendering();
	}
}
