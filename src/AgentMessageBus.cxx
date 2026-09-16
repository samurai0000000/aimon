/*
 * AgentMessageBus.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AgentMessageBus.hxx"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace aimon {

nlohmann::json CollaborationSession::toJson() const {
    return {
        {"plan_file", planFile},
        {"initiator_id", initiatorId},
        {"reviewer_id", reviewerId},
        {"next_actor_id", nextActorId},
        {"current_turn", currentTurn},
        {"max_turns", maxTurns},
        {"status", AgentMessageBus::statusToString(status)},
        {"plan_body_hash", planBodyHash},
        {"last_side_channel_message", lastSideChannelMessage},
        {"last_model", lastModel},
        {"last_turn_epoch", lastTurnEpoch},
        {"last_file_mtime_epoch", lastFileMtime},
        {"latched_turn_ready", latchedTurnReady},
        {"claimed_by", claimedBy},
        {"claim_epoch", claimEpoch},
        {"executor", executor},
        {"last_run_error", lastRunError}
    };
}

AgentMessageBus& AgentMessageBus::getInstance() {
    static AgentMessageBus instance;
    return instance;
}

AgentMessageBus::AgentMessageBus() {
}

std::string AgentMessageBus::generateMessageId() {
    uint64_t num = ++_messageCounter;
    return "msg-" + std::to_string(num);
}

std::string AgentMessageBus::statusToString(CollaborationStatus status) {
    switch (status) {
        case CollaborationStatus::IDLE: return "IDLE";
        case CollaborationStatus::IN_PROGRESS: return "IN_PROGRESS";
        case CollaborationStatus::CONSENSUS_REACHED: return "CONSENSUS_REACHED";
        case CollaborationStatus::OPERATOR_REVIEW: return "OPERATOR_REVIEW";
    }
    return "IDLE";
}

CollaborationStatus AgentMessageBus::stringToStatus(const std::string& str) {
    if (str == "IN_PROGRESS") return CollaborationStatus::IN_PROGRESS;
    if (str == "CONSENSUS_REACHED") return CollaborationStatus::CONSENSUS_REACHED;
    if (str == "OPERATOR_REVIEW") return CollaborationStatus::OPERATOR_REVIEW;
    return CollaborationStatus::IDLE;
}

std::string AgentMessageBus::canonicalAgentId(const std::string& raw) {
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lower.find("cursor") != std::string::npos || lower.find("grok") != std::string::npos) {
        return "agent-cursor-windows";
    }
    if (lower.find("antigravity") != std::string::npos || lower.find("gemini") != std::string::npos || lower.find("builder") != std::string::npos) {
        return "agent-antigravity-builder";
    }
    if (lower.find("operator") != std::string::npos || lower.find("human") != std::string::npos) {
        return "operator";
    }
    return raw;
}

std::string AgentMessageBus::normalizeNewlines(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                i++; // Skip '\r' in CRLF
            }
            result.push_back('\n');
        } else {
            result.push_back(input[i]);
        }
    }
    return result;
}

std::string AgentMessageBus::extractPlanBody(const std::string& fileContent, bool* foundHeading) {
    std::istringstream stream(fileContent);
    std::string line;
    std::string body;
    bool found = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t first = line.find_first_not_of(" \t");
        size_t last = line.find_last_not_of(" \t");
        std::string trimmed = (first == std::string::npos) ? "" : line.substr(first, last - first + 1);

        if (trimmed == "## Collaboration") {
            found = true;
            break;
        }

        body += line;
        body += "\n";
    }

    if (foundHeading) {
        *foundHeading = found;
    }

    if (!found) {
        return fileContent;
    }
    return body;
}

std::string AgentMessageBus::computeSha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

bool AgentMessageBus::validateDocumentContract(const std::string& fileContent, int turnNumber, std::string* outError) {
    bool hasCollabHeading = false;
    std::string body = extractPlanBody(fileContent, &hasCollabHeading);
    (void)body;

    if (!hasCollabHeading) {
        if (outError) *outError = "Missing '## Collaboration' heading in plan document.";
        return false;
    }

    std::string turnHeaderPrefix = "### Turn " + std::to_string(turnNumber);
    size_t turnPos = fileContent.find(turnHeaderPrefix);
    if (turnPos == std::string::npos) {
        if (outError) *outError = "Missing '" + turnHeaderPrefix + ":' section for turn " + std::to_string(turnNumber) + ".";
        return false;
    }

    // Extract lines belonging to this turn section
    size_t nextTurnPos = std::string::npos;
    size_t searchPos = turnPos + turnHeaderPrefix.length();
    while (true) {
        size_t candidate = fileContent.find("### Turn ", searchPos);
        if (candidate == std::string::npos) break;
        size_t digitPos = candidate + 9;
        if (digitPos < fileContent.size() && std::isdigit(static_cast<unsigned char>(fileContent[digitPos]))) {
            nextTurnPos = candidate;
            break;
        }
        searchPos = candidate + 9;
    }
    std::string turnBlock = (nextTurnPos == std::string::npos) ?
                            fileContent.substr(turnPos) :
                            fileContent.substr(turnPos, nextTurnPos - turnPos);

    auto blockContainsNonEmpty = [&](const std::string& marker) -> bool {
        size_t pos = turnBlock.find(marker);
        if (pos == std::string::npos) return false;
        size_t colon = turnBlock.find(':', pos + marker.length() - 1);
        if (colon == std::string::npos) return false;
        size_t endLine = turnBlock.find('\n', colon);
        if (endLine == std::string::npos) endLine = turnBlock.length();
        std::string val = turnBlock.substr(colon + 1, endLine - colon - 1);
        size_t first = val.find_first_not_of(" \t\r");
        return (first != std::string::npos);
    };

    if (!blockContainsNonEmpty("Turn Started")) {
        if (outError) *outError = "Missing or empty 'Turn Started' timestamp in Turn " + std::to_string(turnNumber) + " log.";
        return false;
    }
    if (!blockContainsNonEmpty("Model")) {
        if (outError) *outError = "Missing or empty 'Model' specification in Turn " + std::to_string(turnNumber) + " log.";
        return false;
    }
    if (!blockContainsNonEmpty("Turn Finished")) {
        if (outError) *outError = "Missing or empty 'Turn Finished' timestamp in Turn " + std::to_string(turnNumber) + " log.";
        return false;
    }

    bool hasCritiqueOrFeedback = (turnBlock.find("Critique") != std::string::npos ||
                                  turnBlock.find("Feedback") != std::string::npos ||
                                  turnBlock.find("Re-work") != std::string::npos ||
                                  turnBlock.find("Rework") != std::string::npos ||
                                  turnBlock.find("Summary") != std::string::npos);
    if (!hasCritiqueOrFeedback) {
        if (outError) *outError = "Missing critique, feedback, or re-work content in Turn " + std::to_string(turnNumber) + " log.";
        return false;
    }

    return true;
}

// --- Legacy Chat Methods ---

std::string AgentMessageBus::postMessageToAgent(const std::string& sessionId, const std::string& text) {
    std::string msgId = generateMessageId();
    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    AgentMessage msg;
    msg.messageId = msgId;
    msg.sessionId = sessionId;
    msg.text = text;
    msg.timestampEpoch = nowEpoch;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _inboxes[sessionId].push_back(msg);
    }
    _cv.notify_all();
    return msgId;
}

bool AgentMessageBus::fetchNextMessageForAgent(const std::string& sessionId, int timeoutSec, AgentMessage& outMsg) {
    std::unique_lock<std::mutex> lock(_mutex);

    auto hasMsg = [&]() {
        auto it = _inboxes.find(sessionId);
        if (it != _inboxes.end() && !it->second.empty()) return true;
        auto itAll = _inboxes.find("");
        if (itAll != _inboxes.end() && !itAll->second.empty()) return true;
        return false;
    };

    if (timeoutSec <= 0) {
        _cv.wait(lock, hasMsg);
    } else {
        bool arrived = _cv.wait_for(lock, std::chrono::seconds(timeoutSec), hasMsg);
        if (!arrived) return false;
    }

    auto it = _inboxes.find(sessionId);
    if (it != _inboxes.end() && !it->second.empty()) {
        outMsg = it->second.front();
        it->second.pop_front();
        return true;
    }

    auto itAll = _inboxes.find("");
    if (itAll != _inboxes.end() && !itAll->second.empty()) {
        outMsg = itAll->second.front();
        itAll->second.pop_front();
        return true;
    }

    return false;
}

bool AgentMessageBus::postReplyFromAgent(const std::string& sessionId,
                                        const std::string& messageId,
                                        const std::string& replyText) {
    ReplyCallback cb;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        cb = _replyCallback;
    }
    if (cb) {
        cb(sessionId, messageId, replyText);
        return true;
    }
    return false;
}

void AgentMessageBus::setReplyCallback(ReplyCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _replyCallback = callback;
}

// --- Document-Centric Collaboration Methods ---

bool AgentMessageBus::startCollaboration(const std::string& planFile,
                                         const std::string& initiatorId,
                                         const std::string& reviewerId,
                                         std::string* outError) {
    std::unique_lock<std::mutex> lock(_mutex);

    if (_activeCollabSession.status == CollaborationStatus::IN_PROGRESS ||
        _activeCollabSession.status == CollaborationStatus::OPERATOR_REVIEW) {
        if (outError) {
            *outError = "COLLABORATION_ERROR: A collaboration session is already active for " +
                        _activeCollabSession.planFile + ". Abort or finish it before starting a new one.";
        }
        return false;
    }

    std::string canInit = canonicalAgentId(initiatorId);
    std::string canRev = canonicalAgentId(reviewerId);
    if (canInit == canRev) {
        if (outError) *outError = "COLLABORATION_ERROR: initiator_id and reviewer_id must be different agents.";
        return false;
    }

    if (!fs::exists(planFile)) {
        if (outError) *outError = "COLLABORATION_ERROR: Plan file not found: " + planFile;
        return false;
    }

    std::ifstream file(planFile);
    if (!file.is_open()) {
        if (outError) *outError = "COLLABORATION_ERROR: Unable to open plan file: " + planFile;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    bool foundHeading = false;
    std::string body = extractPlanBody(content, &foundHeading);
    std::string hash = computeSha256(normalizeNewlines(body));

    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto ftime = fs::last_write_time(planFile);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    int64_t mtimeEpoch = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();

    _activeCollabSession = CollaborationSession();
    _activeCollabSession.planFile = planFile;
    _activeCollabSession.initiatorId = canInit;
    _activeCollabSession.reviewerId = canRev;
    _activeCollabSession.currentTurn = 0;
    _activeCollabSession.nextActorId = canInit;
    _activeCollabSession.maxTurns = 8;
    _activeCollabSession.status = CollaborationStatus::IN_PROGRESS;
    _activeCollabSession.planBodyHash = hash;
    _activeCollabSession.latchedTurnReady = true;
    _activeCollabSession.lastTurnEpoch = nowEpoch;
    _activeCollabSession.lastFileMtime = mtimeEpoch;
    _activeCollabSession.lastSideChannelMessage = "Collaboration started by operator. Turn 1 ready for " + canInit;

    CollaborationCallback cb = _collabCallback;
    CollaborationSession snap = _activeCollabSession;
    _collabCv.notify_all();
    lock.unlock();

    if (cb) {
        cb(snap, "collaboration_started");
    }
    return true;
}

bool AgentMessageBus::signalCollaborationTurn(const std::string& planFile,
                                             int turnNumber,
                                             const std::string& agentId,
                                             const std::string& agentModel,
                                             const std::string& status,
                                             const std::string& sideChannelMessage,
                                             nlohmann::json& outResult,
                                             std::string* outError) {
    std::unique_lock<std::mutex> lock(_mutex);

    if (_activeCollabSession.status != CollaborationStatus::IN_PROGRESS) {
        if (outError) *outError = "COLLABORATION_ERROR: No collaboration session in progress.";
        return false;
    }
    if (_activeCollabSession.planFile != planFile) {
        if (outError) *outError = "COLLABORATION_ERROR: plan_file does not match active session (" + _activeCollabSession.planFile + ").";
        return false;
    }

    std::string canAgent = canonicalAgentId(agentId);
    if (canAgent != _activeCollabSession.initiatorId && canAgent != _activeCollabSession.reviewerId) {
        if (outError) *outError = "COLLABORATION_ERROR: Agent " + canAgent + " is not a participant in this collaboration session.";
        return false;
    }
    if (canAgent != _activeCollabSession.nextActorId) {
        if (outError) *outError = "COLLABORATION_ERROR: It is not " + canAgent + "'s turn. Expected: " + _activeCollabSession.nextActorId;
        return false;
    }
    if (turnNumber != _activeCollabSession.currentTurn + 1) {
        if (outError) *outError = "COLLABORATION_ERROR: Turn number sequence mismatch. Expected: " +
                                  std::to_string(_activeCollabSession.currentTurn + 1) + ", got: " + std::to_string(turnNumber);
        return false;
    }

    if (!fs::exists(planFile)) {
        if (outError) *outError = "COLLABORATION_ERROR: Plan file not found on disk: " + planFile;
        return false;
    }
    std::ifstream file(planFile);
    if (!file.is_open()) {
        if (outError) *outError = "COLLABORATION_ERROR: Unable to read plan file: " + planFile;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::string docErr;
    if (!validateDocumentContract(content, turnNumber, &docErr)) {
        if (outError) *outError = "COLLABORATION_ERROR: Document contract failed: " + docErr;
        return false;
    }

    bool isEven = (turnNumber % 2 == 0);
    bool foundHeading = false;
    std::string body = extractPlanBody(content, &foundHeading);
    std::string bodyHash = computeSha256(normalizeNewlines(body));

    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto ftime = fs::last_write_time(planFile);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    int64_t mtimeEpoch = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();

    if (isEven) {
        // Reviewer turn: check body hash
        if (bodyHash != _activeCollabSession.planBodyHash) {
            if (outError) {
                *outError = "COLLABORATION_ERROR: Reviewer is forbidden from modifying the plan body. Plan body SHA-256 mismatch.";
            }
            return false;
        }

        if (status == "CONSENSUS_REACHED") {
            _activeCollabSession.status = CollaborationStatus::CONSENSUS_REACHED;
            _activeCollabSession.currentTurn = turnNumber;
            _activeCollabSession.nextActorId = "NONE";
            _activeCollabSession.latchedTurnReady = true;
            _activeCollabSession.lastSideChannelMessage = sideChannelMessage;
            _activeCollabSession.lastModel = agentModel;
            _activeCollabSession.lastTurnEpoch = nowEpoch;
            _activeCollabSession.lastFileMtime = mtimeEpoch;

            outResult = {
                {"status", "consensus_reached"},
                {"final_turn", turnNumber},
                {"message", "Consensus reached. Return to normal prompt mode."}
            };

            CollaborationCallback cb = _collabCallback;
            CollaborationSession snap = _activeCollabSession;
            _collabCv.notify_all();
            lock.unlock();

            if (cb) {
                cb(snap, "collaboration_turn");
            }
            return true;
        }

        if (status == "IN_PROGRESS" && turnNumber >= _activeCollabSession.maxTurns) {
            _activeCollabSession.status = CollaborationStatus::OPERATOR_REVIEW;
            _activeCollabSession.currentTurn = turnNumber;
            _activeCollabSession.nextActorId = "NONE";
            _activeCollabSession.latchedTurnReady = false;
            _activeCollabSession.lastSideChannelMessage = sideChannelMessage;
            _activeCollabSession.lastModel = agentModel;
            _activeCollabSession.lastTurnEpoch = nowEpoch;
            _activeCollabSession.lastFileMtime = mtimeEpoch;

            outResult = {
                {"status", "operator_review"},
                {"turn_number", turnNumber},
                {"message", "Turn cap reached without consensus. Escalated to operator."}
            };

            CollaborationCallback cb = _collabCallback;
            CollaborationSession snap = _activeCollabSession;
            _collabCv.notify_all();
            lock.unlock();

            if (cb) {
                cb(snap, "collaboration_turn");
            }
            return true;
        }
    } else {
        // Initiator turn:
        if (status == "CONSENSUS_REACHED") {
            if (outError) {
                *outError = "COLLABORATION_ERROR: Only the designated reviewer can signal CONSENSUS_REACHED on an even turn.";
            }
            return false;
        }
        _activeCollabSession.planBodyHash = bodyHash;
    }

    // Normal handoff. The claim belongs to the turn that just ended, so it must
    // not carry over and gate the next actor.
    _activeCollabSession.currentTurn = turnNumber;
    _activeCollabSession.nextActorId = (canAgent == _activeCollabSession.initiatorId) ?
                                       _activeCollabSession.reviewerId : _activeCollabSession.initiatorId;
    _activeCollabSession.claimedBy.clear();
    _activeCollabSession.claimEpoch = 0;
    _activeCollabSession.executor.clear();
    _activeCollabSession.lastSideChannelMessage = sideChannelMessage;
    _activeCollabSession.lastModel = agentModel;
    _activeCollabSession.lastTurnEpoch = nowEpoch;
    _activeCollabSession.lastFileMtime = mtimeEpoch;
    _activeCollabSession.latchedTurnReady = true;

    outResult = {
        {"status", "turn_signaled"},
        {"turn_number", turnNumber},
        {"next_actor_id", _activeCollabSession.nextActorId}
    };

    CollaborationCallback cb = _collabCallback;
    CollaborationSession snap = _activeCollabSession;
    _collabCv.notify_all();
    lock.unlock();

    if (cb) {
        cb(snap, "collaboration_turn");
    }
    return true;
}

bool AgentMessageBus::waitCollaborationTurn(const std::string& planFile,
                                           const std::string& agentId,
                                           int timeoutSeconds,
                                           nlohmann::json& outResult,
                                           std::string* outError) {
    std::unique_lock<std::mutex> lock(_mutex);
    std::string canAgent = canonicalAgentId(agentId);

    auto evaluate = [&]() -> bool {
        // Step 1: No Session or IDLE
        if (_activeCollabSession.status == CollaborationStatus::IDLE) {
            outResult = {{"status", "no_session"}};
            return true;
        }

        // Step 2: Validation
        if (!planFile.empty() && _activeCollabSession.planFile != planFile) {
            if (outError) *outError = "COLLABORATION_ERROR: Requested plan_file does not match active session.";
            return false;
        }
        if (canAgent != _activeCollabSession.initiatorId && canAgent != _activeCollabSession.reviewerId) {
            if (outError) *outError = "COLLABORATION_ERROR: Agent " + canAgent + " is not a participant in this collaboration session.";
            return false;
        }

        // Step 3: Terminal Consensus
        if (_activeCollabSession.status == CollaborationStatus::CONSENSUS_REACHED) {
            outResult = {
                {"status", "consensus_reached"},
                {"plan_file", _activeCollabSession.planFile},
                {"final_turn", _activeCollabSession.currentTurn},
                {"collaboration_status", "CONSENSUS_REACHED"},
                {"side_channel_message", _activeCollabSession.lastSideChannelMessage}
            };
            return true;
        }

        // Step 4: Operator Review Unblock
        if (_activeCollabSession.status == CollaborationStatus::OPERATOR_REVIEW) {
            outResult = {
                {"status", "operator_review"},
                {"plan_file", _activeCollabSession.planFile},
                {"current_turn", _activeCollabSession.currentTurn},
                {"collaboration_status", "OPERATOR_REVIEW"},
                {"message", "Turn cap reached. Session escalated to operator."}
            };
            return true;
        }

        // Step 5: Turn Ready
        if (_activeCollabSession.nextActorId == canAgent && _activeCollabSession.latchedTurnReady) {
            int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _activeCollabSession.claimedBy = canAgent;
            _activeCollabSession.claimEpoch = nowEpoch;
            _activeCollabSession.executor = "desktop";

            std::string role = (canAgent == _activeCollabSession.initiatorId) ? "initiator" : "reviewer";
            std::string priorAgent = (canAgent == _activeCollabSession.initiatorId) ?
                                     _activeCollabSession.reviewerId : _activeCollabSession.initiatorId;
            outResult = {
                {"status", "turn_ready"},
                {"plan_file", _activeCollabSession.planFile},
                {"turn_number", _activeCollabSession.currentTurn + 1},
                {"role", role},
                {"prior_agent", priorAgent},
                {"prior_model", _activeCollabSession.lastModel},
                {"collaboration_status", "IN_PROGRESS"},
                {"side_channel_message", _activeCollabSession.lastSideChannelMessage},
                {"file_mtime_epoch", _activeCollabSession.lastFileMtime},
                {"instruction", "Re-read " + _activeCollabSession.planFile + " fresh from disk before beginning your turn."}
            };
            return true;
        }

        // Step 6: Session IN_PROGRESS, but not ready for this agent
        return false;
    };

    if (evaluate()) {
        return true;
    }
    if (outError && !outError->empty()) {
        return false;
    }

    // Register as a live waiter only for the duration of the actual block, so
    // the orchestrator can tell "an agent is sitting here right now" apart from
    // "an agent called this once and went away".
    struct WaiterScope {
        std::map<std::string, int>& waiters;
        const std::string& id;
        WaiterScope(std::map<std::string, int>& w, const std::string& i) : waiters(w), id(i) {
            ++waiters[id];
        }
        ~WaiterScope() {
            auto it = waiters.find(id);
            if (it != waiters.end() && --it->second <= 0) {
                waiters.erase(it);
            }
        }
    } waiterScope(_liveWaiters, canAgent);

    if (timeoutSeconds <= 0) {
        _collabCv.wait(lock, [&]() {
            return evaluate();
        });
        return true;
    } else {
        bool ready = _collabCv.wait_for(lock, std::chrono::seconds(timeoutSeconds), [&]() {
            return evaluate();
        });
        if (ready) {
            return true;
        }
        outResult = {
            {"status", "idle"},
            {"plan_file", _activeCollabSession.planFile},
            {"current_turn", _activeCollabSession.currentTurn},
            {"next_actor_id", _activeCollabSession.nextActorId}
        };
        return true;
    }
}

bool AgentMessageBus::hasLiveWaiter(const std::string& agentId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _liveWaiters.find(canonicalAgentId(agentId));
    return it != _liveWaiters.end() && it->second > 0;
}

bool AgentMessageBus::nudgeCollaboration(std::string* outError) {
    CollaborationCallback cb;
    CollaborationSession snap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_activeCollabSession.status != CollaborationStatus::IN_PROGRESS &&
            _activeCollabSession.status != CollaborationStatus::OPERATOR_REVIEW) {
            if (outError) *outError = "COLLABORATION_ERROR: No active collaboration session to nudge.";
            return false;
        }
        snap = _activeCollabSession;
        cb = _collabCallback;
    }
    _collabCv.notify_all();
    if (cb) {
        cb(snap, "collaboration_nudge");
    }
    return true;
}

bool AgentMessageBus::takeoverCollaboration(const std::string& nextActorId, std::string* outError) {
    CollaborationCallback cb;
    CollaborationSession snap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_activeCollabSession.status == CollaborationStatus::IDLE) {
            if (outError) *outError = "COLLABORATION_ERROR: No active collaboration session.";
            return false;
        }

        std::string canActor = canonicalAgentId(nextActorId);
        if (canActor != _activeCollabSession.initiatorId &&
            canActor != _activeCollabSession.reviewerId &&
            canActor != "operator") {
            if (outError) *outError = "COLLABORATION_ERROR: Agent is not a participant in this session.";
            return false;
        }

        _activeCollabSession.nextActorId = canActor;
        _activeCollabSession.latchedTurnReady = true;
        _activeCollabSession.status = CollaborationStatus::IN_PROGRESS;
        _activeCollabSession.lastSideChannelMessage = "Operator assigned turn to " + canActor;
        snap = _activeCollabSession;
        cb = _collabCallback;
    }
    _collabCv.notify_all();
    if (cb) {
        cb(snap, "collaboration_takeover");
    }
    return true;
}

bool AgentMessageBus::abortCollaboration(std::string* outError) {
    CollaborationCallback cb;
    CollaborationSession snap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_activeCollabSession.status == CollaborationStatus::IDLE) {
            if (outError) *outError = "COLLABORATION_ERROR: No active collaboration session.";
            return false;
        }
        _activeCollabSession.status = CollaborationStatus::IDLE;
        _activeCollabSession.nextActorId = "NONE";
        _activeCollabSession.latchedTurnReady = false;
        _activeCollabSession.lastSideChannelMessage = "Session aborted by operator.";
        snap = _activeCollabSession;
        cb = _collabCallback;
    }
    _collabCv.notify_all();
    if (cb) {
        cb(snap, "collaboration_aborted");
    }
    return true;
}

CollaborationSession AgentMessageBus::getActiveCollaboration() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _activeCollabSession;
}

bool AgentMessageBus::appendCollaborationTurnBlock(const std::string& planFile,
                                                  const std::string& blockText,
                                                  std::string* outError) {
    if (blockText.empty()) return true;

    std::ifstream inFile(planFile);
    if (!inFile.is_open()) {
        if (outError) *outError = "Unable to open plan file: " + planFile;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    inFile.close();

    std::string formattedBlock = blockText;
    if (formattedBlock.front() != '\n') formattedBlock = "\n" + formattedBlock;
    if (formattedBlock.back() != '\n') formattedBlock += "\n";

    size_t collabPos = content.find("## Collaboration");
    if (collabPos == std::string::npos) {
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += "\n## Collaboration\n" + formattedBlock;
    } else {
        size_t nextH2 = content.find("\n## ", collabPos + 16);
        if (nextH2 != std::string::npos) {
            content.insert(nextH2, formattedBlock);
        } else {
            if (!content.empty() && content.back() != '\n') content += "\n";
            content += formattedBlock;
        }
    }

    std::ofstream outFile(planFile);
    if (!outFile.is_open()) {
        if (outError) *outError = "Unable to write to plan file: " + planFile;
        return false;
    }
    outFile << content;
    outFile.close();
    return true;
}

bool AgentMessageBus::snapshotPlanFile(const std::string& planFile,
                                      int turnNumber,
                                      std::string* outBackupPath,
                                      std::string* outError) {
    if (!fs::exists(planFile)) {
        if (outError) *outError = "Plan file does not exist: " + planFile;
        return false;
    }
    try {
        fs::path p(planFile);
        fs::path backupDir = p.parent_path() / ".collab_backup";
        fs::create_directories(backupDir);
        fs::path backupFile = backupDir / (p.filename().string() + ".turn" + std::to_string(turnNumber) + ".bak");
        fs::copy_file(p, backupFile, fs::copy_options::overwrite_existing);
        if (outBackupPath) *outBackupPath = backupFile.string();
        return true;
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Snapshot failed: ") + e.what();
        return false;
    }
}

bool AgentMessageBus::restorePlanFileSnapshot(const std::string& planFile,
                                             int turnNumber,
                                             std::string* outError) {
    try {
        fs::path p(planFile);
        fs::path backupFile = p.parent_path() / ".collab_backup" / (p.filename().string() + ".turn" + std::to_string(turnNumber) + ".bak");
        if (!fs::exists(backupFile)) {
            if (outError) *outError = "Backup file not found: " + backupFile.string();
            return false;
        }
        fs::copy_file(backupFile, p, fs::copy_options::overwrite_existing);
        return true;
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Restore failed: ") + e.what();
        return false;
    }
}

bool AgentMessageBus::failCollaboration(const std::string& reason) {
    CollaborationCallback cb;
    CollaborationSession snap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_activeCollabSession.status == CollaborationStatus::IDLE) {
            return false;
        }
        _activeCollabSession.status = CollaborationStatus::OPERATOR_REVIEW;
        _activeCollabSession.nextActorId = "NONE";
        _activeCollabSession.latchedTurnReady = false;
        _activeCollabSession.lastRunError = reason;
        _activeCollabSession.lastSideChannelMessage = reason;
        snap = _activeCollabSession;
        cb = _collabCallback;
    }
    _collabCv.notify_all();
    if (cb) {
        cb(snap, "collaboration_failed");
    }
    return true;
}

bool AgentMessageBus::reportRunnerUnavailable(const std::string& reason) {
    CollaborationCallback cb;
    CollaborationSession snap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_activeCollabSession.status != CollaborationStatus::IN_PROGRESS) {
            return false;
        }
        if (_activeCollabSession.lastRunError == reason) {
            return false;
        }
        _activeCollabSession.lastRunError = reason;
        _activeCollabSession.lastSideChannelMessage = reason;
        snap = _activeCollabSession;
        cb = _collabCallback;
    }
    if (cb) {
        cb(snap, "runner_unavailable");
    }
    return true;
}

void AgentMessageBus::setCollaborationCallback(CollaborationCallback cb) {
    std::lock_guard<std::mutex> lock(_mutex);
    _collabCallback = cb;
}

void AgentMessageBus::setMaxTurns(int maxTurns) {
    std::lock_guard<std::mutex> lock(_mutex);
    _activeCollabSession.maxTurns = maxTurns;
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
