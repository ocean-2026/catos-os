int ce_main(){ int x; int *p; p = &x; *p = 5; *p += 3; api->print_int(x); return 0; }
