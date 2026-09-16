/*
 * CollabOrchestrator.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_COLLAB_ORCHESTRATOR_HXX
#define AIMON_COLLAB_ORCHESTRATOR_HXX

#include <string>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include "AgentRunner.hxx"
#include "ConfigManager.hxx"
#include "AgentMessageBus.hxx"

namespace aimon {

class CollabOrchestrator {
public:
    CollabOrchestrator(AgentRunner& runner, ConfigManager& configMgr);
    ~CollabOrchestrator();

    void start();
    void stop();

    void setAutoDrive(bool on);
    bool isAutoDrive() const;

    bool stepTurn(std::string* outError = nullptr);
    bool runUntilConsensus(const std::string& planFile,
                           const std::string& initiatorId = "",
                           const std::string& reviewerId = "",
                           std::string* outError = nullptr);
    bool abortRun(std::string* outError = nullptr);

    nlohmann::json statusJson() const;

private:
    void orchestratorLoop();
    bool executeCurrentTurn(bool bypassWaiterGrace);
    bool executeInitiatorTurn(const CollaborationSession& session);
    bool executeReviewerTurn(const CollaborationSession& session);

    std::string buildInitiatorPrompt(const CollaborationSession& session) const;
    std::string buildReviewerPrompt(const CollaborationSession& session) const;

    // Parks the proxy for this actor without touching the session, so the turn
    // stays available to a desktop agent instead of dying with the runner.
    void deferToDesktop(const std::string& actorId, const std::string& reason);
    bool isRunnerBackedOff(const std::string& actorId) const;

    AgentRunner& _runner;
    ConfigManager& _configMgr;

    std::atomic<bool> _running{false};
    std::atomic<bool> _autoDrive{false};
    std::atomic<bool> _stepRequested{false};
    std::atomic<bool> _runContinuous{false};

    std::thread _workerThread;
    mutable std::mutex _mutex;
    std::condition_variable _cv;

    // Active execution tracking
    bool _isExecutingProxy = false;
    std::string _currentExecutingActor;
    int _currentExecutingTurn = 0;
    int64_t _turnStartTimeEpoch = 0;
    std::string _lastRunSummary;

    std::map<std::string, int64_t> _runnerBackoffUntil;
};

} // namespace aimon

#endif // AIMON_COLLAB_ORCHESTRATOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
