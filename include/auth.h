#ifndef AUTH_H
#define AUTH_H

#include <string>
#include <vector>
#include <unordered_map>
#include "declaration.h"

class AuthManager {
public:
    struct UserRecord {
        int id;
        std::string username;
        std::string password_hash;
    };

    static bool init();
    static bool register_user(const std::string& username, const std::string& password);
    static bool authenticate(const std::string& username, const std::string& password);
    static bool user_exists(const std::string& username);
    static bool has_any_user();
    static std::vector<std::string> list_users();
    static bool drop_user(const std::string& username);

    // Session management for API
    static std::string create_session(const std::string& username);
    static bool validate_session(const std::string& token);
    static void end_session(const std::string& token);
    static std::string get_user_from_session(const std::string& token);

private:
    static void create_auth_table();
    static bool load_users(std::vector<UserRecord>& users_out);
    static bool rewrite_users(const std::vector<UserRecord>& users);
    static std::unordered_map<std::string, std::string> sessions; // token -> username
};

#endif
