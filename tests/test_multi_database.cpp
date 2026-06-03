#include "auth.h"
#include "file_handler.h"
#include "parser.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    system_check();
    AuthManager::init();

    const std::string user = "multi_db_user";
    if (!AuthManager::user_exists(user)) {
        AuthManager::register_user(user, "pw123");
    }

    set_active_user(user);
    ensure_default_database_for_active_user();
    set_active_database("default");
    system_check();

    execute_query_string("DROP DATABASE analytics;");
    execute_query_string("CREATE DATABASE analytics;");
    if (!database_exists("analytics")) {
        std::cerr << "database creation failed\n";
        return 1;
    }

    execute_query_string("CREATE TABLE root_table (id INT, name VARCHAR(50));");
    if (!fs::exists(table_dir_path("root_table") / "met")) {
        std::cerr << "default db table creation failed\n";
        return 1;
    }

    execute_query_string("USE analytics;");
    execute_query_string("CREATE TABLE analytics_table (id INT, name VARCHAR(50));");
    if (!fs::exists(table_dir_path("analytics_table") / "met")) {
        std::cerr << "analytics db table creation failed\n";
        return 1;
    }
    if (fs::exists(table_dir_path("root_table") / "met")) {
        std::cerr << "default db table leaked into analytics db\n";
        return 1;
    }

    execute_query_string("USE default;");
    if (!fs::exists(table_dir_path("root_table") / "met")) {
        std::cerr << "default db table missing after switch back\n";
        return 1;
    }

    std::cout << "multi database ok\n";
    return 0;
}
