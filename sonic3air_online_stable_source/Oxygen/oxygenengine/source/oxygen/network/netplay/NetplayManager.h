/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/network/netplay/ExternalAddressQuery.h"
#include "oxygen/network/netplay/NetplayFailure.h"
#include "oxygen/network/netplay/NetplayLobbyLogic.h"

#include "oxygen_netcore/network/ConnectionListener.h"
#include "oxygen_netcore/network/ConnectionManager.h"

class ControlsIn;
class NetplayClient;
class NetplayHost;


class NetplayManager : public ConnectionListenerInterface, public SingleInstance<NetplayManager>
{
public:
	static const uint16 DEFAULT_HOST_PORT = 28840;
	static const uint32 ROLE_ADDRESS = 0x8fff00;	// Shared memory address that tells the scripts about the online role of this computer

public:
	NetplayManager();
	~NetplayManager();

	inline ConnectionManager& getConnectionManager()		{ return mConnectionManager; }
	inline bool isUsingIPv6() const							{ return mUseIPv6; }

	inline NetplayHost* getNetplayHost() 					{ return mNetplayHost; }
	inline NetplayClient* getNetplayClient()				{ return mNetplayClient; }
	inline const ExternalAddressQuery& getExternalAddressQuery() const  { return mExternalAddressQuery; }

	// Text describing the active mods, in order and with versions (both players need the same ones)
	static std::string getActiveModsInfo();

	// Fingerprint of the current game state, to find out whether two games are still identical
	static uint32 computeStateHash(uint32 frameNumber = 0);

	uint64 mSessionID = 0x12345;		// Identifies the game at the game server (hosts and friends who use the same ID get connected)
	std::string mLastErrorMessage;		// Shown to the player, e.g. when joining failed because of different mods
	NetplayFailure::Reason mLastFailure = NetplayFailure::Reason::NONE;		// The reason behind "mLastErrorMessage", so the menu can offer the right way to continue

	// Hosting: with "directSecret" the host is joined by an IP join code that contains a fresh random secret (see "getHostSecret"), otherwise by the session code
	bool setupAsHost(bool registerSessionAtServer, uint16 port = DEFAULT_HOST_PORT, bool directSecret = false);
	bool setupAsHostWithSecret(uint16 port, uint64 secret);		// Developer test: a host with a given secret
	inline uint64 getHostSecret() const  { return mHostSecret; }
	inline bool isLanRequester(const std::string& ip) const  { return mUDPSocket.hasLanRequester(ip); }
	void startJoinViaServer();
	void startJoinDirect(std::string_view ip, uint16 port, uint64 secret);
	void startJoinIntroduced(uint64 sessionID, std::string_view ip, uint16 port);		// Developer test: as if the game server had introduced the host at this address (the session code is known, so a relay is possible)
	void closeConnections();

	// Relay server, an optional fallback (settings.json: Online / RelayServer; empty by default). It is used only if the setting is not empty:
	//  - a host with a session code registers its lobby at the relay, so friends who cannot reach it directly can still play through the relay
	//  - a friend whose direct connection does not work after a few seconds (strict NAT, firewall) connects through the relay instead
	// Direct (IP) join codes never use it. See RelayProtocol.h and DEPLOY_RELAY.txt.
	enum class RelayState { OFF, CONNECTING, ACTIVE };
	static const constexpr float RELAY_FALLBACK_SECONDS = 8.0f;		// How long the direct connection gets before the relay is tried
	std::string getRelayServerSetting() const;
	bool isRelayConfigured() const;
	inline bool canUseRelayAsClient() const  { return mRelaySessionKnown && isRelayConfigured(); }
	bool beginRelayAsClient();										// Starts talking to the relay; false if there is none or its name does not resolve
	inline bool isRelayJoined() const  { return mUDPSocket.getRelayRole() == UDPSocket::RelayRole::CLIENT && mUDPSocket.isRelayReady(); }
	inline int getRelayRefusal() const  { return mUDPSocket.getRelayRefusal(); }
	inline bool isRelayHostRegistered() const  { return mUDPSocket.getRelayRole() == UDPSocket::RelayRole::HOST && mUDPSocket.isRelayReady(); }
	inline bool isRelayHostConfigured() const  { return mUDPSocket.getRelayRole() == UDPSocket::RelayRole::HOST; }
	inline uint64 getRelayDatagramsIn() const  { return mUDPSocket.getRelayDatagramsIn(); }
	inline uint64 getRelayDatagramsOut() const  { return mUDPSocket.getRelayDatagramsOut(); }

	// The name other players see (the option "Online player name", default "Player"); always clean, see NetplayLobby::sanitizeName
	std::string getLocalPlayerName() const;
	void setLocalPlayerName(const std::string& name);

