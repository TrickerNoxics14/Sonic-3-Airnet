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
	bool setupAsHostWithSecret(uint16 port, uint32 secret);		// Developer test: a host with a given secret
	inline uint32 getHostSecret() const  { return mHostSecret; }
	inline bool isLanRequester(const std::string& ip) const  { return mUDPSocket.hasLanRequester(ip); }
	void startJoinViaServer();
	void startJoinDirect(std::string_view ip, uint16 port, uint32 secret);
	void closeConnections();

	// The name other players see (the option "Online player name", default "Player"); always clean, see NetplayLobby::sanitizeName
	std::string getLocalPlayerName() const;
	void setLocalPlayerName(const std::string& name);

	// Input delay chosen by the host: -1 = Auto, otherwise a number of frames; kept while the lobby is closed and opened again
	inline int getInputDelaySetting() const  { return mInputDelaySetting; }
	void setInputDelaySetting(int setting);

	// For the game scripts: the name and the ping of a player slot while playing online ("" / -1 when unknown)
	std::string getPlayerName(int slot) const;
	int getPlayerPingMs(int slot) const;

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

private:
	UDPSocket mUDPSocket;
	float mLanSearchTimer = 0.0f;
	uint32 mHostSecret = 0;
	int mInputDelaySetting = NetplayLobby::INPUT_DELAY_AUTO;
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
