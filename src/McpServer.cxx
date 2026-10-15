/*
 * McpServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "McpServer.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "TaskRegistry.hxx"
#include "InsightEngine.hxx"
#include "EdaBridge.hxx"
#include "EdaAccelerators.hxx"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace aimon {

namespace {

struct EdaToolDescriptor {
    const char* name;
    const char* component; // "accelerators" | "kicad" | "freecad" | "easyeda"
    const char* description;
    nlohmann::json properties;
    std::vector<std::string> required;
};

std::vector<EdaToolDescriptor> getEdaToolDescriptors() {
    return {
        // 1..7: Local CPU Algorithmic Accelerators
        {
            "eda_calc_schematic_layout",
            "accelerators",
            "Sugiyama layered schematic placement + 50-mil (1.27 mm) A* orthogonal wire router (outputs symbols[] & wires[]).",
            {
                {"components", {{"type", "array"}, {"description", "List of schematic components ({ref, lib_id, value, pins[]})"}}},
                {"nets", {{"type", "array"}, {"description", "List of net connectivity definitions ({name, pins[]})"}}},
                {"grid_mils", {{"type", "integer"}, {"description", "Grid spacing in mils (default: 50)"}}}
            },
            {}
        },
        {
            "eda_calc_pcb_placement",
            "accelerators",
            "Simulated annealing + HPWL force-directed non-overlapping PCB floorplanner (outputs placements[] with zero courtyard overlap).",
            {
                {"board", {{"type", "object"}, {"description", "Board dimensions ({width_mm, height_mm, clearance_mm})"}}},
                {"components", {{"type", "array"}, {"description", "Footprints to place ({ref, width_mm, height_mm, fixed, x_mm, y_mm})"}}},
                {"nets", {{"type", "array"}, {"description", "Netlist connections for HPWL wirelength minimization"}}}
            },
            {}
        },
        {
            "eda_calc_pcb_autoroute",
            "accelerators",
            "PathFinder negotiated-congestion multi-layer 45-degree A* PCB trace & via router (outputs tracks[] & vias[]).",
            {
                {"board", {{"type", "object"}, {"description", "Board outline and routing grid settings"}}},
                {"nets", {{"type", "array"}, {"description", "Nets with pad coordinates ({name, width_mm, clearance_mm, pads[]})"}}},
                {"obstacles", {{"type", "array"}, {"description", "Keepout or obstacle rectangles"}}}
            },
            {}
        },
        {
            "eda_calc_electrical_synth",
            "accelerators",
            "E24/E96/JLCPCB-Basic divider/filter solver, IPC-2152 trace/via sizing, microstrip impedance & power/thermal budget.",
            {
                {"mode", {{"type", "string"}, {"description", "Solver mode: voltage_divider, rc_filter, trace_ipc2152, microstrip_z0, or power_budget"}}},
                {"vin", {{"type", "number"}, {"description", "Input voltage (V)"}}},
                {"vout_target", {{"type", "number"}, {"description", "Target output voltage (V)"}}},
                {"cutoff_hz", {{"type", "number"}, {"description", "Target filter cutoff frequency (Hz)"}}},
                {"current_a", {{"type", "number"}, {"description", "Trace current in Amperes (A)"}}}
            },
            {}
        },
        {
            "cad_calc_robot_kinematics_statics",
            "accelerators",
            "DH/URDF forward/inverse kinematics, gravity/payload joint torque budget, servo safety factor & CoM support-polygon margin.",
            {
                {"joints", {{"type", "array"}, {"description", "Kinematic chain joint definitions ({name, a_mm, d_mm, alpha_deg, theta_deg, mass_kg, stall_torque_nm})"}}},
                {"payload_kg", {{"type", "number"}, {"description", "End-effector payload mass (kg)"}}},
                {"target_xyz_mm", {{"type", "array"}, {"description", "Optional IK target [x, y, z] in mm"}}},
                {"support_polygon_mm", {{"type", "array"}, {"description", "Support polygon 2D vertices [[x, y], ...]"}}}
            },
            {}
        },
        {
            "cad_calc_mechanism_synth",
            "accelerators",
            "Analytical involute gears, planetary sets (Zs + 2*Zp = Zr), GT2/HTD timing pulleys, 25T/24T servo splines & Grashof 4-bar linkages.",
            {
                {"mechanism_type", {{"type", "string"}, {"description", "Mechanism type: spur_gear, planetary, timing_pulley, servo_horn_spline, or four_bar"}}},
                {"module_mm", {{"type", "number"}, {"description", "Gear module in mm"}}},
                {"teeth_pinion", {{"type", "integer"}, {"description", "Pinion or sun gear tooth count"}}},
                {"teeth_gear", {{"type", "integer"}, {"description", "Driven gear or planet tooth count"}}},
                {"links_mm", {{"type", "array"}, {"description", "Four-bar link lengths [ground, crank, coupler, rocker] in mm"}}}
            },
            {}
        },
        {
            "cad_calc_print_orientation_dfm",
            "accelerators",
            "Spherical FDM build-orientation optimizer + anisotropic Z-layer tensile/bending stress, buckling & overhang analyzer.",
            {
                {"bbox_mm", {{"type", "array"}, {"description", "Part bounding box [x_mm, y_mm, z_mm]"}}},
                {"load_vector_n", {{"type", "array"}, {"description", "Principal mechanical load vector [fx, fy, fz] in Newtons"}}},
                {"cross_section_mm2", {{"type", "number"}, {"description", "Critical cross-sectional area in mm^2"}}},
                {"material", {{"type", "string"}, {"description", "Filament material: PLA, PETG, ABS, ASA, or PA-CF"}}}
            },
            {}
        },
        // 8..16: KiCad Tools (frog)
        {
            "kicad_search_library",
            "kicad",
            "Search KiCad .kicad_sym symbol libraries and .kicad_mod footprint libraries (returns lib_id, pin offsets, pad sizes).",
            {
                {"query", {{"type", "string"}, {"description", "Symbol or footprint search substring"}}},
                {"symbol_lib_dir", {{"type", "string"}, {"description", "Optional symbol library directory path"}}},
                {"footprint_lib_dir", {{"type", "string"}, {"description", "Optional footprint library directory path"}}}
            },
            {"query"}
        },
        {
            "kicad_inspect_project",
            "kicad",
            "Inspect .kicad_sch / .kicad_pcb components, pins, nets, board outline, footprints, and unrouted nets.",
            {
                {"schematic_path", {{"type", "string"}, {"description", "Path to .kicad_sch file"}}},
                {"pcb_path", {{"type", "string"}, {"description", "Path to .kicad_pcb file"}}}
            },
            {}
        },
        {
            "kicad_edit_schematic",
            "kicad",
            "Create or modify .kicad_sch symbols, power ports, net labels, and 50-mil grid-snapped orthogonal wires + auto-ERC.",
            {
                {"schematic_path", {{"type", "string"}, {"description", "Target .kicad_sch path"}}},
                {"operations", {{"type", "array"}, {"description", "List of schematic edit operations (add_component, add_wire, add_label, add_power_port)"}}},
                {"symbols", {{"type", "array"}, {"description", "Optional symbol placements directly from eda_calc_schematic_layout"}}},
                {"wires", {{"type", "array"}, {"description", "Optional wire segments directly from eda_calc_schematic_layout"}}}
            },
            {"schematic_path"}
        },
        {
            "kicad_sync_sch_to_pcb",
            "kicad",
            "Synchronize schematic netlist and .kicad_mod footprints from .kicad_sch into .kicad_pcb.",
            {
                {"schematic_path", {{"type", "string"}, {"description", "Source .kicad_sch file path"}}},
                {"pcb_path", {{"type", "string"}, {"description", "Target .kicad_pcb file path"}}}
            },
            {"schematic_path", "pcb_path"}
        },
        {
            "kicad_edit_pcb",
            "kicad",
            "Draw Edge.Cuts outline, place/rotate/flip footprints, add copper pour zones, and route tracks/vias in .kicad_pcb.",
            {
                {"pcb_path", {{"type", "string"}, {"description", "Target .kicad_pcb file path"}}},
                {"outline", {{"type", "object"}, {"description", "Board outline specification ({x_mm, y_mm, width_mm, height_mm})"}}},
                {"placements", {{"type", "array"}, {"description", "Component placements ({ref, x_mm, y_mm, rotation_deg, layer})"}}},
                {"tracks", {{"type", "array"}, {"description", "PCB trace segments ({net, layer, width_mm, x1_mm, y1_mm, x2_mm, y2_mm})"}}},
                {"vias", {{"type", "array"}, {"description", "PCB vias ({net, x_mm, y_mm, size_mm, drill_mm})"}}},
                {"zones", {{"type", "array"}, {"description", "Copper pour zones ({net, layer, x_mm, y_mm, width_mm, height_mm})"}}}
            },
            {"pcb_path"}
        },
        {
            "kicad_run_erc",
            "kicad",
            "Run KiCad Schematic Electrical Rules Check (kicad-cli sch erc) and return structured JSON violations.",
            {
                {"schematic_path", {{"type", "string"}, {"description", "Path to .kicad_sch file"}}}
            },
            {"schematic_path"}
        },
        {
            "kicad_run_drc",
            "kicad",
            "Run KiCad PCB Design Rules Check (kicad-cli pcb drc) with optional --schematic-parity.",
            {
                {"pcb_path", {{"type", "string"}, {"description", "Path to .kicad_pcb file"}}},
                {"schematic_parity", {{"type", "boolean"}, {"description", "Check schematic-to-PCB parity (default: true)"}}}
            },
            {"pcb_path"}
        },
        {
            "kicad_export_fab_pack",
            "kicad",
            "Export turnkey Gerbers, Excellon drill files, pick-and-place .pos/CPL, and BOM CSV.",
            {
                {"pcb_path", {{"type", "string"}, {"description", "Path to .kicad_pcb file"}}},
                {"schematic_path", {{"type", "string"}, {"description", "Optional path to .kicad_sch file for BOM export"}}},
                {"output_dir", {{"type", "string"}, {"description", "Output directory for fabrication package"}}}
            },
            {"pcb_path", "output_dir"}
        },
        {
            "kicad_export_step",
            "kicad",
            "Export 3D .step PCB assembly model (can be imported directly into FreeCAD robot enclosures).",
            {
                {"pcb_path", {{"type", "string"}, {"description", "Path to .kicad_pcb file"}}},
                {"output_path", {{"type", "string"}, {"description", "Target .step file path"}}}
            },
            {"pcb_path", "output_path"}
        },
        // 17..25: FreeCAD Robotics & 3D-Printing Tools (frog)
        {
            "freecad_live_status",
            "freecad",
            "Check live FreeCAD GUI attachment on AppPluginRelay (:3893), active .FCStd document, and available workbenches.",
            nlohmann::json::object(),
            {}
        },
        {
            "freecad_inspect_document",
            "freecad",
            "Inspect FreeCAD bodies, Spreadsheet parameters, bounding boxes, CoM, inertia tensor, hole axes, and LCS frames.",
            {
                {"document_path", {{"type", "string"}, {"description", "Optional path to .FCStd document"}}}
            },
            {}
        },
        {
            "freecad_generate_cad",
            "freecad",
            "Author or modify parametric 3D robot bodies, joints, brackets, and gears in live FreeCAD GUI or .FCStd/.step (guarded by TNP & sandbox checks).",
            {
                {"document_path", {{"type", "string"}, {"description", "Target .FCStd or .step path"}}},
                {"script", {{"type", "string"}, {"description", "CSG / Part / PartDesign Python script (must not reference Face1..N, Edge1..N, Vertex1..N)"}}},
                {"primitives", {{"type", "array"}, {"description", "Declarative 3D primitive/feature list"}}}
            },
            {}
        },
        {
            "freecad_edit_parameters",
            "freecad",
            "Tune parametric Spreadsheet dimensions, joint clearances, and 3D-print tolerances and recompute the model.",
            {
                {"document_path", {{"type", "string"}, {"description", "Path to .FCStd document"}}},
                {"parameters", {{"type", "object"}, {"description", "Key-value map of parameter names to updated values"}}}
            },
            {"parameters"}
        },
        {
            "freecad_assemble_joint",
            "freecad",
            "Assemble robot links/PCBs along joint axes and sweep [min_deg..max_deg] for 3D collision and clearance verification.",
            {
                {"document_path", {{"type", "string"}, {"description", "Path to .FCStd assembly document"}}},
                {"parent_body", {{"type", "string"}, {"description", "Parent link/body name"}}},
                {"child_body", {{"type", "string"}, {"description", "Child link/body name"}}},
                {"min_deg", {{"type", "number"}, {"description", "Minimum joint sweep angle (deg)"}}},
                {"max_deg", {{"type", "number"}, {"description", "Maximum joint sweep angle (deg)"}}},
                {"step_deg", {{"type", "number"}, {"description", "Sweep angular step (deg)"}}}
            },
            {}
        },
        {
            "freecad_analyze_printability",
            "freecad",
            "Validate 3D-printability (manifold watertightness, non-manifold edges, minimum wall thickness, overhang angles).",
            {
                {"document_path", {{"type", "string"}, {"description", "Path to .FCStd or mesh file"}}},
                {"min_wall_thickness_mm", {{"type", "number"}, {"description", "Minimum required wall thickness in mm"}}},
                {"max_overhang_deg", {{"type", "number"}, {"description", "Maximum unsupported overhang angle in degrees"}}}
            },
            {}
        },
        {
            "freecad_render_view",
            "freecad",
            "Render multi-angle (Isometric, Front, Top, Right) PNG snapshots for visual AI shape review.",
            {
                {"document_path", {{"type", "string"}, {"description", "Optional path to .FCStd document"}}},
                {"output_dir", {{"type", "string"}, {"description", "Directory to write rendered PNG images"}}},
                {"views", {{"type", "array"}, {"description", "View angles to render (e.g. ['Isometric', 'Front', 'Top', 'Right'])"}}}
            },
            {}
        },
        {
            "freecad_export_print_files",
            "freecad",
            "Export slicer-ready .stl, .3mf, .step, and robot URDF mass/CoM/inertia metadata.",
            {
                {"document_path", {{"type", "string"}, {"description", "Source .FCStd document path"}}},
                {"output_dir", {{"type", "string"}, {"description", "Target export directory"}}},
                {"formats", {{"type", "array"}, {"description", "Export formats (['stl', '3mf', 'step', 'urdf'])"}}}
            },
            {"output_dir"}
        },
        {
            "freecad_invoke_api",
            "freecad",
            "Invoke whitelisted FreeCAD workbench/module operations (FreeCAD, Part, PartDesign, Sketcher, Mesh, Spreadsheet, TechDraw, Import).",
            {
                {"module", {{"type", "string"}, {"description", "Whitelisted FreeCAD module name"}}},
                {"method", {{"type", "string"}, {"description", "Method name within the module"}}},
                {"args", {{"type", "array"}, {"description", "Positional arguments for the method"}}}
            },
            {"module", "method"}
        },
        // 26..36: EasyEDA Pro Tools (frog) + EasyEDA Converter
        {
            "easyeda_live_status",
            "easyeda",
            "Check live EasyEDA Pro desktop extension attachment on AppPluginRelay (:3893) and active project/document.",
            nlohmann::json::object(),
            {}
        },
        {
            "easyeda_search_library",
            "easyeda",
            "Search LCSC / EasyEDA component catalog (Cxxxxxx part numbers, JLCPCB Basic/Extended tier, live stock).",
            {
                {"query", {{"type", "string"}, {"description", "Component or LCSC C-number search query"}}},
                {"prefer_basic", {{"type", "boolean"}, {"description", "Prefer JLCPCB Basic library parts"}}}
            },
            {"query"}
        },
        {
            "easyeda_inspect_schematic",
            "easyeda",
            "Read live schematic components, wires, pins, and netlist from EasyEDA Pro.",
            {
                {"schematic_path", {{"type", "string"}, {"description", "Optional schematic file path fallback"}}}
            },
            {}
        },
        {
            "easyeda_edit_schematic",
            "easyeda",
            "Place components, draw orthogonal wires, and add net labels in live EasyEDA Pro schematic.",
            {
                {"operations", {{"type", "array"}, {"description", "Schematic primitive operations"}}},
                {"symbols", {{"type", "array"}, {"description", "Component placements"}}},
                {"wires", {{"type", "array"}, {"description", "Wire segments"}}}
            },
            {}
        },
        {
            "easyeda_sync_sch_to_pcb",
            "easyeda",
            "Synchronize schematic changes, footprints, and netlist into the EasyEDA Pro PCB document.",
            nlohmann::json::object(),
            {}
        },
        {
            "easyeda_inspect_pcb",
            "easyeda",
            "Inspect EasyEDA Pro PCB board outline, component placements, pad coordinates, tracks, and unrouted nets.",
            nlohmann::json::object(),
            {}
        },
        {
            "easyeda_edit_pcb",
            "easyeda",
            "Draw board outline, move/rotate/flip footprints, add copper pours, and route tracks/vias in EasyEDA Pro.",
            {
                {"outline", {{"type", "object"}, {"description", "Board outline dimensions"}}},
                {"placements", {{"type", "array"}, {"description", "Component placement updates"}}},
                {"tracks", {{"type", "array"}, {"description", "PCB trace segments"}}},
                {"vias", {{"type", "array"}, {"description", "PCB vias"}}},
                {"zones", {{"type", "array"}, {"description", "Copper pour regions"}}}
            },
            {}
        },
        {
            "easyeda_run_drc",
            "easyeda",
            "Run live Schematic ERC or PCB DRC inside EasyEDA Pro and return structured violations.",
            {
                {"target", {{"type", "string"}, {"description", "Check target: pcb (default) or schematic"}}}
            },
            {}
        },
        {
            "easyeda_export_jlcpcb",
            "easyeda",
            "Export turnkey JLCPCB Gerber ZIP, BOM CSV, Pick-and-Place CPL CSV, and Specctra .dsn into the workspace directory.",
            {
                {"output_dir", {{"type", "string"}, {"description", "Sandboxed output directory for JLCPCB manufacturing files"}}}
            },
            {"output_dir"}
        },
        {
            "easyeda_invoke_api",
            "easyeda",
            "Invoke whitelisted eda.<module>.<method> API calls inside EasyEDA Pro (eda.sch_*, eda.pcb_*, eda.lib_*, eda.dmt_*, eda.drc_*, eda.sys_Document*).",
            {
                {"api_path", {{"type", "string"}, {"description", "Whitelisted API path (e.g. eda.sch_PrimitiveComponent.getAll)"}}},
                {"args", {{"type", "array"}, {"description", "Arguments array"}}}
            },
            {"api_path"}
        },
        {
            "easyeda_convert",
            "easyeda",
            "Convert EasyEDA JSON schematic/PCB files on disk into KiCad 8 .kicad_sch/.kicad_pcb S-expressions on a 50-mil grid.",
            {
                {"input_path", {{"type", "string"}, {"description", "Source EasyEDA JSON file path"}}},
                {"output_path", {{"type", "string"}, {"description", "Target KiCad 8 .kicad_sch or .kicad_pcb file path"}}}
            },
            {"input_path", "output_path"}
        }
    };
}

} // anonymous namespace

McpServer::McpServer(StateStore& stateStore,
                     DynamicToolRegistry* dynamicRegistry,
                     TcpGateway* tcpGateway)
    : _stateStore(stateStore),
      _dynamicRegistry(dynamicRegistry),
      _tcpGateway(tcpGateway),
      _defaultProfile("all") {
}

void McpServer::notifyToolsListChanged() {
    if (_notificationBroadcaster) {
        nlohmann::json notif = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/tools/list_changed"},
            {"params", nlohmann::json::object()}
        };
        _notificationBroadcaster(notif.dump());
    }
}

bool McpServer::isEdaTool(const std::string& toolName) {
    if (EdaAccelerators::isAcceleratorTool(toolName)) {
        return true;
    }
    return (toolName.rfind("kicad_", 0) == 0 ||
            toolName.rfind("freecad_", 0) == 0 ||
            toolName.rfind("easyeda_", 0) == 0);
}

bool McpServer::isEdaToolEnabledInConfig(const std::string& toolName) const {
    EdaBridgeConfig cfg = _edaBridge ? _edaBridge->getConfig() : EdaBridgeConfig();
    if (EdaAccelerators::isAcceleratorTool(toolName)) {
        return cfg.accelerators.enabled;
    }
    if (!cfg.enabled) {
        return false;
    }
    if (toolName.rfind("kicad_", 0) == 0) {
        return cfg.kicad.enabled;
    }
    if (toolName.rfind("freecad_", 0) == 0) {
        return cfg.freecad.enabled;
    }
    if (toolName.rfind("easyeda_", 0) == 0) {
        return cfg.easyeda.enabled;
    }
    return false;
}

bool McpServer::isToolAllowedInProfile(const std::string& toolName, const std::string& profile) {
    if (toolName == "eda_bridge_status") {
        return false;
    }

    if (profile.empty() || profile == "all") {
        return true;
    }

    // Core AI quota and usage insights tools are always accessible in all profiles
    if (toolName == "check_antigravity_quota" ||
        toolName == "check_cursor_usage" ||
        toolName == "check_claude_usage" ||
        toolName == "get_combined_ai_status" ||
        toolName == "get_usage_insights" ||
        toolName == "get_usage_report" ||
        toolName == "export_usage_dataset" ||
        toolName == "describe_usage_dataset" ||
        toolName == "compare_experiment") {
        return true;
    }

    if (profile == "core") {
        return false;
    }

    if (profile == "embedded") {
        return (toolName.rfind("embdevenv_", 0) == 0);
    }

    if (profile == "network") {
        return (toolName.rfind("lan_", 0) == 0 ||
                toolName.rfind("firewall_", 0) == 0 ||
                toolName.rfind("snmp_", 0) == 0);
    }

    if (profile == "mesh") {
        return (toolName.rfind("meshmon_", 0) == 0);
    }

    if (profile == "mail") {
        return (toolName.rfind("mail_", 0) == 0);
    }

    if (profile == "eda") {
        return isEdaTool(toolName);
    }

    return true;
}

std::string McpServer::detectProfileFromWorkspace(const std::string& ws) {
    if (ws.empty()) {
        return "";
    }

    std::string lower = ws;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("kicad") != std::string::npos ||
        lower.find("freecad") != std::string::npos ||
        lower.find("easyeda") != std::string::npos ||
        lower.find("pcb") != std::string::npos ||
        lower.find("cad") != std::string::npos ||
        lower.find("robot") != std::string::npos ||
        lower.find("hw-") != std::string::npos) {
        return "eda";
    }

    if (lower.find("amba-virt") != std::string::npos ||
        lower.find("robohero") != std::string::npos ||
        lower.find("embdevenv") != std::string::npos) {
        return "embedded";
    }

    if (lower.find("mesh") != std::string::npos) {
        return "mesh";
    }

    if (lower.find("netmon") != std::string::npos) {
        return "network";
    }

    if (lower.find("intelligence") != std::string::npos ||
        lower.find("aimon") != std::string::npos) {
        return "core";
    }

    return "";
}

void McpServer::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        try {
            nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
            if (request.is_discarded()) {
                std::cerr << "[McpServer] Warning: malformed JSON input: " << line << std::endl;
                continue;
            }

            nlohmann::json response = handleMessage(request);
            if (!response.is_null()) {
                std::cout << response.dump() << "\n";
                std::cout.flush();
            }
        } catch (const std::exception& e) {
            std::cerr << "[McpServer] Exception handling request: " << e.what() << std::endl;
        }
    }
}

nlohmann::json McpServer::handleMessage(const nlohmann::json& request, const std::string& profile) {
    if (!request.contains("method")) {
        return nullptr;
    }

    std::string method = request["method"].get<std::string>();
    nlohmann::json id = request.value("id", nlohmann::json());

    std::string effectiveProfile = !profile.empty() ? profile : this->_defaultProfile;

    if (method == "initialize") {
        return handleInitialize(id, request.value("params", nlohmann::json::object()));
    } else if (method == "notifications/initialized") {
        return nullptr;
    } else if (method == "ping") {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", nlohmann::json::object()}
        };
    } else if (method == "tools/list") {
        return handleToolsList(id, effectiveProfile);
    } else if (method == "tools/call") {
        return handleToolsCall(id, request.value("params", nlohmann::json::object()), effectiveProfile);
    }

    if (!id.is_null()) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {
                {"code", -32601},
                {"message", "Method not found: " + method}
            }}
        };
    }

    return nullptr;
}

nlohmann::json McpServer::handleInitialize(const nlohmann::json& id, const nlohmann::json& /* params */) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {
                {"tools", nlohmann::json::object()}
            }},
            {"serverInfo", {
                {"name", "aimon"},
                {"version", "1.1.0"}
            }}
        }}
    };
}

