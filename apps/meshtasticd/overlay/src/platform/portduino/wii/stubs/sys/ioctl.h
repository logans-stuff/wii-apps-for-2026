// Wii build: devkitPPC has no <sys/ioctl.h>. Only LinuxSerial's FIONREAD is
// needed and there are no serial ports; ioctl() (portduino_glue_wii.cpp)
// always fails with ENOTTY.
#pragma once
#define FIONREAD 0x541B
#ifdef __cplusplus
extern "C"
#endif
    int
    ioctl(int fd, unsigned long req, ...);
