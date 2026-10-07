/* webterm.c : a jsh terminal over the draw channel.
 *
 * The screen is a grid of monospace text rows (one draw text node each, since a
 * single text node renders as one line). A console sink mirrors everything jsh
 * and its commands print into the grid, so output appears on screen as well as
 * on the host console. Keystrokes arrive as DEV_TEXT / DEV_PASTE draw events and
 * edit the bottom row; Enter hands the line to jsh_exec_line, whose output the
 * sink captures, and then a new prompt is drawn.
 *
 * It is a single task. While a command runs, jsh_exec_line blocks (a program
 * runs in a child task that the shell awaits), so no new keystrokes are read
 * until it finishes; its output is redrawn when it returns. That matches a plain
 * terminal running a foreground command and keeps draw submission to one task. */
#include "webterm.h"

#include "draw.h"
#include "jsh.h"
#include "print.h"
#include "string.h"

#define COLS    72
#define ROWS    24
#define FONTSZ  13
#define LINEH   17
#define MARGIN  8
#define TOP     16
#define SURF_W  (MARGIN * 2 + COLS * 8)
#define SURF_H  (TOP + ROWS * LINEH + 6)

#define NODE_SURF   1
#define NODE_BG     2
#define NODE_ROW0   3           /* row r is NODE_ROW0 + r */

/* The screen grid: ROWS lines, each a NUL-terminated string of up to COLS
 * characters. curcol is the cursor column in the bottom (active) row, and
 * input_start is where the current input begins (just after the prompt). */
static char grid[ROWS][COLS + 1];
static int  curcol;
static int  input_start;

static const struct draw_style bg_style = {
    .mask = DST_FILL,
    .fill = 0x101418,
};
static const struct draw_style text_style = {
    .mask = DST_FILL | DST_FONT,
    .fill = 0x33ff66,
    .font = 2,                  /* monospace */
};

/* Scroll the grid up by one row, clearing the new bottom row. */
static void
scroll_up(void)
{
    memmove(grid[0], grid[1], (size_t)(ROWS - 1) * (COLS + 1));
    memset(grid[ROWS - 1], 0, COLS + 1);
    curcol = 0;
}

/* Place one output byte into the bottom row, wrapping and scrolling as needed.
 * Control bytes other than newline and carriage return are dropped. */
static void
put_char(char c)
{
    if (c == '\n') {
        scroll_up();
        return;
    }
    if (c == '\r') {
        curcol = 0;
        return;
    }
    if ((unsigned char)c < 32)
        return;
    if (curcol >= COLS)
        scroll_up();
    grid[ROWS - 1][curcol++] = c;
    grid[ROWS - 1][curcol] = '\0';
}

/* The console sink: mirror every console byte into the grid. Called from the
 * terminal task and from command child tasks; it only mutates the grid, never
 * touches the draw channel, so drawing stays confined to the event loop. */
static void
term_sink(char c)
{
    put_char(c);
}

static void
prompt(void)
{
    put_char('$');
    put_char(' ');
    input_start = curcol;
}

/* Push the whole grid to the screen. */
static void
redraw(void)
{
    for (int r = 0; r < ROWS; r++)
        draw_set_text(NODE_ROW0 + r, grid[r]);
    draw_flush();
}

/* Run the current input line through jsh and start a fresh prompt. */
static void
submit(void)
{
    char cmd[COLS + 1];
    int j = 0;
    for (int i = input_start; i < COLS && grid[ROWS - 1][i]; i++)
        cmd[j++] = grid[ROWS - 1][i];
    cmd[j] = '\0';

    put_char('\n');             /* keep the typed line, output starts below */
    jsh_exec_line(cmd);         /* output arrives through term_sink */
    prompt();
}

/* Edit the input line with one received character. */
static void
input_char(char c)
{
    if (c == '\n' || c == '\r') {
        submit();
    } else if (c == '\b' || c == 0x7f) {
        if (curcol > input_start) {
            curcol--;
            grid[ROWS - 1][curcol] = '\0';
        }
    } else if ((unsigned char)c >= 32) {
        put_char(c);
    }
}

void
webterm(void)
{
    for (int r = 0; r < ROWS; r++)
        grid[r][0] = '\0';
    curcol = 0;
    input_start = 0;

    /* Build the screen: a window, a dark background, and one text node per row. */
    draw_surface(NODE_SURF, 0, 0, SURF_W, SURF_H, "jotun terminal");
    draw_rect(NODE_BG, NODE_SURF, 0, 0, SURF_W, SURF_H, 0, &bg_style);
    for (int r = 0; r < ROWS; r++)
        draw_text(NODE_ROW0 + r, NODE_SURF, MARGIN, TOP + r * LINEH,
                  FONTSZ, "", &text_style);
    draw_listen(NODE_SURF, DEV_CLOSE);

    /* Capture console output into the grid, then greet and prompt. */
    con_set_sink(term_sink);
    con_puts("jotun web terminal. type a command (try: ls, cat motd, hello)\n");
    prompt();
    redraw();

    for (;;) {
        struct draw_event ev;
        if (draw_next_event(&ev) != 0)
            break;
        if (ev.type == DEV_CLOSE)
            break;
        if (ev.type == DEV_TEXT || ev.type == DEV_PASTE) {
            for (int i = 0; i < ev.vlen; i++)
                input_char(ev.value[i]);
            redraw();
        }
    }

    con_set_sink(NULL);
}
