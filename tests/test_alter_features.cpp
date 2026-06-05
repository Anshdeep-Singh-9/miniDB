#include "file_handler.h"
#include "parser.h"

#include <iostream>
#include <string>

namespace {

bool run_query(const std::string& sql, QueryResult& result) {
    result = QueryResult();
    execute_query_string(sql, &result);
    return result.success;
}

bool expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << msg << "\n";
        return false;
    }
    return true;
}

}

int main() {
    QueryResult result;
    const std::string table_name = "alter_feature_test";

    run_query("DROP TABLE " + table_name + ";", result);
    if (!run_query("CREATE TABLE " + table_name + " (id INT, name VARCHAR(50));", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }

    run_query("INSERT INTO " + table_name + " VALUES (1, \"Aman\");", result);
    run_query("INSERT INTO " + table_name + " VALUES (2, \"Riya\");", result);

    if (!run_query("ALTER TABLE " + table_name + " ADD COLUMN age INT DEFAULT 18;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }

    table* meta = fetch_meta_data(table_name);
    if (!expect(meta != NULL, "ALTER: metadata not found")) return 1;
    if (!expect(meta->count == 3, "ALTER: expected 3 columns")) return 1;
    if (!expect(std::string(meta->col[2].col_name) == "age", "ALTER: new column name mismatch")) return 1;
    delete meta;

    if (!run_query("SELECT * FROM " + table_name + " ORDER BY id;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 2, "ALTER: expected 2 existing rows")) return 1;
    if (!expect(result.rows[0].size() == 3, "ALTER: projected row should have 3 columns")) return 1;
    if (!expect(result.rows[0][2].int_value == 18 && result.rows[1][2].int_value == 18,
                "ALTER: existing rows not backfilled with default")) return 1;

    if (!run_query("INSERT INTO " + table_name + " VALUES (3, \"Kabir\", 21);", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }

    if (!run_query("SELECT * FROM " + table_name + " ORDER BY id;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 3, "ALTER: expected 3 rows after new insert")) return 1;
    if (!expect(result.rows[2][2].int_value == 21, "ALTER: new row value for added column incorrect")) return 1;

    run_query("DROP TABLE " + table_name + ";", result);
    return 0;
}
