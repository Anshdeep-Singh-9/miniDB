#include "alter.h"

#include "BPtree.h"
#include "buffer_pool_manager.h"
#include "data_page.h"
#include "disk_manager.h"
#include "display.h"
#include "file_handler.h"
#include "tuple_serializer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim_copy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string unquote(std::string s) {
    s = trim_copy(s);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string upper_copy(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    }
    return s;
}

bool parse_type_spec(const std::string& raw_type, int& type_out, int& size_out) {
    std::string type = trim_copy(raw_type);
    std::string upper = upper_copy(type);
    size_out = 0;

    std::size_t paren_start = upper.find('(');
    if (paren_start != std::string::npos) {
        std::size_t paren_end = upper.find(')', paren_start);
        if (paren_end == std::string::npos || paren_end <= paren_start + 1) {
            return false;
        }
        try {
            size_out = std::stoi(upper.substr(paren_start + 1, paren_end - paren_start - 1));
        } catch (...) {
            return false;
        }
        upper = upper.substr(0, paren_start);
    }

    if (upper == "INT" || upper == "INTEGER") {
        type_out = INT;
        size_out = sizeof(int);
        return true;
    }

    if (upper == "VARCHAR" || upper == "TEXT" || upper == "STRING") {
        type_out = VARCHAR;
        if (size_out <= 0) size_out = MAX_VARCHAR;
        return true;
    }

    return false;
}

int compute_record_size(table* meta) {
    int size = 0;
    meta->prefix[0] = 0;
    for (int i = 0; i < meta->count; ++i) {
        switch (meta->col[i].type) {
            case INT:
                meta->prefix[i + 1] = sizeof(int) + meta->prefix[i];
                size += sizeof(int);
                break;
            case VARCHAR:
                meta->prefix[i + 1] = sizeof(char) * (MAX_VARCHAR + 1) + meta->prefix[i];
                size += (MAX_VARCHAR + 1);
                break;
            default:
                return size;
        }
    }
    return size;
}

void build_schema_from_meta(const table* meta,
                            std::vector<ColumnSchema>& schema_out) {
    schema_out.clear();
    for (int i = 0; i < meta->count; ++i) {
        schema_out.push_back(ColumnSchema(
            meta->col[i].col_name,
            meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
            static_cast<uint16_t>(meta->col[i].size)));
    }
}

bool load_all_rows(const std::string& table_name,
                   const std::vector<ColumnSchema>& schema,
                   std::vector<std::vector<TupleValue>>& rows_out) {
    rows_out.clear();

    DiskManager disk(table_data_path(table_name).string(), STORAGE_PAGE_SIZE);
    if (!disk.open_or_create()) {
        return false;
    }

    BufferPoolManager buffer_pool(4, &disk);
    for (uint32_t pid = 0; pid < disk.page_count(); ++pid) {
        char* page_buffer = buffer_pool.fetch_page(pid);
        if (page_buffer == NULL) continue;

        DataPage page;
        page.load_from_buffer(page_buffer, STORAGE_PAGE_SIZE);
        for (uint16_t slot = 0; slot < page.slot_count(); ++slot) {
            std::vector<char> tuple_bytes;
            if (!page.read_tuple(slot, tuple_bytes)) continue;
            std::vector<TupleValue> values;
            if (TupleSerializer::deserialize(schema, tuple_bytes, values)) {
                rows_out.push_back(values);
            }
        }
        buffer_pool.unpin_page(pid, false);
    }
    return true;
}

bool rewrite_table_storage(const std::string& table_name,
                           const std::vector<ColumnSchema>& new_schema,
                           const std::vector<std::vector<TupleValue>>& rows) {
    std::error_code ec;
    fs::remove(table_data_path(table_name), ec);
    fs::remove(table_index_path(table_name), ec);
    fs::remove(table_wal_path(table_name), ec);

    DiskManager disk(table_data_path(table_name).string(), STORAGE_PAGE_SIZE);
    if (!disk.open_or_create()) {
        return false;
    }

    BufferPoolManager buffer_pool(4, &disk);
    BPtree index(table_name.c_str());

    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::vector<char> tuple_data;
        if (!TupleSerializer::serialize(new_schema, rows[r], tuple_data)) {
            return false;
        }

        uint32_t target_page_id = INVALID_PAGE_ID;
        DataPage page;
        char* target_buffer = NULL;

        if (disk.page_count() == 0) {
            target_buffer = buffer_pool.new_page(target_page_id);
            if (target_buffer == NULL) return false;
            page.initialize(target_page_id);
        } else {
            target_page_id = disk.page_count() - 1;
            target_buffer = buffer_pool.fetch_page(target_page_id);
            if (target_buffer == NULL) return false;
            page.load_from_buffer(target_buffer, STORAGE_PAGE_SIZE);
            if (!page.can_store(tuple_data.size())) {
                buffer_pool.unpin_page(target_page_id, false);
                target_buffer = buffer_pool.new_page(target_page_id);
                if (target_buffer == NULL) return false;
                page.initialize(target_page_id);
            }
        }

        uint16_t slot_id = 0;
        if (!page.insert_tuple(tuple_data.data(), tuple_data.size(), slot_id)) {
            if (target_page_id != INVALID_PAGE_ID) {
                buffer_pool.unpin_page(target_page_id, false);
            }
            return false;
        }

        std::memcpy(target_buffer, page.data(), STORAGE_PAGE_SIZE);
        buffer_pool.unpin_page(target_page_id, true);
        buffer_pool.flush_page(target_page_id);

        index.insert(rows[r][0].int_value, RID(target_page_id, slot_id));
    }

    return true;
}

}  // namespace

