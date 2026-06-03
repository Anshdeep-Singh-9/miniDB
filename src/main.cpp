#include "declaration.h"
#include "display.h"
#include "parser.h"
#include "file_handler.h"
#include "recovery_manager.h"
#include "where.h"
#include "auth.h"
#include "utilities.h"

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <limits>
#include <cctype>

using namespace std;

void system_check();

#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define CYAN    "\033[36m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define RED     "\033[31m"
#define WHITE   "\033[97m"

void clear_screen() {
    // What: clear the terminal before printing a fresh screen.
    // Why: the CLI is menu-driven, so old output should not visually mix with the next screen.
    // Example: after returning from Query Console, the main menu is redrawn cleanly.
    system("clear");
}

void print_line() {
    // What: print the main visual separator line.
    // Why: it gives the terminal UI a clear boundary between banner, menu, and sections.
    // Example: printed above and below the MiniDB banner.
    cout << CYAN << "======================================================================" << RESET << "\n";
}

void print_small_line() {
    // What: print a smaller section separator.
    // Why: help text and syntax guides become easier to scan in the terminal.
    // Example: used between "Syntax Guide" heading and sample SQL commands.
    cout << DIM << "----------------------------------------------------------------------" << RESET << "\n";
}

void pause_screen(bool clearLeftoverNewline = false) {
    // What: wait for ENTER before moving to the next screen.
    // Why: without pause, result output would disappear immediately after menu actions.
    // Example: after showing metadata, user presses ENTER to return to the main menu.
    cout << DIM << "\nPress ENTER to continue..." << RESET;
    cin.clear();

    if (clearLeftoverNewline) {
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
    }

    cin.get();
}

void print_banner() {
    // What: render the MiniDB title and feature tagline.
    // Why: every screen starts with the same identity and context for the CLI monitor.
    // Example: shows "Storage Engine | B+ Tree Index | Buffer Pool | SQL Parser".
    print_line();

    cout << BOLD << CYAN;
    cout << "  __  __ _       _ ____  ____  \n";
    cout << " |  \\/  (_)_ __ (_)  _ \\| __ ) \n";
    cout << " | |\\/| | | '_ \\| | | | |  _ \\ \n";
    cout << " | |  | | | | | | | |_| | |_) |\n";
    cout << " |_|  |_|_|_| |_|_|____/|____/ \n";
    cout << RESET;

    cout << BOLD << WHITE << "\n        MiniDB Engine - Terminal DBMS Monitor\n" << RESET;
    cout << DIM << "        Storage Engine | B+ Tree Index | Buffer Pool | SQL Parser\n" << RESET;

    print_line();
}

void print_section(string title) {
    // What: start a named screen by clearing terminal, printing banner, then section title.
    // Why: menu options share the same layout but different section names.
    // Example: print_section("Query Console") opens the SQL console screen.
    clear_screen();
    print_banner();

    cout << BOLD << WHITE << " " << title << RESET << "\n";
    print_small_line();
}

