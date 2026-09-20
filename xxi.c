#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TABSTOP 4
#define HINTW 18
#define HINTMIN 68
#define PANEROWS 10
#define PANEMAX 4000
#define MSGSECS 4
#define KCTRL(k) ((k) & 0x1f)

enum {
    K_BACKSPACE = 127,
    K_LEFT = 1000,
    K_RIGHT,
    K_UP,
    K_DOWN,
    K_HOME,
    K_END,
    K_PGUP,
    K_PGDN,
    K_DEL,
    K_MOUSE
};

enum { OP_INS_TEXT, OP_DEL_TEXT, OP_SPLIT, OP_JOIN, OP_INS_LINES, OP_DEL_LINES };
enum { TXN_OTHER, TXN_TYPE, TXN_ERASE };
enum { MODE_EDIT, MODE_SELECT };

typedef struct {
    char *s;
    int len;
    char *r;
    int rlen;
} Line;

typedef struct {
    char *s;
    int len;
    int cap;
} PLine;

typedef struct {
    char **text;
    int *len;
    int n;
} Chunk;

typedef struct {
    int type;
    int y;
    int x;
    char *text;
    int len;
    Chunk chunk;
} Op;

typedef struct {
    Op *op;
    int n;
    int cap;
    int kind;
    int lastc;
    int cy0;
    int cx0;
    int cy1;
    int cx1;
} Txn;

typedef struct {
    Txn *t;
    int n;
    int cap;
} Stack;

typedef struct {
    char *b;
    int len;
    int cap;
} Buf;

static struct {
    Line *row;
    int nrow;
    int caprow;
    int cx;
    int cy;
    int rx;
    int rowoff;
    int coloff;
    int rows;
    int cols;
    char *path;
    int dirty;
    int readonly;
    int savepoint;
    int nocoalesce;
    int mode;
    int any;
    int anx;
    int dragging;
    int dragedge;
    int marked;
    int confirmquit;
    int findopen;
    int findon;
    int findsy;
    int findsx;
    char find[128];
    int findlen;
    char msg[192];
    time_t msgt;
    int paneopen;
    int panefocus;
    int panedone;
    int panestatus;
    int paneoff;
    int pselon;
    int pdrag;
    int pedge;
    int psay;
    int psax;
    int psby;
    int psbx;
    int panewait;
    int runconfirm;
    char panecmd[4096];
    char panelabel[192];
    int panerows;
    int panefd;
    pid_t panepid;
} E;

static const char *c_off = "\x1b[m";
static const char *c_bar = "";
static const char *c_name = "";
static const char *c_stat = "";
static const char *c_edge = "";
static const char *c_key = "";
static const char *c_word = "";
static const char *c_head = "";
static const char *c_sel = "\x1b[7m";
static const char *c_hit = "\x1b[7m";

static const char *bx_h = "-";
static const char *bx_v = "|";
static const char *bx_tl = "+";
static const char *bx_tr = "+";
static const char *bx_bl = "+";
static const char *bx_br = "+";

static struct termios orig_termios;
static int raw_active;
static volatile sig_atomic_t winch_pending;
static volatile sig_atomic_t chld_pending;
static Stack undostk;
static Stack redostk;
static Txn tb;
static int tb_active;
static char *clipbuf;
static int cliplen;
static PLine *pl;
static int pn;
static int pcap;
static int pcur;
static int pcx;
static int pstate;
static char pseq[48];
static int pseqlen;
static int ms_b;
static int ms_x;
static int ms_y;
static int ms_press;

static void set_msg(const char *fmt, ...);
static void mark_dirty(void);
static int text_top(void);
static int text_height(void);
static void pane_resize(void);
static void pane_newline(void);
static void pane_write(const char *s, int n);
static void pane_finish(void);
static void psel_span(int *sy, int *sx, int *ey, int *ex);
static int stdin_ready(int ms);

static void emitn(const char *s, int n)
{
    int off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, s + off, (size_t)(n - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += (int)w;
    }
}

