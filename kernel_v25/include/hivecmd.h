#ifndef HIVECMD_H

#include <stdint.h>
#define HIVECMD_H

/* Shell front-end for the binary hive (v2.67).
 *
 * Usage (also printed by `hive help`):
 *
 *   hive                          summary: roots, node/value counts, gen, crc
 *   hive ls [PATH]                children of a key (default: the roots)
 *   hive ls -R [PATH]             recursive tree
 *   hive get PATH                 typed value (sz / dw / bin)
 *   hive set PATH VALUE           write REG_SZ          (admin)
 *   hive setdw PATH NUMBER        write REG_DWORD       (admin)
 *   hive mkkey PATH               create a key subtree  (admin)
 *   hive del PATH                 delete a value        (admin)
 *   hive delkey PATH              delete a subtree      (admin, confirms)
 *   hive export                   whole tree as INI text
 *   hive import                   re-scan /etc/reg + legacy files (admin)
 *   hive chk                      verify the in-memory tree + on-disk image
 *   hive save                     write through + read-back verify (admin)
 *
 * Every failure prints `hive: <reason>` and returns a POSIX-style status
 * (2 = usage, 1 = runtime/not-found, 13 = permission), so `$?` and
 * `hive ... > file` behave like the rest of the shell.
 */
int hive_cmd_run(char *arg, int uid, uint32_t caps);

#endif /* HIVECMD_H */
