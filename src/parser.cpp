#include "parser.h"
#include "display.h"
#include "delete.h"
#include "file_handler.h"
#include "insert.h"
#include "tuple_serializer.h"
#include "update.h"
#include "query_result.h"
#include "transaction_manager.h"
#include "lock_manager.h"
#include "auth.h"
#include "recovery_manager.h"
#include "alter.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <cctype>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdint>

using namespace std;
namespace fs = std::filesystem;

namespace {

// What: automatic statement-level lock wrapper.
// Why: SELECT needs shared access, writes need exclusive access, and early
// parser returns should not leave locks stuck.
class StatementLockGuard {
  public:
    StatementLockGuard() : mode_(0), locked_(false) {}

    bool lock_for_read() {
        if (TransactionManager::in_transaction()) {
            return true;
        }
        LockManager::lock_shared();
        mode_ = 1;
        locked_ = true;
        return true;
    }

    bool lock_for_write() {
        if (TransactionManager::in_transaction()) {
            if (!TransactionManager::current_transaction_holds_write_lock()) {
                return false;
            }
            return true;
        }
        LockManager::lock_exclusive();
        mode_ = 2;
        locked_ = true;
        return true;
    }

    ~StatementLockGuard() {
        if (!locked_) return;
        if (mode_ == 1) {
            LockManager::unlock_shared();
        } else if (mode_ == 2) {
            LockManager::unlock_exclusive();
        }
    }

  private:
    int mode_;
    bool locked_;
};

}  // namespace

// What: create.cpp function that performs actual table creation.
// Example: parser sends table_name="students" and columns=[("id","INT"), ...].
extern void execute_create_query(string table_name, vector<pair<string, string>> cols);
extern bool execute_alter_add_column(const std::string& table_name,
                                     const std::string& column_name,
                                     const std::string& type_spec,
                                     bool has_default,
                                     const std::string& default_value);

// What: insert.cpp function that writes validated row values to storage.
// Example: parser sends table "students", schema, and values [1, "Aryan", "CSE"].
extern void insert_command(char tname[], const vector<TupleValue>& values, const vector<ColumnSchema>& schema);

// What: remove leading/trailing spaces.
// Why: user input often has extra spaces around keywords or values.
// Example: "  students  " becomes "students".
string trim_string(string s) {
    while (!s.empty() && isspace((unsigned char)s.front())) {
        s.erase(s.begin());
    }

    while (!s.empty() && isspace((unsigned char)s.back())) {
        s.pop_back();
    }

    return s;
}

// What: lowercase a small token.
// Why: SQL keywords should be case-insensitive.
// Example: "SeLeCt" becomes "select".
string to_lower_string(string s) {
    for (char &c : s) {
        c = static_cast<char>(tolower((unsigned char)c));
    }

    return s;
}

// What: strip quotes around a string literal.
// Why: storage should receive raw values, not SQL quote characters.
// Example: "\"Aryan\"" becomes "Aryan", and "'CSE'" becomes "CSE".
string remove_quotes(string s) {
    s = trim_string(s);

    if (s.size() >= 2) {
        if ((s.front() == '\'' && s.back() == '\'') ||
            (s.front() == '"' && s.back() == '"')) {
            return s.substr(1, s.size() - 2);
        }
    }

    return s;
}

// What: lowercase only text outside quotes.
// Why: keyword matching must not change VARCHAR data like "CSE" or "Aryan".
// Example: INSERT ... "Aryan" becomes insert ... "Aryan", not insert ... "aryan".
string keyword_lower_copy(string q) {
    bool in_single = false;
    bool in_double = false;
    for (int i = 0; i < (int)q.size(); i++) {
        if (q[i] == '\'' && !in_double) {
            in_single = !in_single;
        }
        else if (q[i] == '"' && !in_single) {
            in_double = !in_double;
        }
        else if (!in_single && !in_double) {
            q[i] = static_cast<char>(tolower((unsigned char)q[i]));
        }
    }

    return q;
}

// What: split comma-separated INSERT values.
// Why: commas inside quoted VARCHAR values should stay inside the value.
// Example: 1, "Aryan, Saini", "CSE" splits into 3 values, not 4.
vector<string> split_values(string s) {
    vector<string> values;
    string current = "";
    bool in_single = false;
    bool in_double = false;

    for (char c : s) {
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            current += c;
        }
        else if (c == '"' && !in_single) {
            in_double = !in_double;
            current += c;
        }
        else if (c == ',' && !in_single && !in_double) {
            values.push_back(trim_string(current));
            current = "";
        }
        else {
            current += c;
        }
    }

    if (!current.empty()) {
        values.push_back(trim_string(current));
    }

    return values;
}

