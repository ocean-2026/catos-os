/*
 * hive.c -- CatOS binary configuration hive (v2.67).
 * See include/hive.h for the on-disk format and the security position.
 */
#include "hive.h"
#include "fs.h"

/* ---- tiny local string helpers ---------------------------------------- */
static int h_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void h_cpy(char *d, const char *s, int cap) {
    int i = 0;
    if (!d || cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int h_append(char *d, int cap, const char *s) {
    int o = h_len(d);
    if (o >= cap - 1) return -1;
    while (s && *s) {
        if (o >= cap - 1) return -1;
        d[o++] = *s++;
    }
    d[o] = 0;
    return 0;
}
/* case-insensitive, ASCII-only, treats '\' == '/' */
static int h_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ---- node table ------------------------------------------------------- */
typedef struct {
    int16_t  parent, child, sib;      /* 0xFFFF = none */
    uint8_t  kind;                    /* HIVE_KEY..HIVE_BIN, 0xFF = free */
    uint8_t  namelen;
    uint16_t datalen;
    char     name[HIVE_NAME_MAX];
    uint8_t  data[HIVE_DATA_MAX];
} hnode_t;

#define NODE_NONE  ((int16_t)-1)
#define NODE_FREE  0xFFu
#define NODE_BYTES 122

static hnode_t  g_nodes[HIVE_MAX_NODES];
static uint32_t g_slots;          /* high-water mark of allocated slots   */
static uint32_t g_gen;            /* generation, from disk or bumped      */
static int      g_dirty, g_loaded, g_migrated, g_corrupt;
static int      g_user_uid;       /* HKCU re-rooting target               */
static uint32_t g_payload_crc;

/* serialization / read-back buffers: deliberately separate statics so a
 * caller can never alias "what we are about to write" and "what came back". */
static char g_ser[HIVE_MAX_NODES * NODE_BYTES + 64];
static char g_raw[HIVE_MAX_NODES * NODE_BYTES + 64];

static int16_t find_child(int16_t key, const char *name) {
    if (key < 0 || key >= (int16_t)g_slots) return NODE_NONE;
    for (int16_t c = g_nodes[key].child; c != NODE_NONE; c = g_nodes[c].sib) {
        if (c >= (int16_t)g_slots) return NODE_NONE;          /* corrupt link */
        if (g_nodes[c].kind == NODE_FREE) return NODE_NONE;
        if (h_eq(g_nodes[c].name, name)) return c;
    }
    return NODE_NONE;
}

static int16_t new_node(int16_t parent, uint8_t kind, const char *name) {
    if (g_slots >= HIVE_MAX_NODES) return NODE_NONE;
    int nlen = h_len(name);
    if (nlen <= 0 || nlen >= HIVE_NAME_MAX) return NODE_NONE;
    int16_t i = (int16_t)g_slots++;
    g_nodes[i].parent = parent;
    g_nodes[i].child = NODE_NONE;
    g_nodes[i].sib = NODE_NONE;
    g_nodes[i].kind = kind;
    g_nodes[i].namelen = (uint8_t)nlen;
    g_nodes[i].datalen = 0;
    h_cpy(g_nodes[i].name, name, HIVE_NAME_MAX);
    for (int k = 0; k < HIVE_DATA_MAX; k++) g_nodes[i].data[k] = 0;
    /* append to the parent's child list (insertion order is preserved) */
    if (parent >= 0) {
        if (g_nodes[parent].child == NODE_NONE) {
            g_nodes[parent].child = i;
        } else {
            int16_t c = g_nodes[parent].child;
            int16_t guard = 0;
            while (g_nodes[c].sib != NODE_NONE && guard++ < (int16_t)HIVE_MAX_NODES) c = g_nodes[c].sib;
            if (g_nodes[c].sib != NODE_NONE) return NODE_NONE; /* cycle guard */
            g_nodes[c].sib = i;
        }
    }
    return i;
}

static int16_t find_root(const char *name) {
    for (int16_t i = 0; i < (int16_t)g_slots; i++) {
        if (g_nodes[i].kind != HIVE_KEY) continue;
        if (g_nodes[i].parent != NODE_NONE) continue;
        if (h_eq(g_nodes[i].name, name)) return i;
    }
    return NODE_NONE;
}

/* Split a rooted path into components.  HKCU is rewritten to HKU\<uid>.
 * Returns component count, or -1 on a malformed path (empty component,
 * '.'/'..', component too long, no root, too deep). */
static int path_split(const char *path, char comp[][HIVE_NAME_MAX], int *out_n) {
    char buf[HIVE_PATH_MAX];
    if (!path || !*path) return -1;
    if (h_len(path) >= HIVE_PATH_MAX) return -1;
    h_cpy(buf, path, HIVE_PATH_MAX);

    int n = 0;
    int i = 0;
    /* leading separators are tolerated */
    while (buf[i] == '\\' || buf[i] == '/') i++;
    char cur[HIVE_NAME_MAX];
    int cl = 0;
    for (;;) {
        char c = buf[i];
        if (c == '\\' || c == '/' || c == 0) {
            if (cl == 0) return -1;                   /* empty component */
            if (cl >= HIVE_NAME_MAX) return -1;
            /* v2.67 BUGFIX: cur was never NUL-terminated, so h_cpy() copied
             * the previous component's tail into the new one -- every root
             * after the first came out as "HKLM<garbage>" and the tree
             * self-destructed during seeding. */
            cur[cl] = 0;
            if ((cl == 1 && cur[0] == '.') ||
                (cl == 2 && cur[0] == '.' && cur[1] == '.')) return -1;
            if (n >= HIVE_MAX_DEPTH) return -1;
            h_cpy(comp[n], cur, HIVE_NAME_MAX);
            n++;
            cl = 0;
            if (c == 0) break;
            i++;
            /* collapse repeated separators */
            while (buf[i] == '\\' || buf[i] == '/') i++;
            if (buf[i] == 0) break;
            continue;
        }
        if (cl >= HIVE_NAME_MAX - 1) return -1;
        cur[cl++] = c;
        i++;
    }
    /* HKCU -> HKU\<uid> */
    if (h_eq(comp[0], "HKCU")) {
        if (n >= HIVE_MAX_DEPTH) return -1;
        for (int k = n; k > 0; k--) h_cpy(comp[k], comp[k - 1], HIVE_NAME_MAX);
        h_cpy(comp[0], "HKU", HIVE_NAME_MAX);
        /* decimal uid */
        char uid[12];
        uint32_t v = (uint32_t)g_user_uid;
        int d = 0;
        if (v == 0) { uid[d++] = '0'; }
        while (v) { uid[d++] = (char)('0' + (v % 10)); v /= 10; }
        for (int k = 0; k < d / 2; k++) { char t = uid[k]; uid[k] = uid[d - 1 - k]; uid[d - 1 - k] = t; }
        uid[d] = 0;
        h_cpy(comp[1], uid, HIVE_NAME_MAX);
        n++;
        if (n > HIVE_MAX_DEPTH) return -1;
    }
    *out_n = n;
    return n;
}

/* Resolve a path to a node.  `create` != 0 auto-creates missing KEY
 * components (used by set/mkkey); values are only ever the last component. */
/* v2.67 BUGFIX: resolve() used to return HIVE_ENOTDIR (-7) on a
 * value-as-key path.  Every caller treated "< NODE_NONE" results as a
 * valid node index, so g_nodes[-7] was dereferenced and the table silently
 * corrupted itself during seeding.  Failures now ALWAYS return NODE_NONE
 * and leave the reason in g_resolve_err, which callers translate. */
static int g_resolve_err = HIVE_EINVAL;

static int16_t resolve(const char *path, int create) {
    char comp[HIVE_MAX_DEPTH][HIVE_NAME_MAX];
    int n = 0;
    g_resolve_err = HIVE_EINVAL;
    if (path_split(path, comp, &n) < 0 || n < 1) return NODE_NONE;

    int16_t cur = find_root(comp[0]);
    if (cur == NODE_NONE) {
        if (!create) { g_resolve_err = HIVE_ENOENT; return NODE_NONE; }
        cur = new_node(NODE_NONE, HIVE_KEY, comp[0]);
        if (cur == NODE_NONE) { g_resolve_err = HIVE_EFULL; return NODE_NONE; }
    }
    for (int i = 1; i < n; i++) {
        int16_t c = find_child(cur, comp[i]);
        if (c == NODE_NONE) {
            if (!create) { g_resolve_err = HIVE_ENOENT; return NODE_NONE; }
            /* intermediate components are keys by construction; the final
             * one is created as a key too and re-typed by the setter. */
            int16_t k = new_node(cur, HIVE_KEY, comp[i]);
            if (k == NODE_NONE) { g_resolve_err = HIVE_EFULL; return NODE_NONE; }
            cur = k;
            continue;
        }
        if (i < n - 1 && g_nodes[c].kind != HIVE_KEY) {
            g_resolve_err = HIVE_ENOTDIR;
            return NODE_NONE;
        }
        cur = c;
    }
    return cur;
}

/* Return the parent key of a path (creating it), plus the leaf name. */
static int16_t resolve_parent(const char *path, char *leaf, int leafcap, int create) {
    char comp[HIVE_MAX_DEPTH][HIVE_NAME_MAX];
    int n = 0;
    if (path_split(path, comp, &n) < 0 || n < 2) return NODE_NONE;
    h_cpy(leaf, comp[n - 1], leafcap);
    /* rebuild the parent path */
    char parent[HIVE_PATH_MAX];
    parent[0] = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i) h_append(parent, HIVE_PATH_MAX, "\\");
        h_append(parent, HIVE_PATH_MAX, comp[i]);
    }
    return resolve(parent, create);
}

