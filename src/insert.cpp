#include "insert.h"
#include "file_handler.h"
#include "BPtree.h"
#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "data_page.h"
#include "lock_manager.h"
#include "recovery_manager.h"
#include "transaction_manager.h"
#include "tuple_serializer.h"
#include "vacuum.h"
#include <string>
#include <vector>
#include <iostream>
#include <cstring>
#include <limits>

// Removed local search_table definition as it is now in display.h/display.cpp

namespace {

class InsertRowLockGuard {
  public:
    InsertRowLockGuard(const std::string& table_name, const RID& rid)
        : table_name_(table_name), rid_(rid), locked_(false), transaction_lock_(false), ok_(true) {
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

    ~InsertRowLockGuard() {
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

}  // namespace

/*
 * What:
 * This insert path now uses the BufferPoolManager instead of talking directly
 * to DiskManager for every page access.
 *
 * Why:
 * Insert is one of the first places where page reuse matters. While searching
 * for a page with free space, the engine may touch several pages. A buffer
 * pool lets us cache those pages in RAM and update them through frames.
 *
 * Understanding:
 * Flow of one insert:
 * 1. Build tuple values in memory
 * 2. Serialize tuple to bytes
 * 3. Ask buffer pool for candidate pages
 * 4. If a page has space, modify that page in RAM
 * 5. Write a redo record into the recovery log
 * 6. Mark the frame dirty and unpin it
 * 7. Buffer pool flushes dirty pages back to disk later
 * 8. Store key -> RID in the B+ Tree
 *
 * Concept used:
 * - page-based storage
 * - slotted pages
 * - RID based row addressing
 * - buffer pool caching
 * - write-ahead logging
 * - dirty page tracking
 * - pin / unpin discipline
 *
 * Layman version:
 * - before, insert directly touched the disk page file
 * - now, insert first works on a RAM copy of the page, logs the final page
 *   image for crash recovery, and then lets the buffer pool send it to disk
 */
void insert_command(char tname[], const std::vector<TupleValue>& values, const std::vector<ColumnSchema>& schema) {
    // 1. Prepare the Index
    BPtree index(tname);
    
    // Check if primary key already exists (assuming first column is PK)
    int pk_value = values[0].int_value;
    if (index.search(pk_value).page_id != INVALID_PAGE_ID) {
        std::cout << "Error: Primary Key " << pk_value << " already exists." << std::endl;
        return;
    }

    // 2. Serialize the data into a tuple
    vector<char> tuple_data;
    if (!TupleSerializer::serialize(schema, values, tuple_data)) {
        std::cout << "Error: Failed to serialize tuple." << std::endl;
        return;
    }

    // 3. Manage the Data Storage (Page-Based)
    std::string data_path = table_data_path(tname).string();
    
    DiskManager data_disk(data_path, STORAGE_PAGE_SIZE);
    if (!data_disk.open_or_create()) {
        std::cout << "Error: Could not open data file." << std::endl;
        return;
    }

    BufferPoolManager buffer_pool(4, &data_disk);

    // 4. Find a page with enough space
    uint32_t target_page_id = INVALID_PAGE_ID;
    DataPage page;
    bool found_space = false;
    char* target_buffer = NULL;

    // Search existing pages for space
    for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
        char* frame_data = buffer_pool.fetch_page(i);
        if (frame_data == NULL) {
            continue;
        }

        page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);
        if (page.can_store(tuple_data.size())) {
            target_page_id = i;
            found_space = true;
            target_buffer = frame_data;
            break;
        }

        if (compact_page_buffer(frame_data)) {
            page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);
            if (page.can_store(tuple_data.size())) {
                target_page_id = i;
                found_space = true;
                target_buffer = frame_data;
                break;
            }
        }

        buffer_pool.unpin_page(i, false);
    }

    
    if (!found_space) {
        target_buffer = buffer_pool.new_page(target_page_id);
        if (target_buffer == NULL) {
            std::cout << "Error: Could not allocate a new page in the buffer pool." << std::endl;
            return;
        }

        page.initialize(target_page_id);
        std::memcpy(target_buffer, page.data(), STORAGE_PAGE_SIZE);
    }

    // 5. Insert tuple into the in-memory page frame
    uint16_t slot_id;
    if (!page.insert_tuple(tuple_data.data(), tuple_data.size(), slot_id)) {
        std::cout << "Error: Failed to insert tuple into page." << std::endl;
        buffer_pool.unpin_page(target_page_id, false);
        return;
    }

    RID rid(target_page_id, slot_id);
    InsertRowLockGuard inserted_row_lock(tname, rid);
    if (!inserted_row_lock.ok()) {
        std::cout << TransactionManager::last_error() << std::endl;
        buffer_pool.unpin_page(target_page_id, false);
        return;
    }

    RecoveryTicket recovery_ticket =
        RecoveryManager::log_insert_redo(tname, target_page_id, slot_id, pk_value, page.data());
    if (!recovery_ticket.valid) {
        std::cout << "Error: Failed to write recovery log." << std::endl;
        buffer_pool.unpin_page(target_page_id, false);
        return;
    }

    std::memcpy(target_buffer, page.data(), STORAGE_PAGE_SIZE);
    buffer_pool.unpin_page(target_page_id, true);
    buffer_pool.flush_page(target_page_id);

    // 6. Update the B+ Tree Index with the new RID
    index.insert(pk_value, rid);
    RecoveryManager::mark_insert_applied(recovery_ticket);

    std::cout << "Successfully inserted row into " << tname << " at RID(" 
              << target_page_id << ", " << slot_id << ")" << std::endl;
}

