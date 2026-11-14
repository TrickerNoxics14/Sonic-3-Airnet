/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// Direct (peer to peer) connections between the players of a relayed lobby, as an optional shortcut around the relay server.
// Included by Sockets.cpp only (it defines functions, see below).
//
// The relay (RelayProtocol.h) stays the lobby, the signalling channel and the fallback. When two players could also talk to each other directly, the friend offers
// a WebRTC data channel (unordered, no retransmissions: it behaves like UDP, which is what the game's own protocol was written for), the offer / answer / ICE candidates
// travel through the relay as SIGNAL messages, and once the channel works the UDPSocket sends the game's datagrams over it instead of through the relay. Both
// paths always stay accepted for receiving, so a switch in either direction loses at most the datagrams that were in flight (the game's protocol already copes with
// lost, duplicated and reordered datagrams: tracked packets have unique ids, inputs carry frame numbers and older ones are ignored).
//
// Peers are numbered like at the relay: the host sees friend N as peer N, a friend sees the host as peer 0.
//
// Implementations: the browser build (here, the browser's own RTCPeerConnection through emscripten) and nothing else yet. On other platforms this class
// reports "unsupported" and the UDPSocket stays on the relay (see CLAUDE_HANDOFF.md for why the Windows game has no implementation).
#include <rmxbase.h>
#include <vector>
#include <string>

#ifdef __EMSCRIPTEN__
	#include <emscripten.h>
#endif

namespace DirectProtocol
{
	// First byte of a signalling payload (the rest is text); the relay never reads it
	enum SignalKind : uint8
	{
		OFFER	  = 1,	// session description (SDP) of the friend
		ANSWER	  = 2,	// session description (SDP) of the host
		CANDIDATE = 3,	// one ICE candidate, JSON {"c":"candidate:...","m":"0","i":0}
		END		  = 4,	// no more candidates
		BYE		  = 5	// this side gave the direct connection up
	};

	// First byte of every message on the data channel itself
	static const uint8 CHANNEL_GAME = 0;	// the rest is one datagram of the game
	static const uint8 CHANNEL_PROBE = 1;	// keep-alive (second byte: 1 = "I hear you"), what proves that the channel works in both directions
	static const size_t MAX_DIRECT_DATAGRAM = 16000;
}


#ifdef __EMSCRIPTEN__

