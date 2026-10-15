/*
 * TestEasyEdaConverterProps.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <rapidcheck.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "EasyEdaConverter.hxx"
#include "KiCadSchematicEditor.hxx"

using namespace aimon;

namespace {

bool isMultipleOf50Mil(double valMm) {
    double ratio = valMm / EasyEdaConverter::kGridStepMm;
    return std::abs(ratio - std::round(ratio)) < 1e-6;
}

} // anonymous namespace

int main() {
    bool allPassed = true;

    // Property 1: Uniform 50-mil (1.27 mm) grid snapping & idempotency
    allPassed &= rc::check("EasyEdaConverter 50-mil grid snapping is exact and idempotent", []() {
        int rawTenthsMm = *rc::gen::inRange(-5000, 5000);
        double valMm = static_cast<double>(rawTenthsMm) * 0.1;
        double snapped1 = EasyEdaConverter::snapTo50MilGrid(valMm);
        double snapped2 = EasyEdaConverter::snapTo50MilGrid(snapped1);
        RC_ASSERT(isMultipleOf50Mil(snapped1));
        RC_ASSERT(std::abs(snapped1 - snapped2) < 1e-9);
        RC_ASSERT(std::abs(snapped1 - valMm) <= (EasyEdaConverter::kGridStepMm * 0.5 + 1e-6));
    });

    // Property 2: Pin-to-wire connectivity preservation under EasyEDA -> KiCad 8 conversion
    allPassed &= rc::check("EasyEdaConverter preserves pin-to-wire electrical connectivity on 50-mil grid", []() {
        int u1XUnits = *rc::gen::inRange(50, 400);
        int u1YUnits = *rc::gen::inRange(50, 400);
        int u2XUnits = *rc::gen::inRange(450, 800);
        int u2YUnits = *rc::gen::inRange(50, 400);

        // Pin 1 on R1 and Pin 1 on R2 connected by wire in EasyEDA 10-mil units
        int p1X = u1XUnits + 10;
        int p1Y = u1YUnits;
        int p2X = u2XUnits - 10;
        int p2Y = u2YUnits;

        std::string lib1 = "LIB~" + std::to_string(u1XUnits) + "~" + std::to_string(u1YUnits) +
                           "~Reference`R1`Value`10k`package`R_0603`LCSC`C25804~0#@$"
                           "P~show~0~1~" + std::to_string(p1X) + "~" + std::to_string(p1Y) + "~0~p1";
        std::string lib2 = "LIB~" + std::to_string(u2XUnits) + "~" + std::to_string(u2YUnits) +
                           "~Reference`R2`Value`4.7k`package`R_0603`LCSC`C25900~0#@$"
                           "P~show~0~1~" + std::to_string(p2X) + "~" + std::to_string(p2Y) + "~0~p2";
        std::string wire = "W~" + std::to_string(p1X) + " " + std::to_string(p1Y) + " " +
                           std::to_string(p2X) + " " + std::to_string(p2Y) + "~#008800~1";
        std::string label = "N~" + std::to_string(p1X) + "~" + std::to_string(p1Y) + "~0~#0000ff~SIG_A";

        nlohmann::json doc = {
            {"head", {{"docType", "1"}}},
            {"shape", nlohmann::json::array({lib1, lib2, wire, label})}
        };

        auto conv = EasyEdaConverter::convertJson(doc, "kicad_sch");
        RC_ASSERT(conv.ok);
        RC_ASSERT(conv.components.size() == 2u);
        RC_ASSERT(conv.wires.size() == 1u);

        for (const auto& c : conv.components) {
            RC_ASSERT(isMultipleOf50Mil(c.xMm));
            RC_ASSERT(isMultipleOf50Mil(c.yMm));
            for (const auto& p : c.pins) {
                RC_ASSERT(isMultipleOf50Mil(p.xMm));
                RC_ASSERT(isMultipleOf50Mil(p.yMm));
            }
        }
        RC_ASSERT(std::abs(conv.components[0].pins[0].xMm - conv.wires[0].x1Mm) < 1e-6);
        RC_ASSERT(std::abs(conv.components[0].pins[0].yMm - conv.wires[0].y1Mm) < 1e-6);
        RC_ASSERT(std::abs(conv.components[1].pins[0].xMm - conv.wires[0].x2Mm) < 1e-6);
        RC_ASSERT(std::abs(conv.components[1].pins[0].yMm - conv.wires[0].y2Mm) < 1e-6);

        // Verify round-trip S-expression inspection yields the connected net SIG_A with 2 nodes
        nlohmann::json inspected = KiCadSchematicEditor::inspectFile({{"sexp", conv.sexp}});
        RC_ASSERT(inspected.value("ok", false));
        RC_ASSERT(inspected["nets"].size() == 1u);
        RC_ASSERT(inspected["nets"][0].value("name", "") == "SIG_A");
        RC_ASSERT(inspected["nets"][0]["nodes"].size() == 2u);
    });

    // Property 3: KiCadSchematicEditor schematic -> PCB sync & layout round-trip invariant
    allPassed &= rc::check("KiCadSchematicEditor schematic edit and PCB sync round-trip preserves components and nets", []() {
        int nResistors = *rc::gen::inRange(2, 6);
        nlohmann::json addComps = nlohmann::json::array();
        nlohmann::json addWires = nlohmann::json::array();

        for (int i = 0; i < nResistors; ++i) {
            double cx = (i + 1) * 12.7;
            double cy = 25.4;
            addComps.push_back({
                {"reference", "R" + std::to_string(i + 1)},
                {"value", "1k"},
                {"lib_id", "Device:R"},
                {"footprint", "Resistor_SMD:R_0603_1608Metric"},
                {"lcsc", "C21190"},
                {"x", cx + 0.13}, // intentionally off-grid to test 50-mil snapping
                {"y", cy - 0.09}
            });
            if (i > 0) {
                double prevPin2X = i * 12.7 + 2.54;
                double curPin1X = (i + 1) * 12.7 - 2.54;
                addWires.push_back({
                    {"x1", prevPin2X},
                    {"y1", cy},
                    {"x2", curPin1X},
                    {"y2", cy},
                    {"net", "CHAIN_" + std::to_string(i)}
                });
            }
        }

        nlohmann::json schRes = KiCadSchematicEditor::applySchematicEdits({
            {"add_components", addComps},
            {"add_wires", addWires}
        });
        RC_ASSERT(schRes.value("ok", false));
        RC_ASSERT(static_cast<int>(schRes.value("components_count", 0)) == nResistors);
        for (const auto& c : schRes["components"]) {
            RC_ASSERT(isMultipleOf50Mil(c.value("x", 0.0)));
            RC_ASSERT(isMultipleOf50Mil(c.value("y", 0.0)));
        }

        nlohmann::json syncRes = KiCadSchematicEditor::syncSchematicToPcb({
            {"schematic_sexp", schRes.value("sexp", "")}
        });
        RC_ASSERT(syncRes.value("ok", false));
        RC_ASSERT(static_cast<int>(syncRes.value("synced_components", 0)) == nResistors);
        RC_ASSERT(static_cast<int>(syncRes.value("nets_count", 0)) == nResistors - 1);
    });

    // Property 4: S-expression parser safety on empty, truncated, and oversized inputs
    allPassed &= rc::check("KiCadSchematicEditor::parseSexp handles empty, truncated, and oversized inputs safely", []() {
        int mode = *rc::gen::inRange(0, 4);
        SexpNode root;
        std::string err;
        if (mode == 0) {
            RC_ASSERT(!KiCadSchematicEditor::parseSexp("", root, err));
            RC_ASSERT(!err.empty());
        } else if (mode == 1) {
            std::string truncated = "(kicad_sch (version 20231120) (symbol (lib_id \"Device:R\")";
            RC_ASSERT(!KiCadSchematicEditor::parseSexp(truncated, root, err));
            RC_ASSERT(!err.empty());
        } else if (mode == 2) {
            std::string unclosedQuote = "(kicad_sch (paper \"A4))";
            RC_ASSERT(!KiCadSchematicEditor::parseSexp(unclosedQuote, root, err));
            RC_ASSERT(!err.empty());
        } else {
            int count = *rc::gen::inRange(50, 200);
            std::string large = "(kicad_sch (version 20231120)";
            for (int i = 0; i < count; ++i) {
                large += " (wire (pts (xy 0 0) (xy 1.27 1.27)))";
            }
            large += ")";
            RC_ASSERT(KiCadSchematicEditor::parseSexp(large, root, err));
            RC_ASSERT(root.head() == "kicad_sch");
        }
    });

    return allPassed ? 0 : 1;
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
