/*
 * Game-specific hooks: the PRX loading hook main.cpp calls once the lifted function table is registered.
 * Resistance 3 lifts no system PRX; the HLE library and r3_hle.cpp answer those modules.
 */

extern "C" void r3_hle_register(void);   /* r3_hle.cpp */

extern "C" void ps3_load_prx_modules(void)
{
    /* Not a PRX: the title-specific HLE is registered here because this runs before ppu_hle_init(). */
    r3_hle_register();
}
