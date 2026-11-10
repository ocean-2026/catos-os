/*
 * cc.ce -- CatOS tiny C compiler (target one, task #19).
 *     run /bin/cc.ce /SRC/HELLO.C /BIN/OUT.CE
 * Compiles a C subset straight to a CatOS Executable (.ce): a flat i386
 * image with the CE01 header, loaded at 0x02000000, system-call table
 * reachable through the __ce_api slot.
 * Supported subset
 *   types   int / char / void, pointers, 1-D arrays
 *   decls   globals with constant init, locals, string literals
 *   funcs   parameters, recursion, forward calls, return
 *   stmts   blocks, if/else, while, for, break, continue, return
 *   exprs   = += -= *= /= , ?:, || && | ^ & == != < > <= >= << >>
 *           + - * / % , unary - ! ~ * & , calls, indexing
 *   api     `api->write("x");` lowers to a call through the table
 * Codegen: stack machine over EAX/ECX.  Every expression leaves its
 * value in EAX; binary operators push the left side, evaluate the right,
 * pop the left into ECX.  Calls are cdecl like the MinGW-built apps.
 * Two passes: pass 1 records function address slots (all instructions are
 * emitted in one buffer; call sites are patched at the end), pass 2 emits.
 * Build: sh extapp/build_ce.sh cc.c   -> extapp/bin/cc.ce
 *
 * This file (cc_self.c) is the SELF-HOSTING variant of cc.c: with CC_HOST
 * defined MinGW builds it like cc.c (tests/host_shim.c); without it the
 * source only uses the subset cc itself compiles, so a stage-1 cc can
 * compile it again (the bootstrap ring).  Working buffers are heap-backed
 * through cc_alloc with small static fallbacks -- the budgets in the enum
 * below are sized for compiling cc_self.c, not small user programs.
 */

#ifdef CC_HOST
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;
#else
/* subset type shims: no stdint, no API struct, no attributes */
typedef unsigned int  uint32_t;
typedef unsigned char uint8_t;
typedef void catos_api_t;
catos_api_t *__ce_api = 0;
#endif
/* constants formerly #define-d */
enum {
    OB_MAX = 131072, CE_LOAD_BASE = 33554432, SRC_MAX = 98304,
    TOK_MAX = 25000,
    /* TOK_HEAP: how many tokens are actually requested from the heap.
     *
     * cc_self.c draws EIGHT working sets from the per-task heap (see
     * cc_alloc() below).  With the P0-3 heap of 512 KiB the usable
     * arena payload is USER_HEAP_MAX - sizeof(heap_hdr_t) = 524,280 B.
     * The eight requests must all fit in that budget:
     *
     *   OB_MAX                        131,072
     *   SRC_MAX                        98,304
     *   TOK_HEAP * sizeof(token_t)    115,200   (4,800 * 24)
     *   DATA_MAX                       65,536
     *   SYM_MAX * sizeof(sym_t)        65,536
     *   STRUCT_MAX * sizeof(stdef_t)   28,160
     *   2 * REL_MAX * sizeof(int)       8,192
     *   2 * CALLMAX * sizeof(int)       8,192
     *                                  ---------
     *                                   520,192   <= 524,280  (slack 4,088)
     *
     * The exact maximum is 4,968 tokens; 4,800 keeps a >4 KB margin.
     * Requesting the full TOK_MAX (25,000 * 24 = 600,000 B) would NOT
     * fit at all, so the token array silently fell back to the
     * 512-token static buffer -- and because cc_alloc() did not set
     * *cap on the failure path, g_tok_cap became 0 and the lexer
     * rejected EVERY source with "source too complex".
     *
     * Consequence (documented, not a regression): the self-hosting
     * bootstrap (compiling cc_self.c with itself, ~24.2K tokens) is
     * NOT reachable under the 512 KiB heap.  Restoring it would need
     * USER_HEAP_MAX = 1 MiB -- a user-level decision, out of scope
     * here.  TOK_MAX stays 25,000 as the compile-time ceiling.
     */
    TOK_HEAP = 4800,
    SYM_MAX = 1024, LOC_MAX = 128, REL_MAX = 1024,
    CALLMAX = 1024, APIREF_MAX = 256, DREL_MAX = 64, STRUCT_MAX = 16,
    SMEM_MAX = 48, TAG_MAX = 24, TALIAS_MAX = 24, CONST_MAX = 96,
    BRK_MAX = 64, LBL_MAX = 48, IMG_MAX = 262144,
    DATA_MAX = 65536
};

/* ---- runtime-initialised tables (brace init cannot take strings) ---- */
static char *g_punct[46];
static char *g_apin[65];   /* 64 slots + NULL sentinel */

static void init_tables(void) {
    g_punct[0] = "<<=";  g_punct[1] = ">>=";  g_punct[2] = "==";
    g_punct[3] = "!=";   g_punct[4] = "<=";   g_punct[5] = ">=";
    g_punct[6] = "&&";   g_punct[7] = "||";   g_punct[8] = "++";
    g_punct[9] = "--";   g_punct[10] = "+=";  g_punct[11] = "-=";
    g_punct[12] = "*=";  g_punct[13] = "/=";  g_punct[14] = "%=";
    g_punct[15] = "&=";  g_punct[16] = "|=";  g_punct[17] = "^=";
    g_punct[18] = "->";  g_punct[19] = "<<";  g_punct[20] = ">>";
    g_punct[21] = "(";   g_punct[22] = ")";   g_punct[23] = "{";
    g_punct[24] = "}";   g_punct[25] = "[";   g_punct[26] = "]";
    g_punct[27] = ";";   g_punct[28] = ",";   g_punct[29] = ".";
    g_punct[30] = "?";   g_punct[31] = ":";   g_punct[32] = "=";
    g_punct[33] = "+";   g_punct[34] = "-";   g_punct[35] = "*";
    g_punct[36] = "/";   g_punct[37] = "%";   g_punct[38] = "<";
    g_punct[39] = ">";   g_punct[40] = "!";   g_punct[41] = "~";
    g_punct[42] = "&";   g_punct[43] = "|";   g_punct[44] = "^";
    g_punct[45] = 0;

    g_apin[0] = "write";       g_apin[1] = "putc";      g_apin[2] = "cls";
    g_apin[3] = "readline";    g_apin[4] = "time_str";  g_apin[5] = "fs_ready";
    g_apin[6] = "ls";          g_apin[7] = "touch";     g_apin[8] = "mkdir";
    g_apin[9] = "cat";         g_apin[10] = "save";     g_apin[11] = "alloc";
    g_apin[12] = "free";       g_apin[13] = "exit";     g_apin[14] = "print_int";
    g_apin[15] = "print_hex";  g_apin[16] = "set_fg";   g_apin[17] = "delay";
    g_apin[18] = "atoi";       g_apin[19] = "strlen";   g_apin[20] = "strcmp";
    g_apin[21] = "strcpy";     g_apin[22] = "getch";    g_apin[23] = "file_exists";
    g_apin[24] = "fsize";      g_apin[25] = "net_init"; g_apin[26] = "net_up";
    g_apin[27] = "http_get";   g_apin[28] = "net_dns";  g_apin[29] = "net_udp_send";
    g_apin[30] = "net_udp_recv"; g_apin[31] = "net_ping"; g_apin[32] = "net_ip";
    g_apin[33] = "getuid";     g_apin[34] = "gettid";   g_apin[35] = "kill";
    g_apin[36] = "yield";      g_apin[37] = "hw_num";   g_apin[38] = "hw_str";
    g_apin[39] = "file_read";  g_apin[40] = "file_write";
    g_apin[41] = "file_delete"; g_apin[42] = "pipe_create";
    g_apin[43] = "pipe_write"; g_apin[44] = "pipe_read";
    g_apin[45] = "pipe_close"; g_apin[46] = "spawn";    g_apin[47] = "wait";
    /* v2.7..v2.10 (slots 49..56; slot 48 = brk stays unnamed here) */
    g_apin[48] = "brk";
    g_apin[49] = "display_mode"; g_apin[50] = "display_info";
    g_apin[51] = "set_style";  g_apin[52] = "set_style_rgb";
    g_apin[53] = "set_palette"; g_apin[54] = "palette";
    g_apin[55] = "mmap";       g_apin[56] = "munmap";
    /* v2.11 SB16 + v2.12 24bpp drawing (slots 57..63). */
    g_apin[57] = "snd_info";   g_apin[58] = "snd_play";
    g_apin[59] = "snd_stop";   g_apin[60] = "snd_tone";
    g_apin[61] = "gfx_info";   g_apin[62] = "gfx_rect";
    g_apin[63] = "gfx_blit";
}

/* zero n bytes through a char* (replaces ((unsigned char*)p)[i] = 0) */
static void memzero(char *p, int n) {
    int i;
    for (i = 0; i < n; i++) p[i] = 0;
}

