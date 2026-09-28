# Connecting

There is no launcher. `xlive.dll` replaces GFWL's `xlive.dll` in the game
folder and reads its target server from persistent environment variables at
process start. It only connects to the server once the game uses
multiplayer: when the game checks the multiplayer privilege, creates, searches
for or joins a session, sets up QoS, or sends game traffic. Until then no
connection is made and game packets are dropped. The server's error message
(for example when it is full) is written to `ivmp.log`. When the game sets its
rich-presence context (`0x8001`) to `10`, which it uses for single player,
and no session is open, `xlive.dll` disconnects within 50 ms and does not
reconnect until the presence changes again. As a fallback, once the player has
no session and there has been no multiplayer activity or game traffic for a
minute, `xlive.dll` says goodbye to the server and closes the connection,
freeing the slot; the next multiplayer action connects again with the same
address when it is still free.

## Losing the server

If the server cannot be reached, closes the connection or stops answering,
`xlive.dll` reports the player as signed in locally instead of to LIVE and
sends the game a sign-in change notification. The game then treats LIVE as
lost: it leaves any multiplayer game and refuses to start or join new ones.
`XUserCheckPrivilege` also denies multiplayer and `XSessionCreate` fails until
the connection is back. While the server is lost, `xlive.dll` retries it
every 5 seconds in the background, also in single player; once it answers,
the player is reported as signed in to LIVE again (with another sign-in
change notification) and the phone's LIVE option works again. A reconnect in
single player is closed again right away, as usual. Without `IVMP_SERVER` the
player is always signed in locally. Leaving multiplayer on purpose (single
player or a minute of inactivity) does not count as losing the server.

## ASI plugins

At startup, before the game's own entry point runs, `xlive.dll` loads every
`*.asi` file in the working directory (normally the game folder) and in its
`scripts` folder, and calls each plugin's `InitializeASI` export if it has
one. It exports `IsUltimateASILoader`, so plugins written for Ultimate ASI
Loader treat it as one. A plugin that is already loaded, for example by a
separate ASI loader DLL, is skipped. Loading is logged to `ivmp.log`.

## Launcher

`bin\ivmp_gui.exe` (window title "IVMP GUI") asks for the game executable
(usually `LaunchGTAIV.exe`; any program, batch file or shortcut works),
nickname and server address, plus two switches: "Enable debug logs" sets
`IVMP_TRACE=1` and "Disable game hooks" sets `IVMP_GAME_PATCHES=0`.
Launch sets `IVMP_SERVER`, `IVMP_NICKNAME` and `IVMP_REDIRECT_ALL=1` for the
game only (nothing is stored in the user's environment), starts the chosen
file from its own folder and closes. The fields are saved to and restored
from `ivmp_gui.ini` next to `ivmp_gui.exe`. `xlive.dll` still has to be in the
game folder. If the chosen launcher hands off to Steam instead of starting
the game itself, the game will not see these variables; pick `GTAIV.exe`
directly in that case.

## Setup

```
tools\connect.bat "C:\path\to\GTAIV folder" host:port nickname
```

This copies `bin\xlive.dll` into the given GTAIV folder and persists
(`setx`) `IVMP_SERVER`, `IVMP_NICKNAME`, and
`IVMP_REDIRECT_ALL` for the current user.

`setx` only affects new processes started after the change is picked up by
the shell that launches them, so log out and back in (or reboot) once after
running it. After that, launch `GTAIV.exe` directly (Steam, a desktop
shortcut, whatever) and `xlive.dll` connects automatically.

## Manually, without the script

Set these persistently (System Properties -> Environment Variables, or
`setx` from `cmd.exe`) and copy `bin\xlive.dll` into the GTAIV folder
yourself:

- `IVMP_SERVER` — `host:port`; the host may be an IPv4 address or a DNS
  name, which is looked up again on every connection attempt
- `IVMP_NICKNAME` — display name
- `IVMP_REDIRECT_ALL` — `1`
- `IVMP_TRACE` — optional; `1` logs to `ivmp.log` (emptied at startup) next to
  `xlive.dll`: the first five calls of every GFWL export and every 1000th
  after that, session, search and address details, `XSocketBind` results, and
  the peer address and length of the first 60 game datagrams (then every
  1000th)
- `IVMP_CRASH_LOG` — optional; `0` turns off the crash reporter and
  `ivmp-crash.log`
- `IVMP_GAME_PATCHES` — optional; `0` leaves `GTAIV.exe` unpatched (no mod,
  tamper, Social Club or low frame rate patches); multiplayer will then
  refuse to start, so use it only to rule the patches out

