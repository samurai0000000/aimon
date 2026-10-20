/*
 * EdaBridge.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_BRIDGE_HXX
#define AIMON_EDA_BRIDGE_HXX

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"
#include "Models.hxx"
#include "EdaAccelerators.hxx"

namespace aimon {

class TcpGateway;

class EdaBridge {
public:
    explicit EdaBridge(const EdaBridgeConfig& config = EdaBridgeConfig());
    ~EdaBridge() = default;

    void setConfig(const EdaBridgeConfig& config);
    EdaBridgeConfig getConfig() const;

    void attachGateway(TcpGateway* gateway);
    void attachTcpGateway(TcpGateway* gateway) { attachGateway(gateway); }

    static bool validatePathSafety(const std::string& pathStr,
                                   std::string& outError);

    static bool validateJsonPaths(nlohmann::json& payload,
                                  std::string& outError);

    static bool guardFreecadScript(const std::string& scriptCode,
                                   std::string& outReason);

    static bool guardFreecadApiCall(const std::string& moduleName,
                                    const std::string& methodName,
                                    std::string& outReason);

    static bool guardEasyEdaApiCall(const std::string& apiPath,
                                    std::string& outReason);

    static bool normalizeEasyEdaNetlistArgs(const std::string& apiPath,
                                            nlohmann::json& argsArray,
                                            std::string& outError);

    static std::string componentForTool(const std::string& toolName);

    void recordToolCall(const std::string& component,
                        const std::string& toolName,
                        const std::string& targetDocument,
                        int durationMs,
                        bool success,
                        const std::string& summary,
                        const std::string& errorMessage = "");

    EdaBridgeStatus getStatus() const;
    EdaBridgeStatus probeNow();

    nlohmann::json callTool(const std::string& toolName,
                            const nlohmann::json& arguments);

    const EdaAccelerators& getAccelerators() const {
        return _accelerators;
    }

private:
    void initRuntimeToolsLocked();
    EdaToolRuntimeInfo* findToolInfoLocked(const std::string& component);

    // Zero-token live PCB autoroute: eda_calc_pcb_autoroute(from_active_pcb / pcb_state)
    // and easyeda_edit_pcb(autoroute: true). Fetches the live PCB (or uses pcb_state),
    // audits connectivity, routes the unrouted nets, optionally commits and re-audits.
    nlohmann::json runLiveAutoroute(const std::string& toolName,
                                    const nlohmann::json& arguments);

    mutable std::mutex _mutex;
    mutable std::recursive_mutex _easyedaRpcMutex;
    EdaBridgeConfig _config;
    EdaAccelerators _accelerators;
    TcpGateway* _gateway = nullptr;

    bool _connected = false;
    std::string _activeTransport = "none";
    int64_t _lastProbeEpoch = 0;
    int _lastLatencyMs = -1;
    std::string _remotePlatform;
    std::string _lastError;

    std::vector<EdaToolRuntimeInfo> _toolInfos;
    std::deque<EdaToolCallLogEntry> _recentCalls;
    std::map<std::string, nlohmann::json> _cachedSchematicComponentsBySheet;
    std::map<std::string, bool> _cachedPinNoConnect;
};

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
