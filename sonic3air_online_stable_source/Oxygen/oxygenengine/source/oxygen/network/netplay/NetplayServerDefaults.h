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
// Nobody runs one for you by default, so the default is empty and the game then says so kindly and offers private, LAN and matchmaking server play.
//
// Format: a name or address, optionally with the UDP port: "relay.example.org" or "relay.example.org:28842" (the default port is 28842).
// Windows and Android games use UDP to that address; the browser build uses a WebSocket to wss://<name> (the page's relay_config.js can override it).
// A player can always overwrite it in the game (ONLINE > SETTINGS > LOBBY SERVER, or settings.json: Online > LobbyServer).
namespace NetplayServerDefaults
{
	static const char* const DEFAULT_LOBBY_SERVER = "";
}