static void emit(const char *s)
{
    emitn(s, (int)strlen(s));
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        emit("\x1b[2J\x1b[H");
        fputs("xxi: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        emit("\x1b[2J\x1b[H");
        fputs("xxi: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static pid_t xwaitpid(pid_t pid, int *st)
{
    for (;;) {
        pid_t r = waitpid(pid, st, 0);
        if (r == -1 && errno == EINTR) continue;
        return r;
    }
}

static char *xmemdup(const char *s, int n)
{
    char *p = xmalloc((size_t)n + 1);
    if (n > 0) memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

static void disable_raw(void)
{
    if (raw_active) {
        emit("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_active = 0;
    }
}

static void die(const char *s)
{
    emit("\x1b[2J\x1b[H");
    disable_raw();
    perror(s);
    exit(1);
}

static void enable_raw(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    t = orig_termios;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(tcflag_t)(OPOST);
    t.c_cflag |= (tcflag_t)CS8;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == -1) die("tcsetattr");
    raw_active = 1;
    emit("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
}

static int u8_cont(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

static int u8_back(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && u8_cont((unsigned char)s[i])) i--;
    return i;
}

static int u8_fwd(const char *s, int len, int i)
{
    if (i >= len) return len;
    i++;
    while (i < len && u8_cont((unsigned char)s[i])) i++;
    return i;
}

static int term_has_color(void)
{
    const char *t = getenv("TERM");
    if (getenv("NO_COLOR")) return 0;
    if (!t || !*t || strcmp(t, "dumb") == 0) return 0;
    return strstr(t, "color") || strstr(t, "256") || strncmp(t, "xterm", 5) == 0 ||
           strncmp(t, "screen", 6) == 0 || strncmp(t, "tmux", 4) == 0 ||
           strncmp(t, "kitty", 5) == 0 || strncmp(t, "alacritty", 9) == 0 ||
           strncmp(t, "foot", 4) == 0 || strncmp(t, "rxvt", 4) == 0 ||
           strcmp(t, "linux") == 0;
}

static void detect_colors(void)
{
    const char *ct = getenv("COLORTERM");
    if (!term_has_color()) return;
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) {
        c_bar  = "\x1b[48;2;26;27;38m";
        c_name = "\x1b[38;2;187;154;247m";
        c_stat = "\x1b[38;2;122;162;247m";
        c_edge = "\x1b[38;2;122;98;178m";
        c_key  = "\x1b[38;2;122;162;247m";
        c_word = "\x1b[38;2;169;177;214m";
        c_head = "\x1b[38;2;157;124;216m";
        c_sel  = "\x1b[48;2;68;62;120;38;2;222;214;255m";
        c_hit  = "\x1b[48;2;122;162;247;38;2;20;21;32m";
    } else {
        c_bar  = "\x1b[48;5;235m";
        c_name = "\x1b[38;5;183m";
        c_stat = "\x1b[38;5;111m";
        c_edge = "\x1b[38;5;97m";
        c_key  = "\x1b[38;5;111m";
        c_word = "\x1b[38;5;146m";
        c_head = "\x1b[38;5;141m";
        c_sel  = "\x1b[48;5;60;38;5;189m";
        c_hit  = "\x1b[48;5;111;38;5;235m";
    }
}

static void detect_borders(void)
{
    const char *probe = "\x1b[H\xe2\x94\x82\x1b[6n";
    char b[128];
    char junk[64];
    int n = 0, row = 0, col = 0, i;
    emit(probe);
    while (n < (int)sizeof b - 1) {
        if (!stdin_ready(n == 0 ? 300 : 40)) break;
        if (read(STDIN_FILENO, b + n, 1) != 1) break;
        n++;
        if (b[n - 1] == 'R') break;
    }
    b[n] = '\0';
    while (stdin_ready(25)) {
        if (read(STDIN_FILENO, junk, sizeof junk) <= 0) break;
    }
    emit("\x1b[2J\x1b[H");
    for (i = 0; i + 2 < n; i++) {
        if (b[i] == '\x1b' && b[i + 1] == '[' &&
            sscanf(b + i + 2, "%d;%dR", &row, &col) == 2) {
            if (col == 2) {
                bx_h = "\xe2\x94\x80";
                bx_v = "\xe2\x94\x82";
                bx_tl = "\xe2\x94\x8c";
                bx_tr = "\xe2\x94\x90";
                bx_bl = "\xe2\x94\x94";
                bx_br = "\xe2\x94\x98";
            }
            return;
        }
    }
}

static void bput(Buf *b, const char *s, int n)
{
    if (n <= 0) return;
    if (b->len + n > b->cap) {
        int c = b->cap ? b->cap : 8192;
        while (c < b->len + n) c *= 2;
        b->b = xrealloc(b->b, (size_t)c);
        b->cap = c;
    }
    memcpy(b->b + b->len, s, (size_t)n);
    b->len += n;
}

static void bstr(Buf *b, const char *s)
{
    bput(b, s, (int)strlen(s));
}

static const char *plural(int n)
{
    return n == 1 ? "" : "s";
}

static int dispcols(const char *s, int n)
{
    int i, c = 0;
    for (i = 0; i < n; i++)
        if (!u8_cont((unsigned char)s[i])) c++;
    return c;
}

static int fit_tail(char *dst, int dstsz, const char *s, int budget)
{
    int i = 0, col = 0, n = (int)strlen(s);
    dst[0] = '\0';
    if (budget <= 0) return 0;
    while (i < n && col < budget) {
        int nx = u8_fwd(s, n, i);
        if (nx >= dstsz) break;
        i = nx;
        col++;
    }
    memmove(dst, s, (size_t)i);
    dst[i] = '\0';
    return i;
}

static void bpad(Buf *b, int n)
{
    while (n-- > 0) bput(b, " ", 1);
}

static void bfmt(Buf *b, const char *fmt, ...)
{
    char t[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof t - 1) n = (int)sizeof t - 1;
    bput(b, t, n);
}

static Chunk chunk_new(int n)
{
    Chunk c;
    c.n = n;
    c.text = xmalloc(sizeof(char *) * (size_t)(n > 0 ? n : 1));
    c.len = xmalloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    return c;
}

static void chunk_free(Chunk *c)
{
    int i;
    for (i = 0; i < c->n; i++) free(c->text[i]);
    free(c->text);
    free(c->len);
    c->text = NULL;
    c->len = NULL;
    c->n = 0;
}

static Chunk chunk_dup(const Chunk *src)
{
    Chunk c = chunk_new(src->n);
    int i;
    for (i = 0; i < src->n; i++) {
        c.text[i] = xmemdup(src->text[i], src->len[i]);
        c.len[i] = src->len[i];
    }
    return c;
}

static void row_render(Line *l)
{
    int i, j = 0, col = 0;
    free(l->r);
    l->r = xmalloc((size_t)l->len * TABSTOP + 1);
    for (i = 0; i < l->len; i++) {
        if (l->s[i] == '\t') {
            int adv = TABSTOP - (col % TABSTOP);
            while (adv-- > 0) {
                l->r[j++] = ' ';
                col++;
            }
        } else {
            l->r[j++] = l->s[i];
            if (!u8_cont((unsigned char)l->s[i])) col++;
        }
    }
    l->r[j] = '\0';
    l->rlen = j;
}

static int cx_to_rx(Line *l, int cx)
{
    int i, rx = 0;
    if (cx > l->len) cx = l->len;
    for (i = 0; i < cx; i++) {
        if (l->s[i] == '\t') rx += TABSTOP - (rx % TABSTOP);
        else if (!u8_cont((unsigned char)l->s[i])) rx++;
    }
    return rx;
}

static void rows_reserve(int n)
{
    int c;
    if (n <= E.caprow) return;
    c = E.caprow ? E.caprow : 64;
    while (c < n) c *= 2;
    E.row = xrealloc(E.row, sizeof(Line) * (size_t)c);
    E.caprow = c;
}

static void raw_ins_line(int at, const char *s, int len)
{
    rows_reserve(E.nrow + 1);
    memmove(&E.row[at + 1], &E.row[at], sizeof(Line) * (size_t)(E.nrow - at));
    E.row[at].s = xmemdup(s, len);
    E.row[at].len = len;
    E.row[at].r = NULL;
    E.row[at].rlen = 0;
    row_render(&E.row[at]);
    E.nrow++;
}

static void raw_del_line(int at)
{
    free(E.row[at].s);
    free(E.row[at].r);
    memmove(&E.row[at], &E.row[at + 1], sizeof(Line) * (size_t)(E.nrow - at - 1));
    E.nrow--;
}

static void raw_ins_text(int y, int x, const char *s, int len)
{
    Line *l = &E.row[y];
    l->s = xrealloc(l->s, (size_t)l->len + (size_t)len + 1);
    memmove(l->s + x + len, l->s + x, (size_t)(l->len - x));
    memcpy(l->s + x, s, (size_t)len);
    l->len += len;
    l->s[l->len] = '\0';
    row_render(l);
}

static void raw_del_text(int y, int x, int len)
{
    Line *l = &E.row[y];
    memmove(l->s + x, l->s + x + len, (size_t)(l->len - x - len));
    l->len -= len;
    l->s[l->len] = '\0';
    row_render(l);
}

static void raw_split(int y, int x)
{
    Line *l = &E.row[y];
    raw_ins_line(y + 1, l->s + x, l->len - x);
    l = &E.row[y];
    l->len = x;
    l->s[x] = '\0';
    row_render(l);
}

static void raw_join(int y)
{
    int alen = E.row[y].len;
    raw_ins_text(y, alen, E.row[y + 1].s, E.row[y + 1].len);
    raw_del_line(y + 1);
}

static void op_free(Op *o)
{
    free(o->text);
    o->text = NULL;
    chunk_free(&o->chunk);
}

static void txn_free(Txn *t)
{
    int i;
    for (i = 0; i < t->n; i++) op_free(&t->op[i]);
    free(t->op);
    memset(t, 0, sizeof *t);
}

static void stack_push(Stack *s, Txn t)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->t = xrealloc(s->t, sizeof(Txn) * (size_t)s->cap);
    }
    s->t[s->n++] = t;
}

static void stack_clear(Stack *s)
{
    while (s->n > 0) txn_free(&s->t[--s->n]);
}

static Op op_blank(int type)
{
    Op o;
    memset(&o, 0, sizeof o);
    o.type = type;
    return o;
}

static void txn_push(Op o)
{
    if (tb.n == tb.cap) {
        tb.cap = tb.cap ? tb.cap * 2 : 8;
        tb.op = xrealloc(tb.op, sizeof(Op) * (size_t)tb.cap);
    }
    tb.op[tb.n++] = o;
}

static void txn_begin(int kind, int c)
{
    if (tb_active) return;
    stack_clear(&redostk);
    if (kind != TXN_OTHER && !E.nocoalesce && undostk.n > 0) {
        Txn *p = &undostk.t[undostk.n - 1];
        int joinable = p->kind == kind && p->cy1 == E.cy && p->cx1 == E.cx;
        if (joinable && kind == TXN_TYPE)
            joinable = isspace((unsigned char)c) == isspace((unsigned char)p->lastc);
        if (joinable) {
            tb = *p;
            undostk.n--;
            tb.lastc = c;
            tb_active = 1;
            return;
        }
    }
    E.nocoalesce = 0;
    memset(&tb, 0, sizeof tb);
    tb.kind = kind;
    tb.lastc = c;
    tb.cy0 = E.cy;
    tb.cx0 = E.cx;
    tb_active = 1;
}

static void txn_commit(void)
{
    if (!tb_active) return;
    tb_active = 0;
    if (tb.n == 0) {
        txn_free(&tb);
        return;
    }
    tb.cy1 = E.cy;
    tb.cx1 = E.cx;
    E.msg[0] = '\0';
    stack_push(&undostk, tb);
    memset(&tb, 0, sizeof tb);
    mark_dirty();
}

static void do_ins_text(int y, int x, const char *s, int len)
{
    Op o = op_blank(OP_INS_TEXT);
    if (len <= 0) return;
    raw_ins_text(y, x, s, len);
    o.y = y;
    o.x = x;
    o.text = xmemdup(s, len);
    o.len = len;
    txn_push(o);
}

static void do_del_text(int y, int x, int len)
{
    Op o = op_blank(OP_DEL_TEXT);
    if (len <= 0) return;
    o.y = y;
    o.x = x;
    o.text = xmemdup(E.row[y].s + x, len);
    o.len = len;
    raw_del_text(y, x, len);
    txn_push(o);
}

static void do_split(int y, int x)
{
    Op o = op_blank(OP_SPLIT);
    raw_split(y, x);
    o.y = y;
    o.x = x;
    txn_push(o);
}

static void do_join(int y)
{
    Op o = op_blank(OP_JOIN);
    o.y = y;
    o.x = E.row[y].len;
    raw_join(y);
    txn_push(o);
}

static void do_ins_lines(int at, const Chunk *c)
{
    Op o = op_blank(OP_INS_LINES);
    int i;
    for (i = 0; i < c->n; i++) raw_ins_line(at + i, c->text[i], c->len[i]);
    o.y = at;
    o.chunk = chunk_dup(c);
    txn_push(o);
}

static void do_del_lines(int at, int n)
{
    Op o = op_blank(OP_DEL_LINES);
    int i;
    if (n <= 0) return;
    o.chunk = chunk_new(n);
    for (i = 0; i < n; i++) {
        o.chunk.text[i] = xmemdup(E.row[at + i].s, E.row[at + i].len);
        o.chunk.len[i] = E.row[at + i].len;
    }
    for (i = 0; i < n; i++) raw_del_line(at);
    o.y = at;
    txn_push(o);
}

static void op_undo(Op *o)
{
    int i;
    switch (o->type) {
    case OP_INS_TEXT:
        raw_del_text(o->y, o->x, o->len);
        break;
    case OP_DEL_TEXT:
        raw_ins_text(o->y, o->x, o->text, o->len);
        break;
    case OP_SPLIT:
        raw_join(o->y);
        break;
    case OP_JOIN:
        raw_split(o->y, o->x);
        break;
    case OP_INS_LINES:
        for (i = 0; i < o->chunk.n; i++) raw_del_line(o->y);
        break;
    case OP_DEL_LINES:
        for (i = 0; i < o->chunk.n; i++)
            raw_ins_line(o->y + i, o->chunk.text[i], o->chunk.len[i]);
        break;
    }
}

static void op_redo(Op *o)
{
    int i;
    switch (o->type) {
    case OP_INS_TEXT:
        raw_ins_text(o->y, o->x, o->text, o->len);
        break;
    case OP_DEL_TEXT:
        raw_del_text(o->y, o->x, o->len);
        break;
    case OP_SPLIT:
        raw_split(o->y, o->x);
        break;
    case OP_JOIN:
        raw_join(o->y);
        break;
    case OP_INS_LINES:
        for (i = 0; i < o->chunk.n; i++)
            raw_ins_line(o->y + i, o->chunk.text[i], o->chunk.len[i]);
        break;
    case OP_DEL_LINES:
        for (i = 0; i < o->chunk.n; i++) raw_del_line(o->y);
        break;
    }
}

static void mark_dirty(void)
{
    E.dirty = undostk.n != E.savepoint;
}

static void clamp_cursor(void)
{
    if (E.nrow <= 0) return;
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.nrow) E.cy = E.nrow - 1;
    if (E.any < 0) E.any = 0;
    if (E.any >= E.nrow) E.any = E.nrow - 1;
    if (E.anx < 0) E.anx = 0;
    if (E.anx > E.row[E.any].len) E.anx = E.row[E.any].len;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.row[E.cy].len) E.cx = E.row[E.cy].len;
}

static void cmd_undo(void)
{
    Txn t;
    int i;
    txn_commit();
    if (undostk.n == 0) {
        set_msg("nothing to undo");
        return;
    }
    t = undostk.t[--undostk.n];
    for (i = t.n - 1; i >= 0; i--) op_undo(&t.op[i]);
    E.cy = t.cy0;
    E.cx = t.cx0;
    clamp_cursor();
    stack_push(&redostk, t);
    E.nocoalesce = 1;
    mark_dirty();
    set_msg("undo  %d back, %d forward", undostk.n, redostk.n);
}

static void cmd_redo(void)
{
    Txn t;
    int i;
    txn_commit();
    if (redostk.n == 0) {
        set_msg("nothing to redo");
        return;
    }
    t = redostk.t[--redostk.n];
    for (i = 0; i < t.n; i++) op_redo(&t.op[i]);
    E.cy = t.cy1;
    E.cx = t.cx1;
    clamp_cursor();
    stack_push(&undostk, t);
    E.nocoalesce = 1;
    mark_dirty();
    set_msg("redo  %d back, %d forward", undostk.n, redostk.n);
}

static void cmd_insert_char(int c)
{
    char ch = (char)c;
    txn_begin(TXN_TYPE, c);
    do_ins_text(E.cy, E.cx, &ch, 1);
    E.cx++;
    txn_commit();
}

static void cmd_newline(void)
{
    txn_begin(TXN_OTHER, 0);
    do_split(E.cy, E.cx);
    E.cy++;
    E.cx = 0;
    txn_commit();
}

static void cmd_backspace(void)
{
    if (E.cx > 0) {
        int p = u8_back(E.row[E.cy].s, E.cx);
        txn_begin(TXN_ERASE, 0);
        do_del_text(E.cy, p, E.cx - p);
        E.cx = p;
        txn_commit();
    } else if (E.cy > 0) {
        int plen = E.row[E.cy - 1].len;
        txn_begin(TXN_OTHER, 0);
        do_join(E.cy - 1);
        E.cy--;
        E.cx = plen;
        txn_commit();
    }
}

static void cmd_delete(void)
{
    if (E.cx < E.row[E.cy].len) {
        int nx = u8_fwd(E.row[E.cy].s, E.row[E.cy].len, E.cx);
        txn_begin(TXN_OTHER, 0);
        do_del_text(E.cy, E.cx, nx - E.cx);
        txn_commit();
    } else if (E.cy < E.nrow - 1) {
        txn_begin(TXN_OTHER, 0);
        do_join(E.cy);
        txn_commit();
    }
}

static int sel_on(void)
{
    return E.mode == MODE_SELECT || E.marked;
}

static void sel_span(int *sy, int *sx, int *ey, int *ex)
{
    if (!sel_on()) {
        *sy = E.cy;
        *sx = 0;
        *ey = E.cy;
        *ex = E.row[E.cy].len;
        return;
    }
    if (E.any < E.cy || (E.any == E.cy && E.anx <= E.cx)) {
        *sy = E.any;
        *sx = E.anx;
        *ey = E.cy;
        *ex = E.cx;
    } else {
        *sy = E.cy;
        *sx = E.cx;
        *ey = E.any;
        *ex = E.anx;
    }
}

static int sel_empty(void)
{
    int sy, sx, ey, ex;
    sel_span(&sy, &sx, &ey, &ex);
    return sy == ey && sx == ex;
}

static void delete_span(void)
{
    int sy, sx, ey, ex;
    sel_span(&sy, &sx, &ey, &ex);
    if (sy == ey && sx == ex) return;
    txn_begin(TXN_OTHER, 0);
    if (sy == ey) {
        do_del_text(sy, sx, ex - sx);
    } else {
        do_del_text(sy, sx, E.row[sy].len - sx);
        if (ey - sy - 1 > 0) do_del_lines(sy + 1, ey - sy - 1);
        if (ex > 0) do_del_text(sy + 1, 0, ex);
        do_join(sy);
    }
    E.cy = sy;
    E.cx = sx;
    txn_commit();
    mark_dirty();
}

static void delete_range(int a, int b)
{
    int n = b - a + 1;
    txn_begin(TXN_OTHER, 0);
    if (n >= E.nrow) {
        Chunk c = chunk_new(1);
        c.text[0] = xmemdup("", 0);
        c.len[0] = 0;
        do_del_lines(0, E.nrow);
        do_ins_lines(0, &c);
        chunk_free(&c);
        E.cy = 0;
    } else {
        do_del_lines(a, n);
        E.cy = a >= E.nrow ? E.nrow - 1 : a;
    }
    E.cx = 0;
    txn_commit();
}

static void cmd_nuke(void)
{
    if (E.mode == MODE_SELECT) {
        int sy, sx, ey, ex;
        sel_span(&sy, &sx, &ey, &ex);
        if (sel_empty()) {
            E.mode = MODE_EDIT;
            set_msg("nothing selected");
            return;
        }
        delete_span();
        E.mode = MODE_EDIT;
        E.dragging = 0;
        E.dragedge = 0;
        set_msg("deleted selection  (^R brings it back)");
    } else {
        delete_range(E.cy, E.cy);
        set_msg("line nuked  (^R brings it back)");
    }
}

static char *sel_text(int *outlen)
{
    int sy, sx, ey, ex, i;
    Buf t;
    memset(&t, 0, sizeof t);
    sel_span(&sy, &sx, &ey, &ex);
    if (!sel_on()) {
        bput(&t, E.row[E.cy].s, E.row[E.cy].len);
        bput(&t, "\n", 1);
    } else if (sy == ey) {
        bput(&t, E.row[sy].s + sx, ex - sx);
    } else {
        bput(&t, E.row[sy].s + sx, E.row[sy].len - sx);
        bput(&t, "\n", 1);
        for (i = sy + 1; i < ey; i++) {
            bput(&t, E.row[i].s, E.row[i].len);
            bput(&t, "\n", 1);
        }
        bput(&t, E.row[ey].s, ex);
    }
    *outlen = t.len;
    if (!t.b) t.b = xmemdup("", 0);
    return t.b;
}

static void clip_set(const char *s, int len)
{
    int fd[2];
    pid_t pid;
    int off = 0;
    free(clipbuf);
    clipbuf = xmemdup(s, len);
    cliplen = len;
    if (pipe(fd) == -1) return;
    pid = fork();
    if (pid == -1) {
        close(fd[0]);
        close(fd[1]);
        return;
    }
    if (pid == 0) {
        int nul;
        close(fd[1]);
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        nul = open("/dev/null", O_WRONLY);
        if (nul != -1) {
            dup2(nul, STDOUT_FILENO);
            dup2(nul, STDERR_FILENO);
            close(nul);
        }
        execlp("wl-copy", "wl-copy", "--type", "text/plain", (char *)NULL);
        execlp("xclip", "xclip", "-selection", "clipboard", (char *)NULL);
        execlp("xsel", "xsel", "--clipboard", "--input", (char *)NULL);
        _exit(127);
    }
    close(fd[0]);
    while (off < len) {
        ssize_t w = write(fd[1], s + off, (size_t)(len - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += (int)w;
    }
    close(fd[1]);
    xwaitpid(pid, NULL);
}

static char *clip_get(int *outlen)
{
    int fd[2];
    pid_t pid;
    int st = 0;
    Buf t;
    memset(&t, 0, sizeof t);
    *outlen = 0;
    if (pipe(fd) == -1) goto fallback;
    pid = fork();
    if (pid == -1) {
        close(fd[0]);
        close(fd[1]);
        goto fallback;
    }
    if (pid == 0) {
        int nul;
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
        nul = open("/dev/null", O_RDWR);
        if (nul != -1) {
            dup2(nul, STDIN_FILENO);
            dup2(nul, STDERR_FILENO);
            close(nul);
        }
        execlp("wl-paste", "wl-paste", "--no-newline", (char *)NULL);
        execlp("xclip", "xclip", "-selection", "clipboard", "-o", (char *)NULL);
        execlp("xsel", "xsel", "--clipboard", "--output", (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    for (;;) {
        char b[4096];
        ssize_t r = read(fd[0], b, sizeof b);
        if (r > 0) {
            bput(&t, b, (int)r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    close(fd[0]);
    xwaitpid(pid, &st);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && t.len > 0) {
        *outlen = t.len;
        return t.b;
    }
    free(t.b);
fallback:
    if (!clipbuf || cliplen == 0) return NULL;
    *outlen = cliplen;
    return xmemdup(clipbuf, cliplen);
}

static char *psel_text(int *outlen)
{
    int sy, sx, ey, ex, i;
    Buf t;
    memset(&t, 0, sizeof t);
    psel_span(&sy, &sx, &ey, &ex);
    if (sy < 0) sy = 0;
    if (ey >= pn) ey = pn - 1;
    for (i = sy; i <= ey && i < pn; i++) {
        int a = i == sy ? sx : 0;
        int b = i == ey ? ex : pl[i].len;
        if (a > pl[i].len) a = pl[i].len;
        if (b > pl[i].len) b = pl[i].len;
        while (b > a && pl[i].s[b - 1] == ' ') b--;
        if (b > a) bput(&t, pl[i].s + a, b - a);
        if (i < ey) bput(&t, "\n", 1);
    }
    *outlen = t.len;
    if (!t.b) t.b = xmemdup("", 0);
    return t.b;
}

static void cmd_copy_pane(void)
{
    int len;
    char *s = psel_text(&len);
    if (len == 0) {
        free(s);
        E.pselon = 0;
        set_msg("nothing selected in the terminal");
        return;
    }
    clip_set(s, len);
    free(s);
    E.pselon = 0;
    E.pdrag = 0;
    E.pedge = 0;
    set_msg("copied %d byte%s from the terminal", len, plural(len));
}

static void cmd_copy(void)
{
    int len;
    char *s = sel_text(&len);
    clip_set(s, len);
    free(s);
    if (sel_on()) {
        E.mode = MODE_EDIT;
        E.marked = 0;
        E.dragging = 0;
        E.dragedge = 0;
        set_msg("copied %d byte%s", len, plural(len));
    } else {
        set_msg("copied line %d", E.cy + 1);
    }
}

static void cmd_paste(void)
{
    int len = 0, i, start, kept = 0;
    char *s = clip_get(&len);
    if (!s || len == 0) {
        free(s);
        set_msg("clipboard is empty");
        return;
    }
    for (i = 0; i < len; i++)
        if (s[i] != '\r') s[kept++] = s[i];
    len = kept;
    if (E.mode == MODE_SELECT) E.mode = MODE_EDIT;
    txn_begin(TXN_OTHER, 0);
    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || s[i] == '\n') {
            if (i > start) {
                do_ins_text(E.cy, E.cx, s + start, i - start);
                E.cx += i - start;
            }
            if (i < len) {
                do_split(E.cy, E.cx);
                E.cy++;
                E.cx = 0;
            }
            start = i + 1;
        }
    }
    txn_commit();
    free(s);
    set_msg("pasted %d bytes", len);
}

static int path_readonly(void)
{
    struct stat st;
    if (!E.path) return 0;
    if (stat(E.path, &st) == -1) return 0;
    if (!S_ISREG(st.st_mode)) return 1;
    return access(E.path, W_OK) == -1;
}

static void open_file(const char *path)
{
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    E.path = xmemdup(path, (int)strlen(path));
    f = fopen(path, "r");
    if (!f) {
        raw_ins_line(0, "", 0);
        if (errno == ENOENT) set_msg("new file  %s", path);
        else set_msg("cannot read %s: %s", path, strerror(errno));
        return;
    }
    while ((n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        raw_ins_line(E.nrow, line, (int)n);
    }
    free(line);
    fclose(f);
    if (E.nrow == 0) raw_ins_line(0, "", 0);
    E.readonly = path_readonly();
    if (E.readonly) set_msg("%s  %d line%s  - READ ONLY, cannot save", path, E.nrow, plural(E.nrow));
    else set_msg("%s  %d line%s", path, E.nrow, plural(E.nrow));
}

static int save_file(void)
{
    char tmp[4096];
    const char *slash;
    struct stat st;
    mode_t mode = 0644;
    Buf out;
    int fd, i, off = 0, ok = 1, total;
    if (!E.path) return -1;
    memset(&out, 0, sizeof out);
    if (stat(E.path, &st) == 0) mode = st.st_mode & 07777;
    slash = strrchr(E.path, '/');
    if (slash)
        snprintf(tmp, sizeof tmp, "%.*s.xxi.%ld.tmp",
                 (int)(slash - E.path + 1), E.path, (long)getpid());
    else
        snprintf(tmp, sizeof tmp, ".xxi.%ld.tmp", (long)getpid());
    for (i = 0; i < E.nrow; i++) {
        bput(&out, E.row[i].s, E.row[i].len);
        bput(&out, "\n", 1);
    }
    total = out.len;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd == -1) {
        free(out.b);
        return -1;
    }
    while (off < out.len) {
        ssize_t w = write(fd, out.b + off, (size_t)(out.len - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            ok = 0;
            break;
        }
        off += (int)w;
    }
    if (ok && fsync(fd) == -1) ok = 0;
    if (close(fd) == -1) ok = 0;
    free(out.b);
    if (!ok || rename(tmp, E.path) == -1) {
        unlink(tmp);
        return -1;
    }
    return total;
}

static void cmd_save(void)
{
    int n;
    txn_commit();
    E.readonly = path_readonly();
    if (E.readonly) {
        set_msg("%s is read only - not saved", E.path ? E.path : "file");
        return;
    }
    n = save_file();
    if (n < 0) {
        set_msg("save failed: %s", strerror(errno));
        return;
    }
    E.savepoint = undostk.n;
    E.nocoalesce = 1;
    mark_dirty();
    set_msg("saved  %s  %d bytes  %d line%s", E.path, n, E.nrow, plural(E.nrow));
}

static int find_in(const char *hay, int haylen, const char *needle, int nlen, int from)
{
    int i;
    if (nlen <= 0 || nlen > haylen) return -1;
    if (from < 0) from = 0;
    for (i = from; i + nlen <= haylen; i++)
        if (memcmp(hay + i, needle, (size_t)nlen) == 0) return i;
    return -1;
}

static int find_count(void)
{
    int i, c = 0, p;
    if (E.findlen <= 0) return 0;
    for (i = 0; i < E.nrow; i++) {
        p = 0;
        while ((p = find_in(E.row[i].s, E.row[i].len, E.find, E.findlen, p)) != -1) {
            c++;
            p += E.findlen;
        }
    }
    return c;
}

static int find_at(int y, int x)
{
    int i, p;
    if (E.findlen <= 0 || E.nrow == 0) return 0;
    p = find_in(E.row[y].s, E.row[y].len, E.find, E.findlen, x);
    if (p != -1) {
        E.cy = y;
        E.cx = p;
        return 1;
    }
    for (i = 1; i <= E.nrow; i++) {
        int t = (y + i) % E.nrow;
        p = find_in(E.row[t].s, E.row[t].len, E.find, E.findlen, 0);
        if (p != -1) {
            E.cy = t;
            E.cx = p;
            return 1;
        }
    }
    return 0;
}

static int pane_col_to_byte(const PLine *l, int col)
{
    int i = 0, c = 0;
    while (i < l->len && c < col) {
        i++;
        while (i < l->len && u8_cont((unsigned char)l->s[i])) i++;
        c++;
    }
    return i;
}

static void pane_clamp_off(void)
{
    int maxoff = pn - E.panerows;
    if (maxoff < 0) maxoff = 0;
    if (E.paneoff > maxoff) E.paneoff = maxoff;
    if (E.paneoff < 0) E.paneoff = 0;
}

static int pane_first(void)
{
    int f;
    pane_clamp_off();
    f = pn - E.panerows - E.paneoff;
    if (f < 0) f = 0;
    return f;
}

static void psel_span(int *sy, int *sx, int *ey, int *ex)
{
    if (E.psay < E.psby || (E.psay == E.psby && E.psax <= E.psbx)) {
        *sy = E.psay;
        *sx = E.psax;
        *ey = E.psby;
        *ex = E.psbx;
    } else {
        *sy = E.psby;
        *sx = E.psbx;
        *ey = E.psay;
        *ex = E.psax;
    }
}

static void pane_clear(void)
{
    int i;
    for (i = 0; i < pn; i++) free(pl[i].s);
    pn = 0;
    pcur = 0;
    pcx = 0;
    pstate = 0;
    pseqlen = 0;
    E.paneoff = 0;
    E.pselon = 0;
    E.pdrag = 0;
    E.pedge = 0;
}

static void pane_newline(void)
{
    if (pn + 1 > pcap) {
        pcap = pcap ? pcap * 2 : 256;
        pl = xrealloc(pl, sizeof(PLine) * (size_t)pcap);
    }
    pl[pn].s = xmalloc(64);
    pl[pn].s[0] = '\0';
    pl[pn].len = 0;
    pl[pn].cap = 64;
    pn++;
    if (pn > PANEMAX) {
        int drop = pn - PANEMAX, i;
        for (i = 0; i < drop; i++) free(pl[i].s);
        memmove(pl, pl + drop, sizeof(PLine) * (size_t)(pn - drop));
        pn -= drop;
        E.psay -= drop;
        E.psby -= drop;
        if (E.psay < 0 || E.psby < 0) E.pselon = 0;
        if (E.psay < 0) E.psay = 0;
        if (E.psby < 0) E.psby = 0;
    }
    if (E.paneoff > 0) E.paneoff++;
    pane_clamp_off();
    pcur = pn - 1;
    pcx = 0;
}

static void pane_put(char c)
{
    PLine *l;
    if (pn == 0) pane_newline();
    l = &pl[pcur];
    if (pcx + 2 > l->cap) {
        int n = l->cap ? l->cap : 64;
        while (n < pcx + 2) n *= 2;
        l->s = xrealloc(l->s, (size_t)n);
        l->cap = n;
    }
    while (l->len < pcx) l->s[l->len++] = ' ';
    l->s[pcx] = c;
    pcx++;
    if (pcx > l->len) l->len = pcx;
    l->s[l->len] = '\0';
}

static void pane_csi(char final)
{
    int a = 0, has = 0, i;
    PLine *l;
    for (i = 0; i < pseqlen; i++) {
        if (pseq[i] >= '0' && pseq[i] <= '9') {
            a = a * 10 + (pseq[i] - '0');
            has = 1;
        } else {
            break;
        }
    }
    if (!has) a = 0;
    if (pn == 0) pane_newline();
    l = &pl[pcur];
    switch (final) {
    case 'K':
        if (a == 0) {
            if (l->len > pcx) l->len = pcx;
        } else if (a == 1) {
            for (i = 0; i < pcx && i < l->len; i++) l->s[i] = ' ';
        } else {
            l->len = 0;
        }
        l->s[l->len] = '\0';
        break;
    case 'J':
        if (a >= 2) {
            pane_clear();
            pane_newline();
        }
        break;
    case 'C':
        pcx += a > 0 ? a : 1;
        break;
    case 'D':
        pcx -= a > 0 ? a : 1;
        if (pcx < 0) pcx = 0;
        break;
    case 'G':
        pcx = a > 0 ? a - 1 : 0;
        break;
    case 'c':
        if (E.panefd >= 0) {
            if (pseqlen > 0 && pseq[0] == '>') pane_write("\x1b[>0;10;0c", 10);
            else if (pseqlen > 0 && pseq[0] == '=') pane_write("\x1bP!|00000000\x1b\\", 14);
            else pane_write("\x1b[?1;2c", 7);
        }
        break;
    case 'n':
        if (a == 5 && E.panefd >= 0) {
            pane_write("\x1b[0n", 4);
        } else if (a == 6 && E.panefd >= 0) {
            char rep[32];
            int first = pn - E.panerows;
            int n;
            if (first < 0) first = 0;
            n = snprintf(rep, sizeof rep, "\x1b[%d;%dR", pcur - first + 1, pcx + 1);
            pane_write(rep, n);
        }
        break;
    default:
        break;
    }
}

static void pane_feed(const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];
        if (pstate == 1) {
            if (c == '[') {
                pstate = 2;
                pseqlen = 0;
            } else if (c == ']') {
                pstate = 3;
            } else if (c == '(' || c == ')' || c == '#' || c == '%') {
                pstate = 4;
            } else {
                pstate = 0;
            }
            continue;
        }
        if (pstate == 2) {
            if (c >= 0x40 && c <= 0x7e) {
                pane_csi((char)c);
                pstate = 0;
            } else if (pseqlen < (int)sizeof pseq - 1) {
                pseq[pseqlen++] = (char)c;
            }
            continue;
        }
        if (pstate == 3) {
            if (c == 0x07) pstate = 0;
            else if (c == 0x1b) pstate = 5;
            continue;
        }
        if (pstate == 4 || pstate == 5) {
            pstate = 0;
            continue;
        }
        if (c == 0x1b) {
            pstate = 1;
            continue;
        }
        if (c == '\n') {
            pane_newline();
            continue;
        }
        if (c == '\r') {
            pcx = 0;
            continue;
        }
        if (c == '\b') {
            if (pcx > 0) pcx--;
            continue;
        }
        if (c == '\t') {
            do {
                pane_put(' ');
            } while (pcx % 8 != 0);
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        pane_put((char)c);
    }
}

static void pane_resize(void)
{
    struct winsize ws;
    if (E.panefd < 0) return;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)(E.panerows > 0 ? E.panerows : 1);
    ws.ws_col = (unsigned short)(E.cols > 0 ? E.cols : 80);
    ioctl(E.panefd, TIOCSWINSZ, &ws);
}

static void pane_close(void)
{
    if (!E.paneopen) return;
    if (E.panepid > 0) {
        int tries;
        if (kill(-E.panepid, SIGHUP) == -1) kill(E.panepid, SIGHUP);
        for (tries = 0; tries < 20; tries++) {
            pid_t r = waitpid(E.panepid, NULL, WNOHANG);
            if (r == E.panepid || (r == -1 && errno != EINTR)) break;
            usleep(10000);
        }
        if (tries >= 20) {
            if (kill(-E.panepid, SIGKILL) == -1) kill(E.panepid, SIGKILL);
            xwaitpid(E.panepid, NULL);
        }
    }
    if (E.panefd >= 0) close(E.panefd);
    E.panefd = -1;
    E.panepid = -1;
    E.paneopen = 0;
    E.panefocus = 0;
    E.panedone = 0;
}

static void shquote(char *out, size_t n, const char *s)
{
    size_t o = 0;
    if (o + 1 < n) out[o++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') {
            if (o + 5 >= n) break;
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            if (o + 2 >= n) break;
            out[o++] = *s;
        }
    }
    if (o + 1 < n) out[o++] = '\'';
    out[o] = '\0';
}

static int file_shebang(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[256];
    char *p, *e;
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    if (line[0] != '#' || line[1] != '!') return 0;
    p = line + 2;
    while (*p == ' ' || *p == '\t') p++;
    e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    *e = '\0';
    while (e > p && e[-1] == ' ') *--e = '\0';
    if (!*p) return 0;
    snprintf(out, n, "%s", p);
    return 1;
}

static int build_run(char *cmd, size_t n, char *label, size_t ln)
{
    static const struct {
        const char *ext;
        const char *prog;
    } direct[] = {
        { ".py", "python3" }, { ".sh", "bash" },    { ".bash", "bash" },
        { ".js", "node" },    { ".mjs", "node" },   { ".cjs", "node" },
        { ".lua", "lua" },    { ".rb", "ruby" },    { ".pl", "perl" },
        { ".php", "php" },    { ".java", "java" },  { ".tcl", "tclsh" },
        { ".awk", "awk -f" }, { ".jl", "julia" },   { ".R", "Rscript" },
        { NULL, NULL }
    };
    const char *ext = strrchr(E.path, '.');
    const char *base = strrchr(E.path, '/');
    const char *tdir = getenv("TMPDIR");
    char q[1600], tq[600], tmp[256], interp[192];
    struct stat st;
    int i;
    base = base ? base + 1 : E.path;
    if (!tdir || !*tdir) tdir = "/tmp";
    shquote(q, sizeof q, E.path);
    snprintf(tmp, sizeof tmp, "%s/.xxi-run-%ld", tdir, (long)getpid());
    shquote(tq, sizeof tq, tmp);
    if (ext) {
        for (i = 0; direct[i].ext; i++) {
            if (strcmp(ext, direct[i].ext) == 0) {
                snprintf(cmd, n, "%s %s", direct[i].prog, q);
                snprintf(label, ln, "%.16s %.60s", direct[i].prog, base);
                return 1;
            }
        }
        if (strcmp(ext, ".c") == 0) {
            snprintf(cmd, n, "cc -O2 -Wall -o %s %s && %s", tq, q, tq);
            snprintf(label, ln, "cc + run %.60s", base);
            return 1;
        }
        if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
            strcmp(ext, ".cxx") == 0) {
            snprintf(cmd, n, "c++ -O2 -Wall -o %s %s && %s", tq, q, tq);
            snprintf(label, ln, "c++ + run %.60s", base);
            return 1;
        }
        if (strcmp(ext, ".go") == 0) {
            snprintf(cmd, n, "go run %s", q);
            snprintf(label, ln, "go run %.60s", base);
            return 1;
        }
        if (strcmp(ext, ".rs") == 0) {
            snprintf(cmd, n, "rustc -o %s %s && %s", tq, q, tq);
            snprintf(label, ln, "rustc + run %.60s", base);
            return 1;
        }
    }
    if (file_shebang(E.path, interp, sizeof interp)) {
        snprintf(cmd, n, "%s %s", interp, q);
        snprintf(label, ln, "%.60s %.60s", interp, base);
        return 1;
    }
    if (stat(E.path, &st) == 0 && (st.st_mode & S_IXUSR)) {
        snprintf(cmd, n, "%s", q);
        snprintf(label, ln, "./%.60s", base);
        return 1;
    }
    return 0;
}

static const char *shell_plain_flag(const char *sh)
{
    const char *base = strrchr(sh, '/');
    base = base ? base + 1 : sh;
    if (strcmp(base, "bash") == 0) return "--norc";
    if (strcmp(base, "zsh") == 0) return "-f";
    if (strcmp(base, "fish") == 0) return "--no-config";
    return NULL;
}

static int pane_spawn(const char *runcmd)
{
    const char *sh = getenv("SHELL");
    char *name;
    pid_t pid;
    int m, s;
    if (!sh || !*sh) sh = "/bin/sh";
    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) return -1;
    if (grantpt(m) == -1 || unlockpt(m) == -1) {
        close(m);
        return -1;
    }
    name = ptsname(m);
    if (!name) {
        close(m);
        return -1;
    }
    s = open(name, O_RDWR);
    if (s == -1) {
        close(m);
        return -1;
    }
    pid = fork();
    if (pid == -1) {
        close(m);
        close(s);
        return -1;
    }
    if (pid == 0) {
        struct termios t;
        sigset_t empty;
        sigemptyset(&empty);
        close(m);
        setsid();
        ioctl(s, TIOCSCTTY, 0);
        if (tcgetattr(s, &t) == 0) {
            t.c_lflag |= (tcflag_t)(ICANON | ECHO | ECHOE | ISIG);
            t.c_iflag |= (tcflag_t)(ICRNL | IXON);
            t.c_oflag |= (tcflag_t)(OPOST | ONLCR);
            tcsetattr(s, TCSANOW, &t);
        }
        dup2(s, STDIN_FILENO);
        dup2(s, STDOUT_FILENO);
        dup2(s, STDERR_FILENO);
        if (s > STDERR_FILENO) close(s);
        const char *flag = shell_plain_flag(sh);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGWINCH, SIG_DFL);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        if (!getenv("TERM")) setenv("TERM", "xterm-256color", 1);
        setenv("PS1", "xxi $ ", 1);
        setenv("XXI_PANE", "1", 1);
        unsetenv("LINES");
        unsetenv("COLUMNS");
        if (runcmd && *runcmd) {
            execl("/bin/sh", "sh", "-c", runcmd, (char *)NULL);
            _exit(127);
        }
        if (flag) execl(sh, sh, flag, "-i", (char *)NULL);
        execl(sh, sh, "-i", (char *)NULL);
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        _exit(127);
    }
    close(s);
    fcntl(m, F_SETFL, O_NONBLOCK);
    E.panefd = m;
    E.panepid = pid;
    E.paneopen = 1;
    E.panefocus = 1;
    E.panedone = 0;
    E.panestatus = 0;
    pane_clear();
    pane_newline();
    pane_resize();
    return 0;
}

static void pane_pump(void)
{
    char b[8192];
    if (E.panefd < 0) return;
    for (;;) {
        ssize_t r = read(E.panefd, b, sizeof b);
        if (r > 0) {
            pane_feed(b, (int)r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        pane_finish();
        return;
    }
}

static void pane_finish(void)
{
    int tries = 0;
    if (!E.paneopen || E.panedone) return;
    if (E.panefd >= 0) {
        close(E.panefd);
        E.panefd = -1;
    }
    if (E.panepid > 0) {
        for (; tries < 40; tries++) {
            pid_t r = waitpid(E.panepid, &E.panewait, WNOHANG);
            if (r == E.panepid) break;
            if (r == -1 && errno != EINTR) break;
            usleep(10000);
        }
        if (tries >= 40) {
            if (kill(-E.panepid, SIGKILL) == -1) kill(E.panepid, SIGKILL);
            xwaitpid(E.panepid, &E.panewait);
        }
        E.panepid = -1;
    }
    if (E.panecmd[0]) {
        E.panestatus = WIFEXITED(E.panewait) ? WEXITSTATUS(E.panewait) : -1;
        E.panedone = 1;
        E.panefocus = 0;
        if (E.panestatus == 0) set_msg("%s finished", E.panelabel);
        else if (E.panestatus > 0) set_msg("%s exited with code %d", E.panelabel, E.panestatus);
        else set_msg("%s was killed", E.panelabel);
    } else {
        E.paneopen = 0;
        E.panefocus = 0;
        E.panedone = 0;
        set_msg("terminal closed");
    }
}

static void pane_write(const char *s, int n)
{
    int off = 0;
    while (off < n) {
        ssize_t w = write(E.panefd, s + off, (size_t)(n - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += (int)w;
    }
}

static int hint_width(void)
{
    return E.cols >= HINTMIN ? HINTW : 0;
}

static int text_top(void)
{
    return E.findopen ? 1 : 0;
}

static int text_height(void)
{
    int h = E.rows - 1 - text_top() - (E.paneopen ? E.panerows + 1 : 0);
    return h < 1 ? 1 : h;
}

static int text_width(void)
{
    int hw = hint_width();
    int w = E.cols - (hw ? hw + 1 : 0);
    return w < 1 ? 1 : w;
}

static void fix_layout(void)
{
    int avail;
    if (!E.paneopen) return;
    avail = E.rows - 1 - text_top() - 1;
    E.panerows = PANEROWS;
    if (E.panerows > avail - 1) E.panerows = avail - 1;
    if (E.panerows < 1) E.panerows = 1;
}

static void update_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        E.rows = 24;
        E.cols = 80;
    } else {
        E.rows = ws.ws_row;
        E.cols = ws.ws_col;
    }
    if (E.rows < 4) E.rows = 4;
    if (E.cols < 24) E.cols = 24;
    fix_layout();
    pane_resize();
}

static const char *hints_edit[] = {
    "^S  save",
    "^W  quit",
    "",
    "^C  copy",
    "^V  paste",
    "^K  nuke line",
    "",
    "^R  undo",
    "^Y  redo",
    "",
    "^X  select",
    "^F  find",
    "^T  run file",
    NULL
};

static const char *hints_select[] = {
    "SELECT MODE",
    "",
    "drag   to pick",
    "arrows extend",
    "",
    "bksp   delete",
    "^K     delete",
    "^C     copy",
    "",
    "^X     leave",
    "esc    cancel",
    NULL
};

static const char *hints_find[] = {
    "FIND",
    "",
    "type   query",
    "enter  next",
    "^F     next",
    "bksp   edit",
    "",
    "esc    close",
    NULL
};

static const char *hints_pane[] = {
    "RUNNING",
    "",
    "keys go to",
    "the program",
    "",
    "drag   to copy",
    "wheel  scrolls",
    "",
    "^T   editor",
    "^C   interrupt",
    "esc  close",
    NULL
};

static const char *hints_done[] = {
    "OUTPUT",
    "",
    "drag   to copy",
    "^C     copy it",
    "wheel  scrolls",
    "",
    "^T   run again",
    "esc  close",
    "",
    "^S  save",
    "^W  quit",
    "^C  copy",
    "^V  paste",
    "^R  undo",
    "^Y  redo",
    "^X  select",
    "^F  find",
    NULL
};

static void hint_row(Buf *ab, int i, const char **h, int nh)
{
    int inner = HINTW - 2, k;
    if (i == 0) {
        bstr(ab, c_edge);
        bstr(ab, bx_tl);
        bstr(ab, bx_h);
        bstr(ab, c_head);
        bstr(ab, " XXI ");
        bstr(ab, c_edge);
        for (k = 6; k < inner; k++) bstr(ab, bx_h);
        bstr(ab, bx_tr);
        bstr(ab, c_off);
    } else if (i <= nh) {
        const char *s = h[i - 1];
        int L = (int)strlen(s);
        int split = 0;
        if (L > inner - 2) L = inner - 2;
        while (split < L && s[split] != ' ') split++;
        bstr(ab, c_edge);
        bstr(ab, bx_v);
        bput(ab, " ", 1);
        bstr(ab, i == 1 ? c_head : c_key);
        bput(ab, s, split);
        bstr(ab, c_word);
        bput(ab, s + split, L - split);
        bpad(ab, inner - 1 - L);
        bstr(ab, c_edge);
        bstr(ab, bx_v);
        bstr(ab, c_off);
    } else if (i == nh + 1) {
        bstr(ab, c_edge);
        bstr(ab, bx_bl);
        for (k = 0; k < inner; k++) bstr(ab, bx_h);
        bstr(ab, bx_br);
        bstr(ab, c_off);
    } else {
        bpad(ab, HINTW);
    }
}

static int rx_to_cx(Line *l, int rx)
{
    int i = 0, cur = 0;
    while (i < l->len && cur < rx) {
        if (l->s[i] == '\t') cur += TABSTOP - (cur % TABSTOP);
        else cur++;
        i = u8_fwd(l->s, l->len, i);
    }
    return i;
}

static void draw_row(Buf *ab, int fr, int tw, int sy, int sx, int ey, int ex)
{
    Line *l = &E.row[fr];
    int cols = cx_to_rx(l, l->len);
    int hlmax = cols + 1;
    unsigned char *hl = xmalloc((size_t)hlmax + 1);
    int i, col = 0, out = 0, on = 0;
    memset(hl, 0, (size_t)hlmax + 1);
    if (sy >= 0 && fr >= sy && fr <= ey) {
        int a = fr == sy ? cx_to_rx(l, sx) : 0;
        int b = fr == ey ? cx_to_rx(l, ex) : hlmax;
        for (i = a; i < b && i < hlmax; i++) hl[i] = 1;
    }
    if (E.findon && E.findlen > 0) {
        int p = 0;
        while ((p = find_in(l->s, l->len, E.find, E.findlen, p)) != -1) {
            int a = cx_to_rx(l, p);
            int b = cx_to_rx(l, p + E.findlen);
            for (i = a; i < b && i < hlmax; i++) hl[i] = 2;
            p += E.findlen;
        }
    }
    i = 0;
    while (out < tw) {
        int want;
        if (i < l->rlen) {
            int nx = u8_fwd(l->r, l->rlen, i);
            if (col >= E.coloff) {
                want = col < hlmax ? hl[col] : 0;
                if (want != on) {
                    bstr(ab, want == 2 ? c_hit : want == 1 ? c_sel : c_off);
                    on = want;
                }
                bput(ab, l->r + i, nx - i);
                out++;
            }
            col++;
            i = nx;
        } else {
            if (col >= hlmax || !hl[col]) break;
            if (col >= E.coloff) {
                if (on != hl[col]) {
                    bstr(ab, hl[col] == 2 ? c_hit : c_sel);
                    on = hl[col];
                }
                bput(ab, " ", 1);
                out++;
            }
            col++;
        }
    }
    if (on) bstr(ab, c_off);
    bpad(ab, tw - out);
    free(hl);
}

static void draw_findbar(Buf *ab)
{
    char left[256], right[64];
    int ln, rn, lw, n = find_count();
    snprintf(left, sizeof left, " find: %s", E.find);
    rn = snprintf(right, sizeof right, "%d match%s ", n, n == 1 ? "" : "es");
    if (rn > (int)sizeof right - 1) rn = (int)sizeof right - 1;
    if (rn > E.cols) rn = E.cols;
    ln = fit_tail(left, (int)sizeof left, left, E.cols - rn);
    lw = dispcols(left, ln);
    bstr(ab, c_bar);
    bstr(ab, "\x1b[K");
    bstr(ab, c_name);
    bput(ab, left, ln);
    bpad(ab, E.cols - lw - rn);
    bstr(ab, c_stat);
    bput(ab, right, rn);
    bstr(ab, c_off);
    bstr(ab, "\r\n");
}

static void draw_text(Buf *ab)
{
    const char **h;
    int y, nh = 0, th = text_height(), tw = text_width(), hw = hint_width();
    int sy = -1, sx = 0, ey = -1, ex = 0;
    if (E.panefocus) h = hints_pane;
    else if (E.paneopen && E.panedone) h = hints_done;
    else if (E.findopen) h = hints_find;
    else if (E.mode == MODE_SELECT) h = hints_select;
    else h = hints_edit;
    while (h[nh]) nh++;
    if (sel_on()) sel_span(&sy, &sx, &ey, &ex);
    for (y = 0; y < th; y++) {
        int fr = E.rowoff + y;
        bstr(ab, c_off);
        bstr(ab, "\x1b[K");
        if (fr >= E.nrow) {
            bpad(ab, tw);
        } else {
            draw_row(ab, fr, tw, sy, sx, ey, ex);
        }
        if (hw) {
            bput(ab, " ", 1);
            hint_row(ab, y, h, nh);
        }
        bstr(ab, "\r\n");
    }
}

static void draw_pane(Buf *ab)
{
    char head[256];
    int used, i, k, sy, sx, ey, ex;
    int first = pane_first();
    if (!E.panecmd[0])
        snprintf(head, sizeof head, " terminal  %s ",
                 E.panefocus ? "^T editor  esc close" : "^T focus  esc close");
    else if (!E.panedone)
        snprintf(head, sizeof head, " running %.100s  %s ", E.panelabel,
                 E.panefocus ? "^T editor" : "^T focus");
    else if (E.panestatus == 0)
        snprintf(head, sizeof head, " %.100s finished  ^T run again  esc close ", E.panelabel);
    else
        snprintf(head, sizeof head, " %.100s exited %d  ^T run again  esc close ",
                 E.panelabel, E.panestatus);
    bstr(ab, "\x1b[K");
    bstr(ab, c_edge);
    bstr(ab, bx_h);
    bstr(ab, bx_h);
    used = 2;
    bstr(ab, c_head);
    bstr(ab, head);
    used += dispcols(head, (int)strlen(head));
    bstr(ab, c_edge);
    for (k = used; k < E.cols; k++) bstr(ab, bx_h);
    bstr(ab, c_off);
    bstr(ab, "\r\n");
    if (first < 0) first = 0;
    psel_span(&sy, &sx, &ey, &ex);
    for (i = 0; i < E.panerows; i++) {
        int idx = first + i;
        bstr(ab, c_off);
        bstr(ab, "\x1b[K");
        if (idx >= 0 && idx < pn) {
            PLine *l = &pl[idx];
            int a = -1, b = -1, bi = 0, colc = 0, on = 0;
            if (E.pselon && idx >= sy && idx <= ey) {
                a = idx == sy ? sx : 0;
                b = idx == ey ? ex : l->len;
                if (a > l->len) a = l->len;
                if (b > l->len) b = l->len;
            }
            while (bi < l->len && colc < E.cols) {
                int nx = bi + 1;
                int want;
                while (nx < l->len && u8_cont((unsigned char)l->s[nx])) nx++;
                want = a >= 0 && bi >= a && bi < b;
                if (want != on) {
                    bstr(ab, want ? c_sel : c_off);
                    on = want;
                }
                bput(ab, l->s + bi, nx - bi);
                bi = nx;
                colc++;
            }
            if (a >= 0 && b > l->len - 1 && idx != ey && colc < E.cols) {
                bstr(ab, c_sel);
                bput(ab, " ", 1);
                on = 1;
            }
            if (on) bstr(ab, c_off);
        }
        bstr(ab, "\r\n");
    }
}

static void draw_status(Buf *ab)
{
    char left[512], right[192], tmp[640];
    const char *mode, *name, *slash, *flag;
    int ln, rn, lw, rw, span;
    int fresh = E.msg[0] != '\0';
    if (fresh && time(NULL) - E.msgt >= MSGSECS) {
        E.msg[0] = '\0';
        fresh = 0;
    }
    if (E.panefocus) mode = E.panecmd[0] ? "RUNNING" : "TERMINAL";
    else if (E.mode == MODE_SELECT) mode = "SELECT";
    else if (E.findopen) mode = "FIND";
    else mode = "EDIT";
    if (E.mode == MODE_SELECT) {
        int sy, sx, ey, ex;
        sel_span(&sy, &sx, &ey, &ex);
        span = ey - sy + 1;
        rn = snprintf(right, sizeof right, "%s %d line%s  %s  %d:%d  %s  %d line%s ",
                      mode, span, plural(span), bx_v, E.cy + 1, E.cx + 1,
                      bx_v, E.nrow, plural(E.nrow));
    } else {
        rn = snprintf(right, sizeof right, "%s  %s  %d:%d  %s  %d line%s ",
                      mode, bx_v, E.cy + 1, E.cx + 1, bx_v, E.nrow, plural(E.nrow));
    }
    if (rn >= (int)sizeof right) rn = (int)sizeof right - 1;
    rw = dispcols(right, rn);
    if (rw > E.cols / 2) {
        rn = snprintf(right, sizeof right, "%s %d:%d ", mode, E.cy + 1, E.cx + 1);
        rw = dispcols(right, rn);
    }
    if (rw > E.cols / 2) {
        rn = snprintf(right, sizeof right, "%d:%d ", E.cy + 1, E.cx + 1);
        rw = dispcols(right, rn);
    }
    if (rw > E.cols) {
        rn = 0;
        rw = 0;
    }
    name = E.path ? E.path : "[no name]";
    slash = strrchr(name, '/');
    if (E.readonly) flag = E.dirty ? "  * modified  [read only]" : "  [read only]";
    else flag = E.dirty ? "  * modified" : "  saved";
    if (fresh) {
        snprintf(tmp, sizeof tmp, " %s", E.msg);
    } else {
        snprintf(tmp, sizeof tmp, " XXI  %s%s", name, flag);
        if (dispcols(tmp, (int)strlen(tmp)) > E.cols - rw && slash && slash[1])
            snprintf(tmp, sizeof tmp, " XXI  %s%s", slash + 1, flag);
        if (dispcols(tmp, (int)strlen(tmp)) > E.cols - rw)
            snprintf(tmp, sizeof tmp, " %s%s%s", slash && slash[1] ? slash + 1 : name,
                     E.dirty ? " *" : "", E.readonly ? " ro" : "");
    }
    ln = fit_tail(left, (int)sizeof left, tmp, E.cols - rw - 2);
    lw = dispcols(left, ln);
    bstr(ab, c_bar);
    bstr(ab, "\x1b[K");
    bstr(ab, c_name);
    bput(ab, left, ln);
    bpad(ab, E.cols - lw - rw);
    bstr(ab, c_stat);
    if (rn > 0) bput(ab, right, rn);
    bstr(ab, c_off);
}

static void scroll_to_cursor(void)
{
    int th = text_height(), tw = text_width();
    E.rx = cx_to_rx(&E.row[E.cy], E.cx);
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + th) E.rowoff = E.cy - th + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + tw) E.coloff = E.rx - tw + 1;
    if (E.rowoff < 0) E.rowoff = 0;
    if (E.coloff < 0) E.coloff = 0;
}

static void place_cursor(Buf *ab)
{
    int row, col;
    if (E.paneopen && E.panefocus) {
        int first = pn - E.panerows;
        if (first < 0) first = 0;
        row = text_top() + text_height() + 2 + (pcur - first);
        col = pcx + 1;
    } else if (E.findopen) {
        row = 1;
        col = 8 + E.findlen;
    } else {
        row = text_top() + (E.cy - E.rowoff) + 1;
        col = (E.rx - E.coloff) + 1;
    }
    if (row < 1) row = 1;
    if (row > E.rows) row = E.rows;
    if (col < 1) col = 1;
    if (col > E.cols) col = E.cols;
    bfmt(ab, "\x1b[%d;%dH", row, col);
}

static void draw(void)
{
    Buf ab;
    memset(&ab, 0, sizeof ab);
    fix_layout();
    clamp_cursor();
    scroll_to_cursor();
    bstr(&ab, "\x1b[?25l\x1b[H");
    if (E.findopen) draw_findbar(&ab);
    draw_text(&ab);
    if (E.paneopen) draw_pane(&ab);
    draw_status(&ab);
    place_cursor(&ab);
    bstr(&ab, "\x1b[?25h");
    emitn(ab.b, ab.len);
    free(ab.b);
}

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
    E.msgt = time(NULL);
}

static int stdin_ready(int ms)
{
    struct pollfd p;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

static int read_key(void)
{
    char seq[4];
    char c;
    ssize_t n;
    for (;;) {
        n = read(STDIN_FILENO, &c, 1);
        if (n == 1) break;
        if (n == -1 && errno == EINTR) return -1;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        if (n == -1) die("read");
        if (n == 0) return -1;
    }
    if (c != '\x1b') return (unsigned char)c;
    if (!stdin_ready(40) || read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (!stdin_ready(40) || read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[' && seq[1] == '<') {
        char m[40];
        int k = 0;
        while (k < (int)sizeof m - 1) {
            if (!stdin_ready(60) || read(STDIN_FILENO, m + k, 1) != 1) return '\x1b';
            if (m[k] == 'M' || m[k] == 'm') break;
            k++;
        }
        ms_press = m[k] == 'M';
        m[k] = '\0';
        if (sscanf(m, "%d;%d;%d", &ms_b, &ms_x, &ms_y) != 3) return '\x1b';
        return K_MOUSE;
    }
    if (seq[0] == '[') {
        char p[32];
        int k = 0, fin;
        p[k++] = seq[1];
        while (!(p[k - 1] >= 0x40 && p[k - 1] <= 0x7e) && k < (int)sizeof p - 1) {
            if (!stdin_ready(40) || read(STDIN_FILENO, p + k, 1) != 1) return '\x1b';
            k++;
        }
        p[k] = '\0';
        fin = p[k - 1];
        if (fin == '~') {
            switch (p[0]) {
            case '1': return K_HOME;
            case '3': return K_DEL;
            case '4': return K_END;
            case '5': return K_PGUP;
            case '6': return K_PGDN;
            case '7': return K_HOME;
            case '8': return K_END;
            default: return -1;
            }
        }
        switch (fin) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        default: return -1;
        }
    }
    if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H': return K_HOME;
        case 'F': return K_END;
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        default: return '\x1b';
        }
    }
    return '\x1b';
}

static void move_cursor(int key)
{
    switch (key) {
    case K_LEFT:
        if (E.cx > 0) E.cx = u8_back(E.row[E.cy].s, E.cx);
        else if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].len;
        }
        break;
    case K_RIGHT:
        if (E.cx < E.row[E.cy].len) E.cx = u8_fwd(E.row[E.cy].s, E.row[E.cy].len, E.cx);
        else if (E.cy < E.nrow - 1) {
            E.cy++;
            E.cx = 0;
        }
        break;
    case K_UP:
        if (E.cy > 0) E.cy--;
        break;
    case K_DOWN:
        if (E.cy < E.nrow - 1) E.cy++;
        break;
    case K_HOME:
        E.cx = 0;
        break;
    case K_END:
        E.cx = E.row[E.cy].len;
        break;
    case K_PGUP:
        E.cy -= text_height();
        if (E.cy < 0) E.cy = 0;
        break;
    case K_PGDN:
        E.cy += text_height();
        if (E.cy > E.nrow - 1) E.cy = E.nrow - 1;
        break;
    default:
        break;
    }
    if (E.cx > E.row[E.cy].len) E.cx = E.row[E.cy].len;
}

