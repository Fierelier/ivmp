# Reverse-engineering notes

Findings about GTA IV 1.0.7.0 and its use of Games for Windows - LIVE, made
while reimplementing `xlive.dll`. No GTA or Rockstar game files are included
in this repository.

## xlive ordinal signatures

GTA IV 1.0.7.0 imports 115 `xlive.dll` ordinals. Every export in
`src/windows/xlive.def` must match the argument count the game pushes, because
a `WINAPI` stub with the wrong count unbalances the caller's stack on return.

- `@75` is `XNetGetEthernetLinkStatus(void)`, not
  `XNetGetBroadcastVersionStatus(DWORD)`. All eight call sites in 1.0.7.0 call
  it with no arguments and test the result before `XNetGetTitleXnAddr`. The
  earlier one-argument stub popped four extra bytes and crashed the session
  code with `Eip=0`. It now returns `XNET_ETHERNET_LINK_ACTIVE`.
- `XUserReadProfileSettings` (`@5331`) is used in two phases. The game first
  passes a NULL results buffer and only allocates and issues the real
  asynchronous read when the call returns `ERROR_INSUFFICIENT_BUFFER` with the
  required size. Returning success with a zero size made the sign-in code skip
  the allocation while still treating the read as started, then dereference a
  NULL results pointer after `XGetOverlappedResult`. The stub now reports the
  size and fills one 40-byte `XUSER_PROFILE_SETTING` per requested ID with
  `XSOURCE_DEFAULT` and the type encoded in bits 28-31 of the setting ID.

## Multiplayer mod restriction

