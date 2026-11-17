#include "ce_app.h"

struct ls_ctx { int count; };

static void ls_cb(int is_dir, const char *name, uint32_t size, void *ud) {
    catos_api_t *api = CE_API();
    struct ls_ctx *c = (struct ls_ctx *)ud;
    if (is_dir) {
        api->write("  [DIR ] ");
        api->write(name);
        api->write("/\n");
    } else {
        api->write("  [FILE] ");
        api->write(name);
        api->write("  ");
        api->print_int((int)size);
        api->write(" B\n");
    }
    c->count++;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char t[24];
    char path[64];
    (void)argc; (void)argv;

    api->write("CatOS System Info\n");
    api->write("  kernel      : CatOS 1.0 (i386, GRUB 2.06)\n");
    api->write("  time        : ");
    api->time_str(t);
    api->write(t);
    api->write("\n");
    api->write("  filesystem  : ");
    if (api->fs_ready()) api->write("FAT32 on IDE (mounted)\n");
    else                  api->write("none\n");

    api->write("  heap probe  : ");
    {
        char *p = (char *)api->alloc(4096);
        if (p) { api->write("alloc 4096 ok\n"); api->free(p); }
        else   api->write("alloc failed\n");
    }

    api->write("  / listing  :\n");
    {
        struct ls_ctx ctx;
        ctx.count = 0;
        api->ls("/", ls_cb, &ctx);
        api->write("  entries     : ");
        api->print_int(ctx.count);
        api->write("\n");
    }

    api->write("  type a path to list it, or 'q' to quit\n");
    for (;;) {
        api->write("sysinfo> ");
        if (api->readline(path, sizeof(path)) <= 0) break;
        if (api->strcmp(path, "q") == 0 || api->strcmp(path, "quit") == 0) break;
        struct ls_ctx ctx;
        ctx.count = 0;
        api->write("  ");
        api->ls(path, ls_cb, &ctx);
        api->write("  entries: ");
        api->print_int(ctx.count);
        api->write("\n");
    }
    api->write("bye\n");
    return 0;
}
