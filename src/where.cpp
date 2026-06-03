#include "where.h"
#include "BPtree.h"
#include "buffer_pool_manager.h"
#include "data_page.h"
#include "disk_manager.h"
#include "display.h"
#include "file_handler.h"
#include "lock_manager.h"
#include "transaction_manager.h"
#include "tuple_serializer.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define BLUE    "\033[34m"
#define WHITE   "\033[97m"

namespace {

class SharedPageLockGuard {
  public:
    SharedPageLockGuard(const std::string& table_name, uint32_t page_id)
        : table_name_(table_name), page_id_(page_id), locked_(false) {
        if (page_id_ != INVALID_PAGE_ID) {
            LockManager::lock_page_shared(table_name_, page_id_);
            locked_ = true;
        }
    }

    ~SharedPageLockGuard() {
        if (locked_) {
            LockManager::unlock_page_shared(table_name_, page_id_);
        }
    }

  private:
    std::string table_name_;
    uint32_t page_id_;
    bool locked_;
};

class SharedRowLockGuard {
  public:
    SharedRowLockGuard(const std::string& table_name, const RID& rid)
        : table_name_(table_name), rid_(rid), locked_(false), transaction_lock_(false), ok_(true) {
        if (rid_.page_id != INVALID_PAGE_ID && rid_.slot_id != INVALID_SLOT_ID) {
            if (TransactionManager::in_transaction()) {
                transaction_lock_ = true;
                ok_ = LockManager::acquireShared(TransactionManager::current_txn_id(),
                                                 table_name_, rid_);
                if (!ok_) {
                    TransactionManager::abort_current_due_to_lock(
                        LockManager::abort_reason(TransactionManager::current_txn_id()));
                }
            } else {
                LockManager::lock_row_shared(table_name_, rid_);
                locked_ = true;
            }
        }
    }

