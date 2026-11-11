/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

// Pure logic of the in-game updater of the online fork (no sockets, no files, no game classes), so that the offline core tests can check it:
//  - version numbers and the "never downgrade" decision
//  - SHA-256
//  - safe relative paths for the files of an update package (zip-slip protection) and the files an update must never carry
//  - reading the release information of the GitHub REST API ("releases/latest") and the manifest asset
//  - which download addresses are accepted
// The including file must have rmxbase.h (Json::Value) available.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>


namespace selfupdate
{
	// ------------------------------------------------------------------------------------------------------------------------
	// Versions: "x.y.z" (a leading "v" is allowed). Anything else (suffixes like -beta, four parts, empty parts) is invalid.
	// ------------------------------------------------------------------------------------------------------------------------
	struct Version
	{
		int mPart[3] = { 0, 0, 0 };
		bool mValid = false;
	};

	inline bool parseVersion(const std::string& textIn, Version& out)
	{
		out = Version();
		std::string text = textIn;
		// Trim whitespace (version.txt ends with a line break)
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
			text.pop_back();
		size_t start = 0;
		while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
			++start;
		text = text.substr(start);
		if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
			text.erase(0, 1);
		if (text.empty() || text.size() > 24)
			return false;

		int parts = 0;
		size_t pos = 0;
		while (true)
		{
			size_t digits = 0;
			long long value = 0;
			while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
			{
				value = value * 10 + (text[pos] - '0');
				if (value > 99999)
					return false;
				++pos;
				++digits;
			}
			if (digits == 0 || parts >= 3)
				return false;
			out.mPart[parts++] = (int)value;
			if (pos >= text.size())
				break;
			if (text[pos] != '.')
				return false;
			++pos;
		}
		if (parts != 3)
			return false;
		out.mValid = true;
		return true;
	}

	inline int compareVersions(const Version& a, const Version& b)
	{
		for (int i = 0; i < 3; ++i)
		{
			if (a.mPart[i] != b.mPart[i])
				return (a.mPart[i] < b.mPart[i]) ? -1 : 1;
		}
		return 0;
	}

	inline std::string versionToString(const Version& v)
	{
		return std::to_string(v.mPart[0]) + "." + std::to_string(v.mPart[1]) + "." + std::to_string(v.mPart[2]);
	}

	enum class Decision
	{
		INVALID,				// One of the versions could not be read
		UP_TO_DATE,				// Same version
		UPDATE_AVAILABLE,		// Latest is newer
		OLDER_THAN_CURRENT		// Latest is older: never offered (no downgrade)
	};

	inline Decision decide(const Version& current, const Version& latest)
	{
		if (!current.mValid || !latest.mValid)
			return Decision::INVALID;
		const int cmp = compareVersions(latest, current);
		return (cmp == 0) ? Decision::UP_TO_DATE : (cmp > 0) ? Decision::UPDATE_AVAILABLE : Decision::OLDER_THAN_CURRENT;
	}


	// ------------------------------------------------------------------------------------------------------------------------
	// SHA-256
	// ------------------------------------------------------------------------------------------------------------------------
	class Sha256
	{
	public:
		Sha256()
		{
			reset();
		}

