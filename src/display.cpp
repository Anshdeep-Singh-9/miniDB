#include "display.h"
#include "buffer_pool_manager.h"
#include "data_page.h"
#include "disk_manager.h"
#include "file_handler.h"
#include "tuple_serializer.h"
#include "where.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstring>

#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define BLUE    "\033[34m"
#define WHITE   "\033[97m"
#define GREEN   "\033[32m"

namespace {

struct SelectModifiers {
    bool has_order_by = false;
    std::string order_by_column;
    bool order_desc = false;
    int limit = -1;
};

struct HavingClause {
    bool present = false;
    std::string left_expr;
    std::string op;
    std::string right_value;
};

struct AggregateExpr {
    std::string func;
    std::string column;
    std::string label;
};

struct SelectItem {
    bool is_aggregate = false;
    std::string func;
    std::string column;
    std::string label;
};

std::string to_lower_copy(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return s;
}

bool tuple_less(const TupleValue& a, const TupleValue& b) {
    if (a.type == STORAGE_COLUMN_INT && b.type == STORAGE_COLUMN_INT) {
        return a.int_value < b.int_value;
    }
    if (a.type == STORAGE_COLUMN_VARCHAR && b.type == STORAGE_COLUMN_VARCHAR) {
        return a.string_value < b.string_value;
    }
    if (a.type == STORAGE_COLUMN_INT && b.type == STORAGE_COLUMN_VARCHAR) {
        return std::to_string(a.int_value) < b.string_value;
    }
    return a.string_value < std::to_string(b.int_value);
}

bool tuple_equals(const TupleValue& a, const TupleValue& b) {
    if (a.type == STORAGE_COLUMN_INT && b.type == STORAGE_COLUMN_INT) {
        return a.int_value == b.int_value;
    }
    if (a.type == STORAGE_COLUMN_VARCHAR && b.type == STORAGE_COLUMN_VARCHAR) {
        return a.string_value == b.string_value;
    }
    if (a.type == STORAGE_COLUMN_INT && b.type == STORAGE_COLUMN_VARCHAR) {
        return std::to_string(a.int_value) == b.string_value;
    }
    if (a.type == STORAGE_COLUMN_VARCHAR && b.type == STORAGE_COLUMN_INT) {
        return a.string_value == std::to_string(b.int_value);
    }
    return false;
}

bool parse_aggregate_expr(const std::string& token, AggregateExpr& expr) {
    std::string lower = to_lower_copy(token);
    const char* funcs[] = {"count", "sum", "avg", "min", "max"};
    for (std::size_t i = 0; i < 5; ++i) {
        const std::string prefix = std::string(funcs[i]) + "(";
        if (lower.size() > prefix.size() + 1 &&
            lower.compare(0, prefix.size(), prefix) == 0 &&
            lower[lower.size() - 1] == ')') {
            expr.func = funcs[i];
            expr.column = token.substr(prefix.size(), token.size() - prefix.size() - 1);
            expr.label = token;
            return true;
        }
    }
    return false;
}

bool is_aggregate_select(const std::vector<std::string>& target_cols,
                         std::vector<AggregateExpr>& aggregates) {
    aggregates.clear();
    if (target_cols.empty()) return false;

    for (std::size_t i = 0; i < target_cols.size(); ++i) {
        AggregateExpr expr;
        if (!parse_aggregate_expr(target_cols[i], expr)) {
            return false;
        }
        aggregates.push_back(expr);
    }
    return !aggregates.empty();
}

bool parse_select_items(const std::vector<std::string>& target_cols,
                        std::vector<SelectItem>& items,
                        bool& has_aggregate,
                        bool& has_plain_column) {
    items.clear();
    has_aggregate = false;
    has_plain_column = false;

    for (std::size_t i = 0; i < target_cols.size(); ++i) {
        AggregateExpr agg;
        SelectItem item;
        if (parse_aggregate_expr(target_cols[i], agg)) {
            item.is_aggregate = true;
            item.func = agg.func;
            item.column = agg.column;
            item.label = agg.label;
            has_aggregate = true;
        } else {
            item.is_aggregate = false;
            item.column = target_cols[i];
            item.label = target_cols[i];
            has_plain_column = true;
        }
        items.push_back(item);
    }
    return !items.empty();
}

int find_schema_column(const std::vector<ColumnSchema>& schema, const std::string& col_name) {
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (schema[i].name == col_name) return static_cast<int>(i);
    }
    return -1;
}

bool apply_order_by_and_limit(QueryResult& result,
                              const SelectModifiers& modifiers,
                              const std::vector<AggregateExpr>& aggregates) {
    if (!aggregates.empty()) {
        if (modifiers.limit >= 0 &&
            static_cast<int>(result.rows.size()) > modifiers.limit) {
            result.rows.resize(modifiers.limit);
        }
        return true;
    }

    if (modifiers.has_order_by) {
        int order_idx = find_schema_column(result.schema, modifiers.order_by_column);
        if (order_idx >= 0) {
            std::sort(result.rows.begin(), result.rows.end(),
                      [order_idx, &modifiers](const std::vector<TupleValue>& a,
                                              const std::vector<TupleValue>& b) {
                          bool less = tuple_less(a[order_idx], b[order_idx]);
                          bool greater = tuple_less(b[order_idx], a[order_idx]);
                          if (modifiers.order_desc) {
                              return greater;
                          }
                          return less;
                      });
        } else {
            int source_order_idx = find_schema_column(result.source_schema, modifiers.order_by_column);
            if (source_order_idx < 0 ||
                result.source_rows.size() != result.rows.size()) {
                return false;
            }

            std::vector<std::size_t> order(result.rows.size(), 0);
            for (std::size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }

            std::sort(order.begin(), order.end(),
                      [&result, source_order_idx, &modifiers](std::size_t a, std::size_t b) {
                          bool less = tuple_less(result.source_rows[a][source_order_idx],
                                                 result.source_rows[b][source_order_idx]);
                          bool greater = tuple_less(result.source_rows[b][source_order_idx],
                                                    result.source_rows[a][source_order_idx]);
                          if (modifiers.order_desc) {
                              return greater;
                          }
                          return less;
                      });

            std::vector<std::vector<TupleValue>> sorted_rows;
            std::vector<std::vector<TupleValue>> sorted_source_rows;
            sorted_rows.reserve(result.rows.size());
            sorted_source_rows.reserve(result.source_rows.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                sorted_rows.push_back(result.rows[order[i]]);
                sorted_source_rows.push_back(result.source_rows[order[i]]);
            }
            result.rows.swap(sorted_rows);
            result.source_rows.swap(sorted_source_rows);
        }
    }

    if (modifiers.limit >= 0 &&
        static_cast<int>(result.rows.size()) > modifiers.limit) {
        result.rows.resize(modifiers.limit);
    }

    return true;
}

