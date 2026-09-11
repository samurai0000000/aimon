/*
 * TaskRegistry.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_TASK_REGISTRY_HXX
#define AIMON_TASK_REGISTRY_HXX

#include <string>
#include <vector>
#include <map>
#include <shared_mutex>
#include <chrono>
#include "Models.hxx"

namespace aimon {

class TaskRegistry {
public:
    static TaskRegistry& getInstance();

    TaskRegistry();
    ~TaskRegistry() = default;

    std::string registerOrUpdateTask(const AgentTask& task);
    bool completeTask(const std::string& taskId, const std::string& status = "completed",
                      const std::string& details = "");
    bool getTask(const std::string& taskId, AgentTask& outTask) const;
    std::vector<AgentTask> listTasks(bool includeCompleted = false);

    void reapStaleTasks(std::chrono::seconds timeout = std::chrono::minutes(10));
    void handleSessionDisconnected(const std::string& sseSessionId);

    size_t getActiveTaskCount() const;
    void clear();

private:
    std::string generateTaskId();

    mutable std::shared_mutex _mutex;
    std::map<std::string, AgentTask> _tasks;
};

} // namespace aimon

#endif // AIMON_TASK_REGISTRY_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
