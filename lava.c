#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdint.h>

/* ANSI escape-последовательности */
#define RESET_SEQ        "\033[0m"
#define HIDE_CURSOR_SEQ  "\033[?25l"
#define SHOW_CURSOR_SEQ  "\033[?25h"
#define HOME_SEQ         "\033[H"
#define BG_SEQ           "\033[48;2;8;0;0m"

#define TARGET_FPS 30.0

/* На сколько уровней квантуем цвет */
#define COLOR_STEPS 64

/* Размер LUT для sin() */
#define SIN_LUT_BITS 12
#define SIN_LUT_SIZE (1 << SIN_LUT_BITS)
#define SIN_LUT_MASK (SIN_LUT_SIZE - 1)
#define PHASE_SCALE ((double)SIN_LUT_SIZE / (2.0 * M_PI))

#define FIXED_ONE 65536  /* Q16.16 фиксированная точка */

/* Адаптивный downsampling */
#define MAX_BLOCK_SIZE 8
#define SLOW_STREAK_TO_GROW 3    /* столько медленных кадров подряд -> увеличить блок */
#define FAST_STREAK_TO_SHRINK 30 /* столько быстрых кадров подряд -> уменьшить блок */

static int32_t SIN_LUT[SIN_LUT_SIZE];

static const char *PALETTE_GLYPHS[] = {
    " ", ".", "\xE2\x96\x91", "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x96\x88"
};
#define PALETTE_LEN 6

typedef struct {
    char   seq[24];
    int    seq_len;
    const char *glyph;
    int    glyph_len;
} ColorEntry;

static ColorEntry COLOR_LUT[COLOR_STEPS];

static volatile int g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void get_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

static inline int write_uint8(char *dst, int v) {
    if (v >= 100) {
        dst[0] = '0' + (v / 100);
        dst[1] = '0' + ((v / 10) % 10);
        dst[2] = '0' + (v % 10);
        return 3;
    } else if (v >= 10) {
        dst[0] = '0' + (v / 10);
        dst[1] = '0' + (v % 10);
        return 2;
    } else {
        dst[0] = '0' + v;
        return 1;
    }
}

static void build_sin_lut(void) {
    for (int i = 0; i < SIN_LUT_SIZE; i++) {
        double angle = (2.0 * M_PI * i) / SIN_LUT_SIZE;
        SIN_LUT[i] = (int32_t)lround(sin(angle) * FIXED_ONE);
    }
}

static inline int32_t fast_sin(int32_t phase_ticks) {
    return SIN_LUT[phase_ticks & SIN_LUT_MASK];
}

static void build_color_lut(void) {
    for (int i = 0; i < COLOR_STEPS; i++) {
        double heat = (double)i / (double)(COLOR_STEPS - 1);

        int r = (int)(80.0 + 175.0 * heat);
        double heat2 = heat * heat;
        int g = (int)(20.0 + 120.0 * heat2);
        int b = (int)(10.0 + 40.0 * heat2 * heat);

        int glyph_idx = (int)(heat * PALETTE_LEN);
        if (glyph_idx > PALETTE_LEN - 1) glyph_idx = PALETTE_LEN - 1;

        ColorEntry *e = &COLOR_LUT[i];
        int n = 0;
        memcpy(e->seq, "\033[38;2;", 7); n += 7;
        n += write_uint8(e->seq + n, r);
        e->seq[n++] = ';';
        n += write_uint8(e->seq + n, g);
        e->seq[n++] = ';';
        n += write_uint8(e->seq + n, b);
        e->seq[n++] = 'm';
        e->seq_len = n;

        e->glyph = PALETTE_GLYPHS[glyph_idx];
        e->glyph_len = (int)strlen(e->glyph);
    }
}

/* LUT, зависящие от размера терминала */
static int32_t *DIST_PHASE = NULL;
static int32_t *ROW_X_PHASE = NULL;
static int32_t *ROW_XY_PHASE = NULL;

static size_t g_lut_capacity = 0;

static void fail_and_exit(int code) {
    ssize_t _r = write(STDOUT_FILENO, SHOW_CURSOR_SEQ, strlen(SHOW_CURSOR_SEQ));
    (void)_r;
    _exit(code);
}

