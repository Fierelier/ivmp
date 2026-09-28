# Current limitations

This repository is an experimental implementation, not yet a drop-in GTA IV
replacement.

- The relay protocol is independent and currently proven only with fake
  Winsock clients.
- `xlive.dll` only relays UDP game traffic, through synchronous socket
  wrappers; overlapped I/O is not supported. TCP sockets the game opens
  (`XSocketConnect`, `XSocketListen`, `XSocketAccept`) go straight to Winsock
  and are not relayed.
- The launcher does not yet provide a central server list.
- Direct endpoints currently resolve to IPv4; IPv6 literal addresses are not
  implemented.
- Password support is reserved but not implemented.
- Datagrams without a target player go to every other player on the server.
  Addressed UDP traffic, which LIVE sessions use, is routed per player, so
  several LIVE lobbies can run on one server.
- No game executable integrity checks or anti-cheat are present, and
  `xlive.dll` loads any `.asi` plugin it finds.
- Crash reports contain a best-effort text backtrace, not a minidump or full
  memory dump.
- No GTA IV files, patches, scripts, or Rockstar assets are distributed.
- Game patches exist for GTA IV 1.0.7.0 (mod checks, Social Club sign-in and
  the low frame rate disconnect) and 1.0.4.0 (Social Club sign-in and the low
  frame rate disconnect; that build has no multiplayer mod check). Other
  builds are left unpatched and keep the game's own checks. A host running an
  unpatched game can still kick players for being too slow.
- Players are given fake `10.x.y.z` addresses derived from their relay
  connection ID. Datagrams sent to such an address are routed to that player.
- LIVE matchmaking returns every session published on the relay whose
  contexts match the contexts in the search. The search procedure and search
  properties are ignored, and only numeric user properties plus the host name
  are published. QoS results report fixed latency and
  bandwidth values; only the host's QoS listen data is real.
- A server holds at most 1000 players, the most `select()` can watch. A
  session search returns at most about 180 lobbies, as many published
  records as fit in one 64 KiB control message.
- Only one session per player is published, and client and server must both
  use protocol version 5.
- The UDP session token only stops off-path spoofing. It travels in
  cleartext, so anyone who can see a player's traffic can still forge
  datagrams for that player.
- Host migration only updates the session's host address; the session ID and
  key stay the same. Whether the players stay together is up to the game's own
  migration logic.
- Two players with the same nickname get the same online XUID.
- Stats are not stored: `XUserReadStats` reports failure, and the stats
  enumerators and `XSessionWriteStats` are stubs.
- The game's UDP socket never binds the port the game asks for (1000). With
  `IVMP_REDIRECT_ALL` on, `xlive.dll` lets the operating system pick a free
  port and reports port 1000 to the game. All game traffic goes through the
  relay, so the real port does not matter, and ports below 1024 (root-only
  under Wine on Linux) or ports held by other programs are never a problem.