	// Input delay chosen by the host: -1 = Auto, otherwise a number of frames; kept while the lobby is closed and opened again
	inline int getInputDelaySetting() const  { return mInputDelaySetting; }
	void setInputDelaySetting(int setting);

	// For the game scripts: the name and the ping of a player slot while playing online ("" / -1 when unknown)
	std::string getPlayerName(int slot) const;
	int getPlayerPingMs(int slot) const;

	// Character choice. Every player picks the character they play with in the lobby (see NetplayLobbyLogic.h). The list of characters that can be picked is read from the
	// game's scripts (an extra-slot mod provides "ExtraChar.startup_charN"), so it is never fixed here; without such a mod it is Sonic, Tails and Knuckles.
	const NetplayLobby::CharacterList& getAvailableCharacters();		// Cached; read again when the active mods change
	void invalidateCharacterList();
	inline bool takeCharacterListChanged()  { const bool changed = mCharacterListChanged; mCharacterListChanged = false; return changed; }		// True once after the list changed (the host then checks every pick again)
	int getConfiguredPartyExtraSlot(int slot) const;					// The mod setting "Player 3/4 Extra Character Slot" (0 = none): the default character of that player
	inline uint16 getHostCharacterPick() const  { return mHostCharacterPick; }
	void setHostCharacterPick(uint16 pick);								// The host's own pick; checked like everybody's, ignored once the game has started
	uint16 getCharacterPickFromEnvironment() const;						// Developer test switch S3AIR_CHARACTER (a name or a number), CHARACTER_NONE if not set

	// The picks that were fixed when the game started, identical on every computer. They are written to the shared memory every frame while a netplay game runs
	// (PICKS_ADDRESS: u8 "valid", then four times a low byte and a high byte), which is where the game scripts read them.
	static const uint32 PICKS_ADDRESS = 0x8fff50;
	void setSessionCharacters(const uint16 picks[4]);
	void clearSessionCharacters();
	inline bool hasSessionCharacters() const  { return mSessionCharactersValid; }
	inline uint16 getSessionCharacter(int slot) const  { return (slot >= 0 && slot < 4) ? mSessionCharacters.mPicks[slot] : NetplayLobby::CHARACTER_NONE; }

	// Remembers why something failed, shows it in plain language and writes it to the online log
	void reportFailure(NetplayFailure::Reason reason, const std::string& detail = std::string());
	void clearFailure();

	void updateConnections(float deltaSeconds);
	bool onReceivedGameServerPacket(ReceivedPacketEvaluation& evaluation);

	bool canBeginNextFrame(uint32 frameNumber);
	void onFrameUpdate(ControlsIn& controlsIn, uint32 frameNumber);

protected:
	virtual NetConnection* createNetConnection(ConnectionManager& connectionManager, const SocketAddress& senderAddress) override;
	virtual void destroyNetConnection(NetConnection& connection) override;

	virtual bool onReceivedConnectionlessPacket(ConnectionlessPacketEvaluation& evaluation)	override;
	virtual bool onReceivedPacket(ReceivedPacketEvaluation& evaluation) override;

private:
	bool restartConnection(bool asHost, uint16 hostPort = 0);
	bool resolveRelayAddress(SocketAddress& outAddress) const;
	bool startRelay(UDPSocket::RelayRole role);

private:
	UDPSocket mUDPSocket;
	bool mRelaySessionKnown = false;		// This join started from a session code, which is what the relay needs (a direct IP join code has none)
	bool mRelayReadyLogged = false;
	float mLanSearchTimer = 0.0f;
	uint64 mHostSecret = 0;
	int mInputDelaySetting = NetplayLobby::INPUT_DELAY_AUTO;
	NetplayLobby::CharacterList mCharacterList;
	bool mCharacterListKnown = false;
	uint32 mCharacterListTicks = 0;
	bool mCharacterListChanged = false;
	bool mCharacterListLogged = false;
	uint16 mHostCharacterPick = NetplayLobby::CHARACTER_MENU;
	NetplayLobby::SessionCharacters mSessionCharacters;
	bool mSessionCharactersValid = false;
	float mTestCloseTimer = 0.0f;
	float mOnlineLogTimer = 0.0f;
	bool mOnlineLogStarted = false;
	uint32 mOnlineLogLastFrame = 0;
	void writeOnlineLog(float deltaSeconds);
	ConnectionManager mConnectionManager;
	bool mUseIPv6 = false;

	NetplayHost* mNetplayHost = nullptr;
	NetplayClient* mNetplayClient = nullptr;
	ExternalAddressQuery mExternalAddressQuery;
};
