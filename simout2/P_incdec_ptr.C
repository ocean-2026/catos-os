/* incdec_ptr */
int ce_main(){ int a[3]; int *p; p=a; *p=1; p++; *p=2; api->print_int(a[0]+a[1]); return 0; }
