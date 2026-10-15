/*
 * KiCadSchematicEditor.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_KICAD_SCHEMATIC_EDITOR_HXX
#define AIMON_KICAD_SCHEMATIC_EDITOR_HXX

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace aimon {

struct SexpNode {
    bool isList = false;
    std::string atom;
    std::vector<SexpNode> children;

    const SexpNode* findChild(const std::string& headAtom) const;
    std::vector<const SexpNode*> findChildren(const std::string& headAtom) const;
    std::string head() const;
};

class KiCadSchematicEditor {
public:
    static constexpr double kGridStepMm = 1.27; // 50 mils = 1.27 mm

    static double snapTo50MilGrid(double valMm);

    static bool parseSexp(const std::string& text, SexpNode& outRoot, std::string& outError);

    static nlohmann::json searchLibraries(const nlohmann::json& arguments);
    static nlohmann::json inspectFile(const nlohmann::json& arguments);
    static nlohmann::json applySchematicEdits(const nlohmann::json& arguments);
    static nlohmann::json syncSchematicToPcb(const nlohmann::json& arguments);
    static nlohmann::json applyPcbEdits(const nlohmann::json& arguments);
};

} // namespace aimon

#endif // AIMON_KICAD_SCHEMATIC_EDITOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
