// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Resistance 3 -- the host's own reads and writes of guest memory (big-endian words at vm_base + ea).
 *
 * Plain stores, outside the runtime's vm_write32 (no store watch, no SPU coherence, no reservation break): for the
 * host's hooks that touch game state the PPU reads, and for unit tests that link no runtime.
 */
#ifndef R3_GUEST_H
#define R3_GUEST_H

#include <cstdint>

extern "C" uint8_t* vm_base;

namespace r3 {

inline uint32_t load_be32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

inline void store_be32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

}  // namespace r3

#endif /* R3_GUEST_H */