static void mouse_to_buf(int mx, int my, int *py, int *px)
{
    int row = my - 1 - text_top();
    int th = text_height();
    int tw = text_width();
    int rx;
    if (row < 0) row = 0;
    if (row >= th) row = th - 1;
    *py = E.rowoff + row;
    if (*py >= E.nrow) *py = E.nrow - 1;
    if (*py < 0) *py = 0;
    rx = mx - 1 >= tw ? E.coloff + tw - 1 : E.coloff + mx - 1;
    if (rx < 0) rx = 0;
    *px = rx_to_cx(&E.row[*py], rx);
}

static void mouse_to_pane(int mx, int my, int *ly, int *lx)
{
    int row = my - (text_top() + text_height() + 1) - 1;
    int first = pane_first();
    if (row < 0) row = 0;
    if (row >= E.panerows) row = E.panerows - 1;
    *ly = first + row;
    if (*ly >= pn) *ly = pn > 0 ? pn - 1 : 0;
    if (*ly < 0) *ly = 0;
    *lx = pn > 0 ? pane_col_to_byte(&pl[*ly], mx > 0 ? mx - 1 : 0) : 0;
}

static void handle_mouse(void)
{
    int y, x;
    int btn = ms_b & 0x03;
    int motion = (ms_b & 32) != 0;
    int wheel = (ms_b & 64) != 0;
    int ty0 = text_top();
    int th = text_height();
    int head = ty0 + th + 1;
    int inpane = E.paneopen && ms_y >= head && ms_y <= head + E.panerows;
    if (wheel) {
        if (inpane) {
            E.paneoff += (ms_b & 1) ? -3 : 3;
            pane_clamp_off();
            return;
        }
        if (E.panefocus) return;
        E.rowoff += (ms_b & 1) ? 3 : -3;
        if (E.rowoff > E.nrow - 1) E.rowoff = E.nrow - 1;
        if (E.rowoff < 0) E.rowoff = 0;
        if (E.cy < E.rowoff) E.cy = E.rowoff;
        if (E.cy >= E.rowoff + th) E.cy = E.rowoff + th - 1;
        clamp_cursor();
        return;
    }
    if (btn != 0) return;
    if (!ms_press) {
        E.dragging = 0;
        E.dragedge = 0;
        E.pdrag = 0;
        E.pedge = 0;
        return;
    }
    if (inpane) {
        mouse_to_pane(ms_x, ms_y, &y, &x);
        if (!motion) {
            E.psay = y;
            E.psax = x;
            E.psby = y;
            E.psbx = x;
            E.pselon = 0;
            E.pdrag = 1;
            E.pedge = 0;
        } else if (E.pdrag) {
            E.psby = y;
            E.psbx = x;
            E.pselon = 1;
            E.panefocus = 0;
            if (ms_y <= head + 1) E.pedge = -1;
            else if (ms_y >= head + E.panerows) E.pedge = 1;
            else E.pedge = 0;
        }
        return;
    }
    E.pselon = 0;
    E.panefocus = 0;
    if (ms_x > text_width()) return;
    mouse_to_buf(ms_x, ms_y, &y, &x);
    if (!motion) {
        E.cy = y;
        E.cx = x;
        E.any = y;
        E.anx = x;
        E.marked = 0;
        E.dragging = 1;
        E.dragedge = 0;
    } else if (E.dragging) {
        E.cy = y;
        E.cx = x;
        if (E.mode != MODE_SELECT) E.marked = 1;
        if (ms_y <= ty0) E.dragedge = -1;
        else if (ms_y > ty0 + th) E.dragedge = 1;
        else E.dragedge = 0;
    }
}

