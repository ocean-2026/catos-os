int g_hdr[4];
int fw(const char *a, int b) { return b; }
int ce_main(){ if (fw((const char *)g_hdr, 20) < 0) return 1; api->write("ok"); return 0; }
