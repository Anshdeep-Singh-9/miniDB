#include "auth.h"
#include "file_handler.h"
#include "parser.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    system_check();
    AuthManager::init();

    const std::string user_a = "ns_user_a";
    const std::string user_b = "ns_user_b";

    if (!AuthManager::user_exists(user_a)) {
        AuthManager::register_user(user_a, "pw123");
    }
    if (!AuthManager::user_exists(user_b)) {
        AuthManager::register_user(user_b, "pw123");
    }

    set_active_user(user_a);
    system_check();
    execute_query_string("DROP TABLE scoped_table;");
    execute_query_string("CREATE TABLE scoped_table (id INT, name VARCHAR(50));");

    fs::path user_a_meta = table_dir_path("scoped_table") / "met";
    if (!fs::exists(user_a_meta)) {
        std::cerr << "table not created for user A\n";
        return 1;
    }

    set_active_user(user_b);
    system_check();
    fs::path user_b_meta = table_dir_path("scoped_table") / "met";
    if (fs::exists(user_b_meta)) {
        std::cerr << "user B unexpectedly sees user A table\n";
        return 1;
    }

    std::cout << "user namespace ok\n";
    return 0;
}