void push_select_token(vector<string>& token_vector, string token) {
    token = trim_string(token);
    if (token.empty()) return;
    string lower = to_lower_string(token);
    if (lower == "select" || lower == "from" || lower == "where" ||
        lower == "join" || lower == "on" || lower == "left" || lower == "inner" ||
        lower == "having" ||
        lower == "group" || lower == "order" || lower == "by" || lower == "limit" ||
        lower == "asc" || lower == "desc") {
        token_vector.push_back(lower);
    }
    else {
        token_vector.push_back(remove_quotes(token));
    }
}

// What: tokenize SELECT / WHERE / JOIN query forms.
// Why: display.cpp receives a clean token vector and handles execution.
void tokenize_select(char query[], QueryResult* res = nullptr) {
    vector<string> token_vector;
    string current_token = "";
    bool in_single = false;
    bool in_double = false;

    for (int i = 0; query[i] != '\0'; i++) {
        char c = query[i];

        if (c == '\'' && !in_double) {
            in_single = !in_single;
            current_token += c;
        }
        else if (c == '"' && !in_single) {
            in_double = !in_double;
            current_token += c;
        }
        else if (!in_single && !in_double &&
                 (c == ' ' || c == ',' || c == ';' || c == '\n')) {
            push_select_token(token_vector, current_token);
            current_token = "";
        }
        
        else {
            if (c != '\n') {
                current_token += c;
            }
        }
    }

    push_select_token(token_vector, current_token);

    process_select(token_vector, res);
}

// What: parse CREATE TABLE into table name and column definitions.
// Why: create.cpp should receive structured schema input.
void tokenize_create(char query[]) {
    string q(query);
    string q_lower = keyword_lower_copy(q);

    size_t start = q.find('(');
    size_t end = q.rfind(')');

    if (start == string::npos || end == string::npos || end <= start) {
        cout << "Syntax Error: Invalid CREATE TABLE format. Missing parentheses.\n";
        return;
    }

    string before_paren = trim_string(q.substr(0, start));
    stringstream ss(before_paren);

    vector<string> words;
    string word;

    while (ss >> word) {
        words.push_back(word);
    }

    if (words.size() != 3 ||
        to_lower_string(words[0]) != "create" ||
        to_lower_string(words[1]) != "table") {
        cout << "Syntax Error: Use CREATE TABLE table_name (...);\n";
        return;
    }

    string table_name = words[2];

    string cols_str = q.substr(start + 1, end - start - 1);
    vector<pair<string, string>> columns;

    stringstream col_stream(cols_str);
    string col_def;

    while (getline(col_stream, col_def, ',')) {
        stringstream def_stream(trim_string(col_def));
        string col_name, col_type;

        if (def_stream >> col_name >> col_type) {
            columns.push_back({col_name, col_type});
        }
    }

    execute_create_query(table_name, columns);
}

// What: parse INSERT and convert input strings into typed TupleValue objects.
// Why: insert.cpp should receive validated schema + typed values.
void tokenize_insert(char query[]) {
    string q(query);
    string q_lower = keyword_lower_copy(q);

    size_t values_pos = q_lower.find(" values ");
    if (values_pos == string::npos) {
        cout << "Syntax Error: Invalid INSERT statement. Missing VALUES keyword.\n";
        return;
    }

    string before_values = trim_string(q.substr(0, values_pos));
    stringstream ss(before_values);

    string insert_word, into_word, table_name;
    ss >> insert_word >> into_word >> table_name;

    if (to_lower_string(insert_word) != "insert" ||
        to_lower_string(into_word) != "into" ||
        table_name.empty()) {
        cout << "Syntax Error: Use INSERT INTO table_name VALUES (...);\n";
        return;
    }

    size_t open_pos = q.find('(', values_pos);
    size_t close_pos = q.rfind(')');

    if (open_pos == string::npos || close_pos == string::npos || close_pos <= open_pos) {
        cout << "Syntax Error: INSERT values must be inside parentheses.\n";
        return;
    }

    string values_str = q.substr(open_pos + 1, close_pos - open_pos - 1);
    vector<string> raw_values = split_values(values_str);

    table* meta = fetch_meta_data(table_name);
    if (meta == NULL) {
        cout << "ERROR: Table '" << table_name << "' does not exist or metadata could not be loaded.\n";
        return;
    }

    if ((int)raw_values.size() != meta->count) {
        cout << "ERROR: Column count mismatch.\n";
        cout << "Expected " << meta->count << " values, received " << raw_values.size() << ".\n";
        delete meta;
        return;
    }

    vector<ColumnSchema> schema;
    vector<TupleValue> values;

    for (int i = 0; i < meta->count; i++) {
        ColumnSchema col_schema(
            meta->col[i].col_name,
            meta->col[i].type == INT ? STORAGE_COLUMN_INT : STORAGE_COLUMN_VARCHAR,
            meta->col[i].size
        );

        schema.push_back(col_schema);

        string value = remove_quotes(raw_values[i]);

        if (meta->col[i].type == INT) {
            try {
                size_t used = 0;
                int number = stoi(value, &used);

                if (used != value.size()) {
                    cout << "ERROR: Invalid INT value for column '" << meta->col[i].col_name << "'.\n";
                    delete meta;
                    return;
                }

                values.push_back(TupleValue::FromInt(number));
            } catch (...) {
                cout << "ERROR: Invalid INT value for column '" << meta->col[i].col_name << "'.\n";
                delete meta;
                return;
            }
        }
        else if (meta->col[i].type == VARCHAR) {
            if ((int)value.size() > meta->col[i].size) {
                cout << "ERROR: Value too long for column '" << meta->col[i].col_name << "'.\n";
                cout << "Max allowed size: " << meta->col[i].size << "\n";
                delete meta;
                return;
            }

            values.push_back(TupleValue::FromVarchar(value));
        }
    }

    char tname[MAX_NAME];
    strncpy(tname, table_name.c_str(), MAX_NAME - 1);
    tname[MAX_NAME - 1] = '\0';

    insert_command(tname, values, schema);

    delete meta;
}

