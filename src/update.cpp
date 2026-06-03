#include "update.h"

#include "BPtree.h"
#include "data_page.h"
#include "disk_manager.h"
#include "display.h"
#include "file_handler.h"
#include "lock_manager.h"
#include "recovery_manager.h"
#include "storage_types.h"
#include "transaction_manager.h"
#include "tuple_serializer.h"
#include "vacuum.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class ExclusiveRowLockGuard {
  public:
    ExclusiveRowLockGuard(const std::string& table_name, const RID& rid)
        : table_name_(table_name), rid_(rid), locked_(false), transaction_lock_(false), ok_(true) {
        if (rid_.page_id != INVALID_PAGE_ID && rid_.slot_id != INVALID_SLOT_ID) {
            if (TransactionManager::in_transaction()) {
                transaction_lock_ = true;
                ok_ = LockManager::acquireExclusive(TransactionManager::current_txn_id(),
                                                    table_name_, rid_);
                if (!ok_) {
                    TransactionManager::abort_current_due_to_lock(
                        LockManager::abort_reason(TransactionManager::current_txn_id()));
                }
            } else {
                LockManager::lock_row_exclusive(table_name_, rid_);
                locked_ = true;
            }
        }
    }

    ~ExclusiveRowLockGuard() {
        if (locked_ && !transaction_lock_) {
            LockManager::unlock_row_exclusive(table_name_, rid_);
        }
    }

    bool ok() const { return ok_; }

  private:
    std::string table_name_;
    RID rid_;
    bool locked_;
    bool transaction_lock_;
    bool ok_;
};

class ExclusivePageLockGuard {
  public:
    ExclusivePageLockGuard(const std::string& table_name, uint32_t page_id)
        : table_name_(table_name), page_id_(page_id), locked_(false) {
        if (page_id_ != INVALID_PAGE_ID) {
            LockManager::lock_page_exclusive(table_name_, page_id_);
            locked_ = true;
        }
    }

    ~ExclusivePageLockGuard() {
        unlock();
    }

    void unlock() {
        if (locked_) {
            LockManager::unlock_page_exclusive(table_name_, page_id_);
            locked_ = false;
        }
    }

  private:
    std::string table_name_;
    uint32_t page_id_;
    bool locked_;
};

