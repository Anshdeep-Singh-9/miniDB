#include "transaction_manager.h"

#include <filesystem>
#include <fstream>

#include "lock_manager.h"
#include "recovery_manager.h"
#include "file_handler.h"

namespace fs = std::filesystem;

bool TransactionManager::active_ = false;
std::string TransactionManager::error_;
static std::uint64_t g_current_txn_id = 0;
static std::uint64_t g_next_txn_id = 1;

const char* TransactionManager::snapshot_root() {
    // What: directory used to store a table snapshot while a transaction is active.
    // Why: current ROLLBACK is implemented by restoring copied files, not by undo records.
    // Example: BEGIN creates system/txn_snapshot before changes are made.
    static std::string path;
    path = txn_snapshot_root().string();
    return path.c_str();
}

bool TransactionManager::remove_snapshot_dir() {
    std::error_code ec;
    fs::remove_all(snapshot_root(), ec);
    return !ec;
}

bool TransactionManager::copy_tables_to_snapshot() {
    // What: copy current table files into a temporary transaction snapshot.
    // Why: rollback needs a known-good copy of the database state from before BEGIN.
    // Example: BEGIN copies table/students/data.dat and index.dat into system/txn_snapshot.
    std::error_code ec;
    fs::create_directories(snapshot_root(), ec);
    if (ec) {
        error_ = "Could not create transaction snapshot directory.";
        return false;
    }

    const fs::path data_root = table_root();
    if (!fs::exists(data_root) || !fs::is_directory(data_root)) {
        return true;
    }

    for (fs::directory_iterator it(data_root, ec); !ec && it != fs::directory_iterator(); ++it) {
        const fs::path src = it->path();
        if (!fs::is_directory(src)) continue;

        const fs::path dst = fs::path(snapshot_root()) / src.filename();
        fs::create_directories(dst, ec);
        if (ec) {
            error_ = "Could not create table snapshot directory.";
            return false;
        }

        for (fs::directory_iterator fit(src, ec); !ec && fit != fs::directory_iterator(); ++fit) {
            const fs::path fsrc = fit->path();
            if (!fs::is_regular_file(fsrc)) continue;
            fs::copy_file(fsrc, dst / fsrc.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error_ = "Could not copy table files to snapshot.";
                return false;
            }
        }
        if (ec) {
            error_ = "Failed while reading table directory.";
            return false;
        }
    }
    if (ec) {
        error_ = "Failed while scanning tables.";
        return false;
    }

    const fs::path list_path = table_list_path();
    if (fs::exists(list_path)) {
        fs::copy_file(list_path, fs::path(snapshot_root()) / "table_list",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error_ = "Could not snapshot table_list.";
            return false;
        }
    }

    return true;
}

bool TransactionManager::restore_snapshot_to_tables() {
    // What: replace current table files with the snapshot saved at BEGIN.
    // Why: this gives simple atomic rollback for the educational transaction layer.
    // Example: after ROLLBACK, rows inserted during the transaction disappear.
    std::error_code ec;
    const fs::path data_root = table_root();
    const fs::path snap_root(snapshot_root());
    if (!fs::exists(snap_root) || !fs::is_directory(snap_root)) {
        error_ = "No transaction snapshot found for rollback.";
        return false;
    }

    for (fs::directory_iterator it(data_root, ec); !ec && it != fs::directory_iterator(); ++it) {
        const fs::path p = it->path();
        if (fs::is_directory(p) && p.filename() != "table_list") {
            fs::remove_all(p, ec);
            if (ec) {
                error_ = "Could not clear current table state.";
                return false;
            }
        }
    }

    for (fs::directory_iterator it(snap_root, ec); !ec && it != fs::directory_iterator(); ++it) {
        const fs::path src = it->path();
        if (src.filename() == "table_list") continue;
        if (!fs::is_directory(src)) continue;

        const fs::path dst = data_root / src.filename();
        fs::create_directories(dst, ec);
        if (ec) {
            error_ = "Could not recreate table directory.";
            return false;
        }

        for (fs::directory_iterator fit(src, ec); !ec && fit != fs::directory_iterator(); ++fit) {
            const fs::path fsrc = fit->path();
            if (!fs::is_regular_file(fsrc)) continue;
            fs::copy_file(fsrc, dst / fsrc.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error_ = "Could not restore table files from snapshot.";
                return false;
            }
        }
        if (ec) {
            error_ = "Failed while restoring table directory.";
            return false;
        }
    }

    const fs::path snap_table_list = snap_root / "table_list";
    if (fs::exists(snap_table_list)) {
        fs::copy_file(snap_table_list, data_root / "table_list",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error_ = "Could not restore table_list.";
            return false;
        }
    }

    return true;
}