		void reset()
		{
			static const uint32_t INIT[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
			memcpy(mState, INIT, sizeof(mState));
			mBitCount = 0;
			mBufferSize = 0;
		}

		void update(const void* data, size_t size)
		{
			const uint8_t* bytes = (const uint8_t*)data;
			mBitCount += (uint64_t)size * 8;
			while (size > 0)
			{
				const size_t take = std::min<size_t>(size, 64 - mBufferSize);
				memcpy(mBuffer + mBufferSize, bytes, take);
				mBufferSize += take;
				bytes += take;
				size -= take;
				if (mBufferSize == 64)
				{
					transform(mBuffer);
					mBufferSize = 0;
				}
			}
		}

		std::string finishHex()
		{
			const uint64_t bitCount = mBitCount;
			const uint8_t one = 0x80;
			update(&one, 1);
			const uint8_t zero = 0;
			while (mBufferSize != 56)
				update(&zero, 1);
			uint8_t lengthBytes[8];
			for (int i = 0; i < 8; ++i)
				lengthBytes[i] = (uint8_t)(bitCount >> (56 - 8 * i));
			update(lengthBytes, 8);

			static const char* HEX = "0123456789abcdef";
			std::string result;
			for (int i = 0; i < 8; ++i)
			{
				for (int shift = 28; shift >= 0; shift -= 4)
					result += HEX[(mState[i] >> shift) & 0x0f];
			}
			reset();
			return result;
		}

		static std::string hashHex(const void* data, size_t size)
		{
			Sha256 sha;
			sha.update(data, size);
			return sha.finishHex();
		}

	private:
		static uint32_t rotr(uint32_t x, int n)  { return (x >> n) | (x << (32 - n)); }

		void transform(const uint8_t* block)
		{
			static const uint32_t K[64] =
			{
				0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
				0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
				0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
				0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
				0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
				0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
				0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
				0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
			};
			uint32_t w[64];
			for (int i = 0; i < 16; ++i)
				w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
			for (int i = 16; i < 64; ++i)
			{
				const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
				const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3], e = mState[4], f = mState[5], g = mState[6], h = mState[7];
			for (int i = 0; i < 64; ++i)
			{
				const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
				const uint32_t ch = (e & f) ^ (~e & g);
				const uint32_t t1 = h + S1 + ch + K[i] + w[i];
				const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
				const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t t2 = S0 + maj;
				h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			mState[0] += a; mState[1] += b; mState[2] += c; mState[3] += d; mState[4] += e; mState[5] += f; mState[6] += g; mState[7] += h;
		}

	private:
		uint32_t mState[8];
		uint64_t mBitCount = 0;
		uint8_t mBuffer[64];
		size_t mBufferSize = 0;
	};

	inline bool isHexSha256(const std::string& text)
	{
		if (text.size() != 64)
			return false;
		for (char c : text)
		{
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				return false;
		}
		return true;
	}

	inline std::string toLowerAscii(std::string text)
	{
		for (char& c : text)
		{
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
		}
		return text;
	}


	// ------------------------------------------------------------------------------------------------------------------------
	// Paths of an update package
	// ------------------------------------------------------------------------------------------------------------------------
	inline bool isReservedDeviceName(const std::string& componentLower)
	{
		// "con", "con.txt", "com1.bin" ... are devices on Windows whatever the extension
		std::string stem = componentLower.substr(0, componentLower.find('.'));
		while (!stem.empty() && stem.back() == ' ')
			stem.pop_back();
		static const char* NAMES[] = { "con", "prn", "aux", "nul", "conin$", "conout$" };
		for (const char* name : NAMES)
		{
			if (stem == name)
				return true;
		}
		if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '0' && stem[3] <= '9')
			return true;
		return false;
	}

	// Turns a zip entry name into a clean relative path with forward slashes, or refuses it.
	// Refused: empty, absolute (leading slash or backslash), drive letters and any colon (also NTFS streams), "." and ".." components,
	// empty components (a//b), control characters and the characters Windows does not allow, trailing dots or spaces in a component,
	// reserved device names, over-long names.
	// A trailing slash marks a directory entry (outIsDirectory).
	inline bool normalizeEntryPath(const std::string& name, std::string& outPath, bool& outIsDirectory, std::string* outReason = nullptr)
	{
		auto fail = [&](const char* reason) { if (outReason) *outReason = reason; return false; };
		outPath.clear();
		outIsDirectory = false;
		if (name.empty())
			return fail("empty name");
		if (name.size() > 240)
			return fail("name too long");

		std::string path = name;
		for (char& c : path)
		{
			if (c == '\\')
				c = '/';
		}
		for (unsigned char c : path)
		{
			if (c < 0x20 || c == 0x7f)
				return fail("control character");
			if (c == ':' )
				return fail("drive letter or stream marker");
			if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
				return fail("character not allowed on Windows");
		}
		if (path[0] == '/')
			return fail("absolute path");

		if (path.back() == '/')
		{
			outIsDirectory = true;
			path.pop_back();
			if (path.empty())
				return fail("empty name");
		}

		size_t start = 0;
		while (true)
		{
			size_t end = path.find('/', start);
			if (end == std::string::npos)
				end = path.size();
			const std::string component = path.substr(start, end - start);
			if (component.empty())
				return fail("empty path component");
			if (component == "." || component == "..")
				return fail("path climbs out of the folder");
			if (component.back() == '.' || component.back() == ' ')
				return fail("component ends with a dot or a space");
			if (isReservedDeviceName(toLowerAscii(component)))
				return fail("reserved device name");
			if (end >= path.size())
				break;
			start = end + 1;
		}
		outPath = path;
		return true;
	}

