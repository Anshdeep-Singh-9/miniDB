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

}  // namespace

int main() {
    QueryResult result;
    const std::string table = "select_feature_test";

    run_query("DROP TABLE " + table + ";", result);
    if (!run_query("CREATE TABLE " + table + " (id INT, name VARCHAR(50), dept VARCHAR(20));", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }

    run_query("INSERT INTO " + table + " VALUES (2, \"Riya\", \"ECE\");", result);
    run_query("INSERT INTO " + table + " VALUES (1, \"Aman\", \"CSE\");", result);
    run_query("INSERT INTO " + table + " VALUES (3, \"Zoya\", \"ME\");", result);

    if (!run_query("SELECT * FROM " + table + " ORDER BY name;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 3, "ORDER BY should return 3 rows")) return 1;
    if (!expect(result.rows[0][1].string_value == "Aman", "ORDER BY failed")) return 1;

    if (!run_query("SELECT * FROM " + table + " LIMIT 2;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 2, "LIMIT should return 2 rows")) return 1;

    if (!run_query("SELECT name FROM " + table + " ORDER BY id;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 3, "Hidden ORDER BY should return 3 rows")) return 1;
    if (!expect(result.rows[0][0].string_value == "Aman", "ORDER BY hidden column failed")) return 1;

    if (!run_query("SELECT COUNT(*) FROM " + table + ";", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 1 && result.rows[0][0].int_value == 3, "COUNT(*) failed")) return 1;

    if (!run_query("SELECT SUM(id), AVG(id), MIN(name), MAX(name) FROM " + table + ";", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 1, "Aggregate query should return 1 row")) return 1;
    if (!expect(result.rows[0][0].int_value == 6, "SUM(id) failed")) return 1;
    if (!expect(result.rows[0][1].int_value == 2, "AVG(id) failed")) return 1;
    if (!expect(result.rows[0][2].string_value == "Aman", "MIN(name) failed")) return 1;
    if (!expect(result.rows[0][3].string_value == "Zoya", "MAX(name) failed")) return 1;

    if (!run_query("SELECT dept, COUNT(*) FROM " + table + " GROUP BY dept ORDER BY dept;", result)) {
        std::cerr << result.message << "\n";
        return 1;
    }
    if (!expect(result.rows.size() == 3, "GROUP BY should return 3 groups")) return 1;
    if (!expect(result.rows[0][0].string_value == "CSE" && result.rows[0][1].int_value == 1, "GROUP BY CSE failed")) return 1;

    run_query("DROP TABLE " + table + ";", result);
    return 0;
}
