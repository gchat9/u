/* mc - two-panel file manager.
 *
 * The screen follows Midnight Commander 4.8 with its default skin, as drawn
 * on a 16-colour xterm. The configuration it assumes is the one that hides
 * the keybar, menubar, message line and xterm title (the [Layout] section of
 * ~/.config/mc/ini):
 *
 *     keybar_visible=false
 *     menubar_visible=false
 *     message_visible=false
 *     xterm_title=false
 *
 * Keys:
 *     Up/Down          select previous/next file or directory
 *     PgUp/PgDn        one screenful up/down, as mc
 *     Home/End         first/last item
 *     Tab              switch between panels
 *     Ctrl+O           show the shell, or the panels again (see below)
 *     Ctrl+U           swap panels around
 *     Enter            a directory or "..": go into it; an executable file:
 *                      run it, and show the panels again when it exits.
 *                      With something on the command line: run that with
 *                      $SHELL -c instead. An archive (.zip, .tar, .tgz,
 *                      .tar.gz, .txz, .tar.xz, .tar.zst, .tar.zstd) is entered
 *                      as a directory, with a title such as archive.zip/utar://
 *                      or archive.zip/uzip://; in it, ".." goes up, and at its
 *                      top, out of the archive. A file in an archive is only
 *                      read out, see vfs_view(): a copy of it is made under
 *                      TMPDIR, for copying or for mcview, and removed after.
 *     F3, ESC 3        mcview on the highlighted file (or enter a directory).
 *                      In an archive, on a copy of the file, made under TMPDIR
 *                      for mcview and removed when it exits.
 *     F5, ESC 5        copy the marked items, or else the highlighted one.
 *                      A question asks for the destination, which starts as the
 *                      other panel's directory; a directory there takes the items
 *                      under their own names. A directory is copied with what is
 *                      in it, and merged into one that exists. A file that exists
 *                      is asked about: Yes, No, Append, or All, Older, None,
 *                      Smaller, Size differs for the rest of the job. A progress
 *                      box follows; Esc (or A) stops the copy, and removes what
 *                      was being written. From an archive, the items are read
 *                      out in one pass over it, into a directory under TMPDIR,
 *                      and copied from there. An archive is read-only: a copy
 *                      into one is refused.
 *     F6, ESC 6        move (rename) the same way. Across file systems, a copy
 *                      and then the removal of the source. Refused in an archive.
 *     F8, ESC 8        delete the marked items, or else the highlighted one, after
 *                      asking. A non-empty directory is asked about again, once
 *                      for each directory of the selection: Yes, No, All (the
 *                      rest), None (the rest), or Abort. Refused in an archive.
 *     Insert           mark the highlighted item (or unmark it), and move down;
 *     Shift+Down/Up    the same, Shift+Up moving up. ".." is never marked.
 *     Ctrl+R           read the active panel's directory again
 *     Alt+C            quick cd: type a directory, Enter changes to it
 *     Alt+Enter        the highlighted name goes on the command line, at the
 *                      cursor, quoted for the shell, and a space after it
 *     Left/Right, Ctrl+Left/Right, Alt+Backspace, Backspace, Delete
 *                      edit the command line, and the quick cd box
 *     F9, ESC 9        the menu bar, on the side of the active panel. Left and
 *                      Right move between the titles; Down or Enter opens the
 *                      dropdown of Left or Right, whose one entry, Sort order...
 *                      (or its letter, s), opens the dialog for that panel. There
 *                      Tab and the arrows move, Space chooses or toggles, the
 *                      letters of the labels pick, OK applies and Esc cancels.
 *                      The sort is by name, unsorted, extension, size,
 *                      modify, access or change time, or inode (and version, if
 *                      VERSION_SORT in config.h is 1), reversed or not,
 *                      case sensitive or not, with executables after directories
 *                      or not; the directory is read again, the highlight staying
 *                      on its file. File, Command and Options are empty for now.
 *                      The Left and Right dropdowns also have Shell link... (h),
 *                      which opens a directory on another machine, sh://user@host/dir,
 *                      over ssh; "cd sh://user@host/dir" does the same on the command
 *                      line. Going up from its root goes back to the local directory
 *                      it was opened from. F3 fetches a file to view it; F5 and F6
 *                      copy and move between the link and this machine, or within the
 *                      link, and F8 removes files and empty directories there. Nothing
 *                      on the link is replaced: a name that exists is refused, and a
 *                      directory that is not empty is not removed. Enter on a file
 *                      there, and commands, are not done yet.
 *     +, -             select, unselect the entries matching a shell pattern (the
 *                      dialog: the last pattern is not offered at first, Enter
 *                      applies it, Esc cancels; directories are included, \"..\" never)
 *     *                invert the selection of the files (not of the directories)
 *     ESC 0, F10       exit (in the panels)
 *
 * The shell is $SHELL -i on a pseudo-terminal of its own. Ctrl+O starts one in
 * the active panel's directory, and shows it; while it is shown every key goes
 * to it, as typed, except Ctrl+O, which shows the panels again. Ctrl+O is
 * refused (a bell) while the shell runs a command: Ctrl+C first. Leaving ends
 * the shell, and no job it started is kept; the active panel then goes to the
 * directory the shell is in. The next Ctrl+O starts another, where the last
 * prompt was. When the shell exits, mc exits too. In the panels, "cd DIR" on
 * the command line is the same as Alt+C DIR.  A panel is read again when a
 * file has changed, after a command, a viewer, the shell or a file operation, see rescan_changed().  Alt+S is quick search: the info
 * line takes the letters typed, and the highlight goes to the first name that
 * begins with them, see qs_handle().  Enter on a media file (mp3, ogg,
 * opus, m4a, mp4, webm, mkv, gif, png, webp, jpg, jpeg) runs the viewer of
 * /etc/mc/mc.ext.ini for it, as mc does: the panels go while it runs, and a
 * viewer that fails leaves its output up until a key is pressed.
 *
 * Anything typed in the panels goes to the command line, shown after the
 * prompt. The prompt is PS1 (\u \h \w \$ and friends, and $NAME), or
 * mc's usual user@host:path$ when PS1 is not set.
 *
 * ESC and a following key count as one key if the key comes within a second.
 *
 * The sizes, the colours and the file classes are in config.h.
 *
 * Every frame is repainted in full. The terminal size is re-read when
 * SIGWINCH interrupts the wait for a key, so a resize repaints at once.
 *
 * The source is one translation unit, so that every function is static and the
 * compiler sees all of it: this file includes the others, in the order below. A part
 * calls only what the parts before it define (mc.h declares the few exceptions).
 *
 *   mc.h         types, state, the forward declarations     config.h  the parameters
 *   util.h       strings, UTF-8 and cell widths, paths, running a tool
 *   fmt.h        sizes and dates as the panels show them
 *   screen.h     the output buffer, the cells of a frame, the terminal modes
 *   input.h      keys, the line editor, the block of input the keys are taken from
 *   dialog.h     the error box, the quick cd box, the question box (drawn)
 *   sort.h       the order of a panel
 *   panel.h      entries, their classes, the cursor and the marks
 *   archive.h    the index of an archive, listed as a directory
 *   inflate.h    deflate;  extract.h  writing members safely;  zip.h, tar.h  the formats
 *   vfs.h        reading an archive in one pass, staging its members
 *   rshell.h     shell links: ssh, the protocol, the listing
 *   dir.h        reading a panel (directory, archive or link), and again
 *   cmdline.h    the prompt of the command line
 *   nav.h        quick cd, changing directory, entering archives and directories
 *   shell.h      the shell on a pseudo-terminal (Ctrl+O), running a program
 *   fileop.h     copy, move and delete of trees, the questions they ask, progress
 *   rops.h       the same on a shell link
 *   job.h        a job (F5, F6, F8): its items, one after another
 *   enter.h      Enter, F3 and the command line on an item
 *   fileop_ui.h  the boxes of a job: Delete, Copy, Move, Overwrite, progress
 *   menu.h       the menu bar, Sort order, Shell link
 *   select.h     + - * on the marks
 *   draw.h       the panels, and the whole frame
 */

