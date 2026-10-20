// Wii build: newlib's <termios.h> includes this but devkitPPC doesn't ship it.
// The Wii has no serial ports, so framework-portduino's LinuxSerial only needs
// the declarations to compile; every call fails.
#pragma once
#include <errno.h>

typedef unsigned int tcflag_t;
typedef unsigned int speed_t;
typedef unsigned char cc_t;
#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_cc[NCCS];
};

#define VTIME 5
#define VMIN 6
#define TCSANOW 0
#define IGNBRK 0000001
#define BRKINT 0000002
#define PARMRK 0000010
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IXON 0002000
#define IXANY 0004000
#define IXOFF 0010000
#define OPOST 0000001
#define ONLCR 0000004
#define CSIZE 0000060
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define CLOCAL 0004000
#define CRTSCTS 020000000000
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHONL 0000100
#define B9600 0000015
#define B115200 0010002

static inline int tcgetattr(int fd, struct termios *t) { (void)fd; (void)t; errno = ENOTTY; return -1; }
static inline int tcsetattr(int fd, int act, const struct termios *t) { (void)fd; (void)act; (void)t; errno = ENOTTY; return -1; }
static inline int cfsetispeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
static inline int cfsetospeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
