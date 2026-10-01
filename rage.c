#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#define CTRL(x) ((x) & 0x1f)

typedef struct {
    char **lines;
    int count;
} Editor;

static struct termios original;

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static void raw_mode(void)
{
    if (tcgetattr(STDIN_FILENO, &original) == -1)
        die("tcgetattr");

    struct termios raw = original;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
}

static int key(void)
{
    char c;

    if (read(STDIN_FILENO, &c, 1) != 1)
        exit(0);

    if (c != '\x1b')
        return c;

    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
        return '\x1b';

    if (read(STDIN_FILENO, &seq[1], 1) != 1)
        return '\x1b';

    if (seq[0] == '[') {
        switch (seq[1]) {
        case 'A': return 1000; /* up */
        case 'B': return 1001; /* down */
        case 'C': return 1002; /* right */
        case 'D': return 1003; /* left */
        }
    }

    return '\x1b';
}

static void clear_screen(void)
{
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

static void draw(Editor *e, int cx, int cy, const char *filename)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
        die("ioctl");

    clear_screen();

    for (int i = 0; i < e->count && i < ws.ws_row - 1; i++) {
        write(STDOUT_FILENO, e->lines[i], strlen(e->lines[i]));
        write(STDOUT_FILENO, "\r\n", 2);
    }

    char status[256];

    snprintf(status, sizeof(status),
             "\x1b[%d;1H"
             "%s | %d lines | Ctrl-S save | Ctrl-Q quit",
             ws.ws_row,
             filename ? filename : "[untitled]",
             e->count);

    write(STDOUT_FILENO, status, strlen(status));

    printf("\x1b[%d;%dH", cy + 1, cx + 1);
    fflush(stdout);
}

static void insert_char(Editor *e, int row, int col, char c)
{
    char *line = e->lines[row];
    size_t len = strlen(line);

    char *new_line = malloc(len + 2);
    if (!new_line)
        die("malloc");

    memcpy(new_line, line, col);
    new_line[col] = c;
    memcpy(new_line + col + 1, line + col, len - col + 1);

    free(line);
    e->lines[row] = new_line;
}

static void new_line(Editor *e, int row, int col)
{
    char *line = e->lines[row];
    size_t len = strlen(line);

    char *left = malloc(col + 1);
    char *right = malloc(len - col + 1);

    if (!left || !right)
        die("malloc");

    memcpy(left, line, col);
    left[col] = '\0';

    memcpy(right, line + col, len - col + 1);

    char **new_lines = realloc(
        e->lines,
        sizeof(char *) * (e->count + 1)
    );

    if (!new_lines)
        die("realloc");

    e->lines = new_lines;

    memmove(
        &e->lines[row + 2],
        &e->lines[row + 1],
        sizeof(char *) * (e->count - row - 1)
    );

    free(line);

    e->lines[row] = left;
    e->lines[row + 1] = right;
    e->count++;
}

static void delete_char(Editor *e, int *row, int *col)
{
    if (*col > 0) {
        char *line = e->lines[*row];
        size_t len = strlen(line);

        memmove(
            line + *col - 1,
            line + *col,
            len - *col + 1
        );

        (*col)--;
        return;
    }

    if (*row == 0)
        return;

    int previous = *row - 1;

    size_t a = strlen(e->lines[previous]);
    size_t b = strlen(e->lines[*row]);

    char *merged = realloc(
        e->lines[previous],
        a + b + 1
    );

    if (!merged)
        die("realloc");

    memcpy(merged + a, e->lines[*row], b + 1);

    free(e->lines[*row]);

    memmove(
        &e->lines[*row],
        &e->lines[*row + 1],
        sizeof(char *) * (e->count - *row - 1)
    );

    e->lines = realloc(
        e->lines,
        sizeof(char *) * (e->count - 1)
    );

    e->count--;

    e->lines[previous] = merged;
    *row = previous;
    *col = (int)a;
}

static void load_file(Editor *e, const char *filename)
{
    FILE *f = fopen(filename, "r");

    e->lines = NULL;
    e->count = 0;

    if (!f) {
        e->lines = malloc(sizeof(char *));
        if (!e->lines)
            die("malloc");

        e->lines[0] = strdup("");
        e->count = 1;
        return;
    }

    char *line = NULL;
    size_t cap = 0;

    while (getline(&line, &cap, f) != -1) {
        size_t len = strlen(line);

        while (len && (line[len - 1] == '\n' ||
                       line[len - 1] == '\r'))
            line[--len] = '\0';

        char **new_lines = realloc(
            e->lines,
            sizeof(char *) * (e->count + 1)
        );

        if (!new_lines)
            die("realloc");

        e->lines = new_lines;
        e->lines[e->count++] = strdup(line);
    }

    free(line);
    fclose(f);

    if (e->count == 0) {
        e->lines = malloc(sizeof(char *));
        if (!e->lines)
            die("malloc");

        e->lines[0] = strdup("");
        e->count = 1;
    }
}

static void save_file(Editor *e, const char *filename)
{
    if (!filename)
        return;

    FILE *f = fopen(filename, "w");

    if (!f)
        return;

    for (int i = 0; i < e->count; i++) {
        fprintf(f, "%s", e->lines[i]);

        if (i < e->count - 1)
            fputc('\n', f);
    }

    fclose(f);
}

static void free_editor(Editor *e)
{
    for (int i = 0; i < e->count; i++)
        free(e->lines[i]);

    free(e->lines);
}

int main(int argc, char **argv)
{
    const char *filename = argc > 1 ? argv[1] : NULL;

    Editor e;
    load_file(&e, filename);

    raw_mode();
    atexit(restore_terminal);

    int row = 0;
    int col = 0;

    for (;;) {
        draw(&e, col, row, filename);

        int k = key();

        switch (k) {
        case CTRL('q'):
            free_editor(&e);
            return 0;

        case CTRL('s'):
            save_file(&e, filename);
            break;

        case 1000: /* up */
            if (row > 0) {
                row--;

                int len = strlen(e.lines[row]);
                if (col > len)
                    col = len;
            }
            break;

        case 1001: /* down */
            if (row < e.count - 1) {
                row++;

                int len = strlen(e.lines[row]);
                if (col > len)
                    col = len;
            }
            break;

        case 1002: /* right */
            if (col < (int)strlen(e.lines[row])) {
                col++;
            } else if (row < e.count - 1) {
                row++;
                col = 0;
            }
            break;

        case 1003: /* left */
            if (col > 0) {
                col--;
            } else if (row > 0) {
                row--;
                col = strlen(e.lines[row]);
            }
            break;

        case '\r':
        case '\n':
            new_line(&e, row, col);
            row++;
            col = 0;
            break;

        case 127:
        case CTRL('h'):
            delete_char(&e, &row, &col);
            break;

        default:
            if (k >= 32 && k <= 126) {
                insert_char(&e, row, col, (char)k);
                col++;
            }
            break;
        }
    }
}