#include "../_sys/_main.h"
#include "../_lib/width.h"
#include "../_lib/udiv64.h"

#include "mc.h"
#include "util.h"
#include "fmt.h"
#include "screen.h"
#include "input.h"
#include "dialog.h"
#include "sort.h"
#include "panel.h"
#include "archive.h"
#include "inflate.h"
#include "extract.h"
#include "zip.h"
#include "tar.h"
#include "vfs.h"
#include "rshell.h"
#include "dir.h"
#include "cmdline.h"
#include "nav.h"
#include "shell.h"
#include "fileop.h"
#include "rops.h"
#include "job.h"
#include "enter.h"
#include "fileop_ui.h"
#include "menu.h"
#include "select.h"
#include "draw.h"

/* ----------------------------------------------------------- start-up */

/* The directory to start in: getcwd() gives it with symbolic links resolved, but
 * the shell's PWD keeps the link the user came by. PWD is taken when it is an
 * absolute path to the same directory; otherwise the getcwd() path stays. */
static void start_dir(char *cwd, size_t max)
{
    const char *pwd = genv("PWD");
    struct statx a, b;

    if (!pwd || pwd[0] != '/' || strlen(pwd) >= max) return;
    if (statx(AT_FDCWD, pwd, 0, STATX_BASIC_STATS, &a) != 0) return;
    if (statx(AT_FDCWD, ".", 0, STATX_BASIC_STATS, &b) != 0) return;
    if (same_file(&a, &b)) strlcpy(cwd, pwd, max);
}