bool parse_having_clause(const std::vector<std::string>& token_vector,
                         int cursor,
                         HavingClause& having,
                         int& next_cursor) {
    having = HavingClause();
    next_cursor = cursor;

    if ((int)token_vector.size() <= cursor || token_vector[cursor] != "having") {
        return true;
    }

    if ((int)token_vector.size() <= cursor + 3) {
        return false;
    }

    having.present = true;
    having.left_expr = token_vector[cursor + 1];
    having.op = token_vector[cursor + 2];
    having.right_value = token_vector[cursor + 3];

    if (having.op != "=" && having.op != "!=" && having.op != "<>" &&
        having.op != ">" && having.op != "<" &&
        having.op != ">=" && having.op != "<=") {
        return false;
    }

    next_cursor = cursor + 4;
    return true;
}

bool compare_tuple_value(const TupleValue& lhs,
                         const std::string& op,
                         const std::string& rhs_raw) {
    if (lhs.type == STORAGE_COLUMN_INT) {
        int rhs = 0;
        try {
            rhs = std::stoi(rhs_raw);
        } catch (...) {
            return false;
        }
        TupleValue rhs_value = TupleValue::FromInt(rhs);
        if (op == "=") return tuple_equals(lhs, rhs_value);
        if (op == "!=" || op == "<>") return !tuple_equals(lhs, rhs_value);
        if (op == ">") return lhs.int_value > rhs;
        if (op == "<") return lhs.int_value < rhs;
        if (op == ">=") return lhs.int_value >= rhs;
        if (op == "<=") return lhs.int_value <= rhs;
        return false;
    }

    TupleValue rhs_value = TupleValue::FromVarchar(rhs_raw);
    if (op == "=") return tuple_equals(lhs, rhs_value);
    if (op == "!=" || op == "<>") return !tuple_equals(lhs, rhs_value);
    if (op == ">") return lhs.string_value > rhs_raw;
    if (op == "<") return lhs.string_value < rhs_raw;
    if (op == ">=") return lhs.string_value >= rhs_raw;
    if (op == "<=") return lhs.string_value <= rhs_raw;
    return false;
}

bool apply_having(QueryResult& result, const HavingClause& having) {
    if (!having.present) return true;

    int having_idx = find_schema_column(result.schema, having.left_expr);
    if (having_idx < 0) {
        return false;
    }

    std::vector<std::vector<TupleValue>> filtered_rows;
    filtered_rows.reserve(result.rows.size());
    for (std::size_t i = 0; i < result.rows.size(); ++i) {
        if (having_idx >= (int)result.rows[i].size()) {
            return false;
        }
        if (compare_tuple_value(result.rows[i][having_idx], having.op, having.right_value)) {
            filtered_rows.push_back(result.rows[i]);
        }
    }

    result.rows.swap(filtered_rows);
    result.source_schema = result.schema;
    result.source_rows = result.rows;
    return true;
}

std::string group_key_for_value(const TupleValue& value) {
    if (value.type == STORAGE_COLUMN_INT) {
        return "I:" + std::to_string(value.int_value);
    }
    return "S:" + value.string_value;
}

bool is_select_clause_keyword(const std::string& token) {
    return token == "having" || token == "order" || token == "limit";
}

bool apply_group_by(QueryResult& result,
                    const std::vector<SelectItem>& items,
                    const std::vector<std::string>& group_by_columns) {
    if (group_by_columns.empty()) return true;
    if (result.source_schema.empty()) return false;

    std::vector<int> group_indices;
    group_indices.reserve(group_by_columns.size());
    for (std::size_t i = 0; i < group_by_columns.size(); ++i) {
        int group_idx = find_schema_column(result.source_schema, group_by_columns[i]);
        if (group_idx < 0) return false;
        group_indices.push_back(group_idx);
    }

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].is_aggregate) {
            bool found = false;
            for (std::size_t g = 0; g < group_by_columns.size(); ++g) {
                if (items[i].column == group_by_columns[g]) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }

    struct GroupBucket {
        std::vector<TupleValue> group_values;
        std::vector<std::vector<TupleValue>> rows;
    };

    std::vector<GroupBucket> buckets;
    std::unordered_map<std::string, std::size_t> bucket_index;
    for (std::size_t r = 0; r < result.source_rows.size(); ++r) {
        std::string key;
        std::vector<TupleValue> key_values;
        for (std::size_t g = 0; g < group_indices.size(); ++g) {
            const TupleValue& key_value = result.source_rows[r][group_indices[g]];
            if (!key.empty()) key += "|";
            key += group_key_for_value(key_value);
            key_values.push_back(key_value);
        }
        std::unordered_map<std::string, std::size_t>::iterator found = bucket_index.find(key);
        if (found == bucket_index.end()) {
            GroupBucket bucket;
            bucket.group_values = key_values;
            bucket.rows.push_back(result.source_rows[r]);
            bucket_index[key] = buckets.size();
            buckets.push_back(bucket);
        } else {
            buckets[found->second].rows.push_back(result.source_rows[r]);
        }
    }

    std::vector<ColumnSchema> grouped_schema;
    std::vector<std::vector<TupleValue>> grouped_rows;

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].is_aggregate) {
            int plain_idx = find_schema_column(result.source_schema, items[i].column);
            if (plain_idx < 0) return false;
            grouped_schema.push_back(result.source_schema[plain_idx]);
            grouped_schema.back().name = items[i].label;
        } else {
            const std::string lower_func = to_lower_copy(items[i].func);
            if (lower_func == "count") {
                grouped_schema.push_back(ColumnSchema(items[i].label, STORAGE_COLUMN_INT, sizeof(int32_t)));
            } else {
                int agg_idx = find_schema_column(result.source_schema, items[i].column);
                if (agg_idx < 0) return false;
                grouped_schema.push_back(result.source_schema[agg_idx]);
                grouped_schema.back().name = items[i].label;
            }
        }
    }

    for (std::size_t b = 0; b < buckets.size(); ++b) {
        std::vector<TupleValue> out_row;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!items[i].is_aggregate) {
                int plain_group_pos = -1;
                for (std::size_t g = 0; g < group_by_columns.size(); ++g) {
                    if (items[i].column == group_by_columns[g]) {
                        plain_group_pos = static_cast<int>(g);
                        break;
                    }
                }
                if (plain_group_pos < 0) return false;
                out_row.push_back(buckets[b].group_values[plain_group_pos]);
                continue;
            }

            const std::string lower_func = to_lower_copy(items[i].func);
            if (lower_func == "count") {
                out_row.push_back(TupleValue::FromInt(static_cast<int32_t>(buckets[b].rows.size())));
                continue;
            }

            int agg_idx = find_schema_column(result.source_schema, items[i].column);
            if (agg_idx < 0) return false;
            const ColumnSchema& src_col = result.source_schema[agg_idx];

            if (lower_func == "sum" || lower_func == "avg") {
                if (src_col.type != STORAGE_COLUMN_INT) return false;
                long long total = 0;
                for (std::size_t r = 0; r < buckets[b].rows.size(); ++r) {
                    total += buckets[b].rows[r][agg_idx].int_value;
                }
                int32_t value = (lower_func == "avg")
                                    ? static_cast<int32_t>(total / static_cast<long long>(buckets[b].rows.size()))
                                    : static_cast<int32_t>(total);
                out_row.push_back(TupleValue::FromInt(value));
            } else if (src_col.type == STORAGE_COLUMN_INT) {
                int32_t best = buckets[b].rows[0][agg_idx].int_value;
                for (std::size_t r = 1; r < buckets[b].rows.size(); ++r) {
                    int32_t cur = buckets[b].rows[r][agg_idx].int_value;
                    if ((lower_func == "min" && cur < best) ||
                        (lower_func == "max" && cur > best)) {
                        best = cur;
                    }
                }
                out_row.push_back(TupleValue::FromInt(best));
            } else {
                std::string best = buckets[b].rows[0][agg_idx].string_value;
                for (std::size_t r = 1; r < buckets[b].rows.size(); ++r) {
                    const std::string& cur = buckets[b].rows[r][agg_idx].string_value;
                    if ((lower_func == "min" && cur < best) ||
                        (lower_func == "max" && cur > best)) {
                        best = cur;
                    }
                }
                out_row.push_back(TupleValue::FromVarchar(best));
            }
        }
        grouped_rows.push_back(out_row);
    }

    result.schema = grouped_schema;
    result.rows = grouped_rows;
    result.source_schema = grouped_schema;
    result.source_rows = grouped_rows;
    return true;
}

