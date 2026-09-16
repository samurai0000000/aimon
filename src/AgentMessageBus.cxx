/*
 * AgentMessageBus.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AgentMessageBus.hxx"
#include <chrono>

namespace aimon {

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

    auto hasMessage = [this, &sessionId]() {
        if (!sessionId.empty()) {
            auto it = _inboxes.find(sessionId);
            return (it != _inboxes.end() && !it->second.empty());
        }
        for (const auto& kv : _inboxes) {
            if (!kv.second.empty()) {
                return true;
            }
        }
        return false;
    };

    if (timeoutSec <= 0) {
        _cv.wait(lock, hasMessage);
    } else {
        bool arrived = _cv.wait_for(lock, std::chrono::seconds(timeoutSec), hasMessage);
        if (!arrived) {
            return false;
        }
    }

    if (!sessionId.empty()) {
        auto it = _inboxes.find(sessionId);
        if (it != _inboxes.end() && !it->second.empty()) {
            outMsg = it->second.front();
            it->second.pop_front();
            return true;
        }
    } else {
        for (auto& kv : _inboxes) {
            if (!kv.second.empty()) {
                outMsg = kv.second.front();
                kv.second.pop_front();
                return true;
            }
        }
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