static void init(void)
{
    struct winsz ws;
    const char *u;
    long fd, n;

    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.col && ws.row) {
        cols = ws.col;
        rows = ws.row;
    }
    if (cols > MAXW) cols = MAXW;

    if (getcwd(P[0].path, sizeof P[0].path) < 0)
        strlcpy(P[0].path, "/", sizeof P[0].path);
    else
        start_dir(P[0].path, sizeof P[0].path);
    strlcpy(P[1].path, P[0].path, sizeof P[1].path);

    strlcpy(host, "localhost", sizeof host);
    fd = open("/proc/sys/kernel/hostname", O_RDONLY, 0);
    if (fd >= 0) {
        n = read((int)fd, host, sizeof host - 1);
        close(fd);
        if (n > 0) {
            if (host[n - 1] == '\n') n--;
            host[n] = 0;
        }
    }

    uid = getuid();
    user_name();
    if (!user[0]) {
        u = genv("USER");
        strlcpy(user, u ? u : "user", sizeof user);
    }

    fd = open("/etc/localtime", O_RDONLY, 0);
    if (fd >= 0) {
        tzn = read((int)fd, tzb, sizeof tzb);
        close(fd);
        if (tzn < 0) tzn = 0;
    }

    load(&P[0]);
    load(&P[1]);
}

/* ---------------------------------------------------------------- main */

/* Signal handler return path. x86_64 kernels need the program to supply the
 * rt_sigreturn sequence (SA_RESTORER); on the other architectures the kernel
 * provides its own, so no restorer is set. */
#if defined(__x86_64__)
__attribute__((naked)) static void restorer(void)
{
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}
#define NEED_RESTORER 1
#endif

/* SIGWINCH needs no work here: the handler only has to exist, so that a
 * blocked read() returns EINTR and the main loop repaints at the new size. */
static void on_winch(int sig)
{
    (void)sig;
}

static void install_winch(void)
{
    struct sigaction sa;

    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = on_winch;
    sa.sa_flags = 0;                           /* no SA_RESTART: read() returns EINTR */
#ifdef NEED_RESTORER
    sa.sa_flags |= SA_RESTORER;
    sa.sa_restorer = restorer;
#endif
    rt_sigaction(SIGWINCH, &sa, NULL, 8);
}

