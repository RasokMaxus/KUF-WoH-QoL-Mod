# LAN play: how Steam multiplayer works and the plan

Work in progress (2026-11-08). Addresses are for the Steam GOLD exe at image base `0x400000`
(see `REVERSING.md`). Everything here is from reading the exe and from packet captures of a real
Locus session; the LAN design at the end is not built yet.

## Three parts called "Locus"

| Part | What it is | LAN play |
|------|------------|----------|
| **XNet** | Network layer inside `KingdomUnderFire.exe`: UDP port 9237, reliable delivery, acks. Carries the lobby room, game start and in-game commands. | Kept as is |
| **Boot server** | `gamechain.locuschain.com` (3.35.250.231), UDP 54637 and 39999. Tells room members each other's addresses. | Replaced: the host's mod answers instead |
| **LocusGameChain.exe** | Separate blockchain program: account, sync, room list, match records. | Replaced: room list over the LAN |

The CD version's own network path (TCP, then UDP in Steam) is still in the exe behind the fixed
flag `0xB41726`, but Steam stopped maintaining it: with the flag off, the room server receives a
socket number where it now expects an address record and crashes (`0x6D225A`). So LAN play
reuses XNet instead.

## Menus

- Multiplayer button: main-frame command handler `0x590A90`, case at `0x590AFC` (Locus readiness
  checks), then command `0xBBA`.