/* ====================================================================
 * helpers
 * ================================================================== */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (char)*a - (char)*b;
}
static void sncpy(char *d, const char *s, int n) {
    /* indexed form: the generic *p = v path has no type info and stores
     * dwords, which corrupts char buffers */
    int i = 0;
    while (i < n && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_upper(int c) { return c >= 'A' && c <= 'Z'; }

static int  g_err;
static char g_msg[64];
static void err_set(const char *m) {
    if (g_err) return;
    g_err = 1;
    sncpy(g_msg, m, (int)sizeof(g_msg) - 1);
}

/* ====================================================================
 * memory budget
 *
 * A CE program's address space puts the image and its bss inside ONE
 * 512 KiB window (USER_IMAGE_BASE .. USER_VSYSCALL), so every static byte
 * spent on buffers steals from the code budget.  The compiler's working
 * sets (source text, token stream, output image) are therefore taken
 * from the per-task heap via api->alloc(), which lives in a separate
 * 256 KiB window (USER_HEAP_BASE).  If the heap cannot serve a request we
 * fall back to the small static buffers so the compiler still runs on a
 * minimal kernel.
 *
 * The eight heap requests are reconciled with the 512 KiB budget by the
 * TOK_HEAP constant in the enum above; see its comment for the table.
 * ================================================================== */
/* Heap working-set sizes.  USER_HEAP_MAX is 512 KiB total (P0-3),
 * shared with anything else the compiler allocates:
 *     code 128K + source 96K + tokens 112K + data 64K + ... ~= 508K. */
/* kernel loads the CE image at this base (include/ce.h CE_LOAD_BASE);
 * cc.c references it for absolute fixups so define it locally to stay
 * compilable against minimal headers. */
static uint8_t  g_ob_static[8192];
static uint32_t g_ob_cap = 8192;
static uint8_t *g_ob;             /* set to the fallback in ce_main */

/* Draw a working set from the heap, falling back to a static buffer.
 *
 * `fallback_cap` is the REAL capacity of `fallback` in BYTES.  It is
 * written to *cap when the heap request fails.
 *
 * Why that matters: the callers derive an element count from *cap
 * (g_tok_cap = cap / sizeof(token_t), and so on).  The old version
 * left *cap untouched on the failure path, so it stayed 0 and the
 * derived element count became 0 -- and the lexer then rejected EVERY
 * source with "source too complex" (0 >= 0 - 1).  A graceful fallback
 * must report the capacity it actually has.
 */
static void *cc_alloc(catos_api_t *api, uint32_t n, void *fallback,
                      uint32_t fallback_cap, uint32_t *cap) {
    void *p = api->alloc(n);
    if (p) { *cap = n; return p; }
    *cap = fallback_cap;
    return fallback;
}

/* ---- code output buffer (heap-backed, see cc_alloc) ---- */
static uint32_t g_olen;

static uint8_t *ob_buf(void) { return g_ob; }

static void ob8(uint32_t v) { if (g_olen < g_ob_cap) g_ob[g_olen++] = (uint8_t)v; }
static void ob32(uint32_t v) { ob8(v); ob8(v >> 8); ob8(v >> 16); ob8(v >> 24); }
static uint32_t ob_at(void) { return g_olen; }
static void ob_patch(uint32_t at, uint32_t v) {
    if (at + 4 > g_olen) return;
    g_ob[at] = (uint8_t)v; g_ob[at+1] = (uint8_t)(v>>8);
    g_ob[at+2] = (uint8_t)(v>>16); g_ob[at+3] = (uint8_t)(v>>24);
}

/* ====================================================================
 * tokens
 * ================================================================== */
/* source text: heap preferred, small static fallback */
#define SRC_MAX (96 * 1024)      /* cc_self.c is 85 KB */
static char   g_src_static[8192];
static char  *g_src;              /* set to the fallback in ce_main */
static uint32_t g_src_cap = 8192;
static uint32_t g_slen;

enum { TK_EOF = 0, TK_ID, TK_NUM, TK_STR, TK_CHR, TK_PUNCT };

typedef struct { int kind, num, op, len, line; const char *sp; } token_t;
/* TOK_MAX (compile-time ceiling) and TOK_HEAP (heap-backed count) live in
 * the enum at the top of this file -- they used to be two separate
 * #defines here, which shadowed each other.  The heap-backed count is
 * TOK_HEAP; see the enum for the budget table. */
static token_t  g_tok_static[512];
static token_t *g_tok;            /* set to the fallback in ce_main */
static int      g_tok_cap = 512;
static int g_ntok, g_ti;

/* g_punct moved to the runtime-initialised g_punct[] above */
/* token op == index into g_punct; stable names for code */
enum {
    OP_SHLASG = 0, OP_SHRASG, OP_EQ, OP_NE, OP_LE, OP_GE, OP_ANDAND, OP_OROR,
    OP_INC, OP_DEC, OP_PLUSASG, OP_MINUSASG, OP_MULASG, OP_DIVASG, OP_MODASG,
    OP_ANDASG, OP_ORASG, OP_XORASG, OP_ARROW, OP_SHL, OP_SHR,
    OP_LP, OP_RP, OP_LB, OP_RB, OP_LSB, OP_RSB, OP_SEMI, OP_COMMA, OP_DOT,
    OP_QMARK, OP_COLON, OP_ASG, OP_PLUS, OP_MINUS, OP_STAR, OP_SLASH,
    OP_PERCENT, OP_LT, OP_GT, OP_NOT, OP_TILDE, OP_AMP, OP_PIPE, OP_CARET
};

static int tok_op(void) { return g_tok[g_ti].op; }
static int at_op(int op) { return g_tok[g_ti].kind == TK_PUNCT && g_tok[g_ti].op == op; }
static int accept_op(int op) { if (at_op(op)) { g_ti++; return 1; } return 0; }
static token_t *cur(void) { return &g_tok[g_ti]; }
static token_t *peek(int n) { int i = g_ti + n; if (i > g_ntok) i = g_ntok; return &g_tok[i]; }
static int at_kw(const char *kw) {
    token_t *t = cur();
    int n = slen(kw);
    if (t->kind != TK_ID || t->len != n) return 0;
    for (int i = 0; i < n; i++) if (t->sp[i] != kw[i]) return 0;
    return 1;
}
static int accept_kw(const char *kw) { if (at_kw(kw)) { g_ti++; return 1; } return 0; }

static int g_cond_depth, g_cond_skip;   /* #ifdef state for the lexer */

static int tokenize(void) {
    uint32_t i = 0;
    int line = 1;
    g_ntok = 0;
    while (i < g_slen) {
        int c = g_src[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == '#') {
            /* minimal conditional compilation (stage 4): #ifdef NAME /
             * #else / #endif select which lines reach the parser.  The
             * self-hosted build never defines CC_HOST, so host-only
             * blocks disappear.  Every '#' line is skipped as text. */
            i++;
            while (i < g_slen && (g_src[i] == ' ' || g_src[i] == '\t')) i++;
            int dw = i;
            while (dw < g_slen && is_alnum(g_src[dw])) dw++;
            int dl = (int)(dw - i);
            if (dl == 5 && g_src[i]=='i' && g_src[i+1]=='f' && g_src[i+2]=='d' &&
                g_src[i+3]=='e' && g_src[i+4]=='f') {
                g_cond_depth++;
                if (!g_cond_skip) g_cond_skip = 1;   /* CC_HOST: undefined here */
            } else if (dl == 4 && g_src[i]=='e' && g_src[i+1]=='l' &&
                       g_src[i+2]=='s' && g_src[i+3]=='e') {
                if (g_cond_depth == 1) g_cond_skip = !g_cond_skip;
            } else if (dl == 5 && g_src[i]=='e' && g_src[i+1]=='n' &&
                       g_src[i+2]=='d' && g_src[i+3]=='i' && g_src[i+4]=='f') {
                if (g_cond_depth) { g_cond_depth--; if (!g_cond_depth) g_cond_skip = 0; }
            }
            while (i < g_slen && g_src[i] != '\n') i++;
            continue;
        }
        if (g_cond_skip) {                        /* inside a false branch */
            while (i < g_slen && g_src[i] != '\n') i++;
            continue;
        }
        if (is_space(c)) { i++; continue; }
        if (c == '/' && i + 1 < g_slen && g_src[i+1] == '/') {
            while (i < g_slen && g_src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < g_slen && g_src[i+1] == '*') {
            i += 2;
            while (i + 1 < g_slen && !(g_src[i] == '*' && g_src[i+1] == '/')) {
                if (g_src[i] == '\n') line++;
                i++;
            }
            i += 2;
            continue;
        }
        if (g_ntok >= g_tok_cap - 1) { err_set("source too complex"); return 0; }
        token_t *t = &g_tok[g_ntok];
        t->line = line; t->len = 0; t->num = 0; t->op = -1;
        if (is_alpha(c)) {
            uint32_t s = i;
            while (i < g_slen && is_alnum(g_src[i])) i++;
            t->kind = TK_ID; t->sp = &g_src[s]; t->len = (int)(i - s);
            g_ntok++; continue;
        }
        if (is_digit(c)) {
            int v = 0;
            if (c == '0' && i + 1 < g_slen && (g_src[i+1] == 'x' || g_src[i+1] == 'X')) {
                i += 2;
                for (;;) {
                    int d = g_src[i], nv = -1;
                    if (i >= g_slen) break;
                    if (d >= '0' && d <= '9') nv = d - '0';
                    else if (d >= 'a' && d <= 'f') nv = d - 'a' + 10;
                    else if (d >= 'A' && d <= 'F') nv = d - 'A' + 10;
                    if (nv < 0) break;
                    v = v * 16 + nv; i++;
                }
            } else {
                while (i < g_slen && is_digit((unsigned char)g_src[i]))
                    v = v * 10 + (g_src[i++] - '0');
            }
            /* integer suffixes: 15u, 0x2000000u, 100L ... */
            while (i < g_slen && (g_src[i] == 'u' || g_src[i] == 'U' ||
                                  g_src[i] == 'l' || g_src[i] == 'L')) i++;
            t->kind = TK_NUM; t->num = v; g_ntok++; continue;
        }
        if (c == '\'') {
            int v = 0;
            i++;
            if (i < g_slen && g_src[i] == '\\') {
                i++;
                int e = g_src[i++];
                switch (e) {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = 0; break;
                case '\\': v = '\\'; break;
                case '\'': v = '\''; break;
                default: v = e; break;
                }
            } else if (i < g_slen) v = (unsigned char)g_src[i++];
            while (i < g_slen && g_src[i] != '\'') i++;
            if (i < g_slen) i++;
            t->kind = TK_CHR; t->num = v; g_ntok++; continue;
        }
        if (c == '"') {
            uint32_t s = ++i;
            while (i < g_slen && g_src[i] != '"') {
                if (g_src[i] == '\\' && i + 1 < g_slen) i++;
                i++;
            }
            t->kind = TK_STR; t->sp = &g_src[s]; t->len = (int)(i - s);
            if (i < g_slen) i++;
            g_ntok++; continue;
        }
        {
            int best = -1, bn = 0;
            for (int n = 3; n >= 1; n--) {
                if (i + (uint32_t)n > g_slen) continue;
                for (int k = 0; g_punct[k]; k++) {
                    int L = slen(g_punct[k]);
                    if (L != n) continue;
                    int m = 0;
                    while (m < n && g_punct[k][m] == g_src[i + m]) m++;
                    if (m == n) { best = k; bn = n; break; }
                }
                if (best >= 0) break;
            }
            if (best < 0) { err_set("bad character"); return 0; }
            t->kind = TK_PUNCT; t->op = best; t->len = bn;
            i += bn; g_ntok++; continue;
        }
    }
    g_tok[g_ntok].kind = TK_EOF;
    g_tok[g_ntok].op = -1;
    g_tok[g_ntok].num = 0;
    g_tok[g_ntok].len = 0;
    g_tok[g_ntok].sp = &g_src[g_slen];
    g_tok[g_ntok].line = line;
    return 1;
}

/* ====================================================================
 * symbols
 * ================================================================== */
#define SYM_MAX 1024          /* locals never recycle their slots */
#define LOC_MAX 128          /* for-int decls accumulate */

enum { SY_FUNC = 1, SY_GVAR, SY_LVAR };

typedef struct {
    char name[28];
    int  kind, off, sz, is_ptr, is_arr, arr_sz;
    int  addr, nparams, stidx;   /* stidx: index into g_st when typed over a struct */
} sym_t;

static sym_t   g_sym_static[64];
static sym_t  *g_sym;
static uint32_t g_sym_cap = 64;
static int   g_nsym;
static sym_t *g_loc[LOC_MAX];
static int    g_nloc;
static int    g_frame;

/* data section (strings + globals), copied in after code.  Heap-backed
 * like g_ob/g_src: when the compiler builds ITSELF its own globals need
 * ~90 KB of data, beyond any static buffer this image could carry. */
static uint8_t  g_data_static[8192];
static uint8_t *g_data;
static uint32_t g_data_cap = 8192;
static uint32_t g_dlen;

/* relocations in the code: 32-bit immediates holding a data offset */
#define REL_MAX 1024          /* string literals: ~600 in cc_self */
static int  g_rel_static[64];    /* 32 at + 32 off */
static int *g_rel_at;
static int *g_rel_off;
static uint32_t g_rel_cap = 32;
static int g_nrel;

/* call sites: absolute address of the callee resolved at the end */
#define CALLMAX 1024
static int  g_call_static[64];   /* 32 at + 32 sym */
static int *g_call_at;
static int *g_call_sym;
static uint32_t g_call_cap = 32;
static int    g_ncall;

/* api slot loads to patch: mov eax,[imm32] with the api offset */
#define APIREF_MAX 256
static int g_apiref[APIREF_MAX];
static int g_napiref;

/* data-to-data relocations: global pointer slots that hold the absolute
 * address of a string inside the data section (patched in fixup_code,
 * once the final code size -- and therefore the data base -- is known) */
static int g_drel_at[DREL_MAX];
static int g_drel_off[DREL_MAX];
static int g_drel_n;

static int name_eqn(const char *a, const char *b, int nb) {
    int k = 0;
    while (k < nb && a[k] && a[k] == b[k]) k++;
    return k == nb && a[k] == 0;
}
static sym_t *gfind(const char *n, int len) {
    for (int i = 0; i < g_nsym; i++)
        if (name_eqn(g_sym[i].name, n, len)) return &g_sym[i];
    return 0;
}
static sym_t *lfind(const char *n, int len) {
    for (int i = g_nloc - 1; i >= 0; i--)
        if (name_eqn(g_loc[i]->name, n, len)) return g_loc[i];
    return 0;
}
static sym_t *sym_new(const char *n, int len, int kind) {
    if (g_nsym >= (int)g_sym_cap) { err_set("too many symbols"); return 0; }
    sym_t *s = &g_sym[g_nsym++];
    memzero((char *)s, (int)sizeof(*s));
    if (len > 27) len = 27;
    sncpy(s->name, n, len);
    s->kind = kind;
    s->sz = 4;
    return s;
}

/* ---- struct definitions (stage 1) ---- */
typedef struct {
    char tag[TAG_MAX];
    int size, nmemb;
    int m_off[SMEM_MAX];
    int m_sz[SMEM_MAX];     /* 1=char, 4=int/ptr */
    int m_ptr[SMEM_MAX];    /* member is a pointer */
    int m_arr[SMEM_MAX];    /* member is an array */
    int m_asz[SMEM_MAX];    /* array size when m_arr */
    char m_name[768];             /* flat: SMEM_MAX entries x 16 bytes */
} stdef_t;
static stdef_t  g_st_static[4];
static stdef_t *g_st;
static uint32_t g_st_cap = 4;
static int g_nst;

static stdef_t *st_find(const char *n, int len) {
    for (int i = 0; i < g_nst; i++)
        if (name_eqn(g_st[i].tag, n, len)) return &g_st[i];
    return 0;
}
/* resolve a member by name; returns byte offset or -1; fills out params */
static int st_member(stdef_t *d, const char *n, int len,
                     int *osz, int *optr, int *oarr) {
    for (int i = 0; i < d->nmemb; i++)
        if (name_eqn(&d->m_name[i * 16], n, len)) {
            if (osz)  *osz  = d->m_sz[i];
            if (optr) *optr = d->m_ptr[i];
            if (oarr) *oarr = d->m_arr[i];
            return d->m_off[i];
        }
    return -1;
}

/* ---- typedef aliases (stage 1) ---- */
typedef struct {
    char name[TAG_MAX];
    int sz;        /* base size (1/4/struct-size) */
    int ptr;       /* is a pointer typedef */
    int stidx;     /* >=0 when aliases a struct type */
} alias_t;
static alias_t g_alias[TALIAS_MAX];
static int g_nalias;
static alias_t *al_find(const char *n, int len) {
    for (int i = 0; i < g_nalias; i++)
        if (name_eqn(g_alias[i].name, n, len)) return &g_alias[i];
    return 0;
}
static void add_alias(const char *sp, int len, int sz, int ptr, int stidx) {
    if (g_nalias >= TALIAS_MAX) { err_set("too many typedefs"); return; }
    alias_t *a = &g_alias[g_nalias++];
    memzero((char *)a, (int)sizeof(*a));
    if (len > TAG_MAX - 1) len = TAG_MAX - 1;
    sncpy(a->name, sp, len); a->name[len] = 0;
    a->sz = sz; a->ptr = ptr; a->stidx = stidx;
}

/* ---- enum constants (stage 1) ---- */
typedef struct { char name[24]; int val; } const_t;
static const_t g_const[CONST_MAX];
static int g_nconst;
static const_t *c_find(const char *n, int len) {
    for (int i = 0; i < g_nconst; i++)
        if (name_eqn(g_const[i].name, n, len)) return &g_const[i];
    return 0;
}

/* size used by an lvalue chain for store/load (set by gen_lvalue) */
static int g_lv_sz = 4;
static int g_lv_step = 1;   /* ++/-- step: pointee size when the lvalue is a pointer */
static int g_star_no_load;  /* one-shot: outermost '*' in an assignment target keeps the address */

/* g_apin moved to the runtime-initialised g_apin[] above */
static int api_slot(token_t *t) {
    int i;
    for (i = 0; g_apin[i]; i++)
        if (name_eqn(g_apin[i], t->sp, t->len)) return i;
    return -1;
}

/* ====================================================================
 * x86 emitter
 * ================================================================== */
static void e_mov_eax(int v) { ob8(0xB8); ob32((uint32_t)v); }
static void e_lea_eax_ebp(int off) { ob8(0x8D); ob8(0x85); ob32((uint32_t)off); }
static void e_mov_eax_ebp(int off) { ob8(0x8B); ob8(0x85); ob32((uint32_t)off); }
static void e_movsx_eax_ebp(int off) { ob8(0x0F); ob8(0xBE); ob8(0x85); ob32((uint32_t)off); }
static void e_mov_ebp_eax(int off) { ob8(0x89); ob8(0x85); ob32((uint32_t)off); }
static void e_mov8_ebp_eax(int off) { ob8(0x88); ob8(0x85); ob32((uint32_t)off); }
static void e_ld_eax_ptr(void) { ob8(0x8B); ob8(0x00); }
static void e_ldsx_eax_ptr(void) { ob8(0x0F); ob8(0xBE); ob8(0x00); }
static void e_st_eax_ptr(void) { ob8(0x89); ob8(0x08); }     /* mov [eax],ecx */
static void e_st8_eax_ptr(void) { ob8(0x88); ob8(0x08); }    /* mov [eax],cl  */
static void e_push_eax(void) { ob8(0x50); }
static void e_push_ecx(void) { ob8(0x51); }
static void e_pop_eax(void) { ob8(0x58); }
static void e_pop_ecx(void) { ob8(0x59); }
static void e_pop_edx(void) { ob8(0x5A); }
static void e_mov_eax_ecx(void) { ob8(0x89); ob8(0xC8); }
static void e_mov_ecx_eax(void) { ob8(0x89); ob8(0xC1); }
static void e_test_eax(void) { ob8(0x85); ob8(0xC0); }
static void e_neg_eax(void) { ob8(0xF7); ob8(0xD8); }
static void e_not_eax(void) { ob8(0xF7); ob8(0xD0); }
static void e_setcc(int cc) {
    ob8(0x0F); ob8(0x90 | cc); ob8(0xC0);
    ob8(0x0F); ob8(0xB6); ob8(0xC0);
}
static void e_test_setcc(int cc) { e_test_eax(); e_setcc(cc); }
static void e_add_esp(int n) { if (n) { ob8(0x81); ob8(0xC4); ob32((uint32_t)n); } }
static void e_sub_esp(int n) { if (n) { ob8(0x81); ob8(0xEC); ob32((uint32_t)n); } }
static void e_bin_eax_ecx(int op) { ob8(op); ob8(0xC8); }   /* op eax, ecx */
static void e_cmp_eax_ecx(void) { ob8(0x39); ob8(0xC1); }   /* cmp ecx,eax */

/* forward jumps */
static int e_jmp_fwd(void)  { ob8(0xE9); int a = (int)ob_at(); ob32(0); return a; }
static int e_jz_fwd(void)   { ob8(0x0F); ob8(0x84); int a = (int)ob_at(); ob32(0); return a; }
static int e_jnz_fwd(void)  { ob8(0x0F); ob8(0x85); int a = (int)ob_at(); ob32(0); return a; }
static void e_patch_here(int at) { ob_patch((uint32_t)at, ob_at() - (uint32_t)at - 4); }
static void e_jmp_back(int target) { ob8(0xE9); ob32((uint32_t)(target - (int)ob_at() - 4)); }
static void e_jcc_back(int cc, int target) {
    ob8(0x0F); ob8(0x80 | cc); ob32((uint32_t)(target - (int)ob_at() - 4));
}

/* ====================================================================
 * expression codegen
 * ================================================================== */
static void gen_expr(void);
static void gen_assign_expr(void);
static int parse_type(int *sz, int *is_ptr, int *stidx);
static void do_incdec(int kind, int prefix);
static void gen_unary(void);           /* forward: cast in gen_postfix calls it */
static int is_type_kw_at(token_t *t);  /* forward: sizeof type-keyword check */

static sym_t *local_new(const char *n, int len, int sz, int is_ptr, int is_arr, int arr_sz, int stidx) {
    if (g_nloc >= LOC_MAX) { err_set("too many locals"); return 0; }
    sym_t *s = sym_new(n, len, SY_LVAR);
    if (!s) return 0;
    int unit = is_ptr ? 4 : sz;
    int bytes = is_arr ? arr_sz * unit : (is_ptr ? 4 : (sz > 4 ? sz : 4));
    if (bytes < 4) bytes = 4;
    g_frame += bytes;
    s->off = -g_frame;
    s->sz = sz;
    s->is_ptr = is_ptr;
    s->is_arr = is_arr;
    s->arr_sz = arr_sz;
    s->stidx = stidx;
    g_loc[g_nloc++] = s;
    return s;
}

/* helper: emit `mov eax, imm32` with a relocation to a data offset */
static void e_data_addr(int data_off) {
    ob8(0xB8); ob32(0);
    if ((uint32_t)g_nrel < g_rel_cap) { g_rel_at[g_nrel] = (int)ob_at() - 4; g_rel_off[g_nrel] = data_off; g_nrel++; }
    else err_set("too many relocs");
}

/* load variable value / address */
static void gen_load(sym_t *s, int want_addr) {
    if (s->kind == SY_LVAR) {
        if (want_addr) { e_lea_eax_ebp(s->off); return; }
        if (s->is_arr) { e_lea_eax_ebp(s->off); return; }
        if (s->is_ptr) { e_mov_eax_ebp(s->off); return; }
        if (s->sz == 1) e_movsx_eax_ebp(s->off);
        else e_mov_eax_ebp(s->off);
        return;
    }
    if (want_addr || s->is_arr) { e_data_addr(s->addr); return; }
    if (s->is_ptr) { e_data_addr(s->addr); e_ld_eax_ptr(); return; }  /* pointer value */
    e_data_addr(s->addr);
    if (s->sz == 1) e_ldsx_eax_ptr();
    else e_ld_eax_ptr();
}

/* push args right-to-left: we evaluate left-to-right and reverse */
static int push_args(void) {
    int n = 0;
    if (!at_op(OP_RP)) {
        for (;;) {
            gen_assign_expr();
            e_push_eax();
            n++;
            if (!accept_op(OP_COMMA)) break;
        }
    }
    if (!accept_op(OP_RP)) err_set("expected )");
    for (int i = 0; i < n / 2; i++) {
        int o1 = i * 4, o2 = (n - 1 - i) * 4;
        ob8(0x8B); ob8(0x44); ob8(0x24); ob8((uint32_t)o1);
        ob8(0x8B); ob8(0x4C); ob8(0x24); ob8((uint32_t)o2);
        ob8(0x89); ob8(0x4C); ob8(0x24); ob8((uint32_t)o1);
        ob8(0x89); ob8(0x44); ob8(0x24); ob8((uint32_t)o2);
    }
    return n;
}

/* the size of the element an [index] expression yields */
static int elem_sz(sym_t *s) {
    /* a pointer occupies 4 bytes; an array element occupies sizeof(*p) */
    if (s->is_ptr) return 4;
    if (s->is_arr) return s->sz;
    return s->sz;
}

/* emit `base + index*esize` into EAX */
static void gen_addr_indexed(sym_t *s) {
    gen_load(s, 1);                    /* base address */
    e_push_eax();
    gen_expr();
    int es = elem_sz(s);
    if (es != 1) { ob8(0x69); ob8(0xC0); ob32((uint32_t)es); }  /* imul eax,eax,es */
    e_pop_ecx();
    ob8(0x01); ob8(0xC1);              /* add ecx,eax */
    e_mov_eax_ecx();
}

/* decode a string literal's raw source text (sp[0..len)) into dst, up to
 * max bytes; returns bytes written (no NUL appended -- callers terminate).
 * Shared by the local and the global string-initialiser paths so escapes
 * (\n \t \r \0 \\ \") decode identically everywhere. */
static int str_decode(char *dst, int max, const char *sp, int len) {
    int k = 0;
    for (int i = 0; i < len && k < max; i++) {
        char c = sp[i];
        if (c == '\\' && i + 1 < len) {
            char e = sp[++i];
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = 0; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            default: c = e; break;
            }
        }
        dst[k++] = c;
    }
    return k;
}

/* postfix expression: literal / name / call / index */
/* Emit the address of an lvalue (name already consumed).  Applies an optional
 * postfix chain of [index], .member and ->member; when do_load is set the
 * final scalar/pointer value is loaded from the resulting address (array
 * lvalues decay to their address instead).  g_lv_sz receives the element size
 * for a subsequent store. */
static void gen_lvalue(sym_t *s, int do_load) {
    int esize, obj_st, obj_ptr, obj_arr, obj_sz;
    int indexed = 0;
    int pdepth = 0;      /* pointer depth of the current object */
    int ptreis = 0;      /* indexing the current object yields a pointer */
    int base_sz = s->sz; /* ultimate base size (char=1, struct=sizeof) */
    if (s->kind == SY_LVAR) e_lea_eax_ebp(s->off);
    else                    e_data_addr(s->addr);

    if (s->is_arr) {
        obj_arr = 1;
        if (s->is_ptr)            { obj_ptr = 0; obj_st = s->stidx; esize = 4; pdepth = 1; ptreis = 1; obj_sz = 4; }
        else if (s->stidx >= 0)   { obj_ptr = 0; obj_st = s->stidx; esize = s->sz; obj_sz = s->sz; }
        else                      { obj_ptr = 0; obj_st = -1;        esize = s->sz; obj_sz = s->sz; }
    } else if (s->is_ptr) {
        obj_ptr = 1; obj_st = s->stidx; obj_arr = 0; obj_sz = 4;
        pdepth = s->is_ptr;
        ptreis = (pdepth >= 2) ? 1 : 0;
        esize = (pdepth >= 2) ? 4 : base_sz;
    } else {
        obj_ptr = 0; obj_st = s->stidx; obj_arr = 0; obj_sz = s->sz; esize = s->sz;
    }

    for (;;) {
        if (at_op(OP_LSB)) {
            indexed = 1;
            g_ti++;
            /* a POINTER base holds the array address in its VALUE: load it
             * first.  An ARRAY base (or a just-computed address) is already
             * the base -- eax stays.  (argv[0] with argv a char** param.) */
            if (obj_ptr && !obj_arr) e_ld_eax_ptr();
            e_push_eax();
            gen_expr();
            if (esize != 1) { ob8(0x69); ob8(0xC0); ob32((uint32_t)esize); }
            e_pop_ecx();
            ob8(0x01); ob8(0xC1);              /* add ecx,eax */
            e_mov_eax_ecx();
            if (!accept_op(OP_RSB)) { err_set("expected ]"); return; }
            if (ptreis)           { obj_ptr = 1; obj_st = s->stidx; obj_sz = 4; pdepth -= 1; ptreis = (pdepth >= 2) ? 1 : 0; esize = (pdepth >= 2) ? 4 : base_sz; }
            else                  { obj_ptr = 0; obj_st = s->stidx; obj_sz = esize; esize = esize; pdepth = 0; }
            obj_arr = 0;
            continue;
        }
        if (at_op(OP_DOT) || at_op(OP_ARROW)) {
            int arrow = at_op(OP_ARROW);
            indexed = 1;
            g_ti++;
            token_t *mn = cur();
            if (mn->kind != TK_ID) { err_set("member name expected"); return; }
            if (obj_st < 0) { err_set("member access on non-struct"); g_ti++; return; }
            stdef_t *d = &g_st[obj_st];
            int msz = 4, mptr = 0, marr = 0;
            int moff = st_member(d, mn->sp, mn->len, &msz, &mptr, &marr);
            g_ti++;
            if (moff < 0) { err_set("no such member"); return; }
            if (arrow) e_ld_eax_ptr();          /* -> : deref the pointer slot */
            if (moff) { ob8(0x81); ob8(0xC0); ob32((uint32_t)moff); }  /* add eax,imm */
            if (mptr)      {
                obj_ptr = 1; obj_st = -1; obj_arr = 0; obj_sz = 4;
                pdepth = mptr; base_sz = msz;
                ptreis = (mptr >= 2) ? 1 : 0;
                esize = (mptr >= 2) ? 4 : msz;
            }
            else if (marr) { obj_arr = 1; obj_ptr = 0; obj_st = -1; obj_sz = msz; esize = msz; pdepth = 0; ptreis = 0; }
            else           { obj_arr = 0; obj_ptr = 0; obj_st = -1; obj_sz = msz; esize = msz; pdepth = 0; ptreis = 0; }
            continue;
        }
        break;
    }

    g_lv_sz = obj_sz;
    /* ++/-- steps by the pointee size only when the lvalue IS a pointer
     * variable itself; p[i]++ and p->m++ step the object value by 1 */
    g_lv_step = 1;
    if (!indexed && s->is_ptr && !s->is_arr) {
        g_lv_step = (s->is_ptr >= 2) ? 4
                  : ((s->stidx >= 0) ? g_st[s->stidx].size : s->sz);
        if (g_lv_step < 1) g_lv_step = 1;
    }
    if (at_op(OP_INC) || at_op(OP_DEC)) {       /* postfix ++/-- */
        int kind = at_op(OP_INC) ? OP_INC : OP_DEC;
        g_ti++;
        do_incdec(kind, 0);
        return;
    }
    if (do_load && !obj_arr) {
        if (obj_sz == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();
    }
}

static void gen_postfix(void) {
    token_t *t = cur();
    if (t->kind == TK_NUM || t->kind == TK_CHR) {
        e_mov_eax(t->num);
        g_ti++;
        return;
    }
    if (t->kind == TK_STR) {
        if (g_dlen + (uint32_t)t->len + 1 > g_data_cap) {
            err_set("data section full"); g_ti++; return;
        }
        int o = (int)g_dlen;
        int w = str_decode((char *)&g_data[g_dlen], (int)g_data_cap - 1 - (int)g_dlen,
                           t->sp, t->len);
        g_dlen += (uint32_t)w;
        if (g_dlen < g_data_cap) g_data[g_dlen++] = 0;
        e_data_addr(o);
        g_ti++;
        return;
    }
    if (accept_op(OP_LP)) {
        /* C cast: ( type [*]* ) unary.  Lookahead so (expr) still works:
         * the token after '(' must start a type and be followed by stars
         * and ')' -- a variable name never matches. */
        {
            int j = 0, istype = 0, star = 0, ischar = 0;
            token_t *x = peek(0);
            /* optional const qualifier inside a cast: (const char *) */
            if (x->kind == TK_ID && x->len == 5 && x->sp[0]=='c' &&
                x->sp[1]=='o' && x->sp[2]=='n' && x->sp[3]=='s' && x->sp[4]=='t') j = 1;
            x = peek(j);
            /* token text is NOT NUL-terminated: compare by length */
            if (x->kind == TK_ID) {
                if (x->len == 4 && x->sp[0]=='c' && x->sp[1]=='h' &&
                    x->sp[2]=='a' && x->sp[3]=='r')            { istype = 1; ischar = 1; j++; }
                else if ((x->len == 3 && x->sp[0]=='i' && x->sp[1]=='n' && x->sp[2]=='t') ||
                         (x->len == 4 && x->sp[0]=='v' && x->sp[1]=='o' &&
                          x->sp[2]=='i' && x->sp[3]=='d') ||
                         (x->len == 8 && x->sp[0]=='u' && x->sp[1]=='n' && x->sp[2]=='s' &&
                          x->sp[3]=='i' && x->sp[4]=='g' && x->sp[5]=='n' &&
                          x->sp[6]=='e' && x->sp[7]=='d'))     { istype = 1; j++;
                    /* unsigned [int|char]: two-word type */
                    token_t *w = peek(j);
                    if (w->kind == TK_ID && ((w->len == 3 && w->sp[0]=='i' &&
                         w->sp[1]=='n' && w->sp[2]=='t') ||
                        (w->len == 4 && w->sp[0]=='c' && w->sp[1]=='h' &&
                         w->sp[2]=='a' && w->sp[3]=='r'))) j++;
                }
                else if (al_find(x->sp, x->len))               { istype = 1; j++; }
            } else if (x->kind == TK_PUNCT && x->op == OP_LP) {
                /* ( struct name [*] ) */
                token_t *s1 = peek(j + 1);
                if (s1->kind == TK_ID && s1->len == 6 &&
                    s1->sp[0]=='s' && s1->sp[1]=='t' && s1->sp[2]=='r' &&
                    s1->sp[3]=='u' && s1->sp[4]=='c' && s1->sp[5]=='t' &&
                    peek(j + 2)->kind == TK_ID) { istype = 1; j += 3; }
            }
            while (istype) {
                token_t *y = peek(j);
                if (y->kind == TK_PUNCT && y->op == OP_STAR) { star = 1; ischar = 0; j++; continue; }
                if (y->kind == TK_PUNCT && y->op == OP_RP) break;
                istype = 0;
            }
            if (istype) {
                g_ti += j + 1;                  /* type tokens + ')' */
                gen_unary();
                if (ischar) { ob8(0x0F); ob8(0xB6); ob8(0xC0); }  /* movzx eax,al */
                return;
            }
        }
        gen_expr();
        if (!accept_op(OP_RP)) err_set("expected )");
        return;
    }
    if (t->kind != TK_ID) { err_set("expression expected"); return; }

    /* api -> name ( ... ) */
    if (peek(0)->kind == TK_ID && name_eqn("api", t->sp, t->len) &&
        peek(1)->kind == TK_PUNCT && peek(1)->op == OP_ARROW) {
        int slot = api_slot(peek(2));
        if (slot < 0) { err_set("unknown api function"); return; }
        g_ti += 3;
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int n = push_args();
        ob8(0xA1); ob32(0);                    /* mov eax,[api_slot] */
        if (g_napiref < APIREF_MAX) g_apiref[g_napiref++] = (int)ob_at() - 4;
        else err_set("too many api calls");
        if (slot * 4 > 127) { ob8(0xFF); ob8(0x90); ob32((uint32_t)(slot * 4)); }
        else { ob8(0xFF); ob8(0x50); ob8((uint32_t)(slot * 4)); }
        e_add_esp(n * 4);
        return;
    }

    sym_t *s = lfind(t->sp, t->len);
    if (!s) s = gfind(t->sp, t->len);
    if (!s) {
        const_t *c = c_find(t->sp, t->len);
        if (c) { e_mov_eax(c->val); g_ti++; return; }
        err_set("undeclared name");
        return;
    }
    g_ti++;

    if (s->kind == SY_FUNC) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int n = push_args();
        ob8(0xE8); ob32(0);
        if ((uint32_t)g_ncall < g_call_cap) {
            g_call_at[g_ncall] = (int)ob_at() - 4;
            g_call_sym[g_ncall] = (int)(s - g_sym);
            g_ncall++;
        } else err_set("too many calls");
        e_add_esp(n * 4);
        /* postfix ->member on a call result: peek(n)->kind etc.  The
         * function's declared return type (a struct pointer) supplies the
         * member offsets. */
        for (;;) {
            if (at_op(OP_ARROW) && s->stidx >= 0) {
                g_ti++;
                token_t *mn = cur();
                if (mn->kind != TK_ID) { err_set("member name expected"); return; }
                stdef_t *d = &g_st[s->stidx];
                int msz = 4, mptr = 0, marr = 0;
                int moff = st_member(d, mn->sp, mn->len, &msz, &mptr, &marr);
                g_ti++;
                if (moff < 0) { err_set("no such member"); return; }
                e_ld_eax_ptr();                   /* deref the struct pointer */
                if (moff) { ob8(0x81); ob8(0xC0); ob32((uint32_t)moff); }
                if (!mptr) e_ld_eax_ptr();        /* load the member value */
                continue;
            }
            break;
        }
        return;
    }
    gen_lvalue(s, 1);
}

static void gen_unary(void) {
    if (at_kw("sizeof")) {
        g_ti++;
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int deref = 0;
        if (accept_op(OP_STAR)) deref = 1;
        int sz = 4;
        token_t *tt = cur();
        if (tt->kind == TK_ID && !is_type_kw_at(tt)) {
            alias_t *a = al_find(tt->sp, tt->len);
            sym_t *vs = lfind(tt->sp, tt->len);
            if (!vs) vs = gfind(tt->sp, tt->len);
            if (a) {
                g_ti++;
                sz = (a->stidx >= 0) ? g_st[a->stidx].size : a->sz;
                if (deref) sz = 4;
            } else if (vs) {
                g_ti++;
                if (deref)      sz = (vs->stidx >= 0) ? g_st[vs->stidx].size : (vs->sz == 1 ? 1 : 4);
                else if (vs->is_arr) sz = vs->arr_sz * (vs->is_ptr ? 4 : vs->sz);
                else            sz = vs->sz;
            } else { err_set("sizeof expects type"); return; }
        } else {
            int tsz = 4, tp = 0, tst = -1;
            if (!parse_type(&tsz, &tp, &tst)) { err_set("sizeof expects type"); return; }
            sz = (tst >= 0) ? g_st[tst].size : tsz;
            if (deref) sz = 4;
        }
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_mov_eax(sz);
        return;
    }
    int o = tok_op();
    if (g_tok[g_ti].kind == TK_PUNCT) {
        if (o == OP_NOT)   { g_ti++; gen_unary(); e_test_setcc(0x4); return; }
        if (o == OP_MINUS) { g_ti++; gen_unary(); e_neg_eax(); return; }
        if (o == OP_TILDE) { g_ti++; gen_unary(); e_not_eax(); return; }
        if (o == OP_AMP)   {
            g_ti++;
            token_t *a = cur();
            if (a->kind == TK_ID) {
                sym_t *as = lfind(a->sp, a->len);
                if (!as) as = gfind(a->sp, a->len);
                if (as && as->kind != SY_FUNC) { g_ti++; gen_lvalue(as, 0); return; }
            }
            gen_unary();
            return;
        }   /* address-of */
        if (o == OP_STAR)  {
            g_ti++;
            gen_unary();
            if (g_star_no_load) g_star_no_load = 0;   /* assignment target: keep address */
            else e_ld_eax_ptr();
            return;
        }
        if (o == OP_PLUS)  { g_ti++; gen_unary(); return; }
        if (o == OP_INC || o == OP_DEC) {       /* prefix ++/-- */
            g_ti++;
            token_t *op = cur();
            if (op->kind == TK_ID) {
                sym_t *vs = lfind(op->sp, op->len);
                if (!vs) vs = gfind(op->sp, op->len);
                if (vs && vs->kind != SY_FUNC) {
                    g_ti++;
                    gen_lvalue(vs, 0);
                    do_incdec(o, 1);
                    return;
                }
            }
            err_set("bad prefix ++");
            return;
        }
    }
    gen_postfix();
}

static int binop_prec(void) {
    if (g_tok[g_ti].kind != TK_PUNCT) return -1;
    switch (g_tok[g_ti].op) {
    case OP_OROR:    return 2;
    case OP_ANDAND:  return 3;
    case OP_PIPE:    return 4;
    case OP_CARET:   return 5;
    case OP_AMP:     return 6;
    case OP_EQ: case OP_NE: return 7;
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 8;
    case OP_SHL: case OP_SHR: return 9;
    case OP_PLUS: case OP_MINUS: return 10;
    case OP_STAR: case OP_SLASH: case OP_PERCENT: return 11;
    default: return -1;
    }
}

static void gen_binary(int minprec) {
    gen_unary();
    for (;;) {
        int prec = binop_prec();
        if (prec < minprec) break;
        int op = g_tok[g_ti].op;
        g_ti++;
        if (op == OP_ANDAND) {
            e_test_eax();
            int j = e_jz_fwd();
            gen_binary(prec + 1);
            e_test_setcc(0x5);             /* setne */
            int e = e_jmp_fwd();
            e_patch_here(j);
            e_mov_eax(0);
            e_patch_here(e);
            continue;
        }
        if (op == OP_OROR) {
            e_test_eax();
            int j = e_jnz_fwd();
            gen_binary(prec + 1);
            e_test_setcc(0x5);
            int e = e_jmp_fwd();
            e_patch_here(j);
            e_mov_eax(1);
            e_patch_here(e);
            continue;
        }
        e_push_eax();
        gen_binary(prec + 1);
        e_pop_ecx();                        /* ecx = left, eax = right */
        switch (op) {
        case OP_PLUS:    e_bin_eax_ecx(0x01); break;              /* add eax,ecx */
        case OP_MINUS:   ob8(0x29); ob8(0xC1); e_mov_eax_ecx(); break; /* sub ecx,eax */
        case OP_STAR:    ob8(0x0F); ob8(0xAF); ob8(0xC1); break;  /* imul eax,ecx */
        /* left is in ecx, right in eax: xchg puts the right side in ecx */
        case OP_SLASH:   ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9); break;
        case OP_PERCENT: ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9);
                         ob8(0x89); ob8(0xD0); break;             /* mov eax,edx */
        case OP_AMP:     e_bin_eax_ecx(0x21); break;
        case OP_PIPE:    e_bin_eax_ecx(0x09); break;
        case OP_CARET:   e_bin_eax_ecx(0x31); break;
        case OP_SHL:     ob8(0x87); ob8(0xC8); ob8(0xD3); ob8(0xE0); break; /* xchg eax,ecx; shl eax,cl */
        case OP_SHR:     ob8(0x87); ob8(0xC8); ob8(0xD3); ob8(0xF8); break; /* xchg eax,ecx; sar eax,cl */
        case OP_EQ: e_cmp_eax_ecx(); e_setcc(0x4); break;
        case OP_NE: e_cmp_eax_ecx(); e_setcc(0x5); break;
        case OP_LT: e_cmp_eax_ecx(); e_setcc(0xC); break;
        case OP_GT: e_cmp_eax_ecx(); e_setcc(0xF); break;
        case OP_LE: e_cmp_eax_ecx(); e_setcc(0xE); break;
        case OP_GE: e_cmp_eax_ecx(); e_setcc(0xD); break;
        default: err_set("bad operator"); return;
        }
    }
}