/* Struct copy without a memcpy call: the freestanding build has no libc, and
 * some targets would otherwise call an undefined memcpy. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void tcopy(struct ktermios *d, const struct ktermios *s)
{
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;

    for (size_t i = 0; i < sizeof *d; i++) dp[i] = sp[i];
}

/* The first entry, in display order from position FROM on and then round from
 * the top, whose name begins with the N bytes at PREF.  -1 if there is none. */
static int qs_find(const struct panel *p, int from, const char *pref, int n)
{
    if (p->n == 0) return -1;
    for (int j = 0; j < p->n; j++) {
        int i = (from + j) % p->n, k = 0;
        const char *nm = p->nm + p->e[p->ord[i]].name;

        while (k < n && nm[k] == pref[k]) k++;
        if (k == n) return i;
    }
    return -1;
}

/* Quick search (Alt+S): the letters typed are the beginning of a name, and the
 * highlight goes to the first such name from the highlighted one on, round
 * again at the end.  A letter that finds no such name is ignored.  Alt+S again
 * goes on to the next name that begins with the letters.  Esc leaves the search
 * where the highlight is; Backspace takes a letter back.  Any other
 * key leaves the search and is done as it is in the panels: Up and Down move
 * from the highlighted name, Enter and the F keys act on it. */
static int qs_handle(int key)
{
    struct panel *p = &P[act];

    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return 0;
    }
    if (key == (K_ALT | 's')) {                   /* the next name that begins with them */
        int at = qs_find(p, p->cur + 1, qline.buf, qline.len);

        if (at >= 0) {
            p->cur = at;
            scroll(p);
        }
        return 0;
    }
    if (key == 8 || key == 127) {                 /* a character goes, whole */
        if (qline.len) qline.len = char_prev(&qline, qline.len);
        qline.buf[qline.len] = 0;
        return 0;
    }
    if ((key >= 0x20 && key < 0x7f) || (key & K_CHAR)) {
        char b[4];
        int n, at;

        if (key & K_CHAR) {
            n = utf8_enc(b, (uint32_t)(key & ~K_CHAR));
        } else {
            b[0] = (char)key;
            n = 1;
        }
        if (qline.len + n >= (int)sizeof qline.buf) return 0;
        memcpy(qline.buf + qline.len, b, n);
        at = qs_find(p, p->cur, qline.buf, qline.len + n);
        if (at >= 0) {
            qline.len += n;
            qline.buf[qline.len] = 0;
            p->cur = at;
            scroll(p);
        }
        return 0;
    }
    mode = MODE_NORMAL;
    return handle_key(key);
}

/* The keys a shell link does not do yet: Enter on a file there (it would be run) and
 * a command line (it would run here, not there). They are refused, with an error box;
 * true if the key is refused. */
static int remote_refused(const struct panel *p, int key)
{
    int enter = key == '\r' || key == '\n';
    int onfile = enter && !cmdl.len && p->n && !is_dir(&p->e[p->ord[p->cur]]);
    int cmd = enter && cmdl.len;

    if (!((onfile || cmd) && p->rem)) return 0;
    set_error("Running on a shell link is not done yet", p->path, EINVAL);
    return 1;
}

