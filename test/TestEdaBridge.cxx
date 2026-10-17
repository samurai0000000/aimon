/*
 * TestEdaBridge.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <rapidcheck.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include "EdaBridge.hxx"
#include "EdaAccelerators.hxx"
#include "PathUtils.hxx"
#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/MemoryLeakWarningPlugin.h>

using namespace aimon;

TEST_GROUP(EdaBridge_PathMapping) {
    EdaBridgeConfig _cfg;

    void setup() override {
        _cfg.enabled = true;
        _cfg.host = "eda.local";
        _cfg.port = 3892;
        _cfg.pluginPort = 3893;
        _cfg.linuxPathPrefix = "~/work";
        _cfg.windowsPathPrefix = "Z:\\";
    }
};

TEST(EdaBridge_PathMapping, NominalRoundTrip) {
    EdaBridge bridge(_cfg);
    std::string winPath;
    std::string err;
    CHECK_TRUE(bridge.translateLinuxToWindows("~/work/pcb/main.kicad_pcb", winPath, err));
    CHECK_EQUAL("Z:\\pcb\\main.kicad_pcb", winPath);

    std::string linuxPath;
    CHECK_TRUE(bridge.translateWindowsToLinux(winPath, linuxPath));
    std::string expectedLinux = PathUtils::expandHome("~/work") + "/pcb/main.kicad_pcb";
    CHECK_EQUAL(expectedLinux, linuxPath);
}

TEST(EdaBridge_PathMapping, ExactPrefixRootAndCaseInsensitiveDrive) {
    EdaBridge bridge(_cfg);
    std::string winPath;
    std::string err;
    CHECK_TRUE(bridge.translateLinuxToWindows("~/work", winPath, err));
    CHECK_EQUAL("Z:\\", winPath);

    std::string linuxPath;
    CHECK_TRUE(bridge.translateWindowsToLinux("z:\\board\\top.kicad_sch", linuxPath));
    CHECK_EQUAL(PathUtils::expandHome("~/work") + "/board/top.kicad_sch", linuxPath);
}

TEST(EdaBridge_PathMapping, RejectsSiblingCollisionTraversalAndNullByte) {
    EdaBridge bridge(_cfg);
    std::string winPath;
    std::string err;

    CHECK_FALSE(bridge.translateLinuxToWindows("~/work-other/file.kicad_pcb", winPath, err));
    CHECK_FALSE(bridge.translateLinuxToWindows("~/work/../../etc/passwd", winPath, err));
    CHECK_FALSE(bridge.translateLinuxToWindows("", winPath, err));

    std::string withNull = "~/work/pcb/a";
    withNull.push_back('\0');
    withNull += "b.kicad_pcb";
    CHECK_FALSE(bridge.translateLinuxToWindows(withNull, winPath, err));

    std::string linuxPath;
    CHECK_FALSE(bridge.translateWindowsToLinux("Z:\\..\\Windows\\System32\\cmd.exe", linuxPath));
    CHECK_FALSE(bridge.translateWindowsToLinux("C:\\Users\\other\\file.txt", linuxPath));

    std::string overLimit(4100, 'a');
    CHECK_FALSE(bridge.translateLinuxToWindows("~/work/" + overLimit, winPath, err));
}

TEST(EdaBridge_PathMapping, RewriteNestedJsonPayloadPaths) {
    EdaBridge bridge(_cfg);
    nlohmann::json payload = {
        {"project_path", "~/work/pcb/controller.kicad_pro"},
        {"nested", {
            {"schematic_path", "~/work/pcb/controller.kicad_sch"},
            {"output_dir", "~/work/pcb/fab"},
            {"count", 4}
        }},
        {"files", nlohmann::json::array({"~/work/pcb/a.step", "~/work/pcb/b.stl"})}
    };
    std::string err;
    CHECK_TRUE(bridge.rewriteJsonPaths(payload, true, err));
    CHECK_EQUAL("Z:\\pcb\\controller.kicad_pro", payload["project_path"].get<std::string>());
    CHECK_EQUAL("Z:\\pcb\\controller.kicad_sch", payload["nested"]["schematic_path"].get<std::string>());
    CHECK_EQUAL("Z:\\pcb\\fab", payload["nested"]["output_dir"].get<std::string>());
    CHECK_EQUAL("Z:\\pcb\\a.step", payload["files"][0].get<std::string>());

    CHECK_TRUE(bridge.rewriteJsonPaths(payload, false, err));
    std::string root = PathUtils::expandHome("~/work");
    CHECK_EQUAL(root + "/pcb/controller.kicad_pro", payload["project_path"].get<std::string>());
    CHECK_EQUAL(root + "/pcb/a.step", payload["files"][0].get<std::string>());
}

TEST_GROUP(EdaBridge_ScriptGuard) {
};

TEST(EdaBridge_ScriptGuard, AcceptsCleanCsgAndRejectsTnpOrEscapeImports) {
    std::string reason;
    std::string cleanScript =
        "import FreeCAD, Part\n"
        "doc = FreeCAD.newDocument('RobotLink')\n"
        "box = Part.makeBox(40.0, 20.0, 8.0)\n"
        "cyl = Part.makeCylinder(3.1, 10.0)\n"
        "shape = box.cut(cyl)\n";
    CHECK_TRUE(EdaBridge::guardFreecadScript(cleanScript, reason));

    CHECK_FALSE(EdaBridge::guardFreecadScript("sketch.Support = [(body, 'Face3')]", reason));
    CHECK_TRUE(reason.find("Face3") != std::string::npos);

    CHECK_FALSE(EdaBridge::guardFreecadScript("fillet.Edges = ['Edge12']", reason));
    CHECK_TRUE(reason.find("Edge12") != std::string::npos);

    CHECK_FALSE(EdaBridge::guardFreecadScript("import subprocess\nsubprocess.Popen(['calc'])", reason));
    CHECK_FALSE(EdaBridge::guardFreecadScript("import os\nos.system('whoami')", reason));
    CHECK_FALSE(EdaBridge::guardFreecadScript("import socket\ns = socket.socket()", reason));
}

TEST(EdaBridge_ScriptGuard, ApiNamespaceWhitelists) {
    std::string reason;
    CHECK_TRUE(EdaBridge::guardFreecadApiCall("Part", "makeCylinder", reason));
    CHECK_TRUE(EdaBridge::guardFreecadApiCall("PartDesign", "Pad", reason));
    CHECK_TRUE(EdaBridge::guardFreecadApiCall("Mesh", "export", reason));
    CHECK_FALSE(EdaBridge::guardFreecadApiCall("os", "system", reason));
    CHECK_FALSE(EdaBridge::guardFreecadApiCall("subprocess", "Popen", reason));

    CHECK_TRUE(EdaBridge::guardEasyEdaApiCall("eda.sch_PrimitiveComponent.create", reason));
    CHECK_TRUE(EdaBridge::guardEasyEdaApiCall("eda.pcb_ManufactureData.getGerberFile", reason));
    CHECK_TRUE(EdaBridge::guardEasyEdaApiCall("eda.lib_Device.search", reason));
    CHECK_FALSE(EdaBridge::guardEasyEdaApiCall("window.eval", reason));
    CHECK_FALSE(EdaBridge::guardEasyEdaApiCall("process.exit", reason));
    CHECK_FALSE(EdaBridge::guardEasyEdaApiCall("eda.sys_FileSystem.deleteAll", reason));
}

TEST_GROUP(EdaAccelerators_Unit) {
    EdaAcceleratorConfig _accCfg;

    void setup() override {
        _accCfg.enabled = true;
        _accCfg.saPlacementIterations = 2000;
        _accCfg.autorouteMaxRipupPasses = 15;
        _accCfg.autorouteGridMm = 0.25;
        _accCfg.preferJlcpcbBasicESeries = true;
        _accCfg.defaultCopperWeightOz = 1.0;
        _accCfg.defaultFilamentMaterial = "PETG";
        _accCfg.defaultGearPressureAngleDeg = 20.0;
    }
};

TEST(EdaAccelerators_Unit, CalcSchematicLayoutSugiyamaAnd50MilRouting) {
    EdaAccelerators acc(_accCfg);
    nlohmann::json args = {
        {"components", nlohmann::json::array({
            {{"reference", "J1"}, {"category", "connector"}, {"width_mm", 7.62}, {"height_mm", 10.16},
             {"pins", nlohmann::json::array({{{"number", "1"}, {"name", "VIN"}, {"dx_mm", 3.81}, {"dy_mm", -2.54}},
                                             {{"number", "2"}, {"name", "GND"}, {"dx_mm", 3.81}, {"dy_mm", 2.54}}})}},
            {{"reference", "U1"}, {"category", "power"}, {"width_mm", 10.16}, {"height_mm", 10.16},
             {"pins", nlohmann::json::array({{{"number", "1"}, {"name", "IN"}, {"dx_mm", -5.08}, {"dy_mm", -2.54}},
                                             {{"number", "2"}, {"name", "OUT"}, {"dx_mm", 5.08}, {"dy_mm", -2.54}},
                                             {{"number", "3"}, {"name", "GND"}, {"dx_mm", 0.0}, {"dy_mm", 5.08}}})}},
            {{"reference", "U2"}, {"category", "mcu"}, {"width_mm", 15.24}, {"height_mm", 15.24},
             {"pins", nlohmann::json::array({{{"number", "1"}, {"name", "VDD"}, {"dx_mm", -7.62}, {"dy_mm", -2.54}},
                                             {{"number", "2"}, {"name", "PA0"}, {"dx_mm", 7.62}, {"dy_mm", 0.0}}})}},
            {{"reference", "M1"}, {"category", "actuator"}, {"width_mm", 10.16}, {"height_mm", 10.16},
             {"pins", nlohmann::json::array({{{"number", "1"}, {"name", "PWM"}, {"dx_mm", -5.08}, {"dy_mm", 0.0}}})}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "VIN"}, {"connections", nlohmann::json::array({"J1.1", "U1.1"})}},
            {{"name", "+3V3"}, {"connections", nlohmann::json::array({"U1.2", "U2.1"})}},
            {{"name", "SERVO_PWM"}, {"connections", nlohmann::json::array({"U2.2", "M1.1"})}}
        })}
    };

    bool ok = false;
    std::string summary;
    nlohmann::json res = acc.calcSchematicLayout(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_EQUAL(4, static_cast<int>(res["symbols"].size()));
    CHECK_TRUE(res["wires"].size() >= 3);

    for (const auto& sym : res["symbols"]) {
        double x = sym["x_mm"].get<double>();
        double y = sym["y_mm"].get<double>();
        double remX = std::fabs(x / 1.27 - std::round(x / 1.27));
        double remY = std::fabs(y / 1.27 - std::round(y / 1.27));
        CHECK_TRUE(remX < 1e-4);
        CHECK_TRUE(remY < 1e-4);
    }

    for (const auto& w : res["wires"]) {
        double x1 = w["x1_mm"].get<double>();
        double y1 = w["y1_mm"].get<double>();
        double x2 = w["x2_mm"].get<double>();
        double y2 = w["y2_mm"].get<double>();
        CHECK_TRUE(std::fabs(x1 / 1.27 - std::round(x1 / 1.27)) < 1e-4);
        CHECK_TRUE(std::fabs(y1 / 1.27 - std::round(y1 / 1.27)) < 1e-4);
        CHECK_TRUE(std::fabs(x2 / 1.27 - std::round(x2 / 1.27)) < 1e-4);
        CHECK_TRUE(std::fabs(y2 / 1.27 - std::round(y2 / 1.27)) < 1e-4);
        bool orthogonal = (std::fabs(x1 - x2) < 1e-4) || (std::fabs(y1 - y2) < 1e-4);
        CHECK_TRUE(orthogonal);
    }
}

TEST(EdaAccelerators_Unit, CalcPcbPlacementNonOverlappingAndDecouplingProximity) {
    EdaAccelerators acc(_accCfg);
    nlohmann::json args = {
        {"board", {{"width_mm", 50.0}, {"height_mm", 40.0}, {"clearance_mm", 0.5}}},
        {"components", nlohmann::json::array({
            {{"reference", "J1"}, {"width_mm", 6.0}, {"height_mm", 8.0}, {"fixed", true}, {"x_mm", 4.0}, {"y_mm", 20.0}},
            {{"reference", "U1"}, {"width_mm", 8.0}, {"height_mm", 8.0}, {"role", "mcu"}},
            {{"reference", "C1"}, {"width_mm", 2.0}, {"height_mm", 1.2}, {"decouples", "U1"}},
            {{"reference", "R1"}, {"width_mm", 2.0}, {"height_mm", 1.2}},
            {{"reference", "U2"}, {"width_mm", 5.0}, {"height_mm", 4.0}, {"role", "regulator"}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "VIN"}, {"pins", nlohmann::json::array({"J1", "U2"})}},
            {{"name", "3V3"}, {"pins", nlohmann::json::array({"U2", "U1", "C1"})}},
            {{"name", "IO1"}, {"pins", nlohmann::json::array({"U1", "R1"})}}
        })}
    };

    bool ok = false;
    std::string summary;
    nlohmann::json res = acc.calcPcbPlacement(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_EQUAL(0, res.value("courtyard_overlaps", -1));
    CHECK_EQUAL(5, static_cast<int>(res["placements"].size()));
}

TEST(EdaAccelerators_Unit, CalcPcbAutoroutePathFinderMultiLayer) {
    EdaAccelerators acc(_accCfg);
    nlohmann::json args = {
        {"board", {{"width_mm", 30.0}, {"height_mm", 25.0}, {"layers", 2}}},
        {"rules", {{"track_width_mm", 0.25}, {"clearance_mm", 0.2}, {"via_size_mm", 0.6}, {"via_drill_mm", 0.3}}},
        {"obstacles", nlohmann::json::array({
            {{"x_mm", 15.0}, {"y_mm", 12.5}, {"width_mm", 3.0}, {"height_mm", 6.0}, {"layer", "F.Cu"}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "NET_A"}, {"pads", nlohmann::json::array({
                {{"x_mm", 5.0}, {"y_mm", 12.5}, {"layer", "F.Cu"}},
                {{"x_mm", 25.0}, {"y_mm", 12.5}, {"layer", "F.Cu"}}
            })}},
            {{"name", "NET_B"}, {"pads", nlohmann::json::array({
                {{"x_mm", 15.0}, {"y_mm", 4.0}, {"layer", "F.Cu"}},
                {{"x_mm", 15.0}, {"y_mm", 21.0}, {"layer", "F.Cu"}}
            })}}
        })}
    };

    bool ok = false;
    std::string summary;
    nlohmann::json res = acc.calcPcbAutoroute(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_EQUAL(2, res.value("routed_nets", 0));
    CHECK_EQUAL(0, res.value("unrouted_nets", -1));
    CHECK_EQUAL(0, res.value("drc_violations", -1));
    CHECK_TRUE(res["tracks"].size() >= 2);

    // Fault injection: single-layer board completely blocked across the middle
    nlohmann::json blockedArgs = {
        {"board", {{"width_mm", 20.0}, {"height_mm", 20.0}, {"layers", 1}}},
        {"obstacles", nlohmann::json::array({
            {{"x_mm", 10.0}, {"y_mm", 10.0}, {"width_mm", 4.0}, {"height_mm", 25.0}, {"layer", "F.Cu"}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "BLOCKED_NET"}, {"pads", nlohmann::json::array({
                {{"x_mm", 3.0}, {"y_mm", 10.0}, {"layer", "F.Cu"}},
                {{"x_mm", 17.0}, {"y_mm", 10.0}, {"layer", "F.Cu"}}
            })}}
        })}
    };
    bool blockedOk = true;
    nlohmann::json blockedRes = acc.calcPcbAutoroute(blockedArgs, blockedOk, summary);
    CHECK_FALSE(blockedOk);
    CHECK_EQUAL(1, blockedRes.value("unrouted_nets", 0));
}

TEST(EdaAccelerators_Unit, CalcElectricalSynthAllFourModes) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // 1. Voltage divider / regulator feedback
    nlohmann::json divRes = acc.calcElectricalSynth({
        {"mode", "divider"},
        {"vin_v", 12.0},
        {"vout_target_v", 3.3},
        {"series", "E24"}
    }, ok, summary);
    CHECK_TRUE(ok);
    DOUBLES_EQUAL(3.3, divRes.value("vout_actual_v", 0.0), 0.05);

    // 2. IPC-2152 trace width
    nlohmann::json ipcRes = acc.calcElectricalSynth({
        {"mode", "ipc2152"},
        {"current_a", 3.0},
        {"temp_rise_c", 20.0},
        {"copper_oz", 1.0},
        {"length_mm", 40.0}
    }, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(ipcRes.value("external_width_mm", 0.0) > 0.4);
    CHECK_TRUE(ipcRes.value("recommended_vias", 0) >= 2);

    // 3. Microstrip / differential impedance
    nlohmann::json impRes = acc.calcElectricalSynth({
        {"mode", "impedance"},
        {"type", "diff_microstrip"},
        {"target_z_ohm", 90.0},
        {"dielectric_h_mm", 0.15},
        {"er", 4.4},
        {"spacing_mm", 0.15}
    }, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(impRes.value("trace_width_mm", 0.0) > 0.05);

    // 4. Power rail decoupling budget
    nlohmann::json pwrRes = acc.calcElectricalSynth({
        {"mode", "power_budget"},
        {"rails", nlohmann::json::array({
            {{"name", "3V3"}, {"voltage_v", 3.3}, {"current_a", 0.8}, {"ic_power_pins", 6}},
            {{"name", "5V_SERVO"}, {"voltage_v", 5.0}, {"current_a", 2.5}, {"ic_power_pins", 2}}
        })},
        {"vin_v", 12.0}
    }, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(pwrRes.value("total_power_w", 0.0) > 10.0);
    CHECK_TRUE(pwrRes["decoupling_bom"].size() >= 2);
}

TEST(EdaAccelerators_Unit, CalcRobotKinematicsStaticsTorqueAndComMargin) {
    EdaAccelerators acc(_accCfg);
    nlohmann::json args = {
        {"links", nlohmann::json::array({
            {{"name", "shoulder_link"}, {"length_m", 0.12}, {"mass_kg", 0.18}, {"angle_deg", 0.0}},
            {{"name", "elbow_link"}, {"length_m", 0.10}, {"mass_kg", 0.12}, {"angle_deg", 0.0}},
            {{"name", "wrist_link"}, {"length_m", 0.06}, {"mass_kg", 0.08}, {"angle_deg", 0.0}}
        })},
        {"payload_kg", 0.25},
        {"dynamic_factor", 1.5},
        {"servo_stall_torque_kgcm", 25.0},
        {"support_polygon_half_width_m", 0.10}
    };

    bool ok = false;
    std::string summary;
    nlohmann::json res = acc.calcRobotKinematicsStatics(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_EQUAL(3, static_cast<int>(res["joints"].size()));
    CHECK_TRUE(res["joints"][0]["required_torque_nm"].get<double>() >
               res["joints"][1]["required_torque_nm"].get<double>());
    CHECK_TRUE(res["joints"][0]["safety_factor"].get<double>() > 1.0);
    CHECK_TRUE(res["end_effector"]["reach_m"].get<double>() > 0.27);
}

TEST(EdaAccelerators_Unit, CalcMechanismSynthGearsPlanetarySplinesAndLinkage) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // Planetary gearset: Z_s=16, Z_p=16 -> Z_r = 48
    nlohmann::json planRes = acc.calcMechanismSynth({
        {"type", "planetary"},
        {"module_mm", 1.0},
        {"sun_teeth", 16},
        {"planet_teeth", 16},
        {"num_planets", 3},
        {"thickness_mm", 6.0},
        {"clearance_mm", 0.20}
    }, ok, summary);
    CHECK_TRUE(ok);
    CHECK_EQUAL(48, planRes.value("ring_teeth", 0));
    DOUBLES_EQUAL(4.0, planRes.value("carrier_reduction_ratio", 0.0), 0.001);
    CHECK_TRUE(!planRes.value("freecad_python", std::string()).empty());

    // Invalid planetary tooth constraint rejection
    nlohmann::json badPlan = acc.calcMechanismSynth({
        {"type", "planetary"},
        {"module_mm", 1.0},
        {"sun_teeth", 17},
        {"planet_teeth", 16},
        {"ring_teeth", 50},
        {"num_planets", 3}
    }, ok, summary);
    CHECK_FALSE(ok);

    // 25T Futaba servo spline
    nlohmann::json splineRes = acc.calcMechanismSynth({
        {"type", "servo_spline"},
        {"spline_standard", "25T_futaba"},
        {"clearance_mm", 0.15}
    }, ok, summary);
    CHECK_TRUE(ok);
    CHECK_EQUAL(25, splineRes.value("tooth_count", 0));
    CHECK_EQUAL(25, static_cast<int>(splineRes["profile_points"].size() / 2));

    // Grashof 4-bar linkage vs non-Grashof rejection
    nlohmann::json barOk = acc.calcMechanismSynth({
        {"type", "four_bar"},
        {"ground_mm", 40.0},
        {"crank_mm", 15.0},
        {"coupler_mm", 35.0},
        {"rocker_mm", 30.0}
    }, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(barOk.value("is_grashof", false));

    nlohmann::json barBad = acc.calcMechanismSynth({
        {"type", "four_bar"},
        {"ground_mm", 40.0},
        {"crank_mm", 35.0},
        {"coupler_mm", 35.0},
        {"rocker_mm", 35.0}
    }, ok, summary);
    CHECK_FALSE(ok);
    CHECK_FALSE(barBad.value("is_grashof", true));
}

TEST(EdaAccelerators_Unit, CalcPrintOrientationDfmAnisotropicStress) {
    EdaAccelerators acc(_accCfg);
    nlohmann::json args = {
        {"material", "PETG"},
        {"bounding_box_mm", {{"x", 60.0}, {"y", 25.0}, {"z", 12.0}}},
        {"cross_section_mm2", 180.0},
        {"section_modulus_mm3", 360.0},
        {"load_vector_n", {{"fx", 0.0}, {"fy", 0.0}, {"fz", 85.0}}},
        {"moment_vector_nm", {{"mx", 2.5}, {"my", 0.0}, {"mz", 0.0}}},
        {"bearing_bore_axis", {{"x", 0.0}, {"y", 0.0}, {"z", 1.0}}},
        {"pin_diameter_mm", 4.0},
        {"wall_thickness_mm", 2.5}
    };

    bool ok = false;
    std::string summary;
    nlohmann::json res = acc.calcPrintOrientationDfm(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_TRUE(res.contains("optimal_orientation"));
    CHECK_TRUE(res.value("delamination_safety_factor", 0.0) > 1.0);
    CHECK_TRUE(res["candidates"].size() >= 6);
}

TEST(EdaAccelerators_Unit, DirectEdaBridgeCallToolAndTelemetryRingBuffer) {
    EdaBridgeConfig cfg;
    cfg.enabled = false; // Remote Windows bridge offline, local accelerators still work!
    cfg.accelerators.enabled = true;
    EdaBridge bridge(cfg);

    nlohmann::json res = bridge.callTool("eda_calc_electrical_synth", {
        {"mode", "divider"},
        {"vin_v", 5.0},
        {"vout_target_v", 3.3}
    });
    CHECK_TRUE(res.value("ok", false));

    EdaBridgeStatus st = bridge.getStatus();
    CHECK_TRUE(st.tools.size() == 4);
    CHECK_EQUAL("accelerators", st.tools[0].name);
    CHECK_TRUE(st.tools[0].available);
    CHECK_EQUAL(1, static_cast<int>(st.tools[0].totalCalls));
    CHECK_EQUAL(1, static_cast<int>(st.tools[0].successCalls));
    CHECK_EQUAL("eda_calc_electrical_synth", st.tools[0].lastToolCalled);
    CHECK_EQUAL(1, static_cast<int>(st.recentCalls.size()));
    CHECK_EQUAL("eda_calc_electrical_synth", st.recentCalls[0].toolName);
}

TEST(EdaAccelerators_Unit, CalcPcbConnectivityAudit3DMultiLayerAndDefects) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // 1. Nominal 4-layer board:
    //    - NET_3V3: U1.1 (TopLayer/1 at 10,10) -> track on TopLayer to via (15,10)
    //               -> track on InnerLayer1/15 from (15,10) to PTH pad J1.1 (MultiLayer/12 at 22,10)
    //               -> track on BottomLayer/2 from J1.1 (22,10) to SMD pad C1.1 (BottomLayer/2 at 28,10).
    //    - NET_GND: U1.2 (TopLayer at 10,14) -> track on TopLayer to (14,14) -> via (14,14)
    //               -> track on InnerLayer2/16 from (14,14) to (28,14) -> via (28,14) -> C1.2 (BottomLayer at 28,14).
    nlohmann::json nominalArgs = {
        {"pads", nlohmann::json::array({
            {{"id", "p1"}, {"ref", "U1"}, {"pad", "1"}, {"net", "3V3"}, {"x", 10.0}, {"y", 10.0}, {"layer", "TopLayer"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "p2"}, {"ref", "J1"}, {"pad", "1"}, {"net", "3V3"}, {"x", 22.0}, {"y", 10.0}, {"layer", "MultiLayer"}, {"size_x", 1.2}, {"size_y", 1.2}},
            {{"id", "p3"}, {"ref", "C1"}, {"pad", "1"}, {"net", "3V3"}, {"x", 28.0}, {"y", 10.0}, {"layer", "BottomLayer"}, {"size_x", 0.8}, {"size_y", 0.8}},
            {{"id", "p4"}, {"ref", "U1"}, {"pad", "2"}, {"net", "GND"}, {"x", 10.0}, {"y", 14.0}, {"layer", "1"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "p5"}, {"ref", "C1"}, {"pad", "2"}, {"net", "GND"}, {"x", 28.0}, {"y", 14.0}, {"layer", "2"}, {"size_x", 0.8}, {"size_y", 0.8}},
            {{"id", "mh1"}, {"ref", "H1"}, {"pad", "MP"}, {"net", ""}, {"x", 2.0}, {"y", 2.0}, {"layer", "12"}, {"size_x", 3.2}, {"size_y", 3.2}}
        })},
        {"tracks", nlohmann::json::array({
            {{"id", "t1"}, {"net", "3V3"}, {"layer", "TopLayer"}, {"x1", 10.0}, {"y1", 10.0}, {"x2", 15.0}, {"y2", 10.0}, {"width", 0.25}},
            {{"id", "t2"}, {"net", "3V3"}, {"layer", "InnerLayer1"}, {"x1", 15.0}, {"y1", 10.0}, {"x2", 22.0}, {"y2", 10.0}, {"width", 0.25}},
            {{"id", "t3"}, {"net", "3V3"}, {"layer", "BottomLayer"}, {"x1", 22.0}, {"y1", 10.0}, {"x2", 28.0}, {"y2", 10.0}, {"width", 0.25}},
            {{"id", "t4"}, {"net", "GND"}, {"layer", "1"}, {"x1", 10.0}, {"y1", 14.0}, {"x2", 14.0}, {"y2", 14.0}, {"width", 0.3}},
            {{"id", "t5"}, {"net", "GND"}, {"layer", "16"}, {"x1", 14.0}, {"y1", 14.0}, {"x2", 28.0}, {"y2", 14.0}, {"width", 0.3}}
        })},
        {"vias", nlohmann::json::array({
            {{"id", "v1"}, {"net", "3V3"}, {"x", 15.0}, {"y", 10.0}, {"diameter", 0.6}, {"drill", 0.3}},
            {{"id", "v2"}, {"net", "GND"}, {"x", 14.0}, {"y", 14.0}, {"diameter", 0.6}, {"drill", 0.3}},
            {{"id", "v3"}, {"net", "GND"}, {"x", 28.0}, {"y", 14.0}, {"diameter", 0.6}, {"drill", 0.3}}
        })},
        {"schematic_components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "J1"}, {"value", "CONN"}, {"footprint", "HDR-1x2"}},
            {{"reference", "C1"}, {"value", "100nF"}, {"footprint", "C0603"}},
            {{"reference", "FRAME1"}, {"value", "A4"}, {"footprint", ""}, {"component_type", "netport"}}
        })},
        {"pcb_components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "J1"}, {"value", "CONN"}, {"footprint", "HDR-1x2"}},
            {{"reference", "C1"}, {"value", "100nF"}, {"footprint", "C0603"}}
        })}
    };

    nlohmann::json nomRes = acc.calcPcbConnectivityAudit(nominalArgs, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(nomRes.value("ok", false));
    CHECK_TRUE(nomRes.value("all_connected", false));
    CHECK_TRUE(nomRes.value("schematic_pcb_synced", false));
    CHECK_EQUAL(2, nomRes.value("total_nets", 0));
    CHECK_EQUAL(2, nomRes.value("fully_routed_nets", 0));
    CHECK_EQUAL(0, nomRes.value("unrouted_nets_count", -1));
    CHECK_EQUAL(0, nomRes.value("dangling_tracks_count", -1));
    CHECK_EQUAL(0, nomRes.value("unassigned_pads_count", -1));
    CHECK_EQUAL(0, nomRes.value("unsynced_schematic_components_count", -1));

    // 2. Fault injection:
    //    - Broken net "SIG_BROKEN": 2 disconnected islands (U1.3 at 10,20 wired to 14,20; R2.1 at 20,20 unwired)
    //      Plus dangling track endpoint at (14,20) on TopLayer!
    //    - Unassigned pad: R2.2 has net == ""
    //    - Schematic has XT1 and REG0 missing from PCB
    nlohmann::json faultArgs = {
        {"pads", nlohmann::json::array({
            {{"id", "fp1"}, {"ref", "U1"}, {"pad", "3"}, {"net", "SIG_BROKEN"}, {"x", 10.0}, {"y", 20.0}, {"layer", "TopLayer"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "fp2"}, {"ref", "R2"}, {"pad", "1"}, {"net", "SIG_BROKEN"}, {"x", 20.0}, {"y", 20.0}, {"layer", "TopLayer"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "fp3"}, {"ref", "R2"}, {"pad", "2"}, {"net", ""}, {"x", 22.0}, {"y", 20.0}, {"layer", "TopLayer"}, {"size_x", 0.6}, {"size_y", 0.6}}
        })},
        {"tracks", nlohmann::json::array({
            {{"id", "ft1"}, {"net", "SIG_BROKEN"}, {"layer", "TopLayer"}, {"x1", 10.0}, {"y1", 20.0}, {"x2", 14.0}, {"y2", 20.0}, {"width", 0.25}}
        })},
        {"vias", nlohmann::json::array()},
        {"schematic_components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "R2"}, {"value", "10k"}, {"footprint", "R0603"}},
            {{"reference", "XT1"}, {"value", "XT30PW-M"}, {"footprint", "CONN-TH_XT30PW-M"}},
            {{"reference", "REG0"}, {"value", "TPS5430"}, {"footprint", "SOIC-8"}}
        })},
        {"pcb_components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "R2"}, {"value", "10k"}, {"footprint", "R0603"}}
        })}
    };

    bool faultOk = false;
    nlohmann::json faultRes = acc.calcPcbConnectivityAudit(faultArgs, faultOk, summary);
    CHECK_TRUE(faultOk);
    CHECK_TRUE(faultRes.value("ok", false));
    CHECK_FALSE(faultRes.value("all_connected", true));
    CHECK_FALSE(faultRes.value("schematic_pcb_synced", true));
    CHECK_EQUAL(1, faultRes.value("unrouted_nets_count", 0));
    CHECK_EQUAL(1, static_cast<int>(faultRes["unrouted_nets"].size()));
    CHECK_EQUAL("SIG_BROKEN", faultRes["unrouted_nets"][0].value("net", ""));
    CHECK_EQUAL(2, faultRes["unrouted_nets"][0].value("island_count", 0));
    DOUBLES_EQUAL(10.0, faultRes["unrouted_nets"][0]["airwire"].value("distance_mm", 0.0), 0.1);
    CHECK_EQUAL(1, faultRes.value("dangling_tracks_count", 0));
    DOUBLES_EQUAL(14.0, faultRes["dangling_tracks"][0].value("x", 0.0), 0.05);
    DOUBLES_EQUAL(20.0, faultRes["dangling_tracks"][0].value("y", 0.0), 0.05);
    CHECK_EQUAL(1, faultRes.value("unassigned_pads_count", 0));
    CHECK_EQUAL("R2", faultRes["unassigned_pads"][0].value("ref", ""));
    CHECK_EQUAL(2, faultRes.value("unsynced_schematic_components_count", 0));
}

TEST(EdaAccelerators_Unit, CalcPcbConnectivityAuditEasyEdaMilUnitsAndComponentsArray) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // EasyEDA Pro canvas in mil units:
    // - NET_MIL_OK: pads at (1000, 1000) and (2000, 1000) without explicit size_x/size_y
    //   (default pad size in mil is 24.0 mils -> radius 12.0 mils).
    //   Track from (1008, 1000) to (1992, 1000) with width 10 mils (endpoint 8 mils from pad center).
    // - NET_MIL_BROKEN: pads at (1000, 2000) and (2000, 2000) (1000 mils = 25.4 mm apart), unwired.
    // - "components" array (EasyEDA handleInspectPcb format) has U1 and R1, while schematic_components
    //   has U1, R1, and XT1 -> unsynced_schematic_components_count must be 1 (XT1 only).
    nlohmann::json milArgs = {
        {"units", "mil"},
        {"pads", nlohmann::json::array({
            {{"id", "mp1"}, {"ref", "U1"}, {"pad", "1"}, {"net", "NET_MIL_OK"}, {"x", 1000.0}, {"y", 1000.0}, {"layer", "TopLayer"}},
            {{"id", "mp2"}, {"ref", "R1"}, {"pad", "1"}, {"net", "NET_MIL_OK"}, {"x", 2000.0}, {"y", 1000.0}, {"layer", "TopLayer"}},
            {{"id", "mp3"}, {"ref", "U1"}, {"pad", "2"}, {"net", "NET_MIL_BROKEN"}, {"x", 1000.0}, {"y", 2000.0}, {"layer", "TopLayer"}},
            {{"id", "mp4"}, {"ref", "R1"}, {"pad", "2"}, {"net", "NET_MIL_BROKEN"}, {"x", 2000.0}, {"y", 2000.0}, {"layer", "TopLayer"}}
        })},
        {"tracks", nlohmann::json::array({
            {{"id", "mt1"}, {"net", "NET_MIL_OK"}, {"layer", "TopLayer"}, {"x1", 1008.0}, {"y1", 1000.0}, {"x2", 1992.0}, {"y2", 1000.0}, {"width", 10.0}}
        })},
        {"vias", nlohmann::json::array()},
        {"components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "R1"}, {"value", "10k"}, {"footprint", "0603"}}
        })},
        {"schematic_components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "R1"}, {"value", "10k"}, {"footprint", "0603"}},
            {{"reference", "XT1"}, {"value", "XT30"}, {"footprint", "HC-49"}}
        })}
    };

    nlohmann::json milRes = acc.calcPcbConnectivityAudit(milArgs, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(milRes.value("ok", false));
    CHECK_EQUAL(2, milRes.value("total_nets", 0));
    CHECK_EQUAL(1, milRes.value("fully_routed_nets", 0));
    CHECK_EQUAL(1, milRes.value("unrouted_nets_count", 0));
    CHECK_EQUAL(0, milRes.value("dangling_tracks_count", -1));
    CHECK_EQUAL("NET_MIL_BROKEN", milRes["unrouted_nets"][0].value("net", ""));
    DOUBLES_EQUAL(25.4, milRes["unrouted_nets"][0]["airwire"].value("distance_mm", 0.0), 0.1);
    CHECK_EQUAL(1, milRes.value("unsynced_schematic_components_count", -1));
    CHECK_EQUAL("XT1", milRes["unsynced_schematic_components"][0].value("reference", ""));

    // RapidCheck property: mm vs mil coordinate equivalence on calcPcbConnectivityAudit
    setenv("RC_PARAMS", "max_success=50", 0);
    MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();
    bool propOk = rc::check("calcPcbConnectivityAudit mil coordinates match mm coordinates", [&]() {
        const int distMmInt = *rc::gen::inRange(5, 80);
        const bool connected = *rc::gen::arbitrary<bool>();
        double x1Mm = 10.0;
        double y1Mm = 10.0;
        double x2Mm = 10.0 + static_cast<double>(distMmInt);
        double y2Mm = 10.0;

        nlohmann::json mmInput = {
            {"units", "mm"},
            {"pads", nlohmann::json::array({
                {{"id", "p1"}, {"ref", "U1"}, {"pad", "1"}, {"net", "N1"}, {"x", x1Mm}, {"y", y1Mm}, {"layer", "TopLayer"}},
                {{"id", "p2"}, {"ref", "U2"}, {"pad", "1"}, {"net", "N1"}, {"x", x2Mm}, {"y", y2Mm}, {"layer", "TopLayer"}}
            })},
            {"tracks", connected ? nlohmann::json::array({
                {{"id", "t1"}, {"net", "N1"}, {"layer", "TopLayer"}, {"x1", x1Mm + 0.15}, {"y1", y1Mm}, {"x2", x2Mm - 0.15}, {"y2", y2Mm}}
            }) : nlohmann::json::array()}
        };

        const double kMmToMil = 1.0 / 0.0254;
        nlohmann::json milInput = {
            {"units", "mil"},
            {"pads", nlohmann::json::array({
                {{"id", "p1"}, {"ref", "U1"}, {"pad", "1"}, {"net", "N1"}, {"x", x1Mm * kMmToMil}, {"y", y1Mm * kMmToMil}, {"layer", "TopLayer"}},
                {{"id", "p2"}, {"ref", "U2"}, {"pad", "1"}, {"net", "N1"}, {"x", x2Mm * kMmToMil}, {"y", y2Mm * kMmToMil}, {"layer", "TopLayer"}}
            })},
            {"tracks", connected ? nlohmann::json::array({
                {{"id", "t1"}, {"net", "N1"}, {"layer", "TopLayer"}, {"x1", (x1Mm + 0.15) * kMmToMil}, {"y1", y1Mm * kMmToMil}, {"x2", (x2Mm - 0.15) * kMmToMil}, {"y2", y2Mm * kMmToMil}}
            }) : nlohmann::json::array()}
        };

        bool okMm = false, okMil = false;
        std::string sumMm, sumMil;
        nlohmann::json rMm = acc.calcPcbConnectivityAudit(mmInput, okMm, sumMm);
        nlohmann::json rMil = acc.calcPcbConnectivityAudit(milInput, okMil, sumMil);
        RC_ASSERT(okMm && okMil);
        RC_ASSERT(rMm.value("all_connected", false) == connected);
        RC_ASSERT(rMil.value("all_connected", false) == connected);
        RC_ASSERT(rMm.value("fully_routed_nets", -1) == rMil.value("fully_routed_nets", -2));
        if (!connected) {
            double dMm = rMm["unrouted_nets"][0]["airwire"].value("distance_mm", 0.0);
            double dMil = rMil["unrouted_nets"][0]["airwire"].value("distance_mm", -100.0);
            RC_ASSERT(std::fabs(dMm - dMil) < 0.05);
        }
    });
    MemoryLeakWarningPlugin::turnOnDefaultNotThreadSafeNewDeleteOverloads();
    CHECK_TRUE(propOk);
}

TEST(EdaAccelerators_Unit, CalcPcbAutoroute4LayerEasyEdaAndIncrementalExistingTracks) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // 4-layer EasyEDA board:
    // - Foreign existing track on TopLayer blocking direct y=10 path at x=12
    // - Same-net existing track on TopLayer for NET_INC from (5, 10) to (9, 10)
    // - Pad 1 at (5, 10) on TopLayer, Pad 2 at (20, 10) on MultiLayer (PTH pad reachable on any layer!)
    nlohmann::json args = {
        {"board", {{"width_mm", 25.0}, {"height_mm", 20.0}, {"layers", 4}, {"layer_style", "easyeda"}}},
        {"rules", {{"track_width_mm", 0.25}, {"clearance_mm", 0.25}, {"via_size_mm", 0.6}, {"via_drill_mm", 0.3}}},
        {"existing_tracks", nlohmann::json::array({
            {{"net", "BLOCKER_NET"}, {"layer", "TopLayer"}, {"x1_mm", 12.0}, {"y1_mm", 2.0}, {"x2_mm", 12.0}, {"y2_mm", 18.0}, {"width_mm", 0.4}},
            {{"net", "NET_INC"}, {"layer", "TopLayer"}, {"x1_mm", 5.0}, {"y1_mm", 10.0}, {"x2_mm", 9.0}, {"y2_mm", 10.0}, {"width_mm", 0.25}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "NET_INC"}, {"pads", nlohmann::json::array({
                {{"x_mm", 5.0}, {"y_mm", 10.0}, {"layer", "TopLayer"}},
                {{"x_mm", 20.0}, {"y_mm", 10.0}, {"layer", "MultiLayer"}}
            })}}
        })}
    };

    nlohmann::json res = acc.calcPcbAutoroute(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_EQUAL(1, res.value("routed_nets", 0));
    CHECK_EQUAL(0, res.value("unrouted_nets", -1));
    CHECK_TRUE(res["tracks"].size() >= 1);

    // Verify EasyEDA layer naming style is emitted on output tracks
    for (const auto& t : res["tracks"]) {
        std::string ly = t.value("layer", "");
        bool validEasyEdaLayer = (ly == "TopLayer" || ly == "InnerLayer1" ||
                                  ly == "InnerLayer2" || ly == "BottomLayer");
        CHECK_TRUE_TEXT(validEasyEdaLayer, ly.c_str());
    }
}

TEST(EdaAccelerators_Unit, CalcSchematicErcAuditNominalTJunctionNegatedYAndFaults) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // 1. Nominal clean schematic:
    //    - U1 (part) pin 1 at (100, 630), pin 2 at (200, -630) [negated y from sch_PrimitivePin.modify],
    //      pin 3 at (150, 700) with no_connected: true
    //    - R1 (part) pin 1 at (200, 630) [colocated with U1.2 after y-sign normalization],
    //      pin 2 at (300, 630)
    //    - C1 (part) pin 1 at (250, 630) [lands mid-segment on wire w2 from (200,630) to (300,630) -> T-junction],
    //      pin 2 at (250, 800)
    //    - #PWR1 (netflag, value "VCC") pin 1 at (100, 500) wired to U1.1 via w1
    //    - #PWR2 (netflag, value "GND") pin 1 at (300, 800) wired to R1.2 and C1.2 via w3 + T-junction w4
    nlohmann::json cleanArgs = {
        {"components", nlohmann::json::array({
            {
                {"id", "e1"},
                {"reference", "U1"},
                {"value", "STM32G0"},
                {"component_type", "part"},
                {"footprint", "LQFP-32"},
                {"unique_id", "uid_u1"},
                {"x", 150.0},
                {"y", 630.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u1_p1"}, {"number", "1"}, {"name", "VDD"}, {"net", ""}, {"x", 100.0}, {"y", 630.0}, {"no_connected", false}},
                    {{"id", "u1_p2"}, {"number", "2"}, {"name", "PA0"}, {"net", ""}, {"x", 200.0}, {"y", -630.0}, {"no_connected", false}},
                    {{"id", "u1_p3"}, {"number", "3"}, {"name", "NRST"}, {"net", ""}, {"x", 150.0}, {"y", 700.0}, {"no_connected", true}}
                })}
            },
            {
                {"id", "e2"},
                {"reference", "R1"},
                {"value", "10k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uid_r1"},
                {"x", 280.0},
                {"y", 630.0},
                {"pins", nlohmann::json::array({
                    {{"id", "r1_p1"}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 260.0}, {"y", 630.0}, {"no_connected", false}},
                    {{"id", "r1_p2"}, {"number", "2"}, {"name", "2"}, {"net", ""}, {"x", 300.0}, {"y", 630.0}, {"no_connected", false}}
                })}
            },
            {
                {"id", "e3"},
                {"reference", "C1"},
                {"value", "100nF"},
                {"component_type", "part"},
                {"footprint", "C0603"},
                {"unique_id", "uid_c1"},
                {"x", 230.0},
                {"y", 675.0},
                {"pins", nlohmann::json::array({
                    {{"id", "c1_p1"}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 230.0}, {"y", 630.0}, {"no_connected", false}},
                    {{"id", "c1_p2"}, {"number", "2"}, {"name", "2"}, {"net", ""}, {"x", 230.0}, {"y", 720.0}, {"no_connected", false}}
                })}
            },
            {
                {"id", "nf1"},
                {"reference", "#PWR1"},
                {"value", "VCC"},
                {"component_type", "netflag"},
                {"footprint", ""},
                {"unique_id", ""},
                {"x", 100.0},
                {"y", 500.0},
                {"pins", nlohmann::json::array({
                    {{"id", "nf1_p1"}, {"number", "1"}, {"name", "VCC"}, {"net", "VCC"}, {"x", 100.0}, {"y", 500.0}, {"no_connected", false}}
                })}
            },
            {
                {"id", "nf2"},
                {"reference", "#PWR2"},
                {"value", "GND"},
                {"component_type", "netflag"},
                {"footprint", ""},
                {"unique_id", ""},
                {"x", 300.0},
                {"y", 800.0},
                {"pins", nlohmann::json::array({
                    {{"id", "nf2_p1"}, {"number", "1"}, {"name", "GND"}, {"net", "GND"}, {"x", 300.0}, {"y", 800.0}, {"no_connected", false}}
                })}
            }
        })},
        {"wires", nlohmann::json::array({
            {{"id", "w1"}, {"net", "VCC"}, {"line", nlohmann::json::array({{100.0, 500.0}, {100.0, 630.0}})}},
            {{"id", "w2"}, {"net", "PA0_NET"}, {"line", nlohmann::json::array({{200.0, 630.0}, {260.0, 630.0}})}},
            {{"id", "w3"}, {"net", "GND"}, {"line", nlohmann::json::array({{300.0, 630.0}, {300.0, 800.0}})}},
            {{"id", "w4"}, {"net", "GND"}, {"line", nlohmann::json::array({{230.0, 720.0}, {300.0, 720.0}})}}
        })}
    };

    nlohmann::json cleanRes = acc.calcSchematicErcAudit(cleanArgs, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(cleanRes.value("ok", false));
    CHECK_TRUE(cleanRes.value("passed", false));
    CHECK_EQUAL(3, cleanRes.value("total_part_components", 0));
    CHECK_EQUAL(7, cleanRes.value("total_pins", 0));
    CHECK_EQUAL(0, cleanRes.value("unconnected_pins_count", -1));
    CHECK_EQUAL(0, cleanRes.value("dangling_wires_count", -1));
    CHECK_EQUAL(0, cleanRes.value("component_issues_count", -1));
    CHECK_EQUAL(0, cleanRes.value("net_issues_count", -1));
    CHECK_TRUE(cleanRes.contains("inferred_nets"));

    // Also verify executeTool dispatch for eda_calc_schematic_erc_audit
    bool execOk = false;
    std::string execSum;
    nlohmann::json execRes = acc.executeTool("eda_calc_schematic_erc_audit", cleanArgs, execOk, execSum);
    CHECK_TRUE(execOk);
    CHECK_TRUE(execRes.value("passed", false));

    // 2. Fault injection:
    //    - Unconnected pin on U1.2 (no_connected: false, not touching any wire/pin)
    //    - Dangling wire w_dangling with only 1 connected endpoint (or isolated stub)
    //    - Component issues:
    //      * R? unannotated designator
    //      * Duplicate designator U1
    //      * Missing unique_id on U1_dup
    //      * Missing footprint on R?
    //    - Net issues:
    //      * Power rail short: VCC and GND netflags shorted onto the same wire w_short
    nlohmann::json faultArgs = {
        {"components", nlohmann::json::array({
            {
                {"id", "f_u1"},
                {"reference", "U1"},
                {"value", "MCU"},
                {"component_type", "part"},
                {"footprint", "QFN-32"},
                {"unique_id", "uid_1"},
                {"x", 100.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "fu1_p1"}, {"number", "1"}, {"name", "VCC"}, {"net", ""}, {"x", 100.0}, {"y", 100.0}, {"no_connected", false}},
                    {{"id", "fu1_p2"}, {"number", "2"}, {"name", "FLOATING"}, {"net", ""}, {"x", 500.0}, {"y", 500.0}, {"no_connected", false}}
                })}
            },
            {
                {"id", "f_u1_dup"},
                {"reference", "U1"},
                {"value", "MCU_DUP"},
                {"component_type", "part"},
                {"footprint", "QFN-32"},
                {"unique_id", ""},
                {"x", 200.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "fu1d_p1"}, {"number", "1"}, {"name", "GND"}, {"net", ""}, {"x", 200.0}, {"y", 100.0}, {"no_connected", false}}
                })}
            },
            {
                {"id", "f_r_unannot"},
                {"reference", "R?"},
                {"value", "10k"},
                {"component_type", "part"},
                {"footprint", ""},
                {"unique_id", "uid_r"},
                {"x", 300.0},
                {"y", 300.0},
                {"pins", nlohmann::json::array({
                    {{"id", "fr_p1"}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 300.0}, {"y", 300.0}, {"no_connected", true}}
                })}
            },
            {
                {"id", "f_nf_vcc"},
                {"reference", "#PWR1"},
                {"value", "VCC"},
                {"component_type", "netflag"},
                {"footprint", ""},
                {"unique_id", ""},
                {"x", 100.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "fnf1"}, {"number", "1"}, {"name", "VCC"}, {"net", "VCC"}, {"x", 100.0}, {"y", 100.0}, {"no_connected", false}}
                })}
            },
            {
                {"id", "f_nf_gnd"},
                {"reference", "#PWR2"},
                {"value", "GND"},
                {"component_type", "netflag"},
                {"footprint", ""},
                {"unique_id", ""},
                {"x", 200.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "fnf2"}, {"number", "1"}, {"name", "GND"}, {"net", "GND"}, {"x", 200.0}, {"y", 100.0}, {"no_connected", false}}
                })}
            }
        })},
        {"wires", nlohmann::json::array({
            {{"id", "w_short"}, {"net", ""}, {"line", nlohmann::json::array({{100.0, 100.0}, {200.0, 100.0}})}},
            {{"id", "w_dangling"}, {"net", ""}, {"line", nlohmann::json::array({{700.0, 700.0}, {750.0, 700.0}})}}
        })}
    };

    bool faultOk = false;
    std::string faultSummary;
    nlohmann::json faultRes = acc.calcSchematicErcAudit(faultArgs, faultOk, faultSummary);
    CHECK_TRUE(faultOk);
    CHECK_TRUE(faultRes.value("ok", false));
    CHECK_FALSE(faultRes.value("passed", true));
    CHECK_EQUAL(1, faultRes.value("unconnected_pins_count", 0));
    CHECK_EQUAL("fu1_p2", faultRes["unconnected_pins"][0].value("id", ""));
    CHECK_EQUAL(1, faultRes.value("dangling_wires_count", 0));
    CHECK_EQUAL("w_dangling", faultRes["dangling_wires"][0].value("wire_id", ""));
    CHECK_TRUE(faultRes.value("component_issues_count", 0) >= 4);
    CHECK_TRUE(faultRes.value("net_issues_count", 0) >= 1);
}

TEST(EdaAccelerators_Unit, CalcPcbConnectivityAuditCopperPoursPadSizesAndMultiSheetSync) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // 1. Copper pour on BottomLayer for net "GND" connects two pads (J1.2 at 5,5 and C1.2 at 25,15)
    //    without any track between them, and also anchors a track endpoint landing inside the pour!
    // 2. Large XT30 pad (J1.1 at 5,15 with width: 2.5, height: 2.5 mm) captures a track endpoint
    //    at (6.1, 15.0) [1.1 mm from pad center, which is > old 0.38 mm radius but <= 1.25 + 0.125 + 0.08 mm].
    // 3. Pad outside the pour polygon on net "SIG_OUT" (U1.4 at 35,35 vs R1.2 at 45,35) remains disconnected.
    // 4. Multi-sheet schematic_components contains duplicate references across sheets ("J1" twice, "XT_MISSING" twice)
    //    and deduplicates them so unsynced_schematic_components_count is 1 ("XT_MISSING").
    nlohmann::json args = {
        {"units", "mm"},
        {"pads", nlohmann::json::array({
            {{"id", "p_gnd1"}, {"ref", "J1"}, {"pad", "2"}, {"net", "GND"}, {"x", 5.0}, {"y", 5.0}, {"layer", "BottomLayer"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "p_gnd2"}, {"ref", "C1"}, {"pad", "2"}, {"net", "GND"}, {"x", 25.0}, {"y", 15.0}, {"layer", "BottomLayer"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "p_xt1"}, {"ref", "J1"}, {"pad", "1"}, {"net", "VBAT"}, {"x", 5.0}, {"y", 15.0}, {"layer", "MultiLayer"}, {"width", 2.5}, {"height", 2.5}},
            {{"id", "p_xt2"}, {"ref", "C1"}, {"pad", "1"}, {"net", "VBAT"}, {"x", 15.0}, {"y", 15.0}, {"layer", "TopLayer"}, {"size_x", 0.8}, {"size_y", 0.8}},
            {{"id", "p_out1"}, {"ref", "U1"}, {"pad", "4"}, {"net", "SIG_OUT"}, {"x", 35.0}, {"y", 35.0}, {"layer", "BottomLayer"}, {"size_x", 0.6}, {"size_y", 0.6}},
            {{"id", "p_out2"}, {"ref", "R1"}, {"pad", "2"}, {"net", "SIG_OUT"}, {"x", 45.0}, {"y", 35.0}, {"layer", "BottomLayer"}, {"size_x", 0.6}, {"size_y", 0.6}}
        })},
        {"tracks", nlohmann::json::array({
            {{"id", "t_vbat"}, {"net", "VBAT"}, {"layer", "TopLayer"}, {"x1", 6.1}, {"y1", 15.0}, {"x2", 15.0}, {"y2", 15.0}, {"width", 0.25}}
        })},
        {"vias", nlohmann::json::array()},
        {"pours", nlohmann::json::array({
            {
                {"id", "pour_gnd_bot"},
                {"net", "GND"},
                {"layer", "BottomLayer"},
                {"polygon", nlohmann::json::array({
                    {0.0, 0.0}, {30.0, 0.0}, {30.0, 20.0}, {0.0, 20.0}
                })}
            }
        })},
        {"pcb_components", nlohmann::json::array({
            {{"reference", "J1"}},
            {{"reference", "C1"}},
            {{"reference", "U1"}},
            {{"reference", "R1"}}
        })},
        {"schematic_components", nlohmann::json::array({
            {{"reference", "J1"}, {"value", "XT30"}, {"footprint", "XT30"}},
            {{"reference", "J1"}, {"value", "XT30"}, {"footprint", "XT30"}},
            {{"reference", "C1"}, {"value", "100nF"}, {"footprint", "C0603"}},
            {{"reference", "U1"}, {"value", "MCU"}, {"footprint", "QFN-32"}},
            {{"reference", "R1"}, {"value", "10k"}, {"footprint", "R0603"}},
            {{"reference", "XT_MISSING"}, {"value", "CONN"}, {"footprint", "HDR-2"}},
            {{"reference", "XT_MISSING"}, {"value", "CONN"}, {"footprint", "HDR-2"}}
        })}
    };

    nlohmann::json res = acc.calcPcbConnectivityAudit(args, ok, summary);
    CHECK_TRUE(ok);
    CHECK_TRUE(res.value("ok", false));
    CHECK_EQUAL(3, res.value("total_nets", 0));
    // Both GND (via pour) and VBAT (via large pad radius) are routed; only SIG_OUT is unrouted
    CHECK_EQUAL(2, res.value("fully_routed_nets", 0));
    CHECK_EQUAL(1, res.value("unrouted_nets_count", 0));
    CHECK_EQUAL("SIG_OUT", res["unrouted_nets"][0].value("net", ""));
    CHECK_EQUAL(0, res.value("dangling_tracks_count", -1));
    CHECK_EQUAL(1, res.value("unsynced_schematic_components_count", 0));
    CHECK_EQUAL("XT_MISSING", res["unsynced_schematic_components"][0].value("reference", ""));
}

TEST(EdaAccelerators_Unit, CalcSchematicErcAuditTJunctionToleranceAndFiveLayerSemanticErc) {
    EdaAccelerators acc(_accCfg);
    bool ok = false;
    std::string summary;

    // 1. T-Junction Segment Anchoring with configurable tolerance & empty-pins netflags:
    //    - Wire w_trunk from U1.1 (100, 100) to R1.1 (300, 100)
    //    - Wire w_tbranch has endpoint (200, 102.0) [2.0 mil from w_trunk midpoint (200, 100)]
    //      and endpoint (200, 200) landing on netflag #PWR_VCC (which has "pins": [] as returned by EasyEDA)
    //    - Wire w_true_dangle has endpoint (250, 100) on w_trunk and endpoint (250, 50) in free space
    nlohmann::json tjArgs = {
        {"tolerance", 2.5},
        {"components", nlohmann::json::array({
            {
                {"id", "c_u1"},
                {"reference", "U1"},
                {"value", "Sensor"},
                {"component_type", "part"},
                {"footprint", "SOT-23"},
                {"unique_id", "uid_u1"},
                {"x", 80.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u1_1"}, {"number", "1"}, {"name", "OUT"}, {"net", ""}, {"x", 100.0}, {"y", 100.0}}
                })}
            },
            {
                {"id", "c_r1"},
                {"reference", "R1"},
                {"value", "10k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uid_r1"},
                {"x", 320.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "r1_1"}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 300.0}, {"y", 100.0}}
                })}
            },
            {
                {"id", "nf_vcc"},
                {"reference", "#PWR1"},
                {"value", "+3V3"},
                {"component_type", "netflag"},
                {"x", 200.0},
                {"y", 200.0},
                {"pins", nlohmann::json::array()}
            }
        })},
        {"wires", nlohmann::json::array({
            {{"id", "w_trunk"}, {"net", "+3V3"}, {"line", nlohmann::json::array({{100.0, 100.0}, {300.0, 100.0}})}},
            {{"id", "w_tbranch"}, {"net", "+3V3"}, {"line", nlohmann::json::array({{200.0, 102.0}, {200.0, 200.0}})}},
            {{"id", "w_true_dangle"}, {"net", "+3V3"}, {"line", nlohmann::json::array({{250.0, 100.0}, {250.0, 50.0}})}}
        })}
    };

    nlohmann::json tjRes = acc.calcSchematicErcAudit(tjArgs, ok, summary);
    CHECK_TRUE(ok);
    // w_tbranch must be anchored (NOT in dangling_wires), while w_true_dangle MUST be flagged!
    CHECK_EQUAL(1, tjRes.value("dangling_wires_count", -1));
    CHECK_EQUAL("w_true_dangle", tjRes["dangling_wires"][0].value("wire_id", ""));
    CHECK_EQUAL("unanchored_wire_endpoint", tjRes["dangling_wires"][0].value("reason", ""));
    DOUBLES_EQUAL(250.0, tjRes["dangling_wires"][0].value("x", 0.0), 0.1);
    DOUBLES_EQUAL(50.0, tjRes["dangling_wires"][0].value("y", 0.0), 0.1);

    // 2. 5-Layer Semantic Schematic ERC Faults:
    //    - single_pin_nets: "SPI_MISO_ORPHAN" only connected to U1.4 (while "+3V3" and "GND" power nets and NC pin U1.5 are ignored)
    //    - overlapping_components: R2 rotated 90 deg at (510, 300) overlaps U2 bbox at (500, 300)
    //    - pullup_issues: "I2C_SDA" has pull-up R_PU to +3V3 (OK), but "I2C_SCL" and "nRST" lack pull-up resistors!
    //    - decoupling_issues: U1 (on +3V3 and GND) has C_DEC between +3V3 and GND (OK),
    //                         but U2 (on +5V and GND) has NO decoupling capacitor on +5V!
    //    - voltage_domain_issues: wire w_vshort shorts "+5V" and "+3V3" on the same net island!
    nlohmann::json semFaultArgs = {
        {"components", nlohmann::json::array({
            {
                {"id", "u1_id"},
                {"reference", "U1"},
                {"value", "STM32G0"},
                {"component_type", "part"},
                {"footprint", "LQFP-32"},
                {"unique_id", "uid_u1"},
                {"x", 100.0},
                {"y", 100.0},
                {"width", 80.0},
                {"height", 80.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u1_p1"}, {"number", "1"}, {"name", "VDD"}, {"net", "+3V3"}, {"x", 60.0}, {"y", 80.0}},
                    {{"id", "u1_p2"}, {"number", "2"}, {"name", "GND"}, {"net", "GND"}, {"x", 60.0}, {"y", 120.0}},
                    {{"id", "u1_p3"}, {"number", "3"}, {"name", "SDA"}, {"net", "I2C_SDA"}, {"x", 140.0}, {"y", 80.0}},
                    {{"id", "u1_p4"}, {"number", "4"}, {"name", "MISO"}, {"net", "SPI_MISO_ORPHAN"}, {"x", 140.0}, {"y", 100.0}},
                    {{"id", "u1_p5"}, {"number", "5"}, {"name", "SWO"}, {"net", "UNUSED_NC"}, {"x", 140.0}, {"y", 120.0}, {"no_connected", true}},
                    {{"id", "u1_p6"}, {"number", "6"}, {"name", "SCL"}, {"net", "I2C_SCL"}, {"x", 140.0}, {"y", 140.0}},
                    {{"id", "u1_p7"}, {"number", "7"}, {"name", "nRST"}, {"net", "MCU_nRST"}, {"x", 100.0}, {"y", 60.0}}
                })}
            },
            {
                {"id", "r_pu_id"},
                {"reference", "R1"},
                {"value", "4.7k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uid_r1"},
                {"x", 250.0},
                {"y", 80.0},
                {"width", 40.0},
                {"height", 20.0},
                {"pins", nlohmann::json::array({
                    {{"id", "r1_p1"}, {"number", "1"}, {"name", "1"}, {"net", "I2C_SDA"}, {"x", 230.0}, {"y", 80.0}},
                    {{"id", "r1_p2"}, {"number", "2"}, {"name", "2"}, {"net", "+3V3"}, {"x", 270.0}, {"y", 80.0}}
                })}
            },
            {
                {"id", "c_dec_id"},
                {"reference", "C1"},
                {"value", "100nF"},
                {"component_type", "part"},
                {"footprint", "C0603"},
                {"unique_id", "uid_c1"},
                {"x", 250.0},
                {"y", 200.0},
                {"width", 20.0},
                {"height", 40.0},
                {"pins", nlohmann::json::array({
                    {{"id", "c1_p1"}, {"number", "1"}, {"name", "1"}, {"net", "+3V3"}, {"x", 250.0}, {"y", 180.0}},
                    {{"id", "c1_p2"}, {"number", "2"}, {"name", "2"}, {"net", "GND"}, {"x", 250.0}, {"y", 220.0}}
                })}
            },
            {
                {"id", "u2_id"},
                {"reference", "U2"},
                {"value", "INA226"},
                {"component_type", "part"},
                {"footprint", "MSOP-10"},
                {"unique_id", "uid_u2"},
                {"x", 500.0},
                {"y", 300.0},
                {"width", 80.0},
                {"height", 60.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u2_p1"}, {"number", "1"}, {"name", "VS"}, {"net", "+5V"}, {"x", 460.0}, {"y", 280.0}},
                    {{"id", "u2_p2"}, {"number", "2"}, {"name", "GND"}, {"net", "GND"}, {"x", 460.0}, {"y", 320.0}},
                    {{"id", "u2_p3"}, {"number", "3"}, {"name", "SDA"}, {"net", "I2C_SDA"}, {"x", 540.0}, {"y", 280.0}},
                    {{"id", "u2_p4"}, {"number", "4"}, {"name", "SCL"}, {"net", "I2C_SCL"}, {"x", 540.0}, {"y", 300.0}},
                    {{"id", "u2_p5"}, {"number", "5"}, {"name", "RST"}, {"net", "MCU_nRST"}, {"x", 540.0}, {"y", 320.0}}
                })}
            },
            {
                {"id", "r2_overlap_id"},
                {"reference", "R2"},
                {"value", "10k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", "uid_r2"},
                {"x", 510.0},
                {"y", 300.0},
                {"width", 60.0},
                {"height", 20.0},
                {"rotation", 90.0},
                {"pins", nlohmann::json::array({
                    {{"id", "r2_p1"}, {"number", "1"}, {"name", "1"}, {"net", "+3V3"}, {"x", 510.0}, {"y", 270.0}},
                    {{"id", "r2_p2"}, {"number", "2"}, {"name", "2"}, {"net", "GND"}, {"x", 510.0}, {"y", 330.0}}
                })}
            },
            {
                {"id", "nf_5v_short"},
                {"reference", "#PWR_5V"},
                {"value", "+5V"},
                {"component_type", "netflag"},
                {"x", 700.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array()}
            },
            {
                {"id", "nf_3v3_short"},
                {"reference", "#PWR_3V3"},
                {"value", "+3V3"},
                {"component_type", "netflag"},
                {"x", 800.0},
                {"y", 100.0},
                {"pins", nlohmann::json::array()}
            }
        })},
        {"wires", nlohmann::json::array({
            {{"id", "w_vshort"}, {"net", ""}, {"line", nlohmann::json::array({{700.0, 100.0}, {800.0, 100.0}})}}
        })}
    };

    nlohmann::json semRes = acc.calcSchematicErcAudit(semFaultArgs, ok, summary);
    CHECK_TRUE(ok);
    CHECK_FALSE(semRes.value("passed", true));

    // Layer 2a: single_pin_nets catches SPI_MISO_ORPHAN on U1.4
    CHECK_EQUAL(1, semRes.value("single_pin_nets_count", 0));
    CHECK_EQUAL(1, static_cast<int>(semRes["single_pin_nets"].size()));
    CHECK_EQUAL("SPI_MISO_ORPHAN", semRes["single_pin_nets"][0].value("net", ""));
    CHECK_EQUAL("U1", semRes["single_pin_nets"][0]["pin"].value("reference", ""));
    CHECK_EQUAL("4", semRes["single_pin_nets"][0]["pin"].value("pin_number", ""));

    // Layer 2b: overlapping_components catches U2 and R2 collision
    CHECK_EQUAL(1, semRes.value("overlapping_components_count", 0));
    CHECK_EQUAL(1, static_cast<int>(semRes["overlapping_components"].size()));
    CHECK_EQUAL("U2", semRes["overlapping_components"][0].value("reference_a", ""));
    CHECK_EQUAL("R2", semRes["overlapping_components"][0].value("reference_b", ""));
    CHECK_TRUE(semRes["overlapping_components"][0].contains("bbox_a"));
    CHECK_TRUE(semRes["overlapping_components"][0].contains("bbox_b"));

    // Layer 3: pullup_issues catches I2C_SCL and MCU_nRST (while I2C_SDA has R1 pull-up)
    CHECK_EQUAL(2, semRes.value("pullup_issues_count", 0));
    bool sawSclPullup = false;
    bool sawNrstPullup = false;
    for (const auto& pi : semRes["pullup_issues"]) {
        if (pi.value("net", "") == "I2C_SCL") sawSclPullup = true;
        if (pi.value("net", "") == "MCU_nRST") sawNrstPullup = true;
    }
    CHECK_TRUE(sawSclPullup);
    CHECK_TRUE(sawNrstPullup);

    // Layer 4: decoupling_issues catches U2 on +5V (while U1 on +3V3 is decoupled by C1)
    CHECK_EQUAL(1, semRes.value("decoupling_issues_count", 0));
    CHECK_EQUAL("U2", semRes["decoupling_issues"][0].value("reference", ""));
    CHECK_EQUAL("+5V", semRes["decoupling_issues"][0].value("supply_net", ""));

    // Layer 5: voltage_domain_issues catches +5V shorted to +3V3
    CHECK_TRUE(semRes.value("voltage_domain_issues_count", 0) >= 1);
}

TEST(EdaAccelerators_Unit, EasyEdaEditSchematicConnectToNetAutoPlaceAndAnnotate) {
    EdaBridgeConfig cfg;
    cfg.enabled = true;
    cfg.mode = "gateway";
    cfg.easyeda.enabled = true;
    EdaBridge bridge(cfg);

    // Test next-gen schematic edit operations:
    // 1) near_reference / auto_place places C? and R? near U1 on a collision-free 50-mil grid coordinate
    // 2) annotate_schematic deterministically assigns R1, R2 and C1 to unannotated/duplicate parts and verifies readback
    // 3) connect_to_net creates outward 50-mil snapped wire stubs (avoiding U1's body and other pins) + netflags/labels
    nlohmann::json schState = {
        {"active_document", "MainSheet"},
        {"components", nlohmann::json::array({
            {
                {"id", "comp_u1"},
                {"reference", "U1"},
                {"value", "ESP32-S3"},
                {"component_type", "part"},
                {"footprint", "QFN-56"},
                {"unique_id", "uid_u1"},
                {"x", 500.0},
                {"y", 500.0},
                {"width", 100.0},
                {"height", 100.0},
                {"pins", nlohmann::json::array({
                    {{"id", "u1_p1"}, {"number", "1"}, {"name", "VDD"}, {"net", ""}, {"x", 450.0}, {"y", 450.0}},
                    {{"id", "u1_p2"}, {"number", "2"}, {"name", "GND"}, {"net", ""}, {"x", 450.0}, {"y", 550.0}},
                    {{"id", "u1_p3"}, {"number", "3"}, {"name", "SDA"}, {"net", ""}, {"x", 550.0}, {"y", 450.0}},
                    {{"id", "u1_p4"}, {"number", "4"}, {"name", "SCL"}, {"net", ""}, {"x", 550.0}, {"y", 550.0}}
                })}
            },
            {
                {"id", "comp_r_dup"},
                {"reference", "R?"},
                {"value", "4.7k"},
                {"component_type", "part"},
                {"footprint", "R0603"},
                {"unique_id", ""},
                {"x", 800.0},
                {"y", 400.0},
                {"width", 40.0},
                {"height", 20.0},
                {"pins", nlohmann::json::array({
                    {{"id", "rdup_1"}, {"number", "1"}, {"name", "1"}, {"net", ""}, {"x", 780.0}, {"y", 400.0}},
                    {{"id", "rdup_2"}, {"number", "2"}, {"name", "2"}, {"net", ""}, {"x", 820.0}, {"y", 400.0}}
                })}
            }
        })},
        {"wires", nlohmann::json::array()}
    };

    nlohmann::json editRes = bridge.callTool("easyeda_edit_schematic", {
        {"schematic_state", schState},
        {"add_components", nlohmann::json::array({
            {
                {"reference", "C?"},
                {"value", "100nF"},
                {"footprint", "C0603"},
                {"near_reference", "U1"},
                {"width", 40.0},
                {"height", 20.0}
            },
            {
                {"reference", "R?"},
                {"value", "4.7k"},
                {"footprint", "R0603"},
                {"auto_place", true},
                {"width", 40.0},
                {"height", 20.0}
            }
        })},
        {"annotate_schematic", true},
        {"connect_to_net", nlohmann::json::array({
            {{"reference", "U1"}, {"pin_number", "1"}, {"net", "+3V3"}, {"stub_length_mils", 100.0}, {"use_netflag", true}},
            {{"reference", "U1"}, {"pin_number", "2"}, {"net", "GND"}, {"stub_length_mils", 100.0}},
            {{"reference", "U1"}, {"pin_number", "3"}, {"net", "I2C_SDA"}, {"stub_length_mils", 100.0}},
            {{"reference", "U1"}, {"pin_number", "4"}, {"net", "I2C_SCL"}, {"stub_length_mils", 100.0}}
        })}
    });

    CHECK_TRUE(editRes.value("ok", false));
    CHECK_EQUAL(2, editRes.value("placed_components", 0));
    CHECK_TRUE(editRes.contains("placed_component_details"));
    for (const auto& pd : editRes["placed_component_details"]) {
        double px = pd.value("x", -1.0);
        double py = pd.value("y", -1.0);
        CHECK_TRUE(std::fabs(px / 50.0 - std::round(px / 50.0)) < 1e-4);
        CHECK_TRUE(std::fabs(py / 50.0 - std::round(py / 50.0)) < 1e-4);
    }

    // Verify annotate_schematic assigned R1, R2, C1 and verified readback
    CHECK_TRUE(editRes.value("annotated_count", 0) >= 3);
    CHECK_TRUE(editRes.value("annotation_verified", false));

    // Verify connect_to_net generated 4 outward 100-mil stubs without crossing U1 body [450..550, 450..550]
    CHECK_TRUE(editRes.contains("connected_net_stubs"));
    CHECK_EQUAL(4, static_cast<int>(editRes["connected_net_stubs"].size()));
    for (const auto& stub : editRes["connected_net_stubs"]) {
        double x1 = stub.value("x1", 0.0);
        double y1 = stub.value("y1", 0.0);
        double x2 = stub.value("x2", 0.0);
        double y2 = stub.value("y2", 0.0);
        DOUBLES_EQUAL(100.0, std::hypot(x2 - x1, y2 - y1), 1e-3);
        CHECK_TRUE(std::fabs(x2 / 50.0 - std::round(x2 / 50.0)) < 1e-4);
        CHECK_TRUE(std::fabs(y2 / 50.0 - std::round(y2 / 50.0)) < 1e-4);
        // Outward stub tip (x2, y2) must lie strictly outside U1's [450..550, 450..550] box
        bool outsideBody = (x2 < 450.0 || x2 > 550.0 || y2 < 450.0 || y2 > 550.0);
        CHECK_TRUE(outsideBody);
    }
}

TEST(EdaAccelerators_Unit, EdaBridge_PreSyncDiffAndConnectedMove) {
    EdaBridgeConfig cfg;
    cfg.enabled = true;
    cfg.mode = "gateway";
    cfg.easyeda.enabled = true;
    EdaBridge bridge(cfg);

    // 1. Pre-Sync Netlist Diff (dry_run: true) on easyeda_sync_sch_to_pcb
    nlohmann::json schState = {
        {"active_document", "Sheet_Main"},
        {"components", nlohmann::json::array({
            {
                {"id", "u1"},
                {"reference", "U1"},
                {"value", "STM32F103"},
                {"footprint", "LQFP-48"},
                {"x", 500.0},
                {"y", 500.0},
                {"pins", nlohmann::json::array({
                    {{"number", "1"}, {"name", "VDD"}, {"net", "+3V3"}, {"x", 450.0}, {"y", 500.0}},
                    {{"number", "2"}, {"name", "VSS"}, {"net", "GND"}, {"x", 550.0}, {"y", 500.0}}
                })}
            },
            {
                {"id", "r1"},
                {"reference", "R1"},
                {"value", "10k"},
                {"footprint", "R0805"},
                {"x", 700.0},
                {"y", 500.0},
                {"pins", nlohmann::json::array({
                    {{"number", "1"}, {"name", "1"}, {"net", "+3V3"}, {"x", 680.0}, {"y", 500.0}},
                    {{"number", "2"}, {"name", "2"}, {"net", "I2C_SDA"}, {"x", 720.0}, {"y", 500.0}}
                })}
            },
            {
                {"id", "c1"},
                {"reference", "C1"},
                {"value", "100nF"},
                {"footprint", "C0402"},
                {"x", 800.0},
                {"y", 500.0},
                {"pins", nlohmann::json::array({
                    {{"number", "1"}, {"name", "1"}, {"net", "+3V3"}, {"x", 780.0}, {"y", 500.0}},
                    {{"number", "2"}, {"name", "2"}, {"net", "GND"}, {"x", 820.0}, {"y", 500.0}}
                })}
            },
            {
                {"id", "r_unann"},
                {"reference", "R?"},
                {"value", "4.7k"},
                {"footprint", "R0603"},
                {"x", 900.0},
                {"y", 500.0}
            },
            {
                {"id", "u2_nofp"},
                {"reference", "U2"},
                {"value", "AMS1117"},
                {"footprint", ""},
                {"x", 1000.0},
                {"y", 500.0}
            }
        })},
        {"wires", nlohmann::json::array({
            {{"id", "w1"}, {"x1", 300.0}, {"y1", 500.0}, {"x2", 450.0}, {"y2", 500.0}, {"net", "+3V3"}},
            {{"id", "w2"}, {"x1", 550.0}, {"y1", 500.0}, {"x2", 650.0}, {"y2", 500.0}, {"net", "GND"}}
        })}
    };

    nlohmann::json pcbState = {
        {"components", nlohmann::json::array({
            {
                {"reference", "U1"},
                {"footprint", "LQFP-48"},
                {"x_mm", 20.0},
                {"y_mm", 20.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "+3V3"}, {"x_mm", 18.0}, {"y_mm", 20.0}},
                    {{"number", "2"}, {"net", "GND"}, {"x_mm", 22.0}, {"y_mm", 20.0}}
                })}
            },
            {
                {"reference", "R1"},
                {"footprint", "R0603"},
                {"x_mm", 10.0},
                {"y_mm", 10.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "+3V3"}, {"x_mm", 9.2}, {"y_mm", 10.0}},
                    {{"number", "2"}, {"net", "OLD_NET"}, {"x_mm", 10.8}, {"y_mm", 10.0}}
                })}
            },
            {
                {"reference", "R99"},
                {"footprint", "R0603"},
                {"x_mm", 30.0},
                {"y_mm", 10.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "OLD_NET"}, {"x_mm", 29.2}, {"y_mm", 10.0}},
                    {{"number", "2"}, {"net", "GND"}, {"x_mm", 30.8}, {"y_mm", 10.0}}
                })}
            }
        })},
        {"tracks", nlohmann::json::array({
            {{"id", "trk1"}, {"x1", 5.0}, {"y1", 10.0}, {"x2", 9.2}, {"y2", 10.0}, {"layer", "F.Cu"}, {"net", "+3V3"}},
            {{"id", "trk2"}, {"x1", 10.8}, {"y1", 10.0}, {"x2", 29.2}, {"y2", 10.0}, {"layer", "F.Cu"}, {"net", "OLD_NET"}}
        })}
    };

    nlohmann::json diffRes = bridge.callTool("easyeda_sync_sch_to_pcb", {
        {"dry_run", true},
        {"schematic_state", schState},
        {"pcb_state", pcbState}
    });
    CHECK_TRUE(diffRes.value("ok", false));
    CHECK_TRUE(diffRes.value("dry_run", false));
    CHECK_FALSE(diffRes.value("synced", true));
    CHECK_FALSE(diffRes.value("ready_to_sync", true));
    auto toAdd = diffRes.value("components_to_add", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(toAdd.size()));
    if (!toAdd.empty()) {
        CHECK_EQUAL("C1", toAdd[0].value("reference", ""));
    }
    auto toRemove = diffRes.value("components_to_remove", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(toRemove.size()));
    if (!toRemove.empty()) {
        CHECK_EQUAL("R99", toRemove[0].value("reference", ""));
    }
    auto fpMis = diffRes.value("footprint_mismatches", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(fpMis.size()));
    if (!fpMis.empty()) {
        CHECK_EQUAL("R1", fpMis[0].value("reference", ""));
        CHECK_EQUAL("R0805", fpMis[0].value("schematic_footprint", ""));
        CHECK_EQUAL("R0603", fpMis[0].value("pcb_footprint", ""));
    }
    auto unann = diffRes.value("unannotated_references", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(unann.size()));
    if (!unann.empty()) {
        CHECK_EQUAL("R?", unann[0].get<std::string>());
    }
    auto missFp = diffRes.value("missing_footprints", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(missFp.size()));
    if (!missFp.empty()) {
        CHECK_EQUAL("U2", missFp[0].get<std::string>());
    }
    auto padDeltas = diffRes.value("pad_net_deltas", nlohmann::json::array());
    CHECK_TRUE(padDeltas.size() >= 1);
    auto orphanTrk = diffRes.value("orphaned_tracks", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(orphanTrk.size()));
    if (!orphanTrk.empty()) {
        CHECK_EQUAL("OLD_NET", orphanTrk[0].value("net", ""));
    }

    // 2. Connected Move (preserve_connections: true) on easyeda_edit_schematic
    nlohmann::json schMoveRes = bridge.callTool("easyeda_edit_schematic", {
        {"schematic_state", schState},
        {"preserve_connections", true},
        {"modify_components", nlohmann::json::array({
            {{"reference", "U1"}, {"x", 600.0}, {"y", 550.0}, {"preserve_connections", true}}
        })}
    });
    CHECK_TRUE(schMoveRes.value("ok", false));
    CHECK_EQUAL(2, schMoveRes.value("rubber_banded_wires_count", 0));
    auto rbWires = schMoveRes.value("rubber_banded_wires", nlohmann::json::array());
    CHECK_EQUAL(2, static_cast<int>(rbWires.size()));
    if (rbWires.size() >= 2) {
        // U1 moved by (+100, +50), so pin 1 moved from (450, 500) to (550, 550) and pin 2 moved from (550, 500) to (650, 550)
        DOUBLES_EQUAL(550.0, rbWires[0].value("x2", 0.0), 1e-3);
        DOUBLES_EQUAL(550.0, rbWires[0].value("y2", 0.0), 1e-3);
        DOUBLES_EQUAL(650.0, rbWires[1].value("x1", 0.0), 1e-3);
        DOUBLES_EQUAL(550.0, rbWires[1].value("y1", 0.0), 1e-3);
    }

    // 3. Connected Move (preserve_connections: true) on easyeda_edit_pcb
    nlohmann::json pcbMoveRes = bridge.callTool("easyeda_edit_pcb", {
        {"pcb_state", pcbState},
        {"preserve_connections", true},
        {"move_components", nlohmann::json::array({
            {{"reference", "R1"}, {"x_mm", 15.0}, {"y_mm", 12.0}, {"preserve_connections", true}}
        })}
    });
    CHECK_TRUE(pcbMoveRes.value("ok", false));
    CHECK_EQUAL(2, pcbMoveRes.value("rubber_banded_tracks_count", 0));
    auto rbTracks = pcbMoveRes.value("rubber_banded_tracks", nlohmann::json::array());
    CHECK_EQUAL(2, static_cast<int>(rbTracks.size()));
    if (rbTracks.size() >= 2) {
        // R1 moved by (+5.0, +2.0) mm: pad 1 moved from (9.2, 10.0) -> (14.2, 12.0), pad 2 moved from (10.8, 10.0) -> (15.8, 12.0)
        DOUBLES_EQUAL(14.2, rbTracks[0].value("x2", 0.0), 1e-3);
        DOUBLES_EQUAL(12.0, rbTracks[0].value("y2", 0.0), 1e-3);
        DOUBLES_EQUAL(15.8, rbTracks[1].value("x1", 0.0), 1e-3);
        DOUBLES_EQUAL(12.0, rbTracks[1].value("y1", 0.0), 1e-3);
    }
}

TEST(EdaAccelerators_Unit, EdaAccelerators_RipUpAutorouteAndDecouplingAudit) {
    EdaAccelerators accel;
    bool ok = false;
    std::string err;

    // 1. T-corridor ordering trap on single layer (layers=1):
    // NET_B pad 1 at (5.0, 2.0) is inside a U-shaped keepout pocket whose ONLY exit is north through (5.0, 2.8).
    // NET_A is listed first in nets[] with pads at (3.0, 2.8) and (7.0, 2.8).
    // A naive single-pass router routes NET_A straight across y=2.8, blocking NET_B's only escape!
    // Multi-pass rip-up-and-reroute must rip up NET_A, route NET_B out of the pocket to (5.0, 4.0),
    // and reroute NET_A around the pocket so both nets route with 0 DRC violations.
    nlohmann::json routeArgs = {
        {"board", {{"width_mm", 10.0}, {"height_mm", 6.0}, {"layers", 1}}},
        {"rules", {{"track_width_mm", 0.2}, {"clearance_mm", 0.15}, {"grid_step_mm", 0.2}, {"max_ripup_passes", 6}}},
        {"keepouts", nlohmann::json::array({
            {{"x_mm", 4.0}, {"y_mm", 1.2}, {"w_mm", 0.4}, {"h_mm", 1.6}, {"layer", 0}},
            {{"x_mm", 4.0}, {"y_mm", 1.2}, {"w_mm", 2.0}, {"h_mm", 0.4}, {"layer", 0}},
            {{"x_mm", 5.6}, {"y_mm", 1.2}, {"w_mm", 0.4}, {"h_mm", 1.6}, {"layer", 0}}
        })},
        {"all_pads", nlohmann::json::array({
            {{"x_mm", 5.0}, {"y_mm", 5.2}, {"w_mm", 1.2}, {"h_mm", 0.6}, {"rotation_deg", 90.0}, {"layer", 0}, {"net", "FOREIGN_NET"}}
        })},
        {"nets", nlohmann::json::array({
            {
                {"name", "NET_A"},
                {"pads", nlohmann::json::array({
                    {{"x_mm", 3.0}, {"y_mm", 2.8}, {"w_mm", 0.4}, {"h_mm", 0.4}, {"layer", "F.Cu"}},
                    {{"x_mm", 7.0}, {"y_mm", 2.8}, {"w_mm", 0.4}, {"h_mm", 0.4}, {"layer", "F.Cu"}}
                })}
            },
            {
                {"name", "NET_B"},
                {"pads", nlohmann::json::array({
                    {{"x_mm", 5.0}, {"y_mm", 2.0}, {"w_mm", 0.4}, {"h_mm", 0.4}, {"layer", "F.Cu"}},
                    {{"x_mm", 5.0}, {"y_mm", 4.0}, {"w_mm", 0.4}, {"h_mm", 0.4}, {"layer", "F.Cu"}}
                })}
            }
        })}
    };

    nlohmann::json routeRes = accel.calcPcbAutoroute(routeArgs, ok, err);
    CHECK_TRUE(ok);
    CHECK_EQUAL(2, routeRes.value("total_nets", 0));
    CHECK_EQUAL(2, routeRes.value("routed_nets", 0));
    CHECK_EQUAL(0, routeRes.value("unrouted_nets", -1));
    CHECK_EQUAL(0, routeRes.value("drc_violations", -1));
    CHECK_TRUE(routeRes.value("ripup_passes_used", 0) >= 1);
    CHECK_TRUE(routeRes.value("rerouted_nets_count", 0) >= 1);

    // 2. Decoupling capacitor distance audit (> 5.0 mm warning)
    nlohmann::json auditArgs = {
        {"max_decoupling_distance_mm", 5.0},
        {"footprints", nlohmann::json::array({
            {
                {"reference", "U1"},
                {"x_mm", 10.0},
                {"y_mm", 10.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "+3V3"}, {"x_mm", 10.0}, {"y_mm", 10.0}},
                    {{"number", "2"}, {"net", "GND"}, {"x_mm", 11.0}, {"y_mm", 10.0}}
                })}
            },
            {
                {"reference", "C1"},
                {"x_mm", 22.0},
                {"y_mm", 10.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "+3V3"}, {"x_mm", 22.0}, {"y_mm", 10.0}},
                    {{"number", "2"}, {"net", "GND"}, {"x_mm", 23.0}, {"y_mm", 10.0}}
                })}
            },
            {
                {"reference", "U2"},
                {"x_mm", 35.0},
                {"y_mm", 10.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "VDD_1V8"}, {"x_mm", 35.0}, {"y_mm", 10.0}},
                    {{"number", "2"}, {"net", "GND"}, {"x_mm", 36.0}, {"y_mm", 10.0}}
                })}
            },
            {
                {"reference", "C2"},
                {"x_mm", 37.0},
                {"y_mm", 10.0},
                {"pads", nlohmann::json::array({
                    {{"number", "1"}, {"net", "VDD_1V8"}, {"x_mm", 37.0}, {"y_mm", 10.0}},
                    {{"number", "2"}, {"net", "GND"}, {"x_mm", 38.0}, {"y_mm", 10.0}}
                })}
            }
        })},
        {"tracks", nlohmann::json::array({
            {{"x1", 10.0}, {"y1", 10.0}, {"x2", 22.0}, {"y2", 10.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "+3V3"}},
            {{"x1", 11.0}, {"y1", 10.0}, {"x2", 23.0}, {"y2", 10.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "GND"}},
            {{"x1", 23.0}, {"y1", 10.0}, {"x2", 36.0}, {"y2", 10.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "GND"}},
            {{"x1", 36.0}, {"y1", 10.0}, {"x2", 38.0}, {"y2", 10.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "GND"}},
            {{"x1", 35.0}, {"y1", 10.0}, {"x2", 37.0}, {"y2", 10.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "VDD_1V8"}}
        })}
    };

    nlohmann::json auditRes = accel.calcPcbConnectivityAudit(auditArgs, ok, err);
    CHECK_TRUE(ok);
    CHECK_TRUE(auditRes.value("ok", false));
    CHECK_EQUAL(1, auditRes.value("decoupling_distance_warnings_count", 0));
    auto decWarns = auditRes.value("decoupling_distance_warnings", nlohmann::json::array());
    CHECK_EQUAL(1, static_cast<int>(decWarns.size()));
    if (!decWarns.empty()) {
        CHECK_EQUAL("U1", decWarns[0].value("ic_reference", ""));
        CHECK_EQUAL("C1", decWarns[0].value("cap_reference", ""));
        CHECK_EQUAL("+3V3", decWarns[0].value("net", ""));
        DOUBLES_EQUAL(12.0, decWarns[0].value("distance_mm", 0.0), 1e-2);
    }
}


int main(int ac, char** av) {
    for (int i = 1; i < ac; ++i) {
        if (std::strcmp(av[i], "--negative-control") == 0) {
            EdaBridgeConfig cfg;
            cfg.linuxPathPrefix = "~/work";
            cfg.windowsPathPrefix = "Z:\\";
            EdaBridge bridge(cfg);
            std::string winPath;
            std::string err;
            bool allowed = bridge.translateLinuxToWindows("~/work/../../etc/passwd", winPath, err);
            if (!allowed) {
                std::cerr << "[NegativeControl] Path traversal correctly rejected: " << err << std::endl;
                return 1;
            }
            return 0;
        }
    }
    return CommandLineTestRunner::RunAllTests(ac, av);
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
