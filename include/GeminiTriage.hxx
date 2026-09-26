/*
 * GeminiTriage.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_GEMINI_TRIAGE_HXX
#define AIMON_GEMINI_TRIAGE_HXX

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"

namespace aimon {

struct TriageReport {
    int64_t timestampEpoch = 0;
    std::string rootCauseCategory = "UNKNOWN";
    std::string faultingFile = "";
    int faultingLine = 0;
    std::string diagnosisText = "";
    std::string suggestedFixDiff = "";
    bool success = false;
    std::string rawApiResponse = "";

    nlohmann::json toJson() const;
};

class GeminiTriage {
public:
    GeminiTriage(const GeminiConfig& config);
    ~GeminiTriage() = default;

    bool isEnabled() const {
        return _config.enabled && !_config.apiKey.empty();
    }

    // Set custom endpoint for offline mock testing (e.g. "http://127.0.0.1:8080")
    void setTestEndpoint(const std::string& endpoint) {
        _testEndpoint = endpoint;
    }

    TriageReport analyzeCrash(const std::string& binaryPath,
                              const std::string& coreDumpPath,
                              const std::string& trailingLogText,
                              int exitStatus,
                              int termSignal);

    static std::string extractGdbBacktrace(const std::string& binaryPath,
                                          const std::string& coreDumpPath);

    static std::string parseGdbOutput(const std::string& gdbOutput,
                                      std::string& outFaultingFile,
                                      int& outFaultingLine);

    std::string buildTriagePrompt(const std::string& backtrace,
                                  const std::string& logs,
                                  int exitStatus,
                                  int termSignal) const;

    std::string buildJsonPayload(const std::string& prompt) const;

    bool parseJsonResponse(const std::string& jsonStr, TriageReport& outReport) const;

    bool writeIncidentLog(const TriageReport& report, const std::string& customDir = "") const;

private:
    GeminiConfig _config;
    std::string _testEndpoint;
};

} // namespace aimon

#endif // AIMON_GEMINI_TRIAGE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
