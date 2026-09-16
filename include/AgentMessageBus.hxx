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

namespace aimon {

struct AgentMessage {
    std::string messageId;
    std::string sessionId;
    std::string text;
    int64_t timestampEpoch = 0;
};

class AgentMessageBus {
public:
    using ReplyCallback = std::function<void(const std::string& sessionId,
                                             const std::string& messageId,
                                             const std::string& replyText)>;

    static AgentMessageBus& getInstance();

    AgentMessageBus();
    ~AgentMessageBus() = default;

    // Post a message from the operator to an agent session (asynchronous / non-blocking)
    std::string postMessageToAgent(const std::string& sessionId, const std::string& text);

    // Called by desktop agent (agent_check_inbox) to fetch the next pending message
    bool fetchNextMessageForAgent(const std::string& sessionId, int timeoutSec, AgentMessage& outMsg);

    // Called by desktop agent (agent_send_reply) to deliver its response
    bool postReplyFromAgent(const std::string& sessionId,
                            const std::string& messageId,
                            const std::string& replyText);

    // Register callback for asynchronous delivery to the UI / console
    void setReplyCallback(ReplyCallback callback);

private:
    std::string generateMessageId();

    std::mutex _mutex;
    std::condition_variable _cv;
    std::map<std::string, std::deque<AgentMessage>> _inboxes;
    std::atomic<uint64_t> _messageCounter{100};
    ReplyCallback _replyCallback;
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
