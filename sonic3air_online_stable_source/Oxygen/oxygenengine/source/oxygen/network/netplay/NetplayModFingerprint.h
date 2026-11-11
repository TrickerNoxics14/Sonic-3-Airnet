/*
* Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
* Copyright (C) 2017-2026 by Eukaryot
*
* Published under the GNU GPLv3 open source software license, see license.txt
* or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxbase.h>

#include <algorithm>
#include <string>
#include <vector>

namespace NetplayModFingerprint
{
	struct File
	{
		std::string mRelativePath;
		std::vector<uint8> mContent;
		bool mReadable = true;
	};

	inline uint64 hashFiles(std::vector<File> files)
	{
		std::sort(files.begin(), files.end(), [](const File& a, const File& b)
		{
			return a.mRelativePath < b.mRelativePath;
		});

		uint64 hash = 14695981039346656037ull;
		auto addBytes = [&hash](const void* data, size_t size)
		{
			const uint8* bytes = static_cast<const uint8*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		};

		for (const File& file : files)
		{
			addBytes(file.mRelativePath.data(), file.mRelativePath.size());
			const uint8 separator = 0;
			addBytes(&separator, sizeof(separator));

			const uint8 readable = file.mReadable ? 1 : 0;
			addBytes(&readable, sizeof(readable));
			if (!file.mReadable)
				continue;

			const uint64 size = file.mContent.size();
			for (int shift = 56; shift >= 0; shift -= 8)
			{
				const uint8 sizeByte = (uint8)(size >> shift);
				addBytes(&sizeByte, sizeof(sizeByte));
			}
			addBytes(file.mContent.data(), file.mContent.size());
		}
		return hash;
	}
}