    ~SharedRowLockGuard() {
        if (locked_ && !transaction_lock_) {
            LockManager::unlock_row_shared(table_name_, rid_);
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

static std::string remove_quotes_local(std::string s) {
    // What: trim and remove matching quotes from a WHERE value.
    // Why: users may type dept = "CSE" or dept = CSE, but comparison should use CSE.
    // Example: "\"Aryan\"" becomes "Aryan".
    while (!s.empty() && isspace((unsigned char)s.front())) {
        s.erase(s.begin());
    }

    while (!s.empty() && isspace((unsigned char)s.back())) {
        s.pop_back();
    }

    if (s.size() >= 2) {
        if ((s.front() == '\'' && s.back() == '\'') ||
            (s.front() == '"' && s.back() == '"')) {
            return s.substr(1, s.size() - 2);
        }
    }

    return s;
}

static void print_result_table(const std::vector<ColumnSchema>& schema,
                               const std::vector<std::vector<TupleValue>>& table_data) {
    if (schema.empty()) {
        std::cout << "(No columns to display)\n";
        return;
    }

    std::cout << "\n" << BOLD << BLUE;

    for (const auto& col : schema) {
        std::cout << std::left << std::setw(20) << col.name;
    }

    std::cout << RESET << "\n";

    std::cout << BLUE;
    for (size_t i = 0; i < schema.size(); ++i) {
        std::cout << "--------------------";
    }
    std::cout << RESET << "\n";

    if (table_data.empty()) {
        std::cout << WHITE << "(No matching rows found)\n" << RESET;
    } else {
        std::cout << WHITE;

        for (const auto& row : table_data) {
            for (const auto& val : row) {
                if (val.type == STORAGE_COLUMN_INT) {
                    std::cout << std::left << std::setw(20) << val.int_value;
                } else {
                    std::cout << std::left << std::setw(20) << val.string_value;
                }
            }

            std::cout << "\n";
        }

        std::cout << RESET;
    }

    std::cout << BLUE;
    for (size_t i = 0; i < schema.size(); ++i) {
        std::cout << "--------------------";
    }
    std::cout << RESET << "\n";

    std::cout << BOLD << WHITE << "Total rows: " << table_data.size() << RESET << "\n\n";
}

static std::vector<TupleValue> project_row(const std::vector<TupleValue>& values,
                                           const std::vector<int>& col_indices_to_print) {
    std::vector<TupleValue> projected;

    for (int idx : col_indices_to_print) {
        projected.push_back(values[idx]);
    }

    return projected;
}

struct AggregateExprLocal {
    std::string func;
    std::string column;
};

struct SelectItemLocal {
    bool is_aggregate = false;
    std::string column;
};

static std::string to_lower_local(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return s;
}

static bool parse_aggregate_expr_local(const std::string& token, AggregateExprLocal& expr) {
    std::string lower = to_lower_local(token);
    const char* funcs[] = {"count", "sum", "avg", "min", "max"};
    for (std::size_t i = 0; i < 5; ++i) {
        const std::string prefix = std::string(funcs[i]) + "(";
        if (lower.size() > prefix.size() + 1 &&
            lower.compare(0, prefix.size(), prefix) == 0 &&
            lower[lower.size() - 1] == ')') {
            expr.func = funcs[i];
            expr.column = token.substr(prefix.size(), token.size() - prefix.size() - 1);
            return true;
        }
    }
    return false;
}

static void parse_select_items_local(const std::vector<std::string>& target_cols,
                                     std::vector<SelectItemLocal>& items,
                                     bool& has_aggregate) {
    items.clear();
    has_aggregate = false;
    for (std::size_t i = 0; i < target_cols.size(); ++i) {
        AggregateExprLocal expr;
        SelectItemLocal item;
        if (parse_aggregate_expr_local(target_cols[i], expr)) {
            item.is_aggregate = true;
            item.column = expr.column;
            has_aggregate = true;
        } else {
            item.column = target_cols[i];
        }
        items.push_back(item);
    }
}

static bool row_matches_where(const std::vector<TupleValue>& values,
                              int where_col_idx,
                              const WhereClause& where) {
    // What: compare one deserialized row value against the WHERE condition.
    // Why: linear scan needs a reusable row-level predicate check.
    // Example: for WHERE dept = CSE, compare the dept column's string with "CSE".
    const TupleValue& cell = values[where_col_idx];
    std::string where_value = remove_quotes_local(where.value);

    if (cell.type == STORAGE_COLUMN_INT) {
        try {
            int target = std::stoi(where_value);
            return cell.int_value == target;
        } catch (...) {
            return false;
        }
    }

    return cell.string_value == where_value;
}

static void search_via_bptree(const std::string& tab_name,
                              const std::vector<ColumnSchema>& schema,
                              const std::vector<int>& col_indices_to_print,
                              const std::vector<ColumnSchema>& output_schema,
                              const WhereClause& where,
                              QueryResult* res) {
    // What: find one row using primary-key index, then fetch its page and slot.
    // Why: indexed point lookup is much faster than scanning every tuple.
    // Example: SELECT * FROM students WHERE id = 10; uses B+ Tree key 10.
    std::cout << "\n[Search Strategy: B+ Tree Point Lookup on Primary Key]\n";

    int pk_value;

    try {
        pk_value = std::stoi(remove_quotes_local(where.value));
    } catch (...) {
        if (res) {
            res->success = false;
            res->message = "Error: WHERE value '" + where.value + "' cannot be parsed as INT";
        }
        std::cout << "Error: WHERE value '" << where.value
                  << "' cannot be parsed as INT for primary key column '"
                  << where.column << "'.\n";
        return;
    }

    BPtree index(tab_name.c_str());
    RID rid = index.search(pk_value);

    std::vector<std::vector<TupleValue>> output_rows;
    std::vector<std::vector<TupleValue>> matched_rows;

    if (rid.page_id != INVALID_PAGE_ID) {
        SharedPageLockGuard page_lock(tab_name, rid.page_id);
        SharedRowLockGuard row_lock(tab_name, rid);
        if (!row_lock.ok()) {
            if (res) {
                res->success = false;
                res->message = TransactionManager::last_error();
            }
            std::cout << TransactionManager::last_error() << "\n";
            return;
        }
        std::string data_path = table_data_path(tab_name).string();

        DiskManager data_disk(data_path);
        if (!data_disk.open_or_create()) {
            if (res) {
                res->success = false;
                res->message = "Error: Could not open data file.";
            }
            std::cout << "Error: Could not open data file.\n";
            return;
        }

        BufferPoolManager buffer_pool(4, &data_disk);
        char *page_buffer = buffer_pool.fetch_page(rid.page_id);
        if (page_buffer == NULL) {
            if (res) {
                res->success = false;
                res->message = "Error: Could not read page.";
            }
            std::cout << "Error: Could not read page " << rid.page_id << ".\n";
            return;
        }

        DataPage page;
        page.load_from_buffer(page_buffer, STORAGE_PAGE_SIZE);

        std::vector<char> tuple_data;

        if (!page.read_tuple(rid.slot_id, tuple_data)) {
            if (res) {
                res->success = false;
                res->message = "Error: Could not read slot.";
            }
            std::cout << "Error: Could not read slot " << rid.slot_id << " from page "
                      << rid.page_id << ".\n";
            buffer_pool.unpin_page(rid.page_id, false);
            return;
        }

        std::vector<TupleValue> values;

        if (!TupleSerializer::deserialize(schema, tuple_data, values)) {
            if (res) {
                res->success = false;
                res->message = "Error: Failed to deserialize tuple.";
            }
            std::cout << "Error: Failed to deserialize tuple.\n";
            buffer_pool.unpin_page(rid.page_id, false);
            return;
        }

        matched_rows.push_back(values);
        output_rows.push_back(project_row(values, col_indices_to_print));
        buffer_pool.unpin_page(rid.page_id, false);
    }

    if (res) {
        res->is_select = true;
        res->schema = output_schema;
        res->rows = output_rows;
        res->source_schema = schema;
        res->source_rows = matched_rows;
        res->strategy = "B+ Tree Point Lookup";
    }

    if (res == nullptr) {
        print_result_table(output_schema, output_rows);
    }
}

static void search_via_linear_scan(const std::string& tab_name,
                                   const std::vector<ColumnSchema>& schema,
                                   const std::vector<int>& col_indices_to_print,
                                   const std::vector<ColumnSchema>& output_schema,
                                   const WhereClause& where,
                                   int where_col_idx,
                                   QueryResult* res) {
    // What: scan every page and slot to find rows matching WHERE.
    // Why: non-primary columns do not have an index in the current engine.
    // Example: SELECT * FROM students WHERE dept = CSE; checks every row.
    std::cout << "\n[Search Strategy: Linear Scan]\n";

    std::string data_path = table_data_path(tab_name).string();

    DiskManager data_disk(data_path);
    if (!data_disk.open_or_create()) {
        if (res) {
            res->success = false;
            res->message = "Error: Could not open data file.";
        }
        std::cout << "Error: Could not open data file.\n";
        return;
    }

    BufferPoolManager buffer_pool(4, &data_disk);
    std::vector<std::vector<TupleValue>> output_rows;
    std::vector<std::vector<TupleValue>> matched_rows;
    bool lock_failed = false;

    for (uint32_t i = 0; i < data_disk.page_count() && !lock_failed; ++i) {
        SharedPageLockGuard page_lock(tab_name, i);
        char *page_buffer = buffer_pool.fetch_page(i);

        if (page_buffer == NULL) {
            continue;
        }

        DataPage page;
        page.load_from_buffer(page_buffer, STORAGE_PAGE_SIZE);

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            RID rid(i, slot_id);
            SharedRowLockGuard row_lock(tab_name, rid);
            if (!row_lock.ok()) {
                lock_failed = true;
                break;
            }

            std::vector<char> tuple_data;

            if (!page.read_tuple(slot_id, tuple_data)) {
                continue;
            }

            std::vector<TupleValue> values;

            if (!TupleSerializer::deserialize(schema, tuple_data, values)) {
                continue;
            }

            if (row_matches_where(values, where_col_idx, where)) {
                matched_rows.push_back(values);
                output_rows.push_back(project_row(values, col_indices_to_print));
            }
        }

        buffer_pool.unpin_page(i, false);
    }

    if (lock_failed) {
        if (res) {
            res->success = false;
            res->message = TransactionManager::last_error();
        }
        std::cout << TransactionManager::last_error() << "\n";
        return;
    }

    if (res) {
        res->is_select = true;
        res->schema = output_schema;
        res->rows = output_rows;
        res->source_schema = schema;
        res->source_rows = matched_rows;
        res->strategy = "Linear Scan";
    }

    if (res == nullptr) {
        print_result_table(output_schema, output_rows);
    }
}

void execute_select_where(const std::string& tab_name,
                          const std::vector<std::string>& target_cols,
                          const WhereClause& where,
                          QueryResult* res) {
    // What: validate SELECT-WHERE and choose indexed lookup or linear scan.
    // Why: the same SQL syntax can have very different execution cost depending on column indexed.
    // Example: WHERE id uses B+ Tree; WHERE dept uses linear scan.
    char tab[MAX_NAME];
    strncpy(tab, tab_name.c_str(), MAX_NAME - 1);
    tab[MAX_NAME - 1] = '\0';

    if (search_table(tab) == 0) {
        if (res) {
            res->success = false;
            res->message = "Table \"" + tab_name + "\" does not exist.";
        }
        std::cout << "\nTable \"" << tab_name << "\" does not exist.\n";
        return;
    }

    table* meta = fetch_meta_data(tab_name);

    if (meta == NULL) {
        if (res) {
            res->success = false;
            res->message = "Error: Could not load table metadata.";
        }
        std::cout << "Error: Could not load table metadata.\n";
        return;
    }

    int where_col_idx = -1;

    for (int i = 0; i < meta->count; i++) {
        if (where.column == meta->col[i].col_name) {
            where_col_idx = i;
            break;
        }
    }

    if (where_col_idx == -1) {
        if (res) {
            res->success = false;
            res->message = "Error: Column '" + where.column + "' does not exist.";
        }
        std::cout << "Error: Column '" << where.column
                  << "' does not exist in table '" << tab_name << "'.\n";
        delete meta;
        return;
    }

    bool select_all = (target_cols.size() == 1 && target_cols[0] == "*");
    std::vector<int> col_indices_to_print;
    std::vector<SelectItemLocal> select_items;
    bool aggregate_query = false;
    parse_select_items_local(target_cols, select_items, aggregate_query);

    if (aggregate_query) {
        for (std::size_t a = 0; a < select_items.size(); ++a) {
            if (select_items[a].column == "*") {
                continue;
            }
            bool found = false;
            for (int i = 0; i < meta->count; i++) {
                if (select_items[a].column == meta->col[i].col_name) {
                    if (std::find(col_indices_to_print.begin(), col_indices_to_print.end(), i) ==
                        col_indices_to_print.end()) {
                        col_indices_to_print.push_back(i);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (res) {
                    res->success = false;
                    res->message = "Error: Column '" + select_items[a].column + "' does not exist.";
                }
                std::cout << "Error: Column '" << select_items[a].column << "' does not exist.\n";
                delete meta;
                return;
            }
        }
    } else if (select_all) {
        for (int i = 0; i < meta->count; i++) {
            col_indices_to_print.push_back(i);
        }
    } else {
        for (const auto& t_col : target_cols) {
            bool found = false;

            for (int i = 0; i < meta->count; i++) {
                if (t_col == meta->col[i].col_name) {
                    col_indices_to_print.push_back(i);
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (res) {
                    res->success = false;
                    res->message = "Error: Column '" + t_col + "' does not exist.";
                }
                std::cout << "Error: Column '" << t_col << "' does not exist.\n";
                delete meta;
                return;
            }
        }
    }

    std::vector<ColumnSchema> schema;

    for (int i = 0; i < meta->count; i++) {
        schema.push_back(ColumnSchema(
            meta->col[i].col_name,
            meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
            meta->col[i].size
        ));
    }

    std::vector<ColumnSchema> output_schema;

    for (int idx : col_indices_to_print) {
        output_schema.push_back(schema[idx]);
    }

    bool is_pk_column = (where_col_idx == 0);
    bool pk_is_int = (meta->col[0].type == INT);

    if (is_pk_column && pk_is_int) {
        search_via_bptree(tab_name, schema, col_indices_to_print, output_schema, where, res);
    } else {
        search_via_linear_scan(tab_name, schema, col_indices_to_print,
                               output_schema, where, where_col_idx, res);
    }

    delete meta;
}