bool execute_alter_add_column(const std::string& table_name,
                              const std::string& column_name,
                              const std::string& type_spec,
                              bool has_default,
                              const std::string& default_value) {
    char tab[MAX_NAME];
    std::strncpy(tab, table_name.c_str(), MAX_NAME - 1);
    tab[MAX_NAME - 1] = '\0';

    if (search_table(tab) == 0) {
        std::cout << "ERROR: Table '" << table_name << "' does not exist.\n";
        return false;
    }

    table* meta = fetch_meta_data(table_name);
    if (meta == NULL) {
        std::cout << "ERROR: Could not load metadata for table '" << table_name << "'.\n";
        return false;
    }

    if (meta->count >= MAX_ATTR) {
        std::cout << "ERROR: Table already has maximum supported columns.\n";
        delete meta;
        return false;
    }

    for (int i = 0; i < meta->count; ++i) {
        if (column_name == meta->col[i].col_name) {
            std::cout << "ERROR: Column '" << column_name << "' already exists.\n";
            delete meta;
            return false;
        }
    }

    int new_type = 0;
    int new_size = 0;
    if (!parse_type_spec(type_spec, new_type, new_size)) {
        std::cout << "ERROR: Unsupported type '" << type_spec << "' in ALTER TABLE.\n";
        delete meta;
        return false;
    }

    std::vector<ColumnSchema> old_schema;
    build_schema_from_meta(meta, old_schema);

    std::vector<std::vector<TupleValue>> rows;
    if (!load_all_rows(table_name, old_schema, rows)) {
        std::cout << "ERROR: Could not load existing rows for ALTER TABLE.\n";
        delete meta;
        return false;
    }

    TupleValue default_tuple;
    if (new_type == INT) {
        int value = 0;
        if (has_default) {
            try {
                value = std::stoi(trim_copy(default_value));
            } catch (...) {
                std::cout << "ERROR: DEFAULT value for INT column must be an integer.\n";
                delete meta;
                return false;
            }
        }
        default_tuple = TupleValue::FromInt(value);
    } else {
        std::string value = has_default ? unquote(default_value) : "";
        if ((int)value.size() > new_size) {
            std::cout << "ERROR: DEFAULT value too long for VARCHAR(" << new_size << ").\n";
            delete meta;
            return false;
        }
        default_tuple = TupleValue::FromVarchar(value);
    }

    for (std::size_t r = 0; r < rows.size(); ++r) {
        rows[r].push_back(default_tuple);
    }

    std::strncpy(meta->col[meta->count].col_name, column_name.c_str(), MAX_NAME - 1);
    meta->col[meta->count].col_name[MAX_NAME - 1] = '\0';
    meta->col[meta->count].type = new_type;
    meta->col[meta->count].size = new_size;
    meta->count += 1;
    meta->size = compute_record_size(meta);

    if (store_meta_data(meta) != 0) {
        std::cout << "ERROR: Could not update table metadata.\n";
        delete meta;
        return false;
    }

    std::vector<ColumnSchema> new_schema;
    build_schema_from_meta(meta, new_schema);
    meta->rec_count = static_cast<int>(rows.size());
    store_meta_data(meta);
    delete meta;

    if (!rewrite_table_storage(table_name, new_schema, rows)) {
        std::cout << "ERROR: Failed to rewrite table data for ALTER TABLE.\n";
        return false;
    }

    std::cout << "Success: Added column '" << column_name
              << "' to table '" << table_name << "'.\n";
    return true;
}
