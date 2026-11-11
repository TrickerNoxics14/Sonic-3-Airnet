# A small UDP proxy that makes a connection on this computer behave like a bad internet connection.
#
#   Friend (joins 127.0.0.1:ListenPort)  <-->  this proxy  <-->  host (127.0.0.1:TargetPort)
#
# Every datagram in either direction is delayed, randomly delayed a bit more (jitter), and sometimes thrown away (loss).
#   One-way delay of a datagram = RttMs / 2 + a random value between -JitterMs and +JitterMs (so RttMs 200, JitterMs 30 means 100 ms +- 30 ms each way).
#   Loss: each datagram is dropped with the probability LossPercent, independently, in both directions.
#   Datagrams may overtake each other when the jitter is large (like on a real network), unless -NoReorder is given.
# The proxy only listens on 127.0.0.1 (it is never reachable from other computers).
# It writes counters to -StatsFile once per second, so that a test can prove that the traffic really went through it:
#   c2s = friend to host, s2c = host to friend, "delay_avg" is the average added one-way delay in milliseconds.
#
# Usage (normally started by checks\run_checks.ps1, stage W):
#   powershell -File checks\udp_lag_proxy.ps1 -ListenPort 28841 -TargetPort 28840 -RttMs 200 -JitterMs 30 -LossPercent 3 -StatsFile stats.txt
# Stop it with Ctrl+C or by ending the process (or pass -Seconds to let it end by itself).

param(
	[int]$ListenPort = 28841,
	[string]$TargetHost = '127.0.0.1',
	[int]$TargetPort = 28840,
	[double]$RttMs = 200,
	[double]$JitterMs = 30,
	[double]$LossPercent = 3,
	[string]$StatsFile = '',
	[int]$Seed = 1234,
	[int]$Seconds = 0,
	[switch]$NoReorder
)

$ErrorActionPreference = 'Stop'
if ($TargetHost -ne '127.0.0.1' -and $TargetHost -ne 'localhost') { throw 'This tool is for tests on one computer: the target has to be 127.0.0.1.' }

$source = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;

public class LagProxy
{
	[DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint milliseconds);

	class Item
	{
		public double DueMs;
		public long Sequence;
		public byte[] Data;
		public Socket Via;
		public EndPoint To;
	}

	class ItemComparer : IComparer<Item>
	{
		public int Compare(Item a, Item b)
		{
			int result = a.DueMs.CompareTo(b.DueMs);
			return (result != 0) ? result : a.Sequence.CompareTo(b.Sequence);
		}
	}

	class Session
	{
		public EndPoint Client;
		public Socket Upstream;
	}

	static Stopwatch clock = Stopwatch.StartNew();
	static SortedSet<Item> queue = new SortedSet<Item>(new ItemComparer());
	static object queueLock = new object();
	static long sequence = 0;
	static Random random;
	static object randomLock = new object();
	static Dictionary<string, Session> sessions = new Dictionary<string, Session>();
	static Socket listener;
	static IPEndPoint target;
	static double rtt, jitter, loss;
	static bool reorder;
	static double lastDueC2S = 0, lastDueS2C = 0;
	static long c2sForwarded, c2sDropped, s2cForwarded, s2cDropped;
	static double delaySum;
	static long delayCount;
	static volatile bool stop;

	static double NextDouble() { lock (randomLock) { return random.NextDouble(); } }

	static void DisableConnectionReset(Socket socket)
	{
		try { socket.IOControl(-1744830452, new byte[] { 0, 0, 0, 0 }, null); } catch (Exception) { }		// SIO_UDP_CONNRESET: an unreachable port must not end the receive loop
	}

	static void Schedule(byte[] data, Socket via, EndPoint to, bool clientToServer)
	{
		if (NextDouble() * 100.0 < loss)
		{
			if (clientToServer) Interlocked.Increment(ref c2sDropped); else Interlocked.Increment(ref s2cDropped);
			return;
		}
		double delay = rtt / 2.0 + (NextDouble() * 2.0 - 1.0) * jitter;
		if (delay < 0) delay = 0;
		double now = clock.Elapsed.TotalMilliseconds;
		double due = now + delay;
		lock (queueLock)
		{
			if (!reorder)
			{
				if (clientToServer) { if (due < lastDueC2S) due = lastDueC2S; lastDueC2S = due; }
				else { if (due < lastDueS2C) due = lastDueS2C; lastDueS2C = due; }
			}
			Item item = new Item();
			item.DueMs = due; item.Sequence = ++sequence; item.Data = data; item.Via = via; item.To = to;
			queue.Add(item);
			delaySum += (due - now); delayCount++;
		}
		if (clientToServer) Interlocked.Increment(ref c2sForwarded); else Interlocked.Increment(ref s2cForwarded);
	}