/* assignment: `lvalue OP= rhs`; lvalue may carry [index] / .member / ->member */
static void gen_assign_expr(void) {
    token_t *t = cur();
    if (t->kind == TK_PUNCT && t->op == OP_STAR) {
        /* *expr = v / *expr op= v : deref assignment (single-star target).
         * Lookahead walks the unary chain (with nesting and postfix ++/--),
         * looking for an assignment operator at depth 0. */
        int j = 1, pl = 0, hit = 0;
        for (;;) {
            token_t *x = peek(j);
            if (x->kind == TK_EOF) break;
            if (x->kind == TK_PUNCT && (x->op == OP_LP || x->op == OP_LSB)) { pl++; j++; continue; }
            if (x->kind == TK_PUNCT && (x->op == OP_RP || x->op == OP_RSB)) { pl--; j++; continue; }
            if (x->kind == TK_PUNCT && (x->op == OP_INC || x->op == OP_DEC)) { j++; continue; }
            if (pl == 0 && x->kind == TK_PUNCT) {
                int o = x->op;
                if (o == OP_ASG || o == OP_PLUSASG || o == OP_MINUSASG ||
                    o == OP_MULASG || o == OP_DIVASG || o == OP_MODASG ||
                    o == OP_ANDASG || o == OP_ORASG || o == OP_XORASG) hit = 1;
                break;
            }
            j++;
        }
        if (hit) {
            int o = peek(j)->op;
            g_star_no_load = 1;
            g_ti++;                          /* consume '*' */
            gen_unary();                     /* eax = target address */
            e_push_eax();
            if (o == OP_ASG) {
                g_ti++;                      /* consume op */
                gen_assign_expr();           /* rhs -> eax */
                e_mov_ecx_eax();
                e_pop_eax();
                e_st_eax_ptr();
                e_mov_eax_ecx();
                return;
            }
            ob8(0x8B); ob8(0x04); ob8(0x24); /* eax=[esp]=addr */
            e_ld_eax_ptr();                  /* eax = old */
            e_mov_ecx_eax();
            e_push_ecx();
            g_ti++;                          /* consume op */
            gen_assign_expr();               /* eax = rhs */
            e_pop_ecx();                     /* ecx = old */
            switch (o) {
            case OP_PLUSASG:  e_bin_eax_ecx(0x01); break;
            case OP_MINUSASG: ob8(0x29); ob8(0xC1); e_mov_eax_ecx(); break;
            case OP_MULASG:   ob8(0x0F); ob8(0xAF); ob8(0xC1); break;
            case OP_ANDASG:   ob8(0x21); ob8(0xC1); e_mov_eax_ecx(); break;
            case OP_ORASG:    ob8(0x09); ob8(0xC1); e_mov_eax_ecx(); break;
            case OP_XORASG:   ob8(0x31); ob8(0xC1); e_mov_eax_ecx(); break;
            default: err_set("bad compound on deref"); break;
            }
            e_pop_ecx();                     /* ecx = address */
            e_push_eax();
            e_mov_eax_ecx();
            ob8(0x8B); ob8(0x0C); ob8(0x24); /* ecx=[esp]=result */
            e_st_eax_ptr();
            e_pop_eax();
            return;
        }
    }
    if (t->kind == TK_ID) {
        /* look past an optional postfix chain to find the assignment operator */
        int la = 1;
        for (;;) {
            token_t *x = peek(la);
            if (x->kind == TK_PUNCT && x->op == OP_LSB) {
                int depth = 1; la++;
                while (peek(la)->kind != TK_EOF && depth) {
                    if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_LSB) depth++;
                    else if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_RSB) depth--;
                    la++;
                }
            } else if (x->kind == TK_PUNCT && (x->op == OP_DOT || x->op == OP_ARROW)) {
                la++;
                if (peek(la)->kind == TK_ID) la++;
            } else break;
        }
        token_t *nx = peek(la);
        if (nx->kind == TK_PUNCT) {
            int o = nx->op;
            if (o == OP_ASG || o == OP_PLUSASG || o == OP_MINUSASG ||
                o == OP_MULASG || o == OP_DIVASG || o == OP_MODASG ||
                o == OP_ANDASG || o == OP_ORASG || o == OP_XORASG ||
                o == OP_SHLASG || o == OP_SHRASG) {
                sym_t *s = lfind(t->sp, t->len);
                if (!s) s = gfind(t->sp, t->len);
                if (s && s->kind != SY_FUNC) {
                    g_ti++;                       /* consume the NAME */
                    gen_lvalue(s, 0);            /* eax = lvalue address */
                    int es = g_lv_sz;
                    e_push_eax();                 /* keep lvalue address */
                    if (o == OP_ASG) {
                        g_ti++;
                        gen_assign_expr();        /* rhs -> eax */
                        e_mov_ecx_eax();          /* ecx = value */
                        e_pop_eax();              /* eax = address */
                        if (es == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
                        e_mov_eax_ecx();          /* result = value */
                        return;
                    }
                    /* compound: old = [addr]; compute; store */
                    ob8(0x8B); ob8(0x04); ob8(0x24); /* mov eax,[esp] : address */
                    if (es == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();
                    e_mov_ecx_eax();                 /* ecx = old */
                    e_push_ecx();
                    g_ti++;
                    gen_assign_expr();               /* eax = rhs */
                    e_pop_ecx();                     /* ecx = old */
                    switch (o) {
                    case OP_PLUSASG:  e_bin_eax_ecx(0x01); break;
                    case OP_MINUSASG: ob8(0x29); ob8(0xC1); e_mov_eax_ecx(); break;
                    case OP_MULASG:   ob8(0x0F); ob8(0xAF); ob8(0xC1); break;
                    case OP_DIVASG:   ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9); break;
                    case OP_MODASG:   ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9);
                                      ob8(0x89); ob8(0xD0); break;
                    case OP_ANDASG:   ob8(0x21); ob8(0xC1); e_mov_eax_ecx(); break;
                    case OP_ORASG:    ob8(0x09); ob8(0xC1); e_mov_eax_ecx(); break;
                    case OP_XORASG:   ob8(0x31); ob8(0xC1); e_mov_eax_ecx(); break;
                    case OP_SHLASG:   ob8(0x87); ob8(0xC8); ob8(0xD3); ob8(0xE0); break;
                    case OP_SHRASG:   ob8(0x87); ob8(0xC8); ob8(0xD3); ob8(0xF8); break;
                    }
                    e_pop_ecx();                     /* ecx = address */
                    e_push_eax();                    /* result */
                    e_mov_eax_ecx();                 /* eax = address */
                    ob8(0x8B); ob8(0x0C); ob8(0x24); /* mov ecx,[esp] : result */
                    if (es == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
                    e_pop_eax();                     /* result */
                    return;
                }
            }
        }
    }
    gen_binary(1);
    if (at_op(OP_QMARK)) {                 /* ternary: a ? b : c */
        g_ti++;
        e_test_eax();
        int j = e_jz_fwd();
        gen_assign_expr();
        int e = e_jmp_fwd();
        e_patch_here(j);
        if (!accept_op(OP_COLON)) err_set("expected :");
        gen_assign_expr();
        e_patch_here(e);
    }
}