/* ---- CRC-32 (IEEE 802.3, reflected, no table) ------------------------- */
static uint32_t crc32_buf(const char *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint32_t)(uint8_t)p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* ---- compaction: renumber so the file has no holes -------------------- */
static hnode_t g_tmp[HIVE_MAX_NODES];

static int16_t compact_one(int16_t src, int16_t new_parent, int16_t *slots) {
    if (*slots >= HIVE_MAX_NODES) return NODE_NONE;
    int16_t dst = *slots;
    g_tmp[dst] = g_nodes[src];
    g_tmp[dst].parent = new_parent;
    g_tmp[dst].child = NODE_NONE;
    g_tmp[dst].sib = NODE_NONE;
    (*slots)++;
    /* children in insertion order */
    int16_t last = NODE_NONE;
    for (int16_t c = g_nodes[src].child; c != NODE_NONE; c = g_nodes[c].sib) {
        if (c >= (int16_t)g_slots || g_nodes[c].kind == NODE_FREE) break;
        int16_t nc = compact_one(c, dst, slots);
        if (nc == NODE_NONE) break;
        if (last == NODE_NONE) g_tmp[dst].child = nc;
        else                   g_tmp[last].sib = nc;
        last = nc;
    }
    return dst;
}

static void compact(void) {
    int16_t slots = 0;
    for (int16_t i = 0; i < (int16_t)g_slots; i++) {
        if (g_nodes[i].kind != HIVE_KEY || g_nodes[i].parent != NODE_NONE) continue;
        compact_one(i, NODE_NONE, &slots);
    }
    for (int i = 0; i < (int)slots; i++) g_nodes[i] = g_tmp[i];
    g_slots = (uint32_t)slots;
}

/* ---- serialize / parse ------------------------------------------------ */
static uint32_t ser_nodes(char *out) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < g_slots; i++) {
        hnode_t *n = &g_nodes[i];
        *(uint16_t *)(out + o) = (uint16_t)n->parent; o += 2;
        *(uint16_t *)(out + o) = (uint16_t)n->child;  o += 2;
        *(uint16_t *)(out + o) = (uint16_t)n->sib;    o += 2;
        out[o++] = n->kind;
        out[o++] = n->namelen;
        *(uint16_t *)(out + o) = n->datalen; o += 2;
        for (int k = 0; k < HIVE_NAME_MAX; k++) out[o++] = n->name[k];
        for (int k = 0; k < HIVE_DATA_MAX; k++) out[o++] = (char)n->data[k];
    }
    return o;
}