bool apply_aggregates(QueryResult& result,
                      const std::vector<AggregateExpr>& aggregates) {
    if (aggregates.empty()) return true;

    std::vector<ColumnSchema> agg_schema;
    std::vector<TupleValue> agg_row;

    for (std::size_t i = 0; i < aggregates.size(); ++i) {
        const AggregateExpr& expr = aggregates[i];
        const std::string lower_func = to_lower_copy(expr.func);

        if (lower_func == "count") {
            if (expr.column != "*") {
                return false;
            }
            agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_INT, sizeof(int32_t)));
            agg_row.push_back(TupleValue::FromInt(static_cast<int32_t>(result.rows.size())));
            continue;
        }

        int col_idx = find_schema_column(result.schema, expr.column);
        if (col_idx < 0) {
            return false;
        }

        const ColumnSchema& src_col = result.schema[col_idx];
        if (src_col.type != STORAGE_COLUMN_INT && lower_func != "min" && lower_func != "max") {
            return false;
        }

        if (result.rows.empty()) {
            if (lower_func == "sum" || lower_func == "avg" || lower_func == "count") {
                agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_INT, sizeof(int32_t)));
                agg_row.push_back(TupleValue::FromInt(0));
            } else if (src_col.type == STORAGE_COLUMN_INT) {
                agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_INT, sizeof(int32_t)));
                agg_row.push_back(TupleValue::FromInt(0));
            } else {
                agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_VARCHAR, src_col.max_length));
                agg_row.push_back(TupleValue::FromVarchar(""));
            }
            continue;
        }

        if (lower_func == "sum" || lower_func == "avg") {
            long long total = 0;
            for (std::size_t r = 0; r < result.rows.size(); ++r) {
                total += result.rows[r][col_idx].int_value;
            }
            int32_t value = (lower_func == "avg")
                                ? static_cast<int32_t>(total / static_cast<long long>(result.rows.size()))
                                : static_cast<int32_t>(total);
            agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_INT, sizeof(int32_t)));
            agg_row.push_back(TupleValue::FromInt(value));
            continue;
        }

        if (src_col.type == STORAGE_COLUMN_INT) {
            int32_t best = result.rows[0][col_idx].int_value;
            for (std::size_t r = 1; r < result.rows.size(); ++r) {
                int32_t cur = result.rows[r][col_idx].int_value;
                if ((lower_func == "min" && cur < best) ||
                    (lower_func == "max" && cur > best)) {
                    best = cur;
                }
            }
            agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_INT, sizeof(int32_t)));
            agg_row.push_back(TupleValue::FromInt(best));
        } else {
            std::string best = result.rows[0][col_idx].string_value;
            for (std::size_t r = 1; r < result.rows.size(); ++r) {
                const std::string& cur = result.rows[r][col_idx].string_value;
                if ((lower_func == "min" && cur < best) ||
                    (lower_func == "max" && cur > best)) {
                    best = cur;
                }
            }
            agg_schema.push_back(ColumnSchema(expr.label, STORAGE_COLUMN_VARCHAR, src_col.max_length));
            agg_row.push_back(TupleValue::FromVarchar(best));
        }
    }

    result.schema = agg_schema;
    result.rows.clear();
    result.rows.push_back(agg_row);
    return true;
}

}  // namespace
void print_table(const std::vector<ColumnSchema>& schema,
                 const std::vector<std::vector<TupleValue>>& table_data) {
    // What: render rows and columns as a readable terminal table.
    // Why: execution returns TupleValue objects, but users need a clean CLI output.
    // Example: SELECT name, dept FROM students; prints selected columns with aligned widths.
    if (schema.empty()) {
        std::cout << "\n(No columns to display)\n";
        return;
    }

    std::vector<int> widths(schema.size(), 0);

    for (size_t i = 0; i < schema.size(); i++) {
        widths[i] = std::max(12, (int)schema[i].name.length());
    }

    for (const auto& row : table_data) {
        for (size_t i = 0; i < row.size() && i < widths.size(); i++) {
            int len = 0;

            if (row[i].type == STORAGE_COLUMN_INT) {
                len = std::to_string(row[i].int_value).length();
            } else {
                len = row[i].string_value.length();
            }

            widths[i] = std::max(widths[i], len);
        }
    }

    for (size_t i = 0; i < widths.size(); i++) {
        widths[i] += 4;
    }

    auto print_border = [&]() {
        std::cout << BLUE << "+";
        for (int w : widths) {
            std::cout << std::string(w, '-') << "+";
        }
        std::cout << RESET << "\n";
    };

    auto print_empty_message = [&]() {
        std::cout << BLUE << "|" << RESET;

        int total_width = 0;
        for (int w : widths) total_width += w;
        total_width += (int)widths.size() - 1;

        std::string msg = " Table is empty ";
        int left_padding = std::max(0, (total_width - (int)msg.length()) / 2);
        int right_padding = std::max(0, total_width - left_padding - (int)msg.length());

        std::cout << std::string(left_padding, ' ')
                  << WHITE << msg << RESET
                  << std::string(right_padding, ' ');

        std::cout << BLUE << "|" << RESET << "\n";
    };

    std::cout << "\n";

    print_border();

    std::cout << BLUE << "|" << RESET;
    for (size_t i = 0; i < schema.size(); i++) {
        std::cout << BOLD << WHITE
                  << " " << std::left << std::setw(widths[i] - 1) << schema[i].name
                  << RESET << BLUE << "|" << RESET;
    }
    std::cout << "\n";

    print_border();

    if (table_data.empty()) {
        print_empty_message();
    } else {
        for (const auto& row : table_data) {
            std::cout << BLUE << "|" << RESET;

            for (size_t i = 0; i < schema.size(); i++) {
                std::string value;

                if (i < row.size()) {
                    if (row[i].type == STORAGE_COLUMN_INT) {
                        value = std::to_string(row[i].int_value);
                    } else {
                        value = row[i].string_value;
                    }
                } else {
                    value = "";
                }

                std::cout << WHITE
                          << " " << std::left << std::setw(widths[i] - 1) << value
                          << RESET << BLUE << "|" << RESET;
            }

            std::cout << "\n";
        }
    }

    print_border();

    std::cout << BOLD << WHITE
              << "Total rows: " << table_data.size()
              << RESET << "\n\n";
}