static void gen_expr(void) {
    gen_assign_expr();
    while (at_op(OP_COMMA)) { g_ti++; gen_assign_expr(); }
}

/* ====================================================================
 * statements
 * ================================================================== */
static int g_brk[BRK_MAX];   static int g_nbrk;
static int g_cont[BRK_MAX];  static int g_ncont;
static int g_brk_base, g_cont_base;

/* goto labels (stage 3) */
static char g_lbl_name[1152];   /* LBL_MAX x 24, flat */
static int  g_lbl_off[LBL_MAX];      /* code offset of definition; -1 if forward */
static int  g_nlbl;
static int  g_goto_at[LBL_MAX];      /* unresolved forward goto jump site */
static int  g_goto_lbl[LBL_MAX];
static int  g_ngoto;
static int  lbl_decl(const char *sp, int len) {
    for (int i = 0; i < g_nlbl; i++)
        if (name_eqn(&g_lbl_name[i * 24], sp, len)) return i;
    if (g_nlbl >= LBL_MAX) { err_set("too many labels"); return 0; }
    int idx = g_nlbl++;
    int k; for (k = 0; k < 24; k++) g_lbl_name[idx * 24 + k] = 0;
    int L = len > 23 ? 23 : len;
    sncpy(&g_lbl_name[idx * 24], sp, L); g_lbl_off[idx] = -1;
    return idx;
}