/* Validate a parsed tree: links in range, parent backlinks consistent,
 * no child cycles reachable.  Returns 0 sane / -1 corrupt. */
int hive_verify(void) {
    if (g_slots == 0) return 0;
    if (g_slots > HIVE_MAX_NODES) return -1;
    int roots = 0;
    for (uint32_t i = 0; i < g_slots; i++) {
        hnode_t *n = &g_nodes[i];
        if (n->kind == NODE_FREE) return -1;
        if (n->kind > HIVE_BIN) return -1;
        if (n->namelen == 0 || n->namelen >= HIVE_NAME_MAX) return -1;
        if (n->datalen > HIVE_DATA_MAX) return -1;
        if ((uint16_t)n->namelen != (uint16_t)h_len(n->name)) return -1;
        if (n->parent != NODE_NONE) {
            if (n->parent >= (int16_t)g_slots) return -1;
            if (g_nodes[n->parent].kind != HIVE_KEY) return -1;
        }
        if (n->child != NODE_NONE && n->child >= (int16_t)g_slots) return -1;
        if (n->sib != NODE_NONE && n->sib >= (int16_t)g_slots) return -1;
        if (n->parent == NODE_NONE) roots++;
    }
    if (roots < 1) return -1;
    /* walk every node's child chain with a bounded guard */
    for (uint32_t i = 0; i < g_slots; i++) {
        int guard = 0;
        for (int16_t c = g_nodes[i].child; c != NODE_NONE; c = g_nodes[c].sib) {
            if (++guard > (int)HIVE_MAX_NODES) return -1;
            if (g_nodes[c].parent != (int16_t)i) return -1;
        }
    }
    return 0;
}

/* Parse a serialized node area into g_nodes.  Every field is re-validated
 * by hive_verify() afterwards. */
static int parse_nodes(const char *in, uint32_t bytes, uint32_t count) {
    if (count == 0 || count > HIVE_MAX_NODES) return -1;
    if (bytes != count * NODE_BYTES) return -1;
    uint32_t o = 0;
    for (uint32_t i = 0; i < count; i++) {
        hnode_t *n = &g_nodes[i];
        n->parent = (int16_t)*(const uint16_t *)(in + o); o += 2;
        n->child  = (int16_t)*(const uint16_t *)(in + o); o += 2;
        n->sib    = (int16_t)*(const uint16_t *)(in + o); o += 2;
        n->kind   = (uint8_t)in[o++];
        n->namelen= (uint8_t)in[o++];
        n->datalen= *(const uint16_t *)(in + o); o += 2;
        for (int k = 0; k < HIVE_NAME_MAX; k++) n->name[k] = in[o++];
        for (int k = 0; k < HIVE_DATA_MAX; k++) n->data[k] = (uint8_t)in[o++];
    }
    g_slots = count;
    return 0;
}

/* ---- defaults --------------------------------------------------------- */
static void seed_defaults(void) {
    /* machine roots */
    if (find_root("HKLM") == NODE_NONE) new_node(NODE_NONE, HIVE_KEY, "HKLM");
    if (find_root("HKU")  == NODE_NONE) new_node(NODE_NONE, HIVE_KEY, "HKU");
    if (find_root("HKSEC")== NODE_NONE) new_node(NODE_NONE, HIVE_KEY, "HKSEC");

    hive_set_sz("HKLM\\System\\Language", "en");
    hive_set_sz("HKLM\\System\\VideoMode", "gfx24");
    hive_set_sz("HKLM\\System\\Crash\\Mode", "reboot");
    hive_set_dw("HKLM\\System\\Crash\\TimeoutSec", 30);
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Theme", "dark");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\FontSize", "1");
    hive_set_dw("HKLM\\Software\\CatOS\\Terminal\\Cursor\\Blink", 1);
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Cursor\\Style", "underline");
    hive_set_dw("HKLM\\Software\\CatOS\\Terminal\\ScrollbackRows", 240);
    hive_set_dw("HKLM\\Software\\CatOS\\Terminal\\Pager\\Enabled", 1);
    hive_set_dw("HKLM\\Software\\CatOS\\Terminal\\Pager\\PageRows", 24);
    hive_set_dw("HKLM\\Software\\CatOS\\Editor\\TabWidth", 4);
    hive_set_sz("HKLM\\Environment\\PATH", "/bin");
    hive_set_sz("HKLM\\Environment\\PS1", "");
    hive_set_dw("HKLM\\Environment\\HistoryMax", 128);

    /* terminal colour roles -- the v2.37 modern theme, now data */
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Background",  "#0C0C0C");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Foreground",  "#DCDCDC");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\PromptUser",  "#7EE787");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\PromptPath",  "#60A5FA");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\PromptSymbol","#7EE787");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Command",     "#FFFFFF");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Argument",    "#DCDCDC");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Error",       "#F85149");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Warning",     "#E3B341");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Success",     "#56D364");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Info",        "#39C5CF");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Dim",         "#8B949E");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Accent",      "#D299DD");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\Selection",   "#264F78");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\StatusBg",    "#161B22");
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Colors\\StatusFg",    "#8B949E");

    /* crash screen -- separate roles so a BSOD never inherits the palette */
    hive_set_sz("HKLM\\System\\Crash\\Colors\\Background", "#0A1A3A");
    hive_set_sz("HKLM\\System\\Crash\\Colors\\Foreground", "#DCE6F2");
    hive_set_sz("HKLM\\System\\Crash\\Colors\\Title",      "#FFFFFF");
    hive_set_sz("HKLM\\System\\Crash\\Colors\\Hint",       "#8FB8FF");
    hive_set_sz("HKLM\\System\\Crash\\Colors\\Code",       "#FFD866");

    /* security policy defaults (all editable, all enforced by sec.c) */
    hive_set_dw("HKSEC\\Policy\\UnknownBinary\\AllowFromWritableDir", 0);
    hive_set_dw("HKSEC\\Policy\\UnknownBinary\\AllowUnsigned", 1);
    hive_set_dw("HKSEC\\Policy\\Passwd\\MinLength", 6);
    hive_set_dw("HKSEC\\Policy\\Passwd\\MaxAttempts", 5);
    hive_set_dw("HKSEC\\Policy\\Audit\\TrackExec", 1);
    hive_set_dw("HKSEC\\Policy\\Foreground\\AllowCtrlC", 1);
    hive_set_dw("HKSEC\\Policy\\Foreground\\AllowCtrlZ", 1);
    /* empty string = "use the built-in table"; a set path upgrades the
     * lookup for that command without a rebuild. */
    hive_set_sz("HKSEC\\Policy\\Cmd\\format\\Caps", "FSADMIN");
    hive_set_sz("HKSEC\\Policy\\Cmd\\reboot\\Caps", "SYS|REBOOT");
    hive_set_sz("HKSEC\\Policy\\Cmd\\shutdown\\Caps", "SYS");
    hive_set_sz("HKSEC\\Policy\\Cmd\\su\\Caps", "USERADM");
    hive_set_sz("HKSEC\\Policy\\Cmd\\useradd\\Caps", "USERADM");
    hive_set_sz("HKSEC\\Policy\\Cmd\\userdel\\Caps", "USERADM");
    hive_set_sz("HKSEC\\Policy\\Cmd\\passwd\\Caps", "USERADM");
    hive_set_sz("HKSEC\\Policy\\Cmd\\chcaps\\Caps", "USERADM");
    hive_set_sz("HKSEC\\Policy\\Cmd\\kill\\Caps", "KILL");
    hive_set_sz("HKSEC\\Policy\\Cmd\\curl\\Caps", "NET");
    hive_set_sz("HKSEC\\Policy\\Cmd\\ping\\Caps", "NET");
    hive_set_sz("HKSEC\\Policy\\Cmd\\dns\\Caps", "NET");
    hive_set_sz("HKSEC\\Policy\\Cmd\\udp\\Caps", "NET");
    hive_set_sz("HKSEC\\Policy\\Cmd\\netcfg\\Caps", "NET");
}

