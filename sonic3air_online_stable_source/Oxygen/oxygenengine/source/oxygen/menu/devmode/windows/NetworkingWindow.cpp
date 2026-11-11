/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/menu/devmode/windows/NetworkingWindow.h"

#if defined(SUPPORT_IMGUI)

#include "oxygen/menu/imgui/ImGuiHelpers.h"
#include "oxygen/network/EngineServerClient.h"
#include "oxygen/network/netplay/NetplayClient.h"
#include "oxygen/network/netplay/NetplayManager.h"
#include "oxygen/network/netplay/NetplayHost.h"
#include "oxygen/network/netplay/NetplayCodes.h"
#include "oxygen/network/netplay/NetplayAuth.h"
#include <random>




NetworkingWindow::NetworkingWindow() :
	DevModeWindowBase("Networking", Category::MISC, ImGuiWindowFlags_AlwaysAutoResize)
{
}

void NetworkingWindow::buildContent()
{
	ImGui::SetWindowPos(ImVec2(5.0f, 150.0f), ImGuiCond_FirstUseEver);

	// Engine server
	{
		ImGui::SeparatorText("Server");
		ImGuiHelpers::ScopedIndent si;

		EngineServerClient& engineServerClient = EngineServerClient::instance();

		switch (engineServerClient.getServerConnection().getState())
		{
			case NetConnection::State::EMPTY:					ImGui::Text("Inactive");  break;
			case NetConnection::State::TCP_READY:				ImGui::Text("TCP ready");  break;
			case NetConnection::State::REQUESTED_CONNECTION:	ImGui::Text("Connecting...");  break;
			case NetConnection::State::ACCEPTED:				ImGui::Text("Connection accepted");  break;
			case NetConnection::State::CONNECTED:				ImGui::Text("Connected");  break;
			case NetConnection::State::DISCONNECTED:			ImGui::Text("Disconnected");  break;
		}

		if (engineServerClient.getServerConnection().getState() == NetConnection::State::CONNECTED)
		{
			ImGui::SameLine();
			ImGui::Text("|  Server IP = %s, port = %d", engineServerClient.getServerConnection().getRemoteAddress().getIP().c_str(), engineServerClient.getServerConnection().getRemoteAddress().getPort());

			if (ImGui::Button("Disconnect"))
			{
				engineServerClient.disconnectFromServer();
			}
		}
		else
		{
			if (ImGui::Button("Start connection"))
			{
				engineServerClient.connectToServer();
			}

			Configuration::GameServerBase& config = Configuration::instance().mGameServerBase;
			static ImGuiHelpers::InputString serverNameInput(config.mServerHostName);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("   Server:");
			ImGui::SameLine();
			ImGui::PushItemWidth(200);
			if (ImGuiHelpers::InputText("##ServerName", serverNameInput))
			{
				config.mServerHostName = serverNameInput.get();
			}
			ImGui::PopItemWidth();
		}
	}

	// Netplay
	{
		ImGui::SeparatorText("Netplay");
		ImGuiHelpers::ScopedIndent si;

		NetplayManager& netplayManager = NetplayManager::instance();
		NetplayHost* netplayHost = netplayManager.getNetplayHost();
		NetplayClient* netplayClient = netplayManager.getNetplayClient();

		if (nullptr != netplayHost)
		{
			ImGui::Text("Host: %d connections", (int)netplayHost->getPlayerConnections().size());

			int index = 0;
			for (NetplayHost::PlayerConnection* connection : netplayHost->getPlayerConnections())
			{
				ImGui::BulletText("Client %d (IP = %s, port = %d):", index + 1, connection->getRemoteAddress().getIP().c_str(), connection->getRemoteAddress().getPort());
				ImGuiHelpers::ScopedIndent si2(28.0f);

				switch (connection->getState())
				{
					case NetConnection::State::EMPTY:				 ImGui::Text("No connection");  break;
					case NetConnection::State::TCP_READY:			 ImGui::Text("TCP ready");  break;
					case NetConnection::State::REQUESTED_CONNECTION: ImGui::Text("Requested connection");  break;
					case NetConnection::State::ACCEPTED:			 ImGui::Text("Connection accepted");  break;
					case NetConnection::State::CONNECTED:			 ImGui::Text("Connected");  break;
					case NetConnection::State::DISCONNECTED:		 ImGui::Text("Disconnected");  break;
				}

				if (connection->getState() == NetConnection::State::CONNECTED)
				{
					ImGui::SameLine();
					ImGui::Text("-  Netplay latency:  %d frames", connection->mCurrentLatency);
				}
				++index;
			}
		}
		else if (nullptr != netplayClient)
		{
			switch (netplayClient->getHostConnection().getState())
			{
				case NetConnection::State::EMPTY:				 ImGui::Text("Client: No connection");  break;
				case NetConnection::State::TCP_READY:			 ImGui::Text("Client: TCP ready");  break;
				case NetConnection::State::REQUESTED_CONNECTION: ImGui::Text("Client: Requested connection");  break;
				case NetConnection::State::ACCEPTED:			 ImGui::Text("Client: Connection accepted");  break;
				case NetConnection::State::CONNECTED:			 ImGui::Text("Client: Connected");  break;
				case NetConnection::State::DISCONNECTED:		 ImGui::Text("Client: Disconnected");  break;
			}
		}
		else
		{
			ImGui::Text("Inactive");
		}

		ImGui::BeginDisabled(nullptr != netplayHost);
		{
			if (ImGui::Button("Host via server"))
			{
				netplayManager.setupAsHost(true, NetplayManager::DEFAULT_HOST_PORT);
			}

			ImGui::SameLine();
			if (ImGui::Button("Host directly"))
			{
				netplayManager.setupAsHost(false, NetplayManager::DEFAULT_HOST_PORT);
			}
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(nullptr != netplayClient);
		{
			static char ipBuffer[32] = { 0 };
			static int port = NetplayManager::DEFAULT_HOST_PORT;
			if (ipBuffer[0] == 0)
				strcpy_s(ipBuffer, 32, netplayManager.isUsingIPv6() ? "::1" : "127.0.0.1");

			if (ImGui::Button("Join via server"))
			{
				netplayManager.startJoinViaServer();
			}

			ImGui::SameLine();
			if (ImGui::Button("Join directly"))
			{
				netplayManager.startJoinDirect(ipBuffer, port, NetplayAuth::secretFromSession(netplayManager.mSessionID));		// Developer window: the secret comes from the session ID field
			}

			ImGui::SameLine();
			ImGui::Text(" IP:");
			ImGui::SameLine();
			ImGui::PushItemWidth(90);
			ImGuiHelpers::InputText("##IP", ipBuffer, 32);
			ImGui::PopItemWidth();

			ImGui::SameLine();
			ImGui::Text("Port:");
			ImGui::SameLine();
			ImGui::PushItemWidth(50);
			ImGui::InputInt("##Port", &port, 0, 0, ImGuiInputTextFlags_CharsDecimal);
			ImGui::PopItemWidth();
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(nullptr == netplayHost && nullptr == netplayClient);
		if (ImGui::Button("Close connection"))
		{
			netplayManager.closeConnections();
		}
		ImGui::EndDisabled();

		if (!netplayManager.getExternalAddressQuery().mOwnExternalIP.empty())
		{
			ImGui::Text("Retrieved own external address: IP = %s, port = %d", netplayManager.getExternalAddressQuery().mOwnExternalIP.c_str(), netplayManager.getExternalAddressQuery().mOwnExternalPort);
		}

		if (nullptr != netplayHost)
		{
			const NetplayHost::HostState state = netplayHost->getHostState();
			switch (state)
			{
				case NetplayHost::HostState::NONE:				ImGui::Text("Inactive");  break;
				case NetplayHost::HostState::CONNECT_TO_SERVER:	ImGui::Text("Connecting to game server");  break;
				case NetplayHost::HostState::REGISTERED:		ImGui::Text("Registered at game server");  break;
				case NetplayHost::HostState::GAME_RUNNING:		ImGui::Text("Game running");  break;
				case NetplayHost::HostState::FAILED:			ImGui::Text("Failed");  break;
			}

			ImGui::BeginDisabled(state < NetplayHost::HostState::REGISTERED || state > NetplayHost::HostState::GAME_RUNNING);
			if (state == NetplayHost::HostState::REGISTERED)
			{
				if (ImGui::Button("Start game"))
				{
					netplayHost->startGame();
				}
			}
			else
			{
				if (state == NetplayHost::HostState::GAME_RUNNING)
				{
					int frameNumber = 0;
					const uint32 checksum = netplayHost->getRegularInputChecksum(frameNumber);
					ImGui::BulletText("Input checksum:  %08x for frame #%d", checksum, frameNumber);
				}

				if (ImGui::Button("Stop game"))
				{
					// TODO: Provide a better implementation
					netplayManager.closeConnections();
				}
			}
			ImGui::EndDisabled();
		}

		if (nullptr != netplayClient)
		{
			const NetplayClient::State state = netplayClient->getState();
			switch (state)
			{
				case NetplayClient::State::NONE:				ImGui::Text("Inactive");  break;
				case NetplayClient::State::CONNECT_TO_SERVER:	ImGui::Text("Connecting to game server");  break;
				case NetplayClient::State::REGISTERED:			ImGui::Text("Registered at game server");  break;
				case NetplayClient::State::CONNECT_TO_HOST:		ImGui::Text("Connecting to host");  break;
				case NetplayClient::State::CONNECTED:			ImGui::Text("Connected to host");  break;
				case NetplayClient::State::GAME_RUNNING:		ImGui::Text("Game running");  break;
				case NetplayClient::State::FAILED:				ImGui::Text("Failed");  break;
			}

			if (state == NetplayClient::State::CONNECT_TO_HOST)
			{
				if (netplayClient->getReceivedPunchthroughPacketSender().isValid())
					ImGui::Text("Received punchthrough packet from IP = %s, port = %d", netplayClient->getReceivedPunchthroughPacketSender().getIP().c_str(), netplayClient->getReceivedPunchthroughPacketSender().getPort());
				else
					ImGui::Text("No punchthrough packet received");
			}
			else if (state == NetplayClient::State::GAME_RUNNING)
			{
				int frameNumber = 0;
				const uint32 checksum = netplayClient->getRegularInputChecksum(frameNumber);
				ImGui::BulletText("Input checksum:  %08x for frame #%d", checksum, frameNumber);
				ImGui::BulletText("Netplay latency:  %d frames", netplayClient->getCurrentLatency());
			}
		}
	}
}

namespace
{
	using NetplayCodes::makeJoinCode;
	using NetplayCodes::readJoinCode;
	using NetplayCodes::makeSessionCode;
	using NetplayCodes::readSessionCode;
	std::vector<std::string> getLocalAddresses();
	bool isLikelyVpnAddress(const std::string& ip)
	{
		return ip.compare(0, 3, "26.") == 0 || ip.compare(0, 3, "25.") == 0 || ip.compare(0, 4, "100.") == 0;
	}
}
bool OnlineCoopWindow::sIsOpen = false;

void OnlineCoopWindow::buildImGuiContent()
{
	if (!sIsOpen)
		return;

	NetplayManager& netplayManager = NetplayManager::instance();
	NetplayHost* host = netplayManager.getNetplayHost();
	NetplayClient* client = netplayManager.getNetplayClient();

	
	ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Online Co-op", &sIsOpen, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::End();
		return;
	}
	ImGui::SetWindowFontScale(1.6f);

	// Error from the last attempt (like different mods)
	if (!netplayManager.mLastErrorMessage.empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", netplayManager.mLastErrorMessage.c_str());
		if (ImGui::Button("OK"))
			netplayManager.mLastErrorMessage.clear();
		ImGui::Separator();
	}

	if (nullptr != host)
	{
		// Hosting
		if (mHostedViaServer)
		{
			const auto& connections = host->getPlayerConnections();
			bool friendConnected = false;
			for (const auto* player : connections)
				friendConnected = friendConnected || (player->getState() == NetConnection::State::CONNECTED);

			if (host->getHostState() == NetplayHost::HostState::FAILED)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Could not reach the matchmaking server.\nCheck your internet connection, or host with a VPN instead.");
			}
			else if (!host->isRegisteredAtServer())
			{
				ImGui::Text("Contacting the matchmaking server...");
			}
			else
			{
				if (friendConnected)
					ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Your friend is connected.\nThey play as player 2 (Tails).");
				else
					ImGui::Text("Waiting for a friend to join...");
				const std::string code = makeSessionCode(netplayManager.mSessionID);
				ImGui::Spacing();
				ImGui::Text("Give your friend this code:");
				ImGui::SetWindowFontScale(2.4f);
				ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "%s", code.c_str());
				ImGui::SetWindowFontScale(1.6f);
				if (ImGui::Button("Copy code"))
					ImGui::SetClipboardText(code.c_str());
			}
			ImGui::Spacing();
			if (ImGui::Button("Stop hosting"))
				netplayManager.closeConnections();
		}
		else
		{
		const auto& players = host->getPlayerConnections();
		bool connected = false;
		for (const auto* player : players)
			connected = connected || (player->getState() == NetConnection::State::CONNECTED);

		if (connected)
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Your friend is connected.\nThey play as player 2 (Tails).");
		else
			ImGui::Text("Waiting for a friend to join...");

		ImGui::Spacing();
		// Which address does the code stand for? A VPN address (Hamachi, Radmin, ZeroTier, Tailscale...) keeps the public address private
		std::vector<std::string> addresses;
		{
			const std::vector<std::string> local = getLocalAddresses();
			for (const std::string& ip : local)
				if (isLikelyVpnAddress(ip))
					addresses.push_back(ip);
			const ExternalAddressQuery& query = netplayManager.getExternalAddressQuery();
			if (!query.mOwnExternalIP.empty())
				addresses.push_back(query.mOwnExternalIP);
			for (const std::string& ip : local)
				if (!isLikelyVpnAddress(ip))
					addresses.push_back(ip);
		}
		if (mSelectedAddress >= (int)addresses.size())
			mSelectedAddress = 0;

		if (addresses.empty())
		{
			ImGui::Text("Looking up your address...");
		}
		else
		{
			const std::string code = makeJoinCode(addresses[mSelectedAddress], NetplayManager::DEFAULT_HOST_PORT, netplayManager.getHostSecret());
			ImGui::Text("Give your friend this code:");
			ImGui::SetWindowFontScale(2.4f);
			ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "%s", code.c_str());
			ImGui::SetWindowFontScale(1.6f);
			if (ImGui::Button("Copy code"))
				ImGui::SetClipboardText(code.c_str());

			ImGui::Spacing();
			ImGui::TextDisabled("The code works over the address you pick here (the address itself is not shown):");
			for (int i = 0; i < (int)addresses.size(); ++i)
			{
				const bool vpn = isLikelyVpnAddress(addresses[i]);
				const bool isPublic = (addresses[i] == netplayManager.getExternalAddressQuery().mOwnExternalIP);
				const char* label = vpn ? "VPN (recommended: keeps your real address private)" : isPublic ? "Internet (needs a forwarded port: UDP 28840)" : "Home network";
				ImGui::PushID(i);
				ImGui::RadioButton(label, &mSelectedAddress, i);
				ImGui::PopID();
			}
		}

		ImGui::Spacing();
		if (ImGui::Button("Stop hosting"))
			netplayManager.closeConnections();
		}
	}
	else if (nullptr != client)
	{
		// Joining
		switch (client->getState())
		{
			case NetplayClient::State::CONNECT_TO_SERVER:	ImGui::Text("Contacting the matchmaking server...");  break;
			case NetplayClient::State::REGISTERED:		ImGui::Text("Looking for the host...");  break;
			case NetplayClient::State::CONNECT_TO_HOST:	ImGui::Text("Connecting to the host...");  break;
			case NetplayClient::State::CONNECTED:		ImGui::Text("Connected, waiting for the game...");  break;
			case NetplayClient::State::GAME_RUNNING:	ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "You are playing together.\nPress any button to play as Tails.");  break;
			case NetplayClient::State::FAILED:			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Could not join.");  break;
			default:									ImGui::Text("...");  break;
		}
		if (client->getState() == NetplayClient::State::GAME_RUNNING)
		{
			ImGui::Text("Delay: %d frames", client->getCurrentLatency());
			if (client->isReceivingGameState())
				ImGui::Text("Receiving the game from the host...");
			else
				ImGui::Text("Sync: OK (last check at frame %u), repairs so far: %d", client->getLastSyncOkFrame(), client->getDesyncCount());
		}

		ImGui::Spacing();
		if (ImGui::Button("Leave"))
			netplayManager.closeConnections();
	}
	else
	{
		ImGui::TextWrapped("Play together online. One player hosts, the other joins with the host's code.\nBoth players need the same game version and the same mods.");
		ImGui::Separator();

		ImGui::Text("Host a game");
		ImGui::TextWrapped("Start (or keep) playing, open this window and press Host. Your friend can join at any moment.");
		if (ImGui::Button("Host (works anywhere)"))
		{
			// The game server connects both players; the friend only needs the code
			netplayManager.mLastErrorMessage.clear();
			std::random_device randomDevice;
			uint64 sessionID = (((uint64)randomDevice() << 32) | randomDevice()) & 0xffffffffffull;
			if (sessionID < 0x10000000ull)
				sessionID += 0x10000000ull;
			netplayManager.mSessionID = sessionID;
			mHostedViaServer = netplayManager.setupAsHost(true, NetplayManager::DEFAULT_HOST_PORT);
		}
		ImGui::SameLine();
		if (ImGui::Button("Host (VPN or same network)"))
		{
			netplayManager.mLastErrorMessage.clear();
			mHostedViaServer = false;
			// The join code contains a fresh random secret: only friends who got the code can get in
			netplayManager.setupAsHost(false, NetplayManager::DEFAULT_HOST_PORT, true);
		}

		ImGui::Separator();
		ImGui::Text("Join a friend's game");
		ImGui::SetNextItemWidth(260.0f);
		ImGuiHelpers::InputText("Code", mIpBuffer, sizeof(mIpBuffer));
		if (ImGui::Button("Join"))
		{
			netplayManager.mLastErrorMessage.clear();
			uint64 sessionID = 0;
			std::string ip;
			uint16 port = NetplayManager::DEFAULT_HOST_PORT;
			uint32 secret = 0;
			if (readSessionCode(mIpBuffer, sessionID))
			{
				netplayManager.mSessionID = sessionID;
				netplayManager.startJoinViaServer();
			}
			else if (readJoinCode(mIpBuffer, ip, port, secret))
				netplayManager.startJoinDirect(ip, port, secret);
			else
				netplayManager.reportFailure(NetplayFailure::Reason::WRONG_CODE, "Type the code exactly as your friend sees it (an IP address alone is not enough).");
		}
	}

	ImGui::Separator();
	if (nullptr != host || nullptr != client)
		ImGui::TextDisabled("If something goes wrong, send the file  %%APPDATA%%\\Sonic3AIR\\online_log.txt");
	ImGui::TextDisabled("Press F9 to close this window.");
	ImGui::End();
}


// Addresses of this computer's network adapters, including those of a VPN
#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>

namespace
{
	std::vector<std::string> getLocalAddresses()
	{
		std::vector<std::string> result;
		char hostName[256] = { 0 };
		if (gethostname(hostName, sizeof(hostName)) == 0)
		{
			addrinfo hints = {};
			hints.ai_family = AF_INET;
			addrinfo* list = nullptr;
			if (getaddrinfo(hostName, nullptr, &hints, &list) == 0)
			{
				for (addrinfo* entry = list; nullptr != entry; entry = entry->ai_next)
				{
					char buffer[64] = { 0 };
					const sockaddr_in* address = (const sockaddr_in*)entry->ai_addr;
					inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer));
					if (buffer[0] != 0 && strncmp(buffer, "127.", 4) != 0 && strncmp(buffer, "169.254.", 8) != 0)
						result.emplace_back(buffer);
				}
				freeaddrinfo(list);
			}
		}
		return result;
	}
}
#else
namespace
{
	std::vector<std::string> getLocalAddresses()
	{
		return std::vector<std::string>();
	}
}
#endif

#endif
