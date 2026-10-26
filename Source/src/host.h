// Shared between the permanent loader (xinput9_1_0.dll) and the hot-reloadable core (ThugbotCore.dll).
// Anything that must survive a core reload lives in ThugbotHost. Bump THUGBOT_HOST_VERSION when the layout changes
// (a mismatched core refuses to start; restart the server in that case). Same split as AltBotMod's host.h, for the
// 64-bit Hardline server (bfh_server_Final.exe, HavanaXP4 2016-03-02).
#pragma once
#include <stdint.h>

#define THUGBOT_HOST_VERSION 1
#define MAX_BOTS 64

typedef struct ThugbotHost {
    int version;
    void (*log)(const char *fmt, ...);
    unsigned frame;            // incremented by the loader every server frame (once the frame hook is found)
    int gamePort;
    uintptr_t image;           // bfh_server_Final.exe module base (addresses in bfh_addrs.h are RVAs from it)

    int botCount;
    void *bots[MAX_BOTS];      // ServerPlayer* of the pseudo-players we drive
    float botMove[MAX_BOTS][3];// throttle, strafe, yaw
    char levelKey[96];         // "mp_bank_TurfWarLarge0" - kept across core reloads (read when a level loads)

    unsigned char reserved[4096];
} ThugbotHost;

// Core exports (undecorated).
typedef int (*ThugbotInit_t)(ThugbotHost *host);
typedef void (*ThugbotTick_t)(void);
typedef void (*ThugbotCommand_t)(char *line);
typedef void (*ThugbotShutdown_t)(void);