int search_table(char tab_name[]) {
    std::ifstream in(table_list_path());
    std::string name;
    while (in >> name) {
        if (name == tab_name) {
            return 1;
        }
    }
    return 0;
}

static bool build_schema_from_meta(table* meta, std::vector<ColumnSchema>& schema) {
    // What: convert old table metadata into the storage-layer schema format.
    // Why: TupleSerializer uses ColumnSchema, while catalog files store columns in table structs.
    // Example: metadata column INT becomes STORAGE_COLUMN_INT for serialization/deserialization.
    if (meta == NULL) {
        return false;
    }

    schema.clear();

    for (int i = 0; i < meta->count; i++) {
        ColumnSchema col_schema(
            meta->col[i].col_name,
            meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
            meta->col[i].size
        );

        schema.push_back(col_schema);
    }

    return true;
}

static bool load_all_rows_using_buffer_pool(const std::string& tab_name,
                                            const std::vector<ColumnSchema>& full_schema,
                                            std::vector<std::vector<TupleValue>>& rows) {
    // What: read every page through BufferPoolManager and deserialize every live tuple.
    // Why: SELECT without WHERE and JOIN need complete table rows from page storage.
    // Example: SELECT * FROM students; fetches page frames, reads slots, and builds row objects.
    rows.clear();

    std::string data_path = table_data_path(tab_name).string();

    DiskManager data_disk(data_path);
    if (!data_disk.open_or_create()) {
        std::cout << "Error: Could not open data file.\n";
        return false;
    }

    BufferPoolManager buffer_pool(4, &data_disk);

    for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
        char* frame_data = buffer_pool.fetch_page(i);

        if (frame_data == NULL) {
            continue;
        }

        DataPage page;
        page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            std::vector<char> tuple_data;

            if (!page.read_tuple(slot_id, tuple_data)) {
                continue;
            }

            std::vector<TupleValue> values;

            if (TupleSerializer::deserialize(full_schema, tuple_data, values)) {
                rows.push_back(values);
            }
        }

        buffer_pool.unpin_page(i, false);
    }

    return true;
}

static int find_column_index(const table* meta, const std::string& col_name) {
    if (meta == NULL) return -1;
    for (int i = 0; i < meta->count; ++i) {
        if (col_name == meta->col[i].col_name) return i;
    }
    return -1;
}

static bool parse_qualified_column(const std::string& token,
                                   std::string& table_name,
                                   std::string& column_name) {
    std::size_t dot = token.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= token.size()) {
        return false;
    }
    table_name = token.substr(0, dot);
    column_name = token.substr(dot + 1);
    return !table_name.empty() && !column_name.empty();
}

static bool tuple_values_equal(const TupleValue& a, const TupleValue& b) {
    if (a.type == STORAGE_COLUMN_INT && b.type == STORAGE_COLUMN_INT) {
        return a.int_value == b.int_value;
    }
    if (a.type == STORAGE_COLUMN_VARCHAR && b.type == STORAGE_COLUMN_VARCHAR) {
        return a.string_value == b.string_value;
    }
    if (a.type == STORAGE_COLUMN_INT && b.type == STORAGE_COLUMN_VARCHAR) {
        return std::to_string(a.int_value) == b.string_value;
    }
    if (a.type == STORAGE_COLUMN_VARCHAR && b.type == STORAGE_COLUMN_INT) {
        return a.string_value == std::to_string(b.int_value);
    }
    return false;
}

