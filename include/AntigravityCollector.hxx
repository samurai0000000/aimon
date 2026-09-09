/*
 * AntigravityCollector.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_ANTIGRAVITY_COLLECTOR_HXX
#define AIMON_ANTIGRAVITY_COLLECTOR_HXX

#include <string>
#include <vector>
#include "Models.hxx"
#include "ConfigManager.hxx"

namespace aimon {

class AntigravityCollector {
public:
    explicit AntigravityCollector(const AntigravityConfig& config);

    AntigravityStatus fetchStatus();

    bool discoverProcess(int& outPort, std::string& outCsrf);

private:
    std::vector<int> findCandidateListeningPorts(pid_t pid);
    bool probePort(int port, const std::string& csrfToken);

    AntigravityConfig _config;
    int _cachedPort = 0;
    std::string _cachedCsrf;
};

} // namespace aimon

#endif // AIMON_ANTIGRAVITY_COLLECTOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