/* ---- load ------------------------------------------------------------- */
static int load_blob(const char *p, int *out_len) {
    int n = fs_read_text(p, g_raw, (int)sizeof(g_raw));
    if (n <= 0) return -1;
    *out_len = n;
    return 0;
}

int hive_load(void) {
    int len = 0;
    for (int i = 0; i < (int)HIVE_MAX_NODES; i++) g_nodes[i].kind = NODE_FREE;
    g_slots = 0; g_loaded = 0; g_corrupt = 0; g_dirty = 0;

    if (load_blob(HIVE_PATH, &len) != 0) return HIVE_ENOENT;
    if (len < 32) { g_corrupt = 1; return HIVE_ECORRUPT; }

    const char *p = g_raw;
    for (int i = 0; i < 8; i++)
        if (p[i] != HIVE_MAGIC[i]) { g_corrupt = 1; return HIVE_ECORRUPT; }
    uint32_t ver   = *(const uint32_t *)(p + 8);
    uint32_t gen   = *(const uint32_t *)(p + 12);
    uint32_t count = *(const uint32_t *)(p + 16);
    uint32_t plen  = *(const uint32_t *)(p + 20);
    uint32_t crc   = *(const uint32_t *)(p + 24);
    uint32_t hcrc  = *(const uint32_t *)(p + 28);

    if (ver != HIVE_VERSION) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if (hcrc != crc32_buf(p, 28)) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if (count == 0 || count > HIVE_MAX_NODES) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if (plen != count * NODE_BYTES) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if ((uint32_t)len < 32 + plen) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if (crc != crc32_buf(p + 32, plen)) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if (parse_nodes(p + 32, plen, count) != 0) { g_corrupt = 1; return HIVE_ECORRUPT; }
    if (hive_verify() != 0) { g_corrupt = 1; return HIVE_ECORRUPT; }

    g_gen = gen;
    g_payload_crc = crc;
    g_loaded = 1;
    return HIVE_OK;
}

/* v2.67 BUGFIX: `hive chk` used to call hive_load() to "re-read" the volume.
 * hive_load() clears g_nodes first, so when the re-read failed (no volume,
 * or a truncated image) the RUNNING registry was left empty -- chk reported
 * FAILED and every later hive get/ls in that shell session claimed the tree
 * had no keys.  Validate the image on a copy instead and never touch
 * g_nodes: the payload is byte-compared against the live serialization. */
int hive_check_image(int *out_len) {
    if (out_len) *out_len = 0;
    int len = 0;
    if (load_blob(HIVE_PATH, &len) != 0) return HIVE_ENOENT;
    if (len < 32) return HIVE_ECORRUPT;
    const char *p = g_raw;
    for (int i = 0; i < 8; i++)
        if (p[i] != HIVE_MAGIC[i]) return HIVE_ECORRUPT;
    uint32_t ver   = *(const uint32_t *)(p + 8);
    uint32_t count = *(const uint32_t *)(p + 16);
    uint32_t plen  = *(const uint32_t *)(p + 20);
    uint32_t crc   = *(const uint32_t *)(p + 24);
    uint32_t hcrc  = *(const uint32_t *)(p + 28);
    if (ver != HIVE_VERSION) return HIVE_ECORRUPT;
    if (hcrc != crc32_buf(p, 28)) return HIVE_ECORRUPT;
    if (count == 0 || count > HIVE_MAX_NODES) return HIVE_ECORRUPT;
    if (plen != count * NODE_BYTES) return HIVE_ECORRUPT;
    if ((uint32_t)len < 32 + plen) return HIVE_ECORRUPT;
    if (crc != crc32_buf(p + 32, plen)) return HIVE_ECORRUPT;
    if (out_len) *out_len = len;

    /* structural comparison against what we would write right now */
    if (g_slots == 0 || g_slots > HIVE_MAX_NODES) return HIVE_ESTALE;
    uint32_t mylen = ser_nodes(g_ser);
    if (count != g_slots || mylen != plen) return HIVE_ESTALE;
    for (uint32_t i = 0; i < plen; i++)
        if (g_raw[32 + i] != g_ser[i]) return HIVE_ESTALE;
    return HIVE_OK;
}

