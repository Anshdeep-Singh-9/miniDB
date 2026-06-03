#include "auth.h"
#include "sha256.h"
#include "file_handler.h"
#include "disk_manager.h"
#include "buffer_pool_manager.h"
#include "data_page.h"
#include "tuple_serializer.h"
#include "BPtree.h"
#include "vacuum.h"
#include <iostream>
#include <random>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <filesystem>

std::unordered_map<std::string, std::string> AuthManager::sessions;
namespace fs = std::filesystem;

namespace {

std::vector<ColumnSchema> auth_schema() {
    std::vector<ColumnSchema> schema;
    schema.push_back(ColumnSchema("id", STORAGE_COLUMN_INT, 4));
    schema.push_back(ColumnSchema("username", STORAGE_COLUMN_VARCHAR, 50));
    schema.push_back(ColumnSchema("password_hash", STORAGE_COLUMN_VARCHAR, 64));
    return schema;
}

std::string auth_data_path() {
    return (global_system_root() / "auth" / "data.dat").string();
}

std::string auth_index_path() {
    return (project_root() / "table" / "__sys_auth").string();
}

}

bool AuthManager::init() {
    // What: ensure the internal auth table exists before login/register logic runs.
    // Why: MiniDB stores users inside its own page-based storage, not in an external service.
    // Example: first program start creates system/auth metadata and data.dat.
    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) {
        create_auth_table();
    } else {
        delete meta;
    }
    return true;
}

void AuthManager::create_auth_table() {
    // What: create metadata for the internal users table.
    // Why: authentication uses the same storage concepts as normal tables: schema, pages, tuples.
    // Example: auth row stores id, username, and SHA-256 password_hash.
    table* temp = new table();
    strcpy(temp->name, "auth");
    temp->count = 3;
    
    // Column 0: id (INT) - Primary Key for B+ Tree
    strcpy(temp->col[0].col_name, "id");
    temp->col[0].type = INT;
    temp->col[0].size = 4;

    // Column 1: username (VARCHAR 50)
    strcpy(temp->col[1].col_name, "username");
    temp->col[1].type = VARCHAR;
    temp->col[1].size = 50;

    // Column 2: password_hash (VARCHAR 64)
    strcpy(temp->col[2].col_name, "password_hash");
    temp->col[2].type = VARCHAR;
    temp->col[2].size = 64;

    // Calculate record size
    int size = 0;
    temp->prefix[0] = 0;
    temp->prefix[1] = sizeof(int);
    size += sizeof(int);
    temp->prefix[2] = (MAX_VARCHAR + 1) + temp->prefix[1];
    size += (MAX_VARCHAR + 1);
    temp->prefix[3] = (64 + 1) + temp->prefix[2];
    size += (64 + 1);
    
    temp->size = size;
    temp->data_size = 0;
    temp->rec_count = 0;

    store_system_meta_data(temp);

    // Create empty data file and B+ Tree
    std::string data_path = auth_data_path();
    DiskManager dm(data_path);
    dm.open_or_create();
    
    // Note: BPtree implementation seems to assume "./table/TNAME/" path
    // For simplicity, we'll store system indexes in the same place but prefixed with __sys_
    BPtree index((char*)"__sys_auth");
    
    delete temp;
}

bool AuthManager::register_user(const std::string& username, const std::string& password) {
    // What: insert a new username and hashed password into the auth storage table.
    // Why: raw passwords should not be stored; only their hash is persisted.
    // Example: register_user("tester", "pass") stores SHA256("pass") in system/auth/data.dat.
    if (username.empty() || password.empty()) return false;
    if (user_exists(username)) return false;

    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) return false;

    std::vector<UserRecord> users;
    if (!load_users(users)) {
        delete meta;
        return false;
    }

    int id = 1;
    for (std::size_t i = 0; i < users.size(); ++i) {
        id = std::max(id, users[i].id + 1);
    }
    std::string hash = SHA256::hash(password);

    users.push_back(UserRecord{id, username, hash});
    bool ok = rewrite_users(users);

    if (ok) {
        meta->rec_count = static_cast<int>(users.size());
        store_system_meta_data(meta);
    }

    delete meta;
    return ok;
}

