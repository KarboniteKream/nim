#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NIM_VERSION "0.0.1"
#define NIM_TAB_STOP 4

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT { NULL, 0 }

struct erow {
    char *chars;
    size_t len;
    char *render;
    size_t rlen;
};

struct econfig {
    size_t x;
    size_t y;
    size_t rx;
    uint16_t w;
    uint16_t h;
    char *filename;
    struct erow *rows;
    size_t lines;
    size_t rowoff;
    size_t coloff;
    char message[80];
    time_t timestamp;
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
    size_t size;
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

int8_t get_screen_position(uint16_t *row, uint16_t *col) {
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

int8_t get_screen_size(uint16_t *rows, uint16_t *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }

        return get_screen_position(rows, cols);
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;

    return 0;
}

size_t x_to_rx(struct erow *row, size_t x) {
    size_t rx = 0;

    for (size_t i = 0; i < x; i++) {
        if (row->chars[i] == '\t') {
            // A tab is every NIM_TAB_STOP columns.
            rx += (NIM_TAB_STOP - 1) - (rx % NIM_TAB_STOP);
        }

        rx++;
    }

    return rx;
}

void update_row(struct erow *row) {
    size_t idx = 0;
    size_t tabs = 0;

    for (size_t i = 0; i < row->len; i++) {
        if (row->chars[i] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->len + (tabs * (NIM_TAB_STOP - 1)) + 1);

    for (size_t i = 0; i < row->len; i++) {
        if (row->chars[i] == '\t') {
            row->render[idx++] = ' ';

            while (idx % NIM_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[i];
        }
    }

    row->render[idx] = '\0';
    row->rlen = idx;
}

void append_row(char *s, size_t len) {
    E.rows = realloc(E.rows, (E.lines + 1) * sizeof(struct erow));

    size_t at = E.lines;
    E.rows[at].len = len;
    E.rows[at].chars = malloc(len + 1);
    memcpy(E.rows[at].chars, s, len);
    E.rows[at].chars[len] = '\0';

    E.rows[at].render = NULL;
    E.rows[at].rlen = 0;
    update_row(&E.rows[at]);

    E.lines++;
}

void open(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");

    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t size = 0;

    while (true) {
        ssize_t len = getline(&line, &size, fp);

        if (len == -1) {
            break;
        }

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            len--;
        }

        append_row(line, len);
    }

    free(line);
    fclose(fp);
}

void ab_append(struct abuf *ab, const char *s, size_t size) {
    char *new = realloc(ab->buf, ab->size + size);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->size], s, size);
    ab->buf = new;
    ab->size += size;
}

void ab_free(struct abuf *ab) {
    free(ab->buf);
}

void scroll_screen() {
    E.rx = 0;

    if (E.y < E.lines) {
        E.rx = x_to_rx(&E.rows[E.y], E.x);
    }

    if (E.y < E.rowoff) {
        E.rowoff = E.y;
    }

    if (E.y >= E.rowoff + E.h) {
        E.rowoff = E.y - E.h + 1;
    }

    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }

    if (E.rx >= E.coloff + E.w) {
        E.coloff = E.rx - E.w + 1;
    }
}

void draw_lines(struct abuf *ab) {
    char welcome[80];
    size_t len = snprintf(welcome, sizeof(welcome), "Nim (%s)", NIM_VERSION);

    if (len > E.w) {
        len = E.w;
    }

    for (uint16_t y = 0; y < E.h; y++) {
        size_t row = y + E.rowoff;

        if (row >= E.lines) {
            if (E.lines == 0 && y == E.h / 3) {
                size_t padding = (E.w - len) / 2;

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
        } else {
            ssize_t len = E.rows[row].rlen - E.coloff;

            if (len < 0) {
                len = 0;
            } else if (len > E.w) {
                len = E.w;
            }

            ab_append(ab, &E.rows[row].render[E.coloff], len);
        }

        ab_append(ab, "\x1b[K\r\n", 5);
    }
}

void draw_status_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);

    char status[80];
    char meta[80];

    size_t len = snprintf(status, sizeof(status), "%.20s - %ld lines",
            E.filename ? E.filename : "[No Name]", E.lines);
    size_t mlen = snprintf(meta, sizeof(meta), "%ld/%ld", E.y + 1, E.lines);

    if (len > E.w) {
        len = E.w;
    }

    ab_append(ab, status, len);

    while (len < E.w) {
        if (E.w - len == mlen) {
            ab_append(ab, meta, mlen);
            break;
        }

        ab_append(ab, " ", 1);
        len++;
    }

    ab_append(ab, "\x1b[m\r\n", 5);
}

void draw_message_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[K", 3);

    size_t len = strlen(E.message);

    if (len > E.w) {
        len = E.w;
    }

    if (len && time(NULL) - E.timestamp < 5) {
        ab_append(ab, E.message, len);
    }
}

void refresh_screen() {
    scroll_screen();

    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[?25l\x1b[H", 9);

    draw_lines(&ab);
    draw_status_bar(&ab);
    draw_message_bar(&ab);

    char buf[32];
    size_t num = snprintf(buf, sizeof(buf), "\x1b[%ld;%ldH",
            (E.y - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    ab_append(&ab, buf, num);

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.size);
    ab_free(&ab);
}

void set_message(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(E.message, sizeof(E.message), fmt, args);
    va_end(args);

    E.timestamp = time(NULL);
}

void move_cursor(uint16_t key) {
    struct erow *row = (E.y < E.lines) ? &E.rows[E.y] : NULL;

    switch (key) {
        case ARROW_UP:
            if (E.y > 0) {
                E.y--;
            }

            break;

        case ARROW_DOWN:
            if (E.y < E.lines) {
                E.y++;
            }

            break;

        case ARROW_LEFT:
            if (E.x > 0) {
                E.x--;
            } else if (E.y > 0) {
                E.y--;
                E.x = E.rows[E.y].len;
            }

            break;

        case ARROW_RIGHT:
            if (row && E.x < row->len) {
                E.x++;
            } else if (row && E.x == row->len) {
                E.y++;
                E.x = 0;
            }

            break;
    }

    row = (E.y < E.lines) ? &E.rows[E.y] : NULL;
    size_t len = row ? row->len : 0;

    if (E.x > len) {
        E.x = len;
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
            if (E.y < E.lines) {
                E.x = E.rows[E.y].len;
            }
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.y = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.y = E.rowoff + E.h - 1;

                    if (E.y > E.lines) {
                        E.y = E.lines;
                    }
                }

                int rows = E.h;
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
    E.rx = 0;
    E.filename = NULL;
    E.rows = NULL;
    E.lines = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.message[0] = '\0';
    E.timestamp = 0;

    if (get_screen_size(&E.h, &E.w) == -1) {
        die("get_size");
    }

    E.h -= 2;
}

int main(int argc, char **argv) {
    enable_raw_mode();
    init();

    if (argc >= 2) {
        open(argv[1]);
    }

    set_message("HELP: Ctrl-Q = quit");

    while (true) {
        refresh_screen();
        process_key();
    }

    return 0;
}
