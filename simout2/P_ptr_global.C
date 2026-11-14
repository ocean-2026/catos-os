/* ptr_global */
int g; int *gp; int ce_main(){ g=9; gp=&g; api->print_int(*gp); return 0; }
