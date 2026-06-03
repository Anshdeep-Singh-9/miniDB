#include "file_handler.h"
namespace fs = std::filesystem;

namespace {
std::string g_active_user;
std::string g_active_database;

fs::path resolve_project_root() {
    // What: find the repository root even when binary runs from build/bin or Docker.
    // Why: table/ and system/ paths should work from different launch directories.
    // Example: running ./build/bin/miniDB still resolves storage under the project root.
    std::error_code ec;

    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        if (fs::exists(cwd / "table") || fs::exists(cwd / "system") || fs::exists(cwd / "src")) {
            return cwd;
        }

        if (cwd.filename() == "bin" && cwd.parent_path().filename() == "build") {
            return cwd.parent_path().parent_path();
        }
    }

    fs::path exe_path = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        fs::path exe_dir = exe_path.parent_path();
        if (exe_dir.filename() == "bin" && exe_dir.parent_path().filename() == "build") {
            return exe_dir.parent_path().parent_path();
        }
        return exe_dir;
    }

    return fs::path(".");
}

std::string sanitize_user_component(const std::string& username) {
    std::string cleaned;
    for (std::size_t i = 0; i < username.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(username[i]);
        if (std::isalnum(ch) || username[i] == '_' || username[i] == '-') {
            cleaned.push_back(username[i]);
        } else {
            cleaned.push_back('_');
        }
    }
    return cleaned;
}

}

fs::path project_root() {
    return resolve_project_root();
}

fs::path user_root() {
    if (g_active_user.empty()) {
        return project_root();
    }
    return project_root() / "users" / sanitize_user_component(g_active_user);
}

fs::path databases_root() {
    if (g_active_user.empty()) {
        return project_root();
    }
    return user_root() / "databases";
}

fs::path database_root() {
    if (g_active_user.empty()) {
        return project_root();
    }
    if (g_active_database.empty()) {
        return databases_root() / "default";
    }
    return databases_root() / sanitize_user_component(g_active_database);
}

fs::path table_root() {
    if (!g_active_user.empty()) {
        return database_root() / "table";
    }
    return project_root() / "table";
}

fs::path system_root() {
    if (!g_active_user.empty()) {
        return database_root() / "system";
    }
    return project_root() / "system";
}

fs::path global_system_root() {
    return project_root() / "system";
}

fs::path table_dir_path(const std::string& table_name) {
    return table_root() / table_name;
}

fs::path table_data_path(const std::string& table_name) {
    return table_dir_path(table_name) / "data.dat";
}

fs::path table_index_path(const std::string& table_name) {
    return table_dir_path(table_name) / "index.dat";
}

fs::path table_wal_path(const std::string& table_name) {
    return table_dir_path(table_name) / "wal.log";
}

fs::path table_list_path() {
    return table_root() / "table_list";
}

fs::path database_list_path() {
    if (g_active_user.empty()) {
        return project_root() / "database_list";
    }
    return user_root() / "database_list";
}

fs::path txn_snapshot_root() {
    return system_root() / "txn_snapshot";
}

void set_active_user(const std::string& username) {
    g_active_user = username;
}

std::string active_user() {
    return g_active_user;
}

void set_active_database(const std::string& db_name) {
    g_active_database = db_name;
}

std::string active_database() {
    return g_active_database;
}

bool database_exists(const std::string& db_name) {
    if (db_name.empty()) return false;
    if (g_active_user.empty()) return false;
    return fs::exists(databases_root() / sanitize_user_component(db_name));
}

std::vector<std::string> list_databases() {
    std::vector<std::string> names;
    if (g_active_user.empty()) return names;

    const fs::path list_path = database_list_path();
    std::ifstream in(list_path);
    std::string name;
    while (in >> name) {
        names.push_back(name);
    }
    return names;
}

