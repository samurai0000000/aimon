/*
 * TestEdaBridgeDaemon.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "EdaBridge.hxx"
#include "EdaAccelerators.hxx"
#include "EasyEdaConverter.hxx"
#include "KiCadSchematicEditor.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "ProcessLauncher.hxx"
#include "ToolDiscovery.hxx"
#include "EdaRpcServer.hxx"
#include "EdaBridgeAppConfig.hxx"
#include "PathUtils.hxx"

#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

namespace fs = std::filesystem;
using namespace aimon;

namespace {

std::string makeTempWorkspace(const std::string& tag) {
    fs::path base = fs::temp_directory_path() / ("aimon_eda_test_" + tag + "_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base.string();
}

// Simulated EasyEDA Pro WebSocket plugin client connecting to AppPluginRelay (127.0.0.1:pluginPort)
class MockWebSocketEasyEdaPlugin {
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

        // Read HTTP 101 response
        char hdrBuf[1024];
        ssize_t n = ::recv(_sock, hdrBuf, sizeof(hdrBuf) - 1, 0);
        if (n <= 0) {
            ::close(_sock);
            _sock = -1;
            return false;
        }
        hdrBuf[n] = '\0';
        char* hdrEnd = std::strstr(hdrBuf, "\r\n\r\n");
        if (std::strstr(hdrBuf, "101 Switching Protocols") == nullptr || hdrEnd == nullptr) {
            ::close(_sock);
            _sock = -1;
            return false;
        }
        size_t hdrBytes = static_cast<size_t>((hdrEnd - hdrBuf) + 4);
        if (static_cast<size_t>(n) > hdrBytes) {
            _initialBuf.assign(reinterpret_cast<const uint8_t*>(hdrBuf + hdrBytes),
                               reinterpret_cast<const uint8_t*>(hdrBuf + n));
        }

        _running.store(true);

        // Send masked WebSocket registration frame
        nlohmann::json reg = {
            {"jsonrpc", "2.0"},
            {"id", "ws-reg-1"},
            {"method", "plugin/register"},
            {"params", {
                {"app", "easyeda"},
                {"version", "2.2.0"},
                {"active_document", "ControllerBoard.epro"}
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

    ~MockWebSocketEasyEdaPlugin() {
        stop();
    }

private:
    void loop() {
        std::vector<uint8_t> buf = _initialBuf;
        bool needRecv = buf.empty();
        uint8_t chunk[4096];
        while (_running.load()) {
            if (needRecv) {
                ssize_t n = ::recv(_sock, chunk, sizeof(chunk), 0);
                if (n <= 0) break;
                buf.insert(buf.end(), chunk, chunk + n);
            }
            needRecv = true;

            while (!buf.empty()) {
                size_t consumed = 0;
                uint8_t opcode = 0;
                std::string payload;
                std::string err;
                if (!EdaRpcServer::decodeWebSocketFrame(buf.data(), buf.size(), consumed, opcode, payload, err)) {
                    break;
                }
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

                nlohmann::json msg = nlohmann::json::parse(payload, nullptr, false);
                if (msg.is_discarded() || !msg.is_object()) continue;
                if (msg.value("method", "") == "tools/call") {
                    nlohmann::json id = msg["id"];
                    std::string tName = msg["params"].value("name", "");
                    nlohmann::json tArgs = msg["params"].value("arguments", nlohmann::json::object());

                    nlohmann::json res = {
                        {"ok", true},
                        {"tool", tName},
                        {"active_document", "ControllerBoard.epro"}
                    };
                    if (tName == "easyeda_search_library") {
                        res["devices"] = nlohmann::json::array({
                            {{"lcsc", "C25804"}, {"title", "10k 0603 Resistor"}, {"library_type", "Basic"}, {"stock", 50000}}
                        });
                    } else if (tName == "easyeda_inspect_schematic") {
                        res["components"] = nlohmann::json::array({{{"reference", "R1"}, {"value", "10k"}}});
                        res["wires"] = nlohmann::json::array();
                    } else if (tName == "easyeda_edit_schematic") {
                        res["placed_components"] = tArgs.contains("symbols") ? tArgs["symbols"].size() : 1;
                    } else if (tName == "easyeda_sync_sch_to_pcb") {
                        res["synced"] = true;
                    } else if (tName == "easyeda_inspect_pcb") {
                        res["components_count"] = 2;
                        res["unrouted_nets"] = nlohmann::json::array();
                    } else if (tName == "easyeda_edit_pcb") {
                        res["updated_placements"] = 2;
                    } else if (tName == "easyeda_run_drc") {
                        res["violation_count"] = 0;
                        res["violations"] = nlohmann::json::array();
                    } else if (tName == "easyeda_export_jlcpcb") {
                        res["gerber_data"] = "PK\x03\x04JLCPCB_ARCHIVE";
                        res["bom_csv"] = "Comment,Designator,Footprint,LCSC Part #\n10k,R1,0603,C25804\n";
                        res["cpl_csv"] = "Designator,Mid X,Mid Y,Layer,Rotation\nR1,10mm,10mm,Top,0\n";
                        res["dsn_content"] = "(pcb \"easyeda.dsn\")\n";
                    } else if (tName == "easyeda_invoke_api") {
                        res["api_path"] = tArgs.value("api_path", "");
                        res["invoked"] = true;
                    }

                    nlohmann::json reply = {
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
    std::vector<uint8_t> _initialBuf;
    std::atomic<bool> _running{false};
    std::thread _thread;
};

} // anonymous namespace

TEST_GROUP(EasyEdaConverter_Unit) {
};

TEST(EasyEdaConverter_Unit, ConvertsSchematicAndPcbOn50MilGrid) {
    nlohmann::json schDoc = {
        {"head", {{"docType", "1"}}},
        {"shape", nlohmann::json::array({
            "LIB~100~100~Reference`R1`Value`10k`package`R_0603`LCSC`C25804~0#@$P~show~0~1~110~100~0~p1",
            "W~110 100 150 100~#008800~1",
            "N~150~100~0~#0000ff~VCC_3V3"
        })}
    };

    auto schRes = EasyEdaConverter::convertJson(schDoc, "kicad_sch");
    CHECK_TRUE(schRes.ok);
    CHECK_EQUAL("kicad_sch", schRes.format);
    CHECK_EQUAL(1, static_cast<int>(schRes.components.size()));
    CHECK_EQUAL("R1", schRes.components[0].reference);
    CHECK_EQUAL("C25804", schRes.components[0].lcsc);
    DOUBLES_EQUAL(25.4, schRes.components[0].xMm, 1e-6);
    DOUBLES_EQUAL(27.94, schRes.components[0].pins[0].xMm, 1e-6);
    DOUBLES_EQUAL(27.94, schRes.wires[0].x1Mm, 1e-6);

    nlohmann::json pcbDoc = {
        {"head", {{"docType", "3"}}},
        {"board_outline", {{"width_mm", 45.0}, {"height_mm", 35.0}}},
        {"shape", nlohmann::json::array({
            "TRACK~1~1~VCC_3V3~40 40 80 40~trk1",
            "VIA~80~40~2.4~VCC_3V3~1.2~via1"
        })}
    };
    auto pcbRes = EasyEdaConverter::convertJson(pcbDoc, "kicad_pcb");
    CHECK_TRUE(pcbRes.ok);
    CHECK_EQUAL("kicad_pcb", pcbRes.format);
    CHECK_EQUAL(1, static_cast<int>(pcbRes.tracks.size()));
    CHECK_EQUAL(1, static_cast<int>(pcbRes.vias.size()));
    DOUBLES_EQUAL(45.0, pcbRes.outlineWidthMm, 1e-6);
}

TEST_GROUP(KiCadSchematicEditor_Unit) {
    std::string _ws;

    void setup() override {
        _ws = makeTempWorkspace("kicad_unit");
    }

    void teardown() override {
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(KiCadSchematicEditor_Unit, EndToEndLibrarySearchSchematicEditSyncAndPcbLayout) {
    // 1. Create a custom .kicad_sym and .kicad_mod in workspace and search
    std::string symFile = _ws + "/CustomSensor.kicad_sym";
    {
        std::ofstream ofs(symFile);
        ofs << "(kicad_symbol_lib (version 20231120)\n"
            << "  (symbol \"IMU_BMI270\"\n"
            << "    (property \"Description\" \"6-axis IMU sensor\")\n"
            << "    (symbol \"IMU_BMI270_1_1\"\n"
            << "      (pin bidirectional line (at -2.54 0 0) (length 2.54) (name \"SDA\") (number \"1\"))\n"
            << "      (pin input line (at 2.54 0 0) (length 2.54) (name \"SCL\") (number \"2\"))\n"
            << "    )\n"
            << "  )\n"
            << ")\n";
    }

    nlohmann::json libRes = KiCadSchematicEditor::searchLibraries({
        {"query", "BMI270"},
        {"symbol_lib_dir", _ws}
    });
    CHECK_TRUE(libRes.value("ok", false));
    CHECK_TRUE(libRes.value("symbols_count", 0) >= 1);

    // 2. Author schematic on 50-mil grid
    std::string schPath = _ws + "/board.kicad_sch";
    nlohmann::json schRes = KiCadSchematicEditor::applySchematicEdits({
        {"schematic_path", schPath},
        {"add_components", nlohmann::json::array({
            {{"reference", "R1"}, {"value", "4.7k"}, {"lib_id", "Device:R"}, {"footprint", "Resistor_SMD:R_0603_1608Metric"}, {"lcsc", "C25900"}, {"x", 25.4}, {"y", 25.4}},
            {{"reference", "R2"}, {"value", "4.7k"}, {"lib_id", "Device:R"}, {"footprint", "Resistor_SMD:R_0603_1608Metric"}, {"lcsc", "C25900"}, {"x", 50.8}, {"y", 25.4}}
        })},
        {"add_wires", nlohmann::json::array({
            {{"x1", 27.94}, {"y1", 25.4}, {"x2", 48.26}, {"y2", 25.4}, {"net", "I2C_SDA"}}
        })},
        {"add_power_ports", nlohmann::json::array({
            {{"name", "GND"}, {"x", 22.86}, {"y", 25.4}}
        })}
    });
    CHECK_TRUE(schRes.value("ok", false));
    CHECK_EQUAL(2, schRes.value("components_count", 0));

    // 3. Synchronize schematic to PCB
    std::string pcbPath = _ws + "/board.kicad_pcb";
    nlohmann::json syncRes = KiCadSchematicEditor::syncSchematicToPcb({
        {"schematic_path", schPath},
        {"pcb_path", pcbPath}
    });
    CHECK_TRUE(syncRes.value("ok", false));
    CHECK_EQUAL(2, syncRes.value("synced_components", 0));
    CHECK_EQUAL(1, syncRes.value("unrouted_nets_count", 0));

    // 4. Edit PCB outline, placements, zones, mounting holes, and route the net
    nlohmann::json pcbEditRes = KiCadSchematicEditor::applyPcbEdits({
        {"pcb_path", pcbPath},
        {"board_outline", {{"width_mm", 40.0}, {"height_mm", 30.0}}},
        {"placements", nlohmann::json::array({
            {{"reference", "R1"}, {"x_mm", 12.0}, {"y_mm", 15.0}, {"rotation_deg", 0.0}, {"layer", "F.Cu"}},
            {{"reference", "R2"}, {"x_mm", 25.0}, {"y_mm", 15.0}, {"rotation_deg", 90.0}, {"layer", "B.Cu"}}
        })},
        {"mounting_holes", nlohmann::json::array({
            {{"x", 3.0}, {"y", 3.0}, {"drill_mm", 3.2}}
        })},
        {"zones", nlohmann::json::array({
            {{"net", "GND"}, {"layer", "B.Cu"}, {"width_mm", 40.0}, {"height_mm", 30.0}}
        })},
        {"tracks", nlohmann::json::array({
            {{"x1", 12.8}, {"y1", 15.0}, {"x2", 25.0}, {"y2", 15.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "I2C_SDA"}}
        })},
        {"vias", nlohmann::json::array({
            {{"x", 25.0}, {"y", 15.0}, {"size_mm", 0.6}, {"drill_mm", 0.3}, {"net", "I2C_SDA"}}
        })}
    });
    CHECK_TRUE(pcbEditRes.value("ok", false));
    CHECK_EQUAL(0, pcbEditRes.value("unrouted_nets_count", -1));
    CHECK_EQUAL(1, pcbEditRes.value("zones_count", 0));
    CHECK_EQUAL(1, pcbEditRes.value("tracks_count", 0));
    CHECK_EQUAL(1, pcbEditRes.value("vias_count", 0));
}

TEST_GROUP(EdaBridge_ProcessLauncher) {
};

TEST(EdaBridge_ProcessLauncher, TruncatesStdoutAtCapAndEnforcesTimeout) {
    // 1. Test output cap truncation (> 4 MiB output capped at exactly 4 MiB)
    ProcessResult bigOut = ProcessLauncher::run(
        "/usr/bin/python3",
        {"-c", "import sys; sys.stdout.write('A' * (4 * 1024 * 1024 + 65536))"},
        10000,
        ProcessLauncher::kDefaultMaxOutputBytes);
    CHECK_TRUE(bigOut.launched);
    CHECK_TRUE(bigOut.truncated);
    CHECK_EQUAL(ProcessLauncher::kDefaultMaxOutputBytes, bigOut.stdoutText.size());

    // 2. Test hard timeout kills hung child cleanly
    ProcessResult hung = ProcessLauncher::run(
        "/usr/bin/python3",
        {"-c", "import time; time.sleep(10)"},
        150);
    CHECK_TRUE(hung.timedOut);
    CHECK_FALSE(hung.errorMessage.empty());
}

TEST_GROUP(EdaBridge_PluginRelay) {
};

TEST(EdaBridge_PluginRelay, WebSocketFramingAcross7Bit16BitAnd64BitPayloads) {
    // Verify RFC 6455 Sec-WebSocket-Accept test vector
    CHECK_EQUAL("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                EdaRpcServer::computeWebSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="));

    const std::vector<size_t> kSizes = {50, 125, 126, 1024, 65535, 65536, 70000};
    for (size_t sz : kSizes) {
        std::string original(sz, static_cast<char>('A' + (sz % 23)));
        auto encoded = EdaRpcServer::encodeWebSocketFrame(original, 0x1, true);
        size_t consumed = 0;
        uint8_t opcode = 0;
        std::string decoded;
        std::string err;
        CHECK_TRUE(EdaRpcServer::decodeWebSocketFrame(
            encoded.data(), encoded.size(), consumed, opcode, decoded, err));
        CHECK_EQUAL(encoded.size(), consumed);
        CHECK_EQUAL(0x1, static_cast<int>(opcode));
        CHECK_EQUAL(original.size(), decoded.size());
        CHECK_TRUE(original == decoded);
    }
}

TEST_GROUP(EdaBridge_LoopbackRpc) {
    std::string _ws;
    std::unique_ptr<EdaRpcServer> _server;
    std::unique_ptr<MockWebSocketEasyEdaPlugin> _easyEdaPlugin;
    EdaBridgeConfig _cfg;

    void setup() override {
        _ws = makeTempWorkspace("loopback_rpc");

        EdaRpcServerOptions opts;
        opts.listenHost = "127.0.0.1";
        opts.port = 0;       // ephemeral RPC port
        opts.pluginPort = 0; // ephemeral plugin port
        opts.authSecret = "test-secret-token-2026";
        opts.allowedRoot = _ws;

        _server = std::make_unique<EdaRpcServer>(opts);
        CHECK_TRUE(_server->start());

        _cfg.enabled = true;
        _cfg.mode = "client";
        _cfg.host = "127.0.0.1";
        _cfg.port = _server->boundPort();
        _cfg.pluginPort = _server->boundPluginPort();
        _cfg.authSecret = "test-secret-token-2026";
        _cfg.linuxPathPrefix = _ws;
        _cfg.windowsPathPrefix = _ws;
    }

    void teardown() override {
        if (_easyEdaPlugin) {
            _easyEdaPlugin->stop();
            _easyEdaPlugin.reset();
        }
        if (_server) {
            _server->stop();
            _server.reset();
        }
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(EdaBridge_LoopbackRpc, All36ToolsExecuteEndToEndAndEnforceFaultInjection) {
    EdaBridge bridge(_cfg);

    // 1. Before EasyEDA plugin connects, live EasyEDA GUI tools return clear structured error
    nlohmann::json disconnRes = bridge.callTool("easyeda_edit_schematic", {{"symbols", nlohmann::json::array()}});
    CHECK_FALSE(disconnRes.value("ok", true));
    CHECK_TRUE(disconnRes.value("error", "").find("not connected") != std::string::npos);

    nlohmann::json disconnExport = bridge.callTool("easyeda_export_jlcpcb", {{"output_dir", _ws + "/jlc"}});
    CHECK_FALSE(disconnExport.value("ok", true));

    // 2. Connect simulated EasyEDA Pro WebSocket plugin on 127.0.0.1:pluginPort
    _easyEdaPlugin = std::make_unique<MockWebSocketEasyEdaPlugin>();
    CHECK_TRUE(_easyEdaPlugin->start(_server->boundPluginPort()));

    for (int retry = 0; retry < 30 && !_server->isPluginConnected("easyeda"); ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_TRUE(_server->isPluginConnected("easyeda"));

    // 3. Probe live status
    EdaBridgeStatus st = bridge.probeNow();
    CHECK_TRUE(st.connected);
    CHECK_EQUAL("client", st.activeTransport);

    // 4. Execute all 7 local C++17 CPU accelerators (Tools 1..7)
    nlohmann::json t1 = bridge.callTool("eda_calc_schematic_layout", {
        {"components", nlohmann::json::array({
            {{"reference", "J1"}, {"role", "connector"}},
            {{"reference", "U1"}, {"role", "mcu"}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "UART_TX"}, {"connections", nlohmann::json::array({"J1.1", "U1.1"})}}
        })}
    });
    CHECK_TRUE(t1.value("ok", false));

    nlohmann::json t2 = bridge.callTool("eda_calc_pcb_placement", {
        {"board", {{"width_mm", 40.0}, {"height_mm", 30.0}}},
        {"components", nlohmann::json::array({
            {{"reference", "U1"}, {"width_mm", 5.0}, {"height_mm", 5.0}},
            {{"reference", "C1"}, {"width_mm", 2.0}, {"height_mm", 1.2}, {"decouples", "U1"}}
        })},
        {"nets", nlohmann::json::array({
            {{"name", "VCC"}, {"pins", nlohmann::json::array({"U1", "C1"})}}
        })}
    });
    CHECK_TRUE(t2.value("ok", false));

    nlohmann::json t3 = bridge.callTool("eda_calc_pcb_autoroute", {
        {"board", {{"width_mm", 30.0}, {"height_mm", 20.0}, {"layers", 2}}},
        {"nets", nlohmann::json::array({
            {{"name", "SIG1"}, {"pads", nlohmann::json::array({
                {{"x_mm", 5.0}, {"y_mm", 5.0}, {"layer", "F.Cu"}},
                {{"x_mm", 15.0}, {"y_mm", 10.0}, {"layer", "F.Cu"}}
            })}}
        })}
    });
    CHECK_TRUE(t3.value("ok", false));

    nlohmann::json t4 = bridge.callTool("eda_calc_electrical_synth", {
        {"mode", "divider"}, {"vin_v", 12.0}, {"vout_target_v", 3.3}
    });
    CHECK_TRUE(t4.value("ok", false));

    nlohmann::json t5 = bridge.callTool("cad_calc_robot_kinematics_statics", {
        {"links", nlohmann::json::array({{{"name", "L1"}, {"length_m", 0.08}, {"mass_kg", 0.045}, {"angle_deg", 0.0}}})},
        {"payload_kg", 0.10}
    });
    CHECK_TRUE(t5.value("ok", false));

    nlohmann::json t6 = bridge.callTool("cad_calc_mechanism_synth", {
        {"type", "planetary"}, {"module_mm", 1.0}, {"sun_teeth", 16}, {"planet_teeth", 16}, {"num_planets", 3}
    });
    CHECK_TRUE(t6.value("ok", false));

    nlohmann::json t7 = bridge.callTool("cad_calc_print_orientation_dfm", {
        {"material", "PETG"},
        {"bounding_box_mm", {{"x", 50.0}, {"y", 25.0}, {"z", 12.0}}},
        {"cross_section_mm2", 180.0},
        {"section_modulus_mm3", 360.0},
        {"load_vector_n", {{"fx", 0.0}, {"fy", 0.0}, {"fz", 25.0}}},
        {"bearing_bore_axis", {{"x", 0.0}, {"y", 0.0}, {"z", 1.0}}}
    });
    CHECK_TRUE(t7.value("ok", false));

    // 5. Execute all 9 KiCad tools (Tools 8..16)
    std::string schPath = _ws + "/robot_ctrl.kicad_sch";
    std::string pcbPath = _ws + "/robot_ctrl.kicad_pcb";
    std::string fabDir = _ws + "/fab";
    std::string stepPath = _ws + "/robot_ctrl.step";

    CHECK_TRUE(bridge.callTool("kicad_search_library", {{"query", "R_0603"}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_edit_schematic", {
        {"schematic_path", schPath},
        {"symbols", t1["symbols"]},
        {"wires", t1["wires"]}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_inspect_project", {{"schematic_path", schPath}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_sync_sch_to_pcb", {
        {"schematic_path", schPath}, {"pcb_path", pcbPath}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_edit_pcb", {
        {"pcb_path", pcbPath},
        {"board_outline", {{"width_mm", 40.0}, {"height_mm", 30.0}}},
        {"placements", t2["placements"]},
        {"tracks", t3["tracks"]},
        {"vias", t3["vias"]}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_run_erc", {{"schematic_path", schPath}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_run_drc", {{"pcb_path", pcbPath}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_export_fab_pack", {
        {"pcb_path", pcbPath}, {"output_dir", fabDir}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("kicad_export_step", {
        {"pcb_path", pcbPath}, {"output_path", stepPath}
    }).value("ok", false));
    CHECK_TRUE(fs::exists(stepPath));

    // 6. Execute all 9 FreeCAD tools (Tools 17..25)
    std::string fcDocPath = _ws + "/arm_joint.FCStd";
    std::string fcRenderDir = _ws + "/renders";
    std::string fcPrintDir = _ws + "/print_out";

    CHECK_TRUE(bridge.callTool("freecad_live_status", nlohmann::json::object()).value("ok", false));
    CHECK_TRUE(bridge.callTool("freecad_generate_cad", {
        {"document_path", fcDocPath},
        {"body_name", "ShoulderBracket"},
        {"length_mm", 50.0},
        {"width_mm", 30.0},
        {"height_mm", 20.0}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("freecad_inspect_document", {{"document_path", fcDocPath}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("freecad_edit_parameters", {
        {"document_path", fcDocPath},
        {"parameters", {{"wall_thickness_mm", 2.8}, {"print_tolerance_mm", 0.20}}}
    }).value("ok", false));

    // Joint sweep: clean clearance vs obstacle collision at +35 deg
    nlohmann::json sweepClean = bridge.callTool("freecad_assemble_joint", {
        {"min_deg", -90.0}, {"max_deg", 90.0}, {"step_deg", 5.0}, {"link_length_mm", 60.0}
    });
    CHECK_TRUE(sweepClean.value("ok", false));
    CHECK_FALSE(sweepClean.value("collision_detected", true));

    nlohmann::json sweepCollide = bridge.callTool("freecad_assemble_joint", {
        {"min_deg", -90.0}, {"max_deg", 90.0}, {"step_deg", 5.0},
        {"link_length_mm", 60.0}, {"obstacle_angle_deg", 35.0}, {"obstacle_span_deg", 5.0}, {"obstacle_distance_mm", 40.0}
    });
    CHECK_TRUE(sweepCollide.value("ok", false));
    CHECK_TRUE(sweepCollide.value("collision_detected", false));
    DOUBLES_EQUAL(30.0, sweepCollide.value("first_collision_angle_deg", 0.0), 1e-6);

    // Printability: watertight vs non-manifold
    nlohmann::json printGood = bridge.callTool("freecad_analyze_printability", {
        {"watertight", true}, {"non_manifold_edges", 0}, {"wall_thickness_mm", 2.4}
    });
    CHECK_TRUE(printGood.value("printability_pass", false));

    nlohmann::json printBad = bridge.callTool("freecad_analyze_printability", {
        {"watertight", false}, {"non_manifold_edges", 6}, {"wall_thickness_mm", 0.8}
    });
    CHECK_FALSE(printBad.value("printability_pass", true));

    CHECK_TRUE(bridge.callTool("freecad_render_view", {{"output_dir", fcRenderDir}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("freecad_export_print_files", {
        {"output_dir", fcPrintDir}, {"part_name", "shoulder_link"}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("freecad_invoke_api", {
        {"module", "Part"}, {"method", "makeBox"}
    }).value("ok", false));

    // 7. Execute all 11 EasyEDA tools (Tools 26..36)
    std::string jlcDir = _ws + "/jlcpcb_out";
    CHECK_TRUE(bridge.callTool("easyeda_live_status", nlohmann::json::object()).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_search_library", {{"query", "10k"}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_inspect_schematic", nlohmann::json::object()).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_edit_schematic", {{"symbols", t1["symbols"]}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_sync_sch_to_pcb", nlohmann::json::object()).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_inspect_pcb", nlohmann::json::object()).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_edit_pcb", {{"placements", t2["placements"]}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_run_drc", {{"target", "pcb"}}).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_export_jlcpcb", {{"output_dir", jlcDir}}).value("ok", false));
    CHECK_TRUE(fs::exists(jlcDir + "/jlcpcb_gerber.zip"));
    CHECK_TRUE(bridge.callTool("easyeda_invoke_api", {
        {"api_path", "eda.sch_PrimitiveComponent.getAll"}
    }).value("ok", false));
    CHECK_TRUE(bridge.callTool("easyeda_convert", {
        {"easyeda_json", {{"head", {{"docType", "1"}}}, {"shape", nlohmann::json::array()}}},
        {"target_format", "kicad_sch"}
    }).value("ok", false));

    // 8. Fault injection: TNP Face4 script, disallowed API namespaces, and wrong bearer token (-32001)
    nlohmann::json tnpReject = bridge.callTool("freecad_generate_cad", {
        {"script", "import Part\nchamfer = body.Shape.Face4\n"}
    });
    CHECK_FALSE(tnpReject.value("ok", true));

    nlohmann::json fcBadApi = bridge.callTool("freecad_invoke_api", {
        {"module", "os"}, {"method", "system"}
    });
    CHECK_FALSE(fcBadApi.value("ok", true));

    nlohmann::json eeBadApi = bridge.callTool("easyeda_invoke_api", {
        {"api_path", "window.eval"}
    });
    CHECK_FALSE(eeBadApi.value("ok", true));

    EdaBridgeConfig badAuthCfg = _cfg;
    badAuthCfg.authSecret = "wrong-token";
    EdaBridge badAuthBridge(badAuthCfg);
    nlohmann::json authErrRes = badAuthBridge.callTool("kicad_search_library", {{"query", "R"}});
    CHECK_FALSE(authErrRes.value("ok", true));
    CHECK_EQUAL(-32001, authErrRes.value("error_code", 0));
}

TEST_GROUP(EdaBridge_GatewayReverse) {
    std::string _ws;
    std::unique_ptr<DynamicToolRegistry> _registry;
    std::unique_ptr<TcpGateway> _gateway;
    std::unique_ptr<EdaRpcServer> _server;

    void setup() override {
        _ws = makeTempWorkspace("gw_reverse");
        _registry = std::make_unique<DynamicToolRegistry>();
        _gateway = std::make_unique<TcpGateway>(*_registry);
    }

    void teardown() override {
        if (_server) {
            _server->disconnectGateway();
            for (int i = 0; i < 60 && _registry && _registry->getToolClientId("kicad_search_library") >= 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            _server->stop();
            _server.reset();
        }
        if (_gateway) {
            _gateway->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            _gateway.reset();
        }
        if (_registry) {
            _registry.reset();
        }
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(EdaBridge_GatewayReverse, ReverseConnectRegisters29RemoteToolsAndExecutesViaGateway) {
    // Bind TcpGateway on a local test port
    int gwPort = 39885 + (::getpid() % 5000);
    if (!_gateway->start("127.0.0.1", gwPort)) {
        gwPort += 137;
        CHECK_TRUE(_gateway->start("127.0.0.1", gwPort));
    }

    EdaRpcServerOptions opts;
    opts.listenHost = "127.0.0.1";
    opts.port = 0;
    opts.pluginPort = 0;
    opts.authSecret = "gw-secret-2026";
    opts.allowedRoot = _ws;

    _server = std::make_unique<EdaRpcServer>(opts);
    CHECK_TRUE(_server->start());
    CHECK_TRUE(_server->connectToGateway("127.0.0.1", gwPort));

    for (int i = 0; i < 40 && _registry->getToolClientId("kicad_search_library") < 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK_TRUE(_registry->getToolClientId("kicad_search_library") >= 0);

    EdaBridgeConfig cfg;
    cfg.enabled = true;
    cfg.mode = "gateway";
    cfg.authSecret = "gw-secret-2026";
    cfg.linuxPathPrefix = _ws;
    cfg.windowsPathPrefix = _ws;

    EdaBridge bridge(cfg);
    bridge.attachTcpGateway(_gateway.get());

    EdaBridgeStatus st = bridge.probeNow();
    CHECK_TRUE(st.connected);
    CHECK_EQUAL("gateway", st.activeTransport);

    nlohmann::json res = bridge.callTool("kicad_search_library", {{"query", "ESP32"}});
    CHECK_TRUE(res.value("ok", false));
    CHECK_TRUE(res.value("symbols_count", 0) >= 1);
}

TEST_GROUP(EdaBridge_UnifiedAppConfigAndToggles) {
    std::string _ws;

    void setup() override {
        _ws = makeTempWorkspace("unified_app_cfg");
    }

    void teardown() override {
        std::error_code ec;
        fs::remove_all(_ws, ec);
    }
};

TEST(EdaBridge_UnifiedAppConfigAndToggles, RoundTripSettingsSaveLoadAndServerOptions) {
    CHECK_FALSE(EdaBridgeAppConfig::defaultConfigFilePath().empty());
    CHECK_EQUAL("", EdaBridgeAppSettings().allowedRoot);
    CHECK_EQUAL("", EdaBridgeAppSettings().kicadPluginDir);
    CHECK_EQUAL("", EdaBridgeAppSettings().freecadModDir);

    EdaBridgeAppSettings orig;
    orig.version = "1.0.0"; // Stale config version on disk must never overwrite binary version
    orig.enableEasyEda = false;
    orig.enableKiCad = true;
    orig.enableFreeCad = false;
    orig.autoDeployPlugins = false;
    orig.startMinimizedToTray = false;
    orig.launchAtLogin = true;
    orig.disallowExports = true;
    orig.connectionMode = "gateway";
    orig.listenHost = "127.0.0.1";
    orig.rpcPort = 13892;
    orig.pluginPort = 13893;
    orig.aimonHost = "192.168.1.50";
    orig.aimonWebPort = 13883;
    orig.gatewayPort = 13885;
    orig.authSecret = "tray-secret-99";
    orig.allowedRoot = _ws;
    orig.kicadBinaryPath = "/usr/bin/kicad-cli";
    orig.freecadBinaryPath = "/usr/bin/freecadcmd";
    orig.kicadPluginDir = _ws + "/custom_kicad_plugins";
    orig.freecadModDir = _ws + "/custom_freecad_mod";

    std::string cfgFile = _ws + "/sub/aimon-settings.json";
    std::string err;
    CHECK_TRUE(EdaBridgeAppConfig::saveToFile(cfgFile, orig, err));
    CHECK_TRUE(err.empty());
    CHECK_TRUE(fs::exists(cfgFile));

    EdaBridgeAppSettings loaded;
    CHECK_TRUE(EdaBridgeAppConfig::loadFromFile(cfgFile, loaded, err));
    CHECK_TRUE(err.empty());
    STRCMP_EQUAL("1.1.1", loaded.version.c_str());
    CHECK_EQUAL(orig.enableEasyEda, loaded.enableEasyEda);
    CHECK_EQUAL(orig.enableKiCad, loaded.enableKiCad);
    CHECK_EQUAL(orig.enableFreeCad, loaded.enableFreeCad);
    CHECK_EQUAL(orig.autoDeployPlugins, loaded.autoDeployPlugins);
    CHECK_EQUAL(orig.startMinimizedToTray, loaded.startMinimizedToTray);
    CHECK_EQUAL(orig.launchAtLogin, loaded.launchAtLogin);
    CHECK_EQUAL(orig.disallowExports, loaded.disallowExports);
    CHECK_EQUAL(orig.connectionMode, loaded.connectionMode);
    CHECK_EQUAL(orig.listenHost, loaded.listenHost);
    CHECK_EQUAL(orig.rpcPort, loaded.rpcPort);
    CHECK_EQUAL(orig.pluginPort, loaded.pluginPort);
    CHECK_EQUAL(orig.aimonHost, loaded.aimonHost);
    CHECK_EQUAL(orig.aimonWebPort, loaded.aimonWebPort);
    CHECK_EQUAL(orig.gatewayPort, loaded.gatewayPort);
    CHECK_EQUAL(orig.authSecret, loaded.authSecret);
    CHECK_EQUAL(orig.allowedRoot, loaded.allowedRoot);
    CHECK_EQUAL(orig.kicadBinaryPath, loaded.kicadBinaryPath);
    CHECK_EQUAL(orig.freecadBinaryPath, loaded.freecadBinaryPath);
    CHECK_EQUAL(orig.kicadPluginDir, loaded.kicadPluginDir);
    CHECK_EQUAL(orig.freecadModDir, loaded.freecadModDir);

    EdaRpcServerOptions srvOpts = EdaBridgeAppConfig::toServerOptions(loaded);
    CHECK_EQUAL(loaded.listenHost, srvOpts.listenHost);
    CHECK_EQUAL(loaded.rpcPort, srvOpts.port);
    CHECK_EQUAL(loaded.pluginPort, srvOpts.pluginPort);
    CHECK_EQUAL(loaded.authSecret, srvOpts.authSecret);
    CHECK_EQUAL(loaded.allowedRoot, srvOpts.allowedRoot);
    CHECK_EQUAL(loaded.enableEasyEda, srvOpts.enableEasyEda);
    CHECK_EQUAL(loaded.enableKiCad, srvOpts.enableKiCad);
    CHECK_EQUAL(loaded.enableFreeCad, srvOpts.enableFreeCad);
    CHECK_EQUAL(loaded.disallowExports, srvOpts.disallowExports);

    // Fault: non-existent file and malformed JSON
    EdaBridgeAppSettings dummy;
    CHECK_FALSE(EdaBridgeAppConfig::loadFromFile(_ws + "/missing.json", dummy, err));
    CHECK_FALSE(err.empty());

    std::string badFile = _ws + "/bad.json";
    {
        std::ofstream ofs(badFile);
        ofs << "{bad json";
    }
    CHECK_FALSE(EdaBridgeAppConfig::loadFromFile(badFile, dummy, err));
    CHECK_FALSE(err.empty());
}

TEST(EdaBridge_UnifiedAppConfigAndToggles, EmbeddedEextZipAndDesktopPluginDeployment) {
    CHECK_EQUAL("1.1.1", EdaBridgeAppSettings().version);

    std::vector<uint8_t> zipBytes = EdaBridgeAppConfig::buildEasyEdaEextZipBytes();
    CHECK_TRUE(zipBytes.size() > 512);
    // PK\x03\x04 local file header signature
    CHECK_EQUAL(0x50, static_cast<int>(zipBytes[0]));
    CHECK_EQUAL(0x4B, static_cast<int>(zipBytes[1]));
    CHECK_EQUAL(0x03, static_cast<int>(zipBytes[2]));
    CHECK_EQUAL(0x04, static_cast<int>(zipBytes[3]));

    std::string zipView(reinterpret_cast<const char*>(zipBytes.data()), zipBytes.size());
    CHECK_TRUE(zipView.find("extension.json") != std::string::npos);
    CHECK_TRUE(zipView.find("index.js") != std::string::npos);
    CHECK_TRUE(zipView.find("dist/index.js") != std::string::npos);
    CHECK_TRUE(zipView.find("edaEsbuildExportName") != std::string::npos);
    CHECK_TRUE(zipView.find("eda.sys_WebSocket") != std::string::npos);
    CHECK_TRUE(zipView.find("aimon-easyeda-pro-bridge") != std::string::npos);
    CHECK_TRUE(zipView.find("1.1.1") != std::string::npos);
    CHECK_TRUE(zipView.find("8d4c91f2a70e4c6b9b24e31f6a821100") != std::string::npos);
    // PK\x05\x06 End of Central Directory signature
    CHECK_TRUE(zipView.find(std::string("PK\x05\x06", 4)) != std::string::npos);

    std::string eextPath = _ws + "/dist/aimon-easyeda-bridge.eext";
    std::string err;
    CHECK_TRUE(EdaBridgeAppConfig::writeEasyEdaEextFile(eextPath, err));
    CHECK_TRUE(err.empty());
    CHECK_TRUE(fs::exists(eextPath));
    CHECK_EQUAL(zipBytes.size(), static_cast<size_t>(fs::file_size(eextPath)));

    // 1. Verify no phantom directories are created when FreeCAD / KiCad user folders do not exist
    std::string emptyAppData = _ws + "/empty_appdata";
    fs::create_directories(emptyAppData);
    std::string kcDir;
    std::string fcDir;
    bool kcAlready = false;
    bool fcAlready = false;
    CHECK_FALSE(EdaBridgeAppConfig::deployKiCadPlugin(emptyAppData, kcDir, err, "", false, &kcAlready));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(fs::exists(emptyAppData + "/KiCad"));
    CHECK_FALSE(fs::exists(emptyAppData + "/kicad"));

    CHECK_FALSE(EdaBridgeAppConfig::deployFreeCadPlugin(emptyAppData, fcDir, err, "", false, &fcAlready));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(fs::exists(emptyAppData + "/FreeCAD"));

    // 2. Create realistic KiCad 10.0 and FreeCAD v1-1 directories on disk and deploy
    std::string appData = _ws + "/appdata";
    fs::create_directories(appData + "/KiCad/10.0/scripting/plugins");
    fs::create_directories(appData + "/FreeCAD/v1-1/Mod");

    kcAlready = true;
    CHECK_TRUE(EdaBridgeAppConfig::deployKiCadPlugin(appData, kcDir, err, "", false, &kcAlready));
    CHECK_TRUE(err.empty());
    CHECK_FALSE(kcAlready);
    CHECK_TRUE(fs::exists(kcDir + "/__init__.py"));
    CHECK_TRUE(fs::exists(kcDir + "/KiCadAimonBridge.py"));
    {
        std::ifstream kcIfs(kcDir + "/KiCadAimonBridge.py");
        std::string kcContent((std::istreambuf_iterator<char>(kcIfs)),
                              std::istreambuf_iterator<char>());
        CHECK_TRUE(kcContent.find("1.1.1") != std::string::npos);
    }

    fcAlready = true;
    CHECK_TRUE(EdaBridgeAppConfig::deployFreeCadPlugin(appData, fcDir, err, "", false, &fcAlready));
    CHECK_TRUE(err.empty());
    CHECK_FALSE(fcAlready);
    CHECK_TRUE(fs::exists(fcDir + "/InitGui.py"));
    CHECK_TRUE(fs::exists(fcDir + "/AimonFreeCadBridge.py"));
    {
        std::ifstream fcIfs(fcDir + "/AimonFreeCadBridge.py");
        std::string fcContent((std::istreambuf_iterator<char>(fcIfs)),
                              std::istreambuf_iterator<char>());
        CHECK_TRUE(fcContent.find("1.1.1") != std::string::npos);
    }

    // 3a. Re-run when destination files are identical -> do NOT overwrite (alreadyDeployed == true)
    std::string kcDir2;
    std::string fcDir2;
    CHECK_TRUE(EdaBridgeAppConfig::deployKiCadPlugin(appData, kcDir2, err, "", false, &kcAlready));
    CHECK_TRUE(kcAlready);
    CHECK_TRUE(EdaBridgeAppConfig::deployFreeCadPlugin(appData, fcDir2, err, "", false, &fcAlready));
    CHECK_TRUE(fcAlready);

    // 3b. Modify existing deployed files so source and destination differ -> must replace with current embedded content!
    {
        std::ofstream kcOfs(kcDir + "/KiCadAimonBridge.py", std::ios::trunc);
        kcOfs << "# OUTDATED_VERSION_1_1_0\nBRIDGE_VERSION = \"1.1.0\"\n";
    }
    {
        std::ofstream fcOfs(fcDir + "/AimonFreeCadBridge.py", std::ios::trunc);
        fcOfs << "# OUTDATED_VERSION_1_1_0\nBRIDGE_VERSION = \"1.1.0\"\n";
    }

    CHECK_TRUE(EdaBridgeAppConfig::deployKiCadPlugin(appData, kcDir2, err, "", false, &kcAlready));
    CHECK_FALSE(kcAlready);
    {
        std::ifstream kcIfs(kcDir + "/KiCadAimonBridge.py");
        std::string kcContent((std::istreambuf_iterator<char>(kcIfs)),
                              std::istreambuf_iterator<char>());
        CHECK_TRUE(kcContent.find("OUTDATED_VERSION_1_1_0") == std::string::npos);
        CHECK_TRUE(kcContent.find("1.1.1") != std::string::npos);
    }

    CHECK_TRUE(EdaBridgeAppConfig::deployFreeCadPlugin(appData, fcDir2, err, "", false, &fcAlready));
    CHECK_FALSE(fcAlready);
    {
        std::ifstream fcIfs(fcDir + "/AimonFreeCadBridge.py");
        std::string fcContent((std::istreambuf_iterator<char>(fcIfs)),
                              std::istreambuf_iterator<char>());
        CHECK_TRUE(fcContent.find("OUTDATED_VERSION_1_1_0") == std::string::npos);
        CHECK_TRUE(fcContent.find("1.1.1") != std::string::npos);
    }

    // 4. Test customTargetDir (simulating QFileDialog user selection)
    std::string customKc = _ws + "/user_chosen_kicad_plugins";
    std::string customFc = _ws + "/user_chosen_freecad_mod";
    fs::create_directories(customKc);
    fs::create_directories(customFc);

    std::string customKcOut;
    std::string customFcOut;
    CHECK_TRUE(EdaBridgeAppConfig::deployKiCadPlugin(emptyAppData, customKcOut, err, customKc, false, &kcAlready));
    CHECK_FALSE(kcAlready);
    CHECK_TRUE(fs::exists(customKcOut + "/KiCadAimonBridge.py"));

    CHECK_TRUE(EdaBridgeAppConfig::deployFreeCadPlugin(emptyAppData, customFcOut, err, customFc, false, &fcAlready));
    CHECK_FALSE(fcAlready);
    CHECK_TRUE(fs::exists(customFcOut + "/AimonFreeCadBridge.py"));
}

TEST(EdaBridge_UnifiedAppConfigAndToggles, RuntimeBridgeTogglesAndExportLockEnforcement) {
    EdaRpcServerOptions opts;
    opts.listenHost = "127.0.0.1";
    opts.port = 0;
    opts.pluginPort = 0;
    opts.allowedRoot = _ws;

    EdaRpcServer srv(opts);
    CHECK_TRUE(srv.start());

    // Verify dynamic setAllowedRoot updates sandbox validation live at runtime
    CHECK_EQUAL(_ws, srv.allowedRoot());
    std::string sbErr;
    CHECK_FALSE(srv.validateSandboxPath("/tmp/outside_allowed_root/file.kicad_pcb", sbErr));
    srv.setAllowedRoot("/tmp/outside_allowed_root");
    CHECK_EQUAL("/tmp/outside_allowed_root", srv.allowedRoot());
    CHECK_TRUE(srv.validateSandboxPath("/tmp/outside_allowed_root/file.kicad_pcb", sbErr));
    srv.setAllowedRoot(_ws);

    CHECK_TRUE(srv.isBridgeEnabled("easyeda"));
    CHECK_TRUE(srv.isBridgeEnabled("kicad"));
    CHECK_TRUE(srv.isBridgeEnabled("freecad"));
    CHECK_FALSE(srv.isDisallowExports());
    CHECK_EQUAL(30, static_cast<int>(srv.enabledRemoteToolDescriptors().size()));

    // Verify plugin/status dynamically updates active_document in eda/status
    {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK_TRUE(sock >= 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(srv.boundPluginPort()));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        CHECK_EQUAL(0, ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));

        nlohmann::json regReq = {
            {"jsonrpc", "2.0"},
            {"id", "kc-reg-1"},
            {"method", "plugin/register"},
            {"params", {{"app", "kicad"}, {"version", "10.0"}, {"active_document", "initial.kicad_pcb"}}}
        };
        std::string line = regReq.dump() + "\n";
        ::send(sock, line.data(), line.size(), 0);
        char rbuf[1024];
        ssize_t n = ::recv(sock, rbuf, sizeof(rbuf) - 1, 0);
        CHECK_TRUE(n > 0);

        nlohmann::json stReq = {
            {"jsonrpc", "2.0"},
            {"id", "kc-st-1"},
            {"method", "plugin/status"},
            {"params", {{"app", "kicad"}, {"version", "10.0"}, {"active_document", "/opt/live_proj/updated.kicad_pcb"}}}
        };
        line = stReq.dump() + "\n";
        ::send(sock, line.data(), line.size(), 0);
        n = ::recv(sock, rbuf, sizeof(rbuf) - 1, 0);
        CHECK_TRUE(n > 0);

        nlohmann::json statusPayload = srv.buildStatusPayload();
        std::string kcActiveDoc;
        for (const auto& t : statusPayload["tools"]) {
            if (t.value("name", "") == "kicad") {
                kcActiveDoc = t.value("active_document", "");
            }
        }
        CHECK_EQUAL("/opt/live_proj/updated.kicad_pcb", kcActiveDoc);
        // Also verify validateSandboxPath automatically allows parent dir of connected plugin's activeDocument
        CHECK_TRUE(srv.validateSandboxPath("/opt/live_proj/sub/board.kicad_pcb", sbErr));
        ::close(sock);
    }

    // Disable EasyEDA bridge: 11 easyeda_* tools removed -> 19 tools remain
    srv.setBridgeEnabled("easyeda", false);
    CHECK_FALSE(srv.isBridgeEnabled("easyeda"));
    auto toolsNoEe = srv.enabledRemoteToolDescriptors();
    CHECK_EQUAL(19, static_cast<int>(toolsNoEe.size()));
    for (const auto& td : toolsNoEe) {
        CHECK_TRUE(td.value("name", "").rfind("easyeda_", 0) != 0);
    }

    // Calling disabled EasyEDA tool is rejected
    nlohmann::json eeCall = srv.dispatchToolCall("easyeda_live_status", nlohmann::json::object());
    CHECK_FALSE(eeCall.value("ok", true));
    CHECK_TRUE(eeCall.value("error", "").find("disabled") != std::string::npos);

    // Attempting plugin/register for disabled bridge returns JSON-RPC error -32002
    {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK_TRUE(sock >= 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(srv.boundPluginPort()));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        CHECK_EQUAL(0, ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));

        nlohmann::json regReq = {
            {"jsonrpc", "2.0"},
            {"id", "reg-dis-1"},
            {"method", "plugin/register"},
            {"params", {{"app", "easyeda"}, {"version", "2.2.0"}}}
        };
        std::string line = regReq.dump() + "\n";
        ::send(sock, line.data(), line.size(), 0);

        char rbuf[1024];
        ssize_t n = ::recv(sock, rbuf, sizeof(rbuf) - 1, 0);
        ::close(sock);
        CHECK_TRUE(n > 0);
        rbuf[n] = '\0';
        nlohmann::json regResp = nlohmann::json::parse(rbuf, nullptr, false);
        CHECK_FALSE(regResp.is_discarded());
        CHECK_TRUE(regResp.contains("error"));
        CHECK_EQUAL(-32002, regResp["error"].value("code", 0));
    }

    // Re-enable EasyEDA, disable KiCad & FreeCAD
    srv.setBridgeEnabled("easyeda", true);
    srv.setBridgeEnabled("kicad", false);
    srv.setBridgeEnabled("freecad", false);
    CHECK_FALSE(srv.isBridgeEnabled("kicad"));
    CHECK_FALSE(srv.isBridgeEnabled("freecad"));
    CHECK_EQUAL(12, static_cast<int>(srv.enabledRemoteToolDescriptors().size())); // 11 easyeda + 1 status

    CHECK_FALSE(srv.dispatchToolCall("kicad_search_library", {{"query", "R"}}).value("ok", true));
    CHECK_FALSE(srv.dispatchToolCall("freecad_live_status", nlohmann::json::object()).value("ok", true));

    // Re-enable all bridges and test export lock
    srv.setBridgeEnabled("kicad", true);
    srv.setBridgeEnabled("freecad", true);
    srv.setDisallowExports(true);
    CHECK_TRUE(srv.isDisallowExports());

    // Non-export tools still work
    CHECK_TRUE(srv.dispatchToolCall("kicad_search_library", {{"query", "R"}}).value("ok", false));
    CHECK_TRUE(srv.dispatchToolCall("freecad_inspect_document", nlohmann::json::object()).value("ok", false));

    // Export tools are blocked
    nlohmann::json exp1 = srv.dispatchToolCall("easyeda_export_jlcpcb", {{"output_dir", _ws + "/e1"}});
    CHECK_FALSE(exp1.value("ok", true));
    nlohmann::json exp2 = srv.dispatchToolCall("kicad_export_fab_pack", {{"output_dir", _ws + "/e2"}});
    CHECK_FALSE(exp2.value("ok", true));
    nlohmann::json exp3 = srv.dispatchToolCall("kicad_export_step", {{"output_path", _ws + "/e3.step"}});
    CHECK_FALSE(exp3.value("ok", true));
    nlohmann::json exp4 = srv.dispatchToolCall("freecad_export_print_files", {{"output_dir", _ws + "/e4"}});
    CHECK_FALSE(exp4.value("ok", true));

    srv.stop();
}

TEST(EdaBridge_UnifiedAppConfigAndToggles, LiveHttpQuotaClientAndTrayTooltipFormatting) {
    int srvSock = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK_TRUE(srvSock >= 0);
    int opt = 1;
    ::setsockopt(srvSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK_EQUAL(0, ::bind(srvSock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
    CHECK_EQUAL(0, ::listen(srvSock, 4));

    socklen_t len = sizeof(addr);
    CHECK_EQUAL(0, ::getsockname(srvSock, reinterpret_cast<struct sockaddr*>(&addr), &len));
    int httpPort = ntohs(addr.sin_port);

    std::atomic<int> postRefreshCount{0};
    std::atomic<bool> stopHttp{false};
    std::thread httpThread([srvSock, &postRefreshCount, &stopHttp]() {
        for (int reqIdx = 0; reqIdx < 2 && !stopHttp.load(); ) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srvSock, &rfds);
            struct timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 50000;
            int sel = ::select(srvSock + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            struct sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(srvSock, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
            if (cfd < 0) break;
            ++reqIdx;

            char buf[2048];
            ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                if (std::strstr(buf, "POST /api/refresh") != nullptr) {
                    postRefreshCount.fetch_add(1);
                }
            }

            nlohmann::json payload = {
                {"last_updated_epoch", 1760560000},
                {"antigravity", {
                    {"is_running", true},
                    {"plan_tier", "Google AI Ultra"},
                    {"available_prompt_credits", 420},
                    {"available_flow_credits", 180},
                    {"models", nlohmann::json::array({
                        {{"model_name", "Claude Opus 4.6"}, {"remaining_fraction", 0.72}, {"reset_time_remaining_seconds", 3600}},
                        {{"model_name", "Gemini 2.5 Pro"}, {"remaining_fraction", 0.95}, {"reset_time_remaining_seconds", 7200}}
                    })}
                }},
                {"cursor", {
                    {"is_authenticated", true},
                    {"plan_tier", "Pro"},
                    {"usage_mode", "spend"},
                    {"fast_requests_used", 125},
                    {"fast_requests_limit", 500},
                    {"total_spend_usd", 18.50},
                    {"on_demand_spend", 4.25},
                    {"cycle_reset_iso", "2026-11-01T00:00:00Z"}
                }},
                {"claude", {
                    {"enabled", true},
                    {"accounts", nlohmann::json::array({
                        {
                            {"name", "personal"},
                            {"tier", "personal"},
                            {"last_5h", {{"total_tokens", 48000}, {"est_cost_usd", 1.25}}},
                            {"last_7d", {{"total_tokens", 620000}, {"est_cost_usd", 14.80}}},
                            {"window", {{"total_tokens", 1500000}, {"est_cost_usd", 38.40}}},
                            {"cycle_configured", true},
                            {"cycle", {{"est_cost_usd", 24.00}}},
                            {"spend_limit_usd", 100.0},
                            {"est_pct_of_limit", 24.0}
                        }
                    })}
                }}
            };
            std::string body = payload.dump();
            std::string resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            ::send(cfd, resp.data(), resp.size(), 0);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
        }
    });

    struct HttpGuard {
        int sock;
        std::atomic<bool>& stopFlag;
        std::thread& thr;
        ~HttpGuard() {
            stopFlag.store(true);
            if (sock >= 0) {
                ::shutdown(sock, SHUT_RDWR);
                ::close(sock);
            }
            if (thr.joinable()) thr.join();
        }
    } guard{srvSock, stopHttp, httpThread};

    AimonQuotaSnapshot snap;
    CHECK_TRUE(EdaBridgeAppConfig::fetchQuotaSnapshot("127.0.0.1", httpPort, false, snap, 2000));
    CHECK_TRUE(snap.reachable);
    CHECK_TRUE(snap.agRunning);
    CHECK_EQUAL("Google AI Ultra", snap.agPlanTier);
    CHECK_EQUAL(420, snap.agPromptCredits);
    CHECK_EQUAL(180, snap.agFlowCredits);
    CHECK_EQUAL(2, static_cast<int>(snap.agModels.size()));
    CHECK_EQUAL("Claude Opus 4.6", snap.agModels[0].name);
    DOUBLES_EQUAL(72.0, snap.agModels[0].remainingPct, 0.1);
    CHECK_TRUE(snap.cursorAuthenticated);
    CHECK_EQUAL("Pro", snap.cursorPlanTier);
    CHECK_EQUAL(125, snap.cursorRequestsUsed);
    CHECK_EQUAL(500, snap.cursorRequestsLimit);
    DOUBLES_EQUAL(18.50, snap.cursorTotalSpendUsd, 0.01);
    CHECK_TRUE(snap.claudeEnabled);
    CHECK_EQUAL(1, static_cast<int>(snap.claudeAccounts.size()));
    CHECK_EQUAL("personal", snap.claudeAccounts[0].name);
    DOUBLES_EQUAL(14.80, snap.claudeAccounts[0].cost7dUsd, 0.01);

    AimonQuotaSnapshot snapRefresh;
    CHECK_TRUE(EdaBridgeAppConfig::fetchQuotaSnapshot("127.0.0.1", httpPort, true, snapRefresh, 2000));
    CHECK_EQUAL(1, postRefreshCount.load());

    std::string tooltip = EdaBridgeAppConfig::formatTrayTooltipSummary(snap, true, false, true);
    CHECK_TRUE(tooltip.rfind("aimon", 0) == 0);
    CHECK_TRUE(tooltip.find("72%") != std::string::npos);
    CHECK_TRUE(tooltip.find("$18.50") != std::string::npos);

    // Fault injection: unreachable port and malformed JSON
    AimonQuotaSnapshot unreach;
    CHECK_FALSE(EdaBridgeAppConfig::fetchQuotaSnapshot("127.0.0.1", 1, false, unreach, 300));
    CHECK_FALSE(unreach.reachable);
    CHECK_FALSE(unreach.errorMessage.empty());

    std::string parseErr;
    AimonQuotaSnapshot badSnap;
    CHECK_FALSE(EdaBridgeAppConfig::parseQuotaSnapshotJson("not-json", badSnap, parseErr));
    CHECK_FALSE(parseErr.empty());
}

TEST(EdaBridge_UnifiedAppConfigAndToggles, LiveGuiPluginCanvasEnrichmentAndPythonPluginVerification) {
    // 1. Verify pure in-memory KiCadSchematicEditor::syncSchematicToPcb and applyPcbEdits with pin_nets
    nlohmann::json memSync = KiCadSchematicEditor::syncSchematicToPcb({
        {"components", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "ESP32"}, {"footprint", "Package_SO:SOIC-8"}, {"x_mm", 20.0}, {"y_mm", 15.0}},
            {{"reference", "R1"}, {"value", "10k"}, {"footprint", "Resistor_SMD:R_0603_1608Metric"},
             {"pins", nlohmann::json::array({{{"number", "1"}, {"net", "VCC_3V3"}}, {{"number", "2"}, {"net", "GND"}}})}}
        })},
        {"board_outline", {{"width_mm", 50.0}, {"height_mm", 35.0}}}
    });
    CHECK_TRUE(memSync.value("ok", false));
    CHECK_EQUAL(2, memSync.value("synced_components", 0));
    CHECK_TRUE(memSync.contains("footprints"));
    CHECK_EQUAL(2, static_cast<int>(memSync["footprints"].size()));
    CHECK_TRUE(memSync["footprints"][0].contains("pads"));
    CHECK_EQUAL(8, static_cast<int>(memSync["footprints"][0]["pads"].size()));
    CHECK_TRUE(memSync.contains("board_outline"));
    CHECK_EQUAL(4, static_cast<int>(memSync["board_outline"]["lines"].size()));

    nlohmann::json memEdit = KiCadSchematicEditor::applyPcbEdits({
        {"board_outline", {{"width_mm", 45.0}, {"height_mm", 30.0}}},
        {"placements", nlohmann::json::array({
            {{"reference", "R1"}, {"value", "4.7k"}, {"footprint", "Resistor_SMD:R_0603_1608Metric"},
             {"x_mm", 12.0}, {"y_mm", 10.0}, {"pin_nets", {{"1", "I2C_SDA"}, {"2", "VCC_3V3"}}}},
            {{"reference", "J1"}, {"value", "Conn_01x04"}, {"footprint", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical"},
             {"x_mm", 5.0}, {"y_mm", 15.0}, {"pins", nlohmann::json::array({{{"number", "1"}, {"net", "VCC_3V3"}}, {{"number", "2"}, {"net", "I2C_SDA"}}})}}
        })},
        {"tracks", nlohmann::json::array({
            {{"x1", 5.0}, {"y1", 12.54}, {"x2", 11.2}, {"y2", 10.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "I2C_SDA"}}
        })},
        {"vias", nlohmann::json::array({
            {{"x", 11.2}, {"y", 10.0}, {"size_mm", 0.6}, {"drill_mm", 0.3}, {"net", "I2C_SDA"}}
        })}
    });
    CHECK_TRUE(memEdit.value("ok", false));
    CHECK_EQUAL(2, static_cast<int>(memEdit["footprints"].size()));
    CHECK_EQUAL(1, static_cast<int>(memEdit["tracks"].size()));
    CHECK_EQUAL(1, static_cast<int>(memEdit["vias"].size()));
    CHECK_EQUAL("I2C_SDA", memEdit["footprints"][0]["pads"][0].value("net", ""));
    CHECK_TRUE(memEdit["footprints"][1]["pads"][0].value("drill", 0.0) > 0.5);

    // 2. Verify EdaRpcServer enriches KiCad plugin calls and forwards FreeCAD live GUI calls
    EdaRpcServerOptions opts;
    opts.listenHost = "127.0.0.1";
    opts.port = 0;
    opts.pluginPort = 0;
    opts.allowedRoot = _ws;

    EdaRpcServer srv(opts);
    CHECK_TRUE(srv.start());

    std::atomic<bool> stopPlugins{false};
    nlohmann::json lastKicadSyncParams;
    nlohmann::json lastKicadEditParams;
    std::atomic<int> kicadInspectCalls{0};
    std::atomic<int> freecadJointCalls{0};
    std::atomic<int> freecadPrintCalls{0};
    std::atomic<int> freecadRenderCalls{0};

    auto runMockPlugin = [&](const std::string& appName, std::atomic<bool>& readyFlag) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(srv.boundPluginPort()));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            readyFlag.store(true);
            return;
        }
        nlohmann::json reg = {
            {"jsonrpc", "2.0"},
            {"id", appName + "-reg"},
            {"method", "plugin/register"},
            {"params", {{"app", appName}, {"version", "1.1.1"}, {"active_document", "live_" + appName}}}
        };
        std::string regLine = reg.dump() + "\n";
        ::send(sock, regLine.data(), regLine.size(), 0);
        char rbuf[65536];
        ::recv(sock, rbuf, sizeof(rbuf) - 1, 0);
        readyFlag.store(true);

        std::string accum;
        while (!stopPlugins.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 30000;
            int sel = ::select(sock + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            ssize_t n = ::recv(sock, rbuf, sizeof(rbuf) - 1, 0);
            if (n <= 0) break;
            accum.append(rbuf, static_cast<size_t>(n));
            size_t nl = 0;
            while ((nl = accum.find('\n')) != std::string::npos) {
                std::string line = accum.substr(0, nl);
                accum.erase(0, nl + 1);
                auto req = nlohmann::json::parse(line, nullptr, false);
                if (req.is_discarded()) continue;
                std::string method = req.value("method", "");
                nlohmann::json params = req.value("params", nlohmann::json::object());
                std::string toolName = (method == "tools/call")
                    ? params.value("name", "")
                    : method;
                nlohmann::json toolArgs = (method == "tools/call" && params.contains("arguments"))
                    ? params["arguments"]
                    : params;
                if (toolName == "kicad_inspect_project") {
                    kicadInspectCalls.fetch_add(1);
                } else if (toolName == "kicad_sync_sch_to_pcb") {
                    lastKicadSyncParams = toolArgs;
                } else if (toolName == "kicad_edit_pcb") {
                    lastKicadEditParams = toolArgs;
                } else if (toolName == "freecad_assemble_joint") {
                    freecadJointCalls.fetch_add(1);
                } else if (toolName == "freecad_analyze_printability") {
                    freecadPrintCalls.fetch_add(1);
                } else if (toolName == "freecad_render_view") {
                    freecadRenderCalls.fetch_add(1);
                }
                nlohmann::json resp = {
                    {"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result", {{"ok", true}, {"live_plugin_handled", toolName}}}
                };
                std::string out = resp.dump() + "\n";
                ::send(sock, out.data(), out.size(), 0);
            }
        }
        ::close(sock);
    };

    std::atomic<bool> kcReady{false}, fcReady{false};
    std::thread kcThr(runMockPlugin, "kicad", std::ref(kcReady));
    std::thread fcThr(runMockPlugin, "freecad", std::ref(fcReady));
    struct PluginThrGuard {
        std::atomic<bool>& stopFlag;
        EdaRpcServer& srvRef;
        std::thread& t1;
        std::thread& t2;
        ~PluginThrGuard() {
            stopFlag.store(true);
            srvRef.stop();
            if (t1.joinable()) t1.join();
            if (t2.joinable()) t2.join();
        }
    } thrGuard{stopPlugins, srv, kcThr, fcThr};

    for (int i = 0; i < 50 && (!kcReady.load() || !fcReady.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (int i = 0; i < 30 && (!srv.isPluginConnected("kicad") || !srv.isPluginConnected("freecad")); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_TRUE(srv.isPluginConnected("kicad"));
    CHECK_TRUE(srv.isPluginConnected("freecad"));

    // KiCad inspect without file path forwards to live plugin
    nlohmann::json kcInsp = srv.dispatchToolCall("kicad_inspect_project", nlohmann::json::object());
    CHECK_TRUE(kcInsp.value("ok", false));
    CHECK_EQUAL(1, kicadInspectCalls.load());

    // KiCad sync without pcb_path synthesizes footprints + pads and forwards to live plugin
    nlohmann::json kcSync = srv.dispatchToolCall("kicad_sync_sch_to_pcb", {
        {"components", nlohmann::json::array({
            {{"reference", "R1"}, {"value", "10k"}, {"footprint", "Resistor_SMD:R_0603_1608Metric"},
             {"pins", nlohmann::json::array({{{"number", "1"}, {"net", "VCC"}}, {{"number", "2"}, {"net", "GND"}}})}}
        })}
    });
    CHECK_TRUE(kcSync.value("ok", false));
    CHECK_TRUE(lastKicadSyncParams.contains("footprints"));
    CHECK_EQUAL(1, static_cast<int>(lastKicadSyncParams["footprints"].size()));
    CHECK_EQUAL(2, static_cast<int>(lastKicadSyncParams["footprints"][0]["pads"].size()));

    // KiCad edit_pcb without pcb_path synthesizes footprints + pads + tracks + vias and forwards to live plugin
    nlohmann::json kcEdit = srv.dispatchToolCall("kicad_edit_pcb", {
        {"board_outline", {{"width_mm", 40.0}, {"height_mm", 25.0}}},
        {"placements", nlohmann::json::array({
            {{"reference", "U1"}, {"value", "BMI270"}, {"footprint", "Package_SO:SOIC-8"}, {"x_mm", 18.0}, {"y_mm", 12.0}}
        })},
        {"tracks", nlohmann::json::array({
            {{"x1", 10.0}, {"y1", 10.0}, {"x2", 18.0}, {"y2", 12.0}, {"width_mm", 0.25}, {"layer", "F.Cu"}, {"net", "SDA"}}
        })}
    });
    CHECK_TRUE(kcEdit.value("ok", false));
    CHECK_TRUE(lastKicadEditParams.contains("footprints"));
    CHECK_EQUAL(1, static_cast<int>(lastKicadEditParams["footprints"].size()));
    CHECK_EQUAL(8, static_cast<int>(lastKicadEditParams["footprints"][0]["pads"].size()));
    CHECK_EQUAL(1, static_cast<int>(lastKicadEditParams["tracks"].size()));

    // FreeCAD live GUI forwarding by default when plugin is connected
    CHECK_TRUE(srv.dispatchToolCall("freecad_assemble_joint", {{"min_deg", -45.0}, {"max_deg", 45.0}}).value("ok", false));
    CHECK_EQUAL(1, freecadJointCalls.load());
    CHECK_TRUE(srv.dispatchToolCall("freecad_analyze_printability", nlohmann::json::object()).value("ok", false));
    CHECK_EQUAL(1, freecadPrintCalls.load());
    CHECK_TRUE(srv.dispatchToolCall("freecad_render_view", {{"output_dir", _ws + "/renders"}}).value("ok", false));
    CHECK_EQUAL(1, freecadRenderCalls.load());

    stopPlugins.store(true);
    srv.stop();
    if (kcThr.joinable()) kcThr.join();
    if (fcThr.joinable()) fcThr.join();

    // 3. Verify Python plugin implementations (KiCadAimonBridge.py & AimonFreeCadBridge.py) with mock CAD APIs
    std::string appData = _ws + "/py_appdata";
    fs::create_directories(appData + "/KiCad/10.0/scripting/plugins");
    fs::create_directories(appData + "/FreeCAD/v1-1/Mod");
    std::string kcDir, fcDir, deployErr;
    CHECK_TRUE(EdaBridgeAppConfig::deployKiCadPlugin(appData, kcDir, deployErr));
    CHECK_TRUE(EdaBridgeAppConfig::deployFreeCadPlugin(appData, fcDir, deployErr));

    std::string pyTestScript = _ws + "/verify_plugins.py";
    {
        std::ofstream ofs(pyTestScript);
        ofs << "import os, sys, types\n"
            << "sys.path.insert(0, r'" << kcDir << "')\n"
            << "sys.path.insert(0, r'" << fcDir << "')\n"
            << "\n"
            << "# --- Mock pcbnew for KiCadAimonBridge ---\n"
            << "pcbnew = types.ModuleType('pcbnew')\n"
            << "class MockActionPlugin:\n"
            << "    def register(self): pass\n"
            << "pcbnew.ActionPlugin = MockActionPlugin\n"
            << "pcbnew.F_Cu = 0; pcbnew.B_Cu = 31; pcbnew.F_SilkS = 37; pcbnew.Edge_Cuts = 44\n"
            << "pcbnew.F_Mask = 35; pcbnew.B_Mask = 34; pcbnew.F_Paste = 33; pcbnew.B_Paste = 32\n"
            << "pcbnew.PAD_SHAPE_RECT = 1; pcbnew.PAD_SHAPE_CIRCLE = 0\n"
            << "pcbnew.PAD_ATTRIB_SMD = 1; pcbnew.PAD_ATTRIB_PTH = 0; pcbnew.PAD_ATTRIB_NPTH = 3\n"
            << "pcbnew.EDA_ANGLE = lambda deg, kind=None: deg\n"
            << "pcbnew.DEGREES_T = 0\n"
            << "pcbnew.FromMM = lambda v: int(round(float(v) * 1000000))\n"
            << "pcbnew.ToMM = lambda v: float(v) / 1000000.0\n"
            << "pcbnew.VECTOR2I = lambda x, y: (x, y)\n"
            << "pcbnew.EDA_SHAPE_T_SEGMENT = 0\n"
            << "class MockNet:\n"
            << "    def __init__(self, board, name, code=1): self._name = name; self._code = code\n"
            << "    def GetNetname(self): return self._name\n"
            << "    def GetNetCode(self): return self._code\n"
            << "pcbnew.NETINFO_ITEM = MockNet\n"
            << "class MockPad:\n"
            << "    def __init__(self, fp): self.fp = fp; self.num = '1'; self.pos = (0,0); self.size = (0,0); self.net = None; self.drill = (0,0)\n"
            << "    def SetNumber(self, n): self.num = str(n)\n"
            << "    def GetNumber(self): return self.num\n"
            << "    def SetShape(self, s): pass\n"
            << "    def SetAttribute(self, a): pass\n"
            << "    def SetLayerSet(self, ls): pass\n"
            << "    def SetSize(self, sz): self.size = sz\n"
            << "    def SetDrillSize(self, sz): self.drill = sz\n"
            << "    def SetPos0(self, p): self.pos = p\n"
            << "    def SetPosition(self, p): self.pos = p\n"
            << "    def SetNet(self, n): self.net = n\n"
            << "    def SetNetCode(self, c): pass\n"
            << "    def GetNetname(self): return self.net.GetNetname() if self.net else ''\n"
            << "pcbnew.PAD = MockPad\n"
            << "class MockLSET:\n"
            << "    def addLayer(self, l): pass\n"
            << "    @staticmethod\n"
            << "    def AllCuMask(): return MockLSET()\n"
            << "pcbnew.LSET = MockLSET\n"
            << "class MockRef:\n"
            << "    def __init__(self): self.txt = ''\n"
            << "    def SetText(self, t): self.txt = t\n"
            << "    def SetVisible(self, v): pass\n"
            << "class MockFootprint:\n"
            << "    def __init__(self, board): self.ref = ''; self.val = ''; self.pos = (0,0); self.orient = 0.0; self.layer = 0; self.pads = []; self.drawings = []; self._refObj = MockRef(); self._valObj = MockRef()\n"
            << "    def SetReference(self, r): self.ref = r\n"
            << "    def GetReference(self): return self.ref\n"
            << "    def SetValue(self, v): self.val = v\n"
            << "    def GetValue(self): return self.val\n"
            << "    def GetFPIDAsString(self): return 'Resistor_SMD:R_0603'\n"
            << "    def SetPosition(self, p): self.pos = p\n"
            << "    def GetPosition(self): return types.SimpleNamespace(x=self.pos[0], y=self.pos[1])\n"
            << "    def SetOrientationDegrees(self, d): self.orient = float(d)\n"
            << "    def GetOrientationDegrees(self): return self.orient\n"
            << "    def SetLayer(self, l): self.layer = l\n"
            << "    def GetLayer(self): return self.layer\n"
            << "    def Flip(self, p, b): self.layer = pcbnew.B_Cu\n"
            << "    def Add(self, item):\n"
            << "        if isinstance(item, MockPad): self.pads.append(item)\n"
            << "        else: self.drawings.append(item)\n"
            << "    def Pads(self): return self.pads\n"
            << "    def Reference(self): return self._refObj\n"
            << "    def Value(self): return self._valObj\n"
            << "pcbnew.FOOTPRINT = MockFootprint\n"
            << "class MockShape:\n"
            << "    def __init__(self, parent=None): self.layer = 0; self.start = (0,0); self.end = (0,0); self.width = 0\n"
            << "    def SetShape(self, s): pass\n"
            << "    def SetLayer(self, l): self.layer = l\n"
            << "    def GetLayer(self): return self.layer\n"
            << "    def SetWidth(self, w): self.width = w\n"
            << "    def SetStart(self, p): self.start = p\n"
            << "    def SetEnd(self, p): self.end = p\n"
            << "pcbnew.PCB_SHAPE = MockShape\n"
            << "class MockTrack:\n"
            << "    def __init__(self, board): self.start = (0,0); self.end = (0,0); self.width = 0; self.layer = 0; self.net = None\n"
            << "    def SetStart(self, p): self.start = p\n"
            << "    def SetEnd(self, p): self.end = p\n"
            << "    def SetWidth(self, w): self.width = w\n"
            << "    def SetLayer(self, l): self.layer = l\n"
            << "    def SetNet(self, n): self.net = n\n"
            << "    def SetNetCode(self, c): pass\n"
            << "pcbnew.PCB_TRACK = MockTrack\n"
            << "class MockVia:\n"
            << "    def __init__(self, board): self.pos = (0,0); self.width = 0; self.drill = 0; self.net = None\n"
            << "    def SetPosition(self, p): self.pos = p\n"
            << "    def SetWidth(self, w): self.width = w\n"
            << "    def SetDrill(self, d): self.drill = d\n"
            << "    def SetNet(self, n): self.net = n\n"
            << "    def SetNetCode(self, c): pass\n"
            << "pcbnew.PCB_VIA = MockVia\n"
            << "class MockBoard:\n"
            << "    def __init__(self): self.fps = []; self.tracks = []; self.drawings = []; self.zones = []; self.nets = {}; self.refreshed = 0\n"
            << "    def GetFileName(self): return 'live_test.kicad_pcb'\n"
            << "    def GetFootprints(self): return self.fps\n"
            << "    def GetTracks(self): return self.tracks\n"
            << "    def GetDrawings(self): return self.drawings\n"
            << "    def Zones(self): return self.zones\n"
            << "    def GetLayerName(self, l): return 'B.Cu' if l == 31 else ('Edge.Cuts' if l == 44 else 'F.Cu')\n"
            << "    def GetLayerID(self, n): return 31 if n == 'B.Cu' else (44 if n == 'Edge.Cuts' else 0)\n"
            << "    def FindNet(self, name): return self.nets.get(name)\n"
            << "    def GetNetInfo(self): return types.SimpleNamespace(GetNetCount=lambda: len(self.nets)+1, NetsByName=lambda: self.nets)\n"
            << "    def Add(self, item):\n"
            << "        if isinstance(item, MockFootprint): self.fps.append(item)\n"
            << "        elif isinstance(item, (MockTrack, MockVia)): self.tracks.append(item)\n"
            << "        elif isinstance(item, MockShape): self.drawings.append(item)\n"
            << "        elif isinstance(item, MockNet): self.nets[item.GetNetname()] = item\n"
            << "    def Remove(self, item):\n"
            << "        if item in self.drawings: self.drawings.remove(item)\n"
            << "mock_board = MockBoard()\n"
            << "pcbnew.GetBoard = lambda: mock_board\n"
            << "pcbnew.Refresh = lambda: setattr(mock_board, 'refreshed', mock_board.refreshed + 1)\n"
            << "sys.modules['pcbnew'] = pcbnew\n"
            << "\n"
            << "import KiCadAimonBridge as kb\n"
            << "kc_client = kb.KiCadAimonBridgeClient()\n"
            << "res = kc_client._handle_call('kicad_edit_pcb', {\n"
            << "    'board_outline': {'width_mm': 40.0, 'height_mm': 25.0},\n"
            << "    'footprints': [{\n"
            << "        'reference': 'R1', 'value': '10k', 'footprint': 'Resistor_SMD:R_0603', 'x_mm': 15.0, 'y_mm': 10.0, 'rotation_deg': 90.0, 'layer': 'F.Cu',\n"
            << "        'pads': [{'number': '1', 'x_mm': -0.8, 'y_mm': 0.0, 'width_mm': 0.9, 'height_mm': 0.95, 'layer': 'F.Cu', 'net': 'VCC'},\n"
            << "                 {'number': '2', 'x_mm': 0.8, 'y_mm': 0.0, 'width_mm': 0.9, 'height_mm': 0.95, 'layer': 'F.Cu', 'net': 'GND'}]\n"
            << "    }],\n"
            << "    'tracks': [{'x1': 10.0, 'y1': 10.0, 'x2': 15.0, 'y2': 10.0, 'width_mm': 0.25, 'layer': 'F.Cu', 'net': 'VCC'}],\n"
            << "    'vias': [{'x': 15.0, 'y': 10.0, 'size_mm': 0.6, 'drill_mm': 0.3, 'net': 'VCC'}]\n"
            << "})\n"
            << "assert res.get('ok') is True, f'KiCad edit failed: {res}'\n"
            << "assert len(mock_board.fps) == 1 and len(mock_board.fps[0].pads) == 2\n"
            << "assert len(mock_board.tracks) == 2\n"
            << "assert len(mock_board.drawings) == 4\n"
            << "assert mock_board.refreshed >= 1\n"
            << "\n"
            << "# --- Mock FreeCAD / Part / FreeCADGui for AimonFreeCadBridge ---\n"
            << "FreeCAD = types.ModuleType('FreeCAD')\n"
            << "class MockVector:\n"
            << "    def __init__(self, x=0.0, y=0.0, z=0.0): self.x = float(x); self.y = float(y); self.z = float(z)\n"
            << "FreeCAD.Vector = MockVector\n"
            << "class MockRotation:\n"
            << "    def __init__(self, *args): self.args = args\n"
            << "FreeCAD.Rotation = MockRotation\n"
            << "class MockPlacement:\n"
            << "    def __init__(self, base=None, rot=None, center=None): self.Base = base or MockVector(); self.Rotation = rot or MockRotation()\n"
            << "FreeCAD.Placement = MockPlacement\n"
            << "class MockShape3D:\n"
            << "    def __init__(self): self.Volume = 1250.0; self.Area = 650.0; self.Edges = [1,2,3,4]; self.Faces = [1,2]; self.Placement = MockPlacement(); self.BoundBox = types.SimpleNamespace(XLength=50.0, YLength=30.0, ZLength=20.0, XMin=0.0, YMin=0.0, ZMin=0.0, XMax=50.0, YMax=30.0, ZMax=20.0)\n"
            << "    def fuse(self, other): return MockShape3D()\n"
            << "    def cut(self, other): return MockShape3D()\n"
            << "    def common(self, other): return MockShape3D()\n"
            << "    def rotate(self, *a): pass\n"
            << "    def translate(self, *a): pass\n"
            << "    def makeFillet(self, r, edges): return MockShape3D()\n"
            << "    def makeChamfer(self, r, edges): return MockShape3D()\n"
            << "    def isClosed(self): return True\n"
            << "    def isValid(self): return True\n"
            << "    def distToShape(self, other): return (2.5, [], [])\n"
            << "    def exportStep(self, p): open(p, 'w').write('STEP')\n"
            << "    def exportStl(self, p): open(p, 'w').write('STL')\n"
            << "Part = types.ModuleType('Part')\n"
            << "Part.makeBox = lambda *a: MockShape3D()\n"
            << "Part.makeCylinder = lambda *a: MockShape3D()\n"
            << "Part.makeCone = lambda *a: MockShape3D()\n"
            << "Part.makeSphere = lambda *a: MockShape3D()\n"
            << "Part.makeTorus = lambda *a: MockShape3D()\n"
            << "class MockDocObj:\n"
            << "    def __init__(self, name): self.Name = name; self.Label = name; self.TypeId = 'Part::Feature'; self.Shape = MockShape3D(); self.Placement = MockPlacement(); self.ViewObject = types.SimpleNamespace(ShapeColor=(0.8,0.8,0.8), Visibility=True)\n"
            << "class MockDoc:\n"
            << "    def __init__(self, name='RobotArm'): self.Name = name; self.FileName = ''; self.Objects = []; self.recomputed = 0\n"
            << "    def getObject(self, n):\n"
            << "        for o in self.Objects:\n"
            << "            if o.Name == n: return o\n"
            << "        return None\n"
            << "    def addObject(self, t, n): o = MockDocObj(n); self.Objects.append(o); return o\n"
            << "    def recompute(self): self.recomputed += 1\n"
            << "mock_fc_doc = MockDoc()\n"
            << "FreeCAD.ActiveDocument = mock_fc_doc\n"
            << "FreeCAD.newDocument = lambda n='Doc': mock_fc_doc\n"
            << "FreeCAD.listDocuments = lambda: {'RobotArm': mock_fc_doc}\n"
            << "FreeCADGui = types.ModuleType('FreeCADGui')\n"
            << "mock_view = types.SimpleNamespace(fit_calls=0, iso_calls=0, viewIsometric=lambda: None, fitAll=lambda: None, saveImage=lambda p,w,h,bg: open(p,'w').write('PNG'))\n"
            << "FreeCADGui.ActiveDocument = types.SimpleNamespace(ActiveView=mock_view)\n"
            << "FreeCADGui.updateGui = lambda: None\n"
            << "sys.modules['FreeCAD'] = FreeCAD\n"
            << "sys.modules['Part'] = Part\n"
            << "sys.modules['Mesh'] = types.ModuleType('Mesh')\n"
            << "sys.modules['FreeCADGui'] = FreeCADGui\n"
            << "\n"
            << "import AimonFreeCadBridge as fb\n"
            << "fc_client = fb.AimonFreeCadBridgeClient()\n"
            << "cad_res = fc_client.dispatch_tool('freecad_generate_cad', {\n"
            << "    'primitives': [\n"
            << "        {'name': 'BaseBracket', 'shape': 'mounting_bracket', 'length_mm': 48.0, 'width_mm': 32.0, 'height_mm': 24.0, 'thickness_mm': 4.0, 'color': [0.2, 0.6, 0.9]},\n"
            << "        {'name': 'DriveGear', 'shape': 'spur_gear', 'teeth': 24, 'module_mm': 1.0, 'thickness_mm': 6.0, 'bore_diameter_mm': 5.0},\n"
            << "        {'name': 'ServoArm', 'shape': 'servo_horn', 'arm_length_mm': 28.0, 'holes': [{'diameter_mm': 2.0, 'x_mm': 20.0}]}\n"
            << "    ]\n"
            << "})\n"
            << "assert cad_res.get('ok') is True and cad_res.get('bodies_count') == 3, f'FreeCAD generate failed: {cad_res}'\n"
            << "joint_res = fc_client.dispatch_tool('freecad_assemble_joint', {\n"
            << "    'part_name': 'ServoArm', 'obstacle_name': 'BaseBracket', 'min_deg': -30.0, 'max_deg': 30.0, 'step_deg': 15.0\n"
            << "})\n"
            << "assert joint_res.get('ok') is True and joint_res.get('animated_in_gui') is True, f'FreeCAD joint failed: {joint_res}'\n"
            << "print('PYTHON_PLUGIN_VERIFICATION_OK')\n";
    }

    ProcessResult pyRes = ProcessLauncher::run("/usr/bin/python3", {pyTestScript}, 10000);
    CHECK_TRUE(pyRes.launched);
    if (pyRes.exitCode != 0) {
        std::cerr << "[PythonPluginTest Failure] stdout:\n" << pyRes.stdoutText
                  << "\nstderr:\n" << pyRes.stderrText << "\n";
    }
    CHECK_EQUAL(0, pyRes.exitCode);
    CHECK_TRUE(pyRes.stdoutText.find("PYTHON_PLUGIN_VERIFICATION_OK") != std::string::npos);
}

int main(int ac, char** av) {
    for (int i = 1; i < ac; ++i) {
        if (std::strcmp(av[i], "--negative-control") == 0) {
            std::string ws = makeTempWorkspace("neg_ctrl");
            EdaRpcServerOptions opts;
            opts.listenHost = "127.0.0.1";
            opts.port = 0;
            opts.pluginPort = 0;
            opts.authSecret = "expected-secret";
            opts.allowedRoot = ws;

            EdaRpcServer srv(opts);
            if (!srv.start()) {
                return 0;
            }

            EdaBridgeConfig cfg;
            cfg.enabled = true;
            cfg.mode = "client";
            cfg.host = "127.0.0.1";
            cfg.port = srv.boundPort();
            cfg.authSecret = "wrong-secret";
            cfg.linuxPathPrefix = ws;
            cfg.windowsPathPrefix = ws;

            EdaBridge bridge(cfg);
            nlohmann::json unauthRes = bridge.callTool("kicad_search_library", {{"query", "R"}});
            bool unauthRejected = (!unauthRes.value("ok", true) && unauthRes.value("error_code", 0) == -32001);

            std::string reason1, reason2, reason3;
            bool eeRejected = !EdaBridge::guardEasyEdaApiCall("window.eval", reason1);
            bool fcRejected = !EdaBridge::guardFreecadApiCall("subprocess", "Popen", reason2);
            bool tnpRejected = !EdaBridge::guardFreecadScript("part = doc.Body.Shape.Face1", reason3);

            srv.stop();
            std::error_code ec;
            fs::remove_all(ws, ec);

            if (unauthRejected && eeRejected && fcRejected && tnpRejected) {
                std::cerr << "[NegativeControl] Unauthenticated RPC (-32001), disallowed API namespaces, "
                          << "and TNP Face1 script all rejected as expected.\n";
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
