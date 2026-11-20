/*
 * reg.c -- CatOS file-backed registry (v2.62).
 * See include/reg.h for the format and design notes.
 */
#include "reg.h"
#include "fs.h"

#define REG_PATH "/etc/reg"

/* ---- tiny local string helpers (reg.c is linked standalone) ------------ */
static int rlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void rcopy(char *d, const char *s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int req(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
static int risspace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* ---- table ------------------------------------------------------------- */
typedef struct {
    char key[REG_KEY_MAX];
    char val[REG_VAL_MAX];
    int  used;
} reg_key_t;
typedef struct {
    char name[REG_SEC_MAX];
    reg_key_t keys[REG_MAX_KEYS];
    int used;
} reg_sec_t;

static reg_sec_t tabs[REG_MAX_SECTIONS];
static int       sec_used;
static int       dirty;
static int       from_file;

static int find_sec(const char *name) {
    for (int i = 0; i < REG_MAX_SECTIONS; i++)
        if (tabs[i].used && req(tabs[i].name, name)) return i;
    return -1;
}
static int find_key(int s, const char *key) {
    if (s < 0) return -1;
    for (int i = 0; i < REG_MAX_KEYS; i++)
        if (tabs[s].keys[i].used && req(tabs[s].keys[i].key, key)) return i;
    return -1;
}
static int ensure_sec(const char *name) {
    int s = find_sec(name);
    if (s >= 0) return s;
    if (sec_used >= REG_MAX_SECTIONS) return -1;
    s = sec_used++;
    rcopy(tabs[s].name, name, REG_SEC_MAX);
    tabs[s].used = 1;
    return s;
}

int reg_init(void) {
    for (int i = 0; i < REG_MAX_SECTIONS; i++) {
        tabs[i].used = 0;
        for (int k = 0; k < REG_MAX_KEYS; k++) tabs[i].keys[k].used = 0;
    }
    sec_used = 0; dirty = 0; from_file = 0;

    static char buf[6144];
    int n = fs_read_text(REG_PATH, buf, (int)sizeof(buf) - 1);
    if (n > 0) {
        from_file = 1;
        buf[n] = 0;
        int cur = -1;
        char *p = buf;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            /* trim trailing CR / spaces */
            int le = rlen(line);
            while (le > 0 && (line[le - 1] == '\r' || line[le - 1] == ' ')) line[--le] = 0;
            int ls = 0;
            while (risspace(line[ls])) ls++;
            if (!line[ls] || line[ls] == '#') continue;
            if (line[ls] == '[') {
                char *rb = line + ls + 1;
                while (*rb && *rb != ']') rb++;
                if (*rb == ']') *rb = 0;
                cur = ensure_sec(line + ls + 1);
                continue;
            }
            if (cur < 0) continue;
            char *eq = line + ls;
            while (*eq && *eq != '=') eq++;
            if (!*eq) continue;
            *eq = 0;
            char *k = line + ls, *v = eq + 1;
            while (risspace(*k)) k++;
            while (risspace(*v)) v++;
            int ke = rlen(k);
            while (ke > 0 && risspace(k[ke - 1])) k[--ke] = 0;
            if (!*k) continue;
            int s = ensure_sec(tabs[cur].name);
            int slot = find_key(s, k);
            if (slot < 0)
                for (int i = 0; i < REG_MAX_KEYS; i++)
                    if (!tabs[s].keys[i].used) { slot = i; break; }
            if (slot >= 0) {
                rcopy(tabs[s].keys[slot].key, k, REG_KEY_MAX);
                rcopy(tabs[s].keys[slot].val, v, REG_VAL_MAX);
                tabs[s].keys[slot].used = 1;
            }
        }
    }

    /* seed defaults: only keys that are still absent get created, so an
     * existing /etc/reg is never silently overwritten with defaults. */
    if (reg_get("system", "lang") == 0 || !*reg_get("system", "lang"))
        reg_set("system", "lang", "en");
    if (!*reg_get("system", "theme"))    reg_set("system", "theme", "dark");
    if (!*reg_get("system", "videomode")) reg_set("system", "videomode", "gfx24");
    if (!*reg_get("system", "panicmode")) reg_set("system", "panicmode", "reboot");
    if (!*reg_get("system", "size"))     reg_set("system", "size", "1");
    if (!*reg_get("env", "PATH"))        reg_set("env", "PATH", "/bin");
    return 0;
}

const char *reg_get(const char *section, const char *key) {
    static const char empty[] = "";
    if (!section || !key) return empty;
    int k = find_key(find_sec(section), key);
    return k < 0 ? empty : tabs[find_sec(section)].keys[k].val;
}

int reg_set(const char *section, const char *key, const char *value) {
    if (!section || !*section || !key || !*key || !value) return -1;
    int s = ensure_sec(section);
    if (s < 0) return -1;
    int k = find_key(s, key);
    if (k < 0) {
        for (int i = 0; i < REG_MAX_KEYS; i++)
            if (!tabs[s].keys[i].used) { k = i; break; }
        if (k < 0) return -1;
        rcopy(tabs[s].keys[k].key, key, REG_KEY_MAX);
        tabs[s].keys[k].used = 1;
    }
    rcopy(tabs[s].keys[k].val, value, REG_VAL_MAX);
    dirty = 1;
    return 0;
}

int reg_del(const char *section, const char *key) {
    int k = find_key(find_sec(section), key);
    if (k < 0) return -1;
    int s = find_sec(section);
    tabs[s].keys[k].used = 0;
    tabs[s].keys[k].val[0] = 0;
    dirty = 1;
    return 0;
}

int reg_flush(void) {
    if (!dirty) return 0;
    static char buf[8192];
    int o = 0;
    const char hdr[] = "; CatOS registry -- DO NOT EDIT while running\n";
    for (int i = 0; o < (int)sizeof(hdr) - 1; i++) buf[o++] = hdr[i];
    for (int s = 0; s < REG_MAX_SECTIONS; s++) {
        if (!tabs[s].used) continue;
        int any = 0;
        for (int k = 0; k < REG_MAX_KEYS; k++) if (tabs[s].keys[k].used) { any = 1; break; }
        if (!any) continue;
        if (o + 2 + rlen(tabs[s].name) + 2 >= (int)sizeof(buf)) break;
        buf[o++] = '[';
        for (int i = 0; tabs[s].name[i]; i++) buf[o++] = tabs[s].name[i];
        buf[o++] = ']'; buf[o++] = '\n';
        for (int k = 0; k < REG_MAX_KEYS; k++) {
            if (!tabs[s].keys[k].used) continue;
            int kl = rlen(tabs[s].keys[k].key), vl = rlen(tabs[s].keys[k].val);
            if (o + kl + 1 + vl + 2 >= (int)sizeof(buf)) break;
            for (int i = 0; i < kl; i++) buf[o++] = tabs[s].keys[k].key[i];
            buf[o++] = '=';
            for (int i = 0; i < vl; i++) buf[o++] = tabs[s].keys[k].val[i];
            buf[o++] = '\n';
        }
    }
    buf[o] = 0;
    int r = fs_write_raw_mode(REG_PATH, buf, (uint32_t)o, 0);
    if (r == 0) dirty = 0;
    return r;
}

int reg_set_now(const char *s, const char *k, const char *v) {
    int r = reg_set(s, k, v);
    return r ? r : reg_flush();
}
int reg_del_now(const char *s, const char *k) {
    int r = reg_del(s, k);
    return r ? r : reg_flush();
}

int reg_key_count(const char *section) {
    int s = find_sec(section), n = 0;
    if (s < 0) return 0;
    for (int i = 0; i < REG_MAX_KEYS; i++) if (tabs[s].keys[i].used) n++;
    return n;
}
const char *reg_key_at(const char *section, int index) {
    static const char empty[] = "";
    int s = find_sec(section), n = 0;
    if (s < 0) return empty;
    for (int i = 0; i < REG_MAX_KEYS; i++) {
        if (!tabs[s].keys[i].used) continue;
        if (n++ == index) return tabs[s].keys[i].key;
    }
    return empty;
}
const char *reg_val_at(const char *section, int index) {
    static const char empty[] = "";
    int s = find_sec(section), n = 0;
    if (s < 0) return empty;
    for (int i = 0; i < REG_MAX_KEYS; i++) {
        if (!tabs[s].keys[i].used) continue;
        if (n++ == index) return tabs[s].keys[i].val;
    }
    return empty;
}
int reg_section_count(void) { return sec_used; }
const char *reg_section_at(int index) {
    static const char empty[] = "";
    int n = 0;
    for (int i = 0; i < REG_MAX_SECTIONS; i++) {
        if (!tabs[i].used) continue;
        if (n++ == index) return tabs[i].name;
    }
    return empty;
}
int reg_loaded_from_file(void) { return from_file; }
