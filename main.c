/*
 * NIM 0.0.1
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#define NIM_QUIT_TIMES 3
#define NIM_TAB_STOP 4
#define NIM_NUMLINES true

#define HL_NUMBERS (1 << 0)
#define HL_STRINGS (1 << 1)

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT { NULL, 0 }

void set_message(const char *fmt, ...);
void refresh_screen();
char *prompt(char *message, void (*callback)(char *, uint16_t));

enum ekey {
    ENTER = '\r',
    ESCAPE = '\x1b',
    BACKSPACE = 127,
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DELETE,
    HOME,
    END,
    PAGE_UP,
    PAGE_DOWN,
};

struct esyntax {
    char *filetype;
    char **patterns;
    char *comment;
    char *mlcomment_start;
    char *mlcomment_end;
    char **keywords;
    uint8_t flags;
};

enum ehl {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_KEYWORD3,
    HL_KEYWORD4,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH,
};

struct erow {
    size_t idx;
    bool comment;
    char *chars;
    size_t len;
    char *render;
    size_t rlen;
    uint8_t *hl;
};

struct econfig {
    size_t x;
    size_t y;
    uint8_t gw;
    size_t rx;
    uint16_t w;
    uint16_t h;
    char *filename;
    bool dirty;
    struct erow *rows;
    size_t lines;
    size_t rowoff;
    size_t coloff;
    char message[80];
    time_t timestamp;
    struct esyntax *syntax;
    struct termios terminal;
};

struct abuf {
    char *buf;
    size_t size;
};

struct econfig E;

char *C_extensions[] = { ".c", ".h", NULL };

char *C_keywords[] = {
    "!#include", "!#define", "!break", "!continue", "!return",
    "@switch", "@if", "@else", "@struct", "@enum", "@union", "@typedef",
    "#while", "#for", "#case", "#int", "#long", "#double", "#float", "#char",
    "#unsigned", "#signed", "#void", "#int8_t", "#uint8_t", "#int16_t", "#uint16_t",
    "#int32_t", "#uint32_t", "#int64_t", "#uint64_t", "#ssize_t", "#size_t",
    "#NULL", "#const", "#bool", "#true", "#false",
    "$sizeof",
    NULL,
};

struct esyntax HLDB[] = {
    {
        "c",
        C_extensions,
        "//",
        "/*",
        "*/",
        C_keywords,
        HL_STRINGS | HL_NUMBERS,
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

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

    if (c == ESCAPE) {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return ESCAPE;
        }

        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return ESCAPE;
        }

        if (seq[0] == '[') {
            if ('0' <= seq[1] && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return ESCAPE;
                }

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME;
                        case '3': return DELETE;
                        case '4': return END;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME;
                        case '8': return END;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME;
                    case 'F': return END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME;
                case 'F': return END;
            }
        }

        return ESCAPE;
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

    if (buf[0] != ESCAPE || buf[1] != '[') {
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

void update_gutter() {
    uint8_t old_gw = E.gw;
    E.gw = 0;

    if (NIM_NUMLINES) {
        char lines[16];
        E.gw += snprintf(lines, sizeof(lines), "%ld", E.lines);
    }

    if (E.gw > 0) {
        E.gw++;
    }

    E.w -= (E.gw - old_gw);
}

bool is_separator(uint16_t c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void update_syntax(struct erow *row) {
    row->hl = realloc(row->hl, row->rlen);
    memset(row->hl, HL_NORMAL, row->rlen);

    if (E.syntax == NULL) {
        return;
    }

    char *cm = E.syntax->comment;
    char *mcs = E.syntax->mlcomment_start;
    char *mce = E.syntax->mlcomment_end;

    size_t cmlen = cm ? strlen(cm) : 0;
    size_t mcslen = mcs ? strlen(mcs) : 0;
    size_t mcelen = mce ? strlen(mce) : 0;

    char **keywords = E.syntax->keywords;

    bool after_sep = true;
    char in_string = '\0';
    bool in_comment = (row->idx > 0 && E.rows[row->idx - 1].comment);

    size_t i = 0;

    while (i < row->rlen) {
        char c = row->render[i];
        uint8_t prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (cmlen && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], cm, cmlen)) {
                memset(&row->hl[i], HL_COMMENT, row->rlen - i);
                break;
            }
        }

        if (mcslen && mcelen && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;

                if (!strncmp(&row->render[i], mce, mcelen)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mcelen);
                    i += mcelen;
                    in_comment = false;
                    after_sep = true;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcslen)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcslen);
                i += mcslen;
                in_comment = true;
                continue;
            }
        }

        if (E.syntax->flags & HL_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;

                if (c == '\\' && i + 1 < row->rlen) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }

                if (c == in_string) {
                    in_string = '\0';
                }

                i++;
                after_sep = true;
                continue;
            }

            if (c == '"' || c == '\'') {
                in_string = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        if (E.syntax->flags & HL_NUMBERS) {
            if ((isdigit(c) && (after_sep || prev_hl == HL_NUMBER))
                    || (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i++] = HL_NUMBER;
                after_sep = false;
                continue;
            }
        }

        if (after_sep) {
            size_t j;

            for (j = 0; keywords[j]; j++) {
                size_t len = strlen(keywords[j]) - 1;

                uint8_t color = HL_NORMAL;
                if (keywords[j][0] == '!') {
                    color = HL_KEYWORD1;
                } else if (keywords[j][0] == '@') {
                    color = HL_KEYWORD2;
                } else if (keywords[j][0] == '#') {
                    color = HL_KEYWORD3;
                } else if (keywords[j][0] == '$') {
                    color = HL_KEYWORD4;
                }

                if (!strncmp(&row->render[i], &keywords[j][1], len)
                        && is_separator(row->render[i + len])) {
                    memset(&row->hl[i], color, len);
                    i += len;
                    break;
                }
            }

            if (keywords[j] != NULL) {
                after_sep = false;
                continue;
            }
        }

        after_sep = is_separator(c);
        i++;
    }

    bool changed = (row->comment != in_comment);
    row->comment = in_comment;

    if (changed && row->idx + 1 < E.lines) {
        update_syntax(&E.rows[row->idx + 1]);
    }
}

