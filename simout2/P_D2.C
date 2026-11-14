int ce_main(){ char b[4]; char *d; int i; d = b; for (i = 0; i < 3; i++) *d++ = 65 + i; *d = 0; api->write(b); return 0; }
