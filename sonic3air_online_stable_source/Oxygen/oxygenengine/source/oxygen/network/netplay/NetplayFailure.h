/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>


// Why joining or hosting an online game did not work, in words a player understands (the lobby shows the text and offers a retry).
// Header only, so that the developer test program can check that every reason has a message.
namespace NetplayFailure
{
	enum class Reason : uint8
	{
		NONE = 0,
		SERVER_UNREACHABLE,		// The matchmaking server did not answer
		WRONG_CODE,				// Nobody hosts with this code, or the code does not belong to this host
		LOBBY_FULL,				// The host has no free player slot
		VERSION_MISMATCH,		// Different game version
		MOD_MISMATCH,			// Different mods, mod versions, mod settings or mod order
		BLOCKED_OR_TIMEOUT,		// The host was found (or its address is known) but no connection could be made: most likely a router or firewall blocks it
		HOST_CLOSED,			// The host left or closed the lobby, or stopped answering
		CONNECTION_LOST,		// A running game lost its connection and could not get it back
		BAD_GAME_DATA,			// The game data sent by the host was damaged or implausible
		COUNT
	};

	inline const char* getName(Reason reason)
	{
		switch (reason)
		{
			case Reason::SERVER_UNREACHABLE:	return "SERVER_UNREACHABLE";
			case Reason::WRONG_CODE:			return "WRONG_CODE";
			case Reason::LOBBY_FULL:			return "LOBBY_FULL";
			case Reason::VERSION_MISMATCH:		return "VERSION_MISMATCH";
			case Reason::MOD_MISMATCH:			return "MOD_MISMATCH";
			case Reason::BLOCKED_OR_TIMEOUT:	return "BLOCKED_OR_TIMEOUT";
			case Reason::HOST_CLOSED:			return "HOST_CLOSED";
			case Reason::CONNECTION_LOST:		return "CONNECTION_LOST";
			case Reason::BAD_GAME_DATA:			return "BAD_GAME_DATA";
			default:							return "NONE";
		}
	}

	inline const char* getMessage(Reason reason)
	{
		switch (reason)
		{
			case Reason::SERVER_UNREACHABLE:	return "Could not reach the matchmaking server.\nCheck your internet connection and try again.";
			case Reason::WRONG_CODE:			return "That code did not work.\nCheck the code, and that the host has opened a lobby.";
			case Reason::LOBBY_FULL:			return "This lobby is full.\nAsk the host for a free player slot.";
			case Reason::VERSION_MISMATCH:		return "The host uses a different version of the game.\nBoth players need the same version.";
			case Reason::MOD_MISMATCH:			return "The host uses different mods or mod settings.\nBoth players need exactly the same mods.";
			case Reason::BLOCKED_OR_TIMEOUT:	return "Found the host, but could not connect.\nA router or firewall probably blocks the connection.";
			case Reason::HOST_CLOSED:			return "The host closed the lobby or stopped answering.";
			case Reason::CONNECTION_LOST:		return "The connection to the host was lost.\nJoin again with the host's code.";
			case Reason::BAD_GAME_DATA:			return "The game data from the host was damaged.\nTry joining again.";
			default:							return "";
		}
	}

	// A new attempt with the same code can make sense (a fixed network problem, a host that was not ready yet)
	inline bool isRetryable(Reason reason)
	{
		return (reason != Reason::NONE);
	}
}
