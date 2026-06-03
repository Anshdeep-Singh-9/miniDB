#include "lock_manager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const std::chrono::milliseconds LOCK_TIMEOUT_MS(5000);

std::shared_mutex g_database_lock;
std::mutex g_owner_mutex;
std::uint64_t g_writer_owner_txn_id = 0;

struct LockRequest {
    std::uint64_t txn_id;
    char mode;
};

struct LockState {
    std::set<std::uint64_t> shared_holders;
    std::uint64_t exclusive_holder = 0;
    std::deque<LockRequest> waiting_queue;
};

std::mutex g_lock_table_mutex;
std::condition_variable g_lock_cv;
std::unordered_map<std::string, LockState> g_lock_table;
std::unordered_map<std::uint64_t, std::set<std::string>> g_txn_locks;
std::unordered_map<std::uint64_t, std::set<std::uint64_t>> g_waits_for;
std::unordered_map<std::uint64_t, std::uint64_t> g_txn_order;
std::unordered_map<std::uint64_t, std::string> g_aborted_txns;
std::uint64_t g_next_order = 1;

std::mutex g_short_latch_mutex;
std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>> g_short_row_latches;
std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>> g_short_page_latches;

std::string resource_key(const std::string& table_name, const RID& rid) {
    std::ostringstream out;
    out << table_name << ':' << rid.page_id << ':' << rid.slot_id;
    return out.str();
}

std::string page_key(const std::string& table_name, uint32_t page_id) {
    std::ostringstream out;
    out << table_name << ':' << page_id;
    return out.str();
}

std::shared_ptr<std::shared_mutex> short_row_latch(const std::string& table_name,
                                                   const RID& rid) {
    const std::string key = resource_key(table_name, rid);
    std::lock_guard<std::mutex> guard(g_short_latch_mutex);
    std::shared_ptr<std::shared_mutex>& latch = g_short_row_latches[key];
    if (!latch) {
        latch = std::make_shared<std::shared_mutex>();
    }
    return latch;
}

std::shared_ptr<std::shared_mutex> short_page_latch(const std::string& table_name,
                                                    uint32_t page_id) {
    const std::string key = page_key(table_name, page_id);
    std::lock_guard<std::mutex> guard(g_short_latch_mutex);
    std::shared_ptr<std::shared_mutex>& latch = g_short_page_latches[key];
    if (!latch) {
        latch = std::make_shared<std::shared_mutex>();
    }
    return latch;
}

void remember_txn(std::uint64_t txn_id) {
    if (txn_id == 0) return;
    if (g_txn_order.find(txn_id) == g_txn_order.end()) {
        g_txn_order[txn_id] = g_next_order++;
    }
}

bool has_shared(const LockState& state, std::uint64_t txn_id) {
    return state.shared_holders.find(txn_id) != state.shared_holders.end();
}

bool has_exclusive(const LockState& state, std::uint64_t txn_id) {
    return state.exclusive_holder == txn_id;
}

std::set<std::uint64_t> blockers_for_shared(const LockState& state,
                                            std::uint64_t txn_id) {
    std::set<std::uint64_t> blockers;
    if (state.exclusive_holder != 0 && state.exclusive_holder != txn_id) {
        blockers.insert(state.exclusive_holder);
    }
    return blockers;
}

std::set<std::uint64_t> blockers_for_exclusive(const LockState& state,
                                               std::uint64_t txn_id) {
    std::set<std::uint64_t> blockers;
    if (state.exclusive_holder != 0 && state.exclusive_holder != txn_id) {
        blockers.insert(state.exclusive_holder);
    }
    for (std::set<std::uint64_t>::const_iterator it = state.shared_holders.begin();
         it != state.shared_holders.end();
         ++it) {
        if (*it != txn_id) {
            blockers.insert(*it);
        }
    }
    return blockers;
}

bool can_grant_shared(const LockState& state, std::uint64_t txn_id) {
    return blockers_for_shared(state, txn_id).empty();
}

bool can_grant_exclusive(const LockState& state, std::uint64_t txn_id) {
    return blockers_for_exclusive(state, txn_id).empty();
}

void remove_waiter(LockState& state, std::uint64_t txn_id) {
    state.waiting_queue.erase(
        std::remove_if(state.waiting_queue.begin(), state.waiting_queue.end(),
                       [txn_id](const LockRequest& req) {
                           return req.txn_id == txn_id;
                       }),
        state.waiting_queue.end());
}

void add_waiter_once(LockState& state, std::uint64_t txn_id, char mode) {
    for (std::deque<LockRequest>::const_iterator it = state.waiting_queue.begin();
         it != state.waiting_queue.end();
         ++it) {
        if (it->txn_id == txn_id) return;
    }
    state.waiting_queue.push_back({txn_id, mode});
}

