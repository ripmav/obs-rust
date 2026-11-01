/* Interface between the two translation units of the obsvideo runner (spec S-058).
 * Test tooling, not part of libobs. */
#pragma once
#include "script.h"

/* Handles the Linux platform glue commands (obs-nix*.c); returns true when the command was consumed. */
bool nix_command(struct script_line *sl);
/* Re-executes the runner from a private directory so that the executable-relative data path is
 * deterministic (see obsvideo_nix.c). Returns only if no re-exec is needed. */
void nix_reexec(char **argv);