static void pane_drag_scroll(void)
{
    int first, ly;
    if (!E.pdrag || !E.pedge || pn == 0) return;
    E.paneoff += E.pedge > 0 ? -1 : 1;
    pane_clamp_off();
    first = pane_first();
    ly = E.pedge > 0 ? first + E.panerows - 1 : first;
    if (ly >= pn) ly = pn - 1;
    if (ly < 0) ly = 0;
    E.psby = ly;
    E.psbx = E.pedge > 0 ? pl[ly].len : 0;
    E.pselon = 1;
}

static void drag_scroll(void)
{
    if (!E.dragging || !E.dragedge) return;
    if (E.mode != MODE_SELECT) E.marked = 1;
    E.cy += E.dragedge;
    if (E.cy < 0) E.cy = 0;
    if (E.cy > E.nrow - 1) E.cy = E.nrow - 1;
    E.cx = E.dragedge < 0 ? 0 : E.row[E.cy].len;
    clamp_cursor();
}

static void quit_now(void)
{
    pane_close();
    emit("\x1b[2J\x1b[H");
    disable_raw();
    exit(0);
}

static void do_run(void)
{
    if (E.panecmd[0] && E.dirty) {
        cmd_save();
        if (E.dirty) {
            set_msg("%s is read only - not saved, not run",
                    E.path ? E.path : "file");
            return;
        }
    }
    if (E.paneopen) pane_close();
    if (pane_spawn(E.panecmd[0] ? E.panecmd : NULL) == -1) {
        set_msg("cannot start %s", E.panelabel);
        return;
    }
    fix_layout();
    pane_resize();
    if (E.panecmd[0]) set_msg("running  %s", E.panelabel);
    else set_msg("shell open  - ^T returns to the editor, ^D closes");
}

