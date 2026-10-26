// Thugbot core - bot logic for bfh_server_Final.exe (64-bit). Hot-reloadable: loaded by the loader
// (xinput9_1_0.dll), which owns persistent state (ThugbotHost). Every hook installed here is undone in
// thugbot_shutdown so the DLL can be unloaded and replaced while the server runs.
//
// Takes over the engine's pseudo-players. Two E8 call sites are redirected (through a stub page near the
// image, since this DLL can load more than 2 GB away): the PseudoPlayerManager's per-frame update (our tick, on the
// game thread) and each pseudo-player's input update (where we replace its random input with ours).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include "host.h"
#include "bfh_addrs.h"

static ThugbotHost *H;

typedef void (*PpmUpdate_t)(void *mgr, int a2, float dt);
typedef void (*PpInput_t)(void *mgr, void *pp, float dt);
static PpmUpdate_t o_ppmUpdate;
static PpInput_t o_ppInput;

// ---- call-site redirection ----
typedef struct CallHook { uintptr_t site; int32_t origRel; int installed; } CallHook;
static CallHook g_hookUpdate, g_hookInput;
static unsigned char *g_stubs;   // one page within +-2 GB of the image: "mov rax, imm64; jmp rax" per hook

static unsigned char *alloc_near(uintptr_t to)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    for (uintptr_t a = (to & ~(uintptr_t)0xffff) - 0x10000; a > to - 0x7ff00000; a -= 0x10000) {
        void *p = VirtualAlloc((void *)a, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

static int patch_rel32(uintptr_t at, int32_t rel)
{
    DWORD old;
    if (!VirtualProtect((void *)at, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    InterlockedExchange((volatile LONG *)at, rel);   // the rel32 after E8 is 4 bytes; written in one go
    VirtualProtect((void *)at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 4);
    return 1;
}

// Redirects the E8 call at RVA siteRva (which must call RVA targetRva) to fn, via stub slot. Returns the original.
static void *hook_call(CallHook *h, uintptr_t siteRva, uintptr_t targetRva, void *fn, int slot)
{
    uintptr_t site = H->image + siteRva;
    if (*(unsigned char *)site != 0xE8) { H->log("hook: no call at %#llx", (unsigned long long)siteRva); return NULL; }
    int32_t rel = *(int32_t *)(site + 1);
    uintptr_t target = site + 5 + rel;
    if (target != H->image + targetRva) {
        H->log("hook: call at %#llx goes to %#llx, expected %#llx - wrong server build?", (unsigned long long)siteRva,
               (unsigned long long)(target - H->image), (unsigned long long)targetRva);
        return NULL;
    }
    unsigned char *stub = g_stubs + slot * 16;
    stub[0] = 0x48; stub[1] = 0xB8; memcpy(stub + 2, &fn, 8);   // mov rax, fn
    stub[10] = 0xFF; stub[11] = 0xE0;                           // jmp rax
    if (!patch_rel32(site + 1, (int32_t)((intptr_t)stub - (intptr_t)(site + 5)))) return NULL;
    h->site = site; h->origRel = rel; h->installed = 1;
    return (void *)target;
}

static void unhook_call(CallHook *h)
{
    if (!h->installed) return;
    patch_rel32(h->site + 1, h->origRel);
    h->installed = 0;
}

// ---- bot driving ----
// play = the brain (brain.inc); the others are test patterns; off = the engine's own random input.
enum { DRIVE_OFF, DRIVE_STILL, DRIVE_FORWARD, DRIVE_CIRCLE, DRIVE_TURN, DRIVE_PLAY, DRIVE_COUNT };
static int g_drive = DRIVE_PLAY;
static const char *kDriveNames[] = { "off", "still", "forward", "circle", "turn", "play" };
static int brain_input(void *pp, float *fwd, float *strafe, float *turn, int *action);
static int g_noBackward = 0;   // ("backward off" bans it: on 2026-10-23 it pinned headings at 0 - that was the turn-across-0 bug, fixed since. Banned, bots in a fight whose route ran behind them stood still: the tester's groups "stuck behind walls"; heading locks went 30 -> 21 per 2 min with it allowed)
static volatile int g_traceBot = -1, g_traceFrames;   // "backward on|off" toggles it (live tests)
static void *g_mgr;
static volatile LONG g_probe;   // set by "probe": the next tick dumps the first bot's soldier memory

#define FIELD(p, off, type) (*(type *)((char *)(p) + (off)))

// The pseudo-player input update applies a soldier's input only when this virtual (slot 0x298) says yes (RVA 0x46d090,
// end); it runs every frame for every pseudo-player, so calling it again is safe. Expected: no while a soldier is down
// (Hardline's revive state) - its body stays in the world, alive-looking, but it cannot turn, aim or shoot.
#define VT_SOLDIER_TAKES_INPUT 0x298
typedef char (*SoldierPred_t)(void *soldier);
static int soldier_takes_input(void *soldier)
{
    if (!soldier || IsBadReadPtr(soldier, 8)) return 0;
    uintptr_t vt = *(uintptr_t *)soldier;
    if (vt < H->image || vt > H->image + 0x6000000) return 0;
    return ((SoldierPred_t)(*(uintptr_t *)(vt + VT_SOLDIER_TAKES_INPUT)))(soldier) != 0;
}

static void h_ppInput(void *mgr, void *pp, float dt)
{
    if (g_drive != DRIVE_OFF) {
        float fwd = 0, strafe = 0, turn = 0;
        int action = 0;
        if (g_drive == DRIVE_FORWARD) fwd = 1;
        else if (g_drive == DRIVE_CIRCLE) { fwd = 1; turn = 0.3f; }
        else if (g_drive == DRIVE_TURN) turn = 1;
        int inVeh = 0;
        if (g_drive == DRIVE_PLAY) inVeh = brain_input(pp, &fwd, &strafe, &turn, &action) == 2;
        FIELD(pp, OFF_PP_REROLL_TIMER, float) = 1e9f;   // never re-roll its own random input
        FIELD(pp, OFF_PP_ACTION_COUNT, unsigned) = action ? 1 : 0;
        FIELD(pp, OFF_PP_ACTIONS, unsigned) = action;
        // Never backwards: the pseudo-player sends negative forward as EIABrake, and soldiers walking backwards had their
        // heading reset to 0 and pinned there (turn input ignored) - checked live 2026-10-23. Turn and side-step instead.
        // (in a vehicle backward is brake / reverse: allowed - drivers "backed up" with zero input and stayed stuck)
        if (fwd < 0 && g_noBackward && !inVeh) fwd = 0;
        FIELD(pp, OFF_PP_FORWARD, float) = fwd;
        FIELD(pp, OFF_PP_STRAFE, float) = strafe;
        FIELD(pp, OFF_PP_TURN, float) = turn;
    }
    void *tpl = FIELD(pp, OFF_PP_PLAYER, void *);
    char *ts = g_traceFrames > 0 && g_traceBot >= 0 && g_traceBot < H->botCount && H->bots[g_traceBot] == tpl ? FIELD(tpl, OFF_PLAYER_SOLDIER, char *) : NULL;
    float y0 = ts ? FIELD(ts, OFF_SOLDIER_YAW, float) : 0;
    o_ppInput(mgr, pp, dt);
    if (ts) {
        g_traceFrames--;
        const float *v = (const float *)(ts + OFF_SOLDIER_VELOCITY);
        H->log("trace: input %d f%u dt %.4f yaw %.4f -> %.4f | in fwd %.3f str %.3f turn %.3f act %u x%u | vel %.2f %.2f",
               soldier_takes_input(ts), H->frame, dt, y0, FIELD(ts, OFF_SOLDIER_YAW, float), FIELD(pp, OFF_PP_FORWARD, float), FIELD(pp, OFF_PP_STRAFE, float),
               FIELD(pp, OFF_PP_TURN, float), FIELD(pp, OFF_PP_ACTIONS, unsigned), FIELD(pp, OFF_PP_ACTION_COUNT, unsigned), v[0], v[2]);
    }
}

// ---- extra inputs: pressed straight into the input state the pseudo-player update built, before it is copied ----
typedef void (*InputCopy_t)(void *dst, void *src);
typedef void (*InputTrack_t)(void *state, unsigned hash, char on);
typedef void (*InputDigital_t)(void *state, unsigned hash, char pressed);
typedef void (*InputAnalog_t)(void *state, unsigned hash, float level);
static InputCopy_t o_inputCopy;
static CallHook g_hookCopy;
enum { BTN_FIRE = 1, BTN_SPRINT = 2, BTN_JUMP = 4, BTN_INTERACT = 8, BTN_MELEE = 16, BTN_RELOAD = 32, BTN_ZOOM = 64, BTN_CROUCH = 128, BTN_EXIT = 256, BTN_LEAN = 512, BTN_CHUTE = 1024, BTN_GL = 2048, BTN_GADGET1 = 4096, BTN_GADGET2 = 8192, BTN_PRIMARY = 16384, BTN_STATIC = 32768, BTN_COUNT = 16 };
static const unsigned kBtnHash[BTN_COUNT] = { EIA_FIRE, EIA_SPRINT, EIA_JUMP, EIA_INTERACT, EIA_MELEE, EIA_RELOAD, EIA_ZOOM, EIA_CROUCH, EIA_CHANGE_VEHICLE, EIA_VEHICLE_POSE, EIA_PARACHUTE, EIA_SWITCH_GL, EIA_SWITCH_GADGET1, EIA_SWITCH_GADGET2, EIA_SWITCH_PRIMARY, EIA_SWITCH_STATIC };
static const char *kBtnNames[BTN_COUNT] = { "fire", "sprint", "jump", "interact", "melee", "reload", "zoom", "crouch", "exit", "lean", "chute", "launcher", "gadget1", "gadget2", "primary", "static" };
static unsigned g_testHash;   // "presshash <hex>": that input held (digital) on every bot, for finding keys
static unsigned g_testButtons;   // "press <name>": held on every bot (toggles), for live tests
static float g_testPitch;        // "pitch <level>": pitch input on every bot
static float g_testYaw;          // "yawtest hook": yaw input through the hook on every bot
static int brain_buttons(void *pp, unsigned *buttons, float *pitch, float *roll, float *yaw, float *thr);

static void h_inputCopy(void *dst, void *src)
{
    if (g_drive != DRIVE_OFF) {
        unsigned btn = g_testButtons;
        float pitch = g_testPitch, roll = 0, yaw = g_testYaw, thr = 0;
        if (g_drive == DRIVE_PLAY) {
            unsigned b = 0; float p = 0, r = 0, y = 0, th = 0;
            if (brain_buttons((char *)dst - 0x10, &b, &p, &r, &y, &th)) { btn |= b; if (p) pitch = p; roll = r; if (y) yaw = y; thr = th; }
        }
        InputTrack_t track = (InputTrack_t)(H->image + RVA_INPUT_TRACK);
        for (int k = 0; k < BTN_COUNT; k++)
            if (btn & (1u << k)) { track(src, kBtnHash[k], 1); ((InputDigital_t)(H->image + RVA_INPUT_SET_DIGITAL))(src, kBtnHash[k], 1); }
        if (g_testHash) { track(src, g_testHash, 1); ((InputDigital_t)(H->image + RVA_INPUT_SET_DIGITAL))(src, g_testHash, 1); }
        if (pitch) { track(src, EIA_PITCH, 1); ((InputAnalog_t)(H->image + RVA_INPUT_SET_ANALOG))(src, EIA_PITCH, pitch); }
        if (yaw) { track(src, EIA_YAW, 1); ((InputAnalog_t)(H->image + RVA_INPUT_SET_ANALOG))(src, EIA_YAW, yaw); }
        if (thr) { track(src, EIA_THROTTLE, 1); ((InputAnalog_t)(H->image + RVA_INPUT_SET_ANALOG))(src, EIA_THROTTLE, thr); }
        if (roll) { track(src, EIA_ROLL, 1); ((InputAnalog_t)(H->image + RVA_INPUT_SET_ANALOG))(src, EIA_ROLL, roll); }
    }
    o_inputCopy(dst, src);
}

static void probe_soldier(void)
{
    void **b = FIELD(g_mgr, OFF_PPM_PLAYERS_BEGIN, void **), **e = FIELD(g_mgr, OFF_PPM_PLAYERS_END, void **);
    for (; b && b != e; b++) {
        void *player = FIELD(*b, OFF_PP_PLAYER, void *);
        void *soldier = player ? FIELD(player, OFF_PLAYER_SOLDIER, void *) : NULL;
        if (!soldier) continue;
        H->log("probe: pp %p player %p soldier %p", *b, player, soldier);
        // Float triples that look like world coordinates (|x|,|z| < 4096, finite): candidates for the position.
        for (int off = 0; off < 0x1000; off += 4) {
            float *f = (float *)((char *)soldier + off);
            if (IsBadReadPtr(f, 12)) break;
            if (f[0] == 0 || f[2] == 0) continue;
            if (f[0] != f[0] || f[1] != f[1] || f[2] != f[2]) continue;
            if (f[0] > -4096 && f[0] < 4096 && f[1] > -512 && f[1] < 1024 && f[2] > -4096 && f[2] < 4096 &&
                (f[0] > 1 || f[0] < -1) && (f[2] > 1 || f[2] < -1))
                H->log("probe: +%#05x %.2f %.2f %.2f", off, f[0], f[1], f[2]);
        }
        return;
    }
    H->log("probe: no live bot");
}

// "snap N": raw copy of the first live bot's soldier (0x2000 bytes) and player (0x2000) to Thugbot_snapN.bin, for
// diffing offline (which fields change while it turns, walks, gets shot...).
static volatile LONG g_snap = -1;
static int g_snapBot;   // "snap N B": bot index B (default: the first live one)
static void snap_bot(int n)
{
    void **b = FIELD(g_mgr, OFF_PPM_PLAYERS_BEGIN, void **), **e = FIELD(g_mgr, OFF_PPM_PLAYERS_END, void **);
    if (g_snapBot > 0 && b + g_snapBot < e) b += g_snapBot;
    for (; b && b != e; b++) {
        void *player = FIELD(*b, OFF_PP_PLAYER, void *);
        void *soldier = player ? FIELD(player, OFF_PLAYER_SOLDIER, void *) : NULL;
        if (!soldier) continue;
        char path[64];
        snprintf(path, sizeof path, "Thugbot_snap%d.bin", n);
        FILE *f = fopen(path, "wb");
        if (!f) return;
        static unsigned char buf[0x2000];
        for (int k = 0; k < 2; k++) {
            unsigned char *src = k ? (unsigned char *)player : (unsigned char *)soldier;
            for (int off = 0; off < (int)sizeof buf; off += 0x1000)
                if (IsBadReadPtr(src + off, 0x1000)) memset(buf + off, 0, 0x1000); else memcpy(buf + off, src + off, 0x1000);
            fwrite(buf, 1, sizeof buf, f);
        }
        fclose(f);
        H->log("snap %d: soldier %p player %p", n, soldier, player);
        return;
    }
    H->log("snap: no live bot");
}

// ---- raycast ----
// The engine loads these with movaps: they must be 16-byte aligned (unaligned ones crashed the server).
typedef __declspec(align(16)) struct Vec4 { float x, y, z, w; } Vec4;
typedef __declspec(align(16)) struct RayHit { Vec4 pos, normal; unsigned char rest[0x60]; } RayHit;   // fraction at +0x34
typedef char (*RayQuery_t)(void *caster, const char *ident, Vec4 *from, Vec4 *to, RayHit *hit, int flags, void *excl);
static void *g_rayCaster;
static int g_rayFlags = 0x10 | 0x20 | 0x80;   // no characters / ragdolls, detail mesh (BF4's usual set; checked live)
static unsigned g_raysTotal;   // (perf)
static RayHit g_lastHit;                       // the last ray that hit (body / material fields still being mapped)
static float g_lastFrom[3], g_lastTo[3];       // ... and where it went

// The ray caster is a sub-object of the GameWorld singleton; its vtable must be the one holding physicsRayQuery.
static void ray_find(void)
{
    void *world = *(void **)(H->image + RVA_GAME_WORLD);
    void *caster = world ? (char *)world + OFF_WORLD_RAYCASTER : NULL;
    uintptr_t vt = caster ? FIELD(caster, 0, uintptr_t) : 0;
    g_rayCaster = vt && !IsBadReadPtr((void *)vt, 8) && *(uintptr_t *)vt == H->image + RVA_PHYSICS_RAY_QUERY ? caster : NULL;
    H->log("ray: world %p caster %p vtable %#llx -> %s", world, caster, (unsigned long long)(vt ? vt - H->image : 0),
           g_rayCaster ? "ok" : "not the ray caster, raycasts off");
}

static void ray_find(void);
#define RAY_SEGMENT_M 45.0f   // under the engine's 50 m "long ray" warning
static int ray_cast(const float *from, const float *to, float *hitPos, float *hitNormal)
{
    // The world is replaced on every level change: never cast through a stale one (that crashed the server).
    void *world = *(void **)(H->image + RVA_GAME_WORLD);
    if (!world) { g_rayCaster = NULL; return 0; }
    if (g_rayCaster != (char *)world + OFF_WORLD_RAYCASTER) g_rayCaster = NULL;
    static unsigned triedAt = ~0u;
    if (!g_rayCaster && triedAt != H->frame) { triedAt = H->frame; ray_find(); }
    if (!g_rayCaster) return 0;
    // The engine warns ("Long server physicsRayQuery (N m)", GameWorld.cpp 363) for every ray over 50 m - thousands a
    // minute from line of sight, cover and the grid builder. Long rays go out as segments of up to 45 m instead (each
    // starting 10 cm back, so a surface right at a joint is not missed); the first segment that hits is the answer.
    float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    int segs = len > RAY_SEGMENT_M ? (int)ceilf(len / RAY_SEGMENT_M) : 1;
    RayHit h;
    int hit = 0;
    float sa[3], sb[3];
    for (int k = 0; k < segs && !hit; k++) {
        float t0 = (float)k / segs, t1 = (float)(k + 1) / segs;
        if (k) t0 -= 0.1f / len;
        for (int c = 0; c < 3; c++) { sa[c] = from[c] + d[c] * t0; sb[c] = from[c] + d[c] * t1; }
        if (k == segs - 1) memcpy(sb, to, 12);
        Vec4 a = { sa[0], sa[1], sa[2], 0 }, b = { sb[0], sb[1], sb[2], 0 };
        memset(&h, 0, sizeof h);
        g_raysTotal++;
        hit = ((RayQuery_t)(H->image + RVA_PHYSICS_RAY_QUERY))(g_rayCaster, "Thugbot", &a, &b, &h, g_rayFlags, NULL) != 0;
    }
    if (!hit) return 0;
    g_lastHit = h;
    memcpy(g_lastFrom, sa, 12); memcpy(g_lastTo, sb, 12);   // the segment that hit (nav_hit_class reads its direction)
    if (hitPos) { hitPos[0] = h.pos.x; hitPos[1] = h.pos.y; hitPos[2] = h.pos.z; }
    if (hitNormal) { hitNormal[0] = h.normal.x; hitNormal[1] = h.normal.y; hitNormal[2] = h.normal.z; }
    return 1;
}

// "raytest": from the first live bot, a ray straight down and one 20 m ahead at chest height.
static void ray_test(void)
{
    if (!g_rayCaster) ray_find();
    void **b = FIELD(g_mgr, OFF_PPM_PLAYERS_BEGIN, void **), **e = FIELD(g_mgr, OFF_PPM_PLAYERS_END, void **);
    for (; b && b != e; b++) {
        void *player = FIELD(*b, OFF_PP_PLAYER, void *);
        void *soldier = player ? FIELD(player, OFF_PLAYER_SOLDIER, void *) : NULL;
        if (!soldier) continue;
        float *p = (float *)((char *)soldier + OFF_SOLDIER_POSITION), yaw = FIELD(soldier, OFF_SOLDIER_YAW, float);
        float hp[3], hn[3];
        float a[3] = { p[0], p[1] + 2, p[2] }, d[3] = { p[0], p[1] - 5, p[2] };
        int hit = ray_cast(a, d, hp, hn);
        H->log("raytest down from %.2f %.2f %.2f: hit %d at %.2f %.2f %.2f normal %.2f %.2f %.2f", a[0], a[1], a[2], hit, hp[0], hp[1], hp[2], hn[0], hn[1], hn[2]);
        float c[3] = { p[0], p[1] + 1.2f, p[2] }, f[3] = { p[0] + 20 * sinf(yaw), p[1] + 1.2f, p[2] + 20 * cosf(yaw) };
        hit = ray_cast(c, f, hp, hn);
        H->log("raytest ahead (yaw %.2f): hit %d at %.2f %.2f %.2f (%.1f m)", yaw, hit, hp[0], hp[1], hp[2],
               hit ? sqrtf((hp[0] - c[0]) * (hp[0] - c[0]) + (hp[2] - c[2]) * (hp[2] - c[2])) : 0.0f);
        if (hit) {   // the hit record past position / normal, to map the body and material fields
            unsigned *r = (unsigned *)&g_lastHit.rest;
            H->log("raytest rest: %08x %08x %08x %08x %08x %08x %08x %08x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        }
        return;
    }
    H->log("raytest: no live bot");
}

static volatile LONG g_rayTest, g_rayDiff, g_vehProbe, g_vehScan, g_vehName, g_groundProbe, g_perfDump;

// ---- capture points: collected as the engine updates them ----
typedef void (*CpUpdate_t)(void *cp, void *info);
static CpUpdate_t o_cpUpdate;
static CallHook g_hookCp;
#define MAX_CPS 32
static void *g_cps[MAX_CPS];
static int g_cpCount;
static volatile LONG g_flagsDump;

static void h_cpUpdate(void *cp, void *info)
{
    o_cpUpdate(cp, info);
    for (int i = 0; i < g_cpCount; i++) if (g_cps[i] == cp) return;
    if (g_cpCount < MAX_CPS) { g_cps[g_cpCount++] = cp; H->log("flag %d seen: %p", g_cpCount - 1, cp); }
}

static void flags_dump(void)
{
    for (int i = 0; i < g_cpCount; i++) {
        char *cp = g_cps[i];
        H->log("flag %d %p: +118 %d +11c %d +120 %d +178 %d +17c %d", i, cp, FIELD(cp, 0x118, int), FIELD(cp, 0x11c, int),
               FIELD(cp, OFF_CP_TEAM_B, int), FIELD(cp, OFF_CP_COUNT_A, int), FIELD(cp, OFF_CP_COUNT_B, int));
        for (int off = 0; off < 0x400; off += 4) {
            float *f = (float *)(cp + off);
            if (IsBadReadPtr(f, 12)) break;
            if (f[0] != f[0] || f[1] != f[1] || f[2] != f[2]) continue;
            if (f[0] > -4096 && f[0] < 4096 && f[1] > -512 && f[1] < 1024 && f[2] > -4096 && f[2] < 4096 &&
                (f[0] > 2 || f[0] < -2) && (f[2] > 2 || f[2] < -2) && (f[1] > 1 || f[1] < -1))
                H->log("flag %d: +%#05x %.2f %.2f %.2f", i, off, f[0], f[1], f[2]);
        }
    }
    if (!g_cpCount) H->log("flags: none seen yet");
}

// ---- level name: read from the map sequencer when a level finishes loading ----
typedef void (*SeqLevelLoaded_t)(void *seq);
static SeqLevelLoaded_t o_seqLevelLoaded;
static CallHook g_hookSeq;
static void brain_level_reset(void);
static char g_levelKey[96];
static void read_level_key(void *seq)
{
    const char *lvl = FIELD(seq, OFF_SEQ_LEVEL, const char *), *mode = FIELD(seq, OFF_SEQ_MODE, const char *);
    if (!lvl || IsBadReadPtr(lvl, 8) || !mode || IsBadReadPtr(mode, 4)) { H->log("level: sequencer strings unreadable"); return; }
    const char *slash = strrchr(lvl, '/');
    char key[96];
    snprintf(key, sizeof key, "%s_%s", slash ? slash + 1 : lvl, mode);
    for (char *k = key; *k; k++) if (!(isalnum((unsigned char)*k) || *k == '_')) *k = '_';
    if (strcmp(key, g_levelKey)) { brain_level_reset(); strcpy(g_levelKey, key); }
    H->log("level: %s (%s %s)", g_levelKey, lvl, mode);
}
static void h_seqLevelLoaded(void *seq)
{
    o_seqLevelLoaded(seq);
    read_level_key(seq);
}

// ---- every player (humans too): the player manager's list, as the capture points read it ----
static void *g_players[128];
static int g_playerCount;
static void players_refresh(void)
{
    g_playerCount = 0;
    char *gc = *(char **)(H->image + RVA_SERVER_GAME_CONTEXT);
    char *pm = gc ? FIELD(gc, OFF_GC_PLAYER_MANAGER, char *) : NULL;
    if (!pm) return;
    void **b = FIELD(pm, OFF_PM_PLAYERS_BEGIN, void **), **e = FIELD(pm, OFF_PM_PLAYERS_END, void **);
    for (; b && b < e && g_playerCount < 128; b++) if (*b) g_players[g_playerCount++] = *b;
}

static volatile LONG g_ctfScanDo;   // "ctfscan" (run on the game thread in brain_tick)
#include "brain.inc"

// "players": every pseudo-player's ServerPlayer (0x2000 bytes each) to Thugbot_players.bin, to find fields such as
// the team by comparing against RCON's listPlayers.
static volatile LONG g_playersDump, g_botsDump, g_combatDump, g_iniReload, g_doorsDump;
// "yawtest": can a soldier's heading be set by writing it (instead of turn input)? For the soldiers whose turn input is
// ignored. Logs the yaw field, the rotation row heading and the walking direction while it writes.
static volatile int g_yawTestBot, g_yawTestFrames; static volatile float g_yawTestVal;
// "aimscan B": where else bot B's heading (+0x58c) is kept - every float within 0.002 rad of it (mod 2 pi) in the
// objects the soldier points to, two levels deep. Run it on a bot that is turning (heading not 0).
static volatile LONG g_aimScanBot = -1;
static int aim_near(float v, float y) { if (!(v == v) || v > 100 || v < -100) return 0; float d = fmodf(v - y + 3141.5926f * 2 + 3.14159265f, 6.2831853f) - 3.14159265f; return d < 0.002f && d > -0.002f; }
static int heap_like(uintptr_t p) { return p > 0x1000000 && p < 0x7fff00000000ull && !(p & 7) && !(p >= H->image && p < H->image + 0x6000000) && !IsBadReadPtr((void *)p, 0x400); }
static void aim_scan(int bot)
{
    if (bot < 0 || bot >= H->botCount) return;
    char *s = H->bots[bot] ? FIELD(H->bots[bot], OFF_PLAYER_SOLDIER, char *) : NULL;
    if (!s) { H->log("aimscan: bot %d dead", bot); return; }
    float y = FIELD(s, OFF_SOLDIER_YAW, float);
    H->log("aimscan: bot %d soldier %p yaw %.4f", bot, s, y);
    int found = 0;
    for (int o1 = 0; o1 < 0x2000 && found < 40; o1 += 8) {
        uintptr_t p1 = *(uintptr_t *)(s + o1);
        if (!heap_like(p1) || p1 == (uintptr_t)s) continue;
        for (int k = 0; k < 0x400; k += 4) if (aim_near(*(float *)(p1 + k), y)) { H->log("aimscan: *(+%#x) +%#x = %.4f (next %.4f)", o1, k, *(float *)(p1 + k), *(float *)(p1 + k + 4)); found++; }
        for (int o2 = 0; o2 < 0x400 && found < 40; o2 += 8) {
            uintptr_t p2 = *(uintptr_t *)(p1 + o2);
            if (!heap_like(p2) || p2 == (uintptr_t)s || p2 == p1) continue;
            for (int k = 0; k < 0x400; k += 4) if (aim_near(*(float *)(p2 + k), y)) { H->log("aimscan: *(*(+%#x) +%#x) +%#x = %.4f (next %.4f)", o1, o2, k, *(float *)(p2 + k), *(float *)(p2 + k + 4)); found++; }
        }
    }
    H->log("aimscan: %d match(es)", found);
}
// "peek B a,b,c": from bot B's soldier, follow pointers at +a, then +b, and show 6 floats at +c of the last object.
// "poke B a,b,c V N": write float V there every frame for N frames (watch what follows it).
static char g_peekPath[128]; static volatile LONG g_peekBot = -1; static int g_pokeFrames, g_pokeBot; static float g_pokeVal; static char g_pokePath[128];
static float *path_addr(int bot, const char *path)
{
    if (bot < 0 || bot >= H->botCount || !H->bots[bot]) return NULL;
    uintptr_t p = (uintptr_t)FIELD(H->bots[bot], OFF_PLAYER_SOLDIER, char *);
    if (!p) return NULL;
    char buf[128]; snprintf(buf, sizeof buf, "%s", path);
    char *tok = strtok(buf, ",");
    while (tok) {
        uintptr_t off = strtoul(tok, NULL, 16);
        tok = strtok(NULL, ",");
        if (!tok) return IsBadReadPtr((void *)(p + off), 24) ? NULL : (float *)(p + off);
        if (IsBadReadPtr((void *)(p + off), 8)) return NULL;
        p = *(uintptr_t *)(p + off);
        if (!heap_like(p)) return NULL;
    }
    return NULL;
}
static void yaw_test_tick(void)
{
    LONG pb = InterlockedExchange(&g_peekBot, -1);
    if (pb >= 0) {
        float *a = path_addr(pb, g_peekPath);
        char *s = H->bots[pb] ? FIELD(H->bots[pb], OFF_PLAYER_SOLDIER, char *) : NULL;
        if (a) H->log("peek: bot %d [%s] = %.4f %.4f %.4f %.4f %.4f %.4f (yaw +58c %.4f)", pb, g_peekPath, a[0], a[1], a[2], a[3], a[4], a[5], s ? FIELD(s, OFF_SOLDIER_YAW, float) : 0.0f);
        else H->log("peek: bot %d [%s] unreadable", pb, g_peekPath);
    }
    if (g_pokeFrames > 0) {
        float *a = path_addr(g_pokeBot, g_pokePath);
        char *s = H->bots[g_pokeBot] ? FIELD(H->bots[g_pokeBot], OFF_PLAYER_SOLDIER, char *) : NULL;
        if (!a || !s) { g_pokeFrames = 0; H->log("poke: unreadable"); }
        else {
            if (g_pokeFrames % 20 == 0) {
                const float *v = (const float *)(s + OFF_SOLDIER_VELOCITY);
                H->log("poke: [%s] was %.4f, yaw +58c %.4f, walking %.2f", g_pokePath, *a, FIELD(s, OFF_SOLDIER_YAW, float), atan2f(v[0], v[2]));
            }
            *a = g_pokeVal;
            if (--g_pokeFrames == 0) H->log("poke: done");
        }
    }
    LONG as = InterlockedExchange(&g_aimScanBot, -1);
    if (as >= 0) aim_scan(as);
    if (g_yawTestFrames <= 0 || g_yawTestBot < 0 || g_yawTestBot >= H->botCount) return;
    void *pl = H->bots[g_yawTestBot], *s = pl ? FIELD(pl, OFF_PLAYER_SOLDIER, void *) : NULL;
    if (!s) { g_yawTestFrames = 0; H->log("yawtest: bot dead"); return; }
    if (g_yawTestFrames % 30 == 0) {
        const float *r = (const float *)((char *)s + OFF_SOLDIER_ROT_FWD), *v = (const float *)((char *)s + OFF_SOLDIER_VELOCITY);
        H->log("yawtest: +58c %.2f, row heading %.2f, walking %.2f (speed %.1f)", FIELD(s, OFF_SOLDIER_YAW, float), atan2f(-r[0], -r[2]),
               atan2f(v[0], v[2]), sqrtf(v[0] * v[0] + v[2] * v[2]));
    }
    FIELD(s, OFF_SOLDIER_YAW, float) = g_yawTestVal;
    if (--g_yawTestFrames == 0) H->log("yawtest: done");
}
static void players_dump(void)
{
    FILE *f = fopen("Thugbot_players.bin", "wb");
    if (!f) return;
    static unsigned char buf[0x2000];
    int n = 0;
    void **b = FIELD(g_mgr, OFF_PPM_PLAYERS_BEGIN, void **), **e = FIELD(g_mgr, OFF_PPM_PLAYERS_END, void **);
    for (; b && b != e; b++, n++) {
        unsigned char *p = FIELD(*b, OFF_PP_PLAYER, unsigned char *);
        for (int off = 0; off < (int)sizeof buf; off += 0x1000)
            if (!p || IsBadReadPtr(p + off, 0x1000)) memset(buf + off, 0, 0x1000); else memcpy(buf + off, p + off, 0x1000);
        fwrite(buf, 1, sizeof buf, f);
    }
    fclose(f);
    H->log("players: %d written", n);
}

static void damage_dump(void);
// ---- perf: what the bot code costs per server frame ("perf" prints it; counters reset each time) ----
static double g_perfSum, g_perfMax, g_perfFreq; static unsigned g_perfFrames, g_perfRays, g_perfRaysMax;
static double g_perfPart[8]; static const char *kPerfPart[8] = { "players", "nav / planner", "combat frame", "bots", "routes", "learn / misc", "", "" };
static void perf_note(double ticks, unsigned rays)
{
    if (!g_perfFreq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_perfFreq = (double)f.QuadPart / 1000.0; }
    double ms = ticks / g_perfFreq;
    g_perfSum += ms; if (ms > g_perfMax) g_perfMax = ms; g_perfFrames++;
    g_perfRays += rays; if (rays > g_perfRaysMax) g_perfRaysMax = rays;
}
static void perf_part(int k, LARGE_INTEGER *since)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_perfFreq) g_perfPart[k] += (double)(now.QuadPart - since->QuadPart) / g_perfFreq;
    *since = now;
}
static void perf_dump(void)
{
    if (!g_perfFrames) { H->log("perf: no frames yet"); return; }
    H->log("perf: %u frame(s), bot code %.2f ms a frame on average, %.1f ms at worst; %.0f rays a frame (%u at most); %d bot(s)",
           g_perfFrames, g_perfSum / g_perfFrames, g_perfMax, (double)g_perfRays / g_perfFrames, g_perfRaysMax, H->botCount);
    for (int k = 0; k < 6; k++) H->log("perf:   %-14s %.2f ms a frame", kPerfPart[k], g_perfPart[k] / g_perfFrames);
    H->log("perf: input flips (left <-> right in one frame): turn %.2f, strafe %.2f per bot a second", g_jitterTurn * 60.0 / g_perfFrames / (H->botCount ? H->botCount : 1), g_jitterStrafe * 60.0 / g_perfFrames / (H->botCount ? H->botCount : 1));
    H->log("perf: of the turn flips, %u%% while fighting; steering alone flipped %u time(s)", g_jitterTurn ? g_jitterTurnFight * 100 / g_jitterTurn : 0, g_jitterSteer);
    H->log("perf: (turn flips counted: %u)", g_jitterTurn);
    jitter_dump();
    g_jitterTurn = g_jitterStrafe = g_jitterTurnFight = g_jitterSteer = 0;
    g_perfSum = g_perfMax = 0; g_perfFrames = g_perfRays = g_perfRaysMax = 0; memset(g_perfPart, 0, sizeof g_perfPart);
}
static void h_ppmUpdate(void *mgr, int a2, float dt)
{
    o_ppmUpdate(mgr, a2, dt);
    // Level going away or a new one in: the flags, the grid and every bot's plan belong to the old world.
    static void *lastWorld;
    void *world = *(void **)(H->image + RVA_GAME_WORLD);
    if (world != lastWorld) {
        H->log("world %p -> %p: flags and plans cleared", lastWorld, world);
        lastWorld = world;
        g_cpCount = 0;
        brain_level_reset();
    }
    if (!world) return;
    g_mgr = mgr;
    H->frame++;
    int n = 0;
    void **b = FIELD(mgr, OFF_PPM_PLAYERS_BEGIN, void **), **e = FIELD(mgr, OFF_PPM_PLAYERS_END, void **);
    for (; b && b != e && n < MAX_BOTS; b++) H->bots[n++] = FIELD(*b, OFF_PP_PLAYER, void *);
    H->botCount = n;
    if (InterlockedExchange(&g_probe, 0)) probe_soldier();
    LONG s = InterlockedExchange(&g_snap, -1);
    if (s >= 0) snap_bot(s);
    if (InterlockedExchange(&g_rayTest, 0)) ray_test();
    if (InterlockedExchange(&g_flagsDump, 0)) flags_dump();
    if (InterlockedExchange(&g_playersDump, 0)) players_dump();
    if (InterlockedExchange(&g_rayDiff, 0)) raydiff();
    if (InterlockedExchange(&g_vehProbe, 0)) vehicles_probe();
    if (InterlockedExchange(&g_vehScan, 0)) vehicles_scan();
    if (InterlockedExchange(&g_vehName, 0)) vehicles_names();
    if (InterlockedExchange(&g_groundProbe, 0)) ground_probe();
    if (InterlockedExchange(&g_perfDump, 0)) perf_dump();
    if (InterlockedExchange(&g_botsDump, 0)) { brain_dump(); damage_dump(); drive_dump(); }
    if (InterlockedExchange(&g_combatDump, 0)) combat_dump();
    if (InterlockedExchange(&g_iniReload, 0)) settings_load();
    if (InterlockedExchange(&g_doorsDump, 0)) doors_dump();
    if (InterlockedExchange(&g_navRebuild, 0)) nav_rebuild();
    if (g_drive == DRIVE_PLAY) {
        LARGE_INTEGER a, c;
        QueryPerformanceCounter(&a);
        unsigned rays0 = g_raysTotal;
        brain_tick();
        QueryPerformanceCounter(&c);
        perf_note((double)(c.QuadPart - a.QuadPart), g_raysTotal - rays0);
    }
    yaw_test_tick();
}

// ---- bot bullets count: mark hits from a bot's gun as client-confirmed (see RVA_SOLDIER_ON_DAMAGE) ----
typedef void *(*OnDamage_t)(void *self, void *ret, void *info);
static OnDamage_t o_onDamage;
static uintptr_t *g_onDamageSlot;
static unsigned g_dmgBotHits, g_dmgBotPassed, g_dmgOther, g_dmgLogged;

static void *damage_giver_player(void *info)
{
    void *gi = info && !IsBadReadPtr((char *)info + OFF_DMG_GIVER_INFO, 8) ? FIELD(info, OFF_DMG_GIVER_INFO, void *) : NULL;
    void **ref = gi && !IsBadReadPtr(gi, 8) ? *(void ***)gi : NULL;
    char *obj = ref && !IsBadReadPtr(ref, 8) ? (char *)*ref : NULL;
    return obj ? obj - 8 : NULL;
}

static void *h_onDamage(void *self, void *ret, void *info)
{
    void *giver = damage_giver_player(info);
    // the victim: the component's owner pointer (+8) leads to the ServerSoldierEntity (BFServerSoldierHealthComponent)
    void **own = !IsBadReadPtr((char *)self + 8, 8) ? FIELD(self, 8, void **) : NULL;
    void *victim = own && !IsBadReadPtr(own, 8) ? *own : NULL;
    if (victim) combat_note_hit(victim, giver);
    int bot = 0;
    for (int i = 0; giver && i < H->botCount; i++) if (H->bots[i] == giver) { bot = 1; g_hitsBy[i]++; break; }
    if (bot && !IsBadWritePtr((char *)info + OFF_DMG_FLAGS, 1)) {
        unsigned char *flags = (unsigned char *)info + OFF_DMG_FLAGS;
        g_dmgBotHits++;
        if (!(*flags & 1)) { *flags |= 1; g_dmgBotPassed++; }
        if (g_dmgLogged++ < 8) H->log("damage: hit by bot %s (flags were %#x) - counted", (char *)giver + OFF_PLAYER_NAME, *flags);
    } else g_dmgOther++;
    return o_onDamage(self, ret, info);
}

static void damage_dump(void)
{
    H->log("damage: %u hit(s) from bots (%u needed the client-confirmed mark), %u other hit(s)%s", g_dmgBotHits, g_dmgBotPassed, g_dmgOther,
           g_onDamageSlot ? "" : " - hook OFF");
}

static int hook_vtable_slot(uintptr_t **slotOut, uintptr_t slotRva, uintptr_t expectRva, void *fn, void **orig)
{
    uintptr_t *slot = (uintptr_t *)(H->image + slotRva);
    DWORD old;
    if (*slot != H->image + expectRva && *slot != (uintptr_t)fn) { H->log("hook: vtable slot %#llx holds %#llx - wrong server build?", (unsigned long long)slotRva, (unsigned long long)(*slot - H->image)); return 0; }
    *orig = (void *)(H->image + expectRva);
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer((void **)slot, fn);
    VirtualProtect(slot, 8, old, &old);
    *slotOut = slot;
    return 1;
}

static void unhook_vtable_slot(uintptr_t **slotRef, void *orig)
{
    uintptr_t *slot = *slotRef;
    DWORD old;
    if (!slot || !VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return;
    InterlockedExchangePointer((void **)slot, orig);
    VirtualProtect(slot, 8, old, &old);
    *slotRef = NULL;
}

__declspec(dllexport) int thugbot_init(ThugbotHost *host)
{
    if (!host || host->version != THUGBOT_HOST_VERSION) return 0;
    H = host;
    if (!g_stubs) g_stubs = alloc_near(H->image);
    if (!g_stubs) { H->log("core: no stub page near the image"); return 0; }
    o_ppInput = (PpInput_t)hook_call(&g_hookInput, RVA_PP_INPUT_CALL, RVA_PP_INPUT, (void *)h_ppInput, 0);
    o_ppmUpdate = (PpmUpdate_t)hook_call(&g_hookUpdate, RVA_PPM_UPDATE_CALL, RVA_PPM_UPDATE, (void *)h_ppmUpdate, 1);
    if (!o_ppInput || !o_ppmUpdate) {
        unhook_call(&g_hookInput); unhook_call(&g_hookUpdate);
        return 0;
    }
    o_cpUpdate = (CpUpdate_t)hook_call(&g_hookCp, RVA_CP_UPDATE_CALL, RVA_CP_UPDATE, (void *)h_cpUpdate, 2);
    o_seqLevelLoaded = (SeqLevelLoaded_t)hook_call(&g_hookSeq, RVA_SEQ_LEVEL_LOADED_CALL, RVA_SEQ_LEVEL_LOADED, (void *)h_seqLevelLoaded, 3);
    o_inputCopy = (InputCopy_t)hook_call(&g_hookCopy, RVA_PP_INPUT_COPY_CALL, RVA_INPUT_COPY, (void *)h_inputCopy, 4);
    int dmgHook = hook_vtable_slot(&g_onDamageSlot, RVA_SOLDIER_ON_DAMAGE_VT, RVA_SOLDIER_ON_DAMAGE, (void *)h_onDamage, (void **)&o_onDamage);
    int ctfHook = hook_vtable_slot(&g_ctfSlot, CTF_MSG_VT_SLOT, CTF_MSG_FN, (void *)h_ctfOnMsg, (void **)&o_ctfOnMsg);
    if (H->levelKey[0]) strcpy(g_levelKey, H->levelKey);
    nav_set_hit_classifier(nav_hit_class);
    H->log("core init: hooks in (flags %s, buttons %s, bot damage %s, ctf %s), drive %s", o_cpUpdate ? "on" : "off", o_inputCopy ? "on" : "off",
           dmgHook ? "on" : "off", ctfHook ? "on" : "off", kDriveNames[g_drive]);
    return 1;
}

__declspec(dllexport) void thugbot_tick(void)
{
}

// Commands (Thugbot_cmd.txt): status, bots, combat, ini (re-read Thugbot.ini), forget engine, level <key>, learnsave, probe, snap N, raytest, rayflags N, flags, players,
// drive off|still|forward|circle|turn|play, press <button> (toggle on every bot), pitch <level>, navrebuild
__declspec(dllexport) void thugbot_command(char *line)
{
    if (!strcmp(line, "status")) H->log("status: frame %u, %d pseudo-player(s), drive %s", H->frame, H->botCount, kDriveNames[g_drive]);
    else if (!strcmp(line, "probe")) InterlockedExchange(&g_probe, 1);
    else if (!strncmp(line, "snap ", 5)) { char *sp = strchr(line + 5, ' '); g_snapBot = sp ? atoi(sp + 1) : 0; InterlockedExchange(&g_snap, atoi(line + 5)); }
    else if (!strcmp(line, "raytest")) InterlockedExchange(&g_rayTest, 1);
    else if (!strcmp(line, "flags")) InterlockedExchange(&g_flagsDump, 1);
    else if (!strcmp(line, "players")) InterlockedExchange(&g_playersDump, 1);
    else if (!strcmp(line, "bots")) InterlockedExchange(&g_botsDump, 1);
    else if (!strcmp(line, "combat")) InterlockedExchange(&g_combatDump, 1);
    else if (!strcmp(line, "doors")) InterlockedExchange(&g_doorsDump, 1);
    else if (!strncmp(line, "trace ", 6)) { g_traceBot = atoi(line + 6); g_traceFrames = 200; }
    else if (!strncmp(line, "backward ", 9)) { g_noBackward = strcmp(line + 9, "on") != 0; H->log("backward movement %s", g_noBackward ? "off" : "on"); }
    else if (!strncmp(line, "peek ", 5)) { char *sp = strchr(line + 5, ' '); if (sp) { snprintf(g_peekPath, sizeof g_peekPath, "%s", sp + 1); InterlockedExchange(&g_peekBot, atoi(line + 5)); } }
    else if (!strncmp(line, "poke ", 5)) {
        int bot, n; char path[128]; float v;
        if (sscanf(line + 5, "%d %127s %f %d", &bot, path, &v, &n) == 4) { snprintf(g_pokePath, sizeof g_pokePath, "%s", path); g_pokeVal = v; g_pokeBot = bot; g_pokeFrames = n; H->log("poke: bot %d [%s] = %.3f for %d frames", bot, path, v, n); }
    }
    else if (!strncmp(line, "aimscan ", 8)) InterlockedExchange(&g_aimScanBot, atoi(line + 8));
    else if (!strncmp(line, "yawtest ", 8)) {   // yawtest <bot> <radians>: write the soldier's yaw directly for 3 s
        char *sp = strchr(line + 8, ' ');
        g_yawTestVal = sp ? (float)atof(sp + 1) : 0; g_yawTestBot = atoi(line + 8); g_yawTestFrames = 180;
        H->log("yawtest: bot %d yaw %.2f for 180 frames", g_yawTestBot, g_yawTestVal);
    }
    else if (!strcmp(line, "ini")) InterlockedExchange(&g_iniReload, 1);
    else if (!strcmp(line, "forget engine")) { DeleteFileA("Thugbot_learned\\engine.txt"); H->log("engine: forgotten - restart the server to search again"); }
    else if (!strcmp(line, "learnsave")) learn_save();
    else if (!strncmp(line, "presshash ", 10)) { g_testHash = (unsigned)strtoul(line + 10, NULL, 16); H->log("presshash %#x", g_testHash); }
    else if (!strncmp(line, "learnroutes ", 12)) { g_learnRoutes = atoi(line + 12); H->log("learning mode %s", g_learnRoutes ? "on" : "off"); }
    else if (!strncmp(line, "vaulttp ", 8)) { g_vaultTeleport = atoi(line + 8); H->log("vault placing %s", g_vaultTeleport ? "on" : "off"); }
    else if (!strncmp(line, "heli ", 5)) { g_heliMode = atoi(line + 5); H->log("heli: mode %d (0 get out, 1 control probe, 2 fly)", g_heliMode); }
    else if (!strncmp(line, "drivetest", 9)) {
        const char *a = line[9] == ' ' ? line + 10 : "";
        if (*a == 's') drivetest_start(0, (float)atof(a + 1)); else drivetest_start(*a ? (float)atof(a) : 0.0f, 0);
    }
    else if (!strncmp(line, "chat ", 5)) chat_enqueue(line + 5);   // from Thugbot-Chat.ps1: "chat <name>|<text>"
    else if (!strcmp(line, "unblock")) InterlockedExchange(&g_unblockAll, 1);
    else if (!strncmp(line, "cell ", 5)) { if (sscanf(line + 5, "%f %f", &g_cdAt[0], &g_cdAt[1]) == 2) InterlockedExchange(&g_cdDo, 1); }
    else if (!strncmp(line, "bridge ", 7)) { int k = sscanf(line + 7, "%f %f %f %f %f %f", &g_brAt[0], &g_brAt[1], &g_brAt[2], &g_brAt[3], &g_brAt[4], &g_brAt[5]); g_brRef = k == 6; if (k >= 3) InterlockedExchange(&g_brDo, 1); }
    else if (!strncmp(line, "reprobe ", 8)) { if (sscanf(line + 8, "%f %f %f %f %f", &g_rpArea[0], &g_rpArea[1], &g_rpArea[2], &g_rpArea[3], &g_rpArea[4]) == 5) InterlockedExchange(&g_rpDo, 1); }
    else if (!strncmp(line, "routepoints ", 12)) { if (sscanf(line + 12, "%f %f %f %f %f %f", &g_rtFrom[0], &g_rtFrom[1], &g_rtFrom[2], &g_rtTo[0], &g_rtTo[1], &g_rtTo[2]) == 6) InterlockedExchange(&g_rtPoints, 1); }
    else if (!strncmp(line, "routetest ", 10)) InterlockedExchange(&g_routeTestBot, atoi(line + 10));
    else if (!strcmp(line, "perf")) InterlockedExchange(&g_perfDump, 1);
    else if (!strcmp(line, "groundprobe")) InterlockedExchange(&g_groundProbe, 1);
    else if (!strcmp(line, "vehname")) InterlockedExchange(&g_vehName, 1);
    else if (!strcmp(line, "vehscan")) InterlockedExchange(&g_vehScan, 1);
    else if (!strcmp(line, "vehprobe")) InterlockedExchange(&g_vehProbe, 1);
    else if (!strncmp(line, "supplies ", 9)) { g_set.supplies = atoi(line + 9) != 0; H->log("supplies: bots %s kits for teammates", g_set.supplies ? "drop" : "don't drop"); }
    else if (!strncmp(line, "jumpmode ", 9)) { g_jumpMode = atoi(line + 9); H->log("jumpmode %d (0 both, 1 action, 2 button, 3 alternate)", g_jumpMode); }
    else if (!strcmp(line, "refine")) refine_dump();
    else if (!strncmp(line, "refine ", 7)) { g_refineOn = atoi(line + 7) != 0; refine_dump(); }
    else if (!strncmp(line, "vehicles ", 9)) { g_set.vehicles = atoi(line + 9) != 0; H->log("vehicles: bots %s vehicles", g_set.vehicles ? "use" : "don't use"); }
    else if (!strncmp(line, "vehiclestay ", 12)) { g_vehicleStay = atoi(line + 12); H->log("vehicles: spawn-vehicle exit %s", g_vehicleStay ? "off" : "on"); }
    else if (!strcmp(line, "raydiff")) InterlockedExchange(&g_rayDiff, 1);
    else if (!strcmp(line, "routes")) { routes_dump(); human_routes_dump(); }
    else if (!strcmp(line, "camp")) camp_dump();
    else if (!strcmp(line, "ctf")) ctf_dump();
    else if (!strcmp(line, "plantrace")) g_planTrace = 40;
    else if (!strcmp(line, "rescue")) rescue_dump();
    else if (!strcmp(line, "ctfscan")) InterlockedExchange(&g_ctfScanDo, 1);
    else if (!strcmp(line, "camp rescan")) { camp_rescan(); H->log("camp: finding spots again"); }
    else if (!strncmp(line, "camp ", 5)) { g_campShare = atoi(line + 5); H->log("camp: %d%% of each team camp windows", g_campShare); }
    else if (!strncmp(line, "jumptest", 8)) jumptest_start(line[8] == ' ' && isdigit((unsigned char)line[9]) ? atoi(line + 9) : strstr(line, "act") ? 1 : 0);
    else if (!strncmp(line, "turntest", 8)) jumptest_start(strstr(line, "hook") ? 2 : strstr(line, "left") ? 4 : 3);
    else if (!strcmp(line, "routes rebuild")) InterlockedExchange(&g_routesRebuild, 1);
    else if (!strcmp(line, "report")) { InterlockedExchange(&g_reportNow, 1); H->log("report: writing Thugbot_report/%s.html", g_levelKey); }
    else if (!strncmp(line, "strafesign ", 11)) { g_strafeSign = (float)atof(line + 11); H->log("strafe sign %.0f", g_strafeSign); }
    else if (!strncmp(line, "act ", 4)) { g_testAction = atoi(line + 4); H->log("pseudo-player action %d held on every bot", g_testAction); }
    else if (!strncmp(line, "press ", 6)) {
        for (int k = 0; k < BTN_COUNT; k++)
            if (!strcmp(line + 6, kBtnNames[k])) { g_testButtons ^= 1u << k; H->log("press %s %s", kBtnNames[k], g_testButtons & (1u << k) ? "on" : "off"); return; }
        g_testButtons = 0; H->log("press: all off (fire, sprint, jump, interact, melee, reload, zoom)");
    }
    else if (!strncmp(line, "pitch ", 6)) { g_testPitch = (float)atof(line + 6); H->log("pitch input %.2f", g_testPitch); }
    else if (!strcmp(line, "navrebuild")) { InterlockedExchange(&g_navRebuild, 1); H->log("nav: rebuilding this level's grid"); }
    else if (!strncmp(line, "level ", 6)) { brain_level_reset(); snprintf(g_levelKey, sizeof g_levelKey, "%s", line + 6); H->log("level: %s (set by command)", g_levelKey); }
    else if (!strncmp(line, "rayflags ", 9)) { g_rayFlags = (int)strtol(line + 9, NULL, 0); H->log("ray flags %#x", g_rayFlags); }
    else if (!strncmp(line, "drive ", 6)) {
        for (int i = 0; i < DRIVE_COUNT; i++) if (!strcmp(line + 6, kDriveNames[i])) { g_drive = i; H->log("drive %s", kDriveNames[i]); return; }
        H->log("drive: off, still, forward, circle, turn or play");
    }
    else H->log("unknown command: %s", line);
}

__declspec(dllexport) void thugbot_shutdown(void)
{
    unhook_call(&g_hookUpdate);
    unhook_call(&g_hookInput);
    unhook_call(&g_hookCp);
    unhook_call(&g_hookSeq);
    unhook_call(&g_hookCopy);
    unhook_vtable_slot(&g_onDamageSlot, (void *)o_onDamage);
    if (g_ctfSlot) unhook_vtable_slot(&g_ctfSlot, (void *)o_ctfOnMsg);
    for (int k = 0; k < 200 && g_vehScanBusy; k++) Sleep(50);   // the vehicle memory scan thread runs our code: let it finish
    Sleep(100);
    learn_save();
    routes_save();
    strcpy(H->levelKey, g_levelKey);   // the next core keeps the level name until the next level loads
    if (g_nav) { nav_cancel_paths(); nav_free(g_nav); g_nav = NULL; }   // let a frame that is inside our hooks finish before the DLL goes away
    H->log("core shutdown: hooks removed");
}
