// lan.cpp - LAN play (setting lan-play). WORK IN PROGRESS. See docs/LAN.md for the full picture.
//
// Steam's multiplayer runs on Locus's network layer (XNet, UDP 9237, inside the exe). Locus also
// uses an internet "boot server" (gamechain.locuschain.com, UDP 54637 and 39999) that tells room
// members each other's addresses, and the LocusGameChain program for the room list. LAN play keeps
// XNet and replaces the other two:
//   - Multiplayer shows the CD version's chooser again, as "Locus Online / LAN".
//   - Boot server: the room's host runs a small one (BootThread). In LAN mode every boot-server
//     packet XNet sends goes to the host of the room being joined (LanRedirect, called from the
//     WSASendTo hook in nettrace.cpp), and the replies are made to look as if they came from the
//     real boot server (LanFixSource, from the completion hook).
//   - Room list: the host broadcasts its room's JSON (the same text the game sends to
//     LocusGameChain) on UDP 9246; in LAN mode the lobby's room-list request (0x57C5F0) is
//     answered from those broadcasts through the game's own JSON parser.
//   - Create no longer waits for LocusGameChain to be synchronised.
#include "kufmod.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool g_lanPlay = true;
bool g_locusOnline = false;
bool g_lanTrace = false;
char g_lanIp[64] = "auto";
char g_lanPeers[256] = "";  // lan-peers: addresses to send room announcements to, besides broadcast
void InstallNetTrace();     // nettrace.cpp

static volatile bool g_lan;             // LAN chosen on the Multiplayer screen
static CRITICAL_SECTION g_lock;          // guards the room data below
static const USHORT BOOT_PORT = 54637, RELAY_PORT = 39999, ANNOUNCE_PORT = 9246;

// ---------------------------------------------------------------- which address is "mine"
// Preference: Radmin VPN (26.x.x.x) and Hamachi (25.x.x.x) first, then an adapter with a default
// gateway (the real LAN), then any other working adapter. Never 127.x or 169.254.x (no network).
// Also collects each adapter's broadcast address for the room announcements.
static char g_ip[32];
static ULONG g_broadcast[16];
static int g_numBroadcast;

static const char* PickAddress() {
    ULONG size = 32 * 1024;
    BYTE* buf = NULL;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(buf);
        buf = (BYTE*)malloc(size);
        if (!buf) return g_ip;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, NULL,
                                 (IP_ADAPTER_ADDRESSES*)buf, &size);
    }
    char best[32] = "";
    int bestScore = 0, nb = 0;
    ULONG bc[16];
    static char logged[512];
    char seen[512] = "";
    if (r == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = (IP_ADAPTER_ADDRESSES*)buf; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                if (u->DadState != IpDadStatePreferred) continue;
                ULONG addr = ((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr;
                const BYTE* b = (const BYTE*)&addr;
                if (b[0] == 127 || (b[0] == 169 && b[1] == 254) || b[0] == 0) continue;
                int prefix = u->OnLinkPrefixLength;
                if (prefix > 0 && prefix < 31 && nb < 16) {
                    ULONG mask = htonl(0xFFFFFFFFu << (32 - prefix));
                    bc[nb++] = addr | ~mask;
                }
                int score = (b[0] == 26 || b[0] == 25) ? 3 : a->FirstGatewayAddress ? 2 : 1;
                char ip[32];
                sprintf_s(ip, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
                char item[80];
                sprintf_s(item, " %s(%d)", ip, score);
                strcat_s(seen, item);
                if (score > bestScore) {
                    bestScore = score;
                    strcpy_s(best, ip);
                }
            }
        }
    }
    free(buf);
    if (*g_lanIp && _stricmp(g_lanIp, "auto")) strcpy_s(best, g_lanIp);
    if (*best) strcpy_s(g_ip, best);
    memcpy(g_broadcast, bc, nb * sizeof(ULONG));
    g_numBroadcast = nb;
    if (strcmp(seen, logged)) {  // log only when the adapters change
        strcpy_s(logged, seen);
        Log("lan: addresses (score):%s -> using %s", seen, *g_ip ? g_ip : "(none found)");
    }
    return g_ip;
}

static ULONG MyAddress() {
    IN_ADDR a = {};
    inet_pton(AF_INET, *g_ip ? g_ip : PickAddress(), &a);
    return a.s_addr;
}

