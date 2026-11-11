/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/network/netplay/ExternalAddressQuery.h"

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

	bool setupAsHost(bool registerSessionAtServer, uint16 port = DEFAULT_HOST_PORT);
	inline bool isLanRequester(const std::string& ip) const  { return mUDPSocket.hasLanRequester(ip); }
	void startJoinViaServer();
	void startJoinDirect(std::string_view ip, uint16 port);
	void closeConnections();

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