RID insert_relocated_tuple(DiskManager& disk,
                           const std::string& tab_name,
                           int primary_key,
                           const std::vector<char>& new_bytes,
                           std::vector<RecoveryTicket>& recovery_tickets,
                           uint32_t already_locked_page_id = INVALID_PAGE_ID) {
    // What: place an updated tuple somewhere else when it no longer fits in its old slot.
    // Why: VARCHAR updates can become larger, so overwriting the old byte range may corrupt nearby rows.
    // Example: name "A" updated to "Aryan Saini" may need a new RID and index update.
    for (uint32_t pid = 0; pid < disk.page_count(); ++pid) {
        std::unique_ptr<ExclusivePageLockGuard> page_lock;
        if (pid != already_locked_page_id) {
            page_lock.reset(new ExclusivePageLockGuard(tab_name, pid));
        }
        char pg[STORAGE_PAGE_SIZE];
        if (!disk.read_page(pid, pg)) continue;

        DataPage dp;
        dp.load_from_buffer(pg, STORAGE_PAGE_SIZE);

        if (dp.can_store(new_bytes.size())) {
            uint16_t new_slot;
            if (dp.insert_tuple(new_bytes.data(), new_bytes.size(), new_slot)) {
                std::memcpy(pg, dp.data(), STORAGE_PAGE_SIZE);
                RecoveryTicket ticket =
                    RecoveryManager::log_page_redo(tab_name, pid, new_slot, primary_key, pg, true);
                if (!ticket.valid) {
                    return RID();
                }
                if (!disk.write_page(pid, pg)) {
                    return RID();
                }
                recovery_tickets.push_back(ticket);
                return RID(pid, new_slot);
            }
        }

        if (compact_page_buffer(pg)) {
            dp.load_from_buffer(pg, STORAGE_PAGE_SIZE);
            if (dp.can_store(new_bytes.size())) {
                uint16_t new_slot;
                if (dp.insert_tuple(new_bytes.data(), new_bytes.size(), new_slot)) {
                    std::memcpy(pg, dp.data(), STORAGE_PAGE_SIZE);
                    RecoveryTicket ticket =
                        RecoveryManager::log_page_redo(tab_name, pid, new_slot, primary_key, pg, true);
                    if (!ticket.valid) {
                        return RID();
                    }
                    RecoveryManager::maybe_crash_after_wal("update");
                    if (!disk.write_page(pid, pg)) {
                        return RID();
                    }
                    recovery_tickets.push_back(ticket);
                    return RID(pid, new_slot);
                }
            }
        }
    }

    uint32_t new_pid = disk.allocate_page();
    if (new_pid == INVALID_PAGE_ID) {
        return RID();
    }

    std::unique_ptr<ExclusivePageLockGuard> page_lock;
    if (new_pid != already_locked_page_id) {
        page_lock.reset(new ExclusivePageLockGuard(tab_name, new_pid));
    }
    DataPage dp;
    dp.initialize(new_pid);

    uint16_t new_slot;
    if (!dp.insert_tuple(new_bytes.data(), new_bytes.size(), new_slot)) {
        return RID();
    }

    RecoveryTicket ticket =
        RecoveryManager::log_page_redo(tab_name, new_pid, new_slot, primary_key, dp.data(), true);
    if (!ticket.valid) {
        return RID();
    }

    if (!disk.write_page(new_pid, dp.data())) {
        return RID();
    }

    recovery_tickets.push_back(ticket);

    return RID(new_pid, new_slot);
}

