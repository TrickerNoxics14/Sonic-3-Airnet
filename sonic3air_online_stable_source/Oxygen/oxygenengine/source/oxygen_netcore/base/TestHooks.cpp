/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen_netcore/pch.h"
#include "oxygen_netcore/base/TestHooks.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <unistd.h>
	#include <climits>
#endif


namespace
{
	const char* const MARKER_FILE_NAME = "testhooks.enabled";

	bool markerFileExists()
	{
	#ifdef _WIN32
		wchar_t path[32768];
		const DWORD length = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
		if (length == 0 || length >= (DWORD)(sizeof(path) / sizeof(path[0])))
			return false;
		std::wstring file(path, length);
		const size_t slash = file.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
			return false;
		file.resize(slash + 1);
		for (const char* p = MARKER_FILE_NAME; *p != 0; ++p)
			file += (wchar_t)*p;
		const DWORD attributes = GetFileAttributesW(file.c_str());
		return (attributes != INVALID_FILE_ATTRIBUTES) && ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
	#else
		char path[PATH_MAX + 1] = { 0 };
		const ssize_t length = readlink("/proc/self/exe", path, PATH_MAX);
		if (length <= 0)
			return false;
		std::string directory(path, (size_t)length);
		const size_t slash = directory.find_last_of('/');
		if (slash == std::string::npos)
			return false;
		directory.resize(slash + 1);
		return (access((directory + MARKER_FILE_NAME).c_str(), F_OK) == 0);
	#endif
	}
}


namespace TestHooks
{
	bool isEnabled()
	{
		static const bool enabled = markerFileExists();
		return enabled;
	}

	const char* getEnv(const char* name)
	{
		return isEnabled() ? std::getenv(name) : nullptr;
	}

	const char* getMarkerFileName()
	{
		return MARKER_FILE_NAME;
	}
}