static void find_key(int c)
{
    int changed = 0;
    switch (c) {
    case '\x1b':
        E.findopen = 0;
        set_msg("find closed  - esc again clears highlights");
        return;
    case KCTRL('w'):
    case KCTRL('x'):
        E.findopen = 0;
        return;
    case '\r':
    case '\n':
    case KCTRL('f'):
        if (!find_at(E.cy, E.cx + 1)) set_msg("no match for \"%s\"", E.find);
        return;
    case K_BACKSPACE:
    case KCTRL('h'):
        if (E.findlen > 0) {
            E.findlen = u8_back(E.find, E.findlen);
            E.find[E.findlen] = '\0';
            changed = 1;
        }
        break;
    case K_UP:
    case K_DOWN:
    case K_LEFT:
    case K_RIGHT:
    case K_HOME:
    case K_END:
    case K_PGUP:
    case K_PGDN:
        move_cursor(c);
        E.findsy = E.cy;
        E.findsx = E.cx;
        return;
    default:
        if (c >= 32 && c != K_BACKSPACE && c < 1000) {
            if (E.findlen < (int)sizeof E.find - 1) {
                E.find[E.findlen++] = (char)c;
                E.find[E.findlen] = '\0';
                changed = 1;
            }
        }
        break;
    }
    if (changed && E.findlen > 0) find_at(E.findsy, E.findsx);
}

