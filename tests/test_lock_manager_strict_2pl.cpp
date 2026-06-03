#include "lock_manager.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

void reset_txn(std::uint64_t txn_id) {
    LockManager::releaseLocks(txn_id);
    LockManager::clear_transaction_state(txn_id);
}

bool test_shared_compatible() {
    RID rid(1, 1);
    bool t1 = LockManager::begin_transaction(1) &&
              LockManager::acquireShared(1, "students", rid);
    bool t2 = LockManager::begin_transaction(2) &&
              LockManager::acquireShared(2, "students", rid);
    reset_txn(1);
    reset_txn(2);
    return t1 && t2;
}

bool test_write_blocks_write_timeout() {
    RID rid(1, 1);
    LockManager::begin_transaction(10);
    LockManager::begin_transaction(11);
    bool first = LockManager::acquireExclusive(10, "students", rid);
    bool second = LockManager::acquireExclusive(11, "students", rid);
    bool timed_out = !second && LockManager::transaction_aborted(11);
    reset_txn(10);
    reset_txn(11);
    return first && timed_out;
}

bool test_deadlock_detection() {
    RID a(1, 1);
    RID b(1, 2);
    LockManager::begin_transaction(20);
    LockManager::begin_transaction(21);
    LockManager::acquireExclusive(20, "students", a);
    LockManager::acquireExclusive(21, "students", b);

    std::atomic<bool> t1_done(false);
    std::atomic<bool> t2_done(false);
    std::atomic<bool> t1_ok(false);
    std::atomic<bool> t2_ok(false);

    std::thread t1([&]() {
        t1_ok = LockManager::acquireExclusive(20, "students", b);
        t1_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::thread t2([&]() {
        t2_ok = LockManager::acquireExclusive(21, "students", a);
        t2_done = true;
    });

    t1.join();
    t2.join();

    bool one_aborted = LockManager::transaction_aborted(20) ||
                       LockManager::transaction_aborted(21);
    bool one_continued = t1_ok || t2_ok;

    reset_txn(20);
    reset_txn(21);
    return t1_done && t2_done && one_aborted && one_continued;
}

}  // namespace

int main() {
    bool ok = true;

    ok = test_shared_compatible() && ok;
    std::cout << "Case A shared locks compatible: "
              << (ok ? "ok" : "failed") << "\n";

    bool timeout_ok = test_write_blocks_write_timeout();
    ok = timeout_ok && ok;
    std::cout << "Case C/E write conflict timeout fallback: "
              << (timeout_ok ? "ok" : "failed") << "\n";

    bool deadlock_ok = test_deadlock_detection();
    ok = deadlock_ok && ok;
    std::cout << "Case D deadlock detection: "
              << (deadlock_ok ? "ok" : "failed") << "\n";

    return ok ? 0 : 1;
}