// ---- the JavaScript side: one object (globalThis.__s3p2p) owns the RTCPeerConnections ----
EM_JS(int, s3p2p_init, (const char* iceText, int iceLen, int enabled), {
	const text = iceLen > 0 ? new TextDecoder().decode(HEAPU8.subarray(iceText, iceText + iceLen)) : "";
	if (globalThis.__s3p2p) { try { globalThis.__s3p2p.closeAll(); } catch (e) {} clearInterval(globalThis.__s3p2p.timer); }
	const G = globalThis.__s3p2p = {
		peers: new Map(), outSignals: [], inbox: [], ice: text.split(',').map((s) => s.trim()).filter(Boolean), lastID: 0,
		enabled: !!enabled && typeof RTCPeerConnection !== 'undefined', msgsIn: 0, msgsOut: 0, bytesIn: 0, bytesOut: 0,
		ATTEMPT_MS: 8000, STALE_MS: 2000, DEAD_MS: 5000,
	};
	const now = () => performance.now();
	const log = (m) => console.log('Netplay (browser): ' + m);
	G.out = (id, kind, str) => {
		const body = new TextEncoder().encode(str);
		const msg = new Uint8Array(body.length + 1); msg[0] = kind; msg.set(body, 1);
		if (G.outSignals.length < 64) G.outSignals.push({ id, msg });
	};
	G.fail = (peer, why) => {
		if (peer.state === 'failed') return;
		const wasOpen = peer.state === 'open';
		peer.state = 'failed'; peer.failedAt = now();
		try { peer.dc && peer.dc.close(); } catch (e) {}
		try { peer.pc.close(); } catch (e) {}
		log('direct connection to peer ' + peer.id + ' ' + (wasOpen ? 'lost' : 'did not work') + ' (' + why + '), using the relay server');
	};
	// The channel carries the game only while BOTH directions work: we hear the peer (fresh) and the peer's last keep-alive said that it hears us (a one-way
	// failure would otherwise lose everything the healthy direction still sends over it)
	G.hearing = (peer) => peer.gotRx && (now() - peer.lastRx) < G.STALE_MS;
	G.usable = (peer) => peer.state === 'open' && G.hearing(peer) && peer.peerHears && (now() - peer.lastProbe) < G.STALE_MS;
	G.probe = (peer) => { try { peer.dc.send(new Uint8Array([1, G.hearing(peer) ? 1 : 0])); } catch (e) {} };
	G.close = (id, sendBye) => {
		const peer = G.peers.get(id);
		if (!peer) return;
		G.peers.delete(id);
		if (sendBye && peer.state !== 'failed') G.out(id, 5, "");
		peer.state = 'closed';
		try { peer.dc && peer.dc.close(); } catch (e) {}
		try { peer.pc.close(); } catch (e) {}
	};
	G.closeAll = () => { for (const id of [...G.peers.keys()]) G.close(id, true); G.inbox.length = 0; };
	G.create = (id, initiator) => {
		G.close(id, false);
		const pc = new RTCPeerConnection({ iceServers: G.ice.length ? [{ urls: G.ice }] : [] });
		const peer = { id, pc, dc: null, state: 'connecting', startedAt: now(), openedAt: 0, lastRx: 0, gotRx: false, failedAt: 0, pending: [], remoteSet: false, initiator, peerHears: false, lastProbe: 0 };
		G.peers.set(id, peer);
		pc.onicecandidate = (e) => { if (e.candidate) G.out(id, 3, JSON.stringify({ c: e.candidate.candidate, m: e.candidate.sdpMid, i: e.candidate.sdpMLineIndex })); else G.out(id, 4, ""); };
		pc.onconnectionstatechange = () => { if (peer.state !== 'closed' && (pc.connectionState === 'failed' || pc.connectionState === 'closed')) G.fail(peer, 'connection ' + pc.connectionState); };
		const hook = (dc) => {
			peer.dc = dc; dc.binaryType = 'arraybuffer';
			dc.onopen = () => { if (peer.state === 'connecting') { peer.state = 'open'; peer.openedAt = now(); G.probe(peer); } };
			dc.onclose = () => { if (peer.state !== 'closed') G.fail(peer, 'data channel closed'); };
			dc.onmessage = (e) => {
				const b = new Uint8Array(e.data);
				if (b.length === 0) return;
				peer.lastRx = now(); peer.gotRx = true;
				if (b[0] === 1) { peer.lastProbe = now(); peer.peerHears = (b.length > 1 && b[1] === 1); }
				if (b[0] === 0 && b.length > 1 && G.inbox.length < 2048) { G.inbox.push({ id, data: b.subarray(1) }); ++G.msgsIn; G.bytesIn += b.length - 1; }
			};
		};
		if (initiator) {
			hook(pc.createDataChannel('s3air', { ordered: false, maxRetransmits: 0 }));
			pc.createOffer().then((o) => pc.setLocalDescription(o)).then(() => G.out(id, 1, pc.localDescription.sdp)).catch((e) => G.fail(peer, 'offer: ' + e));
		} else {
			pc.ondatachannel = (e) => hook(e.channel);
		}
		return peer;
	};
	G.flush = (peer) => { peer.remoteSet = true; for (const c of peer.pending.splice(0)) peer.pc.addIceCandidate(c).catch(() => {}); };
	G.signal = (id, kind, str) => {
		if (!G.enabled) return;
		if (kind === 1) {
			if (!G.peers.has(id) && G.peers.size >= 8) return;
			const peer = G.create(id, false);
			peer.pc.setRemoteDescription({ type: 'offer', sdp: str }).then(() => { G.flush(peer); return peer.pc.createAnswer(); })
				.then((a) => peer.pc.setLocalDescription(a)).then(() => G.out(id, 2, peer.pc.localDescription.sdp)).catch((e) => G.fail(peer, 'answer: ' + e));
			return;
		}
		const peer = G.peers.get(id);
		if (!peer || peer.state === 'failed') return;
		if (kind === 2) {
			if (peer.remoteSet) return;
			peer.pc.setRemoteDescription({ type: 'answer', sdp: str }).then(() => G.flush(peer)).catch((e) => G.fail(peer, 'answer rejected: ' + e));
		} else if (kind === 3) {
			let c; try { const j = JSON.parse(str); c = { candidate: j.c, sdpMid: j.m, sdpMLineIndex: j.i }; } catch (e) { return; }
			if (peer.remoteSet) peer.pc.addIceCandidate(c).catch(() => {}); else if (peer.pending.length < 64) peer.pending.push(c);
		} else if (kind === 5) {
			G.fail(peer, 'the other side gave up');
		}
	};
	G.timer = setInterval(() => {
		const t = now();
		for (const peer of G.peers.values()) {
			if (peer.state === 'connecting' && t - peer.startedAt > G.ATTEMPT_MS) G.fail(peer, 'no connection after ' + (G.ATTEMPT_MS / 1000) + ' s');
			else if (peer.state === 'open') {
				if (peer.gotRx && t - peer.lastRx > G.DEAD_MS) G.fail(peer, 'nothing received for ' + (G.DEAD_MS / 1000) + ' s');
				else if (!peer.gotRx && t - peer.openedAt > G.ATTEMPT_MS) G.fail(peer, 'the channel opened but nothing came through');
				else G.probe(peer);
			}
		}
	}, 250);
	return G.enabled ? 1 : 0;
});

