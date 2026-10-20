/* POSIX functions that OpenSSL calls and the Wii does not have.
 * OpenSSL uses them only to check for setuid programs; the Wii has no
 * users, so every id is 0. */
#include <sys/types.h>

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