static int handle_key(int key)
{
    struct panel *p = &P[act];

    if (mode == MODE_ERR) {                        /* any key closes the error box */
        mode = MODE_NORMAL;
        return 0;
    }
    if (mode == MODE_MENU) return menu_handle(key);
    if (mode == MODE_SORT) return sd_handle(key);
    if (mode == MODE_LINK) {
        lk_handle(key);
        return 0;
    }
    if (mode == MODE_GROUP) {
        gr_handle(key);
        return 0;
    }
    if (mode == MODE_QCD) {
        qcd_handle(key);
        return 0;
    }
    if (mode == MODE_CONFLICT) return cf_handle(key);
    if (mode == MODE_OP) return op_handle(key);
    if (mode == MODE_QUERY) return qy_handle(key);
    if (mode == MODE_QSEARCH) return qs_handle(key);
    if (key == K_F9 || key == (K_ALT | '9')) {
        menu_open();
        return 0;
    }
    if (key == (K_ALT | '0') || key == K_F10) return 1;
    if (key == (K_ALT | '3') || key == K_F3) {
        view_item(p);
        return 0;
    }
    if (key == (K_ALT | 'c')) {
        mode = MODE_QCD;
        line_clear(&qline);
        return 0;
    }
    if (key == (K_ALT | 's')) {
        mode = MODE_QSEARCH;
        line_clear(&qline);
        return 0;
    }
    /* Alt+Enter: the highlighted name goes on the command line (put_name) */
    if (key == (K_ALT | '\r') || key == (K_ALT | '\n')) {
        put_name(p);
        return 0;
    }
    if (remote_refused(p, key)) return 0;
    if (key == '\r' || key == '\n') {
        if (cmdl.len) run_command();
        else enter_item(p);
        return 0;
    }
    if (key == 9) act ^= 1;                        /* Tab */
    else if (key == 15) toggle_hidden();           /* Ctrl+O */
    else if (key == 18) panel_rescan(p);           /* Ctrl+R: read the directory again */
    else if (key == K_F8 || key == (K_ALT | '8')) delete_open(p);
    else if (key == K_F5 || key == (K_ALT | '5')) op_open(JOB_COPY);
    else if (key == K_F6 || key == (K_ALT | '6')) op_open(JOB_MOVE);
    else if (key == 21) lft ^= 1;                  /* Ctrl+U */
    else if (key == K_INS || key == K_SUP || key == K_SDOWN) sel_key(p, key);
    else if (cmdl.len == 0 && (key == '+' || key == '-')) gr_open(key == '+');
    else if (cmdl.len == 0 && key == '*') gr_invert(p);
    else if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN ||
             key == K_HOME || key == K_END)
        nav_key(p, key);
    else if (key == K_LEFT || key == K_RIGHT || key == K_CLEFT || key == K_CRIGHT ||
             key == K_ALTBS || key == K_DEL || key == 8 || key == 127 ||
             (key >= 0x20 && key < 0x7f) || (key >= 0x80 && key < 0x100) || (key & K_CHAR))
        line_key(&cmdl, key, cmd_width(), 0);
    return 0;
}

__attribute__((noreturn, noinline)) static void main(int argc, char **argv)
{
    int quit = 0;
    long n;

    envp = argv + argc + 1;
    init();
    {
        const char *v = genv("LC_ALL");

        if (!v || !*v) v = genv("LC_CTYPE");
        if (!v || !*v) v = genv("LANG");
        utf8_lines = v && has_utf8(v);
    }

    tty_ok = ioctl(0, TCGETS, &tty_saved) == 0;
    if (tty_ok) {
        tcopy(&tty_raw, &tty_saved);
        tty_raw.c_iflag &= ~(IXON | ICRNL);
        tty_raw.c_oflag &= ~OPOST;
        tty_raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        tty_raw.c_cc[VMIN] = 1;
        tty_raw.c_cc[VTIME] = 0;
    }
    install_winch();
    tty_enter();

    while (!quit) {
        check_size();
        draw();

        if (!wait_term()) continue;          /* SIGWINCH: the loop re-reads the size */
        n = read(0, kbuf, sizeof kbuf);
        if (n == -EINTR) continue;
        if (n <= 0) break;
        kbuf_n = (int)n;
        kbuf_i = 0;

        while (kbuf_i < kbuf_n && !quit) {
            int key;

            if (hidden) {                    /* the shell view: as typed, but Ctrl+O */
                int j = kbuf_i;

                while (j < kbuf_n && kbuf[j] != 15) j++;
                shell_in(kbuf + kbuf_i, j - kbuf_i);
                if (j == kbuf_n) {
                    kbuf_i = kbuf_n;
                    break;
                }
                kbuf_i = j + 1;
                toggle_hidden();
                continue;
            }
            key = take_key();
            if (key == K_NONE) continue;
            quit = handle_key(key);
        }
    }

    tty_leave();
    exit(0);
    __builtin_unreachable();
}
