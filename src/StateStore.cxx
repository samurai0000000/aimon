/*
 * StateStore.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "StateStore.hxx"
#include <mutex>

namespace aimon {

StateStore& StateStore::getInstance() {
    static StateStore instance;
    return instance;
}

StateStore::StateStore() {
    _currentStatus.lastUpdated = std::chrono::system_clock::now();
}

AggregateStatus StateStore::getStatus() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _currentStatus;
}

void StateStore::update(const AggregateStatus& status) {
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _currentStatus = status;
        _currentStatus.lastUpdated = std::chrono::system_clock::now();
    }
    notifyListeners(_currentStatus);
}

void StateStore::updateAntigravity(const AntigravityStatus& ag) {
    AggregateStatus copy;
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _currentStatus.antigravity = ag;
        _currentStatus.lastUpdated = std::chrono::system_clock::now();
        copy = _currentStatus;
    }
    notifyListeners(copy);
}

void StateStore::updateCursor(const CursorStatus& cr) {
    AggregateStatus copy;
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _currentStatus.cursor = cr;
        _currentStatus.lastUpdated = std::chrono::system_clock::now();
        copy = _currentStatus;
    }
    notifyListeners(copy);
}

void StateStore::addListener(UpdateListener listener) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _listeners.push_back(listener);
}

void StateStore::notifyListeners(const AggregateStatus& status) {
    std::vector<UpdateListener> listenersCopy;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        listenersCopy = _listeners;
    }

    for (const auto& listener : listenersCopy) {
        try {
            listener(status);
        } catch (...) {
            // Suppress exceptions from observers
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