void help() {
    // What: show supported SQL-like commands and execution notes.
    // Why: users need quick syntax reference without reading README or source code.
    // Example: lists CREATE, INSERT, SELECT WHERE, JOIN, UPDATE, DELETE, transactions.
    print_section("Help");
    cout << BOLD << "MiniDB Supported Operations\n" << RESET;
    print_small_line();

    cout << "1. Query Console\n";
    cout << "2. Search table or search inside table\n";
    cout << "3. Print metadata of a table\n";
    cout << "4. Help\n";
    cout << "5. Quit\n\n";

    cout << BOLD << "Supported Query Console Syntax\n" << RESET;
    print_small_line();

    cout << GREEN << "SHOW TABLES;\n" << RESET;
    cout << GREEN << "SHOW DATABASES;\n" << RESET;
    cout << GREEN << "SHOW USERS;\n" << RESET;
    cout << GREEN << "CREATE DATABASE semester4;\n" << RESET;
    cout << GREEN << "CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));\n" << RESET;
    cout << GREEN << "CREATE USER administrator IDENTIFIED BY \"pass123\";\n" << RESET;
    cout << GREEN << "INSERT INTO students VALUES (1, \"Aditya\", \"CSE\");\n" << RESET;
    cout << GREEN << "SELECT * FROM Students WHERE ID = 1;\n" << RESET;
    cout << GREEN << "SELECT * FROM students ORDER BY name;\n" << RESET;
    cout << GREEN << "SELECT * FROM students LIMIT 5;\n" << RESET;
    cout << GREEN << "SELECT COUNT(*) FROM students;\n" << RESET;
    cout << GREEN << "SELECT SUM(id), AVG(id), MIN(name), MAX(name) FROM students;\n" << RESET;
    cout << GREEN << "SELECT s.name, d.hod FROM students JOIN departments ON students.dept = departments.code;\n" << RESET;
    cout << GREEN << "SELECT s.name, d.hod FROM students LEFT JOIN departments ON students.dept = departments.code;\n" << RESET;
    cout << GREEN << "SELECT * FROM Students WHERE Dept = CSE;\n" << RESET;
    cout << GREEN << "UPDATE Students SET Dept = ECE WHERE ID = 1;\n" << RESET;
    cout << GREEN << "UPDATE Students SET Dept = ECE WHERE Name = Anshdeep Singh;\n" << RESET;
    cout << GREEN << "DELETE FROM Students WHERE ID = 1;\n" << RESET;
    cout << GREEN << "DELETE FROM Students WHERE Dept = CSE;\n" << RESET;
    cout << GREEN << "DROP TABLE Students;\n" << RESET;
    cout << GREEN << "DROP DATABASE semester4;\n" << RESET;
    cout << GREEN << "BEGIN;\n" << RESET;
    cout << GREEN << "COMMIT;\n" << RESET;
    cout << GREEN << "ROLLBACK;\n" << RESET;
    cout << GREEN << "USE semester4;\n" << RESET;

    cout << "\n" << BOLD << "Notes\n" << RESET;
    print_small_line();
    cout << "- SQL keywords are case-insensitive.\n";
    cout << "- Table names and column names are case-sensitive.\n";
    cout << "- VARCHAR values keep the original case exactly as typed.\n";
    cout << "- Quotes are optional for VARCHAR values, but recommended for clarity.\n";
    cout << "- First column must be INT because it is used as primary key.\n";
    cout << "- Primary key column CANNOT be updated (immutable index key).\n";
    cout << "- INSERT values must follow the same order as table columns.\n";
    cout << "- UPDATE: if WHERE column is primary key => B+ Tree lookup (fast).\n";
    cout << "- UPDATE: if WHERE column is non-primary => Linear scan (all matching rows).\n";
    cout << "- DELETE: if WHERE column is primary key => B+ Tree lookup (fast).\n";
    cout << "- DELETE: if WHERE column is non-primary => Linear scan (all matching rows).\n";
    cout << "- SELECT WHERE: primary key => B+ Tree; other column => Linear scan.\n";
    cout << "- Type BACK or EXIT inside Query Console to return to the main menu.\n";

    print_small_line();
}


int take_input_option() {
    // What: read and validate the main menu choice.
    // Why: only options 1-5 are valid; invalid input should not crash or enter wrong flow.
    // Example: input "1" opens Query Console, input "abc" shows invalid choice.
    string option;

    print_line();

    cout << BOLD << WHITE << " MAIN MENU\n" << RESET;
    print_small_line();

    cout << CYAN << " 1 " << RESET << "Query Console  (CREATE / INSERT / SELECT / SHOW / DROP)\n";
    cout << CYAN << " 2 " << RESET << "Search table / search inside table\n";
    cout << CYAN << " 3 " << RESET << "Print metadata of a table\n";
    cout << CYAN << " 4 " << RESET << "Help\n";
    cout << CYAN << " 5 " << RESET << "Quit\n";

    print_line();

    cout << BOLD << "Enter choice [1-5]: " << RESET;
    cin >> option;

    if (option.length() != 1 || option[0] < '1' || option[0] > '5') {
        cout << RED << "\nInvalid input. Please enter a number from 1 to 5.\n" << RESET;
        return -1;
    }

    return option[0] - '0';
}



