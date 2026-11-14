int ce_main(int argc, char **argv) { api->write("argv0="); api->write(argv[0]); api->write("
"); api->write("argc="); api->print_int(argc); api->write("
"); return 0; }
