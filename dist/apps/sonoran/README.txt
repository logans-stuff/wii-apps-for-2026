Sonoran for Wii 0.3

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
Left/Right  change screen (Chat, Nodes, Map, Discord)
-           change screen (on all screens, also Map)
Up/Down     select a line
A           open the line
B           go back
+           load again
1           log in
2           pairing (get the session cookie over the LAN)
Home        exit

MAP
D-Pad moves the map. A zooms in, B zooms out. The status line shows the
node nearest to the cross. Red = node, blue = MQTT gateway.
Map tiles: (c) OpenStreetMap contributors, tile.openstreetmap.org.

DISCORD
Push 1 on the Discord screen. The Wii makes a key (up to one minute) and
shows a QR code. On your phone, open Discord > your profile > Scan QR Code,
scan it, and push "Log in". The token is saved in
sd:/apps/sonoran/discord.txt. Keep the SD card safe: the token gives full
access to your Discord account.
Note: Discord does not allow unofficial clients on user accounts. Discord
can ban an account that uses one. You use this at your own risk.
A opens a server, a channel or a message. B goes back. In a channel,
1 writes a message, + loads the messages again.

The Wii clock must be correct, or the certificate check fails.
Times are in Arizona time (UTC-7).
