// proxy.cpp - kufmod is built as dinput8.dll. The game imports DirectInput8Create from
// DINPUT8.dll, and Windows looks in the game's own folder first, so the game loads this file at
// startup. These exports pass every call on to the real dinput8.dll in the Windows system folder,
// which is loaded the first time it is needed (never inside DllMain).
#include "kufmod.h"
#include <string.h>

static HMODULE g_real;

static FARPROC Real(const char* name) {
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);  // a 32-bit game gets SysWOW64 automatically
        if (!n || n > MAX_PATH - 16) return NULL;
        strcat_s(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) return NULL;
    }
    return GetProcAddress(g_real, name);
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out,
                                                    void* outer) {
    typedef HRESULT(WINAPI * Fn)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    Fn f = (Fn)Real("DirectInput8Create");
    return f ? f(inst, version, riid, out, outer) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow() {
    typedef HRESULT(WINAPI * Fn)();
    Fn f = (Fn)Real("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    typedef HRESULT(WINAPI * Fn)(REFCLSID, REFIID, LPVOID*);
    Fn f = (Fn)Real("DllGetClassObject");
    return f ? f(clsid, riid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer() {
    typedef HRESULT(WINAPI * Fn)();
    Fn f = (Fn)Real("DllRegisterServer");
    return f ? f() : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer() {
    typedef HRESULT(WINAPI * Fn)();
    Fn f = (Fn)Real("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

extern "C" const void* WINAPI Proxy_GetdfDIJoystick() {
    typedef const void*(WINAPI * Fn)();
    Fn f = (Fn)Real("GetdfDIJoystick");
    return f ? f() : NULL;
}
