#define _GNU_SOURCE
#include <spawn.h>
#include <stdint.h>

// The flag POSIX_SPAWN_SETSID is on track, but not officially standardized yet
// glibc and musl should both support it
uint32_t posix_spawn_setsid_flag() {
#ifdef POSIX_SPAWN_SETSID
	return POSIX_SPAWN_SETSID;
#else
	return 0;
#endif
}
