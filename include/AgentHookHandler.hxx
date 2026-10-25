/*
 * AgentHookHandler.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_AGENT_HOOK_HANDLER_HXX
#define AIMON_AGENT_HOOK_HANDLER_HXX

#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

namespace aimon {

class AgentHookHandler {
public:
    // Process a hook invocation from CLI args and stdin JSON.
    // Always outputs valid JSON ("{}\n" or "{\"decision\":\"allow\"}\n") to outStream and returns 0.
    static int run(int argc, char* argv[], std::istream& inStream, std::ostream& outStream,
                   const std::string& customStateDir = "",
                   const std::string& customDbPath = "");

    // Process a single parsed hook payload directly (used by CppUTest).
    static bool processPayload(const nlohmann::json& payload,
                               const std::string& forcedProvider = "",
                               const std::string& eventKindOverride = "",
                               const std::string& customStateDir = "",
                               const std::string& customDbPath = "");

    // Inspect /proc/<pid>/cmdline to determine if pid is an interactive standalone CLI
    // (cursor-agent, agent, or claude without -p / --print, excluding IDE workers & daemons).
    static bool isInteractiveCliProcess(int pid, const std::string& provider,
                                        std::string* outScreenSession = nullptr);
};

} // namespace aimon

#endif // AIMON_AGENT_HOOK_HANDLER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
