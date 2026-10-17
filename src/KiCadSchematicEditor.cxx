/*
 * KiCadSchematicEditor.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "KiCadSchematicEditor.hxx"
#include "EasyEdaConverter.hxx"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace aimon {

namespace {

double toDoubleSafe(const std::string& s, double fallback = 0.0) {
    if (s.empty()) return fallback;
    try {
        double v = std::stod(s);
        return std::isfinite(v) ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

int toIntSafe(const std::string& s, int fallback = 0) {
    if (s.empty()) return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

std::string fmtMm(double val) {
    if (std::abs(val) < 1e-9) val = 0.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4) << val;
    std::string s = oss.str();
    while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') {
        s.pop_back();
    }
    return s;
}

std::string escapeSexp(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        if (c != '\n' && c != '\r' && c != '\0') {
            out.push_back(c);
        }
    }
    return out;
}

std::string toLowerStr(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

struct SchPinModel {
    std::string number;
    std::string name;
    std::string type = "passive";
    double x = 0.0;
    double y = 0.0;
    std::string net;
};

struct SchCompModel {
    std::string reference;
    std::string value;
    std::string libId = "Device:R";
    std::string footprint = "Resistor_SMD:R_0603_1608Metric";
    std::string lcsc;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    std::map<std::string, std::string> extraProps;
    std::vector<SchPinModel> pins;
};

struct SchWireModel {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    std::string net;
};

struct SchLabelModel {
    std::string name;
    double x = 0.0;
    double y = 0.0;
    double angle = 0.0;
    bool isPower = false;
};

struct PcbPadModel {
    std::string number;
    std::string type = "smd";
    std::string shape = "rect";
    double relX = 0.0;
    double relY = 0.0;
    double sizeX = 0.8;
    double sizeY = 0.8;
    double drill = 0.0;
    std::string layer = "F.Cu";
    int netCode = 0;
    std::string netName;
};

struct PcbFootprintModel {
    std::string reference;
    std::string value;
    std::string footprint = "Resistor_SMD:R_0603_1608Metric";
    std::string lcsc;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    std::string layer = "F.Cu";
    std::vector<PcbPadModel> pads;
};

struct PcbTrackModel {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double width = 0.25;
    std::string layer = "F.Cu";
    int netCode = 0;
    std::string netName;
};

struct PcbViaModel {
    double x = 0.0;
    double y = 0.0;
    double size = 0.6;
    double drill = 0.3;
    int netCode = 0;
    std::string netName;
};

struct PcbZoneModel {
    std::string netName = "GND";
    int netCode = 0;
    std::string layer = "F.Cu";
    double clearance = 0.25;
    std::vector<std::pair<double, double>> points;
};

struct PcbHoleModel {
    double x = 0.0;
    double y = 0.0;
    double drill = 3.2;
    double pad = 6.0;
};

struct PcbOutlineLine {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
};

std::vector<SchPinModel> defaultPinsForSymbol(const std::string& libId, double compX, double compY) {
    std::vector<SchPinModel> pins;
    std::string lower = toLowerStr(libId);
    if (lower.find("sot-223") != std::string::npos || lower.find("ams1117") != std::string::npos ||
        lower.find("regulator") != std::string::npos) {
        pins.push_back({"1", "GND", "power_in", EasyEdaConverter::snapTo50MilGrid(compX - 2.54), EasyEdaConverter::snapTo50MilGrid(compY + 2.54), ""});
        pins.push_back({"2", "VOUT", "power_out", EasyEdaConverter::snapTo50MilGrid(compX + 2.54), EasyEdaConverter::snapTo50MilGrid(compY), ""});
        pins.push_back({"3", "VIN", "power_in", EasyEdaConverter::snapTo50MilGrid(compX - 2.54), EasyEdaConverter::snapTo50MilGrid(compY - 2.54), ""});
    } else if (lower.find("conn_01x04") != std::string::npos || lower.find("1x04") != std::string::npos) {
        for (int i = 0; i < 4; ++i) {
            pins.push_back({std::to_string(i + 1), "P" + std::to_string(i + 1), "passive",
                            EasyEdaConverter::snapTo50MilGrid(compX + 2.54),
                            EasyEdaConverter::snapTo50MilGrid(compY + (i - 1) * 2.54), ""});
        }
    } else {
        pins.push_back({"1", "1", "passive", EasyEdaConverter::snapTo50MilGrid(compX - 2.54), EasyEdaConverter::snapTo50MilGrid(compY), ""});
        pins.push_back({"2", "2", "passive", EasyEdaConverter::snapTo50MilGrid(compX + 2.54), EasyEdaConverter::snapTo50MilGrid(compY), ""});
    }
    return pins;
}

std::vector<PcbPadModel> defaultPadsForFootprint(const std::string& fpName, const std::string& layer) {
    std::vector<PcbPadModel> pads;
    std::string lower = toLowerStr(fpName);
    if (lower.find("sot-223") != std::string::npos) {
        pads.push_back({"1", "smd", "rect", -2.3, 3.15, 1.0, 1.5, 0.0, layer, 0, ""});
        pads.push_back({"2", "smd", "rect", 0.0, 3.15, 1.0, 1.5, 0.0, layer, 0, ""});
        pads.push_back({"3", "smd", "rect", 2.3, 3.15, 1.0, 1.5, 0.0, layer, 0, ""});
    } else if (lower.find("pinheader_1x04") != std::string::npos || lower.find("1x04") != std::string::npos) {
        for (int i = 0; i < 4; ++i) {
            pads.push_back({std::to_string(i + 1), "thru_hole", "circle", 0.0, i * 2.54, 1.7, 1.7, 1.0, "*.Cu", 0, ""});
        }
    } else if (lower.find("soic-8") != std::string::npos) {
        for (int i = 0; i < 4; ++i) {
            pads.push_back({std::to_string(i + 1), "smd", "rect", -2.4, (i - 1.5) * 1.27, 1.5, 0.6, 0.0, layer, 0, ""});
            pads.push_back({std::to_string(8 - i), "smd", "rect", 2.4, (i - 1.5) * 1.27, 1.5, 0.6, 0.0, layer, 0, ""});
        }
    } else {
        pads.push_back({"1", "smd", "rect", -0.8, 0.0, 0.9, 0.95, 0.0, layer, 0, ""});
        pads.push_back({"2", "smd", "rect", 0.8, 0.0, 0.9, 0.95, 0.0, layer, 0, ""});
    }
    return pads;
}

std::string coordKey(double x, double y) {
    long long gx = std::llround(x / 0.01);
    long long gy = std::llround(y / 0.01);
    return std::to_string(gx) + ":" + std::to_string(gy);
}

void extractSchematicFromSexp(const SexpNode& root,
                              std::vector<SchCompModel>& comps,
                              std::vector<SchWireModel>& wires,
                              std::vector<SchLabelModel>& labels) {
    if (!root.isList || root.head() != "kicad_sch") return;

    // Map lib_symbols pin offsets if defined
    std::map<std::string, std::vector<SchPinModel>> libSymbolPins;
    if (const SexpNode* libSyms = root.findChild("lib_symbols")) {
        for (const SexpNode* sym : libSyms->findChildren("symbol")) {
            if (sym->children.size() < 2) continue;
            std::string symId = sym->children[1].atom;
            std::vector<SchPinModel> pins;
            for (const auto& sub : sym->children) {
                if (!sub.isList) continue;
                if (sub.head() == "pin") {
                    SchPinModel p;
                    if (sub.children.size() >= 2) p.type = sub.children[1].atom;
                    if (const SexpNode* atNode = sub.findChild("at")) {
                        if (atNode->children.size() >= 3) {
                            p.x = toDoubleSafe(atNode->children[1].atom);
                            p.y = toDoubleSafe(atNode->children[2].atom);
                        }
                    }
                    if (const SexpNode* nameNode = sub.findChild("name")) {
                        if (nameNode->children.size() >= 2) p.name = nameNode->children[1].atom;
                    }
                    if (const SexpNode* numNode = sub.findChild("number")) {
                        if (numNode->children.size() >= 2) p.number = numNode->children[1].atom;
                    }
                    pins.push_back(p);
                } else if (sub.head() == "symbol") {
                    for (const SexpNode* pinNode : sub.findChildren("pin")) {
                        SchPinModel p;
                        if (pinNode->children.size() >= 2) p.type = pinNode->children[1].atom;
                        if (const SexpNode* atNode = pinNode->findChild("at")) {
                            if (atNode->children.size() >= 3) {
                                p.x = toDoubleSafe(atNode->children[1].atom);
                                p.y = toDoubleSafe(atNode->children[2].atom);
                            }
                        }
                        if (const SexpNode* nameNode = pinNode->findChild("name")) {
                            if (nameNode->children.size() >= 2) p.name = nameNode->children[1].atom;
                        }
                        if (const SexpNode* numNode = pinNode->findChild("number")) {
                            if (numNode->children.size() >= 2) p.number = numNode->children[1].atom;
                        }
                        pins.push_back(p);
                    }
                }
            }
            libSymbolPins[symId] = pins;
        }
    }

    for (const auto& child : root.children) {
        if (!child.isList) continue;
        std::string h = child.head();
        if (h == "symbol") {
            SchCompModel c;
            if (const SexpNode* libNode = child.findChild("lib_id")) {
                if (libNode->children.size() >= 2) c.libId = libNode->children[1].atom;
            }
            if (const SexpNode* atNode = child.findChild("at")) {
                if (atNode->children.size() >= 3) {
                    c.x = toDoubleSafe(atNode->children[1].atom);
                    c.y = toDoubleSafe(atNode->children[2].atom);
                }
                if (atNode->children.size() >= 4) {
                    c.rotation = toDoubleSafe(atNode->children[3].atom);
                }
            }
            for (const SexpNode* prop : child.findChildren("property")) {
                if (prop->children.size() >= 3) {
                    std::string k = prop->children[1].atom;
                    std::string v = prop->children[2].atom;
                    if (k == "Reference") c.reference = v;
                    else if (k == "Value") c.value = v;
                    else if (k == "Footprint") c.footprint = v;
                    else if (k == "LCSC") c.lcsc = v;
                    else c.extraProps[k] = v;
                }
            }
            for (const SexpNode* pinNode : child.findChildren("pin")) {
                if (pinNode->children.size() >= 2) {
                    SchPinModel p;
                    p.number = pinNode->children[1].atom;
                    p.name = p.number;
                    p.x = c.x;
                    p.y = c.y;
                    if (const SexpNode* atNode = pinNode->findChild("at")) {
                        if (atNode->children.size() >= 3) {
                            p.x = toDoubleSafe(atNode->children[1].atom);
                            p.y = toDoubleSafe(atNode->children[2].atom);
                        }
                    }
                    if (const SexpNode* netNode = pinNode->findChild("net")) {
                        if (netNode->children.size() >= 2) p.net = netNode->children[1].atom;
                    }
                    c.pins.push_back(p);
                }
            }
            if (c.pins.empty()) {
                auto it = libSymbolPins.find(c.libId);
                if (it != libSymbolPins.end() && !it->second.empty()) {
                    for (auto p : it->second) {
                        p.x = EasyEdaConverter::snapTo50MilGrid(c.x + p.x);
                        p.y = EasyEdaConverter::snapTo50MilGrid(c.y + p.y);
                        c.pins.push_back(p);
                    }
                } else {
                    c.pins = defaultPinsForSymbol(c.libId, c.x, c.y);
                }
            }
            if (!c.reference.empty()) {
                comps.push_back(c);
            }
        } else if (h == "wire") {
            if (const SexpNode* pts = child.findChild("pts")) {
                std::vector<const SexpNode*> xys = pts->findChildren("xy");
                if (xys.size() >= 2 && xys[0]->children.size() >= 3 && xys[1]->children.size() >= 3) {
                    SchWireModel w;
                    w.x1 = toDoubleSafe(xys[0]->children[1].atom);
                    w.y1 = toDoubleSafe(xys[0]->children[2].atom);
                    w.x2 = toDoubleSafe(xys[1]->children[1].atom);
                    w.y2 = toDoubleSafe(xys[1]->children[2].atom);
                    wires.push_back(w);
                }
            }
        } else if (h == "label" || h == "global_label" || h == "power_port") {
            if (child.children.size() >= 2) {
                SchLabelModel l;
                l.name = child.children[1].atom;
                l.isPower = (h == "power_port");
                if (const SexpNode* atNode = child.findChild("at")) {
                    if (atNode->children.size() >= 3) {
                        l.x = toDoubleSafe(atNode->children[1].atom);
                        l.y = toDoubleSafe(atNode->children[2].atom);
                    }
                    if (atNode->children.size() >= 4) {
                        l.angle = toDoubleSafe(atNode->children[3].atom);
                    }
                }
                labels.push_back(l);
            }
        }
    }
}

// Disjoint-set union to group connected coordinates into electrical nets
nlohmann::json buildSchematicNetlist(const std::vector<SchCompModel>& comps,
                                     const std::vector<SchWireModel>& wires,
                                     const std::vector<SchLabelModel>& labels) {
    std::map<std::string, std::string> parent;
    std::map<std::string, std::string> netLabelForCoord;

    auto findRoot = [&](const std::string& k) -> std::string {
        if (parent.find(k) == parent.end()) parent[k] = k;
        std::string cur = k;
        while (parent[cur] != cur) {
            parent[cur] = parent[parent[cur]];
            cur = parent[cur];
        }
        return cur;
    };

    auto unite = [&](const std::string& a, const std::string& b) {
        std::string ra = findRoot(a);
        std::string rb = findRoot(b);
        if (ra != rb) parent[rb] = ra;
    };

    for (const auto& w : wires) {
        std::string k1 = coordKey(w.x1, w.y1);
        std::string k2 = coordKey(w.x2, w.y2);
        unite(k1, k2);
        if (!w.net.empty()) {
            netLabelForCoord[k1] = w.net;
            netLabelForCoord[k2] = w.net;
        }
    }

    for (const auto& l : labels) {
        std::string k = coordKey(l.x, l.y);
        findRoot(k);
        if (!l.name.empty()) {
            netLabelForCoord[k] = l.name;
        }
    }

    for (const auto& c : comps) {
        for (const auto& p : c.pins) {
            std::string k = coordKey(p.x, p.y);
            findRoot(k);
            if (!p.net.empty()) {
                netLabelForCoord[k] = p.net;
            }
        }
    }

    // Propagate named labels to cluster roots
    std::map<std::string, std::string> rootNetName;
    for (const auto& kv : netLabelForCoord) {
        std::string r = findRoot(kv.first);
        if (rootNetName[r].empty()) {
            rootNetName[r] = kv.second;
        }
    }

    // Group component pins by net name
    std::map<std::string, std::vector<nlohmann::json>> netNodes;
    int autoNetCounter = 1;
    for (const auto& c : comps) {
        if (!c.reference.empty() && c.reference[0] == '#') continue;
        for (const auto& p : c.pins) {
            std::string netName = p.net;
            if (netName.empty()) {
                std::string r = findRoot(coordKey(p.x, p.y));
                if (!rootNetName[r].empty()) {
                    netName = rootNetName[r];
                } else {
                    // Check if any other pin or wire shares this root
                    int deg = 0;
                    for (const auto& kv : parent) {
                        if (findRoot(kv.first) == r) ++deg;
                    }
                    if (deg > 1) {
                        rootNetName[r] = "Net-(" + c.reference + "-Pad" + p.number + "_" + std::to_string(autoNetCounter++) + ")";
                        netName = rootNetName[r];
                    }
                }
            }
            if (!netName.empty()) {
                netNodes[netName].push_back({
                    {"ref", c.reference},
                    {"pin", p.number},
                    {"pin_name", p.name},
                    {"x", p.x},
                    {"y", p.y}
                });
            }
        }
    }

    nlohmann::json netsArr = nlohmann::json::array();
    int code = 1;
    for (const auto& kv : netNodes) {
        netsArr.push_back({
            {"code", code++},
            {"name", kv.first},
            {"nodes", kv.second}
        });
    }
    return netsArr;
}

std::string serializeSchematicSexp(const std::vector<SchCompModel>& comps,
                                   const std::vector<SchWireModel>& wires,
                                   const std::vector<SchLabelModel>& labels) {
    std::ostringstream out;
    out << "(kicad_sch (version 20231120) (generator \"aimon-kicad-editor\")\n";
    out << "  (paper \"A4\")\n";
    out << "  (lib_symbols\n";
    std::set<std::string> emittedLibIds;
    for (const auto& c : comps) {
        std::string libId = c.libId.empty() ? ("Custom:" + c.reference) : c.libId;
        if (emittedLibIds.count(libId)) continue;
        emittedLibIds.insert(libId);
        out << "    (symbol \"" << escapeSexp(libId) << "\" (in_bom yes) (on_board yes)\n";
        out << "      (symbol \"" << escapeSexp(c.reference) << "_1_1\"\n";
        for (const auto& p : c.pins) {
            double relX = EasyEdaConverter::snapTo50MilGrid(p.x - c.x);
            double relY = EasyEdaConverter::snapTo50MilGrid(p.y - c.y);
            out << "        (pin " << escapeSexp(p.type.empty() ? "passive" : p.type)
                << " line (at " << fmtMm(relX) << " " << fmtMm(relY) << " 0) (length 2.54)\n";
            out << "          (name \"" << escapeSexp(p.name) << "\")\n";
            out << "          (number \"" << escapeSexp(p.number) << "\")\n";
            out << "        )\n";
        }
        out << "      )\n";
        out << "    )\n";
    }
    out << "  )\n";

    for (const auto& c : comps) {
        out << "  (symbol (lib_id \"" << escapeSexp(c.libId) << "\") (at "
            << fmtMm(c.x) << " " << fmtMm(c.y) << " " << fmtMm(c.rotation) << ") (unit 1)\n";
        out << "    (property \"Reference\" \"" << escapeSexp(c.reference) << "\" (at "
            << fmtMm(c.x) << " " << fmtMm(c.y) << " 0))\n";
        out << "    (property \"Value\" \"" << escapeSexp(c.value) << "\" (at "
            << fmtMm(c.x) << " " << fmtMm(c.y) << " 0))\n";
        out << "    (property \"Footprint\" \"" << escapeSexp(c.footprint) << "\" (at "
            << fmtMm(c.x) << " " << fmtMm(c.y) << " 0))\n";
        if (!c.lcsc.empty()) {
            out << "    (property \"LCSC\" \"" << escapeSexp(c.lcsc) << "\" (at "
                << fmtMm(c.x) << " " << fmtMm(c.y) << " 0))\n";
        }
        for (const auto& kv : c.extraProps) {
            out << "    (property \"" << escapeSexp(kv.first) << "\" \"" << escapeSexp(kv.second)
                << "\" (at " << fmtMm(c.x) << " " << fmtMm(c.y) << " 0))\n";
        }
        for (const auto& p : c.pins) {
            out << "    (pin \"" << escapeSexp(p.number) << "\" (at "
                << fmtMm(p.x) << " " << fmtMm(p.y) << ")";
            if (!p.net.empty()) {
                out << " (net \"" << escapeSexp(p.net) << "\")";
            }
            out << ")\n";
        }
        out << "  )\n";
    }

    for (const auto& l : labels) {
        if (l.isPower) {
            out << "  (power_port \"" << escapeSexp(l.name) << "\" (at "
                << fmtMm(l.x) << " " << fmtMm(l.y) << " " << fmtMm(l.angle) << "))\n";
        }
        out << "  (label \"" << escapeSexp(l.name) << "\" (at "
            << fmtMm(l.x) << " " << fmtMm(l.y) << " " << fmtMm(l.angle) << "))\n";
    }

    for (const auto& w : wires) {
        out << "  (wire (pts (xy " << fmtMm(w.x1) << " " << fmtMm(w.y1) << ") (xy "
            << fmtMm(w.x2) << " " << fmtMm(w.y2) << ")))\n";
        if (!w.net.empty()) {
            out << "  (label \"" << escapeSexp(w.net) << "\" (at "
                << fmtMm(w.x1) << " " << fmtMm(w.y1) << " 0))\n";
        }
    }
    out << ")\n";
    return out.str();
}

void extractPcbFromSexp(const SexpNode& root,
                        std::map<int, std::string>& netByCode,
                        std::map<std::string, int>& codeByNet,
                        std::vector<PcbOutlineLine>& outline,
                        double& outlineW,
                        double& outlineH,
                        std::vector<PcbFootprintModel>& footprints,
                        std::vector<PcbTrackModel>& tracks,
                        std::vector<PcbViaModel>& vias,
                        std::vector<PcbZoneModel>& zones,
                        std::vector<PcbHoleModel>& holes) {
    if (!root.isList || root.head() != "kicad_pcb") return;

    netByCode[0] = "";
    codeByNet[""] = 0;

    for (const auto& child : root.children) {
        if (!child.isList) continue;
        std::string h = child.head();
        if (h == "net" && child.children.size() >= 3) {
            int code = toIntSafe(child.children[1].atom, 0);
            std::string name = child.children[2].atom;
            netByCode[code] = name;
            codeByNet[name] = code;
        } else if (h == "gr_rect") {
            const SexpNode* st = child.findChild("start");
            const SexpNode* en = child.findChild("end");
            const SexpNode* ly = child.findChild("layer");
            if (st && en && st->children.size() >= 3 && en->children.size() >= 3) {
                std::string layerName = (ly && ly->children.size() >= 2) ? ly->children[1].atom : "Edge.Cuts";
                if (layerName == "Edge.Cuts") {
                    double x1 = toDoubleSafe(st->children[1].atom);
                    double y1 = toDoubleSafe(st->children[2].atom);
                    double x2 = toDoubleSafe(en->children[1].atom);
                    double y2 = toDoubleSafe(en->children[2].atom);
                    outlineW = std::max(outlineW, std::abs(x2 - x1));
                    outlineH = std::max(outlineH, std::abs(y2 - y1));
                }
            }
        } else if (h == "gr_line") {
            const SexpNode* st = child.findChild("start");
            const SexpNode* en = child.findChild("end");
            const SexpNode* ly = child.findChild("layer");
            if (st && en && st->children.size() >= 3 && en->children.size() >= 3) {
                std::string layerName = (ly && ly->children.size() >= 2) ? ly->children[1].atom : "Edge.Cuts";
                if (layerName == "Edge.Cuts") {
                    PcbOutlineLine ln;
                    ln.x1 = toDoubleSafe(st->children[1].atom);
                    ln.y1 = toDoubleSafe(st->children[2].atom);
                    ln.x2 = toDoubleSafe(en->children[1].atom);
                    ln.y2 = toDoubleSafe(en->children[2].atom);
                    outline.push_back(ln);
                    outlineW = std::max({outlineW, std::abs(ln.x2 - ln.x1), ln.x1, ln.x2});
                    outlineH = std::max({outlineH, std::abs(ln.y2 - ln.y1), ln.y1, ln.y2});
                }
            }
        } else if (h == "footprint" && child.children.size() >= 2) {
            PcbFootprintModel fp;
            fp.footprint = child.children[1].atom;
            if (const SexpNode* ly = child.findChild("layer")) {
                if (ly->children.size() >= 2) fp.layer = ly->children[1].atom;
            }
            if (const SexpNode* at = child.findChild("at")) {
                if (at->children.size() >= 3) {
                    fp.x = toDoubleSafe(at->children[1].atom);
                    fp.y = toDoubleSafe(at->children[2].atom);
                }
                if (at->children.size() >= 4) {
                    fp.rotation = toDoubleSafe(at->children[3].atom);
                }
            }
            for (const SexpNode* prop : child.findChildren("property")) {
                if (prop->children.size() >= 3) {
                    std::string k = prop->children[1].atom;
                    std::string v = prop->children[2].atom;
                    if (k == "Reference") fp.reference = v;
                    else if (k == "Value") fp.value = v;
                    else if (k == "LCSC") fp.lcsc = v;
                }
            }
            for (const SexpNode* fpText : child.findChildren("fp_text")) {
                if (fpText->children.size() >= 3) {
                    if (fpText->children[1].atom == "reference") fp.reference = fpText->children[2].atom;
                    else if (fpText->children[1].atom == "value") fp.value = fpText->children[2].atom;
                }
            }
            for (const SexpNode* padNode : child.findChildren("pad")) {
                if (padNode->children.size() >= 4) {
                    PcbPadModel pad;
                    pad.number = padNode->children[1].atom;
                    pad.type = padNode->children[2].atom;
                    pad.shape = padNode->children[3].atom;
                    if (const SexpNode* at = padNode->findChild("at")) {
                        if (at->children.size() >= 3) {
                            pad.relX = toDoubleSafe(at->children[1].atom);
                            pad.relY = toDoubleSafe(at->children[2].atom);
                        }
                    }
                    if (const SexpNode* sz = padNode->findChild("size")) {
                        if (sz->children.size() >= 3) {
                            pad.sizeX = toDoubleSafe(sz->children[1].atom, 0.8);
                            pad.sizeY = toDoubleSafe(sz->children[2].atom, 0.8);
                        }
                    }
                    if (const SexpNode* dr = padNode->findChild("drill")) {
                        if (dr->children.size() >= 2) {
                            pad.drill = toDoubleSafe(dr->children[1].atom, 0.0);
                        }
                    }
                    if (const SexpNode* n = padNode->findChild("net")) {
                        if (n->children.size() >= 2) {
                            pad.netCode = toIntSafe(n->children[1].atom, 0);
                            if (n->children.size() >= 3) {
                                pad.netName = n->children[2].atom;
                            } else if (netByCode.count(pad.netCode)) {
                                pad.netName = netByCode[pad.netCode];
                            }
                        }
                    }
                    fp.pads.push_back(pad);
                }
            }
            if (fp.footprint.find("MountingHole") != std::string::npos) {
                PcbHoleModel hole;
                hole.x = fp.x;
                hole.y = fp.y;
                hole.drill = (!fp.pads.empty() && fp.pads[0].drill > 0.0) ? fp.pads[0].drill : 3.2;
                hole.pad = (!fp.pads.empty()) ? fp.pads[0].sizeX : 6.0;
                holes.push_back(hole);
            }
            footprints.push_back(fp);
        } else if (h == "segment") {
            PcbTrackModel trk;
            if (const SexpNode* st = child.findChild("start")) {
                if (st->children.size() >= 3) {
                    trk.x1 = toDoubleSafe(st->children[1].atom);
                    trk.y1 = toDoubleSafe(st->children[2].atom);
                }
            }
            if (const SexpNode* en = child.findChild("end")) {
                if (en->children.size() >= 3) {
                    trk.x2 = toDoubleSafe(en->children[1].atom);
                    trk.y2 = toDoubleSafe(en->children[2].atom);
                }
            }
            if (const SexpNode* w = child.findChild("width")) {
                if (w->children.size() >= 2) trk.width = toDoubleSafe(w->children[1].atom, 0.25);
            }
            if (const SexpNode* ly = child.findChild("layer")) {
                if (ly->children.size() >= 2) trk.layer = ly->children[1].atom;
            }
            if (const SexpNode* n = child.findChild("net")) {
                if (n->children.size() >= 2) {
                    trk.netCode = toIntSafe(n->children[1].atom, 0);
                    if (n->children.size() >= 3) trk.netName = n->children[2].atom;
                    else if (netByCode.count(trk.netCode)) trk.netName = netByCode[trk.netCode];
                }
            }
            tracks.push_back(trk);
        } else if (h == "via") {
            PcbViaModel v;
            if (const SexpNode* at = child.findChild("at")) {
                if (at->children.size() >= 3) {
                    v.x = toDoubleSafe(at->children[1].atom);
                    v.y = toDoubleSafe(at->children[2].atom);
                }
            }
            if (const SexpNode* sz = child.findChild("size")) {
                if (sz->children.size() >= 2) v.size = toDoubleSafe(sz->children[1].atom, 0.6);
            }
            if (const SexpNode* dr = child.findChild("drill")) {
                if (dr->children.size() >= 2) v.drill = toDoubleSafe(dr->children[1].atom, 0.3);
            }
            if (const SexpNode* n = child.findChild("net")) {
                if (n->children.size() >= 2) {
                    v.netCode = toIntSafe(n->children[1].atom, 0);
                    if (n->children.size() >= 3) v.netName = n->children[2].atom;
                    else if (netByCode.count(v.netCode)) v.netName = netByCode[v.netCode];
                }
            }
            vias.push_back(v);
        } else if (h == "zone") {
            PcbZoneModel z;
            if (const SexpNode* n = child.findChild("net")) {
                if (n->children.size() >= 2) z.netCode = toIntSafe(n->children[1].atom, 0);
            }
            if (const SexpNode* nn = child.findChild("net_name")) {
                if (nn->children.size() >= 2) z.netName = nn->children[1].atom;
            } else if (netByCode.count(z.netCode)) {
                z.netName = netByCode[z.netCode];
            }
            if (const SexpNode* ly = child.findChild("layer")) {
                if (ly->children.size() >= 2) z.layer = ly->children[1].atom;
            }
            if (const SexpNode* poly = child.findChild("polygon")) {
                if (const SexpNode* pts = poly->findChild("pts")) {
                    for (const SexpNode* xy : pts->findChildren("xy")) {
                        if (xy->children.size() >= 3) {
                            z.points.push_back({toDoubleSafe(xy->children[1].atom),
                                                toDoubleSafe(xy->children[2].atom)});
                        }
                    }
                }
            }
            zones.push_back(z);
        }
    }
}

std::vector<std::string> computeUnroutedNets(const std::vector<PcbFootprintModel>& footprints,
                                             const std::vector<PcbTrackModel>& tracks,
                                             const std::vector<PcbZoneModel>& zones) {
    std::map<std::string, int> padCountPerNet;
    for (const auto& fp : footprints) {
        for (const auto& pad : fp.pads) {
            if (!pad.netName.empty()) {
                padCountPerNet[pad.netName]++;
            }
        }
    }

    std::set<std::string> routedOrPouredNets;
    for (const auto& t : tracks) {
        if (!t.netName.empty()) routedOrPouredNets.insert(t.netName);
    }
    for (const auto& z : zones) {
        if (!z.netName.empty()) routedOrPouredNets.insert(z.netName);
    }

    std::vector<std::string> unrouted;
    for (const auto& kv : padCountPerNet) {
        if (kv.second >= 2 && routedOrPouredNets.count(kv.first) == 0) {
            unrouted.push_back(kv.first);
        }
    }
    return unrouted;
}

std::string serializePcbSexp(const std::map<int, std::string>& netByCode,
                             double outlineW,
                             double outlineH,
                             const std::vector<PcbOutlineLine>& outlineLines,
                             const std::vector<PcbFootprintModel>& footprints,
                             const std::vector<PcbTrackModel>& tracks,
                             const std::vector<PcbViaModel>& vias,
                             const std::vector<PcbZoneModel>& zones) {
    std::ostringstream out;
    out << "(kicad_pcb (version 20240108) (generator \"aimon-kicad-editor\")\n";
    out << "  (net 0 \"\")\n";
    for (const auto& kv : netByCode) {
        if (kv.first > 0 && !kv.second.empty()) {
            out << "  (net " << kv.first << " \"" << escapeSexp(kv.second) << "\")\n";
        }
    }

    if (!outlineLines.empty()) {
        for (const auto& ln : outlineLines) {
            out << "  (gr_line (start " << fmtMm(ln.x1) << " " << fmtMm(ln.y1)
                << ") (end " << fmtMm(ln.x2) << " " << fmtMm(ln.y2)
                << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        }
    } else if (outlineW > 0.0 && outlineH > 0.0) {
        out << "  (gr_rect (start 0 0) (end " << fmtMm(outlineW) << " " << fmtMm(outlineH)
            << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start 0 0) (end " << fmtMm(outlineW) << " 0) (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start " << fmtMm(outlineW) << " 0) (end " << fmtMm(outlineW) << " " << fmtMm(outlineH)
            << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start " << fmtMm(outlineW) << " " << fmtMm(outlineH) << ") (end 0 " << fmtMm(outlineH)
            << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start 0 " << fmtMm(outlineH) << ") (end 0 0) (layer \"Edge.Cuts\") (width 0.15))\n";
    }

    for (const auto& fp : footprints) {
        out << "  (footprint \"" << escapeSexp(fp.footprint) << "\" (layer \""
            << escapeSexp(fp.layer) << "\") (at " << fmtMm(fp.x) << " "
            << fmtMm(fp.y) << " " << fmtMm(fp.rotation) << ")\n";
        out << "    (property \"Reference\" \"" << escapeSexp(fp.reference) << "\")\n";
        out << "    (property \"Value\" \"" << escapeSexp(fp.value) << "\")\n";
        if (!fp.lcsc.empty()) {
            out << "    (property \"LCSC\" \"" << escapeSexp(fp.lcsc) << "\")\n";
        }
        for (const auto& pad : fp.pads) {
            out << "    (pad \"" << escapeSexp(pad.number) << "\" " << escapeSexp(pad.type)
                << " " << escapeSexp(pad.shape) << " (at " << fmtMm(pad.relX) << " "
                << fmtMm(pad.relY) << ") (size " << fmtMm(pad.sizeX) << " " << fmtMm(pad.sizeY) << ")";
            if (pad.drill > 0.0) {
                out << " (drill " << fmtMm(pad.drill) << ")";
            }
            out << " (layers \"" << escapeSexp(pad.layer.empty() ? fp.layer : pad.layer) << "\")";
            out << " (net " << pad.netCode << " \"" << escapeSexp(pad.netName) << "\"))\n";
        }
        out << "  )\n";
    }

    for (const auto& z : zones) {
        out << "  (zone (net " << z.netCode << ") (net_name \"" << escapeSexp(z.netName)
            << "\") (layer \"" << escapeSexp(z.layer) << "\")\n";
        out << "    (connect_pads (clearance " << fmtMm(z.clearance) << "))\n";
        out << "    (polygon (pts";
        for (const auto& pt : z.points) {
            out << " (xy " << fmtMm(pt.first) << " " << fmtMm(pt.second) << ")";
        }
        out << "))\n";
        out << "  )\n";
    }

    for (const auto& t : tracks) {
        out << "  (segment (start " << fmtMm(t.x1) << " " << fmtMm(t.y1)
            << ") (end " << fmtMm(t.x2) << " " << fmtMm(t.y2)
            << ") (width " << fmtMm(t.width) << ") (layer \"" << escapeSexp(t.layer)
            << "\") (net " << t.netCode << "))\n";
    }

    for (const auto& v : vias) {
        out << "  (via (at " << fmtMm(v.x) << " " << fmtMm(v.y)
            << ") (size " << fmtMm(v.size) << ") (drill " << fmtMm(v.drill)
            << ") (layers \"F.Cu\" \"B.Cu\") (net " << v.netCode << "))\n";
    }

    out << ")\n";
    return out.str();
}

} // anonymous namespace

std::string SexpNode::head() const {
    if (!isList || children.empty() || children[0].isList) return "";
    return children[0].atom;
}

const SexpNode* SexpNode::findChild(const std::string& headAtom) const {
    if (!isList) return nullptr;
    for (const auto& ch : children) {
        if (ch.isList && ch.head() == headAtom) {
            return &ch;
        }
    }
    return nullptr;
}

std::vector<const SexpNode*> SexpNode::findChildren(const std::string& headAtom) const {
    std::vector<const SexpNode*> out;
    if (!isList) return out;
    for (const auto& ch : children) {
        if (ch.isList && ch.head() == headAtom) {
            out.push_back(&ch);
        }
    }
    return out;
}

double KiCadSchematicEditor::snapTo50MilGrid(double valMm) {
    return EasyEdaConverter::snapTo50MilGrid(valMm);
}

bool KiCadSchematicEditor::parseSexp(const std::string& text, SexpNode& outRoot, std::string& outError) {
    outRoot = SexpNode{};
    outError.clear();
    if (text.empty()) {
        outError = "Empty S-expression input";
        return false;
    }

    std::vector<SexpNode*> stack;
    SexpNode container;
    container.isList = true;
    stack.push_back(&container);

    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '(') {
            if (stack.size() > 256) {
                outError = "S-expression nesting depth exceeds limit (256)";
                return false;
            }
            stack.back()->children.push_back(SexpNode{true, "", {}});
            stack.push_back(&stack.back()->children.back());
            ++i;
        } else if (c == ')') {
            if (stack.size() <= 1) {
                outError = "Unmatched closing parenthesis in S-expression";
                return false;
            }
            stack.pop_back();
            ++i;
        } else if (c == '"') {
            ++i;
            std::string str;
            bool closed = false;
            while (i < n) {
                if (text[i] == '\\' && i + 1 < n) {
                    str.push_back(text[i + 1]);
                    i += 2;
                } else if (text[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                } else {
                    str.push_back(text[i]);
                    ++i;
                }
            }
            if (!closed) {
                outError = "Unterminated quoted string in S-expression";
                return false;
            }
            stack.back()->children.push_back(SexpNode{false, str, {}});
        } else {
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) &&
                   text[i] != '(' && text[i] != ')' && text[i] != '"') {
                ++i;
            }
            stack.back()->children.push_back(SexpNode{false, text.substr(start, i - start), {}});
        }
    }

    if (stack.size() != 1) {
        outError = "Unclosed parenthesis in S-expression";
        return false;
    }
    if (container.children.empty()) {
        outError = "No root node in S-expression";
        return false;
    }

    outRoot = std::move(container.children[0]);
    return true;
}

nlohmann::json KiCadSchematicEditor::searchLibraries(const nlohmann::json& arguments) {
    std::string query = toLowerStr(arguments.value("query", ""));
    std::string symDir = arguments.value("symbol_lib_dir", arguments.value("library_path", ""));
    std::string fpDir = arguments.value("footprint_lib_dir", "");

    nlohmann::json symbols = nlohmann::json::array();
    nlohmann::json footprints = nlohmann::json::array();

    auto matchesQuery = [&](const std::string& text) -> bool {
        if (query.empty()) return true;
        return toLowerStr(text).find(query) != std::string::npos;
    };

    // 1. Scan any `.kicad_sym` files in symDir
    if (!symDir.empty() && fs::exists(symDir)) {
        auto scanSymFile = [&](const fs::path& filePath) {
            std::ifstream ifs(filePath);
            if (!ifs) return;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SexpNode root;
            std::string err;
            if (!parseSexp(content, root, err) || root.head() != "kicad_symbol_lib") return;
            std::string libPrefix = filePath.stem().string();
            for (const SexpNode* sym : root.findChildren("symbol")) {
                if (sym->children.size() < 2) continue;
                std::string symName = sym->children[1].atom;
                std::string fullId = (symName.find(':') != std::string::npos) ? symName : (libPrefix + ":" + symName);
                std::string desc;
                for (const SexpNode* prop : sym->findChildren("property")) {
                    if (prop->children.size() >= 3 && prop->children[1].atom == "Description") {
                        desc = prop->children[2].atom;
                    }
                }
                if (!matchesQuery(fullId) && !matchesQuery(desc)) continue;

                nlohmann::json pinArr = nlohmann::json::array();
                auto collectPins = [&](const SexpNode* node) {
                    for (const SexpNode* pin : node->findChildren("pin")) {
                        std::string pType = (pin->children.size() >= 2) ? pin->children[1].atom : "passive";
                        double px = 0.0, py = 0.0, pang = 0.0;
                        if (const SexpNode* at = pin->findChild("at")) {
                            if (at->children.size() >= 3) {
                                px = toDoubleSafe(at->children[1].atom);
                                py = toDoubleSafe(at->children[2].atom);
                            }
                            if (at->children.size() >= 4) pang = toDoubleSafe(at->children[3].atom);
                        }
                        std::string pName, pNum;
                        if (const SexpNode* nm = pin->findChild("name")) {
                            if (nm->children.size() >= 2) pName = nm->children[1].atom;
                        }
                        if (const SexpNode* nb = pin->findChild("number")) {
                            if (nb->children.size() >= 2) pNum = nb->children[1].atom;
                        }
                        pinArr.push_back({
                            {"number", pNum},
                            {"name", pName},
                            {"type", pType},
                            {"x", px},
                            {"y", py},
                            {"angle", pang}
                        });
                    }
                };
                collectPins(sym);
                for (const SexpNode* subSym : sym->findChildren("symbol")) {
                    collectPins(subSym);
                }

                symbols.push_back({
                    {"lib_id", fullId},
                    {"name", symName},
                    {"description", desc},
                    {"pins", pinArr}
                });
            }
        };

        if (fs::is_regular_file(symDir)) {
            scanSymFile(symDir);
        } else if (fs::is_directory(symDir)) {
            for (const auto& entry : fs::recursive_directory_iterator(symDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".kicad_sym") {
                    scanSymFile(entry.path());
                }
            }
        }
    }

    // 2. Scan any `.kicad_mod` files in fpDir (or symDir if it also contains footprints)
    std::vector<std::string> fpDirsToScan;
    if (!fpDir.empty()) fpDirsToScan.push_back(fpDir);
    if (!symDir.empty() && symDir != fpDir) fpDirsToScan.push_back(symDir);

    for (const auto& dirStr : fpDirsToScan) {
        if (!fs::exists(dirStr)) continue;
        auto scanModFile = [&](const fs::path& modPath) {
            std::ifstream ifs(modPath);
            if (!ifs) return;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SexpNode root;
            std::string err;
            if (!parseSexp(content, root, err) || (root.head() != "footprint" && root.head() != "module")) return;
            std::string fpName = (root.children.size() >= 2) ? root.children[1].atom : modPath.stem().string();
            std::string libName = modPath.parent_path().stem().string();
            std::string fullId = (!libName.empty() && fpName.find(':') == std::string::npos)
                                 ? (libName + ":" + fpName) : fpName;
            std::string desc;
            if (const SexpNode* d = root.findChild("descr")) {
                if (d->children.size() >= 2) desc = d->children[1].atom;
            }
            if (!matchesQuery(fullId) && !matchesQuery(desc)) return;

            nlohmann::json padArr = nlohmann::json::array();
            for (const SexpNode* pad : root.findChildren("pad")) {
                if (pad->children.size() < 4) continue;
                double px = 0.0, py = 0.0, sx = 0.8, sy = 0.8;
                if (const SexpNode* at = pad->findChild("at")) {
                    if (at->children.size() >= 3) {
                        px = toDoubleSafe(at->children[1].atom);
                        py = toDoubleSafe(at->children[2].atom);
                    }
                }
                if (const SexpNode* sz = pad->findChild("size")) {
                    if (sz->children.size() >= 3) {
                        sx = toDoubleSafe(sz->children[1].atom, 0.8);
                        sy = toDoubleSafe(sz->children[2].atom, 0.8);
                    }
                }
                padArr.push_back({
                    {"number", pad->children[1].atom},
                    {"type", pad->children[2].atom},
                    {"shape", pad->children[3].atom},
                    {"x", px},
                    {"y", py},
                    {"size_x", sx},
                    {"size_y", sy}
                });
            }
            footprints.push_back({
                {"lib_id", fullId},
                {"name", fpName},
                {"description", desc},
                {"pads", padArr}
            });
        };

        if (fs::is_regular_file(dirStr) && fs::path(dirStr).extension() == ".kicad_mod") {
            scanModFile(dirStr);
        } else if (fs::is_directory(dirStr)) {
            for (const auto& entry : fs::recursive_directory_iterator(dirStr)) {
                if (entry.is_regular_file() && entry.path().extension() == ".kicad_mod") {
                    scanModFile(entry.path());
                }
            }
        }
    }

    // 3. Built-in standard catalog fallback / supplement
    struct BuiltinSym {
        const char* libId;
        const char* name;
        const char* desc;
        std::vector<SchPinModel> pins;
    };
    const std::vector<BuiltinSym> kBuiltins = {
        {"Device:R", "R", "Resistor (50-mil grid standard)", {{"1", "~", "passive", -2.54, 0.0, ""}, {"2", "~", "passive", 2.54, 0.0, ""}}},
        {"Device:C", "C", "Unpolarized capacitor", {{"1", "~", "passive", -2.54, 0.0, ""}, {"2", "~", "passive", 2.54, 0.0, ""}}},
        {"Device:LED", "LED", "Light emitting diode", {{"1", "K", "passive", -2.54, 0.0, ""}, {"2", "A", "passive", 2.54, 0.0, ""}}},
        {"Regulator_Linear:AMS1117-3.3", "AMS1117-3.3", "1A Low Dropout regulator 3.3V SOT-223", {
            {"1", "GND", "power_in", -2.54, 2.54, ""},
            {"2", "VO", "power_out", 2.54, 0.0, ""},
            {"3", "VI", "power_in", -2.54, -2.54, ""}
        }},
        {"MCU_Espressif:ESP32-S3-WROOM-1", "ESP32-S3-WROOM-1", "ESP32-S3 Wi-Fi/BLE MCU Module", {
            {"1", "GND", "power_in", -5.08, 2.54, ""},
            {"2", "3V3", "power_in", -5.08, -2.54, ""},
            {"3", "EN", "input", -5.08, 0.0, ""},
            {"4", "IO4", "bidirectional", 5.08, -2.54, ""},
            {"5", "IO5", "bidirectional", 5.08, 0.0, ""}
        }},
        {"Connector_Generic:Conn_01x04", "Conn_01x04", "Generic 1x04 header connector", {
            {"1", "Pin_1", "passive", -2.54, -3.81, ""},
            {"2", "Pin_2", "passive", -2.54, -1.27, ""},
            {"3", "Pin_3", "passive", -2.54, 1.27, ""},
            {"4", "Pin_4", "passive", -2.54, 3.81, ""}
        }}
    };
    for (const auto& b : kBuiltins) {
        if (matchesQuery(b.libId) || matchesQuery(b.desc)) {
            nlohmann::json pinArr = nlohmann::json::array();
            for (const auto& p : b.pins) {
                pinArr.push_back({
                    {"number", p.number},
                    {"name", p.name},
                    {"type", p.type},
                    {"x", p.x},
                    {"y", p.y},
                    {"angle", 0.0}
                });
            }
            symbols.push_back({
                {"lib_id", b.libId},
                {"name", b.name},
                {"description", b.desc},
                {"pins", pinArr}
            });
        }
    }

    struct BuiltinFp {
        const char* libId;
        const char* name;
        const char* desc;
    };
    const std::vector<BuiltinFp> kBuiltinFps = {
        {"Resistor_SMD:R_0603_1608Metric", "R_0603_1608Metric", "SMD Resistor 0603 (1608 Metric)"},
        {"Capacitor_SMD:C_0603_1608Metric", "C_0603_1608Metric", "SMD Capacitor 0603 (1608 Metric)"},
        {"Package_TO_SOT_SMD:SOT-223-3_TabPin2", "SOT-223-3_TabPin2", "SOT-223 3-pin with tab on pin 2"},
        {"Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "SOIC-8_3.9x4.9mm_P1.27mm", "SOIC-8 3.9x4.9mm pitch 1.27mm"},
        {"Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", "PinHeader_1x04_P2.54mm_Vertical", "1x04 2.54mm pitch vertical header"}
    };
    for (const auto& bf : kBuiltinFps) {
        if (matchesQuery(bf.libId) || matchesQuery(bf.desc)) {
            auto pads = defaultPadsForFootprint(bf.libId, "F.Cu");
            nlohmann::json padArr = nlohmann::json::array();
            for (const auto& p : pads) {
                padArr.push_back({
                    {"number", p.number},
                    {"type", p.type},
                    {"shape", p.shape},
                    {"x", p.relX},
                    {"y", p.relY},
                    {"size_x", p.sizeX},
                    {"size_y", p.sizeY}
                });
            }
            footprints.push_back({
                {"lib_id", bf.libId},
                {"name", bf.name},
                {"description", bf.desc},
                {"pads", padArr}
            });
        }
    }

    return {
        {"ok", true},
        {"query", query},
        {"symbols_count", symbols.size()},
        {"footprints_count", footprints.size()},
        {"symbols", symbols},
        {"footprints", footprints}
    };
}

nlohmann::json KiCadSchematicEditor::inspectFile(const nlohmann::json& arguments) {
    std::string pathStr = arguments.value("project_path",
                          arguments.value("file_path",
                          arguments.value("schematic_path",
                          arguments.value("pcb_path",
                          arguments.value("path", "")))));

    std::string inlineSexp = arguments.value("sexp", "");

    std::vector<SchCompModel> comps;
    std::vector<SchWireModel> wires;
    std::vector<SchLabelModel> labels;

    std::map<int, std::string> netByCode;
    std::map<std::string, int> codeByNet;
    std::vector<PcbOutlineLine> outline;
    double outlineW = 0.0, outlineH = 0.0;
    std::vector<PcbFootprintModel> footprints;
    std::vector<PcbTrackModel> tracks;
    std::vector<PcbViaModel> vias;
    std::vector<PcbZoneModel> zones;
    std::vector<PcbHoleModel> holes;

    auto parseAndExtract = [&](const std::string& content, std::string& outErr) -> bool {
        SexpNode root;
        if (!parseSexp(content, root, outErr)) return false;
        if (root.head() == "kicad_sch") {
            extractSchematicFromSexp(root, comps, wires, labels);
            return true;
        }
        if (root.head() == "kicad_pcb") {
            extractPcbFromSexp(root, netByCode, codeByNet, outline, outlineW, outlineH,
                               footprints, tracks, vias, zones, holes);
            return true;
        }
        outErr = "Unrecognized KiCad root node '" + root.head() + "'";
        return false;
    };

    if (!inlineSexp.empty()) {
        std::string err;
        if (!parseAndExtract(inlineSexp, err)) {
            return {{"ok", false}, {"error", err}};
        }
    } else if (!pathStr.empty()) {
        fs::path p(pathStr);
        std::vector<fs::path> filesToRead;
        if (p.extension() == ".kicad_pro") {
            fs::path sch = p;
            sch.replace_extension(".kicad_sch");
            fs::path pcb = p;
            pcb.replace_extension(".kicad_pcb");
            if (fs::exists(sch)) filesToRead.push_back(sch);
            if (fs::exists(pcb)) filesToRead.push_back(pcb);
        } else if (fs::is_directory(p)) {
            for (const auto& entry : fs::directory_iterator(p)) {
                if (entry.path().extension() == ".kicad_sch" || entry.path().extension() == ".kicad_pcb") {
                    filesToRead.push_back(entry.path());
                }
            }
        } else {
            filesToRead.push_back(p);
            if (p.extension() == ".kicad_sch") {
                fs::path sibPcb = p;
                sibPcb.replace_extension(".kicad_pcb");
                if (fs::exists(sibPcb)) filesToRead.push_back(sibPcb);
            }
        }

        if (filesToRead.empty() || !fs::exists(filesToRead[0])) {
            return {{"ok", false}, {"error", "KiCad file not found: " + pathStr}};
        }

        for (const auto& fp : filesToRead) {
            std::ifstream ifs(fp);
            if (!ifs) continue;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            std::string err;
            if (!parseAndExtract(content, err)) {
                return {{"ok", false}, {"error", err}};
            }
        }
    } else {
        return {{"ok", false}, {"error", "kicad_inspect_project requires 'project_path', 'file_path', or 'sexp'"}};
    }

    nlohmann::json compArr = nlohmann::json::array();
    for (const auto& c : comps) {
        nlohmann::json pinArr = nlohmann::json::array();
        for (const auto& p : c.pins) {
            pinArr.push_back({
                {"number", p.number},
                {"name", p.name},
                {"type", p.type},
                {"x", p.x},
                {"y", p.y},
                {"net", p.net}
            });
        }
        compArr.push_back({
            {"reference", c.reference},
            {"value", c.value},
            {"lib_id", c.libId},
            {"footprint", c.footprint},
            {"lcsc", c.lcsc},
            {"x", c.x},
            {"y", c.y},
            {"rotation", c.rotation},
            {"pins", pinArr}
        });
    }

    nlohmann::json wireArr = nlohmann::json::array();
    for (const auto& w : wires) {
        wireArr.push_back({{"x1", w.x1}, {"y1", w.y1}, {"x2", w.x2}, {"y2", w.y2}});
    }

    nlohmann::json labelArr = nlohmann::json::array();
    for (const auto& l : labels) {
        labelArr.push_back({
            {"name", l.name},
            {"x", l.x},
            {"y", l.y},
            {"angle", l.angle},
            {"is_power", l.isPower}
        });
    }

    nlohmann::json netsArr = buildSchematicNetlist(comps, wires, labels);
    if (netsArr.empty() && !netByCode.empty()) {
        for (const auto& kv : netByCode) {
            if (kv.first > 0 && !kv.second.empty()) {
                netsArr.push_back({{"code", kv.first}, {"name", kv.second}});
            }
        }
    }

    nlohmann::json fpArr = nlohmann::json::array();
    for (const auto& fp : footprints) {
        nlohmann::json padArr = nlohmann::json::array();
        for (const auto& pad : fp.pads) {
            padArr.push_back({
                {"number", pad.number},
                {"type", pad.type},
                {"shape", pad.shape},
                {"x", fp.x + pad.relX},
                {"y", fp.y + pad.relY},
                {"rel_x", pad.relX},
                {"rel_y", pad.relY},
                {"size_x", pad.sizeX},
                {"size_y", pad.sizeY},
                {"drill", pad.drill},
                {"layer", pad.layer},
                {"net_code", pad.netCode},
                {"net", pad.netName}
            });
        }
        fpArr.push_back({
            {"reference", fp.reference},
            {"value", fp.value},
            {"footprint", fp.footprint},
            {"lcsc", fp.lcsc},
            {"x", fp.x},
            {"y", fp.y},
            {"rotation", fp.rotation},
            {"layer", fp.layer},
            {"pads", padArr}
        });
    }

    nlohmann::json trkArr = nlohmann::json::array();
    for (const auto& t : tracks) {
        trkArr.push_back({
            {"x1", t.x1},
            {"y1", t.y1},
            {"x2", t.x2},
            {"y2", t.y2},
            {"width", t.width},
            {"layer", t.layer},
            {"net_code", t.netCode},
            {"net", t.netName}
        });
    }

    nlohmann::json viaArr = nlohmann::json::array();
    for (const auto& v : vias) {
        viaArr.push_back({
            {"x", v.x},
            {"y", v.y},
            {"size", v.size},
            {"drill", v.drill},
            {"net_code", v.netCode},
            {"net", v.netName}
        });
    }

    nlohmann::json zoneArr = nlohmann::json::array();
    for (const auto& z : zones) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& pt : z.points) {
            pts.push_back({{"x", pt.first}, {"y", pt.second}});
        }
        zoneArr.push_back({
            {"net", z.netName},
            {"net_code", z.netCode},
            {"layer", z.layer},
            {"points", pts}
        });
    }

    nlohmann::json holeArr = nlohmann::json::array();
    for (const auto& h : holes) {
        holeArr.push_back({{"x", h.x}, {"y", h.y}, {"drill", h.drill}, {"pad", h.pad}});
    }

    nlohmann::json outlineLinesArr = nlohmann::json::array();
    for (const auto& ln : outline) {
        outlineLinesArr.push_back({
            {"x1", ln.x1},
            {"y1", ln.y1},
            {"x2", ln.x2},
            {"y2", ln.y2}
        });
    }

    std::vector<std::string> unrouted = computeUnroutedNets(footprints, tracks, zones);

    return {
        {"ok", true},
        {"path", pathStr},
        {"components_count", compArr.size()},
        {"wires_count", wireArr.size()},
        {"labels_count", labelArr.size()},
        {"nets_count", netsArr.size()},
        {"footprints_count", fpArr.size()},
        {"tracks_count", trkArr.size()},
        {"vias_count", viaArr.size()},
        {"zones_count", zoneArr.size()},
        {"mounting_holes_count", holeArr.size()},
        {"unrouted_nets_count", unrouted.size()},
        {"components", compArr},
        {"wires", wireArr},
        {"labels", labelArr},
        {"nets", netsArr},
        {"board_outline", {
            {"width_mm", outlineW},
            {"height_mm", outlineH},
            {"segments", outline.size()},
            {"lines", outlineLinesArr}
        }},
        {"footprints", fpArr},
        {"tracks", trkArr},
        {"vias", viaArr},
        {"zones", zoneArr},
        {"mounting_holes", holeArr},
        {"unrouted_nets", unrouted}
    };
}

nlohmann::json KiCadSchematicEditor::applySchematicEdits(const nlohmann::json& arguments) {
    std::string schPath = arguments.value("schematic_path",
                          arguments.value("file_path",
                          arguments.value("path", "")));

    std::vector<SchCompModel> comps;
    std::vector<SchWireModel> wires;
    std::vector<SchLabelModel> labels;

    if (!schPath.empty() && fs::exists(schPath)) {
        std::ifstream ifs(schPath);
        if (ifs) {
            std::string existing((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SexpNode root;
            std::string err;
            if (parseSexp(existing, root, err) && root.head() == "kicad_sch") {
                extractSchematicFromSexp(root, comps, wires, labels);
            }
        }
    }

    // 1. Remove references if requested
    if (arguments.contains("remove_references") && arguments["remove_references"].is_array()) {
        std::set<std::string> toRemove;
        for (const auto& r : arguments["remove_references"]) {
            if (r.is_string()) toRemove.insert(r.get<std::string>());
        }
        comps.erase(std::remove_if(comps.begin(), comps.end(), [&](const SchCompModel& c) {
            return toRemove.count(c.reference) > 0;
        }), comps.end());
    }

    // 2. Add or update components (`add_components` or `symbols` from `eda_calc_schematic_layout`)
    auto processCompArray = [&](const nlohmann::json& arr) {
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            SchCompModel c;
            c.reference = item.value("reference", item.value("ref", "U1"));
            c.value = item.value("value", c.reference);
            c.libId = item.value("lib_id", item.value("symbol", "Device:R"));
            c.footprint = item.value("footprint", "Resistor_SMD:R_0603_1608Metric");
            c.lcsc = item.value("lcsc", "");
            c.x = snapTo50MilGrid(item.value("x", 25.4));
            c.y = snapTo50MilGrid(item.value("y", 25.4));
            c.rotation = item.value("rotation", item.value("angle", 0.0));

            if (item.contains("pins") && item["pins"].is_array()) {
                for (const auto& pObj : item["pins"]) {
                    if (!pObj.is_object()) continue;
                    SchPinModel p;
                    p.number = pObj.value("number", "1");
                    p.name = pObj.value("name", p.number);
                    p.type = pObj.value("type", "passive");
                    if (pObj.contains("x") && pObj.contains("y")) {
                        p.x = snapTo50MilGrid(pObj.value("x", c.x));
                        p.y = snapTo50MilGrid(pObj.value("y", c.y));
                    } else {
                        p.x = snapTo50MilGrid(c.x + pObj.value("dx", -2.54));
                        p.y = snapTo50MilGrid(c.y + pObj.value("dy", 0.0));
                    }
                    p.net = pObj.value("net", "");
                    c.pins.push_back(p);
                }
            }
            if (c.pins.empty()) {
                c.pins = defaultPinsForSymbol(c.libId, c.x, c.y);
            }

            auto existingIt = std::find_if(comps.begin(), comps.end(), [&](const SchCompModel& ec) {
                return ec.reference == c.reference;
            });
            if (existingIt != comps.end()) {
                *existingIt = c;
            } else {
                comps.push_back(c);
            }
        }
    };

    if (arguments.contains("add_components") && arguments["add_components"].is_array()) {
        processCompArray(arguments["add_components"]);
    }
    if (arguments.contains("components") && arguments["components"].is_array()) {
        processCompArray(arguments["components"]);
    }
    if (arguments.contains("symbols") && arguments["symbols"].is_array()) {
        processCompArray(arguments["symbols"]);
    }

    // 3. Apply property updates
    if (arguments.contains("set_properties") && arguments["set_properties"].is_array()) {
        for (const auto& prop : arguments["set_properties"]) {
            if (!prop.is_object()) continue;
            std::string ref = prop.value("reference", "");
            std::string key = prop.value("name", prop.value("key", ""));
            std::string val = prop.value("value", "");
            for (auto& c : comps) {
                if (c.reference == ref) {
                    if (key == "Value") c.value = val;
                    else if (key == "Footprint") c.footprint = val;
                    else if (key == "LCSC") c.lcsc = val;
                    else if (!key.empty()) c.extraProps[key] = val;
                }
            }
        }
    }

    // 4. Add power ports and net labels
    auto processLabels = [&](const nlohmann::json& arr, bool isPowerDefault) {
        for (const auto& lObj : arr) {
            if (!lObj.is_object()) continue;
            SchLabelModel l;
            l.name = lObj.value("name", lObj.value("net", ""));
            l.x = snapTo50MilGrid(lObj.value("x", 0.0));
            l.y = snapTo50MilGrid(lObj.value("y", 0.0));
            l.angle = lObj.value("angle", 0.0);
            l.isPower = lObj.value("is_power", isPowerDefault);
            if (!l.name.empty()) {
                labels.push_back(l);
            }
        }
    };
    if (arguments.contains("add_power_ports") && arguments["add_power_ports"].is_array()) {
        processLabels(arguments["add_power_ports"], true);
    }
    if (arguments.contains("power_ports") && arguments["power_ports"].is_array()) {
        processLabels(arguments["power_ports"], true);
    }
    if (arguments.contains("add_labels") && arguments["add_labels"].is_array()) {
        processLabels(arguments["add_labels"], false);
    }
    if (arguments.contains("labels") && arguments["labels"].is_array()) {
        processLabels(arguments["labels"], false);
    }

    // 5. Add wires (`add_wires` or `wires` from `eda_calc_schematic_layout`)
    auto processWires = [&](const nlohmann::json& arr) {
        for (const auto& wObj : arr) {
            if (!wObj.is_object()) continue;
            SchWireModel w;
            w.x1 = snapTo50MilGrid(wObj.value("x1", 0.0));
            w.y1 = snapTo50MilGrid(wObj.value("y1", 0.0));
            w.x2 = snapTo50MilGrid(wObj.value("x2", 0.0));
            w.y2 = snapTo50MilGrid(wObj.value("y2", 0.0));
            w.net = wObj.value("net", "");
            wires.push_back(w);
        }
    };
    if (arguments.contains("add_wires") && arguments["add_wires"].is_array()) {
        processWires(arguments["add_wires"]);
    }
    if (arguments.contains("wires") && arguments["wires"].is_array()) {
        processWires(arguments["wires"]);
    }

    // 6. Auto-route orthogonal 50-mil wires for `connect_pins`
    if (arguments.contains("connect_pins") && arguments["connect_pins"].is_array()) {
        auto resolvePinCoord = [&](const std::string& spec, double& outX, double& outY) -> bool {
            size_t dot = spec.find('.');
            if (dot == std::string::npos) return false;
            std::string ref = spec.substr(0, dot);
            std::string pinKey = spec.substr(dot + 1);
            for (const auto& c : comps) {
                if (c.reference == ref) {
                    for (const auto& p : c.pins) {
                        if (p.number == pinKey || p.name == pinKey) {
                            outX = snapTo50MilGrid(p.x);
                            outY = snapTo50MilGrid(p.y);
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        for (const auto& cpObj : arguments["connect_pins"]) {
            if (!cpObj.is_object()) continue;
            std::string fromSpec = cpObj.value("from", cpObj.value("from_pin", ""));
            std::string toSpec = cpObj.value("to", cpObj.value("to_pin", ""));
            std::string netName = cpObj.value("net", "");
            double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
            if (!resolvePinCoord(fromSpec, x1, y1) || !resolvePinCoord(toSpec, x2, y2)) {
                continue;
            }
            if (std::fabs(x1 - x2) < 1e-4 || std::fabs(y1 - y2) < 1e-4) {
                SchWireModel w;
                w.x1 = x1;
                w.y1 = y1;
                w.x2 = x2;
                w.y2 = y2;
                w.net = netName;
                wires.push_back(w);
            } else {
                double midX = x2;
                double midY = y1;
                SchWireModel w1;
                w1.x1 = x1;
                w1.y1 = y1;
                w1.x2 = midX;
                w1.y2 = midY;
                w1.net = netName;
                wires.push_back(w1);

                SchWireModel w2;
                w2.x1 = midX;
                w2.y1 = midY;
                w2.x2 = x2;
                w2.y2 = y2;
                w2.net = "";
                wires.push_back(w2);
            }
        }
    }

    std::string sexp = serializeSchematicSexp(comps, wires, labels);
    if (!schPath.empty()) {
        fs::path p(schPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }
        std::ofstream ofs(schPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return {{"ok", false}, {"error", "Failed to write schematic file: " + schPath}};
        }
        ofs << sexp;
        ofs.close();
    }

    nlohmann::json summary = inspectFile({{"sexp", sexp}});
    summary["schematic_path"] = schPath;
    summary["sexp"] = sexp;
    summary["grid_snapped_50mil"] = true;
    return summary;
}

nlohmann::json KiCadSchematicEditor::syncSchematicToPcb(const nlohmann::json& arguments) {
    std::string schPath = arguments.value("schematic_path", arguments.value("input_path", ""));
    std::string pcbPath = arguments.value("pcb_path", arguments.value("output_path", ""));
    std::string schSexp = arguments.value("schematic_sexp", "");

    if (pcbPath.empty() && !schPath.empty()) {
        fs::path p(schPath);
        p.replace_extension(".kicad_pcb");
        pcbPath = p.string();
    }

    if (schSexp.empty() && schPath.empty() &&
        (arguments.contains("components") || arguments.contains("add_components") || arguments.contains("symbols"))) {
        nlohmann::json inlineSch = applySchematicEdits(arguments);
        if (inlineSch.value("ok", false)) {
            schSexp = inlineSch.value("sexp", "");
        }
    }

    nlohmann::json schInspect;
    if (schSexp.empty() && !schPath.empty()) {
        std::ifstream ifs(schPath);
        if (!ifs) {
            return {{"ok", false}, {"error", "KiCad file not found: " + schPath}};
        }
        schSexp.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    if (!schSexp.empty()) {
        schInspect = inspectFile({{"sexp", schSexp}});
    } else {
        return {{"ok", false}, {"error", "kicad_sync_sch_to_pcb requires 'schematic_path', 'schematic_sexp', or 'components'"}};
    }

    if (!schInspect.value("ok", false)) {
        return schInspect;
    }

    // Load existing PCB if present so placements, outline, tracks, vias, and zones are preserved
    std::map<int, std::string> netByCode;
    std::map<std::string, int> codeByNet;
    std::vector<PcbOutlineLine> outline;
    double outlineW = 60.0, outlineH = 45.0;
    if (arguments.contains("board_outline") && arguments["board_outline"].is_object()) {
        outlineW = arguments["board_outline"].value("width_mm", outlineW);
        outlineH = arguments["board_outline"].value("height_mm", outlineH);
    }
    std::vector<PcbFootprintModel> footprints;
    std::vector<PcbTrackModel> tracks;
    std::vector<PcbViaModel> vias;
    std::vector<PcbZoneModel> zones;
    std::vector<PcbHoleModel> holes;

    if (!pcbPath.empty() && fs::exists(pcbPath)) {
        std::ifstream ifs(pcbPath);
        if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SexpNode root;
            std::string err;
            if (parseSexp(content, root, err) && root.head() == "kicad_pcb") {
                extractPcbFromSexp(root, netByCode, codeByNet, outline, outlineW, outlineH,
                                   footprints, tracks, vias, zones, holes);
            }
        }
    }

    netByCode[0] = "";
    codeByNet[""] = 0;
    int nextNetCode = 1;
    for (const auto& kv : netByCode) {
        nextNetCode = std::max(nextNetCode, kv.first + 1);
    }

    auto ensureNetCode = [&](const std::string& netName) -> int {
        if (netName.empty()) return 0;
        auto it = codeByNet.find(netName);
        if (it != codeByNet.end()) return it->second;
        int code = nextNetCode++;
        codeByNet[netName] = code;
        netByCode[code] = netName;
        return code;
    };

    // Map (ref, pin) -> netName from schematic netlist
    std::map<std::pair<std::string, std::string>, std::string> pinNetMap;
    std::set<std::string> validSchNets;
    if (schInspect.contains("nets") && schInspect["nets"].is_array()) {
        for (const auto& netObj : schInspect["nets"]) {
            std::string netName = netObj.value("name", "");
            if (!netName.empty()) {
                validSchNets.insert(netName);
            }
            ensureNetCode(netName);
            if (netObj.contains("nodes") && netObj["nodes"].is_array()) {
                for (const auto& node : netObj["nodes"]) {
                    std::string r = node.value("ref", "");
                    std::string p = node.value("pin", "");
                    if (!r.empty() && !p.empty()) {
                        pinNetMap[{r, p}] = netName;
                    }
                }
            }
        }
    }

    const bool dryRun = arguments.value("dry_run", false);
    const std::vector<PcbFootprintModel> existingFootprints = footprints;
    nlohmann::json componentsToAdd = nlohmann::json::array();
    nlohmann::json componentsToRemove = nlohmann::json::array();
    nlohmann::json footprintMismatches = nlohmann::json::array();
    nlohmann::json unannotatedReferences = nlohmann::json::array();
    nlohmann::json missingFootprints = nlohmann::json::array();
    nlohmann::json padNetDeltas = nlohmann::json::array();
    nlohmann::json orphanedTracks = nlohmann::json::array();
    std::set<std::string> schRefsSet;

    int syncedCount = 0;
    int autoGridIdx = 0;
    if (schInspect.contains("components") && schInspect["components"].is_array()) {
        for (const auto& comp : schInspect["components"]) {
            std::string ref = comp.value("reference", "");
            if (ref.empty() || ref[0] == '#') continue;
            schRefsSet.insert(ref);
            std::string val = comp.value("value", ref);
            std::string rawFpName = comp.value("footprint", "");
            if (ref.find('?') != std::string::npos) {
                unannotatedReferences.push_back(ref);
            }
            if (rawFpName.empty()) {
                missingFootprints.push_back(ref);
            }
            std::string fpName = rawFpName.empty() ? "Resistor_SMD:R_0603_1608Metric" : rawFpName;
            std::string lcsc = comp.value("lcsc", "");

            auto fpIt = std::find_if(footprints.begin(), footprints.end(), [&](const PcbFootprintModel& f) {
                return f.reference == ref;
            });

            if (fpIt == footprints.end()) {
                if (ref.find('?') == std::string::npos && !rawFpName.empty()) {
                    componentsToAdd.push_back({
                        {"reference", ref},
                        {"value", val},
                        {"footprint", rawFpName}
                    });
                }
            } else {
                if (!rawFpName.empty() && !fpIt->footprint.empty() && rawFpName != fpIt->footprint) {
                    footprintMismatches.push_back({
                        {"reference", ref},
                        {"schematic_footprint", rawFpName},
                        {"pcb_footprint", fpIt->footprint}
                    });
                }
            }

            PcbFootprintModel fpModel;
            if (fpIt != footprints.end()) {
                fpModel = *fpIt;
                fpModel.value = val;
                if (!fpName.empty()) fpModel.footprint = fpName;
                if (!lcsc.empty()) fpModel.lcsc = lcsc;
            } else {
                fpModel.reference = ref;
                fpModel.value = val;
                fpModel.footprint = fpName;
                fpModel.lcsc = lcsc;
                fpModel.x = 10.0 + (autoGridIdx % 5) * 10.0;
                fpModel.y = 10.0 + (autoGridIdx / 5) * 10.0;
                fpModel.rotation = 0.0;
                fpModel.layer = "F.Cu";
                fpModel.pads = defaultPadsForFootprint(fpName, fpModel.layer);
                ++autoGridIdx;
            }

            // Ensure pads cover all schematic pins
            if (comp.contains("pins") && comp["pins"].is_array()) {
                for (const auto& sp : comp["pins"]) {
                    std::string pNum = sp.value("number", "");
                    std::string explicitNet = sp.value("net", "");
                    if (!explicitNet.empty()) {
                        pinNetMap[{ref, pNum}] = explicitNet;
                        validSchNets.insert(explicitNet);
                        ensureNetCode(explicitNet);
                    }
                    auto padIt = std::find_if(fpModel.pads.begin(), fpModel.pads.end(), [&](const PcbPadModel& pd) {
                        return pd.number == pNum;
                    });
                    if (padIt == fpModel.pads.end() && !pNum.empty()) {
                        PcbPadModel newPad;
                        newPad.number = pNum;
                        newPad.relX = static_cast<double>(fpModel.pads.size()) * 1.27;
                        newPad.relY = 0.0;
                        newPad.layer = fpModel.layer;
                        fpModel.pads.push_back(newPad);
                    }
                }
            }

            for (auto& pad : fpModel.pads) {
                auto nmIt = pinNetMap.find({ref, pad.number});
                std::string newNet = (nmIt != pinNetMap.end()) ? nmIt->second : "";
                if (fpIt != footprints.end() && pad.netName != newNet &&
                    (!pad.netName.empty() || !newNet.empty())) {
                    padNetDeltas.push_back({
                        {"reference", ref},
                        {"pad", pad.number},
                        {"old_net", pad.netName},
                        {"new_net", newNet}
                    });
                }
                if (nmIt != pinNetMap.end()) {
                    pad.netName = nmIt->second;
                    pad.netCode = ensureNetCode(pad.netName);
                }
            }

            if (fpIt != footprints.end()) {
                *fpIt = fpModel;
            } else {
                footprints.push_back(fpModel);
            }
            ++syncedCount;
        }
    }

    for (const auto& oldFp : existingFootprints) {
        if (!oldFp.reference.empty() && schRefsSet.find(oldFp.reference) == schRefsSet.end()) {
            componentsToRemove.push_back({
                {"reference", oldFp.reference},
                {"value", oldFp.value},
                {"footprint", oldFp.footprint}
            });
        }
    }

    for (const auto& t : tracks) {
        if (!t.netName.empty() && validSchNets.find(t.netName) == validSchNets.end()) {
            orphanedTracks.push_back({
                {"layer", t.layer},
                {"net", t.netName},
                {"x1", t.x1},
                {"y1", t.y1},
                {"x2", t.x2},
                {"y2", t.y2}
            });
        }
    }

    if (dryRun) {
        const bool readyToSync = unannotatedReferences.empty() && missingFootprints.empty();
        return {
            {"ok", true},
            {"dry_run", true},
            {"synced", false},
            {"ready_to_sync", readyToSync},
            {"schematic_path", schPath},
            {"pcb_path", pcbPath},
            {"components_to_add", componentsToAdd},
            {"components_to_remove", componentsToRemove},
            {"footprint_mismatches", footprintMismatches},
            {"unannotated_references", unannotatedReferences},
            {"missing_footprints", missingFootprints},
            {"pad_net_deltas", padNetDeltas},
            {"orphaned_tracks", orphanedTracks}
        };
    }

    std::string pcbSexp = serializePcbSexp(netByCode, outlineW, outlineH, outline,
                                           footprints, tracks, vias, zones);
    if (!pcbPath.empty()) {
        fs::path p(pcbPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }
        std::ofstream ofs(pcbPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return {{"ok", false}, {"error", "Failed to write PCB file: " + pcbPath}};
        }
        ofs << pcbSexp;
        ofs.close();
    }

    nlohmann::json pcbSummary = inspectFile({{"sexp", pcbSexp}});
    std::vector<std::string> unrouted = computeUnroutedNets(footprints, tracks, zones);
    return {
        {"ok", true},
        {"schematic_path", schPath},
        {"pcb_path", pcbPath},
        {"synced_components", syncedCount},
        {"footprints_count", footprints.size()},
        {"footprints", pcbSummary.value("footprints", nlohmann::json::array())},
        {"board_outline", pcbSummary.value("board_outline", nlohmann::json::object())},
        {"tracks", pcbSummary.value("tracks", nlohmann::json::array())},
        {"vias", pcbSummary.value("vias", nlohmann::json::array())},
        {"zones", pcbSummary.value("zones", nlohmann::json::array())},
        {"nets_count", schInspect["nets"].size()},
        {"nets", schInspect["nets"]},
        {"unrouted_nets_count", unrouted.size()},
        {"unrouted_nets", unrouted},
        {"sexp", pcbSexp}
    };
}

nlohmann::json KiCadSchematicEditor::applyPcbEdits(const nlohmann::json& arguments) {
    std::string pcbPath = arguments.value("pcb_path",
                          arguments.value("file_path",
                          arguments.value("path", "")));

    std::map<int, std::string> netByCode;
    std::map<std::string, int> codeByNet;
    std::vector<PcbOutlineLine> outline;
    double outlineW = 50.0, outlineH = 40.0;
    std::vector<PcbFootprintModel> footprints;
    std::vector<PcbTrackModel> tracks;
    std::vector<PcbViaModel> vias;
    std::vector<PcbZoneModel> zones;
    std::vector<PcbHoleModel> holes;

    if (!pcbPath.empty() && fs::exists(pcbPath)) {
        std::ifstream ifs(pcbPath);
        if (ifs) {
            std::string existing((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            SexpNode root;
            std::string err;
            if (parseSexp(existing, root, err) && root.head() == "kicad_pcb") {
                extractPcbFromSexp(root, netByCode, codeByNet, outline, outlineW, outlineH,
                                   footprints, tracks, vias, zones, holes);
            }
        }
    }

    netByCode[0] = "";
    codeByNet[""] = 0;
    int nextNetCode = 1;
    for (const auto& kv : netByCode) {
        nextNetCode = std::max(nextNetCode, kv.first + 1);
    }

    auto ensureNetCode = [&](const std::string& netName) -> int {
        if (netName.empty()) return 0;
        auto it = codeByNet.find(netName);
        if (it != codeByNet.end()) return it->second;
        int code = nextNetCode++;
        codeByNet[netName] = code;
        netByCode[code] = netName;
        return code;
    };

    // 1. Board outline (`board_outline`)
    if (arguments.contains("board_outline")) {
        const auto& bo = arguments["board_outline"];
        if (bo.is_object()) {
            double x0 = bo.value("x", 0.0);
            double y0 = bo.value("y", 0.0);
            outlineW = bo.value("width_mm", bo.value("width", outlineW));
            outlineH = bo.value("height_mm", bo.value("height", outlineH));
            outline.clear();
            outline.push_back({x0, y0, x0 + outlineW, y0});
            outline.push_back({x0 + outlineW, y0, x0 + outlineW, y0 + outlineH});
            outline.push_back({x0 + outlineW, y0 + outlineH, x0, y0 + outlineH});
            outline.push_back({x0, y0 + outlineH, x0, y0});
        } else if (bo.is_array()) {
            outline.clear();
            for (const auto& seg : bo) {
                if (!seg.is_object()) continue;
                PcbOutlineLine ln;
                ln.x1 = seg.value("x1", 0.0);
                ln.y1 = seg.value("y1", 0.0);
                ln.x2 = seg.value("x2", 0.0);
                ln.y2 = seg.value("y2", 0.0);
                outline.push_back(ln);
                outlineW = std::max({outlineW, std::abs(ln.x2 - ln.x1), ln.x1, ln.x2});
                outlineH = std::max({outlineH, std::abs(ln.y2 - ln.y1), ln.y1, ln.y2});
            }
        }
    }

    // 2. Footprint placements (`placements` from `eda_calc_pcb_placement` or `footprints`)
    auto applyPlacements = [&](const nlohmann::json& arr) {
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            std::string ref = item.value("reference", item.value("ref", ""));
            if (ref.empty()) continue;
            double x = item.value("x_mm", item.value("x", 0.0));
            double y = item.value("y_mm", item.value("y", 0.0));
            double rot = item.value("rotation_deg", item.value("rotation", 0.0));
            std::string layer = item.value("layer", "F.Cu");
            std::string fpLib = item.value("footprint", "Resistor_SMD:R_0603_1608Metric");
            std::string val = item.value("value", ref);
            std::string lcsc = item.value("lcsc", "");

            auto applyItemPinNets = [&](PcbFootprintModel& targetFp) {
                if (item.contains("pin_nets") && item["pin_nets"].is_object()) {
                    for (auto pit = item["pin_nets"].begin(); pit != item["pin_nets"].end(); ++pit) {
                        std::string pNum = pit.key();
                        std::string pNet = pit.value().is_string() ? pit.value().get<std::string>() : "";
                        if (!pNet.empty()) {
                            for (auto& pad : targetFp.pads) {
                                if (pad.number == pNum) {
                                    pad.netName = pNet;
                                    pad.netCode = ensureNetCode(pNet);
                                }
                            }
                        }
                    }
                }
                if (item.contains("pins") && item["pins"].is_array()) {
                    for (const auto& pinObj : item["pins"]) {
                        if (!pinObj.is_object()) continue;
                        std::string pNum = pinObj.value("number", pinObj.value("pin", ""));
                        std::string pNet = pinObj.value("net", "");
                        if (!pNum.empty() && !pNet.empty()) {
                            for (auto& pad : targetFp.pads) {
                                if (pad.number == pNum) {
                                    pad.netName = pNet;
                                    pad.netCode = ensureNetCode(pNet);
                                }
                            }
                        }
                    }
                }
            };

            auto it = std::find_if(footprints.begin(), footprints.end(), [&](const PcbFootprintModel& f) {
                return f.reference == ref;
            });

            if (it != footprints.end()) {
                it->x = x;
                it->y = y;
                it->rotation = rot;
                it->layer = layer;
                if (item.contains("footprint")) it->footprint = fpLib;
                if (item.contains("value")) it->value = val;
                if (!lcsc.empty()) it->lcsc = lcsc;
                for (auto& pad : it->pads) {
                    if (pad.type == "smd") pad.layer = layer;
                }
                applyItemPinNets(*it);
            } else {
                PcbFootprintModel fp;
                fp.reference = ref;
                fp.value = val;
                fp.footprint = fpLib;
                fp.lcsc = lcsc;
                fp.x = x;
                fp.y = y;
                fp.rotation = rot;
                fp.layer = layer;
                if (item.contains("pads") && item["pads"].is_array()) {
                    for (const auto& pObj : item["pads"]) {
                        if (!pObj.is_object()) continue;
                        PcbPadModel pad;
                        pad.number = pObj.value("number", "1");
                        pad.type = pObj.value("type", "smd");
                        pad.shape = pObj.value("shape", "rect");
                        pad.relX = pObj.value("dx", pObj.value("rel_x", 0.0));
                        pad.relY = pObj.value("dy", pObj.value("rel_y", 0.0));
                        pad.sizeX = pObj.value("size_x", 0.8);
                        pad.sizeY = pObj.value("size_y", 0.8);
                        pad.drill = pObj.value("drill_mm", pObj.value("drill", 0.0));
                        pad.layer = pObj.value("layer", (pad.type == "smd") ? layer : "*.Cu");
                        pad.netName = pObj.value("net", "");
                        pad.netCode = ensureNetCode(pad.netName);
                        fp.pads.push_back(pad);
                    }
                }
                if (fp.pads.empty()) {
                    fp.pads = defaultPadsForFootprint(fpLib, layer);
                }
                applyItemPinNets(fp);
                footprints.push_back(fp);
            }
        }
    };
    if (arguments.contains("placements") && arguments["placements"].is_array()) {
        applyPlacements(arguments["placements"]);
    }
    if (arguments.contains("footprints") && arguments["footprints"].is_array()) {
        applyPlacements(arguments["footprints"]);
    }

    // 3. Mounting holes
    if (arguments.contains("mounting_holes") && arguments["mounting_holes"].is_array()) {
        int mhIdx = 1;
        for (const auto& hObj : arguments["mounting_holes"]) {
            if (!hObj.is_object()) continue;
            double hx = hObj.value("x", hObj.value("x_mm", 0.0));
            double hy = hObj.value("y", hObj.value("y_mm", 0.0));
            double drill = hObj.value("drill_mm", hObj.value("drill", 3.2));
            double padSz = hObj.value("pad_mm", hObj.value("pad", drill * 1.8));

            PcbFootprintModel mhFp;
            mhFp.reference = hObj.value("reference", "H" + std::to_string(mhIdx++));
            mhFp.value = "MountingHole";
            mhFp.footprint = "MountingHole:MountingHole_" + fmtMm(drill) + "mm";
            mhFp.x = hx;
            mhFp.y = hy;
            mhFp.layer = "F.Cu";
            PcbPadModel mhPad;
            mhPad.number = "1";
            mhPad.type = "np_thru_hole";
            mhPad.shape = "circle";
            mhPad.sizeX = padSz;
            mhPad.sizeY = padSz;
            mhPad.drill = drill;
            mhPad.layer = "*.Cu";
            mhFp.pads.push_back(mhPad);
            footprints.push_back(mhFp);
            holes.push_back({hx, hy, drill, padSz});
        }
    }

    // 4. Copper pour zones (`zones` or `copper_pours`)
    auto applyZones = [&](const nlohmann::json& arr) {
        for (const auto& zObj : arr) {
            if (!zObj.is_object()) continue;
            PcbZoneModel z;
            z.netName = zObj.value("net", zObj.value("net_name", "GND"));
            z.netCode = ensureNetCode(z.netName);
            z.layer = zObj.value("layer", "F.Cu");
            z.clearance = zObj.value("clearance_mm", zObj.value("clearance", 0.25));
            if (zObj.contains("points") && zObj["points"].is_array()) {
                for (const auto& pt : zObj["points"]) {
                    if (pt.is_object()) {
                        z.points.push_back({pt.value("x", 0.0), pt.value("y", 0.0)});
                    }
                }
            }
            if (z.points.empty()) {
                double x0 = zObj.value("x", 0.0);
                double y0 = zObj.value("y", 0.0);
                double zw = zObj.value("width_mm", outlineW);
                double zh = zObj.value("height_mm", outlineH);
                z.points = {{x0, y0}, {x0 + zw, y0}, {x0 + zw, y0 + zh}, {x0, y0 + zh}};
            }
            zones.push_back(z);
        }
    };
    if (arguments.contains("zones") && arguments["zones"].is_array()) {
        applyZones(arguments["zones"]);
    }
    if (arguments.contains("copper_pours") && arguments["copper_pours"].is_array()) {
        applyZones(arguments["copper_pours"]);
    }

    // 5. Copper tracks (`tracks` from `eda_calc_pcb_autoroute` or `add_tracks`)
    auto applyTracks = [&](const nlohmann::json& arr) {
        for (const auto& tObj : arr) {
            if (!tObj.is_object()) continue;
            PcbTrackModel t;
            t.x1 = tObj.value("x1", 0.0);
            t.y1 = tObj.value("y1", 0.0);
            t.x2 = tObj.value("x2", 0.0);
            t.y2 = tObj.value("y2", 0.0);
            t.width = tObj.value("width_mm", tObj.value("width", 0.25));
            t.layer = tObj.value("layer", "F.Cu");
            t.netName = tObj.value("net", "");
            t.netCode = ensureNetCode(t.netName);
            tracks.push_back(t);
        }
    };
    if (arguments.contains("tracks") && arguments["tracks"].is_array()) {
        applyTracks(arguments["tracks"]);
    }
    if (arguments.contains("add_tracks") && arguments["add_tracks"].is_array()) {
        applyTracks(arguments["add_tracks"]);
    }

    // 6. Vias (`vias` from `eda_calc_pcb_autoroute` or `add_vias`)
    auto applyVias = [&](const nlohmann::json& arr) {
        for (const auto& vObj : arr) {
            if (!vObj.is_object()) continue;
            PcbViaModel v;
            v.x = vObj.value("x", 0.0);
            v.y = vObj.value("y", 0.0);
            v.size = vObj.value("size_mm", vObj.value("size", 0.6));
            v.drill = vObj.value("drill_mm", vObj.value("drill", 0.3));
            v.netName = vObj.value("net", "");
            v.netCode = ensureNetCode(v.netName);
            vias.push_back(v);
        }
    };
    if (arguments.contains("vias") && arguments["vias"].is_array()) {
        applyVias(arguments["vias"]);
    }
    if (arguments.contains("add_vias") && arguments["add_vias"].is_array()) {
        applyVias(arguments["add_vias"]);
    }

    // 7. Optional Specctra `.ses` session import (`import_ses_path` or `specctra_ses`)
    std::string sesContent = arguments.value("specctra_ses", "");
    std::string sesPath = arguments.value("import_ses_path", "");
    if (sesContent.empty() && !sesPath.empty() && fs::exists(sesPath)) {
        std::ifstream ifs(sesPath);
        if (ifs) {
            sesContent.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
    }
    if (!sesContent.empty()) {
        SexpNode sesRoot;
        std::string sesErr;
        if (parseSexp(sesContent, sesRoot, sesErr)) {
            // Walk Specctra `(routes ... (network_out (net <name> (wire (path <layer> <width> x1 y1 x2 y2)))))`
            std::function<void(const SexpNode&, const std::string&)> walkSes;
            walkSes = [&](const SexpNode& node, const std::string& currentNet) {
                if (!node.isList) return;
                std::string nextNet = currentNet;
                if (node.head() == "net" && node.children.size() >= 2 && !node.children[1].isList) {
                    nextNet = node.children[1].atom;
                    ensureNetCode(nextNet);
                } else if (node.head() == "path" && node.children.size() >= 7) {
                    std::string ly = node.children[1].atom;
                    double w = toDoubleSafe(node.children[2].atom, 0.25);
                    if (w > 10.0) w *= 0.001; // convert um to mm if in Specctra resolution
                    for (size_t i = 3; i + 3 < node.children.size(); i += 2) {
                        PcbTrackModel trk;
                        trk.x1 = toDoubleSafe(node.children[i].atom);
                        trk.y1 = toDoubleSafe(node.children[i + 1].atom);
                        trk.x2 = toDoubleSafe(node.children[i + 2].atom);
                        trk.y2 = toDoubleSafe(node.children[i + 3].atom);
                        trk.width = w;
                        trk.layer = (ly.find("B") != std::string::npos || ly == "2") ? "B.Cu" : "F.Cu";
                        trk.netName = nextNet;
                        trk.netCode = ensureNetCode(nextNet);
                        tracks.push_back(trk);
                    }
                }
                for (const auto& ch : node.children) {
                    walkSes(ch, nextNet);
                }
            };
            walkSes(sesRoot, "");
        }
    }

    std::string pcbSexp = serializePcbSexp(netByCode, outlineW, outlineH, outline,
                                           footprints, tracks, vias, zones);
    if (!pcbPath.empty()) {
        fs::path p(pcbPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }
        std::ofstream ofs(pcbPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return {{"ok", false}, {"error", "Failed to write PCB file: " + pcbPath}};
        }
        ofs << pcbSexp;
        ofs.close();
    }

    nlohmann::json summary = inspectFile({{"sexp", pcbSexp}});
    summary["pcb_path"] = pcbPath;
    summary["sexp"] = pcbSexp;
    return summary;
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