bool apply_update_at_rid(const std::string& tab_name,
                         const std::vector<ColumnSchema>& schema,
                         const RID& rid,
                         int set_col_idx,
                         const std::string& new_val_str) {
    // What: update one row already located by RID.
    // Why: both B+ Tree lookup and scan paths eventually need the same "modify tuple bytes" logic.
    // Example: RID(0, 2) is read, deserialized, changed, serialized, WAL-logged, and written back.
    ExclusivePageLockGuard page_lock(tab_name, rid.page_id);
    ExclusiveRowLockGuard row_lock(tab_name, rid);
    if (!row_lock.ok()) {
        std::cout << TransactionManager::last_error() << "\n";
        return false;
    }

    std::string data_path = table_data_path(tab_name).string();
    DiskManager disk(data_path);
    if (!disk.open_or_create()) {
        std::cout << "Error: Cannot open data file.\n";
        return false;
    }

    char buf[STORAGE_PAGE_SIZE];
    if (!disk.read_page(rid.page_id, buf)) {
        std::cout << "Error: Cannot read page " << rid.page_id << ".\n";
        return false;
    }

    DataPage dp;
    dp.load_from_buffer(buf, STORAGE_PAGE_SIZE);

    std::vector<char> old_bytes;
    if (!dp.read_tuple(rid.slot_id, old_bytes)) {
        std::cout << "Error: Cannot read slot " << rid.slot_id << ".\n";
        return false;
    }

    std::vector<TupleValue> values;
    if (!TupleSerializer::deserialize(schema, old_bytes, values)) {
        std::cout << "Error: Cannot deserialize tuple.\n";
        return false;
    }

    const ColumnSchema& col = schema[set_col_idx];
    if (col.type == STORAGE_COLUMN_INT) {
        try {
            size_t used = 0;
            int new_int = std::stoi(new_val_str, &used);
            if (used != new_val_str.size()) throw std::invalid_argument("");
            values[set_col_idx] = TupleValue::FromInt(new_int);
        } catch (...) {
            std::cout << "Error: '" << new_val_str << "' is not a valid INT for column '"
                      << col.name << "'.\n";
            return false;
        }
    } else {
        if ((int)new_val_str.size() > (int)col.max_length) {
            std::cout << "Error: Value too long for column '" << col.name << "'.\n";
            return false;
        }
        values[set_col_idx] = TupleValue::FromVarchar(new_val_str);
    }

    std::vector<char> new_bytes;
    if (!TupleSerializer::serialize(schema, values, new_bytes)) {
        std::cout << "Error: Cannot serialize updated tuple.\n";
        return false;
    }

    SlotEntry* slots = reinterpret_cast<SlotEntry*>(buf + sizeof(PageHeader));
    SlotEntry& slot = slots[rid.slot_id];

    if (new_bytes.size() <= slot.length) {
        std::memcpy(buf + slot.offset, new_bytes.data(), new_bytes.size());
        slot.length = static_cast<uint16_t>(new_bytes.size());
        RecoveryTicket ticket =
            RecoveryManager::log_page_redo(tab_name, rid.page_id, rid.slot_id, values[0].int_value, buf, false);
        if (!ticket.valid) {
            std::cout << "Error: Could not write recovery log for UPDATE.\n";
            return false;
        }
        if (!disk.write_page(rid.page_id, buf)) {
            return false;
        }
        return RecoveryManager::mark_page_applied(ticket);
    }

    int pk_value = values[0].int_value;
    slot.length = 0;
    RecoveryTicket dead_slot_ticket =
        RecoveryManager::log_page_redo(tab_name, rid.page_id, rid.slot_id, pk_value, buf, false);
    if (!dead_slot_ticket.valid) {
        std::cout << "Error: Could not write recovery log for slot relocation.\n";
        return false;
    }
    if (!disk.write_page(rid.page_id, buf)) {
        return false;
    }
    page_lock.unlock();

    std::vector<RecoveryTicket> recovery_tickets;
    recovery_tickets.push_back(dead_slot_ticket);

    RID new_rid = insert_relocated_tuple(disk, tab_name, pk_value, new_bytes,
                                         recovery_tickets);
    if (new_rid.page_id == INVALID_PAGE_ID) {
        std::cout << "Error: Could not relocate updated tuple.\n";
        return false;
    }

    BPtree index(tab_name.c_str());
    if (!index.update_rid(pk_value, new_rid)) {
        std::cout << "Warning: B+ Tree RID update failed for key " << pk_value << ".\n";
    }

    for (std::size_t i = 0; i < recovery_tickets.size(); ++i) {
        RecoveryManager::mark_page_applied(recovery_tickets[i]);
    }

    return true;
}

int update_via_bptree(const std::string& tab_name,
                      const std::vector<ColumnSchema>& schema,
                      const UpdateStatement& stmt,
                      int set_col_idx) {
    // What: update one row by primary-key lookup through B+ Tree.
    // Why: primary-key WHERE gives a direct RID, so update can avoid scanning all table pages.
    // Example: UPDATE students SET dept = ECE WHERE id = 1;
    int pk_value;
    try {
        size_t used = 0;
        pk_value = std::stoi(stmt.where_value, &used);
        if (used != stmt.where_value.size()) throw std::invalid_argument("");
    } catch (...) {
        std::cout << "Error: WHERE value '" << stmt.where_value
                  << "' is not a valid INT for primary key column.\n";
        return 0;
    }

    BPtree index(tab_name.c_str());
    RID rid = index.search(pk_value);
    if (rid.page_id == INVALID_PAGE_ID) {
        std::cout << "No row found with " << stmt.where_column << " = " << pk_value << ".\n";
        return 0;
    }

    if (!apply_update_at_rid(tab_name, schema, rid, set_col_idx, stmt.set_value)) {
        return 0;
    }

    std::cout << "Updated 1 row where " << stmt.where_column << " = " << pk_value << ".\n";
    return 1;
}

