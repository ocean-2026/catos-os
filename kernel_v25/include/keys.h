#ifndef KEYS_H
#define KEYS_H

/*
 * keys.h -- centralized key-binding descriptor table.
 *
 * This table is the SINGLE SOURCE OF TRUTH for every special key the
 * kernel decodes and acts on.  The keyboard decoder (keyboard.c) maps
 * scancodes to the KB_* / ASCII codes listed in each row's `codes`
 * array; the `keys` shell command walks this same table to print the
 * grouped legend.  Adding a newly bound key means: define/use its code
 * in keyboard.h, add the dispatch branch, and append/extend ONE row
 * here -- `keys` picks it up automatically, no separate hand-written
 * legend to keep in sync.
 *
 * Groups mirror the `keys` command layout: edit / scroll / history /
 * IME / console / file (editor).
 */

enum keys_group {
    KG_EDIT = 0,    /* line / editor cursor editing            */
    KG_SCROLL,      /* console scrollback                      */
    KG_HISTORY,     /* command-line history                    */
    KG_IME,         /* pinyin input method                      */
    KG_CONSOLE,     /* shell console line control              */
    KG_FILE,        /* built-in editor file actions            */
    KG__COUNT
};

typedef struct {
    const int   *codes;       /* decoded codes bound by this row; 0 = synthetic */
    const char  *labels;      /* human-readable labels, joined by " / "         */
    enum keys_group group;
    const char  *desc;        /* what the keys do                          */
    const char  *scene;       /* where they apply                           */
    const char  *limit;       /* mode / context restriction, "-" if none  */
} keys_desc_t;

extern const keys_desc_t keys_desc[];
extern const int         keys_desc_count;

/* Group names in display order (English, matches the on-screen legend). */
const char *keys_group_name(enum keys_group g);

/* Look up the row whose codes contain `code`; 0 when unbound. */
const keys_desc_t *keys_lookup(int code);

#endif /* KEYS_H */
