// nettrace.cpp - network hooks for LAN play (installed by InstallLan).
//   - Always: WS2_32 WSASendTo / WSARecvFrom and GetQueuedCompletionStatus, which LAN mode uses
//     to redirect XNet's boot-server traffic (lan.cpp: LanRedirect, LanFixSource).
//   - With the setting lan-trace (for debugging): the game's WSOCK32 calls are logged to
//     kufmod.log and every XNet packet is written to netdump.txt.
#include "kufmod.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <share.h>

static volatile LONG g_lines;
static const LONG MAX_LINES = 400;  // keep the log readable; sends/receives stop being logged after this

static void Addr(const sockaddr* sa, char* out, size_t size) {
    if (sa && sa->sa_family == AF_INET) {
        const sockaddr_in* in = (const sockaddr_in*)sa;
        const BYTE* b = (const BYTE*)&in->sin_addr;
        sprintf_s(out, size, "%d.%d.%d.%d:%d", b[0], b[1], b[2], b[3], ntohs(in->sin_port));
    } else {
        sprintf_s(out, size, "?");
    }
}

#define TRACE(...)                                                  \
    do {                                                            \
        int saved = WSAGetLastError();                              \
        if (InterlockedIncrement(&g_lines) <= MAX_LINES) Log(__VA_ARGS__); \
        WSASetLastError(saved);                                     \
    } while (0)

typedef SOCKET(WINAPI* SocketFn)(int, int, int);
typedef int(WINAPI* SockAddrFn)(SOCKET, const sockaddr*, int);
typedef int(WINAPI* ListenFn)(SOCKET, int);
typedef SOCKET(WINAPI* AcceptFn)(SOCKET, sockaddr*, int*);
typedef int(WINAPI* CloseFn)(SOCKET);
typedef int(WINAPI* SendFn)(SOCKET, const char*, int, int);
typedef int(WINAPI* RecvFn)(SOCKET, char*, int, int);
typedef int(WINAPI* SendToFn)(SOCKET, const char*, int, int, const sockaddr*, int);
typedef int(WINAPI* RecvFromFn)(SOCKET, char*, int, int, sockaddr*, int*);
typedef int(WINAPI* AsyncSelectFn)(SOCKET, HWND, unsigned, long);

static SocketFn o_socket;
static SockAddrFn o_bind, o_connect;
static ListenFn o_listen;
static AcceptFn o_accept;
static CloseFn o_close;
static SendFn o_send;
static RecvFn o_recv;
static SendToFn o_sendto;
static RecvFromFn o_recvfrom;
static AsyncSelectFn o_async;

static SOCKET WINAPI T_socket(int af, int type, int proto) {
    SOCKET s = o_socket(af, type, proto);
    TRACE("net: socket(type %d) = %u", type, (unsigned)s);
    return s;
}
static int WINAPI T_bind(SOCKET s, const sockaddr* a, int n) {
    int r = o_bind(s, a, n);
    char t[40];
    Addr(a, t, sizeof t);
    TRACE("net: bind(%u, %s) = %d err %d", (unsigned)s, t, r, r ? WSAGetLastError() : 0);
    return r;
}
static int WINAPI T_connect(SOCKET s, const sockaddr* a, int n) {
    int r = o_connect(s, a, n);
    char t[40];
    Addr(a, t, sizeof t);
    TRACE("net: connect(%u, %s) = %d err %d", (unsigned)s, t, r, r ? WSAGetLastError() : 0);
    return r;
}
static int WINAPI T_listen(SOCKET s, int backlog) {
    int r = o_listen(s, backlog);
    TRACE("net: listen(%u) = %d err %d", (unsigned)s, r, r ? WSAGetLastError() : 0);
    return r;
}
static SOCKET WINAPI T_accept(SOCKET s, sockaddr* a, int* n) {
    SOCKET r = o_accept(s, a, n);
    char t[40];
    Addr(r != INVALID_SOCKET ? a : NULL, t, sizeof t);
    if (r != INVALID_SOCKET || WSAGetLastError() != WSAEWOULDBLOCK)
        TRACE("net: accept(%u) = %d from %s err %d", (unsigned)s, (int)r, t, r == INVALID_SOCKET ? WSAGetLastError() : 0);
    return r;
}
static int WINAPI T_close(SOCKET s) {
    TRACE("net: closesocket(%u)", (unsigned)s);
    return o_close(s);
}
static void Bytes(const char* b, int n, char* out, size_t size) {
    out[0] = 0;
    for (int i = 0; i < n && i < 12; i++) {
        char h[4];
        sprintf_s(h, "%02X ", (BYTE)b[i]);
        strcat_s(out, size, h);
    }
}
static int WINAPI T_send(SOCKET s, const char* b, int n, int f) {
    int r = o_send(s, b, n, f);
    char h[40];
    Bytes(b, n, h, sizeof h);
    TRACE("net: send(%u, %d) = %d err %d [%s]", (unsigned)s, n, r, r < 0 ? WSAGetLastError() : 0, h);
    return r;
}
static int WINAPI T_recv(SOCKET s, char* b, int n, int f) {
    int r = o_recv(s, b, n, f);
    if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return r;
    char h[40] = "";
    if (r > 0) Bytes(b, r, h, sizeof h);
    TRACE("net: recv(%u) = %d err %d [%s]", (unsigned)s, r, r < 0 ? WSAGetLastError() : 0, h);
    return r;
}
static int WINAPI T_sendto(SOCKET s, const char* b, int n, int f, const sockaddr* a, int an) {
    int r = o_sendto(s, b, n, f, a, an);
    char t[40];
    Addr(a, t, sizeof t);
    TRACE("net: sendto(%u, %d, %s) = %d err %d", (unsigned)s, n, t, r, r < 0 ? WSAGetLastError() : 0);
    return r;
}
static int WINAPI T_recvfrom(SOCKET s, char* b, int n, int f, sockaddr* a, int* an) {
    int r = o_recvfrom(s, b, n, f, a, an);
    if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return r;
    char t[40];
    Addr(r >= 0 ? a : NULL, t, sizeof t);
    TRACE("net: recvfrom(%u) = %d from %s err %d", (unsigned)s, r, t, r < 0 ? WSAGetLastError() : 0);
    return r;
}
static int WINAPI T_async(SOCKET s, HWND h, unsigned msg, long events) {
    int r = o_async(s, h, msg, events);
    TRACE("net: WSAAsyncSelect(%u, msg 0x%X, events 0x%lX) = %d", (unsigned)s, msg, events, r);
    return r;
}

