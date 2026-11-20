/* atk_fs.c -- attack vector: the FAT32 / VFS layer through the CE file
 * syscalls (ls / touch / mkdir / cat / save / file_read / file_write /
 * file_delete / fsize / file_exists) -- src/fs.c, src/fat.c.
 *
 * WHY IT MIGHT CRASH
 *   The on-disk structures are attacker-adjacent: a path that resolves to a
 *   directory can be read as a file, a name can be 300 bytes (FS_PATH_MAX
 *   is 160), a delete can target a directory that still has children, and
 *   an 8.3 name may be all dots or blank.  Any of those can make a FAT
 *   cluster walk or a directory scan run past its buffer, or loop.
 *   Read-only probes come first; the destructive ones (delete / overwrite)
 *   are last so the evidence above them stays valid.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

static char big[17000];
static char path[400];

/* api->ls() runs a user-supplied callback from the ring3 vsyscall thunk.
 * NOTE: passing NULL here does not return an error -- the thunk just does
 * `call 0` and the task dies at eip=0x00000000 (found by this very probe),
 * so every ls() below passes a real function. */
static int nent;
static void cb(int is_dir, const char *name, uint32_t size, void *ud) {
    (void)is_dir; (void)name; (void)size; (void)ud;
    nent++;
}

static void say(const char *k, int v) { m(k, v); }

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    int i;
    (void)argc; (void)argv;

    w("ATK-FS: filesystem argument abuse\n");
    for (i = 0; i < 17000; i++) big[i] = (char)('a' + (i % 26));

    step(1); mark("read-only probes: dirs, weird names, empty paths\n");
    say("ls /", api->ls("/", cb, &nent));
    say("ls /BIN", api->ls("/BIN", cb, &nent));
    say("ls HELLO", api->ls("/BIN/HELLO.CE", cb, &nent));
    say("ls ''", api->ls("", cb, &nent));
    say("ls '.'", api->ls(".", cb, &nent));
    say("ls '..'", api->ls("..", cb, &nent));
    say("ls 200dots", api->ls("........................................................"
                              "........................................................"
                              "........................................................",
                              cb, &nent));
    say("cat /BIN", api->cat("/BIN", big, 4096));
    say("fsize /BIN", api->fsize("/BIN"));
    say("exists /BIN", api->file_exists("/BIN"));
    nl();

    step(2); mark("path traversal / odd separators\n");
    {
        static const char *P[] = {
            "/../etc/passwd", "/./bin/hello.ce", "//bin//hello.ce",
            "/bin/../../etc/acl", "etc/passwd", "/BIN/HELLO.CE/", "/.../",
            "/BIN/.", "/BIN/..", "/..", "/. ", " /bin/hello.ce",
        };
        for (i = 0; i < (int)(sizeof(P) / sizeof(P[0])); i++) {
            say(P[i], api->cat(P[i], big, 64));
            w(" ");
        }
        nl();
    }

    step(3); mark("file_read offsets past EOF / huge\n");
    {
        static const int O[] = { 0, 1, 100, 65535, 0x7FFFFFFF, -1, -65536 };
        for (i = 0; i < 7; i++) {
            say("off", O[i]);
            wd(api->file_read("/BIN/HELLO.CE", big, 16384, O[i]));
            w(" ");
        }
        nl();
    }

    step(4); mark("create 40 files in /TMP, then read them back\n");
    {
        int ok = 0, bad = 0;
        for (i = 0; i < 40; i++) {
            /* unique-ish 8.3 names: F00.TXT .. F39.TXT */
            path[0] = '/'; path[1] = 'T'; path[2] = 'M'; path[3] = 'P';
            path[4] = '/'; path[5] = 'F';
            path[6] = (char)('0' + (i / 10)); path[7] = (char)('0' + (i % 10));
            path[8] = '.'; path[9] = 'T'; path[10] = 'X'; path[11] = 'T';
            path[12] = 0;
            if (api->save(path, big, 200) == 0) ok++; else bad++;
        }
        say("created", ok); say("failed", bad); nl();
    }

    step(5); mark("mkdir abuse\n");
    say("mkdir /", api->mkdir("/"));
    say("mkdir /BIN", api->mkdir("/BIN"));
    say("mkdir /TMP/A", api->mkdir("/TMP/A"));
    say("mkdir /TMP/A again", api->mkdir("/TMP/A"));
    say("mkdir /TMP/A/B/C/D", api->mkdir("/TMP/A/B/C/D"));
    say("cat /TMP/A", api->cat("/TMP/A", big, 64));
    nl();

    step(6); mark("write/append to dirs and odd names\n");
    say("fw-dir", api->file_write("/BIN", big, 64, 0));
    say("fw-mode9", api->file_write("/TMP/W.TXT", big, 64, 9));
    say("save-empty", api->save("/TMP/EMPTY.TXT", big, 0));
    say("save-4k", api->save("/TMP/FOURK.TXT", big, 4096));
    say("save-17k", api->save("/TMP/BIG.TXT", big, 17000));
    say("fsize-big", api->fsize("/TMP/BIG.TXT"));
    nl();

    step(7); mark("all-dot / blank 8.3 names\n");
    {
        static const char *N[] = { "/TMP/.", "/TMP/..", "/TMP/...", "/TMP/    ",
                                   "/TMP/A..B", "/TMP/.HIDDEN", "/TMP/ABCDEFGH.XYZW",
                                   "/TMP/abc.txt", "/TMP/ABC.TXT" };
        for (i = 0; i < (int)(sizeof(N) / sizeof(N[0])); i++) {
            say("touch", api->touch(N[i]));
            wd(api->file_exists(N[i])); w(" ");
        }
        nl();
    }

    step(8); mark("destructive: delete dirs and files\n");
    say("del /", api->file_delete("/"));
    say("del /BIN", api->file_delete("/BIN"));
    say("del /TMP/A", api->file_delete("/TMP/A"));
    say("del /TMP/BIG.TXT", api->file_delete("/TMP/BIG.TXT"));
    say("del /TMP/NOPE", api->file_delete("/TMP/NOPE"));
    say("del empty", api->file_delete(""));
    say("del /BIN/HELLO.CE", api->file_delete("/BIN/HELLO.CE"));
    say("exists after", api->file_exists("/BIN/HELLO.CE"));
    nl();

    step(9); mark("volume still coherent? (ls /, /BIN, /TMP + entry counts)\n");
    nent = 0; say("ls /", api->ls("/", cb, &nent)); say("/ ents", nent);
    nent = 0; say("ls /BIN", api->ls("/BIN", cb, &nent)); say("BIN ents", nent);
    nent = 0; say("ls /TMP", api->ls("/TMP", cb, &nent)); say("TMP ents", nent);
    say("fsize /BIN/HELLO.CE", api->fsize("/BIN/HELLO.CE"));
    say("exists /BIN/HELLO.CE", api->file_exists("/BIN/HELLO.CE"));
    nl();

    step(10); mark("deep 150-char path (inside FS_PATH_MAX=160) -> real lookup\n");
    /* FS_PATH_MAX is 160 (include/fs.h:14), so a 300-char path only proves
     * u2k_str() rejects it.  This one is short enough to reach fat_lookup()
     * and walk ~37 path components. */
    {
        int n = 0;
        path[n++] = '/';
        for (i = 0; i < 37; i++) { path[n++] = 'A'; path[n++] = '/'; }
        path[n++] = 'X'; path[n++] = '.';
        path[n++] = 'T'; path[n++] = 'X'; path[n++] = 'T';
        path[n] = 0;
        say("pathlen", n);
        say("mk150", api->mkdir(path));
        say("touch150", api->touch(path));
        say("cat150", api->cat(path, big, 64));
        say("fw150", api->file_write(path, big, 32, 0));
        say("fr150", api->file_read(path, big, 32, 0));
        say("del150", api->file_delete(path));
        nl();
    }

    w("\nATK-DONE\n");
    return 0;
}