static void process_key(void)
{
    int c = read_key();
    if (c == -1) return;
    if (E.confirmquit) {
        E.confirmquit = 0;
        if (c == '\r' || c == '\n') quit_now();
        set_msg("not quitting");
        return;
    }
    if (E.runconfirm) {
        E.runconfirm = 0;
        if (c == '\r' || c == '\n') do_run();
        else set_msg("not running");
        return;
    }
    if (c == K_MOUSE) {
        handle_mouse();
        return;
    }
    if (c != KCTRL('c')) {
        E.marked = 0;
        E.pselon = 0;
    }
    if (E.findopen) {
        find_key(c);
        return;
    }
    switch (c) {
    case KCTRL('s'):
        cmd_save();
        break;
    case KCTRL('w'):
        E.confirmquit = 1;
        if (E.dirty) set_msg("UNSAVED CHANGES - quit anyway?  enter = yes, any other key = no");
        else set_msg("quit XXI?  enter = yes, any other key = no");
        break;
    case KCTRL('c'):
        if (E.pselon) cmd_copy_pane();
        else cmd_copy();
        break;
    case KCTRL('v'):
        cmd_paste();
        break;
    case KCTRL('k'):
        cmd_nuke();
        break;
    case KCTRL('r'):
        cmd_undo();
        break;
    case KCTRL('y'):
        cmd_redo();
        break;
    case KCTRL('x'):
        if (E.mode == MODE_SELECT) {
            E.mode = MODE_EDIT;
            set_msg("selection off");
        } else {
            E.mode = MODE_SELECT;
            E.any = E.cy;
            E.anx = E.cx;
            E.dragging = 0;
            E.dragedge = 0;
            set_msg("selection on  - drag or arrows to pick, backspace deletes");
        }
        break;
    case KCTRL('f'):
        E.findopen = 1;
        E.findon = 1;
        E.mode = MODE_EDIT;
        E.findsy = E.cy;
        E.findsx = E.cx;
        break;
    case KCTRL('t'):
        if (E.paneopen && !E.panedone && E.panefd >= 0) {
            E.panefocus = 1;
            set_msg("terminal focused  - ^T returns to the editor");
            break;
        }
        if (build_run(E.panecmd, sizeof E.panecmd, E.panelabel, sizeof E.panelabel)) {
            set_msg("run  %s ?   enter = yes, any other key = no", E.panelabel);
        } else {
            E.panecmd[0] = '\0';
            snprintf(E.panelabel, sizeof E.panelabel, "shell");
            set_msg("no runner for this file - open a shell?   enter = yes, any other key = no");
        }
        E.runconfirm = 1;
        break;
    case KCTRL('d'):
        if (E.paneopen) {
            pane_close();
            set_msg("terminal closed");
        }
        break;
    case '\r':
    case '\n':
        if (E.mode == MODE_SELECT) {
            E.mode = MODE_EDIT;
            break;
        }
        cmd_newline();
        break;
    case K_BACKSPACE:
    case KCTRL('h'):
        if (E.mode == MODE_SELECT) cmd_nuke();
        else cmd_backspace();
        break;
    case K_DEL:
        if (E.mode == MODE_SELECT) cmd_nuke();
        else cmd_delete();
        break;
    case K_UP:
    case K_DOWN:
    case K_LEFT:
    case K_RIGHT:
    case K_HOME:
    case K_END:
    case K_PGUP:
    case K_PGDN:
        move_cursor(c);
        break;
    case '\x1b':
        if (E.paneopen) {
            pane_close();
            set_msg("terminal closed");
            break;
        }
        if (E.mode == MODE_SELECT) {
            E.mode = MODE_EDIT;
            E.dragging = 0;
            E.dragedge = 0;
            set_msg("selection off");
        } else if (E.findon) {
            E.findon = 0;
            set_msg("highlights cleared");
        }
        E.marked = 0;
        break;
    case KCTRL('l'):
        break;
    default:
        if (E.mode == MODE_SELECT) break;
        if (c == '\t' || (c >= 32 && c < 1000)) cmd_insert_char(c);
        break;
    }
}