// What: parse SHOW TABLES.
// Why: expose the table registry/catalog through a SQL-like command.
void tokenize_show(char query[]) {
    string q(query);

    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    q = trim_string(q);
    string q_lower = to_lower_string(q);

    if (q_lower == "show tables") {
        show_tables();
    } else if (q_lower == "show databases") {
        std::vector<std::string> dbs = list_databases();
        if (dbs.empty()) {
            cout << "No databases found.\n";
            return;
        }

        cout << "Databases\n";
        cout << "--------------------------------------------------\n";
        for (std::size_t i = 0; i < dbs.size(); ++i) {
            cout << dbs[i];
            if (dbs[i] == active_database()) {
                cout << "  <- active";
            }
            cout << "\n";
        }
    } else if (q_lower == "show users") {
        std::vector<std::string> users = AuthManager::list_users();
        if (users.empty()) {
            cout << "No users found.\n";
            return;
        }

        cout << "Users\n";
        cout << "--------------------------------------------------\n";
        for (std::size_t i = 0; i < users.size(); ++i) {
            cout << users[i] << "\n";
        }
    } else {
        cout << "Syntax Error: Supported SHOW syntax is:\n";
        cout << "SHOW TABLES;\n";
        cout << "SHOW DATABASES;\n";
        cout << "SHOW USERS;\n";
    }
}

bool parse_create_user_statement(const std::string& q, std::string& username, std::string& password) {
    std::string lowered = keyword_lower_copy(q);
    const std::string prefix = "create user ";
    if (lowered.rfind(prefix, 0) != 0) {
        return false;
    }

    std::size_t identified_pos = lowered.find(" identified by ");
    if (identified_pos == std::string::npos) {
        return false;
    }

    username = trim_string(q.substr(prefix.size(), identified_pos - prefix.size()));
    password = remove_quotes(trim_string(q.substr(identified_pos + 15)));
    return !username.empty() && !password.empty();
}

void tokenize_create_user(char query[]) {
    std::string q(query);
    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    std::string username;
    std::string password;
    if (!parse_create_user_statement(q, username, password)) {
        cout << "Syntax Error: Use CREATE USER username IDENTIFIED BY \"password\";\n";
        return;
    }

    if (AuthManager::user_exists(username)) {
        cout << "ERROR: User '" << username << "' already exists.\n";
        return;
    }

    if (!AuthManager::register_user(username, password)) {
        cout << "ERROR: Failed to create user '" << username << "'.\n";
        return;
    }

    cout << "Success: User '" << username << "' created successfully.\n";
}

void tokenize_create_database(char query[]) {
    std::string q(query);
    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    std::stringstream ss(q);
    std::string create_word, database_word, db_name, extra;
    ss >> create_word >> database_word >> db_name >> extra;

    if (to_lower_string(create_word) != "create" ||
        to_lower_string(database_word) != "database" ||
        db_name.empty() || !extra.empty()) {
        cout << "Syntax Error: Use CREATE DATABASE db_name;\n";
        return;
    }

    if (database_exists(db_name)) {
        cout << "ERROR: Database '" << db_name << "' already exists.\n";
        return;
    }

    if (!create_database_namespace(db_name)) {
        cout << "ERROR: Could not create database '" << db_name << "'.\n";
        return;
    }

    cout << "Success: Database '" << db_name << "' created successfully.\n";
}