// ---------------------------------------------------------------- Locus UDP packet dump
// Locus's network layer (XNet, around 0x40D000) uses WS2_32 WSASendTo / WSARecvFrom with
// overlapped I/O. Every packet goes to netdump.txt (game folder) in full. A receive completes
// later, so it is dumped when the same OVERLAPPED is used for the next receive (by then the
// game has processed it; InternalHigh holds the byte count).
static FILE* g_dump;
static CRITICAL_SECTION g_dumpLock;
static volatile LONG g_packets;
static const LONG MAX_PACKETS = 20000;

static void Dump(const char* dir, SOCKET s, const sockaddr* a, const BYTE* data, DWORD n) {
    if (!g_dump || InterlockedIncrement(&g_packets) > MAX_PACKETS) return;
    char t[40];
    Addr(a, t, sizeof t);
    EnterCriticalSection(&g_dumpLock);
    fprintf(g_dump, "%lu %s sock %u %s %lu bytes\n", GetTickCount(), dir, (unsigned)s, t, n);
    for (DWORD i = 0; i < n && i < 2048; i += 32) {
        fprintf(g_dump, "  %04lX ", i);
        for (DWORD j = i; j < i + 32 && j < n; j++) fprintf(g_dump, "%02X", data[j]);
        fputs("  ", g_dump);
        for (DWORD j = i; j < i + 32 && j < n; j++) fputc(data[j] >= 32 && data[j] < 127 ? data[j] : '.', g_dump);
        fputc('\n', g_dump);
    }
    fflush(g_dump);
    LeaveCriticalSection(&g_dumpLock);
}

