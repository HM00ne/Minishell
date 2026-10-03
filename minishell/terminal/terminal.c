#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "proc.h"

#define MAX_SCROLLBACK 5000
#define MAX_LINE_LEN   65536
#define MAX_INPUT      1024
#define MAX_HISTORY    200
static float dpi = 1.0f;
#define S(x)           ((int)((x) * dpi + 0.5f))
#define PAD            S(12)
#define BAR_H          S(30)
#define UI_FONT        S(15)
#define FONT_PATH      "C:/Windows/Fonts/consola.ttf"
#define DEFAULT_FONT_SIZE S(18)

static const Color palette[17] = {
    {220, 223, 228, 255},
    { 92,  99, 112, 255}, {224, 108, 117, 255}, {152, 195, 121, 255}, {229, 192, 123, 255},
    { 97, 175, 239, 255}, {198, 120, 221, 255}, { 86, 182, 194, 255}, {200, 204, 212, 255},
    {127, 132, 142, 255}, {255, 123, 134, 255}, {175, 225, 140, 255}, {255, 214, 140, 255},
    {120, 195, 255, 255}, {220, 145, 240, 255}, {110, 210, 222, 255}, {255, 255, 255, 255},
};
#define C_DEFAULT 0
#define C_RED     2
#define C_YELLOW  4
#define C_INPUT   16

static const Color COL_BG     = { 30,  33,  39, 255};
static const Color COL_BAR    = { 22,  24,  29, 255};
static const Color COL_DIM    = {120, 126, 138, 255};
static const Color COL_CURSOR = { 97, 175, 239, 255};
static const Color COL_SCROLL = { 70,  76,  88, 255};

typedef struct {
    char *ch;
    unsigned char *col;
    int len, cap;
} Line;

static Line lines[MAX_SCROLLBACK];
static int line_start = 0;
static int line_count = 0;
static int cur_x = 0;

static Line *line_at(int i)
{
    return &lines[(line_start + i) % MAX_SCROLLBACK];
}

static void new_line(void)
{
    if (line_count == MAX_SCROLLBACK) {
        line_start = (line_start + 1) % MAX_SCROLLBACK;
        line_at(line_count - 1)->len = 0;
    } else {
        line_at(line_count)->len = 0;
        line_count++;
    }
    cur_x = 0;
}

static void ensure_cap(Line *l, int need)
{
    if (need <= l->cap)
        return;
    int cap = l->cap ? l->cap * 2 : 128;
    while (cap < need)
        cap *= 2;
    l->ch = realloc(l->ch, cap);
    l->col = realloc(l->col, cap);
    l->cap = cap;
}

static void put_cell(char c, unsigned char color)
{
    if (cur_x >= MAX_LINE_LEN)
        new_line();
    Line *l = line_at(line_count - 1);
    ensure_cap(l, cur_x + 1);
    while (l->len < cur_x) {
        l->ch[l->len] = ' ';
        l->col[l->len] = C_DEFAULT;
        l->len++;
    }
    l->ch[cur_x] = c;
    l->col[cur_x] = color;
    if (cur_x == l->len)
        l->len++;
    cur_x++;
}

static void clear_all(void)
{
    line_start = 0;
    line_count = 1;
    lines[0].len = 0;
    cur_x = 0;
}

static void clear_keep_last(void)
{
    Line last = *line_at(line_count - 1);
    Line *slot = line_at(line_count - 1);
    *slot = lines[0];
    lines[0] = last;
    line_start = 0;
    line_count = 1;
}

enum { S_TEXT, S_ESC, S_CSI, S_OSC, S_OSC_ESC };
static int ansi_state = S_TEXT;
static char csi_buf[32];
static int csi_len = 0;
static int sgr_color = C_DEFAULT;
static int sgr_bold = 0;

static unsigned char current_color(void)
{
    if (sgr_bold && sgr_color >= 1 && sgr_color <= 8)
        return (unsigned char)(sgr_color + 8);
    if (sgr_bold && sgr_color == C_DEFAULT)
        return C_INPUT;
    return (unsigned char)sgr_color;
}