void tokenize_use_database(char query[]) {
    std::string q(query);
    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    std::stringstream ss(q);
    std::string use_word, db_name, extra;
    ss >> use_word >> db_name >> extra;

    if (to_lower_string(use_word) != "use" || db_name.empty() || !extra.empty()) {
        cout << "Syntax Error: Use USE db_name;\n";
        return;
    }

    if (!database_exists(db_name)) {
        cout << "ERROR: Database '" << db_name << "' does not exist.\n";
        return;
    }

    set_active_database(db_name);
    system_check();
    RecoveryManager::recover_all_tables();
    cout << "Switched to database '" << db_name << "'.\n";
}

// What: parse DROP TABLE and remove table directory plus registry entry.
// Why: DDL deletion must clean both physical files and table_list.
void tokenize_drop(char query[]) {
    string q(query);

    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    q = trim_string(q);

    stringstream ss(q);
    string drop_word, table_word, table_name, extra;
    ss >> drop_word >> table_word >> table_name >> extra;

    std::string object_word = to_lower_string(table_word);
    if (to_lower_string(drop_word) != "drop" ||
        (object_word != "table" && object_word != "user" && object_word != "database") ||
        table_name.empty() ||
        !extra.empty()) {
        cout << "Syntax Error: Use DROP TABLE table_name; DROP USER username; or DROP DATABASE db_name;\n";
        return;
    }

    if (object_word == "database") {
        if (!database_exists(table_name)) {
            cout << "ERROR: Database '" << table_name << "' does not exist.\n";
            return;
        }
        if (table_name == active_database()) {
            cout << "ERROR: Cannot drop the currently active database. USE another database first.\n";
            return;
        }
        if (!drop_database_namespace(table_name)) {
            cout << "ERROR: Could not drop database '" << table_name << "'.\n";
            return;
        }
        cout << "Success: Database '" << table_name << "' dropped successfully.\n";
        return;
    }

    if (object_word == "user") {
        if (!AuthManager::user_exists(table_name)) {
            cout << "ERROR: User '" << table_name << "' does not exist.\n";
            return;
        }

        if (!AuthManager::drop_user(table_name)) {
            cout << "ERROR: Could not drop user '" << table_name << "'.\n";
            return;
        }

        cout << "Success: User '" << table_name << "' dropped successfully.\n";
        return;
    }

    char tab[MAX_NAME];
    strncpy(tab, table_name.c_str(), MAX_NAME - 1);
    tab[MAX_NAME - 1] = '\0';

    if (search_table(tab) == 0) {
        cout << "ERROR: Table '" << table_name << "' does not exist.\n";
        return;
    }

    string table_path = table_dir_path(table_name).string();

    try {
        if (fs::exists(table_path)) {
            fs::remove_all(table_path);
        }
    } catch (...) {
        cout << "ERROR: Could not remove table directory for '" << table_name << "'.\n";
        return;
    }

    const fs::path list_path = table_list_path();
    const fs::path tmp_path = list_path.string() + ".tmp";
    ifstream in(list_path);
    ofstream out(tmp_path);

    if (!in.is_open() || !out.is_open()) {
        cout << "ERROR: Could not update table registry.\n";
        return;
    }

    string name;
    while (in >> name) {
        if (name != table_name) {
            out << name << "\n";
        }
    }

    in.close();
    out.close();

    remove(list_path.string().c_str());
    rename(tmp_path.string().c_str(), list_path.string().c_str());

    cout << "Success: Table '" << table_name << "' dropped successfully.\n";
}

// What: parse UPDATE ... SET ... WHERE ... into UpdateStatement.
// Why: update.cpp can then choose indexed or scan-based update.
void tokenize_update(char query[]) {
    string q(query);

    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    string q_lower = keyword_lower_copy(q);
    size_t set_pos = q_lower.find(" set ");
    size_t where_pos = q_lower.find(" where ");

    if (set_pos == string::npos) {
        cout << "Syntax Error: Missing SET keyword.\n";
        return;
    }
    if (where_pos == string::npos || where_pos <= set_pos) {
        cout << "Syntax Error: Missing WHERE keyword.\n";
        return;
    }

    string before_set = trim_string(q.substr(0, set_pos));
    stringstream ss_tbl(before_set);
    string update_kw, table_name;
    ss_tbl >> update_kw >> table_name;
    if (to_lower_string(update_kw) != "update" || table_name.empty()) {
        cout << "Syntax Error: Use UPDATE table SET col = val WHERE ...\n";
        return;
    }

    string set_clause = trim_string(q.substr(set_pos + 5, where_pos - set_pos - 5));
    size_t eq_pos = set_clause.find('=');
    if (eq_pos == string::npos) {
        cout << "Syntax Error: SET clause must be col = value.\n";
        return;
    }

    string set_col = trim_string(set_clause.substr(0, eq_pos));
    string set_val = remove_quotes(trim_string(set_clause.substr(eq_pos + 1)));
    if (set_col.empty() || set_val.empty()) {
        cout << "Syntax Error: SET column or value is empty.\n";
        return;
    }

    string where_clause = trim_string(q.substr(where_pos + 7));
    size_t where_eq = where_clause.find('=');
    if (where_eq == string::npos) {
        cout << "Syntax Error: WHERE clause must be col = value.\n";
        return;
    }

    string where_col = trim_string(where_clause.substr(0, where_eq));
    string where_val = remove_quotes(trim_string(where_clause.substr(where_eq + 1)));
    if (where_col.empty() || where_val.empty()) {
        cout << "Syntax Error: WHERE column or value is empty.\n";
        return;
    }

    UpdateStatement stmt;
    stmt.table_name = table_name;
    stmt.set_column = set_col;
    stmt.set_value = set_val;
    stmt.where_column = where_col;
    stmt.where_value = where_val;

    execute_update(stmt);
}

