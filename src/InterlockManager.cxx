/*
 * InterlockManager.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "InterlockManager.hxx"
#include "TranscriptSink.hxx"
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace aimon {

nlohmann::json InterlockRequest::toJson() const {
    return {
        {"interlock_id", interlockId},
        {"run_id", runId},
        {"checkpoint_id", checkpointId},
        {"seq", seq},
        {"description", description},
        {"proposed_action", proposedAction},
        {"requested_epoch", requestedEpoch},
        {"resolved", resolved},
        {"approved", approved},
        {"reject_reason", rejectReason},
        {"resolved_epoch", resolvedEpoch}
    };
}

InterlockRequest InterlockRequest::fromJson(const nlohmann::json& j) {
    InterlockRequest req;
    req.interlockId = j.value("interlock_id", "");
    req.runId = j.value("run_id", "");
    req.checkpointId = j.value("checkpoint_id", "");
    req.seq = j.value("seq", 0);
    req.description = j.value("description", "");
    req.proposedAction = j.value("proposed_action", "");
    req.requestedEpoch = j.value("requested_epoch", (int64_t)0);
    req.resolved = j.value("resolved", false);
    req.approved = j.value("approved", false);
    req.rejectReason = j.value("reject_reason", "");
    req.resolvedEpoch = j.value("resolved_epoch", (int64_t)0);
    return req;
}

InterlockManager& InterlockManager::getInstance() {
    static InterlockManager instance;
    return instance;
}

InterlockManager::InterlockManager() {
}

std::string InterlockManager::generateInterlockId() const {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&tt, &tm);

    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint16_t> dis;
    uint16_t suffix = dis(gen);

    std::ostringstream oss;
    oss << "intk-" << std::put_time(&tm, "%Y%m%d-%H%M%S") << "-"
        << std::hex << std::setfill('0') << std::setw(4) << suffix;
    return oss.str();
}

std::string InterlockManager::createInterlock(const std::string& runId,
                                             const std::string& checkpointId,
                                             int seq,
                                             const std::string& description,
                                             const std::string& proposedAction) {
    InterlockRequest req;
    req.interlockId = generateInterlockId();
    req.runId = runId;
    req.checkpointId = checkpointId;
    req.seq = seq;
    req.description = description;
    req.proposedAction = proposedAction;
    req.requestedEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    req.resolved = false;
    req.approved = false;

    InterlockNotifyCallback cb;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _interlocks[req.interlockId] = req;
        cb = _notifyCb;
    }

    // Log interlock event into TranscriptSink
    if (!runId.empty()) {
        TranscriptEvent ev;
        ev.runId = runId;
        ev.checkpointId = checkpointId;
        ev.actor = "aimon";
        ev.kind = "interlock";
        ev.decision = "wait_human";
        ev.proposalNext = proposedAction;
        ev.notes = description;
        std::string err;
        TranscriptSink::getInstance().appendEvent(ev, err);
    }

    if (cb) {
        cb(req, "interlock_request");
    }

    return req.interlockId;
}

bool InterlockManager::waitInterlock(const std::string& interlockIdOrRunId,
                                    int timeoutSeconds,
                                    InterlockRequest& outResult) {
    std::unique_lock<std::mutex> lock(_mutex);

    auto findInterlock = [&]() -> InterlockRequest* {
        // First check exact ID match
        auto it = _interlocks.find(interlockIdOrRunId);
        if (it != _interlocks.end()) {
            return &it->second;
        }
        // Next check for latest matching runId
        InterlockRequest* latest = nullptr;
        for (auto& pair : _interlocks) {
            if (pair.second.runId == interlockIdOrRunId) {
                if (!latest || pair.second.requestedEpoch >= latest->requestedEpoch) {
                    latest = &pair.second;
                }
            }
        }
        return latest;
    };

    if (_shutdown.load()) {
        return false;
    }

    auto isResolved = [&]() -> bool {
        if (_shutdown.load()) return true;
        InterlockRequest* req = findInterlock();
        return req != nullptr && req->resolved;
    };

    if (timeoutSeconds > 0) {
        bool ok = _cv.wait_for(lock, std::chrono::seconds(timeoutSeconds), isResolved);
        if (!ok || _shutdown.load()) {
            return false;
        }
    } else {
        _cv.wait(lock, isResolved);
        if (_shutdown.load()) {
            return false;
        }
    }

    InterlockRequest* req = findInterlock();
    if (req && req->resolved) {
        outResult = *req;
        return true;
    }

    return false;
}

void InterlockManager::shutdown() {
    std::lock_guard<std::mutex> lock(_mutex);
    _shutdown = true;
    _cv.notify_all();
}

bool InterlockManager::resolveInterlock(const std::string& interlockId,
                                       bool approved,
                                       const std::string& rejectReason) {
    InterlockRequest resolvedReq;
    InterlockNotifyCallback cb;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _interlocks.find(interlockId);
        if (it == _interlocks.end()) {
            // Also search by runId if caller passed runId
            for (auto& pair : _interlocks) {
                if (pair.second.runId == interlockId && !pair.second.resolved) {
                    it = _interlocks.find(pair.first);
                    break;
                }
            }
        }

        if (it == _interlocks.end()) {
            return false;
        }

        it->second.resolved = true;
        it->second.approved = approved;
        it->second.rejectReason = rejectReason;
        it->second.resolvedEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        resolvedReq = it->second;
        cb = _notifyCb;
    }

    _cv.notify_all();

    // Log resolution event to TranscriptSink
    if (!resolvedReq.runId.empty()) {
        TranscriptEvent ev;
        ev.runId = resolvedReq.runId;
        ev.checkpointId = resolvedReq.checkpointId;
        ev.actor = "human";
        ev.kind = "interlock_resolved";
        ev.decision = approved ? "approved" : "rejected";
        ev.notes = approved ? "Approved by human operator." : ("Rejected by human operator: " + rejectReason);
        std::string err;
        TranscriptSink::getInstance().appendEvent(ev, err);
    }

    if (cb) {
        cb(resolvedReq, "interlock_resolved");
    }

    return true;
}

bool InterlockManager::getInterlock(const std::string& interlockId, InterlockRequest& outReq) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _interlocks.find(interlockId);
    if (it != _interlocks.end()) {
        outReq = it->second;
        return true;
    }
    return false;
}

std::vector<InterlockRequest> InterlockManager::getPendingInterlocks(const std::string& runId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<InterlockRequest> pending;
    for (const auto& pair : _interlocks) {
        if (!pair.second.resolved) {
            if (runId.empty() || pair.second.runId == runId) {
                pending.push_back(pair.second);
            }
        }
    }
    return pending;
}

std::vector<InterlockRequest> InterlockManager::listInterlocks(const std::string& runId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<InterlockRequest> list;
    for (const auto& pair : _interlocks) {
        if (runId.empty() || pair.second.runId == runId) {
            list.push_back(pair.second);
        }
    }
    return list;
}

void InterlockManager::setNotifyCallback(InterlockNotifyCallback cb) {
    std::lock_guard<std::mutex> lock(_mutex);
    _notifyCb = cb;
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
