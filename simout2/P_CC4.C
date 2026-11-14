int g[2];
int f(int b) { return b; }
int ce_main(){ if (f((const char *)g, 20) < 0) return 1; api->write("ok"); return 0; }
