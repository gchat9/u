#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

/*
 * CSI parameter accessor.
 * Returns the i-th parameter if it was explicitly set and non-zero,
 * otherwise returns `def`.  (VT100 convention: 0 == "use default".)
 */
#define P(i, def) ((i) < p->num_params && p->params[(i)] > 0 \
                   ? p->params[(i)] : (def))

/* ------------------------------------------------------------------ */
/* Cell / row helpers                                                   */
/* ------------------------------------------------------------------ */

static Cell blank_cell(const Screen *s)
{
    Cell c = {0};
    c.ch    = ' ';
    if (s->cur_fg == COLOR_DEFAULT) {
        c.flags |= CELL_FG_DFL;
    } else {
        c.fg = (uint8_t)s->cur_fg;
    }
    if (s->cur_bg == COLOR_DEFAULT) {
        c.flags |= CELL_BG_DFL;
    } else {
        c.bg = (uint8_t)s->cur_bg;
    }
    return c;
}

static void clear_row_range(Screen *s, Cell *row, int from, int to)
{
    Cell b = blank_cell(s);
    for (int c = from; c <= to; c++)
        row[c] = b;
}

/* ------------------------------------------------------------------ */
/* Cell grid allocation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Cell grid layout: one contiguous allocation holds both the row-pointer
 * array and the cell data, making the whole thing one free() call and —
 * crucially — one contiguous region that madvise() can act on usefully.
 *
 *   [ Cell* [rows] ][ Cell [rows*cols] ]
 *     ^--- grid       ^--- cells_data(grid, rows)
 *
 * sizeof(Cell*)==8 and sizeof(Cell)==8, so alignment is always satisfied.
 */
static inline Cell *cells_data(Cell **grid, int rows)
{
    return (Cell *)((char *)grid + (size_t)rows * sizeof(Cell *));
}

static inline size_t cells_data_bytes(int rows, int cols)
{
    return (size_t)rows * (size_t)cols * sizeof(Cell);
}

static Cell **alloc_cells(int rows, int cols)
{
    size_t ptr_sz  = (size_t)rows * sizeof(Cell *);
    size_t data_sz = cells_data_bytes(rows, cols);
    Cell **grid    = calloc(1, ptr_sz + data_sz);
    if (!grid) return NULL;

    Cell *data = cells_data(grid, rows);
    for (int r = 0; r < rows; r++) {
        grid[r] = data + r * cols;
        for (int c = 0; c < cols; c++) {
            grid[r][c].ch    = ' ';
            grid[r][c].flags = CELL_FG_DFL | CELL_BG_DFL;
        }
    }
    return grid;
}

static void free_cells(Cell **grid, int rows)
{
    (void)rows;   /* no longer needed – single allocation */
    free(grid);
}

/* ---- madvise helpers (Linux only) ---- */

#ifdef __linux__
#include <sys/mman.h>

/* Round a pointer UP to the next page boundary. */
static inline void *page_align_up(void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (void *)((v + 4095u) & ~(uintptr_t)4095u);
}

/* Round a pointer DOWN to the previous page boundary. */
static inline void *page_align_down(void *p)
{
    return (void *)((uintptr_t)p & ~(uintptr_t)4095u);
}

/*
 * Apply an madvise hint to the page-aligned interior of [buf, buf+len).
 * Rounds inward so we never advise pages that belong to neighbouring
 * allocations.  Does nothing if the region is smaller than one page.
 */
static void cells_madvise(void *buf, size_t len, int advice)
{
    void *start = page_align_up(buf);
    void *end   = page_align_down((char *)buf + len);
    if (end <= start) return;
    madvise(start, (size_t)((char *)end - (char *)start), advice);
}

/* Tell the kernel the data pages backing a cell grid are no longer
 * needed.  Call immediately before free_cells(). */
static void cells_dontneed(Cell **grid, int rows, int cols)
{
    cells_madvise(cells_data(grid, rows), cells_data_bytes(rows, cols),
                  MADV_DONTNEED);
}

/* Tell the kernel the data pages of an inactive (alt) grid are cold
 * and can be deprioritised in the LRU.  MADV_COLD is a soft hint:
 * the data is still valid if accessed; the kernel just treats these
 * pages as candidates for reclaim under memory pressure.
 * Requires Linux 5.4+; guarded so older headers compile cleanly. */
static void cells_cold(Cell **grid, int rows, int cols)
{
#if defined(MADV_COLD)
    cells_madvise(cells_data(grid, rows), cells_data_bytes(rows, cols),
                  MADV_COLD);
#else
    (void)grid; (void)rows; (void)cols;
#endif
}

#else  /* !__linux__ */
static void cells_dontneed(Cell **grid, int rows, int cols)
{ (void)grid; (void)rows; (void)cols; }
static void cells_cold(Cell **grid, int rows, int cols)
{ (void)grid; (void)rows; (void)cols; }
#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* Tab stops                                                            */
/* ------------------------------------------------------------------ */

static void tabs_reset(VTParser *p)
{
    memset(p->tabstops, 0, sizeof(p->tabstops));
    for (int c = 0; c < p->scr.cols && c < VT_MAX_COLS; c += 8)
        p->tabstops[c / 8] |= (uint8_t)(1u << (c % 8));
}

static bool tab_get(const VTParser *p, int col)
{
    if (col < 0 || col >= VT_MAX_COLS) return false;
    return (p->tabstops[col / 8] >> (col % 8)) & 1;
}

static void tab_set(VTParser *p, int col)
{
    if (col >= 0 && col < VT_MAX_COLS)
        p->tabstops[col / 8] |= (uint8_t)(1u << (col % 8));
}

static void tab_clear(VTParser *p, int col)
{
    if (col >= 0 && col < VT_MAX_COLS)
        p->tabstops[col / 8] &= (uint8_t)~(1u << (col % 8));
}

/* ================================================================== */
/* Screen operations                                                    */
/* ================================================================== */

static void scr_init(Screen *s, int rows, int cols)
{
    memset(s, 0, sizeof(*s));
    s->rows           = rows;
    s->cols           = cols;
    s->cells     = alloc_cells(rows, cols);
    s->alt_cells = NULL;   /* allocated lazily on first ?1047h/?1049h */
    s->row_flags = calloc((size_t)rows, 1);
#if SCROLLBACK_ENABLED
    s->scrollback     = calloc(1, SCROLLBACK_BYTES);
    s->scrollback_len = 0;
#endif
    s->cur_visible    = true;
    s->auto_wrap      = true;
    s->scroll_top     = 0;
    s->scroll_bottom  = rows - 1;
    s->cur_fg         = COLOR_DEFAULT;
    s->cur_bg         = COLOR_DEFAULT;
}