bool dfs_cycle(std::uint64_t current,
               std::uint64_t target,
               std::set<std::uint64_t>& visited,
               std::vector<std::uint64_t>& path) {
    if (current == target && !path.empty()) {
        path.push_back(current);
        return true;
    }
    if (visited.find(current) != visited.end()) {
        return false;
    }

    visited.insert(current);
    path.push_back(current);

    std::unordered_map<std::uint64_t, std::set<std::uint64_t>>::const_iterator edges =
        g_waits_for.find(current);
    if (edges != g_waits_for.end()) {
        for (std::set<std::uint64_t>::const_iterator it = edges->second.begin();
             it != edges->second.end();
             ++it) {
            if (dfs_cycle(*it, target, visited, path)) {
                return true;
            }
        }
    }

    path.pop_back();
    return false;
}

bool find_cycle_from(std::uint64_t txn_id, std::vector<std::uint64_t>& cycle) {
    std::set<std::uint64_t> visited;
    cycle.clear();
    return dfs_cycle(txn_id, txn_id, visited, cycle);
}

std::uint64_t choose_youngest_victim(const std::vector<std::uint64_t>& cycle) {
    std::uint64_t victim = 0;
    std::uint64_t newest_order = 0;

    for (std::size_t i = 0; i < cycle.size(); ++i) {
        std::uint64_t txn_id = cycle[i];
        remember_txn(txn_id);
        std::uint64_t order = g_txn_order[txn_id];
        if (victim == 0 || order > newest_order) {
            victim = txn_id;
            newest_order = order;
        }
    }

    return victim;
}

void release_locks_locked(std::uint64_t txn_id) {
    std::unordered_map<std::uint64_t, std::set<std::string>>::iterator held =
        g_txn_locks.find(txn_id);
    if (held != g_txn_locks.end()) {
        std::vector<std::string> resources(held->second.begin(), held->second.end());
        for (std::size_t i = 0; i < resources.size(); ++i) {
            std::unordered_map<std::string, LockState>::iterator state_it =
                g_lock_table.find(resources[i]);
            if (state_it == g_lock_table.end()) continue;

            state_it->second.shared_holders.erase(txn_id);
            if (state_it->second.exclusive_holder == txn_id) {
                state_it->second.exclusive_holder = 0;
            }
            remove_waiter(state_it->second, txn_id);

            if (state_it->second.shared_holders.empty() &&
                state_it->second.exclusive_holder == 0 &&
                state_it->second.waiting_queue.empty()) {
                g_lock_table.erase(state_it);
            }
        }
        g_txn_locks.erase(held);
    }

    g_waits_for.erase(txn_id);
    for (std::unordered_map<std::uint64_t, std::set<std::uint64_t>>::iterator it =
             g_waits_for.begin();
         it != g_waits_for.end();
         ++it) {
        it->second.erase(txn_id);
    }
}

void abort_locked(std::uint64_t txn_id, const std::string& reason) {
    if (txn_id == 0) return;
    g_aborted_txns[txn_id] = reason;
    release_locks_locked(txn_id);
}

bool is_aborted_locked(std::uint64_t txn_id) {
    return txn_id != 0 && g_aborted_txns.find(txn_id) != g_aborted_txns.end();
}

bool acquire_lock(std::uint64_t txn_id,
                  const std::string& key,
                  char mode,
                  bool upgrade) {
    if (txn_id == 0) {
        return true;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + LOCK_TIMEOUT_MS;

    std::unique_lock<std::mutex> lock(g_lock_table_mutex);
    remember_txn(txn_id);

    while (true) {
        if (is_aborted_locked(txn_id)) {
            return false;
        }

        LockState& state = g_lock_table[key];

        if (mode == 'S') {
            if (has_shared(state, txn_id) || has_exclusive(state, txn_id)) {
                remove_waiter(state, txn_id);
                g_waits_for.erase(txn_id);
                return true;
            }

            if (can_grant_shared(state, txn_id)) {
                state.shared_holders.insert(txn_id);
                g_txn_locks[txn_id].insert(key);
                remove_waiter(state, txn_id);
                g_waits_for.erase(txn_id);
                return true;
            }

            g_waits_for[txn_id] = blockers_for_shared(state, txn_id);
            add_waiter_once(state, txn_id, mode);
        } else {
            if (has_exclusive(state, txn_id)) {
                remove_waiter(state, txn_id);
                g_waits_for.erase(txn_id);
                return true;
            }

            if (can_grant_exclusive(state, txn_id)) {
                state.shared_holders.erase(txn_id);
                state.exclusive_holder = txn_id;
                g_txn_locks[txn_id].insert(key);
                remove_waiter(state, txn_id);
                g_waits_for.erase(txn_id);
                return true;
            }

            g_waits_for[txn_id] = blockers_for_exclusive(state, txn_id);
            add_waiter_once(state, txn_id, upgrade ? 'U' : mode);
        }

        std::vector<std::uint64_t> cycle;
        if (find_cycle_from(txn_id, cycle)) {
            std::uint64_t victim = choose_youngest_victim(cycle);
            abort_locked(victim, "Transaction aborted due to deadlock.");
            g_lock_cv.notify_all();
            if (victim == txn_id) {
                return false;
            }
            continue;
        }

        if (g_lock_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            abort_locked(txn_id, "Transaction aborted due to lock wait timeout.");
            g_lock_cv.notify_all();
            return false;
        }
    }
}

}  // namespace