int update_via_linear_scan(const std::string& tab_name,
                           const std::vector<ColumnSchema>& schema,
                           const UpdateStatement& stmt,
                           int set_col_idx,
                           int where_col_idx) {
    // What: update every row whose non-indexed WHERE column matches.
    // Why: without a secondary index, the only correct path is checking each tuple.
    // Example: UPDATE students SET dept = ECE WHERE name = Aryan;
    std::string data_path = table_data_path(tab_name).string();
    DiskManager disk(data_path);
    if (!disk.open_or_create()) {
        std::cout << "Error: Cannot open data file.\n";
        return 0;
    }

    int updated_count = 0;

    struct Relocation {
        int pk_value;
        std::vector<char> new_bytes;
    };
    std::vector<Relocation> pending;
    std::vector<RecoveryTicket> page_tickets;

    for (uint32_t page_id = 0; page_id < disk.page_count(); ++page_id) {
        ExclusivePageLockGuard page_lock(tab_name, page_id);
        char buf[STORAGE_PAGE_SIZE];
        if (!disk.read_page(page_id, buf)) continue;

        DataPage dp;
        dp.load_from_buffer(buf, STORAGE_PAGE_SIZE);
        bool page_dirty = false;
        std::vector<std::unique_ptr<ExclusiveRowLockGuard>> row_locks;
        bool lock_failed = false;

        for (uint16_t slot_id = 0; slot_id < dp.slot_count(); ++slot_id) {
            std::vector<char> old_bytes;
            if (!dp.read_tuple(slot_id, old_bytes)) continue;

            std::vector<TupleValue> values;
            if (!TupleSerializer::deserialize(schema, old_bytes, values)) continue;

            const TupleValue& cell = values[where_col_idx];
            bool matches = false;
            if (cell.type == STORAGE_COLUMN_INT) {
                try {
                    matches = (cell.int_value == std::stoi(stmt.where_value));
                } catch (...) {
                    matches = false;
                }
            } else {
                matches = (cell.string_value == stmt.where_value);
            }

            if (!matches) continue;

            RID current_rid(page_id, slot_id);
            row_locks.emplace_back(new ExclusiveRowLockGuard(tab_name, current_rid));
            if (!row_locks.back()->ok()) {
                lock_failed = true;
                break;
            }

            const ColumnSchema& col = schema[set_col_idx];
            if (col.type == STORAGE_COLUMN_INT) {
                try {
                    size_t used = 0;
                    int nv = std::stoi(stmt.set_value, &used);
                    if (used != stmt.set_value.size()) throw std::invalid_argument("");
                    values[set_col_idx] = TupleValue::FromInt(nv);
                } catch (...) {
                    std::cout << "Error: '" << stmt.set_value
                              << "' is not a valid INT. Skipping row.\n";
                    continue;
                }
            } else {
                if ((int)stmt.set_value.size() > (int)col.max_length) {
                    std::cout << "Error: Value too long for column '" << col.name
                              << "'. Skipping row.\n";
                    continue;
                }
                values[set_col_idx] = TupleValue::FromVarchar(stmt.set_value);
            }

            std::vector<char> new_bytes;
            if (!TupleSerializer::serialize(schema, values, new_bytes)) continue;

            SlotEntry* slots = reinterpret_cast<SlotEntry*>(buf + sizeof(PageHeader));
            SlotEntry& slot = slots[slot_id];

            if (new_bytes.size() <= slot.length) {
                std::memcpy(buf + slot.offset, new_bytes.data(), new_bytes.size());
                slot.length = static_cast<uint16_t>(new_bytes.size());
                page_dirty = true;
                updated_count++;
            } else {
                int pk_value = values[0].int_value;
                slot.length = 0;
                page_dirty = true;
                pending.push_back({pk_value, new_bytes});
            }
        }

        if (lock_failed) {
            return 0;
        }

        if (page_dirty) {
            RecoveryTicket ticket =
                RecoveryManager::log_page_redo(tab_name, page_id, 0, 0, buf, false);
            if (!ticket.valid) {
                std::cout << "Error: Could not write recovery log for updated page " << page_id << ".\n";
            } else if (disk.write_page(page_id, buf)) {
                page_tickets.push_back(ticket);
            }
        }
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        RID new_rid = insert_relocated_tuple(disk, tab_name, pending[i].pk_value,
                                             pending[i].new_bytes, page_tickets);
        if (new_rid.page_id == INVALID_PAGE_ID) {
            std::cout << "Error: Could not relocate tuple for pk="
                      << pending[i].pk_value << ".\n";
            continue;
        }

        BPtree index(tab_name.c_str());
        if (!index.update_rid(pending[i].pk_value, new_rid)) {
            std::cout << "Warning: B+ Tree update failed for pk="
                      << pending[i].pk_value << ".\n";
        }
        updated_count++;
    }

    for (std::size_t i = 0; i < page_tickets.size(); ++i) {
        RecoveryManager::mark_page_applied(page_tickets[i]);
    }

    if (updated_count == 0) {
        std::cout << "No rows matched WHERE " << stmt.where_column
                  << " = " << stmt.where_value << ".\n";
    } else {
        std::cout << "Updated " << updated_count << " row(s) where "
                  << stmt.where_column << " = " << stmt.where_value << ".\n";
    }

    return updated_count;
}

}  // namespace