static bool execute_select_join_nested(const std::vector<std::string>& target_cols,
                                       const std::string& left_table,
                                       const std::string& right_table,
                                       const std::string& left_on,
                                       const std::string& right_on,
                                       bool left_join,
                                       bool where_present,
                                       const std::string& where_column,
                                       const std::string& where_value,
                                       QueryResult* res) {
    // What: execute INNER/LEFT JOIN using nested loops over both table row sets.
    // Why: MiniDB has no join optimizer yet, so nested loop is the simplest correct join algorithm.
    // Example: SELECT s.name, d.hod FROM students JOIN departments ON students.dept = departments.code;
    char left_tab[MAX_NAME];
    char right_tab[MAX_NAME];
    strncpy(left_tab, left_table.c_str(), MAX_NAME - 1);
    left_tab[MAX_NAME - 1] = '\0';
    strncpy(right_tab, right_table.c_str(), MAX_NAME - 1);
    right_tab[MAX_NAME - 1] = '\0';

    if (search_table(left_tab) == 0 || search_table(right_tab) == 0) {
        if (res) {
            res->success = false;
            res->message = "Error: One or both JOIN tables do not exist.";
        }
        std::cout << "Error: One or both JOIN tables do not exist.\n";
        return false;
    }

    table* left_meta = fetch_meta_data(left_table);
    table* right_meta = fetch_meta_data(right_table);
    if (left_meta == NULL || right_meta == NULL) {
        if (res) {
            res->success = false;
            res->message = "Error: Could not load JOIN metadata.";
        }
        std::cout << "Error: Could not load JOIN metadata.\n";
        delete left_meta;
        delete right_meta;
        return false;
    }

    std::vector<ColumnSchema> left_schema;
    std::vector<ColumnSchema> right_schema;
    build_schema_from_meta(left_meta, left_schema);
    build_schema_from_meta(right_meta, right_schema);

    std::string left_on_table, left_on_col, right_on_table, right_on_col;
    if (!parse_qualified_column(left_on, left_on_table, left_on_col) ||
        !parse_qualified_column(right_on, right_on_table, right_on_col)) {
        std::cout << "Syntax Error: JOIN ON needs qualified columns like a.id = b.id\n";
        delete left_meta;
        delete right_meta;
        return false;
    }

    if (left_on_table != left_table || right_on_table != right_table) {
        std::cout << "Syntax Error: JOIN ON columns must match table names in FROM/JOIN.\n";
        delete left_meta;
        delete right_meta;
        return false;
    }

    int left_on_idx = find_column_index(left_meta, left_on_col);
    int right_on_idx = find_column_index(right_meta, right_on_col);
    if (left_on_idx < 0 || right_on_idx < 0) {
        std::cout << "Error: JOIN ON column not found in one of the tables.\n";
        delete left_meta;
        delete right_meta;
        return false;
    }

    std::vector<std::vector<TupleValue>> left_rows;
    std::vector<std::vector<TupleValue>> right_rows;
    if (!load_all_rows_using_buffer_pool(left_table, left_schema, left_rows) ||
        !load_all_rows_using_buffer_pool(right_table, right_schema, right_rows)) {
        delete left_meta;
        delete right_meta;
        return false;
    }

    std::vector<ColumnSchema> output_schema;
    std::vector<std::pair<int, int> > projections; // 0=left,1=right ; col idx
    bool select_all = (target_cols.size() == 1 && target_cols[0] == "*");

    int where_side = -1;
    int where_idx = -1;
    if (where_present) {
        std::string wt, wc;
        if (parse_qualified_column(where_column, wt, wc)) {
            if (wt == left_table) {
                where_side = 0;
                where_idx = find_column_index(left_meta, wc);
            } else if (wt == right_table) {
                where_side = 1;
                where_idx = find_column_index(right_meta, wc);
            }
        } else {
            int li = find_column_index(left_meta, where_column);
            int ri = find_column_index(right_meta, where_column);
            if (li >= 0 && ri >= 0) {
                std::cout << "Error: Ambiguous WHERE column '" << where_column
                          << "'. Use qualified name.\n";
                delete left_meta;
                delete right_meta;
                return false;
            }
            if (li >= 0) {
                where_side = 0;
                where_idx = li;
            } else if (ri >= 0) {
                where_side = 1;
                where_idx = ri;
            }
        }

        if (where_idx < 0) {
            std::cout << "Error: WHERE column not found in JOIN tables.\n";
            delete left_meta;
            delete right_meta;
            return false;
        }
    }

    if (select_all) {
        for (int i = 0; i < left_meta->count; ++i) {
            ColumnSchema col = left_schema[i];
            col.name = left_table + "." + col.name;
            output_schema.push_back(col);
            projections.push_back(std::make_pair(0, i));
        }
        for (int i = 0; i < right_meta->count; ++i) {
            ColumnSchema col = right_schema[i];
            col.name = right_table + "." + col.name;
            output_schema.push_back(col);
            projections.push_back(std::make_pair(1, i));
        }
    } else {
        for (std::size_t i = 0; i < target_cols.size(); ++i) {
            std::string tname, cname;
            if (!parse_qualified_column(target_cols[i], tname, cname)) {
                std::cout << "Error: In JOIN select list, use qualified columns like table.column\n";
                delete left_meta;
                delete right_meta;
                return false;
            }

            if (tname == left_table) {
                int idx = find_column_index(left_meta, cname);
                if (idx < 0) {
                    std::cout << "Error: Column " << target_cols[i] << " not found.\n";
                    delete left_meta;
                    delete right_meta;
                    return false;
                }
                ColumnSchema col = left_schema[idx];
                col.name = target_cols[i];
                output_schema.push_back(col);
                projections.push_back(std::make_pair(0, idx));
            } else if (tname == right_table) {
                int idx = find_column_index(right_meta, cname);
                if (idx < 0) {
                    std::cout << "Error: Column " << target_cols[i] << " not found.\n";
                    delete left_meta;
                    delete right_meta;
                    return false;
                }
                ColumnSchema col = right_schema[idx];
                col.name = target_cols[i];
                output_schema.push_back(col);
                projections.push_back(std::make_pair(1, idx));
            } else {
                std::cout << "Error: Unknown table prefix in " << target_cols[i] << "\n";
                delete left_meta;
                delete right_meta;
                return false;
            }
        }
    }

    std::vector<std::vector<TupleValue>> output_rows;
    for (std::size_t i = 0; i < left_rows.size(); ++i) {
        bool matched_any = false;
        for (std::size_t j = 0; j < right_rows.size(); ++j) {
            if (!tuple_values_equal(left_rows[i][left_on_idx], right_rows[j][right_on_idx])) {
                continue;
            }

            if (where_present) {
                TupleValue expected = TupleValue::FromVarchar(where_value);
                if (where_side == 0 && left_rows[i][where_idx].type == STORAGE_COLUMN_INT) {
                    try {
                        expected = TupleValue::FromInt(std::stoi(where_value));
                    } catch (...) {
                        continue;
                    }
                }
                if (where_side == 1 && right_rows[j][where_idx].type == STORAGE_COLUMN_INT) {
                    try {
                        expected = TupleValue::FromInt(std::stoi(where_value));
                    } catch (...) {
                        continue;
                    }
                }

                bool pass = (where_side == 0)
                                ? tuple_values_equal(left_rows[i][where_idx], expected)
                                : tuple_values_equal(right_rows[j][where_idx], expected);
                if (!pass) continue;
            }

            std::vector<TupleValue> out;
            for (std::size_t p = 0; p < projections.size(); ++p) {
                if (projections[p].first == 0) out.push_back(left_rows[i][projections[p].second]);
                else out.push_back(right_rows[j][projections[p].second]);
            }
            output_rows.push_back(out);
            matched_any = true;
        }

        if (left_join && !matched_any) {
            if (where_present && where_side == 0) {
                TupleValue expected = TupleValue::FromVarchar(where_value);
                if (left_rows[i][where_idx].type == STORAGE_COLUMN_INT) {
                    try {
                        expected = TupleValue::FromInt(std::stoi(where_value));
                    } catch (...) {
                        continue;
                    }
                }
                if (!tuple_values_equal(left_rows[i][where_idx], expected)) continue;
            } else if (where_present && where_side == 1) {
                continue;
            }

            std::vector<TupleValue> out;
            for (std::size_t p = 0; p < projections.size(); ++p) {
                if (projections[p].first == 0) {
                    out.push_back(left_rows[i][projections[p].second]);
                } else {
                    const ColumnSchema& col = right_schema[projections[p].second];
                    if (col.type == STORAGE_COLUMN_INT) out.push_back(TupleValue::FromInt(0));
                    else out.push_back(TupleValue::FromVarchar(""));
                }
            }
            output_rows.push_back(out);
        }
    }

    if (res) {
        res->is_select = true;
        res->schema = output_schema;
        res->rows = output_rows;
        res->strategy = "Nested Loop Join";
    }

    if (res == nullptr) {
        print_table(output_schema, output_rows);
    }

    delete left_meta;
    delete right_meta;
    return true;
}

