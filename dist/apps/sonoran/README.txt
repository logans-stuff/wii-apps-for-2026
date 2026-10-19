Sonoran for Wii 0.1

This app shows chat messages and nodes from s.azmsh.dev (Sonoran).
It uses TLS 1.3 (wolfSSL). It does not use the Internet Channel.

LOGIN
Push 1 in the app. Type your s.azmsh.dev username and password with the
on-screen keyboard. The app sends them only to auth.azmsh.dev (Authelia)
over TLS 1.3. It does not save the password. It saves the session cookie
in sd:/apps/sonoran/token.txt and the username in user.txt.
When Authelia ends the session, push 1 and log in again.
Keep the SD card safe. The session cookie gives access to your account.

Keyboard: all characters show at the same time. D-Pad move, A type,
B delete, 2 show password,
+ OK, Home cancel.

Other way (pairing): push 2, then run ./pair.sh <wii-ip> on a computer
and paste the value of the "authelia_session" cookie.

CONTROLS (Wii Remote)
Left/Right  change screen (Chat, Nodes)
Up/Down     select a line
A           open the line
B           go back
+           load again
1           log in
2           pairing (get the session cookie over the LAN)
Home        exit

The Wii clock must be correct, or the certificate check fails.
Times are in Arizona time (UTC-7).