nlohmann::json McpServer::handleToolsList(const nlohmann::json& id, const std::string& profile) {
    nlohmann::json toolsArray = nlohmann::json::array();

    // 1. Core AI Quota Tools (always included)
    toolsArray.push_back({
        {"name", "check_antigravity_quota"},
        {"description", "Returns Google Antigravity remaining capacity %, prompt credits, flow credits, and model reset timestamps."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "check_cursor_usage"},
        {"description", "Returns Cursor fast requests used vs limit, plan tier, and billing cycle reset date."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "check_claude_usage"},
        {"description", "Returns estimated Claude Code usage and cost per account: tokens and est. cost over 5 hours, 7 days, the configured window and the billing cycle, per-model breakdown, plan tier, and price freshness. Figures are estimates at published list prices from local transcripts and can differ from the billing page."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "get_combined_ai_status"},
        {"description", "Returns a comprehensive markdown summary of Google Antigravity, Cursor and Claude Code usage, quotas and limits."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    // 2. Usage Insights & Workflow-Coaching Tools (available in all profiles)
    toolsArray.push_back({
        {"name", "get_usage_insights"},
        {"description", "Returns programmatic AI usage and workflow-coaching findings across Claude Code, Cursor, and Google Antigravity, with severity, confidence, sample size n, and suggested experiments."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"platform", {{"type", "string"}, {"description", "Optional platform filter: claude, cursor, or antigravity"}}},
                {"days", {{"type", "integer"}, {"description", "Lookback window in days (default: 30)"}}},
                {"min_severity", {{"type", "string"}, {"description", "Optional minimum severity filter: info, warn, or high"}}}
            }}
        }}
    });

    toolsArray.push_back({
        {"name", "get_usage_report"},
        {"description", "Returns structured AI token, cache-hit ratio, thinking token, and estimated cost breakdowns grouped by project, model, day, tool, or session."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"platform", {{"type", "string"}, {"description", "Optional platform filter: claude, cursor, or antigravity"}}},
                {"days", {{"type", "integer"}, {"description", "Lookback window in days (default: 30)"}}},
                {"group_by", {{"type", "string"}, {"description", "Grouping dimension: project (default), model, day, tool, or session"}}}
            }}
        }}
    });

    toolsArray.push_back({
        {"name", "export_usage_dataset"},
        {"description", "Exports the normalized, privacy-preserving AI usage dataset (ins_requests, ins_sessions, ins_daily, ins_tool_calls, ins_quota, ins_spend_daily, ins_generations, ins_prompts, ins_findings) to JSONL, CSV, or self-contained SQLite."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"output_dir", {{"type", "string"}, {"description", "Target directory (default: ~/.local/share/aimon/exports)"}}},
                {"format", {{"type", "string"}, {"description", "Export format: jsonl (default), csv, or sqlite"}}},
                {"days", {{"type", "integer"}, {"description", "Lookback window in days (default: 90)"}}},
                {"anonymize_projects", {{"type", "boolean"}, {"description", "Hash project names in exported files (default: false)"}}}
            }}
        }}
    });

    toolsArray.push_back({
        {"name", "describe_usage_dataset"},
        {"description", "Returns schema documentation, privacy guarantees, and sample SQL queries for the normalized insights.db tables."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        }}
    });

    toolsArray.push_back({
        {"name", "compare_experiment"},
        {"description", "Compares before/after metrics for a workflow-coaching detector around a split timestamp and returns an improved/regressed/unchanged/insufficient_data verdict."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"detector", {{"type", "string"}, {"description", "Detector identifier (e.g. cache_hit_collapse, subagent_fanout_overhead, short_vague_prompt_tax)"}}},
                {"platform", {{"type", "string"}, {"description", "Optional platform filter: claude, cursor, or antigravity"}}},
                {"split_epoch", {{"type", "integer"}, {"description", "Unix timestamp dividing before and after windows (default: 7 days ago)"}}}
            }},
            {"required", nlohmann::json::array({"detector"})}
        }}
    });

    // 3. EDA/CAD & Local CPU Accelerator Tools (36 tools, filtered by profile & component config)
    for (const auto& desc : getEdaToolDescriptors()) {
        std::string tName(desc.name);
        if (!isToolAllowedInProfile(tName, profile)) {
            continue;
        }
        if (!isEdaToolEnabledInConfig(tName)) {
            continue;
        }
        nlohmann::json schema = {
            {"type", "object"},
            {"properties", desc.properties}
        };
        if (!desc.required.empty()) {
            schema["required"] = desc.required;
        }
        toolsArray.push_back({
            {"name", tName},
            {"description", desc.description},
            {"inputSchema", schema}
        });
    }

    // 4. Dynamic Satellite Tools filtered by active profile (excluding EDA tools handled above)
    if (profile != "core" && _dynamicRegistry) {
        nlohmann::json dynTools = _dynamicRegistry->getToolsListJson();
        if (dynTools.is_array()) {
            for (const auto& dt : dynTools) {
                if (dt.contains("name") && dt["name"].is_string()) {
                    std::string tName = dt["name"].get<std::string>();
                    if (isEdaTool(tName) || tName == "eda_bridge_status") {
                        continue;
                    }
                    if (isToolAllowedInProfile(tName, profile)) {
                        toolsArray.push_back(dt);
                    }
                }
            }
        }
    }

    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"tools", toolsArray}
        }}
    };
}

