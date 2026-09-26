/*
 * GeminiTriage.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <filesystem>
#include <array>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include "GeminiTriage.hxx"
#include "PathUtils.hxx"

namespace aimon {

nlohmann::json TriageReport::toJson() const {
    return {
        {"timestamp_epoch", timestampEpoch},
        {"root_cause_category", rootCauseCategory},
        {"faulting_file", faultingFile},
        {"faulting_line", faultingLine},
        {"diagnosis_text", diagnosisText},
        {"suggested_fix_diff", suggestedFixDiff},
        {"success", success},
        {"raw_api_response", rawApiResponse}
    };
}

GeminiTriage::GeminiTriage(const GeminiConfig& config)
    : _config(config) {
}

TriageReport GeminiTriage::analyzeCrash(const std::string& binaryPath,
                                       const std::string& coreDumpPath,
                                       const std::string& trailingLogText,
                                       int exitStatus,
                                       int termSignal) {
    TriageReport report;
    report.timestampEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Strict dormancy check: must exit immediately if disabled or key missing
    if (!isEnabled()) {
        report.success = false;
        report.diagnosisText = "Gemini triage disabled or API key empty";
        return report;
    }

    std::string backtrace = extractGdbBacktrace(binaryPath, coreDumpPath);
    parseGdbOutput(backtrace, report.faultingFile, report.faultingLine);

    std::string prompt = buildTriagePrompt(backtrace, trailingLogText, exitStatus, termSignal);
    std::string payload = buildJsonPayload(prompt);

    std::string responseBody;
    int httpStatusCode = 0;

    if (!_testEndpoint.empty()) {
        // Mock offline testing endpoint (e.g. http://127.0.0.1:port)
        std::string host = _testEndpoint;
        int port = 80;

        if (host.rfind("http://", 0) == 0) {
            host = host.substr(7);
        }
        auto colonPos = host.find(':');
        if (colonPos != std::string::npos) {
            port = std::stoi(host.substr(colonPos + 1));
            host = host.substr(0, colonPos);
        }

        httplib::Client cli(host, port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);

        auto res = cli.Post("/v1beta/models/test:generateContent", payload, "application/json");
        if (!res) {
            report.success = false;
            report.diagnosisText = "HTTP mock request failed: connection error";
            return report;
        }
        httpStatusCode = res->status;
        responseBody = res->body;
    } else {
        // Direct HTTPS to Google Gemini Cloud REST API
        httplib::SSLClient cli("generativelanguage.googleapis.com", 443);
        int timeoutSec = (_config.maxTokens > 0) ? 30 : 10;
        cli.set_connection_timeout(timeoutSec);
        cli.set_read_timeout(timeoutSec);

        std::string path = "/v1beta/models/" + _config.model + ":generateContent?key=" + _config.apiKey;
        auto res = cli.Post(path.c_str(), payload, "application/json");
        if (!res) {
            report.success = false;
            report.diagnosisText = "HTTPS request failed: network timeout or connect error";
            return report;
        }
        httpStatusCode = res->status;
        responseBody = res->body;
    }

    if (httpStatusCode != 200) {
        report.success = false;
        report.rawApiResponse = responseBody;
        report.diagnosisText = "Gemini API error (HTTP " + std::to_string(httpStatusCode) + "): " + responseBody;
        return report;
    }

    if (!parseJsonResponse(responseBody, report)) {
        report.success = false;
        report.diagnosisText = "Failed to parse Gemini API JSON response";
        return report;
    }

    report.success = true;
    writeIncidentLog(report);
    return report;
}

std::string GeminiTriage::extractGdbBacktrace(const std::string& binaryPath,
                                             const std::string& coreDumpPath) {
    if (coreDumpPath.empty() || !std::filesystem::exists(coreDumpPath)) {
        return "No core dump available at: " + coreDumpPath;
    }

    std::string cmd = "gdb -batch -ex 'bt 50' " + binaryPath + " " + coreDumpPath + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "Failed to run gdb";
    }

    std::string result;
    std::array<char, 512> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result.append(buffer.data());
    }
    pclose(pipe);

    if (result.empty()) {
        return "GDB backtrace empty";
    }
    return result;
}

std::string GeminiTriage::parseGdbOutput(const std::string& gdbOutput,
                                        std::string& outFaultingFile,
                                        int& outFaultingLine) {
    outFaultingFile.clear();
    outFaultingLine = 0;

    std::istringstream stream(gdbOutput);
    std::string line;
    std::regex atRegex(R"(at\s+([^:\s]+):(\d+))");
    std::smatch match;

    while (std::getline(stream, line)) {
        if (std::regex_search(line, match, atRegex)) {
            outFaultingFile = match[1].str();
            outFaultingLine = std::stoi(match[2].str());
            return outFaultingFile + ":" + std::to_string(outFaultingLine);
        }
    }

    return "";
}

std::string GeminiTriage::buildTriagePrompt(const std::string& backtrace,
                                           const std::string& logs,
                                           int exitStatus,
                                           int termSignal) const {
    std::ostringstream oss;
    oss << "A daemon crashed. Please diagnose the root cause and provide a fix.\n\n"
        << "CRASH METADATA:\n"
        << "- Exit Status: " << exitStatus << "\n"
        << "- Terminating Signal: " << termSignal << "\n\n"
        << "STACK BACKTRACE:\n" << backtrace << "\n\n"
        << "TRAILING LOGS:\n" << logs << "\n\n"
        << "REQUIRED FORMAT IN RESPONSE:\n"
        << "ROOT_CAUSE_CATEGORY: <SEGFAULT|ABORT|BAD_ALLOC|EXCEPTION|LOGIC_ERROR>\n"
        << "DIAGNOSIS: <detailed diagnosis>\n"
        << "SUGGESTED_FIX:\n```diff\n<unified diff>\n```\n";

    return oss.str();
}

std::string GeminiTriage::buildJsonPayload(const std::string& prompt) const {
    nlohmann::json j;
    j["contents"] = nlohmann::json::array({
        {
            {"parts", nlohmann::json::array({
                {{"text", prompt}}
            })}
        }
    });

    j["generationConfig"] = {
        {"temperature", _config.temperature},
        {"maxOutputTokens", _config.maxTokens > 0 ? _config.maxTokens : 1024}
    };

    return j.dump();
}

bool GeminiTriage::parseJsonResponse(const std::string& jsonStr, TriageReport& outReport) const {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        outReport.rawApiResponse = jsonStr;

        if (!j.contains("candidates") || !j["candidates"].is_array() || j["candidates"].empty()) {
            return false;
        }

        const auto& cand = j["candidates"][0];
        if (!cand.contains("content") || !cand["content"].contains("parts")) {
            return false;
        }

        const auto& parts = cand["content"]["parts"];
        if (!parts.is_array() || parts.empty()) {
            return false;
        }

        std::string text = parts[0].value("text", "");
        outReport.diagnosisText = text;

        // Parse category
        std::regex catRegex(R"(ROOT_CAUSE_CATEGORY:\s*([A-Z_]+))");
        std::smatch match;
        if (std::regex_search(text, match, catRegex)) {
            outReport.rootCauseCategory = match[1].str();
        } else {
            outReport.rootCauseCategory = "UNKNOWN";
        }

        // Parse diff
        std::regex diffRegex(R"(```diff\n([\s\S]*?)```)");
        if (std::regex_search(text, match, diffRegex)) {
            outReport.suggestedFixDiff = match[1].str();
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool GeminiTriage::writeIncidentLog(const TriageReport& report, const std::string& customDir) const {
    std::string dir = customDir;
    if (dir.empty()) {
        dir = _config.incidentLogDir;
    }
    if (dir.empty()) {
        dir = "~/.config/aimon/incidents";
    }

    dir = PathUtils::expandHome(dir);
    try {
        std::filesystem::create_directories(dir);
        std::string filename = dir + "/incident_" + std::to_string(report.timestampEpoch) + ".md";
        std::ofstream ofs(filename);
        if (!ofs.is_open()) {
            return false;
        }

        ofs << "# Aimon Crash Incident Report\n\n"
            << "- **Timestamp**: " << report.timestampEpoch << "\n"
            << "- **Root Cause Category**: " << report.rootCauseCategory << "\n"
            << "- **Faulting File**: " << report.faultingFile << ":" << report.faultingLine << "\n"
            << "- **Triage Success**: " << (report.success ? "YES" : "NO") << "\n\n"
            << "## Diagnosis\n\n"
            << report.diagnosisText << "\n\n";

        if (!report.suggestedFixDiff.empty()) {
            ofs << "## Suggested Fix\n\n```diff\n"
                << report.suggestedFixDiff << "\n```\n";
        }

        ofs.close();
        return true;
    } catch (...) {
        return false;
    }
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
