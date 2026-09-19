/*
 * AgentMessageBus.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_AGENT_MESSAGE_BUS_HXX
#define AIMON_AGENT_MESSAGE_BUS_HXX

#include <string>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <atomic>
#include <nlohmann/json.hpp>

namespace aimon {

struct AgentMessage {
    std::string messageId;
    std::string sessionId;
    std::string text;
    int64_t timestampEpoch = 0;
};

enum class CollaborationStatus {
    IDLE,
    IN_PROGRESS,
    CONSENSUS_REACHED,
    OPERATOR_REVIEW
};

struct CollaborationSession {
    std::string planFile;
    std::string initiatorId;
    std::string reviewerId;
    std::string nextActorId;
    int currentTurn = 0;
    int maxTurns = 8;
    CollaborationStatus status = CollaborationStatus::IDLE;
    std::string planBodyHash;
    std::string lastSideChannelMessage;
    std::string lastModel;
    int64_t lastTurnEpoch = 0;
    int64_t lastFileMtime = 0;
    bool latchedTurnReady = false;
    std::string claimedBy;
    int64_t claimEpoch = 0;
    std::string executor;
    std::string lastRunError;

    nlohmann::json toJson() const;
};

class AgentMessageBus {
public:
    using ReplyCallback = std::function<void(const std::string& sessionId,
                                             const std::string& messageId,
                                             const std::string& replyText)>;

    using CollaborationCallback = std::function<void(const CollaborationSession& session,
                                                     const std::string& eventType)>;

    static AgentMessageBus& getInstance();

    AgentMessageBus();
    ~AgentMessageBus() = default;

    // --- Legacy Operator Chat Messages ---
    std::string postMessageToAgent(const std::string& sessionId, const std::string& text);
    bool fetchNextMessageForAgent(const std::string& sessionId, int timeoutSec, AgentMessage& outMsg);
    bool postReplyFromAgent(const std::string& sessionId,
                            const std::string& messageId,
                            const std::string& replyText);
    void setReplyCallback(ReplyCallback callback);
    void shutdown();

    // --- Document-Centric Collaboration Signaling ---
    bool startCollaboration(const std::string& planFile,
                            const std::string& initiatorId,
                            const std::string& reviewerId,
                            std::string* outError = nullptr);

    bool signalCollaborationTurn(const std::string& planFile,
                                 int turnNumber,
                                 const std::string& agentId,
                                 const std::string& agentModel,
                                 const std::string& status,
                                 const std::string& sideChannelMessage,
                                 nlohmann::json& outResult,
                                 std::string* outError = nullptr);

    bool waitCollaborationTurn(const std::string& planFile,
                               const std::string& agentId,
                               int timeoutSeconds,
                               nlohmann::json& outResult,
                               std::string* outError = nullptr);

    bool appendCollaborationTurnBlock(const std::string& planFile,
                                      const std::string& blockText,
                                      std::string* outError = nullptr);

    bool snapshotPlanFile(const std::string& planFile,
                          int turnNumber,
                          std::string* outBackupPath = nullptr,
                          std::string* outError = nullptr);

    bool restorePlanFileSnapshot(const std::string& planFile,
                                 int turnNumber,
                                 std::string* outError = nullptr);

    bool failCollaboration(const std::string& reason);

    // Records a runner-side problem (missing binary, expired credentials,
    // exhausted quota) without disturbing the session. Status, turn latch and
    // next actor are left untouched so a desktop agent can still take the turn.
    bool reportRunnerUnavailable(const std::string& reason);

    void setMaxTurns(int maxTurns);

    // True while at least one thread is currently blocked inside
    // waitCollaborationTurn() for this agent. This is the only evidence that a
    // desktop agent is actually present; a past claim proves nothing.
    bool hasLiveWaiter(const std::string& agentId) const;

    bool nudgeCollaboration(std::string* outError = nullptr);
    bool takeoverCollaboration(const std::string& nextActorId, std::string* outError = nullptr);
    bool abortCollaboration(std::string* outError = nullptr);

    CollaborationSession getActiveCollaboration() const;
    void setCollaborationCallback(CollaborationCallback cb);

    // Static Utilities
    static std::string statusToString(CollaborationStatus status);
    static CollaborationStatus stringToStatus(const std::string& str);
    static std::string canonicalAgentId(const std::string& rawName);
    static std::string normalizeNewlines(const std::string& input);
    static std::string extractPlanBody(const std::string& fileContent, bool* foundHeading = nullptr);
    static std::string computeSha256(const std::string& data);
    static bool validateDocumentContract(const std::string& fileContent, int turnNumber, std::string* outError = nullptr);

private:
    std::string generateMessageId();

    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::map<std::string, std::deque<AgentMessage>> _inboxes;
    std::atomic<uint64_t> _messageCounter{100};
    ReplyCallback _replyCallback;

    // Collaboration session
    CollaborationSession _activeCollabSession;
    std::condition_variable _collabCv;
    CollaborationCallback _collabCallback;
    std::map<std::string, int> _liveWaiters;
    std::atomic<bool> _shutdown{false};
};

} // namespace aimon

#endif // AIMON_AGENT_MESSAGE_BUS_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
