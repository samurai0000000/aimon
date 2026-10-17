/*
 * EdaAccelerators.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_ACCELERATORS_HXX
#define AIMON_EDA_ACCELERATORS_HXX

#include <string>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"

namespace aimon {

class EdaAccelerators {
public:
    explicit EdaAccelerators(const EdaAcceleratorConfig& config = EdaAcceleratorConfig());

    void setConfig(const EdaAcceleratorConfig& config);
    const EdaAcceleratorConfig& getConfig() const {
        return _config;
    }

    static bool isAcceleratorTool(const std::string& toolName);

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
    EdaAcceleratorConfig _config;
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
