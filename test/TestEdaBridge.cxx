/*
 * TestEdaBridge.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

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
