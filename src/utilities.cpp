#include "utilities.h"
#include <iostream>
#include <unistd.h>
#include <termios.h>
#include <vector>
#include <string>
#include <errno.h>

HistoryManager::HistoryManager() : history_index(-1) {
    tcgetattr(STDIN_FILENO, &orig_termios);
}

HistoryManager::~HistoryManager() {
    disable_raw_mode();
}

enum SpecialKeys {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_CTRL_LEFT,
    KEY_CTRL_RIGHT,
    KEY_DELETE,
    KEY_CTRL_BACKSPACE,
    KEY_ALT_BACKSPACE
};

void HistoryManager::enable_raw_mode() {
    // What: switch terminal input into character-by-character mode.
    // Why: readline needs arrow keys, backspace, and history handling before ENTER is pressed.
    // Example: pressing left arrow moves cursor inside minidb> input instead of printing symbols.
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void HistoryManager::disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

int HistoryManager::read_key() {
    // What: read one key press and translate escape sequences into internal key codes.
    // Why: arrow keys and Ctrl combinations arrive as multi-byte terminal sequences.
    // Example: Up arrow becomes KEY_UP so the query console can show previous command.
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) return -1;
    }

    if (c == '\x1b') {
        char seq[8];
        // Set a short timeout to see if there's more after Esc
        struct termios raw;
        tcgetattr(STDIN_FILENO, &raw);
        struct termios temp = raw;
        temp.c_cc[VMIN] = 0;
        temp.c_cc[VTIME] = 1; // 100ms timeout
        tcsetattr(STDIN_FILENO, TCSANOW, &temp);
        int n = read(STDIN_FILENO, &seq[0], 1);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        if (n != 1) return '\x1b';

        if (seq[0] == '\x7f' ) return KEY_ALT_BACKSPACE;

        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return KEY_HOME;
                        case '3': return KEY_DELETE;
                        case '4': return KEY_END;
                        case '7': return KEY_HOME;
                        case '8': return KEY_END;
                    }
                } else if (seq[2] == ';') {
                    if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &seq[4], 1) != 1) return '\x1b';
                    if (seq[3] == '5') {
                        if (seq[4] == 'D') return KEY_CTRL_LEFT;
                        if (seq[4] == 'C') return KEY_CTRL_RIGHT;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                }
            }
        } else if (seq[0] == 'O') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
        return '\x1b';
    }
    return c;
}

std::string HistoryManager::readline(const std::string& prompt) {
    // What: custom command-line editor for the MiniDB query console.
    // Why: users need command history, cursor movement, delete, and Ctrl navigation.
    // Example: type SELECT, press Up to recall old query, then edit before ENTER.
    std::string current_line = "";
    std::string draft = "";
    int cursor_pos = 0;
    history_index = history.size();

    auto refresh_line = [&]() {
        std::cout << "\r" << prompt << "\x1b[K" << current_line << std::flush;
        for (size_t i = 0; i < current_line.length() - cursor_pos; ++i) {
            std::cout << "\b";
        }
        std::cout << std::flush;
    };

    auto move_cursor = [&](int new_pos) {
        if (new_pos < 0) new_pos = 0;
        if (new_pos > (int)current_line.length()) new_pos = current_line.length();

        if (new_pos < cursor_pos) {
            for (int i = 0; i < cursor_pos - new_pos; ++i) std::cout << "\b";
        } else if (new_pos > cursor_pos) {
            for (int i = 0; i < new_pos - cursor_pos; ++i) {
                std::cout << current_line[cursor_pos + i];
            }
        }
        cursor_pos = new_pos;
        std::cout << std::flush;
    };

    std::cout << prompt << std::flush;
    enable_raw_mode();

    while (true) {
        int c = read_key();

        if (c == 13 || c == 10) { // Enter
            std::cout << "\r\n";
            break;
        } else if (c == 127) { // Backspace
            if (cursor_pos > 0) {
                current_line.erase(cursor_pos - 1, 1);
                cursor_pos--;
                refresh_line();
                if (history_index == (int)history.size()) draft = current_line;
            }
        } 
        else if (c ==23) { // Ctrl+Backspace
            if (cursor_pos > 0) {
                int old_pos = cursor_pos;
                
                // 1. Skip any spaces immediately to the left of the cursor
                while (cursor_pos > 0 && current_line[cursor_pos - 1] == ' ') {
                    cursor_pos--;
                }
                
                // 2. Skip the actual word characters to find the start of the word
                while (cursor_pos > 0 && current_line[cursor_pos - 1] != ' ') {
                    cursor_pos--;
                }
                
                current_line.erase(cursor_pos, old_pos - cursor_pos);
                refresh_line();
                if (history_index == (int)history.size()) draft = current_line;
            }
        } 
        else if(c == KEY_ALT_BACKSPACE){
            current_line.clear();
            cursor_pos=0;
            refresh_line();
        }
        else if (c == KEY_DELETE) {
            if (cursor_pos < (int)current_line.length()) {
                current_line.erase(cursor_pos, 1);
                refresh_line();
                if (history_index == (int)history.size()) draft = current_line;
            }
        } else if (c == KEY_LEFT) {
            move_cursor(cursor_pos - 1);
        } else if (c == KEY_RIGHT) {
            move_cursor(cursor_pos + 1);
        } else if (c == KEY_HOME) {
            move_cursor(0);
        } else if (c == KEY_END) {
            move_cursor(current_line.length());
        } else if (c == KEY_CTRL_LEFT) {
            int new_pos = cursor_pos;
            if (new_pos > 0) {
                new_pos--;
                while (new_pos > 0 && current_line[new_pos] == ' ') new_pos--;
                while (new_pos > 0 && current_line[new_pos - 1] != ' ') new_pos--;
            }
            move_cursor(new_pos);
        } else if (c == KEY_CTRL_RIGHT) {
            int new_pos = cursor_pos;
            int len = current_line.length();
            if (new_pos < len) {
                while (new_pos < len && current_line[new_pos] != ' ') new_pos++;
                while (new_pos < len && current_line[new_pos] == ' ') new_pos++;
            }
            move_cursor(new_pos);
        } else if (c == KEY_UP) {
            if (history_index > 0) {
                if (history_index == (int)history.size()) draft = current_line;
                history_index--;
                current_line = history[history_index];
                cursor_pos = current_line.length();
                refresh_line();
            }
        } else if (c == KEY_DOWN) {
            if (history_index < (int)history.size()) {
                history_index++;
                if (history_index < (int)history.size()) current_line = history[history_index];
                else current_line = draft;
                cursor_pos = current_line.length();
                refresh_line();
            }
        } else if (c == 4) { // Ctrl-D
            if (current_line.empty()) {
                disable_raw_mode();
                return "exit";
            }
        } else if (c >= 32 && c <= 126) {
            current_line.insert(cursor_pos, 1, (char)c);
            cursor_pos++;
            refresh_line();
            if (history_index == (int)history.size()) draft = current_line;
        }
    }

    disable_raw_mode();
    return current_line;
}

void HistoryManager::add_to_history(const std::string& command) {
    // What: save a command in memory for Up/Down navigation.
    // Why: repeated SQL testing is faster when previous commands can be recalled.
    // Example: after INSERT, pressing Up shows that INSERT again.
    if (!command.empty() && (history.empty() || history.back() != command)) {
        history.push_back(command);
    }
}