GTA IV decides whether a player is modded in its tamper check
(`0x403870` in 1.0.7.0). It verifies the Take-Two Authenticode signatures of
`GTAIV.exe`, `gta4Browser.exe`, `gtaEncoder.exe` and `binkw32.dll`, looks for a
`dsound.dll` in the game folder, and stores the result in XLive protected
buffers. A host copies its own result into the match configuration as
"mods allowed" (`0x1088f68`, read by `0x460050`). The host join handler
(`0x488d00`) answers a join request with reason 4 (`MP_NOPASS`, "This match
does not allow mods.") when the host is clean and the joiner reports mods.

The joiner fills the "has mods" field of its join request (offset `0x30`,
built at `0x4641ec`) from the same tamper check, so patching only the host
does not help a player joining an unpatched host.

The project allows modified clients, so on the known 1.0.7.0 build (PE
timestamp `0x4bd9efbe`) `src/windows/game_patch.c` makes the tamper check
`0x403870` report "not modded" and makes `0x460050` report "mods allowed",
after checking the original bytes of each and leaving any other build
untouched. Every caller of `0x403870` is matchmaking code (`0x461850`,
`0x461b20`, `0x463d20`), so this covers both the join request and the host
decision without touching single-player tamper handling.

## Player addressing

GTA IV 1.0.7.0 System Link uses one UDP socket per player (`0x776470`,
`SOCK_DGRAM`/`IPPROTO_UDP` with `SO_BROADCAST`; online mode would use
`IPPROTO_VDP` instead). Its own address comes from `getsockname`, falling back
to `XNetGetTitleXnAddr` when bound to `INADDR_ANY` (`0x7763b0`). Peers are
addressed through `XNetXnAddrToInAddr`, and received packets are attributed to
peers by the `(address, port)` that `recvfrom` reports. Packets that appear to
come from the player's own address are dropped (`0x776785`). If that traffic
stops arriving, the session times out and the local player is removed with the
"connection lost" flag, shown as `WARNING_1PC` ("The connection to Games for
Windows - LIVE has been lost. Returning to single player.").

Earlier builds reported the relay server's address as the player's own
address, as every peer's address and as the source of every received packet,
so the game could not tell peers apart or from itself. The client now gives
each relay connection a stable fake address `10.x.y.z` built from its
connection ID: `XNetGetTitleXnAddr` fills the whole 36-byte `XNADDR` with it
(and returns ethernet, static, gateway and DNS flags, since the game checks
`0x1c`), `XNetXnAddrToInAddr` returns the `ina` of the given `XNADDR`, and
tunnelled `recvfrom`/`WSARecvFrom` report the sender's fake address with the
receiving socket's own port. The relay wire format is unchanged; it already
forwards frames with the sender's connection ID.

## Sign-in and identity

Multiplayer menus are script-driven; the "You must be connected to Games for
Windows - LIVE to access Multiplayer." text is not referenced by `GTAIV.exe`.
The exe's online test (`0x75cf50`) requires `XNetGetEthernetLinkStatus`, a
usable `XNetGetTitleXnAddr` and a cached `XUserGetSigninState` (`0x7674d0`)
equal to `eXUserSigninState_SignedInToLive`. The stubs now report that state.

`XUserGetSigninInfo` is called with `XUSER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY`
for network identity (`0x764130`, `0x7c3850`) and with
`XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY` for the save folder
(`savegames\user_%016I64x`, `0x5b00b0`). The offline XUID stays
`0x0009000100000001` so existing saves are kept; the online XUID is
`0x00090000` followed by a 32-bit FNV-1a hash of `IVMP_NICKNAME`, so
players with different nicknames no longer share one XUID. `XUserGetName` and
`szUserName` use the nickname, truncated to 15 characters.

LIVE game sockets use `IPPROTO_VDP` (254), which Winsock cannot create, so
`XCreateSocket` maps it to UDP. The game converts its own two-byte VDP length
prefix before sending and after receiving (`0x7765b0`, `0x7766a0`), so the
payload is passed through unchanged.

## Social Club gate

After the LIVE check, the multiplayer entry code (`0x466e10`) calls
`0x7b05c0(name)`, which reads the Social Club `userinfo.dat` from the profile
folder and succeeds only if it holds a user name (copied into `name`, up to 63
characters). Otherwise it shows `CP_SC_SIGN_INPC` ("You must be logged into
Rockstar Social Club to access Multiplayer."). The service behind that file is
gone, so on 1.0.7.0 `src/windows/game_patch.c` redirects `0x7b05c0` (cdecl,
one argument, every caller pops it) to a function that returns true and copies
`IVMP_NICKNAME` as the Social Club name, after checking the original
bytes.

## Session creation

`XSessionCreate` (`0x772080`) passes an `XSESSION_INFO` that is an output for
a host (`XSESSION_CREATE_HOST`) and an input for a joiner. The stub used to
clear it in both cases, so hosts published an empty host address and joiners
lost the session they were joining. It now fills a host's session ID (random,
with the online-peer flag in `ab[0]` when presence, stats or matchmaking are
used), host address (the player's own `XNADDR`), key and nonce, leaves a
joiner's input untouched, and keeps a copy for `XSessionGetDetails`.

## Stats reads

Entering LIVE multiplayer reads the player's stats with `XUserReadStats`
(`0x764fa0`), in two phases like `XUserReadProfileSettings`. The stats parser
(`0x764b00`) walks only the first view, skips rows with rank 0 and skips
column types it does not know. Returning a well-formed but empty result
(unranked rows, or ranked rows with `XUSER_DATA_TYPE_NULL` columns) made the
multiplayer entry flow stall before `XSessionCreate`, while a failed read lets
the game fall back to default stats and reach the lobby. With no stats service
behind the relay, the stub reports `ERROR_FUNCTION_FAILED`.

## Script-raised warnings

Temporary tracing hooks on the warning label copy (`0x84f4e0`) and the script
VM's native call site (`0x5a5355`, `call edx` inside `0x5a4cd0`) showed the
following; the hooks have since been removed.
Natives are registered as `push fn; push hash; call 0x5a6200`; their hashes
are not joaat of the native names. The LIVE drop is raised by a script through
native `0x4e4c2f92` (`0xb63be0` -> `0xb602f0` -> `0x84f580`) with label
`CP_ERROR_0PC` ("The connection to Games for Windows - LIVE has been lost.
Returning to single player."); `CP_ERROR_2PC` is the System Link variant.

The script's host native `0xb61bf0` returns false with no GFWL calls. It
reaches `0x463d20` (host RENDEZVOUS session) through `0xb5f470` and `0x4613b0`.
The eight call sites in `0x463d20` whose results were traced are `0x463e23`, `0x463e7a`, `0x463e90`, `0x463eb7`, `0x463ec5`,
`0x463f1b` and `0x463fb8`, plus `0x463fc5`, which is the failure cleanup at
`0x463a40`.

The host trace showed that `0x4820d0` fails. It calls `0x484400` and then
`0x53e170`, which creates the session. For an online session (type 1),
`0x53e170` needs `XNET_GET_XNADDR_ONLINE` (`0x80`) in the flags returned by
`XNetGetTitleXnAddr`. For a System Link session (type 2), it needs `0x04` or
`0x18`. `XNetGetTitleXnAddr` now also returns the online flag.

## LIVE session search

The Party Mode browser calls `XSessionSearchEx` twice, from `0x76bb2d` and
`0x76bcf9`. The first call has no result buffer and must return `0` or
`ERROR_INSUFFICIENT_BUFFER` with the required size. Any other value aborts the
search. The second call passes that buffer with an `XOVERLAPPED`, and accepts
`0` or `ERROR_IO_PENDING`.

`xlive.dll` now publishes the host's session to the relay whenever the session
or the host's contexts, properties or QoS listen data change. A search asks the
relay for all published sessions on a worker thread and completes the
overlapped request. `XNetQosLookup` returns the host's QoS listen data for each
session ID it was given.

## Host migration

`0x772d3b` calls `XSessionMigrateHost` with the local user index when this
machine becomes the host, or `0xfe` (`XUSER_INDEX_NONE`) when another player
does. It waits on the overlapped event and checks `XGetOverlappedExtendedError`.
Just before, it releases QoS listening (`XNetQosListen` flags `0x10`).

For a new host, `xlive.dll` writes the session info with this machine's
address back to the game, marks the session as hosted and publishes it to the
relay. For everyone else it stores the session info the game passes in, which
comes from the new host, and stops publishing.

## Search result filtering

`0x76b920` copies each search result (92 bytes apart) into the browser list.
It looks for `X_PROPERTY_GAMER_HOSTNAME` (`0x40008109`) and shows
`***NONAME***` when it is missing, so `xlive.dll` adds that property with the
host's nickname. Every result is added; the filtering happens later.

`0x46329a` runs `XNetQosLookup` for each listed session. `0x460600` handles
the replies. It keeps a session only if the reply has
`XNET_XNQOSINFO_DATA_RECEIVED` and exactly `0xb8` bytes of data. `0x45fc10`
then compares that data with the local match settings at `[0x18a8304]`
(offsets `0`, `4` and `0x10`) and with the session's version property
`0x10000004`. Several fields of the data (`0x38`, `0x3c`, `0x44`, `0x64`,
`0x88`) can also clear the compatible flag. Sessions whose average round trip
exceeds 600 ms are dropped.

With `IVMP_TRACE=1`, the log shows the search contexts, the compatibility
result, and those data and local fields.

`XNetQosLookup` finds the session by its ID and, failing that, by the host
address. With tracing on, each lookup logs the IDs and addresses it was given
and the sessions `xlive.dll` knows about.

## Joining a LIVE session

A LIVE host only listens on its game socket until someone joins. The relay
learned a player's UDP address from the first datagram that player sent, so
the joiner's packets had nowhere to go. `xlive.dll` now registers each game
UDP socket with the relay when the game binds it.

The browser entries built by `0x76ab70` keep the 60-byte `XSESSION_INFO` at
`+0x159`, right after a property container whose flag word at `+0x158`
(`0x7657e0` tests it) overlaps the first three bytes of the session ID. The
copy zeroes them, so the joiner saw `000000a9773e1355` for the host's
`9f8f61a9773e1355` and then tried to join with it. Host session IDs now start
with three zero bytes, which makes that overlap harmless. `XNetQosLookup` also
falls back to matching on the host address.

## Game socket port

The game binds its VDP socket to port 1000. Under Wine on Linux that fails
with `WSAEACCES` (10013), because ports below 1024 need root there, so the game closed the socket and could neither host
nor join, even though matchmaking still listed its sessions. `XSocketBind`
now always binds game UDP sockets to a port the operating system picks. `XSocketGetSockName` and the rewritten sender addresses of
received packets keep reporting the requested port.

## Presence context

GTA IV sets `X_CONTEXT_PRESENCE` (`0x8001`) to `10` when single player starts.
In multiplayer it uses other values: `1` in the multiplayer menus, `0` while
setting up a host, `12` while searching, and `3`, `4` and `8` in games.
`xlive.dll` treats `10` as "multiplayer not needed".

## GTA IV 1.0.4.0

Build `0x499dc616` imports 107 `xlive.dll` ordinals, including
`XUserGetXUID` (`@5261`), which 1.0.7.0 does not use. It reads the local
XUID at `0x7bf00e` and the player name at `0x4c1170`, so `xlive.dll` returns
the same online XUID as `XUserGetSigninInfo`.

The Social Club sign-in check is byte-for-byte the 1.0.7.0 one at `0x970ee0`
and gets the same detour. The host join handler (`0x6beca0`) has no mods
reason, so neither the "is modified" nor the "mods allowed" patch is needed.

## Relay simplification (protocol 4)

GTA IV's LIVE traffic is all UDP on the VDP socket, addressed to peers. The
TCP game tunnel and the relay "host" role were never exercised by the game, so
protocol 4 dropped them: `XSocketConnect`, `XSocketSend`, `XSocketListen` and
`XSocketAccept` are plain Winsock calls, and untargeted datagrams go to every
other player. `XSocketRecv` still unwraps tunnel frames on relayed UDP sockets.

## Low frame rate disconnect

Once a second during a multiplayer game, `0x460310` (1.0.7.0) stores the
number of frames drawn in the last second in a 15-entry ring (`0x19ac924`)
and keeps their sum at `0x19ac934`. After 15 samples, if the sum times 1/15 is
below 10 frames per second, it shows `MP_WARNING_5` ("You have been
disconnected because your computer is running too slowly.") and leaves the
session through `0x4645e0`. `xlive.dll` turns the `jb` at `0x46043c`, which
skips the check until 15 samples exist, into a `jmp`, so the check never runs.
1.0.4.0 has the same code at `0x6b234c`. The label is also shown when the host
kicks a player with reason 3 (`0x465000`); that path is left alone.

## Sign-in notifications

The game polls `XNotifyGetNext` every frame (`0x75e260`, filter `0`). For
`XN_SYS_SIGNINCHANGED` (`0xa`) with bit 0 of the parameter set, it clears the
sign-in cache of `0x7674d0` (which otherwise asks `XUserGetSigninState` at
most once a second) and re-evaluates user 0 through `0x75de80`, which runs the
online test (`0x75cf50`). The same online test gates the multiplayer menus, so
reporting `eXUserSigninState_SignedInLocally` and sending that notification
when the relay connection is lost drops the player out of LIVE multiplayer.
The handler also knows `0xe`, `0x15`, `0x16`, `XN_LIVE_CONNECTIONCHANGED`
(`0x2000001`, only acting on `0x80151005`), `XN_LIVE_LINK_STATE_CHANGED` and
`0x4000001`-`0x4000003`.