static const char* IpText(ULONG addr, char* out, size_t size) {
    const BYTE* b = (const BYTE*)&addr;
    sprintf_s(out, size, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    return out;
}

// ---------------------------------------------------------------- packet checksum
// Every XNet and boot-server packet starts with a reflected CRC-32 (polynomial 0xD5828281) of the
// rest of the packet, big-endian (checked by the game at 0x4080A8).
static ULONG g_crcTable[256];

static void InitCrc() {
    for (ULONG i = 0; i < 256; i++) {
        ULONG c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xD5828281u : c >> 1;
        g_crcTable[i] = c;
    }
}

static ULONG Crc(const BYTE* p, int n) {
    ULONG c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = (c >> 8) ^ g_crcTable[(c ^ p[i]) & 0xFF];
    return ~c;
}

static void SetCrc(BYTE* p, int n) {
    ULONG c = Crc(p + 4, n - 4);
    p[0] = (BYTE)(c >> 24), p[1] = (BYTE)(c >> 16), p[2] = (BYTE)(c >> 8), p[3] = (BYTE)c;
}

// ---------------------------------------------------------------- boot server (room host)
// UDP 54637, packet = [crc 4][type 1][body]:
//   01 register (host) / 02 join: body = room key (32) + private address (6). Reply 04 to the
//      sender: 32 bytes + member count + sender's index + host index + 12 bytes per member
//      (public IP, port, private IP, port); for a join, also 05 (32 bytes + the new member's
//      12 bytes) to everyone already in the room.
//   03 leave: key.   06 keep-alive: key, reply 07 = key + 01.
// UDP 39999: "REG=..." -> "REG=a.b.c.d:port\r\n" (the sender's address); "RLY=" + key +
// destination (6) + "\r\n" + packet is the relay for players who cannot reach each other
// directly: forwarded unchanged to the destination.
// Every member's public and private address is the address its packets come from (loopback is
// replaced by this PC's LAN address).
struct Member { ULONG ip; USHORT port; };  // network order
struct Group { char key[32]; Member m[8]; int n; DWORD seen; };
static Group g_groups[8];
static SOCKET g_bootSock = INVALID_SOCKET, g_relaySock = INVALID_SOCKET, g_annSock = INVALID_SOCKET;

static void PutMember(BYTE* p, const Member& m) {
    memcpy(p, &m.ip, 4), memcpy(p + 4, &m.port, 2), memcpy(p + 6, &m.ip, 4), memcpy(p + 10, &m.port, 2);
}

static void SendTo(SOCKET s, const void* data, int n, ULONG ip, USHORT port) {
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    to.sin_port = port;
    sendto(s, (const char*)data, n, 0, (sockaddr*)&to, sizeof to);
}

static void SendMembers(const Group& g, int self, const sockaddr_in& to) {
    BYTE p[5 + 32 + 3 + 12 * 8] = {};
    p[4] = 0x04;
    p[37] = (BYTE)g.n, p[38] = (BYTE)self, p[39] = 0;  // the host is always member 0
    for (int i = 0; i < g.n; i++) PutMember(p + 40 + 12 * i, g.m[i]);
    int n = 40 + 12 * g.n;
    SetCrc(p, n);
    sendto(g_bootSock, (const char*)p, n, 0, (const sockaddr*)&to, sizeof to);
}

static void OnBootPacket(BYTE* p, int n, const sockaddr_in& from) {
    if (n < 5 || Crc(p + 4, n - 4) != ((ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3])) {
        Log("lan boot: bad packet (%d bytes)", n);
        return;
    }
    BYTE type = p[4];
    if (n < 37) return;
    const char* key = (const char*)p + 5;
    Member me = {from.sin_addr.s_addr, from.sin_port};
    if (((const BYTE*)&me.ip)[0] == 127) me.ip = MyAddress();
    char ip[32];
    EnterCriticalSection(&g_lock);
    for (Group& x : g_groups)  // rooms nobody kept alive for 30 s are gone
        if (x.n && GetTickCount() - x.seen > 30000) x.n = 0;
    Group* g = NULL;
    for (Group& x : g_groups)
        if (x.n && !memcmp(x.key, key, 32)) g = &x;
    if (type == 0x01 || type == 0x02) {
        if (!g && type == 0x01) {
            for (Group& x : g_groups)
                if (!x.n) { g = &x; break; }
            if (g) memcpy(g->key, key, 32);
        }
        if (!g) {
            Log("lan boot: join for an unknown room from %s", IpText(me.ip, ip, sizeof ip));
        } else {
            int self = -1;
            for (int i = 0; i < g->n; i++)
                if (g->m[i].ip == me.ip && g->m[i].port == me.port) self = i;
            if (self < 0 && g->n < 8) {
                self = g->n++;
                g->m[self] = me;
                for (int i = 0; i < self; i++) {  // 05: tell the others
                    BYTE a[5 + 32 + 12] = {};
                    a[4] = 0x05;
                    memcpy(a + 5, key, 32);  // the real server sends the room key here
                    PutMember(a + 37, me);
                    SetCrc(a, sizeof a);
                    SendTo(g_bootSock, a, sizeof a, g->m[i].ip, g->m[i].port);
                }
            }
            g->seen = GetTickCount();
            if (self >= 0) SendMembers(*g, self, from);
            Log("lan boot: %s %s:%d -> member %d of %d", type == 1 ? "register" : "join",
                IpText(me.ip, ip, sizeof ip), ntohs(me.port), self, g->n);
        }
    } else if (type == 0x03 && g) {
        for (int i = 0; i < g->n; i++)
            if (g->m[i].ip == me.ip && g->m[i].port == me.port) {
                memmove(&g->m[i], &g->m[i + 1], (g->n - i - 1) * sizeof(Member));
                g->n--;
                Log("lan boot: member %d left, %d remain", i, g->n);
                break;
            }
    } else if (type == 0x06) {
        BYTE a[5 + 32 + 1] = {};
        a[4] = 0x07;
        memcpy(a + 5, key, 32);
        a[37] = 1;
        SetCrc(a, sizeof a);
        sendto(g_bootSock, (const char*)a, sizeof a, 0, (const sockaddr*)&from, sizeof from);
        if (g) g->seen = GetTickCount();
    } else {
        Log("lan boot: unhandled type 0x%02X (%d bytes)", type, n);
    }
    LeaveCriticalSection(&g_lock);
}

static void OnRelayPacket(const char* p, int n, const sockaddr_in& from) {
    if (n >= 4 && !memcmp(p, "REG=", 4)) {
        ULONG addr = from.sin_addr.s_addr;
        if (((const BYTE*)&addr)[0] == 127) addr = MyAddress();
        char ip[32], reply[64];
        int len = sprintf_s(reply, "REG=%s:%d\r\n", IpText(addr, ip, sizeof ip), ntohs(from.sin_port));
        sendto(g_relaySock, reply, len, 0, (const sockaddr*)&from, sizeof from);
    } else if (n >= 42 && !memcmp(p, "RLY=", 4)) {
        // "RLY=" + room key (32) + destination IP (4) + port (2) + "\r\n" + an XNet packet:
        // forwarded unchanged to the destination (a keep-alive names the sender itself).
        ULONG ip;
        USHORT port;
        memcpy(&ip, p + 36, 4);
        memcpy(&port, p + 40, 2);
        SendTo(g_relaySock, p, n, ip, port);
    }
}

// ---------------------------------------------------------------- room announcements
// UDP 9246, "KUFLAN1\n" + the room's JSON, every 2 s while this PC hosts a room in LAN mode.
struct Room { ULONG from; DWORD time; char json[1500]; };
static Room g_rooms[16];
static char g_myRoom[1500];  // last room JSON the game produced (LAN mode)

static bool Hosting() {
    return g_lan && Game<uint32_t>(0xBED1FC) && Game<uint32_t>(0xBED22C);  // room server + room dialog
}

static void Announce() {
    char msg[1600];
    EnterCriticalSection(&g_lock);
    int len = *g_myRoom ? sprintf_s(msg, "KUFLAN1\n%s", g_myRoom) : 0;
    LeaveCriticalSection(&g_lock);
    if (!len) return;
    SendTo(g_annSock, msg, len, INADDR_BROADCAST, htons(ANNOUNCE_PORT));
    for (int i = 0; i < g_numBroadcast; i++) SendTo(g_annSock, msg, len, g_broadcast[i], htons(ANNOUNCE_PORT));
    char peers[256];
    strcpy_s(peers, g_lanPeers);
    char* next = NULL;
    for (char* t = strtok_s(peers, ", ", &next); t; t = strtok_s(NULL, ", ", &next)) {
        IN_ADDR a;
        if (inet_pton(AF_INET, t, &a) == 1) SendTo(g_annSock, msg, len, a.s_addr, htons(ANNOUNCE_PORT));
    }
}

// The room's "ip" value ("26.1.2.3:9237"): identifies the host. One announcement can arrive
// several times (one per network adapter), so rooms are matched by it, not by sender.
static void HostOf(const char* json, char* out, size_t size) {
    *out = 0;
    const char* p = strstr(json, "\"ip\":\"");
    if (!p) return;
    p += 6;
    size_t i = 0;
    while (p[i] && p[i] != '"' && i + 1 < size) out[i] = p[i], i++;
    out[i] = 0;
}

// ---------------------------------------------------------------- lobby users
// Every LAN lobby sends "KUFLAN1U\n<id hex> <name>" every 2 s on UDP 9246. The lobby's user list
// lives in the presence object 0xBEBB40: 0x556D70(name, id, 0, quiet) adds a user ("entered the
// lobby" unless quiet), 0x557120(name, id) removes one ("left the lobby"). Those are UI-thread
// calls, so the network thread only records who it heard and SyncUsers (on the UI thread, from
// the room-list request) applies the changes. Locus's own channel events (0x6CCB90) are ignored
// in LAN mode; they made players flicker in and out.
struct User { uint32_t id; char name[32]; DWORD seen; bool shown; };
static User g_users[16];
static void* g_userLobby;  // the lobby the users were added to

static bool InLobby() {
    return g_lan && Game<uint32_t>(0xBED204) && !Game<uint32_t>(0xBED22C);
}

static void SendPresence() {
    char msg[80];
    int len = sprintf_s(msg, "KUFLAN1U\n%08X %.31s", Game<uint32_t>(0xBEDA60), (const char*)(0xBEDA40 + g_slide));
    SendTo(g_annSock, msg, len, INADDR_BROADCAST, htons(ANNOUNCE_PORT));
    for (int i = 0; i < g_numBroadcast; i++) SendTo(g_annSock, msg, len, g_broadcast[i], htons(ANNOUNCE_PORT));
    char peers[256];
    strcpy_s(peers, g_lanPeers);
    char* next = NULL;
    for (char* t = strtok_s(peers, ", ", &next); t; t = strtok_s(NULL, ", ", &next)) {
        IN_ADDR a;
        if (inet_pton(AF_INET, t, &a) == 1) SendTo(g_annSock, msg, len, a.s_addr, htons(ANNOUNCE_PORT));
    }
}

static void OnPresence(const char* p, int n) {
    char text[64];
    int len = n - 9 < (int)sizeof text - 1 ? n - 9 : (int)sizeof text - 1;
    memcpy(text, p + 9, len);
    text[len] = 0;
    char* end = NULL;
    uint32_t id = strtoul(text, &end, 16);
    if (!id || !end || *end != ' ' || id == Game<uint32_t>(0xBEDA60)) return;
    EnterCriticalSection(&g_lock);
    User* slot = NULL;
    for (User& u : g_users)
        if (u.seen && u.id == id) slot = &u;
    if (!slot)
        for (User& u : g_users)
            if (!u.seen) { slot = &u; break; }
    if (slot) {
        if (!slot->seen) {
            slot->id = id;
            strncpy_s(slot->name, end + 1, _TRUNCATE);
            slot->shown = false;
        }
        slot->seen = GetTickCount();
    }
    LeaveCriticalSection(&g_lock);
}

typedef void(__thiscall* AddUserFn)(void* self, const char* name, uint32_t id, int unused, int quiet);
typedef void(__thiscall* RemoveUserFn)(void* self, const char* name, uint32_t id);
// 0x557CD0(name, id, time): keeps a user listed until time + 33 s (GetTickCount64 clock); the
// lobby drops users that are not refreshed. Locus refreshed them with every channel update.
typedef void(__thiscall* RefreshUserFn)(void* self, const char* name, uint32_t id, uint32_t timeLow, uint32_t timeHigh);

static void SyncUsers() {  // UI thread
    void* presence = (void*)(0xBEBB40 + g_slide);
    void* lobby = Game<void*>(0xBED204);
    EnterCriticalSection(&g_lock);
    if (lobby != g_userLobby) {  // a new lobby screen starts with an empty list
        g_userLobby = lobby;
        for (User& u : g_users) u.shown = false;
    }
    for (User& u : g_users) {
        if (!u.seen) continue;
        bool gone = GetTickCount() - u.seen > 8000;
        char name[32];
        strcpy_s(name, u.name);
        uint32_t id = u.id;
        if (gone) {
            bool shown = u.shown;
            u = User{};
            LeaveCriticalSection(&g_lock);
            if (shown) ((RemoveUserFn)(0x557120 + g_slide))(presence, name, id);
            Log("lan: %s <%08X> left the lobby", name, id);
            EnterCriticalSection(&g_lock);
        } else {
            bool added = !u.shown;
            u.shown = true;
            LeaveCriticalSection(&g_lock);
            if (added) {
                ((AddUserFn)(0x556D70 + g_slide))(presence, name, id, 0, 0);
                Log("lan: %s <%08X> entered the lobby", name, id);
            }
            ULONGLONG now = GetTickCount64();
            ((RefreshUserFn)(0x557CD0 + g_slide))(presence, name, id, (uint32_t)now, (uint32_t)(now >> 32));
            EnterCriticalSection(&g_lock);
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void OnAnnouncement(const char* p, int n, const sockaddr_in& from) {
    if (n > 9 && !memcmp(p, "KUFLAN1U\n", 9)) {
        OnPresence(p, n);
        return;
    }
    if (n < 9 || memcmp(p, "KUFLAN1\n", 8) || n - 8 >= (int)sizeof g_rooms[0].json) return;
    char json[1500], host[64], other[64];
    memcpy(json, p + 8, n - 8);
    json[n - 8] = 0;
    HostOf(json, host, sizeof host);
    if (!*host) return;
    EnterCriticalSection(&g_lock);
    Room* slot = NULL;
    for (Room& r : g_rooms)
        if (r.time && (HostOf(r.json, other, sizeof other), !strcmp(other, host))) slot = &r;
    if (!slot)
        for (Room& r : g_rooms)
            if (!r.time || GetTickCount() - r.time > 7000) { slot = &r; break; }
    if (slot) {
        if (!slot->time || GetTickCount() - slot->time > 7000) {
            char ip[32];
            Log("lan: room of %s announced (received from %s)", host, IpText(from.sin_addr.s_addr, ip, sizeof ip));
        }
        slot->from = from.sin_addr.s_addr;
        slot->time = GetTickCount();
        memcpy(slot->json, p + 8, n - 8);
        slot->json[n - 8] = 0;
    }
    LeaveCriticalSection(&g_lock);
}

static SOCKET Bind(USHORT port, bool broadcast) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    BOOL on = TRUE;
    if (broadcast) {
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof a)) {
        Log("lan: could not open UDP port %d (error %d)", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static DWORD WINAPI LanThread(LPVOID) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    PickAddress();
    g_bootSock = Bind(BOOT_PORT, false);
    g_relaySock = Bind(RELAY_PORT, false);
    g_annSock = Bind(ANNOUNCE_PORT, true);
    DWORD lastAnnounce = 0, lastAddress = GetTickCount();
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        SOCKET all[3] = {g_bootSock, g_relaySock, g_annSock};
        for (SOCKET s : all)
            if (s != INVALID_SOCKET) FD_SET(s, &rd);
        timeval tv = {0, 250000};
        if (rd.fd_count && select(0, &rd, NULL, NULL, &tv) > 0) {
            for (SOCKET s : all) {
                if (s == INVALID_SOCKET || !FD_ISSET(s, &rd)) continue;
                char buf[2048];
                sockaddr_in from = {};
                int fl = sizeof from;
                int n = recvfrom(s, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
                if (n <= 0) continue;
                if (s == g_bootSock) OnBootPacket((BYTE*)buf, n, from);
                else if (s == g_relaySock) OnRelayPacket(buf, n, from);
                else OnAnnouncement(buf, n, from);
            }
        } else if (!rd.fd_count) {
            Sleep(250);
        }
        DWORD now = GetTickCount();
        if (now - lastAddress > 10000) PickAddress(), lastAddress = now;
        if (now - lastAnnounce > 2000) {
            lastAnnounce = now;
            if (Hosting()) Announce();
            if (InLobby()) SendPresence();
        }
    }
}

// ---------------------------------------------------------------- redirecting XNet's boot traffic
// Called by nettrace.cpp for every packet XNet sends and receives.
static ULONG g_target;    // host of the room being created or joined (network order)
static ULONG g_bootAddr;  // the real boot server's address, as XNet knows it

// Returns true and fills *out when the packet must go somewhere else.
bool LanRedirect(const sockaddr* to, sockaddr_in* out) {
    if (!g_lan || !to || to->sa_family != AF_INET) return false;
    const sockaddr_in* in = (const sockaddr_in*)to;
    USHORT port = ntohs(in->sin_port);
    if (port != BOOT_PORT && port != RELAY_PORT) return false;
    if (in->sin_addr.s_addr == g_target) return false;
    g_bootAddr = in->sin_addr.s_addr;
    *out = *in;
    if (g_target) {
        out->sin_addr.s_addr = g_target;
    } else {  // no room yet: somewhere harmless
        inet_pton(AF_INET, "127.0.0.1", &out->sin_addr);
        out->sin_port = htons(9);
    }
    return true;
}

// Replies from the room host's boot server are made to look as if the real one sent them.
void LanFixSource(sockaddr* from) {
    if (!g_lan || !from || from->sa_family != AF_INET || !g_target || !g_bootAddr) return;
    sockaddr_in* in = (sockaddr_in*)from;
    USHORT port = ntohs(in->sin_port);
    if ((port == BOOT_PORT || port == RELAY_PORT) && in->sin_addr.s_addr == g_target) in->sin_addr.s_addr = g_bootAddr;
}

// 0x6D8420 (room client start): the room is known here. As host, the boot server is this PC;
// joining, it is the room's host, whose address is in the selected room's info
// ([0xBED204]+0x28C, a 0x208-byte struct; address text at +0x54).
static uintptr_t g_clientStartResume;

static void __stdcall OnClientStart() {
    if (!g_lan) return;
    char ip[32];
    if (Game<uint32_t>(0xBED1FC)) {
        g_target = MyAddress();
        Log("lan: hosting a room, boot server on this PC (%s)", IpText(g_target, ip, sizeof ip));
        return;
    }
    const char* info = (const char*)(Game<uintptr_t>(0xBED204) + 0x28C);
    char host[64] = "";
    for (int i = 0; i < 63 && info[0x54 + i] && info[0x54 + i] != ':'; i++) host[i] = info[0x54 + i], host[i + 1] = 0;
    IN_ADDR a;
    if (inet_pton(AF_INET, host, &a) == 1) g_target = a.s_addr;
    Log("lan: joining room at \"%s\" (fields: \"%.30s\" \"%.30s\" \"%.30s\" port \"%.10s\")", host, info + 0xE,
        info + 0x2E, info + 0x1DA, info + 0xB8);
}

__declspec(naked) static void ClientStartHook() {  // replaces push ebp / mov ebp,esp / sub esp,0x254
    __asm {
        pushad
        call OnClientStart
        popad
        push ebp
        mov ebp, esp
        sub esp, 0x254
        jmp dword ptr [g_clientStartResume]
    }
}

// ---------------------------------------------------------------- the room list
// 0x57FA91, inside 0x57F5A0 (the room's JSON for LocusGameChain): eax = the JSON text. In LAN
// mode the JSON is only kept for the announcements: the hook skips the hand-over to
// LocusGameChain (shared memory + SendMessage 0x3414) and continues at 0x57FB0B with eax = 0,
// which the function reports as success.
static uintptr_t g_roomJsonResume, g_roomJsonSkip;

static void __stdcall OnRoomJson(const char* json) {
    if (!g_lan || !json || !strstr(json, "\"KUF\"")) return;
    EnterCriticalSection(&g_lock);
    bool changed = strcmp(g_myRoom, json) != 0;
    strncpy_s(g_myRoom, json, _TRUNCATE);
    LeaveCriticalSection(&g_lock);
    if (changed) Log("lan: my room: %.300s", json);
}

__declspec(naked) static void RoomJsonHook() {  // replaces xor esi,esi / mov [ebp-0x474],eax
    __asm {
        pushad
        push eax
        call OnRoomJson
        popad
        xor esi, esi
        mov dword ptr [ebp - 0x474], eax
        cmp byte ptr [g_lan], 0
        jne lan
        jmp dword ptr [g_roomJsonResume]
    lan:
        xor eax, eax
        jmp dword ptr [g_roomJsonSkip]
    }
}

// ---------------------------------------------------------------- Registry without a password
// The Registry's OK (0x72780F, button 0xBDF) asks for the profile's Locus account password
// (0x7278DB opens the box) unless that profile is already loaded, in which case it passes the OK
// on (0x72793E) and the lobby opens. In LAN mode it always passes it on: no account is needed.
// The player's name and ID normally come with the account: the current player record is at
// 0xBEDA40 (name, 32 bytes) with the ID at 0xBEDA60; the room server refuses an ID of 0 ("Server
// is Full", 0x6D2A00) and the lobby shows "<00000000>". multi\<name>\account.dat is not
// encrypted, so in LAN mode we load it the way the account loader does: 0x5A4A40 (empty record),
// 0x5AC270(name, record) (read account.dat), 0x5A5FA0 (copy into the current player), 0x5A55F0
// (free the temporary record).
static uintptr_t g_passwordResume, g_passwordSkip;
typedef void(__thiscall* RecordFn)(void* self);
typedef void(__thiscall* CopyRecordFn)(void* self, void* from);
typedef bool(__cdecl* LoadAccountFn)(const char* name, void* record);

static void __stdcall LanLoadProfile(const char* name) {
    static BYTE record[0x1000];
    memset(record, 0, sizeof record);
    ((RecordFn)(0x5A4A40 + g_slide))(record);
    bool ok = ((LoadAccountFn)(0x5AC270 + g_slide))(name, record);
    if (ok) {
        ((CopyRecordFn)(0x5A5FA0 + g_slide))((void*)(0xBEDA40 + g_slide), record);
        *(BYTE*)(0xBEDC08 + g_slide) = 0;
    }
    ((RecordFn)(0x5A55F0 + g_slide))(record);
    char* current = (char*)(0xBEDA40 + g_slide);
    uint32_t* id = (uint32_t*)(0xBEDA60 + g_slide);
    if (!ok || !*id || _stricmp(current, name)) {  // no usable account.dat: name + an ID made from it
        strncpy_s(current, 32, name, _TRUNCATE);
        char pc[64] = "";
        DWORD n = sizeof pc;
        GetComputerNameA(pc, &n);
        uint32_t h = 2166136261u;
        for (const char* s = name; *s; s++) h = (h ^ (BYTE)*s) * 16777619u;
        for (const char* s = pc; *s; s++) h = (h ^ (BYTE)*s) * 16777619u;
        *id = h ? h : 1;
    }
    Log("lan: profile \"%s\" (%s), id %08X", current, ok ? "from account.dat" : "made up", *id);
}

__declspec(naked) static void PasswordHook() {  // replaces push 0 / push 0xCA
    __asm {
        cmp byte ptr [g_lan], 0
        jne lan
        push 0
        push 0xCA
        jmp dword ptr [g_passwordResume]
    lan:
        pushad
        push dword ptr [ebp - 0x218]      // the chosen profile's name
        call LanLoadProfile
        popad
        jmp dword ptr [g_passwordSkip]
    }
}

// ---------------------------------------------------------------- lobby chat text
// 0x6C0470(state) puts the Locus welcome text into the lobby chat (through 0x6CCF50 on
// [0xBED204]); 0x6CCFD0 (thiscall, 1 argument) adds LocusGameChain's "Node Status" lines. In
// LAN mode the first shows our own text and the second does nothing.
static uintptr_t g_welcomeResume, g_welcomeShow, g_mainData, g_nodeStatusResume;
static const char LAN_WELCOME[] =
    "Welcome to Kingdom Under Fire.\n\n"
    "LAN play: rooms are shared with the players on your network (a LAN, or a virtual LAN such as "
    "Radmin VPN or Hamachi). Every player needs the mod with lan-play on.\n\n"
    "Create a room, or join one from the list.\n\n";

__declspec(naked) static void WelcomeHook() {  // replaces push ebp / mov ebp,esp / mov ecx,0xB826F0
    __asm {
        cmp byte ptr [g_lan], 0
        jne lan
        push ebp
        mov ebp, esp
        mov ecx, dword ptr [g_mainData]
        jmp dword ptr [g_welcomeResume]   // 0x6C0478
    lan:
        mov eax, offset LAN_WELCOME
        mov dword ptr [esp + 4], eax
        mov eax, 0xBED204
        add eax, dword ptr [g_slide]
        mov ecx, dword ptr [eax]
        jmp dword ptr [g_welcomeShow]     // 0x6CCF50 (thiscall, 1 argument)
    }
}

__declspec(naked) static void NodeStatusHook() {  // replaces push ebp / mov ebp,esp / sub esp,0x10C
    __asm {
        cmp byte ptr [g_lan], 0
        jne lan
        push ebp
        mov ebp, esp
        sub esp, 0x10C
        jmp dword ptr [g_nodeStatusResume]
    lan:
        ret 4
    }
}

// ---------------------------------------------------------------- lobby status box
// 0x6C04C0 (thiscall, 3 arguments) works out the Locus state and shows it in the lobby through
// the lobby server object [0xBED204]: 0x6CCF30 color, 0x6CCEF0 status box, 0x6CCF10 message
// line, 0x6CCF70 when ready. In LAN mode it shows LAN instead.
typedef void(__thiscall* TextFn)(void* self, const char* text);
typedef void(__thiscall* ColorFn)(void* self, int color);
typedef void(__thiscall* VoidFn)(void* self);
static uintptr_t g_statusResume;

static void __stdcall LanStatus() {
    static void* readyLobby;  // 0x6CCF70 also puts this player into the user list: once per lobby
    void* lobby = Game<void*>(0xBED204);
    if (!lobby) return;
    ((ColorFn)(0x6CCF30 + g_slide))(lobby, 0xFF00);
    ((TextFn)(0x6CCEF0 + g_slide))(lobby, "LAN play");
    ((TextFn)(0x6CCF10 + g_slide))(lobby, "Create or Join a Game");
    if (lobby != readyLobby) {
        readyLobby = lobby;
        ((VoidFn)(0x6CCF70 + g_slide))(lobby);
    }
}

static uintptr_t g_channelResume;

__declspec(naked) static void ChannelHook() {  // 0x6CCB90: replaces push ebp / mov ebp,esp / push -1
    __asm {
        cmp byte ptr [g_lan], 0
        jne lan
        push ebp
        mov ebp, esp
        push -1
        jmp dword ptr [g_channelResume]
    lan:
        ret 4                             // Locus lobby-channel events are ignored in LAN mode
    }
}

__declspec(naked) static void StatusHook() {  // replaces push ebp / mov ebp,esp / push -1
    __asm {
        cmp byte ptr [g_lan], 0
        jne lan
        push ebp
        mov ebp, esp
        push -1
        jmp dword ptr [g_statusResume]
    lan:
        pushad
        call LanStatus
        popad
        ret 0xC
    }
}

// 0x57C5F0: bool __cdecl RequestRoomList(void (*callback)(records, count)). In LAN mode the
// list is built from the announcements and run through the game's parser 0x57FD90; the records
// (0x120 bytes each, count before them) are freed the way the original does.
typedef void(__cdecl* RoomCallback)(void* records, int count);
typedef void*(__cdecl* ParseFn)(int* count, char* text);
typedef void(__stdcall* VecDtorFn)(void* p, int size, int count, void* dtor);
typedef void(__cdecl* DeleteFn)(void* p, int size);
static uintptr_t g_roomListResume;
static int g_lastRoomCount = -1;

static bool __cdecl LanRoomList(RoomCallback callback) {
    static char text[16 * 1500 + 8];
    int len = 0, count = 0;
    text[len++] = '[';
    EnterCriticalSection(&g_lock);
    for (const Room& r : g_rooms) {
        if (!r.time || GetTickCount() - r.time > 7000) continue;
        if (count++) text[len++] = ',';
        int n = (int)strlen(r.json);
        memcpy(text + len, r.json, n);
        len += n;
    }
    LeaveCriticalSection(&g_lock);
    text[len++] = ']';
    text[len] = 0;
    if (count != g_lastRoomCount) Log("lan: lobby room list: %d room(s)", count), g_lastRoomCount = count;
    SyncUsers();
    int parsed = 0;
    void* records = ((ParseFn)(0x57FD90 + g_slide))(&parsed, text);
    if (callback) callback(records, parsed);
    if (records) {
        int n = ((int*)records)[-1];
        ((VecDtorFn)(0x922B2B + g_slide))(records, 0x120, n, (void*)(0x57BCA0 + g_slide));
        ((DeleteFn)(0x922C6F + g_slide))((int*)records - 1, n * 0x120 + 4);
    }
    return true;
}

__declspec(naked) static void RoomListHook() {  // replaces push ebp / mov ebp,esp / push -1
    __asm {
        cmp byte ptr [g_lan], 0
        je locus
        jmp LanRoomList
    locus:
        push ebp
        mov ebp, esp
        push -1
        jmp dword ptr [g_roomListResume]
    }
}

// ---------------------------------------------------------------- Create without LocusGameChain
// Create (0x6E05EF) needs 0x57D1A0() >= 2 and 0x57C4E0() >= 6, both asked from the LocusGameChain
// window. In LAN mode they report ready.
typedef int(__cdecl* IntFn)();
static int __cdecl ChainState1() { return g_lan ? 2 : ((IntFn)(0x57D1A0 + g_slide))(); }
static int __cdecl ChainState2() { return g_lan ? 6 : ((IntFn)(0x57C4E0 + g_slide))(); }

static bool HookCallTo(uintptr_t site, uintptr_t original, void* target) {
    BYTE call[5] = {0xE8};
    int32_t rel = (int32_t)(original - (site + 5));
    memcpy(call + 1, &rel, 4);
    return HookCall(site, call, 5, target);
}

// ---------------------------------------------------------------- gethostbyname
// The boot server's name is looked up once at game start. Without internet that fails, and Locus
// may give up on its network layer; we then answer with a reserved address (192.0.2.1) that LAN
// mode redirects anyway.
typedef hostent*(WINAPI* GetHostByNameFn)(const char*);
static GetHostByNameFn g_realGetHostByName;

static hostent* WINAPI HostByName(const char* name) {
    hostent* h = g_realGetHostByName(name);
    if (h || !name || _stricmp(name, "gamechain.locuschain.com")) return h;
    static IN_ADDR fake;
    static char* list[2] = {(char*)&fake, NULL};
    static hostent answer;
    inet_pton(AF_INET, "192.0.2.1", &fake);
    answer.h_name = (char*)name;
    answer.h_addrtype = AF_INET;
    answer.h_length = 4;
    answer.h_addr_list = list;
    Log("lan: %s not found (offline?), using a placeholder address", name);
    return &answer;
}

static bool HookGetHostByName() {
    void** iat = (void**)(0xA16D78 + g_slide);
    void* real = NULL;
    const char* dlls[2] = {"WSOCK32.dll", "WS2_32.dll"};  // the import may be forwarded
    for (const char* dll : dlls) {
        HMODULE m = GetModuleHandleA(dll);
        void* f = m ? (void*)GetProcAddress(m, "gethostbyname") : NULL;
        if (f && *iat == f) real = f;
    }
    if (!real) return false;
    g_realGetHostByName = (GetHostByNameFn)real;
    DWORD old;
    if (!VirtualProtect(iat, 4, PAGE_READWRITE, &old)) return false;
    *iat = (void*)HostByName;
    VirtualProtect(iat, 4, old, &old);
    return true;
}

// ---------------------------------------------------------------- screen state log
// 0x58D120 (main frame, ecx = this): void SetState(int state) - stores it at this+0x128.
static uintptr_t g_stateResume;
static int g_lastState = -1;

static void __stdcall LogState(int state) {
    if (state != g_lastState) Log("lan: screen state %d -> %d (0x%X)", g_lastState, state, state);
    g_lastState = state;
}

__declspec(naked) static void StateHook() {  // replaces push ebp / mov ebp,esp / mov eax,[ebp+8]
    __asm {
        pushad
        push dword ptr [esp + 0x24]  // the state argument (above the 8 saved registers + return)
        call LogState
        popad
        push ebp
        mov ebp, esp
        mov eax, [ebp + 8]
        jmp dword ptr [g_stateResume]
    }
}

// ---------------------------------------------------------------- Locus Online / LAN screen
// Screen state 0x19 shows the CD version's "WarGate / LAN / Cancel" menu (table 0xB5B040). All
// three hooks are inside the main frame's command handler (0x590A90): edi = main frame,
// 0x591A8E = its common exit.
static uintptr_t g_setState, g_handlerExit, g_locusChecks, g_lobbyResume, g_locusPtr;
static BYTE g_locusChosen;  // set by Locus Online until its checks reach command 0xBBA

static uintptr_t g_lobbyHookAddr;  // LobbyHook below

__declspec(naked) static void MultiplayerHook() {  // at 0x590AFC: show the chooser (state 0x19)
    __asm {
        cmp byte ptr [g_locusOnline], 0
        je lanOnly
        push 0x19
        mov ecx, edi
        call dword ptr [g_setState]
        jmp dword ptr [g_handlerExit]
    lanOnly:                              // locus-online: false: straight to LAN, as if LAN was clicked
        jmp dword ptr [g_lobbyHookAddr]   // (command 0xBBA's case in the same handler: edi = frame)
    }
}

__declspec(naked) static void LocusOnlineHook() {  // at 0x59120D (button 3001, was WarGate)
    __asm {
        mov byte ptr [g_locusChosen], 1
        mov eax, dword ptr [g_locusPtr]
        mov eax, [eax]                    // mov eax,[0xBED200]: the instruction MultiplayerHook replaced
        jmp dword ptr [g_locusChecks]     // 0x590B01: Steam's Locus checks, then command 0xBBA
    }
}

static void __stdcall ChooseNetwork() {
    g_lan = !g_locusChosen;
    Log("lan: %s chosen", g_lan ? "LAN" : "Locus Online");
    g_locusChosen = 0;
    if (g_lan) PickAddress();
}

__declspec(naked) static void LobbyHook() {  // at 0x591263 (command 0xBBA): push 0x32 / call SetState
    __asm {
        call ChooseNetwork
        push 0x32
        mov ecx, edi
        call dword ptr [g_setState]
        jmp dword ptr [g_lobbyResume]     // 0x59126A
    }
}

static bool InstallChooser() {
    uint32_t locus = (uint32_t)(0xBED200 + g_slide);
    BYTE checks[5] = {0xA1};
    memcpy(checks + 1, &locus, 4);
    static const BYTE wargate[10] = {0xC7, 0x87, 0x28, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
    static const BYTE lobby[7] = {0x6A, 0x32, 0xE8, 0xB6, 0xBE, 0xFF, 0xFF};
    if (memcmp((void*)(0x590AFC + g_slide), checks, 5) || memcmp((void*)(0x59120D + g_slide), wargate, 10) ||
        memcmp((void*)(0x591263 + g_slide), lobby, 7))
        return false;
    g_setState = 0x58D120 + g_slide;
    g_handlerExit = 0x591A8E + g_slide;
    g_locusChecks = 0x590B01 + g_slide;
    g_locusPtr = 0xBED200 + g_slide;
    g_lobbyHookAddr = (uintptr_t)LobbyHook;
    uintptr_t unused;
    Hook(0x590AFC, checks, 5, MultiplayerHook, &unused);
    Hook(0x59120D, wargate, 10, LocusOnlineHook, &unused);
    Hook(0x591263, lobby, 7, LobbyHook, &g_lobbyResume);
    // Button labels (may only be used for the hotkey; the screen's picture may hold its own text).
    Patch(0xB5B1D4, "&WARGATE\0\0\0\0\0\0", "&Locus Online", 14);
    Patch(0xB5B238, "&LOCUS", "&LAN\0\0", 6);
    return true;
}

static bool InstallLanHooks() {
    static const BYTE clientStart[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x54, 0x02, 0x00, 0x00};
    static const BYTE roomJson[8] = {0x33, 0xF6, 0x89, 0x85, 0x8C, 0xFB, 0xFF, 0xFF};
    static const BYTE roomList[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    BYTE call1[5] = {0xE8}, call2[5] = {0xE8};
    int32_t r1 = (int32_t)(0x57D1A0 - (0x6E05EF + 5)), r2 = (int32_t)(0x57C4E0 - (0x6E05F9 + 5));
    memcpy(call1 + 1, &r1, 4);
    memcpy(call2 + 1, &r2, 4);
    static const BYTE password[7] = {0x6A, 0x00, 0x68, 0xCA, 0x00, 0x00, 0x00};
    static const BYTE status[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    if (memcmp((void*)(0x6D8420 + g_slide), clientStart, 9) || memcmp((void*)(0x57FA91 + g_slide), roomJson, 8) ||
        memcmp((void*)(0x57C5F0 + g_slide), roomList, 5) || memcmp((void*)(0x6E05EF + g_slide), call1, 5) ||
        memcmp((void*)(0x6E05F9 + g_slide), call2, 5) || memcmp((void*)(0x7278DB + g_slide), password, 7) ||
        memcmp((void*)(0x6C04C0 + g_slide), status, 5))
        return false;
    g_roomJsonSkip = 0x57FB0B + g_slide;
    g_passwordSkip = 0x72793E + g_slide;
    Hook(0x7278DB, password, 7, PasswordHook, &g_passwordResume);
    Hook(0x6C04C0, status, 5, StatusHook, &g_statusResume);
    // welcome text: push ebp / mov ebp,esp / mov ecx,0xB826F0 (relocated)
    uint32_t mainData = (uint32_t)(0xB826F0 + g_slide);
    BYTE welcome[8] = {0x55, 0x8B, 0xEC, 0xB9};
    memcpy(welcome + 4, &mainData, 4);
    static const BYTE nodeStatus[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x0C, 0x01, 0x00, 0x00};
    g_mainData = mainData;
    g_welcomeShow = 0x6CCF50 + g_slide;
    if (!Hook(0x6C0470, welcome, 8, WelcomeHook, &g_welcomeResume)) Log("lan: unexpected code at 0x6C0470, welcome text unchanged");
    if (!Hook(0x6CCFD0, nodeStatus, 9, NodeStatusHook, &g_nodeStatusResume)) Log("lan: unexpected code at 0x6CCFD0, node status unchanged");
    static const BYTE channel[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    if (!Hook(0x6CCB90, channel, 5, ChannelHook, &g_channelResume)) Log("lan: unexpected code at 0x6CCB90, Locus lobby users not blocked");
    Hook(0x6D8420, clientStart, 9, ClientStartHook, &g_clientStartResume);
    Hook(0x57FA91, roomJson, 8, RoomJsonHook, &g_roomJsonResume);
    Hook(0x57C5F0, roomList, 5, RoomListHook, &g_roomListResume);
    HookCallTo(0x6E05EF, 0x57D1A0, ChainState1);
    HookCallTo(0x6E05F9, 0x57C4E0, ChainState2);
    return true;
}

void InstallLan() {
    if (!g_lanPlay) return;
    InitializeCriticalSection(&g_lock);
    InitCrc();
    static const BYTE stateCode[6] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08};
    if (!Hook(0x58D120, stateCode, sizeof stateCode, StateHook, &g_stateResume))
        Log("lan: unexpected game code at 0x58D120, no state log");
    if (!InstallChooser() || !InstallLanHooks()) {
        Log("lan: unexpected game code for the Multiplayer screens, LAN play left off");
        return;
    }
    if (!HookGetHostByName()) Log("lan: could not hook gethostbyname");
    if (g_instance > 1) {  // a second copy on this PC: XNet on port 9238 (9239...) instead of 9237
        USHORT port = (USHORT)(9237 + g_instance - 1);
        static const BYTE original[5] = {0xB8, 0x15, 0x24, 0x00, 0x00};  // 0x6BCDF1: mov eax, 9237
        BYTE changed[5] = {0xB8, (BYTE)port, (BYTE)(port >> 8), 0x00, 0x00};
        Log(Patch(0x6BCDF1, original, changed, 5) ? "lan: instance %d, network port %d"
                                                    : "lan: instance %d, could not change the port to %d",
            g_instance, port);
    }
    InstallNetTrace();
    HANDLE t = CreateThread(NULL, 0, LanThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    Log("lan: Multiplayer %s, address setting \"%s\", extra peers \"%s\"",
        g_locusOnline ? "shows Locus Online / LAN" : "goes straight to LAN (locus-online: false)", g_lanIp,
        g_lanPeers);
}