uint8_t syntax_to_color(uint8_t hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1: return 91;
        case HL_KEYWORD2: return 94;
        case HL_KEYWORD3: return 33;
        case HL_KEYWORD4: return 36;
        case HL_STRING: return 32;
        case HL_NUMBER: return 91;
        case HL_MATCH: return 33;
        default: return 37;
    }
}

void select_syntax() {
    E.syntax = NULL;

    if (E.filename == NULL) {
        return;
    }

    char *ext = strrchr(E.filename, '.');

    for (size_t idx = 0; idx < HLDB_ENTRIES; idx++) {
        struct esyntax *syntax = &HLDB[idx];

        size_t i = 0;

        while (syntax->patterns[i]) {
            bool is_ext = (syntax->patterns[i][0] == '.');

            if ((is_ext && ext && !strcmp(ext, syntax->patterns[i]))
                    || (!is_ext && strstr(E.filename, syntax->patterns[i]))) {
                E.syntax = syntax;

                for (size_t row = 0; row < E.lines; row++) {
                    update_syntax(&E.rows[row]);
                }

                return;
            }

            i++;
        }
    }
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

size_t rx_to_x(struct erow *row, size_t rx) {
    size_t curr_rx = 0;
    size_t x;

    for (x = 0; x < row->len; x++) {
        if (row->chars[x] == '\t') {
            curr_rx += (NIM_TAB_STOP - 1) - (curr_rx % NIM_TAB_STOP);
        }

        curr_rx++;

        if (curr_rx > rx) {
            return x;
        }
    }

    return x;
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

    update_syntax(row);
}

void insert_row(size_t at, char *s, size_t len) {
    if (at > E.lines) {
        return;
    }

    E.rows = realloc(E.rows, (E.lines + 1) * sizeof(struct erow));
    memmove(&E.rows[at + 1], &E.rows[at], (E.lines - at) * sizeof(struct erow));

    for (size_t i = at + 1; i <= E.lines; i++) {
        E.rows[i].idx++;
    }

    struct erow *row = &E.rows[at];
    row->idx = at;
    row->comment = false;
    row->len = len;
    row->chars = malloc(len + 1);
    memcpy(row->chars, s, len);
    row->chars[len] = '\0';
    row->render = NULL;
    row->rlen = 0;
    row->hl = NULL;
    update_row(row);

    E.lines++;
    E.dirty = true;

    update_gutter();
}

void free_row(struct erow *row) {
    free(row->hl);
    free(row->render);
    free(row->chars);
}

void delete_row(size_t at) {
    if (at >= E.lines) {
        return;
    }

    free_row(&E.rows[at]);
    memmove(&E.rows[at], &E.rows[at + 1], (E.lines - at - 1) * sizeof(struct erow));

    for (size_t i = at; i < E.lines - 1; i++) {
        E.rows[i].idx--;
    }

    E.lines--;
    E.dirty = true;

    update_gutter();
}

void insert_char_at_row(struct erow *row, size_t at, uint16_t c) {
    if (at > row->len) {
        at = row->len;
    }

    row->chars = realloc(row->chars, row->len + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
    row->chars[at] = c;
    row->len++;

    update_row(row);
    E.dirty = true;
}

void delete_char_at_row(struct erow *row, size_t at) {
    if (at >= row->len) {
        return;
    }

    memmove(&row->chars[at], &row->chars[at + 1], row->len - at);
    row->len--;

    update_row(row);
    E.dirty = true;
}

void append_string_at_row(struct erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->len + len + 1);
    memcpy(&row->chars[row->len], s, len);
    row->len += len;
    row->chars[row->len] = '\0';
    update_row(row);
    E.dirty = true;
}

void insert_char(uint16_t c) {
    if (E.y == E.lines) {
        insert_row(E.lines, "", 0);
    }

    insert_char_at_row(&E.rows[E.y], E.x, c);
    E.x++;
}

void insert_newline() {
    if (E.x == 0) {
        insert_row(E.y, "", 0);
    } else {
        struct erow *row = &E.rows[E.y];
        insert_row(E.y + 1, &row->chars[E.x], row->len - E.x);

        row = &E.rows[E.y];
        row->len = E.x;
        row->chars[row->len] = '\0';
        update_row(row);
    }

    E.y++;
    E.x = 0;
}

void delete_char() {
    if ((E.x == 0 && E.y == 0) || E.y == E.lines) {
        return;
    }

    struct erow *row = &E.rows[E.y];

    if (E.x > 0) {
        delete_char_at_row(row, E.x - 1);
        E.x--;
    } else {
        E.x = E.rows[E.y - 1].len;
        append_string_at_row(&E.rows[E.y - 1], row->chars, row->len);
        delete_row(E.y);
        E.y--;
    }
}

void open_file(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");

    if (!fp) {
        die("fopen");
    }

    select_syntax();

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

        insert_row(E.lines, line, len);
    }

    free(line);
    fclose(fp);
    E.dirty = false;
}