void tokenize_alter(char query[]) {
    std::string q(query);
    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    std::string q_lower = keyword_lower_copy(q);
    const std::string prefix = "alter table ";
    if (q_lower.rfind(prefix, 0) != 0) {
        std::cout << "Syntax Error: Use ALTER TABLE table_name ADD COLUMN col TYPE [DEFAULT value];\n";
        return;
    }

    std::size_t add_col_pos = q_lower.find(" add column ");
    if (add_col_pos == std::string::npos) {
        std::cout << "Syntax Error: Currently only ALTER TABLE ... ADD COLUMN ... is supported.\n";
        return;
    }

    std::string table_name = trim_string(q.substr(prefix.size(), add_col_pos - prefix.size()));
    std::string remainder = trim_string(q.substr(add_col_pos + 12));
    if (table_name.empty() || remainder.empty()) {
        std::cout << "Syntax Error: Incomplete ALTER TABLE statement.\n";
        return;
    }

    std::stringstream ss(remainder);
    std::string column_name;
    ss >> column_name;
    if (column_name.empty()) {
        std::cout << "Syntax Error: Missing column name in ALTER TABLE.\n";
        return;
    }

    std::string after_column = trim_string(remainder.substr(column_name.size()));
    if (after_column.empty()) {
        std::cout << "Syntax Error: Missing type in ALTER TABLE ADD COLUMN.\n";
        return;
    }

    std::string default_value;
    bool has_default = false;
    std::string type_spec = after_column;
    std::size_t default_pos = keyword_lower_copy(after_column).find(" default ");
    if (default_pos != std::string::npos) {
        type_spec = trim_string(after_column.substr(0, default_pos));
        default_value = trim_string(after_column.substr(default_pos + 9));
        has_default = true;
    }

    if (type_spec.empty()) {
        std::cout << "Syntax Error: Missing type in ALTER TABLE ADD COLUMN.\n";
        return;
    }

    execute_alter_add_column(table_name, column_name, type_spec, has_default, default_value);
}

// What: parse DELETE FROM ... WHERE ... into DeleteStatement.
// Why: delete.cpp can then choose indexed or scan-based delete.
void tokenize_delete(char query[]) {
    string q(query);

    while (!q.empty() && (q.back() == ';' || q.back() == '\n' || q.back() == ' ')) {
        q.pop_back();
    }

    string q_lower = keyword_lower_copy(q);
    size_t from_pos = q_lower.find(" from ");
    size_t where_pos = q_lower.find(" where ");

    if (from_pos == string::npos || where_pos == string::npos || where_pos <= from_pos) {
        cout << "Syntax Error: Use DELETE FROM table WHERE column = value;\n";
        return;
    }

    string delete_word = trim_string(q.substr(0, from_pos));
    if (to_lower_string(delete_word) != "delete") {
        cout << "Syntax Error: Use DELETE FROM table WHERE column = value;\n";
        return;
    }

    string table_name = trim_string(q.substr(from_pos + 6, where_pos - from_pos - 6));
    if (table_name.empty()) {
        cout << "Syntax Error: Missing table name in DELETE statement.\n";
        return;
    }

    string where_clause = trim_string(q.substr(where_pos + 7));
    size_t eq_pos = where_clause.find('=');
    if (eq_pos == string::npos) {
        cout << "Syntax Error: WHERE clause must be column = value.\n";
        return;
    }

    string where_col = trim_string(where_clause.substr(0, eq_pos));
    string where_val = remove_quotes(trim_string(where_clause.substr(eq_pos + 1)));
    if (where_col.empty() || where_val.empty()) {
        cout << "Syntax Error: WHERE column or value is empty.\n";
        return;
    }

    DeleteStatement stmt;
    stmt.table_name = table_name;
    stmt.where_column = where_col;
    stmt.where_value = where_val;

    execute_delete(stmt);
}

