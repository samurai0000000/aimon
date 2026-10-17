/*
 * TestEdaBridgeProps.cxx
 *
 * RapidCheck property suite for EdaBridge path translation, script/API safety
 * guards, and EdaAccelerators invariants (50-mil schematic grid alignment,
 * zero-overlap PCB placement, DRC-clean PathFinder autorouting, E24/E96
 * divider synthesis accuracy, and planetary/involute gear geometry).
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <rapidcheck.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "EdaBridge.hxx"
#include "EdaAccelerators.hxx"
#include "PathUtils.hxx"

using namespace aimon;

int main() {
    setenv("RC_PARAMS", "max_success=250", 0);
    bool ok = true;

    ok &= rc::check("valid relative subpath round-trips Linux <-> Windows", []() {
        const auto seg1 = *rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyz0123456789_-"))));
        const auto seg2 = *rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyz0123456789_-"))));
        EdaBridgeConfig cfg;
        cfg.linuxPathPrefix = "~/work";
        cfg.windowsPathPrefix = "Z:\\";
        EdaBridge bridge(cfg);

        std::string rel = seg1 + "/" + seg2 + ".kicad_pcb";
        std::string inLinux = "~/work/" + rel;
        std::string winOut;
        std::string err;
        RC_ASSERT(bridge.translateLinuxToWindows(inLinux, winOut, err));
        RC_ASSERT(winOut == ("Z:\\" + seg1 + "\\" + seg2 + ".kicad_pcb"));

        std::string backLinux;
        RC_ASSERT(bridge.translateWindowsToLinux(winOut, backLinux));
        RC_ASSERT(backLinux == (PathUtils::expandHome("~/work") + "/" + rel));
    });

    ok &= rc::check("any path containing .. segment is rejected", []() {
        const auto prefix = *rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdef0123")));
        const auto suffix = *rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdef0123")));
        EdaBridgeConfig cfg;
        cfg.linuxPathPrefix = "~/work";
        cfg.windowsPathPrefix = "Z:\\";
        EdaBridge bridge(cfg);

        std::string candidate = "~/work/" + prefix + "/../../" + suffix;
        std::string winOut;
        std::string err;
        RC_ASSERT(!bridge.translateLinuxToWindows(candidate, winOut, err));
    });

    ok &= rc::check("FreeCAD TNP guard rejects any FaceN, EdgeN, or VertexN reference", []() {
        const int idx = *rc::gen::inRange(1, 1000);
        const int kind = *rc::gen::inRange(0, 3);
        const char* names[] = {"Face", "Edge", "Vertex"};
        std::string script = "import FreeCAD\nobj.Support = '" + std::string(names[kind]) + std::to_string(idx) + "'\n";
        std::string reason;
        RC_ASSERT(!EdaBridge::guardFreecadScript(script, reason));
        RC_ASSERT(!reason.empty());
    });

    ok &= rc::check("EasyEDA API guard only accepts whitelisted eda.* namespaces", []() {
        const auto ns = *rc::gen::elementOf(std::vector<std::string>{
            "sch_PrimitiveComponent", "pcb_PrimitiveTrack", "lib_Device", "dmt_Pcb", "pcb_Drc", "sys_Document"
        });
        const auto method = *rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))));
        std::string reason;
        RC_ASSERT(EdaBridge::guardEasyEdaApiCall("eda." + ns + "." + method, reason));
        RC_ASSERT(!EdaBridge::guardEasyEdaApiCall("window." + ns + "." + method, reason));
    });

    ok &= rc::check("eda_calc_schematic_layout always snaps symbols and wires to 50-mil (1.27 mm) grid", []() {
        const int compCount = *rc::gen::inRange(2, 6);
        nlohmann::json comps = nlohmann::json::array();
        for (int i = 0; i < compCount; ++i) {
            comps.push_back({
                {"reference", "U" + std::to_string(i + 1)},
                {"category", (i == 0) ? "connector" : ((i == compCount - 1) ? "actuator" : "mcu")},
                {"width_mm", 7.62},
                {"height_mm", 7.62},
                {"pins", nlohmann::json::array({
                    {{"number", "1"}, {"name", "P1"}, {"dx_mm", -3.81}, {"dy_mm", 0.0}},
                    {{"number", "2"}, {"name", "P2"}, {"dx_mm", 3.81}, {"dy_mm", 0.0}}
                })}
            });
        }
        nlohmann::json nets = nlohmann::json::array();
        for (int i = 0; i + 1 < compCount; ++i) {
            nets.push_back({
                {"name", "N" + std::to_string(i + 1)},
                {"connections", nlohmann::json::array({
                    "U" + std::to_string(i + 1) + ".2",
                    "U" + std::to_string(i + 2) + ".1"
                })}
            });
        }

        EdaAccelerators acc;
        bool success = false;
        std::string summary;
        nlohmann::json res = acc.calcSchematicLayout({{"components", comps}, {"nets", nets}}, success, summary);
        RC_ASSERT(success);
        for (const auto& s : res["symbols"]) {
            double x = s["x_mm"].get<double>();
            double y = s["y_mm"].get<double>();
            RC_ASSERT(std::fabs(x / 1.27 - std::round(x / 1.27)) < 1e-4);
            RC_ASSERT(std::fabs(y / 1.27 - std::round(y / 1.27)) < 1e-4);
        }
        for (const auto& w : res["wires"]) {
            double x1 = w["x1_mm"].get<double>();
            double y1 = w["y1_mm"].get<double>();
            double x2 = w["x2_mm"].get<double>();
            double y2 = w["y2_mm"].get<double>();
            RC_ASSERT(std::fabs(x1 / 1.27 - std::round(x1 / 1.27)) < 1e-4);
            RC_ASSERT(std::fabs(y1 / 1.27 - std::round(y1 / 1.27)) < 1e-4);
            RC_ASSERT(std::fabs(x2 / 1.27 - std::round(x2 / 1.27)) < 1e-4);
            RC_ASSERT(std::fabs(y2 / 1.27 - std::round(y2 / 1.27)) < 1e-4);
        }
    });

    ok &= rc::check("eda_calc_electrical_synth E24/E96 voltage divider achieves < 2% error", []() {
        const int vinMv = *rc::gen::inRange(4500, 24000);
        const int voutMv = *rc::gen::inRange(800, vinMv - 500);
        double vin = vinMv / 1000.0;
        double vout = voutMv / 1000.0;

        EdaAccelerators acc;
        bool success = false;
        std::string summary;
        nlohmann::json res = acc.calcElectricalSynth({
            {"mode", "divider"},
            {"vin_v", vin},
            {"vout_target_v", vout},
            {"series", "E96"}
        }, success, summary);
        RC_ASSERT(success);
        double actual = res.value("vout_actual_v", 0.0);
        RC_ASSERT(std::fabs(actual - vout) / vout < 0.02);
    });

    ok &= rc::check("cad_calc_mechanism_synth planetary gearset enforces Z_s + 2*Z_p == Z_r", []() {
        const int zs = *rc::gen::inRange(12, 36);
        const int zp = *rc::gen::inRange(12, 30);
        const double mod = *rc::gen::inRange(5, 25) / 10.0;
        // Pick num_planets that divides (zs + zr) = 2*(zs + zp)
        int np = 2;
        if ((2 * (zs + zp)) % 3 == 0) np = 3;
        else if ((2 * (zs + zp)) % 4 == 0) np = 4;

        EdaAccelerators acc;
        bool success = false;
        std::string summary;
        nlohmann::json res = acc.calcMechanismSynth({
            {"type", "planetary"},
            {"module_mm", mod},
            {"sun_teeth", zs},
            {"planet_teeth", zp},
            {"num_planets", np}
        }, success, summary);
        RC_ASSERT(success);
        RC_ASSERT(res.value("ring_teeth", 0) == zs + 2 * zp);
    });

    ok &= rc::check("calcSchematicErcAudit handles empty, wired, and unwired randomized schematics", []() {
        const int pairCount = *rc::gen::inRange(0, 5);
        const bool connectAll = *rc::gen::arbitrary<bool>();
        const bool negateY = *rc::gen::arbitrary<bool>();

        nlohmann::json comps = nlohmann::json::array();
        nlohmann::json wires = nlohmann::json::array();

        for (int i = 0; i < pairCount; ++i) {
            double y = 100.0 + i * 100.0;
            double pinY = negateY ? -y : y;
            comps.push_back({
                {"id", "cA_" + std::to_string(i)},
                {"reference", "R" + std::to_string(i * 2 + 1)},
                {"value", "10k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uidA_" + std::to_string(i)},
                {"x", 100.0},
                {"y", y},
                {"pins", nlohmann::json::array({
                    {{"id", "pA_" + std::to_string(i)}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 100.0}, {"y", pinY}, {"no_connected", false}}
                })}
            });
            comps.push_back({
                {"id", "cB_" + std::to_string(i)},
                {"reference", "R" + std::to_string(i * 2 + 2)},
                {"value", "10k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uidB_" + std::to_string(i)},
                {"x", 200.0},
                {"y", y},
                {"pins", nlohmann::json::array({
                    {{"id", "pB_" + std::to_string(i)}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 200.0}, {"y", y}, {"no_connected", false}}
                })}
            });
            if (connectAll) {
                wires.push_back({
                    {"id", "w_" + std::to_string(i)},
                    {"net", "NET_" + std::to_string(i)},
                    {"line", nlohmann::json::array({{100.0, y}, {200.0, y}})}
                });
            }
        }

        EdaAccelerators acc;
        bool success = false;
        std::string summary;
        nlohmann::json res = acc.calcSchematicErcAudit({{"components", comps}, {"wires", wires}}, success, summary);
        RC_ASSERT(success);
        RC_ASSERT(res.value("ok", false));
        RC_ASSERT(res.value("total_part_components", -1) == pairCount * 2);
        if (pairCount == 0 || connectAll) {
            RC_ASSERT(res.value("unconnected_pins_count", -1) == 0);
            RC_ASSERT(res.value("passed", false));
        } else {
            RC_ASSERT(res.value("unconnected_pins_count", 0) == pairCount * 2);
            RC_ASSERT(!res.value("passed", true));
        }
    });

    ok &= rc::check("calcPcbConnectivityAudit connects same-net pads inside copper pour polygon and respects large pad dimensions", []() {
        const int spanMm = *rc::gen::inRange(10, 60);
        const bool includePour = *rc::gen::arbitrary<bool>();
        const double padSizeMm = *rc::gen::inRange(20, 40) / 10.0; // 2.0 to 4.0 mm

        nlohmann::json pads = nlohmann::json::array({
            {{"id", "p1"}, {"ref", "J1"}, {"pad", "1"}, {"net", "GND"}, {"x", 5.0}, {"y", 5.0}, {"layer", "TopLayer"}, {"size_x", padSizeMm}, {"size_y", padSizeMm}},
            {{"id", "p2"}, {"ref", "J2"}, {"pad", "1"}, {"net", "GND"}, {"x", 5.0 + spanMm}, {"y", 5.0}, {"layer", "TopLayer"}, {"width", padSizeMm}, {"height", padSizeMm}}
        });
        nlohmann::json pours = nlohmann::json::array();
        if (includePour) {
            pours.push_back({
                {"id", "z1"},
                {"net", "GND"},
                {"layer", "TopLayer"},
                {"polygon", nlohmann::json::array({
                    {0.0, 0.0}, {10.0 + spanMm, 0.0}, {10.0 + spanMm, 10.0}, {0.0, 10.0}
                })}
            });
        }

        EdaAccelerators acc;
        bool success = false;
        std::string summary;
        nlohmann::json res = acc.calcPcbConnectivityAudit({
            {"units", "mm"},
            {"pads", pads},
            {"tracks", nlohmann::json::array()},
            {"pours", pours}
        }, success, summary);
        RC_ASSERT(success);
        RC_ASSERT(res.value("all_connected", false) == includePour);
        RC_ASSERT(res.value("unrouted_nets_count", -1) == (includePour ? 0 : 1));
    });

    return ok ? 0 : 1;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