/* increment/decrement an lvalue whose address is in EAX; uses g_lv_sz.
 * kind=OP_INC/OP_DEC; prefix=1 gives new value, 0 gives old value. */
static void do_incdec(int kind, int prefix) {
    int es = g_lv_sz;
    int step = g_lv_step;                      /* pointer: element size */
    if (step < 1) step = 1;
    e_push_eax();                              /* [esp]=addr */
    ob8(0x8B); ob8(0x04); ob8(0x24);           /* eax=[esp]=addr */
    if (es == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();   /* eax=old */
    e_push_eax();                              /* [esp]=addr,[esp+4]=old */
    if (kind == OP_INC) { ob8(0x05); ob32((uint32_t)step); } /* add eax,step */
    else                { ob8(0x2D); ob32((uint32_t)step); } /* sub eax,step */
    ob8(0x8B); ob8(0x4C); ob8(0x24); ob8(4);  /* ecx=[esp+4]=addr */
    if (es == 1) { ob8(0x88); ob8(0x01); }    /* mov [ecx],al */
    else         { ob8(0x89); ob8(0x01); }    /* mov [ecx],eax */
    if (prefix) {
        /* result already in eax (new) */
    } else {
        ob8(0x8B); ob8(0x04); ob8(0x24);      /* eax=[esp]=old */
    }
    e_add_esp(8);
}

static int is_type_kw(void) {
    if (at_kw("static") || at_kw("const")) return 1;   /* storage class prefixes */
    if (at_kw("int") || at_kw("char") || at_kw("void") || at_kw("unsigned") ||
        at_kw("struct")) return 1;
    token_t *t = cur();
    if (t->kind == TK_ID) {
        for (int i = 0; i < g_nalias; i++)
            if (name_eqn(g_alias[i].name, t->sp, t->len)) return 1;
    }
    return 0;
}

/* length-based type-keyword test for a token (text is not NUL-terminated) */
static int is_type_kw_at(token_t *t) {
    if (t->kind != TK_ID) return 0;
    if (t->len == 3 && t->sp[0]=='i' && t->sp[1]=='n' && t->sp[2]=='t') return 1;
    if (t->len == 4 && t->sp[0]=='c' && t->sp[1]=='h' && t->sp[2]=='a' && t->sp[3]=='r') return 1;
    if (t->len == 4 && t->sp[0]=='v' && t->sp[1]=='o' && t->sp[2]=='i' && t->sp[3]=='d') return 1;
    if (t->len == 8 && t->sp[0]=='u' && t->sp[1]=='n' && t->sp[2]=='s' && t->sp[3]=='i' &&
        t->sp[4]=='g' && t->sp[5]=='n' && t->sp[6]=='e' && t->sp[7]=='d') return 1;
    if (t->len == 6 && t->sp[0]=='s' && t->sp[1]=='t' && t->sp[2]=='r' &&
        t->sp[3]=='u' && t->sp[4]=='c' && t->sp[5]=='t') return 1;
    for (int i = 0; i < g_nalias; i++)
        if (name_eqn(g_alias[i].name, t->sp, t->len)) return 1;
    return 0;
}

/* parse a type.  *stidx receives the struct index (>=0) when the type is a
 * struct or a typedef over a struct; pass NULL when unneeded. */
static int parse_type(int *sz, int *is_ptr, int *stidx) {
    *sz = 4; *is_ptr = 0;
    if (stidx) *stidx = -1;
    accept_kw("static");                       /* storage class: accepted, ignored */
    accept_kw("const");                        /* const: accepted, ignored */
    accept_kw("static");
    if (accept_kw("char")) *sz = 1;
    else if (accept_kw("int")) *sz = 4;
    else if (accept_kw("unsigned")) { *sz = 4; accept_kw("int");
        if (accept_kw("char")) *sz = 1; }
    else if (accept_kw("void")) *sz = 4;
    else if (accept_kw("struct")) {
        token_t *t = cur();
        if (t->kind != TK_ID) { err_set("struct tag expected"); return 0; }
        stdef_t *d = st_find(t->sp, t->len);
        if (!d) { err_set("undefined struct"); return 0; }
        g_ti++;
        *sz = d->size;
        if (stidx) *stidx = (int)(d - g_st);
    }
    else {
        token_t *t = cur();
        if (t->kind != TK_ID) return 0;
        alias_t *a = al_find(t->sp, t->len);
        if (!a) return 0;
        g_ti++;
        *sz = a->sz;
        *is_ptr = a->ptr;
        if (stidx) *stidx = a->stidx;
    }
    for (;;) {
        if (!accept_op(OP_STAR)) break;
        int d = *is_ptr;
        d = d + 1;
        *is_ptr = d;
    }
    return 1;
}

static int const_size(void) {
    int neg = 0;
    if (accept_op(OP_MINUS)) neg = 1;
    if (cur()->kind == TK_ID) {
        /* enum constant as an array size: g_sym[SYM_MAX] */
        const_t *c = c_find(cur()->sp, cur()->len);
        if (!c) return 0;                    /* unsized []: silent */
        int v = c->val;
        g_ti++;
        return neg ? -v : v;
    }
    if (cur()->kind != TK_NUM) { return 0; }  /* unsized []: silent */
    int v = cur()->num;
    g_ti++;
    return neg ? -v : v;
}

static void gen_local_decl(int need_semi) {
    int sz, ptr, st = -1, is_arr = 0, arr_sz = 0;
    if (!parse_type(&sz, &ptr, &st)) { err_set("type expected"); return; }
    for (;;) {
        token_t *nm = cur();
        if (nm->kind != TK_ID) { err_set("name expected"); return; }
        g_ti++;
        is_arr = 0; arr_sz = 0;
        if (accept_op(OP_LSB)) {
            arr_sz = const_size();
            if (!arr_sz) arr_sz = 16;
            if (!accept_op(OP_RSB)) err_set("expected ]");
            is_arr = 1;
        }
        sym_t *s = local_new(nm->sp, nm->len, sz, ptr, is_arr, arr_sz, st);
        if (!s) return;
        if (accept_op(OP_ASG)) {
            if (at_op(OP_LB)) {
                /* local array brace init: int a[4] = { 0, 0, 0, 0 }; */
                g_ti++;
                uint32_t unit = ptr ? 4 : sz;
                int idx = 0;
                while (!at_op(OP_RB) && cur()->kind != TK_EOF) {
                    int neg = 0;
                    if (accept_op(OP_MINUS)) neg = 1;
                    int v = 0;
                    if (cur()->kind == TK_NUM || cur()->kind == TK_CHR) { v = cur()->num; g_ti++; }
                    else { err_set("bad local array initialiser"); return; }
                    if (neg) v = -v;
                    e_mov_eax(v);
                    e_push_eax();
                    e_lea_eax_ebp(s->off + idx * (int)unit);
                    e_pop_ecx();
                    if (unit == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
                    idx++;
                    accept_op(OP_COMMA);
                }
                g_ti++;
                if (!is_arr) err_set("brace init on scalar");
            } else {
            /* zero the array, or store the scalar */
            gen_assign_expr();
            e_push_eax();
            e_lea_eax_ebp(s->off);
            e_pop_ecx();
            if (!is_arr && sz == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
            }
        }
        if (!accept_op(OP_COMMA)) break;
    }
    if (need_semi && !accept_op(OP_SEMI)) { err_set("expected ;"); return; }
}

static void gen_stmt(void) {
    if (g_err) return;
    if (at_op(OP_LB)) { g_ti++; while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) { if (accept_op(OP_SEMI)) continue; gen_stmt(); } g_ti++; return; }
    if (accept_op(OP_SEMI)) return;

    if (accept_kw("return")) {
        if (!at_op(OP_SEMI)) gen_expr(); else e_mov_eax(0);
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        ob8(0x89); ob8(0xEC); ob8(0x5D); ob8(0xC3);      /* mov esp,ebp; pop ebp; ret */
        return;
    }
    if (accept_kw("if")) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_test_eax();
        int j = e_jz_fwd();
        gen_stmt();
        if (accept_kw("else")) {
            int e = e_jmp_fwd();
            e_patch_here(j);
            gen_stmt();
            e_patch_here(e);
        } else e_patch_here(j);
        return;
    }
    if (accept_kw("while")) {
        int top = (int)ob_at();
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_test_eax();
        int j = e_jz_fwd();
        int bb = g_nbrk, cb = g_ncont;
        gen_stmt();
        e_jmp_back(top);
        e_patch_here(j);
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        for (int i = cb; i < g_ncont; i++) { ob_patch((uint32_t)g_cont[i], (uint32_t)(top - g_cont[i] - 4)); }
        g_ncont = cb;
        return;
    }
    if (accept_kw("do")) {
        int top = (int)ob_at();
        int bb = g_nbrk, cb = g_ncont;
        gen_stmt();
        if (!accept_kw("while")) { err_set("expected while"); return; }
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        accept_op(OP_SEMI);
        e_test_eax();
        ob8(0x0F); ob8(0x85); ob32(0);   /* jnz rel32 -> top */
        ob_patch((uint32_t)(ob_at() - 4), (uint32_t)((int)top - (int)ob_at()));
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        for (int i = cb; i < g_ncont; i++) { ob_patch((uint32_t)g_cont[i], (uint32_t)(top - g_cont[i] - 4)); }
        g_ncont = cb;
        return;
    }
    if (accept_kw("for")) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        if (is_type_kw()) gen_local_decl(0);
        else if (!at_op(OP_SEMI)) gen_expr();
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        int top = (int)ob_at();
        int jz = -1;
        if (!at_op(OP_SEMI)) { gen_expr(); e_test_eax(); jz = e_jz_fwd(); }
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        /* The step clause is written before the body but must execute
         * after it.  Emit it first and jump over it from `top`; the body
         * then falls through to the step; the step jumps back to `top`:
         *   top: cond / jz end / jmp body
         *   body: <body> / jmp step
         *   step: <step code> / jmp top
         *   end: */
        int has_step = !at_op(OP_RP);
        int jbody = -1;
        if (has_step) jbody = e_jmp_fwd();      /* top -> body */
        int step_pos = (int)ob_at();
        int bb = g_nbrk, cb = g_ncont;
        if (has_step) gen_expr();               /* step code (cursor ends at ')') */
        if (has_step) e_jmp_back(top);          /* step -> cond */
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        if (jbody >= 0) e_patch_here(jbody);    /* body starts here */
        gen_stmt();                             /* body */
        /* body -> step; with no step clause the body loops straight back
         * to the condition (top) so the condition is re-evaluated */
        e_jmp_back(has_step ? step_pos : top);
        if (jz >= 0) e_patch_here(jz);
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        /* continue must jump to the step (or to the condition when there
         * is no step clause); leaving the E9 placeholder unpatched made
         * `continue` fall through in the body and skip the step */
        for (int i = cb; i < g_ncont; i++)
            ob_patch((uint32_t)g_cont[i], (uint32_t)((has_step ? step_pos : top) - g_cont[i] - 4));
        g_ncont = cb;
        return;
    }
    if (accept_kw("switch")) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_push_eax();                          /* save switch value */
        if (!accept_op(OP_LB)) { err_set("expected {"); return; }
        int bb = g_nbrk;
        /* emit statements until break / next label / '}' */
        for (;;) {
            if (at_op(OP_RB) || cur()->kind == TK_EOF) break;
            if (accept_op(OP_SEMI)) continue;
            if (accept_kw("break")) {
                if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
                if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
                continue;                     /* break out of case body */
            }
            if (accept_kw("default")) {
                if (!accept_op(OP_COLON)) { err_set("expected :"); return; }
                /* default body: run statements until next label / rb */
                while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) {
                    if (accept_op(OP_SEMI)) continue;
                    if (accept_kw("case") || accept_kw("default")) { err_set("stray label"); return; }
                    if (accept_kw("break")) {
                        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
                        if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
                        break;
                    }
                    gen_stmt();
                }
                continue;
            }
            if (accept_kw("case")) {
                int v = 0;
                if (accept_op(OP_MINUS)) v = -1;
                if (cur()->kind == TK_NUM || cur()->kind == TK_CHR) {
                    int base = cur()->num; g_ti++;
                    v = (v == -1) ? -base : base;
                } else {
                    const_t *cf = c_find(cur()->sp, cur()->len);
                    if (cf) { v = cf->val; g_ti++; }
                    else { err_set("case constant expected"); return; }
                }
                if (!accept_op(OP_COLON)) { err_set("expected :"); return; }
                ob8(0x8B); ob8(0x04); ob8(0x24);    /* mov eax,[esp] */
                ob8(0x3D); ob32((uint32_t)v);        /* cmp eax,imm32 */
                int jne = e_jnz_fwd();
                /* case body: statements until break / next label / rb */
                while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) {
                    if (accept_op(OP_SEMI)) continue;
                    if (at_kw("case") || at_kw("default")) break;
                    if (accept_kw("break")) {
                        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
                        if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
                        break;
                    }
                    gen_stmt();
                }
                e_patch_here(jne);                   /* next case */
                continue;
            }
            gen_stmt();
        }
        if (!accept_op(OP_RB)) { err_set("expected }"); return; }
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        e_add_esp(4);                          /* pop switch value */
        return;
    }
    if (accept_kw("break")) {
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
        return;
    }
    if (accept_kw("continue")) {
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        if (g_ncont < BRK_MAX) g_cont[g_ncont++] = e_jmp_fwd();
        return;
    }
    if (accept_kw("goto")) {
        token_t *ln = cur();
        if (ln->kind != TK_ID) { err_set("label expected"); return; }
        g_ti++;
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        int idx = lbl_decl(ln->sp, ln->len);
        if (g_lbl_off[idx] >= 0) {
            e_jmp_back(g_lbl_off[idx]);
        } else {
            int at = e_jmp_fwd();
            if (g_ngoto < LBL_MAX) { g_goto_at[g_ngoto] = at; g_goto_lbl[g_ngoto] = idx; g_ngoto++; }
            else err_set("too many gotos");
        }
        return;
    }
    /* label:   (identifier followed by ':') */
    if (cur()->kind == TK_ID && peek(1)->kind == TK_PUNCT && peek(1)->op == OP_COLON) {
        token_t *ln = cur();
        int idx = lbl_decl(ln->sp, ln->len);
        g_ti += 2;
        g_lbl_off[idx] = (int)ob_at();
        for (int i = 0; i < g_ngoto; i++)
            if (g_goto_lbl[i] == idx && g_goto_at[i] >= 0) {
                ob_patch((uint32_t)g_goto_at[i],
                         (uint32_t)(ob_at() - g_goto_at[i] - 4));
                g_goto_at[i] = -1;
            }
        return;
    }
    if (is_type_kw()) { gen_local_decl(1); return; }

    gen_expr();
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
}

