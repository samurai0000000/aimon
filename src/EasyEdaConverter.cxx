/*
 * EasyEdaConverter.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EasyEdaConverter.hxx"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
#include <algorithm>

namespace aimon {

namespace {

std::vector<std::string> splitByChar(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}

std::vector<std::string> splitBySubstring(const std::string& s, const std::string& delim) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(delim, pos);
        if (next == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, next - pos));
        pos = next + delim.size();
    }
    return out;
}

double parseDoubleSafe(const std::string& s, double fallback = 0.0) {
    if (s.empty()) return fallback;
    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        if (!std::isfinite(v)) return fallback;
        return v;
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

std::string escapeSexpString(const std::string& in) {
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

std::string mapEasyEdaPcbLayer(const std::string& layerId) {
    if (layerId == "1" || layerId == "F.Cu" || layerId == "TopLayer") return "F.Cu";
    if (layerId == "2" || layerId == "B.Cu" || layerId == "BottomLayer") return "B.Cu";
    if (layerId == "3" || layerId == "F.SilkS") return "F.SilkS";
    if (layerId == "4" || layerId == "B.SilkS") return "B.SilkS";
    if (layerId == "10" || layerId == "11" || layerId == "Edge.Cuts" || layerId == "BoardOutLine") {
        return "Edge.Cuts";
    }
    return "F.Cu";
}

} // anonymous namespace

nlohmann::json EasyEdaConversionResult::toJson() const {
    nlohmann::json compArr = nlohmann::json::array();
    for (const auto& c : components) {
        nlohmann::json pinArr = nlohmann::json::array();
        for (const auto& p : c.pins) {
            pinArr.push_back({
                {"number", p.number},
                {"name", p.name},
                {"x", p.xMm},
                {"y", p.yMm}
            });
        }
        compArr.push_back({
            {"reference", c.reference},
            {"value", c.value},
            {"footprint", c.footprint},
            {"lcsc", c.lcsc},
            {"x", c.xMm},
            {"y", c.yMm},
            {"rotation", c.rotationDeg},
            {"layer", c.layer},
            {"pins", pinArr}
        });
    }

    nlohmann::json wireArr = nlohmann::json::array();
    for (const auto& w : wires) {
        wireArr.push_back({
            {"x1", w.x1Mm},
            {"y1", w.y1Mm},
            {"x2", w.x2Mm},
            {"y2", w.y2Mm},
            {"net", w.net}
        });
    }

    nlohmann::json labelArr = nlohmann::json::array();
    for (const auto& l : labels) {
        labelArr.push_back({
            {"name", l.name},
            {"x", l.xMm},
            {"y", l.yMm},
            {"is_power", l.isPower}
        });
    }

    nlohmann::json trackArr = nlohmann::json::array();
    for (const auto& t : tracks) {
        trackArr.push_back({
            {"x1", t.x1Mm},
            {"y1", t.y1Mm},
            {"x2", t.x2Mm},
            {"y2", t.y2Mm},
            {"width", t.widthMm},
            {"layer", t.layer},
            {"net", t.net}
        });
    }

    nlohmann::json viaArr = nlohmann::json::array();
    for (const auto& v : vias) {
        viaArr.push_back({
            {"x", v.xMm},
            {"y", v.yMm},
            {"size", v.sizeMm},
            {"drill", v.drillMm},
            {"net", v.net}
        });
    }

    return {
        {"ok", ok},
        {"format", format},
        {"output_path", outputPath},
        {"sexp", sexp},
        {"components_count", components.size()},
        {"wires_count", wires.size()},
        {"labels_count", labels.size()},
        {"tracks_count", tracks.size()},
        {"vias_count", vias.size()},
        {"outline_width_mm", outlineWidthMm},
        {"outline_height_mm", outlineHeightMm},
        {"components", compArr},
        {"wires", wireArr},
        {"labels", labelArr},
        {"tracks", trackArr},
        {"vias", viaArr},
        {"error", errorMessage}
    };
}

double EasyEdaConverter::snapTo50MilGrid(double valMm) {
    if (!std::isfinite(valMm)) return 0.0;
    double snapped = static_cast<double>(std::llround(valMm / kGridStepMm)) * kGridStepMm;
    if (std::abs(snapped) < 1e-9) return 0.0;
    return snapped;
}

double EasyEdaConverter::easyEdaUnitsToMmSnapped(double units) {
    if (!std::isfinite(units)) return 0.0;
    return snapTo50MilGrid(units * kEasyEdaUnitToMm);
}

EasyEdaConversionResult EasyEdaConverter::convertJson(const nlohmann::json& easyEdaDoc,
                                                      const std::string& targetFormat,
                                                      const std::string& outputPath) {
    EasyEdaConversionResult res;
    res.outputPath = outputPath;

    if (!easyEdaDoc.is_object()) {
        res.errorMessage = "EasyEDA input must be a JSON object";
        return res;
    }

    std::string fmt = targetFormat;
    std::string docType;
    if (easyEdaDoc.contains("head") && easyEdaDoc["head"].is_object()) {
        const auto& head = easyEdaDoc["head"];
        if (head.contains("docType")) {
            if (head["docType"].is_string()) {
                docType = head["docType"].get<std::string>();
            } else if (head["docType"].is_number_integer()) {
                docType = std::to_string(head["docType"].get<int>());
            }
        }
    } else if (easyEdaDoc.contains("docType") && easyEdaDoc["docType"].is_string()) {
        docType = easyEdaDoc["docType"].get<std::string>();
    }

    if (fmt.empty() || fmt == "auto") {
        if (docType == "3" || docType == "4" || docType == "pcb" ||
            easyEdaDoc.contains("tracks") || easyEdaDoc.contains("board_outline")) {
            fmt = "kicad_pcb";
        } else {
            fmt = "kicad_sch";
        }
    }
    res.format = fmt;

    bool inputInMm = (easyEdaDoc.value("units", "") == "mm");
    auto toCoordMm = [&](double rawVal, bool snap50Mil) -> double {
        double mm = inputInMm ? rawVal : (rawVal * kEasyEdaUnitToMm);
        return snap50Mil ? snapTo50MilGrid(mm) : mm;
    };

    // 1. Parse EasyEDA Standard `shape` array if present
    if (easyEdaDoc.contains("shape") && easyEdaDoc["shape"].is_array()) {
        int autoRefIdx = 1;
        for (const auto& item : easyEdaDoc["shape"]) {
            if (!item.is_string()) continue;
            std::string shapeStr = item.get<std::string>();
            if (shapeStr.empty()) continue;

            std::vector<std::string> subShapes = splitBySubstring(shapeStr, "#@$");
            const std::string& header = subShapes[0];
            std::vector<std::string> tok = splitByChar(header, '~');
            if (tok.empty()) continue;

            const std::string& type = tok[0];
            if (type == "W" && tok.size() >= 2) {
                // Wire: W~x1 y1 x2 y2 ...~color~width~...
                std::istringstream iss(tok[1]);
                std::vector<std::pair<double, double>> pts;
                double rx = 0.0, ry = 0.0;
                while (iss >> rx >> ry) {
                    pts.push_back({easyEdaUnitsToMmSnapped(rx), easyEdaUnitsToMmSnapped(ry)});
                }
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    EasyEdaConvertedWire w;
                    w.x1Mm = pts[i].first;
                    w.y1Mm = pts[i].second;
                    w.x2Mm = pts[i + 1].first;
                    w.y2Mm = pts[i + 1].second;
                    res.wires.push_back(w);
                }
            } else if (type == "N" && tok.size() >= 6) {
                // Net label: N~x~y~rot~color~name~...
                EasyEdaConvertedLabel lbl;
                lbl.xMm = easyEdaUnitsToMmSnapped(parseDoubleSafe(tok[1]));
                lbl.yMm = easyEdaUnitsToMmSnapped(parseDoubleSafe(tok[2]));
                lbl.name = tok[5];
                lbl.isPower = (lbl.name == "GND" || lbl.name == "VCC" ||
                               (!lbl.name.empty() && lbl.name[0] == '+'));
                if (!lbl.name.empty()) {
                    res.labels.push_back(lbl);
                }
            } else if (type == "LIB" && tok.size() >= 3) {
                EasyEdaConvertedComponent comp;
                double rawX = parseDoubleSafe(tok[1]);
                double rawY = parseDoubleSafe(tok[2]);
                comp.xMm = (fmt == "kicad_sch") ? easyEdaUnitsToMmSnapped(rawX) : (rawX * kEasyEdaUnitToMm);
                comp.yMm = (fmt == "kicad_sch") ? easyEdaUnitsToMmSnapped(rawY) : (rawY * kEasyEdaUnitToMm);
                comp.reference = "U" + std::to_string(autoRefIdx++);
                comp.value = "Component";
                comp.footprint = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm";

                if (tok.size() >= 4) {
                    // Parse key`val`key`val pairs in tok[3]
                    std::vector<std::string> kv = splitByChar(tok[3], '`');
                    for (size_t i = 0; i + 1 < kv.size(); i += 2) {
                        const std::string& k = kv[i];
                        const std::string& v = kv[i + 1];
                        if (k == "package" || k == "Footprint") comp.footprint = v;
                        else if (k == "Supplier Part" || k == "LCSC" || k == "lcsc") comp.lcsc = v;
                        else if (k == "Reference" || k == "spicePre") comp.reference = v;
                        else if (k == "Value" || k == "Comment" || k == "Manufacturer Part") comp.value = v;
                    }
                }
                if (tok.size() >= 5 && !tok[4].empty()) {
                    comp.rotationDeg = parseDoubleSafe(tok[4]);
                }

                for (size_t sIdx = 1; sIdx < subShapes.size(); ++sIdx) {
                    std::vector<std::string> ptok = splitByChar(subShapes[sIdx], '~');
                    if (ptok.empty()) continue;
                    if (ptok[0] == "P" && ptok.size() >= 6) {
                        EasyEdaConvertedPin pin;
                        pin.number = ptok[3].empty() ? std::to_string(comp.pins.size() + 1) : ptok[3];
                        pin.name = (ptok.size() >= 15 && !ptok[14].empty()) ? ptok[14] : pin.number;
                        pin.xMm = easyEdaUnitsToMmSnapped(parseDoubleSafe(ptok[4]));
                        pin.yMm = easyEdaUnitsToMmSnapped(parseDoubleSafe(ptok[5]));
                        comp.pins.push_back(pin);
                    } else if (ptok[0] == "PAD" && ptok.size() >= 10) {
                        EasyEdaConvertedPin pad;
                        pad.number = ptok[9].empty() ? std::to_string(comp.pins.size() + 1) : ptok[9];
                        pad.name = (ptok.size() >= 7) ? ptok[6] : "";
                        pad.xMm = parseDoubleSafe(ptok[2]) * kEasyEdaUnitToMm;
                        pad.yMm = parseDoubleSafe(ptok[3]) * kEasyEdaUnitToMm;
                        comp.pins.push_back(pad);
                    } else if (ptok[0] == "T" && ptok.size() >= 6) {
                        if (ptok[1] == "P") comp.reference = ptok[5];
                        else if (ptok[1] == "N") comp.value = ptok[5];
                    }
                }
                res.components.push_back(comp);
            } else if (type == "TRACK" && tok.size() >= 5) {
                double widthMm = std::max(0.1, parseDoubleSafe(tok[1], 1.0) * kEasyEdaUnitToMm);
                std::string layer = mapEasyEdaPcbLayer(tok[2]);
                std::string net = tok[3];
                std::istringstream iss(tok[4]);
                std::vector<std::pair<double, double>> pts;
                double rx = 0.0, ry = 0.0;
                while (iss >> rx >> ry) {
                    pts.push_back({rx * kEasyEdaUnitToMm, ry * kEasyEdaUnitToMm});
                }
                if (layer == "Edge.Cuts" && pts.size() >= 2) {
                    double minX = pts[0].first, maxX = pts[0].first;
                    double minY = pts[0].second, maxY = pts[0].second;
                    for (const auto& pt : pts) {
                        minX = std::min(minX, pt.first);
                        maxX = std::max(maxX, pt.first);
                        minY = std::min(minY, pt.second);
                        maxY = std::max(maxY, pt.second);
                    }
                    res.outlineWidthMm = std::max(res.outlineWidthMm, maxX - minX);
                    res.outlineHeightMm = std::max(res.outlineHeightMm, maxY - minY);
                } else {
                    for (size_t i = 0; i + 1 < pts.size(); ++i) {
                        EasyEdaConvertedTrack trk;
                        trk.x1Mm = pts[i].first;
                        trk.y1Mm = pts[i].second;
                        trk.x2Mm = pts[i + 1].first;
                        trk.y2Mm = pts[i + 1].second;
                        trk.widthMm = widthMm;
                        trk.layer = layer;
                        trk.net = net;
                        res.tracks.push_back(trk);
                    }
                }
            } else if (type == "VIA" && tok.size() >= 6) {
                EasyEdaConvertedVia via;
                via.xMm = parseDoubleSafe(tok[1]) * kEasyEdaUnitToMm;
                via.yMm = parseDoubleSafe(tok[2]) * kEasyEdaUnitToMm;
                via.sizeMm = std::max(0.3, parseDoubleSafe(tok[3], 2.4) * kEasyEdaUnitToMm);
                via.net = tok[4];
                via.drillMm = std::max(0.15, parseDoubleSafe(tok[5], 1.2) * kEasyEdaUnitToMm);
                res.vias.push_back(via);
            }
        }
    }

    // 2. Parse structured JSON arrays (`components`, `wires`, `labels`, `tracks`, `vias`, `board_outline`)
    if (easyEdaDoc.contains("components") && easyEdaDoc["components"].is_array()) {
        bool snapSch = (fmt == "kicad_sch");
        for (const auto& cObj : easyEdaDoc["components"]) {
            if (!cObj.is_object()) continue;
            EasyEdaConvertedComponent comp;
            comp.reference = cObj.value("reference", cObj.value("designator", "U1"));
            comp.value = cObj.value("value", cObj.value("name", "Component"));
            comp.footprint = cObj.value("footprint", cObj.value("package", "Resistor_SMD:R_0603_1608Metric"));
            comp.lcsc = cObj.value("lcsc", "");
            comp.xMm = toCoordMm(cObj.value("x", 0.0), snapSch);
            comp.yMm = toCoordMm(cObj.value("y", 0.0), snapSch);
            comp.rotationDeg = cObj.value("rotation", 0.0);
            comp.layer = cObj.value("layer", "F.Cu");

            if (cObj.contains("pins") && cObj["pins"].is_array()) {
                for (const auto& pObj : cObj["pins"]) {
                    if (!pObj.is_object()) continue;
                    EasyEdaConvertedPin pin;
                    pin.number = pObj.value("number", "1");
                    pin.name = pObj.value("name", pin.number);
                    if (pObj.contains("x") && pObj.contains("y")) {
                        pin.xMm = toCoordMm(pObj.value("x", 0.0), snapSch);
                        pin.yMm = toCoordMm(pObj.value("y", 0.0), snapSch);
                    } else {
                        double dx = toCoordMm(pObj.value("dx", 0.0), snapSch);
                        double dy = toCoordMm(pObj.value("dy", 0.0), snapSch);
                        pin.xMm = snapSch ? snapTo50MilGrid(comp.xMm + dx) : (comp.xMm + dx);
                        pin.yMm = snapSch ? snapTo50MilGrid(comp.yMm + dy) : (comp.yMm + dy);
                    }
                    comp.pins.push_back(pin);
                }
            }
            res.components.push_back(comp);
        }
    }

    if (easyEdaDoc.contains("wires") && easyEdaDoc["wires"].is_array()) {
        for (const auto& wObj : easyEdaDoc["wires"]) {
            if (!wObj.is_object()) continue;
            EasyEdaConvertedWire w;
            w.x1Mm = toCoordMm(wObj.value("x1", 0.0), true);
            w.y1Mm = toCoordMm(wObj.value("y1", 0.0), true);
            w.x2Mm = toCoordMm(wObj.value("x2", 0.0), true);
            w.y2Mm = toCoordMm(wObj.value("y2", 0.0), true);
            w.net = wObj.value("net", "");
            res.wires.push_back(w);
        }
    }

    if (easyEdaDoc.contains("labels") && easyEdaDoc["labels"].is_array()) {
        for (const auto& lObj : easyEdaDoc["labels"]) {
            if (!lObj.is_object()) continue;
            EasyEdaConvertedLabel lbl;
            lbl.name = lObj.value("name", lObj.value("net", ""));
            lbl.xMm = toCoordMm(lObj.value("x", 0.0), true);
            lbl.yMm = toCoordMm(lObj.value("y", 0.0), true);
            lbl.isPower = lObj.value("is_power", false);
            if (!lbl.name.empty()) {
                res.labels.push_back(lbl);
            }
        }
    }

    if (easyEdaDoc.contains("tracks") && easyEdaDoc["tracks"].is_array()) {
        for (const auto& tObj : easyEdaDoc["tracks"]) {
            if (!tObj.is_object()) continue;
            EasyEdaConvertedTrack t;
            t.x1Mm = toCoordMm(tObj.value("x1", 0.0), false);
            t.y1Mm = toCoordMm(tObj.value("y1", 0.0), false);
            t.x2Mm = toCoordMm(tObj.value("x2", 0.0), false);
            t.y2Mm = toCoordMm(tObj.value("y2", 0.0), false);
            t.widthMm = std::max(0.05, tObj.value("width", tObj.value("width_mm", 0.25)));
            t.layer = mapEasyEdaPcbLayer(tObj.value("layer", "F.Cu"));
            t.net = tObj.value("net", "");
            res.tracks.push_back(t);
        }
    }

    if (easyEdaDoc.contains("vias") && easyEdaDoc["vias"].is_array()) {
        for (const auto& vObj : easyEdaDoc["vias"]) {
            if (!vObj.is_object()) continue;
            EasyEdaConvertedVia v;
            v.xMm = toCoordMm(vObj.value("x", 0.0), false);
            v.yMm = toCoordMm(vObj.value("y", 0.0), false);
            v.sizeMm = std::max(0.2, vObj.value("size", vObj.value("size_mm", 0.6)));
            v.drillMm = std::max(0.1, vObj.value("drill", vObj.value("drill_mm", 0.3)));
            v.net = vObj.value("net", "");
            res.vias.push_back(v);
        }
    }

    if (easyEdaDoc.contains("board_outline") && easyEdaDoc["board_outline"].is_object()) {
        const auto& bo = easyEdaDoc["board_outline"];
        res.outlineWidthMm = bo.value("width_mm", bo.value("width", res.outlineWidthMm));
        res.outlineHeightMm = bo.value("height_mm", bo.value("height", res.outlineHeightMm));
    }

    // 3. Emit KiCad 8 S-expression
    std::ostringstream out;
    if (fmt == "kicad_sch") {
        out << "(kicad_sch (version 20231120) (generator \"aimon-easyeda-converter\")\n";
        out << "  (paper \"A4\")\n";
        out << "  (lib_symbols\n";
        for (const auto& c : res.components) {
            std::string libId = "EasyEDA:" + escapeSexpString(c.reference);
            out << "    (symbol \"" << libId << "\" (in_bom yes) (on_board yes)\n";
            out << "      (symbol \"" << escapeSexpString(c.reference) << "_1_1\"\n";
            for (const auto& p : c.pins) {
                double relX = snapTo50MilGrid(p.xMm - c.xMm);
                double relY = snapTo50MilGrid(p.yMm - c.yMm);
                out << "        (pin passive line (at " << fmtMm(relX) << " " << fmtMm(relY) << " 0) (length 2.54)\n";
                out << "          (name \"" << escapeSexpString(p.name) << "\")\n";
                out << "          (number \"" << escapeSexpString(p.number) << "\")\n";
                out << "        )\n";
            }
            out << "      )\n";
            out << "    )\n";
        }
        out << "  )\n";

        for (const auto& c : res.components) {
            std::string libId = "EasyEDA:" + escapeSexpString(c.reference);
            out << "  (symbol (lib_id \"" << libId << "\") (at "
                << fmtMm(c.xMm) << " " << fmtMm(c.yMm) << " " << fmtMm(c.rotationDeg) << ") (unit 1)\n";
            out << "    (property \"Reference\" \"" << escapeSexpString(c.reference) << "\" (at "
                << fmtMm(c.xMm) << " " << fmtMm(c.yMm) << " 0))\n";
            out << "    (property \"Value\" \"" << escapeSexpString(c.value) << "\" (at "
                << fmtMm(c.xMm) << " " << fmtMm(c.yMm) << " 0))\n";
            out << "    (property \"Footprint\" \"" << escapeSexpString(c.footprint) << "\" (at "
                << fmtMm(c.xMm) << " " << fmtMm(c.yMm) << " 0))\n";
            if (!c.lcsc.empty()) {
                out << "    (property \"LCSC\" \"" << escapeSexpString(c.lcsc) << "\" (at "
                    << fmtMm(c.xMm) << " " << fmtMm(c.yMm) << " 0))\n";
            }
            for (const auto& p : c.pins) {
                out << "    (pin \"" << escapeSexpString(p.number) << "\" (at "
                    << fmtMm(p.xMm) << " " << fmtMm(p.yMm) << "))\n";
            }
            out << "  )\n";
        }

        for (const auto& l : res.labels) {
            if (l.isPower) {
                out << "  (power_port \"" << escapeSexpString(l.name) << "\" (at "
                    << fmtMm(l.xMm) << " " << fmtMm(l.yMm) << " 0))\n";
            }
            out << "  (label \"" << escapeSexpString(l.name) << "\" (at "
                << fmtMm(l.xMm) << " " << fmtMm(l.yMm) << " 0))\n";
        }

        for (const auto& w : res.wires) {
            out << "  (wire (pts (xy " << fmtMm(w.x1Mm) << " " << fmtMm(w.y1Mm) << ") (xy "
                << fmtMm(w.x2Mm) << " " << fmtMm(w.y2Mm) << ")))\n";
            if (!w.net.empty()) {
                out << "  (label \"" << escapeSexpString(w.net) << "\" (at "
                    << fmtMm(w.x1Mm) << " " << fmtMm(w.y1Mm) << " 0))\n";
            }
        }
        out << ")\n";
    } else if (fmt == "kicad_pcb") {
        if (res.outlineWidthMm <= 0.0) res.outlineWidthMm = 50.0;
        if (res.outlineHeightMm <= 0.0) res.outlineHeightMm = 40.0;

        std::map<std::string, int> netCodes;
        netCodes[""] = 0;
        int nextNetCode = 1;
        auto ensureNet = [&](const std::string& netName) -> int {
            if (netName.empty()) return 0;
            auto it = netCodes.find(netName);
            if (it != netCodes.end()) return it->second;
            int c = nextNetCode++;
            netCodes[netName] = c;
            return c;
        };
        for (const auto& t : res.tracks) ensureNet(t.net);
        for (const auto& v : res.vias) ensureNet(v.net);
        for (const auto& c : res.components) {
            for (const auto& p : c.pins) ensureNet(p.name);
        }

        out << "(kicad_pcb (version 20240108) (generator \"aimon-easyeda-converter\")\n";
        out << "  (net 0 \"\")\n";
        for (const auto& kv : netCodes) {
            if (kv.second > 0) {
                out << "  (net " << kv.second << " \"" << escapeSexpString(kv.first) << "\")\n";
            }
        }

        double w = res.outlineWidthMm;
        double h = res.outlineHeightMm;
        out << "  (gr_rect (start 0 0) (end " << fmtMm(w) << " " << fmtMm(h)
            << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start 0 0) (end " << fmtMm(w) << " 0) (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start " << fmtMm(w) << " 0) (end " << fmtMm(w) << " " << fmtMm(h)
            << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start " << fmtMm(w) << " " << fmtMm(h) << ") (end 0 " << fmtMm(h)
            << ") (layer \"Edge.Cuts\") (width 0.15))\n";
        out << "  (gr_line (start 0 " << fmtMm(h) << ") (end 0 0) (layer \"Edge.Cuts\") (width 0.15))\n";

        for (const auto& c : res.components) {
            out << "  (footprint \"" << escapeSexpString(c.footprint) << "\" (layer \""
                << escapeSexpString(c.layer) << "\") (at " << fmtMm(c.xMm) << " "
                << fmtMm(c.yMm) << " " << fmtMm(c.rotationDeg) << ")\n";
            out << "    (property \"Reference\" \"" << escapeSexpString(c.reference) << "\")\n";
            out << "    (property \"Value\" \"" << escapeSexpString(c.value) << "\")\n";
            if (!c.lcsc.empty()) {
                out << "    (property \"LCSC\" \"" << escapeSexpString(c.lcsc) << "\")\n";
            }
            for (const auto& p : c.pins) {
                int nc = ensureNet(p.name);
                double relX = p.xMm - c.xMm;
                double relY = p.yMm - c.yMm;
                out << "    (pad \"" << escapeSexpString(p.number) << "\" smd rect (at "
                    << fmtMm(relX) << " " << fmtMm(relY) << ") (size 0.8 0.8) (layers \""
                    << escapeSexpString(c.layer) << "\") (net " << nc << " \""
                    << escapeSexpString(p.name) << "\"))\n";
            }
            out << "  )\n";
        }

        for (const auto& t : res.tracks) {
            int nc = ensureNet(t.net);
            out << "  (segment (start " << fmtMm(t.x1Mm) << " " << fmtMm(t.y1Mm)
                << ") (end " << fmtMm(t.x2Mm) << " " << fmtMm(t.y2Mm)
                << ") (width " << fmtMm(t.widthMm) << ") (layer \"" << escapeSexpString(t.layer)
                << "\") (net " << nc << "))\n";
        }

        for (const auto& v : res.vias) {
            int nc = ensureNet(v.net);
            out << "  (via (at " << fmtMm(v.xMm) << " " << fmtMm(v.yMm)
                << ") (size " << fmtMm(v.sizeMm) << ") (drill " << fmtMm(v.drillMm)
                << ") (layers \"F.Cu\" \"B.Cu\") (net " << nc << "))\n";
        }
        out << ")\n";
    } else {
        res.errorMessage = "Unsupported target format '" + fmt + "' (expected kicad_sch or kicad_pcb)";
        return res;
    }

    res.sexp = out.str();
    if (!outputPath.empty()) {
        std::ofstream ofs(outputPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            res.errorMessage = "Failed to open output file for writing: " + outputPath;
            return res;
        }
        ofs << res.sexp;
        ofs.close();
    }

    res.ok = true;
    return res;
}

EasyEdaConversionResult EasyEdaConverter::convertFile(const std::string& inputPath,
                                                      const std::string& outputPath,
                                                      const std::string& targetFormat) {
    EasyEdaConversionResult res;
    res.outputPath = outputPath;
    if (inputPath.empty()) {
        res.errorMessage = "Empty EasyEDA input file path";
        return res;
    }

    std::ifstream ifs(inputPath, std::ios::binary);
    if (!ifs) {
        res.errorMessage = "Failed to open EasyEDA input file: " + inputPath;
        return res;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    nlohmann::json doc = nlohmann::json::parse(content, nullptr, false);
    if (doc.is_discarded()) {
        res.errorMessage = "Invalid JSON in EasyEDA input file: " + inputPath;
        return res;
    }

    return convertJson(doc, targetFormat, outputPath);
}

nlohmann::json EasyEdaConverter::executeTool(const nlohmann::json& arguments) {
    std::string targetFormat = arguments.value("target_format", arguments.value("format", "auto"));
    std::string outputPath = arguments.value("output_path", arguments.value("output_file", ""));
    std::string inputPath = arguments.value("input_path", arguments.value("file_path", arguments.value("path", "")));

    if (arguments.contains("easyeda_json") && arguments["easyeda_json"].is_object()) {
        return convertJson(arguments["easyeda_json"], targetFormat, outputPath).toJson();
    }
    if (arguments.contains("source_json") && arguments["source_json"].is_object()) {
        return convertJson(arguments["source_json"], targetFormat, outputPath).toJson();
    }
    if (!inputPath.empty()) {
        return convertFile(inputPath, outputPath, targetFormat).toJson();
    }

    EasyEdaConversionResult err;
    err.errorMessage = "easyeda_convert requires 'input_path' or 'easyeda_json'";
    return err.toJson();
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
