#include "auth.h"
#include "file_handler.h"
#include "parser.h"

#include <iostream>

int main() {
    system_check();
    AuthManager::init();

    const std::string username = "user_mgmt_case";
    if (AuthManager::user_exists(username)) {
        AuthManager::drop_user(username);
    }

    execute_query_string("CREATE USER user_mgmt_case IDENTIFIED BY \"pw123\";");
    if (!AuthManager::user_exists(username)) {
        std::cerr << "create user failed\n";
        return 1;
    }

    execute_query_string("SHOW USERS;");

    execute_query_string("DROP USER user_mgmt_case;");
    if (AuthManager::user_exists(username)) {
        std::cerr << "drop user failed\n";
        return 1;
    }

    std::cout << "user management ok\n";
    return 0;
}
