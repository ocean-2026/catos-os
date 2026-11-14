/* fptr_member */
typedef struct{ int (*fn)(int); } F; int dbl(int v){ return v*2; } int ce_main(){ F f; f.fn = dbl; api->print_int(f.fn(21)); return 0; }
