/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>


class LogDisplay : public SingleInstance<LogDisplay>
{
friend class Application;

public:
	// Mode display (single line)
	void setModeDisplay(const String& string);

	// Log display (single line)
	void setLogDisplay(const String& string, float time = 2.0f);

	// Banner (browser build, online play): a notice that stays as long as the situation does, e.g. "Waiting for Sam"; empty clears it
	void setBannerDisplay(const std::string& text);

	// Log error display
	void clearLogErrors();
	void addLogError(const String& string);

private:
	String mModeDisplayString;

	std::string mBannerString;

	String mLogDisplayString;
	float  mLogDisplayTimeout = 0.0f;

	std::vector<String> mLogErrorStrings;
};