static void scr_free(Screen *s)
{
    free(s->row_flags);
    s->row_flags = NULL;
#if SCROLLBACK_ENABLED
    free(s->scrollback);
    s->scrollback = NULL;
#endif
    /* Hint to the kernel: these pages will not be read again.
     * This immediately reduces RSS for the calling process even though
     * the virtual address range stays committed to the allocator. */
    if (s->cells)     cells_dontneed(s->cells,     s->rows, s->cols);
    if (s->alt_cells) cells_dontneed(s->alt_cells, s->rows, s->cols);
    free_cells(s->cells,     s->rows);
    free_cells(s->alt_cells, s->rows);
    s->cells     = NULL;
    s->alt_cells = NULL;
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                            */
/* ------------------------------------------------------------------ */

/*
 * Scroll the scroll region UP by `n` lines.
 * Lines at scroll_top..scroll_top+n-1 are discarded;
 * scroll_top+n..scroll_bottom shift up; n blank lines appear at bottom.
 */

#if SCROLLBACK_ENABLED
/*
 * Encode one Cell row as plain UTF-8 into the scrollback buffer.
 * Only called when scroll_top==0 (full-screen scroll) and the primary
 * screen is active, so we don't capture app-internal scroll regions or
 * alternate-screen output.
 *
 * Algorithm:
 *  - Convert each cell's codepoint to UTF-8 (skip wide continuations)
 *  - Stop at first cell from the right that is non-space (trim trailing ws)
 *  - Append '\n'
 *  - If the buffer has no room, drop the oldest line (first '\n') first
 */
static void scrollback_capture(Screen *s, Cell **row_ptr, uint8_t rflags)
{
    if (!s->scrollback) return;

    /* Encode row to a temp buffer */
    char tmp[4096];
    int  tlen = 0;
    int  last_nonspace = -1;

    for (int c = 0; c < s->cols && tlen < (int)sizeof(tmp) - 4; c++) {
        Cell *cell = &(*row_ptr)[c];
        if (cell->flags & CELL_WIDE_CONT) continue;

        uint32_t cp = cell->ch ? cell->ch : ' ';
        /* Encode UTF-8 */
        if (cp < 0x80) {
            tmp[tlen++] = (char)cp;
        } else if (cp < 0x800) {
            tmp[tlen++] = (char)(0xC0 | (cp >> 6));
            tmp[tlen++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            tmp[tlen++] = (char)(0xE0 | (cp >> 12));
            tmp[tlen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            tmp[tlen++] = (char)(0x80 | (cp & 0x3F));
        } else {
            tmp[tlen++] = (char)(0xF0 | (cp >> 18));
            tmp[tlen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            tmp[tlen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            tmp[tlen++] = (char)(0x80 | (cp & 0x3F));
        }
        if (cp != ' ' && cp != 0) last_nonspace = tlen;
    }

    /* Trim trailing whitespace.
     * If the row was autowrapped (ROW_WRAPPED), don't append '\n': the
     * next captured row continues this logical line.  The buffer's last
     * byte implicitly encodes the pending-wrap state (no '\n' = open line).
     * When the buffer is full we must always append '\n' first so the eviction
     * loop can find a complete line boundary to drop. */
    if (rflags & ROW_WRAPPED) {
        /* Keep trailing spaces on wrapped rows — they pad to the full
         * width and the next row's content must start in the right column */
        tlen = tlen;   /* no trim: spaces are significant for column alignment */
    } else {
        tlen = last_nonspace >= 0 ? last_nonspace : 0;
        tmp[tlen++] = '\n';
    }

    /* Make room: drop oldest line(s) until tlen bytes fit */
    while (s->scrollback_len + tlen > SCROLLBACK_BYTES) {
        char *nl = memchr(s->scrollback, '\n', (size_t)s->scrollback_len);
        if (!nl) { s->scrollback_len = 0; break; }   /* shouldn't happen */
        int drop = (int)(nl - s->scrollback) + 1;
        memmove(s->scrollback, s->scrollback + drop,
                (size_t)(s->scrollback_len - drop));
        s->scrollback_len -= drop;
    }

    /* Append */
    memcpy(s->scrollback + s->scrollback_len, tmp, (size_t)tlen);
    s->scrollback_len += tlen;
}
#endif /* SCROLLBACK_ENABLED */

static void scroll_up(Screen *s, int n)
{
    if (n <= 0) return;
    int height = s->scroll_bottom - s->scroll_top + 1;
    if (n > height) n = height;

    /* Save the lines that will be erased */
    Cell **save = malloc((size_t)n * sizeof(Cell *));
    uint8_t *fsave = s->row_flags ? malloc((size_t)n) : NULL;
    for (int i = 0; i < n; i++) {
        save[i] = s->cells[s->scroll_top + i];
        if (fsave) fsave[i] = s->row_flags[s->scroll_top + i];
    }

#if SCROLLBACK_ENABLED
    /* Capture departing lines into scrollback.
     * Only for full-screen scrolls (scroll_top==0) on the primary screen:
     * - apps with internal scroll regions (vim, less) manage their own
     *   viewport and their lines should not appear in terminal history;
     * - alternate screen content is temporary and not historically useful. */
    if (s->scroll_top == 0 && !s->in_alt_screen) {
        for (int i = 0; i < n; i++)
            scrollback_capture(s, &save[i], fsave ? fsave[i] : 0);
    }
#endif

    /* Shift surviving lines up */
    memmove(&s->cells[s->scroll_top],
            &s->cells[s->scroll_top + n],
            (size_t)(height - n) * sizeof(Cell *));
    if (s->row_flags)
        memmove(&s->row_flags[s->scroll_top],
                &s->row_flags[s->scroll_top + n],
                (size_t)(height - n));

    /* Put the recycled rows at the bottom and blank them */
    for (int i = 0; i < n; i++) {
        s->cells[s->scroll_bottom - n + 1 + i] = save[i];
        clear_row_range(s, s->cells[s->scroll_bottom - n + 1 + i],
                        0, s->cols - 1);
        if (s->row_flags) s->row_flags[s->scroll_bottom - n + 1 + i] = 0;
    }
    free(save);
    free(fsave);
}

/*
 * Scroll the scroll region DOWN by `n` lines.
 * Lines at scroll_bottom-n+1..scroll_bottom are discarded;
 * scroll_top..scroll_bottom-n shift down; n blank lines at top.
 */
static void scroll_down(Screen *s, int n)
{
    if (n <= 0) return;
    int height = s->scroll_bottom - s->scroll_top + 1;
    if (n > height) n = height;

    Cell **save = malloc((size_t)n * sizeof(Cell *));
    for (int i = 0; i < n; i++)
        save[i] = s->cells[s->scroll_bottom - n + 1 + i];

    memmove(&s->cells[s->scroll_top + n],
            &s->cells[s->scroll_top],
            (size_t)(height - n) * sizeof(Cell *));

    for (int i = 0; i < n; i++) {
        s->cells[s->scroll_top + i] = save[i];
        clear_row_range(s, s->cells[s->scroll_top + i], 0, s->cols - 1);
    }
    free(save);
}

/* ------------------------------------------------------------------ */
/* Cursor movement                                                      */
/* ------------------------------------------------------------------ */

static void cursor_set(Screen *s, int row, int col)
{
    int min_row = s->origin_mode ? s->scroll_top    : 0;
    int max_row = s->origin_mode ? s->scroll_bottom : s->rows - 1;

    if (row < min_row) row = min_row;
    if (row > max_row) row = max_row;
    if (col < 0)       col = 0;
    if (col >= s->cols) col = s->cols - 1;

    s->cur_row    = row;
    s->cur_col    = col;
    s->pending_wrap = false;
}

/* ------------------------------------------------------------------ */
/* LF / reverse-index helpers (used by both C0 and CSI handlers)       */
/* ------------------------------------------------------------------ */

static void do_lf(Screen *s)
{
    if (s->cur_row == s->scroll_bottom)
        scroll_up(s, 1);
    else if (s->cur_row < s->rows - 1)
        s->cur_row++;
    s->pending_wrap = false;
}

static void do_ri(Screen *s)   /* reverse index */
{
    if (s->cur_row == s->scroll_top)
        scroll_down(s, 1);
    else if (s->cur_row > 0)
        s->cur_row--;
    s->pending_wrap = false;
}

/* ================================================================== */
/* Character output                                                     */
/* ================================================================== */

static void put_char(VTParser *p, uint32_t ch)
{
    Screen *s = &p->scr;

    /* Determine display width */
    int width = 1;
    if (ch >= 0x80) {
        int w = wcwidth((wchar_t)ch);
        if (w < 0) return;   /* non-printable (e.g. combining) */
        if (w == 0) return;  /* zero-width combining – skip for now */
        width = w;
    }

    /* Handle pending wrap: wrap to next line before placing char */
    if (s->pending_wrap) {
        if (s->auto_wrap) {
            s->cur_col    = 0;
            s->pending_wrap = false;
            do_lf(s);
        } else {
            s->pending_wrap = false;
        }
    }

    /* If a double-wide char doesn't fit, pad with space and wrap */
    if (width == 2 && s->cur_col + 1 >= s->cols) {
        s->cells[s->cur_row][s->cur_col] = blank_cell(s);
        if (s->auto_wrap) {
            s->cur_col = 0;
            do_lf(s);
        } else {
            return; /* no room and no wrap – discard */
        }
    }

    /* Insert mode: shift line content right */
    if (s->insert_mode) {
        Cell *row = s->cells[s->cur_row];
        int   tail = s->cols - s->cur_col - width;
        if (tail > 0)
            memmove(&row[s->cur_col + width], &row[s->cur_col],
                    (size_t)tail * sizeof(Cell));
        /* blank the inserted slot(s) */
        for (int i = 0; i < width; i++)
            row[s->cur_col + i] = blank_cell(s);
    }

    /* Write main cell */
    Cell cell = { .ch = ch, .attrs = s->cur_attrs };
    if (s->cur_fg == COLOR_DEFAULT) cell.flags |= CELL_FG_DFL;
    else                             cell.fg     = (uint8_t)s->cur_fg;
    if (s->cur_bg == COLOR_DEFAULT) cell.flags |= CELL_BG_DFL;
    else                             cell.bg     = (uint8_t)s->cur_bg;
    if (width == 2)                  cell.flags |= CELL_WIDE;
    s->cells[s->cur_row][s->cur_col] = cell;

    /* Write wide continuation cell */
    if (width == 2 && s->cur_col + 1 < s->cols) {
        Cell cont  = cell;
        cont.ch    = 0;
        cont.flags = (cont.flags & ~CELL_WIDE) | CELL_WIDE_CONT;
        s->cells[s->cur_row][s->cur_col + 1] = cont;
    }

    /* Advance cursor */
    int new_col = s->cur_col + width;
    if (new_col >= s->cols) {
        s->cur_col    = s->cols - 1;
        s->pending_wrap = true;
        if (s->row_flags) s->row_flags[s->cur_row] |= ROW_WRAPPED;
    } else {
        s->cur_col    = new_col;
        s->pending_wrap = false;
    }
}

/* ================================================================== */
/* C0 controls                                                          */
/* ================================================================== */

static void handle_c0(VTParser *p, uint8_t c)
{
    Screen *s = &p->scr;
    switch (c) {
    case 0x07: /* BEL */
        break;
    case 0x08: /* BS */
        if (s->cur_col > 0) { s->cur_col--; s->pending_wrap = false; }
        break;
    case 0x09: /* HT */
        if (s->cur_col < s->cols - 1) {
            do {
                s->cur_col++;
            } while (s->cur_col < s->cols - 1 && !tab_get(p, s->cur_col));
        }
        s->pending_wrap = false;
        break;
    case 0x0A: case 0x0B: case 0x0C: /* LF / VT / FF */
        do_lf(s);
        break;
    case 0x0D: /* CR */
        s->cur_col    = 0;
        s->pending_wrap = false;
        break;
    case 0x0E: case 0x0F: /* SO / SI – charset switching, ignore */
        break;
    }
}

/* ================================================================== */
/* SGR handler                                                          */
/* ================================================================== */

static void handle_sgr(VTParser *p)
{
    Screen *s = &p->scr;

    if (p->num_params == 0) {
        s->cur_fg    = COLOR_DEFAULT;
        s->cur_bg    = COLOR_DEFAULT;
        s->cur_attrs = 0;
        return;
    }

    int i = 0;
    while (i < p->num_params) {
        int v = p->params[i];
        switch (v) {
        case 0:
            s->cur_fg    = COLOR_DEFAULT;
            s->cur_bg    = COLOR_DEFAULT;
            s->cur_attrs = 0;
            break;
        case 1: s->cur_attrs |=  ATTR_BOLD;      break;
        case 2: s->cur_attrs |=  ATTR_DIM;       break;
        case 3: s->cur_attrs |=  ATTR_ITALIC;    break;
        case 4: s->cur_attrs |=  ATTR_UNDERLINE; break;
        case 5: s->cur_attrs |=  ATTR_BLINK;     break;
        case 7: s->cur_attrs |=  ATTR_REVERSE;   break;
        case 8: s->cur_attrs |=  ATTR_INVIS;     break;
        case 22: s->cur_attrs &= ~(ATTR_BOLD | ATTR_DIM); break;
        case 23: s->cur_attrs &= ~ATTR_ITALIC;    break;
        case 24: s->cur_attrs &= ~ATTR_UNDERLINE; break;
        case 25: s->cur_attrs &= ~ATTR_BLINK;     break;
        case 27: s->cur_attrs &= ~ATTR_REVERSE;   break;
        case 28: s->cur_attrs &= ~ATTR_INVIS;     break;
        case 39: s->cur_fg = COLOR_DEFAULT; break;
        case 49: s->cur_bg = COLOR_DEFAULT; break;
        /* 8-colour fg */
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            s->cur_fg = (uint16_t)(v - 30); break;
        /* 8-colour bg */
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            s->cur_bg = (uint16_t)(v - 40); break;
        /* bright fg */
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            s->cur_fg = (uint16_t)(v - 90 + 8); break;
        /* bright bg */
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            s->cur_bg = (uint16_t)(v - 100 + 8); break;
        /* 256-colour fg */
        case 38:
            if (i + 2 < p->num_params && p->params[i + 1] == 5) {
                s->cur_fg = (uint16_t)p->params[i + 2];
                i += 2;
            } else if (i + 4 < p->num_params && p->params[i + 1] == 2) {
                /* truecolor: approximate as 256-colour – TODO later */
                i += 4;
            }
            break;
        /* 256-colour bg */
        case 48:
            if (i + 2 < p->num_params && p->params[i + 1] == 5) {
                s->cur_bg = (uint16_t)p->params[i + 2];
                i += 2;
            } else if (i + 4 < p->num_params && p->params[i + 1] == 2) {
                i += 4;
            }
            break;
        default: break;
        }
        i++;
    }
}

/* ================================================================== */
/* CSI dispatch                                                         */
/* ================================================================== */

static void dispatch_csi(VTParser *p, char final)
{
    Screen *s = &p->scr;

    if (!p->priv) {
        switch (final) {

        /* ---- cursor movement ---- */
        case 'A': cursor_set(s, s->cur_row - P(0,1), s->cur_col);  break;
        case 'B': cursor_set(s, s->cur_row + P(0,1), s->cur_col);  break;
        case 'C': cursor_set(s, s->cur_row, s->cur_col + P(0,1));  break;
        case 'D': cursor_set(s, s->cur_row, s->cur_col - P(0,1));  break;
        case 'E': cursor_set(s, s->cur_row + P(0,1), 0);           break;
        case 'F': cursor_set(s, s->cur_row - P(0,1), 0);           break;
        case 'G': cursor_set(s, s->cur_row, P(0,1) - 1);           break;
        case 'd': cursor_set(s, P(0,1) - 1, s->cur_col);           break;

        case 'H': case 'f': {
            int row = P(0,1) - 1;
            int col = P(1,1) - 1;
            if (s->origin_mode) row += s->scroll_top;
            cursor_set(s, row, col);
            break;
        }

        /* CHT – forward tab */
        case 'I':
            for (int n = P(0,1); n > 0; n--) handle_c0(p, 0x09);
            break;

        /* CBT – backward tab */
        case 'Z': {
            int n = P(0,1);
            while (n-- > 0 && s->cur_col > 0) {
                s->cur_col--;
                while (s->cur_col > 0 && !tab_get(p, s->cur_col))
                    s->cur_col--;
            }
            s->pending_wrap = false;
            break;
        }

        /* ---- erase ---- */
        case 'J': {
            int m = P(0,0);
            if (m == 0 || m == 2) {
                /* erase from cursor to end (0), or whole screen (2) */
                int start_row = (m == 2) ? 0 : s->cur_row;
                int start_col = (m == 2) ? 0 : s->cur_col;
                clear_row_range(s, s->cells[start_row], start_col, s->cols - 1);
                for (int r = start_row + 1; r < s->rows; r++)
                    clear_row_range(s, s->cells[r], 0, s->cols - 1);
            } else if (m == 1) {
                /* erase from top to cursor */
                for (int r = 0; r < s->cur_row; r++)
                    clear_row_range(s, s->cells[r], 0, s->cols - 1);
                clear_row_range(s, s->cells[s->cur_row], 0, s->cur_col);
            } else if (m == 3) {
                /* erase scrollback – same as 2 for our purposes */
                for (int r = 0; r < s->rows; r++)
                    clear_row_range(s, s->cells[r], 0, s->cols - 1);
            }
            break;
        }
        case 'K': {
            int m = P(0,0);
            if (m == 0) {
                /* Erase to end of line: kills the trailing content that
                 * established the wrap, so clear the wrap flag. */
                clear_row_range(s, s->cells[s->cur_row], s->cur_col, s->cols - 1);
                if (s->cur_col == 0 && s->row_flags)
                    s->row_flags[s->cur_row] = 0;
            } else if (m == 1) {
                clear_row_range(s, s->cells[s->cur_row], 0, s->cur_col);
            } else if (m == 2) {
                clear_row_range(s, s->cells[s->cur_row], 0, s->cols - 1);
                if (s->row_flags) s->row_flags[s->cur_row] = 0;
            }
            break;
        }
        case 'X': { /* ECH – erase N chars */
            int n = P(0,1);
            int end = s->cur_col + n - 1;
            if (end >= s->cols) end = s->cols - 1;
            clear_row_range(s, s->cells[s->cur_row], s->cur_col, end);
            break;
        }

        /* ---- insert / delete ---- */
        case '@': { /* ICH – insert N blank chars */
            int n = P(0,1);
            if (n > s->cols - s->cur_col) n = s->cols - s->cur_col;
            Cell *row = s->cells[s->cur_row];
            memmove(&row[s->cur_col + n], &row[s->cur_col],
                    (size_t)(s->cols - s->cur_col - n) * sizeof(Cell));
            clear_row_range(s, row, s->cur_col, s->cur_col + n - 1);
            break;
        }
        case 'P': { /* DCH – delete N chars */
            int n = P(0,1);
            if (n > s->cols - s->cur_col) n = s->cols - s->cur_col;
            Cell *row = s->cells[s->cur_row];
            memmove(&row[s->cur_col], &row[s->cur_col + n],
                    (size_t)(s->cols - s->cur_col - n) * sizeof(Cell));
            clear_row_range(s, row, s->cols - n, s->cols - 1);
            break;
        }
        case 'L': { /* IL – insert N lines */
            if (s->cur_row >= s->scroll_top && s->cur_row <= s->scroll_bottom) {
                int saved = s->scroll_top;
                s->scroll_top = s->cur_row;
                scroll_down(s, P(0,1));
                s->scroll_top = saved;
            }
            s->cur_col    = 0;
            s->pending_wrap = false;
            break;
        }
        case 'M': { /* DL – delete N lines */
            if (s->cur_row >= s->scroll_top && s->cur_row <= s->scroll_bottom) {
                int saved = s->scroll_top;
                s->scroll_top = s->cur_row;
                scroll_up(s, P(0,1));
                s->scroll_top = saved;
            }
            s->cur_col    = 0;
            s->pending_wrap = false;
            break;
        }

        /* ---- scroll ---- */
        case 'S': scroll_up  (s, P(0,1)); break;
        case 'T': scroll_down(s, P(0,1)); break;

        /* ---- attributes ---- */
        case 'm': handle_sgr(p); break;

        /* ---- modes ---- */
        case 'h':
            if (P(0,0) == 4) s->insert_mode = true;
            break;
        case 'l':
            if (P(0,0) == 4) s->insert_mode = false;
            break;

        /* ---- tab stops ---- */
        case 'g':
            if (P(0,0) == 0)
                tab_clear(p, s->cur_col);
            else if (P(0,0) == 3)
                memset(p->tabstops, 0, sizeof(p->tabstops));
            break;

        /* ---- scroll region ---- */
        case 'r': {
            int top = P(0,1) - 1;
            int bot = P(1, s->rows) - 1;
            if (top < 0) top = 0;
            if (bot >= s->rows) bot = s->rows - 1;
            if (top < bot) {
                s->scroll_top    = top;
                s->scroll_bottom = bot;
            }
            /* Cursor goes to home (origin-relative if DECOM) */
            cursor_set(s, s->origin_mode ? s->scroll_top : 0, 0);
            break;
        }

        /* ---- save / restore cursor (ANSI variants) ---- */
        case 's':
            s->saved_row   = s->cur_row;
            s->saved_col   = s->cur_col;
            s->saved_fg    = s->cur_fg;
            s->saved_bg    = s->cur_bg;
            s->saved_attrs = s->cur_attrs;
            s->saved_wrap  = s->auto_wrap;
            break;
        case 'u':
            s->cur_row    = s->saved_row;
            s->cur_col    = s->saved_col;
            s->cur_fg     = s->saved_fg;
            s->cur_bg     = s->saved_bg;
            s->cur_attrs  = s->saved_attrs;
            s->auto_wrap  = s->saved_wrap;
            s->pending_wrap = false;
            break;

        /* ---- device status ---- */
        case 'n':
            if (P(0,0) == 6)
                p->cpr_requested = true;   /* main loop will send reply */
            break;

        default: break;
        }

    } else {
        /* CSI ? ... */
        switch (final) {

        case 'h': /* DEC private set */
            for (int i = 0; i < p->num_params; i++) {
                switch (p->params[i]) {
                case 1:  s->app_cursor  = true;  break;
                case 6:  s->origin_mode = true;
                         cursor_set(s, s->scroll_top, 0); break;
                case 7:  s->auto_wrap   = true;  break;
                case 12: /* blinking cursor – ignore */  break;
                case 25: s->cur_visible = true;  break;
                case 47: case 1047: /* alt screen (simple) */
                    if (!s->in_alt_screen) {
                        s->alt_saved_row   = s->cur_row;
                        s->alt_saved_col   = s->cur_col;
                        s->alt_saved_fg    = s->cur_fg;
                        s->alt_saved_bg    = s->cur_bg;
                        s->alt_saved_attrs = s->cur_attrs;
                        /* Lazy alt-screen allocation */
                        if (!s->alt_cells)
                            s->alt_cells = alloc_cells(s->rows, s->cols);
                        Cell **tmp = s->cells;
                        s->cells     = s->alt_cells;
                        s->alt_cells = tmp;
                        s->in_alt_screen = true;
                        /* Primary grid (now in alt_cells) is no longer hot;
                         * let the kernel demote its pages in the LRU. */
                        cells_cold(s->alt_cells, s->rows, s->cols);
                        for (int r = 0; r < s->rows; r++)
                            clear_row_range(s, s->cells[r], 0, s->cols - 1);
                        s->cur_row = 0; s->cur_col = 0;
                        s->scroll_top = 0; s->scroll_bottom = s->rows - 1;
                    }
                    break;
                case 1049: /* alt screen + save/restore cursor */
                    if (!s->in_alt_screen) {
                        s->alt_saved_row   = s->cur_row;
                        s->alt_saved_col   = s->cur_col;
                        s->alt_saved_fg    = s->cur_fg;
                        s->alt_saved_bg    = s->cur_bg;
                        s->alt_saved_attrs = s->cur_attrs;
                        /* Lazy alt-screen allocation */
                        if (!s->alt_cells)
                            s->alt_cells = alloc_cells(s->rows, s->cols);
                        Cell **tmp = s->cells;
                        s->cells     = s->alt_cells;
                        s->alt_cells = tmp;
                        s->in_alt_screen = true;
                        /* Primary grid (now in alt_cells) is no longer hot;
                         * let the kernel demote its pages in the LRU. */
                        cells_cold(s->alt_cells, s->rows, s->cols);
                        for (int r = 0; r < s->rows; r++)
                            clear_row_range(s, s->cells[r], 0, s->cols - 1);
                        s->cur_row = 0; s->cur_col = 0;
                        s->scroll_top = 0; s->scroll_bottom = s->rows - 1;
                    }
                    break;
                case 2004: s->bracketed_paste = true; break;
                default:   break;
                }
            }
            break;

        case 'l': /* DEC private reset */
            for (int i = 0; i < p->num_params; i++) {
                switch (p->params[i]) {
                case 1:  s->app_cursor  = false; break;
                case 6:  s->origin_mode = false;
                         cursor_set(s, 0, 0);    break;
                case 7:  s->auto_wrap   = false; break;
                case 12: break;
                case 25: s->cur_visible = false; break;
                case 47: case 1047:
                    if (s->in_alt_screen) {
                        Cell **tmp = s->cells;
                        s->cells     = s->alt_cells;
                        s->alt_cells = tmp;
                        s->in_alt_screen = false;
                        /* Alt grid (now in alt_cells) will be cold until
                         * next alt-screen entry; hint the kernel. */
                        cells_cold(s->alt_cells, s->rows, s->cols);
                        s->cur_row  = s->alt_saved_row;
                        s->cur_col  = s->alt_saved_col;
                        s->cur_fg   = s->alt_saved_fg;
                        s->cur_bg   = s->alt_saved_bg;
                        s->cur_attrs = s->alt_saved_attrs;
                    }
                    break;
                case 1049:
                    if (s->in_alt_screen) {
                        Cell **tmp = s->cells;
                        s->cells     = s->alt_cells;
                        s->alt_cells = tmp;
                        s->in_alt_screen = false;
                        /* Alt grid (now in alt_cells) will be cold until
                         * next alt-screen entry; hint the kernel. */
                        cells_cold(s->alt_cells, s->rows, s->cols);
                        s->cur_row  = s->alt_saved_row;
                        s->cur_col  = s->alt_saved_col;
                        s->cur_fg   = s->alt_saved_fg;
                        s->cur_bg   = s->alt_saved_bg;
                        s->cur_attrs = s->alt_saved_attrs;
                    }
                    break;
                case 2004: s->bracketed_paste = false; break;
                default:   break;
                }
            }
            break;

        default: break;
        }
    }
}

/* ================================================================== */
/* OSC dispatch                                                         */
/* ================================================================== */

static void dispatch_osc(VTParser *p)
{
    p->osc[p->osc_len < VT_MAX_OSC - 1 ? p->osc_len : VT_MAX_OSC - 1] = '\0';
    char *sep = strchr(p->osc, ';');
    if (!sep) return;
    int cmd = atoi(p->osc);
    if (cmd == 0 || cmd == 1 || cmd == 2)
        snprintf(p->scr.title, sizeof(p->scr.title), "%s", sep + 1);
}

/* ================================================================== */
/* ESC dispatch                                                         */
/* ================================================================== */

static void dispatch_esc(VTParser *p, char final)
{
    Screen *s = &p->scr;
    switch (final) {
    case '7': /* DECSC – save cursor */
        s->saved_row   = s->cur_row;
        s->saved_col   = s->cur_col;
        s->saved_fg    = s->cur_fg;
        s->saved_bg    = s->cur_bg;
        s->saved_attrs = s->cur_attrs;
        s->saved_wrap  = s->auto_wrap;
        break;
    case '8': /* DECRC – restore cursor */
        s->cur_row    = s->saved_row;
        s->cur_col    = s->saved_col;
        s->cur_fg     = s->saved_fg;
        s->cur_bg     = s->saved_bg;
        s->cur_attrs  = s->saved_attrs;
        s->auto_wrap  = s->saved_wrap;
        s->pending_wrap = false;
        break;
    case 'D': /* IND – index (acts like LF) */
        do_lf(s);
        break;
    case 'E': /* NEL – next line */
        s->cur_col = 0;
        do_lf(s);
        break;
    case 'H': /* HTS – set tab stop */
        tab_set(p, s->cur_col);
        break;
    case 'M': /* RI – reverse index */
        do_ri(s);
        break;
    case 'c': /* RIS – full reset */
        scr_free(s);
        scr_init(s, s->rows, s->cols);
        tabs_reset(p);
        break;
    case '\\': /* ST – string terminator (already handled inline) */
        break;
    default:
        break;
    }
}

/* ================================================================== */
/* UTF-8 byte-at-a-time decoder                                        */
/* ================================================================== */

/*
 * Feed one byte into the carry buffer.
 * Returns  1 if a complete codepoint is available in *out.
 *          0 if more bytes are needed.
 *         -1 on encoding error (caller should reset and re-feed).
 */
static int utf8_feed(VTParser *p, uint8_t byte, uint32_t *out)
{
    if (p->utf8_want == 0) {
        /* Start of a new codepoint */
        if (byte < 0x80) {
            *out = byte;
            return 1;
        } else if ((byte & 0xE0) == 0xC0) {
            p->utf8_buf[0] = byte; p->utf8_len = 1; p->utf8_want = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            p->utf8_buf[0] = byte; p->utf8_len = 1; p->utf8_want = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            p->utf8_buf[0] = byte; p->utf8_len = 1; p->utf8_want = 4;
        } else {
            return -1; /* invalid lead byte */
        }
        return 0;
    }

    /* Continuation byte expected */
    if ((byte & 0xC0) != 0x80) {
        /* Bad continuation: reset and signal error */
        p->utf8_want = 0;
        p->utf8_len  = 0;
        return -1;
    }

    p->utf8_buf[p->utf8_len++] = byte;
    if (p->utf8_len < p->utf8_want)
        return 0; /* still need more */

    /* Decode complete multi-byte sequence */
    uint8_t *b = p->utf8_buf;
    uint32_t cp;
    switch (p->utf8_want) {
    case 2: cp = ((uint32_t)(b[0] & 0x1F) <<  6) |  (b[1] & 0x3F); break;
    case 3: cp = ((uint32_t)(b[0] & 0x0F) << 12) | ((uint32_t)(b[1] & 0x3F) << 6)
               |  (b[2] & 0x3F); break;
    default:
    case 4: cp = ((uint32_t)(b[0] & 0x07) << 18) | ((uint32_t)(b[1] & 0x3F) << 12)
               | ((uint32_t)(b[2] & 0x3F) <<  6) |  (b[3] & 0x3F); break;
    }
    p->utf8_want = 0;
    p->utf8_len  = 0;
    *out = cp;
    return 1;
}

/* ================================================================== */
/* CSI parameter accumulation helpers                                  */
/* ================================================================== */

static void csi_clear(VTParser *p)
{
    memset(p->params, 0, sizeof(p->params));
    p->num_params = 0;
    memset(p->inter, 0, sizeof(p->inter));
    p->num_inter  = 0;
    p->priv       = false;
}

static void csi_digit(VTParser *p, int digit)
{
    if (p->num_params == 0) p->num_params = 1;
    if (p->params[p->num_params - 1] < 16384) /* overflow guard */
        p->params[p->num_params - 1] = p->params[p->num_params - 1] * 10 + digit;
}

static void csi_sep(VTParser *p)
{
    if (p->num_params == 0) p->num_params = 1;   /* empty first field */
    if (p->num_params < VT_MAX_PARAMS) p->num_params++;
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

void vt_init(VTParser *p, int rows, int cols)
{
    memset(p, 0, sizeof(*p));
    scr_init(&p->scr, rows, cols);
    tabs_reset(p);
    p->state = ST_GROUND;
}

void vt_free(VTParser *p)
{
    scr_free(&p->scr);
}

/* ------------------------------------------------------------------ */
/* Screen resize                                                        */
/* ------------------------------------------------------------------ */

/*
 * Allocate a new grid at (new_rows × new_cols), copy as much content
 * from old_grid as fits (top-left anchored), fill remainder with blank
 * cells, then release the old grid.  Handles NULL old_grid (first use).
 *
 * Caller must pass the OLD rows/cols so cells_dontneed can find the
 * data block inside the single allocation before we free it.
 */
static Cell **resize_grid(Cell **old_grid, int old_rows, int old_cols,
                           int new_rows, int new_cols)
{
    Cell **new_grid = alloc_cells(new_rows, new_cols);
    if (!new_grid) return old_grid;   /* OOM: keep old grid unchanged */

    if (old_grid) {
        int copy_rows = old_rows < new_rows ? old_rows : new_rows;
        int copy_cols = old_cols < new_cols ? old_cols : new_cols;
        for (int r = 0; r < copy_rows; r++)
            memcpy(new_grid[r], old_grid[r], (size_t)copy_cols * sizeof(Cell));

        cells_dontneed(old_grid, old_rows, old_cols);
        free_cells(old_grid, old_rows);
    }
    return new_grid;
}

/*
 * Resize the Screen's cell grids and fix up all cursor/region state.
 * Called with the new terminal dimensions (not including the status row;
 * that adjustment is done by the caller).
 */
static void scr_resize(Screen *s, int new_rows, int new_cols)
{
    int old_rows = s->rows;
    int old_cols = s->cols;

#if SCROLLBACK_ENABLED
    /*
     * When growing vertically while NOT in the alt screen, pull lines from
     * the scrollback buffer to fill the new space at the top, shifting
     * existing content down so it stays flush with the bottom.  This
     * mirrors xterm/tmux behaviour: the screen "unscrolls" on resize.
     *
     * We only do this for the primary grid (not alt_cells) and only when
     * the scroll region covers the full screen (apps with private scroll
     * regions, e.g. vim, handle their own geometry).
     */
    int extra = new_rows - old_rows;
    if (extra > 0 && !s->in_alt_screen
            && s->scroll_top == 0 && s->scroll_bottom == old_rows - 1
            && s->scrollback && s->scrollback_len > 0) {

        /* Count available scrollback lines */
        int avail = 0;
        for (int i = 0; i < s->scrollback_len; i++)
            if (s->scrollback[i] == '\n') avail++;
        if (extra > avail) extra = avail;   /* can't pull more than we have */

        if (extra > 0) {
            /* Allocate new grid */
            Cell **ng = alloc_cells(new_rows, new_cols);

            /* Find the start of the last `extra` lines in the buffer.
             * The buffer always ends with '\n'.  Start the scan at len-1
             * so that terminal newline doesn't count as a line boundary,
             * which would otherwise cause an empty line to be decoded. */
            int pos = s->scrollback_len - 1;   /* index of terminal '\n' */
            for (int found = 0; found < extra && pos > 0; ) {
                pos--;
                if (s->scrollback[pos] == '\n') found++;
            }
            /* pos is now the '\n' preceding our block (or 0 if at start) */
            int src_pos = (s->scrollback[pos] == '\n' && pos < s->scrollback_len - 1)
                          ? pos + 1 : pos;
            for (int r = 0; r < extra; r++) {
                int end = src_pos;
                while (end < s->scrollback_len && s->scrollback[end] != '\n') end++;
                /* Write codepoints into row r of ng */
                int c = 0;
                int p2 = src_pos;
                while (p2 < end && c < new_cols) {
                    unsigned char b0 = (unsigned char)s->scrollback[p2];
                    uint32_t cp; int seqlen;
                    if      (b0 < 0x80)  { cp = b0; seqlen = 1; }
                    else if (b0 < 0xE0)  { cp = b0 & 0x1F; seqlen = 2; }
                    else if (b0 < 0xF0)  { cp = b0 & 0x0F; seqlen = 3; }
                    else                  { cp = b0 & 0x07; seqlen = 4; }
                    for (int k = 1; k < seqlen && p2+k < end; k++)
                        cp = (cp << 6) | ((unsigned char)s->scrollback[p2+k] & 0x3F);
                    ng[r][c].ch    = cp;
                    ng[r][c].flags = CELL_FG_DFL | CELL_BG_DFL;
                    p2 += seqlen;
                    c++;
                }
                /* remaining cols already blank from alloc_cells */
                src_pos = end + 1;
            }

            /* Copy old primary grid into ng[extra..extra+old_rows-1] */
            int copy_cols = old_cols < new_cols ? old_cols : new_cols;
            for (int r = 0; r < old_rows; r++)
                memcpy(ng[extra + r], s->cells[r],
                       (size_t)copy_cols * sizeof(Cell));

            /* Adjust cursor and scroll region to account for shift */
            s->cur_row  += extra;
            s->saved_row += extra;
            if (s->cur_row  >= new_rows) s->cur_row  = new_rows - 1;
            if (s->saved_row >= new_rows) s->saved_row = new_rows - 1;

            /* Retire the consumed lines.
             * pos is the '\n' that separates the kept part from the consumed
             * part.  We want to keep bytes 0..pos inclusive, so len = pos+1.
             * If pos==0 we consumed everything. */
            s->scrollback_len = (pos > 0) ? pos + 1 : 0;

            /* Install new grid, free old */
            cells_dontneed(s->cells, old_rows, old_cols);
            free_cells(s->cells, old_rows);
            s->cells = ng;

            /* row_flags: shift down by extra, fill new top rows with 0 */
            if (s->row_flags) {
                s->row_flags = realloc(s->row_flags, (size_t)new_rows);
                if (s->row_flags) {
                    memmove(s->row_flags + extra, s->row_flags,
                            (size_t)old_rows);
                    memset(s->row_flags, 0, (size_t)extra);
                }
            }

            /* alt_cells: resize normally (no scrollback fill for alt screen) */
            if (s->alt_cells)
                s->alt_cells = resize_grid(s->alt_cells, old_rows, old_cols,
                                            new_rows, new_cols);

            s->rows = new_rows;
            s->cols = new_cols;
            s->scroll_bottom = new_rows - 1;
            if (s->alt_saved_row >= new_rows) s->alt_saved_row = new_rows - 1;
            if (s->alt_saved_col >= new_cols) s->alt_saved_col = new_cols - 1;
            if (s->saved_col     >= new_cols) s->saved_col     = new_cols - 1;
            if (s->cur_col       >= new_cols) { s->cur_col = new_cols - 1; s->pending_wrap = false; }
            return;
        }
    }
#endif

#if SCROLLBACK_ENABLED
    /*
     * When shrinking vertically on the primary screen with a full scroll
     * region, push the top `reduce` rows into scrollback before discarding
     * them.  This keeps the bottom of the screen visually anchored and
     * mirrors what xterm/tmux do.
     */
    int reduce = old_rows - new_rows;
    if (reduce > 0 && !s->in_alt_screen
            && s->scroll_top == 0 && s->scroll_bottom == old_rows - 1
            && s->scrollback) {

        /* Capture rows 0..reduce-1 into scrollback (oldest first) */
        for (int r = 0; r < reduce; r++)
            scrollback_capture(s, &s->cells[r],
                               s->row_flags ? s->row_flags[r] : 0);

        /* Allocate new smaller grid, copy surviving rows */
        Cell **ng = alloc_cells(new_rows, new_cols);
        int copy_cols = old_cols < new_cols ? old_cols : new_cols;
        for (int r = 0; r < new_rows; r++)
            memcpy(ng[r], s->cells[reduce + r],
                   (size_t)copy_cols * sizeof(Cell));

        /* Adjust cursor upward by reduce rows */
        s->cur_row  -= reduce;
        s->saved_row -= reduce;
        if (s->cur_row  < 0) { s->cur_row  = 0; s->pending_wrap = false; }
        if (s->saved_row < 0)  s->saved_row  = 0;

        cells_dontneed(s->cells, old_rows, old_cols);
        free_cells(s->cells, old_rows);
        s->cells = ng;

        /* row_flags: discard top `reduce` rows, realloc to new size */
        if (s->row_flags) {
            memmove(s->row_flags, s->row_flags + reduce,
                    (size_t)new_rows);
            s->row_flags = realloc(s->row_flags, (size_t)new_rows);
        }

        if (s->alt_cells)
            s->alt_cells = resize_grid(s->alt_cells, old_rows, old_cols,
                                        new_rows, new_cols);

        s->rows = new_rows;
        s->cols = new_cols;
        s->scroll_bottom = new_rows - 1;
        if (s->cur_col       >= new_cols) { s->cur_col = new_cols - 1; s->pending_wrap = false; }
        if (s->saved_col     >= new_cols)   s->saved_col     = new_cols - 1;
        if (s->alt_saved_row >= new_rows)   s->alt_saved_row = new_rows - 1;
        if (s->alt_saved_col >= new_cols)   s->alt_saved_col = new_cols - 1;
        return;
    }
#endif

    /* Default resize (shrink with no scrollback, or alt screen) */
    s->cells = resize_grid(s->cells, old_rows, old_cols, new_rows, new_cols);
    if (s->alt_cells)
        s->alt_cells = resize_grid(s->alt_cells, old_rows, old_cols,
                                    new_rows, new_cols);

    /* Resize row_flags: realloc and zero any new rows */
    if (s->row_flags) {
        s->row_flags = realloc(s->row_flags, (size_t)new_rows);
        if (s->row_flags && new_rows > old_rows)
            memset(s->row_flags + old_rows, 0,
                   (size_t)(new_rows - old_rows));
    }

    s->rows = new_rows;
    s->cols = new_cols;

    /* ---- Clamp cursor ---- */
    if (s->cur_row >= new_rows) { s->cur_row = new_rows - 1; s->pending_wrap = false; }
    if (s->cur_col >= new_cols) { s->cur_col = new_cols - 1; s->pending_wrap = false; }

    /* ---- Scroll region ----
     * If the region spanned the full old screen, extend it to the full
     * new screen.  Otherwise just clamp the bottom to the new last row. */
    if (s->scroll_top == 0 && s->scroll_bottom == old_rows - 1) {
        s->scroll_bottom = new_rows - 1;
    } else {
        if (s->scroll_top  >= new_rows) s->scroll_top  = 0;
        if (s->scroll_bottom >= new_rows) s->scroll_bottom = new_rows - 1;
        if (s->scroll_top > s->scroll_bottom)
            s->scroll_bottom = new_rows - 1;   /* degenerate: reset */
    }

    /* ---- Clamp saved cursors ---- */
    if (s->saved_row     >= new_rows) s->saved_row     = new_rows - 1;
    if (s->saved_col     >= new_cols) s->saved_col     = new_cols - 1;
    if (s->alt_saved_row >= new_rows) s->alt_saved_row = new_rows - 1;
    if (s->alt_saved_col >= new_cols) s->alt_saved_col = new_cols - 1;
}

void vt_resize(VTParser *p, int rows, int cols)
{
    if (rows == p->scr.rows && cols == p->scr.cols) return;

    scr_resize(&p->scr, rows, cols);

    /* Rebuild tab stops for the new column count.  scr_resize has already
     * updated p->scr.cols, so tabs_reset will use the new width. */
    tabs_reset(p);
}
/* ================================================================== */
/* Serialization                                                        */
/* ================================================================== */

#include <unistd.h>
#include <errno.h>

static int vt_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int vt_read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = ECONNRESET; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

/*
 * Packed wire structs — no padding, explicit widths.
 * Used only for serialize/deserialize; never stored on disk.
 */
typedef struct __attribute__((packed)) {
    int32_t  state;
    int32_t  params[VT_MAX_PARAMS];
    int32_t  num_params;
    char     inter[VT_MAX_INTER];
    int32_t  num_inter;
    uint8_t  priv;
    int32_t  osc_len;
    uint8_t  utf8_buf[4];
    int32_t  utf8_len;
    int32_t  utf8_want;
    uint8_t  tabstops[VT_MAX_COLS / 8 + 1];
    uint8_t  cpr_requested;
} VTWire;

typedef struct __attribute__((packed)) {
    int32_t  rows, cols;
    int32_t  cur_row, cur_col;
    uint8_t  cur_visible, pending_wrap;
    uint8_t  origin_mode, auto_wrap, app_cursor, bracketed_paste;
    uint8_t  insert_mode, in_alt_screen;
    int32_t  scroll_top, scroll_bottom;
    uint16_t cur_fg, cur_bg;
    uint8_t  cur_attrs;
    int32_t  saved_row, saved_col;
    uint16_t saved_fg, saved_bg;
    uint8_t  saved_attrs, saved_wrap;
    int32_t  alt_saved_row, alt_saved_col;
    uint16_t alt_saved_fg, alt_saved_bg;
    uint8_t  alt_saved_attrs;
    char     title[256];
} ScreenWire;

int vt_serialize(const VTParser *p, int fd)
{
    /* VT parser metadata */
    VTWire vw;
    memset(&vw, 0, sizeof(vw));
    vw.state      = (int32_t)p->state;
    vw.num_params = (int32_t)p->num_params;
    vw.num_inter  = (int32_t)p->num_inter;
    vw.priv       = p->priv ? 1 : 0;
    vw.osc_len    = (int32_t)p->osc_len;
    vw.utf8_len   = (int32_t)p->utf8_len;
    vw.utf8_want  = (int32_t)p->utf8_want;
    vw.cpr_requested = p->cpr_requested ? 1 : 0;
    for (int i = 0; i < VT_MAX_PARAMS; i++) vw.params[i] = (int32_t)p->params[i];
    memcpy(vw.inter,    p->inter,    VT_MAX_INTER);
    memcpy(vw.utf8_buf, p->utf8_buf, 4);
    memcpy(vw.tabstops, p->tabstops, sizeof(p->tabstops));
    if (vt_write_all(fd, &vw, sizeof(vw)) < 0) return -1;

    /* OSC buffer (full fixed-size block; osc_len tells how much is valid) */
    if (vt_write_all(fd, p->osc, VT_MAX_OSC) < 0) return -1;

    /* Screen metadata */
    const Screen *s = &p->scr;
    ScreenWire sw;
    memset(&sw, 0, sizeof(sw));
    sw.rows           = (int32_t)s->rows;
    sw.cols           = (int32_t)s->cols;
    sw.cur_row        = (int32_t)s->cur_row;
    sw.cur_col        = (int32_t)s->cur_col;
    sw.cur_visible    = s->cur_visible    ? 1 : 0;
    sw.pending_wrap   = s->pending_wrap   ? 1 : 0;
    sw.origin_mode    = s->origin_mode    ? 1 : 0;
    sw.auto_wrap      = s->auto_wrap      ? 1 : 0;
    sw.app_cursor     = s->app_cursor     ? 1 : 0;
    sw.bracketed_paste = s->bracketed_paste ? 1 : 0;
    sw.insert_mode    = s->insert_mode    ? 1 : 0;
    sw.in_alt_screen  = s->in_alt_screen  ? 1 : 0;
    sw.scroll_top     = (int32_t)s->scroll_top;
    sw.scroll_bottom  = (int32_t)s->scroll_bottom;
    sw.cur_fg         = s->cur_fg;
    sw.cur_bg         = s->cur_bg;
    sw.cur_attrs      = s->cur_attrs;
    sw.saved_row      = (int32_t)s->saved_row;
    sw.saved_col      = (int32_t)s->saved_col;
    sw.saved_fg       = s->saved_fg;
    sw.saved_bg       = s->saved_bg;
    sw.saved_attrs    = s->saved_attrs;
    sw.saved_wrap     = s->saved_wrap     ? 1 : 0;
    sw.alt_saved_row  = (int32_t)s->alt_saved_row;
    sw.alt_saved_col  = (int32_t)s->alt_saved_col;
    sw.alt_saved_fg   = s->alt_saved_fg;
    sw.alt_saved_bg   = s->alt_saved_bg;
    sw.alt_saved_attrs = s->alt_saved_attrs;
    memcpy(sw.title, s->title, 256);
    if (vt_write_all(fd, &sw, sizeof(sw)) < 0) return -1;

    /*
     * Cell data: write rows in LOGICAL order (s->cells[0..rows-1]) not
     * physical memory order.  Scrolling shuffles the row pointer array
     * without moving cell data, so physical order may be rotated relative
     * to logical order.  Deserializing physical order back into a fresh
     * sequential grid would produce a circularly-shifted screen.
     */
    size_t row_bytes = (size_t)s->cols * sizeof(Cell);
    for (int r = 0; r < s->rows; r++)
        if (vt_write_all(fd, s->cells[r], row_bytes) < 0) return -1;

    /* Alt cell data (also logical order) */
    uint8_t has_alt = s->alt_cells ? 1 : 0;
    if (vt_write_all(fd, &has_alt, 1) < 0) return -1;
    if (has_alt)
        for (int r = 0; r < s->rows; r++)
            if (vt_write_all(fd, s->alt_cells[r], row_bytes) < 0) return -1;

    return 0;
}

int vt_deserialize(VTParser *p, int fd)
{
    VTWire vw;
    if (vt_read_all(fd, &vw, sizeof(vw)) < 0) return -1;

    char osc_buf[VT_MAX_OSC];
    if (vt_read_all(fd, osc_buf, VT_MAX_OSC) < 0) return -1;

    ScreenWire sw;
    if (vt_read_all(fd, &sw, sizeof(sw)) < 0) return -1;

    /* Re-initialize at the stored dimensions */
    vt_free(p);
    vt_init(p, (int)sw.rows, (int)sw.cols);

    /* Restore VT parser state */
    p->state         = (VTState)vw.state;
    p->num_params    = (int)vw.num_params;
    p->num_inter     = (int)vw.num_inter;
    p->priv          = vw.priv;
    p->osc_len       = (int)vw.osc_len;
    p->utf8_len      = (int)vw.utf8_len;
    p->utf8_want     = (int)vw.utf8_want;
    p->cpr_requested = vw.cpr_requested;
    for (int i = 0; i < VT_MAX_PARAMS; i++) p->params[i] = (int)vw.params[i];
    memcpy(p->inter,    vw.inter,    VT_MAX_INTER);
    memcpy(p->utf8_buf, vw.utf8_buf, 4);
    memcpy(p->tabstops, vw.tabstops, sizeof(p->tabstops));
    memcpy(p->osc,      osc_buf,     VT_MAX_OSC);

    /* Restore screen metadata */
    Screen *s        = &p->scr;
    s->cur_row       = (int)sw.cur_row;
    s->cur_col       = (int)sw.cur_col;
    s->cur_visible   = sw.cur_visible;
    s->pending_wrap  = sw.pending_wrap;
    s->origin_mode   = sw.origin_mode;
    s->auto_wrap     = sw.auto_wrap;
    s->app_cursor    = sw.app_cursor;
    s->bracketed_paste = sw.bracketed_paste;
    s->insert_mode   = sw.insert_mode;
    s->in_alt_screen = sw.in_alt_screen;
    s->scroll_top    = (int)sw.scroll_top;
    s->scroll_bottom = (int)sw.scroll_bottom;
    s->cur_fg        = sw.cur_fg;
    s->cur_bg        = sw.cur_bg;
    s->cur_attrs     = sw.cur_attrs;
    s->saved_row     = (int)sw.saved_row;
    s->saved_col     = (int)sw.saved_col;
    s->saved_fg      = sw.saved_fg;
    s->saved_bg      = sw.saved_bg;
    s->saved_attrs   = sw.saved_attrs;
    s->saved_wrap    = sw.saved_wrap;
    s->alt_saved_row = (int)sw.alt_saved_row;
    s->alt_saved_col = (int)sw.alt_saved_col;
    s->alt_saved_fg  = sw.alt_saved_fg;
    s->alt_saved_bg  = sw.alt_saved_bg;
    s->alt_saved_attrs = sw.alt_saved_attrs;
    memcpy(s->title, sw.title, 256);

    /*
     * Read cell data row by row in logical order.  The fresh grid from
     * vt_init has sequential row pointers, which is exactly the logical
     * order we serialized, so each row lands in the right place.
     */
    size_t row_bytes = (size_t)s->cols * sizeof(Cell);
    for (int r = 0; r < s->rows; r++)
        if (vt_read_all(fd, s->cells[r], row_bytes) < 0) return -1;

    /* Alt cells */
    uint8_t has_alt;
    if (vt_read_all(fd, &has_alt, 1) < 0) return -1;
    if (has_alt) {
        if (!s->alt_cells)
            s->alt_cells = alloc_cells(s->rows, s->cols);
        if (!s->alt_cells) return -1;
        for (int r = 0; r < s->rows; r++)
            if (vt_read_all(fd, s->alt_cells[r], row_bytes) < 0) return -1;
    }

    return 0;
}


void vt_feed(VTParser *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        /*
         * CAN (0x18) and SUB (0x1A) cancel the current sequence anywhere
         * except GROUND (where they'd just be discarded anyway).
         */
        if ((c == 0x18 || c == 0x1A) && p->state != ST_GROUND) {
            p->state = ST_GROUND;
            continue;
        }

        /*
         * ESC (0x1B) interrupts and restarts a new sequence from any state.
         * Exception: if we're mid-OSC, ESC might begin the ST (ESC \).
         * We handle that by transitioning to ST_ESC; the following '\' will
         * be a harmless no-op in dispatch_esc.
         */
        if (c == 0x1B) {
            if (p->state == ST_OSC) dispatch_osc(p);
            csi_clear(p);
            p->state = ST_ESC;
            continue;
        }

        switch (p->state) {

        /* ---- GROUND ---- */
        case ST_GROUND:
            if (c < 0x20) {
                handle_c0(p, c);
            } else if (c == 0x7F) {
                /* DEL – ignore */
            } else {
                uint32_t cp;
                int r = utf8_feed(p, c, &cp);
                if (r == 1)  put_char(p, cp);
                /* r == 0: partial multibyte, state held in p->utf8_* */
                /* r == -1: invalid byte, skip */
            }
            break;

        /* ---- ESC ---- */
        case ST_ESC:
            if (c == '[') {
                /* Start of CSI */
                csi_clear(p);
                p->state = ST_CSI_PARAM;
            } else if (c == ']') {
                p->osc_len = 0;
                p->state   = ST_OSC;
            } else if (c == 'P' || c == '^' || c == '_') {
                /* DCS / PM / APC – ignore until ST */
                p->state = ST_DCS;
            } else if (c >= 0x20 && c <= 0x2F) {
                /* ESC intermediate byte (e.g. charset: ESC ( B) */
                p->state = ST_ESC_INTER;
            } else if (c >= 0x30 && c <= 0x7E) {
                dispatch_esc(p, (char)c);
                p->state = ST_GROUND;
            } else {
                p->state = ST_GROUND;
            }
            break;

        /* ---- ESC + intermediate byte(s) ---- */
        case ST_ESC_INTER:
            /* Consume exactly one final byte and return to ground */
            if (c >= 0x30 && c <= 0x7E)
                p->state = ST_GROUND;
            /* else stay: accumulate more intermediates (rare) */
            break;

        /* ---- CSI parameter collection ---- */
        case ST_CSI_PARAM:
            if (c >= '0' && c <= '9') {
                csi_digit(p, c - '0');
            } else if (c == ';') {
                csi_sep(p);
            } else if (c == '?') {
                p->priv = true;
            } else if (c == '>' || c == '<' || c == '=') {
                /* DEC modifier – record or ignore */
            } else if (c >= 0x20 && c <= 0x2F) {
                if (p->num_inter < VT_MAX_INTER)
                    p->inter[p->num_inter++] = (char)c;
                p->state = ST_CSI_INTER;
            } else if (c >= 0x40 && c <= 0x7E) {
                dispatch_csi(p, (char)c);
                p->state = ST_GROUND;
            } else {
                /* Anything else is an error – abort */
                p->state = ST_GROUND;
            }
            break;

        /* ---- CSI intermediate bytes ---- */
        case ST_CSI_INTER:
            if (c >= 0x20 && c <= 0x2F) {
                if (p->num_inter < VT_MAX_INTER)
                    p->inter[p->num_inter++] = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                dispatch_csi(p, (char)c);
                p->state = ST_GROUND;
            } else {
                p->state = ST_GROUND;
            }
            break;

        /* ---- OSC string ---- */
        case ST_OSC:
            if (c == 0x07) {   /* BEL terminates OSC (xterm extension) */
                dispatch_osc(p);
                p->state = ST_GROUND;
            } else if (c == 0x9C) { /* 8-bit ST */
                dispatch_osc(p);
                p->state = ST_GROUND;
            } else {
                if (p->osc_len < VT_MAX_OSC - 1)
                    p->osc[p->osc_len++] = (char)c;
            }
            break;

        /* ---- DCS / PM / APC – wait for ST ---- */
        case ST_DCS:
            if (c == 0x07 || c == 0x9C)
                p->state = ST_GROUND;
            /* ESC handled at top → ST_ESC; '\' will be a no-op */
            break;
        }
    }
}