string normalize_console_command(string s) {
    // What: normalize only control commands typed inside Query Console.
    // Why: BACK, back;, and "  Exit  " should all be understood as exit commands.
    // Example: "BACK;" becomes "back"; normal SQL is still passed unchanged to parser.
    while (!s.empty() && isspace((unsigned char)s.front())) {
        s.erase(s.begin());
    }

    while (!s.empty() && isspace((unsigned char)s.back())) {
        s.pop_back();
    }

    if (!s.empty() && s.back() == ';') {
        s.pop_back();
    }

    for (char &c : s) {
        c = static_cast<char>(tolower((unsigned char)c));
    }

    return s;
}

void print_query_console_syntax() {
    // What: print short examples directly above the query prompt.
    // Why: MiniDB syntax is limited, so visible examples prevent common input mistakes.
    // Example: user can copy CREATE TABLE students (...) from this guide.
    cout << BOLD << "Syntax Guide\n" << RESET;
    print_small_line();

    cout << GREEN << "SHOW TABLES;\n" << RESET;
    cout << GREEN << "SHOW DATABASES;\n" << RESET;
    cout << GREEN << "CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));\n" << RESET;
    cout << GREEN << "INSERT INTO students VALUES (1, \"Aditya\", \"CSE\");\n" << RESET;
    cout << GREEN << "SELECT * FROM students;\n" << RESET;
    cout << GREEN << "SELECT name, dept FROM students;\n" << RESET;
    cout << GREEN << "SELECT * FROM students WHERE id = 1;\n" << RESET;
    cout << GREEN << "SELECT s.name, d.hod FROM students JOIN departments ON students.dept = departments.code;\n" << RESET;
    cout << GREEN << "SELECT s.name, d.hod FROM students LEFT JOIN departments ON students.dept = departments.code;\n" << RESET;
    cout << GREEN << "UPDATE students SET dept = ECE WHERE id = 1;\n" << RESET;
    cout << GREEN << "DELETE FROM students WHERE id = 1;\n" << RESET;
    cout << GREEN << "DELETE FROM students WHERE dept = CSE;\n" << RESET;
    cout << GREEN << "DROP TABLE students;\n" << RESET;
    cout << GREEN << "DROP DATABASE semester4;\n" << RESET;
    cout << GREEN << "DROP USER administrator;\n" << RESET;
    cout << GREEN << "USE semester4;\n" << RESET;
    cout << GREEN << "BEGIN;\n" << RESET;
    cout << GREEN << "COMMIT;\n" << RESET;
    cout << GREEN << "ROLLBACK;\n" << RESET;

    print_small_line();
}

HistoryManager history_manager;

void query_console_loop() {
    // What: interactive SQL-like console loop.
    // Why: this is the main user-facing path for CREATE/INSERT/SELECT/UPDATE/DELETE/JOIN.
    // Example: user types INSERT INTO students VALUES (1, "Aryan", "CSE");
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    while (true) {
        cout << "\n" << BOLD << "MiniDB Query Console" << RESET << "\n";
        print_small_line();

        cout << DIM << "Type BACK or EXIT to return to main menu.\n\n" << RESET;

        print_query_console_syntax();
        cout << "\n";

        string query = history_manager.readline(string(BOLD) + "minidb> " + RESET);

        string command = normalize_console_command(query);

        if (command == "back" || command == "exit") {
            cout << GREEN << "Returning to main menu...\n" << RESET;
            break;
        }

        if (!query.empty()) {
            history_manager.add_to_history(query);
            execute_query_string(query);
        }
    }
}