EM_JS(void, s3p2p_start, (int id), { const G = globalThis.__s3p2p; if (G && G.enabled) { try { G.create(id, true); } catch (e) { console.log('Netplay (browser): could not start a direct connection: ' + e); } } });
EM_JS(void, s3p2p_close, (int id), { const G = globalThis.__s3p2p; if (G) G.close(id, true); });
EM_JS(void, s3p2p_shutdown, (), { const G = globalThis.__s3p2p; if (G) { G.closeAll(); clearInterval(G.timer); G.enabled = false; } });
// 0 = nothing, 1 = connecting (or open but not proven yet / gone quiet), 2 = direct path in use, 3 = failed
EM_JS(int, s3p2p_state, (int id), { const G = globalThis.__s3p2p; if (!G) return 0; const p = G.peers.get(id); if (!p) return 0; if (p.state === 'failed') return 3; return G.usable(p) ? 2 : 1; });
EM_JS(double, s3p2p_failed_ago, (int id), { const G = globalThis.__s3p2p; const p = G && G.peers.get(id); return (p && p.state === 'failed') ? (performance.now() - p.failedAt) / 1000 : -1; });
EM_JS(void, s3p2p_signal_in, (int id, const uint8_t* data, int len), {
	const G = globalThis.__s3p2p; if (!G || len < 1) return;
	try { G.signal(id, HEAPU8[data], new TextDecoder().decode(HEAPU8.subarray(data + 1, data + len))); } catch (e) { console.log('Netplay (browser): bad direct-connection message: ' + e); }
});
// Next signalling message to send to the relay: returns its length and writes the peer number to *outID (0 = none)
EM_JS(int, s3p2p_signal_out, (uint8_t* buffer, int capacity, int* outID), {
	const G = globalThis.__s3p2p; if (!G || G.outSignals.length === 0) return 0;
	const item = G.outSignals.shift();
	if (item.msg.length > capacity) return 0;
	HEAPU8.set(item.msg, buffer); HEAP32[outID >> 2] = item.id;
	return item.msg.length;
});
EM_JS(int, s3p2p_send, (int id, const uint8_t* data, int len), {
	const G = globalThis.__s3p2p; if (!G) return 0;
	const p = G.peers.get(id);
	if (!p || !G.usable(p) || p.dc.bufferedAmount > 262144) return 0;
	try { const u = new Uint8Array(len + 1); u[0] = 0; u.set(HEAPU8.subarray(data, data + len), 1); p.dc.send(u); ++G.msgsOut; G.bytesOut += len; return 1; } catch (e) { return 0; }
});
EM_JS(int, s3p2p_receive, (uint8_t* buffer, int capacity, int* outID), {
	const G = globalThis.__s3p2p; if (!G || G.inbox.length === 0) return 0;
	const item = G.inbox.shift();
	if (item.data.length > capacity) return -1;
	HEAPU8.set(item.data, buffer); HEAP32[outID >> 2] = item.id;
	return item.data.length;
});