/* ====================================================================
 * top level
 * ================================================================== */
/* address of the __ce_api slot inside the image (fixed after codegen) */
static int g_api_slot = -1;
static uint32_t g_entry_off;          /* image offset of ce_main (header entry) */
static int g_entry_jmp = -1;          /* stub jmp at code offset 0 -> ce_main */

static void parse_func(int rsz, int rptr, int rst) {
    /* caller consumed the return type and name, cursor is at '(' */
    g_ti++;                                   /* ( */
    g_nloc = 0;
    g_frame = 0;
    int np = 0;
    if (at_kw("void") && peek(1)->kind == TK_PUNCT && peek(1)->op == OP_RP) {
        g_ti++;                               /* (void): empty parameter list */
    }
    if (!at_op(OP_RP)) {
        for (;;) {
            int psz = 4, pp = 0, pst = -1;
            if (!parse_type(&psz, &pp, &pst)) { accept_kw("int"); }
            token_t *pn = cur();
            if (pn->kind != TK_ID) { err_set("parameter expected"); return; }
            g_ti++;
            sym_t *ps = local_new(pn->sp, pn->len, psz, pp, 0, 0, pst);
            if (ps) ps->off = 8 + np * 4;
            np++;
            if (accept_op(OP_COMMA)) continue;
            break;
        }
    }
    if (!accept_op(OP_RP)) err_set("expected )");
    if (accept_op(OP_SEMI)) return;           /* prototype */

    /* prologue: push ebp / mov ebp,esp / sub esp,frame */
    ob8(0x55); ob8(0x89); ob8(0xE5);
    uint32_t sub_at = ob_at();
    ob8(0x81); ob8(0xEC); ob32(0);
    if (!at_op(OP_LB)) { err_set("expected {"); return; }
    gen_stmt();
    e_mov_eax(0);
    ob8(0x89); ob8(0xEC); ob8(0x5D); ob8(0xC3);
    ob_patch(sub_at + 2, (uint32_t)((g_frame + 15) & ~15u));
}

/* ---- stage 1: struct / typedef / enum top-level parsers ---- */

/* parse a struct body `struct [tag] { members }`; leaves cursor after '}'.
 * Used both for plain struct definitions and by typedef. */