void input() {
    // What: main menu event loop of the CLI.
    // Why: after login/startup, all user actions are routed from here.
    // Example: option 1 goes to SQL parser, option 2 builds a WHERE clause manually.
    while (true) {
        int c = take_input_option();
        if (c == -1) {
            cin.clear();
            pause_screen(true);
            clear_screen();
            print_banner();
            continue;
        }
        switch (c) {
            case 1:
                // What: route user into full SQL-like query console.
                // Why: this path supports parser-driven CREATE/INSERT/SELECT/JOIN/UPDATE/DELETE.
                // Example: minidb> SELECT * FROM students WHERE id = 1;
                print_section("Query Console");
                query_console_loop();
                pause_screen(false);
                break;

            case 2: {
                // What: legacy guided search flow that builds SELECT * WHERE manually.
                // Why: useful for demos because it asks table, column, and value step by step.
                // Example: table=students, column=id, value=1 becomes WHERE id = 1.
                print_section("Search Inside Table");
                string search_table_name;
                cout << BOLD << "Enter table name: " << RESET;
                cin >> search_table_name;
                char tab_check[MAX_NAME];
                strncpy(tab_check, search_table_name.c_str(), MAX_NAME - 1);
                tab_check[MAX_NAME - 1] = '\0';

                if (search_table(tab_check) == 0) {
                    cout << RED << "Error: Table '" << search_table_name
                         << "' does not exist." << RESET << "\n";
                    pause_screen(true);
                    break;
                }

                table* search_meta = fetch_meta_data(search_table_name);
                if (search_meta == NULL) {
                    cout << RED << "Error: Could not load metadata." << RESET << "\n";
                    pause_screen(true);
                    break;
                }

                cout << "\n" << BOLD << "Available columns:\n" << RESET;
                for (int ci = 0; ci < search_meta->count; ci++) {
                    string type_str = (search_meta->col[ci].type == INT) ? "INT" : "VARCHAR";
                    cout << "  [" << ci << "] "
                         << search_meta->col[ci].col_name
                         << " (" << type_str << ")";
                    if (ci == 0) cout << " <- Primary Key";
                    cout << "\n";
                }

                string search_col, search_val;
                cout << "\n" << BOLD << "Enter column to search on: " << RESET;
                cin >> search_col;
                cout << BOLD << "Enter value to search for: " << RESET;
                cin >> ws;
                getline(cin, search_val);

                if (search_val.size() >= 2 &&
                    ((search_val.front() == '"' && search_val.back() == '"') ||
                     (search_val.front() == '\'' && search_val.back() == '\''))) {
                    search_val = search_val.substr(1, search_val.size() - 2);
                }

                delete search_meta;

                WhereClause wc;
                wc.present = true;
                wc.column = search_col;
                wc.op = "=";
                wc.value = search_val;

                vector<string> all_cols;
                all_cols.push_back("*");
                execute_select_where(search_table_name, all_cols, wc);

                pause_screen(true);
                break;
            }

            case 3:
                // What: show schema/catalog details for one table.
                // Why: helps verify table structure before insert/search/debugging.
                // Example: students metadata shows each column type and size.
                print_section("Table Metadata");
                display_meta_data();
                pause_screen(true);
                break;

            case 4:
                // What: open built-in help screen.
                // Why: users can check supported syntax while staying inside MiniDB.
                // Example: confirms that BEGIN, COMMIT, and ROLLBACK are available.
                help();
                pause_screen(true);
                break;

            case 5:
                // What: exit the CLI process.
                // Why: clean user-controlled shutdown path from the main menu.
                // Example: choosing 5 prints goodbye and terminates the program.
                print_section("Exit");
                cout << GREEN << "MiniDB closed successfully.\n" << RESET;
                cout << BOLD << CYAN << "\nGood bye!\n\n" << RESET;
                exit(0);

            default:
                cout << RED << "\nPlease choose a correct option.\n" << RESET;
                pause_screen(true);
                break;
        }

        clear_screen();
        print_banner();
    }
}

void start_system() {
    // What: initialize auth/storage directories, run recovery, then open the menu UI.
    // Why: MiniDB must repair pending WAL changes before users run new queries.
    // Example: after crash-after-WAL test, recover_all_tables() replays pending page writes.
    system_check();
    AuthManager::init();
    RecoveryManager::recover_all_tables();

    clear_screen();
    print_banner();

    cout << GREEN << "Welcome to MiniDB Monitor.\n" << RESET;
    cout << DIM << "Use Query Console to execute SQL-like commands.\n\n" << RESET;

    input();
}