bool AuthManager::authenticate(const std::string& username, const std::string& password) {
    // What: verify login by scanning stored auth tuples and comparing password hashes.
    // Why: the entered password is hashed and compared with the saved hash, not with plaintext.
    // Example: ./miniDB -u tester -p asks password, then checks username + SHA-256 hash.
    if (username.empty() || password.empty()) return false;
    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) return false;

    std::vector<ColumnSchema> schema = auth_schema();
    std::string data_path = auth_data_path();
    DiskManager data_disk(data_path);
    if (!data_disk.open_or_create()) {
        delete meta;
        return false;
    }
    BufferPoolManager buffer_pool(4, &data_disk);

    std::string input_hash = SHA256::hash(password);

    for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
        char* frame_data = buffer_pool.fetch_page(i);
        DataPage page;
        page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            std::vector<char> tuple_data;
            if (page.read_tuple(slot_id, tuple_data)) {
                std::vector<TupleValue> values;
                TupleSerializer::deserialize(schema, tuple_data, values);
                if (values[1].string_value == username && values[2].string_value == input_hash) {
                    buffer_pool.unpin_page(i, false);
                    delete meta;
                    return true;
                }
            }
        }
        buffer_pool.unpin_page(i, false);
    }

    delete meta;
    return false;
}

bool AuthManager::user_exists(const std::string& username) {
    // What: check whether a username is already present in auth storage.
    // Why: duplicate usernames would make login ambiguous.
    // Example: registering "tester" twice returns false on the second attempt.
    if (username.empty()) return false;
    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) return false;

    std::vector<ColumnSchema> schema = auth_schema();
    std::string data_path = auth_data_path();
    DiskManager data_disk(data_path);
    if (!data_disk.open_or_create()) {
        delete meta;
        return false;
    }
    BufferPoolManager buffer_pool(4, &data_disk);

    for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
        char* frame_data = buffer_pool.fetch_page(i);
        DataPage page;
        page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            std::vector<char> tuple_data;
            if (page.read_tuple(slot_id, tuple_data)) {
                std::vector<TupleValue> values;
                TupleSerializer::deserialize(schema, tuple_data, values);
                if (values[1].string_value == username) {
                    buffer_pool.unpin_page(i, false);
                    delete meta;
                    return true;
                }
            }
        }
        buffer_pool.unpin_page(i, false);
    }

    delete meta;
    return false;
}

bool AuthManager::has_any_user() {
    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) return false;
    bool exists = meta->rec_count > 0;
    delete meta;
    return exists;
}

std::vector<std::string> AuthManager::list_users() {
    std::vector<UserRecord> users;
    std::vector<std::string> usernames;
    if (!load_users(users)) {
        return usernames;
    }

    std::sort(users.begin(), users.end(),
              [](const UserRecord& lhs, const UserRecord& rhs) {
                  return lhs.username < rhs.username;
              });

    for (std::size_t i = 0; i < users.size(); ++i) {
        usernames.push_back(users[i].username);
    }

    return usernames;
}

bool AuthManager::drop_user(const std::string& username) {
    if (username.empty()) return false;

    std::vector<UserRecord> users;
    if (!load_users(users)) return false;

    std::vector<UserRecord> kept;
    kept.reserve(users.size());
    bool removed = false;

    for (std::size_t i = 0; i < users.size(); ++i) {
        if (users[i].username == username) {
            removed = true;
            continue;
        }
        kept.push_back(users[i]);
    }

    if (!removed) return false;

    if (!rewrite_users(kept)) return false;

    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) return false;
    meta->rec_count = static_cast<int>(kept.size());
    store_system_meta_data(meta);
    delete meta;

    for (std::unordered_map<std::string, std::string>::iterator it = sessions.begin();
         it != sessions.end();) {
        if (it->second == username) {
            it = sessions.erase(it);
        } else {
            ++it;
        }
    }

    return true;
}