static stdef_t *parse_struct_def(void) {
    accept_kw("struct");
    token_t *tag = cur();
    stdef_t *d = 0;
    int have_tag = 0;
    if (tag->kind == TK_ID) {
        g_ti++; have_tag = 1;
        d = st_find(tag->sp, tag->len);
        if (!d) {
            if (g_nst >= (int)g_st_cap) { err_set("too many structs"); return 0; }
            d = &g_st[g_nst++];
            memzero((char *)d, (int)sizeof(*d));
            int L = tag->len > TAG_MAX - 1 ? TAG_MAX - 1 : tag->len;
            sncpy(d->tag, tag->sp, L); d->tag[L] = 0;
        }
    }
    if (!at_op(OP_LB)) {
        /* `struct tag;` forward declaration with no body */
        if (!accept_op(OP_SEMI)) err_set("expected ;");
        return 0;
    }
    if (!d) {
        if (g_nst >= STRUCT_MAX) { err_set("too many structs"); return 0; }
        d = &g_st[g_nst++];
        memzero((char *)d, (int)sizeof(*d));
    }
    g_ti++;                                   /* '{' */
    int off = 0;
    d->nmemb = 0;
    while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) {
        if (accept_op(OP_SEMI)) continue;
        int msz = 4, mptr = 0, mst = -1;
        if (!parse_type(&msz, &mptr, &mst)) { err_set("member type expected"); return 0; }
        token_t *nm = cur();
        if (nm->kind != TK_ID) { err_set("member name expected"); return 0; }
        g_ti++;
        int is_arr = 0, asz = 0;
        if (accept_op(OP_LSB)) {
            asz = const_size(); if (!asz) asz = 16;
            if (!accept_op(OP_RSB)) err_set("expected ]");
            is_arr = 1;
        }
        int align = mptr ? 4 : msz;
        if (align != 1) off = (off + align - 1) & ~(align - 1);
        int idx = d->nmemb;
        int L = nm->len > 15 ? 15 : nm->len;
        sncpy(&d->m_name[idx * 16], nm->sp, L); d->m_name[idx * 16 + L] = 0;
        d->m_off[idx] = off;
        d->m_sz[idx] = msz;
        d->m_ptr[idx] = mptr;
        d->m_arr[idx] = is_arr;
        d->m_asz[idx] = asz;
        int unit = mptr ? 4 : msz;
        off += is_arr ? asz * unit : unit;
        d->nmemb++;
        /* multiple declarators: int a, b, c; */
        while (accept_op(OP_COMMA)) {
            token_t *n2 = cur();
            if (n2->kind != TK_ID) { err_set("member name expected"); return 0; }
            g_ti++;
            int a2 = 0, z2 = 0;
            if (accept_op(OP_LSB)) {
                z2 = const_size(); if (!z2) z2 = 16;
                if (!accept_op(OP_RSB)) err_set("expected ]");
                a2 = 1;
            }
            int al2 = mptr ? 4 : msz;
            if (al2 != 1) off = (off + al2 - 1) & ~(al2 - 1);
            int i2 = d->nmemb;
            int L2 = n2->len > 15 ? 15 : n2->len;
            sncpy(&d->m_name[i2 * 16], n2->sp, L2); d->m_name[i2 * 16 + L2] = 0;
            d->m_off[i2] = off;
            d->m_sz[i2] = msz;
            d->m_ptr[i2] = mptr;
            d->m_arr[i2] = a2;
            d->m_asz[i2] = z2;
            int u2 = mptr ? 4 : msz;
            off += a2 ? z2 * u2 : u2;
            d->nmemb++;
        }
        if (!accept_op(OP_SEMI)) { err_set("expected ; in struct"); return 0; }
    }
    if (!accept_op(OP_RB)) { err_set("expected }"); return 0; }
    d->size = (off + 3) & ~3;
    return d;
}

static void parse_typedef(void) {
    accept_kw("typedef");
    if (at_kw("struct")) {
        stdef_t *d = parse_struct_def();
        token_t *nm = cur();
        if (nm->kind == TK_ID) {
            g_ti++;
            int st = d ? (int)(d - g_st) : -1;
            add_alias(nm->sp, nm->len, d ? d->size : 4, 0, st);
        }
        if (!accept_op(OP_SEMI)) err_set("expected ;");
        return;
    }
    int sz = 4, ptr = 0, st = -1;
    if (!parse_type(&sz, &ptr, &st)) { err_set("typedef type expected"); return; }
    token_t *nm = cur();
    if (nm->kind != TK_ID) { err_set("typedef name expected"); return; }
    g_ti++;
    while (accept_op(OP_STAR)) ptr++;
    add_alias(nm->sp, nm->len, sz, ptr, st);
    if (!accept_op(OP_SEMI)) err_set("expected ;");
}

static void parse_enum(void) {
    accept_kw("enum");
    if (cur()->kind == TK_ID) g_ti++;          /* tag ignored */
    if (accept_op(OP_LB)) {
        int v = 0;
        for (;;) {
            token_t *nm = cur();
            if (nm->kind != TK_ID) { err_set("enum name expected"); return; }
            g_ti++;
            if (accept_op(OP_ASG)) {
                int neg = 0;
                if (accept_op(OP_MINUS)) neg = 1;
                if (cur()->kind != TK_NUM) { err_set("enum value"); return; }
                v = cur()->num; g_ti++;
                if (neg) v = -v;
            }
            if (g_nconst < CONST_MAX) {
                int L = nm->len > 23 ? 23 : nm->len;
                unsigned i;
                for (i = 0; i < 24; i++) g_const[g_nconst].name[i] = 0;   /* sizeof(const_t::name) */
                sncpy(g_const[g_nconst].name, nm->sp, L);
                g_const[g_nconst].val = v;
                g_nconst++;
            } else err_set("too many enum constants");
            v++;
            if (accept_op(OP_COMMA)) continue;
            break;
        }
        if (!accept_op(OP_RB)) { err_set("expected }"); return; }
    }
    if (!accept_op(OP_SEMI)) err_set("expected ;");
}

static void parse_program(void) {
    /* The kernel's CE loader demands entry == 0 and api_off + 4 <= image_size.
     * ce_main may be defined anywhere in the source, so we plant a 5-byte
     * `jmp ce_main` stub at code offset 0 (= image offset 0 after the 20-byte
     * header); the header entry stays 0 and the stub is patched in fixup_code. */
    g_entry_jmp = e_jmp_fwd();
    while (cur()->kind != TK_EOF && !g_err) {
        if (accept_op(OP_SEMI)) continue;
        if (accept_kw("typedef")) { parse_typedef(); continue; }
        if (at_kw("enum")) { parse_enum(); continue; }
        if (at_kw("struct")) {
            int li = 1, isdef = 0;
            if (peek(li)->kind == TK_ID) li++;
            if (peek(li)->kind == TK_PUNCT && peek(li)->op == OP_LB) isdef = 1;
            if (isdef) {
                parse_struct_def();
                if (!accept_op(OP_SEMI)) err_set("expected ; after struct");
                continue;
            }
            /* otherwise: `struct tag v;` falls through as a normal decl */
        }
        int sz, ptr, st = -1;
        int is_func = 0;
        if (!parse_type(&sz, &ptr, &st)) {
            token_t *t = cur();
            if (t->kind == TK_EOF) break;
            if (t->kind == TK_ID) { g_ti++; continue; }  /* unknown type: skip */
            err_set("type expected");
            return;
        }
        for (;;) {
            token_t *nm = cur();
            if (nm->kind != TK_ID) { err_set("name expected"); return; }
            g_ti++;
            int is_arr = 0, arr_sz = 0;
            if (accept_op(OP_LSB)) {
                arr_sz = const_size();
                if (!arr_sz) {
                    /* unsized array: derive the size from the initialiser */
                    if (peek(0)->kind == TK_PUNCT && peek(0)->op == OP_ASG) {
                        if (peek(1)->kind == TK_STR) {
                            token_t *stt = peek(1);
                            char scratch[256];
                            int w = str_decode(scratch, (int)sizeof(scratch) - 1,
                                               stt->sp, stt->len);
                            arr_sz = w + 1;
                        } else if (peek(1)->kind == TK_PUNCT && peek(1)->op == OP_LB) {
                            int j = 2, cnt = 0;
                            for (;;) {
                                token_t *y = peek(j);
                                if (y->kind == TK_PUNCT && y->op == OP_RB) break;
                                if (y->kind == TK_NUM || y->kind == TK_CHR) cnt++;
                                j++;
                                if (j > 128) break;
                            }
                            arr_sz = cnt > 0 ? cnt : 1;
                        }
                    }
                }
                if (!arr_sz) arr_sz = 16;
                if (!accept_op(OP_RSB)) err_set("expected ]");
                is_arr = 1;
            }
            if (at_op(OP_LP) && !is_arr) {
                /* remember ce_main: the kernel jumps to the header entry */
                if (name_eqn("ce_main", nm->sp, nm->len)) g_entry_off = (uint32_t)ob_at();
                sym_t *f = sym_new(nm->sp, nm->len, SY_FUNC);
                if (!f) return;
                f->addr = (int)ob_at();
                f->sz = sz; f->is_ptr = ptr; f->stidx = st;      /* return type */
                is_func = 1;
                parse_func(sz, ptr, st);
                break;                        /* function: no comma list */
            }            sym_t *g = sym_new(nm->sp, nm->len, SY_GVAR);
            if (!g) return;
            g->sz = sz; g->is_ptr = ptr; g->is_arr = is_arr; g->arr_sz = arr_sz;
            g->stidx = st;
            g->addr = (int)g_dlen;
            {
                int unit = ptr ? 4 : sz;
                int bytes = is_arr ? arr_sz * unit : (ptr ? 4 : (sz > 4 ? sz : 4));
                if (bytes < 4) bytes = 4;
                if (g_dlen + (uint32_t)bytes > g_data_cap) {
                    err_set("data section full"); return;
                }
                for (int i = 0; i < bytes; i++) g_data[g_dlen++] = 0;
            }
            if (accept_op(OP_ASG)) {
                if (cur()->kind == TK_STR) {
                    token_t *st = cur();
                    if (is_arr) {
                        /* FIND-03: a global char array initialised from a
                         * string inlines the decoded bytes into its own
                         * reserved slot (NUL-terminated when it fits); the
                         * old pointer-style init left garbage in the array. */
                        int cap = arr_sz * (ptr ? 4 : sz);
                        if (cap < 4) cap = 4;
                        int w = str_decode((char *)&g_data[g->addr], cap,
                                           st->sp, st->len);
                        if (w < cap) g_data[g->addr + (uint32_t)w] = 0;
                        g_ti++;
                    } else {
                        /* char *p = "str": the string is appended at the
                         * end of the data section and the slot stores its
                         * offset (shared escape decoding with locals) */
                        if (g_dlen + (uint32_t)st->len + 1 > g_data_cap) {
                            err_set("data section full"); return;
                        }
                        int o = (int)g_dlen;
                        int w = str_decode((char *)&g_data[g_dlen],
                                           (int)g_data_cap - 1 - (int)g_dlen,
                                           st->sp, st->len);
                        g_dlen += (uint32_t)w;
                        if (g_dlen < g_data_cap) g_data[g_dlen++] = 0;
                        g_ti++;
                        if (g_drel_n < DREL_MAX) {
                            g_drel_at[g_drel_n] = (int)g->addr;
                            g_drel_off[g_drel_n] = o;
                            g_drel_n++;
                        } else err_set("too many data relocs");
                    }
                } else if (at_op(OP_LB)) {
                    /* int a[N] = { v, v, ... } / unsized t a[] = { ... } */
                    g_ti++;
                    uint32_t unit = ptr ? 4 : sz;
                    int idx = 0;
                    while (!at_op(OP_RB) && cur()->kind != TK_EOF) {
                        int neg = 0;
                        if (accept_op(OP_MINUS)) neg = 1;
                        int v = 0;
                        if (cur()->kind == TK_NUM || cur()->kind == TK_CHR) { v = cur()->num; g_ti++; }
                        else { err_set("bad array initialiser"); return; }
                        if (neg) v = -v;
                        uint32_t b2 = (uint32_t)g->addr + (uint32_t)(idx * (int)unit);
                        g_data[b2] = (uint8_t)v;
                        if (unit >= 2) g_data[b2 + 1] = (uint8_t)(v >> 8);
                        if (unit >= 3) g_data[b2 + 2] = (uint8_t)(v >> 16);
                        if (unit >= 4) g_data[b2 + 3] = (uint8_t)(v >> 24);
                        idx++;
                        accept_op(OP_COMMA);
                    }
                    g_ti++;                       /* consume ']' */
                    if (!is_arr) err_set("brace init on scalar");
                } else {
                    int neg = 0;
                    if (accept_op(OP_MINUS)) neg = 1;
                    int v = 0;
                    if (cur()->kind == TK_NUM || cur()->kind == TK_CHR) { v = cur()->num; g_ti++; }
                    else { err_set("constant initialiser expected"); return; }
                    if (neg) v = -v;
                    uint32_t base = (uint32_t)g->addr;
                    g_data[base] = (uint8_t)v;
                    if (!is_arr) {
                        g_data[base+1] = (uint8_t)(v>>8);
                        g_data[base+2] = (uint8_t)(v>>16);
                        g_data[base+3] = (uint8_t)(v>>24);
                    }
                }
            }
            if (accept_op(OP_COMMA)) continue;
            break;
        }
        if (is_func) continue;                     /* a function body has no trailing ; */
        if (!accept_op(OP_SEMI)) {
            if (!g_err) err_set("expected ; after declaration");
            return;
        }
    }
    /* FIND-01: with no ce_main the entry stub would be patched to jump to
     * itself (rel32 = 0-(1+4) = -5, E9 FB FF FF FF) and hang the task;
     * reject the source instead of emitting a dead image. */
    if (!g_err && g_entry_off == 0) err_set("no ce_main");
}