/* ---- save: write, read back, compare, roll back ------------------------ */
int hive_save(void) {
    if (!fs_fs_ready()) return HIVE_EIO;
    compact();
    if (hive_verify() != 0) return HIVE_ECODE;

    uint32_t plen = ser_nodes(g_ser);
    uint32_t crc  = crc32_buf(g_ser, plen);
    uint32_t gen  = g_gen + 1;

    char *h = g_ser - 32;   /* not valid: use the raw buffer as header space */
    (void)h;

    /* header lives in its own small static so g_ser stays the payload */
    static char hdr[32];
    for (int i = 0; i < 8; i++) hdr[i] = HIVE_MAGIC[i];
    *(uint32_t *)(hdr + 8)  = HIVE_VERSION;
    *(uint32_t *)(hdr + 12) = gen;
    *(uint32_t *)(hdr + 16) = g_slots;
    *(uint32_t *)(hdr + 20) = plen;
    *(uint32_t *)(hdr + 24) = crc;
    *(uint32_t *)(hdr + 28) = crc32_buf(hdr, 28);

    /* keep the previous good image before overwriting it */
    int oldlen = 0;
    if (load_blob(HIVE_PATH, &oldlen) == 0) {
        fs_write_raw_mode(HIVE_BAK, g_raw, (uint32_t)oldlen, 0);   /* best effort */
    }
    /* header and payload are written as one buffer: patch payload into place
     * by writing header + payload sequentially through the raw writer. */
    static char whole[32 + HIVE_MAX_NODES * NODE_BYTES];
    for (int i = 0; i < 32; i++) whole[i] = hdr[i];
    for (uint32_t i = 0; i < plen; i++) whole[32 + i] = g_ser[i];
    if (fs_write_raw_mode(HIVE_PATH, whole, 32 + plen, 0) != 0) return HIVE_EIO;

    /* read-back verification -- trust nothing, including our own writer */
    int back_len = 0;
    if (load_blob(HIVE_PATH, &back_len) != 0) return HIVE_EIO;
    if ((uint32_t)back_len != 32 + plen) goto bad;
    if (*(const uint32_t *)(g_raw + 16) != g_slots) goto bad;
    if (*(const uint32_t *)(g_raw + 20) != plen) goto bad;
    if (*(const uint32_t *)(g_raw + 24) != crc) goto bad;
    if (crc32_buf(g_raw + 32, plen) != crc) goto bad;
    for (uint32_t i = 0; i < plen; i++)
        if (g_raw[32 + i] != g_ser[i]) goto bad;

    g_gen = gen;
    g_payload_crc = crc;
    g_dirty = 0;
    g_loaded = 1;
    return HIVE_OK;

bad:
    /* restore the previous image so a torn write cannot brick the boot */
    int bl = 0;
    if (load_blob(HIVE_BAK, &bl) == 0)
        fs_write_raw_mode(HIVE_PATH, g_raw, (uint32_t)bl, 0);
    return HIVE_ECODE;
}

int hive_dirty(void) { return g_dirty; }

void hive_stat(hive_stat_t *st) {
    if (!st) return;
    uint32_t k = 0, v = 0;
    for (uint32_t i = 0; i < g_slots; i++) {
        if (g_nodes[i].kind == HIVE_KEY) k++; else v++;
    }
    st->nodes = g_slots;
    st->keys = k;
    st->values = v;
    st->generation = g_gen;
    st->bytes = g_slots * NODE_BYTES + 32;
    st->loaded = g_loaded;
    st->migrated = g_migrated;
    st->corrupt = g_corrupt;
    st->crc = g_payload_crc;
}

void hive_set_user(int uid) { g_user_uid = uid < 0 ? 0 : uid; }

/* ---- read API --------------------------------------------------------- */
const char *hive_sz(const char *path, const char *def) {
    static const char empty[] = "";
    int16_t n = resolve(path, 0);
    if (n == NODE_NONE || g_nodes[n].kind == HIVE_KEY) return def ? def : empty;
    if (g_nodes[n].kind != HIVE_SZ) return def ? def : empty;
    g_nodes[n].data[g_nodes[n].datalen < HIVE_DATA_MAX ? g_nodes[n].datalen
                                                       : HIVE_DATA_MAX - 1] = 0;
    return (const char *)g_nodes[n].data;
}

uint32_t hive_dw(const char *path, uint32_t def) {
    int16_t n = resolve(path, 0);
    if (n == NODE_NONE || g_nodes[n].kind != HIVE_DW || g_nodes[n].datalen != 4)
        return def;
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | g_nodes[n].data[i];
    return v;
}

int hive_get(const char *path, hive_val_t *out) {
    if (!out) return HIVE_EINVAL;
    int16_t n = resolve(path, 0);
    if (n == NODE_NONE) return HIVE_ENOENT;
    if (g_nodes[n].kind == HIVE_KEY) return HIVE_ENOTDIR;
    out->kind = g_nodes[n].kind;
    out->len = g_nodes[n].datalen;
    for (int i = 0; i < HIVE_DATA_MAX; i++) out->data[i] = g_nodes[n].data[i];
    return HIVE_OK;
}

int hive_exists(const char *path) { return resolve(path, 0) != NODE_NONE; }
int hive_kind(const char *path) {
    int16_t n = resolve(path, 0);
    return (n == NODE_NONE) ? -1 : (int)g_nodes[n].kind;
}

/* ---- write API -------------------------------------------------------- */
static int16_t obtain_value(const char *path, uint8_t kind) {
    char leaf[HIVE_NAME_MAX];
    int16_t parent = resolve_parent(path, leaf, HIVE_NAME_MAX, 1);
    if (parent == NODE_NONE) return NODE_NONE;      /* reason in g_resolve_err */
    if (g_nodes[parent].kind != HIVE_KEY) {
        g_resolve_err = HIVE_ENOTDIR;
        return NODE_NONE;
    }
    int16_t v = find_child(parent, leaf);
    if (v == NODE_NONE) {
        v = new_node(parent, kind, leaf);
        if (v == NODE_NONE) g_resolve_err = HIVE_EFULL;
        return v;
    }
    if (g_nodes[v].kind == HIVE_KEY) {
        g_resolve_err = HIVE_ENOTDIR;
        return NODE_NONE;
    }
    g_nodes[v].kind = kind;
    return v;
}

