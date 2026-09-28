# IVMP wire protocol

Version: 5

This document describes the protocol implemented by this repository.

All integers are unsigned and encoded in network byte order. The relay accepts
only the exact magic and version values below.

## Control channel

`xlive.dll` opens a TCP control connection to the configured
`host:port`. The control header is 16 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `IVMPCTL\0` |
| 8 | 2 | Protocol version (`5`) |
| 10 | 2 | Message type |
| 12 | 4 | Payload length |

Control payloads may be up to 65536 bytes.

### Client hello

Type `1`, payload:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Minimum protocol version |
| 2 | 2 | Maximum protocol version |
| 4 | 4 | Requested connection ID, or `0` |
| 8 | 2 | Nickname byte length (1–64) |
| 10 | n | UTF-8/ASCII nickname |

`xlive.dll` picks a random connection ID at startup so the game can be given
its `10.x.y.z` address before it connects. The server grants the requested ID
if it is between `1` and `0xfffffe` and not in use, and otherwise assigns
another one; the welcome message carries the ID actually granted.

### Server welcome

Type `2`, 12-byte payload:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Granted connection ID |
| 4 | 8 | Session token |

The connection ID is a public routing value. The session token is a random
64-bit value generated per control connection; every tunnel datagram from the
player must carry it (see below). A new token is issued on every reconnect.

### Server error

Type `6`, payload: a 2-byte error code followed by an ASCII message. The server
sends it instead of a welcome and then closes the connection. `xlive.dll`
writes the code and message to `ivmp.log`.

| Code | Meaning |
|---:|---|
| 2 | Unsupported protocol version |
| 3 | Invalid hello |
| 4 | Server is full |

### Heartbeat

Type `3` (ping), client to server, empty payload; the server answers with type
`4` (pong), also empty. `xlive.dll` pings every 5 seconds. Type `5` (goodbye),
empty, closes the connection.

Any control message or game datagram counts as activity. The server
disconnects a player who has sent nothing for 60 seconds, which also removes
their session record. A control connection that has not completed the hello
within 10 seconds is closed.

The server never blocks on a slow reader: replies that do not fit in the socket
send buffer are queued, up to 131104 bytes per connection. A client that lets
more than that pile up is disconnected.

### Session publishing

Type `8`, client to server. The payload is an opaque session record of at most
4096 bytes, written by `xlive.dll` while the player hosts a GFWL session. An
empty payload removes the record. The server keeps one record per player and
drops it when the player disconnects.

Type `9`, client to server, empty payload: asks for the published sessions.

Type `10`, server to client, reply to type `9`:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Record count |
| 4 | … | Per record: 4-byte connection ID, 4-byte serial, 4-byte length, record bytes |

The serial increases each time a player starts publishing a record. After a
host migration the old and new host may briefly both list the same session ID;
`xlive.dll` keeps the record with the higher serial.

The session record is little-endian and only interpreted by `xlive.dll`:
`IVS2` magic, the host's nickname (16 bytes), the 60-byte `XSESSION_INFO`,
session flags, maximum public and private slots, filled public and private
slots, the host's user contexts (count, then id/value pairs), numeric user
properties (count, then id, 1-byte type, 8-byte value) and the QoS listen data
(length, bytes).

## Game traffic

Game traffic is UDP only. `xlive.dll` sends every datagram of a game UDP socket
to the relay's UDP port, wrapped in a 28-byte tunnel header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `IVMT` |
| 4 | 2 | Protocol version |
| 6 | 2 | Flags (`0`) |
| 8 | 4 | Sender connection ID |
| 12 | 4 | Payload length |
| 16 | 4 | Target connection ID, or `0` |
| 20 | 8 | Session token from the welcome message |

The relay drops any datagram whose token does not match the token issued to
the sender connection ID, so a third party cannot redirect a player's traffic
or inject frames in their name. The relay overwrites the token with zeros
before forwarding a frame, so peers never learn each other's tokens.

The relay remembers the source address of the last valid datagram from each
player. A frame with a target connection ID goes only to that player. Datagrams
sent to a relay peer address (`10.x.y.z`, see `docs/reverse-engineering.md`)
carry the peer's connection ID as the target. Frames without a target go to
every other player. The relay does not parse or validate gameplay payloads.

A zero-length tunnel frame only registers the sender's address and is not
forwarded. As soon as the game binds a UDP socket, `xlive.dll` sends one from
it and repeats that on every heartbeat, so the relay can reach a LIVE host that
has not sent anything yet and NAT mappings stay open.

## Failure handling

- Invalid magic, version or length closes the control stream; invalid tunnel
  datagrams are dropped.
- The server has no TLS, password, or player identity system.
- Gameplay modifications and cheating are not inspected.