bool create_database_namespace(const std::string& db_name) {
    if (db_name.empty() || g_active_user.empty()) return false;
    const std::string cleaned = sanitize_user_component(db_name);
    const fs::path db_root = databases_root() / cleaned;
    std::error_code ec;
    fs::create_directories(db_root / "table", ec);
    if (ec) return false;
    fs::create_directories(db_root / "system", ec);
    if (ec) return false;

    const fs::path table_list = db_root / "table" / "table_list";
    if (!fs::exists(table_list)) {
        std::ofstream out(table_list);
        if (!out.is_open()) return false;
    }

    const fs::path db_list = database_list_path();
    fs::create_directories(db_list.parent_path(), ec);
    if (ec) return false;

    if (!database_exists(cleaned)) {
        std::ofstream out(db_list, std::ios::app);
        if (!out.is_open()) return false;
        out << cleaned << "\n";
    }

    return true;
}

bool drop_database_namespace(const std::string& db_name) {
    if (db_name.empty() || g_active_user.empty()) return false;
    const std::string cleaned = sanitize_user_component(db_name);
    const fs::path db_root = databases_root() / cleaned;
    if (!fs::exists(db_root)) return false;

    std::error_code ec;
    fs::remove_all(db_root, ec);
    if (ec) return false;

    const fs::path db_list = database_list_path();
    const fs::path tmp_list = db_list.string() + ".tmp";
    std::ifstream in(db_list);
    std::ofstream out(tmp_list);
    if (in.is_open() && out.is_open()) {
        std::string name;
        while (in >> name) {
            if (name != cleaned) {
                out << name << "\n";
            }
        }
    }
    in.close();
    out.close();
    if (fs::exists(tmp_list)) {
        fs::rename(tmp_list, db_list, ec);
        if (ec) return false;
    }
    return true;
}

bool ensure_default_database_for_active_user() {
    if (g_active_user.empty()) return true;
    if (!create_database_namespace("default")) return false;
    if (g_active_database.empty()) {
        g_active_database = "default";
    }
    return true;
}

// --- FSTREAM FUNCTIONS ---
fstream_t open_file_fstream(const char* t_name ,std::ios::openmode mode){
    // What: open or create a normal table metadata file using C++ fstream.
    // Why: CREATE and metadata fetch need a stable table/<name>/met catalog file.
    // Example: table/students/met stores the serialized table struct.
    fs::path file_path = table_root();

    if(!fs::exists(file_path)) fs::create_directories(file_path);
    file_path = file_path / t_name ;
    if(!fs::exists(file_path)) fs::create_directory(file_path);
    file_path = file_path / "met";

    if(!fs::exists(file_path)){
        fstream_t newfile(file_path, std::ios::out | std::ios::binary); 
        newfile.close();
    }

    fstream_t file(file_path, mode);
    if(!file){
        cout << "Error opening file!" << endl;
        exit(0);
    }
    return file;
}

fstream_t open_file_read_fstream(const char* t_name ,std::ios::openmode mode){
    fs::path file_path = table_root() / t_name / "met";
    
    if(!fs::exists(file_path)){
        return fstream_t(); 
    }

    fstream_t file(file_path, mode);
    return file;
}


// --- FILEPTR FUNCTIONS ---
FilePtr open_file(char* t_name , const char* perm){
    // What: legacy C FILE* helper for opening table metadata.
    // Why: older code paths still use fopen/fread/fwrite, so this keeps compatibility.
    // Example: open_file("students", "w") creates/opens table/students/met.
    FilePtr fp;
    struct stat st = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    std::string path = (table_dir_path(t_name) / "met").string();
    char *name = (char *)malloc(path.size() + 1);
    strcpy(name, path.c_str());

    fs::create_directories(table_dir_path(t_name));
    
    fp = fopen(name, perm);
    if (!fp) printf("\nError in opening file\n");
    free(name);
    return fp;
}

FilePtr open_file_read(char* t_name , const char* perm){
    FilePtr fp;
    struct stat st = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    std::string path = (table_dir_path(t_name) / "met").string();
    char *name = (char *)malloc(path.size() + 1);
    strcpy(name, path.c_str());

    if (stat(name, &st) == -1) {
        free(name);
        return NULL; 
    }

    fp = fopen(name, perm);
    free(name);
    return fp;
}
// --- UTILITY FUNCTIONS ---
int store_meta_data(struct table *t_ptr)
{
    // What: persist one table's schema/catalog metadata.
    // Why: future INSERT/SELECT calls must reload column names, types, and sizes.
    // Example: CREATE TABLE students writes table/students/met here.
    // Fix: Explicitly pass the pointer to the first character to bypass array decay glitches
    FilePtr fp = open_file((t_ptr->name), "w"); 
    fwrite(t_ptr, sizeof(struct table), 1, fp);
    fclose(fp);
    return 0;
}