static void handle_sgr(void)
{
    char *p = csi_buf;
    if (*p == '\0') {
        sgr_color = C_DEFAULT;
        sgr_bold = 0;
        return;
    }
    while (*p) {
        int n = (int)strtol(p, &p, 10);
        if (n == 0)                    { sgr_color = C_DEFAULT; sgr_bold = 0; }
        else if (n == 1)               sgr_bold = 1;
        else if (n == 22)              sgr_bold = 0;
        else if (n >= 30 && n <= 37)   sgr_color = 1 + (n - 30);
        else if (n == 39)              sgr_color = C_DEFAULT;
        else if (n >= 90 && n <= 97)   sgr_color = 9 + (n - 90);
        else if (n == 38 || n == 48)   return;
        if (*p == ';')
            p++;
        else if (*p)
            return;
    }
}

static void handle_csi(char final)
{
    csi_buf[csi_len] = '\0';
    if (final == 'm') {
        handle_sgr();
    } else if (final == 'J') {
        int n = atoi(csi_buf);
        if (n == 2 || n == 3)
            clear_all();
    } else if (final == 'K') {
        Line *l = line_at(line_count - 1);
        if (l->len > cur_x)
            l->len = cur_x;
    }
}

static void feed_byte(unsigned char c)
{
    switch (ansi_state) {
    case S_TEXT:
        if (c == 0x1b)                  ansi_state = S_ESC;
        else if (c == '\n')             new_line();
        else if (c == '\r')             cur_x = 0;
        else if (c == '\b')             { if (cur_x > 0) cur_x--; }
        else if (c == '\t')             { do put_cell(' ', C_DEFAULT); while (cur_x % 8); }
        else if (c >= 32 && c < 127)    put_cell((char)c, current_color());
        else if (c >= 0xC0)             put_cell('?', current_color());
        break;
    case S_ESC:
        if (c == '[')      { ansi_state = S_CSI; csi_len = 0; }
        else if (c == ']') ansi_state = S_OSC;
        else               ansi_state = S_TEXT;
        break;
    case S_CSI:
        if (c >= 0x40 && c <= 0x7e) {
            handle_csi((char)c);
            ansi_state = S_TEXT;
        } else if (csi_len < (int)sizeof csi_buf - 1) {
            csi_buf[csi_len++] = (char)c;
        }
        break;
    case S_OSC:
        if (c == 7)         ansi_state = S_TEXT;
        else if (c == 0x1b) ansi_state = S_OSC_ESC;
        break;
    case S_OSC_ESC:
        ansi_state = S_TEXT;
        break;
    }
}

static void feed(const char *data, int n)
{
    for (int i = 0; i < n; i++)
        feed_byte((unsigned char)data[i]);
}

static void feed_str(const char *s)
{
    feed(s, (int)strlen(s));
}

static int alive = 0;

static void start_shell(void)
{
    char exe[1024], err[1200];

    ansi_state = S_TEXT;
    sgr_color = C_DEFAULT;
    sgr_bold = 0;

    if (proc_find_shell(exe, sizeof exe) != 0) {
        feed_str("\x1b[31mminishell.exe was not found next to terminal.exe.\x1b[0m\n"
                 "Build both with build.bat, then press Enter to try again.\n");
        return;
    }
    if (proc_start(exe, err, sizeof err) != 0) {
        feed_str("\x1b[31m");
        feed_str(err);
        feed_str("\x1b[0m\nPress Enter to try again.\n");
        return;
    }
    alive = 1;
}

static void pump_output(void)
{
    char buf[8192];
    int total = 0;
    while (total < 256 * 1024) {
        int n = proc_read(buf, sizeof buf);
        if (n <= 0)
            break;
        feed(buf, n);
        total += n;
    }
}

static void check_alive(void)
{
    int code;
    if (!alive || proc_running(&code))
        return;

    pump_output();
    proc_stop();
    alive = 0;

    char msg[160];
    snprintf(msg, sizeof msg,
             "%s\x1b[33m[process exited with code %d - press Enter to restart, Esc to close]\x1b[0m\n",
             cur_x > 0 ? "\n" : "", code);
    feed_str(msg);
}

static char input[MAX_INPUT];
static int in_len = 0, in_cur = 0;

