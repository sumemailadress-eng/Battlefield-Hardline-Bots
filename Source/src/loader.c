// Thugbot loader - loaded by bfh_server_Final.exe as a proxy xinput9_1_0.dll (real exports forwarded to
// xinput9_1_0_orig.dll, see loader.def). Owns the log, the command file and persistent bot state, and hot-loads the
// bot logic from ThugbotCore.dll so it can be rebuilt without restarting the server ("reload").
//
// Stage 1: no engine hooks yet. A service thread polls Thugbot_cmd.txt; the per-frame hook goes in once its address
// is confirmed (bfh_addrs.h), and from then on the core ticks on the game thread only.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "host.h"

// The real XInput exports, forwarded to the stock DLL (renamed xinput9_1_0_orig.dll by the installer). MSVC takes
// forwarders as linker directives; loader.def keeps the same list for MinGW.
#pragma comment(linker, "/EXPORT:XInputGetCapabilities=xinput9_1_0_orig.XInputGetCapabilities")
#pragma comment(linker, "/EXPORT:XInputGetState=xinput9_1_0_orig.XInputGetState")
#pragma comment(linker, "/EXPORT:XInputSetState=xinput9_1_0_orig.XInputSetState")

static ThugbotHost g_host;
static FILE *g_log;
static char g_dir[MAX_PATH];
static char g_cmdPath[MAX_PATH];
static CRITICAL_SECTION g_logLock;

static HMODULE g_core;
static ThugbotTick_t g_coreTick;
static ThugbotCommand_t g_coreCommand;
static ThugbotShutdown_t g_coreShutdown;

static void host_log(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logLock);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, buf);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
}

// The core is copied to ThugbotCore.loaded.dll before loading, so ThugbotCore.dll stays writable for a rebuild.
static void core_unload(void)
{
    if (!g_core) return;
    if (g_coreShutdown) g_coreShutdown();
    FreeLibrary(g_core);
    g_core = NULL; g_coreTick = NULL; g_coreCommand = NULL; g_coreShutdown = NULL;
    host_log("core unloaded");
}

static void core_load(void)
{
    char src[MAX_PATH + 32], dst[MAX_PATH + 32];
    core_unload();
    snprintf(src, sizeof src, "%s\\ThugbotCore.dll", g_dir);
    snprintf(dst, sizeof dst, "%s\\ThugbotCore.loaded.dll", g_dir);
    if (!CopyFileA(src, dst, FALSE)) { host_log("core: no %s (error %lu)", src, GetLastError()); return; }
    g_core = LoadLibraryA(dst);
    if (!g_core) { host_log("core: LoadLibrary failed (error %lu)", GetLastError()); return; }
    ThugbotInit_t init = (ThugbotInit_t)GetProcAddress(g_core, "thugbot_init");
    g_coreTick = (ThugbotTick_t)GetProcAddress(g_core, "thugbot_tick");
    g_coreCommand = (ThugbotCommand_t)GetProcAddress(g_core, "thugbot_command");
    g_coreShutdown = (ThugbotShutdown_t)GetProcAddress(g_core, "thugbot_shutdown");
    if (!init || !init(&g_host)) {
        host_log("core: init refused (host version %d)", THUGBOT_HOST_VERSION);
        g_coreShutdown = NULL;
        core_unload();
        return;
    }
    host_log("core loaded");
}

// Thugbot_cmd.txt: one command per line, consumed (deleted) once read. "reload" reloads the core; the rest go to it.
static void poll_commands(void)
{
    FILE *f = fopen(g_cmdPath, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        host_log("command: %s", line);
        if (!strcmp(line, "reload")) core_load();
        else if (g_coreCommand) g_coreCommand(line);
    }
    fclose(f);
    DeleteFileA(g_cmdPath);
}

static DWORD WINAPI service_thread(LPVOID unused)
{
    (void)unused;
    core_load();
    for (;;) {
        poll_commands();
        Sleep(250);
    }
}

static int game_port(void)
{
    const char *p = strstr(GetCommandLineA(), "-GamePort ");
    return p ? atoi(p + 10) : 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_logLock);
    GetModuleFileNameA(inst, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;

    g_host.version = THUGBOT_HOST_VERSION;
    g_host.log = host_log;
    g_host.gamePort = game_port();
    g_host.image = (uintptr_t)GetModuleHandleA(NULL);

    char logPath[MAX_PATH + 32];
    snprintf(logPath, sizeof logPath, "%s\\Thugbot_%d.log", g_dir, g_host.gamePort);
    g_log = fopen(logPath, "a");
    snprintf(g_cmdPath, sizeof g_cmdPath, "%s\\Thugbot_cmd.txt", g_dir);
    host_log("Thugbot loader up: image %p port %d", (void *)g_host.image, g_host.gamePort);

    // LoadLibrary is not allowed under the loader lock: the core is loaded from the service thread.
    CreateThread(NULL, 0, service_thread, NULL, 0, NULL);
    return TRUE;
}