static void build_size_luts(int w, int h) {
    size_t cells = (size_t)w * (size_t)h;

    if (cells > g_lut_capacity) {
        free(DIST_PHASE);
        free(ROW_XY_PHASE);
        DIST_PHASE   = malloc(cells * sizeof(int32_t));
        ROW_XY_PHASE = malloc(cells * sizeof(int32_t));
        if (!DIST_PHASE || !ROW_XY_PHASE) fail_and_exit(1);
        g_lut_capacity = cells;
    }

    free(ROW_X_PHASE);
    ROW_X_PHASE = malloc((size_t)w * sizeof(int32_t));
    if (!ROW_X_PHASE) fail_and_exit(1);

    for (int x = 0; x < w; x++) {
        ROW_X_PHASE[x] = (int32_t)lround(x * 0.08 * PHASE_SCALE);
    }

    for (int y = 0; y < h; y++) {
        double dy = (double)y - 12.0;
        double dy2 = dy * dy;
        for (int x = 0; x < w; x++) {
            double dx = (double)x - 40.0;
            double dist = sqrt(dx * dx + dy2);
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            DIST_PHASE[idx]   = (int32_t)lround(dist * 0.12 * PHASE_SCALE);
            ROW_XY_PHASE[idx] = (int32_t)lround((x + y) * 0.07 * PHASE_SCALE);
        }
    }
}

/* Цвет для конкретного (x,y) при текущих тиках времени.
 * sin_y_t уже посчитан вызывающей стороной один раз на строку. */
static inline int compute_color_idx(int x, int y, int w,
                                     int32_t t_ticks_x, int32_t sin_y_t,
                                     int32_t t_ticks_xy, int32_t t_ticks_d) {
    size_t idx2d = (size_t)y * (size_t)w + (size_t)x;
    int32_t sin_x_t  = fast_sin(ROW_X_PHASE[x] + t_ticks_x);
    int32_t sin_xy_t = fast_sin(ROW_XY_PHASE[idx2d] + t_ticks_xy);
    int32_t sin_dist = fast_sin(DIST_PHASE[idx2d] - t_ticks_d);

    int32_t v_fixed = (sin_x_t + sin_y_t + sin_xy_t + sin_dist) >> 2;
    int32_t heat_fixed = (v_fixed + FIXED_ONE) >> 1;
    if (heat_fixed < 0) heat_fixed = 0;
    if (heat_fixed > FIXED_ONE) heat_fixed = FIXED_ONE;

    return (int)(((int64_t)heat_fixed * (COLOR_STEPS - 1)) / FIXED_ONE);
}

