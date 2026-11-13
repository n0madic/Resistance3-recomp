// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test of host/r3_fx.cpp: the R3_FX_OFF list and the guards the lifted code reads.
 *
 *   cmake --build build/host --target test_r3_fx && build/host/test_r3_fx
 */
#include <cstdio>
#include <string>

#include "../r3_fx.h"

namespace {

int g_failures, g_checks;

void check(bool ok, const char* what)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL: %s\n", what);
}

bool is(const r3::FxOff& o, bool grain, bool motion_blur, bool dof)
{
    return o.grain == grain && o.motion_blur == motion_blur && o.dof == dof;
}

}  // namespace

int main()
{
    std::string unknown;
    check(is(r3::parse_fx_off(nullptr), false, false, false), "unset: everything on");
    check(is(r3::parse_fx_off(""), false, false, false), "empty: everything on");
    check(is(r3::parse_fx_off("grain"), true, false, false), "grain");
    check(is(r3::parse_fx_off("motion-blur"), false, true, false), "motion-blur");
    check(is(r3::parse_fx_off("dof"), false, false, true), "dof");
    check(is(r3::parse_fx_off("grain, dof,"), true, false, true), "list with spaces and a trailing comma");
    check(is(r3::parse_fx_off("all"), true, true, true), "all");
    check(is(r3::parse_fx_off("blur,dof,Grain", &unknown), false, false, true) && unknown == "blur,Grain",
          "unknown names are reported and ignored");

    r3::apply_fx_off(r3::parse_fx_off("grain,motion-blur"));
    check(r3_fx_no_grain == 1 && r3_fx_no_motion_blur == 1 && r3_fx_no_dof == 0, "guards follow the list");
    r3::apply_fx_off({});
    check(r3_fx_no_grain == 0 && r3_fx_no_motion_blur == 0 && r3_fx_no_dof == 0, "guards cleared");

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