static char *history[MAX_HISTORY];
static int hist_count = 0, hist_pos = 0;
static char draft[MAX_INPUT];

static int scroll = 0;
static double last_key_time = 0;

static void set_input(const char *s)
{
    strncpy(input, s, MAX_INPUT - 1);
    input[MAX_INPUT - 1] = '\0';
    in_len = in_cur = (int)strlen(input);
}

static void insert_char(char c)
{
    if (in_len >= MAX_INPUT - 1)
        return;
    memmove(input + in_cur + 1, input + in_cur, in_len - in_cur);
    input[in_cur++] = c;
    in_len++;
}

static void add_history(void)
{
    if (in_len == 0)
        return;
    input[in_len] = '\0';
    if (hist_count > 0 && strcmp(history[hist_count - 1], input) == 0)
        return;
    if (hist_count == MAX_HISTORY) {
        free(history[0]);
        memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        hist_count--;
    }
    history[hist_count++] = strdup(input);
}

static void submit_line(void)
{
    for (int i = 0; i < in_len; i++)
        put_cell(input[i], C_INPUT);
    new_line();

    add_history();
    proc_write(input, in_len);
    proc_write("\n", 1);

    in_len = in_cur = 0;
    hist_pos = hist_count;
}

static void history_move(int dir)
{
    if (hist_count == 0)
        return;
    if (hist_pos == hist_count) {
        input[in_len] = '\0';
        strcpy(draft, input);
    }
    hist_pos += dir;
    if (hist_pos < 0)
        hist_pos = 0;
    if (hist_pos >= hist_count) {
        hist_pos = hist_count;
        set_input(draft);
    } else {
        set_input(history[hist_pos]);
    }
}

static void paste(void)
{
    const char *text = GetClipboardText();
    if (text == NULL)
        return;
    for (const char *p = text; *p; p++) {
        if (*p == '\n')
            submit_line();
        else if (*p == '\t')
            insert_char(' ');
        else if ((unsigned char)*p >= 32 && (unsigned char)*p < 127)
            insert_char(*p);
    }
}

static Font font, ui_font;
static int font_size = 18;
static float cell_w, cell_h;
static int cols = 80, vis_rows = 24, total_rows = 0;

static Font load_mono(int size)
{
    if (FileExists(FONT_PATH))
        return LoadFontEx(FONT_PATH, size, NULL, 0);
    return GetFontDefault();
}

static void load_fonts(void)
{
    UnloadFont(font);
    font = load_mono(font_size);
    cell_w = MeasureTextEx(font, "M", (float)font_size, 0).x;
    cell_h = roundf(font_size * 1.3f);
}