static void pane_keys(void)
{
    char b[512];
    ssize_t r = read(STDIN_FILENO, b, sizeof b);
    int i = 0, start = 0;
    if (r <= 0) return;
    while (i < (int)r) {
        if (b[i] == KCTRL('t')) {
            if (i > start) pane_write(b + start, i - start);
            E.panefocus = 0;
            set_msg("editor focused  - ^T returns to the terminal");
            return;
        }
        if (b[i] == '\x1b' && i + 2 < (int)r && b[i + 1] == '[' && b[i + 2] == '<') {
            int j = i + 3;
            while (j < (int)r && b[j] != 'M' && b[j] != 'm') j++;
            if (j < (int)r) {
                char save = b[j];
                if (i > start) pane_write(b + start, i - start);
                b[j] = '\0';
                if (sscanf(b + i + 3, "%d;%d;%d", &ms_b, &ms_x, &ms_y) == 3) {
                    ms_press = save == 'M';
                    handle_mouse();
                }
                i = j + 1;
                start = i;
                if (!E.panefocus) return;
                continue;
            }
        }
        if (b[i] == '\x1b' && i == (int)r - 1 && !stdin_ready(50)) {
            if (i > start) pane_write(b + start, i - start);
            pane_close();
            set_msg("terminal closed");
            return;
        }
        i++;
    }
    pane_write(b + start, (int)r - start);
}

