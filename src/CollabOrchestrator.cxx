/*
 * CollabOrchestrator.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "CollabOrchestrator.hxx"
#include "PathUtils.hxx"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>

namespace aimon {

static std::string currentIsoTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%H:%M:%S%z");
    std::string s = ss.str();
    if (s.size() >= 5 && (s[s.size() - 5] == '+' || s[s.size() - 5] == '-')) {
        s.insert(s.size() - 2, ":");
    }
    return s;
}

CollabOrchestrator::CollabOrchestrator(AgentRunner& runner, ConfigManager& configMgr)
    : _runner(runner), _configMgr(configMgr) {
    _autoDrive = _configMgr.getConfig().collaboration.autoDrive;
}

CollabOrchestrator::~CollabOrchestrator() {
    stop();
}

void CollabOrchestrator::start() {
    if (_running) return;
    _running = true;
    _workerThread = std::thread(&CollabOrchestrator::orchestratorLoop, this);
}

void CollabOrchestrator::stop() {
    if (!_running) return;
    _running = false;
    _cv.notify_all();
    if (_workerThread.joinable()) {
        _workerThread.join();
    }
}

void CollabOrchestrator::setAutoDrive(bool on) {
    _autoDrive = on;
    _cv.notify_all();
}

bool CollabOrchestrator::isAutoDrive() const {
    return _autoDrive.load();
}

bool CollabOrchestrator::stepTurn(std::string* outError) {
    auto s = AgentMessageBus::getInstance().getActiveCollaboration();
    if (s.status != CollaborationStatus::IN_PROGRESS) {
        if (outError) *outError = "No active collaboration session in progress to step.";
        return false;
    }
    _stepRequested = true;
    _cv.notify_all();
    return true;
}

bool CollabOrchestrator::runUntilConsensus(const std::string& planFile,
                                         const std::string& initiatorId,
                                         const std::string& reviewerId,
                                         std::string* outError) {
    auto s = AgentMessageBus::getInstance().getActiveCollaboration();
    if (s.status != CollaborationStatus::IN_PROGRESS) {
        if (planFile.empty()) {
            if (outError) *outError = "No active session in progress. Plan file required to start.";
            return false;
        }
        std::string init = initiatorId.empty() ? "agent-antigravity-builder" : initiatorId;
        std::string rev = reviewerId.empty() ? "agent-cursor-windows" : reviewerId;
        if (!AgentMessageBus::getInstance().startCollaboration(planFile, init, rev, outError)) {
            return false;
        }
    }
    _autoDrive = true;
    _runContinuous = true;
    _cv.notify_all();
    return true;
}

bool CollabOrchestrator::abortRun(std::string* outError) {
    _autoDrive = false;
    _runContinuous = false;
    _stepRequested = false;
    bool ok = AgentMessageBus::getInstance().abortCollaboration(outError);
    _cv.notify_all();
    return ok;
}

nlohmann::json CollabOrchestrator::statusJson() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return {
        {"auto_drive", _autoDrive.load()},
        {"is_executing", _isExecutingProxy},
        {"executing_actor", _currentExecutingActor},
        {"executing_turn", _currentExecutingTurn},
        {"turn_start_time_epoch", _turnStartTimeEpoch},
        {"last_summary", _lastRunSummary}
    };
}

std::string CollabOrchestrator::buildReviewerPrompt(const CollaborationSession& session) const {
    std::stringstream ss;
    ss << "You are the Reviewer (role: Reviewer) in an autonomous collaboration.\n"
       << "Target plan file: " << session.planFile << "\n"
       << "Current turn: " << (session.currentTurn + 1) << "\n\n"
       << "CRITICAL CONSTRAINT:\n"
       << "You are running in strict READ-ONLY mode. DO NOT edit or modify the plan file on disk.\n"
       << "Your entire output must be printed to stdout.\n\n"
       << "REQUIRED FORMAT:\n"
       << "The very FIRST LINE of your response must be exactly one of:\n"
       << "VERDICT: CONSENSUS\n"
       << "or\n"
       << "VERDICT: REVISE\n\n"
       << "Following that first line, provide your itemized architectural critique:\n"
       << "- Review the plan body and verify all requirements, edge cases, risks, and interface designs.\n"
       << "- If the plan is completely sound and ready for implementation, state VERDICT: CONSENSUS.\n"
       << "- If revisions are needed, state VERDICT: REVISE and detail actionable feedback for the initiator.\n";
    return ss.str();
}

std::string CollabOrchestrator::buildInitiatorPrompt(const CollaborationSession& session) const {
    std::stringstream ss;
    ss << "You are the Initiator (role: Initiator) in an autonomous collaboration.\n"
       << "Target plan file: " << session.planFile << "\n"
       << "Current turn: " << (session.currentTurn + 1) << "\n\n"
       << "INSTRUCTIONS:\n"
       << "1. Inspect the reviewer feedback and critique under '## Collaboration'.\n"
       << "2. Rework the plan body above '## Collaboration' to address all feedback items.\n"
       << "3. Ensure '## License & Copyright' remains intact above '## Collaboration'.\n"
       << "4. Append your Turn " << (session.currentTurn + 1) << " entry under '## Collaboration' adhering strictly to the four-line contract:\n"
       << "   ### Turn " << (session.currentTurn + 1) << ": <Your Model> (Initiator)\n"
       << "   - **Turn Started**: <ISO Timestamp>\n"
       << "   - **Model**: <Model Identifier>\n"
       << "   - **Role**: Initiator\n"
       << "   - **Re-work & Responses**:\n"
       << "     <Itemized summary of updates>\n"
       << "   - **Turn Finished**: <ISO Timestamp>\n"
       << "5. Save the file and exit.\n";
    return ss.str();
}

void CollabOrchestrator::deferToDesktop(const std::string& actorId, const std::string& reason) {
    int backoff = _configMgr.getConfig().collaboration.runnerBackoffSeconds;
    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _runnerBackoffUntil[actorId] = nowEpoch + backoff;
        _lastRunSummary = "Proxy for " + actorId + " deferred to desktop: " + reason;
    }

    AgentMessageBus::getInstance().reportRunnerUnavailable(
        "Proxy unavailable for " + actorId + " (" + reason +
        "). Turn is still open; a desktop agent may take it via agent_wait_turn. "
        "Retrying proxy in " + std::to_string(backoff) + "s.");
}

bool CollabOrchestrator::isRunnerBackedOff(const std::string& actorId) const {
    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _runnerBackoffUntil.find(actorId);
    return it != _runnerBackoffUntil.end() && nowEpoch < it->second;
}

bool CollabOrchestrator::executeReviewerTurn(const CollaborationSession& session) {
    std::string startIso = currentIsoTimestamp();
    int turnToExecute = session.currentTurn + 1;

    std::string quotaReason;
    if (!_runner.checkQuotaOk(session.nextActorId, quotaReason)) {
        deferToDesktop(session.nextActorId, quotaReason);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _isExecutingProxy = true;
        _currentExecutingActor = session.nextActorId;
        _currentExecutingTurn = turnToExecute;
        _turnStartTimeEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        _lastRunSummary = "Spawning reviewer proxy for " + session.nextActorId;
    }

    AgentRunSpec spec;
    spec.agentId = session.nextActorId;
    spec.planFile = session.planFile;
    spec.turnNumber = turnToExecute;
    spec.role = "Reviewer";
    spec.prompt = buildReviewerPrompt(session);
    spec.workspace = ".";
    spec.writeEnabled = false; // Structural read-only guarantee
    spec.timeoutSec = _runner.timeoutForAgent(
        session.nextActorId, _configMgr.getConfig().collaboration.readTimeoutSeconds);

    AgentRunResult res = _runner.run(spec);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _isExecutingProxy = false;
    }

    if (!res.ok) {
        std::string unavailReason;
        if (AgentRunner::isUnavailabilityFailure(res, unavailReason)) {
            deferToDesktop(session.nextActorId, unavailReason);
            return false;
        }
        std::string err = res.timedOut ? "Reviewer proxy timed out" :
                          ("Reviewer proxy failed with exit code " + std::to_string(res.exitCode) + ": " + res.stderrText);
        AgentMessageBus::getInstance().failCollaboration(err);
        return false;
    }

    std::string finishIso = currentIsoTimestamp();

    // Parse verdict from stdout
    std::string verdict = "IN_PROGRESS";
    std::string critiqueBody = res.stdoutText;

    std::istringstream stream(res.stdoutText);
    std::string firstLine;
    if (std::getline(stream, firstLine)) {
        if (firstLine.find("VERDICT: CONSENSUS") != std::string::npos ||
            firstLine.find("CONSENSUS_REACHED") != std::string::npos) {
            verdict = "CONSENSUS_REACHED";
        }
        size_t restPos = res.stdoutText.find('\n');
        if (restPos != std::string::npos) {
            critiqueBody = res.stdoutText.substr(restPos + 1);
        }
    }

    // Format turn block
    std::stringstream block;
    block << "### Turn " << turnToExecute << ": " << session.nextActorId << " (Reviewer)\n"
          << "- **Turn Started**: " << startIso << "\n"
          << "- **Model**: " << session.nextActorId << "\n"
          << "- **Role**: Reviewer\n"
          << "- **Feedback & Critique**:\n"
          << "  " << critiqueBody << "\n"
          << "- **Status**: " << verdict << "\n"
          << "- **Turn Finished**: " << finishIso << "\n";

    std::string appendErr;
    if (!AgentMessageBus::getInstance().appendCollaborationTurnBlock(session.planFile, block.str(), &appendErr)) {
        AgentMessageBus::getInstance().failCollaboration("Failed to append turn block: " + appendErr);
        return false;
    }

    nlohmann::json outResult;
    std::string signalErr;
    if (!AgentMessageBus::getInstance().signalCollaborationTurn(
            session.planFile,
            turnToExecute,
            session.nextActorId,
            session.nextActorId,
            verdict,
            "Proxy completed Reviewer turn " + std::to_string(turnToExecute),
            outResult,
            &signalErr)) {
        AgentMessageBus::getInstance().failCollaboration("Reviewer turn signal failed: " + signalErr);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastRunSummary = "Reviewer turn " + std::to_string(turnToExecute) + " completed (" + verdict + ")";
    }

    return true;
}

bool CollabOrchestrator::executeInitiatorTurn(const CollaborationSession& session) {
    std::string startIso = currentIsoTimestamp();
    int turnToExecute = session.currentTurn + 1;

    std::string quotaReason;
    if (!_runner.checkQuotaOk(session.nextActorId, quotaReason)) {
        deferToDesktop(session.nextActorId, quotaReason);
        return false;
    }

    // Snapshot plan file before write turn
    std::string bakPath, snapErr;
    if (!AgentMessageBus::getInstance().snapshotPlanFile(session.planFile, turnToExecute, &bakPath, &snapErr)) {
        AgentMessageBus::getInstance().failCollaboration("Pre-turn snapshot failed: " + snapErr);
        return false;
    }

    int retriesLeft = _configMgr.getConfig().collaboration.maxRetriesPerTurn;
    std::string prompt = buildInitiatorPrompt(session);

    while (true) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _isExecutingProxy = true;
            _currentExecutingActor = session.nextActorId;
            _currentExecutingTurn = turnToExecute;
            _turnStartTimeEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _lastRunSummary = "Spawning initiator proxy for " + session.nextActorId;
        }

        AgentRunSpec spec;
        spec.agentId = session.nextActorId;
        spec.planFile = session.planFile;
        spec.turnNumber = turnToExecute;
        spec.role = "Initiator";
        spec.prompt = prompt;
        spec.workspace = ".";
        spec.writeEnabled = true;
        spec.timeoutSec = _runner.timeoutForAgent(
            session.nextActorId, _configMgr.getConfig().collaboration.writeTimeoutSeconds);

        AgentRunResult res = _runner.run(spec);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _isExecutingProxy = false;
        }

        if (!res.ok) {
            AgentMessageBus::getInstance().restorePlanFileSnapshot(session.planFile, turnToExecute);
            std::string unavailReason;
            if (AgentRunner::isUnavailabilityFailure(res, unavailReason)) {
                deferToDesktop(session.nextActorId, unavailReason);
                return false;
            }
            if (retriesLeft > 0) {
                retriesLeft--;
                prompt += "\n\nWARNING: Previous execution failed or timed out: " + res.stderrText + "\nPlease try again.";
                continue;
            }
            AgentMessageBus::getInstance().failCollaboration("Initiator proxy execution failed: " + res.stderrText);
            return false;
        }

        // Validate on disk via signalCollaborationTurn
        nlohmann::json outResult;
        std::string signalErr;
        bool ok = AgentMessageBus::getInstance().signalCollaborationTurn(
            session.planFile,
            turnToExecute,
            session.nextActorId,
            session.nextActorId,
            "IN_PROGRESS",
            "Proxy completed Initiator turn " + std::to_string(turnToExecute),
            outResult,
            &signalErr);

        if (!ok) {
            auto currentSession = AgentMessageBus::getInstance().getActiveCollaboration();
            if (currentSession.currentTurn >= turnToExecute) {
                // The turn has already been successfully advanced (e.g. by a previous attempt or concurrent signal),
                // so we do not need to treat this as an error. Break out of the retry loop.
                break;
            }
            AgentMessageBus::getInstance().restorePlanFileSnapshot(session.planFile, turnToExecute);
            if (retriesLeft > 0) {
                retriesLeft--;
                prompt += "\n\nCRITICAL CONTRACT ERROR: " + signalErr + "\nPlease fix and ensure all requirements are met.";
                continue;
            }
            AgentMessageBus::getInstance().failCollaboration("Initiator turn rejected: " + signalErr);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _lastRunSummary = "Initiator turn " + std::to_string(turnToExecute) + " completed";
        }

        break;
    }

    return true;
}

bool CollabOrchestrator::executeCurrentTurn(bool bypassWaiterGrace) {
    auto session = AgentMessageBus::getInstance().getActiveCollaboration();
    if (session.status != CollaborationStatus::IN_PROGRESS) {
        return false;
    }
    if (!session.latchedTurnReady || session.nextActorId == "NONE") {
        return false;
    }

    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // The proxy drives every turn. A desktop agent only keeps the turn while it
    // is actually blocked in agent_wait_turn, and only up to the grace SLA; an
    // idle IDE must never stall the session.
    if (!bypassWaiterGrace &&
        AgentMessageBus::getInstance().hasLiveWaiter(session.nextActorId)) {
        int grace = _configMgr.getConfig().collaboration.liveWaiterGraceSeconds;
        if (nowEpoch - session.lastTurnEpoch < grace) {
            return false;
        }
    }

    if (isRunnerBackedOff(session.nextActorId)) {
        return false;
    }

    if (!_runner.isAvailable(session.nextActorId)) {
        deferToDesktop(session.nextActorId, "no runner command available");
        return false;
    }

    bool isReviewer = (session.nextActorId == session.reviewerId);
    if (isReviewer) {
        return executeReviewerTurn(session);
    } else {
        return executeInitiatorTurn(session);
    }
}

void CollabOrchestrator::orchestratorLoop() {
    while (_running) {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return !_running || _stepRequested;
        });

        if (!_running) break;

        if (_stepRequested) {
            _stepRequested = false;
            lock.unlock();
            executeCurrentTurn(/*bypassWaiterGrace=*/true);
            continue;
        }

        if (_autoDrive) {
            lock.unlock();
            auto session = AgentMessageBus::getInstance().getActiveCollaboration();
            if (session.status == CollaborationStatus::CONSENSUS_REACHED ||
                session.status == CollaborationStatus::OPERATOR_REVIEW) {
                if (_runContinuous) {
                    _autoDrive = false;
                    _runContinuous = false;
                }
            } else if (session.status == CollaborationStatus::IN_PROGRESS) {
                executeCurrentTurn(/*bypassWaiterGrace=*/_runContinuous.load());
            }
        }
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