void print_query_syntax_help() {
    cout << "\nSupported Query Syntax\n";
    cout << "--------------------------------------------------\n";
    cout << "SHOW TABLES;\n";
    cout << "SHOW DATABASES;\n";
    cout << "SHOW USERS;\n";
    cout << "CREATE DATABASE semester4;\n";
    cout << "CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));\n";
    cout << "CREATE USER kairo IDENTIFIED BY \"pass123\";\n";
    cout << "ALTER TABLE students ADD COLUMN age INT DEFAULT 18;\n";
    cout << "INSERT INTO students VALUES (1, \"Aditya\", \"CSE\");\n";
    cout << "SELECT * FROM students;\n";
    cout << "SELECT name, dept FROM students;\n";
    cout << "SELECT * FROM students WHERE id = 1;\n";
    cout << "SELECT * FROM students ORDER BY name;\n";
    cout << "SELECT * FROM students LIMIT 5;\n";
    cout << "SELECT COUNT(*) FROM students;\n";
    cout << "SELECT SUM(id), AVG(id), MIN(name), MAX(name) FROM students;\n";
    cout << "SELECT dept, COUNT(*) FROM students GROUP BY dept HAVING COUNT(*) > 1;\n";
    cout << "SELECT dept, name, COUNT(*) FROM students GROUP BY dept, name;\n";
    cout << "SELECT students.name, departments.hod FROM students JOIN departments ON students.dept = departments.code;\n";
    cout << "SELECT students.name, departments.hod FROM students LEFT JOIN departments ON students.dept = departments.code;\n";
    cout << "UPDATE students SET dept = ECE WHERE id = 1;\n";
    cout << "DELETE FROM students WHERE id = 1;\n";
    cout << "DELETE FROM students WHERE dept = CSE;\n";
    cout << "DROP TABLE students;\n";
    cout << "DROP DATABASE semester4;\n";
    cout << "DROP USER kairo;\n";
    cout << "USE semester4;\n";
    cout << "BEGIN;\n";
    cout << "COMMIT;\n";
    cout << "ROLLBACK;\n";
    cout << "--------------------------------------------------\n\n";
}

