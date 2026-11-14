/* static_local */
int f(){ static int n; n++; return n; } int ce_main(){ api->print_int(f()+f()); return 0; }