nlohmann::json McpServer::handleToolsCall(const nlohmann::json& id, const nlohmann::json& params, const std::string& profile) {
    std::string toolName = params.value("name", "");

    if (!isToolAllowedInProfile(toolName, profile)) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {
                {"code", -32601},
                {"message", "Tool '" + toolName + "' is not available under active profile '" + profile + "'"}
            }}
        };
    }

    if (isEdaTool(toolName)) {
        if (!isEdaToolEnabledInConfig(toolName)) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {
                    {"code", -32601},
                    {"message", "Tool '" + toolName + "' is disabled in EdaBridgeConfig"}
                }}
            };
        }

        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        nlohmann::json edaRes;
        if (_edaBridge) {
            edaRes = _edaBridge->callTool(toolName, args);
        } else {
            EdaBridge fallbackBridge;
            edaRes = fallbackBridge.callTool(toolName, args);
        }

        bool isErr = edaRes.is_object() && !edaRes.value("ok", true);
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", {
                {"isError", isErr},
                {"content", {
                    {
                        {"type", "text"},
                        {"text", edaRes.dump(2)}
                    }
                }}
            }}
        };
    }

    AggregateStatus current = _stateStore.getStatus();
    std::string contentText;

    if (toolName == "check_antigravity_quota") {
        contentText = formatAntigravityStatus(current.antigravity);
    } else if (toolName == "check_cursor_usage") {
        contentText = formatCursorStatus(current.cursor);
    } else if (toolName == "check_claude_usage") {
        contentText = formatClaudeStatus(current.claude);
    } else if (toolName == "get_combined_ai_status") {
        contentText = formatCombinedStatus(current);
    } else if (toolName == "describe_usage_dataset") {
        contentText = InsightEngine::describeDataset().dump(2);
    } else if (toolName == "get_usage_insights") {
        if (!_insightEngine) {
            contentText = nlohmann::json({{"error", "InsightEngine is not enabled"}, {"findings", nlohmann::json::array()}}).dump(2);
        } else {
            nlohmann::json args = params.value("arguments", nlohmann::json::object());
            std::string plat = args.value("platform", "");
            int days = args.value("days", 30);
            std::string minSev = args.value("min_severity", "");
            contentText = _insightEngine->getInsights(plat, days, minSev).dump(2);
        }
    } else if (toolName == "get_usage_report") {
        if (!_insightEngine) {
            contentText = nlohmann::json({{"error", "InsightEngine is not enabled"}, {"rows", nlohmann::json::array()}}).dump(2);
        } else {
            nlohmann::json args = params.value("arguments", nlohmann::json::object());
            std::string plat = args.value("platform", "");
            int days = args.value("days", 30);
            std::string groupBy = args.value("group_by", "project");
            contentText = _insightEngine->getUsageReport(plat, days, groupBy).dump(2);
        }
    } else if (toolName == "export_usage_dataset") {
        if (!_insightEngine) {
            contentText = nlohmann::json({{"error", "InsightEngine is not enabled"}, {"files", nlohmann::json::array()}}).dump(2);
        } else {
            nlohmann::json args = params.value("arguments", nlohmann::json::object());
            std::string outDir = args.value("output_dir", "");
            std::string fmt = args.value("format", "jsonl");
            int days = args.value("days", 90);
            bool anon = args.value("anonymize_projects", false);
            contentText = _insightEngine->exportDataset(outDir, fmt, days, anon).dump(2);
        }
    } else if (toolName == "compare_experiment") {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        std::string det = args.value("detector", "");
        if (det.empty()) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32602}, {"message", "Missing required argument 'detector'"}}}
            };
        }
        if (!_insightEngine) {
            contentText = nlohmann::json({{"detector", det}, {"verdict", "insufficient_data"}}).dump(2);
        } else {
            std::string plat = args.value("platform", "");
            int64_t splitEpoch = args.value("split_epoch", int64_t(0));
            contentText = _insightEngine->compareExperiment(det, plat, splitEpoch).dump(2);
        }
    } else if (_dynamicRegistry && _tcpGateway && _dynamicRegistry->hasTool(toolName)) {
        nlohmann::json args = params.value("arguments", nlohmann::json::object());
        nlohmann::json gwResult;
        std::string gwError;

        int timeoutMs = 45000;
        if (args.contains("timeout_sec") && args["timeout_sec"].is_number()) {
            timeoutMs = (args["timeout_sec"].get<int>() + 10) * 1000;
        } else if (toolName.find("flash") != std::string::npos) {
            timeoutMs = 180000;
        }

        bool ok = _tcpGateway->callTool(toolName, args, gwResult, gwError, timeoutMs);
        if (!ok) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {
                    {"code", -32603},
                    {"message", "Subsystem tool call failed: " + gwError}
                }}
            };
        }
        if (gwResult.contains("content")) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", gwResult}
            };
        }
        contentText = gwResult.is_string() ? gwResult.get<std::string>() : gwResult.dump(2);
    } else {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {
                {"code", -32602},
                {"message", "Unknown tool: " + toolName}
            }}
        };
    }

    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"content", {
                {
                    {"type", "text"},
                    {"text", contentText}
                }
            }}
        }}
    };
}