bool TransactionManager::begin() {
    // What: start one transaction, register it with LockManager, and snapshot table files.
    // Why: MiniDB needs a controlled boundary for COMMIT/ROLLBACK behavior.
    // Example: BEGIN; INSERT ...; ROLLBACK; restores the previous table state.
    if (active_) {
        error_ = "A transaction is already active.";
        return false;
    }

    error_.clear();
    g_current_txn_id = g_next_txn_id++;

    if (!LockManager::begin_transaction(g_current_txn_id)) {
        error_ = "Could not start transaction.";
        g_current_txn_id = 0;
        return false;
    }

    if (!remove_snapshot_dir()) {
        error_ = "Could not reset old transaction snapshot directory.";
        LockManager::end_transaction(g_current_txn_id);
        g_current_txn_id = 0;
        return false;
    }
    if (!copy_tables_to_snapshot()) {
        remove_snapshot_dir();
        LockManager::end_transaction(g_current_txn_id);
        g_current_txn_id = 0;
        return false;
    }

    RecoveryManager::set_transaction_context(g_current_txn_id, true);
    active_ = true;
    return true;
}

bool TransactionManager::commit() {
    // What: finalize a transaction and mark its WAL records committed.
    // Why: committed changes should survive restart and should not be rolled back.
    // Example: BEGIN; INSERT ...; COMMIT; keeps the inserted row.
    if (!active_) {
        error_ = "No active transaction to commit.";
        return false;
    }

    const std::uint64_t txn_id = g_current_txn_id;
    if (LockManager::transaction_aborted(txn_id)) {
        error_ = LockManager::abort_reason(txn_id);
        if (error_.empty()) {
            error_ = "Transaction was aborted before commit.";
        }
        return abort_current_due_to_lock(error_);
    }

    if (!RecoveryManager::commit_transaction(txn_id)) {
        error_ = "Could not finalize WAL commit markers.";
        return false;
    }

    RecoveryManager::set_transaction_context(0, false);

    active_ = false;
    g_current_txn_id = 0;
    LockManager::end_transaction(txn_id);

    if (!remove_snapshot_dir()) {
        error_ = "Committed, but failed to clear snapshot directory.";
        return false;
    }

    return true;
}

bool TransactionManager::rollback() {
    // What: abort a transaction and restore table files from the BEGIN snapshot.
    // Why: uncommitted changes should be removed from the user-visible database state.
    // Example: BEGIN; UPDATE ...; ROLLBACK; returns the row to its old value.
    if (!active_) {
        error_ = "No active transaction to rollback.";
        return false;
    }

    const std::uint64_t txn_id = g_current_txn_id;

    if (!RecoveryManager::abort_transaction(txn_id)) {
        error_ = "Could not finalize WAL abort markers.";
        return false;
    }

    if (!restore_snapshot_to_tables()) {
        return false;
    }

    RecoveryManager::set_transaction_context(0, false);
    active_ = false;
    g_current_txn_id = 0;
    LockManager::end_transaction(txn_id);
    if (!remove_snapshot_dir()) {
        error_ = "Rolled back, but failed to clear snapshot directory.";
        return false;
    }

    return true;
}

bool TransactionManager::in_transaction() {
    return active_;
}

std::uint64_t TransactionManager::current_txn_id() {
    return g_current_txn_id;
}

bool TransactionManager::current_transaction_holds_write_lock() {
    return LockManager::transaction_holds_exclusive(g_current_txn_id);
}

bool TransactionManager::current_transaction_aborted() {
    return active_ && LockManager::transaction_aborted(g_current_txn_id);
}

bool TransactionManager::abort_current_due_to_lock(const std::string& reason) {
    if (!active_) {
        error_ = reason;
        return false;
    }

    const std::uint64_t txn_id = g_current_txn_id;
    error_ = reason.empty() ? LockManager::abort_reason(txn_id) : reason;
    if (error_.empty()) {
        error_ = "Transaction aborted due to lock conflict.";
    }

    RecoveryManager::abort_transaction(txn_id);
    restore_snapshot_to_tables();
    RecoveryManager::set_transaction_context(0, false);
    active_ = false;
    g_current_txn_id = 0;
    LockManager::end_transaction(txn_id);
    remove_snapshot_dir();
    return false;
}

std::string TransactionManager::last_error() {
    return error_;
}