typedef int(WINAPI* WSASendToFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int(WINAPI* WSARecvFromFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static WSASendToFn o_wsasendto;
static WSARecvFromFn o_wsarecvfrom;

struct Pending { LPWSAOVERLAPPED ovl; SOCKET s; BYTE* buf; sockaddr* from; };
static Pending g_pending[16];

bool LanRedirect(const sockaddr* to, sockaddr_in* out);  // lan.cpp
void LanFixSource(sockaddr* from);

static int WINAPI T_wsasendto(SOCKET s, LPWSABUF b, DWORD n, LPDWORD sent, DWORD f, const sockaddr* to, int tl,
                              LPWSAOVERLAPPED o, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    sockaddr_in other;
    if (LanRedirect(to, &other)) {
        to = (const sockaddr*)&other;
        tl = sizeof other;
    }
    for (DWORD i = 0; i < n; i++) Dump("SEND", s, to, (const BYTE*)b[i].buf, b[i].len);
    return o_wsasendto(s, b, n, sent, f, to, tl, o, cr);
}

static int WINAPI T_wsarecvfrom(SOCKET s, LPWSABUF b, DWORD n, LPDWORD got, LPDWORD f, sockaddr* from, LPINT fl,
                                LPWSAOVERLAPPED o, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (o) {  // remember where this receive lands; T_gqcs dumps it when it completes
        EnterCriticalSection(&g_dumpLock);
        Pending* slot = NULL;
        for (Pending& p : g_pending)
            if (p.ovl == o) slot = &p;
        if (!slot)
            for (Pending& p : g_pending)
                if (!p.ovl) { slot = &p; break; }
        if (slot) *slot = {o, s, n ? (BYTE*)b[0].buf : NULL, from};
        LeaveCriticalSection(&g_dumpLock);
    }
    int r = o_wsarecvfrom(s, b, n, got, f, from, fl, o, cr);
    if (!o && r == 0 && got && n) Dump("RECV", s, from, (const BYTE*)b[0].buf, *got);
    return r;
}

typedef BOOL(WINAPI* GqcsFn)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);
static GqcsFn o_gqcs;

static BOOL WINAPI T_gqcs(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED* ovl, DWORD ms) {
    BOOL r = o_gqcs(port, bytes, key, ovl, ms);
    if (r && ovl && *ovl && bytes) {
        EnterCriticalSection(&g_dumpLock);
        for (Pending& p : g_pending)
            if (p.ovl == (LPWSAOVERLAPPED)*ovl) {
                if (p.buf) Dump("RECV", p.s, p.from, p.buf, *bytes);
                LanFixSource(p.from);
                p.ovl = NULL;
                break;
            }
        LeaveCriticalSection(&g_dumpLock);
    }
    return r;
}

static void HookByName(const char* dll, const char* name, void* hook, void** original) {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char*)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, name)) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) return;
            *original = (void*)iat->u1.Function;
            iat->u1.Function = (DWORD)(uintptr_t)hook;
            VirtualProtect(&iat->u1.Function, 4, old, &old);
            Log("net: dumping %s", name);
            return;
        }
    }
}

static void InstallPacketDump() {
    InitializeCriticalSection(&g_dumpLock);
    char path[MAX_PATH];
    if (g_instance > 1) sprintf_s(path, "%snetdump%d.txt", g_dir, g_instance);
    else sprintf_s(path, "%snetdump.txt", g_dir);
    if (g_lanTrace) g_dump = _fsopen(path, "w", _SH_DENYNO);  // shared, readable while the game runs
    // The two WS2_32 hooks and the completion hook are always needed: LAN mode redirects the
    // boot-server traffic through them (LanRedirect / LanFixSource). Only the dump is optional.
    HookByName("WS2_32.dll", "WSASendTo", T_wsasendto, (void**)&o_wsasendto);
    HookByName("WS2_32.dll", "WSARecvFrom", T_wsarecvfrom, (void**)&o_wsarecvfrom);
    HookByName("KERNEL32.dll", "GetQueuedCompletionStatus", T_gqcs, (void**)&o_gqcs);
}

struct Entry { WORD ordinal; void* hook; void** original; };

// Walks KingdomUnderFire.exe's imports from WSOCK32.dll (imported by ordinal) and redirects the
// ones listed to the tracing functions above.
void InstallNetTrace() {
    if (!g_lanTrace) {  // setting lan-trace: off by default
        InstallPacketDump();
        return;
    }
    static const Entry table[] = {
        {23, T_socket, (void**)&o_socket},   {2, T_bind, (void**)&o_bind},
        {4, T_connect, (void**)&o_connect},  {13, T_listen, (void**)&o_listen},
        {1, T_accept, (void**)&o_accept},    {3, T_close, (void**)&o_close},
        {19, T_send, (void**)&o_send},       {16, T_recv, (void**)&o_recv},
        {20, T_sendto, (void**)&o_sendto},   {17, T_recvfrom, (void**)&o_recvfrom},
        {101, T_async, (void**)&o_async},
    };
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int hooked = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char*)(base + d->Name), "WSOCK32.dll")) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.Ordinal; names++, iat++) {
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            WORD ord = (WORD)IMAGE_ORDINAL(names->u1.Ordinal);
            for (const Entry& e : table) {
                if (e.ordinal != ord) continue;
                DWORD old;
                if (!VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) break;
                *e.original = (void*)iat->u1.Function;
                iat->u1.Function = (DWORD)(uintptr_t)e.hook;
                VirtualProtect(&iat->u1.Function, 4, old, &old);
                hooked++;
            }
        }
    }
    Log("net: tracing %d Winsock functions", hooked);
    InstallPacketDump();
}