bool AuthManager::load_users(std::vector<UserRecord>& users_out) {
    users_out.clear();

    struct table* meta = fetch_system_meta_data("auth");
    if (!meta) return false;

    std::vector<ColumnSchema> schema = auth_schema();
    std::string data_path = auth_data_path();
    DiskManager data_disk(data_path);
    if (!data_disk.open_or_create()) {
        delete meta;
        return false;
    }
    BufferPoolManager buffer_pool(4, &data_disk);

    for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
        char* frame_data = buffer_pool.fetch_page(i);
        DataPage page;
        page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            std::vector<char> tuple_data;
            if (!page.read_tuple(slot_id, tuple_data)) continue;

            std::vector<TupleValue> values;
            if (!TupleSerializer::deserialize(schema, tuple_data, values) || values.size() != 3) {
                continue;
            }

            users_out.push_back(UserRecord{
                values[0].int_value,
                values[1].string_value,
                values[2].string_value
            });
        }

        buffer_pool.unpin_page(i, false);
    }

    delete meta;
    return true;
}

bool AuthManager::rewrite_users(const std::vector<UserRecord>& users) {
    std::error_code ec;
    fs::create_directories(global_system_root() / "auth", ec);
    fs::remove(auth_data_path(), ec);
    fs::remove_all(auth_index_path(), ec);

    DiskManager data_disk(auth_data_path());
    if (!data_disk.open_or_create()) {
        return false;
    }

    BPtree index((char*)"__sys_auth");
    BufferPoolManager buffer_pool(4, &data_disk);
    std::vector<ColumnSchema> schema = auth_schema();

    for (std::size_t i = 0; i < users.size(); ++i) {
        const UserRecord& user = users[i];
        std::vector<TupleValue> values;
        values.push_back(TupleValue::FromInt(user.id));
        values.push_back(TupleValue::FromVarchar(user.username));
        values.push_back(TupleValue::FromVarchar(user.password_hash));

        std::vector<char> tuple_data;
        if (!TupleSerializer::serialize(schema, values, tuple_data)) {
            return false;
        }

        uint32_t target_page_id = INVALID_PAGE_ID;
        DataPage page;
        char* target_buffer = NULL;

        if (data_disk.page_count() == 0) {
            target_buffer = buffer_pool.new_page(target_page_id);
            if (target_buffer == NULL) return false;
            page.initialize(target_page_id);
        } else {
            target_page_id = data_disk.page_count() - 1;
            target_buffer = buffer_pool.fetch_page(target_page_id);
            if (target_buffer == NULL) return false;
            page.load_from_buffer(target_buffer, STORAGE_PAGE_SIZE);
            if (!page.can_store(tuple_data.size()) && compact_page_buffer(target_buffer)) {
                page.load_from_buffer(target_buffer, STORAGE_PAGE_SIZE);
            }
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
        index.insert(user.id, RID(target_page_id, slot_id));
    }

    return true;
}

std::string AuthManager::create_session(const std::string& username) {
    // What: create an in-memory session token for API-style usage.
    // Why: after password validation, callers can reuse a token instead of resending password.
    // Example: successful login maps random token -> "tester" until logout.
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(alphabet) - 2);

    std::string token;
    for (int i = 0; i < 32; ++i) {
        token += alphabet[dis(gen)];
    }

    sessions[token] = username;
    return token;
}

bool AuthManager::validate_session(const std::string& token) {
    return sessions.find(token) != sessions.end();
}

void AuthManager::end_session(const std::string& token) {
    sessions.erase(token);
}

std::string AuthManager::get_user_from_session(const std::string& token) {
    if (validate_session(token)) return sessions[token];
    return "";
}