std::string McpServer::formatAntigravityStatus(const AntigravityStatus& ag) {
    std::ostringstream ss;
    ss << "### Google Antigravity Quota Status\n\n";

    if (!ag.isRunning) {
        ss << "**Status**: Offline\n";
        if (!ag.errorMessage.empty()) {
            ss << "**Note**: " << ag.errorMessage << "\n";
        }
        return ss.str();
    }

    ss << "- **Plan Tier**: " << ag.planTier << "\n";

    if (!ag.availableCredits.empty()) {
        for (const auto& uc : ag.availableCredits) {
            std::string typeDisplay = uc.creditType;
            if (typeDisplay == "GOOGLE_ONE_AI") {
                typeDisplay = "Google One AI credits";
            }
            ss << "- **Available Credits**: **" << uc.creditAmount << "** " << typeDisplay;
            if (uc.minimumCreditAmountForUsage > 0) {
                ss << " (minimum " << uc.minimumCreditAmountForUsage << " per request)";
            }
            ss << "\n";
        }
    }

    if (ag.availablePromptCredits > 0 || ag.availableFlowCredits > 0 ||
        ag.monthlyPromptCredits > 0 || ag.monthlyFlowCredits > 0) {
        ss << "- **Prompt / Flow Credits**: " << ag.availablePromptCredits << " prompt / "
           << ag.availableFlowCredits << " flow available";
        if (ag.monthlyPromptCredits > 0 || ag.monthlyFlowCredits > 0) {
            ss << " (Monthly allocation: " << ag.monthlyPromptCredits << " / "
               << ag.monthlyFlowCredits << ")";
        }
        ss << "\n";
    }
    ss << "\n";

    if (!ag.quotaGroups.empty()) {
        for (const auto& g : ag.quotaGroups) {
            ss << "#### " << g.displayName << "\n";
            if (!g.description.empty()) {
                ss << "*" << g.description << "*\n\n";
            }
            ss << "| Limit Window | Remaining | Reset Time |\n";
            ss << "| :--- | :--- | :--- |\n";
            for (const auto& b : g.buckets) {
                int pct = static_cast<int>(b.remainingFraction * 100.0 + 0.5);
                ss << "| " << b.displayName << " | **" << pct << "%** | "
                   << (b.resetTimeIso.empty() ? "N/A" : b.resetTimeIso) << " |\n";
            }
            ss << "\n";
        }
    } else if (!ag.models.empty()) {
        ss << "| Model | Remaining Capacity | Reset Time |\n";
        ss << "| :--- | :--- | :--- |\n";
        for (const auto& m : ag.models) {
            int pct = static_cast<int>(m.remainingFraction * 100.0 + 0.5);
            ss << "| " << m.modelName << " | " << pct << "% | " << (m.resetTimeIso.empty() ? "N/A" : m.resetTimeIso) << " |\n";
        }
    }

    return ss.str();
}

