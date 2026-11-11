/*
*	rmx Library
*	Copyright (C) 2008-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once


namespace ZlibDeflate
{
	FUNCTION_EXPORT bool decode(std::vector<uint8>& output, const void* inputData, size_t inputSize, size_t maxOutputSize = 0);	// A "maxOutputSize" other than 0 makes decoding fail instead of expanding beyond that size (plus one internal chunk)
	FUNCTION_EXPORT bool encode(std::vector<uint8>& output, const void* inputData, size_t inputSize, int compressionLevel = 5);
}
