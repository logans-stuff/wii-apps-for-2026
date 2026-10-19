Sonoran for Wii 0.1

This app shows chat messages and nodes from s.azmsh.dev (Sonoran).
It uses TLS 1.3 (wolfSSL). It does not use the Internet Channel.

LOGIN (session cookie)
s.azmsh.dev uses an Authelia login. The app sends your Authelia session
cookie. It does not know your password.
1. On a computer, log in to https://s.azmsh.dev.
2. Push F12. Firefox: Storage > Cookies. Chrome: Application > Cookies.
   Select https://s.azmsh.dev.
3. Copy the value of the cookie "authelia_session".
4. On the Wii, push 1 in the app (pairing). On the computer, run
   ./pair.sh <wii-ip> and paste the value. The app saves it to
   sd:/apps/sonoran/token.txt.
   Or put the value on one line in sd:/apps/sonoran/token.txt yourself.
When Authelia ends the session, do these steps again.
Keep the SD card safe. The cookie gives access to your account.

CONTROLS (Wii Remote)
Left/Right  change screen (Chat, Nodes)
Up/Down     select a line
A           open the line
B           go back
+           load again
1           pairing (get the session cookie over the LAN)
Home        exit

The Wii clock must be correct, or the certificate check fails.
Times are in Arizona time (UTC-7).
