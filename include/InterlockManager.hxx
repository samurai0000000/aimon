/*
 * InterlockManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_INTERLOCK_MANAGER_HXX
#define AIMON_INTERLOCK_MANAGER_HXX

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace aimon {

struct InterlockRequest {
    std::string interlockId;
    std::string runId;
    std::string checkpointId;
    int seq = 0;
    std::string description;
    std::string proposedAction;
    int64_t requestedEpoch = 0;
    bool resolved = false;
    bool approved = false;
    std::string rejectReason;
    int64_t resolvedEpoch = 0;

    nlohmann::json toJson() const;
    static InterlockRequest fromJson(const nlohmann::json& j);
};

class InterlockManager {
public:
    using InterlockNotifyCallback = std::function<void(const InterlockRequest& req, const std::string& eventType)>;

    static InterlockManager& getInstance();

    InterlockManager();
    ~InterlockManager() = default;

    std::string createInterlock(const std::string& runId,
                                const std::string& checkpointId,
                                int seq,
                                const std::string& description,
                                const std::string& proposedAction);

    bool waitInterlock(const std::string& interlockIdOrRunId,
                       int timeoutSeconds,
                       InterlockRequest& outResult);

    bool resolveInterlock(const std::string& interlockId,
                          bool approved,
                          const std::string& rejectReason);

    bool getInterlock(const std::string& interlockId, InterlockRequest& outReq) const;
    std::vector<InterlockRequest> getPendingInterlocks(const std::string& runId = "") const;
    std::vector<InterlockRequest> listInterlocks(const std::string& runId = "") const;

    void setNotifyCallback(InterlockNotifyCallback cb);

private:
    std::string generateInterlockId() const;

    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::map<std::string, InterlockRequest> _interlocks;
    InterlockNotifyCallback _notifyCb;
};

} // namespace aimon

#endif // AIMON_INTERLOCK_MANAGER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