int hive_set_sz(const char *path, const char *val) {
    if (!path || !val) return HIVE_EINVAL;
    int len = h_len(val);
    if (len >= HIVE_DATA_MAX) return HIVE_E2BIG;      /* never truncate */
    int16_t v = obtain_value(path, HIVE_SZ);
    if (v == NODE_NONE) return g_resolve_err ? g_resolve_err : HIVE_EFULL;
    g_nodes[v].kind = HIVE_SZ;
    g_nodes[v].datalen = (uint16_t)len;
    for (int i = 0; i < HIVE_DATA_MAX; i++) g_nodes[v].data[i] = 0;
    for (int i = 0; i < len; i++) g_nodes[v].data[i] = (uint8_t)val[i];
    g_dirty = 1;
    return HIVE_OK;
}

int hive_set_dw(const char *path, uint32_t val) {
    int16_t v = obtain_value(path, HIVE_DW);
    if (v == NODE_NONE) return g_resolve_err ? g_resolve_err : HIVE_EFULL;
    g_nodes[v].kind = HIVE_DW;
    g_nodes[v].datalen = 4;
    for (int i = 0; i < 4; i++) g_nodes[v].data[i] = (uint8_t)(val >> (8 * i));
    for (int i = 4; i < HIVE_DATA_MAX; i++) g_nodes[v].data[i] = 0;
    g_dirty = 1;
    return HIVE_OK;
}

int hive_set_bin(const char *path, const uint8_t *data, uint16_t len) {
    if (!path || (!data && len)) return HIVE_EINVAL;
    if (len > HIVE_DATA_MAX) return HIVE_E2BIG;
    int16_t v = obtain_value(path, HIVE_BIN);
    if (v == NODE_NONE) return g_resolve_err ? g_resolve_err : HIVE_EFULL;
    g_nodes[v].kind = HIVE_BIN;
    g_nodes[v].datalen = len;
    for (int i = 0; i < HIVE_DATA_MAX; i++) g_nodes[v].data[i] = 0;
    for (int i = 0; i < (int)len; i++) g_nodes[v].data[i] = data[i];
    g_dirty = 1;
    return HIVE_OK;
}

int hive_set(const char *path, const hive_val_t *v) {
    if (!v) return HIVE_EINVAL;
    switch (v->kind) {
    case HIVE_SZ: {
        char tmp[HIVE_DATA_MAX + 1];
        int n = v->len < HIVE_DATA_MAX ? (int)v->len : HIVE_DATA_MAX - 1;
        for (int i = 0; i < n; i++) tmp[i] = (char)v->data[i];
        tmp[n] = 0;
        return hive_set_sz(path, tmp);
    }
    case HIVE_DW: {
        uint32_t d = 0;
        for (int i = 0; i < 4; i++) d |= (uint32_t)v->data[i] << (8 * i);
        return hive_set_dw(path, d);
    }
    case HIVE_BIN:
        return hive_set_bin(path, v->data, v->len);
    default:
        return HIVE_EINVAL;
    }
}

int hive_mkkey(const char *path) {
    int16_t n = resolve(path, 1);
    if (n == NODE_NONE) return g_resolve_err ? g_resolve_err : HIVE_EFULL;
    return HIVE_OK;
}

static void collect_desc(int16_t root, int16_t *list, int *n, int max) {
    for (int16_t c = g_nodes[root].child; c != NODE_NONE; c = g_nodes[c].sib) {
        if (*n >= max) return;
        list[(*n)++] = c;
        collect_desc(c, list, n, max);
    }
}

static void free_subtree(int16_t n) {
    int16_t kids[HIVE_MAX_NODES];
    int kn = 0;
    collect_desc(n, kids, &kn, (int)HIVE_MAX_NODES);
    for (int i = 0; i < kn; i++) g_nodes[kids[i]].kind = NODE_FREE;
    g_nodes[n].kind = NODE_FREE;
}

static int unlink_node(int16_t n) {
    int16_t p = g_nodes[n].parent;
    if (p == NODE_NONE) return -1;
    if (g_nodes[p].child == n) {
        g_nodes[p].child = g_nodes[n].sib;
    } else {
        int16_t c = g_nodes[p].child, guard = 0;
        while (c != NODE_NONE && g_nodes[c].sib != n && guard++ < (int16_t)HIVE_MAX_NODES)
            c = g_nodes[c].sib;
        if (c == NODE_NONE || g_nodes[c].sib != n) return -1;
        g_nodes[c].sib = g_nodes[n].sib;
    }
    g_nodes[n].parent = NODE_NONE;
    g_nodes[n].sib = NODE_NONE;
    return 0;
}

int hive_del(const char *path) {
    int16_t n = resolve(path, 0);
    if (n == NODE_NONE) return HIVE_ENOENT;
    if (g_nodes[n].kind == HIVE_KEY) return HIVE_ENOTDIR;   /* use delkey */
    if (unlink_node(n) != 0) return HIVE_EINVAL;
    g_nodes[n].kind = NODE_FREE;
    g_dirty = 1;
    return HIVE_OK;
}

int hive_delkey(const char *path) {
    int16_t n = resolve(path, 0);
    if (n == NODE_NONE) return HIVE_ENOENT;
    if (g_nodes[n].kind != HIVE_KEY) return HIVE_EINVAL;
    if (g_nodes[n].parent == NODE_NONE) return HIVE_EPERM;  /* never a root */
    if (unlink_node(n) != 0) return HIVE_EINVAL;
    free_subtree(n);
    g_dirty = 1;
    return HIVE_OK;
}

/* ---- enumeration ------------------------------------------------------ */
int hive_child_count(const char *keypath) {
    int16_t n = resolve(keypath, 0);
    if (n == NODE_NONE || g_nodes[n].kind != HIVE_KEY) return 0;
    int c = 0;
    int guard = 0;
    for (int16_t i = g_nodes[n].child; i != NODE_NONE; i = g_nodes[i].sib) {
        if (++guard > (int)HIVE_MAX_NODES) break;
        if (g_nodes[i].kind != NODE_FREE) c++;
    }
    return c;
}