	// Things an update package must never carry: saves, save states, user settings, user mods, ROM and ROM-like files, test fixtures, the developer test marker.
	// (The check is the same as the package builder's guards, plus the user's own folders.)
	inline bool isForbiddenPackagePath(const std::string& normalizedPath, uint64_t size, std::string* outReason = nullptr)
	{
		const std::string lower = toLowerAscii(normalizedPath);
		auto fail = [&](const char* reason) { if (outReason) *outReason = reason; return true; };

		std::vector<std::string> components;
		size_t start = 0;
		while (start <= lower.size())
		{
			size_t end = lower.find('/', start);
			if (end == std::string::npos)
				end = lower.size();
			components.push_back(lower.substr(start, end - start));
			start = end + 1;
		}
		const std::string& fileName = components.back();
		for (const std::string& component : components)
		{
			if (component == "saves" || component == "states" || component == "savedata" || component == "mods")
				return fail("a save, save state or user mod folder");
			if (component == "checksharness" || component == "actharness" || component == "bossharness" || component == "netpartyharness" || component == "partyharness" || component == "teleportharness")
				return fail("a test fixture");
		}
		if (fileName == "testhooks.enabled")
			return fail("the developer test marker");
		if (fileName.compare(0, 8, "settings") == 0 && fileName.size() >= 5 && fileName.compare(fileName.size() - 5, 5, ".json") == 0)
			return fail("user settings");
		if (fileName == "active-mods.json")
			return fail("user mod settings");
		if (fileName.find("sonic_knuckles") != std::string::npos || fileName.find("sonic_3_and") != std::string::npos || fileName.find("_harness") != std::string::npos)
			return fail("a ROM or test fixture");
		const size_t dot = fileName.rfind('.');
		const std::string extension = (dot == std::string::npos) ? std::string() : fileName.substr(dot);
		if (extension == ".gen" || extension == ".smd" || extension == ".md" || extension == ".sav" || extension == ".srm")
			return fail("a ROM or save file");
		if (extension == ".bin" && size > 512 * 1024)
			return fail("a large .bin file (ROM-like)");
		return false;
	}

	// Files that are installed once but afterwards belong to the user: never overwritten by an update
	inline bool isKeepIfExistsPath(const std::string& normalizedPath)
	{
		return toLowerAscii(normalizedPath) == "config.json";
	}


	// ------------------------------------------------------------------------------------------------------------------------
	// Download addresses
	// ------------------------------------------------------------------------------------------------------------------------
	struct ParsedUrl
	{
		std::string mScheme;
		std::string mHost;		// lower case
		std::string mPort;		// without colon, may be empty
		std::string mPath;		// starts with "/" (at least "/")
	};

