#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <cstdint>
#include <string>

#include "storage_types.h"

/*
 * What:
 * LockManager provides the first concurrency-control layer for MiniDB.
 *
 * Why:
 * The project now has transactions and recovery, but concurrent requests can
 * still interleave in unsafe ways. A basic strict-2PL style lock layer is the
 * minimum step required before claiming safe multi-user writes.
 *
 * Understanding:
 * - autocommit SELECT/UPDATE/DELETE can use short RID-level locks
 * - transaction SELECT/UPDATE/DELETE use Strict 2PL RID-level locks
 * - INSERT/DDL may still use coarse database protection where needed
 * - transaction RID locks are released only at COMMIT/ROLLBACK
 *
 * Current scope:
 * - RID-level shared/exclusive locks
 * - waits-for graph deadlock detection
 * - lock wait timeout fallback
 * - coarse database-level protection for catalog/snapshot-sensitive paths
 *
 * This is not MVCC, predicate locking, next-key locking, or full production
 * serializable isolation. Phantom prevention for range predicates is future
 * work.
 */
class LockManager {
  public:
    static bool begin_transaction(std::uint64_t txn_id);
    static void end_transaction(std::uint64_t txn_id);

    static void lock_shared();
    static void unlock_shared();

    static void lock_exclusive();
    static void unlock_exclusive();

    static bool acquireShared(std::uint64_t txn_id,
                              const std::string& table_name,
                              const RID& rid);
    static bool acquireExclusive(std::uint64_t txn_id,
                                 const std::string& table_name,
                                 const RID& rid);
    static bool upgradeLock(std::uint64_t txn_id,
                            const std::string& table_name,
                            const RID& rid);
    static void releaseLocks(std::uint64_t txn_id);
    static void abortTransaction(std::uint64_t txn_id, const std::string& reason);
    static bool transaction_aborted(std::uint64_t txn_id);
    static std::string abort_reason(std::uint64_t txn_id);
    static void clear_transaction_state(std::uint64_t txn_id);

    static void lock_row_shared(const std::string& table_name, const RID& rid);
    static void unlock_row_shared(const std::string& table_name, const RID& rid);

    static void lock_row_exclusive(const std::string& table_name, const RID& rid);
    static void unlock_row_exclusive(const std::string& table_name, const RID& rid);

    static void lock_page_shared(const std::string& table_name, uint32_t page_id);
    static void unlock_page_shared(const std::string& table_name, uint32_t page_id);

    static void lock_page_exclusive(const std::string& table_name, uint32_t page_id);
    static void unlock_page_exclusive(const std::string& table_name, uint32_t page_id);

    static bool transaction_holds_exclusive(std::uint64_t txn_id);
};

#endif