int hive_child_at(const char *keypath, int index, char *name, int namecap, uint8_t *kind) {
    int16_t n = resolve(keypath, 0);
    if (n == NODE_NONE || g_nodes[n].kind != HIVE_KEY) return HIVE_ENOENT;
    if (index < 0) return HIVE_EINVAL;
    int c = 0, guard = 0;
    for (int16_t i = g_nodes[n].child; i != NODE_NONE; i = g_nodes[i].sib) {
        if (++guard > (int)HIVE_MAX_NODES) break;
        if (g_nodes[i].kind == NODE_FREE) continue;
        if (c++ == index) {
            if (name) h_cpy(name, g_nodes[i].name, namecap);
            if (kind) *kind = g_nodes[i].kind;
            return HIVE_OK;
        }
    }
    return HIVE_ENOENT;
}

int hive_child_path(const char *keypath, int index, char *out, int cap) {
    char nm[HIVE_NAME_MAX];
    uint8_t k;
    int r = hive_child_at(keypath, index, nm, HIVE_NAME_MAX, &k);
    if (r != HIVE_OK) return r;
    h_cpy(out, keypath, cap);
    if (h_append(out, cap, "\\") != 0) return HIVE_E2BIG;
    if (h_append(out, cap, nm) != 0) return HIVE_E2BIG;
    return HIVE_OK;
}

/* ---- INI import (legacy /etc/reg + /etc/lang etc) --------------------- */
static int map_legacy(const char *sec, const char *key, char *out, int cap) {
    if (h_eq(sec, "system") && h_eq(key, "lang"))
        { h_cpy(out, "HKLM\\System\\Language", cap); return 0; }
    if (h_eq(sec, "system") && h_eq(key, "theme"))
        { h_cpy(out, "HKLM\\Software\\CatOS\\Terminal\\Theme", cap); return 0; }
    if (h_eq(sec, "system") && h_eq(key, "videomode"))
        { h_cpy(out, "HKLM\\System\\VideoMode", cap); return 0; }
    if (h_eq(sec, "system") && h_eq(key, "panicmode"))
        { h_cpy(out, "HKLM\\System\\Crash\\Mode", cap); return 0; }
    if (h_eq(sec, "system") && h_eq(key, "size"))
        { h_cpy(out, "HKLM\\Software\\CatOS\\Terminal\\FontSize", cap); return 0; }
    if (h_eq(sec, "env"))
        { h_cpy(out, "HKLM\\Environment", cap); h_append(out, cap, "\\");
          h_append(out, cap, key); return 0; }
    h_cpy(out, "HKLM\\Legacy", cap);
    h_append(out, cap, "\\"); h_append(out, cap, sec);
    h_append(out, cap, "\\"); h_append(out, cap, key);
    return 0;
}

int hive_import_ini(const char *a_root, const char *text, int len) {
    char sec[HIVE_NAME_MAX] = "legacy";
    int imported = 0;
    static char line[256];
    int li = 0;
    for (int i = 0; i <= len; i++) {
        char c = (i < len) ? text[i] : '\n';
        if (c != '\n' && c != '\r') {
            if (li < (int)sizeof(line) - 1) line[li++] = c;
            continue;
        }
        if (c == '\r') continue;
        line[li] = 0;
        li = 0;
        int s = 0;
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (!line[s] || line[s] == '#' || line[s] == ';') continue;
        if (line[s] == '[') {
            char *p = line + s + 1;
            int k = 0;
            while (*p && *p != ']' && k < HIVE_NAME_MAX - 1) sec[k++] = *p++;
            sec[k] = 0;
            continue;
        }
        char *eq = line + s;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        char kbuf[HIVE_NAME_MAX];
        char vbuf[HIVE_DATA_MAX];
        char *kp = line + s;
        while (*kp == ' ' || *kp == '\t') kp++;
        int kl = h_len(kp);
        while (kl > 0 && (kp[kl - 1] == ' ' || kp[kl - 1] == '\t')) kp[--kl] = 0;
        if (!kl || kl >= HIVE_NAME_MAX) continue;
        h_cpy(kbuf, kp, HIVE_NAME_MAX);
        char *vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') vp++;
        if (h_len(vp) >= HIVE_DATA_MAX) continue;       /* refuse, don't truncate */
        h_cpy(vbuf, vp, HIVE_DATA_MAX);
        char path[HIVE_PATH_MAX];
        if (a_root && *a_root) {
            h_cpy(path, a_root, HIVE_PATH_MAX);
            h_append(path, HIVE_PATH_MAX, "\\");
            h_append(path, HIVE_PATH_MAX, sec);
            h_append(path, HIVE_PATH_MAX, "\\");
            h_append(path, HIVE_PATH_MAX, kbuf);
        } else {
            map_legacy(sec, kbuf, path, HIVE_PATH_MAX);
        }
        hive_mkkey(path);
        if (hive_set_sz(path, vbuf) == HIVE_OK) imported++;
    }
    if (imported) g_dirty = 1;
    return imported;
}

/* one-key legacy files: /etc/lang, /etc/size, /etc/videomode,
 * /etc/panicmode, /etc/env ("KEY=VALUE" lines). */
static void migrate_legacy_files(int *any) {
    static char buf[512];
    struct { const char *file; const char *path; } one[] = {
        { "/etc/lang",       "HKLM\\System\\Language" },
        { "/etc/videomode",  "HKLM\\System\\VideoMode" },
        { "/etc/panicmode",  "HKLM\\System\\Crash\\Mode" },
        { "/etc/size",       "HKLM\\Software\\CatOS\\Terminal\\FontSize" },
    };
    for (int i = 0; i < 4; i++) {
        int n = fs_read_text(one[i].file, buf, (int)sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = 0;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                         buf[n - 1] == ' ')) buf[--n] = 0;
        int s = 0;
        while (buf[s] == ' ') s++;
        if (!buf[s]) continue;
        /* only fill a gap: a value the hive already holds wins */
        if (hive_kind(one[i].path) < 0) {
            hive_set_sz(one[i].path, buf + s);
            *any = 1;
        }
    }
    int n = fs_read_text("/etc/env", buf, (int)sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        char *p = buf;
        while (*p) {
            char *nl = p;
            while (*nl && *nl != '\n') nl++;
            int stop = (*nl == 0);
            *nl = 0;
            char *eq = p;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                char path[HIVE_PATH_MAX];
                h_cpy(path, "HKLM\\Environment\\", HIVE_PATH_MAX);
                h_append(path, HIVE_PATH_MAX, p);
                if (hive_kind(path) < 0 && hive_set_sz(path, eq + 1) == HIVE_OK)
                    *any = 1;
                *eq = '=';
            }
            if (stop) break;
            p = nl + 1;
        }
    }
}

