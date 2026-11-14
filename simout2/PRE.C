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
    TOK_MAX = 25000, SYM_MAX = 256, LOC_MAX = 32, REL_MAX = 512,
    CALLMAX = 256, APIREF_MAX = 256, DREL_MAX = 64, STRUCT_MAX = 16,
    SMEM_MAX = 48, TAG_MAX = 24, TALIAS_MAX = 24, CONST_MAX = 96,
    BRK_MAX = 64, LBL_MAX = 48, IMG_MAX = 262144
};

/* ---- runtime-initialised tables (brace init cannot take strings) ---- */
static char *g_punct[46];
static char *g_apin[48];

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
    while (n-- > 0 && *s) *d++ = *s++;
    *d = 0;
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
 * ================================================================== */
/* Heap working-set sizes.  USER_HEAP_MAX is 256 KiB total, shared
 * with anything else the compiler allocates:
 *     code 128K + source 64K + tokens 56K ~= 248K. */
/* kernel loads the CE image at this base (include/ce.h CE_LOAD_BASE);
 * cc.c references it for absolute fixups so define it locally to stay
 * compilable against minimal headers. */
static uint8_t  g_ob_static[8192];
static uint32_t g_ob_cap = 8192;
static uint8_t *g_ob;             /* set to the fallback in ce_main */

static void *cc_alloc(catos_api_t *api, uint32_t n, void *fallback, uint32_t *cap) {
    void *p = api->alloc(n);
    if (p) { *cap = n; return p; }
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
#define TOK_MAX (25000)       /* self-compile of cc_self.c needs ~24.2K */
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
#define SYM_MAX 256

enum { SY_FUNC = 1, SY_GVAR, SY_LVAR };

typedef struct {
    char name[28];
    int  kind, off, sz, is_ptr, is_arr, arr_sz;
    int  addr, nparams, stidx;   /* stidx: index into g_st when typed over a struct */
} sym_t;

static sym_t g_sym[SYM_MAX];
static int   g_nsym;
static sym_t *g_loc[LOC_MAX];
static int    g_nloc;
static int    g_frame;

/* data section (strings + globals), copied in after code */
static uint8_t  g_data[8192];
static uint32_t g_dlen;

/* relocations in the code: 32-bit immediates holding a data offset */
#define REL_MAX 512           /* init_tables alone carries 93 strings */
static int g_rel_at[REL_MAX];
static int g_rel_off[REL_MAX];
static int g_nrel;

/* call sites: absolute address of the callee resolved at the end */
#define CALLMAX 256
static int    g_call_at[CALLMAX];
static int    g_call_sym[CALLMAX];
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
    if (g_nsym >= SYM_MAX) { err_set("too many symbols"); return 0; }
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
static stdef_t g_st[STRUCT_MAX];
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

int ce_main() { return 0; }