std::string McpServer::formatCursorStatus(const CursorStatus& cr) {
    std::ostringstream ss;
    ss << "### Cursor Usage & Quota Status\n\n";

    if (!cr.isAuthenticated) {
        ss << "**Status**: Unauthenticated / Free\n";
        if (!cr.planTier.empty()) {
            ss << "- **Tier**: " << cr.planTier << "\n";
        }
        if (!cr.errorMessage.empty()) {
            ss << "- **Note**: " << cr.errorMessage << "\n";
        }
        return ss.str();
    }

    ss << "- **Plan Tier**: " << cr.planTier << "\n";
    ss << "- **Fast Requests Used**: " << cr.fastRequestsUsed;
    if (cr.fastRequestsLimit > 0) {
        ss << " / " << cr.fastRequestsLimit;
        int remaining = cr.fastRequestsLimit - cr.fastRequestsUsed;
        ss << " (" << remaining << " remaining)";
    }
    ss << "\n";
    if (cr.totalSpendUsd > 0.0) {
        ss << "- **Current Billing Cycle Spend**: $" << std::fixed << std::setprecision(2) << cr.totalSpendUsd;
        if (cr.prevCycleSpendUsd > 0.0) {
            ss << " (vs $" << std::fixed << std::setprecision(2) << cr.prevCycleSpendUsd << " previous cycle)";
        }
        ss << "\n";
    }
    if (!cr.cycleResetIso.empty()) {
        ss << "- **Billing Cycle Reset**: " << cr.cycleResetIso << "\n";
    }

    if (!cr.categorySpendList.empty()) {
        ss << "\n**Personal Usage by Model**:\n";
        for (const auto& cat : cr.categorySpendList) {
            if (cat.spendUsd <= 0.0) continue;
            ss << "  - `" << cat.category << "`: $" << std::fixed << std::setprecision(2)
               << cat.spendUsd << " (" << std::fixed << std::setprecision(1) << cat.percentage << "%)\n";
        }
    }

    return ss.str();
}

