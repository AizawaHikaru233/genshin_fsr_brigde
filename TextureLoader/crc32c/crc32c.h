/*
  Copyright (c) 2013 - 2014 Mark Adler, Robert Vazan

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
  claim that you wrote the original software. If you use this software
  in a product, an acknowledgment in the product documentation would be
  appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
  misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef CRC32C_H
#define CRC32C_H

#ifndef CRC32C_STATIC
#ifdef CRC32C_EXPORTS
#define CRC32C_API __declspec(dllexport)
#else
#define CRC32C_API __declspec(dllimport)
#endif
#else
#define CRC32C_API
#endif

#include <stdint.h>

/*
    Computes CRC-32C using Castagnoli polynomial of 0x82f63b78.
    This polynomial is better at detecting errors than the more common CRC-32 polynomial.
    CRC-32C is implemented in hardware on newer Intel processors.
    This function will use the hardware if available and fall back to fast software implementation.
*/
extern "C" CRC32C_API uint32_t crc32c_append(
    uint32_t crc,               // initial CRC, typically 0, may be used to accumulate CRC from multiple buffers
    const uint8_t *input,       // data to be put through the CRC algorithm
    size_t length);             // length of the data in the input buffer

extern "C" CRC32C_API void crc32c_unittest();
uint32_t crc32_fast(const void* data, size_t length, uint32_t previousCrc32 = 0);
#endif