void bulk_insert_command(char tname[], const std::vector<std::vector<TupleValue>>& all_values, const std::vector<ColumnSchema>& schema) {
    // 1. Prepare the Index
    BPtree index(tname);
    
    // 2. Manage the Data Storage (Page-Based)
    std::string data_path = table_data_path(tname).string();
    
    DiskManager data_disk(data_path, STORAGE_PAGE_SIZE);
    if (!data_disk.open_or_create()) {
        std::cout << "Error: Could not open data file." << std::endl;
        return;
    }

    BufferPoolManager buffer_pool(4, &data_disk);

    // Keep track of the current page to avoid searching from page 0 every time
    uint32_t current_page_id = INVALID_PAGE_ID;
    char* current_buffer = NULL;
    DataPage current_page;

    for (const auto& values : all_values) {
        if (values.empty()) continue;

        // Check if primary key already exists (assuming first column is PK)
        int pk_value = values[0].int_value;
        if (index.search(pk_value).page_id != INVALID_PAGE_ID) {
            std::cout << "Error: Primary Key " << pk_value << " already exists. Skipping row." << std::endl;
            continue;
        }

        // Serialize the data into a tuple
        std::vector<char> tuple_data;
        if (!TupleSerializer::serialize(schema, values, tuple_data)) {
            std::cout << "Error: Failed to serialize tuple. Skipping row." << std::endl;
            continue;
        }

        // 4. Find/Use a page with enough space
        bool found_space = false;

        // Check if the currently pinned page has space
        if (current_page_id != INVALID_PAGE_ID) {
            if (current_page.can_store(tuple_data.size())) {
                found_space = true;
            } else {
                // Current page is full, unpin it and look for a new one
                buffer_pool.unpin_page(current_page_id, true);
                buffer_pool.flush_page(current_page_id);
                current_page_id = INVALID_PAGE_ID;
                current_buffer = NULL;
            }
        }

        if (!found_space) {
            // Search existing pages (starting from the last known page to be efficient)
            // For simplicity and correctness in this architecture, we search from 0 
            // but we only do this when the current page is full.
            for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
                char* frame_data = buffer_pool.fetch_page(i);
                if (frame_data == NULL) continue;

                current_page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);
                if (current_page.can_store(tuple_data.size())) {
                    current_page_id = i;
                    current_buffer = frame_data;
                    found_space = true;
                    break;
                }

                if (compact_page_buffer(frame_data)) {
                    current_page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);
                    if (current_page.can_store(tuple_data.size())) {
                        current_page_id = i;
                        current_buffer = frame_data;
                        found_space = true;
                        break;
                    }
                }
                buffer_pool.unpin_page(i, false);
            }
        }

        if (!found_space) {
            // Create a new page
            current_buffer = buffer_pool.new_page(current_page_id);
            if (current_buffer == NULL) {
                std::cout << "Error: Could not allocate a new page. Skipping row." << std::endl;
                continue;
            }
            current_page.initialize(current_page_id);
            std::memcpy(current_buffer, current_page.data(), STORAGE_PAGE_SIZE);
            found_space = true;
        }

        // 5. Insert tuple into the in-memory page frame
        uint16_t slot_id;
        if (!current_page.insert_tuple(tuple_data.data(), tuple_data.size(), slot_id)) {
            std::cout << "Error: Failed to insert tuple into page. Skipping row." << std::endl;
            // This shouldn't happen if can_store returned true
            continue;
        }

        RID rid(current_page_id, slot_id);
        InsertRowLockGuard inserted_row_lock(tname, rid);
        if (!inserted_row_lock.ok()) {
            std::cout << TransactionManager::last_error() << std::endl;
            continue;
        }

        std::memcpy(current_buffer, current_page.data(), STORAGE_PAGE_SIZE);
        // We keep the page pinned for the next iteration

        // 6. Update the B+ Tree Index with the new RID
        index.insert(pk_value, rid);

        std::cout << "Successfully inserted row into " << tname << " at RID(" 
                  << current_page_id << ", " << slot_id << ")" << std::endl;
    }

    // Final cleanup: unpin the last used page
    if (current_page_id != INVALID_PAGE_ID) {
        buffer_pool.unpin_page(current_page_id, true);
        buffer_pool.flush_page(current_page_id);
    }
}

void insert(){
    char tab[MAX_NAME];
    std::cout << "enter table name: ";
    std::cin >> tab;
    
    if(search_table(tab) == 0){
        printf("\nTable \" %s \" don't exist\n", tab);
        return ;
    }

    // Load Table Metadata
    table meta;
    FilePtr fp = open_file_read(tab, "r");
    if (!fp) return;
    fread(&meta, sizeof(table), 1, fp);
    fclose(fp);

    std::cout << "\nTable exists, enter data for " << meta.count << " columns:\n";

    std::vector<ColumnSchema> schema;
    std::vector<TupleValue> values;

    for(int i = 0; i < meta.count; i++){
        std::cout << meta.col[i].col_name << " (" 
                  << (meta.col[i].type == INT ? "INT" : "VARCHAR") << "): ";
        
        ColumnSchema col_schema(meta.col[i].col_name, 
                                meta.col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
                                meta.col[i].size);
        schema.push_back(col_schema);

        if(meta.col[i].type == INT){
            int val;
            if (!(std::cin >> val)) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cout << "Error: Invalid INT value for column '"
                          << meta.col[i].col_name << "'. Insert cancelled.\n";
                return;
            }
            values.push_back(TupleValue::FromInt(val));
        } else {
            std::string val;
            std::cin >> std::ws;
            std::getline(std::cin, val);
            values.push_back(TupleValue::FromVarchar(val));
        }
    }

    insert_command(tab, values, schema);
}
