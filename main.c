#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT { NULL, 0 }

struct econfig {
    uint16_t x;
    uint16_t y;
    uint16_t rows;
    uint16_t cols;
    struct termios terminal;
};

enum ekey {
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

struct abuf {
    char *buf;
    size_t len;
};

struct econfig E;

void clear_screen() {
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

void die(const char *s) {
    clear_screen();
    perror(s);
    exit(1);
}

void restore_terminal() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.terminal) == -1) {
        die("tcsetattr");
    }

    write(STDIN_FILENO, "\x1b[?1049l", 8);
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.terminal) == -1) {
        die("tcgetattr");
    }

    atexit(restore_terminal);

    struct termios raw = E.terminal;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }

    write(STDIN_FILENO, "\x1b[?1049h", 8);
}

uint16_t read_key() {
    char c;

    while (true) {
        ssize_t num = read(STDIN_FILENO, &c, 1);

        if (num == 1) {
            break;
        }

        if (num == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }

        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if ('0' <= seq[1] && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }

    return c;
}

int8_t get_position(uint16_t *row, uint16_t *col) {
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    char buf[32];
    size_t i = 0;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1 || buf[i] == 'R') {
            break;
        }

        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }

    if (sscanf(&buf[2], "%hu;%hu", row, col) != 2) {
        return -1;
    }

    return 0;
}

int8_t get_window_size(uint16_t *rows, uint16_t *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }

        return get_position(rows, cols);
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;

    return 0;
}

void ab_append(struct abuf *ab, const char *s, size_t len) {
    char *new = realloc(ab->buf, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->buf = new;
    ab->len += len;
}

void ab_free(struct abuf *ab) {
    free(ab->buf);
}

void draw_rows(struct abuf *ab) {
    char welcome[80];
    size_t len = snprintf(welcome, sizeof(welcome), "Nim (%s)", VERSION);

    if (len > E.cols) {
        len = E.cols;
    }

    for (uint16_t y = 0; y < E.rows; y++) {
        if (y == E.rows / 3) {
            size_t padding = (E.cols - len) / 2;

            if (padding > 0) {
                ab_append(ab, "~", 1);
                padding--;
            }

            while (padding--) {
                ab_append(ab, " ", 1);
            }

            ab_append(ab, welcome, len);
        } else {
            ab_append(ab, "~", 1);
        }

        ab_append(ab, "\x1b[K", 3);

        if (y < E.rows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

void refresh_screen() {
    struct abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l\x1b[H", 9);
    draw_rows(&ab);

    char buf[32];
    size_t len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.y + 1, E.x + 1);
    ab_append(&ab, buf, len);

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    ab_free(&ab);
}

void move_cursor(uint16_t key) {
    switch (key) {
        case ARROW_UP:
            if (E.y > 0) {
                E.y--;
            }
            break;

        case ARROW_DOWN:
            if (E.y < E.rows - 1) {
                E.y++;
            }
            break;

        case ARROW_LEFT:
            if (E.x > 0) {
                E.x--;
            }
            break;

        case ARROW_RIGHT:
            if (E.x < E.cols - 1) {
                E.x++;
            }
            break;
    }
}

void process_key() {
    uint16_t c = read_key();

    switch (c) {
        case CTRL_KEY('q'):
            clear_screen();
            exit(0);
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;

        case HOME_KEY:
            E.x = 0;
            break;

        case END_KEY:
            E.x = E.cols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int rows = E.rows;

                while (rows--) {
                    move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
    }
}

void init() {
    E.x = 0;
    E.y = 0;

    if (get_window_size(&E.rows, &E.cols) == -1) {
        die("get_size");
    }
}

int main() {
    enable_raw_mode();
    init();

    while (true) {
        refresh_screen();
        process_key();
    }

    return 0;
}