/* ====================================================================
 * final image assembly
 * Layout inside the CE image (loaded at CE_LOAD_BASE):
 *     [0 .. 3]            __ce_api slot (kernel writes the table pointer)
 *     [4 .. 3+code)       code (g_ob)
 *     [4+code .. end)     data (g_data)
 * Functions were emitted at code offsets and every `call`/data/immediate
 * that names an address was recorded as a relocation in code coordinates;
 * here everything is shifted by 4 (the api slot) and made absolute.
 * ================================================================== */
#define IMG_MAX (256 * 1024)      /* CE_MAX_IMAGE */
static int      g_imglen;         /* total .ce file size, header included */
static uint8_t  g_hdr[20];

/* little-endian store into a byte buffer (no alignment requirement) */
static void put32le(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

/* patch a dword inside g_ob (code buffer) */
static void code_patch(int at, uint32_t v) { ob_patch((uint32_t)at, v); }

/* the api slot address equals CE_LOAD_BASE + code + data (see fixup_code) */
static uint32_t g_api_slot_addr_code;

static void fixup_code(void) {
    /* The CE file is a 20-byte header followed by the raw body (code, data,
     * 4-byte api slot).  The kernel loads the BODY at CE_LOAD_BASE, so all
     * absolute addresses equal base + body offsets (no header shift).
     * rel32 jumps/calls inside the code buffer need no adjustment. */
    g_api_slot_addr_code = CE_LOAD_BASE + (uint32_t)g_olen + (uint32_t)g_dlen;
    uint32_t base = CE_LOAD_BASE;
    /* data relocations: mov eax, imm32 -> absolute address of data */
    for (int i = 0; i < g_nrel; i++)
        code_patch(g_rel_at[i], base + g_olen + (uint32_t)g_rel_off[i]);
    for (int i = 0; i < g_ncall; i++) {
        sym_t *f = &g_sym[g_call_sym[i]];
        int site = g_call_at[i];
        int rel = f->addr - (site + 4);
        code_patch(site, (uint32_t)rel);
    }
    /* api slot loads: absolute address of the slot itself */
    for (int i = 0; i < g_napiref; i++)
        code_patch(g_apiref[i], (uint32_t)g_api_slot_addr_code);
    /* data-to-data relocations: global pointer slots -> absolute address
     * of their string in the data section (base + code + data offset) */
    for (int i = 0; i < g_drel_n; i++) {
        uint32_t v = base + g_olen + (uint32_t)g_drel_off[i];
        uint32_t a = (uint32_t)g_drel_at[i];
        g_data[a]     = (uint8_t)v;
        g_data[a + 1] = (uint8_t)(v >> 8);
        g_data[a + 2] = (uint8_t)(v >> 16);
        g_data[a + 3] = (uint8_t)(v >> 24);
    }
    /* entry stub: jmp ce_main (rel32 from the stub's next instruction) */
    if (g_entry_jmp >= 0)
        code_patch(g_entry_jmp, (uint32_t)(g_entry_off - (g_entry_jmp + 4)));
}



static int build_image(catos_api_t *api) {
    /* header (20 bytes): magic, entry, bss, api slot offset, image size */
    uint32_t img = (uint32_t)g_olen + (uint32_t)g_dlen + 4;   /* body + slot */
    g_imglen = 20 + (int)img;
    if (img > IMG_MAX) { (void)api; err_set("image too large"); return 0; }
    g_hdr[0] = (uint8_t)'C'; g_hdr[1] = (uint8_t)'E';
    g_hdr[2] = (uint8_t)'0'; g_hdr[3] = (uint8_t)'1';
    put32le(&g_hdr[4],  0);                    /* entry: stub at body offset 0 */
    put32le(&g_hdr[8],  0);                    /* bss (none)            */
    put32le(&g_hdr[12], (uint32_t)(g_olen + g_dlen));  /* api slot off   */
    put32le(&g_hdr[16], img);                  /* image size            */
    return 1;
}

/* Stream the .ce file to the volume: header, code, data, api slot.
 * api->file_write caps a single call at 16 KiB, so each piece is written
 * in chunks; mode 0 truncates on the first call and 1 appends after.
 * Writing straight from the code/data buffers avoids materialising a
 * second full copy of the image (see the memory-budget note above). */
static int save_image(catos_api_t *api, const char *out) {
    int mode = 0;                      /* first chunk truncates the file */
    int total = 0;

    /* 1. the 20-byte header */
    if (api->file_write(out, (const char *)g_hdr, 20, mode) < 0) goto werr;
    mode = 1; total += 20;

    /* 2. code */
    for (uint32_t off = 0; off < g_olen; ) {
        uint32_t n = g_olen - off;
        if (n > 16384u) n = 16384u;
        if (api->file_write(out, (const char *)(g_ob + off), (int)n, mode) < 0) goto werr;
        off += n; total += (int)n;
    }

    /* 3. data */
    for (uint32_t off = 0; off < g_dlen; ) {
        uint32_t n = g_dlen - off;
        if (n > 16384u) n = 16384u;
        if (api->file_write(out, (const char *)(g_data + off), (int)n, mode) < 0) goto werr;
        off += n; total += (int)n;
    }

    /* 4. the __ce_api slot (kernel fills it at load time) */
    {
        uint8_t zero[4] = { 0, 0, 0, 0 };
        if (api->file_write(out, (const char *)zero, 4, mode) < 0) goto werr;
        total += 4;
    }

    if (total != g_imglen) { api->write("cc: short write\n"); return 0; }
    return 1;

werr:
    api->write("cc: write failed on "); api->write(out); api->write("\n");
    return 0;
}

/* ====================================================================
 * entry
 * ================================================================== */
/* ce_main must sit at image offset 0 (see ce.ld / build_ce.sh) */
#ifdef CC_HOST
__attribute__((section(".text.ce_entry")))
#endif
int ce_main(int argc, char **argv) {
    init_tables();
    /* subset init: globals cannot be initialised from other globals */
    g_ob = g_ob_static;
    g_src = g_src_static;
    g_tok = g_tok_static;
    g_data = g_data_static;
    g_sym = g_sym_static;
    g_st = g_st_static;
    g_rel_at = g_rel_static; g_rel_off = g_rel_static + 32;
    g_call_at = g_call_static; g_call_sym = g_call_static + 32;
    catos_api_t *api = __ce_api;

    api->write("CatOS cc -- tiny C compiler (target one)\n");
    api->write("--------------------------------------\n");
    if (argc < 3) {
        api->write("usage: run /bin/cc.ce SRC.C OUT.CE\n");
        return 1;
    }

    /* Take the working sets from the heap: the CE image+bss window is only
     * 512 KiB (USER_IMAGE_BASE..USER_VSYSCALL) and the code must fit too,
     * while api->alloc() draws from a separate 512 KiB window.
     *
     * The `fallback_cap` argument is the real capacity of the static
     * buffer in bytes; cc_alloc() writes it to *cap when the heap
     * request fails so the derived element count stays meaningful.
     * The eight requests total 520,192 B, which fits the 524,280 B
     * arena payload of the 512 KiB heap with 4,088 B to spare. */
    g_ob  = (uint8_t *)cc_alloc(api, OB_MAX,  g_ob_static,
                                 (uint32_t)sizeof(g_ob_static),  &g_ob_cap);
    g_src = (char *)   cc_alloc(api, SRC_MAX, (void *)g_src_static,
                                 (uint32_t)sizeof(g_src_static), &g_src_cap);
    {
        uint32_t cap = 0;
        token_t *t = (token_t *)cc_alloc(api, (uint32_t)TOK_HEAP * sizeof(token_t),
                                         g_tok_static,
                                         (uint32_t)sizeof(g_tok_static), &cap);
        g_tok = t; g_tok_cap = cap / (int)sizeof(token_t);
    }
    g_data = (uint8_t *)cc_alloc(api, DATA_MAX, g_data_static,
                                 (uint32_t)sizeof(g_data_static), &g_data_cap);
    {
        uint32_t cap = 0;
        sym_t *t = (sym_t *)cc_alloc(api, (uint32_t)SYM_MAX * sizeof(sym_t),
                                     g_sym_static,
                                     (uint32_t)sizeof(g_sym_static), &cap);
        g_sym = t; g_sym_cap = cap / (uint32_t)sizeof(sym_t);
    }
    {
        uint32_t cap = 0;
        stdef_t *t = (stdef_t *)cc_alloc(api, (uint32_t)STRUCT_MAX * sizeof(stdef_t),
                                         g_st_static,
                                         (uint32_t)sizeof(g_st_static), &cap);
        g_st = t; g_st_cap = cap / (uint32_t)sizeof(stdef_t);
    }
    {
        uint32_t cap = 0;
        int *t = (int *)cc_alloc(api, 2u * REL_MAX * sizeof(int), g_rel_static,
                                 (uint32_t)sizeof(g_rel_static), &cap);
        g_rel_cap = cap / (2u * (uint32_t)sizeof(int));
        g_rel_at = t; g_rel_off = t + g_rel_cap;
    }
    {
        uint32_t cap = 0;
        int *t = (int *)cc_alloc(api, 2u * CALLMAX * sizeof(int), g_call_static,
                                 (uint32_t)sizeof(g_call_static), &cap);
        g_call_cap = cap / (2u * (uint32_t)sizeof(int));
        g_call_at = t; g_call_sym = t + g_call_cap;
    }
    api->write("heap: code "); api->print_int((int)g_ob_cap);
    api->write(" src "); api->print_int((int)g_src_cap);
    api->write(" tok "); api->print_int(g_tok_cap);
    api->write(" data "); api->print_int((int)g_data_cap);
    api->write(" sym "); api->print_int((int)g_sym_cap);
    api->write(" st "); api->print_int((int)g_st_cap); api->write("\n");
    api->write("source: "); api->write(argv[1]); api->write("\n");
    /* api->file_read caps one call at 16 KiB, so read the source in
     * chunks until the buffer is full or the file ends. */
    int n = 0;
    for (;;) {
        int want = (int)g_src_cap - 1 - n;
        if (want <= 0) break;
        if (want > 16384) want = 16384;
        int got = api->file_read(argv[1], g_src + n, want, n);
        if (got < 0) { if (n > 0) break; n = got; break; }
        if (got == 0) break;
        n += got;
        if (got < want) break;
    }
    if (n < 0) {
        api->write("cc: cannot read source ("); api->print_int(n); api->write(")\n");
        return 1;
    }
    g_slen = (uint32_t)n;
    g_src[g_slen] = 0;

    if (!tokenize()) {
        api->write("cc: lexer: "); api->write(g_msg); api->write("\n");
        return 1;
    }
    api->write("  "); api->print_int(n); api->write(" bytes, ");
    api->print_int(g_ntok); api->write(" tokens\n");

    /* the api slot occupies image offset 4 (after nothing); code starts at
     * code offset 0 which maps to image offset 4 */
    /* The api slot is stored after the code and the data, so no code
     * byte is clobbered; its absolute address is CE_LOAD_BASE + code + data. */
    /* g_api_slot_addr_code is computed in fixup_code(), once code+data are final */

    parse_program();
    if (g_err) {
        api->write("cc: line "); api->print_int(g_tok[g_ti].line);
        api->write(": "); api->write(g_msg);
        api->write(" [token: ");
        if (g_tok[g_ti].kind == TK_ID) {
            char tmp[32];
            int n = g_tok[g_ti].len; if (n > 30) n = 30;
            for (int q = 0; q < n; q++) tmp[q] = g_tok[g_ti].sp[q];
            tmp[n] = 0;
            api->write("id '");
            api->write(tmp);
            api->write("'");
        } else if (g_tok[g_ti].kind == TK_PUNCT) {
            api->write("punct ");
            api->write(g_punct[g_tok[g_ti].op]);
        } else {
            api->write("kind ");
            api->print_int(g_tok[g_ti].kind);
        }
        api->write("]\n");
        return 1;
    }

    fixup_code();

    api->write("  code "); api->print_int((int)g_olen);
    api->write(" B, data "); api->print_int((int)g_dlen);
    api->write(" B\n");

    if (!build_image(api)) {
        api->write("cc: "); api->write(g_msg); api->write("\n");
        return 1;
    }
    api->write("  image "); api->print_int(g_imglen); api->write(" bytes\n");

    if (!save_image(api, argv[2])) return 1;

    api->write("OK -> "); api->write(argv[2]); api->write("\n");
    return 0;
}