bool LockManager::begin_transaction(std::uint64_t txn_id) {
    if (txn_id == 0) {
        return false;
    }

    if (!g_database_lock.try_lock()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(g_owner_mutex);
        g_writer_owner_txn_id = txn_id;
    }

    std::lock_guard<std::mutex> table_guard(g_lock_table_mutex);
    remember_txn(txn_id);
    g_aborted_txns.erase(txn_id);
    return true;
}

void LockManager::end_transaction(std::uint64_t txn_id) {
    releaseLocks(txn_id);
    clear_transaction_state(txn_id);

    {
        std::lock_guard<std::mutex> guard(g_owner_mutex);
        if (g_writer_owner_txn_id != txn_id) {
            return;
        }
        g_writer_owner_txn_id = 0;
    }
    g_database_lock.unlock();
}

void LockManager::lock_shared() {
    g_database_lock.lock_shared();
}

void LockManager::unlock_shared() {
    g_database_lock.unlock_shared();
}

void LockManager::lock_exclusive() {
    g_database_lock.lock();
}

void LockManager::unlock_exclusive() {
    {
        std::lock_guard<std::mutex> guard(g_owner_mutex);
        g_writer_owner_txn_id = 0;
    }
    g_database_lock.unlock();
}

bool LockManager::acquireShared(std::uint64_t txn_id,
                                const std::string& table_name,
                                const RID& rid) {
    return acquire_lock(txn_id, resource_key(table_name, rid), 'S', false);
}

bool LockManager::acquireExclusive(std::uint64_t txn_id,
                                   const std::string& table_name,
                                   const RID& rid) {
    return acquire_lock(txn_id, resource_key(table_name, rid), 'X', false);
}

bool LockManager::upgradeLock(std::uint64_t txn_id,
                              const std::string& table_name,
                              const RID& rid) {
    return acquire_lock(txn_id, resource_key(table_name, rid), 'X', true);
}

void LockManager::releaseLocks(std::uint64_t txn_id) {
    {
        std::lock_guard<std::mutex> guard(g_lock_table_mutex);
        release_locks_locked(txn_id);
    }
    g_lock_cv.notify_all();
}

void LockManager::abortTransaction(std::uint64_t txn_id, const std::string& reason) {
    {
        std::lock_guard<std::mutex> guard(g_lock_table_mutex);
        abort_locked(txn_id, reason);
    }
    g_lock_cv.notify_all();
}

bool LockManager::transaction_aborted(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> guard(g_lock_table_mutex);
    return is_aborted_locked(txn_id);
}

std::string LockManager::abort_reason(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> guard(g_lock_table_mutex);
    std::unordered_map<std::uint64_t, std::string>::const_iterator found =
        g_aborted_txns.find(txn_id);
    return found == g_aborted_txns.end() ? std::string() : found->second;
}

void LockManager::clear_transaction_state(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> guard(g_lock_table_mutex);
    g_aborted_txns.erase(txn_id);
    g_txn_order.erase(txn_id);
    g_waits_for.erase(txn_id);
}

void LockManager::lock_row_shared(const std::string& table_name, const RID& rid) {
    short_row_latch(table_name, rid)->lock_shared();
}

void LockManager::unlock_row_shared(const std::string& table_name, const RID& rid) {
    short_row_latch(table_name, rid)->unlock_shared();
}

void LockManager::lock_row_exclusive(const std::string& table_name, const RID& rid) {
    short_row_latch(table_name, rid)->lock();
}

void LockManager::unlock_row_exclusive(const std::string& table_name, const RID& rid) {
    short_row_latch(table_name, rid)->unlock();
}

void LockManager::lock_page_shared(const std::string& table_name, uint32_t page_id) {
    short_page_latch(table_name, page_id)->lock_shared();
}

void LockManager::unlock_page_shared(const std::string& table_name, uint32_t page_id) {
    short_page_latch(table_name, page_id)->unlock_shared();
}

void LockManager::lock_page_exclusive(const std::string& table_name, uint32_t page_id) {
    short_page_latch(table_name, page_id)->lock();
}

void LockManager::unlock_page_exclusive(const std::string& table_name, uint32_t page_id) {
    short_page_latch(table_name, page_id)->unlock();
}

bool LockManager::transaction_holds_exclusive(std::uint64_t txn_id) {
    return txn_id != 0 && !transaction_aborted(txn_id);
}
