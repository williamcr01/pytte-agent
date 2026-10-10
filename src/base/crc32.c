#include "pytte_base.h"

/* CRC-32 (IEEE 802.3, reflected), four bits at a time: a 64-byte table
 * instead of 1 KB, which is plenty for commit lines. */
static const uint32_t nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
    0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

uint32_t pt_crc32(uint32_t crc, const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    crc = ~crc;
    while (n--) {
        crc ^= *s++;
        crc = (crc >> 4) ^ nibble[crc & 15];
        crc = (crc >> 4) ^ nibble[crc & 15];
    }
    return ~crc;
}