	static void SchedulerLoop()
	{
		while (!stop)
		{
			Item item = null;
			lock (queueLock)
			{
				if (queue.Count > 0)
				{
					Item first = queue.Min;
					if (first.DueMs <= clock.Elapsed.TotalMilliseconds)
					{
						item = first;
						queue.Remove(first);
					}
				}
			}
			if (item == null) { Thread.Sleep(1); continue; }
			try { item.Via.SendTo(item.Data, item.To); } catch (Exception) { }
		}
	}

	static void UpstreamLoop(Session session)
	{
		byte[] buffer = new byte[65536];
		while (!stop)
		{
			try
			{
				EndPoint from = new IPEndPoint(IPAddress.Any, 0);
				int length = session.Upstream.ReceiveFrom(buffer, ref from);
				byte[] copy = new byte[length];
				Buffer.BlockCopy(buffer, 0, copy, 0, length);
				Schedule(copy, listener, session.Client, false);
			}
			catch (SocketException) { if (stop) return; }
			catch (ObjectDisposedException) { return; }
		}
	}

	static void ClientLoop()
	{
		byte[] buffer = new byte[65536];
		while (!stop)
		{
			try
			{
				EndPoint from = new IPEndPoint(IPAddress.Any, 0);
				int length = listener.ReceiveFrom(buffer, ref from);
				string key = from.ToString();
				Session session;
				lock (sessions)
				{
					if (!sessions.TryGetValue(key, out session))
					{
						session = new Session();
						session.Client = from;
						session.Upstream = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
						session.Upstream.Bind(new IPEndPoint(IPAddress.Loopback, 0));
						DisableConnectionReset(session.Upstream);
						sessions[key] = session;
						Thread thread = new Thread(() => UpstreamLoop(session));
						thread.IsBackground = true;
						thread.Start();
					}
				}
				byte[] copy = new byte[length];
				Buffer.BlockCopy(buffer, 0, copy, 0, length);
				Schedule(copy, session.Upstream, target, true);
			}
			catch (SocketException) { if (stop) return; }
			catch (ObjectDisposedException) { return; }
		}
	}

	static void WriteStats(string path, Stopwatch started)
	{
		if (string.IsNullOrEmpty(path)) return;
		string text;
		lock (queueLock)
		{
			text = "uptime_s=" + (int)started.Elapsed.TotalSeconds
				+ "\nc2s_forwarded=" + Interlocked.Read(ref c2sForwarded) + "\nc2s_dropped=" + Interlocked.Read(ref c2sDropped)
				+ "\ns2c_forwarded=" + Interlocked.Read(ref s2cForwarded) + "\ns2c_dropped=" + Interlocked.Read(ref s2cDropped)
				+ "\ndelay_avg_ms=" + (delayCount > 0 ? (delaySum / delayCount).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) : "0")
				+ "\nsessions=" + sessions.Count + "\n";
		}
		try { File.WriteAllText(path, text); } catch (Exception) { }
	}

	public static void Run(int listenPort, int targetPort, double rttMs, double jitterMs, double lossPercent, string statsFile, int seed, int seconds, bool allowReorder)
	{
		rtt = rttMs; jitter = jitterMs; loss = lossPercent; reorder = allowReorder;
		random = new Random(seed);
		target = new IPEndPoint(IPAddress.Loopback, targetPort);
		try { timeBeginPeriod(1); } catch (Exception) { }

		listener = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
		listener.Bind(new IPEndPoint(IPAddress.Loopback, listenPort));
		DisableConnectionReset(listener);

		Thread scheduler = new Thread(SchedulerLoop); scheduler.IsBackground = true; scheduler.Start();
		Thread clientThread = new Thread(ClientLoop); clientThread.IsBackground = true; clientThread.Start();

		Stopwatch started = Stopwatch.StartNew();
		Console.WriteLine("Lag proxy: 127.0.0.1:" + listenPort + " -> 127.0.0.1:" + targetPort + ", RTT " + rttMs + " ms, jitter +-" + jitterMs + " ms per direction, loss " + lossPercent + " %" + (allowReorder ? "" : ", no reordering"));
		while (!stop)
		{
			Thread.Sleep(1000);
			WriteStats(statsFile, started);
			if (seconds > 0 && started.Elapsed.TotalSeconds >= seconds) stop = true;
		}
		try { listener.Close(); } catch (Exception) { }
	}
}
'@

Add-Type -TypeDefinition $source -Language CSharp
if ($StatsFile -ne '') { Set-Content -LiteralPath $StatsFile -Value 'uptime_s=0' -Encoding ascii }
[LagProxy]::Run($ListenPort, $TargetPort, $RttMs, $JitterMs, $LossPercent, $StatsFile, $Seed, $Seconds, (-not $NoReorder.IsPresent))