	inline bool parseUrl(const std::string& url, ParsedUrl& out)
	{
		out = ParsedUrl();
		if (url.empty() || url.size() > 1024)
			return false;
		for (unsigned char c : url)
		{
			if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '@' || c == '#')
				return false;		// no spaces, no user info (a@b), no backslashes
		}
		const size_t schemeEnd = url.find("://");
		if (schemeEnd == std::string::npos)
			return false;
		out.mScheme = toLowerAscii(url.substr(0, schemeEnd));
		const size_t hostStart = schemeEnd + 3;
		size_t pathStart = url.find('/', hostStart);
		if (pathStart == std::string::npos)
			pathStart = url.size();
		std::string authority = url.substr(hostStart, pathStart - hostStart);
		out.mPath = (pathStart < url.size()) ? url.substr(pathStart) : std::string("/");
		const size_t colon = authority.find(':');
		if (colon != std::string::npos)
		{
			out.mPort = authority.substr(colon + 1);
			authority = authority.substr(0, colon);
			if (out.mPort.empty() || out.mPort.size() > 5 || out.mPort.find_first_not_of("0123456789") != std::string::npos)
				return false;
		}
		out.mHost = toLowerAscii(authority);
		return !out.mHost.empty();
	}

	// "owner/name" as it appears in a GitHub address
	inline bool isValidRepoName(const std::string& repo)
	{
		const size_t slash = repo.find('/');
		if (slash == std::string::npos || slash == 0 || slash + 1 >= repo.size() || repo.find('/', slash + 1) != std::string::npos || repo.size() > 100)
			return false;
		for (char c : repo)
		{
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/'))
				return false;
		}
		if (repo.find("..") != std::string::npos)
			return false;
		return true;
	}

	// Where the updater talks to. Normally the public GitHub; a developer test switch can point it at a loopback server.
	struct Endpoints
	{
		std::string mApiBase = "https://api.github.com";	// no trailing slash
		bool mTestMode = false;								// only set by the developer test switch: allows http://127.0.0.1
	};

	// The base address must be https://api.github.com, or (test mode only) http://127.0.0.1:port
	inline bool isAcceptedApiBase(const Endpoints& endpoints)
	{
		ParsedUrl url;
		if (!parseUrl(endpoints.mApiBase, url) || url.mPath != "/")
			return false;
		if (url.mScheme == "https" && url.mHost == "api.github.com" && url.mPort.empty())
			return true;
		if (endpoints.mTestMode && url.mScheme == "http" && url.mHost == "127.0.0.1" && !url.mPort.empty())
			return true;
		return false;
	}

	// An asset of the release may only be fetched from the repository's own release downloads on github.com (GitHub redirects to its own CDN;
	// that redirect is followed by the download code, https only), or from the loopback test server.
	inline bool isAcceptedAssetUrl(const std::string& urlText, const std::string& repo, const Endpoints& endpoints)
	{
		ParsedUrl url;
		if (!parseUrl(urlText, url))
			return false;
		if (url.mPath.find("..") != std::string::npos)
			return false;
		if (url.mScheme == "https" && url.mHost == "github.com" && url.mPort.empty())
		{
			const std::string prefix = "/" + repo + "/releases/download/";
			return url.mPath.size() > prefix.size() && toLowerAscii(url.mPath.substr(0, prefix.size())) == toLowerAscii(prefix);
		}
		if (endpoints.mTestMode && url.mScheme == "http" && url.mHost == "127.0.0.1" && !url.mPort.empty())
		{
			ParsedUrl base;
			return parseUrl(endpoints.mApiBase, base) && base.mHost == url.mHost && base.mPort == url.mPort;
		}
		return false;
	}


	// ------------------------------------------------------------------------------------------------------------------------
	// Release information (GitHub REST API, GET /repos/{owner}/{repo}/releases/latest) and the manifest asset
	// ------------------------------------------------------------------------------------------------------------------------
	static const char* const PACKAGE_ASSET_NAME = "Sonic3Airnet.zip";
	static const char* const MANIFEST_ASSET_NAME = "Sonic3Airnet.manifest.json";
	static const uint64_t MAX_PACKAGE_SIZE = 700ull * 1024 * 1024;
	static const uint64_t MAX_MANIFEST_SIZE = 64 * 1024;

