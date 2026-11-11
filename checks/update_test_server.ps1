# Loopback-only test server for the in-game updater checks (stage T). Pretends to be the GitHub release API and its download CDN.
# Binds 127.0.0.1 only and runs only while a test runs (run_update_checks.ps1 starts it and stops it again).
#
# Folder layout under -Root:
#   server.json      behaviour, read again for every request: { "apiStatus": 200, "rateLimit": false, "partialAfter": 0, "slowKBps": 0, "redirect": true }
#   api_latest.json  the answer to GET /repos/<anything>/releases/latest
#   files\<name>     served at /dl/<name> (with redirect: 302 to /cdn/<name>, like GitHub does for release assets)
# Every request is appended to -LogFile as "METHOD path status".
param(
	[Parameter(Mandatory = $true)][int]$Port,
	[Parameter(Mandatory = $true)][string]$Root,
	[Parameter(Mandatory = $true)][string]$LogFile,
	[int]$RunSeconds = 900
)

$source = @"
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Collections.Generic;

public static class UpdateTestServer
{
	static string root, logFile;
	static object logLock = new object();

	static void Log(string text)
	{
		lock (logLock) { File.AppendAllText(logFile, text + "\r\n"); }
	}

	static Dictionary<string, object> LoadConfig()
	{
		try { return new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(root, "server.json"))); }
		catch { return new Dictionary<string, object>(); }
	}

	static long GetLong(Dictionary<string, object> config, string key, long fallback)
	{
		object value;
		if (config.TryGetValue(key, out value) && value != null) { try { return Convert.ToInt64(value); } catch { } }
		return fallback;
	}

	static bool GetBool(Dictionary<string, object> config, string key, bool fallback)
	{
		object value;
		if (config.TryGetValue(key, out value) && value is bool) return (bool)value;
		return fallback;
	}

	static void Send(NetworkStream stream, int status, string reason, string contentType, byte[] body, string extraHeaders, long declaredLength)
	{
		string head = "HTTP/1.1 " + status + " " + reason + "\r\nContent-Type: " + contentType + "\r\nContent-Length: " + declaredLength + "\r\nConnection: close\r\n" + extraHeaders + "\r\n";
		byte[] headBytes = Encoding.ASCII.GetBytes(head);
		stream.Write(headBytes, 0, headBytes.Length);
		if (body != null && body.Length > 0) stream.Write(body, 0, body.Length);
	}

	static void Handle(object state)
	{
		TcpClient client = (TcpClient)state;
		try
		{
			client.ReceiveTimeout = 10000;
			NetworkStream stream = client.GetStream();
			StreamReader reader = new StreamReader(stream, Encoding.ASCII);
			string requestLine = reader.ReadLine();
			if (requestLine == null) return;
			string line;
			while ((line = reader.ReadLine()) != null && line.Length > 0) { }
			string[] parts = requestLine.Split(' ');
			string method = parts[0];
			string path = parts.Length > 1 ? parts[1] : "/";
			Dictionary<string, object> config = LoadConfig();
			int apiStatus = (int)GetLong(config, "apiStatus", 200);

			if (path.StartsWith("/repos/") && path.EndsWith("/releases/latest"))
			{
				if (apiStatus != 200)
				{
					string extra = GetBool(config, "rateLimit", false) ? "X-RateLimit-Remaining: 0\r\n" : "";
					byte[] errorBody = Encoding.ASCII.GetBytes("{\"message\":\"test error\"}");
					Send(stream, apiStatus, "Test", "application/json", errorBody, extra, errorBody.Length);
					Log(method + " " + path + " " + apiStatus);
					return;
				}
				byte[] body = File.ReadAllBytes(Path.Combine(root, "api_latest.json"));
				Send(stream, 200, "OK", "application/json", body, "", body.Length);
				Log(method + " " + path + " 200");
				return;
			}

			bool isDownload = path.StartsWith("/dl/");
			bool isCdn = path.StartsWith("/cdn/");
			if (isDownload || isCdn)
			{
				string name = path.Substring(isDownload ? 4 : 5);
				if (name.Contains("/") || name.Contains("\\") || name.Contains("..")) { Send(stream, 400, "Bad", "text/plain", null, "", 0); Log(method + " " + path + " 400"); return; }
				string file = Path.Combine(root, "files", name);
				if (!File.Exists(file)) { Send(stream, 404, "Not Found", "text/plain", null, "", 0); Log(method + " " + path + " 404"); return; }
				if (isDownload && GetBool(config, "redirect", true))
				{
					Send(stream, 302, "Found", "text/plain", null, "Location: http://127.0.0.1:" + ((IPEndPoint)client.Client.LocalEndPoint).Port + "/cdn/" + name + "\r\n", 0);
					Log(method + " " + path + " 302");
					return;
				}
				long partialAfter = GetLong(config, "partialAfter", 0);
				long slowKBps = GetLong(config, "slowKBps", 0);
				using (FileStream input = File.OpenRead(file))
				{
					string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + input.Length + "\r\nConnection: close\r\n\r\n";
					byte[] headBytes = Encoding.ASCII.GetBytes(head);
					stream.Write(headBytes, 0, headBytes.Length);
					byte[] buffer = new byte[16 * 1024];
					long sent = 0;
					int read;
					while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
					{
						if (partialAfter > 0 && sent + read > partialAfter) { read = (int)(partialAfter - sent); stream.Write(buffer, 0, read); stream.Flush(); Log(method + " " + path + " partial " + (sent + read)); return; }
						stream.Write(buffer, 0, read);
						sent += read;
						if (slowKBps > 0) Thread.Sleep((int)(1000L * read / (slowKBps * 1024L)));
					}
				}
				Log(method + " " + path + " 200");
				return;
			}
			Send(stream, 404, "Not Found", "text/plain", null, "", 0);
			Log(method + " " + path + " 404");
		}
		catch (Exception ex) { try { Log("error " + ex.GetType().Name); } catch { } }
		finally { try { client.Close(); } catch { } }
	}

	public static void Run(int port, string rootFolder, string log, int seconds)
	{
		root = rootFolder; logFile = log;
		TcpListener listener = new TcpListener(IPAddress.Loopback, port);
		listener.Start();
		Log("listening " + port);
		DateTime end = DateTime.UtcNow.AddSeconds(seconds);
		while (DateTime.UtcNow < end)
		{
			if (listener.Pending()) { TcpClient client = listener.AcceptTcpClient(); ThreadPool.QueueUserWorkItem(Handle, client); }
			else Thread.Sleep(20);
		}
		listener.Stop();
	}
}
"@

Add-Type -TypeDefinition $source -ReferencedAssemblies System.Web.Extensions
[UpdateTestServer]::Run($Port, $Root, $LogFile, $RunSeconds)
