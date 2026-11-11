/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once


// Developer test switches (S3AIR_NETPLAY, S3AIR_LOGNAME, S3AIR_LOBBY_*, S3AIR_NET_LAG, ...) are environment variables.
// A leftover variable on a player's computer must never make the game host a session, write memory dumps or drop network packets.
// So the switches are honoured only if a marker file named "testhooks.enabled" lies next to the executable.
// The check runners create that file for the duration of a run; the release package never contains it
// (make_package.ps1 refuses to ship it).
namespace TestHooks
{
	// True only if the marker file exists next to the executable (checked once per process)
	bool isEnabled();

	// Replacement for getenv() for every S3AIR_* test switch: returns nullptr unless the test hooks are enabled
	const char* getEnv(const char* name);

	inline bool hasEnv(const char* name)  { return getEnv(name) != nullptr; }

	// Name of the marker file (next to the executable)
	const char* getMarkerFileName();
}
