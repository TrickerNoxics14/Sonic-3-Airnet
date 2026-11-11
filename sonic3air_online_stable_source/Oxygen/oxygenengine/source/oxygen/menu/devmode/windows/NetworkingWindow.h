/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/menu/imgui/ImGuiDefinitions.h"

#if defined(SUPPORT_IMGUI)

#include "oxygen/menu/devmode/DevModeWindowBase.h"


class NetworkingWindow : public DevModeWindowBase
{
public:
	NetworkingWindow();

	virtual void buildContent() override;
};

class OnlineCoopWindow : public ImGuiContentProvider
{
public:
	static const uint64 PROVIDER_KEY = rmx::constMurmur2_64("OnlineCoopWindow");
	static bool sIsOpen;
	virtual void buildImGuiContent() override;

private:
	char mIpBuffer[64] = "";
	int mSelectedAddress = 0;
	bool mHostedViaServer = false;
};

#endif
