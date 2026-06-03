#include "crow_all.h"
#include "declaration.h"
#include "file_handler.h"
#include "buffer_pool_manager.h"
#include "data_page.h"
#include "disk_manager.h"
#include "tuple_serializer.h"
#include "BPtree.h"
#include "create.h"
#include "insert.h"
#include "recovery_manager.h"
#include "auth.h"
#include "parser.h"
#include "query_result.h"
#include <vector>
#include <string>
#include <cctype>

// ---------------------------------------------------------------------------
// Convert QueryResult to JSON for API response
// ---------------------------------------------------------------------------
static crow::json::wvalue query_result_to_json(const QueryResult& res) {
    crow::json::wvalue result;
    if (!res.success) {
        result["type"] = "error";
        result["message"] = res.message;
        return result;
    }

    if (res.is_select) {
        result["type"]      = "select";
        result["strategy"]  = res.strategy;
        result["row_count"] = (int)res.rows.size();

        std::vector<crow::json::wvalue> cols_json;
        for (const auto& col : res.schema) {
            crow::json::wvalue c;
            c["name"] = col.name;
            c["type"] = (col.type == STORAGE_COLUMN_INT ? "INT" : "VARCHAR");
            c["size"] = col.max_length;
            cols_json.push_back(std::move(c));
        }
        result["columns"] = std::move(cols_json);

        std::vector<crow::json::wvalue> rows_json;
        for (const auto& row : res.rows) {
            crow::json::wvalue r;
            for (size_t i = 0; i < row.size() && i < res.schema.size(); i++) {
                if (row[i].type == STORAGE_COLUMN_INT) r[res.schema[i].name] = row[i].int_value;
                else                                   r[res.schema[i].name] = row[i].string_value;
            }
            rows_json.push_back(std::move(r));
        }
        result["rows"] = std::move(rows_json);
    } else {
        result["type"]    = "dml";
        result["message"] = res.message.empty() ? "Query executed successfully" : res.message;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Function to convert full table data to JSON (used by GET /table/<name>)
// ---------------------------------------------------------------------------
crow::json::wvalue table_to_json(const std::string& table_name) {
    struct table* meta = fetch_meta_data(table_name);
    if (!meta) {
        return crow::json::wvalue({{"error", "Table not found"}});
    }

    std::vector<ColumnSchema> schema;
    for (int i = 0; i < meta->count; i++) {
        ColumnSchema col_schema(meta->col[i].col_name,
                                meta->col[i].type == INT ? STORAGE_COLUMN_INT
                                                         : STORAGE_COLUMN_VARCHAR,
                                meta->col[i].size);
        schema.push_back(col_schema);
    }

    std::string data_path = "table/" + table_name + "/data.dat";
    DiskManager data_disk(data_path);
    if (!data_disk.open_or_create()) {
        delete meta;
        return crow::json::wvalue({{"error", "Could not open data file"}});
    }

    BufferPoolManager buffer_pool(4, &data_disk);
    std::vector<crow::json::wvalue> rows;

    for (uint32_t i = 0; i < data_disk.page_count(); ++i) {
        char* frame_data = buffer_pool.fetch_page(i);
        if (frame_data == NULL) continue;

        DataPage page;
        page.load_from_buffer(frame_data, STORAGE_PAGE_SIZE);

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            std::vector<char> tuple_data;
            if (page.read_tuple(slot_id, tuple_data)) {
                std::vector<TupleValue> values;
                if (TupleSerializer::deserialize(schema, tuple_data, values)) {
                    crow::json::wvalue row;
                    for (size_t j = 0; j < values.size(); ++j) {
                        if (values[j].type == STORAGE_COLUMN_INT)
                            row[schema[j].name] = values[j].int_value;
                        else
                            row[schema[j].name] = values[j].string_value;
                    }
                    rows.push_back(std::move(row));
                }
            }
        }
        buffer_pool.unpin_page(i, false);
    }

    delete meta;
    return crow::json::wvalue(rows);
}

int main() {
    system_check();
    AuthManager::init();
    crow::SimpleApp app;

        auto is_authenticated = [](const crow::request& req) {
            auto token = req.get_header_value("X-Session-Token");
            return AuthManager::validate_session(token);
        };

        CROW_ROUTE(app, "/login").methods(crow::HTTPMethod::POST)
        ([](const crow::request& req) {
            auto x = crow::json::load(req.body);
            if (!x || !x.has("username") || !x.has("password")) {
                return crow::response(400, "Invalid JSON: username and password required");
            }
            std::string username = x["username"].s();
            std::string password = x["password"].s();
            if (AuthManager::authenticate(username, password)) {
                std::string token = AuthManager::create_session(username);
                crow::json::wvalue res;
                res["token"] = token;
                res["status"] = "success";
                return crow::response(res);
            } else {
                return crow::response(401, "Invalid credentials");
            }
        });

        CROW_ROUTE(app, "/register").methods(crow::HTTPMethod::POST)
        ([](const crow::request& req) {
            auto x = crow::json::load(req.body);
            if (!x || !x.has("username") || !x.has("password")) {
                return crow::response(400, "Invalid JSON: username and password required");
            }
            std::string username = x["username"].s();
            std::string password = x["password"].s();
            if (AuthManager::user_exists(username)) {
                return crow::response(400, "User already exists");
            }
            if (AuthManager::register_user(username, password)) {
                return crow::response(201, "User registered successfully");
            } else {
                return crow::response(500, "Failed to register user");
            }
        });

        CROW_ROUTE(app, "/logout").methods(crow::HTTPMethod::POST)
        ([&is_authenticated](const crow::request& req) {
            auto token = req.get_header_value("X-Session-Token");
            if (AuthManager::validate_session(token)) {
                AuthManager::end_session(token);
                return crow::response(200, "Logged out successfully");
            }
            return crow::response(401, "Not logged in");
        });

        CROW_ROUTE(app, "/table/<string>")
        ([&is_authenticated](const crow::request& req, std::string table_name) {
            if (!is_authenticated(req)) return crow::response(401, "Authentication required");
            auto result = table_to_json(table_name);
            return crow::response(result);
        });

        CROW_ROUTE(app, "/meta/<string>")
        ([&is_authenticated](const crow::request& req, std::string table_name) {
            if (!is_authenticated(req)) return crow::response(401, "Authentication required");
            struct table* meta = fetch_meta_data(table_name);
            if (!meta) return crow::response(404, "Table not found");

            crow::json::wvalue result;
            result["table_name"]   = meta->name;
            result["column_count"] = meta->count;
            result["record_size"]  = meta->size;

            std::vector<crow::json::wvalue> columns;
            for (int i = 0; i < meta->count; i++) {
                crow::json::wvalue col;
                col["name"] = meta->col[i].col_name;
                col["type"] = (meta->col[i].type == INT ? "INT" : "VARCHAR");
                col["size"] = meta->col[i].size;
                columns.push_back(std::move(col));
            }
            result["columns"] = std::move(columns);
            delete meta;
            return crow::response(result);
        });

    CROW_ROUTE(app, "/tables")
    ([&is_authenticated](const crow::request& req) {
        if (!is_authenticated(req)) return crow::response(401, "Authentication required");
        std::vector<std::string> tables;
        std::ifstream fp("./table/table_list");
        std::string name;
        if (fp.is_open()) {
            while (fp >> name) tables.push_back(name);
            fp.close();
        }
        crow::json::wvalue result;
        result["tables"] = tables;
        return crow::response(result);
    });

    CROW_ROUTE(app, "/create").methods(crow::HTTPMethod::POST)
    ([&is_authenticated](const crow::request& req) {
        if (!is_authenticated(req)) return crow::response(401, "Authentication required");
        auto x = crow::json::load(req.body);
        if (!x || !x.has("table_name") || !x.has("columns")) {
            return crow::response(400, "Invalid JSON");
        }
        std::string table_name = x["table_name"].s();
        std::vector<std::pair<std::string, std::string>> columns;
        for (auto& col : x["columns"]) {
            if (!col.has("name") || !col.has("type")) return crow::response(400, "Invalid column definition");
            columns.push_back({col["name"].s(), col["type"].s()});
        }
        execute_create_query(table_name, columns);
        return crow::response(201, "Table created (check logs for success/failure)");
    });

    CROW_ROUTE(app, "/insert/<string>").methods(crow::HTTPMethod::POST)
    ([&is_authenticated](const crow::request& req, std::string table_name) {
        if (!is_authenticated(req)) return crow::response(401, "Authentication required");
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Invalid JSON");

        struct table* meta = fetch_meta_data(table_name);
        if (!meta) return crow::response(404, "Table not found");

        std::vector<ColumnSchema> schema;
        std::vector<TupleValue> values;
        for (int i = 0; i < meta->count; i++) {
            std::string col_name = meta->col[i].col_name;
            schema.push_back(ColumnSchema(col_name,
                                          meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
                                          meta->col[i].size));
            if (!x.has(col_name)) { delete meta; return crow::response(400, "Missing column: " + col_name); }
            if (meta->col[i].type == INT) values.push_back(TupleValue::FromInt(x[col_name].i()));
            else                          values.push_back(TupleValue::FromVarchar(x[col_name].s()));
        }
        char tname[MAX_NAME];
        strcpy(tname, table_name.c_str());
        insert_command(tname, values, schema);
        delete meta;
        return crow::response(200, "Insertion triggered (check logs for success/failure)");
    });

    CROW_ROUTE(app, "/bulk_insert/<string>").methods(crow::HTTPMethod::POST)
    ([&is_authenticated](const crow::request& req, std::string table_name) {
        if (!is_authenticated(req)) return crow::response(401, "Authentication required");
        auto x = crow::json::load(req.body);
        if (!x || x.size() == 0) return crow::response(400, "Invalid JSON or empty array");

        struct table* meta = fetch_meta_data(table_name);
        if (!meta) return crow::response(404, "Table not found");

        std::vector<ColumnSchema> schema;
        for (int i = 0; i < meta->count; i++) {
            schema.push_back(ColumnSchema(meta->col[i].col_name,
                                         meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
                                         meta->col[i].size));
        }
        std::vector<std::vector<TupleValue>> all_values;
        for (auto& row : x) {
            std::vector<TupleValue> values;
            bool valid_row = true;
            for (int i = 0; i < meta->count; i++) {
                std::string col_name = meta->col[i].col_name;
                if (!row.has(col_name)) { valid_row = false; break; }
                if (meta->col[i].type == INT) values.push_back(TupleValue::FromInt(row[col_name].i()));
                else                          values.push_back(TupleValue::FromVarchar(row[col_name].s()));
            }
            if (valid_row) all_values.push_back(values);
        }
        char tname[MAX_NAME];
        strcpy(tname, table_name.c_str());
        bulk_insert_command(tname, all_values, schema);
        delete meta;
        return crow::response(200, "Bulk insertion triggered (check logs for success/failure)");
    });

    CROW_ROUTE(app, "/health")
    ([]() { return "MiniDB API is running!"; });

    // ---------------------------------------------------------------------------
    // /query — detects SELECT and returns structured JSON; DML returns a message
    // ---------------------------------------------------------------------------
    CROW_ROUTE(app, "/query").methods(crow::HTTPMethod::POST)
    ([&is_authenticated](const crow::request& req) {
        if (!is_authenticated(req)) return crow::response(401, "Authentication required");

        auto x = crow::json::load(req.body);
        if (!x || !x.has("query")) return crow::response(400, "Invalid JSON: 'query' field required");

        std::string query = x["query"].s();

        QueryResult res;
        execute_query_string(query, &res);
        
        auto json_res = query_result_to_json(res);
        return crow::response(json_res);
    });

    app.port(18080).multithreaded().run();
}