void execute_update(const UpdateStatement& stmt) {
    // What: validate UPDATE command and route it to indexed or scan-based execution.
    // Why: the parser gives a structured UpdateStatement; this layer checks schema and storage rules.
    // Example: primary key cannot be updated because B+ Tree keys must stay stable.
    char tab[MAX_NAME];
    strncpy(tab, stmt.table_name.c_str(), MAX_NAME - 1);
    tab[MAX_NAME - 1] = '\0';

    if (search_table(tab) == 0) {
        std::cout << "Error: Table '" << stmt.table_name << "' does not exist.\n";
        return;
    }

    table* meta = fetch_meta_data(stmt.table_name);
    if (meta == NULL) {
        std::cout << "Error: Could not load metadata for '" << stmt.table_name << "'.\n";
        return;
    }

    int set_col_idx = -1;
    for (int i = 0; i < meta->count; i++) {
        if (stmt.set_column == meta->col[i].col_name) {
            set_col_idx = i;
            break;
        }
    }
    if (set_col_idx == -1) {
        std::cout << "Error: Column '" << stmt.set_column << "' does not exist.\n";
        delete meta;
        return;
    }
    if (set_col_idx == 0) {
        std::cout << "Error: Cannot update primary key column '" << stmt.set_column << "'.\n";
        delete meta;
        return;
    }

    int where_col_idx = -1;
    for (int i = 0; i < meta->count; i++) {
        if (stmt.where_column == meta->col[i].col_name) {
            where_col_idx = i;
            break;
        }
    }
    if (where_col_idx == -1) {
        std::cout << "Error: WHERE column '" << stmt.where_column << "' does not exist.\n";
        delete meta;
        return;
    }

    std::vector<ColumnSchema> schema;
    for (int i = 0; i < meta->count; i++) {
        schema.push_back(ColumnSchema(
            meta->col[i].col_name,
            meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
            meta->col[i].size));
    }

    bool is_pk_where = (where_col_idx == 0);
    bool pk_is_int = (meta->col[0].type == INT);

    if (is_pk_where && pk_is_int) {
        update_via_bptree(stmt.table_name, schema, stmt, set_col_idx);
    } else {
        update_via_linear_scan(stmt.table_name, schema, stmt, set_col_idx, where_col_idx);
    }

    delete meta;
}
