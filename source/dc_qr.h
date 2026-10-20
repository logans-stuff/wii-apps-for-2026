/* Discord QR code login ("remote auth", the same login as the desktop
 * app). The phone app scans the code, and the token comes encrypted to
 * our own RSA key. */
#ifndef SONORAN_DC_QR_H
#define SONORAN_DC_QR_H

#include <stddef.h>

enum ra_state {
    RA_WAIT = 0,     /* nothing new */
    RA_QR,           /* qr_text is ready: show it */
    RA_SCANNED,      /* user name is ready: confirm on the phone */
    RA_DONE,         /* token is ready */
    RA_CANCELLED,    /* the user pushed Cancel on the phone */
    RA_ERROR
};

/* Start: make the RSA key and connect. This can take some seconds. */
int ra_start(char *err, size_t errlen);
/* Wait up to ms for news. Fills qr_text, user or token for those states. */
int ra_poll(int ms, char *qr_text, size_t qrlen, char *user, size_t userlen,
            char *token, size_t tokenlen, char *err, size_t errlen);
void ra_end(void);

#endif
