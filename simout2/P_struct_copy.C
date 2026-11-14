/* struct_copy */
typedef struct{int x; int y;} S; int ce_main(){ S a; S b; a.x=3; a.y=4; b=a; api->print_int(b.x+b.y); return 0; }
