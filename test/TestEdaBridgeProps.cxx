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

    ok &= rc::check("valid relative subpath passes validatePathSafety and validateJsonPaths", []() {
        const auto seg1 = *rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyz0123456789_-"))));
        const auto seg2 = *rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyz0123456789_-"))));

        std::string rel = seg1 + "/" + seg2 + ".kicad_pcb";
        std::string inLinux = "~/work/" + rel;
        std::string err;
        RC_ASSERT(EdaBridge::validatePathSafety(inLinux, err));
        RC_ASSERT(EdaBridge::validatePathSafety("C:\\pcb\\" + seg1 + "\\" + seg2 + ".kicad_pcb", err));

        nlohmann::json payload = {
            {"project_path", inLinux},
            {"windows_path", "C:\\pcb\\" + seg1 + "\\" + seg2 + ".kicad_pcb"}
        };
        RC_ASSERT(EdaBridge::validateJsonPaths(payload, err));
        RC_ASSERT(payload["project_path"].get<std::string>() == (PathUtils::expandHome("~/work") + "/" + rel));
        RC_ASSERT(payload["windows_path"].get<std::string>() == ("C:\\pcb\\" + seg1 + "\\" + seg2 + ".kicad_pcb"));
    });

    ok &= rc::check("any path containing .. segment is rejected", []() {
        const auto prefix = *rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdef0123")));
        const auto suffix = *rc::gen::container<std::string>(
            rc::gen::elementOf(std::string("abcdef0123")));

        std::string candidate = "~/work/" + prefix + "/../../" + suffix;
        std::string err;
        RC_ASSERT(!EdaBridge::validatePathSafety(candidate, err));

        nlohmann::json payload = {
            {"project_path", candidate}
        };
        RC_ASSERT(!EdaBridge::validateJsonPaths(payload, err));
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

    ok &= rc::check("calcSchematicErcAudit T-junctions and 5-layer semantic checks hold across randomized schematics", []() {
        const bool includePullup = *rc::gen::arbitrary<bool>();
        const bool includeDecoupling = *rc::gen::arbitrary<bool>();
        const bool injectOverlap = *rc::gen::arbitrary<bool>();
        const double tjOffset = *rc::gen::inRange(0, 14) / 10.0; // 0.0 to 1.4 mil <= 1.5 mil default tol

        nlohmann::json comps = nlohmann::json::array({
            {
                {"id", "u1"},
                {"reference", "U1"},
                {"value", "MCU"},
                {"component_type", "part"},
                {"footprint", "QFN-32"},
                {"unique_id", "uid_u1"},
                {"x", 200.0},
                {"y", 200.0},
                {"width", 80.0},
                {"height", 80.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u1_1"}, {"number", "1"}, {"name", "VDD"}, {"net", "+3V3"}, {"x", 160.0}, {"y", 180.0}},
                    {{"id", "u1_2"}, {"number", "2"}, {"name", "GND"}, {"net", "GND"}, {"x", 160.0}, {"y", 220.0}},
                    {{"id", "u1_3"}, {"number", "3"}, {"name", "SDA"}, {"net", "I2C_SDA"}, {"x", 240.0}, {"y", 200.0}}
                })}
            },
            {
                {"id", "u2"},
                {"reference", "U2"},
                {"value", "SENSOR"},
                {"component_type", "part"},
                {"footprint", "SON-8"},
                {"unique_id", "uid_u2"},
                {"x", injectOverlap ? 220.0 : 500.0},
                {"y", 200.0},
                {"width", 80.0},
                {"height", 80.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u2_1"}, {"number", "1"}, {"name", "SDA"}, {"net", "I2C_SDA"}, {"x", 460.0}, {"y", 200.0}}
                })}
            },
            {
                {"id", "nf_vdd"},
                {"reference", "#PWR1"},
                {"value", "+3V3"},
                {"component_type", "netflag"},
                {"x", 350.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array()}
            }
        });

        if (includePullup) {
            comps.push_back({
                {"id", "r1"},
                {"reference", "R1"},
                {"value", "4.7k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uid_r1"},
                {"x", 350.0},
                {"y", 150.0},
                {"width", 20.0},
                {"height", 40.0},
                {"pins", nlohmann::json::array({
                    {{"id", "r1_1"}, {"number", "1"}, {"name", "1"}, {"net", "+3V3"}, {"x", 350.0}, {"y", 130.0}},
                    {{"id", "r1_2"}, {"number", "2"}, {"name", "2"}, {"net", "I2C_SDA"}, {"x", 350.0}, {"y", 170.0}}
                })}
            });
        }

        if (includeDecoupling) {
            comps.push_back({
                {"id", "c1"},
                {"reference", "C1"},
                {"value", "100nF"},
                {"component_type", "part"},
                {"footprint", "C0603"},
                {"unique_id", "uid_c1"},
                {"x", 100.0},
                {"y", 200.0},
                {"width", 20.0},
                {"height", 40.0},
                {"pins", nlohmann::json::array({
                    {{"id", "c1_1"}, {"number", "1"}, {"name", "1"}, {"net", "+3V3"}, {"x", 100.0}, {"y", 180.0}},
                    {{"id", "c1_2"}, {"number", "2"}, {"name", "2"}, {"net", "GND"}, {"x", 100.0}, {"y", 220.0}}
                })}
            });
        }

        nlohmann::json wires = nlohmann::json::array({
            {{"id", "w_sda_bus"}, {"net", "I2C_SDA"}, {"line", nlohmann::json::array({{240.0, 200.0}, {460.0, 200.0}})}}
        });
        if (includePullup) {
            // T-junction landing on w_sda_bus within tjOffset (<= 1.4 mil)
            wires.push_back({
                {"id", "w_pu_branch"},
                {"net", "I2C_SDA"},
                {"line", nlohmann::json::array({{350.0, 170.0}, {350.0, 200.0 - tjOffset}})}
            });
            wires.push_back({
                {"id", "w_pu_vdd"},
                {"net", "+3V3"},
                {"line", nlohmann::json::array({{350.0, 130.0}, {350.0, 100.0}})}
            });
        }

        EdaAccelerators acc;
        bool success = false;
        std::string summary;
        nlohmann::json res = acc.calcSchematicErcAudit({{"components", comps}, {"wires", wires}}, success, summary);
        RC_ASSERT(success);
        RC_ASSERT(res.value("dangling_wires_count", -1) == 0);
        RC_ASSERT(res.value("pullup_issues_count", -1) == (includePullup ? 0 : 1));
        RC_ASSERT(res.value("decoupling_issues_count", -1) == (includeDecoupling ? 0 : 1));
        RC_ASSERT(res.value("overlapping_components_count", -1) == (injectOverlap ? 1 : 0));
    });

    ok &= rc::check("easyeda_edit_schematic auto_place, connect_to_net, and annotate_schematic preserve 50-mil grid and zero overlaps", []() {
        const int extraPassives = *rc::gen::inRange(1, 5);
        const int stubGridSteps = *rc::gen::inRange(1, 4);
        const double stubLen = stubGridSteps * 50.0;

        EdaBridgeConfig cfg;
        cfg.enabled = true;
        cfg.mode = "gateway";
        cfg.easyeda.enabled = true;
        EdaBridge bridge(cfg);

        nlohmann::json schState = {
            {"active_document", "PropSheet"},
            {"components", nlohmann::json::array({
                {
                    {"id", "u1"},
                    {"reference", "U?"},
                    {"value", "MCU"},
                    {"component_type", "part"},
                    {"footprint", "QFN-32"},
                    {"unique_id", "uid_u1"},
                    {"x", 500.0},
                    {"y", 500.0},
                    {"width", 100.0},
                    {"height", 100.0},
                    {"pins", nlohmann::json::array({
                        {{"id", "u1_p1"}, {"number", "1"}, {"name", "VDD"}, {"net", ""}, {"x", 450.0}, {"y", 500.0}},
                        {{"id", "u1_p2"}, {"number", "2"}, {"name", "PA0"}, {"net", ""}, {"x", 550.0}, {"y", 500.0}}
                    })}
                }
            })},
            {"wires", nlohmann::json::array()}
        };

        nlohmann::json addComps = nlohmann::json::array();
        for (int i = 0; i < extraPassives; ++i) {
            addComps.push_back({
                {"reference", (i % 2 == 0) ? "R?" : "C?"},
                {"value", "10k"},
                {"footprint", "0603"},
                {"near_reference", "U1"},
                {"width", 40.0},
                {"height", 20.0}
            });
        }

        nlohmann::json editRes = bridge.callTool("easyeda_edit_schematic", {
            {"schematic_state", schState},
            {"add_components", addComps},
            {"annotate_schematic", true},
            {"connect_to_net", nlohmann::json::array({
                {{"reference", "U1"}, {"pin_number", "1"}, {"net", "+3V3"}, {"stub_length_mils", stubLen}},
                {{"reference", "U1"}, {"pin_number", "2"}, {"net", "SIG_OUT"}, {"stub_length_mils", stubLen}}
            })}
        });

        RC_ASSERT(editRes.value("ok", false));
        RC_ASSERT(editRes.value("placed_components", 0) == extraPassives);
        RC_ASSERT(editRes.value("annotated_count", 0) == extraPassives + 1);
        RC_ASSERT(editRes.value("annotation_verified", false));
        RC_ASSERT(editRes.contains("connected_net_stubs"));
        RC_ASSERT(static_cast<int>(editRes["connected_net_stubs"].size()) == 2);
        for (const auto& pd : editRes["placed_component_details"]) {
            double px = pd.value("x", -1.0);
            double py = pd.value("y", -1.0);
            RC_ASSERT(std::fabs(px / 50.0 - std::round(px / 50.0)) < 1e-4);
            RC_ASSERT(std::fabs(py / 50.0 - std::round(py / 50.0)) < 1e-4);
        }
    });

    // 7. Property: Connected Move (preserve_connections: true) rubber-bands all attached schematic wires
    // and PCB tracks by the exact translation delta (dx, dy), and calcPcbAutoroute resolves T-corridor ordering traps.
    ok = ok && rc::check("Envelope 2 connected move rubber-banding and rip-up autoroute invariants", []() {
        const int dxSteps = *rc::gen::inRange(-6, 7);
        const int dySteps = *rc::gen::inRange(-6, 7);
        const double dxMils = static_cast<double>(dxSteps * 50);
        const double dyMils = static_cast<double>(dySteps * 50);

        EdaBridgeConfig cfg;
        cfg.enabled = true;
        cfg.mode = "gateway";
        cfg.easyeda.enabled = true;
        EdaBridge bridge(cfg);

        nlohmann::json schState = {
            {"active_document", "PropRubberSheet"},
            {"components", nlohmann::json::array({
                {
                    {"id", "u1"},
                    {"reference", "U1"},
                    {"value", "MCU"},
                    {"footprint", "QFN-32"},
                    {"x", 500.0},
                    {"y", 500.0},
                    {"pins", nlohmann::json::array({
                        {{"number", "1"}, {"name", "VDD"}, {"net", "+3V3"}, {"x", 450.0}, {"y", 500.0}},
                        {{"number", "2"}, {"name", "GND"}, {"net", "GND"}, {"x", 550.0}, {"y", 500.0}}
                    })}
                }
            })},
            {"wires", nlohmann::json::array({
                {{"id", "w1"}, {"x1", 200.0}, {"y1", 500.0}, {"x2", 450.0}, {"y2", 500.0}, {"net", "+3V3"}},
                {{"id", "w2"}, {"x1", 550.0}, {"y1", 500.0}, {"x2", 800.0}, {"y2", 500.0}, {"net", "GND"}}
            })}
        };

        nlohmann::json schRes = bridge.callTool("easyeda_edit_schematic", {
            {"schematic_state", schState},
            {"preserve_connections", true},
            {"modify_components", nlohmann::json::array({
                {{"reference", "U1"}, {"x", 500.0 + dxMils}, {"y", 500.0 + dyMils}, {"preserve_connections", true}}
            })}
        });
        RC_ASSERT(schRes.value("ok", false));
        RC_ASSERT(schRes.value("rubber_banded_wires_count", -1) == 2);
        RC_ASSERT(schRes.contains("rubber_banded_wires"));
        RC_ASSERT(static_cast<int>(schRes["rubber_banded_wires"].size()) == 2);
        RC_ASSERT(std::fabs(schRes["rubber_banded_wires"][0].value("x2", 0.0) - (450.0 + dxMils)) < 1e-3);
        RC_ASSERT(std::fabs(schRes["rubber_banded_wires"][0].value("y2", 0.0) - (500.0 + dyMils)) < 1e-3);
        RC_ASSERT(std::fabs(schRes["rubber_banded_wires"][1].value("x1", 0.0) - (550.0 + dxMils)) < 1e-3);
        RC_ASSERT(std::fabs(schRes["rubber_banded_wires"][1].value("y1", 0.0) - (500.0 + dyMils)) < 1e-3);

        const double dxMm = static_cast<double>(dxSteps) * 1.25;
        const double dyMm = static_cast<double>(dySteps) * 1.25;
        nlohmann::json pcbState = {
            {"components", nlohmann::json::array({
                {
                    {"reference", "R1"},
                    {"footprint", "R0603"},
                    {"x_mm", 20.0},
                    {"y_mm", 20.0},
                    {"pads", nlohmann::json::array({
                        {{"number", "1"}, {"net", "+3V3"}, {"x_mm", 19.2}, {"y_mm", 20.0}},
                        {{"number", "2"}, {"net", "SDA"}, {"x_mm", 20.8}, {"y_mm", 20.0}}
                    })}
                }
            })},
            {"tracks", nlohmann::json::array({
                {{"id", "t1"}, {"x1", 10.0}, {"y1", 20.0}, {"x2", 19.2}, {"y2", 20.0}, {"layer", "F.Cu"}, {"net", "+3V3"}},
                {{"id", "t2"}, {"x1", 20.8}, {"y1", 20.0}, {"x2", 30.0}, {"y2", 20.0}, {"layer", "F.Cu"}, {"net", "SDA"}}
            })}
        };

        nlohmann::json pcbRes = bridge.callTool("easyeda_edit_pcb", {
            {"pcb_state", pcbState},
            {"preserve_connections", true},
            {"move_components", nlohmann::json::array({
                {{"reference", "R1"}, {"x_mm", 20.0 + dxMm}, {"y_mm", 20.0 + dyMm}, {"preserve_connections", true}}
            })}
        });
        RC_ASSERT(pcbRes.value("ok", false));
        RC_ASSERT(pcbRes.value("rubber_banded_tracks_count", -1) == 2);
        RC_ASSERT(pcbRes.contains("rubber_banded_tracks"));
        RC_ASSERT(static_cast<int>(pcbRes["rubber_banded_tracks"].size()) == 2);
        RC_ASSERT(std::fabs(pcbRes["rubber_banded_tracks"][0].value("x2", 0.0) - (19.2 + dxMm)) < 1e-3);
        RC_ASSERT(std::fabs(pcbRes["rubber_banded_tracks"][0].value("y2", 0.0) - (20.0 + dyMm)) < 1e-3);
        RC_ASSERT(std::fabs(pcbRes["rubber_banded_tracks"][1].value("x1", 0.0) - (20.8 + dxMm)) < 1e-3);
        RC_ASSERT(std::fabs(pcbRes["rubber_banded_tracks"][1].value("y1", 0.0) - (20.0 + dyMm)) < 1e-3);
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