// What: central query router for CLI/API calls.
// Why: detect command type, apply read/write locks, then dispatch to the
// command-specific tokenizer/executor.
void execute_query_string(string input_query, QueryResult* res) {
    input_query = trim_string(input_query);

    if (input_query.empty()) {
        if (res) {
            res->success = false;
            res->message = "Error: Empty query.";
        }
        cout << "Error: Empty query.\n";
        return;
    }

    stringstream ss(input_query);
    string first_word;
    ss >> first_word;

    string token_temp = to_lower_string(first_word);

    char final_query[1024];
    strncpy(final_query, input_query.c_str(), sizeof(final_query) - 1);
    final_query[sizeof(final_query) - 1] = '\0';

    if (token_temp == "select") {
        // SELECT is read-only, so it uses a shared statement lock.
        StatementLockGuard guard;
        guard.lock_for_read();
        tokenize_select(final_query, res);
    }
    else if (token_temp == "begin") {
        if (TransactionManager::begin()) {
            cout << "Transaction started.\n";
            if (res) res->message = "Transaction started.";
        } else {
            cout << "Transaction error: " << TransactionManager::last_error() << "\n";
            if (res) {
                res->success = false;
                res->message = TransactionManager::last_error();
            }
        }
    }
    else if (token_temp == "commit") {
        if (TransactionManager::commit()) {
            cout << "Transaction committed.\n";
            if (res) res->message = "Transaction committed.";
        } else {
            cout << "Transaction error: " << TransactionManager::last_error() << "\n";
            if (res) {
                res->success = false;
                res->message = TransactionManager::last_error();
            }
        }
    }
    else if (token_temp == "rollback") {
        if (TransactionManager::rollback()) {
            cout << "Transaction rolled back.\n";
            if (res) res->message = "Transaction rolled back.";
        } else {
            cout << "Transaction error: " << TransactionManager::last_error() << "\n";
            if (res) {
                res->success = false;
                res->message = TransactionManager::last_error();
            }
        }
    }
    else if (token_temp == "create") {
        // DDL/DML writes need exclusive access unless a transaction already owns it.
        StatementLockGuard guard;
        if (!guard.lock_for_write()) {
            cout << "Concurrency error: active transaction does not hold write lock.\n";
            if (res) {
                res->success = false;
                res->message = "Concurrency error: active transaction does not hold write lock.";
            }
            return;
        }
        std::string lowered = keyword_lower_copy(input_query);
        if (lowered.rfind("create user ", 0) == 0) {
            tokenize_create_user(final_query);
            if (res) res->message = "User created successfully";
        } else if (lowered.rfind("create database ", 0) == 0) {
            tokenize_create_database(final_query);
            if (res) res->message = "Database created successfully";
        } else {
            tokenize_create(final_query);
            if (res) res->message = "Table created successfully";
        }
    }
    else if (token_temp == "alter") {
        StatementLockGuard guard;
        if (!guard.lock_for_write()) {
            cout << "Concurrency error: active transaction does not hold write lock.\n";
            if (res) {
                res->success = false;
                res->message = "Concurrency error: active transaction does not hold write lock.";
            }
            return;
        }
        tokenize_alter(final_query);
        if (res) res->message = "ALTER TABLE executed successfully";
    }
    else if (token_temp == "use") {
        StatementLockGuard guard;
        guard.lock_for_read();
        tokenize_use_database(final_query);
        if (res) res->message = "Database switched successfully";
    }
    else if (token_temp == "insert") {
        // INSERT changes data pages, WAL, and B+ Tree entries.
        StatementLockGuard guard;
        if (!guard.lock_for_write()) {
            cout << "Concurrency error: active transaction does not hold write lock.\n";
            if (res) {
                res->success = false;
                res->message = "Concurrency error: active transaction does not hold write lock.";
            }
            return;
        }
        tokenize_insert(final_query);
        if (res) res->message = "Row inserted successfully";
    }
    else if (token_temp == "show") {
        // SHOW reads catalog/table registry state.
        StatementLockGuard guard;
        guard.lock_for_read();
        tokenize_show(final_query);
        if (res) res->message = "Show command executed successfully";
    }
    else if (token_temp == "drop") {
        // DROP removes table files and updates table registry.
        StatementLockGuard guard;
        if (!guard.lock_for_write()) {
            cout << "Concurrency error: active transaction does not hold write lock.\n";
            if (res) {
                res->success = false;
                res->message = "Concurrency error: active transaction does not hold write lock.";
            }
            return;
        }
        tokenize_drop(final_query);
        if (res) res->message = "Drop command executed successfully";
    }
    else if (token_temp == "update") {
        // UPDATE keeps a shared catalog lock and lets the executor take
        // exclusive RID-level locks for rows it actually modifies.
        StatementLockGuard guard;
        guard.lock_for_read();
        tokenize_update(final_query);
        if (res) res->message = "Table updated successfully";
    }
    else if (token_temp == "delete") {
        // DELETE keeps a shared catalog lock and lets the executor take
        // exclusive RID-level locks for rows it actually removes.
        StatementLockGuard guard;
        guard.lock_for_read();
        tokenize_delete(final_query);
        if (res) res->message = "Row(s) deleted successfully";
    }
    else {
        if (res) {
            res->success = false;
            res->message = "Error: Wrong syntax or unsupported command.";
        }
        cout << "\nError: Wrong syntax or unsupported command.\n";
        print_query_syntax_help();
    }
}

