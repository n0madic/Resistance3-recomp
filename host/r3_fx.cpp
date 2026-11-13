// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Resistance 3 -- post effects the player may turn off (see r3_fx.h).
 */
#include "r3_fx.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

extern "C" {
int r3_fx_no_grain = 0;
int r3_fx_no_motion_blur = 0;
int r3_fx_no_dof = 0;
}

namespace r3 {

FxOff parse_fx_off(const char* list, std::string* unknown)
{
    FxOff off;
    if (!list) return off;
    std::string_view rest(list);
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        std::string_view name = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name.empty()) continue;
        if (name == "grain") off.grain = true;
        else if (name == "motion-blur") off.motion_blur = true;
        else if (name == "dof") off.dof = true;
        else if (name == "all") off = {true, true, true};
        else if (unknown) {
            if (!unknown->empty()) *unknown += ',';
            unknown->append(name);
        }
    }
    return off;
}

void apply_fx_off(const FxOff& off)
{
    r3_fx_no_grain = off.grain;
    r3_fx_no_motion_blur = off.motion_blur;
    r3_fx_no_dof = off.dof;
}

}  // namespace r3

extern "C" void r3_fx_register(void)
{
    const char* list = std::getenv("R3_FX_OFF");
    if (!list || !*list) return;
    std::string unknown;
    const r3::FxOff off = r3::parse_fx_off(list, &unknown);
    if (!unknown.empty())
        std::printf("[r3] R3_FX_OFF: unknown effect(s) %s (known: grain, motion-blur, dof, all)\n", unknown.c_str());
    r3::apply_fx_off(off);
    std::printf("[r3] post effects off:%s%s%s%s\n", off.grain ? " film grain" : "", off.motion_blur ? " motion blur" : "",
                off.dof ? " depth of field" : "", off.grain || off.motion_blur || off.dof ? "" : " none");
}
