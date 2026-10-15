/*
 * EasyEdaConverter.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EASYEDA_CONVERTER_HXX
#define AIMON_EASYEDA_CONVERTER_HXX

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace aimon {

struct EasyEdaConvertedPin {
    std::string number;
    std::string name;
    double xMm = 0.0;
    double yMm = 0.0;
};

struct EasyEdaConvertedComponent {
    std::string reference;
    std::string value;
    std::string footprint;
    std::string lcsc;
    double xMm = 0.0;
    double yMm = 0.0;
    double rotationDeg = 0.0;
    std::string layer = "F.Cu";
    std::vector<EasyEdaConvertedPin> pins;
};

struct EasyEdaConvertedWire {
    double x1Mm = 0.0;
    double y1Mm = 0.0;
    double x2Mm = 0.0;
    double y2Mm = 0.0;
    std::string net;
};

struct EasyEdaConvertedLabel {
    std::string name;
    double xMm = 0.0;
    double yMm = 0.0;
    bool isPower = false;
};

struct EasyEdaConvertedTrack {
    double x1Mm = 0.0;
    double y1Mm = 0.0;
    double x2Mm = 0.0;
    double y2Mm = 0.0;
    double widthMm = 0.25;
    std::string layer = "F.Cu";
    std::string net;
};

struct EasyEdaConvertedVia {
    double xMm = 0.0;
    double yMm = 0.0;
    double sizeMm = 0.6;
    double drillMm = 0.3;
    std::string net;
};

struct EasyEdaConversionResult {
    bool ok = false;
    std::string format; // "kicad_sch" or "kicad_pcb"
    std::string sexp;
    std::string outputPath;
    std::vector<EasyEdaConvertedComponent> components;
    std::vector<EasyEdaConvertedWire> wires;
    std::vector<EasyEdaConvertedLabel> labels;
    std::vector<EasyEdaConvertedTrack> tracks;
    std::vector<EasyEdaConvertedVia> vias;
    double outlineWidthMm = 0.0;
    double outlineHeightMm = 0.0;
    std::string errorMessage;

    nlohmann::json toJson() const;
};

class EasyEdaConverter {
public:
    static constexpr double kGridStepMm = 1.27; // 50 mils = 1.27 mm
    static constexpr double kEasyEdaUnitToMm = 0.254; // 10 mils = 0.254 mm

    static double snapTo50MilGrid(double valMm);
    static double easyEdaUnitsToMmSnapped(double units);

    static EasyEdaConversionResult convertJson(const nlohmann::json& easyEdaDoc,
                                               const std::string& targetFormat = "auto",
                                               const std::string& outputPath = "");

    static EasyEdaConversionResult convertFile(const std::string& inputPath,
                                               const std::string& outputPath = "",
                                               const std::string& targetFormat = "auto");

    static nlohmann::json executeTool(const nlohmann::json& arguments);
};

} // namespace aimon

#endif // AIMON_EASYEDA_CONVERTER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