void store_meta_data_fstream(struct table *t_ptr) {
    // Fix: Explicit pointer passing here as well
    fstream_t fp = open_file_fstream((const char*)t_ptr->name, std::ios::out | std::ios::trunc | std::ios::binary);
    
    // Explicitly cast the struct to a char pointer for the write function
    fp.write(reinterpret_cast<char*>(t_ptr), sizeof(struct table));
    fp.close();
}

struct table* fetch_meta_data(string name){
    // What: read table metadata from disk into a table struct.
    // Why: query execution needs schema before it can parse values or deserialize tuple bytes.
    // Example: INSERT INTO students first fetches metadata for students.
    fstream_t fp = open_file_read_fstream(name.c_str(), READ_BIN); 

    if(!fp.is_open()) return NULL; 

    struct table* t = new table();
    fp.read(reinterpret_cast<char*>(t), sizeof(struct table));

    if(fp.fail() || fp.gcount() != (std::streamsize)sizeof(struct table)){
        cout << "ERROR! Failed to read metadata for table: " << name << endl;
        delete t;
        fp.close();
        return NULL;
    }
    fp.close();
    return t;
}

void system_check() {
    // What: create required MiniDB directories and table_list if missing.
    // Why: startup should not crash on a fresh clone with no table/ or system/ folder yet.
    // Example: first run creates table/, system/, and table/table_list.
    try {
        fs::path table_dir = table_root();
        fs::path system_dir = system_root();
        fs::path global_system_dir = global_system_root();
        fs::path table_list = table_list_path();
        fs::path db_list = database_list_path();

        if (!g_active_user.empty()) {
            fs::create_directories(databases_root());
            if (!fs::exists(db_list)) {
                ofstream file(db_list);
                file.close();
            }
            if (g_active_database.empty()) {
                ensure_default_database_for_active_user();
            }
        }

        if (!fs::exists(table_dir)) {
            fs::create_directories(table_dir);
        }

        if (!fs::exists(system_dir)) {
            fs::create_directories(system_dir);
        }

        if (!fs::exists(global_system_dir)) {
            fs::create_directories(global_system_dir);
        }

        if (!fs::exists(table_list)) {
            ofstream file(table_list);
            file.close();
        }
    } catch (...) {
        // Fallback if /proc/self/exe fails
        if (!fs::exists("./table")) mkdir("./table", 0775);
        if (!fs::exists("./system")) mkdir("./system", 0775);
        if (!fs::exists("./table/table_list")) {
            ofstream file("./table/table_list");
            file.close();
        }
    }
}

// --- SYSTEM FILE PTR FUNCTIONS ---
FilePtr open_system_file(const char* t_name, const char* perm) {
    fs::path dir = global_system_root() / t_name;
    fs::create_directories(dir);
    std::string path = (dir / "met").string();
    FilePtr fp = fopen(path.c_str(), perm);
    if (!fp) printf("\nError in opening system file: %s\n", path.c_str());
    return fp;
}

FilePtr open_system_file_read(const char* t_name, const char* perm) {
    std::string path = (global_system_root() / t_name / "met").string();
    struct stat st;
    if (stat(path.c_str(), &st) == -1) {
        return NULL; 
    }

    return fopen(path.c_str(), perm);
}

int store_system_meta_data(struct table *t_ptr) {
    FilePtr fp = open_system_file(t_ptr->name, "w"); 
    if (!fp) return -1;
    fwrite(t_ptr, sizeof(struct table), 1, fp);
    fclose(fp);
    return 0;
}

struct table* fetch_system_meta_data(string name) {
    FilePtr fp = open_system_file_read(name.c_str(), "r"); 
    if (!fp) return NULL;

    struct table* t = new table();
    if (fread(t, sizeof(struct table), 1, fp) != 1) {
        delete t;
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return t;
}
