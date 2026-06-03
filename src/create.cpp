#include "create.h"
#include "file_handler.h"
#include <cctype>
#include <sys/stat.h>
#include "BPtree.h"       // Required to create the initial index
#include <vector>
#include <utility>
#include <algorithm>

namespace {
std::string to_lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool parse_column_type(const std::string& input, int& type_out) {
    const std::string value = to_lower(input);

    if (value == "1" || value == "int" || value == "integer") {
        type_out = INT;
        return true;
    }

    if (value == "2" || value == "varchar" || value == "string" ||
        value == "text") {
        type_out = VARCHAR;
        return true;
    }

    return false;
}
}

int record_size(table *temp){
    // What: compute the maximum byte size of one logical row from table metadata.
    // Why: older metadata code needs this size to validate and describe fixed-width rows.
    // Example: INT + VARCHAR becomes 4 bytes + varchar capacity in the table schema.
    int size = 0;
    temp->prefix[0] = 0;
    for(int i = 0; i < temp->count; i++){
        switch(temp->col[i].type){
        case INT:
            temp->prefix[i+1] = sizeof(int) + temp->prefix[i];
            size += sizeof(int);
            break;
        case VARCHAR:
            temp->prefix[i+1] = sizeof(char)*(MAX_VARCHAR +1) + temp->prefix[i];
            size += (MAX_VARCHAR + 1);
            break;
        }
    }
    return size;
}


// --- NEW AUTOMATED CREATE IMPLEMENTATION ---

void execute_create_query(std::string table_name, std::vector<std::pair<std::string, std::string>> cols) {
    // What: create the table directory, metadata file, table_list entry, and empty B+ Tree index.
    // Why: every INSERT/SELECT later depends on this catalog information to know column names/types.
    // Example: CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));
    char t_name[MAX_NAME];
    strcpy(t_name, table_name.c_str());

    // 1. Validation
    if(search_table(t_name) == 1){
        std::cout << "ERROR: Table '" << table_name << "' already exists.\n";
        return;
    }

    if(cols.empty()) {
        std::cout << "ERROR: No columns defined in query.\n";
        return;
    }

    // 2. PRIMARY KEY CONSTRAINT CHECK
    std::string first_col_type = cols[0].second;
    // Strip out sizes like "(10)" if someone typed "INT(10)" for the PK
    size_t pk_paren_pos = first_col_type.find('(');
    if(pk_paren_pos != std::string::npos) first_col_type = first_col_type.substr(0, pk_paren_pos);
    
    std::transform(first_col_type.begin(), first_col_type.end(), first_col_type.begin(), ::toupper);
    
    if (first_col_type != "INT") {
        std::cout << "ERROR: Primary Key Constraint Failed! The first column (" 
                  << cols[0].first << ") must be of type INT.\n";
        return;
    }

    // 3. Initialize the Table Metadata
    table *temp = new table();
    temp->fp = NULL;
    temp->blockbuf = NULL;
    strcpy(temp->name, t_name);
    temp->count = cols.size();
    temp->rec_count = 0;
    temp->data_size = 0;

    std::unordered_set<std::string> create_col_set;

    // 4. Populate Columns and Extract Sizes
    for(size_t i = 0; i < cols.size(); i++) {
        std::string c_name = cols[i].first;
        std::string c_type_raw = cols[i].second;
        std::string c_type = c_type_raw;
        int parsed_size = 0;

        // NEW: Extract size if format is VARCHAR(50) or INT(10)
        size_t paren_start = c_type_raw.find('(');
        if (paren_start != std::string::npos) {
            size_t paren_end = c_type_raw.find(')', paren_start);
            if (paren_end != std::string::npos) {
                c_type = c_type_raw.substr(0, paren_start); // Gets "VARCHAR"
                std::string size_str = c_type_raw.substr(paren_start + 1, paren_end - paren_start - 1); // Gets "50"
                parsed_size = std::stoi(size_str);
            }
        }

        std::transform(c_type.begin(), c_type.end(), c_type.begin(), ::toupper);

        // Check for duplicate column names
        if(create_col_set.count(c_name) == 0){
            strcpy(temp->col[i].col_name, c_name.c_str());
            create_col_set.insert(c_name);
        } else {
            std::cout << "ERROR: Duplicate column name '" << c_name << "'.\n";
            delete temp;
            return;
        }

        // Map String types to Engine Internal Types and Sizes
        if(c_type == "INT") {
            temp->col[i].type = 1; 
            temp->col[i].size = sizeof(int); // Always lock to 4 bytes for C++ safety
        } else if(c_type == "VARCHAR") {
            temp->col[i].type = 2; 
            // Apply parsed size, or fallback to MAX_VARCHAR if user just typed "VARCHAR"
            temp->col[i].size = (parsed_size > 0) ? parsed_size : MAX_VARCHAR; 
        } else {
            std::cout << "ERROR: Unknown data type '" << c_type << "' for column '" << c_name << "'.\n";
            delete temp;
            return;
        }
    }

    // 5. File System Setup
    std::filesystem::create_directories(table_root());

    FilePtr fp = fopen(table_list_path().string().c_str(), "a+");
    if(fp == NULL){
        std::cout << "ERROR: Could not open central table_list file.\n";
        delete temp;
        return;
    }

    // 6. Save Metadata and Update Table List
    temp->size = record_size(temp);
    store_meta_data(temp);
    
    fseek(fp, 0, SEEK_END);
    fprintf(fp, "%s\n", t_name);
    fclose(fp);
    free(temp);

    // 7. INITIALIZE B+ TREE INDEX
    BPtree index(t_name);

    std::cout << "Success: Table '" << table_name << "' created with Primary Key '" << cols[0].first << "'.\n";
}