void display() {
    // What: interactive menu path for displaying a full table.
    // Why: the older menu still needs to reuse the newer page + serializer storage path.
    // Example: menu option "display table contents" asks table name and prints all rows.
    char tab[MAX_NAME];

    std::cout << "Enter table name: ";
    std::cin >> tab;

    if (search_table(tab) == 0) {
        std::cout << "\nTable \"" << tab << "\" does not exist.\n";
        return;
    }

    table* meta = fetch_meta_data(tab);
    if (meta == NULL) {
        std::cout << "Error: Could not load table metadata.\n";
        return;
    }

    std::vector<ColumnSchema> full_schema;
    build_schema_from_meta(meta, full_schema);

    std::vector<std::vector<TupleValue>> rows;
    load_all_rows_using_buffer_pool(tab, full_schema, rows);

    std::cout << "\nDisplaying contents for table: " << tab << "\n";
    print_table(full_schema, rows);

    delete meta;
}

void show_tables() {
    // What: list table names from the central table catalog file.
    // Why: MiniDB stores table existence separately from each table's data pages.
    // Example: SHOW TABLES; reads table/table_list and prints all names.
    char name[MAX_NAME];

    std::vector<ColumnSchema> schema;
    schema.push_back(ColumnSchema("Table Name", STORAGE_COLUMN_VARCHAR, MAX_NAME));

    std::vector<std::vector<TupleValue>> rows;

    FilePtr fp = fopen(table_list_path().string().c_str(), "r");

    if (fp) {
        while (fscanf(fp, "%s", name) != EOF) {
            std::vector<TupleValue> row;
            row.push_back(TupleValue::FromVarchar(name));
            rows.push_back(row);
        }

        fclose(fp);
    }

    std::cout << "\n" << GREEN << BOLD << "LIST OF TABLES" << RESET << "\n";
    print_table(schema, rows);
}

void display_meta_data() {
    // What: show schema metadata for one table.
    // Why: users and developers can verify column names, types, and sizes before running queries.
    // Example: metadata for students shows id INT, name VARCHAR, dept VARCHAR.
    std::string name;

    std::cout << "Enter the name of table: ";
    std::cin >> name;

    if (name.empty()) {
        std::cout << "ERROR! No name entered.\n";
        return;
    }

    table* t_ptr = fetch_meta_data(name);

    if (t_ptr == NULL) {
        std::cout << "ERROR! Table not found or failed to read metadata.\n";
        return;
    }

    std::cout << "\n" << BOLD << BLUE << "TABLE METADATA" << RESET << "\n";
    std::cout << BLUE << "----------------------------------------" << RESET << "\n";
    std::cout << BOLD << "Table Name   : " << RESET << t_ptr->name << "\n";
    std::cout << BOLD << "Max Row Size : " << RESET << t_ptr->size << "\n";
    std::cout << BOLD << "Columns      : " << RESET << t_ptr->count << "\n";
    std::cout << BLUE << "----------------------------------------" << RESET << "\n\n";

    std::vector<ColumnSchema> schema;
    schema.push_back(ColumnSchema("Column Name", STORAGE_COLUMN_VARCHAR, MAX_NAME));
    schema.push_back(ColumnSchema("Type", STORAGE_COLUMN_VARCHAR, 20));
    schema.push_back(ColumnSchema("Size", STORAGE_COLUMN_INT, sizeof(int)));

    std::vector<std::vector<TupleValue>> rows;

    for (int i = 0; i < t_ptr->count; i++) {
        std::vector<TupleValue> row;

        row.push_back(TupleValue::FromVarchar(t_ptr->col[i].col_name));
        row.push_back(TupleValue::FromVarchar(t_ptr->col[i].type == INT ? "INT" : "VARCHAR"));
        row.push_back(TupleValue::FromInt(t_ptr->col[i].size));

        rows.push_back(row);
    }

    print_table(schema, rows);

    delete t_ptr;
}

