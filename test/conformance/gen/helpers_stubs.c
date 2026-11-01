/*
 * Conformance harness (area helpers, spec S-024): platform functions that the helper
 * modules link against but that are not part of gen/stubs.c. The runner itself defines
 * the rest (fake clock, fake curl, in-memory file system, fake obs_* getters).
 *
 * os_set_thread_name: the reference sets a debugger-visible thread name; irrelevant here.
 */
#include <stdint.h>

/* used by util/task.c through threading-posix.c in the reference; defined there already */