/* ---- init ------------------------------------------------------------- */
int hive_init(void) {
    for (int i = 0; i < (int)HIVE_MAX_NODES; i++) g_nodes[i].kind = NODE_FREE;
    g_slots = 0; g_gen = 0; g_dirty = 0; g_loaded = 0;
    g_migrated = 0; g_corrupt = 0; g_payload_crc = 0;

    int lr = hive_load();
    if (lr == HIVE_OK) {
        if (hive_verify() != 0) { lr = HIVE_ECORRUPT; }
        else return HIVE_OK;                 /* good image, nothing to migrate */
    }

    /* Load failed.  Quarantine whatever was there before overwriting it:
     * a corrupt hive is evidence, not garbage -- `hive chk` may want to
     * show it. */
    int have_bad = 0;
    int blen = 0;
    if (load_blob(HIVE_PATH, &blen) == 0) {
        fs_write_raw_mode(HIVE_BAD, g_raw, (uint32_t)blen, 0);
        have_bad = 1;
        (void)have_bad;
    }

    /* start from defaults */
    for (int i = 0; i < (int)HIVE_MAX_NODES; i++) g_nodes[i].kind = NODE_FREE;
    g_slots = 0; g_loaded = 0;
    seed_defaults();

    /* then adopt any legacy settings still on the volume */
    static char tbuf[6144];
    int any = 0;
    int tn = fs_read_text("/etc/reg", tbuf, (int)sizeof(tbuf) - 1);
    if (tn > 0) {
        tbuf[tn] = 0;
        int n = hive_import_ini(0, tbuf, tn);
        if (n > 0) any = 1;
    }
    migrate_legacy_files(&any);
    if (any) {
        g_migrated = 1;
        g_dirty = 1;
    }
    hive_save();          /* persist the migrated/default table */
    return HIVE_OK;
}

const char *hive_compat_get(const char *section, const char *key) {
    static char path[HIVE_PATH_MAX];
    map_legacy(section ? section : "", key ? key : "", path, HIVE_PATH_MAX);
    return hive_sz(path, "");
}

int hive_compat_set(const char *section, const char *key, const char *value) {
    static char path[HIVE_PATH_MAX];
    map_legacy(section ? section : "", key ? key : "", path, HIVE_PATH_MAX);
    return hive_set_sz(path, value);
}

/* ---- dump (export) ---------------------------------------------------- */
static int dump_key(int16_t key, char *out, int cap, int level) {
    int o = h_len(out);
    if (cap - o < 4) return HIVE_E2BIG;
    for (int i = 0; i < level; i++) out[o++] = ' ';
    out[o++] = '[';
    h_cpy(out + o, g_nodes[key].name, cap - o);
    o += h_len(out + o);
    out[o++] = ']';
    out[o++] = '\n';
    out[o] = 0;
    for (int16_t c = g_nodes[key].child; c != NODE_NONE; c = g_nodes[c].sib) {
        if (g_nodes[c].kind == NODE_FREE) continue;
        o = h_len(out);
        if (g_nodes[c].kind == HIVE_KEY) {
            int r = dump_key(c, out, cap, level + 1);
            if (r != HIVE_OK) return r;
            continue;
        }
        if (cap - o < 8 + h_len(g_nodes[c].name)) return HIVE_E2BIG;
        for (int i = 0; i < level + 1; i++) out[o++] = ' ';
        h_cpy(out + o, g_nodes[c].name, cap - o);
        o += h_len(out + o);
        out[o++] = '=';
        if (g_nodes[c].kind == HIVE_DW) {
            uint32_t v = 0;
            for (int i = 3; i >= 0; i--) v = (v << 8) | g_nodes[c].data[i];
            /* decimal + hex suffix */
            char num[24];
            int d = 0;
            if (v == 0) num[d++] = '0';
            uint32_t tv = v;
            while (tv) { num[d++] = (char)('0' + tv % 10); tv /= 10; }
            for (int i = 0; i < d / 2; i++) { char t = num[i]; num[i] = num[d-1-i]; num[d-1-i] = t; }
            num[d] = 0;
            h_cpy(out + o, num, cap - o);
            o += h_len(out + o);
            out[o++] = ' '; out[o++] = '('; out[o++] = 'd'; out[o++] = 'w'; out[o++] = ')';
        } else {
            int dl = g_nodes[c].datalen;
            if (dl > HIVE_DATA_MAX) dl = HIVE_DATA_MAX;
            for (int i = 0; i < dl; i++) {
                if (o >= cap - 1) return HIVE_E2BIG;
                out[o++] = (char)g_nodes[c].data[i];
            }
            if (g_nodes[c].kind == HIVE_BIN) {
                for (int i = 0; i < 6 && o < cap - 1; i++) out[o++] = '.';
            }
        }
        out[o++] = '\n';
        out[o] = 0;
    }
    return HIVE_OK;
}

int hive_dump(char *out, int cap) {
    if (!out || cap < 8) return -1;
    out[0] = 0;
    for (int16_t i = 0; i < (int16_t)g_slots; i++) {
        if (g_nodes[i].kind != HIVE_KEY || g_nodes[i].parent != NODE_NONE) continue;
        int r = dump_key(i, out, cap, 0);
        if (r != HIVE_OK) return -1;
    }
    return (int)h_len(out);
}

int hive_load_or_default(void) { return hive_load(); }
