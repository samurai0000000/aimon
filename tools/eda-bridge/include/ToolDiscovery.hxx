/*
 * ToolDiscovery.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_BRIDGE_TOOL_DISCOVERY_HXX
#define AIMON_EDA_BRIDGE_TOOL_DISCOVERY_HXX

#include <string>
#include <vector>

namespace aimon {

struct DiscoveredToolInfo {
    std::string name; // "kicad", "freecad", "easyeda"
    bool available = false;
    std::string binaryPath;
    std::string version;
    std::string symbolLibDir;
    std::string footprintLibDir;
    std::string errorMessage;
};

class ToolDiscovery {
public:
    static DiscoveredToolInfo discoverKiCad(const std::string& customBinaryPath = "",
                                            const std::string& customSymbolDir = "",
                                            const std::string& customFootprintDir = "");

    static DiscoveredToolInfo discoverFreeCad(const std::string& customBinaryPath = "");

    static DiscoveredToolInfo discoverEasyEda(const std::string& customBinaryPath = "");
};

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_TOOL_DISCOVERY_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