static int key_hit(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

static int line_vis_len(int i)
{
    int n = line_at(i)->len;
    if (i == line_count - 1 && alive) {
        int end = cur_x + in_len + 1;
        if (end > n)
            n = end;
    }
    return n;
}

static int rows_for(int len)
{
    return len <= 0 ? 1 : (len + cols - 1) / cols;
}

static void compute_layout(void)
{
    float text_w = GetScreenWidth() - 2 * PAD - 8;
    float text_h = GetScreenHeight() - BAR_H - PAD;
    cols = (int)(text_w / cell_w);
    if (cols < 1)
        cols = 1;
    vis_rows = (int)(text_h / cell_h);
    if (vis_rows < 1)
        vis_rows = 1;

    total_rows = 0;
    for (int i = 0; i < line_count; i++)
        total_rows += rows_for(line_vis_len(i));

    int max_scroll = total_rows - vis_rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (scroll > max_scroll)
        scroll = max_scroll;
    if (scroll < 0)
        scroll = 0;
}

static void handle_input(void)
{
    int ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int alt   = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int typed = 0;

    if (ctrl && (key_hit(KEY_EQUAL) || key_hit(KEY_KP_ADD)) && font_size < S(40)) {
        font_size += 2;
        load_fonts();
    }
    if (ctrl && (key_hit(KEY_MINUS) || key_hit(KEY_KP_SUBTRACT)) && font_size > S(10)) {
        font_size -= 2;
        load_fonts();
    }
    if (ctrl && IsKeyPressed(KEY_ZERO)) {
        font_size = DEFAULT_FONT_SIZE;
        load_fonts();
    }

    float wheel = GetMouseWheelMove();
    if (wheel != 0)
        scroll += (int)(wheel * 3);
    if (key_hit(KEY_PAGE_UP))           scroll += vis_rows - 1;
    if (key_hit(KEY_PAGE_DOWN))         scroll -= vis_rows - 1;
    if (shift && key_hit(KEY_UP))       scroll += 1;
    if (shift && key_hit(KEY_DOWN))     scroll -= 1;

    if (ctrl && IsKeyPressed(KEY_L))
        clear_keep_last();

    if (!alive) {
        while (GetCharPressed() != 0)
            ;
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            feed_str("\n");
            start_shell();
            scroll = 0;
        }
        return;
    }

    int c;
    while ((c = GetCharPressed()) != 0) {
        if (ctrl && !alt)
            continue;
        if (c >= 32 && c < 127) {
            insert_char((char)c);
            typed = 1;
        }
    }

    if (ctrl && !alt) {
        if (IsKeyPressed(KEY_C)) {
            for (int i = 0; i < in_len; i++)
                put_cell(input[i], C_INPUT);
            feed_str("\x1b[31m^C\x1b[0m");
            in_len = in_cur = 0;
            hist_pos = hist_count;
            proc_interrupt();
            typed = 1;
        }
        if (IsKeyPressed(KEY_D)) {
            if (in_len == 0) {
                proc_close_input();
            } else if (in_cur < in_len) {
                memmove(input + in_cur, input + in_cur + 1, in_len - in_cur - 1);
                in_len--;
            }
            typed = 1;
        }
        if (IsKeyPressed(KEY_U)) { in_len = in_cur = 0; typed = 1; }
        if (IsKeyPressed(KEY_V)) { paste(); typed = 1; }
        if (IsKeyPressed(KEY_A)) { in_cur = 0; typed = 1; }
        if (IsKeyPressed(KEY_E)) { in_cur = in_len; typed = 1; }
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        submit_line();
        typed = 1;
    }
    if (key_hit(KEY_BACKSPACE) && in_cur > 0) {
        memmove(input + in_cur - 1, input + in_cur, in_len - in_cur);
        in_cur--;
        in_len--;
        typed = 1;
    }
    if (key_hit(KEY_DELETE) && in_cur < in_len) {
        memmove(input + in_cur, input + in_cur + 1, in_len - in_cur - 1);
        in_len--;
        typed = 1;
    }
    if (key_hit(KEY_LEFT) && in_cur > 0)       { in_cur--; typed = 1; }
    if (key_hit(KEY_RIGHT) && in_cur < in_len) { in_cur++; typed = 1; }
    if (IsKeyPressed(KEY_HOME))                { in_cur = 0; typed = 1; }
    if (IsKeyPressed(KEY_END))                 { in_cur = in_len; typed = 1; }
    if (!shift && key_hit(KEY_UP))             { history_move(-1); typed = 1; }
    if (!shift && key_hit(KEY_DOWN))           { history_move(+1); typed = 1; }
    if (IsKeyPressed(KEY_ESCAPE))              { in_len = in_cur = 0; typed = 1; }

    if (typed) {
        scroll = 0;
        last_key_time = GetTime();
    }
}

static void draw_cell(float x, float y, char c, Color color)
{
    if (c == ' ')
        return;
    Vector2 pos = { floorf(x), floorf(y + (cell_h - font_size) / 2) };
    DrawTextCodepoint(font, c, pos, (float)font_size, color);
}