	struct ReleaseInfo
	{
		std::string mTag;
		Version mVersion;
		std::string mNotes;				// plain ASCII, shortened
		std::string mPackageUrl;
		uint64_t mPackageSize = 0;
		std::string mPackageDigest;		// SHA-256 from GitHub's "digest" field of the asset, lower case hex, may be empty
		bool mHasManifest = false;
		std::string mManifestUrl;
	};

	struct Manifest
	{
		std::string mVersion;
		std::string mAsset;
		uint64_t mSize = 0;
		std::string mSha256;
	};

	// Release notes are shown in the game's small font: plain ASCII, no markup characters that are noise there, at most a few hundred characters
	inline std::string sanitizeNotes(const std::string& text, size_t maxLength = 700)
	{
		std::string result;
		for (unsigned char c : text)
		{
			if (result.size() >= maxLength)
			{
				result += "...";
				break;
			}
			if (c == '\r')
				continue;
			if (c == '\n' || (c >= 0x20 && c < 0x7f))
			{
				if (c == '`' || c == '#' || c == '*')
					continue;
				result += (char)c;
			}
			else if (c >= 0x80)
			{
				result += '?';
			}
		}
		// No runs of blank lines
		std::string compact;
		int newlines = 0;
		for (char c : result)
		{
			newlines = (c == '\n') ? newlines + 1 : 0;
			if (newlines <= 2)
				compact += c;
		}
		return compact;
	}

	inline bool readUInt64(const Json::Value& value, uint64_t& out)
	{
		if (value.isUInt64() || value.isUInt())
		{
			out = value.asUInt64();
			return true;
		}
		if (value.isInt() && value.asInt64() >= 0)
		{
			out = (uint64_t)value.asInt64();
			return true;
		}
		return false;
	}

	inline bool parseRelease(const Json::Value& root, const std::string& repo, const Endpoints& endpoints, ReleaseInfo& out, std::string& outError)
	{
		out = ReleaseInfo();
		if (!root.isObject())
		{
			outError = "The release information could not be read.";
			return false;
		}
		const Json::Value& tag = root["tag_name"];
		if (!tag.isString() || !parseVersion(tag.asString(), out.mVersion))
		{
			outError = "The latest release has no usable version number (expected a tag like v1.2.3).";
			return false;
		}
		out.mTag = tag.asString();
		if ((root["draft"].isBool() && root["draft"].asBool()) || (root["prerelease"].isBool() && root["prerelease"].asBool()))
		{
			outError = "The latest release is a draft or pre-release and is not offered.";
			return false;
		}
		if (root["body"].isString())
			out.mNotes = sanitizeNotes(root["body"].asString());

		const Json::Value& assets = root["assets"];
		bool foundPackage = false;
		if (assets.isArray())
		{
			for (const Json::Value& asset : assets)
			{
				if (!asset.isObject() || !asset["name"].isString() || !asset["browser_download_url"].isString())
					continue;
				const std::string name = asset["name"].asString();
				const std::string url = asset["browser_download_url"].asString();
				if (name == PACKAGE_ASSET_NAME)
				{
					if (!isAcceptedAssetUrl(url, repo, endpoints))
					{
						outError = "The release points to a download address that is not accepted.";
						return false;
					}
					out.mPackageUrl = url;
					readUInt64(asset["size"], out.mPackageSize);
					if (asset["digest"].isString())
					{
						const std::string digest = toLowerAscii(asset["digest"].asString());
						if (digest.compare(0, 7, "sha256:") == 0 && isHexSha256(digest.substr(7)))
							out.mPackageDigest = digest.substr(7);
					}
					foundPackage = true;
				}
				else if (name == MANIFEST_ASSET_NAME)
				{
					if (!isAcceptedAssetUrl(url, repo, endpoints))
					{
						outError = "The release points to a download address that is not accepted.";
						return false;
					}
					out.mManifestUrl = url;
					out.mHasManifest = true;
				}
			}
		}
		if (!foundPackage)
		{
			outError = std::string("The latest release has no ") + PACKAGE_ASSET_NAME + " file.";
			return false;
		}
		if (out.mPackageSize > MAX_PACKAGE_SIZE)
		{
			outError = "The update package is larger than allowed.";
			return false;
		}
		return true;
	}

