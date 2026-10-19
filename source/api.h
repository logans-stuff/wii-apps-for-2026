/* Sonoran JSON API client. See beta-sonoran docs/API.md. */
#ifndef SONORAN_API_H
#define SONORAN_API_H

#include <stddef.h>

#define SONORAN_HOST "s.azmsh.dev"

/* s.azmsh.dev is behind Authelia. Authelia keeps the login in this
 * cookie. token.txt holds its value, or a full "name=value" pair. */
#define SESSION_COOKIE "authelia_session"

struct chat_msg {
    char when[24];     /* "10-19 15:31" in Arizona time */
    char from[48];
    char channel[24];
    char body[240];
};

struct node_row {
    char id[12];       /* "!a1b2c3d4" */
    char name[48];
    char short_name[8];
    char hw[32];
    char role[24];
    char heard[24];
    int gateway;
    int has_position;
};

enum api_result { API_OK = 0, API_FAILED, API_LOGIN };

/* Each call fills the array and sets *count. On error, err has the text. */
int api_chat(const char *token, struct chat_msg *out, int max, int *count,
             char *err, size_t errlen);
int api_nodes(const char *token, struct node_row *out, int max, int *count,
              int *total, char *err, size_t errlen);

/* Copy UTF-8 text as plain ASCII. Characters out of ASCII become '?'. */
void to_ascii(char *dst, size_t dstlen, const char *src);

#endif