static void draw_text_area(void)
{
    float x0 = PAD, y0 = BAR_H + PAD / 2;
    int first = total_rows - vis_rows - scroll;
    if (first < 0)
        first = 0;

    int focused = IsWindowFocused();
    int blink_on = (GetTime() - last_key_time) < 0.5 || fmod(GetTime(), 1.0) < 0.6;

    int row = 0;
    for (int i = 0; i < line_count; i++) {
        int len = line_vis_len(i);
        int r = rows_for(len);
        if (row + r <= first) {
            row += r;
            continue;
        }
        if (row >= first + vis_rows)
            break;

        Line *l = line_at(i);
        int is_last = (i == line_count - 1);

        for (int k = 0; k < len; k++) {
            int sr = row + k / cols - first;
            if (sr < 0)
                continue;
            if (sr >= vis_rows)
                break;
            float x = x0 + (k % cols) * cell_w;
            float y = y0 + sr * cell_h;

            char c = ' ';
            unsigned char color = C_DEFAULT;
            if (is_last && alive && k >= cur_x && k < cur_x + in_len) {
                c = input[k - cur_x];
                color = C_INPUT;
            } else if (k < l->len) {
                c = l->ch[k];
                color = l->col[k];
            }

            if (is_last && alive && k == cur_x + in_cur) {
                if (!focused) {
                    DrawRectangleLines((int)x, (int)y, (int)ceilf(cell_w), (int)cell_h, COL_CURSOR);
                } else if (blink_on) {
                    DrawRectangle((int)x, (int)y, (int)ceilf(cell_w), (int)cell_h, COL_CURSOR);
                    draw_cell(x, y, c, COL_BG);
                    continue;
                }
            }
            draw_cell(x, y, c, palette[color]);
        }
        row += r;
    }

    if (total_rows > vis_rows) {
        float track_h = GetScreenHeight() - BAR_H - 8;
        float thumb_h = track_h * vis_rows / total_rows;
        if (thumb_h < 24)
            thumb_h = 24;
        float pos = (float)first / (total_rows - vis_rows);
        float y = BAR_H + 4 + (track_h - thumb_h) * pos;
        DrawRectangleRounded((Rectangle){ GetScreenWidth() - 9.0f, y, 5, thumb_h },
                             1.0f, 4, COL_SCROLL);
    }
}

static void draw_bar(void)
{
    int w = GetScreenWidth();
    DrawRectangle(0, 0, w, BAR_H, COL_BAR);
    DrawCircle(S(18), BAR_H / 2, S(5), alive ? palette[3] : palette[2]);

    float ty = (BAR_H - UI_FONT) / 2.0f;
    DrawTextEx(ui_font, alive ? "minishell" : "minishell (stopped)",
               (Vector2){ S(32), ty }, UI_FONT, 0, palette[0]);

    const char *hint = "Ctrl+C interrupt   Ctrl+D exit   Ctrl+L clear   Ctrl +/- zoom";
    Vector2 size = MeasureTextEx(ui_font, hint, UI_FONT, 0);
    if (w - size.x - S(16) > S(200))
        DrawTextEx(ui_font, hint, (Vector2){ w - size.x - S(14), ty }, UI_FONT, 0, COL_DIM);

    if (scroll > 0) {
        const char *s = "scrolled - type to return";
        Vector2 ss = MeasureTextEx(ui_font, s, UI_FONT, 0);
        DrawTextEx(ui_font, s, (Vector2){ (w - ss.x) / 2, ty }, UI_FONT, 0, palette[4]);
    }
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(980, 620, "Minishell Terminal");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    dpi = GetWindowScaleDPI().x;
    if (dpi < 1.0f)
        dpi = 1.0f;
    if (dpi > 1.0f) {
        int m = GetCurrentMonitor();
        int mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);
        int w = S(980) < mw * 9 / 10 ? S(980) : mw * 9 / 10;
        int h = S(620) < mh * 8 / 10 ? S(620) : mh * 8 / 10;
        SetWindowSize(w, h);
        SetWindowPosition((mw - w) / 2, (mh - h) / 2);
    }
    SetWindowMinSize(S(420), S(260));
    font_size = DEFAULT_FONT_SIZE;

    font = GetFontDefault();
    load_fonts();
    ui_font = load_mono(UI_FONT);

    new_line();
    start_shell();

    while (!WindowShouldClose()) {
        if (alive)
            pump_output();
        check_alive();

        if (!alive && IsKeyPressed(KEY_ESCAPE))
            break;
        handle_input();
        compute_layout();

        BeginDrawing();
        ClearBackground(COL_BG);
        draw_text_area();
        draw_bar();
        EndDrawing();
    }

    proc_stop();
    UnloadFont(font);
    UnloadFont(ui_font);
    CloseWindow();
    return 0;
}
