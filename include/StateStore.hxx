/*
 * StateStore.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_STATE_STORE_HXX
#define AIMON_STATE_STORE_HXX

#include <shared_mutex>
#include <vector>
#include <functional>
#include "Models.hxx"

namespace aimon {

class StateStore {
public:
    using UpdateListener = std::function<void(const AggregateStatus&)>;

    static StateStore& getInstance();

    AggregateStatus getStatus() const;
    void update(const AggregateStatus& status);
    void updateAntigravity(const AntigravityStatus& ag);
    void updateCursor(const CursorStatus& cr);

    void addListener(UpdateListener listener);

private:
    StateStore();
    ~StateStore() = default;

    void notifyListeners(const AggregateStatus& status);

    mutable std::shared_mutex _mutex;
    AggregateStatus _currentStatus;
    std::vector<UpdateListener> _listeners;
};

} // namespace aimon

#endif // AIMON_STATE_STORE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
