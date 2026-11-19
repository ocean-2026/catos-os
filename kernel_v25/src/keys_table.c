/*
 * keys_table.c -- centralized key-binding descriptors (see keys.h).
 *
 * Each row groups the sibling keys that share one action; the `codes`
 * array lists the real decoded key codes (keyboard.h KB_* or ASCII) the
 * dispatch branches in keyboard.c / shell.c / ime.c act on.  Rows are in
 * `keys` display order: edit, scroll, history, IME, console, file.
 */
#include "keys.h"
#include "keyboard.h"

/* synthetic: digits 1-9 are one IME selection branch in ime.c */
static const int codes_digits[] = { 0 };

const keys_desc_t keys_desc[] = {
    /* ---- editing ------------------------------------------------------ */
    { (int[]){ KB_LEFT, KB_RIGHT }, "Left / Right", KG_EDIT,
      "move cursor left / right (UTF-8 aware)", "command line / editor",
      "page IME candidates while IME active" },
    { (int[]){ KB_HOME, KB_END }, "Home / End", KG_EDIT,
      "jump to line start / end", "command line / editor", "-" },
    { (int[]){ '\b' }, "Backspace", KG_EDIT,
      "delete char before cursor", "command line / editor / IME",
      "deletes pinyin letter while IME active" },
    { (int[]){ KB_DEL }, "Del", KG_EDIT,
      "delete char at cursor", "command line / editor", "-" },
    { (int[]){ KB_CTRL_LEFT, KB_CTRL_RIGHT }, "Ctrl+Left / Ctrl+Right", KG_EDIT,
      "move one word left / right", "command line", "-" },
    { (int[]){ KB_PGUP, KB_PGDN }, "PgUp / PgDn", KG_EDIT,
      "page up / down one editor screen", "editor",
      "unbound on the command line" },

    /* ---- scrollback --------------------------------------------------- */
    { (int[]){ KB_PGUP, KB_PGDN },
      "PgUp / PgDn", KG_SCROLL,
      "scroll back/forward one full page per press",
      "console / command line", "-" },

    /* ---- history ------------------------------------------------------- */
    { (int[]){ KB_UP, KB_DOWN }, "Up / Down", KG_HISTORY,
      "previous / next command in history", "command line", "-" },

    /* ---- pinyin IME ---------------------------------------------------- */
    { (int[]){ KB_F2 }, "F2", KG_IME,
      "toggle pinyin IME", "command line / editor", "-" },
    { codes_digits, "1-9", KG_IME,
      "pick candidate by number", "IME active", "-" },
    { (int[]){ ' ' }, "Space", KG_IME,
      "pick the first candidate", "IME active", "-" },
    { (int[]){ '-', '=', '[', ']' }, "- = [ ]", KG_IME,
      "candidate page up / down", "IME active",
      "arrows page too" },
    { (int[]){ 27 }, "Esc", KG_IME,
      "cancel composition, clear pinyin string", "IME active", "-" },
    { (int[]){ '\n' }, "Enter", KG_IME,
      "commit the raw pinyin string", "IME active", "-" },

    /* ---- console line control ----------------------------------------- */
    { (int[]){ 0x03 }, "Ctrl+C", KG_CONSOLE,
      "cancel input line / send SIGINT to fg task", "command line", "-" },
    { (int[]){ KB_CTRL_Z }, "Ctrl+Z", KG_CONSOLE,
      "suspend fg task to background (SIGTSTP)", "command line", "-" },
    { (int[]){ KB_TAB }, "Tab", KG_CONSOLE,
      "complete command / path", "command line", "-" },

    /* ---- built-in editor file actions ---------------------------------- */
    { (int[]){ 0x0F, 0x13 }, "Ctrl+O / Ctrl+S", KG_FILE,
      "save file", "editor", "-" },
    { (int[]){ 0x18, 0x19 }, "Ctrl+X / Ctrl+Y", KG_FILE,
      "exit editor; ask on modified / discard", "editor",
      "Ctrl+Y discards" },
    { (int[]){ 0x0B, 0x15, 0x07 }, "Ctrl+K/U/G", KG_FILE,
      "delete line / clear all / help", "editor", "-" },
};

const int keys_desc_count =
    (int)(sizeof(keys_desc) / sizeof(keys_desc[0]));

const char *keys_group_name(enum keys_group g) {
    switch (g) {
    case KG_EDIT:    return "edit";
    case KG_SCROLL:  return "scroll";
    case KG_HISTORY: return "history";
    case KG_IME:     return "IME";
    case KG_CONSOLE: return "console";
    case KG_FILE:    return "file";
    default:         return "?";
    }
}

const keys_desc_t *keys_lookup(int code) {
    if (code == 0) return 0;
    for (int i = 0; i < keys_desc_count; i++) {
        const int *c = keys_desc[i].codes;
        if (!c) continue;
        for (int j = 0; c[j]; j++)
            if (c[j] == code) return &keys_desc[i];
    }
    return 0;
}