- The CD's "WarGate / LAN / Cancel" screen still exists: screen state `0x19` shows menu table
  `0xB5B040` (records of 0x64 bytes; a record's label belongs to the next button). Buttons 3001
  (WarGate, case `0x59120D`), 3002 (LAN, renamed LOCUS: command `0xBBA`, case `0x591263`), 3003
  (Cancel). The mod shows it again as **Locus Online / LAN**.
- `0xBBA` sets screen state `0x32` (`0x58D120` stores it at main frame `+0x128`; the state switch
  is in `0x593F80`): the Registry (`CMultiGameRegistrationMenu`). Its OK sends `0xBDF`, state 5,
  `0x598AF0`: the lobby (`CXLocalServer` + `CXLocalGameJoinDlg`, the CD's LAN lobby).
- Create is refused with "Gamechain network is preparing" at `0x6E05EF` unless `0x57D1A0() >= 2`
  and `0x57C4E0() >= 6` (both ask the LocusGameChain window for its state).

## Packets

Every XNet and boot packet starts with a checksum: reflected CRC-32 with polynomial
`0xD5828281`, initial value and final XOR `0xFFFFFFFF`, over the bytes after the checksum, stored
big-endian. Checked at `0x4080A8` (`0x6CF630(data+4, len-4, 0xD5828281)`); verified on captured
packets. Byte 4 is the message type. IP addresses are in network order, ports big-endian.

### Boot server, UDP 54637

| Type | Direction | Body after `[crc 4][type 1]` |
|------|-----------|-------------------------------|
| 01 | host to server: register a room | room key (32 ASCII hex), private IP (4), port (2) |
| 02 | player to server: join a room | same as 01 |
| 03 | to server: leave | room key |
| 06 | to server, every 2 s: keep-alive | room key |
| 04 | server reply: room members | 32 bytes (zero in replies), count (1), your index (1), host index (1), then per member: public IP (4), public port (2), private IP (4), private port (2) |
| 05 | server to existing members: new member | room key, public IP, port, private IP, port |
| 07 | server reply to 06 | room key, one status byte (01 seen by the host, 03 by a joiner; the game's handler `0x6BBD00` ignores it) |

Confirmed by a real two-player Locus capture (two copies of the game on one PC): the joiner's 04
listed count 2, self 1, host 0, the host's entry then the joiner's.

Sent by XNet's vtable (`0xAC0A5C`): slot 6 `0x4046D0` (01), slot 7 `0x404850` (02), `0x404A4C`
(03), slot 8 `0x404AF0` (06). Replies go to `0x6BA470` (registered with XNet at `0x6BDDE1`),
which switches on the type: 04 `on_htm_response` `0x6BB790` (fields at packet+5: `+0x20` count,
`+0x21` self, `+0x22` host, `+0x23` entries), 05 `on_htm_response_add` `0x6BBB20`, 07 `0x6BBD00`,
and the peer messages 0x14 ping, 0x17 change_peer_addr, 0x1A ack_heart, 0x1B disconnect_peer,
0x1C/0x1D notify_address, 0x28/0x29, 0x33, 0x34 notify_connect_peer.

### UDP 39999 (text)

`REG=<key><public IP 4><port 2>\r\n` is answered with `REG=a.b.c.d:port\r\n` (the address the
server sees: the player's public address). `RLY=<key><destination IP 4><port 2>\r\n<XNet
packet>` is the relay used when two players cannot reach each other directly: the server
forwards the whole datagram unchanged to the destination (the keep-alive names the sender
itself, so it comes back to it). In the capture of two copies on one PC, hole punching failed
(dead 169.254 private address, no hairpin on the router) and all room traffic went through the
relay, which is why the room showed ~400 ms pings.

### XNet packets, UDP 9237

`[crc 4][0x28][sender public IP 4][port 2][sequence and flags...][payload]`; 15-byte packets are
acks. The payload is the original KUF message (CS_HELLO with the player name and address strings,
"Server Ready", SC_SLOT_INIT_END ...). The host also talks to itself at `127.0.0.1:9237`.

## Rooms

- **Room key**: `0x6C0C70` hashes three fields of the room's game info (offsets `+0x1DA`,
  `+0x2E`, `+0xE` of the 0x208-byte struct) into 32 hex characters. Host and joiners compute it
  from the same room info. `0x6D8420` (room client start) sets it with `0x6BE500` and calls
  `0x6BED00` (host: XNet register) or `0x6BED50` (XNet join), then waits for the boot reply.
- **Publishing**: when a room changes, `0x6BA0C0` builds the room's JSON with `0x57F5A0`
  (`code` "KUF", `ver` "1.416a", `ip`, `mac`, `player`, `room`, `map`, `state`, `maxuser`,
  `speed`, `maptype`, `subarg`, `type`, `account`, `time`) and sends it to LocusGameChain.
- **Listing**: every 3 s the lobby calls `0x57C5F0(0x6CD380)`, which asks the LocusGameChain
  window (message `0x3416`) for the list, reads it from shared memory, parses the JSON array with
  `0x57FD90` (it also writes `gamelist.json.txt`) into records of 0x120 bytes (std::strings:
  `+0x00` room, `+0x18` time, `+0x30` ip, `+0x48` num, `+0x60` code, `+0x78` ver, `+0x90`
  player, `+0xF0` mac; ints `+0x108` type, `+0x10C` state, `+0x110` maxuser, `+0x114` subarg,
  `+0x118` speed, `+0x11C` maptype) and passes them to the lobby callback `0x6CD380`.

## Addresses

- The boot server name is in the table at `0xB4A9C4`..`0xB4A9D8` and is looked up once at game
  start (`0x6BDEB0`, `gethostbyname`), so XNet keeps the resolved address.
- Locus reports the first network adapter it finds as the private address; on a PC with virtual
  adapters that can be a dead 169.254.x.x one. Radmin VPN (26.x) is never picked by the game's
  own address code (`0x6CC6E0`), so the mod chooses the address (`lan-ip`).

## Lobby, profiles and users (LAN mode)

- **Registry without a password**: OK (button `0xBDF`, case `0x72780F`) opens the password box at
  `0x7278DB` unless the profile is already loaded, in which case it passes OK on (`0x72793E`).
  LAN mode always passes it on and loads the profile itself: `multi\<name>\account.dat` is not
  encrypted (version 6, name, player ID at `+0x24`); `0x5A4A40` (empty record),
  `0x5AC270(name, record)`, `0x5A5FA0` (copy into the current player `0xBEDA40`, ID at
  `0xBEDA60`), `0x5A55F0` (free). An ID of 0 makes the room server answer "Server is Full"
  (`0x6D2A00`, in `CXServer::OnCSHello`) and the lobby show `<00000000>`.
- **Room hand-over to LocusGameChain** (`0x57F5A0`, shared memory + `SendMessage 0x3414`) is
  skipped in LAN mode.
- **Status box** (`0x6C04C0`) shows "LAN play"; **welcome text** (`0x6C0470`) is replaced;
  **Node Status** lines (`0x6CCFD0`) are dropped.
- **Users**: presence object `0xBEBB40`. `0x556D70(name, id, 0, quiet)` adds a user,
  `0x557120(name, id)` removes one, `0x557CD0(name, id, time)` keeps one listed until
  time + 33 s (GetTickCount64), `0x556BC0` (through `0x6CCF70`) adds yourself; the events reach
  `0x6CDCC0` ("entered/left the lobby"). Locus's lobby-channel handler `0x6CCB90` is ignored in
  LAN mode; each LAN lobby announces "KUFLAN1U\n<id> <name>" every 2 s on UDP 9246, and the
  mod adds, refreshes (every 3 s) and removes (after 8 s of silence) users on the UI thread.
- Not done: lobby chat over LAN.

Tested 2026-11-09 with two copies of the game on one PC over Radmin: no password, correct names
and IDs, LAN welcome and status, stable user list with real enter/leave messages, host, join and
a full match with correct result screens. Not yet tested between two PCs or offline.

## LAN design (built in `src/lan.cpp`)

The Locus flag `0xB41726` stays on in both modes; "LAN" is the mod's own mode (`g_lan`).
Hooks: `0x6D8420` (room client start: choose the boot target), `0x57FA91` (copy the host's room
JSON), `0x57C5F0` (room list from LAN announcements), calls at `0x6E05EF` / `0x6E05F9` (Create
readiness), `WSASendTo` / `GetQueuedCompletionStatus` (redirect boot traffic, fix reply source),
`gethostbyname` (placeholder address for the boot server when offline). UDP ports used by the
mod: 54637 and 39999 (boot server, on the host), 9246 (room announcements).


1. **Locus Online / LAN screen**: done (test builds).
2. **Host = boot server**: in LAN mode the host's mod listens on UDP 54637 and 39999 at its LAN
   address and answers 01/02/03/06 with 04/05/07 (and REG/RLY), grouping players by room key and
   using the address each packet came from as both public and private address.
3. **Redirect**: in LAN mode the mod sends XNet's boot-server packets to the chosen room's host
   instead of the internet server, and makes the replies look as if they came from the boot
   server (in the `WSASendTo` / completion hooks).
4. **Room list**: the host's mod broadcasts the room JSON from `0x57F5A0` on the LAN (and to
   addresses listed in the config, for networks without broadcast); in LAN mode `0x57C5F0` builds
   a JSON array from what it heard and runs the game's own parser and callback.
5. **Gates**: Create (`0x6E05EF`) and any other LocusGameChain readiness checks report "ready";
   the match record upload at the end of a game is skipped; the Registry password is skipped.
6. **Offline start**: check that the game starts with no internet and no LocusGameChain sync.

Still to confirm with a two-player capture: the join exchange (02, 04, 05) and the peer messages
that follow it.