std::string McpServer::formatCombinedStatus(const AggregateStatus& status) {
    std::ostringstream ss;
    ss << "# AI Subscription & Quota Summary\n\n";
    ss << formatAntigravityStatus(status.antigravity);
    ss << "\n---\n\n";
    ss << formatCursorStatus(status.cursor);
    ss << "\n---\n\n";
    ss << formatClaudeStatus(status.claude);
    return ss.str();
}

namespace {

// Dollars from integer nano-dollars, rounded half up to the cent.
std::string claudeUsd(int64_t nano) {
    if (nano < 0) {
        nano = 0;
    }
    const int64_t cents = (nano + 5000000) / 10000000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "$%lld.%02lld", static_cast<long long>(cents / 100),
                  static_cast<long long>(cents % 100));
    return buf;
}

std::string claudeTokens(int64_t n) {
    char buf[48];
    if (n < 1000) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
    } else if (n < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(n) / 1e3);
    } else if (n < 1000000000) {
        std::snprintf(buf, sizeof(buf), "%.2fM", static_cast<double>(n) / 1e6);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fB", static_cast<double>(n) / 1e9);
    }
    return buf;
}

// Names come from files and config; keep them from breaking markdown tables.
std::string claudeMdSafe(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '|' || c == '`' || c == '\n' || c == '\r') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
        if (out.size() >= 80) {
            break;
        }
    }
    return out;
}