/*
 * Parser study notes:
 *
 * This file is the Parser + Router layer. It does not store rows by itself.
 * It reads raw SQL-like text, extracts structured meaning from it, takes the
 * correct statement lock, and then calls the actual executor file.
 *
 * Example query:
 * INSERT INTO students VALUES (1, "Aryan, Saini", "CSE");
 *
 * 1. split_values(string s)
 *
 * Use:
 * Splits values inside VALUES (...).
 *
 * Why:
 * Normal comma splitting would break "Aryan, Saini" wrongly because that comma
 * is inside quotes and belongs to the VARCHAR value.
 *
 * Input:
 * 1, "Aryan, Saini", "CSE"
 *
 * Correct output:
 * ["1", "\"Aryan, Saini\"", "\"CSE\""]
 *
 * Meaning:
 * The comma inside quotes is part of the name, not a separator.
 *
 * ------------------------------------------------------------
 *
 * 2. push_select_token(...)
 *
 * Use:
 * Prepares tokens for SELECT.
 *
 * Example:
 * SELECT students.name FROM students;
 *
 * It lowercases SQL keywords:
 * select, from, where, join, on
 *
 * But keeps table/column/value names usable:
 * students.name
 *
 * Why:
 * SQL keywords should be case-insensitive, but table/column names may be
 * case-sensitive in this project.
 *
 * ------------------------------------------------------------
 *
 * 3. tokenize_select(...)
 *
 * Use:
 * Breaks a SELECT query into tokens and sends it to process_select().
 *
 * Example:
 * SELECT name, dept FROM students WHERE id = 1;
 *
 * Token vector becomes roughly:
 * ["select", "name", "dept", "from", "students", "where", "id", "=", "1"]
 *
 * Then process_select() decides:
 * - normal SELECT
 * - SELECT WHERE
 * - JOIN
 * - LEFT JOIN
 *
 * ------------------------------------------------------------
 *
 * 4. tokenize_create(...)
 *
 * Use:
 * Parses table name and column definitions.
 *
 * Example:
 * CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));
 *
 * It extracts:
 * table_name = students
 * columns = [(id, INT), (name, VARCHAR(50)), (dept, VARCHAR(20))]
 *
 * Then calls:
 * execute_create_query(table_name, columns);
 *
 * That function in create.cpp creates metadata, folder, table_list entry, and
 * the B+ Tree index file.
 *
 * ------------------------------------------------------------
 *
 * 5. tokenize_insert(...)
 *
 * Use:
 * Parses INSERT, validates values using table metadata, and converts strings
 * into typed TupleValue objects.
 *
 * Example:
 * INSERT INTO students VALUES (1, "Aryan", "CSE");
 *
 * Flow:
 * table_name = students
 * raw_values = ["1", "Aryan", "CSE"]
 * fetch metadata of students
 * id is INT, so "1" -> TupleValue::FromInt(1)
 * name is VARCHAR, so "Aryan" -> TupleValue::FromVarchar("Aryan")
 * dept is VARCHAR, so "CSE" -> TupleValue::FromVarchar("CSE")
 *
 * Then calls:
 * insert_command(tname, values, schema);
 *
 * That goes to insert.cpp, where tuple is serialized into bytes, stored inside
 * a page, WAL is written, and the B+ Tree is updated.
 *
 * ------------------------------------------------------------
 *
 * 6. tokenize_show(...)
 *
 * Use:
 * Handles only:
 * SHOW TABLES;
 *
 * It calls:
 * show_tables();
 *
 * That reads table/table_list and prints all table names.
 *
 * ------------------------------------------------------------
 *
 * 7. tokenize_drop(...)
 *
 * Use:
 * Deletes table directory and removes table name from registry.
 *
 * Example:
 * DROP TABLE students;
 *
 * Flow:
 * check table exists
 * delete ./table/students/
 * remove students from ./table/table_list
 *
 * This is DDL deletion.
 *
 * ------------------------------------------------------------
 *
 * 8. tokenize_update(...)
 *
 * Use:
 * Parses UPDATE query into structured UpdateStatement.
 *
 * Example:
 * UPDATE students SET dept = ECE WHERE id = 1;
 *
 * It extracts:
 * table_name = students
 * set_column = dept
 * set_value = ECE
 * where_column = id
 * where_value = 1
 *
 * Then calls:
 * execute_update(stmt);
 *
 * In update.cpp:
 * if WHERE is primary key -> use B+ Tree
 * else -> linear scan
 *
 * ------------------------------------------------------------
 *
 * 9. tokenize_delete(...)
 *
 * Use:
 * Parses DELETE query into DeleteStatement.
 *
 * Example:
 * DELETE FROM students WHERE dept = CSE;
 *
 * It extracts:
 * table_name = students
 * where_column = dept
 * where_value = CSE
 *
 * Then calls:
 * execute_delete(stmt);
 *
 * In delete.cpp:
 * if WHERE is primary key -> B+ Tree lookup
 * else -> scan all rows
 *
 * ------------------------------------------------------------
 *
 * 10. execute_query_string(...)
 *
 * This is the central router.
 * Every query first comes here.
 *
 * Example:
 * INSERT INTO students VALUES (1, "Aryan", "CSE");
 *
 * It reads first word:
 * insert
 *
 * Then dispatches:
 * tokenize_insert(final_query);
 *
 * For each command:
 * select   -> read lock  -> tokenize_select
 * create   -> write lock -> tokenize_create
 * insert   -> write lock -> tokenize_insert
 * show     -> read lock  -> tokenize_show
 * drop     -> write lock -> tokenize_drop
 * update   -> write lock -> tokenize_update
 * delete   -> write lock -> tokenize_delete
 * begin    -> TransactionManager::begin()
 * commit   -> TransactionManager::commit()
 * rollback -> TransactionManager::rollback()
 *
 * Simple view:
 * Raw SQL
 *   -> execute_query_string()
 *   -> identify first keyword
 *   -> take read/write lock
 *   -> call tokenizer
 *   -> tokenizer validates + structures data
 *   -> executor file performs actual DB work
 *
 * Example full dry run:
 * SELECT * FROM students WHERE id = 1;
 *
 * Flow:
 * execute_query_string()
 * first word = select
 * read lock taken
 * tokenize_select()
 * tokens created
 * process_select()
 * WHERE detected
 * execute_select_where()
 * WHERE column is primary key
 * B+ Tree search gives RID
 * BufferPool fetches page
 * DataPage reads slot
 * TupleSerializer converts bytes to values
 * print result table
 */