char *to_string(size_t *len) {
    *len = 0;

    for (size_t i = 0; i < E.lines; i++) {
        *len += E.rows[i].len + 1;
    }

    char *buf = malloc(*len);
    char *ptr = buf;

    for (size_t i = 0; i < E.lines; i++) {
        memcpy(ptr, E.rows[i].chars, E.rows[i].len);
        ptr += E.rows[i].len;
        *ptr = '\n';
        ptr++;
    }

    return buf;
}

void save_file() {
    if (E.filename == NULL) {
        E.filename = prompt("Save as: %s (ESC to cancel)", NULL);

        if (E.filename == NULL) {
            set_message("");
            return;
        }

        select_syntax();
    }

    size_t len;
    char *buf = to_string(&len);

    int32_t fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == (ssize_t) len) {
                close(fd);
                free(buf);
                E.dirty = false;
                set_message("%d bytes written to disk.", len);
                return;
            }
        }

        close(fd);
    }

    free(buf);
    set_message("Save failed: %s", strerror(errno));
}

void find(char *query, uint16_t key) {
    static ssize_t last_match = -1;
    static int8_t direction = 1;

    static size_t line;
    static char *hl = NULL;

    if (hl) {
        memcpy(E.rows[line].hl, hl, E.rows[line].rlen);
        free(hl);
        hl = NULL;
    }

    if (key == ENTER || key == ESCAPE) {
        last_match = -1;
        direction = 1;
        return;
    }

    if (key == ARROW_DOWN || key == ARROW_RIGHT) {
        direction = 1;
    } else if (key == ARROW_UP || key == ARROW_LEFT) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) {
        direction = 1;
    }

    ssize_t y = last_match;

    for (size_t i = 0; i < E.lines; i++) {
        y += direction;

        if (y == -1) {
            y = E.lines - 1;
        } else if (y == (ssize_t) E.lines) {
            y = 0;
        }

        struct erow *row = &E.rows[y];
        char *match = strstr(row->render, query);

        if (match) {
            last_match = y;
            E.y = y;
            E.x = rx_to_x(row, match - row->render);
            E.rowoff = E.lines;

            line = y;
            hl = malloc(row->rlen);
            memcpy(hl, row->hl, row->rlen);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));

            break;
        }
    }
}