int main(void) {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    build_sin_lut();
    build_color_lut();

    if (write(STDOUT_FILENO, HIDE_CURSOR_SEQ, strlen(HIDE_CURSOR_SEQ)) < 0) return 1;
    /* фон — один раз за весь сеанс, не на каждой строке каждого кадра */
    if (write(STDOUT_FILENO, BG_SEQ, strlen(BG_SEQ)) < 0) return 1;

    const double frame_dt = 1.0 / TARGET_FPS;

    const int32_t T_STEP_09 = (int32_t)lround(0.08 * 1.3 * PHASE_SCALE);
    const int32_t T_STEP_Y  = (int32_t)lround(0.08 * 0.9 * PHASE_SCALE);
    const int32_t T_STEP_XY = (int32_t)lround(0.08 * 0.7 * PHASE_SCALE);
    const int32_t T_STEP_D  = (int32_t)lround(0.08 * 1.0 * PHASE_SCALE);

    int32_t t_ticks_x  = 0;
    int32_t t_ticks_y  = 0;
    int32_t t_ticks_xy = 0;
    int32_t t_ticks_d  = 0;

    /* основной буфер кадра */
    size_t buf_cap = 0;
    char *buf = NULL;

    /* буфер одной собранной строки (без HOME, без финального RESET) —
     * нужен, чтобы при block_size > 1 просто продублировать готовую
     * строку несколько раз вместо пересчёта цвета */
    size_t row_buf_cap = 0;
    char *row_buf = NULL;

    int prev_w = -1, prev_h = -1;

    int block_size = 1;
    int slow_frame_streak = 0;
    int fast_frame_streak = 0;

    /* время следующего кадра — без накопления долга */
    struct timespec next_frame_time;
    clock_gettime(CLOCK_MONOTONIC, &next_frame_time);

    while (g_running) {
        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        int w, h;
        get_size(&w, &h);

        if (w != prev_w || h != prev_h) {
            size_t need = (size_t)w * (size_t)h * 32 + (size_t)h * 8 + 64;
            if (need > buf_cap) {
                free(buf);
                buf = malloc(need);
                if (!buf) fail_and_exit(1);
                buf_cap = need;
            }
            size_t row_need = (size_t)w * 32 + 64;
            if (row_need > row_buf_cap) {
                free(row_buf);
                row_buf = malloc(row_need);
                if (!row_buf) fail_and_exit(1);
                row_buf_cap = row_need;
            }
            build_size_luts(w, h);
            prev_w = w;
            prev_h = h;
            block_size = 1;
            slow_frame_streak = 0;
            fast_frame_streak = 0;
        }

        char *p = buf;
        memcpy(p, HOME_SEQ, strlen(HOME_SEQ)); p += strlen(HOME_SEQ);

        for (int y = 0; y < h; y += block_size) {
            int32_t y_phase = (int32_t)lround(y * 0.15 * PHASE_SCALE) - t_ticks_y;
            int32_t sin_y_t = fast_sin(y_phase);

            int rows_in_block = block_size;
            if (y + rows_in_block > h) rows_in_block = h - y;

            /* собираем одну строку в row_buf */
            char *rp = row_buf;
            int prev_color_idx = -1;

            for (int x = 0; x < w; x += block_size) {
                int color_idx = compute_color_idx(x, y, w, t_ticks_x, sin_y_t,
                                                   t_ticks_xy, t_ticks_d);

                int cols_in_block = block_size;
                if (x + cols_in_block > w) cols_in_block = w - x;

                ColorEntry *e = &COLOR_LUT[color_idx];

                if (color_idx != prev_color_idx) {
                    memcpy(rp, e->seq, e->seq_len);
                    rp += e->seq_len;
                    prev_color_idx = color_idx;
                }
                for (int k = 0; k < cols_in_block; k++) {
                    memcpy(rp, e->glyph, e->glyph_len);
                    rp += e->glyph_len;
                }
            }

            size_t row_len = (size_t)(rp - row_buf);

            /* пишем собранную строку в буфер кадра столько раз, сколько
             * строк высоты в текущем блоке — без пересчёта цвета */
            for (int r = 0; r < rows_in_block; r++) {
                memcpy(p, row_buf, row_len);
                p += row_len;
                int is_last_row = (y + r == h - 1);
                if (!is_last_row) {
                    *p++ = '\n';
                }
            }
        }

        memcpy(p, RESET_SEQ, strlen(RESET_SEQ)); p += strlen(RESET_SEQ);

        if (write(STDOUT_FILENO, buf, (size_t)(p - buf)) < 0) break;

        t_ticks_x  += T_STEP_09;
        t_ticks_y  += T_STEP_Y;
        t_ticks_xy += T_STEP_XY;
        t_ticks_d  += T_STEP_D;

        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double frame_cost = (t_end.tv_sec - t_start.tv_sec) +
                             (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

        /* подстраиваем block_size под реальный темп */
        if (frame_cost > frame_dt) {
            slow_frame_streak++;
            fast_frame_streak = 0;
            if (slow_frame_streak >= SLOW_STREAK_TO_GROW && block_size < MAX_BLOCK_SIZE) {
                block_size++;
                slow_frame_streak = 0;
            }
        } else if (frame_cost < frame_dt * 0.5) {
            fast_frame_streak++;
            slow_frame_streak = 0;
            if (fast_frame_streak >= FAST_STREAK_TO_SHRINK && block_size > 1) {
                block_size--;
                fast_frame_streak = 0;
            }
        } else {
            slow_frame_streak = 0;
            fast_frame_streak = 0;
        }

        /* стабильный темп без накопления долга по времени */
        next_frame_time.tv_nsec += (long)(frame_dt * 1e9);
        while (next_frame_time.tv_nsec >= 1000000000L) {
            next_frame_time.tv_nsec -= 1000000000L;
            next_frame_time.tv_sec += 1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double until_next = (next_frame_time.tv_sec - now.tv_sec) +
                             (next_frame_time.tv_nsec - now.tv_nsec) / 1e9;

        if (until_next > 0) {
            struct timespec ts;
            ts.tv_sec = (time_t)until_next;
            ts.tv_nsec = (long)((until_next - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        } else {
            /* опоздали — не копим долг, просто переставляем точку
             * отсчёта на "сейчас" */
            next_frame_time = now;
        }
    }

    free(buf);
    free(row_buf);
    free(DIST_PHASE);
    free(ROW_XY_PHASE);
    free(ROW_X_PHASE);

    ssize_t r1 = write(STDOUT_FILENO, SHOW_CURSOR_SEQ, strlen(SHOW_CURSOR_SEQ));
    ssize_t r2 = write(STDOUT_FILENO, RESET_SEQ, strlen(RESET_SEQ));
    ssize_t r3 = write(STDOUT_FILENO, "\n", 1);
    (void)r1; (void)r2; (void)r3;
    return 0;
}
