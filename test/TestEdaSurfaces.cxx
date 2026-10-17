/*
 * TestEdaSurfaces.cxx
 *
 * Qualification suite for Windows EDA/CAD & Local CPU Accelerator MCP profile
 * ("eda"), 36-tool listing/dispatch, per-component enable/disable gating,
 * workspace auto-detection, and session-protected WebServer REST endpoints
 * (GET /api/eda/status, POST /api/eda/config, POST /api/eda/probe).
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ConfigManager.hxx"
#include "DynamicToolRegistry.hxx"
#include "EasyEdaConverter.hxx"
#include "EdaAccelerators.hxx"
#include "EdaBridge.hxx"
#include "EdaRpcServer.hxx"
#include "HistoryStore.hxx"
#include "KiCadSchematicEditor.hxx"
#include "McpServer.hxx"
#include "PathUtils.hxx"
#include "StateStore.hxx"
#include "TcpGateway.hxx"
#include "WebAssets.hxx"
#include "WebServer.hxx"

// CppUTest's memory leak macros redefine `new`; its headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

namespace fs = std::filesystem;
using namespace aimon;
using json = nlohmann::json;

namespace {

std::string makeTempDir(const std::string& tag) {
    fs::path base = fs::temp_directory_path() / ("aimon_eda_surf_" + tag + "_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base.string();
}

json callMcpTool(McpServer& server,
                 const std::string& toolName,
                 const json& args = json::object(),
                 const std::string& profile = "eda") {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 101},
        {"method", "tools/call"},
        {"params", {
            {"name", toolName},
            {"arguments", args}
        }}
    };
    return server.handleMessage(req, profile);
}

std::set<std::string> listMcpToolNames(McpServer& server, const std::string& profile) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 102},
        {"method", "tools/list"},
        {"params", json::object()}
    };
    json res = server.handleMessage(req, profile);
    std::set<std::string> names;
    if (res.contains("result") && res["result"].contains("tools") && res["result"]["tools"].is_array()) {
        for (const auto& t : res["result"]["tools"]) {
            if (t.is_object() && t.contains("name") && t["name"].is_string()) {
                names.insert(t["name"].get<std::string>());
            }
        }
    }
    return names;
}

std::string extractSessionCookieToken(const httplib::Response& res) {
    if (!res.has_header("Set-Cookie")) return "";
    std::string cookie = res.get_header_value("Set-Cookie");
    size_t pos = cookie.find("aimon_session=");
    if (pos == std::string::npos) return "";
    size_t start = pos + 14;
    size_t end = cookie.find(';', start);
    return (end == std::string::npos) ? cookie.substr(start) : cookie.substr(start, end - start);
}

// Simulated EasyEDA Pro WebSocket plugin client connecting to AppPluginRelay (127.0.0.1:pluginPort)
class MockEasyEdaPluginClient {
public:
    bool start(int pluginPort) {
        _sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (_sock < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(pluginPort));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(_sock);
            _sock = -1;
            return false;
        }

        std::string handshake =
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1:" + std::to_string(pluginPort) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (::send(_sock, handshake.data(), handshake.size(), 0) <= 0) {
            ::close(_sock);
            _sock = -1;
            return false;
        }

        char hdrBuf[1024];
        ssize_t n = ::recv(_sock, hdrBuf, sizeof(hdrBuf) - 1, 0);
        if (n <= 0) {
            ::close(_sock);
            _sock = -1;
            return false;
        }
        hdrBuf[n] = '\0';
        if (std::strstr(hdrBuf, "101 Switching Protocols") == nullptr) {
            ::close(_sock);
            _sock = -1;
            return false;
        }

        _running.store(true);
        json reg = {
            {"jsonrpc", "2.0"},
            {"id", "ws-reg-1"},
            {"method", "plugin/register"},
            {"params", {
                {"app", "easyeda"},
                {"version", "2.2.0"},
                {"active_document", "ServoDriver.epro"}
            }}
        };
        auto frame = EdaRpcServer::encodeWebSocketFrame(reg.dump(), 0x1, true);
        ::send(_sock, frame.data(), frame.size(), 0);

        _thread = std::thread([this]() { loop(); });
        return true;
    }

    void stop() {
        if (!_running.exchange(false)) return;
        if (_sock >= 0) {
            ::shutdown(_sock, SHUT_RDWR);
            ::close(_sock);
            _sock = -1;
        }
        if (_thread.joinable()) _thread.join();
    }

    ~MockEasyEdaPluginClient() {
        stop();
    }

private:
    void loop() {
        std::vector<uint8_t> buf;
        uint8_t chunk[4096];
        while (_running.load()) {
            ssize_t n = ::recv(_sock, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.insert(buf.end(), chunk, chunk + n);

            while (!buf.empty()) {
                size_t consumed = 0;
                uint8_t opcode = 0;
                std::string payload;
                std::string err;
                if (!EdaRpcServer::decodeWebSocketFrame(buf.data(), buf.size(), consumed, opcode, payload, err)) {
                    break;
                }
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

                json msg = json::parse(payload, nullptr, false);
                if (msg.is_discarded() || !msg.is_object()) continue;
                if (msg.value("method", "") == "tools/call") {
                    json id = msg["id"];
                    std::string tName = msg["params"].value("name", "");
                    json tArgs = msg["params"].value("arguments", json::object());

                    json res = {
                        {"ok", true},
                        {"tool", tName},
                        {"active_document", "ServoDriver.epro"}
                    };
                    if (tName == "easyeda_search_library") {
                        res["devices"] = json::array({
                            {{"lcsc", "C25804"}, {"title", "10k 0603 Resistor"}, {"library_type", "Basic"}, {"stock", 50000}}
                        });
                    } else if (tName == "easyeda_export_jlcpcb") {
                        res["gerber_data"] = "PK\x03\x04JLCPCB_GERBER";
                        res["bom_csv"] = "Comment,Designator,Footprint,LCSC Part #\n10k,R1,0603,C25804\n";
                        res["cpl_csv"] = "Designator,Mid X,Mid Y,Layer,Rotation\nR1,10mm,10mm,Top,0\n";
                        res["dsn_content"] = "(pcb \"easyeda.dsn\")\n";
                    } else if (tName == "easyeda_run_drc") {
                        res["violation_count"] = 0;
                        res["violations"] = json::array();
                    }

                    json reply = {
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", res}
                    };
                    auto outFrame = EdaRpcServer::encodeWebSocketFrame(reply.dump(), 0x1, true);
                    ::send(_sock, outFrame.data(), outFrame.size(), 0);
                }
            }
        }
    }

    int _sock = -1;
    std::atomic<bool> _running{false};
    std::thread _thread;
};

static constexpr const char* kAllAcceleratorTools[] = {
    "eda_calc_schematic_layout",
    "eda_calc_pcb_placement",
    "eda_calc_pcb_autoroute",
    "eda_calc_electrical_synth",
    "eda_calc_pcb_connectivity_audit",
    "cad_calc_robot_kinematics_statics",
    "cad_calc_mechanism_synth",
    "cad_calc_print_orientation_dfm"
};

static constexpr const char* kAllKiCadTools[] = {
    "kicad_search_library",
    "kicad_inspect_project",
    "kicad_edit_schematic",
    "kicad_sync_sch_to_pcb",
    "kicad_edit_pcb",
    "kicad_run_erc",
    "kicad_run_drc",
    "kicad_export_fab_pack",
    "kicad_export_step"
};

static constexpr const char* kAllFreeCadTools[] = {
    "freecad_live_status",
    "freecad_inspect_document",
    "freecad_generate_cad",
    "freecad_edit_parameters",
    "freecad_assemble_joint",
    "freecad_analyze_printability",
    "freecad_render_view",
    "freecad_export_print_files",
    "freecad_invoke_api"
};

static constexpr const char* kAllEasyEdaTools[] = {
    "easyeda_live_status",
    "easyeda_search_library",
    "easyeda_inspect_schematic",
    "easyeda_edit_schematic",
    "easyeda_sync_sch_to_pcb",
    "easyeda_inspect_pcb",
    "easyeda_edit_pcb",
    "easyeda_run_drc",
    "easyeda_export_jlcpcb",
    "easyeda_invoke_api",
    "easyeda_convert"
};

} // anonymous namespace

TEST_GROUP(McpServer_EdaProfile) {
    std::string _ws;
    std::unique_ptr<EdaRpcServer> _rpcServer;
    std::unique_ptr<MockEasyEdaPluginClient> _easyEdaPlugin;
    EdaBridgeConfig _cfg;
    std::unique_ptr<EdaBridge> _bridge;
    std::unique_ptr<McpServer> _mcp;

    void setup() override {
        _ws = makeTempDir("mcp_eda");

        EdaRpcServerOptions opts;
        opts.listenHost = "127.0.0.1";
        opts.port = 0;
        opts.pluginPort = 0;
        opts.authSecret = "mcp-surface-secret-2026";
        opts.allowedRoot = _ws;

        _rpcServer = std::make_unique<EdaRpcServer>(opts);
        CHECK_TRUE(_rpcServer->start());

        _easyEdaPlugin = std::make_unique<MockEasyEdaPluginClient>();
        CHECK_TRUE(_easyEdaPlugin->start(_rpcServer->boundPluginPort()));
        for (int i = 0; i < 30 && !_rpcServer->isPluginConnected("easyeda"); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK_TRUE(_rpcServer->isPluginConnected("easyeda"));

        _cfg.enabled = true;
        _cfg.mode = "client";
        _cfg.host = "127.0.0.1";
        _cfg.port = _rpcServer->boundPort();
        _cfg.pluginPort = _rpcServer->boundPluginPort();
        _cfg.authSecret = "mcp-surface-secret-2026";
        _cfg.linuxPathPrefix = _ws;
        _cfg.windowsPathPrefix = _ws;

        _bridge = std::make_unique<EdaBridge>(_cfg);
        _mcp = std::make_unique<McpServer>(StateStore::getInstance());
        _mcp->setEdaBridge(_bridge.get());
    }

    void teardown() override {
        _mcp.reset();
        _bridge.reset();
        if (_easyEdaPlugin) {
            _easyEdaPlugin->stop();
            _easyEdaPlugin.reset();
        }
        if (_rpcServer) {
            _rpcServer->stop();
            _rpcServer.reset();
        }
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(McpServer_EdaProfile, WorkspaceAutoDetectsEdaProfileAndPreservesExistingProfiles) {
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("/workspace/hw-controller/kicad"));
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("/workspace/freecad-parts"));
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("/workspace/easyeda-board"));
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("/workspace/pcb-main"));
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("/workspace/robot/arm-joint"));
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("/workspace/cad-enclosure"));
    CHECK_EQUAL("eda", McpServer::detectProfileFromWorkspace("file:///projects/hw-power-board"));

    // Existing workspace mappings remain intact
    CHECK_EQUAL("embedded", McpServer::detectProfileFromWorkspace("/workspace/robohero"));
    CHECK_EQUAL("embedded", McpServer::detectProfileFromWorkspace("/workspace/embdevenv"));
    CHECK_EQUAL("network", McpServer::detectProfileFromWorkspace("/workspace/netmon"));
    CHECK_EQUAL("mesh", McpServer::detectProfileFromWorkspace("/workspace/meshmon"));
    CHECK_EQUAL("core", McpServer::detectProfileFromWorkspace("/workspace/aimon"));
}

TEST(McpServer_EdaProfile, ProfileScopingAndPerComponentTogglesFilterToolsListAndCall) {
    // 1. When all 4 components and eda.enabled are true, "eda", "cad", and "all" list all 37 EDA tools
    for (const char* prof : {"eda", "cad", "all"}) {
        auto names = listMcpToolNames(*_mcp, prof);
        for (const char* t : kAllAcceleratorTools) CHECK_TRUE_TEXT(names.count(t) == 1, t);
        for (const char* t : kAllKiCadTools) CHECK_TRUE_TEXT(names.count(t) == 1, t);
        for (const char* t : kAllFreeCadTools) CHECK_TRUE_TEXT(names.count(t) == 1, t);
        for (const char* t : kAllEasyEdaTools) CHECK_TRUE_TEXT(names.count(t) == 1, t);
        // Core AI quota + insights tools are also present
        CHECK_TRUE(names.count("check_antigravity_quota") == 1);
        CHECK_TRUE(names.count("get_usage_insights") == 1);
    }

    // 2. Under "core" and "network" profiles, none of the 36 EDA tools are listed or callable
    for (const char* prof : {"core", "network", "embedded", "mesh"}) {
        auto names = listMcpToolNames(*_mcp, prof);
        for (const char* t : kAllAcceleratorTools) CHECK_FALSE_TEXT(names.count(t) > 0, t);
        for (const char* t : kAllKiCadTools) CHECK_FALSE_TEXT(names.count(t) > 0, t);
        for (const char* t : kAllFreeCadTools) CHECK_FALSE_TEXT(names.count(t) > 0, t);
        for (const char* t : kAllEasyEdaTools) CHECK_FALSE_TEXT(names.count(t) > 0, t);

        json errRes = callMcpTool(*_mcp, "kicad_run_erc", {{"schematic_path", _ws + "/a.kicad_sch"}}, prof);
        CHECK_TRUE(errRes.contains("error"));
        LONGS_EQUAL(-32601, errRes["error"].value("code", 0));
    }

    // 3. Boundary: When remote Windows bridge eda.enabled == false and no gateway session is registered,
    // the 8 local CPU accelerators remain listed and callable under "eda", "cad", and "all", while the 29 remote tools are hidden & rejected
    EdaBridgeConfig localOnlyCfg = _cfg;
    localOnlyCfg.enabled = false;
    localOnlyCfg.accelerators.enabled = true;
    _bridge->setConfig(localOnlyCfg);

    auto localOnlyNames = listMcpToolNames(*_mcp, "eda");
    for (const char* t : kAllAcceleratorTools) CHECK_TRUE_TEXT(localOnlyNames.count(t) == 1, t);
    for (const char* t : kAllKiCadTools) CHECK_FALSE_TEXT(localOnlyNames.count(t) > 0, t);
    for (const char* t : kAllFreeCadTools) CHECK_FALSE_TEXT(localOnlyNames.count(t) > 0, t);
    for (const char* t : kAllEasyEdaTools) CHECK_FALSE_TEXT(localOnlyNames.count(t) > 0, t);

    json localCall = callMcpTool(*_mcp, "eda_calc_electrical_synth",
                                 {{"mode", "divider"}, {"vin_v", 5.0}, {"vout_target_v", 3.3}}, "cad");
    CHECK_TRUE(localCall.contains("result"));
    CHECK_FALSE(localCall["result"].value("isError", true));

    json remoteRejected = callMcpTool(*_mcp, "kicad_run_erc", {{"schematic_path", _ws + "/a.kicad_sch"}}, "eda");
    CHECK_TRUE(remoteRejected.contains("error"));
    LONGS_EQUAL(-32601, remoteRejected["error"].value("code", 0));

    // 3b. Dynamic TcpGateway Registration: When eda.enabled == false in config, but aimon-eda-bridge
    // connects to TcpGateway and registers tools in DynamicToolRegistry, McpServer lists and dispatches them!
    {
        DynamicToolRegistry dynReg;
        TcpGateway tcpGw(dynReg);
        int gwPort = 41885 + (::getpid() % 3000);
        if (!tcpGw.start("127.0.0.1", gwPort)) {
            gwPort += 173;
            CHECK_TRUE(tcpGw.start("127.0.0.1", gwPort));
        }
        CHECK_TRUE(_rpcServer->connectToGateway("127.0.0.1", gwPort));
        for (int i = 0; i < 40 && !dynReg.hasTool("easyeda_live_status"); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        CHECK_TRUE(dynReg.hasTool("easyeda_live_status"));

        McpServer dynMcp(StateStore::getInstance(), &dynReg, &tcpGw);
        dynMcp.setEdaBridge(_bridge.get()); // _bridge still has localOnlyCfg.enabled == false!

        auto dynNames = listMcpToolNames(dynMcp, "cad");
        CHECK_TRUE(dynNames.count("easyeda_live_status") == 1);
        CHECK_TRUE(dynNames.count("eda_calc_pcb_connectivity_audit") == 1);

        json dynCall = callMcpTool(dynMcp, "easyeda_live_status", json::object(), "cad");
        CHECK_TRUE(dynCall.contains("result"));
        CHECK_FALSE(dynCall["result"].value("isError", true));

        _rpcServer->disconnectGateway();
        tcpGw.stop();
    }

    // 4. Per-component toggles: disabling kicad, freecad, easyeda, or accelerators individually
    EdaBridgeConfig kicadOffCfg = _cfg;
    kicadOffCfg.kicad.enabled = false;
    _bridge->setConfig(kicadOffCfg);
    auto noKicadNames = listMcpToolNames(*_mcp, "eda");
    for (const char* t : kAllKiCadTools) CHECK_FALSE_TEXT(noKicadNames.count(t) > 0, t);
    for (const char* t : kAllFreeCadTools) CHECK_TRUE_TEXT(noKicadNames.count(t) == 1, t);
    for (const char* t : kAllEasyEdaTools) CHECK_TRUE_TEXT(noKicadNames.count(t) == 1, t);
    json kicadOffCall = callMcpTool(*_mcp, "kicad_run_erc", {{"schematic_path", _ws + "/a.kicad_sch"}}, "eda");
    CHECK_TRUE(kicadOffCall.contains("error"));
    LONGS_EQUAL(-32601, kicadOffCall["error"].value("code", 0));

    EdaBridgeConfig accOffCfg = _cfg;
    accOffCfg.accelerators.enabled = false;
    _bridge->setConfig(accOffCfg);
    auto noAccNames = listMcpToolNames(*_mcp, "eda");
    for (const char* t : kAllAcceleratorTools) CHECK_FALSE_TEXT(noAccNames.count(t) > 0, t);
    json accOffCall = callMcpTool(*_mcp, "eda_calc_schematic_layout", json::object(), "eda");
    CHECK_TRUE(accOffCall.contains("error"));
    LONGS_EQUAL(-32601, accOffCall["error"].value("code", 0));

    // Restore full config
    _bridge->setConfig(_cfg);
}

TEST(McpServer_EdaProfile, All36ToolsExecuteThroughMcpServerAndUpdateTelemetry) {
    auto callAndParse = [&](const std::string& name, const json& args) -> json {
        json mcpResp = callMcpTool(*_mcp, name, args, "eda");
        CHECK_TRUE_TEXT(mcpResp.contains("result"), name.c_str());
        CHECK_FALSE_TEXT(mcpResp["result"].value("isError", true), name.c_str());
        std::string txt = mcpResp["result"]["content"][0]["text"].get<std::string>();
        json parsed = json::parse(txt, nullptr, false);
        CHECK_FALSE_TEXT(parsed.is_discarded(), name.c_str());
        CHECK_TRUE_TEXT(parsed.value("ok", false), name.c_str());
        return parsed;
    };

    // 1..8: Local CPU Accelerators
    json r1 = callAndParse("eda_calc_schematic_layout", {
        {"components", json::array({
            {{"reference", "J1"}, {"role", "connector"}},
            {{"reference", "U1"}, {"role", "mcu"}}
        })},
        {"nets", json::array({
            {{"name", "SIG"}, {"connections", json::array({"J1.1", "U1.1"})}}
        })}
    });
    json r2 = callAndParse("eda_calc_pcb_placement", {
        {"board", {{"width_mm", 40.0}, {"height_mm", 30.0}}},
        {"components", json::array({
            {{"reference", "U1"}, {"width_mm", 5.0}, {"height_mm", 5.0}},
            {{"reference", "C1"}, {"width_mm", 2.0}, {"height_mm", 1.2}}
        })},
        {"nets", json::array({{{"name", "VCC"}, {"pins", json::array({"U1", "C1"})}}})}
    });
    json r3 = callAndParse("eda_calc_pcb_autoroute", {
        {"board", {{"width_mm", 30.0}, {"height_mm", 20.0}, {"layers", 2}}},
        {"nets", json::array({
            {{"name", "SIG"}, {"pads", json::array({
                {{"x_mm", 5.0}, {"y_mm", 5.0}, {"layer", "F.Cu"}},
                {{"x_mm", 12.0}, {"y_mm", 8.0}, {"layer", "F.Cu"}}
            })}}
        })}
    });
    callAndParse("eda_calc_electrical_synth", {{"mode", "divider"}, {"vin_v", 12.0}, {"vout_target_v", 3.3}});
    callAndParse("eda_calc_pcb_connectivity_audit", {
        {"pads", json::array({
            {{"ref", "U1"}, {"pad", "1"}, {"net", "SIG"}, {"x", 5.0}, {"y", 5.0}, {"layer", "F.Cu"}},
            {{"ref", "C1"}, {"pad", "1"}, {"net", "SIG"}, {"x", 12.0}, {"y", 5.0}, {"layer", "F.Cu"}}
        })},
        {"tracks", json::array({
            {{"net", "SIG"}, {"layer", "F.Cu"}, {"x1", 5.0}, {"y1", 5.0}, {"x2", 12.0}, {"y2", 5.0}, {"width", 0.25}}
        })}
    });
    callAndParse("cad_calc_robot_kinematics_statics", {
        {"links", json::array({{{"name", "L1"}, {"length_m", 0.08}, {"mass_kg", 0.045}, {"angle_deg", 0.0}}})},
        {"payload_kg", 0.10}
    });
    callAndParse("cad_calc_mechanism_synth", {
        {"type", "planetary"}, {"module_mm", 1.0}, {"sun_teeth", 16}, {"planet_teeth", 16}, {"num_planets", 3}
    });
    callAndParse("cad_calc_print_orientation_dfm", {
        {"material", "PETG"},
        {"bounding_box_mm", {{"x", 40.0}, {"y", 20.0}, {"z", 10.0}}},
        {"cross_section_mm2", 160.0},
        {"load_vector_n", {{"fx", 0.0}, {"fy", 0.0}, {"fz", 20.0}}}
    });

    // 9..17: KiCad Tools
    std::string schPath = _ws + "/mcp_board.kicad_sch";
    std::string pcbPath = _ws + "/mcp_board.kicad_pcb";
    std::string fabDir = _ws + "/fab_out";
    std::string stepPath = _ws + "/mcp_board.step";

    callAndParse("kicad_search_library", {{"query", "R_0603"}});
    callAndParse("kicad_edit_schematic", {
        {"schematic_path", schPath},
        {"symbols", r1["symbols"]},
        {"wires", r1["wires"]}
    });
    callAndParse("kicad_inspect_project", {{"schematic_path", schPath}});
    callAndParse("kicad_sync_sch_to_pcb", {{"schematic_path", schPath}, {"pcb_path", pcbPath}});
    callAndParse("kicad_edit_pcb", {
        {"pcb_path", pcbPath},
        {"board_outline", {{"width_mm", 40.0}, {"height_mm", 30.0}}},
        {"placements", r2["placements"]},
        {"tracks", r3["tracks"]},
        {"vias", r3["vias"]}
    });
    callAndParse("kicad_run_erc", {{"schematic_path", schPath}});
    callAndParse("kicad_run_drc", {{"pcb_path", pcbPath}});
    callAndParse("kicad_export_fab_pack", {{"pcb_path", pcbPath}, {"output_dir", fabDir}});
    callAndParse("kicad_export_step", {{"pcb_path", pcbPath}, {"output_path", stepPath}});

    // 18..26: FreeCAD Tools
    std::string fcDoc = _ws + "/robot_bracket.FCStd";
    std::string fcRenders = _ws + "/renders";
    std::string fcExports = _ws + "/prints";

    callAndParse("freecad_live_status", json::object());
    callAndParse("freecad_generate_cad", {
        {"document_path", fcDoc},
        {"body_name", "JointHousing"},
        {"length_mm", 45.0},
        {"width_mm", 25.0},
        {"height_mm", 18.0}
    });
    callAndParse("freecad_inspect_document", {{"document_path", fcDoc}});
    callAndParse("freecad_edit_parameters", {
        {"document_path", fcDoc},
        {"parameters", {{"wall_thickness_mm", 2.4}}}
    });
    callAndParse("freecad_assemble_joint", {{"min_deg", -45.0}, {"max_deg", 45.0}, {"step_deg", 15.0}});
    callAndParse("freecad_analyze_printability", {{"watertight", true}, {"non_manifold_edges", 0}, {"wall_thickness_mm", 2.4}});
    callAndParse("freecad_render_view", {{"output_dir", fcRenders}});
    callAndParse("freecad_export_print_files", {{"output_dir", fcExports}, {"part_name", "joint_housing"}});
    callAndParse("freecad_invoke_api", {{"module", "Part"}, {"method", "makeBox"}});

    // 27..37: EasyEDA Tools
    std::string jlcDir = _ws + "/jlcpcb";
    callAndParse("easyeda_live_status", json::object());
    callAndParse("easyeda_search_library", {{"query", "10k"}});
    callAndParse("easyeda_inspect_schematic", json::object());
    callAndParse("easyeda_edit_schematic", {{"symbols", r1["symbols"]}});
    callAndParse("easyeda_sync_sch_to_pcb", json::object());
    callAndParse("easyeda_inspect_pcb", json::object());
    callAndParse("easyeda_edit_pcb", {{"placements", r2["placements"]}});
    callAndParse("easyeda_run_drc", {{"target", "pcb"}});
    callAndParse("easyeda_export_jlcpcb", {{"output_dir", jlcDir}});
    callAndParse("easyeda_invoke_api", {{"api_path", "eda.sch_PrimitiveComponent.getAll"}});
    callAndParse("easyeda_convert", {
        {"easyeda_json", {{"head", {{"docType", "1"}}}, {"shape", json::array()}}},
        {"target_format", "kicad_sch"}
    });

    // Verify telemetry counters across all 4 components and 37-call ring buffer
    EdaBridgeStatus st = _bridge->getStatus();
    LONGS_EQUAL(37, static_cast<long>(st.recentCalls.size()));
    for (const auto& ti : st.tools) {
        if (ti.name == "accelerators") {
            LONGS_EQUAL(8, static_cast<long>(ti.totalCalls));
            LONGS_EQUAL(8, static_cast<long>(ti.successCalls));
        } else if (ti.name == "kicad") {
            LONGS_EQUAL(9, static_cast<long>(ti.totalCalls));
            LONGS_EQUAL(9, static_cast<long>(ti.successCalls));
        } else if (ti.name == "freecad") {
            LONGS_EQUAL(9, static_cast<long>(ti.totalCalls));
            LONGS_EQUAL(9, static_cast<long>(ti.successCalls));
        } else if (ti.name == "easyeda") {
            LONGS_EQUAL(11, static_cast<long>(ti.totalCalls));
            LONGS_EQUAL(11, static_cast<long>(ti.successCalls));
        }
    }
}

TEST(McpServer_EdaProfile, EasyEdaAndAuditSchemasExposeFilteringDeclarativeEditsAndMilDefaults) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 202},
        {"method", "tools/list"},
        {"params", json::object()}
    };
    json res = _mcp->handleMessage(req, "eda");
    CHECK_TRUE(res.contains("result"));
    CHECK_TRUE(res["result"].contains("tools"));

    std::map<std::string, json> schemasByName;
    for (const auto& t : res["result"]["tools"]) {
        if (t.is_object() && t.contains("name") && t.contains("inputSchema")) {
            schemasByName[t["name"].get<std::string>()] = t["inputSchema"];
        }
    }

    // 1. easyeda_inspect_schematic schema
    CHECK_TRUE(schemasByName.count("easyeda_inspect_schematic") == 1);
    const json& inspSchProps = schemasByName["easyeda_inspect_schematic"]["properties"];
    CHECK_TRUE(inspSchProps.contains("references"));
    CHECK_TRUE(inspSchProps.contains("nets"));
    CHECK_TRUE(inspSchProps.contains("component_types"));
    CHECK_TRUE(inspSchProps.contains("include_pins"));
    CHECK_TRUE(inspSchProps.contains("include_wires"));
    CHECK_TRUE(inspSchProps.contains("summary_only"));

    // 2. easyeda_inspect_pcb schema
    CHECK_TRUE(schemasByName.count("easyeda_inspect_pcb") == 1);
    const json& inspPcbProps = schemasByName["easyeda_inspect_pcb"]["properties"];
    CHECK_TRUE(inspPcbProps.contains("references"));
    CHECK_TRUE(inspPcbProps.contains("nets"));
    CHECK_TRUE(inspPcbProps.contains("include_raw_primitives"));
    CHECK_TRUE(inspPcbProps.contains("summary_only"));

    // 3. easyeda_edit_schematic schema
    CHECK_TRUE(schemasByName.count("easyeda_edit_schematic") == 1);
    const json& editSchProps = schemasByName["easyeda_edit_schematic"]["properties"];
    CHECK_TRUE(editSchProps.contains("units"));
    CHECK_EQUAL("mil", editSchProps["units"].value("default", ""));
    CHECK_TRUE(editSchProps.contains("add_components"));
    CHECK_TRUE(editSchProps.contains("modify_components"));
    CHECK_TRUE(editSchProps.contains("modify_pins"));
    CHECK_TRUE(editSchProps.contains("add_netflags"));
    CHECK_TRUE(editSchProps.contains("delete_primitives"));
    CHECK_TRUE(editSchProps.contains("connect_pins"));

    // 4. easyeda_edit_pcb schema
    CHECK_TRUE(schemasByName.count("easyeda_edit_pcb") == 1);
    const json& editPcbProps = schemasByName["easyeda_edit_pcb"]["properties"];
    CHECK_TRUE(editPcbProps.contains("units"));
    CHECK_EQUAL("mil", editPcbProps["units"].value("default", ""));
    CHECK_TRUE(editPcbProps.contains("delete_primitives"));
    CHECK_TRUE(editPcbProps.contains("modify_pads"));
    CHECK_TRUE(editPcbProps.contains("place_components"));
    CHECK_TRUE(editPcbProps.contains("move_components"));
    CHECK_TRUE(editPcbProps.contains("add_tracks"));
    CHECK_TRUE(editPcbProps.contains("add_vias"));

    // 5. eda_calc_pcb_connectivity_audit schema
    CHECK_TRUE(schemasByName.count("eda_calc_pcb_connectivity_audit") == 1);
    const json& auditProps = schemasByName["eda_calc_pcb_connectivity_audit"]["properties"];
    CHECK_TRUE(auditProps.contains("pours"));
    CHECK_TRUE(auditProps.contains("zones"));
}

TEST_GROUP(WebServer_EdaApi) {
    std::string _ws;
    std::string _oldHome;
    std::unique_ptr<EdaRpcServer> _rpcServer;
    std::unique_ptr<MockEasyEdaPluginClient> _easyEdaPlugin;
    std::unique_ptr<ConfigManager> _cfgMgr;
    std::unique_ptr<EdaBridge> _bridge;
    std::unique_ptr<HistoryStore> _hstore;
    std::unique_ptr<McpServer> _mcp;

    void setup() override {
        _ws = makeTempDir("web_eda");
        const char* h = std::getenv("HOME");
        _oldHome = h ? h : "";
        ::setenv("HOME", _ws.c_str(), 1);

        EdaRpcServerOptions opts;
        opts.listenHost = "127.0.0.1";
        opts.port = 0;
        opts.pluginPort = 0;
        opts.authSecret = "super-secret-eda-key-999";
        opts.allowedRoot = _ws;

        _rpcServer = std::make_unique<EdaRpcServer>(opts);
        CHECK_TRUE(_rpcServer->start());

        _easyEdaPlugin = std::make_unique<MockEasyEdaPluginClient>();
        CHECK_TRUE(_easyEdaPlugin->start(_rpcServer->boundPluginPort()));
        for (int i = 0; i < 30 && !_rpcServer->isPluginConnected("easyeda"); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        _cfgMgr = std::make_unique<ConfigManager>();
        EdaBridgeConfig& edaCfg = _cfgMgr->getConfig().eda;
        edaCfg.enabled = true;
        edaCfg.mode = "client";
        edaCfg.host = "127.0.0.1";
        edaCfg.port = _rpcServer->boundPort();
        edaCfg.pluginPort = _rpcServer->boundPluginPort();
        edaCfg.authSecret = "super-secret-eda-key-999";
        edaCfg.linuxPathPrefix = _ws;
        edaCfg.windowsPathPrefix = _ws;

        _bridge = std::make_unique<EdaBridge>(edaCfg);
        _hstore = std::make_unique<HistoryStore>();
        _hstore->open(_ws + "/history.db");
        _mcp = std::make_unique<McpServer>(StateStore::getInstance());
        _mcp->setEdaBridge(_bridge.get());
    }

    void teardown() override {
        _mcp.reset();
        if (_hstore) {
            _hstore->close();
            _hstore.reset();
        }
        _bridge.reset();
        _cfgMgr.reset();
        if (_easyEdaPlugin) {
            _easyEdaPlugin->stop();
            _easyEdaPlugin.reset();
        }
        if (_rpcServer) {
            _rpcServer->stop();
            _rpcServer.reset();
        }
        if (!_oldHome.empty()) {
            ::setenv("HOME", _oldHome.c_str(), 1);
        }
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(WebServer_EdaApi, AuthGateSecretRedactionPersistenceAndLiveProbe) {
    // 1. Start WebServer with endpointsEnabled = false (default production security posture)
    WebConfig wcfg;
    wcfg.host = "127.0.0.1";
    wcfg.port = 0;
    wcfg.endpointsEnabled = false;

    WebServer server(StateStore::getInstance(), *_hstore, wcfg, nullptr, _mcp.get(), nullptr);
    server.setEdaBridge(_bridge.get());
    server.setConfigManager(_cfgMgr.get());
    CHECK_TRUE(server.start(true));

    httplib::Client cli("127.0.0.1", server.getPort());
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    // Unauthenticated GET /api/eda/status must be rejected with HTTP 403 when endpointsEnabled == false
    auto unauthStatus = cli.Get("/api/eda/status");
    CHECK_TRUE(unauthStatus != nullptr);
    LONGS_EQUAL(403, unauthStatus->status);

    // Unauthenticated POST /api/eda/config and /api/eda/probe must be rejected with HTTP 401
    auto unauthConfig = cli.Post("/api/eda/config", "{\"enabled\":true}", "application/json");
    CHECK_TRUE(unauthConfig != nullptr);
    LONGS_EQUAL(401, unauthConfig->status);

    auto unauthProbe = cli.Post("/api/eda/probe", "{}", "application/json");
    CHECK_TRUE(unauthProbe != nullptr);
    LONGS_EQUAL(401, unauthProbe->status);

    // 2. Obtain a valid UI session cookie from GET /
    auto rootRes = cli.Get("/");
    CHECK_TRUE(rootRes != nullptr);
    LONGS_EQUAL(200, rootRes->status);
    std::string sessionToken = extractSessionCookieToken(*rootRes);
    CHECK_FALSE(sessionToken.empty());

    httplib::Headers uiHeaders = {
        {"X-UI-Session", sessionToken},
        {"Cookie", "aimon_session=" + sessionToken}
    };

    // 3. Authenticated GET /api/eda/status and GET /api/status must redact authSecret ("***")
    auto statusRes = cli.Get("/api/eda/status", uiHeaders);
    CHECK_TRUE(statusRes != nullptr);
    LONGS_EQUAL(200, statusRes->status);
    CHECK_TRUE(statusRes->body.find("super-secret-eda-key-999") == std::string::npos);
    json stJson = json::parse(statusRes->body);
    CHECK_TRUE(stJson.value("auth_configured", false));
    CHECK_EQUAL("***", stJson["config_snapshot"].value("auth_secret", ""));

    auto aggStatusRes = cli.Get("/api/status");
    CHECK_TRUE(aggStatusRes != nullptr);
    LONGS_EQUAL(200, aggStatusRes->status);
    CHECK_TRUE(aggStatusRes->body.find("super-secret-eda-key-999") == std::string::npos);
    json aggJson = json::parse(aggStatusRes->body);
    CHECK_TRUE(aggJson.contains("eda"));
    CHECK_EQUAL("***", aggJson["eda"]["config_snapshot"].value("auth_secret", ""));

    // 4. Authenticated POST /api/eda/config with "auth_secret": "***" preserves existing secret
    // and persists to both ~/.config/aimon/config.json and ~/.config/aimon/aimon.cfg
    json updatePayload = {
        {"enabled", true},
        {"mode", "client"},
        {"host", "127.0.0.1"},
        {"port", _rpcServer->boundPort()},
        {"plugin_port", _rpcServer->boundPluginPort()},
        {"auth_secret", "***"},
        {"linux_path_prefix", _ws},
        {"windows_path_prefix", _ws},
        {"accelerators", {
            {"enabled", true},
            {"sa_placement_iterations", 42000},
            {"default_filament_material", "ASA"}
        }},
        {"kicad", {
            {"enabled", true},
            {"schematic_grid_mils", 50},
            {"fab_target_house", "jlcpcb"}
        }},
        {"freecad", {
            {"enabled", true},
            {"default_print_tolerance_mm", 0.25}
        }}
    };
    auto cfgRes = cli.Post("/api/eda/config", uiHeaders, updatePayload.dump(), "application/json");
    CHECK_TRUE(cfgRes != nullptr);
    LONGS_EQUAL(200, cfgRes->status);
    CHECK_TRUE(cfgRes->body.find("super-secret-eda-key-999") == std::string::npos);
    json cfgRespJson = json::parse(cfgRes->body);
    CHECK_TRUE(cfgRespJson.value("ok", false));
    CHECK_TRUE(cfgRespJson.value("auth_configured", false));
    CHECK_EQUAL("***", cfgRespJson["config_snapshot"].value("auth_secret", ""));
    LONGS_EQUAL(42000, cfgRespJson["config_snapshot"]["accelerators"].value("sa_placement_iterations", 0));
    CHECK_EQUAL("ASA", cfgRespJson["config_snapshot"]["accelerators"].value("default_filament_material", ""));

    // Verify existing authSecret was preserved in EdaBridge and persisted to disk
    CHECK_EQUAL("super-secret-eda-key-999", _bridge->getConfig().authSecret);
    ConfigManager reloadJsonMgr;
    CHECK_TRUE(reloadJsonMgr.load(_ws + "/.config/aimon/config.json"));
    CHECK_EQUAL("super-secret-eda-key-999", reloadJsonMgr.getConfig().eda.authSecret);
    LONGS_EQUAL(42000, reloadJsonMgr.getConfig().eda.accelerators.saPlacementIterations);

    ConfigManager reloadLibCfgMgr;
    CHECK_TRUE(reloadLibCfgMgr.loadLibConfig(_ws + "/.config/aimon/aimon.cfg"));
    CHECK_EQUAL("super-secret-eda-key-999", reloadLibCfgMgr.getConfig().eda.authSecret);
    LONGS_EQUAL(42000, reloadLibCfgMgr.getConfig().eda.accelerators.saPlacementIterations);

    // 5. Fault injection: invalid port or '..' traversal in POST /api/eda/config returns HTTP 400
    json badPortPayload = {{"port", 70000}};
    auto badPortRes = cli.Post("/api/eda/config", uiHeaders, badPortPayload.dump(), "application/json");
    CHECK_TRUE(badPortRes != nullptr);
    LONGS_EQUAL(400, badPortRes->status);

    json badPathPayload = {{"linux_path_prefix", _ws + "/../etc"}};
    auto badPathRes = cli.Post("/api/eda/config", uiHeaders, badPathPayload.dump(), "application/json");
    CHECK_TRUE(badPathRes != nullptr);
    LONGS_EQUAL(400, badPathRes->status);

    // 6. Authenticated POST /api/eda/probe measures live latency and plugin attachment
    auto probeRes = cli.Post("/api/eda/probe", uiHeaders, "{}", "application/json");
    CHECK_TRUE(probeRes != nullptr);
    LONGS_EQUAL(200, probeRes->status);
    json probeJson = json::parse(probeRes->body);
    CHECK_TRUE(probeJson.value("ok", false));
    CHECK_TRUE(probeJson.value("connected", false));
    CHECK_TRUE(probeJson.value("last_latency_ms", -1) >= 0);

    bool foundEasyEdaConnected = false;
    for (const auto& t : probeJson["tools"]) {
        if (t.value("name", "") == "easyeda" && t.value("plugin_connected", false)) {
            foundEasyEdaConnected = true;
        }
    }
    CHECK_TRUE(foundEasyEdaConnected);

    server.stop();
}

TEST(WebServer_EdaApi, EndpointsEnabledTrueStillRequiresAuthForConfigAndProbe) {
    WebConfig wcfg;
    wcfg.host = "127.0.0.1";
    wcfg.port = 0;
    wcfg.endpointsEnabled = true;

    WebServer server(StateStore::getInstance(), *_hstore, wcfg, nullptr, _mcp.get(), nullptr);
    server.setEdaBridge(_bridge.get());
    server.setConfigManager(_cfgMgr.get());
    CHECK_TRUE(server.start(true));

    httplib::Client cli("127.0.0.1", server.getPort());
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    // Even when endpointsEnabled == true, unauthenticated mutation/probe endpoints return 401
    auto unauthCfg = cli.Post("/api/eda/config", "{\"enabled\":false}", "application/json");
    CHECK_TRUE(unauthCfg != nullptr);
    LONGS_EQUAL(401, unauthCfg->status);

    auto unauthProbe = cli.Post("/api/eda/probe", "{}", "application/json");
    CHECK_TRUE(unauthProbe != nullptr);
    LONGS_EQUAL(401, unauthProbe->status);

    server.stop();
}

TEST(WebServer_EdaApi, ServesToolBridgesSubpanelAndWebAssetsMatchDiskByteForByte) {
    WebConfig wcfg;
    wcfg.host = "127.0.0.1";
    wcfg.port = 0;
    wcfg.endpointsEnabled = false;

    WebServer server(StateStore::getInstance(), *_hstore, wcfg, nullptr, _mcp.get(), nullptr);
    server.setEdaBridge(_bridge.get());
    server.setConfigManager(_cfgMgr.get());
    CHECK_TRUE(server.start(true));

    httplib::Client cli("127.0.0.1", server.getPort());
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    // 1. Verify GET / serves the "tool bridges" subnav tab inside #aimon-subnav and #subpanel-bridges markup
    auto htmlRes = cli.Get("/");
    CHECK_TRUE(htmlRes != nullptr);
    LONGS_EQUAL(200, htmlRes->status);
    const std::string& html = htmlRes->body;

    size_t insightsTabPos = html.find("data-subpanel=\"insights\"");
    size_t bridgesTabPos = html.find("data-subpanel=\"bridges\"");
    CHECK_TRUE_TEXT(insightsTabPos != std::string::npos, "Missing data-subpanel=\"insights\" tab");
    CHECK_TRUE_TEXT(bridgesTabPos != std::string::npos, "Missing data-subpanel=\"bridges\" tab");
    CHECK_TRUE_TEXT(bridgesTabPos > insightsTabPos, "tool bridges tab must appear after usage insights tab");
    CHECK_TRUE_TEXT(html.find("<span class=\"subnav-title\">tool bridges</span>") != std::string::npos,
                    "Missing tool bridges subnav title");
    CHECK_TRUE_TEXT(html.find("id=\"subpanel-bridges\"") != std::string::npos,
                    "Missing #subpanel-bridges container");

    // Verify key Global + 4 Component + Activity Log IDs exist in served HTML
    static constexpr const char* kRequiredDomIds[] = {
        "id=\"eda-status-pill\"",
        "id=\"eda-latency-pill\"",
        "id=\"eda-cfg-enabled\"",
        "id=\"eda-cfg-mode\"",
        "id=\"eda-cfg-host\"",
        "id=\"eda-cfg-port\"",
        "id=\"eda-cfg-plugin-port\"",
        "id=\"eda-cfg-linux-prefix\"",
        "id=\"eda-cfg-win-prefix\"",
        "id=\"eda-cfg-auth-secret\"",
        "id=\"eda-cfg-poll-interval\"",
        "id=\"eda-cfg-require-approval\"",
        "id=\"btn-eda-probe\"",
        "id=\"btn-eda-save\"",
        "id=\"eda-card-accelerators\"",
        "id=\"eda-card-easyeda\"",
        "id=\"eda-card-kicad\"",
        "id=\"eda-card-freecad\"",
        "id=\"eda-acc-enabled\"",
        "id=\"eda-easyeda-enabled\"",
        "id=\"eda-kicad-enabled\"",
        "id=\"eda-freecad-enabled\"",
        "id=\"eda-recent-calls-body\""
    };
    for (const char* domId : kRequiredDomIds) {
        CHECK_TRUE_TEXT(html.find(domId) != std::string::npos, domId);
    }

    // 2. Verify GET /style.css serves #subpanel-bridges styling
    auto cssRes = cli.Get("/style.css");
    CHECK_TRUE(cssRes != nullptr);
    LONGS_EQUAL(200, cssRes->status);
    CHECK_TRUE_TEXT(cssRes->body.find("#subpanel-bridges") != std::string::npos,
                    "Missing #subpanel-bridges in style.css");

    // 3. Verify GET /app.js serves renderToolBridges, saveToolBridgesConfig, and probeToolBridges
    auto jsRes = cli.Get("/app.js");
    CHECK_TRUE(jsRes != nullptr);
    LONGS_EQUAL(200, jsRes->status);
    CHECK_TRUE_TEXT(jsRes->body.find("renderToolBridges") != std::string::npos,
                    "Missing renderToolBridges in app.js");
    CHECK_TRUE_TEXT(jsRes->body.find("saveToolBridgesConfig") != std::string::npos,
                    "Missing saveToolBridgesConfig in app.js");
    CHECK_TRUE_TEXT(jsRes->body.find("probeToolBridges") != std::string::npos,
                    "Missing probeToolBridges in app.js");

    server.stop();

    // 4. Verify byte-for-byte parity between web/* files and include/WebAssets.hxx
    auto readDiskFile = [](const std::string& relPath) -> std::string {
        std::ifstream ifs(relPath, std::ios::binary);
        if (!ifs.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    };

    std::string diskHtml = readDiskFile("web/index.html");
    std::string diskCss = readDiskFile("web/style.css");
    std::string diskJs = readDiskFile("web/app.js");
    std::string diskQr = readDiskFile("web/qrcode.js");

    CHECK_FALSE(diskHtml.empty());
    CHECK_FALSE(diskCss.empty());
    CHECK_FALSE(diskJs.empty());
    CHECK_FALSE(diskQr.empty());

    CHECK_TRUE_TEXT(diskHtml == std::string(assets::INDEX_HTML), "web/index.html != WebAssets::INDEX_HTML");
    CHECK_TRUE_TEXT(diskCss == std::string(assets::STYLE_CSS), "web/style.css != WebAssets::STYLE_CSS");
    CHECK_TRUE_TEXT(diskJs == std::string(assets::APP_JS), "web/app.js != WebAssets::APP_JS");
    CHECK_TRUE_TEXT(diskQr == std::string(assets::QRCODE_JS), "web/qrcode.js != WebAssets::QRCODE_JS");
}

int main(int argc, char** argv) {
    MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--negative-control") == 0) {
            std::string ws = makeTempDir("neg_ctrl");
            HistoryStore hstore;
            hstore.open(ws + "/history.db");
            McpServer mcp(StateStore::getInstance());

            // 1. Verify kicad_run_erc is rejected under "core" profile with -32601
            json mcpErr = callMcpTool(mcp, "kicad_run_erc", {{"schematic_path", ws + "/a.kicad_sch"}}, "core");
            bool profileRejected = mcpErr.contains("error") && mcpErr["error"].value("code", 0) == -32601;

            // 2. Verify unauthenticated /api/eda/config is rejected with HTTP 401 even when endpointsEnabled == true
            WebConfig wcfg;
            wcfg.host = "127.0.0.1";
            wcfg.port = 0;
            wcfg.endpointsEnabled = true;

            WebServer server(StateStore::getInstance(), hstore, wcfg, nullptr, &mcp, nullptr);
            bool httpRejected = false;
            if (server.start(true)) {
                httplib::Client cli("127.0.0.1", server.getPort());
                cli.set_connection_timeout(2);
                cli.set_read_timeout(5);
                auto res = cli.Post("/api/eda/config", "{\"enabled\":true}", "application/json");
                if (res && res->status == 401) {
                    httpRejected = true;
                }
                server.stop();
            }
            hstore.close();
            std::error_code ec;
            fs::remove_all(ws, ec);

            if (profileRejected && httpRejected) {
                std::cerr << "[NegativeControl] Unauthenticated /api/eda/config (HTTP 401) and disallowed "
                          << "profile tool call (-32601) rejected as expected.\n";
                return 1;
            }
            return 0;
        }
    }

    return CommandLineTestRunner::RunAllTests(argc, argv);
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