	inline bool parseManifest(const Json::Value& root, Manifest& out, std::string& outError)
	{
		out = Manifest();
		if (!root.isObject() || !root["version"].isString() || !root["asset"].isString() || !root["sha256"].isString() || !readUInt64(root["size"], out.mSize))
		{
			outError = "The manifest of the release is incomplete.";
			return false;
		}
		out.mVersion = root["version"].asString();
		out.mAsset = root["asset"].asString();
		out.mSha256 = toLowerAscii(root["sha256"].asString());
		if (!isHexSha256(out.mSha256))
		{
			outError = "The manifest of the release has no valid SHA-256.";
			return false;
		}
		return true;
	}

	struct ExpectedPackage
	{
		std::string mSha256;
		uint64_t mSize = 0;		// 0 = unknown
	};

	// Decides which hash and size the downloaded package must have. Never returns a result without a hash ("never run anything unverified").
	// If the manifest and GitHub's digest are both there they must agree, and the manifest must belong to this release and this file.
	inline bool resolveExpectedPackage(const ReleaseInfo& release, const Manifest* manifest, ExpectedPackage& out, std::string& outError)
	{
		out = ExpectedPackage();
		if (nullptr != manifest)
		{
			Version manifestVersion;
			if (!parseVersion(manifest->mVersion, manifestVersion) || compareVersions(manifestVersion, release.mVersion) != 0)
			{
				outError = "The manifest of the release belongs to a different version.";
				return false;
			}
			if (manifest->mAsset != PACKAGE_ASSET_NAME)
			{
				outError = "The manifest of the release names a different file.";
				return false;
			}
			if (release.mPackageSize != 0 && manifest->mSize != release.mPackageSize)
			{
				outError = "The manifest and the release disagree about the size of the update.";
				return false;
			}
			if (!release.mPackageDigest.empty() && release.mPackageDigest != manifest->mSha256)
			{
				outError = "The manifest and the release disagree about the SHA-256 of the update.";
				return false;
			}
			out.mSha256 = manifest->mSha256;
			out.mSize = manifest->mSize;
		}
		else if (!release.mPackageDigest.empty())
		{
			out.mSha256 = release.mPackageDigest;
			out.mSize = release.mPackageSize;
		}
		else
		{
			outError = "The release has no SHA-256 (no manifest and no digest), so the update cannot be verified and is not installed.";
			return false;
		}
		if (out.mSize > MAX_PACKAGE_SIZE)
		{
			outError = "The update package is larger than allowed.";
			return false;
		}
		return true;
	}

	// The update package must lie in a single top folder "Sonic3Airnet/" or directly at the root. Returns the path without that folder, or empty if the entry is outside it.
	inline std::string stripPackageFolder(const std::string& normalizedPath, bool hasTopFolder)
	{
		if (!hasTopFolder)
			return normalizedPath;
		static const std::string PREFIX = "Sonic3Airnet/";
		if (normalizedPath.size() > PREFIX.size() && normalizedPath.compare(0, PREFIX.size(), PREFIX) == 0)
			return normalizedPath.substr(PREFIX.size());
		return std::string();
	}


	// Text shown when a download or the API fails, from the HTTP status
	inline std::string describeHttpStatus(long status, bool rateLimited)
	{
		if (status == 404)
			return "No release was found. The repository has no releases yet, or it is private (private releases cannot be downloaded without a login, which this game never uses).";
		if (status == 403 || status == 429)
			return rateLimited ? "GitHub says too many requests were made from this network. Try again in a while." : "GitHub refused the request (HTTP " + std::to_string(status) + ").";
		if (status >= 500)
			return "GitHub is having problems (HTTP " + std::to_string(status) + "). Try again later.";
		return "The server answered with an unexpected status (HTTP " + std::to_string(status) + ").";
	}
}
