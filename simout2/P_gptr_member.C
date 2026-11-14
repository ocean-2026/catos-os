/* gptr_member */
typedef struct{ int v; } S; S g; S *gp; int ce_main(){ gp=&g; gp->v=8; api->print_int(g.v); return 0; }
