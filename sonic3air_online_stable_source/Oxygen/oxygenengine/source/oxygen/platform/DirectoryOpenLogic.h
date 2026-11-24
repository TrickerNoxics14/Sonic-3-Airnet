/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once


// What a button like "Open mods folder" does on a platform. Pure logic without any engine dependency, so that the developer test program can check it.
//  - the computer has a file manager the game can start (Windows, Mac, Linux): start it
//  - it has none (a browser, a phone) but the game has its own file browser: use that one (a button must never do nothing)
//  - neither: do not show the button at all
enum class DirectoryOpenAction
{
	EXTERNAL,		// PlatformFunctions::openDirectoryExternal
	FILE_BROWSER,	// the game's own file browser (GameApp::openFileBrowser)
	NONE			// nothing can open a folder here, so the button is not offered
};

constexpr DirectoryOpenAction chooseDirectoryOpenAction(bool canOpenExternally, bool hasOwnFileBrowser)
{
	return canOpenExternally ? DirectoryOpenAction::EXTERNAL :
		   hasOwnFileBrowser ? DirectoryOpenAction::FILE_BROWSER : DirectoryOpenAction::NONE;
}

// The game's own file browser fills the whole screen while a running level plays on beneath it (and an online match goes on without this player),
// so it is only offered where the game stands still (menus, title screen, pause menu). A file manager opens in a window of its own, at any time.
constexpr bool isDirectoryOpenAllowedNow(DirectoryOpenAction action, bool gameplayRunning)
{
	return action == DirectoryOpenAction::EXTERNAL || (action == DirectoryOpenAction::FILE_BROWSER && !gameplayRunning);
}
