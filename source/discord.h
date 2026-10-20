/* Small Discord client: REST API v9 with a user session token. */
#ifndef SONORAN_DISCORD_H
#define SONORAN_DISCORD_H

#include <stddef.h>

#define DISCORD_HOST "discord.com"

struct dc_item {          /* a server or a channel */
    char id[24];
    char name[64];
};

struct dc_msg {
    char when[24];        /* "10-20 08:15" Arizona time */
    char author[40];
    char text[400];
};

enum dc_result {
    DC_OK = 0,
    DC_FAILED,
    DC_BAD_LOGIN,         /* wrong email or password, or token not valid */
    DC_NEED_MFA,          /* 2FA code needed; ticket is set */
    DC_CAPTCHA,           /* Discord asks for a captcha; the Wii cannot show it */
    DC_VERIFY             /* Discord sent a "new login location" email */
};

int dc_login(const char *email, const char *password, char *token, size_t tokenlen,
             char *ticket, size_t ticketlen, char *err, size_t errlen);
int dc_mfa(const char *ticket, const char *code, char *token, size_t tokenlen,
           char *err, size_t errlen);

int dc_guilds(const char *token, struct dc_item *out, int max, int *count,
              char *err, size_t errlen);
int dc_channels(const char *token, const char *guild, struct dc_item *out, int max,
                int *count, char *err, size_t errlen);
int dc_messages(const char *token, const char *channel, struct dc_msg *out, int max,
                int *count, char *err, size_t errlen);
int dc_send(const char *token, const char *channel, const char *text,
            char *err, size_t errlen);

#endif
