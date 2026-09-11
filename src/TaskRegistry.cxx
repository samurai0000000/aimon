/*
 * TaskRegistry.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TaskRegistry.hxx"
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace aimon {

TaskRegistry& TaskRegistry::getInstance() {
    static TaskRegistry instance;
    return instance;
}

TaskRegistry::TaskRegistry() {
}

std::string TaskRegistry::generateTaskId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis;

    std::stringstream ss;
    ss << "tsk-" << std::hex << std::setw(8) << std::setfill('0') << (dis(gen) & 0xffffffff);
    return ss.str();
}

std::string TaskRegistry::registerOrUpdateTask(const AgentTask& task) {
    std::unique_lock<std::shared_mutex> lock(_mutex);

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string id = task.taskId;
    if (id.empty()) {
        id = generateTaskId();
    }

    auto it = _tasks.find(id);
    if (it != _tasks.end()) {
        if (!task.agentName.empty()) it->second.agentName = task.agentName;
        if (!task.workspace.empty()) it->second.workspace = task.workspace;
        if (!task.taskDescription.empty()) it->second.taskDescription = task.taskDescription;
        if (!task.currentAction.empty()) it->second.currentAction = task.currentAction;
        if (!task.status.empty()) it->second.status = task.status;
        if (!task.details.empty()) it->second.details = task.details;
        if (!task.sseSessionId.empty()) it->second.sseSessionId = task.sseSessionId;

        it->second.lastHeartbeatEpoch = now;
        if (task.status == "completed" || task.status == "failed") {
            if (it->second.completedTimeEpoch == 0) {
                it->second.completedTimeEpoch = now;
            }
        }
    } else {
        AgentTask newTask = task;
        newTask.taskId = id;
        if (newTask.startTimeEpoch == 0) {
            newTask.startTimeEpoch = now;
        }
        newTask.lastHeartbeatEpoch = now;
        if (newTask.status.empty()) {
            newTask.status = "running";
        }
        if (newTask.status == "completed" || newTask.status == "failed") {
            newTask.completedTimeEpoch = now;
        }
        _tasks[id] = newTask;
    }

    return id;
}

bool TaskRegistry::completeTask(const std::string& taskId, const std::string& status,
                                const std::string& details) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _tasks.find(taskId);
    if (it == _tasks.end()) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    it->second.status = status.empty() ? "completed" : status;
    if (!details.empty()) {
        it->second.details = details;
    }
    it->second.completedTimeEpoch = now;
    it->second.lastHeartbeatEpoch = now;
    return true;
}

bool TaskRegistry::getTask(const std::string& taskId, AgentTask& outTask) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _tasks.find(taskId);
    if (it == _tasks.end()) {
        return false;
    }
    outTask = it->second;
    return true;
}

std::vector<AgentTask> TaskRegistry::listTasks(bool includeCompleted) {
    reapStaleTasks();

    std::shared_lock<std::shared_mutex> lock(_mutex);
    std::vector<AgentTask> result;
    for (const auto& pair : _tasks) {
        if (!includeCompleted) {
            if (pair.second.status == "completed" || pair.second.status == "failed") {
                continue;
            }
        }
        result.push_back(pair.second);
    }

    std::sort(result.begin(), result.end(), [](const AgentTask& a, const AgentTask& b) {
        return a.startTimeEpoch > b.startTimeEpoch;
    });

    return result;
}

void TaskRegistry::reapStaleTasks(std::chrono::seconds timeout) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (auto& pair : _tasks) {
        if (pair.second.status == "running" || pair.second.status == "waiting_for_user") {
            if ((now - pair.second.lastHeartbeatEpoch) > timeout.count()) {
                pair.second.status = "stale";
            }
        }
    }
}

void TaskRegistry::handleSessionDisconnected(const std::string& sseSessionId) {
    if (sseSessionId.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    for (auto& pair : _tasks) {
        if (pair.second.sseSessionId == sseSessionId &&
            (pair.second.status == "running" || pair.second.status == "waiting_for_user")) {
            pair.second.status = "disconnected";
        }
    }
}

size_t TaskRegistry::getActiveTaskCount() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    size_t count = 0;
    for (const auto& pair : _tasks) {
        if (pair.second.status == "running" || pair.second.status == "waiting_for_user") {
            count++;
        }
    }
    return count;
}

void TaskRegistry::clear() {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _tasks.clear();
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
