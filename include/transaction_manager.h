#ifndef TRANSACTION_MANAGER_H
#define TRANSACTION_MANAGER_H

#include <cstdint>
#include <string>

class TransactionManager {
  public:
    static bool begin();
    static bool commit();
    static bool rollback();
    static bool in_transaction();
    static std::uint64_t current_txn_id();
    static bool current_transaction_holds_write_lock();
    static bool current_transaction_aborted();
    static bool abort_current_due_to_lock(const std::string& reason);
    static std::string last_error();

  private:
    static bool active_;
    static std::string error_;
    static const char* snapshot_root();
    static bool remove_snapshot_dir();
    static bool copy_tables_to_snapshot();
    static bool restore_snapshot_to_tables();
};

#endif
