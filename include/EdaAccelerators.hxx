/*
 * EdaAccelerators.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_ACCELERATORS_HXX
#define AIMON_EDA_ACCELERATORS_HXX

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"

namespace aimon {

struct AcceleratorCapabilities {
    bool enabled = true;
    bool gpuAvailable = false;
    std::string backend = "cpu_cpp17"; // "opencl_gpu" | "cpu_cpp17"
    std::string deviceName = "CPU C++17 Solver";
    std::string vendor = "Host CPU";
    std::string driverVersion;
    int computeUnits = 1;
    int clockMhz = 0;
    uint64_t globalMemBytes = 0;
    uint64_t maxAllocBytes = 0;
    bool fp16Supported = false;
    bool fp64Supported = false;
    int cpuThreads = 1;
    int64_t capabilityScore = 100;
    std::vector<std::string> gpuAcceleratedTools;

    int64_t scoreForTool(const std::string& toolName) const;
    nlohmann::json toJson() const;
    static AcceleratorCapabilities fromJson(const nlohmann::json& j);
};

class EdaAccelerators {
public:
    explicit EdaAccelerators(const EdaAcceleratorConfig& config = EdaAcceleratorConfig());

    void setConfig(const EdaAcceleratorConfig& config);
    const EdaAcceleratorConfig& getConfig() const {
        return _config;
    }

    static bool isAcceleratorTool(const std::string& toolName);
    static bool isGpuAccelerableTool(const std::string& toolName);
    static AcceleratorCapabilities probeSystemCapabilities(bool enabled = true);

    const AcceleratorCapabilities& getCapabilities() const {
        return _capabilities;
    }
    void setSimulatedCapabilitiesForTest(const AcceleratorCapabilities& caps);

    nlohmann::json executeTool(const std::string& toolName,
                               const nlohmann::json& args,
                               bool& outSuccess,
                               std::string& outSummary) const;

    nlohmann::json calcSchematicLayout(const nlohmann::json& args,
                                       bool& outSuccess,
                                       std::string& outSummary) const;

    nlohmann::json calcPcbPlacement(const nlohmann::json& args,
                                    bool& outSuccess,
                                    std::string& outSummary) const;

    nlohmann::json calcPcbAutoroute(const nlohmann::json& args,
                                    bool& outSuccess,
                                    std::string& outSummary) const;

    nlohmann::json calcElectricalSynth(const nlohmann::json& args,
                                       bool& outSuccess,
                                       std::string& outSummary) const;

    nlohmann::json calcRobotKinematicsStatics(const nlohmann::json& args,
                                              bool& outSuccess,
                                              std::string& outSummary) const;

    nlohmann::json calcMechanismSynth(const nlohmann::json& args,
                                      bool& outSuccess,
                                      std::string& outSummary) const;

    nlohmann::json calcPrintOrientationDfm(const nlohmann::json& args,
                                           bool& outSuccess,
                                           std::string& outSummary) const;

    nlohmann::json calcPcbConnectivityAudit(const nlohmann::json& args,
                                            bool& outSuccess,
                                            std::string& outSummary) const;

    nlohmann::json calcSchematicErcAudit(const nlohmann::json& args,
                                         bool& outSuccess,
                                         std::string& outSummary) const;

private:
    void attachProvenance(nlohmann::json& res, const std::string& toolName) const;

    EdaAcceleratorConfig _config;
    AcceleratorCapabilities _capabilities;
    bool _hasSimulatedCaps = false;
};

} // namespace aimon

#endif // AIMON_EDA_ACCELERATORS_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
