#ifndef CATOS_VERSION_H
#define CATOS_VERSION_H

/*
 * Single source for the user-visible kernel version string.
 * Bump this one constant instead of editing copies across shell / fs /
 * about screens.  Guarded so a -DCATOS_VERSION=... build flag still wins.
 */
#ifndef CATOS_VERSION
#define CATOS_VERSION "2.82"
#endif

#endif /* CATOS_VERSION_H */
