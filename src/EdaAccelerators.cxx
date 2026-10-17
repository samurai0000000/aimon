/*
 * EdaAccelerators.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaAccelerators.hxx"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

namespace aimon {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kGravity = 9.80665;
constexpr double kSchematicGridMm = 1.27; // 50 mils

double snapTo50Mil(double valMm) {
    return std::llround(valMm / kSchematicGridMm) * kSchematicGridMm;
}

int toGridCoord(double valMm, double stepMm) {
    return static_cast<int>(std::llround(valMm / stepMm));
}

double fromGridCoord(int idx, double stepMm) {
    return static_cast<double>(idx) * stepMm;
}

int layerPriorityForCategory(const std::string& cat) {
    if (cat == "connector" || cat == "input" || cat == "sensor") return 0;
    if (cat == "power" || cat == "regulator" || cat == "filter") return 1;
    if (cat == "mcu" || cat == "core" || cat == "logic" || cat == "ic") return 2;
    if (cat == "driver" || cat == "actuator" || cat == "output") return 3;
    return 2;
}

constexpr double kE24Basic[] = {
    1.0, 1.2, 1.5, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0, 3.3, 3.6, 3.9,
    4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1
};
constexpr size_t kE24BasicLen = sizeof(kE24Basic) / sizeof(kE24Basic[0]);

constexpr double kE24Full[] = {
    1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
    3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1
};
constexpr size_t kE24FullLen = sizeof(kE24Full) / sizeof(kE24Full[0]);

constexpr double kE96[] = {
    1.00, 1.02, 1.05, 1.07, 1.10, 1.13, 1.15, 1.18, 1.21, 1.24, 1.27, 1.30,
    1.33, 1.37, 1.40, 1.43, 1.47, 1.50, 1.54, 1.58, 1.62, 1.65, 1.69, 1.74,
    1.78, 1.82, 1.87, 1.91, 1.96, 2.00, 2.05, 2.10, 2.15, 2.21, 2.26, 2.32,
    2.37, 2.43, 2.49, 2.55, 2.61, 2.67, 2.74, 2.80, 2.87, 2.94, 3.01, 3.09,
    3.16, 3.24, 3.32, 3.40, 3.48, 3.57, 3.65, 3.74, 3.83, 3.92, 4.02, 4.12,
    4.22, 4.32, 4.42, 4.53, 4.64, 4.75, 4.87, 4.99, 5.11, 5.23, 5.36, 5.49,
    5.62, 5.76, 5.90, 6.04, 6.19, 6.34, 6.49, 6.65, 6.81, 6.98, 7.15, 7.32,
    7.50, 7.68, 7.87, 8.06, 8.25, 8.45, 8.66, 8.87, 9.09, 9.31, 9.53, 9.76
};
constexpr size_t kE96Len = sizeof(kE96) / sizeof(kE96[0]);

void getESeriesBase(const std::string& series, bool jlcpcbBasic,
                    const double*& outPtr, size_t& outLen) {
    if (series == "E96") {
        outPtr = kE96;
        outLen = kE96Len;
    } else if (jlcpcbBasic) {
        outPtr = kE24Basic;
        outLen = kE24BasicLen;
    } else {
        outPtr = kE24Full;
        outLen = kE24FullLen;
    }
}

} // anonymous namespace

EdaAccelerators::EdaAccelerators(const EdaAcceleratorConfig& config)
    : _config(config) {
}

void EdaAccelerators::setConfig(const EdaAcceleratorConfig& config) {
    _config = config;
}

bool EdaAccelerators::isAcceleratorTool(const std::string& toolName) {
    return toolName == "eda_calc_schematic_layout" ||
           toolName == "eda_calc_pcb_placement" ||
           toolName == "eda_calc_pcb_autoroute" ||
           toolName == "eda_calc_pcb_connectivity_audit" ||
           toolName == "eda_calc_schematic_erc_audit" ||
           toolName == "eda_calc_electrical_synth" ||
           toolName == "cad_calc_robot_kinematics_statics" ||
           toolName == "cad_calc_mechanism_synth" ||
           toolName == "cad_calc_print_orientation_dfm";
}

nlohmann::json EdaAccelerators::executeTool(const std::string& toolName,
                                            const nlohmann::json& args,
                                            bool& outSuccess,
                                            std::string& outSummary) const {
    if (toolName == "eda_calc_schematic_layout") {
        return calcSchematicLayout(args, outSuccess, outSummary);
    }
    if (toolName == "eda_calc_pcb_placement") {
        return calcPcbPlacement(args, outSuccess, outSummary);
    }
    if (toolName == "eda_calc_pcb_autoroute") {
        return calcPcbAutoroute(args, outSuccess, outSummary);
    }
    if (toolName == "eda_calc_schematic_erc_audit") {
        return calcSchematicErcAudit(args, outSuccess, outSummary);
    }
    if (toolName == "eda_calc_pcb_connectivity_audit") {
        if (args.is_object() &&
            (args.value("target", "") == "schematic" ||
             (args.contains("wires") && !args.contains("pads") &&
              !args.contains("tracks") && !args.contains("footprints")))) {
            return calcSchematicErcAudit(args, outSuccess, outSummary);
        }
        return calcPcbConnectivityAudit(args, outSuccess, outSummary);
    }
    if (toolName == "eda_calc_electrical_synth") {
        return calcElectricalSynth(args, outSuccess, outSummary);
    }
    if (toolName == "cad_calc_robot_kinematics_statics") {
        return calcRobotKinematicsStatics(args, outSuccess, outSummary);
    }
    if (toolName == "cad_calc_mechanism_synth") {
        return calcMechanismSynth(args, outSuccess, outSummary);
    }
    if (toolName == "cad_calc_print_orientation_dfm") {
        return calcPrintOrientationDfm(args, outSuccess, outSummary);
    }
    outSuccess = false;
    outSummary = "Unknown accelerator tool: " + toolName;
    return {{"ok", false}, {"error", outSummary}};
}

nlohmann::json EdaAccelerators::calcSchematicLayout(const nlohmann::json& args,
                                                    bool& outSuccess,
                                                    std::string& outSummary) const {
    if (!args.is_object() || !args.contains("components") || !args["components"].is_array() ||
        args["components"].empty()) {
        outSuccess = false;
        outSummary = "eda_calc_schematic_layout requires non-empty components[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    struct PinSpec {
        std::string number;
        std::string name;
        double dxMm = 0.0;
        double dyMm = 0.0;
    };
    struct CompNode {
        std::string ref;
        std::string category;
        double widthMm = 10.16;
        double heightMm = 10.16;
        int rank = 0;
        double barycenter = 0.0;
        double xMm = 0.0;
        double yMm = 0.0;
        std::vector<PinSpec> pins;
    };

    std::vector<CompNode> nodes;
    std::map<std::string, size_t> refToIdx;
    for (const auto& c : args["components"]) {
        if (!c.is_object()) continue;
        CompNode node;
        node.ref = c.value("reference", "U" + std::to_string(nodes.size() + 1));
        node.category = c.value("category", "mcu");
        node.widthMm = snapTo50Mil(std::max(5.08, c.value("width_mm", 10.16)));
        node.heightMm = snapTo50Mil(std::max(5.08, c.value("height_mm", 10.16)));
        node.rank = layerPriorityForCategory(node.category);

        if (c.contains("pins") && c["pins"].is_array()) {
            for (const auto& p : c["pins"]) {
                if (!p.is_object()) continue;
                PinSpec ps;
                ps.number = p.value("number", "1");
                ps.name = p.value("name", ps.number);
                ps.dxMm = snapTo50Mil(p.value("dx_mm", 0.0));
                ps.dyMm = snapTo50Mil(p.value("dy_mm", 0.0));
                node.pins.push_back(ps);
            }
        }
        if (node.pins.empty()) {
            node.pins.push_back({"1", "IN", -snapTo50Mil(node.widthMm * 0.5), 0.0});
            node.pins.push_back({"2", "OUT", snapTo50Mil(node.widthMm * 0.5), 0.0});
        }
        refToIdx[node.ref] = nodes.size();
        nodes.push_back(node);
    }

    if (nodes.empty()) {
        outSuccess = false;
        outSummary = "No valid component objects in components[]";
        return {{"ok", false}, {"error", outSummary}};
    }

    // Build adjacency from nets for barycentric ordering inside Sugiyama layers
    std::vector<std::vector<size_t>> adj(nodes.size());
    if (args.contains("nets") && args["nets"].is_array()) {
        for (const auto& net : args["nets"]) {
            if (!net.is_object() || !net.contains("connections") || !net["connections"].is_array()) continue;
            std::vector<size_t> connectedComps;
            for (const auto& conn : net["connections"]) {
                if (!conn.is_string()) continue;
                std::string s = conn.get<std::string>();
                size_t dot = s.find('.');
                std::string ref = (dot == std::string::npos) ? s : s.substr(0, dot);
                auto it = refToIdx.find(ref);
                if (it != refToIdx.end()) {
                    connectedComps.push_back(it->second);
                }
            }
            for (size_t i = 0; i < connectedComps.size(); ++i) {
                for (size_t j = i + 1; j < connectedComps.size(); ++j) {
                    adj[connectedComps[i]].push_back(connectedComps[j]);
                    adj[connectedComps[j]].push_back(connectedComps[i]);
                }
            }
        }
    }

    std::map<int, std::vector<size_t>> layers;
    for (size_t i = 0; i < nodes.size(); ++i) {
        layers[nodes[i].rank].push_back(i);
    }

    // Assign initial vertical order and run 2-pass barycenter crossing minimization
    for (auto& kv : layers) {
        for (size_t pos = 0; pos < kv.second.size(); ++pos) {
            nodes[kv.second[pos]].barycenter = static_cast<double>(pos);
        }
    }
    for (int iter = 0; iter < 2; ++iter) {
        for (auto& kv : layers) {
            auto& vec = kv.second;
            for (size_t idx : vec) {
                if (!adj[idx].empty()) {
                    double sum = 0.0;
                    for (size_t nb : adj[idx]) {
                        sum += nodes[nb].barycenter;
                    }
                    nodes[idx].barycenter = sum / static_cast<double>(adj[idx].size());
                }
            }
            std::stable_sort(vec.begin(), vec.end(), [&](size_t a, size_t b) {
                return nodes[a].barycenter < nodes[b].barycenter;
            });
            for (size_t pos = 0; pos < vec.size(); ++pos) {
                nodes[vec[pos]].barycenter = static_cast<double>(pos);
            }
        }
    }

    // Place layers horizontally with 30-grid (38.1 mm) spacing and vertical 20-grid (25.4 mm) spacing
    int colIndex = 0;
    for (const auto& kv : layers) {
        double colX = snapTo50Mil(25.4 + colIndex * 38.1);
        double curY = snapTo50Mil(25.4);
        for (size_t idx : kv.second) {
            nodes[idx].xMm = colX;
            nodes[idx].yMm = curY;
            curY = snapTo50Mil(curY + nodes[idx].heightMm + 15.24);
        }
        ++colIndex;
    }

    // Build pin coordinate lookup and body obstacle set on 50-mil integer grid
    struct GridPt {
        int gx = 0;
        int gy = 0;
        bool operator==(const GridPt& o) const { return gx == o.gx && gy == o.gy; }
        bool operator<(const GridPt& o) const {
            return (gx != o.gx) ? (gx < o.gx) : (gy < o.gy);
        }
    };

    std::set<GridPt> blockedCells;
    std::map<std::string, GridPt> pinGridLookup;

    for (const auto& node : nodes) {
        int cx = toGridCoord(node.xMm, kSchematicGridMm);
        int cy = toGridCoord(node.yMm, kSchematicGridMm);
        int hw = std::max(1, toGridCoord(node.widthMm * 0.5, kSchematicGridMm) - 1);
        int hh = std::max(1, toGridCoord(node.heightMm * 0.5, kSchematicGridMm) - 1);
        for (int gx = cx - hw; gx <= cx + hw; ++gx) {
            for (int gy = cy - hh; gy <= cy + hh; ++gy) {
                blockedCells.insert({gx, gy});
            }
        }
        for (const auto& pin : node.pins) {
            int px = toGridCoord(snapTo50Mil(node.xMm + pin.dxMm), kSchematicGridMm);
            int py = toGridCoord(snapTo50Mil(node.yMm + pin.dyMm), kSchematicGridMm);
            blockedCells.erase({px, py});
            pinGridLookup[node.ref + "." + pin.number] = {px, py};
            pinGridLookup[node.ref + "." + pin.name] = {px, py};
        }
    }

    // Route nets on 50-mil Manhattan grid using A* orthogonal search
    nlohmann::json wiresJson = nlohmann::json::array();
    nlohmann::json labelsJson = nlohmann::json::array();
    int routedNets = 0;

    auto routeTwoPoints = [&](GridPt start, GridPt goal, const std::string& netName) {
        if (start == goal) return;
        int minX = std::min(start.gx, goal.gx) - 15;
        int maxX = std::max(start.gx, goal.gx) + 15;
        int minY = std::min(start.gy, goal.gy) - 15;
        int maxY = std::max(start.gy, goal.gy) + 15;

        struct QNode {
            int f;
            int g;
            GridPt pt;
            int dir;
            bool operator>(const QNode& o) const { return f > o.f; }
        };
        std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> pq;
        std::map<GridPt, int> bestG;
        std::map<GridPt, GridPt> parent;

        auto hCost = [&](GridPt p) {
            return std::abs(p.gx - goal.gx) + std::abs(p.gy - goal.gy);
        };

        pq.push({hCost(start), 0, start, -1});
        bestG[start] = 0;

        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        bool found = false;

        while (!pq.empty()) {
            QNode cur = pq.top();
            pq.pop();
            if (cur.pt == goal) {
                found = true;
                break;
            }
            if (cur.g > bestG[cur.pt]) continue;

            for (int d = 0; d < 4; ++d) {
                GridPt nxt{cur.pt.gx + dx[d], cur.pt.gy + dy[d]};
                if (nxt.gx < minX || nxt.gx > maxX || nxt.gy < minY || nxt.gy > maxY) continue;
                if (!(nxt == goal) && !(nxt == start) && blockedCells.count(nxt)) continue;
                int bendPenalty = (cur.dir != -1 && cur.dir != d) ? 2 : 0;
                int ng = cur.g + 1 + bendPenalty;
                auto it = bestG.find(nxt);
                if (it == bestG.end() || ng < it->second) {
                    bestG[nxt] = ng;
                    parent[nxt] = cur.pt;
                    pq.push({ng + hCost(nxt), ng, nxt, d});
                }
            }
        }

        std::vector<GridPt> path;
        if (found) {
            GridPt cur = goal;
            while (!(cur == start)) {
                path.push_back(cur);
                cur = parent[cur];
            }
            path.push_back(start);
            std::reverse(path.begin(), path.end());
        } else {
            // Fallback L-route snapped to 50-mil grid
            GridPt mid{goal.gx, start.gy};
            path.push_back(start);
            if (!(mid == start) && !(mid == goal)) path.push_back(mid);
            path.push_back(goal);
        }

        // Coalesce collinear grid steps into orthogonal segments
        size_t segStart = 0;
        while (segStart + 1 < path.size()) {
            size_t segEnd = segStart + 1;
            int sdx = path[segEnd].gx - path[segStart].gx;
            int sdy = path[segEnd].gy - path[segStart].gy;
            while (segEnd + 1 < path.size()) {
                int ndx = path[segEnd + 1].gx - path[segEnd].gx;
                int ndy = path[segEnd + 1].gy - path[segEnd].gy;
                if ((sdx == 0 && ndx == 0) || (sdy == 0 && ndy == 0)) {
                    ++segEnd;
                } else {
                    break;
                }
            }
            double x1 = fromGridCoord(path[segStart].gx, kSchematicGridMm);
            double y1 = fromGridCoord(path[segStart].gy, kSchematicGridMm);
            double x2 = fromGridCoord(path[segEnd].gx, kSchematicGridMm);
            double y2 = fromGridCoord(path[segEnd].gy, kSchematicGridMm);
            wiresJson.push_back({
                {"net", netName},
                {"x1_mm", x1},
                {"y1_mm", y1},
                {"x2_mm", x2},
                {"y2_mm", y2}
            });
            segStart = segEnd;
        }
    };

    if (args.contains("nets") && args["nets"].is_array()) {
        for (const auto& net : args["nets"]) {
            if (!net.is_object() || !net.contains("connections") || !net["connections"].is_array()) continue;
            std::string netName = net.value("name", "NET");
            std::vector<GridPt> netPins;
            for (const auto& conn : net["connections"]) {
                if (!conn.is_string()) continue;
                auto it = pinGridLookup.find(conn.get<std::string>());
                if (it != pinGridLookup.end()) {
                    netPins.push_back(it->second);
                }
            }
            if (netPins.size() >= 2) {
                for (size_t i = 0; i + 1 < netPins.size(); ++i) {
                    routeTwoPoints(netPins[i], netPins[i + 1], netName);
                }
                labelsJson.push_back({
                    {"name", netName},
                    {"x_mm", fromGridCoord(netPins[0].gx, kSchematicGridMm)},
                    {"y_mm", fromGridCoord(netPins[0].gy, kSchematicGridMm)}
                });
                ++routedNets;
            }
        }
    }

    nlohmann::json symbolsJson = nlohmann::json::array();
    for (const auto& n : nodes) {
        nlohmann::json pinsArr = nlohmann::json::array();
        for (const auto& p : n.pins) {
            pinsArr.push_back({
                {"number", p.number},
                {"name", p.name},
                {"x_mm", snapTo50Mil(n.xMm + p.dxMm)},
                {"y_mm", snapTo50Mil(n.yMm + p.dyMm)}
            });
        }
        symbolsJson.push_back({
            {"reference", n.ref},
            {"category", n.category},
            {"layer_rank", n.rank},
            {"x_mm", n.xMm},
            {"y_mm", n.yMm},
            {"rotation_deg", 0},
            {"pins", pinsArr}
        });
    }

    outSuccess = true;
    outSummary = "Placed " + std::to_string(nodes.size()) + " symbols • Routed " +
                 std::to_string(routedNets) + " nets on 50-mil grid";
    return {
        {"ok", true},
        {"grid_mm", kSchematicGridMm},
        {"symbols", symbolsJson},
        {"wires", wiresJson},
        {"labels", labelsJson},
        {"routed_nets", routedNets},
        {"summary", outSummary}
    };
}

nlohmann::json EdaAccelerators::calcPcbPlacement(const nlohmann::json& args,
                                                 bool& outSuccess,
                                                 std::string& outSummary) const {
    if (!args.is_object() || !args.contains("components") || !args["components"].is_array() ||
        args["components"].empty()) {
        outSuccess = false;
        outSummary = "eda_calc_pcb_placement requires non-empty components[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    double boardW = 60.0;
    double boardH = 40.0;
    double clearanceMm = 0.5;
    if (args.contains("board") && args["board"].is_object()) {
        boardW = std::max(10.0, args["board"].value("width_mm", 60.0));
        boardH = std::max(10.0, args["board"].value("height_mm", 40.0));
        clearanceMm = std::max(0.1, args["board"].value("clearance_mm", 0.5));
    }

    struct PcbItem {
        std::string ref;
        double w = 3.0;
        double h = 3.0;
        double x = 0.0;
        double y = 0.0;
        int rotDeg = 0;
        std::string layer = "F.Cu";
        bool fixed = false;
        std::string decouplesRef;
    };

    std::vector<PcbItem> items;
    std::map<std::string, size_t> refIdx;
    int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(args["components"].size())))));
    for (size_t i = 0; i < args["components"].size(); ++i) {
        const auto& c = args["components"][i];
        if (!c.is_object()) continue;
        PcbItem item;
        item.ref = c.value("reference", "U" + std::to_string(i + 1));
        item.w = std::max(0.5, c.value("width_mm", 3.0));
        item.h = std::max(0.5, c.value("height_mm", 3.0));
        item.fixed = c.value("fixed", false);
        item.layer = c.value("layer", "F.Cu");
        item.rotDeg = c.value("rotation_deg", 0);
        item.decouplesRef = c.value("decouples", "");

        int r = static_cast<int>(i) / cols;
        int col = static_cast<int>(i) % cols;
        double initX = (col + 1) * (boardW / (cols + 1));
        double initY = (r + 1) * (boardH / (cols + 1));
        item.x = c.value("x_mm", initX);
        item.y = c.value("y_mm", initY);
        refIdx[item.ref] = items.size();
        items.push_back(item);
    }

    std::vector<std::vector<size_t>> netLists;
    if (args.contains("nets") && args["nets"].is_array()) {
        for (const auto& net : args["nets"]) {
            if (!net.is_object() || !net.contains("pins") || !net["pins"].is_array()) continue;
            std::vector<size_t> members;
            for (const auto& p : net["pins"]) {
                if (!p.is_string()) continue;
                std::string s = p.get<std::string>();
                size_t dot = s.find('.');
                std::string ref = (dot == std::string::npos) ? s : s.substr(0, dot);
                auto it = refIdx.find(ref);
                if (it != refIdx.end()) members.push_back(it->second);
            }
            if (members.size() >= 2) netLists.push_back(members);
        }
    }

    auto effW = [](const PcbItem& it) { return (it.rotDeg % 180 != 0) ? it.h : it.w; };
    auto effH = [](const PcbItem& it) { return (it.rotDeg % 180 != 0) ? it.w : it.h; };

    auto computeCost = [&](const std::vector<PcbItem>& state, double& outHpwl, int& outOverlaps) {
        outHpwl = 0.0;
        for (const auto& nl : netLists) {
            double minX = state[nl[0]].x, maxX = state[nl[0]].x;
            double minY = state[nl[0]].y, maxY = state[nl[0]].y;
            for (size_t idx : nl) {
                minX = std::min(minX, state[idx].x);
                maxX = std::max(maxX, state[idx].x);
                minY = std::min(minY, state[idx].y);
                maxY = std::max(maxY, state[idx].y);
            }
            outHpwl += (maxX - minX) + (maxY - minY);
        }

        double overlapArea = 0.0;
        outOverlaps = 0;
        for (size_t i = 0; i < state.size(); ++i) {
            for (size_t j = i + 1; j < state.size(); ++j) {
                if (state[i].layer != state[j].layer) continue;
                double wi = effW(state[i]) * 0.5 + clearanceMm;
                double hi = effH(state[i]) * 0.5 + clearanceMm;
                double wj = effW(state[j]) * 0.5 + clearanceMm;
                double hj = effH(state[j]) * 0.5 + clearanceMm;
                double ox = (wi + wj) - std::fabs(state[i].x - state[j].x);
                double oy = (hi + hj) - std::fabs(state[i].y - state[j].y);
                if (ox > 0.0 && oy > 0.0) {
                    overlapArea += ox * oy;
                    ++outOverlaps;
                }
            }
        }

        double decapPenalty = 0.0;
        for (const auto& it : state) {
            if (!it.decouplesRef.empty()) {
                auto targetIt = refIdx.find(it.decouplesRef);
                if (targetIt != refIdx.end()) {
                    const auto& target = state[targetIt->second];
                    double dist = std::hypot(it.x - target.x, it.y - target.y);
                    double idealDist = (effW(target) * 0.5 + effW(it) * 0.5 + clearanceMm * 2.0 + 0.5);
                    if (dist > idealDist + 2.5) {
                        decapPenalty += (dist - idealDist) * 25.0;
                    }
                }
            }
        }

        return outHpwl + overlapArea * 500.0 + decapPenalty;
    };

    // Deterministic xorshift64* PRNG for Simulated Annealing
    uint64_t rngState = 0x9E3779B97F4A7C15ULL;
    auto nextRand = [&]() -> double {
        rngState ^= rngState >> 12;
        rngState ^= rngState << 25;
        rngState ^= rngState >> 27;
        uint64_t val = rngState * 2685821657736338717ULL;
        return static_cast<double>(val & 0xFFFFFFULL) / static_cast<double>(0x1000000ULL);
    };

    double curHpwl = 0.0;
    int curOverlaps = 0;
    double curCost = computeCost(items, curHpwl, curOverlaps);
    std::vector<PcbItem> bestItems = items;
    double bestCost = curCost;

    int totalIters = std::clamp(_config.saPlacementIterations, 500, 15000);
    for (int iter = 0; iter < totalIters; ++iter) {
        double temp = std::max(0.05, 10.0 * (1.0 - static_cast<double>(iter) / totalIters));
        size_t idx = static_cast<size_t>(nextRand() * items.size()) % items.size();
        if (items[idx].fixed) continue;

        PcbItem saved = items[idx];
        double moveRange = std::max(0.5, boardW * 0.25 * (temp / 10.0));
        items[idx].x += (nextRand() * 2.0 - 1.0) * moveRange;
        items[idx].y += (nextRand() * 2.0 - 1.0) * moveRange;
        if (nextRand() < 0.15) {
            items[idx].rotDeg = (items[idx].rotDeg + 90) % 360;
        }

        double halfW = effW(items[idx]) * 0.5 + clearanceMm;
        double halfH = effH(items[idx]) * 0.5 + clearanceMm;
        items[idx].x = std::clamp(items[idx].x, halfW, std::max(halfW, boardW - halfW));
        items[idx].y = std::clamp(items[idx].y, halfH, std::max(halfH, boardH - halfH));

        double newHpwl = 0.0;
        int newOverlaps = 0;
        double newCost = computeCost(items, newHpwl, newOverlaps);
        double delta = newCost - curCost;
        if (delta <= 0.0 || std::exp(-delta / temp) > nextRand()) {
            curCost = newCost;
            if (newCost < bestCost) {
                bestCost = newCost;
                bestItems = items;
            }
        } else {
            items[idx] = saved;
        }
    }

    items = bestItems;

    // Final deterministic overlap legalizer pass
    for (int pass = 0; pass < 40; ++pass) {
        double hpwl = 0.0;
        int overlaps = 0;
        computeCost(items, hpwl, overlaps);
        if (overlaps == 0) break;
        for (size_t i = 0; i < items.size(); ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                if (items[i].layer != items[j].layer) continue;
                double wi = effW(items[i]) * 0.5 + clearanceMm;
                double hi = effH(items[i]) * 0.5 + clearanceMm;
                double wj = effW(items[j]) * 0.5 + clearanceMm;
                double hj = effH(items[j]) * 0.5 + clearanceMm;
                double ox = (wi + wj) - std::fabs(items[i].x - items[j].x);
                double oy = (hi + hj) - std::fabs(items[i].y - items[j].y);
                if (ox > 0.0 && oy > 0.0) {
                    size_t mover = items[j].fixed ? i : j;
                    if (!items[mover].fixed) {
                        if (ox < oy) {
                            items[mover].x += (items[mover].x >= items[i].x) ? (ox + 0.25) : -(ox + 0.25);
                        } else {
                            items[mover].y += (items[mover].y >= items[i].y) ? (oy + 0.25) : -(oy + 0.25);
                        }
                        double hw = effW(items[mover]) * 0.5 + clearanceMm;
                        double hh = effH(items[mover]) * 0.5 + clearanceMm;
                        items[mover].x = std::clamp(items[mover].x, hw, std::max(hw, boardW - hw));
                        items[mover].y = std::clamp(items[mover].y, hh, std::max(hh, boardH - hh));
                    }
                }
            }
        }
    }

    double finalHpwl = 0.0;
    int finalOverlaps = 0;
    computeCost(items, finalHpwl, finalOverlaps);

    nlohmann::json placementsJson = nlohmann::json::array();
    for (const auto& it : items) {
        placementsJson.push_back({
            {"reference", it.ref},
            {"x_mm", std::round(it.x * 100.0) / 100.0},
            {"y_mm", std::round(it.y * 100.0) / 100.0},
            {"rotation_deg", it.rotDeg},
            {"layer", it.layer}
        });
    }

    outSuccess = (finalOverlaps == 0);
    outSummary = "Placed " + std::to_string(items.size()) + " footprints • HPWL " +
                 std::to_string(static_cast<int>(std::round(finalHpwl))) + " mm • " +
                 std::to_string(finalOverlaps) + " overlaps";
    return {
        {"ok", outSuccess},
        {"placements", placementsJson},
        {"hpwl_mm", std::round(finalHpwl * 100.0) / 100.0},
        {"courtyard_overlaps", finalOverlaps},
        {"summary", outSummary}
    };
}

nlohmann::json EdaAccelerators::calcPcbAutoroute(const nlohmann::json& args,
                                                 bool& outSuccess,
                                                 std::string& outSummary) const {
    if (!args.is_object() || !args.contains("nets") || !args["nets"].is_array()) {
        outSuccess = false;
        outSummary = "eda_calc_pcb_autoroute requires nets[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    double boardW = 40.0;
    double boardH = 30.0;
    int numLayers = 2;
    bool useEasyEdaNames = false;
    if (args.contains("board") && args["board"].is_object()) {
        boardW = std::max(5.0, args["board"].value("width_mm", 40.0));
        boardH = std::max(5.0, args["board"].value("height_mm", 30.0));
        numLayers = std::clamp(args["board"].value("layers", 2), 1, 6);
        std::string style = args["board"].value("layer_style", "");
        if (style == "easyeda" || style == "EasyEDA") {
            useEasyEdaNames = true;
        }
    }

    auto isEasyEdaLayerToken = [](const std::string& l) -> bool {
        return l == "TopLayer" || l == "BottomLayer" ||
               l == "InnerLayer1" || l == "InnerLayer2" ||
               l == "InnerLayer3" || l == "InnerLayer4" ||
               l == "MultiLayer" || l == "1" || l == "2" ||
               l == "12" || l == "15" || l == "16" || l == "17" || l == "18";
    };

    if (!useEasyEdaNames) {
        for (const auto& n : args["nets"]) {
            if (!n.is_object() || !n.contains("pads") || !n["pads"].is_array()) continue;
            for (const auto& p : n["pads"]) {
                if (p.is_object() && isEasyEdaLayerToken(p.value("layer", ""))) {
                    useEasyEdaNames = true;
                    break;
                }
            }
        }
    }

    double trackWidthMm = 0.25;
    double clearanceMm = 0.20;
    double viaSizeMm = 0.60;
    double viaDrillMm = 0.30;
    if (args.contains("rules") && args["rules"].is_object()) {
        trackWidthMm = std::max(0.1, args["rules"].value("track_width_mm", 0.25));
        clearanceMm = std::max(0.1, args["rules"].value("clearance_mm", 0.20));
        viaSizeMm = std::max(0.3, args["rules"].value("via_size_mm", 0.60));
        viaDrillMm = std::max(0.15, args["rules"].value("via_drill_mm", 0.30));
    }

    double gridMm = std::clamp(_config.autorouteGridMm, 0.20, 1.0);
    int gxMax = std::max(4, static_cast<int>(std::ceil(boardW / gridMm)));
    int gyMax = std::max(4, static_cast<int>(std::ceil(boardH / gridMm)));
    if (gxMax * gyMax > 40000) {
        gridMm = std::sqrt((boardW * boardH) / 25000.0);
        gxMax = std::max(4, static_cast<int>(std::ceil(boardW / gridMm)));
        gyMax = std::max(4, static_cast<int>(std::ceil(boardH / gridMm)));
    }

    // Returns -1 for MultiLayer / PTH pads that span all copper layers
    auto layerNameToIdx = [&](const std::string& l) -> int {
        if (l == "MultiLayer" || l == "12" || l == "*.Cu" ||
            l == "Multi-Layer" || l == "PTH" || l == "thru_hole") {
            return -1;
        }
        if ((l == "B.Cu" || l == "BottomLayer" || l == "2" || l == "Bottom") && numLayers >= 2) {
            return numLayers - 1;
        }
        if ((l == "In1.Cu" || l == "InnerLayer1" || l == "15") && numLayers >= 3) return 1;
        if ((l == "In2.Cu" || l == "InnerLayer2" || l == "16") && numLayers >= 4) return 2;
        if ((l == "In3.Cu" || l == "InnerLayer3" || l == "17") && numLayers >= 5) return 3;
        if ((l == "In4.Cu" || l == "InnerLayer4" || l == "18") && numLayers >= 6) return 4;
        return 0;
    };
    auto layerIdxToName = [&](int idx) -> std::string {
        if (useEasyEdaNames) {
            if (idx <= 0) return "TopLayer";
            if (idx == numLayers - 1 && numLayers >= 2) return "BottomLayer";
            return "InnerLayer" + std::to_string(idx);
        }
        if (idx <= 0) return "F.Cu";
        if (idx == numLayers - 1 && numLayers >= 2) return "B.Cu";
        return "In" + std::to_string(idx) + ".Cu";
    };

    auto nodeIdx = [&](int l, int x, int y) -> size_t {
        return static_cast<size_t>((l * gyMax + y) * gxMax + x);
    };
    size_t totalCells = static_cast<size_t>(numLayers * gxMax * gyMax);
    std::vector<uint8_t> obstacleMask(totalCells, 0);
    std::vector<std::string> existingNetOwner(totalCells, "");

    if (args.contains("obstacles") && args["obstacles"].is_array()) {
        for (const auto& obs : args["obstacles"]) {
            if (!obs.is_object()) continue;
            double ox = obs.value("x_mm", obs.value("x", 0.0));
            double oy = obs.value("y_mm", obs.value("y", 0.0));
            double ow = obs.value("width_mm", obs.value("size_x", 1.0)) * 0.5 + clearanceMm;
            double oh = obs.value("height_mm", obs.value("size_y", 1.0)) * 0.5 + clearanceMm;
            int l = layerNameToIdx(obs.value("layer", "F.Cu"));
            int x0 = std::max(0, toGridCoord(ox - ow, gridMm));
            int x1 = std::min(gxMax - 1, toGridCoord(ox + ow, gridMm));
            int y0 = std::max(0, toGridCoord(oy - oh, gridMm));
            int y1 = std::min(gyMax - 1, toGridCoord(oy + oh, gridMm));
            for (int ly = 0; ly < numLayers; ++ly) {
                if (l != -1 && ly != l) continue;
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        obstacleMask[nodeIdx(ly, x, y)] = 1;
                    }
                }
            }
        }
    }

    // Rasterize existing_tracks and existing_vias for incremental routing
    if (args.contains("existing_tracks") && args["existing_tracks"].is_array()) {
        for (const auto& et : args["existing_tracks"]) {
            if (!et.is_object()) continue;
            std::string netName = et.value("net", "");
            int l = layerNameToIdx(et.value("layer", "F.Cu"));
            if (l < 0) l = 0;
            double x1 = et.value("x1_mm", et.value("x1", 0.0));
            double y1 = et.value("y1_mm", et.value("y1", 0.0));
            double x2 = et.value("x2_mm", et.value("x2", 0.0));
            double y2 = et.value("y2_mm", et.value("y2", 0.0));
            double w = et.value("width_mm", et.value("width", trackWidthMm));
            double halo = 0.5 * (w + trackWidthMm) + clearanceMm;
            int haloCells = std::max(1, static_cast<int>(std::ceil(halo / gridMm)));

            double segLen = std::hypot(x2 - x1, y2 - y1);
            int steps = std::max(1, static_cast<int>(std::ceil(segLen / (gridMm * 0.5))));
            for (int s = 0; s <= steps; ++s) {
                double t = static_cast<double>(s) / static_cast<double>(steps);
                int cx = std::clamp(toGridCoord(x1 + t * (x2 - x1), gridMm), 0, gxMax - 1);
                int cy = std::clamp(toGridCoord(y1 + t * (y2 - y1), gridMm), 0, gyMax - 1);
                existingNetOwner[nodeIdx(l, cx, cy)] = netName;
                for (int dy = -haloCells; dy <= haloCells; ++dy) {
                    for (int dx = -haloCells; dx <= haloCells; ++dx) {
                        int nx = cx + dx;
                        int ny = cy + dy;
                        if (nx < 0 || nx >= gxMax || ny < 0 || ny >= gyMax) continue;
                        size_t cid = nodeIdx(l, nx, ny);
                        if (existingNetOwner[cid].empty()) {
                            existingNetOwner[cid] = netName;
                        }
                        obstacleMask[cid] = 1;
                    }
                }
            }
        }
    }

    if (args.contains("existing_vias") && args["existing_vias"].is_array()) {
        for (const auto& ev : args["existing_vias"]) {
            if (!ev.is_object()) continue;
            std::string netName = ev.value("net", "");
            double vx = ev.value("x_mm", ev.value("x", 0.0));
            double vy = ev.value("y_mm", ev.value("y", 0.0));
            double vd = ev.value("size_mm", ev.value("diameter", viaSizeMm));
            int haloCells = std::max(1, static_cast<int>(std::ceil((0.5 * vd + clearanceMm) / gridMm)));
            int cx = std::clamp(toGridCoord(vx, gridMm), 0, gxMax - 1);
            int cy = std::clamp(toGridCoord(vy, gridMm), 0, gyMax - 1);
            for (int ly = 0; ly < numLayers; ++ly) {
                for (int dy = -haloCells; dy <= haloCells; ++dy) {
                    for (int dx = -haloCells; dx <= haloCells; ++dx) {
                        int nx = cx + dx;
                        int ny = cy + dy;
                        if (nx < 0 || nx >= gxMax || ny < 0 || ny >= gyMax) continue;
                        size_t cid = nodeIdx(ly, nx, ny);
                        if (existingNetOwner[cid].empty()) {
                            existingNetOwner[cid] = netName;
                        }
                        obstacleMask[cid] = 1;
                    }
                }
            }
        }
    }

    struct PadCell {
        int l = 0; // -1 means MultiLayer (all copper layers)
        int x = 0;
        int y = 0;
    };
    struct NetSpec {
        std::string name;
        std::vector<PadCell> pads;
        std::vector<size_t> routedCells;
        std::vector<std::vector<size_t>> pairPaths;
        bool routed = false;
    };

    std::vector<NetSpec> nets;
    for (const auto& n : args["nets"]) {
        if (!n.is_object() || !n.contains("pads") || !n["pads"].is_array()) continue;
        NetSpec ns;
        ns.name = n.value("name", "NET_" + std::to_string(nets.size() + 1));
        for (const auto& p : n["pads"]) {
            if (!p.is_object()) continue;
            PadCell pc;
            pc.l = layerNameToIdx(p.value("layer", "F.Cu"));
            pc.x = std::clamp(toGridCoord(p.value("x_mm", p.value("x", 0.0)), gridMm), 0, gxMax - 1);
            pc.y = std::clamp(toGridCoord(p.value("y_mm", p.value("y", 0.0)), gridMm), 0, gyMax - 1);
            if (pc.l == -1) {
                for (int ly = 0; ly < numLayers; ++ly) {
                    obstacleMask[nodeIdx(ly, pc.x, pc.y)] = 0;
                }
            } else {
                obstacleMask[nodeIdx(pc.l, pc.x, pc.y)] = 0;
            }
            ns.pads.push_back(pc);
        }
        if (ns.pads.size() >= 2) nets.push_back(ns);
    }

    std::vector<int> occupancy(totalCells, 0);
    std::vector<double> historyCost(totalCells, 1.0);
    int maxPasses = std::clamp(_config.autorouteMaxRipupPasses, 1, 50);

    // 8 octilinear 2D directions (dx, dy)
    const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    const int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const double stepCost[8] = {1.0, 1.0, 1.0, 1.0, 1.4142, 1.4142, 1.4142, 1.4142};

    for (int pass = 0; pass < maxPasses; ++pass) {
        double presentFactor = 1.0 + pass * 1.5;
        bool anyCongestion = false;
        for (size_t c = 0; c < totalCells; ++c) {
            if (occupancy[c] > 1) {
                anyCongestion = true;
                historyCost[c] += (occupancy[c] - 1) * 1.0;
            }
        }
        if (pass > 0 && !anyCongestion) {
            bool allDone = true;
            for (const auto& n : nets) {
                if (!n.routed) allDone = false;
            }
            if (allDone) break;
        }

        for (auto& net : nets) {
            if (pass > 0 && net.routed) {
                bool sharesOveruse = false;
                for (size_t c : net.routedCells) {
                    if (occupancy[c] > 1) {
                        sharesOveruse = true;
                        break;
                    }
                }
                if (!sharesOveruse) continue;
                for (size_t c : net.routedCells) {
                    occupancy[c] = std::max(0, occupancy[c] - 1);
                }
                net.routedCells.clear();
                net.pairPaths.clear();
                net.routed = false;
            }

            std::set<size_t> netCells;
            std::vector<std::vector<size_t>> pairPaths;
            bool okNet = true;
            for (size_t pIdx = 0; pIdx + 1 < net.pads.size(); ++pIdx) {
                PadCell src = net.pads[pIdx];
                PadCell dst = net.pads[pIdx + 1];

                struct ANode {
                    double f;
                    double g;
                    size_t id;
                    bool operator>(const ANode& o) const { return f > o.f; }
                };
                std::priority_queue<ANode, std::vector<ANode>, std::greater<ANode>> pq;
                std::vector<double> bestG(totalCells, std::numeric_limits<double>::infinity());
                std::vector<size_t> parent(totalCells, std::numeric_limits<size_t>::max());

                auto heuristic = [&](int l, int x, int y) {
                    double ddx = std::abs(x - dst.x);
                    double ddy = std::abs(y - dst.y);
                    double oct = (ddx + ddy) + (1.4142 - 2.0) * std::min(ddx, ddy);
                    double layerPen = (dst.l != -1 && l != dst.l) ? 4.0 : 0.0;
                    return oct + layerPen;
                };

                auto isGoalCell = [&](int l, int x, int y) -> bool {
                    if (x != dst.x || y != dst.y) return false;
                    return (dst.l == -1 || l == dst.l);
                };

                auto isCellBlockedForNet = [&](size_t cid) -> bool {
                    if (!obstacleMask[cid]) return false;
                    if (!existingNetOwner[cid].empty() && existingNetOwner[cid] == net.name) {
                        return false;
                    }
                    return true;
                };

                if (src.l == -1) {
                    for (int ly = 0; ly < numLayers; ++ly) {
                        size_t sid = nodeIdx(ly, src.x, src.y);
                        bestG[sid] = 0.0;
                        pq.push({heuristic(ly, src.x, src.y), 0.0, sid});
                    }
                } else {
                    size_t sid = nodeIdx(src.l, src.x, src.y);
                    bestG[sid] = 0.0;
                    pq.push({heuristic(src.l, src.x, src.y), 0.0, sid});
                }

                bool segFound = false;
                size_t reachedGoalId = std::numeric_limits<size_t>::max();

                while (!pq.empty()) {
                    ANode cur = pq.top();
                    pq.pop();

                    int rem = static_cast<int>(cur.id);
                    int cx = rem % gxMax;
                    rem /= gxMax;
                    int cy = rem % gyMax;
                    int cl = rem / gyMax;

                    if (isGoalCell(cl, cx, cy)) {
                        segFound = true;
                        reachedGoalId = cur.id;
                        break;
                    }
                    if (cur.g > bestG[cur.id]) continue;

                    // Planar 8-way moves
                    for (int d = 0; d < 8; ++d) {
                        int nx = cx + dx[d];
                        int ny = cy + dy[d];
                        if (nx < 0 || nx >= gxMax || ny < 0 || ny >= gyMax) continue;
                        size_t nid = nodeIdx(cl, nx, ny);
                        if (!isGoalCell(cl, nx, ny) && isCellBlockedForNet(nid)) continue;
                        double cong = 1.0 + occupancy[nid] * presentFactor;
                        double stepMult = (!existingNetOwner[nid].empty() && existingNetOwner[nid] == net.name)
                                              ? 0.1
                                              : stepCost[d];
                        double ng = cur.g + stepMult * historyCost[nid] * cong;
                        if (ng < bestG[nid]) {
                            bestG[nid] = ng;
                            parent[nid] = cur.id;
                            pq.push({ng + heuristic(cl, nx, ny), ng, nid});
                        }
                    }

                    // Layer transition (via)
                    for (int nl = 0; nl < numLayers; ++nl) {
                        if (nl == cl) continue;
                        size_t nid = nodeIdx(nl, cx, cy);
                        if (!isGoalCell(nl, cx, cy) && isCellBlockedForNet(nid)) continue;
                        double cong = 1.0 + occupancy[nid] * presentFactor;
                        double ng = cur.g + 5.0 * historyCost[nid] * cong;
                        if (ng < bestG[nid]) {
                            bestG[nid] = ng;
                            parent[nid] = cur.id;
                            pq.push({ng + heuristic(nl, cx, cy), ng, nid});
                        }
                    }
                }

                if (!segFound) {
                    okNet = false;
                    break;
                }
                std::vector<size_t> ordered;
                size_t cur = reachedGoalId;
                while (cur != std::numeric_limits<size_t>::max()) {
                    netCells.insert(cur);
                    ordered.push_back(cur);
                    cur = parent[cur];
                }
                std::reverse(ordered.begin(), ordered.end());
                pairPaths.push_back(std::move(ordered));
            }

            if (okNet) {
                net.routed = true;
                net.routedCells.assign(netCells.begin(), netCells.end());
                net.pairPaths = std::move(pairPaths);
                for (size_t c : net.routedCells) {
                    occupancy[c] += 1;
                }
            }
        }
    }

    int drcViolations = 0;
    for (size_t c = 0; c < totalCells; ++c) {
        if (occupancy[c] > 1) ++drcViolations;
    }

    nlohmann::json tracksJson = nlohmann::json::array();
    nlohmann::json viasJson = nlohmann::json::array();
    int routedCount = 0;
    int unroutedCount = 0;

    auto decodeCell = [&](size_t cid, int& outL, int& outX, int& outY) {
        int rem = static_cast<int>(cid);
        outX = rem % gxMax;
        rem /= gxMax;
        outY = rem % gyMax;
        outL = rem / gyMax;
    };

    for (const auto& net : nets) {
        if (!net.routed || net.routedCells.empty()) {
            ++unroutedCount;
            continue;
        }
        ++routedCount;

        for (const auto& path : net.pairPaths) {
            if (path.size() < 2) continue;
            int curL = 0, startX = 0, startY = 0;
            decodeCell(path[0], curL, startX, startY);
            int prevX = startX, prevY = startY;
            int dirX = 0, dirY = 0;
            bool hasDir = false;

            for (size_t k = 1; k < path.size(); ++k) {
                int nl = 0, nx = 0, ny = 0;
                decodeCell(path[k], nl, nx, ny);
                if (nl != curL) {
                    if (prevX != startX || prevY != startY) {
                        tracksJson.push_back({
                            {"net", net.name},
                            {"layer", layerIdxToName(curL)},
                            {"x1_mm", fromGridCoord(startX, gridMm)},
                            {"y1_mm", fromGridCoord(startY, gridMm)},
                            {"x2_mm", fromGridCoord(prevX, gridMm)},
                            {"y2_mm", fromGridCoord(prevY, gridMm)},
                            {"width_mm", trackWidthMm}
                        });
                    }
                    viasJson.push_back({
                        {"net", net.name},
                        {"x_mm", fromGridCoord(nx, gridMm)},
                        {"y_mm", fromGridCoord(ny, gridMm)},
                        {"size_mm", viaSizeMm},
                        {"drill_mm", viaDrillMm}
                    });
                    curL = nl;
                    startX = nx;
                    startY = ny;
                    prevX = nx;
                    prevY = ny;
                    hasDir = false;
                } else {
                    int sdx = nx - prevX;
                    int sdy = ny - prevY;
                    if (!hasDir) {
                        dirX = sdx;
                        dirY = sdy;
                        hasDir = true;
                    } else if (sdx != dirX || sdy != dirY) {
                        tracksJson.push_back({
                            {"net", net.name},
                            {"layer", layerIdxToName(curL)},
                            {"x1_mm", fromGridCoord(startX, gridMm)},
                            {"y1_mm", fromGridCoord(startY, gridMm)},
                            {"x2_mm", fromGridCoord(prevX, gridMm)},
                            {"y2_mm", fromGridCoord(prevY, gridMm)},
                            {"width_mm", trackWidthMm}
                        });
                        startX = prevX;
                        startY = prevY;
                        dirX = sdx;
                        dirY = sdy;
                    }
                    prevX = nx;
                    prevY = ny;
                }
            }
            if (prevX != startX || prevY != startY) {
                tracksJson.push_back({
                    {"net", net.name},
                    {"layer", layerIdxToName(curL)},
                    {"x1_mm", fromGridCoord(startX, gridMm)},
                    {"y1_mm", fromGridCoord(startY, gridMm)},
                    {"x2_mm", fromGridCoord(prevX, gridMm)},
                    {"y2_mm", fromGridCoord(prevY, gridMm)},
                    {"width_mm", trackWidthMm}
                });
            }
        }
    }

    outSuccess = (unroutedCount == 0 && drcViolations == 0);
    outSummary = "Routed " + std::to_string(routedCount) + "/" + std::to_string(nets.size()) +
                 " nets • " + std::to_string(drcViolations) + " DRC";
    return {
        {"ok", outSuccess},
        {"routed_nets", routedCount},
        {"unrouted_nets", unroutedCount},
        {"drc_violations", drcViolations},
        {"tracks", tracksJson},
        {"vias", viasJson},
        {"summary", outSummary}
    };
}

nlohmann::json EdaAccelerators::calcElectricalSynth(const nlohmann::json& args,
                                                    bool& outSuccess,
                                                    std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "eda_calc_electrical_synth requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    std::string mode = args.value("mode", "divider");

    if (mode == "divider" || mode == "feedback" || mode == "rc_filter") {
        double vin = args.value("vin_v", 5.0);
        double voutTarget = args.value("vout_target_v", 3.3);
        std::string series = args.value("series", "E24");
        if (vin <= 0.0 || voutTarget <= 0.0 || voutTarget >= vin) {
            outSuccess = false;
            outSummary = "Invalid divider voltages (require 0 < vout_target_v < vin_v)";
            return {{"ok", false}, {"error", outSummary}};
        }

        const double* basePtr = nullptr;
        size_t baseLen = 0;
        getESeriesBase(series, _config.preferJlcpcbBasicESeries, basePtr, baseLen);
        std::vector<double> candidates;
        for (double decade : {1000.0, 10000.0, 100000.0}) {
            for (size_t i = 0; i < baseLen; ++i) {
                candidates.push_back(basePtr[i] * decade);
            }
        }

        double bestRTop = 10000.0;
        double bestRBot = 10000.0;
        double bestErr = std::numeric_limits<double>::max();
        for (double rt : candidates) {
            for (double rb : candidates) {
                double vActual = vin * (rb / (rt + rb));
                double err = std::fabs(vActual - voutTarget) / voutTarget;
                if (err < bestErr) {
                    bestErr = err;
                    bestRTop = rt;
                    bestRBot = rb;
                }
            }
        }

        double voutActual = vin * (bestRBot / (bestRTop + bestRBot));
        double tol = (series == "E96") ? 0.01 : 0.01;
        double voutMin = vin * ((bestRBot * (1.0 - tol)) / (bestRTop * (1.0 + tol) + bestRBot * (1.0 - tol)));
        double voutMax = vin * ((bestRBot * (1.0 + tol)) / (bestRTop * (1.0 - tol) + bestRBot * (1.0 + tol)));

        outSuccess = true;
        outSummary = "Synthesized " + series + " divider R_top=" + std::to_string(static_cast<int>(bestRTop)) +
                     "R, R_bot=" + std::to_string(static_cast<int>(bestRBot)) + "R (err " +
                     std::to_string(bestErr * 100.0).substr(0, 4) + "%)";
        return {
            {"ok", true},
            {"mode", mode},
            {"r_top_ohm", bestRTop},
            {"r_bottom_ohm", bestRBot},
            {"vout_target_v", voutTarget},
            {"vout_actual_v", voutActual},
            {"vout_min_v", voutMin},
            {"vout_max_v", voutMax},
            {"relative_error_pct", bestErr * 100.0},
            {"summary", outSummary}
        };
    }

    if (mode == "ipc2152") {
        double currentA = std::max(0.05, args.value("current_a", 1.0));
        double tempRiseC = std::max(5.0, args.value("temp_rise_c", 20.0));
        double copperOz = std::max(0.5, args.value("copper_oz", _config.defaultCopperWeightOz));
        double lengthMm = std::max(1.0, args.value("length_mm", 25.0));

        // IPC-2152 / IPC-2221 analytical cross-sectional area in sq mils: I = k * dT^0.44 * A^0.725
        double areaExtMils2 = std::pow(currentA / (0.048 * std::pow(tempRiseC, 0.44)), 1.0 / 0.725);
        double areaIntMils2 = std::pow(currentA / (0.024 * std::pow(tempRiseC, 0.44)), 1.0 / 0.725);
        double thicknessMils = copperOz * 1.378;
        double extWidthMm = (areaExtMils2 / thicknessMils) * 0.0254;
        double intWidthMm = (areaIntMils2 / thicknessMils) * 0.0254;

        double thicknessM = (thicknessMils * 0.0254) * 1e-3;
        double widthM = std::max(0.15e-3, extWidthMm * 1e-3);
        double resistanceOhm = 1.724e-8 * (lengthMm * 1e-3) / (widthM * thicknessM);
        double voltageDropV = currentA * resistanceOhm;
        int recommendedVias = std::max(1, static_cast<int>(std::ceil(currentA / 1.2)));

        outSuccess = true;
        outSummary = "IPC-2152 trace width " + std::to_string(extWidthMm).substr(0, 4) + " mm (" +
                     std::to_string(recommendedVias) + " vias) for " + std::to_string(currentA).substr(0, 4) + " A";
        return {
            {"ok", true},
            {"mode", "ipc2152"},
            {"external_width_mm", extWidthMm},
            {"internal_width_mm", intWidthMm},
            {"resistance_ohm", resistanceOhm},
            {"voltage_drop_v", voltageDropV},
            {"recommended_vias", recommendedVias},
            {"summary", outSummary}
        };
    }

    if (mode == "impedance") {
        std::string type = args.value("type", "microstrip");
        double targetZ = std::max(20.0, args.value("target_z_ohm", 50.0));
        double hMm = std::max(0.05, args.value("dielectric_h_mm", 0.15));
        double er = std::max(2.0, args.value("er", 4.4));
        double sMm = std::max(0.08, args.value("spacing_mm", 0.15));
        double tMm = 0.035;

        // Binary search width wMm for Hammerstad-Jensen microstrip impedance
        auto calcZ0 = [&](double w) {
            double effEr = (er + 1.0) * 0.5 + ((er - 1.0) * 0.5) / std::sqrt(1.0 + 12.0 * hMm / w);
            double z0 = (87.0 / std::sqrt(effEr + 1.41)) * std::log((5.98 * hMm) / (0.8 * w + tMm));
            if (type == "diff_microstrip") {
                return 2.0 * z0 * (1.0 - 0.48 * std::exp(-0.96 * (sMm / hMm)));
            }
            return z0;
        };

        double lo = 0.04, hi = 4.0;
        for (int i = 0; i < 40; ++i) {
            double mid = 0.5 * (lo + hi);
            if (calcZ0(mid) > targetZ) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        double bestW = 0.5 * (lo + hi);
        double achievedZ = calcZ0(bestW);

        outSuccess = true;
        outSummary = "Solved " + type + " width=" + std::to_string(bestW).substr(0, 5) +
                     " mm for Z=" + std::to_string(static_cast<int>(targetZ)) + " Ohm";
        return {
            {"ok", true},
            {"mode", "impedance"},
            {"type", type},
            {"trace_width_mm", bestW},
            {"spacing_mm", sMm},
            {"achieved_z_ohm", achievedZ},
            {"summary", outSummary}
        };
    }

    if (mode == "power_budget") {
        double totalPowerW = 0.0;
        nlohmann::json bom = nlohmann::json::array();
        if (args.contains("rails") && args["rails"].is_array()) {
            for (const auto& r : args["rails"]) {
                if (!r.is_object()) continue;
                std::string rName = r.value("name", "VCC");
                double v = r.value("voltage_v", 3.3);
                double iA = r.value("current_a", 0.5);
                int pins = std::max(1, r.value("ic_power_pins", 2));
                totalPowerW += v * iA;
                bom.push_back({
                    {"rail", rName},
                    {"hf_100nf_count", pins},
                    {"bulk_10uf_count", std::max(1, static_cast<int>(std::ceil(iA / 0.8)))},
                    {"voltage_rating_v", std::max(10.0, std::ceil(v * 2.0))}
                });
            }
        }
        outSuccess = true;
        outSummary = "Synthesized power & decoupling budget (" +
                     std::to_string(totalPowerW).substr(0, 5) + " W total)";
        return {
            {"ok", true},
            {"mode", "power_budget"},
            {"total_power_w", totalPowerW},
            {"decoupling_bom", bom},
            {"summary", outSummary}
        };
    }

    outSuccess = false;
    outSummary = "Unsupported electrical synth mode: " + mode;
    return {{"ok", false}, {"error", outSummary}};
}

nlohmann::json EdaAccelerators::calcRobotKinematicsStatics(const nlohmann::json& args,
                                                           bool& outSuccess,
                                                           std::string& outSummary) const {
    if (!args.is_object() || !args.contains("links") || !args["links"].is_array() ||
        args["links"].empty()) {
        outSuccess = false;
        outSummary = "cad_calc_robot_kinematics_statics requires non-empty links[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    struct LinkSpec {
        std::string name;
        double lengthM = 0.1;
        double massKg = 0.1;
        double angleDeg = 0.0;
    };

    std::vector<LinkSpec> links;
    for (const auto& l : args["links"]) {
        if (!l.is_object()) continue;
        LinkSpec ls;
        ls.name = l.value("name", "link_" + std::to_string(links.size() + 1));
        ls.lengthM = std::max(0.005, l.value("length_m", 0.10));
        ls.massKg = std::max(0.001, l.value("mass_kg", 0.10));
        ls.angleDeg = l.value("angle_deg", 0.0);
        links.push_back(ls);
    }

    double payloadKg = std::max(0.0, args.value("payload_kg", 0.10));
    double dynFactor = std::max(1.0, args.value("dynamic_factor", 1.35));
    double stallTorqueKgCm = std::max(0.5, args.value("servo_stall_torque_kgcm", 20.0));
    double stallTorqueNm = stallTorqueKgCm * 0.0980665;
    double polyHalfWidthM = std::max(0.01, args.value("support_polygon_half_width_m", 0.08));

    // Forward kinematics in planar XY (and full horizontal worst-case lever arm)
    double curX = 0.0, curY = 0.0, cumAngleRad = 0.0;
    double totalMassKg = payloadKg;
    double comMomentX = 0.0;

    std::vector<double> jointX(links.size(), 0.0);
    std::vector<double> linkComX(links.size(), 0.0);

    for (size_t i = 0; i < links.size(); ++i) {
        jointX[i] = curX;
        cumAngleRad += links[i].angleDeg * (kPi / 180.0);
        double nx = curX + links[i].lengthM * std::cos(cumAngleRad);
        double ny = curY + links[i].lengthM * std::sin(cumAngleRad);
        linkComX[i] = 0.5 * (curX + nx);
        totalMassKg += links[i].massKg;
        comMomentX += links[i].massKg * linkComX[i];
        curX = nx;
        curY = ny;
    }
    comMomentX += payloadKg * curX;
    double wholeBodyComX = (totalMassKg > 0.0) ? (comMomentX / totalMassKg) : 0.0;
    double supportMarginM = polyHalfWidthM - std::fabs(wholeBodyComX);

    nlohmann::json jointsJson = nlohmann::json::array();
    double minSafetyFactor = std::numeric_limits<double>::max();

    for (size_t i = 0; i < links.size(); ++i) {
        // Worst-case horizontal outreach gravity moment from joint i outward
        double distAccumM = 0.0;
        double staticMomentNm = 0.0;
        for (size_t j = i; j < links.size(); ++j) {
            double comArm = distAccumM + 0.5 * links[j].lengthM;
            staticMomentNm += links[j].massKg * kGravity * comArm;
            distAccumM += links[j].lengthM;
        }
        staticMomentNm += payloadKg * kGravity * distAccumM;
        double reqTorqueNm = staticMomentNm * dynFactor;
        double reqTorqueKgCm = reqTorqueNm / 0.0980665;
        double safetyFactor = stallTorqueNm / std::max(1e-6, reqTorqueNm);
        minSafetyFactor = std::min(minSafetyFactor, safetyFactor);

        // Torsional shaft sizing: d = (16 * T / (pi * tau_allow))^(1/3), tau_allow = 45 MPa
        double minShaftDiaMm = std::max(3.0, std::cbrt((16.0 * reqTorqueNm) / (kPi * 45e6)) * 1000.0);

        jointsJson.push_back({
            {"joint_index", i},
            {"link_name", links[i].name},
            {"static_torque_nm", staticMomentNm},
            {"required_torque_nm", reqTorqueNm},
            {"required_torque_kgcm", reqTorqueKgCm},
            {"safety_factor", safetyFactor},
            {"min_shaft_diameter_mm", minShaftDiaMm}
        });
    }

    double reachM = std::hypot(curX, curY);
    outSuccess = true;
    outSummary = "Computed FK/statics for " + std::to_string(links.size()) +
                 " joints • Min servo SF=" + std::to_string(minSafetyFactor).substr(0, 4);
    return {
        {"ok", true},
        {"joints", jointsJson},
        {"end_effector", {
            {"x_m", curX},
            {"y_m", curY},
            {"reach_m", reachM}
        }},
        {"whole_body_com_x_m", wholeBodyComX},
        {"support_polygon_margin_m", supportMarginM},
        {"min_servo_safety_factor", minSafetyFactor},
        {"summary", outSummary}
    };
}

nlohmann::json EdaAccelerators::calcMechanismSynth(const nlohmann::json& args,
                                                   bool& outSuccess,
                                                   std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "cad_calc_mechanism_synth requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    std::string type = args.value("type", "spur_gear");
    double clearanceMm = std::max(0.02, args.value("clearance_mm", 0.20));
    double pressureAngleDeg = args.value("pressure_angle_deg", _config.defaultGearPressureAngleDeg);
    double alphaRad = pressureAngleDeg * (kPi / 180.0);

    if (type == "spur_gear" || type == "planetary") {
        double modMm = std::max(0.3, args.value("module_mm", 1.0));
        int zSun = args.value("sun_teeth", args.value("teeth_1", 16));
        int zPlanet = args.value("planet_teeth", args.value("teeth_2", 16));
        double thicknessMm = std::max(1.0, args.value("thickness_mm", 6.0));

        if (zSun < 6 || zPlanet < 6) {
            outSuccess = false;
            outSummary = "Gear tooth counts must be >= 6";
            return {{"ok", false}, {"error", outSummary}};
        }

        int zRing = zSun + 2 * zPlanet;
        if (type == "planetary") {
            if (args.contains("ring_teeth") && args["ring_teeth"].is_number_integer()) {
                int reqRing = args["ring_teeth"].get<int>();
                if (reqRing != zRing) {
                    outSuccess = false;
                    outSummary = "Planetary gear assembly constraint violated: Z_s + 2*Z_p != Z_r";
                    return {{"ok", false}, {"error", outSummary}};
                }
            }
        }

        double pitchRadiusSun = 0.5 * modMm * zSun;
        double baseRadiusSun = pitchRadiusSun * std::cos(alphaRad);
        double outerRadiusSun = pitchRadiusSun + modMm - 0.5 * clearanceMm;
        double rootRadiusSun = std::max(1.0, pitchRadiusSun - 1.25 * modMm - 0.5 * clearanceMm);
        double centerDistMm = 0.5 * modMm * (zSun + zPlanet) + clearanceMm;
        double carrierRatio = 1.0 + static_cast<double>(zRing) / static_cast<double>(zSun);

        // Emit involute tooth profile coordinates
        nlohmann::json profilePts = nlohmann::json::array();
        for (int i = 0; i <= 10; ++i) {
            double r = std::max(baseRadiusSun, rootRadiusSun) +
                       (outerRadiusSun - std::max(baseRadiusSun, rootRadiusSun)) * (i / 10.0);
            double phi = std::acos(std::clamp(baseRadiusSun / r, -1.0, 1.0));
            double invPhi = std::tan(phi) - phi;
            profilePts.push_back({
                {"x_mm", r * std::cos(invPhi)},
                {"y_mm", r * std::sin(invPhi)}
            });
        }

        std::ostringstream py;
        py << "import FreeCAD, Part\n"
           << "doc = FreeCAD.ActiveDocument or FreeCAD.newDocument('Mechanism')\n"
           << "sun = Part.makeCylinder(" << outerRadiusSun << ", " << thicknessMm << ")\n"
           << "bore = Part.makeCylinder(" << (2.5 + 0.5 * clearanceMm) << ", " << thicknessMm << ")\n"
           << "sun_body = doc.addObject('Part::Feature', 'SunGear')\n"
           << "sun_body.Shape = sun.cut(bore)\n"
           << "doc.recompute()\n";

        outSuccess = true;
        outSummary = "Synthesized " + type + " (m=" + std::to_string(modMm).substr(0, 4) +
                     " mm, center_dist=" + std::to_string(centerDistMm).substr(0, 5) + " mm)";
        return {
            {"ok", true},
            {"type", type},
            {"module_mm", modMm},
            {"sun_teeth", zSun},
            {"planet_teeth", zPlanet},
            {"ring_teeth", zRing},
            {"center_distance_mm", centerDistMm},
            {"carrier_reduction_ratio", carrierRatio},
            {"pitch_diameter_sun_mm", 2.0 * pitchRadiusSun},
            {"outer_diameter_sun_mm", 2.0 * outerRadiusSun},
            {"root_diameter_sun_mm", 2.0 * rootRadiusSun},
            {"profile_points", profilePts},
            {"freecad_python", py.str()},
            {"summary", outSummary}
        };
    }

    if (type == "servo_spline" || type == "timing_pulley") {
        std::string stdName = args.value("spline_standard", "25T_futaba");
        int teeth = (stdName.find("24T") != std::string::npos) ? 24 : args.value("teeth", 25);
        double outerDiaMm = (teeth == 24) ? 5.80 : 5.92;
        double rootDiaMm = outerDiaMm - 0.60;
        double rOuter = 0.5 * outerDiaMm + 0.5 * clearanceMm;
        double rRoot = 0.5 * rootDiaMm + 0.5 * clearanceMm;

        nlohmann::json pts = nlohmann::json::array();
        for (int i = 0; i < teeth * 2; ++i) {
            double ang = (kPi * i) / teeth;
            double r = (i % 2 == 0) ? rOuter : rRoot;
            pts.push_back({{"x_mm", r * std::cos(ang)}, {"y_mm", r * std::sin(ang)}});
        }

        outSuccess = true;
        outSummary = "Synthesized " + std::to_string(teeth) + "T spline/pulley profile with " +
                     std::to_string(clearanceMm).substr(0, 4) + " mm 3D-print clearance";
        return {
            {"ok", true},
            {"type", type},
            {"tooth_count", teeth},
            {"outer_diameter_mm", 2.0 * rOuter},
            {"root_diameter_mm", 2.0 * rRoot},
            {"profile_points", pts},
            {"summary", outSummary}
        };
    }

    if (type == "four_bar") {
        double g = args.value("ground_mm", 40.0);
        double a = args.value("crank_mm", 15.0);
        double b = args.value("coupler_mm", 35.0);
        double c = args.value("rocker_mm", 30.0);
        std::vector<double> lengths = {g, a, b, c};
        std::sort(lengths.begin(), lengths.end());
        double s = lengths[0];
        double l = lengths[3];
        double p = lengths[1];
        double q = lengths[2];
        bool isGrashof = (s + l <= p + q) && (a == s);

        double cosMin = std::clamp((b * b + c * c - (g - a) * (g - a)) / (2.0 * b * c), -1.0, 1.0);
        double cosMax = std::clamp((b * b + c * c - (g + a) * (g + a)) / (2.0 * b * c), -1.0, 1.0);
        double minTransDeg = std::acos(cosMin) * (180.0 / kPi);
        double maxTransDeg = std::acos(cosMax) * (180.0 / kPi);

        outSuccess = isGrashof;
        outSummary = isGrashof ? "Grashof crank-rocker 4-bar linkage verified"
                               : "Non-Grashof 4-bar linkage rejected (crank cannot rotate 360 deg)";
        return {
            {"ok", isGrashof},
            {"type", "four_bar"},
            {"is_grashof", isGrashof},
            {"min_transmission_angle_deg", minTransDeg},
            {"max_transmission_angle_deg", maxTransDeg},
            {"summary", outSummary}
        };
    }

    outSuccess = false;
    outSummary = "Unknown mechanism type: " + type;
    return {{"ok", false}, {"error", outSummary}};
}

nlohmann::json EdaAccelerators::calcPrintOrientationDfm(const nlohmann::json& args,
                                                        bool& outSuccess,
                                                        std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "cad_calc_print_orientation_dfm requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    std::string mat = args.value("material", _config.defaultFilamentMaterial);
    double sigmaXyMpa = 45.0;
    double sigmaZMpa = 22.0;
    if (mat == "PLA") {
        sigmaXyMpa = 55.0;
        sigmaZMpa = 26.0;
    } else if (mat == "ABS" || mat == "ASA") {
        sigmaXyMpa = 40.0;
        sigmaZMpa = 18.0;
    } else if (mat == "PA-CF") {
        sigmaXyMpa = 85.0;
        sigmaZMpa = 38.0;
    }

    double areaMm2 = std::max(10.0, args.value("cross_section_mm2", 120.0));
    double secModMm3 = std::max(20.0, args.value("section_modulus_mm3", 250.0));

    double fx = 0.0, fy = 0.0, fz = 50.0;
    if (args.contains("load_vector_n") && args["load_vector_n"].is_object()) {
        fx = args["load_vector_n"].value("fx", 0.0);
        fy = args["load_vector_n"].value("fy", 0.0);
        fz = args["load_vector_n"].value("fz", 50.0);
    }
    double mx = 1.0, my = 0.0, mz = 0.0;
    if (args.contains("moment_vector_nm") && args["moment_vector_nm"].is_object()) {
        mx = args["moment_vector_nm"].value("mx", 1.0);
        my = args["moment_vector_nm"].value("my", 0.0);
        mz = args["moment_vector_nm"].value("mz", 0.0);
    }
    double hx = 0.0, hy = 0.0, hz = 1.0;
    if (args.contains("bearing_bore_axis") && args["bearing_bore_axis"].is_object()) {
        hx = args["bearing_bore_axis"].value("x", 0.0);
        hy = args["bearing_bore_axis"].value("y", 0.0);
        hz = args["bearing_bore_axis"].value("z", 1.0);
    }

    double fMag = std::max(1.0, std::sqrt(fx * fx + fy * fy + fz * fz));
    double mMagNmm = std::sqrt(mx * mx + my * my + mz * mz) * 1000.0;

    // Principal tensile direction unit vector (combination of force and bending normal)
    double px = fx + my * 100.0;
    double py = fy - mx * 100.0;
    double pz = fz;
    double pNorm = std::max(1e-6, std::sqrt(px * px + py * py + pz * pz));
    px /= pNorm;
    py /= pNorm;
    pz /= pNorm;

    double hNorm = std::max(1e-6, std::sqrt(hx * hx + hy * hy + hz * hz));
    hx /= hNorm;
    hy /= hNorm;
    hz /= hNorm;

    struct CandidateOrientation {
        std::string label;
        double rxDeg;
        double ryDeg;
        double rzDeg;
        double nx, ny, nz; // Slicer Z-build normal in part frame
    };

    const std::vector<CandidateOrientation> candidates = {
        {"flat_xy", 0.0, 0.0, 0.0, 0.0, 0.0, 1.0},
        {"on_side_x90", 90.0, 0.0, 0.0, 0.0, 1.0, 0.0},
        {"standing_y90", 0.0, 90.0, 0.0, 1.0, 0.0, 0.0},
        {"tilted_45_x", 45.0, 0.0, 0.0, 0.0, 0.7071, 0.7071},
        {"tilted_45_y", 0.0, 45.0, 0.0, 0.7071, 0.0, 0.7071},
        {"inverted_z180", 180.0, 0.0, 0.0, 0.0, 0.0, -1.0}
    };

    nlohmann::json candArray = nlohmann::json::array();
    double bestScore = -1e9;
    nlohmann::json bestOrientation;
    double bestDelamSf = 1.0;

    for (const auto& c : candidates) {
        double zAlign = std::fabs(px * c.nx + py * c.ny + pz * c.nz);
        double xyAlign = std::sqrt(std::max(0.0, 1.0 - zAlign * zAlign));
        double normalStressMpa = (fMag / areaMm2) + (mMagNmm / secModMm3);
        double zInterlayerStressMpa = std::max(0.1, normalStressMpa * (0.25 + 0.75 * zAlign));
        double xyFiberStressMpa = std::max(0.1, normalStressMpa * xyAlign);

        double delamSf = sigmaZMpa / zInterlayerStressMpa;
        double fiberSf = sigmaXyMpa / xyFiberStressMpa;
        double boreCircularity = std::fabs(hx * c.nx + hy * c.ny + hz * c.nz);
        double supportPenalty = (c.rxDeg == 45.0 || c.ryDeg == 45.0) ? 0.25 : 0.05;

        double score = delamSf * 2.0 + boreCircularity * 1.5 - supportPenalty;
        nlohmann::json entry = {
            {"label", c.label},
            {"rx_deg", c.rxDeg},
            {"ry_deg", c.ryDeg},
            {"rz_deg", c.rzDeg},
            {"delamination_safety_factor", delamSf},
            {"in_plane_safety_factor", fiberSf},
            {"bore_circularity_index", boreCircularity},
            {"score", score}
        };
        candArray.push_back(entry);
        if (score > bestScore) {
            bestScore = score;
            bestOrientation = entry;
            bestDelamSf = delamSf;
        }
    }

    outSuccess = true;
    outSummary = "Optimal FDM orientation " + bestOrientation.value("label", "on_side_x90") +
                 " (" + mat + " interlayer SF=" + std::to_string(bestDelamSf).substr(0, 4) + ")";
    return {
        {"ok", true},
        {"material", mat},
        {"optimal_orientation", bestOrientation},
        {"delamination_safety_factor", bestDelamSf},
        {"candidates", candArray},
        {"summary", outSummary}
    };
}

nlohmann::json EdaAccelerators::calcPcbConnectivityAudit(const nlohmann::json& args,
                                                         bool& outSuccess,
                                                         std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "eda_calc_pcb_connectivity_audit requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    auto canonicalLayer = [](const nlohmann::json& obj, const std::string& defVal) -> std::string {
        std::string raw = defVal;
        if (obj.contains("layer")) {
            if (obj["layer"].is_string()) {
                raw = obj["layer"].get<std::string>();
            } else if (obj["layer"].is_number_integer()) {
                raw = std::to_string(obj["layer"].get<int>());
            }
        }
        if (raw == "TopLayer" || raw == "1" || raw == "F.Cu" || raw == "Top") return "TopLayer";
        if (raw == "BottomLayer" || raw == "2" || raw == "B.Cu" || raw == "Bottom") return "BottomLayer";
        if (raw == "InnerLayer1" || raw == "15" || raw == "In1.Cu") return "InnerLayer1";
        if (raw == "InnerLayer2" || raw == "16" || raw == "In2.Cu") return "InnerLayer2";
        if (raw == "InnerLayer3" || raw == "17" || raw == "In3.Cu") return "InnerLayer3";
        if (raw == "InnerLayer4" || raw == "18" || raw == "In4.Cu") return "InnerLayer4";
        if (raw == "MultiLayer" || raw == "12" || raw == "*.Cu" ||
            raw == "Multi-Layer" || raw == "PTH" || raw == "thru_hole") {
            return "MultiLayer";
        }
        return raw;
    };

    auto layersMatch = [](const std::string& l1, const std::string& l2) -> bool {
        return l1 == l2 || l1 == "MultiLayer" || l2 == "MultiLayer";
    };

    auto distPointToSeg = [](double px, double py,
                             double x1, double y1,
                             double x2, double y2) -> double {
        double dx = x2 - x1;
        double dy = y2 - y1;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
            return std::hypot(px - x1, py - y1);
        }
        double t = std::clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0.0, 1.0);
        return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
    };

    auto distSegToSeg = [&](double ax1, double ay1, double ax2, double ay2,
                            double bx1, double by1, double bx2, double by2) -> double {
        double d1 = distPointToSeg(ax1, ay1, bx1, by1, bx2, by2);
        double d2 = distPointToSeg(ax2, ay2, bx1, by1, bx2, by2);
        double d3 = distPointToSeg(bx1, by1, ax1, ay1, ax2, ay2);
        double d4 = distPointToSeg(bx2, by2, ax1, ay1, ax2, ay2);
        return std::min({d1, d2, d3, d4});
    };

    struct AuditPad {
        std::string id;
        std::string ref;
        std::string pad;
        std::string net;
        double x = 0.0;
        double y = 0.0;
        double sx = 0.6;
        double sy = 0.6;
        std::string layer;
    };
    struct AuditTrack {
        std::string id;
        std::string net;
        std::string layer;
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        double width = 0.25;
    };
    struct AuditVia {
        std::string id;
        std::string net;
        double x = 0.0;
        double y = 0.0;
        double diameter = 0.6;
        double drill = 0.3;
    };
    struct AuditPour {
        std::string id;
        std::string net;
        std::string layer;
        std::vector<std::pair<double, double>> poly;
        bool hasBbox = false;
        double xMin = 0.0;
        double yMin = 0.0;
        double xMax = 0.0;
        double yMax = 0.0;
    };

    std::string units = args.value("units", "");
    bool isMilUnits = (units == "mil");
    if (units.empty() && args.contains("components") && !args.contains("footprints") &&
        !args.contains("pcb_components")) {
        if (args.contains("pads") && args["pads"].is_array()) {
            for (const auto& p : args["pads"]) {
                if (p.is_object() && (std::fabs(p.value("x", 0.0)) > 300.0 ||
                                      std::fabs(p.value("y", 0.0)) > 300.0)) {
                    isMilUnits = true;
                    break;
                }
            }
        }
    }

    const double mmToCoord = isMilUnits ? (1.0 / 0.0254) : 1.0;
    const double defaultPadSize = 0.6 * mmToCoord;
    const double minPadSize = 0.1 * mmToCoord;
    const double defaultTrackWidth = 0.25 * mmToCoord;
    const double minTrackWidth = 0.05 * mmToCoord;
    const double defaultViaDia = 0.6 * mmToCoord;
    const double minViaDia = 0.2 * mmToCoord;
    const double defaultViaDrill = 0.3 * mmToCoord;
    const double minViaDrill = 0.1 * mmToCoord;
    const double touchTol = 0.08 * mmToCoord;

    std::vector<AuditPad> allPads;
    std::set<std::string> pcbRefs;

    auto extractPositiveNumber = [](const nlohmann::json& obj, const char* key) -> double {
        if (obj.contains(key) && obj[key].is_number()) {
            double v = obj[key].get<double>();
            if (v > 0.0) return v;
        }
        return 0.0;
    };

    auto ingestPadObj = [&](const nlohmann::json& p, const std::string& parentRef) {
        if (!p.is_object()) return;
        AuditPad ap;
        ap.id = p.value("id", "pad_" + std::to_string(allPads.size() + 1));
        ap.ref = p.value("ref", p.value("reference", parentRef));
        ap.pad = p.value("pad", p.value("number", ""));
        ap.net = p.value("net", "");
        ap.x = p.value("x", p.value("x_mm", 0.0));
        ap.y = p.value("y", p.value("y_mm", 0.0));

        double rawSx = extractPositiveNumber(p, "size_x");
        if (rawSx <= 0.0) rawSx = extractPositiveNumber(p, "width");
        if (rawSx <= 0.0) {
            double wMm = extractPositiveNumber(p, "width_mm");
            if (wMm > 0.0) rawSx = wMm * mmToCoord;
        }
        double rawSy = extractPositiveNumber(p, "size_y");
        if (rawSy <= 0.0) rawSy = extractPositiveNumber(p, "height");
        if (rawSy <= 0.0) {
            double hMm = extractPositiveNumber(p, "height_mm");
            if (hMm > 0.0) rawSy = hMm * mmToCoord;
        }
        double holeDia = extractPositiveNumber(p, "hole_diameter");
        if (holeDia <= 0.0) holeDia = extractPositiveNumber(p, "drill");
        if (holeDia <= 0.0) {
            double dMm = extractPositiveNumber(p, "drill_mm");
            if (dMm > 0.0) holeDia = dMm * mmToCoord;
        }
        if (holeDia > 0.0) {
            rawSx = std::max(rawSx, holeDia);
            rawSy = std::max(rawSy, holeDia);
        }
        ap.sx = std::max(minPadSize, (rawSx > 0.0) ? rawSx : defaultPadSize);
        ap.sy = std::max(minPadSize, (rawSy > 0.0) ? rawSy : defaultPadSize);
        ap.layer = canonicalLayer(p, "TopLayer");
        if (!ap.ref.empty()) pcbRefs.insert(ap.ref);
        allPads.push_back(ap);
    };

    if (args.contains("pads") && args["pads"].is_array()) {
        for (const auto& p : args["pads"]) {
            ingestPadObj(p, "");
        }
    }
    bool hadTopLevelPads = !allPads.empty();
    if (args.contains("footprints") && args["footprints"].is_array()) {
        for (const auto& fp : args["footprints"]) {
            if (!fp.is_object()) continue;
            std::string fpRef = fp.value("reference", fp.value("ref", ""));
            if (!fpRef.empty()) pcbRefs.insert(fpRef);
            if (!hadTopLevelPads && fp.contains("pads") && fp["pads"].is_array()) {
                double fpx = fp.value("x", fp.value("x_mm", 0.0));
                double fpy = fp.value("y", fp.value("y_mm", 0.0));
                for (const auto& p : fp["pads"]) {
                    if (!p.is_object()) continue;
                    nlohmann::json copyP = p;
                    if (!copyP.contains("ref") && !copyP.contains("reference")) {
                        copyP["ref"] = fpRef;
                    }
                    if (!copyP.contains("x") && !copyP.contains("x_mm")) {
                        copyP["x"] = copyP.value("rel_x", copyP.value("dx", 0.0)) + fpx;
                        copyP["y"] = copyP.value("rel_y", copyP.value("dy", 0.0)) + fpy;
                    }
                    ingestPadObj(copyP, fpRef);
                }
            }
        }
    }

    std::vector<AuditTrack> allTracks;
    if (args.contains("tracks") && args["tracks"].is_array()) {
        for (const auto& t : args["tracks"]) {
            if (!t.is_object()) continue;
            std::string tid = t.value("id", "trk_" + std::to_string(allTracks.size() + 1));
            std::string tnet = t.value("net", "");
            std::string tlayer = canonicalLayer(t, "TopLayer");
            double tw = std::max(minTrackWidth, t.value("width", t.value("width_mm", defaultTrackWidth)));
            if (t.contains("points") && t["points"].is_array() && t["points"].size() >= 2 &&
                !t.contains("x1") && !t.contains("x1_mm")) {
                const auto& pts = t["points"];
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    if (pts[i].is_array() && pts[i].size() >= 2 &&
                        pts[i + 1].is_array() && pts[i + 1].size() >= 2) {
                        AuditTrack at;
                        at.id = tid;
                        at.net = tnet;
                        at.layer = tlayer;
                        at.x1 = pts[i][0].get<double>();
                        at.y1 = pts[i][1].get<double>();
                        at.x2 = pts[i + 1][0].get<double>();
                        at.y2 = pts[i + 1][1].get<double>();
                        at.width = tw;
                        allTracks.push_back(at);
                    }
                }
            } else {
                AuditTrack at;
                at.id = tid;
                at.net = tnet;
                at.layer = tlayer;
                at.x1 = t.value("x1", t.value("x1_mm", 0.0));
                at.y1 = t.value("y1", t.value("y1_mm", 0.0));
                at.x2 = t.value("x2", t.value("x2_mm", 0.0));
                at.y2 = t.value("y2", t.value("y2_mm", 0.0));
                at.width = tw;
                allTracks.push_back(at);
            }
        }
    }

    std::vector<AuditVia> allVias;
    if (args.contains("vias") && args["vias"].is_array()) {
        for (const auto& v : args["vias"]) {
            if (!v.is_object()) continue;
            AuditVia av;
            av.id = v.value("id", "via_" + std::to_string(allVias.size() + 1));
            av.net = v.value("net", "");
            av.x = v.value("x", v.value("x_mm", 0.0));
            av.y = v.value("y", v.value("y_mm", 0.0));
            av.diameter = std::max(minViaDia, v.value("diameter", v.value("size_mm", defaultViaDia)));
            av.drill = std::max(minViaDrill, v.value("drill", v.value("drill_mm", defaultViaDrill)));
            allVias.push_back(av);
        }
    }

    std::vector<AuditPour> allPours;
    for (const char* pourKey : {"pours", "zones"}) {
        if (!args.contains(pourKey) || !args[pourKey].is_array()) continue;
        for (const auto& z : args[pourKey]) {
            if (!z.is_object()) continue;
            AuditPour ap;
            ap.id = z.value("id", "pour_" + std::to_string(allPours.size() + 1));
            ap.net = z.value("net", "");
            ap.layer = canonicalLayer(z, "TopLayer");

            for (const char* polyKey : {"polygon", "points", "outline"}) {
                if (z.contains(polyKey) && z[polyKey].is_array()) {
                    for (const auto& pt : z[polyKey]) {
                        if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                            ap.poly.push_back({pt[0].get<double>(), pt[1].get<double>()});
                        } else if (pt.is_object() && pt.contains("x") && pt.contains("y") &&
                                   pt["x"].is_number() && pt["y"].is_number()) {
                            ap.poly.push_back({pt["x"].get<double>(), pt["y"].get<double>()});
                        }
                    }
                    if (!ap.poly.empty()) break;
                }
            }

            if (z.contains("x_min") && z.contains("y_min") && z.contains("x_max") && z.contains("y_max") &&
                z["x_min"].is_number() && z["y_min"].is_number() &&
                z["x_max"].is_number() && z["y_max"].is_number()) {
                ap.hasBbox = true;
                ap.xMin = z["x_min"].get<double>();
                ap.yMin = z["y_min"].get<double>();
                ap.xMax = z["x_max"].get<double>();
                ap.yMax = z["y_max"].get<double>();
            } else if (z.contains("bbox") && z["bbox"].is_object()) {
                const auto& bb = z["bbox"];
                if (bb.contains("x_min") && bb.contains("y_min") && bb.contains("x_max") && bb.contains("y_max")) {
                    ap.hasBbox = true;
                    ap.xMin = bb.value("x_min", 0.0);
                    ap.yMin = bb.value("y_min", 0.0);
                    ap.xMax = bb.value("x_max", 0.0);
                    ap.yMax = bb.value("y_max", 0.0);
                }
            } else if (!ap.poly.empty()) {
                ap.hasBbox = true;
                ap.xMin = ap.xMax = ap.poly[0].first;
                ap.yMin = ap.yMax = ap.poly[0].second;
                for (const auto& pt : ap.poly) {
                    ap.xMin = std::min(ap.xMin, pt.first);
                    ap.xMax = std::max(ap.xMax, pt.first);
                    ap.yMin = std::min(ap.yMin, pt.second);
                    ap.yMax = std::max(ap.yMax, pt.second);
                }
            }

            if (!ap.net.empty() && (ap.poly.size() >= 3 || ap.hasBbox)) {
                allPours.push_back(ap);
            }
        }
    }

    auto pointInOrNearPour = [&](const AuditPour& pour, double px, double py, double radius) -> bool {
        if (pour.hasBbox) {
            if (px < pour.xMin - radius || px > pour.xMax + radius ||
                py < pour.yMin - radius || py > pour.yMax + radius) {
                return false;
            }
        }
        if (pour.poly.size() >= 3) {
            bool inside = false;
            size_t n = pour.poly.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                double xi = pour.poly[i].first;
                double yi = pour.poly[i].second;
                double xj = pour.poly[j].first;
                double yj = pour.poly[j].second;
                bool intersect = ((yi > py) != (yj > py)) &&
                                 (px < (xj - xi) * (py - yi) / ((yj - yi) + ((yj >= yi) ? 1e-15 : -1e-15)) + xi);
                if (intersect) inside = !inside;
                if (distPointToSeg(px, py, xj, yj, xi, yi) <= radius) {
                    return true;
                }
            }
            return inside;
        }
        return pour.hasBbox;
    };

    // 1. Unassigned pads (ignoring mechanical mounting holes H*, MH*, MP)
    nlohmann::json unassignedPadsJson = nlohmann::json::array();
    std::map<std::string, std::vector<size_t>> netPadsMap;
    for (size_t i = 0; i < allPads.size(); ++i) {
        const auto& p = allPads[i];
        if (p.net.empty()) {
            bool isMountingHole = (p.ref.rfind("H", 0) == 0 || p.ref.rfind("MH", 0) == 0 ||
                                   p.pad == "MP" || p.pad == "MH" || p.id.rfind("mh", 0) == 0);
            if (!isMountingHole) {
                unassignedPadsJson.push_back({
                    {"id", p.id},
                    {"ref", p.ref},
                    {"pad", p.pad},
                    {"x", p.x},
                    {"y", p.y},
                    {"layer", p.layer}
                });
            }
        } else {
            netPadsMap[p.net].push_back(i);
        }
    }

    // 2. Per-net 3D Layer-Aware Union-Find connectivity analysis (including copper pours/zones)
    int totalNets = 0;
    int fullyRoutedNets = 0;
    nlohmann::json unroutedNetsJson = nlohmann::json::array();

    for (const auto& kv : netPadsMap) {
        const std::string& netName = kv.first;
        const auto& padIndices = kv.second;
        if (padIndices.size() < 2) continue;
        ++totalNets;

        std::vector<size_t> trkIndices;
        for (size_t i = 0; i < allTracks.size(); ++i) {
            if (allTracks[i].net == netName || allTracks[i].net.empty()) {
                trkIndices.push_back(i);
            }
        }
        std::vector<size_t> viaIndices;
        for (size_t i = 0; i < allVias.size(); ++i) {
            if (allVias[i].net == netName || allVias[i].net.empty()) {
                viaIndices.push_back(i);
            }
        }
        std::vector<size_t> pourIndices;
        for (size_t i = 0; i < allPours.size(); ++i) {
            if (allPours[i].net == netName) {
                pourIndices.push_back(i);
            }
        }

        size_t nP = padIndices.size();
        size_t nT = trkIndices.size();
        size_t nV = viaIndices.size();
        size_t nZ = pourIndices.size();
        size_t totalNodes = nP + nT + nV + nZ;
        std::vector<size_t> ufParent(totalNodes);
        for (size_t i = 0; i < totalNodes; ++i) ufParent[i] = i;

        auto ufFind = [&](size_t x) {
            size_t r = x;
            while (ufParent[r] != r) r = ufParent[r];
            while (ufParent[x] != r) {
                size_t nxt = ufParent[x];
                ufParent[x] = r;
                x = nxt;
            }
            return r;
        };
        auto ufUnion = [&](size_t a, size_t b) {
            size_t ra = ufFind(a);
            size_t rb = ufFind(b);
            if (ra != rb) ufParent[rb] = ra;
        };

        // Pad-to-Pad
        for (size_t i = 0; i < nP; ++i) {
            const auto& pa = allPads[padIndices[i]];
            double ra = 0.5 * std::max(pa.sx, pa.sy);
            for (size_t j = i + 1; j < nP; ++j) {
                const auto& pb = allPads[padIndices[j]];
                if (!layersMatch(pa.layer, pb.layer)) continue;
                double rb = 0.5 * std::max(pb.sx, pb.sy);
                if (std::hypot(pa.x - pb.x, pa.y - pb.y) <= ra + rb + touchTol) {
                    ufUnion(i, j);
                }
            }
        }

        // Pad-to-Track
        for (size_t i = 0; i < nP; ++i) {
            const auto& pa = allPads[padIndices[i]];
            double ra = 0.5 * std::max(pa.sx, pa.sy);
            for (size_t j = 0; j < nT; ++j) {
                const auto& trk = allTracks[trkIndices[j]];
                if (!layersMatch(pa.layer, trk.layer)) continue;
                double d = distPointToSeg(pa.x, pa.y, trk.x1, trk.y1, trk.x2, trk.y2);
                if (d <= ra + 0.5 * trk.width + touchTol) {
                    ufUnion(i, nP + j);
                }
            }
        }

        // Pad-to-Via (vias span all layers)
        for (size_t i = 0; i < nP; ++i) {
            const auto& pa = allPads[padIndices[i]];
            double ra = 0.5 * std::max(pa.sx, pa.sy);
            for (size_t k = 0; k < nV; ++k) {
                const auto& via = allVias[viaIndices[k]];
                if (std::hypot(pa.x - via.x, pa.y - via.y) <= ra + 0.5 * via.diameter + touchTol) {
                    ufUnion(i, nP + nT + k);
                }
            }
        }

        // Track-to-Via (vias span all layers)
        for (size_t j = 0; j < nT; ++j) {
            const auto& trk = allTracks[trkIndices[j]];
            for (size_t k = 0; k < nV; ++k) {
                const auto& via = allVias[viaIndices[k]];
                double d = distPointToSeg(via.x, via.y, trk.x1, trk.y1, trk.x2, trk.y2);
                if (d <= 0.5 * via.diameter + 0.5 * trk.width + touchTol) {
                    ufUnion(nP + j, nP + nT + k);
                }
            }
        }

        // Track-to-Track on matching layer
        for (size_t i = 0; i < nT; ++i) {
            const auto& ta = allTracks[trkIndices[i]];
            for (size_t j = i + 1; j < nT; ++j) {
                const auto& tb = allTracks[trkIndices[j]];
                if (!layersMatch(ta.layer, tb.layer)) continue;
                double d = distSegToSeg(ta.x1, ta.y1, ta.x2, ta.y2, tb.x1, tb.y1, tb.x2, tb.y2);
                if (d <= 0.5 * (ta.width + tb.width) + touchTol) {
                    ufUnion(nP + i, nP + j);
                }
            }
        }

        // Via-to-Via
        for (size_t i = 0; i < nV; ++i) {
            const auto& va = allVias[viaIndices[i]];
            for (size_t j = i + 1; j < nV; ++j) {
                const auto& vb = allVias[viaIndices[j]];
                if (std::hypot(va.x - vb.x, va.y - vb.y) <= 0.5 * (va.diameter + vb.diameter) + touchTol) {
                    ufUnion(nP + nT + i, nP + nT + j);
                }
            }
        }

        // Copper Pour / Zone unions (Pad-to-Pour, Track-to-Pour, Via-to-Pour)
        for (size_t z = 0; z < nZ; ++z) {
            const auto& pour = allPours[pourIndices[z]];
            size_t pourNode = nP + nT + nV + z;
            for (size_t i = 0; i < nP; ++i) {
                const auto& pa = allPads[padIndices[i]];
                if (!layersMatch(pa.layer, pour.layer)) continue;
                double ra = 0.5 * std::max(pa.sx, pa.sy);
                if (pointInOrNearPour(pour, pa.x, pa.y, ra + touchTol)) {
                    ufUnion(i, pourNode);
                }
            }
            for (size_t j = 0; j < nT; ++j) {
                const auto& trk = allTracks[trkIndices[j]];
                if (!layersMatch(trk.layer, pour.layer)) continue;
                double rt = 0.5 * trk.width + touchTol;
                if (pointInOrNearPour(pour, trk.x1, trk.y1, rt) ||
                    pointInOrNearPour(pour, trk.x2, trk.y2, rt)) {
                    ufUnion(nP + j, pourNode);
                }
            }
            for (size_t k = 0; k < nV; ++k) {
                const auto& via = allVias[viaIndices[k]];
                if (pointInOrNearPour(pour, via.x, via.y, 0.5 * via.diameter + touchTol)) {
                    ufUnion(nP + nT + k, pourNode);
                }
            }
        }

        std::map<size_t, std::vector<size_t>> islands;
        for (size_t i = 0; i < nP; ++i) {
            islands[ufFind(i)].push_back(padIndices[i]);
        }

        if (islands.size() == 1) {
            ++fullyRoutedNets;
        } else {
            double minDist = std::numeric_limits<double>::infinity();
            size_t bestA = padIndices[0];
            size_t bestB = padIndices[1];
            for (auto itA = islands.begin(); itA != islands.end(); ++itA) {
                auto itB = itA;
                ++itB;
                for (; itB != islands.end(); ++itB) {
                    for (size_t idxA : itA->second) {
                        for (size_t idxB : itB->second) {
                            double d = std::hypot(allPads[idxA].x - allPads[idxB].x,
                                                  allPads[idxA].y - allPads[idxB].y);
                            if (d < minDist) {
                                minDist = d;
                                bestA = idxA;
                                bestB = idxB;
                            }
                        }
                    }
                }
            }
            double distMm = isMilUnits ? (minDist * 0.0254) : minDist;
            unroutedNetsJson.push_back({
                {"net", netName},
                {"island_count", static_cast<int>(islands.size())},
                {"pad_count", static_cast<int>(nP)},
                {"airwire", {
                    {"from_ref", allPads[bestA].ref},
                    {"from_pad", allPads[bestA].pad},
                    {"from_x", allPads[bestA].x},
                    {"from_y", allPads[bestA].y},
                    {"to_ref", allPads[bestB].ref},
                    {"to_pad", allPads[bestB].pad},
                    {"to_x", allPads[bestB].x},
                    {"to_y", allPads[bestB].y},
                    {"distance_mm", std::round(distMm * 100.0) / 100.0}
                }}
            });
        }
    }

    // 3. Dangling track endpoints detection
    nlohmann::json danglingTracksJson = nlohmann::json::array();
    for (size_t i = 0; i < allTracks.size(); ++i) {
        const auto& trk = allTracks[i];
        double endPts[2][2] = {{trk.x1, trk.y1}, {trk.x2, trk.y2}};
        for (int ep = 0; ep < 2; ++ep) {
            double ex = endPts[ep][0];
            double ey = endPts[ep][1];
            bool anchored = false;

            for (const auto& pad : allPads) {
                if (!trk.net.empty() && !pad.net.empty() && trk.net != pad.net) continue;
                if (!layersMatch(trk.layer, pad.layer)) continue;
                double rp = 0.5 * std::max(pad.sx, pad.sy);
                if (std::hypot(ex - pad.x, ey - pad.y) <= rp + 0.5 * trk.width + touchTol) {
                    anchored = true;
                    break;
                }
            }
            if (!anchored) {
                for (const auto& via : allVias) {
                    if (!trk.net.empty() && !via.net.empty() && trk.net != via.net) continue;
                    if (std::hypot(ex - via.x, ey - via.y) <= 0.5 * via.diameter + 0.5 * trk.width + touchTol) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (!anchored) {
                for (size_t j = 0; j < allTracks.size(); ++j) {
                    if (j == i) continue;
                    const auto& other = allTracks[j];
                    if (!trk.net.empty() && !other.net.empty() && trk.net != other.net) continue;
                    if (!layersMatch(trk.layer, other.layer)) continue;
                    double d = distPointToSeg(ex, ey, other.x1, other.y1, other.x2, other.y2);
                    if (d <= 0.5 * (trk.width + other.width) + touchTol) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (!anchored) {
                for (const auto& pour : allPours) {
                    if (!trk.net.empty() && !pour.net.empty() && trk.net != pour.net) continue;
                    if (!layersMatch(trk.layer, pour.layer)) continue;
                    if (pointInOrNearPour(pour, ex, ey, 0.5 * trk.width + touchTol)) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (!anchored) {
                danglingTracksJson.push_back({
                    {"id", trk.id},
                    {"net", trk.net},
                    {"layer", trk.layer},
                    {"x", ex},
                    {"y", ey}
                });
            }
        }
    }

    // 4. Schematic-to-PCB component parity check (with multi-sheet deduplication)
    for (const char* compKey : {"pcb_components", "components"}) {
        if (args.contains(compKey) && args[compKey].is_array()) {
            for (const auto& pc : args[compKey]) {
                if (!pc.is_object()) continue;
                std::string ref = pc.value("reference", pc.value("ref", pc.value("designator", "")));
                if (!ref.empty()) pcbRefs.insert(ref);
            }
        }
    }

    nlohmann::json unsyncedJson = nlohmann::json::array();
    std::set<std::string> seenSchRefs;
    if (args.contains("schematic_components") && args["schematic_components"].is_array()) {
        for (const auto& sc : args["schematic_components"]) {
            if (!sc.is_object()) continue;
            std::string ref = sc.value("reference", sc.value("ref", sc.value("designator", "")));
            std::string compType = sc.value("component_type", sc.value("type", ""));
            std::string fp = sc.value("footprint", "");
            if (ref.empty() || compType == "netport" || compType == "frame" || compType == "power" ||
                compType == "netflag" || ref.rfind("FRAME", 0) == 0 || ref.rfind("#", 0) == 0 ||
                ref == "GND" || ref == "VCC") {
                continue;
            }
            if (fp.empty() && !compType.empty() && compType != "part") {
                continue;
            }
            if (!seenSchRefs.insert(ref).second) {
                continue;
            }
            if (pcbRefs.find(ref) == pcbRefs.end()) {
                unsyncedJson.push_back({
                    {"reference", ref},
                    {"value", sc.value("value", "")},
                    {"footprint", fp}
                });
            }
        }
    }

    int unroutedCount = static_cast<int>(unroutedNetsJson.size());
    int danglingCount = static_cast<int>(danglingTracksJson.size());
    int unassignedCount = static_cast<int>(unassignedPadsJson.size());
    int unsyncedCount = static_cast<int>(unsyncedJson.size());
    bool allConnected = (unroutedCount == 0 && danglingCount == 0 && unassignedCount == 0);
    bool schPcbSynced = (unsyncedCount == 0);

    outSuccess = true;
    outSummary = "Nets " + std::to_string(fullyRoutedNets) + "/" + std::to_string(totalNets) +
                 " connected • " + std::to_string(unroutedCount) + " broken • " +
                 std::to_string(danglingCount) + " dangling • " +
                 std::to_string(unassignedCount) + " unassigned pads • " +
                 std::to_string(unsyncedCount) + " unsynced components";

    return {
        {"ok", true},
        {"all_connected", allConnected},
        {"schematic_pcb_synced", schPcbSynced},
        {"total_nets", totalNets},
        {"fully_routed_nets", fullyRoutedNets},
        {"unrouted_nets_count", unroutedCount},
        {"unrouted_nets", unroutedNetsJson},
        {"dangling_tracks_count", danglingCount},
        {"dangling_tracks", danglingTracksJson},
        {"unassigned_pads_count", unassignedCount},
        {"unassigned_pads", unassignedPadsJson},
        {"unsynced_schematic_components_count", unsyncedCount},
        {"unsynced_schematic_components", unsyncedJson},
        {"summary", outSummary}
    };
}

nlohmann::json EdaAccelerators::calcSchematicErcAudit(const nlohmann::json& args,
                                                      bool& outSuccess,
                                                      std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "eda_calc_schematic_erc_audit requires a JSON object payload";
        return {{"ok", false}, {"error", outSummary}};
    }

    auto distPointToSeg = [](double px, double py,
                             double x1, double y1,
                             double x2, double y2) -> double {
        double dx = x2 - x1;
        double dy = y2 - y1;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
            return std::hypot(px - x1, py - y1);
        }
        double t = std::clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0.0, 1.0);
        return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
    };

    struct SchWire {
        std::string id;
        std::string net;
        std::vector<std::pair<double, double>> pts;
    };

    struct SchPin {
        std::string id;
        std::string compId;
        std::string ref;
        std::string number;
        std::string name;
        std::string net;
        double x = 0.0;
        double rawY = 0.0;
        double normY = 0.0;
        bool noConnected = false;
        bool isPartPin = true;
        bool isNetFlag = false;
        std::string netFlagName;
        bool touchesWire = false;
    };

    std::vector<SchWire> wires;
    bool anyWireVertex = false;
    bool allWireYNonNegative = true;

    if (args.contains("wires") && args["wires"].is_array()) {
        for (const auto& w : args["wires"]) {
            if (!w.is_object()) continue;
            SchWire sw;
            sw.id = w.value("id", "w_" + std::to_string(wires.size() + 1));
            sw.net = w.value("net", "");
            for (const char* lineKey : {"line", "points"}) {
                if (w.contains(lineKey) && w[lineKey].is_array()) {
                    const auto& arr = w[lineKey];
                    if (!arr.empty() && arr[0].is_number()) {
                        for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                            if (arr[i].is_number() && arr[i + 1].is_number()) {
                                double wx = arr[i].get<double>();
                                double wy = arr[i + 1].get<double>();
                                sw.pts.push_back({wx, wy});
                                anyWireVertex = true;
                                if (wy < 0.0) allWireYNonNegative = false;
                            }
                        }
                    } else {
                        for (const auto& pt : arr) {
                            if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                                double wx = pt[0].get<double>();
                                double wy = pt[1].get<double>();
                                sw.pts.push_back({wx, wy});
                                anyWireVertex = true;
                                if (wy < 0.0) allWireYNonNegative = false;
                            } else if (pt.is_object() && pt.contains("x") && pt.contains("y") &&
                                       pt["x"].is_number() && pt["y"].is_number()) {
                                double wx = pt["x"].get<double>();
                                double wy = pt["y"].get<double>();
                                sw.pts.push_back({wx, wy});
                                anyWireVertex = true;
                                if (wy < 0.0) allWireYNonNegative = false;
                            }
                        }
                    }
                    if (!sw.pts.empty()) break;
                }
            }
            if (sw.pts.empty() && w.contains("x1") && w.contains("y1") &&
                w.contains("x2") && w.contains("y2")) {
                double x1 = w.value("x1", 0.0);
                double y1 = w.value("y1", 0.0);
                double x2 = w.value("x2", 0.0);
                double y2 = w.value("y2", 0.0);
                sw.pts.push_back({x1, y1});
                sw.pts.push_back({x2, y2});
                anyWireVertex = true;
                if (y1 < 0.0 || y2 < 0.0) allWireYNonNegative = false;
            }
            if (sw.pts.size() >= 2) {
                wires.push_back(sw);
            }
        }
    }

    int totalPartComponents = 0;
    int totalPartPins = 0;
    nlohmann::json componentIssuesJson = nlohmann::json::array();
    std::map<std::string, int> partRefCounts;
    std::vector<SchPin> pins;

    if (args.contains("components") && args["components"].is_array()) {
        for (const auto& c : args["components"]) {
            if (!c.is_object()) continue;
            std::string cid = c.value("id", "");
            std::string ref = c.value("reference", c.value("ref", c.value("designator", "")));
            std::string val = c.value("value", c.value("name", ""));
            std::string compType = c.value("component_type", c.value("type", ""));
            std::string fp = c.value("footprint", "");
            std::string uid = c.value("unique_id", c.value("uniqueId", ""));
            double cx = c.value("x", 0.0);
            double cy = c.value("y", 0.0);

            bool isFrame = (compType == "frame" || ref.rfind("FRAME", 0) == 0);
            if (isFrame) continue;

            bool isNetFlag = (compType == "netflag" || compType == "netport" || compType == "power" ||
                              ref.rfind("#", 0) == 0 || ref == "GND" || ref == "VCC");
            bool isPart = (!isNetFlag && (compType == "part" || compType.empty()));

            if (isPart) {
                ++totalPartComponents;
                if (ref.empty() || ref.find('?') != std::string::npos) {
                    componentIssuesJson.push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"issue", "unannotated_designator"},
                        {"x", cx},
                        {"y", cy}
                    });
                } else {
                    if (++partRefCounts[ref] > 1) {
                        componentIssuesJson.push_back({
                            {"id", cid},
                            {"reference", ref},
                            {"issue", "duplicate_designator"},
                            {"x", cx},
                            {"y", cy}
                        });
                    }
                }
                if (uid.empty()) {
                    componentIssuesJson.push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"issue", "missing_unique_id"},
                        {"x", cx},
                        {"y", cy}
                    });
                }
                if (fp.empty()) {
                    componentIssuesJson.push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"issue", "missing_footprint"},
                        {"x", cx},
                        {"y", cy}
                    });
                }
            }

            if (c.contains("pins") && c["pins"].is_array()) {
                for (const auto& p : c["pins"]) {
                    if (!p.is_object()) continue;
                    SchPin sp;
                    sp.id = p.value("id", cid + "_" + p.value("number", ""));
                    sp.compId = cid;
                    sp.ref = ref;
                    sp.number = p.value("number", p.value("pin_number", ""));
                    sp.name = p.value("name", p.value("pin_name", ""));
                    sp.net = p.value("net", "");
                    sp.x = p.value("x", cx);
                    sp.rawY = p.value("y", cy);
                    sp.normY = (sp.rawY < 0.0 && ((anyWireVertex && allWireYNonNegative) || cy > 0.0))
                                   ? std::fabs(sp.rawY)
                                   : sp.rawY;
                    sp.noConnected = p.value("no_connected", p.value("noConnected", false));
                    sp.isPartPin = isPart;
                    sp.isNetFlag = isNetFlag;
                    if (isNetFlag) {
                        if (!val.empty()) sp.netFlagName = val;
                        else if (!sp.net.empty()) sp.netFlagName = sp.net;
                        else if (!sp.name.empty()) sp.netFlagName = sp.name;
                    }
                    if (isPart) {
                        ++totalPartPins;
                    }
                    pins.push_back(sp);
                }
            } else if (isNetFlag) {
                SchPin sp;
                sp.id = cid + "_p1";
                sp.compId = cid;
                sp.ref = ref;
                sp.number = "1";
                sp.name = val;
                sp.net = val;
                sp.x = cx;
                sp.rawY = cy;
                sp.normY = (sp.rawY < 0.0 && anyWireVertex && allWireYNonNegative)
                               ? std::fabs(sp.rawY)
                               : sp.rawY;
                sp.noConnected = false;
                sp.isPartPin = false;
                sp.isNetFlag = true;
                sp.netFlagName = val;
                pins.push_back(sp);
            }
        }
    }

    const double kTol = 1.5;
    size_t nW = wires.size();
    size_t nP = pins.size();
    size_t totalNodes = nW + nP;
    std::vector<size_t> ufParent(totalNodes);
    for (size_t i = 0; i < totalNodes; ++i) ufParent[i] = i;

    auto ufFind = [&](size_t x) {
        size_t r = x;
        while (ufParent[r] != r) r = ufParent[r];
        while (ufParent[x] != r) {
            size_t nxt = ufParent[x];
            ufParent[x] = r;
            x = nxt;
        }
        return r;
    };
    auto ufUnion = [&](size_t a, size_t b) {
        size_t ra = ufFind(a);
        size_t rb = ufFind(b);
        if (ra != rb) ufParent[rb] = ra;
    };

    auto pointTouchesWire = [&](double px, double py, const SchWire& w) -> bool {
        for (size_t k = 0; k + 1 < w.pts.size(); ++k) {
            if (distPointToSeg(px, py, w.pts[k].first, w.pts[k].second,
                               w.pts[k + 1].first, w.pts[k + 1].second) <= kTol) {
                return true;
            }
        }
        return false;
    };

    auto pinMatchesPoint = [&](const SchPin& pin, double px, double py) -> bool {
        if (std::hypot(pin.x - px, pin.normY - py) <= kTol) return true;
        if (std::hypot(pin.x - px, pin.rawY - py) <= kTol) return true;
        return false;
    };

    auto pinMatchesWire = [&](const SchPin& pin, const SchWire& w) -> bool {
        if (pointTouchesWire(pin.x, pin.normY, w)) return true;
        if (std::fabs(pin.normY - pin.rawY) > 1e-6 && pointTouchesWire(pin.x, pin.rawY, w)) {
            return true;
        }
        return false;
    };

    // 1. Wire-to-Wire union (shared vertices and T-junction mid-segment landings)
    for (size_t i = 0; i < nW; ++i) {
        for (size_t j = i + 1; j < nW; ++j) {
            bool touches = false;
            for (const auto& pt : wires[i].pts) {
                if (pointTouchesWire(pt.first, pt.second, wires[j])) {
                    touches = true;
                    break;
                }
            }
            if (!touches) {
                for (const auto& pt : wires[j].pts) {
                    if (pointTouchesWire(pt.first, pt.second, wires[i])) {
                        touches = true;
                        break;
                    }
                }
            }
            if (touches) {
                ufUnion(i, j);
            }
        }
    }

    // 2. Pin-to-Wire union
    for (size_t p = 0; p < nP; ++p) {
        for (size_t w = 0; w < nW; ++w) {
            if (pinMatchesWire(pins[p], wires[w])) {
                ufUnion(nW + p, w);
                pins[p].touchesWire = true;
            }
        }
    }

    // 3. Pin-to-Pin union (colocated pins)
    for (size_t i = 0; i < nP; ++i) {
        for (size_t j = i + 1; j < nP; ++j) {
            if (pinMatchesPoint(pins[i], pins[j].x, pins[j].normY) ||
                pinMatchesPoint(pins[i], pins[j].x, pins[j].rawY)) {
                ufUnion(nW + i, nW + j);
            }
        }
    }

    // Propagate explicit net names & netflags across each DSU component
    struct IslandData {
        std::vector<size_t> wireIndices;
        std::vector<size_t> partPinIndices;
        std::vector<size_t> netFlagPinIndices;
        std::set<std::string> netFlagNames;
        std::set<std::string> explicitNetNames;
    };
    std::map<size_t, IslandData> islands;
    for (size_t w = 0; w < nW; ++w) {
        size_t r = ufFind(w);
        islands[r].wireIndices.push_back(w);
        if (!wires[w].net.empty()) {
            islands[r].explicitNetNames.insert(wires[w].net);
        }
    }
    for (size_t p = 0; p < nP; ++p) {
        size_t r = ufFind(nW + p);
        if (pins[p].isPartPin) {
            islands[r].partPinIndices.push_back(p);
        }
        if (pins[p].isNetFlag) {
            islands[r].netFlagPinIndices.push_back(p);
            if (!pins[p].netFlagName.empty()) {
                islands[r].netFlagNames.insert(pins[p].netFlagName);
                islands[r].explicitNetNames.insert(pins[p].netFlagName);
            }
        }
        if (!pins[p].net.empty()) {
            islands[r].explicitNetNames.insert(pins[p].net);
        }
    }

    // Count global occurrences of each explicit net name across part pins / netflags
    std::map<std::string, int> globalNetPinCount;
    for (const auto& kv : islands) {
        const auto& isl = kv.second;
        int pinCount = static_cast<int>(isl.partPinIndices.size() + isl.netFlagPinIndices.size());
        for (const auto& netName : isl.explicitNetNames) {
            globalNetPinCount[netName] += pinCount;
        }
    }

    // 4. Identify unconnected part pins
    nlohmann::json unconnectedPinsJson = nlohmann::json::array();
    for (size_t p = 0; p < nP; ++p) {
        const auto& pin = pins[p];
        if (!pin.isPartPin || pin.noConnected) continue;
        size_t r = ufFind(nW + p);
        const auto& isl = islands[r];
        size_t totalIslandPins = isl.partPinIndices.size() + isl.netFlagPinIndices.size();
        bool connectedByNetName = false;
        for (const auto& netName : isl.explicitNetNames) {
            if (!netName.empty() && (globalNetPinCount[netName] >= 2 || !isl.netFlagPinIndices.empty())) {
                connectedByNetName = true;
                break;
            }
        }
        if (totalIslandPins < 2 && !connectedByNetName) {
            unconnectedPinsJson.push_back({
                {"id", pin.id},
                {"reference", pin.ref},
                {"pin_number", pin.number},
                {"pin_name", pin.name},
                {"x", pin.x},
                {"y", pin.normY},
                {"reason", pin.touchesWire ? "single_pin_net" : "unconnected_pin"}
            });
        }
    }

    // 5. Identify dangling wires
    nlohmann::json danglingWiresJson = nlohmann::json::array();
    for (size_t w = 0; w < nW; ++w) {
        const auto& wire = wires[w];
        size_t r = ufFind(w);
        const auto& isl = islands[r];
        size_t totalIslandPins = isl.partPinIndices.size() + isl.netFlagPinIndices.size();

        std::pair<double, double> eps[2] = {wire.pts.front(), wire.pts.back()};
        int anchoredEps = 0;
        std::pair<double, double> unanchoredPt = eps[0];

        for (int ep = 0; ep < 2; ++ep) {
            double ex = eps[ep].first;
            double ey = eps[ep].second;
            bool anchored = false;
            for (size_t p = 0; p < nP; ++p) {
                if (pinMatchesPoint(pins[p], ex, ey)) {
                    anchored = true;
                    break;
                }
            }
            if (!anchored) {
                for (size_t otherW = 0; otherW < nW; ++otherW) {
                    if (otherW == w) continue;
                    if (pointTouchesWire(ex, ey, wires[otherW])) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (anchored) {
                ++anchoredEps;
            } else {
                unanchoredPt = {ex, ey};
            }
        }

        if (totalIslandPins < 2 || anchoredEps < 2) {
            danglingWiresJson.push_back({
                {"wire_id", wire.id},
                {"net", wire.net},
                {"x", unanchoredPt.first},
                {"y", unanchoredPt.second},
                {"reason", (totalIslandPins == 0) ? "isolated_wire" : "unanchored_wire_endpoint"}
            });
        }
    }

    // 6. Net issues (shorted netflags / conflicting net names) and inferred nets
    nlohmann::json netIssuesJson = nlohmann::json::array();
    nlohmann::json inferredNetsJson = nlohmann::json::array();
    int powerShortsCount = 0;
    int autoNetIdx = 1;

    for (const auto& kv : islands) {
        const auto& isl = kv.second;
        if (isl.partPinIndices.empty() && isl.netFlagPinIndices.empty()) continue;

        nlohmann::json pinRefsJson = nlohmann::json::array();
        for (size_t pIdx : isl.partPinIndices) {
            pinRefsJson.push_back(pins[pIdx].ref + "." + pins[pIdx].number);
        }

        if (isl.netFlagNames.size() > 1 || isl.explicitNetNames.size() > 1) {
            std::string joined;
            const auto& nameSet = (isl.netFlagNames.size() > 1) ? isl.netFlagNames : isl.explicitNetNames;
            for (const auto& nm : nameSet) {
                if (!joined.empty()) joined += "/";
                joined += nm;
            }
            ++powerShortsCount;
            netIssuesJson.push_back({
                {"net", joined},
                {"issue", "shorted_nets"},
                {"pins", pinRefsJson}
            });
        }

        if (!isl.partPinIndices.empty()) {
            std::string resolvedNet;
            if (!isl.netFlagNames.empty()) {
                resolvedNet = *isl.netFlagNames.begin();
            } else if (!isl.explicitNetNames.empty()) {
                resolvedNet = *isl.explicitNetNames.begin();
            } else {
                resolvedNet = "$NET_" + std::to_string(autoNetIdx++);
            }
            inferredNetsJson.push_back({
                {"name", resolvedNet},
                {"pins", pinRefsJson}
            });
        }
    }

    int unconnectedCount = static_cast<int>(unconnectedPinsJson.size());
    int danglingWiresCount = static_cast<int>(danglingWiresJson.size());
    int compIssuesCount = static_cast<int>(componentIssuesJson.size());
    int netIssuesCount = static_cast<int>(netIssuesJson.size());
    bool passed = (unconnectedCount == 0 && danglingWiresCount == 0 &&
                   compIssuesCount == 0 && powerShortsCount == 0 && netIssuesCount == 0);

    outSuccess = true;
    outSummary = std::string("Schematic ERC ") + (passed ? "PASSED" : "FAILED") +
                 " • " + std::to_string(unconnectedCount) + " unconnected pins • " +
                 std::to_string(danglingWiresCount) + " dangling wires • " +
                 std::to_string(compIssuesCount) + " component issues • " +
                 std::to_string(netIssuesCount) + " net issues";

    return {
        {"ok", true},
        {"passed", passed},
        {"total_part_components", totalPartComponents},
        {"total_pins", totalPartPins},
        {"unconnected_pins_count", unconnectedCount},
        {"unconnected_pins", unconnectedPinsJson},
        {"dangling_wires_count", danglingWiresCount},
        {"dangling_wires", danglingWiresJson},
        {"component_issues_count", compIssuesCount},
        {"component_issues", componentIssuesJson},
        {"net_issues_count", netIssuesCount},
        {"net_issues", netIssuesJson},
        {"inferred_nets", inferredNetsJson},
        {"summary", outSummary}
    };
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
