// SPDX-License-Identifier: GPL-2.0-or-later
//
// Decrypts the retail (non-NPDRM, program type APP) SELF a PS3 disc carries as USRDIR/EBOOT.BIN into a plain ELF.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace r3 {

struct SelfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The AES-256 key and IV (hex) that decrypt the metadata of a SELF of one key revision.
struct AppKey {
    uint16_t revision;
    const char* erk;
    const char* riv;
};

// The table of self_app_keys.inc.
const AppKey* app_keys(size_t* count);

// The plain ELF of an APP SELF, decrypted with the key of the SELF's revision found in `keys`.
// Throws SelfError for anything else (debug or NPDRM SELF, unknown revision, truncated or corrupt data).
std::vector<uint8_t> decrypt_app_self(const std::vector<uint8_t>& blob, const AppKey* keys, size_t key_count);

// `r3_host unself <EBOOT.BIN> -o <EBOOT.elf>`; argv is what follows the subcommand.
int unself_main(int argc, char** argv);
// `r3_host selftest`: the crypto vectors and a round trip of a synthetic SELF.
int selftest_main();

}  // namespace r3
