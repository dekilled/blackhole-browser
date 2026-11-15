/* Diagnostics: records which web features the engine ignored or approximated (for --diagnose). */
#ifndef BH_DIAG_H
#define BH_DIAG_H

#include <stdbool.h>
#include <stdio.h>

extern bool diag_on;
void        diag_note(const char *category, const char *name);
void        diag_report(FILE *f);

#endif