void start_find() {
    size_t x = E.x;
    size_t y = E.y;
    size_t rowoff = E.rowoff;
    size_t coloff = E.coloff;

    char *query = prompt("Find: %s (ESC to cancel, arrows to navigate)", find);

    if (query == NULL) {
        E.x = x;
        E.y = y;
        E.rowoff = rowoff;
        E.coloff = coloff;
    }

    free(query);
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

void draw_gutter(struct abuf *ab, struct erow *row) {
    if (E.gw == 0) {
        return;
    }

    char gutter[E.gw + 1];

    if (row && NIM_NUMLINES) {
        snprintf(gutter, sizeof(gutter), "%*ld ", E.gw - 1, row->idx + 1);
    }

    ab_append(ab, "\x1b[90m", 5);
    ab_append(ab, gutter, E.gw);
    ab_append(ab, "\x1b[39m", 5);
}

void draw_lines(struct abuf *ab) {
    char welcome[80];
    size_t len = snprintf(welcome, sizeof(welcome), "Nim (%s)", NIM_VERSION);

    if (len > E.w) {
        len = E.w;
    }

    for (uint16_t y = 0; y < E.h; y++) {
        size_t idx = y + E.rowoff;

        if (idx >= E.lines) {
            draw_gutter(ab, NULL);

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
            struct erow *row = &E.rows[idx];
            draw_gutter(ab, row);

            ssize_t len = row->rlen - E.coloff;

            if (len < 0) {
                len = 0;
            } else if (len > E.w) {
                len = E.w;
            }

            char *c = &row->render[E.coloff];
            uint8_t *hl = &row->hl[E.coloff];
            int16_t curr_color = -1;

            for (ssize_t i = 0; i < len; i++) {
                if (iscntrl(c[i])) {
                    char sym = (c[i] <= 26) ? '@' + c[i] : '?';
                    ab_append(ab, "\x1b[7m", 4);
                    ab_append(ab, &sym, 1);
                    ab_append(ab, "\x1b[m", 3);

                    if (curr_color != -1) {
                        char buf[16];
                        size_t clen = snprintf(buf, sizeof(buf), "\x1b[%dm", curr_color);
                        ab_append(ab, buf, clen);
                    }
                } else if (hl[i] == HL_NORMAL) {
                    if (curr_color != -1) {
                        ab_append(ab, "\x1b[39m", 5);
                        curr_color = -1;
                    }

                    ab_append(ab, &c[i], 1);
                } else {
                    uint8_t color = syntax_to_color(hl[i]);

                    if (color != curr_color) {
                        curr_color = color;

                        char buf[16];
                        size_t clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        ab_append(ab, buf, clen);
                    }

                    ab_append(ab, &c[i], 1);
                }
            }

            ab_append(ab, "\x1b[39m", 5);
        }

        ab_append(ab, "\x1b[K\r\n", 5);
    }
}

void draw_status_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);

    char status[80];
    char meta[80];

    size_t len = snprintf(status, sizeof(status), "%.20s - %ld lines%s",
            E.filename ? E.filename : "[No Name]", E.lines,
            E.dirty ? " (modified)" : "");
    size_t mlen = snprintf(meta, sizeof(meta), "%s | %ld/%ld",
            E.syntax ? E.syntax->filetype : "no ft", E.y + 1, E.lines);

    if (len > E.gw + E.w) {
        len = E.gw + E.w;
    }

    ab_append(ab, status, len);

    while (len < E.gw + E.w) {
        if (E.gw + E.w - len == mlen) {
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

    if (len > E.gw + E.w) {
        len = E.gw + E.w;
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
            (E.y - E.rowoff) + 1, (E.rx - E.coloff) + E.gw + 1);
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

char *prompt(char *message, void (*callback)(char *, uint16_t)) {
    size_t size = 128;
    size_t len = 0;
    char *buf = calloc(size, sizeof(char));

    while (true) {
        set_message(message, buf);
        refresh_screen();

        uint16_t key = read_key();

        if (key == ESCAPE) {
            set_message("");

            if (callback) {
                callback(buf, key);
            }

            free(buf);
            return NULL;
        } else if (key == BACKSPACE || key == CTRL_KEY('h') || key == DELETE) {
            if (len != 0) {
                buf[--len] = '\0';
            }
        } else if (key == ENTER) {
            if (len != 0) {
                set_message("");

                if (callback) {
                    callback(buf, key);
                }

                return buf;
            }
        } else if (!iscntrl(key) && key < 128) {
            if (len == size - 1) {
                size *= 2;
                buf = realloc(buf, size);
            }

            buf[len++] = key;
            buf[len] = '\0';
        }

        if (callback) {
            callback(buf, key);
        }
    }
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
    static uint8_t quit_times = NIM_QUIT_TIMES;
    uint16_t c = read_key();

    switch (c) {
        case ENTER:
            insert_newline();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DELETE:
            if (c == DELETE) {
                move_cursor(ARROW_RIGHT);
            }

            delete_char();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                set_message("WARNING! File has unsaved changes (%d more time%s...)",
                        quit_times, quit_times > 1 ? "s" : "");
                quit_times--;
                return;
            }

            clear_screen();
            exit(0);
            break;

        case CTRL_KEY('s'):
            save_file();
            break;

        case CTRL_KEY('f'):
            start_find();
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;

        case HOME:
            E.x = 0;
            break;

        case END:
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

        case CTRL_KEY('l'):
        case ESCAPE:
            break;

        default:
            insert_char(c);
            break;
    }

    quit_times = NIM_QUIT_TIMES;
}

void init() {
    E.x = 0;
    E.y = 0;
    E.gw = 0;
    E.rx = 0;
    E.filename = NULL;
    E.dirty = false;
    E.rows = NULL;
    E.lines = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.message[0] = '\0';
    E.timestamp = 0;
    E.syntax = NULL;

    if (get_screen_size(&E.h, &E.w) == -1) {
        die("get_size");
    }

    E.h -= 2;
}

int main(int argc, char **argv) {
    enable_raw_mode();
    init();

    if (argc >= 2) {
        open_file(argv[1]);
    }

    set_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (true) {
        refresh_screen();
        process_key();
    }

    return 0;
}