std::string claudeUtc(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv);
    return buf;
}

} // namespace

std::string McpServer::formatClaudeStatus(const ClaudeStatus& cl) {
    std::ostringstream ss;
    ss << "### Claude Code Usage (estimated)\n\n";
    if (!cl.enabled) {
        ss << "Claude usage collection is disabled.\n";
        return ss.str();
    }
    if (cl.accounts.empty()) {
        ss << "No Claude accounts are configured.\n";
        return ss.str();
    }
    ss << "_Estimates from Claude Code transcripts on this host at published list prices. "
       << "All dollar figures are est. values, not invoices, and can differ from the billing page "
       << "(price changes, other machines, other products)._\n";

    for (const ClaudeAccountStatus& a : cl.accounts) {
        const char* tierName = a.tier == ClaudeTier::Enterprise ? "Enterprise"
                             : a.tier == ClaudeTier::Personal ? "Personal" : "Unknown tier";
        const char* valueLabel = a.tier == ClaudeTier::Enterprise ? "Est. cost"
                               : a.tier == ClaudeTier::Personal ? "Est. API-equivalent value" : "Est. value";
        ss << "\n#### Account `" << claudeMdSafe(a.name) << "` \xE2\x80\x94 " << tierName;
        if (!a.rawSubscriptionType.empty()) {
            ss << " (" << claudeMdSafe(a.rawSubscriptionType);
            if (!a.rawRateLimitTier.empty()) {
                ss << " / " << claudeMdSafe(a.rawRateLimitTier);
            }
            ss << ")";
        }
        ss << "\n";

        if (!a.hasData) {
            ss << "- **Status**: no Claude Code usage found\n";
        } else {
            if (a.cycleConfigured) {
                ss << "- **" << valueLabel << " this cycle**: " << claudeUsd(a.cycle.costNano);
                if (a.spendLimitUsd > 0.0) {
                    ss << " of $" << std::fixed << std::setprecision(2) << a.spendLimitUsd << " ("
                       << std::setprecision(1) << a.estPctOfLimit << "%)";
                }
                ss << ", cycle resets " << a.cycleResetIso << "\n";
            }
            ss << "- **" << valueLabel << "**: " << claudeUsd(a.last5h.costNano) << " (5 h) \xC2\xB7 "
               << claudeUsd(a.last7d.costNano) << " (7 d) \xC2\xB7 " << claudeUsd(a.window.costNano)
               << " (" << a.windowDays << " d)\n";
            ss << "- **Tokens**: " << claudeTokens(a.last5h.totalTokens()) << " (5 h) \xC2\xB7 "
               << claudeTokens(a.last7d.totalTokens()) << " (7 d) \xC2\xB7 "
               << claudeTokens(a.window.totalTokens()) << " (" << a.windowDays << " d)\n";
            ss << "- **Messages (" << a.windowDays << " d)**: " << a.window.messages << "\n";

            if (!a.models.empty()) {
                ss << "\n| Model | Messages | Tokens | Est. cost |\n|---|---:|---:|---:|\n";
                size_t shown = 0;
                for (const ClaudeModelUsage& m : a.models) {
                    if (++shown > 10) {
                        break;
                    }
                    const int64_t tokens = m.input + m.output + m.cacheRead + m.cacheWrite5m + m.cacheWrite1h;
                    ss << "| `" << claudeMdSafe(m.model) << "` | " << m.messages << " | " << claudeTokens(tokens)
                       << " | " << claudeUsd(m.costNano) << " |\n";
                }
                ss << "\n";
            }
            if (!a.unpricedModels.empty()) {
                ss << "- **Unpriced models**: ";
                for (size_t i = 0; i < a.unpricedModels.size(); ++i) {
                    ss << (i ? ", " : "") << "`" << claudeMdSafe(a.unpricedModels[i]) << "`";
                }
                ss << " (tokens counted, no cost)\n";
            }
        }
        if (a.skippedLines > 0) {
            ss << "- **Skipped**: " << a.skippedLines << " lines skipped (malformed or out of range)\n";
        }
        if (!a.errorMessage.empty()) {
            ss << "- **Error**: " << claudeMdSafe(a.errorMessage) << "\n";
        }
        if (!a.warning.empty()) {
            ss << "- **Warning**: " << claudeMdSafe(a.warning) << "\n";
        }
    }

    ss << "\n- **Prices**: ";
    if (cl.pricing.fetchedAtEpoch > 0) {
        ss << cl.pricing.modelsLoaded << " models, as of " << claudeUtc(cl.pricing.fetchedAtEpoch);
        if (cl.pricing.stale) {
            ss << " \xE2\x80\x94 STALE (no successful fetch for over 3 refresh intervals)";
        }
        if (cl.pricing.quarantinedCount > 0) {
            ss << "; " << cl.pricing.quarantinedCount << " models quarantined (left unpriced)";
        }
    } else {
        ss << "none loaded";
    }
    ss << "\n";
    if (!cl.pricing.error.empty()) {
        ss << "- **Price fetch**: " << claudeMdSafe(cl.pricing.error) << "\n";
    }
    return ss.str();
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
