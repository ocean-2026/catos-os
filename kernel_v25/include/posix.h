/*
 * posix.h -- v2.67 uniform POSIX-style command-line diagnostics.
 *
 * Every shell builtin answers the same three questions the way a real
 * POSIX utility does:
 *
 *   cat: invalid option -- 'z'          -> status 2 (usage error)
 *   cat: missing operand                -> status 2
 *   cat: /no/such/file: No such file or directory -> status 1
 *   cat: permission denied              -> status 1
 *
 * The spec table below is the single source of truth for the syntax of
 * the builtins; `--help` on any listed command prints its own synopsis
 * without needing a hand-written help body.
 */
#ifndef CATOS_POSIX_H
#define CATOS_POSIX_H

#include <stdint.h>

/* POSIX utility exit statuses used across the shell. */
#define CLI_OK       0
#define CLI_FAIL     1
#define CLI_USAGE    2   /* command line syntax / invalid option        */
#define CLI_NOPERM   13  /* EACCES                                      */
#define CLI_NOENT    2   /* treated as a usage error by most utilities  */

/* Parse + validate one command line.  Returns 0 when the line is
 * acceptable, otherwise prints the diagnostic and returns the status
 * the shell should record in $?.  `arg` is the raw argument text after
 * the command word (not modified). */
int cli_validate(const char *cmd, const char *arg);

/* Print "<cmd>: <detail>" and, when the failure is a usage error, the
 * "Try '<cmd> --help'" hint.  Returns the status to use. */
int cli_usage(const char *cmd, const char *detail);
int cli_fail(const char *cmd, const char *what, const char *why);

/* Synopsis string for a known builtin, or NULL. */
const char *cli_synopsis(const char *cmd);

/* Print the generic help block for a builtin (synopsis + options). */
void cli_help(const char *cmd);

/* True when the command is described by the table (used by help/completion). */
int cli_known(const char *cmd);

/* quoted/escaped tokenizer shared by the validators and new utilities.
 * Returns the number of tokens, at most max.  Destructive on `buf`. */
int cli_tokenize(char *buf, char *tok[], int max);

#endif /* CATOS_POSIX_H */