#endif


// The same interface on every platform (on others it does nothing and says it is unsupported)
class DirectLink
{
public:
	enum class State { NONE = 0, CONNECTING = 1, DIRECT = 2, FAILED = 3 };

	~DirectLink()  { shutdown(); }

	// Starts (or restarts) the service. "iceServers" is a comma separated list of STUN server URLs (may be empty: then only addresses of the computer itself are tried).
	// Returns false if the platform or browser cannot do direct connections.
	bool setup(const std::string& iceServers, bool enabled)
	{
	#ifdef __EMSCRIPTEN__
		mActive = (s3p2p_init(iceServers.c_str(), (int)iceServers.size(), enabled ? 1 : 0) != 0);
	#else
		(void)iceServers;  (void)enabled;
		mActive = false;
	#endif
		return mActive;
	}

	void shutdown()
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)
			s3p2p_shutdown();
	#endif
		mActive = false;
	}

	inline bool isActive() const  { return mActive; }

	void startOffer(uint16 peer)
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)  s3p2p_start(peer);
	#else
		(void)peer;
	#endif
	}

	void closePeer(uint16 peer)
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)  s3p2p_close(peer);
	#else
		(void)peer;
	#endif
	}

	State getState(uint16 peer) const
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)  return (State)s3p2p_state(peer);
	#else
		(void)peer;
	#endif
		return State::NONE;
	}

	// Seconds since the attempt failed (-1 if it did not)
	double getFailedAgo(uint16 peer) const
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)  return s3p2p_failed_ago(peer);
	#else
		(void)peer;
	#endif
		return -1.0;
	}

	// A signalling message (first byte = DirectProtocol::SignalKind) that the relay brought from the peer
	void onSignal(uint16 peer, const uint8* data, size_t length)
	{
	#ifdef __EMSCRIPTEN__
		if (mActive && length >= 1 && length <= 3000)  s3p2p_signal_in(peer, data, (int)length);
	#else
		(void)peer;  (void)data;  (void)length;
	#endif
	}

	// Next signalling message that has to go to the relay (for this peer), if any
	bool pollSignal(uint16& outPeer, std::vector<uint8>& outMessage)
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)
		{
			uint8 buffer[3000];
			int id = 0;
			const int length = s3p2p_signal_out(buffer, (int)sizeof(buffer), &id);
			if (length > 0)
			{
				outPeer = (uint16)id;
				outMessage.assign(buffer, buffer + length);
				return true;
			}
		}
	#else
		(void)outPeer;  (void)outMessage;
	#endif
		return false;
	}

	bool sendTo(uint16 peer, const uint8* data, size_t length)
	{
	#ifdef __EMSCRIPTEN__
		if (mActive && length > 0 && length <= DirectProtocol::MAX_DIRECT_DATAGRAM)
			return s3p2p_send(peer, data, (int)length) != 0;
	#else
		(void)peer;  (void)data;  (void)length;
	#endif
		return false;
	}

	// Next datagram received over a direct channel (false if none)
	bool receive(uint16& outPeer, std::vector<uint8>& outData)
	{
	#ifdef __EMSCRIPTEN__
		if (mActive)
		{
			static uint8 buffer[DirectProtocol::MAX_DIRECT_DATAGRAM + 16];
			for (int tries = 0; tries < 4; ++tries)
			{
				int id = 0;
				const int length = s3p2p_receive(buffer, (int)sizeof(buffer), &id);
				if (length == 0)
					break;
				if (length < 0)
					continue;
				outPeer = (uint16)id;
				outData.assign(buffer, buffer + length);
				return true;
			}
		}
	#else
		(void)outPeer;  (void)outData;
	#endif
		return false;
	}

private:
	bool mActive = false;
};