static void on_winch(int sig)
{
    (void)sig;
    winch_pending = 1;
}

static void on_chld(int sig)
{
    (void)sig;
    chld_pending = 1;
}

static void pane_reap(void)
{
    if (!E.paneopen || E.panepid <= 0) return;
    if (waitpid(E.panepid, &E.panewait, WNOHANG) != E.panepid) return;
    E.panepid = -1;
    pane_pump();
    pane_finish();
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    struct pollfd pf[2];
    if (argc != 2) {
        fprintf(stderr, "usage: xxi <file>\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_chld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);
    E.panefd = -1;
    E.panepid = -1;
    E.panerows = PANEROWS;
    E.mode = MODE_EDIT;
    enable_raw();
    atexit(disable_raw);
    detect_colors();
    detect_borders();
    update_size();
    open_file(argv[1]);
    E.savepoint = 0;
    mark_dirty();
    for (;;) {
        int n = 1, r;
        if (winch_pending) {
            winch_pending = 0;
            update_size();
        }
        if (chld_pending) {
            chld_pending = 0;
            pane_reap();
        }
        draw();
        pf[0].fd = STDIN_FILENO;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        if (E.paneopen && E.panefd >= 0) {
            pf[1].fd = E.panefd;
            pf[1].events = POLLIN;
            pf[1].revents = 0;
            n = 2;
        }
        r = poll(pf, (nfds_t)n,
                 ((E.dragging && E.dragedge) || (E.pdrag && E.pedge))
                     ? 60
                     : (E.msg[0] ? 400 : -1));
        if (r < 0) {
            if (errno == EINTR) continue;
            die("poll");
        }
        if (n == 2 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) pane_pump();
        if (pf[0].revents & POLLIN) {
            if (E.paneopen && E.panefocus) pane_keys();
            else process_key();
        } else if (r == 0) {
            drag_scroll();
            pane_drag_scroll();
        }
    }
    return 0;
}
