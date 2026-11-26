/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// THE one place where the address of the public lobby server can be put before a build is shipped.
//
// The lobby server is the relay server program (checks/relay_ws, "node server.mjs") run by somebody: it keeps the list of public lobbies, hands out the
// 6 digit lobby codes and relays game data for players whose direct connection fails and for browser players.
//
// Format: "relay.example.org" or "relay.example.org:28842" for a server reached by UDP (the default port is 28842), or "wss://relay.example.org" for one reached by
// a WebSocket (a free web host takes no UDP; the Windows game then talks to it like a browser, so Windows and browser players share its lobbies).
// The default is the free Render relay that the published web page uses (checks/relay_ws/HOSTING.md, A). It is used by the Windows game only: the browser build
// takes its server from the page (relay_config.js), and the automated checks (test hooks on) never use it.
// A player can always overwrite it in the game (ONLINE > SETTINGS > LOBBY SERVER, or settings.json: Online > LobbyServer).
namespace NetplayServerDefaults
{
	static const char* const DEFAULT_LOBBY_SERVER = "wss://sonic3-airnet-relay.onrender.com";
}
