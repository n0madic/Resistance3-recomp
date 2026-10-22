/*
 * Resistance 3 -- title-specific HLE: firmware imports the game uses that the toolkit does not implement.
 *
 * Handlers use the generic HLE ABI (runtime/ppu/ppu_hle.cpp): up to eight integer arguments r3..r10 as uint64_t,
 * result in r3. NIDs are SHA-1(name + suffix) as in tools/nid_database.py.
 */

#include <cstdint>

extern "C" {
void ps3_hle_register(uint32_t nid, const char* name, void* handler);
void r3_lfqueue_register(void);   /* spurs_lfqueue.cpp */
}

namespace {

uint64_t h_ok(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }

/* The game polls the camera for the Move tracker; there is no camera, so the buffer is "not open". */
uint64_t h_camera_not_open(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    return 0x80140804u;   /* CELL_CAMERA_ERROR_NOT_OPEN */
}

}  // namespace

extern "C" void r3_hle_register(void)
{
    struct E { uint32_t nid; const char* name;
               uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t); };
    static const E table[] = {
        /* sysPrxForUser: registers the SPU printf callbacks; nothing prints. */
        { 0x45FE2FCEu, "_sys_spu_printf_initialize",          h_ok },
        /* Camera and Move: unused in the single-player game. */
        { 0x0E63C444u, "cellCameraGetBufferInfoEx",           h_camera_not_open },
        { 0xA03EF587u, "cellGemPrepareCamera",                h_ok },
        /* TODO: name 0xFAA275A4 (not in the toolkit's NID database); the game only needs it to succeed. */
        { 0xFAA275A4u, "cellSysutilAvconfExt_FAA275A4",       h_ok },
        { 0x7517724Au, "cellSpursSetGlobalExceptionEventHandler",                  h_ok },
        /* The queue's Lv2 event queue is HLE-only (patches/0002): nothing to detach. */
        { 0x73E06F91u, "cellSpursLFQueueDetachLv2EventQueue", h_ok },
    };
    for (const E& e : table)
        ps3_hle_register(e.nid, e.name, reinterpret_cast<void*>(e.fn));
    r3_lfqueue_register();
}