string get_password() {
    // What: read password without echoing characters to the terminal.
    // Why: login should not show typed password on screen.
    // Example: ./miniDB -u tester -p calls this before authenticate().
    struct termios termios_p;

    tcgetattr(STDIN_FILENO, &termios_p);

    termios_p.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_p);

    cout << BOLD << "Enter Password: " << RESET;

    string pass;
    cin >> pass;

    termios_p.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_p);

    cout << "\n";

    return pass;
}

string get_password_with_prompt(const string& prompt) {
    struct termios termios_p;

    tcgetattr(STDIN_FILENO, &termios_p);
    termios_p.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_p);

    cout << prompt;

    string pass;
    cin >> pass;

    termios_p.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_p);

    cout << "\n";

    return pass;
}

bool create_user_interactively(const string& username, bool bootstrap_user) {
    if (username.empty()) {
        cout << RED << "Username cannot be empty.\n" << RESET;
        return false;
    }

    if (AuthManager::user_exists(username)) {
        cout << RED << "User already exists.\n" << RESET;
        return false;
    }

    if (bootstrap_user) {
        cout << YELLOW << "No users found in the system.\n" << RESET;
        cout << BOLD << "Creating initial user: " << RESET << username << "\n";
    } else {
        cout << YELLOW << "User '" << username << "' does not exist.\n" << RESET;
        cout << BOLD << "Creating new user: " << RESET << username << "\n";
    }

    while (true) {
        string pass1 = get_password_with_prompt("Set password: ");
        string pass2 = get_password_with_prompt("Confirm password: ");

        if (pass1.empty()) {
            cout << RED << "Password cannot be empty.\n" << RESET;
            continue;
        }

        if (pass1 != pass2) {
            cout << RED << "Passwords do not match. Try again.\n" << RESET;
            continue;
        }

        if (!AuthManager::register_user(username, pass1)) {
            cout << RED << "Failed to create user.\n" << RESET;
            return false;
        }

        set_active_user(username);
        ensure_default_database_for_active_user();
        system_check();
        cout << GREEN << "User created successfully!\n" << RESET;
        return true;
    }
}

int main(int argc, char *argv[]) {
    // What: command-line entry point for MiniDB.
    // Why: validates login arguments, creates first user if needed, then starts the DB monitor.
    // Example: ./miniDB -u tester -p launches authenticated CLI mode.
    system_check();
    AuthManager::init();

    if (argc == 4 || argc == 5) {
        if (strcmp(argv[1], "-u") == 0 && strcmp(argv[3], "-p") == 0) {
            string username = argv[2];

            clear_screen();
            print_banner();

            // First run check
            if (!AuthManager::has_any_user()) {
                // What: create the first MiniDB user when auth storage is empty.
                // Why: fresh installs need a bootstrap account before normal login can work.
                // Example: first run with ./miniDB -u tester -p creates user "tester".
                if (!create_user_interactively(username, true)) {
                    return 0;
                }
            } else if (!AuthManager::user_exists(username)) {
                cout << YELLOW << "User '" << username << "' was not found.\n" << RESET;
                cout << "Create this user now? (y/n): ";
                char choice = 'n';
                cin >> choice;

                if (tolower(static_cast<unsigned char>(choice)) != 'y') {
                    cout << RED << "Authentication cancelled.\n" << RESET;
                    return 0;
                }

                if (!create_user_interactively(username, false)) {
                    return 0;
                }
            } else {
                // What: authenticate an existing user before opening the database monitor.
                // Why: MiniDB should not expose tables until username/password are verified.
                // Example: stored SHA-256 hash is compared with hash of typed password.
                cout << BOLD << "User: " << RESET << username << "\n\n";

                string password = get_password();

                if (AuthManager::authenticate(username, password)) {
                    set_active_user(username);
                    ensure_default_database_for_active_user();
                    system_check();
                    cout << GREEN << "Correct password!\n" << RESET;
                } else {
                    cout << RED << "Incorrect password!\n" << RESET;
                    return 0;
                }
            }
        } else {
            cout << RED << "\nInvalid usage.\n" << RESET;
            cout << "Usage: ./minidb -u username -p\n";
            return 0;
        }
    } else {
        cout << RED << "\nInvalid usage.\n" << RESET;
        cout << "Usage: ./minidb -u username -p\n";
        return 0;
    }

    start_system();

    return 0;
}
