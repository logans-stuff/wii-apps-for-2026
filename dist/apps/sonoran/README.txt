Sonoran for Wii 0.1

This app shows chat messages and nodes from view.azmsh.net.
It uses TLS 1.3 (wolfSSL). It does not use the Internet Channel.

TOKEN
1. On a computer, log in to https://view.azmsh.net.
2. Open the developer tools (F12) > Application (Chrome) or Storage (Firefox)
   > Cookies > https://view.azmsh.net.
3. Copy the value of the cookie "access_token".
4. On the Wii, push 1 in the app (pairing). On the computer, run
   ./pair.sh <wii-ip> and paste the value. The app saves it to
   sd:/apps/sonoran/token.txt.
   Or put the value on one line in sd:/apps/sonoran/token.txt yourself.
The token is valid for 7 days. Then do these steps again.
Keep the SD card safe. The token gives access to your account for 7 days.

CONTROLS (Wii Remote)
Left/Right  change screen (Chat, Nodes)
Up/Down     select a line
A           open the line
B           go back
+           load again
1           pairing (get a token over the LAN)
Home        exit

The Wii clock must be correct, or the certificate check fails.
Times are in Arizona time (UTC-7).