void execute_select(const std::string& tab_name,
                    const std::vector<std::string>& target_cols, QueryResult* res) {
    // What: execute SELECT without WHERE by loading all rows and projecting requested columns.
    // Why: this is the base read path for full table reads before filtering or indexing is needed.
    // Example: SELECT name, dept FROM students;
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

    std::vector<SelectItem> select_items;
    bool has_aggregate = false;
    bool has_plain_column = false;
    parse_select_items(target_cols, select_items, has_aggregate, has_plain_column);
    bool aggregate_query = has_aggregate;
    bool select_all = (target_cols.size() == 1 && target_cols[0] == "*");
    std::vector<int> selected_indices;

    if (aggregate_query) {
        for (std::size_t i = 0; i < select_items.size(); ++i) {
            if (!select_items[i].is_aggregate || select_items[i].column == "*") {
                continue;
            }
            bool found = false;
            for (int c = 0; c < meta->count; ++c) {
                if (select_items[i].column == meta->col[c].col_name) {
                    if (std::find(selected_indices.begin(), selected_indices.end(), c) ==
                        selected_indices.end()) {
                        selected_indices.push_back(c);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (res) {
                    res->success = false;
                    res->message = "Error: Column '" + select_items[i].column + "' does not exist in table.";
                }
                std::cout << "Error: Column '" << select_items[i].column
                          << "' does not exist in table.\n";
                delete meta;
                return;
            }
        }
        for (std::size_t i = 0; i < select_items.size(); ++i) {
            if (select_items[i].is_aggregate || select_items[i].column == "*") continue;
            bool found = false;
            for (int c = 0; c < meta->count; ++c) {
                if (select_items[i].column == meta->col[c].col_name) {
                    if (std::find(selected_indices.begin(), selected_indices.end(), c) ==
                        selected_indices.end()) {
                        selected_indices.push_back(c);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (res) {
                    res->success = false;
                    res->message = "Error: Column '" + select_items[i].column + "' does not exist in table.";
                }
                std::cout << "Error: Column '" << select_items[i].column
                          << "' does not exist in table.\n";
                delete meta;
                return;
            }
        }
    } else if (select_all) {
        for (int i = 0; i < meta->count; i++) {
            selected_indices.push_back(i);
        }
    } else {
        for (const auto& col_name : target_cols) {
            bool found = false;

            for (int i = 0; i < meta->count; i++) {
                if (col_name == meta->col[i].col_name) {
                    selected_indices.push_back(i);
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (res) {
                    res->success = false;
                    res->message = "Error: Column '" + col_name + "' does not exist in table.";
                }
                std::cout << "Error: Column '" << col_name << "' does not exist in table.\n";
                delete meta;
                return;
            }
        }
    }

    std::vector<ColumnSchema> full_schema;
    build_schema_from_meta(meta, full_schema);

    std::vector<ColumnSchema> output_schema;

    for (int idx : selected_indices) {
        output_schema.push_back(full_schema[idx]);
    }

    std::vector<std::vector<TupleValue>> all_rows;

    if (!load_all_rows_using_buffer_pool(tab_name, full_schema, all_rows)) {
        if (res) {
            res->success = false;
            res->message = "Error: Failed to load rows.";
        }
        delete meta;
        return;
    }

    std::vector<std::vector<TupleValue>> output_rows;

    for (const auto& row : all_rows) {
        std::vector<TupleValue> selected_row;

        for (int idx : selected_indices) {
            selected_row.push_back(row[idx]);
        }

        output_rows.push_back(selected_row);
    }

    if (res) {
        res->is_select = true;
        res->schema = output_schema;
        res->rows = output_rows;
        res->source_schema = full_schema;
        res->source_rows = all_rows;
        res->strategy = "Linear Scan (No WHERE clause)";
    }

    if (res == nullptr) {
        print_table(output_schema, output_rows);
    }

    delete meta;
}
void process_select(std::vector<std::string>& token_vector, QueryResult* res) {
    // What: turn SELECT tokens into either normal SELECT, WHERE SELECT, or JOIN execution.
    // Why: parser tokenization is generic; this function understands SELECT clause order.
    // Example: SELECT * FROM students WHERE id = 1; routes to execute_select_where().
    if (token_vector.size() < 4 || token_vector[0] != "select") {
        if (res) {
            res->success = false;
            res->message = "Syntax Error: Invalid SELECT statement.";
        }
        std::cout << "Syntax Error: Invalid SELECT statement.\n";
        return;
    }

    std::vector<std::string> target_cols;
    std::string table_name = "";
    int from_index = -1;

    for (size_t i = 1; i < token_vector.size(); i++) {
        if (token_vector[i] == "from") {
            from_index = static_cast<int>(i);
            break;
        } else {
            target_cols.push_back(token_vector[i]);
        }
    }

    if (from_index == -1 || (int)token_vector.size() <= from_index + 1) {
        if (res) {
            res->success = false;
            res->message = "Syntax Error: Missing FROM clause or table name.";
        }
        std::cout << "Syntax Error: Missing FROM clause or table name.\n";
        return;
    }

    table_name = token_vector[from_index + 1];

    int join_kw_index = from_index + 2;
    bool left_join = false;
    if ((int)token_vector.size() > join_kw_index &&
        (token_vector[join_kw_index] == "left" || token_vector[join_kw_index] == "inner")) {
        left_join = (token_vector[join_kw_index] == "left");
        join_kw_index++;
    }

    std::vector<AggregateExpr> aggregates;
    bool aggregate_query = is_aggregate_select(target_cols, aggregates);
    std::vector<SelectItem> select_items;
    bool has_aggregate = false;
    bool has_plain_column = false;
    parse_select_items(target_cols, select_items, has_aggregate, has_plain_column);
    QueryResult local_result;
    QueryResult* target_result = (res != nullptr) ? res : &local_result;

    if ((int)token_vector.size() > join_kw_index && token_vector[join_kw_index] == "join") {
        if (aggregate_query) {
            target_result->success = false;
            target_result->message = "Error: Aggregate functions over JOIN are not supported yet.";
            std::cout << "Error: Aggregate functions over JOIN are not supported yet.\n";
            return;
        }
        if ((int)token_vector.size() < join_kw_index + 6) {
            std::cout << "Syntax Error: Incomplete JOIN.\n";
            std::cout << "Use: SELECT ... FROM t1 JOIN t2 ON t1.col = t2.col;\n";
            return;
        }

        std::string right_table = token_vector[join_kw_index + 1];
        if (token_vector[join_kw_index + 2] != "on") {
            std::cout << "Syntax Error: Missing ON in JOIN clause.\n";
            return;
        }

        std::string left_on = token_vector[join_kw_index + 3];
        std::string op = token_vector[join_kw_index + 4];
        std::string right_on = token_vector[join_kw_index + 5];

        if (op != "=") {
            std::cout << "Syntax Error: Only '=' is supported in JOIN ON.\n";
            return;
        }

        bool where_present = false;
        std::string where_col, where_val;
        SelectModifiers modifiers;
        std::vector<std::string> group_by_columns;
        HavingClause having;
        int cursor = join_kw_index + 6;

        if ((int)token_vector.size() > cursor && token_vector[cursor] == "where") {
            if ((int)token_vector.size() < cursor + 4) {
                std::cout << "Syntax Error: Incomplete WHERE after JOIN.\n";
                return;
            }
            if (token_vector[cursor + 2] != "=") {
                std::cout << "Syntax Error: Only '=' is supported in WHERE.\n";
                return;
            }

            where_present = true;
            where_col = token_vector[cursor + 1];
            where_val = token_vector[cursor + 3];
            cursor += 4;
        }

        while ((int)token_vector.size() > cursor) {
            if (token_vector[cursor] == "group") {
                if ((int)token_vector.size() <= cursor + 2 || token_vector[cursor + 1] != "by") {
                    std::cout << "Syntax Error: GROUP BY requires a column.\n";
                    return;
                }
                cursor += 2;
                while ((int)token_vector.size() > cursor && !is_select_clause_keyword(token_vector[cursor])) {
                    group_by_columns.push_back(token_vector[cursor]);
                    cursor++;
                }
                if (group_by_columns.empty()) {
                    std::cout << "Syntax Error: GROUP BY requires at least one column.\n";
                    return;
                }
            } else if (token_vector[cursor] == "having") {
                if (!parse_having_clause(token_vector, cursor, having, cursor)) {
                    std::cout << "Syntax Error: HAVING must be of the form HAVING expr op value.\n";
                    return;
                }
            } else if (token_vector[cursor] == "order") {
                if ((int)token_vector.size() <= cursor + 2 || token_vector[cursor + 1] != "by") {
                    std::cout << "Syntax Error: ORDER BY requires a column.\n";
                    return;
                }
                modifiers.has_order_by = true;
                modifiers.order_by_column = token_vector[cursor + 2];
                cursor += 3;
                if ((int)token_vector.size() > cursor &&
                    (token_vector[cursor] == "asc" || token_vector[cursor] == "desc")) {
                    modifiers.order_desc = (token_vector[cursor] == "desc");
                    cursor++;
                }
            } else if (token_vector[cursor] == "limit") {
                if ((int)token_vector.size() <= cursor + 1) {
                    std::cout << "Syntax Error: LIMIT requires a number.\n";
                    return;
                }
                try {
                    modifiers.limit = std::stoi(token_vector[cursor + 1]);
                } catch (...) {
                    std::cout << "Syntax Error: LIMIT must be an integer.\n";
                    return;
                }
                cursor += 2;
            } else {
                std::cout << "Syntax Error: Unsupported clause after JOIN.\n";
                return;
            }
        }

        execute_select_join_nested(target_cols, table_name, right_table, left_on, right_on,
                                   left_join,
                                   where_present, where_col, where_val, target_result);
        if (!target_result->success) return;
        if (!group_by_columns.empty()) {
            target_result->success = false;
            target_result->message = "Error: GROUP BY over JOIN is not supported yet.";
            std::cout << "Error: GROUP BY over JOIN is not supported yet.\n";
            return;
        }
        if (having.present) {
            target_result->success = false;
            target_result->message = "Error: HAVING over JOIN is not supported yet.";
            std::cout << "Error: HAVING over JOIN is not supported yet.\n";
            return;
        }
        if (!apply_aggregates(*target_result, aggregates)) {
            target_result->success = false;
            target_result->message = "Error: Invalid aggregate usage.";
            std::cout << "Error: Invalid aggregate usage.\n";
            return;
        }
        if (!apply_order_by_and_limit(*target_result, modifiers, aggregates)) {
            target_result->success = false;
            target_result->message = "Error: ORDER BY column not found in selected output.";
            std::cout << "Error: ORDER BY column not found in selected output.\n";
            return;
        }
        if (res == nullptr) {
            print_table(target_result->schema, target_result->rows);
        }
        return;
    }

    WhereClause where_clause;
    SelectModifiers modifiers;
    std::vector<std::string> group_by_columns;
    HavingClause having;
    int where_start = from_index + 2;
    int cursor = where_start;

    if ((int)token_vector.size() > cursor &&
        token_vector[cursor] == "where") {

        if ((int)token_vector.size() < cursor + 4) {
            if (res) {
                res->success = false;
                res->message = "Syntax Error: Incomplete WHERE clause.";
            }
            std::cout << "Syntax Error: Incomplete WHERE clause.\n";
            std::cout << "Usage: WHERE <column> = <value>\n";
            return;
        }

        std::string op = token_vector[cursor + 2];

        if (op != "=") {
            if (res) {
                res->success = false;
                res->message = "Syntax Error: Only '=' is supported in WHERE clause.";
            }
            std::cout << "Syntax Error: Only '=' is supported in WHERE clause.\n";
            return;
        }

        where_clause.present = true;
        where_clause.column = token_vector[cursor + 1];
        where_clause.op = op;
        where_clause.value = token_vector[cursor + 3];
        cursor += 4;

        if (where_clause.value.empty()) {
            if (res) {
                res->success = false;
                res->message = "Syntax Error: Missing value in WHERE clause.";
            }
            std::cout << "Syntax Error: Missing value in WHERE clause.\n";
            return;
        }
    }

    while ((int)token_vector.size() > cursor) {
        if (token_vector[cursor] == "group") {
            if ((int)token_vector.size() <= cursor + 2 || token_vector[cursor + 1] != "by") {
                if (res) {
                    res->success = false;
                    res->message = "Syntax Error: GROUP BY requires a column.";
                }
                std::cout << "Syntax Error: GROUP BY requires a column.\n";
                return;
            }
            cursor += 2;
            while ((int)token_vector.size() > cursor && !is_select_clause_keyword(token_vector[cursor])) {
                group_by_columns.push_back(token_vector[cursor]);
                cursor++;
            }
            if (group_by_columns.empty()) {
                if (res) {
                    res->success = false;
                    res->message = "Syntax Error: GROUP BY requires at least one column.";
                }
                std::cout << "Syntax Error: GROUP BY requires at least one column.\n";
                return;
            }
        } else if (token_vector[cursor] == "having") {
            if (!parse_having_clause(token_vector, cursor, having, cursor)) {
                if (res) {
                    res->success = false;
                    res->message = "Syntax Error: HAVING must be of the form HAVING expr op value.";
                }
                std::cout << "Syntax Error: HAVING must be of the form HAVING expr op value.\n";
                return;
            }
        } else if (token_vector[cursor] == "order") {
            if ((int)token_vector.size() <= cursor + 2 || token_vector[cursor + 1] != "by") {
                if (res) {
                    res->success = false;
                    res->message = "Syntax Error: ORDER BY requires a column.";
                }
                std::cout << "Syntax Error: ORDER BY requires a column.\n";
                return;
            }
            modifiers.has_order_by = true;
            modifiers.order_by_column = token_vector[cursor + 2];
            cursor += 3;
            if ((int)token_vector.size() > cursor &&
                (token_vector[cursor] == "asc" || token_vector[cursor] == "desc")) {
                modifiers.order_desc = (token_vector[cursor] == "desc");
                cursor++;
            }
        } else if (token_vector[cursor] == "limit") {
            if ((int)token_vector.size() <= cursor + 1) {
                if (res) {
                    res->success = false;
                    res->message = "Syntax Error: LIMIT requires a number.";
                }
                std::cout << "Syntax Error: LIMIT requires a number.\n";
                return;
            }
            try {
                modifiers.limit = std::stoi(token_vector[cursor + 1]);
            } catch (...) {
                if (res) {
                    res->success = false;
                    res->message = "Syntax Error: LIMIT must be an integer.";
                }
                std::cout << "Syntax Error: LIMIT must be an integer.\n";
                return;
            }
            cursor += 2;
        } else {
            if (res) {
                res->success = false;
                res->message = "Syntax Error: Unsupported trailing clause in SELECT.";
            }
            std::cout << "Syntax Error: Unsupported trailing clause in SELECT.\n";
            return;
        }
    }

    if (where_clause.present) {
        execute_select_where(table_name, target_cols, where_clause, target_result);
    } else {
        execute_select(table_name, target_cols, target_result);
    }

    if (!target_result->success) {
        return;
    }

    if (!group_by_columns.empty()) {
        if (!apply_group_by(*target_result, select_items, group_by_columns)) {
            target_result->success = false;
            target_result->message = "Error: Invalid GROUP BY usage.";
            std::cout << "Error: Invalid GROUP BY usage.\n";
            return;
        }
    } else if (has_plain_column && has_aggregate) {
        target_result->success = false;
        target_result->message = "Error: Mixed columns and aggregates require GROUP BY.";
        std::cout << "Error: Mixed columns and aggregates require GROUP BY.\n";
        return;
    } else if (!apply_aggregates(*target_result, aggregates)) {
        target_result->success = false;
        target_result->message = "Error: Invalid aggregate usage.";
        std::cout << "Error: Invalid aggregate usage.\n";
        return;
    }

    if (!apply_having(*target_result, having)) {
        target_result->success = false;
        target_result->message = "Error: Invalid HAVING usage.";
        std::cout << "Error: Invalid HAVING usage.\n";
        return;
    }

    if (!apply_order_by_and_limit(*target_result, modifiers, aggregates)) {
        target_result->success = false;
        target_result->message = "Error: ORDER BY column not found in selected output.";
        std::cout << "Error: ORDER BY column not found in selected output.\n";
        return;
    }

    if (res == nullptr) {
        print_table(target_result->schema, target_result->rows);
    }
}
