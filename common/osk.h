/* On-screen keyboard for the Wii Remote. Shows every printable ASCII key at
 * once (no shift): D-Pad = move, A = type, B = delete, 1 types '!', + = OK,
 * Home = cancel, 2 = show/hide when masked. Returns 1 on OK, 0 on cancel.
 * It draws over the whole console; redraw your own screen afterward. */
#ifndef SONORAN_OSK_H
#define SONORAN_OSK_H

#include <stddef.h>

int osk_input(const char *title, char *buf, size_t buflen, int masked);

#endif
