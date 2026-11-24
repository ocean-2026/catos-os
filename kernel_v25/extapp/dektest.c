/* dektest.c -- ring3 probe for the P3 declarative GUI ABI (slots 72..76).
 *
 * Exercises the CE-facing channel from user mode (not the kernel self-test):
 *   72 blk_commit      -- submit a 3-block WORK subtree
 *   73 blk_event_poll  -- drain one pending event
 *   74 blk_image_load  -- load a sample bitmap (best effort)
 *   75 blk_image_free  -- release the handle
 *   76 blk_text_width  -- measure "Hi" (two 8px ASCII glyphs => 16)
 *
 * Verdict lines consumed by the probe gate:
 *   DEKTEST commit=<rc> poll=<n> tw=<w> img=<res>
 *   DEKTEST result=1   (commit==0 && tw==16) / DEKTEST result=0 otherwise
 *
 * Built freestanding; no libc.  Keep comments plain ASCII.
 */
#include "ce_app.h"

static void put_int(int v) {
    char b[12];
    int n = 0;
    unsigned int u;
    if (v < 0) { ce_write("-"); u = (unsigned int)(-(int)v); }
    else u = (unsigned int)v;
    if (u == 0) { ce_write("0"); return; }
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) ce_putc(b[--n]);
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *api = ce_api();

    ce_blk_rec_t r[3];
    char pool[4];
    pool[0] = 'H'; pool[1] = 'i'; pool[2] = 0; pool[3] = 0;

    /* r0: root column */
    r[0].id = 1; r[0].parent = 0;
    r[0].kind = 1; r[0].layer = 200;              /* BLK_COLUMN / LAYER_WORK */
    r[0].req_w = -1; r[0].req_h = -1;
    r[0].text_off = -1; r[0].pix_off = -1;
    r[0].pix_w = 0; r[0].pix_h = 0; r[0].res = -1;

    /* r1: text child */
    r[1].id = 2; r[1].parent = 1;
    r[1].kind = 4; r[1].layer = 200;              /* BLK_TEXT */
    r[1].req_w = -1; r[1].req_h = -1;
    r[1].text_off = 0; r[1].pix_off = -1;
    r[1].pix_w = 0; r[1].pix_h = 0; r[1].res = -1;

    /* r2: rect child */
    r[2].id = 3; r[2].parent = 1;
    r[2].kind = 5; r[2].layer = 200;             /* BLK_RECT */
    r[2].req_w = -1; r[2].req_h = -1;
    r[2].text_off = -1; r[2].pix_off = -1;
    r[2].pix_w = 0; r[2].pix_h = 0; r[2].res = -1;

    int rc  = api->blk_commit(r, 3, pool, 2);
    ce_blk_event_t ev;
    int polled = api->blk_event_poll(&ev, 0);
    int tw = api->blk_text_width("Hi");
    int img = api->blk_image_load("/samples/desktop.bmp");
    if (img >= 0) api->blk_image_free(img);

    ce_write("DEKTEST commit="); put_int(rc);
    ce_write(" poll="); put_int(polled);
    ce_write(" tw="); put_int(tw);
    ce_write(" img="); put_int(img);
    ce_write("\n");

    int ok = (rc == 0 && tw == 16);
    ce_write(ok ? "DEKTEST result=1\n" : "DEKTEST result=0\n");
    return ok ? 0 : 1;
}
