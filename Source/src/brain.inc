// Thugbot brain (included by core.c): what each pseudo-player does in "play" mode.
//
//   Conquest (the level has takeable flags): head for the nearest flag the team doesn't own (a per-bot jitter spreads
//   the team over the nearest few), hold it by milling inside it, then move on. TDM: hunt the nearest enemy.
//   Routes come from AltBotMod's raycast grid (nav.c), built in the background on the first round of a level and saved
//   per level; until it is done bots go straight at the target.
//   Stuck (under 0.8 m of progress in 2 s): back off and strafe, block the grid link it was trying, replan, and record
//   the spot in the level's learned file (learn.c) - spots that keep failing stay blocked on the next rounds.
//   Doors and glass: a thin panel across the route (back face within 30 cm, room behind it) is tried with E (a door),
//   then the knife (glass), then a few shots. What worked is remembered per level (Thugbot_learned\<level>.breach) and the
//   nav grid treats thin panels as crossable (at a cost) unless one was learned to stay shut.
#include "nav.h"
#include "learn.h"

#define BRAIN_PATH_MAX 128
typedef struct Bot {
    void *player;
    float path[BRAIN_PATH_MAX * 3];
    int npts, wp;
    float target[3];
    int targetFlag;                 // flag index, -1 = an enemy / milling point
    ULONGLONG planAt, progressAt, unstickUntil, millAt;
    float progressPos[3], progressYaw;
    float fwd, strafe, turn;
    float unstickStrafe;
    int action;                     // pseudo-player action id (1..20) pressed this frame, 0 none
    unsigned buttons;               // BTN_* held this frame (pressed through the input-copy hook)
    float pitch;
    int brStage;                    // breaching a panel: 0 no, 1 E (door), 2 knife, 3 shots
    int brShut;                     // stage 1: still shut after the first E
    ULONGLONG brAt, brNext;         // stage start; no new breach before brNext
    float brHit[3], brFrom[3], brDir[2];
    ULONGLONG directAt, noRouteSince; // no route yet: when the straight line was last checked / since when
    int directOk;
    float planTo[3];                // the target the last plan was for
    ULONGLONG sideAt, sideFloorAt; float sideNudge; int sideFloorOk;   // wall at the side: which way to step, floor there
    ULONGLONG lookAheadAt;
    const char *why;                // what it is doing this frame ("bots" dump)
    int attackFlag;                 // flag index + 1 it last chose to attack (0 none) - kept across target resets
    ULONGLONG attackChooseAt;       // next time it may change the flag it attacks
    ULONGLONG liftSince;            // waiting for / riding an elevator
    float steerPrev, flipYaw, flipWant, flipDist; unsigned flips;
    float stuckAt[3], escape[3]; int stuckRepeat; ULONGLONG stuckFirst, escapeUntil;   // repeated stuck -> escape point
    ULONGLONG takingAt;             // pressing E at a car it is taking
    void *noVeh; ULONGLONG noVehUntil;   // a car it couldn't get into (4 s of E): left alone for a minute
    ULONGLONG seatedSince, seatedStillSince;   // standing still inside a known car (a passenger seat); the car stopped too
    ULONGLONG slowSince, slideUntil; float slideStrafe; int slides;   // scraping along a wall
    float flank[3]; int flankFlag; ULONGLONG flankUntil;   // flanking point on the way to flankFlag
    ULONGLONG spawnAt; float spawnPos[3];   // when / where it last spawned (spawn vehicles)
    int route;                      // the saved route it is on (routes.inc), -1 none
    int brClass;                    // index into g_panelClass: what kind of physics body the panel is
    void *enemy;                    // soldier being engaged
    int stuckSpot;
    ULONGLONG idleAt; float idlePos[3];   // watchdog: where it was 25 s ago
    int avoidFlag;                  // a flag it gave up on (watchdog), -1 none
    ULONGLONG avoidUntil;
    int alive;
    float lastPos[3];
    unsigned seed;
    int inVehicle;                  // seated in a vehicle this frame (reverse / brake input allowed)
    ULONGLONG poolSince; float poolRim[3];   // in a pool, heading for the rim to climb out
    float roll, yawIn, throttleIn;  // extra analog inputs through the input hook (EIARoll, EIAYaw): aircraft
    // explore (no real progress for 12 s): walking an open direction picked by rays
    ULONGLONG progAt, exploreUntil, exploreBlockedAt; float progPos[3], exploreDir, exploreFrom[3], tried[3]; int exploreN, triedN;
    int objRoute;                   // the target is an objective (CTF base / flag): plan with known routes like a flag
    int lroute;                     // the learned route it took for its target (human_routes.inc), -1 none
    ULONGLONG spaceAt, spaceFloorAt; float spacePush; int spaceAhead, spaceHeld, spaceFloorOk;   // (unused since the spacing steer came out)
    ULONGLONG waitDirAt; float waitDir; int waitDirOk;
    ULONGLONG crowdSince, pushUntil; float crowdPos[3];
    ULONGLONG poolTry, poolJumpAt, pitLookAt; float poolWall; int poolJumped;   // getting out of a pool / pit (pool.inc)   // stepping round people: since when, and a push-through spell   // no route yet: the open way towards the target it walks meanwhile
} Bot;
static Bot g_bot[MAX_BOTS];
static void survey_level_reset(void);   // survey.inc
static const unsigned char *drive_mask(void);   // drive.inc
static void camp_level_reset(void);     // camp.inc
static void ctf_level_reset(void);      // ctf.inc
static void rescue_level_reset(void);   // rescue.inc
static float air_below(const float *p);   // parachute.inc
static int pool_exit(Bot *b, void *soldier, const float *pos, ULONGLONG t);   // pool.inc
static int soldier_in_pit(const float *p);   // pool.inc
static void jumps_level_reset(void);   // parachute.inc
static void *g_kickVeh; static ULONGLONG g_kickUntil;   // "!out" (editor.inc): bots in that vehicle get out
static int vehicle_known_stopped_human(int k);   // drive.inc
static unsigned g_jitterTurn, g_jitterStrafe, g_jitterTurnFight, g_jitterSteer;   // jitter meter (perf)
static unsigned g_routesShared;   // routes copied from a teammate (plan)
static Bot *g_stTraceBot; static int g_stTraceFrames;   // 30 frames of the top flipper's steering
static void jitter_dump(void)   // the bots whose steering flipped most since the last "perf"
{
    for (int r = 0; r < 4; r++) {
        int best = -1;
        for (int i = 0; i < H->botCount; i++) if (g_bot[i].flips && (best < 0 || g_bot[i].flips > g_bot[best].flips)) best = i;
        if (best < 0) break;
        Bot *b = &g_bot[best];
        if (r == 0) { g_stTraceBot = b; g_stTraceFrames = 30; }
        H->log("perf: flips %u: %s - last at yaw %.2f want %.2f, %.1f m from its aim point, route %d/%d, target flag %d", b->flips,
               (char *)H->bots[best] + OFF_PLAYER_NAME, b->flipYaw, b->flipWant, b->flipDist, b->wp, b->npts, b->targetFlag);
        b->flips = 0;
    }
    for (int i = 0; i < H->botCount; i++) g_bot[i].flips = 0;
}
// [combat.inc hook] defined in objectives.inc / combat.inc (included after the breach code)
static int objective_target(int i, Bot *b, int team, const float *pos);
static void combat_level_reset(void);
static void routes_level_reset(void);   // routes.inc
static void center_path(float *pts, int n, int from);
static int target_dead(const float *p);
static volatile LONG g_unblockAll;   // "unblock": every temporary grid block back now
static void target_mark_alive(const float *p);
static void lifts_level_reset(void);   // lifts.inc
static int lift_ride(Bot *b, const float *pos, ULONGLONG t);
static int parachute_tick(int i, Bot *b, void *soldier, const float *pos, ULONGLONG t);   // parachute.inc
static void vehicles_level_reset(void);   // drive.inc
static int vehicle_to_take(int i, const float *pos, const float *target, float *vpOut, const char **nameOut);
static void vehicle_taken_note(void);
static void *vehicle_known_ptr(int k);
static int soldier_seated(const float *pos);
static int vehicle_has_driver(int k);
static void *player_vehicle(void *pl, int *seatOut);   // vehicles.inc
static int vehicle_think(int i, Bot *b, void *v, int seat, int team);   // drive.inc
static void combat_frame(void);
static void combat_dump(void);
static void combat_tick(int i, Bot *b, void *soldier, int team);

static NavGrid *g_nav;
static int g_navBuilding;
static int g_stairPass;             // 0 = the staircase re-check of this grid still to run
static BLStore *g_learn;
static ULONGLONG g_learnSavedAt;
static int g_learnBlocked;          // grid links blocked from learned bad spots this level
static int g_testAction;            // "act N": pseudo-player action id N held on every bot (live tests)
static volatile LONG g_navRebuild;
static int g_breachN, g_breachLoaded;   // learned door / pane spots (defined with the breach code below)
static float g_strafeSign = -1;  // which way +strafe goes relative to the heading (-1: checked live on locked bots)
static unsigned g_seenInRange, g_seenBlocked;   // engage() since the last "bots": enemies within 60 m / behind walls

static ULONGLONG now_ms(void) { return GetTickCount64(); }
static float dist2d(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }
static float wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
// The turn input that brings heading `yaw` round to `want`. A pseudo-player's heading can't be turned UP across 0 (in
// our convention; negative turn input raises it): it sticks at exactly 0 and stays there while the input keeps pushing
// that way - the "locked heading" bots that faced one way and walked sideways (turntest, 2026-10-23: at 0, turn -1
// moved none of the stuck soldiers, +1 freed all of them). So a turn that would cross 0 upwards goes the long way round.
static float turn_input(float yaw, float want, float gain)
{
    float err = wrap_pi(want - yaw);
    if (err > 0 && yaw <= 0.001f && yaw + err > 0) err -= 6.2831853f;
    return fmaxf(-1, fminf(1, -err * gain));
}

// A player's soldier, or NULL when it has none - or when it is down. Hardline keeps a downed soldier (waiting for a
// revive) in the world: before this, bots kept fighting bodies, walked right up to them and ignored live enemies, and
// the bodies themselves were the "locked heading" bots (they take no input). Down = health 0, or the engine's own
// "takes input" test says no (that test is switched off if it ever says no for most soldiers: see combat_frame).
static void *bot_soldier_raw(void *player) { return player ? FIELD(player, OFF_PLAYER_SOLDIER, void *) : NULL; }
static float soldier_health(void *soldier);
static int g_downTestOk = 1;
static int soldier_up(void *s)
{
    if (soldier_health(s) == 0) return 0;
    return !g_downTestOk || soldier_takes_input(s);
}
static void *bot_soldier(void *player) { void *s = bot_soldier_raw(player); return s && soldier_up(s) ? s : NULL; }
static const float *soldier_pos(void *soldier) { return (const float *)((char *)soldier + OFF_SOLDIER_POSITION); }
// Heading from the rotation matrix (its 0x550 row points opposite the walking direction). The float at
// OFF_SOLDIER_YAW matched on the first bot tested but reads 0 on others.
static float soldier_yaw(void *soldier)
{
    const float *r = (const float *)((char *)soldier + OFF_SOLDIER_ROT_FWD);
    if (r[0] != 0 || r[2] != 0) return atan2f(-r[0], -r[2]);
    return FIELD(soldier, OFF_SOLDIER_YAW, float);   // same convention; both read 0 on some soldiers
}

// ---- level key, nav grid and learned file ----
// Per-map data files, shared by every mode of a map (the tester 2026-10-25: "why aren't routes shared across modes? Some
// routes can help go from base to flag"). The level key is "<map>_<Mode>" (xp4_snowcrash_CaptureTheFlag0); the map part
// names the file. The first time a map-wide file is wanted, the biggest per-mode file of that map is copied in.
static void map_key(char *out, size_t n)
{
    snprintf(out, n, "%s", g_levelKey);
    char *u = strrchr(out, '_');
    if (u && u > out && u[1] && isdigit((unsigned char)u[strlen(u) - 1])) *u = 0;   // drop "_TurfWarLarge0" etc.
}
static void map_path(char *out, size_t n, const char *dir, const char *suffix)
{
    char key[64]; map_key(key, sizeof key);
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s\\%s%s", dir, key, suffix);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;
    char pat[200], best[200] = ""; ULONGLONG bestSize = 0;
    snprintf(pat, sizeof pat, "%s\\%s_*%s", dir, key, suffix);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (sz > bestSize) { bestSize = sz; snprintf(best, sizeof best, "%s\\%s", dir, fd.cFileName); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (best[0] && CopyFileA(best, out, TRUE)) H->log("files: %s is now shared by every mode of the map (copied from %s)", out, best);
}

static void brain_paths(char *nav, size_t navSize, char *learn, size_t learnSize)
{
    CreateDirectoryA("Thugbot_nav", NULL);
    CreateDirectoryA("Thugbot_learned", NULL);
    map_path(nav, navSize, "Thugbot_nav", ".v8.nav");   // v2: door / glass crossings; v3: vault links over low obstacles; v4: water; v5: rebuilt after the water fix (doors were taken for water)
    map_path(learn, learnSize, "Thugbot_learned", ".v2.learn");   // v2: fresh - v1 learned downed bodies as stuck spots
}

static void learn_save(void)
{
    if (!g_learn || !g_learn->dirty || !g_levelKey[0]) return;
    char nav[160], path[160];
    brain_paths(nav, sizeof nav, path, sizeof path);
    size_t n = bl_text_size(g_learn);
    char *buf = malloc(n + 1);
    if (!buf) return;
    n = bl_write(g_learn, buf, n + 1);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, n, f); fclose(f); bl_mark_saved(g_learn); }
    free(buf);
    H->log("learn: saved %s (%d stuck spot(s), %d heat cell(s))", path, g_learn->nSpots, g_learn->nHeat);
}

static void brain_level_reset(void)
{
    learn_save();
    if (g_nav) { nav_cancel_paths(); nav_free(g_nav); g_nav = NULL; }
    g_navBuilding = 0;
    g_learnBlocked = 0;
    if (g_learn) { bl_free(g_learn); free(g_learn); g_learn = NULL; }
    memset(g_bot, 0, sizeof g_bot);
    combat_level_reset();   // [combat.inc hook]
    routes_level_reset();
    lifts_level_reset();
    vehicles_level_reset();
    nav_clear_links();   // (extra links point into the old grid)
    g_breachLoaded = 0; g_breachN = 0;
    survey_level_reset();
    jumps_level_reset();
    camp_level_reset();
    ctf_level_reset();
    rescue_level_reset();
}

static void learn_open(void)
{
    if (g_learn || !g_levelKey[0]) return;
    g_learn = calloc(1, sizeof *g_learn);
    if (!g_learn) return;
    bl_init(g_learn, g_levelKey);
    char nav[160], path[160];
    brain_paths(nav, sizeof nav, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = malloc(n + 1);
        if (buf && fread(buf, 1, n, f) == (size_t)n) { buf[n] = 0; H->log("learn: %s - %d record(s)", path, bl_load_text(g_learn, buf, n)); }
        free(buf);
        fclose(f);
        bl_fade(g_learn);
    }
    g_learnSavedAt = now_ms();
}

// Learned spots nobody gets through: block the grid links into them for this level.
static void learn_block_bad(void)
{
    if (!g_learn || !g_nav || g_learnBlocked) return;
    for (int i = 0; i < g_learn->nSpots; i++) {
        BLSpot *s = &g_learn->spots[i];
        if (s->kind != BL_FOOT || !bl_spot_bad(s)) continue;
        float tx = s->p[0] + s->h[0] * 4, tz = s->p[2] + s->h[1] * 4;
        g_learnBlocked += nav_block_towards(g_nav, s->p[0], s->p[1], s->p[2], tx, tz);
    }
    H->log("learn: %d grid link(s) blocked at learned bad spots", g_learnBlocked);
    if (!g_learnBlocked) g_learnBlocked = -1;   // done, nothing to block
}

static void nav_start(void)
{
    g_stairPass = 0;   // a new or loaded grid gets the staircase re-check
    if (g_nav || !g_levelKey[0]) return;
    float probe[3] = { 0, 0, 0 };
    if (!ray_cast(probe, probe, NULL, NULL) && !g_rayCaster) return;   // no physics world yet: a grid now would be empty
    char path[160], learn[160];
    brain_paths(path, sizeof path, learn, sizeof learn);
    g_nav = nav_load(path);
    if (g_nav) { H->log("nav: loaded %s (%dx%d cells)", path, g_nav->w, g_nav->h); return; }
    // New level: cover the flags and everyone alive, plus a margin.
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
    int n = 0;
    for (int i = 0; i < g_cpCount; i++) {
        const float *p = (const float *)((char *)g_cps[i] + OFF_CP_POSITION);
        if (p[0] == 0 && p[2] == 0) continue;
        if (p[0] < lo[0]) lo[0] = p[0]; if (p[2] < lo[1]) lo[1] = p[2]; if (p[0] > hi[0]) hi[0] = p[0]; if (p[2] > hi[1]) hi[1] = p[2]; n++;
    }
    for (int i = 0; i < H->botCount; i++) {
        void *s = bot_soldier(H->bots[i]);
        if (!s) continue;
        const float *p = soldier_pos(s);
        if (p[0] < lo[0]) lo[0] = p[0]; if (p[2] < lo[1]) lo[1] = p[2]; if (p[0] > hi[0]) hi[0] = p[0]; if (p[2] > hi[1]) hi[1] = p[2]; n++;
    }
    if (n < 2) return;   // wait until someone has spawned
    const float margin = 150;
    g_nav = nav_create(g_levelKey, lo[0] - margin, lo[1] - margin, hi[0] + margin, hi[1] + margin, 2.0f);
    g_navBuilding = g_nav != NULL;
    if (g_nav) H->log("nav: building %s, %dx%d cells (%.0f x %.0f m)", g_levelKey, g_nav->w, g_nav->h, g_nav->w * 2.0f, g_nav->h * 2.0f);
}

static void nav_tick(void)
{
    if (!g_nav) { nav_start(); return; }
    if (g_navBuilding) {
        int phase = g_nav->phase;
        if (nav_build_step(g_nav, ray_cast, 12000, 7.0)) {   // (the server sleeps ~75% of each frame: building 2-3x faster cost nothing visible)
            g_navBuilding = 0;
            char path[160], learn[160];
            brain_paths(path, sizeof path, learn, sizeof learn);
            H->log("nav: built %s - %u rays in %.0f ms, %u vault link(s) -> %s", g_levelKey, g_nav->raysCast, g_nav->buildMs, nav_vault_links(), nav_save(g_nav, path) ? "saved" : "SAVE FAILED");
            g_stairPass = 0;
        } else if (phase != g_nav->phase || H->frame % 600 == 0) {
            int cells = g_nav->w * g_nav->h;
            H->log("nav: phase %d %.0f%% (%u rays)", g_nav->phase, 100.0 * g_nav->cursor / (g_nav->phase ? cells * 4.0 : cells), g_nav->raysCast);
        }
        return;
    }
    // AltBotMod's staircase re-check (real stairs show intermediate heights, a wall face does not), a few thousand rays
    // a frame until done - once per grid.
    if (g_stairPass == 0) {
        int removed = 0;
        if (nav_fix_stairs_step(g_nav, ray_cast, 3000, &removed)) {
            g_stairPass = 1;
            H->log("nav: stairs checked - %d false staircase link(s) dropped, %d added, %d handrail top(s) made unwalkable",
                   removed, nav_stairs_added(), nav_rails_removed());
        }
        return;
    }
    // Grow the grid when anyone is outside it (the first build covers the flags and the first spawns plus 150 m).
    if (H->frame % 300 == 0) {
        float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f }; int out = 0;
        for (int j = 0; j < g_playerCount; j++) {
            void *s = bot_soldier_raw(g_players[j]);
            if (!s) continue;
            const float *p = soldier_pos(s);
            if (!nav_covers(g_nav, p[0], p[2], 20)) out++;
            lo[0] = fminf(lo[0], p[0]); lo[1] = fminf(lo[1], p[2]); hi[0] = fmaxf(hi[0], p[0]); hi[1] = fmaxf(hi[1], p[2]);
        }
        if (out) {
            float gx1 = g_nav->x0 + (g_nav->w - 1) * g_nav->cell, gz1 = g_nav->z0 + (g_nav->h - 1) * g_nav->cell;
            NavGrid *ng = nav_expand_bounds(g_nav, fminf(g_nav->x0, lo[0] - 80), fminf(g_nav->z0, lo[1] - 80), fmaxf(gx1, hi[0] + 80), fmaxf(gz1, hi[1] + 80));
            if (ng) {
                H->log("nav: %d player(s) outside the grid - growing it from %dx%d to %dx%d cells", out, g_nav->w, g_nav->h, ng->w, ng->h);
                nav_cancel_paths(); nav_free(g_nav); g_nav = ng; g_navBuilding = 1;
                for (int i = 0; i < MAX_BOTS; i++) g_bot[i].npts = 0;
                return;
            }
        }
    }
    learn_block_bad();
    nav_unblock_expired(g_nav, InterlockedExchange(&g_unblockAll, 0) ? 1 : 0);
    // The planner runs in slices each frame. With 60 bots it took 4 ms of every 16.7 ms frame (perf, 2026-10-23) and the
    // server had no time left: 1.5 ms (2.5 when the queue is long) above 40 bots.
    // (2026-10-26: 63 bots kept 14 requests queued and bots stood "waiting for a route" - more time when the queue is long)
    nav_process_paths(H->botCount > 40 ? (nav_pending_paths() > 12 ? 6.0 : nav_pending_paths() > 4 ? 4.0 : 2.5) : (nav_pending_paths() > 4 ? 6.0 : 4.0));
}


// ---- targets ----
static int flag_takeable(int f)
{
    const float *p = (const float *)((char *)g_cps[f] + OFF_CP_POSITION);
    return p[0] != 0 || p[2] != 0;
}

static int conquest_flags(void)
{
    int n = 0;
    for (int f = 0; f < g_cpCount; f++) n += flag_takeable(f);
    return n;
}

static void choose_target(int i, Bot *b, int team, const float *pos)
{
    if (objective_target(i, b, team, pos)) return;   // [combat.inc hook] roles, cover and chase (objectives.inc)
    ULONGLONG t = now_ms();
    if (conquest_flags()) {
        // The nearest flag not ours; jitter (0..60 m per bot and flag) spreads the team. All ours: a random one.
        int best = -1; float bestScore = 1e9f;
        for (int f = 0; f < g_cpCount; f++) {
            if (!flag_takeable(f) || FIELD(g_cps[f], OFF_CP_OWNER_TEAM, int) == team) continue;
            const float *p = (const float *)((char *)g_cps[f] + OFF_CP_POSITION);
            float score = dist2d(pos, p) + (float)((b->seed * 2654435761u + f * 40503u) % 60);
            if (score < bestScore) { bestScore = score; best = f; }
        }
        if (best < 0) {
            int k = (int)((b->seed + t / 30000) % (unsigned)conquest_flags());
            for (int f = 0; f < g_cpCount; f++) if (flag_takeable(f) && k-- == 0) { best = f; break; }
        }
        if (best >= 0) {
            const float *p = (const float *)((char *)g_cps[best] + OFF_CP_POSITION);
            float d = dist2d(pos, p);
            if (best == b->targetFlag && d < 8) {
                // On the flag: mill around inside it (a new spot within 6 m every few seconds) until it is ours.
                if (t > b->millAt) {
                    float a = (float)((b->seed + (unsigned)t) % 628) / 100.0f, r = 2.0f + (float)((b->seed ^ (unsigned)t) % 40) / 10.0f;
                    b->target[0] = p[0] + r * sinf(a); b->target[1] = p[1]; b->target[2] = p[2] + r * cosf(a);
                    b->millAt = t + 3000 + (b->seed % 2000);
                    b->npts = 0;
                }
                return;
            }
            if (best != b->targetFlag) b->npts = 0;
            b->targetFlag = best;
            memcpy(b->target, p, sizeof b->target);
            return;
        }
    }
    // TDM (or no flags): the nearest enemy bot.
    b->targetFlag = -1;
    float bestD = 1e9f;
    for (int j = 0; j < H->botCount; j++) {
        if (j == i || FIELD(H->bots[j], OFF_PLAYER_TEAM, int) == team) continue;
        void *s = bot_soldier(H->bots[j]);
        if (!s) continue;
        float d = dist2d(pos, soldier_pos(s));
        if (d < bestD) { bestD = d; memcpy(b->target, soldier_pos(s), sizeof b->target); }
    }
}

// ---- combat: face the nearest enemy in sight (60 m, chest-to-chest ray) and fire ----
static void engage(int i, Bot *b, void *soldier, int team)
{
    (void)i;
    b->action = g_testAction;
    b->enemy = NULL;
    const float *p = soldier_pos(soldier);
    float best = 60;
    void *pick = NULL;
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        int t = FIELD(pl, OFF_PLAYER_TEAM, int);
        void *s = bot_soldier(pl);
        if (!s || s == soldier || !t || t == team) continue;
        float d = dist2d(p, soldier_pos(s));
        if (d >= best) continue;
        g_seenInRange++;
        const float *q = soldier_pos(s);
        float a[3] = { p[0], p[1] + 1.4f, p[2] }, e[3] = { q[0], q[1] + 1.2f, q[2] }, hit[3];
        // Line of sight: the ray ignores characters, so any hit short of the enemy is a wall in the way.
        if (ray_cast(a, e, hit, NULL) && dist2d(a, hit) < d - 0.8f) { g_seenBlocked++; continue; }
        best = d; pick = s;
    }
    if (!pick) return;
    b->enemy = pick;
    const float *q = soldier_pos(pick);
    float want = atan2f(q[0] - p[0], q[2] - p[2]), err = wrap_pi(want - soldier_yaw(soldier));
    b->turn = turn_input(soldier_yaw(soldier), want, 3.0f);
    if (best < 25) b->fwd = 0.3f;        // close: slow down and shoot
    // Fire in bursts when roughly on target (pulsed: semi-automatic weapons need the trigger released between shots).
    if (fabsf(err) < 0.12f && ((H->frame / 3) & 1)) b->buttons |= BTN_FIRE;
}

// ---- doors and glass ----
#define MAX_BREACH 4096
// 'o' opens with E, 'b' breaks (knife / shots), 'n' stays shut (two failed tries), '?' failed once (not saved)
typedef struct Breach { float p[3]; char kind; unsigned char fails; ULONGLONG openedAt; } Breach;   // openedAt: run time, not saved
static Breach g_breach[MAX_BREACH];
static int g_breachN, g_breachLoaded;
static unsigned g_breachTried, g_breachOpened, g_breachBroken;
static unsigned char g_lastHitAtPanel[0x20];   // the panel ray's hit record past position / normal

static void breach_path(char *out, size_t n) { map_path(out, n, "Thugbot_learned", ".breach"); }
static int breach_find(const float *p, float r)
{
    int best = -1; float bd = r * r;
    for (int k = 0; k < g_breachN; k++) {
        float dx = g_breach[k].p[0] - p[0], dy = g_breach[k].p[1] - p[1], dz = g_breach[k].p[2] - p[2], d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}
static void breach_load(void)
{
    if (g_breachLoaded || !g_levelKey[0]) return;
    g_breachLoaded = 1;
    g_breachN = 0;
    char path[160], line[96];
    breach_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && g_breachN < MAX_BREACH) {
        Breach *e = &g_breach[g_breachN];
        if (sscanf(line, " %c %f %f %f", &e->kind, &e->p[0], &e->p[1], &e->p[2]) == 4) g_breachN++;
    }
    fclose(f);
    H->log("breach: %s - %d learned door / pane spot(s)", path, g_breachN);
}
// Takes the knifeable-panel crossings (fences[] even bits) out of the grid within 2.5 m of p, both sides. The builder
// takes any thin body (30 cm or less) for a pane / fence - on Code Blue that is every hospital wall (class 0x2819d40):
// routes went straight through walls and groups of bots stood pushing at them (the tester 2026-10-25: "groups of bots all get
// stuck behind walls or objects and don't try to find routes"). Spots written off ('n') and walls bots got stuck at
// are cut; the next plans go round. Not saved with the grid: re-applied from the learned 'n' spots on each load.
static unsigned g_fenceCuts;
static void routes_cut_near(const float *p);   // routes.inc
static unsigned g_fenceCuts;
// A low wall a soldier vaults (the tester, mp_hills 2026-10-26: "a lot of cops are stuck behind a vaultable wall... vaulting is
// still broken"): something within 1.5 m at knee height (0.6 m over the floor) in one of 8 directions, nothing at 1.8 m
// in that direction. p: a point at the obstacle (any height within 2 m of the floor next to it).
static int low_wall_at(const float *p)
{
    float fl[3], up[3] = { p[0], p[1] + 1.0f, p[2] }, dn[3] = { p[0], p[1] - 2.5f, p[2] };
    if (!ray_cast(up, dn, fl, NULL)) return 0;
    for (int d = 0; d < 8; d++) {
        float a = d * 0.785398f, dx = sinf(a) * 1.5f, dz = cosf(a) * 1.5f;
        float k0[3] = { fl[0] - dx, fl[1] + 0.6f, fl[2] - dz }, k1[3] = { fl[0] + dx, fl[1] + 0.6f, fl[2] + dz };
        float h0[3] = { fl[0] - dx, fl[1] + 1.8f, fl[2] - dz }, h1[3] = { fl[0] + dx, fl[1] + 1.8f, fl[2] + dz };
        if (ray_cast(k0, k1, NULL, NULL) && !ray_cast(h0, h1, NULL, NULL)) return 1;
    }
    return 0;
}

static int fence_cut_at(const float *p, const char *why)
{
    NavGrid *g = g_nav;
    if (!g || g_navBuilding || !g->fences) return 0;
    int r = (int)(2.5f / g->cell) + 1, n = 0;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            int cell = nav_cell_index(g, p[0] + dx * g->cell, p[2] + dz * g->cell);
            if (cell < 0) continue;
            for (int l = 0; l < NAV_LAYERS; l++) {
                int node = cell * NAV_LAYERS + l;
                if (!(g->flags[node] & NAV_WALKABLE) || fabsf(g->height[node] - p[1]) > 2.0f) continue;
                if (g->fences[node] & 0x55) { g->fences[node] &= (unsigned char)~0x55; n++; }
            }
        }
    if (n) routes_cut_near(p);
    if (n) { g_fenceCuts += n; static unsigned logged; if (logged++ < 40) H->log("grid: %d panel crossing(s) cut at %.0f %.0f %.0f (%s)", n, p[0], p[1], p[2], why); }
    return n;
}
static void breach_learn(const float *p, char kind)
{
    int k = breach_find(p, 1.2f);
    if (k >= 0) {
        if (kind != 'n') g_breach[k].fails = 0;   // it opened / broke: start counting failures again
        if (g_breach[k].kind == kind) return;
        // Written off after a second failure - also a door that opened once before (the tester 2026-10-25: "there are doors that
        // cannot be opened, why are bots trying?" - a spot that ever opened used to be tried for ever; some doors are
        // locked for the round, some were "opened" by a player walking past a doorway next to them). Not within 10 min of
        // opening this run.
        if (kind == 'n') {
            if (++g_breach[k].fails < 2) return;
            if (g_breach[k].openedAt && now_ms() - g_breach[k].openedAt < 600000) return;
        }
        g_breach[k].kind = kind;
        if (kind == 'n') fence_cut_at(g_breach[k].p, "written off");
    } else if (g_breachN < MAX_BREACH) {
        k = g_breachN++;
        memcpy(g_breach[k].p, p, 12); g_breach[k].kind = kind == 'n' ? '?' : kind; g_breach[k].fails = kind == 'n';
        if (kind == 'n') return;
    } else return;
    char path[160];
    breach_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_breachN; i++) if (g_breach[i].kind != '?') fprintf(f, "%c %.2f %.2f %.2f\n", g_breach[i].kind, g_breach[i].p[0], g_breach[i].p[1], g_breach[i].p[2]);
    fclose(f);
}
// Something thin within `reach` along (fx, fz) at chest height from pos (a door, a pane), with room behind it: the back
// face within 30 cm (a wall is thicker), clear for a metre beyond, floor there near our level.
static int panel_dir(const float *pos, float fx, float fz, float reach, float *hitOut, float *fromOut)
{
    float a[3] = { pos[0], pos[1] + 1.2f, pos[2] }, b[3] = { pos[0] + fx * reach, pos[1] + 1.2f, pos[2] + fz * reach }, h[3], h2[3], g[3], nrm[3];
    if (!ray_cast(a, b, h, nrm)) return 0;
    // Facing us: an open door's leaf, hit edge-on or at a glancing angle beside the doorway, is not "shut".
    float facing = -(nrm[0] * fx + nrm[2] * fz) / fmaxf(0.01f, sqrtf(nrm[0] * nrm[0] + nrm[2] * nrm[2]));
    if (facing < 0.6f) return 0;
    // Shut across its width: 35 cm to either side is blocked at about the same distance too (a half-open door or a
    // gap beside the leaf: walk through instead of pressing E, which would close it).
    {
        float hitD = (h[0] - a[0]) * fx + (h[2] - a[2]) * fz;
        for (int side = -1; side <= 1; side += 2) {
            float ox = -fz * 0.35f * side, oz = fx * 0.35f * side, h3[3];
            float a2[3] = { a[0] + ox, a[1], a[2] + oz }, b2[3] = { b[0] + ox, b[1], b[2] + oz };
            if (!ray_cast(a2, b2, h3, NULL)) return 0;
            if (fabsf((h3[0] - a2[0]) * fx + (h3[2] - a2[2]) * fz - hitD) > 0.5f) return 0;
        }
    }
    if (!ray_cast(a, b, h, NULL)) return 0;   // (again: the side rays replaced the last hit record)
    memcpy(g_lastHitAtPanel, g_lastHit.rest, sizeof g_lastHitAtPanel);
    float back[3] = { h[0] + fx * 0.45f, h[1], h[2] + fz * 0.45f }, front[3] = { h[0] - fx * 0.05f, h[1], h[2] - fz * 0.05f };
    if (ray_cast(back, front, h2, NULL) && (h2[0] - h[0]) * fx + (h2[2] - h[2]) * fz > 0.30f) return 0;
    float beyond[3] = { back[0] + fx, back[1], back[2] + fz };
    if (ray_cast(back, beyond, NULL, NULL)) return 0;
    float down[3] = { back[0], pos[1] - 1.5f, back[2] };
    if (!ray_cast(back, down, g, NULL) || fabsf(g[1] - pos[1]) > 1.2f) return 0;
    memcpy(hitOut, h, 12); memcpy(fromOut, a, 12);
    return 1;
}

// For the nav builder: the ray that just hit met a thin panel (door, glass) - crossable at a price - unless that spot was
// learned to stay shut.
// Water: the sea's physics body class. Riptide 2026-10-23 ("groundprobe"): bots in the sea had the surface 0.2 - 0.7 m
// above their feet, on a body of class RVA 0x2808500. Learned again live (a submerged soldier's surface class) in case
// another map's water differs.
static uintptr_t g_waterVtRva = 0x2808500;
static int last_hit_is_water(void)
{
    uintptr_t body = *(uintptr_t *)g_lastHit.rest;
    return body && !IsBadReadPtr((void *)body, 8) && *(uintptr_t *)body == H->image + g_waterVtRva;
}
static void water_learn(void)
{
    static ULONGLONG at; static int k;
    ULONGLONG t = now_ms();
    if (t - at < 500 || !H->botCount) return;
    at = t;
    void *s = bot_soldier(H->bots[k++ % H->botCount]);
    if (!s) return;
    const float *p = soldier_pos(s);
    float a[3] = { p[0], p[1] + 2, p[2] }, b[3] = { p[0], p[1] - 1, p[2] }, h[3];
    if (!ray_cast(a, b, h, NULL) || h[1] - p[1] < 0.3f || h[1] - p[1] > 1.6f) return;   // something 0.3 - 1.6 m above its feet that the ray stops at: the surface it is under
    uintptr_t body = *(uintptr_t *)g_lastHit.rest, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
    // Water is a wide flat sheet: the same class at the same height 2.5 m away on all four sides (a ceiling, a door,
    // a table or a stair a bot stood under passed the old test and was taken for water - doors became "swimming").
    for (int d = 0; d < 4 && vt; d++) {
        float a2[3] = { p[0] + (d == 0 ? 2.5f : d == 1 ? -2.5f : 0), h[1] + 1.0f, p[2] + (d == 2 ? 2.5f : d == 3 ? -2.5f : 0) }, b2[3] = { a2[0], h[1] - 0.5f, a2[2] }, h2[3];
        uintptr_t bd = ray_cast(a2, b2, h2, NULL) ? *(uintptr_t *)g_lastHit.rest : 0;
        if (!bd || IsBadReadPtr((void *)bd, 8) || *(uintptr_t *)bd - H->image != vt || fabsf(h2[1] - h[1]) > 0.08f) vt = 0;
    }
    if (vt && vt != g_waterVtRva && vt < 0x6000000) { H->log("water: a bot is under a surface of class %#llx - taking that for water", (unsigned long long)vt); g_waterVtRva = vt; }
}

static int nav_hit_class(void)
{
    if (last_hit_is_water()) return NAV_HIT_WATER;
    RayHit savedHit = g_lastHit;
    float from[3], to[3], h[3], h2[3];
    memcpy(from, g_lastFrom, 12); memcpy(to, g_lastTo, 12);
    memcpy(h, &g_lastHit.pos, 12);
    float dx = to[0] - from[0], dz = to[2] - from[2], l = sqrtf(dx * dx + dz * dz);
    int thin = 0; float thick = 1;
    if (l > 0.01f) {
        dx /= l; dz /= l;
        float back[3] = { h[0] + dx * 0.45f, h[1], h[2] + dz * 0.45f }, front[3] = { h[0] - dx * 0.05f, h[1], h[2] - dz * 0.05f };
        int backHit = ray_cast(back, front, h2, NULL);
        thick = backHit ? (h2[0] - h[0]) * dx + (h2[2] - h[2]) * dz : 0;
        thin = !backHit || thick <= 0.30f;
        if (thin) { int k = breach_find(h, 1.2f); if (k >= 0 && g_breach[k].kind == 'n') thin = 0; }
        // Only doors (classes that opened with E) and panels bots or players actually got through count as crossable.
        // Any thin body used to: Code Blue's hospital walls (0x2819d40, 10 - 30 cm) were all "panes", routes ran through
        // them and groups of bots walked into walls beside open doors (the tester 2026-10-25). Grids v7.
        if (thin) {
            uintptr_t body = *(uintptr_t *)savedHit.rest, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
            int k = breach_find(h, 1.5f);
            // (glass - a pane under 8 cm - stays crossable whatever its class: Code Blue's club lounge (flag B) has glass
            // doors, and without them its floor was a 61-node pocket no route reached. Walls are 10 cm and more.)
            if (vt != 0x2808310 && vt != 0x2819780 && thick > 0.08f && !(k >= 0 && (g_breach[k].kind == 'o' || g_breach[k].kind == 'b'))) thin = 0;
        }
    }
    g_lastHit = savedHit; memcpy(g_lastFrom, from, 12); memcpy(g_lastTo, to, 12);
    return thin ? NAV_HIT_FENCE : 0;
}

// What kind of physics body a panel ray hit (the vtable of the body pointer in the hit record), and how breaching each
// kind has gone. A kind that is tried again and again and never opens or breaks is a wall, not a door: skipped.
#define MAX_PANEL_CLASSES 32
typedef struct PanelClass { uintptr_t vt; unsigned tries, opened, broken, failed; } PanelClass;
static PanelClass g_panelClass[MAX_PANEL_CLASSES];
static int g_panelClassN;
static int panel_class_of_last_hit(void)
{
    uintptr_t body = *(uintptr_t *)g_lastHitAtPanel, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
    for (int k = 0; k < g_panelClassN; k++) if (g_panelClass[k].vt == vt) return k;
    if (g_panelClassN >= MAX_PANEL_CLASSES) return -1;
    memset(&g_panelClass[g_panelClassN], 0, sizeof g_panelClass[0]);
    g_panelClass[g_panelClassN].vt = vt;
    H->log("doors: new body kind vt %#llx", (unsigned long long)vt);
    return g_panelClassN++;
}
static int panel_class_hopeless(int k)
{
    if (k < 0) return 0;
    PanelClass *c = &g_panelClass[k];
    return c->tries >= 15 && c->opened + c->broken == 0;
}
static void doors_dump(void)
{
    H->log("doors: %d body kind(s) met by panel rays (vt = class of the physics body)", g_panelClassN);
    for (int k = 0; k < g_panelClassN; k++) {
        PanelClass *c = &g_panelClass[k];
        H->log("doors: vt %#llx - %u tries, %u opened with E, %u broken, %u stayed shut%s", (unsigned long long)c->vt, c->tries, c->opened,
               c->broken, c->failed, panel_class_hopeless(k) ? " - skipped now (never opens: a wall)" : c->opened > c->broken ? " - DOORS" : c->broken ? " - GLASS" : "");
    }
}

#include "survey.inc"

// A thin panel across the way to `aim`: start trying it (E, then the knife, then shots). Checked every 250 ms per bot.
static int breach_start(Bot *b, const float *pos, const float *aim, ULONGLONG t)
{
    if (t < b->brNext) return 0;
    b->brNext = t + 250;
    float dx = aim[0] - pos[0], dz = aim[2] - pos[2], dl = sqrtf(dx * dx + dz * dz);
    if (dl < 0.5f) return 0;
    dx /= dl; dz /= dl;
    float h[3], a[3];
    if (!panel_dir(pos, dx, dz, fminf(1.8f, dl + 0.5f), h, a)) return 0;
    int k = breach_find(h, 1.2f);
    if (k >= 0 && g_breach[k].kind == 'n') return 0;
    // Opened in the last 10 s (a door still swinging, or one whose open leaf the ray meets): pressing E would shut it.
    if (k >= 0 && g_breach[k].openedAt && t - g_breach[k].openedAt < 10000) { b->brNext = t + 1000; return 0; }
    int cls = panel_class_of_last_hit();
    if (panel_class_hopeless(cls) && !(k >= 0 && g_breach[k].kind != 'n')) return 0;   // that kind of body never opens
    b->brClass = cls;
    if (cls >= 0) g_panelClass[cls].tries++;
    // Someone else is already opening this door: wait for them (two E presses cancel each other out).
    for (int o = 0; o < MAX_BOTS; o++)
        if (&g_bot[o] != b && g_bot[o].brStage && dist2d(g_bot[o].brHit, h) < 1.5f && fabsf(g_bot[o].brHit[1] - h[1]) < 1.5f) { b->brNext = t + 600; return 0; }
    b->brStage = k >= 0 && g_breach[k].kind == 'b' ? 2 : 1;   // a known pane: straight to the knife (by body class it was wrong: "glass" classes are walls too)
    b->brShut = 1;
    b->brAt = t;
    memcpy(b->brHit, h, 12); memcpy(b->brFrom, a, 12);
    b->brDir[0] = dx; b->brDir[1] = dz;
    if (g_breachTried++ < 40) {   // with what the ray met (the hit record past the normal: body, material - being mapped)
        unsigned *r = (unsigned *)g_lastHitAtPanel;
        uintptr_t body = *(uintptr_t *)r, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body : 0;
        H->log("breach: %s tries %s at %.1f %.1f %.1f - body %p vt %#llx rest %08x %08x %08x %08x %08x %08x", (char *)b->player + OFF_PLAYER_NAME,
               b->brStage == 1 ? "E" : "the knife", h[0], h[1], h[2], (void *)body, (unsigned long long)(vt ? vt - H->image : 0), r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    return 1;
}

static void breach_tick(Bot *b, void *soldier, ULONGLONG t)
{
    static const ULONGLONG kStageMs[4] = { 0, 2600, 1600, 900 };
    float want = atan2f(b->brDir[0], b->brDir[1]);
    // Still shut after two presses: some doors open from a keypad on the wall beside them - face 35 degrees to one
    // side, press E, then the other side (1.1 - 2.5 s into the E stage).
    ULONGLONG el0 = t - b->brAt;
    if (b->brStage == 1 && b->brShut && el0 >= 1100) want += el0 < 1800 ? 0.6f : -0.6f;
    b->turn = turn_input(soldier_yaw(soldier), want, 3.0f);
    b->fwd = b->brStage == 1 ? 0.25f : 0;   // lean on a door (some swing open when pushed)
    b->strafe = 0;
    b->pitch = 0;
    unsigned phase = (unsigned)((t - b->brAt) / 120);
    // E: one press (held 250 ms); a second one after 800 ms only if the door is still shut. Tapping on (as before) shut
    // the door again as soon as it had opened.
    ULONGLONG el = t - b->brAt;
    if (b->brStage == 1 && (el < 250 || (b->brShut && ((el >= 800 && el < 1050) || (el >= 1450 && el < 1700) || (el >= 2150 && el < 2400))))) b->buttons |= BTN_INTERACT;
    if (b->brStage == 1 && el >= 300) {   // open yet? stop at once
        float probe[3] = { b->brHit[0] + b->brDir[0] * 0.1f, b->brHit[1], b->brHit[2] + b->brDir[1] * 0.1f };
        b->brShut = ray_cast(b->brFrom, probe, NULL, NULL);
        if (!b->brShut) el = kStageMs[1];   // go straight to the result below
    }
    if (b->brStage == 1 && el >= kStageMs[1]) b->brAt = t - kStageMs[1];
    if (b->brStage == 2 && (phase % 4) == 0) b->buttons |= BTN_MELEE;
    if (b->brStage == 3 && (phase & 1)) b->buttons |= BTN_FIRE;
    if (t - b->brAt < kStageMs[b->brStage]) return;
    float through[3] = { b->brHit[0] + b->brDir[0] * 0.1f, b->brHit[1], b->brHit[2] + b->brDir[1] * 0.1f };
    if (!ray_cast(b->brFrom, through, NULL, NULL)) {
        breach_learn(b->brHit, b->brStage == 1 ? 'o' : 'b');
        { int k = breach_find(b->brHit, 1.2f); if (k >= 0) g_breach[k].openedAt = t; }
        if (b->brStage == 1) g_breachOpened++; else g_breachBroken++;
        if (b->brClass >= 0) { if (b->brStage == 1) g_panelClass[b->brClass].opened++; else g_panelClass[b->brClass].broken++; }
        H->log("breach: %s got through (%s) - body vt %#llx", (char *)b->player + OFF_PLAYER_NAME, b->brStage == 1 ? "door opened" : b->brStage == 2 ? "knifed" : "shot",
               (unsigned long long)(b->brClass >= 0 ? g_panelClass[b->brClass].vt : 0));
        b->brStage = 0; b->brNext = t + 2000;
        return;
    }
    if (b->brStage < 3) { b->brStage++; b->brAt = t; return; }
    if (b->brClass >= 0) g_panelClass[b->brClass].failed++;
    breach_learn(b->brHit, 'n');
    if (g_nav && !g_navBuilding) nav_block_towards(g_nav, b->brFrom[0], b->brFrom[1] - 1.2f, b->brFrom[2], b->brHit[0], b->brHit[2]);
    b->brStage = 0; b->brNext = t + 20000; b->npts = 0;
}

// [combat.inc hook]
static int bot_block_towards(float x, float y, float z, float tx, float tz);   // (below: not when stuck in a crowd)
#include "combat.inc"
#include "objectives.inc"
#include "movement.inc"
#include "camp.inc"
#include "routes.inc"
#include "human_routes.inc"
#include "route_refine.inc"
#include "ctf.inc"
#include "rescue.inc"

static int lr_users(int r)
{
    int n = 0;
    for (int o = 0; o < H->botCount; o++) if (g_bot[o].alive && g_bot[o].lroute == r && g_bot[o].npts && g_bot[o].wp < g_bot[o].npts) n++;
    return n;
}

static void nav_rebuild(void)
{
    char path[160], learn[160];
    brain_paths(path, sizeof path, learn, sizeof learn);
    DeleteFileA(path);
    if (g_nav) { nav_cancel_paths(); nav_free(g_nav); g_nav = NULL; }
    g_navBuilding = 0; g_learnBlocked = 0;
    for (int i = 0; i < MAX_BOTS; i++) g_bot[i].npts = 0;
}

// ---- movement ----
// Floor all the way from a to b: a ray down every metre finds ground within 0.7 m of the straight line between their
// heights. Rays alone only see walls: shortcuts and centring used to step off the open side of stairs without rails.
static int floor_continuous(const float *a, const float *b)
{
    float d = dist2d(a, b);
    int steps = (int)d + 1;
    for (int k = 1; k < steps; k++) {
        float f = (float)k / steps, x = a[0] + (b[0] - a[0]) * f, z = a[2] + (b[2] - a[2]) * f, y = a[1] + (b[1] - a[1]) * f, h[3];
        float up[3] = { x, y + 1.0f, z }, dn[3] = { x, y - 1.2f, z };
        if (!ray_cast(up, dn, h, NULL) || fabsf(h[1] - y) > 0.7f) return 0;
    }
    return 1;
}

// Pull route points away from walls: chest-height rays in 8 directions (3 m); walls closer than that push the point
// away (both walls of a hallway push equally - it settles in the middle). The moved point must still have floor under
// it and a clear line from the point before and to the point after. Up to 1.2 m per point, from point `from` on.
static unsigned g_centered, g_centerRays;
static void center_path(float *pts, int n, int from)
{
    for (int k = from > 1 ? from : 1; k < n - 1; k++) {
        float *p = &pts[k * 3], c[3] = { p[0], p[1] + 1.2f, p[2] }, push[2] = { 0, 0 };
        for (int d = 0; d < 8; d++) {
            float a = d * 0.785398f, dx = sinf(a), dz = cosf(a), e[3] = { c[0] + dx * 3, c[1], c[2] + dz * 3 }, h[3];
            g_centerRays++;
            if (!ray_cast(c, e, h, NULL)) continue;
            float dist = sqrtf((h[0] - c[0]) * (h[0] - c[0]) + (h[2] - c[2]) * (h[2] - c[2])), w = (3 - dist) / 3;
            push[0] -= dx * w; push[1] -= dz * w;
        }
        float len = sqrtf(push[0] * push[0] + push[1] * push[1]);
        if (len < 0.15f) continue;
        float move = fminf(1.2f, len * 1.1f), q[3] = { p[0] + push[0] / len * move, p[1], p[2] + push[1] / len * move };
        float up[3] = { q[0], p[1] + 1.5f, q[2] }, dn[3] = { q[0], p[1] - 1.5f, q[2] }, floor[3];
        if (!ray_cast(up, dn, floor, NULL) || fabsf(floor[1] - p[1]) > 0.6f) continue;
        q[1] = floor[1];
        const float *prev = &pts[(k - 1) * 3], *next = &pts[(k + 1) * 3];
        float a1[3] = { prev[0], prev[1] + 1.2f, prev[2] }, b1[3] = { q[0], q[1] + 1.2f, q[2] }, a2[3] = { next[0], next[1] + 1.2f, next[2] };
        float k1[3] = { prev[0], prev[1] + 0.6f, prev[2] }, k2[3] = { q[0], q[1] + 0.6f, q[2] }, k3[3] = { next[0], next[1] + 0.6f, next[2] };
        if (ray_cast(a1, b1, NULL, NULL) || ray_cast(b1, a2, NULL, NULL) || ray_cast(k1, k2, NULL, NULL) || ray_cast(k2, k3, NULL, NULL)) continue;
        if (!floor_continuous(prev, q) || !floor_continuous(q, next)) continue;   // (not off the side of a stair or a ledge)
        memcpy(p, q, 12);
        g_centered++;
    }
}

// Targets the planner just found unreachable (A* went through every node it could reach): asking again within 30 s
// fails at once instead of tying up the shared planner for seconds.
#define MAX_DEAD_TARGETS 32
static struct { float p[3]; ULONGLONG until; } g_deadTargets[MAX_DEAD_TARGETS];
static int g_deadNext;
static unsigned g_deadSkips;
static int target_dead(const float *p)
{
    ULONGLONG t = now_ms();
    for (int k = 0; k < MAX_DEAD_TARGETS; k++)
        if (g_deadTargets[k].until > t && dist2d(g_deadTargets[k].p, p) < 6 && fabsf(g_deadTargets[k].p[1] - p[1]) < 4) return 1;
    return 0;
}
static void target_mark_dead(const float *p)
{
    int k = g_deadNext++ % MAX_DEAD_TARGETS;
    memcpy(g_deadTargets[k].p, p, 12);
    g_deadTargets[k].until = now_ms() + 30000;
}
static void target_mark_alive(const float *p)
{
    for (int k = 0; k < MAX_DEAD_TARGETS; k++) if (dist2d(g_deadTargets[k].p, p) < 6 && fabsf(g_deadTargets[k].p[1] - p[1]) < 4) g_deadTargets[k].until = 0;
}

// A bot stuck in a crowd is stuck on bodies, not on the map: blocking the grid there cut a whole flag area off (Hollywood
// Heights, 2026-10-24: 15 SWAT bots by their flag in a pocket the planner couldn't leave - "A* exhausted after 1 node").
// (someone in the way: within 2 m, in the 60 degrees ahead - bots side by side at a glass railing are still stuck on
// the railing)
static int crowded_at(const float *p, float dx, float dz)
{
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 0.01f) return 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *s = bot_soldier(g_players[j]);
        if (!s) continue;
        const float *q = soldier_pos(s);
        float ox = q[0] - p[0], oz = q[2] - p[2], od = sqrtf(ox * ox + oz * oz);
        if (od < 0.05f || od > 2.0f || fabsf(p[1] - q[1]) > 2) continue;
        if ((ox * dx + oz * dz) / (od * len) > 0.5f) return 1;
    }
    return 0;
}
static int bot_block_towards(float x, float y, float z, float tx, float tz)
{
    float p[3] = { x, y, z };
    if (!g_nav || g_navBuilding || crowded_at(p, tx - x, tz - z)) return 0;
    return nav_block_towards(g_nav, x, y, z, tx, tz);
}
static int g_planTrace;
// the tester 2026-10-26: "make sure bots are always assigned routes". While its own plan is still being searched (the shared
// planner queue: 63 bots), a bot with no route used to stand "waiting for a route" or walk straight at the target.
// Now it takes a known route meanwhile - a learned one to the target, a saved one, or a learned one that gets it closer -
// and asks again in 5 s (the planner's answer then replaces it).
static unsigned g_stopgaps;
static int plan_stopgap(int i, Bot *b, const float *pos, ULONGLONG t)
{
    (void)i;
    if (b->npts && b->wp < b->npts) return 0;   // still has one
    int n = 0, k = -1;
    g_lrSeed = b->seed; g_lrPick = -1;
    if (b->targetFlag >= 0 || b->objRoute) n = human_route_to_flag(pos, b->target, b->path, BRAIN_PATH_MAX);
    if (n) b->lroute = g_lrPick;
    if (!n && (b->targetFlag >= 0 || b->objRoute)) { g_routePickSeed = b->seed; n = routes_pick(pos, b->target, b->path, BRAIN_PATH_MAX, &k); }
    if (!n) n = human_route_pick(pos, b->target, b->path, BRAIN_PATH_MAX);
    if (!n) {   // a teammate going to the same place with a route that passes within 25 m, reachable in a straight line
        int best = -1, bestK = 0; float bd = 25;
        for (int o = 0; o < H->botCount; o++) {
            Bot *ob = &g_bot[o];
            if (ob == b || !ob->alive || ob->npts < 2 || dist2d(ob->target, b->target) > 8) continue;
            for (int q = ob->wp > 0 ? ob->wp - 1 : 0; q < ob->npts; q++) {
                float d = dist2d(pos, &ob->path[q * 3]);
                if (d < bd && fabsf(ob->path[q * 3 + 1] - pos[1]) < 2.5f) { bd = d; best = o; bestK = q; }
            }
        }
        if (best >= 0 && (bd < 2 || nav_direct_route(g_nav, ray_cast, pos, &g_bot[best].path[bestK * 3]))) {
            n = g_bot[best].npts - bestK;
            memcpy(b->path, &g_bot[best].path[bestK * 3], (size_t)n * 12);
        }
    }
    if (!n) return 0;
    b->npts = n; b->wp = n > 1 ? 1 : 0; b->route = k; b->noRouteSince = 0;
    b->planAt = t - ((b->targetFlag >= 0 || b->objRoute) ? 15000 : 1000);   // (plan() looks again in 5 s)
    g_stopgaps++;
    return 1;
}

static void plan(int i, Bot *b, const float *pos)
{
    ULONGLONG t = now_ms();
    if (!g_nav || g_navBuilding) {   // no grid yet (a new map): a saved route if one passes close by, else nothing
        if ((b->targetFlag >= 0 || b->objRoute) && !(b->npts && t < b->planAt + 8000)) {
            int k = -1, n = routes_pick(pos, b->target, b->path, BRAIN_PATH_MAX, &k);
            b->npts = n; b->wp = n > 1 ? 1 : 0; b->planAt = t; b->route = k;
        }
        return;
    }
    if (b->npts && b->wp < b->npts && t < b->planAt + ((b->targetFlag >= 0 || b->objRoute) ? 20000 : 6000)) return;   // (a route still being walked: re-planned every 20 s for a flag, 6 s for anything else - was 8 / 3: the planner never caught up)
    if (!b->npts && b->planAt && t < b->planAt + 1500 && dist2d(b->target, b->planTo) < 3) return;   // none found: retry in 1.5 s
    // At the end of its route and at the target (or as near as the route got): nothing to plan. It used to plan again every
    // frame there - about one plan a second per bot on Code Blue, each a fresh learned / saved route.
    if (b->npts && b->wp >= b->npts && dist2d(pos, b->target) < 3 && fabsf(pos[1] - b->target[1]) < 2.5f && dist2d(b->target, b->planTo) < 1) return;
    if (b->npts && b->wp >= b->npts && t < b->planAt + 1500 && dist2d(b->target, b->planTo) < 3) return;   // (ended short of it: not every frame either)
    g_planWhy[!b->npts ? 0 : b->wp >= b->npts ? 1 : 2]++;
    {   // (trace: "plantrace": the next 40 plans begun by bot 5 - what it was doing, where it was going)
        if (g_planTrace > 0 && i == 5) { g_planTrace--; H->log("plantrace: %s npts %d wp %d target %.1f %.1f %.1f planTo %.1f %.1f %.1f flag %d why %s fight %d", (char *)b->player + OFF_PLAYER_NAME,
            b->npts, b->wp, b->target[0], b->target[1], b->target[2], b->planTo[0], b->planTo[1], b->planTo[2], b->targetFlag, b->why ? b->why : "", g_fight[i].state); }
    }
    memcpy(b->planTo, b->target, 12);
    b->lroute = -1; g_lrSeed = b->seed; g_lrPick = -1;
    // Going for a flag: a learned route first - a way a player walked there, or a bot trip that got there with no trouble
    // (the tester: "are the bots actually learning anything?" - they learned, but the planner's saved routes were always taken
    // first, so the learned ones were only used by lost bots). Then a saved route (joined at its nearest point), then its
    // own plan.
    // 1 plan in 20 (per bot, changing every 30 s) is the bot's own, so new ways still get tried and learned; the other 19
    // must use a known route - learned, saved, or the nearest point of one (the tester: "mandatory 95% of the time").
    // (the tester 2026-10-26: "ALL BOTS AT ALL TIMES INCLUDING AT RESPAWN SHOULD HAVE ROUTES ASSIGNED" - the 1-in-20 own plans
    // are gone: a known route whenever there is one; the bot plans its own only when none fits.)
    int own = 0;
    if ((b->targetFlag >= 0 || b->objRoute) && own) g_plansOwn++;
    if ((b->targetFlag >= 0 || b->objRoute) && !own) {
        int n = human_route_to_flag(pos, b->target, b->path, BRAIN_PATH_MAX);
        if (n) { nav_cancel_path(i); b->planAt = t; b->npts = n; b->wp = n > 1 ? 1 : 0; b->route = -1; b->lroute = g_lrPick; g_learnedFirst++; return; }
        n = human_route_leg(pos, b->target, b->path, BRAIN_PATH_MAX);   // a learned route part of the way (plans on from its end)
        if (n) { nav_cancel_path(i); b->planAt = t; b->npts = n; b->wp = n > 1 ? 1 : 0; b->route = -1; b->lroute = g_lrPick; return; }
        int k = -1; g_routePickSeed = b->seed; n = routes_pick(pos, b->target, b->path, BRAIN_PATH_MAX, &k);
        if (n) { nav_cancel_path(i); b->planAt = t; b->npts = n; b->wp = n > 1 ? 1 : 0; b->route = k; return; }
    }
    b->route = -1;
    // A teammate already walking to the same target with a route that passes within 6 m of us (a clear straight line
    // to it): share it from that point. With 63 bots a third of them were standing waiting for the planner, mostly
    // right next to someone who already had the route (spawn waves going to the same flag).
    if ((b->targetFlag >= 0 || b->objRoute)) {
        int best = -1, bestK = 0; float bd = 6;
        for (int o = 0; o < H->botCount; o++) {
            Bot *ob = &g_bot[o];
            if (ob == b || ob->npts < 3 || ob->targetFlag != b->targetFlag || dist2d(ob->target, b->target) > 4) continue;
            for (int k = ob->wp > 0 ? ob->wp - 1 : 0; k < ob->npts && k < ob->wp + 8; k++) {
                float d = dist2d(pos, &ob->path[k * 3]);
                if (d < bd && fabsf(ob->path[k * 3 + 1] - pos[1]) < 2) { bd = d; best = o; bestK = k; }
            }
        }
        if (best >= 0 && nav_segment_clear(ray_cast, pos, &g_bot[best].path[bestK * 3])) {
            Bot *ob = &g_bot[best];
            int n = ob->npts - bestK;
            memcpy(b->path, &ob->path[bestK * 3], (size_t)n * 12);
            b->npts = n; b->wp = 0; b->planAt = t; b->noRouteSince = 0;
            g_routesShared++;
            return;
        }
    }
    if (target_dead(b->target)) {   // known unreachable for now: a route a human walked, else wait for the retry
        g_deadSkips++;
        int h = human_route_pick(pos, b->target, b->path, BRAIN_PATH_MAX);
        b->planAt = t; b->npts = h; b->wp = 0;
        return;
    }
    // A learned route to the flag further off (250 m): plan to its nearest point and follow it from there.
    if ((b->targetFlag >= 0 || b->objRoute) && !own) {
        static float tail[BRAIN_PATH_MAX * 3]; float join[3];
        int nt = human_route_far(pos, b->target, join, tail, BRAIN_PATH_MAX / 2);
        if (nt) {
            nav_set_path_owner(i);
            int n = dist2d(pos, join) < 2 ? 0 : nav_find_path(g_nav, ray_cast, pos[0], pos[1], pos[2], join[0], join[1], join[2], b->path, BRAIN_PATH_MAX - nt);
            int deferred = !n && dist2d(pos, join) >= 2 && nav_last_deferred();
            nav_set_path_owner(-1);
            if (deferred) { plan_stopgap(i, b, pos, t); return; }
            if (n || dist2d(pos, join) < 2) {
                memcpy(&b->path[n * 3], tail, (size_t)nt * 12); n += nt;
                b->planAt = t; b->npts = n; b->wp = n > 1 ? 1 : 0; b->route = -1; b->noRouteSince = 0; b->lroute = g_lrPick; g_learnedFar++;
                return;
            }
        }
    }
    // A saved route to the flag further off: plan to its nearest point and follow it from there.
    if ((b->targetFlag >= 0 || b->objRoute) && !own) {
        int jk = 0, jrev = 0; float join[3];
        int jr = routes_far_join(pos, b->target, &jk, &jrev, join);
        if (jr >= 0) {
            nav_set_path_owner(i);
            int n = nav_find_path(g_nav, ray_cast, pos[0], pos[1], pos[2], join[0], join[1], join[2], b->path, BRAIN_PATH_MAX - 8);
            int deferred = !n && nav_last_deferred();
            nav_set_path_owner(-1);
            if (deferred) { plan_stopgap(i, b, pos, t); return; }
            if (n) {
                n = routes_append(jr, jk, jrev, b->path, n, BRAIN_PATH_MAX, b->target);
                b->planAt = t; b->npts = n; b->wp = n > 1 ? 1 : 0; b->route = jr; b->noRouteSince = 0;
                static unsigned logged; if (logged++ < 30) H->log("routes: %s plans %.0f m to saved route %s -> %s and follows it", (char *)b->player + OFF_PLAYER_NAME, dist2d(pos, join), g_routes[jr].from, g_routes[jr].to);
                return;
            }
        }
    }
    nav_set_path_owner(i);
    int n = nav_find_path(g_nav, ray_cast, pos[0], pos[1], pos[2], b->target[0], b->target[1], b->target[2], b->path, BRAIN_PATH_MAX);
    int deferred = !n && nav_last_deferred();
    nav_set_path_owner(-1);
    if (deferred) { plan_stopgap(i, b, pos, t); return; }   // still planning: keep the old route, else a known one meanwhile
    if (!n && (strstr(nav_last_error(), "exhausted") || strstr(nav_last_error(), "limit"))) target_mark_dead(b->target);
    if (!n) {   // lost: a route a human player walked that gets us closer, if there is one
        int h = human_route_pick(pos, b->target, b->path, BRAIN_PATH_MAX);
        if (h) { b->planAt = t; b->npts = h; b->wp = 0; return; }
    }
    if (!n) {
        static unsigned logged;
        if (logged++ < 60) H->log("route: %s found none from %.0f %.0f %.0f to %.0f %.0f %.0f (%s)", (char *)b->player + OFF_PLAYER_NAME,
                                  pos[0], pos[1], pos[2], b->target[0], b->target[1], b->target[2], nav_last_error());
    }
    // keep the first 40 m of a new route off the walls (the rest is centred when it is planned again)
    if (n > 2) {
        int upto = 1; float run = 0;
        while (upto < n - 1 && run < (H->botCount > 40 ? 25 : 40)) { run += dist2d(&b->path[(upto - 1) * 3], &b->path[upto * 3]); upto++; }
        center_path(b->path, upto + 1 < n ? upto + 1 : n, 1);
    }
    b->planAt = t;
    b->npts = n;
    b->wp = n > 1 ? 1 : 0;
    // The route starts where the bot was when it asked; it may have moved on since. Pick up at the nearest of the first
    // points (and the one after it when that is still reachable in a straight line).
    if (n > 2) {
        int best = 0; float bd = 1e9f;
        for (int k = 0; k < n && k < 12; k++) { float d = dist2d(pos, &b->path[k * 3]) + 3 * fabsf(pos[1] - b->path[k * 3 + 1]); if (d < bd) { bd = d; best = k; } }   // (height counts: not a point on the floor below)
        b->wp = best + 1 < n ? best + 1 : best;
    }
}

static void steer_towards(Bot *b, void *soldier, const float *to)
{
    const float *p = soldier_pos(soldier);
    float want = atan2f(to[0] - p[0], to[2] - p[2]);
    float err = wrap_pi(want - soldier_yaw(soldier));
    // Positive turn input lowers this heading (the live loop settled facing exactly away with the other sign). Facing
    // almost away, the error flips sign from frame to frame across +-pi: keep turning the way we already were (unless
    // that way runs into the stop at 0 - turn_input).
    // Close to the point it is heading for, a small step sideways swings the direction to it a lot: turn gently then
    // (gain falls off inside 3 m), or it twitches left / right around the point.
    float pd = dist2d(p, to), gain = 2.5f * (pd < 3 ? fmaxf(0.25f, pd / 3) : 1.0f);
    float turnWanted = turn_input(soldier_yaw(soldier), want, gain);
    {   // jitter meter: flips in what steering alone asks for
        if ((turnWanted > 0.3f && b->steerPrev < -0.3f) || (turnWanted < -0.3f && b->steerPrev > 0.3f)) {
            g_jitterSteer++; b->flips++;
            b->flipYaw = soldier_yaw(soldier); b->flipWant = want; b->flipDist = dist2d(p, to);
        }
        if (g_stTraceFrames > 0 && b == g_stTraceBot) {
            g_stTraceFrames--;
            H->log("steertrace: frame %u pos %.1f %.1f yaw %.3f aim %.1f %.1f want %.3f turn %.2f target %.0f %.0f flag %d route %d/%d", H->frame, p[0], p[2],
                   soldier_yaw(soldier), to[0], to[2], want, turnWanted, b->target[0], b->target[2], b->targetFlag, b->wp, b->npts);
        }
        b->steerPrev = turnWanted;
    }
    if (fabsf(err) > 2.6f && b->turn != 0 && (b->turn > 0) != (turnWanted > 0) && !(soldier_yaw(soldier) <= 0.001f && b->turn < 0)) b->turn = b->turn > 0 ? 1.0f : -1.0f;
    else b->turn = turnWanted;
    // Walk the wanted way whatever the facing: forward / strafe split of the error. Some soldiers' heading locks at 0
    // and ignores turn input (translation still works), so they get there sideways; the rest also turn to face it.
    b->fwd = cosf(err);
    b->strafe = g_strafeSign * sinf(err);
    float m = fmaxf(fabsf(b->fwd), fabsf(b->strafe));
    if (m > 0.01f) { b->fwd /= m; b->strafe /= m; }
}

static void bot_think(int i)
{
    Bot *b = &g_bot[i];
    void *player = H->bots[i];
    if (b->player != player) { memset(b, 0, sizeof *b); b->player = player; b->targetFlag = -1; b->lroute = -1; b->stuckSpot = -1; b->seed = (unsigned)(uintptr_t)player >> 4; }
    {   // in a vehicle: drive.inc
        int seat = -1;
        void *veh = player_vehicle(player, &seat);
        b->inVehicle = veh != NULL;
        if (veh) { b->alive = 1; vehicle_think(i, b, veh, seat, FIELD(player, OFF_PLAYER_TEAM, int)); return; }
    }
    void *soldier = bot_soldier(player);
    ULONGLONG t = now_ms();
    if (!soldier) {
        if (b->alive && g_learn) bl_note_death(g_learn, b->lastPos);
        b->alive = 0; b->npts = 0; b->fwd = b->strafe = b->turn = 0; b->buttons = 0; b->brStage = 0;
        return;
    }
    const float *pos = soldier_pos(soldier);
    b->buttons = 0; b->why = "walking";
    int team = FIELD(player, OFF_PLAYER_TEAM, int);
    if (!b->alive) { b->alive = 1; memcpy(b->progressPos, pos, 12); b->progressAt = t; b->targetFlag = -1; b->route = -1; b->lroute = -1; routes_note_spawn(team, pos); b->spawnAt = t; memcpy(b->spawnPos, pos, 12); }
    memcpy(b->lastPos, pos, 12);

    choose_target(i, b, team, pos);
    if (b->target[0] == 0 && b->target[2] == 0) { b->fwd = b->strafe = b->turn = 0; return; }

    bot_trail_tick(i, b, pos, t);   // learning mode: its own trips become routes
    // Watchdog: alive, not fighting, but within 5 m of where it was 25 s ago (and not holding a flag): it is doing
    // nothing useful. Forget its route and send it to another flag for a while.
    if (!b->idleAt || dist2d(pos, b->idlePos) > 5) { b->idleAt = t; memcpy(b->idlePos, pos, 12); }
    else if (t - b->idleAt > 25000 && g_fight[i].state == CS_IDLE && !(b->targetFlag >= 0 && dist2d(pos, b->target) < 15)) {
        static unsigned logged;
        if (logged++ < 40) H->log("watchdog: %s idle 25 s at %.0f %.0f %.0f (target %.0f %.0f, route %d/%d) - new target", (char *)player + OFF_PLAYER_NAME,
                                  pos[0], pos[1], pos[2], b->target[0], b->target[2], b->wp, b->npts);
        g_botTrail[i].bad = 1;
        b->avoidFlag = b->targetFlag >= 0 ? b->targetFlag : -1;
        b->avoidUntil = t + 30000;
        b->npts = 0; b->planAt = 0; b->targetFlag = -1; b->route = -1;
        b->idleAt = t; memcpy(b->idlePos, pos, 12);
        nav_cancel_path(i);
    }

    // Explore (the tester 2026-10-25: "groups of bots all get stuck behind walls or objects" / "if no major movement is
    // happening, have them move around objects"). Not 3 m from where it was 12 s ago, not fighting, not holding a flag or
    // a camp spot: look round 16 ways (knee and chest rays 6 m, floor 3 m out) and walk the open way that points most
    // towards the target (not one tried in the last few goes) for 2.5 s, sprinting - vaulting, E-ing doors and knifing
    // glass in the way - then plan again from there. Three goes that get nowhere (8 m): another flag for a while.
    if (!b->progAt || dist2d(pos, b->progPos) > 3 || g_fight[i].state != CS_IDLE || b->enemy) { b->progAt = t; memcpy(b->progPos, pos, 12); }
    else if (t - b->progAt > 12000 && !b->exploreUntil && !(b->targetFlag >= 0 && dist2d(pos, b->target) < 12) &&
             !(g_camp[i].flag >= 0 && g_camp[i].arrived) && dist2d(pos, b->target) > 4) {
        float best = -1e9f, want = atan2f(b->target[0] - pos[0], b->target[2] - pos[2]);
        {   // what is in the way: the next route point, and what a chest-height ray towards it hits (class, distance)
            static unsigned wlog;
            if (b->npts && b->wp < b->npts && wlog++ < 60) {
                const float *w = &b->path[b->wp * 3];
                float c0[3] = { pos[0], pos[1] + 1.2f, pos[2] }, c1[3] = { w[0], w[1] + 1.2f, w[2] }, h[3];
                int hit = ray_cast(c0, c1, h, NULL);
                uintptr_t body = hit ? *(uintptr_t *)g_lastHit.rest : 0, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
                H->log("explore: %s at %.1f %.1f %.1f, next route point %d/%d at %.1f %.1f %.1f (%.1f m): %s%.1f m, class 0x%llx", (char *)player + OFF_PLAYER_NAME,
                       pos[0], pos[1], pos[2], b->wp, b->npts, w[0], w[1], w[2], dist2d(pos, w), hit ? "blocked at " : "clear ", hit ? dist2d(pos, h) : 0.0f, (unsigned long long)vt);
            }
            if (b->npts && b->wp < b->npts) {
                const float *w = &b->path[b->wp * 3];
                float c0[3] = { pos[0], pos[1] + 1.2f, pos[2] }, c1[3] = { w[0], w[1] + 1.2f, w[2] }, h[3];
                if (ray_cast(c0, c1, h, NULL) && dist2d(pos, h) < 1.6f) {
                    uintptr_t body = *(uintptr_t *)g_lastHit.rest, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
                    int person = 0;
                    for (int j = 0; j < g_playerCount && !person; j++) { void *s2 = bot_soldier(g_players[j]); if (s2 && s2 != soldier && dist2d(soldier_pos(s2), h) < 1.0f) person = 1; }
                    if (vt != 0x2808310 && vt != 0x2819780 && !person && !low_wall_at(h)) { fence_cut_at(h, "a bot stuck 12 s at it"); breach_learn(h, 'n'); }
                }
            }
        }
        if (dist2d(b->exploreFrom, pos) > 8) { b->exploreN = 0; b->triedN = 0; memcpy(b->exploreFrom, pos, 12); }
        for (int k = 0; k < 16; k++) {
            float a = k * 0.3927f, dx = sinf(a), dz = cosf(a), clear = 6, h[3];
            for (int hgt = 0; hgt < 2; hgt++) {
                float c0[3] = { pos[0], pos[1] + (hgt ? 1.3f : 0.5f), pos[2] }, c1[3] = { pos[0] + dx * 6, c0[1], pos[2] + dz * 6 };
                if (ray_cast(c0, c1, h, NULL)) clear = fminf(clear, dist2d(c0, h));
            }
            if (clear < 1.5f) continue;
            float f0[3] = { pos[0] + dx * fminf(clear, 3), pos[1] + 1.0f, pos[2] + dz * fminf(clear, 3) }, f1[3] = { f0[0], pos[1] - 1.5f, f0[2] };
            if (!ray_cast(f0, f1, h, NULL) || h[1] > pos[1] + 0.6f) continue;   // a drop / a step too high
            float score = clear * 1.5f + 4.0f * cosf(wrap_pi(a - want));
            for (int q = 0; q < b->triedN && q < 3; q++) if (fabsf(wrap_pi(a - b->tried[q])) < 0.8f) score -= 6;
            if (score > best) { best = score; b->exploreDir = a; }
        }
        static unsigned logged;
        if (best > -1e9f) {
            b->tried[b->triedN++ % 3] = b->exploreDir;
            b->exploreUntil = t + 2500; b->exploreN++; b->exploreBlockedAt = 0;
            if (logged++ < 40) H->log("explore: %s no progress 12 s at %.0f %.0f %.0f - walks the open way %.0f deg off its target (go %d)", (char *)player + OFF_PLAYER_NAME,
                                      pos[0], pos[1], pos[2], fabsf(wrap_pi(b->exploreDir - want)) * 57.3f, b->exploreN);
        } else if (logged++ < 40) H->log("explore: %s boxed in at %.0f %.0f %.0f - no open way", (char *)player + OFF_PLAYER_NAME, pos[0], pos[1], pos[2]);
        if (b->exploreN >= 3) {   // three goes and still here: another target for a while
            b->avoidFlag = b->targetFlag >= 0 ? b->targetFlag : -1; b->avoidUntil = t + 30000; b->targetFlag = -1; b->exploreN = 0;
        }
        b->progAt = t; memcpy(b->progPos, pos, 12);
        b->npts = 0; b->planAt = 0; b->route = -1; nav_cancel_path(i);
    }
    if (b->exploreUntil) {
        if (t >= b->exploreUntil || g_fight[i].state != CS_IDLE) { b->exploreUntil = 0; b->npts = 0; b->planAt = 0; }
        else {
            b->why = "exploring a way out";
            b->turn = turn_input(soldier_yaw(soldier), b->exploreDir, 3.0f);
            b->fwd = fabsf(wrap_pi(b->exploreDir - soldier_yaw(soldier))) < 0.6f ? 1.0f : 0.2f; b->strafe = 0;
            float aimPt[3] = { pos[0] + sinf(b->exploreDir) * 3, pos[1], pos[2] + cosf(b->exploreDir) * 3 };
            {   // something right in front at chest height: E (a door), then the knife (glass)
                float c0[3] = { pos[0], pos[1] + 1.3f, pos[2] }, c1[3] = { pos[0] + sinf(b->exploreDir) * 1.0f, c0[1], pos[2] + cosf(b->exploreDir) * 1.0f };
                float hb[3];
                int blocked = ray_cast(c0, c1, hb, NULL);
                int known = blocked ? breach_find(hb, 1.2f) : -1;
                if (blocked && !(known >= 0 && g_breach[known].kind == 'n')) {
                    if (!b->exploreBlockedAt) b->exploreBlockedAt = t;
                    ULONGLONG e = t - b->exploreBlockedAt;
                    if (e < 600) b->buttons |= BTN_INTERACT; else if (e < 1400 && (e / 200) % 2) b->buttons |= BTN_MELEE;
                } else b->exploreBlockedAt = 0;
            }
            combat_tick(i, b, soldier, team);
            movement_tick(i, b, soldier, aimPt);   // vault what is in the way, sprint
            b->progressAt = t; memcpy(b->progressPos, pos, 12);
            return;
        }
    }

    // Sitting in a passenger seat (a shooting seat keeps the soldier as the controllable): stay while someone drives
    // (and fight from there), else get out - E and the change-vehicle key in turn.
    {
        const float *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
        int k = (v[0] * v[0] + v[2] * v[2] < 0.04f) ? soldier_seated(pos) : -1;
        if (k >= 0 && !b->seatedSince) b->seatedSince = t;
        if (k < 0) b->seatedSince = 0;
        if (k >= 0 && t - b->seatedSince > 1500) {
            void *kv = vehicle_known_ptr(k);
            int told = kv && kv == g_kickVeh && t < g_kickUntil;
            // a human's vehicle that has stopped on the ground for 3 s: they've arrived - get out with them
            if (kv && vehicle_has_driver(k) && !told) {
                if (vehicle_known_stopped_human(k) && air_below(pos) < 3) { if (!b->seatedStillSince) b->seatedStillSince = t; }
                else b->seatedStillSince = 0;
                if (b->seatedStillSince && t - b->seatedStillSince > 3000) told = 1;
            }
            if (told) { b->noVeh = kv; b->noVehUntil = t + 30000; }
            if (vehicle_has_driver(k) && !told) { b->why = "passenger"; combat_tick(i, b, soldier, team); b->fwd = b->strafe = 0; b->buttons |= BTN_LEAN; return; }
            b->fwd = b->strafe = b->turn = 0;
            if ((t / 300) % 5 == 0) b->buttons |= ((t / 1500) & 1) ? BTN_EXIT : BTN_INTERACT;
            static unsigned logged;
            if (t - b->seatedSince < 1600 && logged++ < 30) H->log("drive: %s is sitting in a car nobody drives - gets out", (char *)player + OFF_PLAYER_NAME);
            return;
        }
    }

    // A car to take (far flag, an empty known one close by): walk to it; at 3 m face it and press E.
    if (b->targetFlag >= 0 && g_fight[i].state == CS_IDLE && !b->enemy) {
        float vp[3]; const char *vname = "";
        int k = vehicle_to_take(i, pos, b->target, vp, &vname);
        if (k >= 0 && b->takingAt && t - b->takingAt > 4000) {   // E did nothing (full, locked, wrong side): give up on it
            b->noVeh = vehicle_known_ptr(k); b->noVehUntil = t + 60000; b->takingAt = 0; k = -1;
        }
        if (k >= 0) {
            if (dist2d(pos, vp) < 3.5f) {
                b->turn = turn_input(soldier_yaw(soldier), atan2f(vp[0] - pos[0], vp[2] - pos[2]), 3.0f);
                b->fwd = 0.2f; b->strafe = 0;
                if ((t / 400) % 2 == 0) b->buttons |= BTN_INTERACT;
                b->why = "getting in a car";
                if (!b->takingAt) { b->takingAt = t; vehicle_taken_note(); static unsigned logged; if (logged++ < 30) H->log("drive: %s takes the %s at %.0f %.0f", (char *)player + OFF_PLAYER_NAME, vname, vp[0], vp[2]); }
                return;
            }
            if (dist2d(b->target, vp) > 2) { memcpy(b->target, vp, 12); b->npts = 0; }
        } else b->takingAt = 0;
    }

    // Unsticking: back off and strafe for a moment, then replan.
    // Unsticking: back up and away to the roomier side for most of it, then sidestep; then plan again. (Backing up used to
    // pin headings - that was the turn-across-0 lock, fixed in turn_input.)
    if (t < b->unstickUntil) {
        b->why = "unstick";
        b->turn = 0;
        b->fwd = b->unstickUntil - t > 400 ? -1.0f : 0.0f;
        b->strafe = b->unstickStrafe;
        if (t < g_mx[i].vaultUntil) { b->fwd = 1; b->strafe = 0; if (t < g_mx[i].jumpUntil) { b->buttons |= BTN_JUMP; b->action = PP_ACT_JUMP; } }
        return;
    }
    // Escaping a spot it got stuck at three times: walk to a point 8 - 12 m back the way it came first.
    if (b->escapeUntil && t < b->escapeUntil && dist2d(pos, b->escape) > 2) memcpy(b->target, b->escape, 12);   // (targetFlag kept: changing it re-plans every frame)
    else if (b->escapeUntil) { b->escapeUntil = 0; b->npts = 0; b->planAt = 0; }

    plan(i, b, pos);
    const float *aim = b->target;
    // (2.2 m: at 1.6 bots circled points they had all but reached, flipping left / right.) And on the same floor: in
    // Downtown's garage the route ran along the level below - bots "reached" each point from the floor above it, then
    // walked into the walls up there (the tester, 2026-10-25). A point more than 2.2 m above / below isn't reached yet.
    while (b->npts && b->wp < b->npts && dist2d(pos, &b->path[b->wp * 3]) < 2.2f && fabsf(pos[1] - b->path[b->wp * 3 + 1]) < 2.2f) b->wp++;
    // Look ahead: when the point after the next one is in plain view (body-wide, knee and chest), head for it instead -
    // round corners smoothly rather than walking out to each route point and turning there. Every 300 ms.
    if (b->npts && b->wp + 1 < b->npts && t >= b->lookAheadAt && dist2d(pos, &b->path[b->wp * 3]) < 6) {
        b->lookAheadAt = t + 300;
        const float *nx = &b->path[(b->wp + 1) * 3];
        if (fabsf(nx[1] - pos[1]) < 1.5f && dist2d(pos, nx) < 14 && nav_segment_clear(ray_cast, pos, nx) && floor_continuous(pos, nx)) b->wp++;
    }
    if (b->npts && b->wp < b->npts) aim = &b->path[b->wp * 3];
    // No route (still being planned, or none found): walking straight at the target is what ran bots into walls after
    // a retarget. Only do it when the straight line is walkable; otherwise turn towards it and wait for the planner,
    // and after 5 s without any route fall back to the straight line (the stuck handling takes over from there).
    int hold = 0;
    if (b->npts && b->wp < b->npts) b->noRouteSince = 0;
    else if (g_nav && !g_navBuilding && dist2d(pos, b->target) > 2.5f) {
        if (!b->noRouteSince) b->noRouteSince = t;
        if (t >= b->directAt) { b->directAt = t + 400; b->directOk = nav_direct_route(g_nav, ray_cast, pos, b->target); }
        hold = !b->directOk && t - b->noRouteSince < 5000;
    }
    // Waiting for its route (63 bots share one planner: after a flag changes hands 30 requests queue up and bots stood
    // "waiting for a route" for seconds - the tester 2026-10-26: "they just stay still and do nothing"): walk meanwhile the open
    // way that points most towards the target - 12 rays at knee and chest height (6 m), floor 3 m out, no drop - looked
    // at every 600 ms. The planned route takes over when it arrives.
    float waitAim[3];
    if (hold) {
        if (t >= b->waitDirAt) {
            b->waitDirAt = t + 600;
            float best = -1e9f, want = atan2f(b->target[0] - pos[0], b->target[2] - pos[2]);
            b->waitDirOk = 0;
            for (int k = 0; k < 12; k++) {
                float a = want + (k / 2 + (k & 1 ? 1 : 0)) * 0.5236f * (k & 1 ? 1 : -1), dx = sinf(a), dz = cosf(a), clear = 6, h[3];
                if (fabsf(wrap_pi(a - want)) > 1.6f) continue;   // (not away from the target)
                for (int hgt = 0; hgt < 2; hgt++) {
                    float c0[3] = { pos[0], pos[1] + (hgt ? 1.3f : 0.5f), pos[2] }, c1[3] = { pos[0] + dx * 6, c0[1], pos[2] + dz * 6 };
                    if (ray_cast(c0, c1, h, NULL)) clear = fminf(clear, dist2d(c0, h));
                }
                if (clear < 2.5f) continue;
                float f0[3] = { pos[0] + dx * 3, pos[1] + 1.0f, pos[2] + dz * 3 }, f1[3] = { f0[0], pos[1] - 1.5f, f0[2] };
                if (!ray_cast(f0, f1, h, NULL) || h[1] > pos[1] + 0.6f) continue;
                float score = clear + 5.0f * cosf(wrap_pi(a - want));
                if (score > best) { best = score; b->waitDir = a; b->waitDirOk = 1; }
            }
        }
        if (b->waitDirOk) {
            waitAim[0] = pos[0] + sinf(b->waitDir) * 4; waitAim[1] = pos[1]; waitAim[2] = pos[2] + cosf(b->waitDir) * 4;
            aim = waitAim; hold = 0; b->why = "heading out while its route is planned";
        }
    }
    // A door or pane in the way: open / break it before walking on (unless an enemy needs shooting first).
    if (b->brStage || breach_start(b, pos, aim, t)) {
        combat_tick(i, b, soldier, team);   // [combat.inc hook]
        if (!b->enemy) { b->why = "door / pane"; breach_tick(b, soldier, t); b->progressAt = t; memcpy(b->progressPos, pos, 12); return; }
        b->brStage = 0;
    }
    if (lift_ride(b, pos, t)) { b->why = "elevator"; b->progressAt = t; memcpy(b->progressPos, pos, 12); combat_tick(i, b, soldier, team); b->fwd = b->strafe = 0; return; }
    steer_towards(b, soldier, aim);
    if (pool_exit(b, soldier, pos, t)) { b->why = "climbing out of water"; b->progressAt = t; memcpy(b->progressPos, pos, 12); return; }
    if (parachute_tick(i, b, soldier, pos, t)) { b->why = "parachute"; b->progressAt = t; memcpy(b->progressPos, pos, 12); return; }   // off a roof / falling
    // Just spawned: turn to face the way first (up to 1.2 s), then walk - no walking off backwards or sideways.
    if (b->spawnAt && t - b->spawnAt < 1200 && fabsf(wrap_pi(atan2f(aim[0] - pos[0], aim[2] - pos[2]) - soldier_yaw(soldier))) > 0.5f) { b->fwd = 0; b->strafe = 0; }
    // Walking: keep off a wall at the side (1 m rays at chest height, 45 degrees off the way it goes, every 100 ms) -
    // a sidestep away from the nearer one.
    if (!hold && b->fwd > 0.5f && g_fight[i].state == CS_IDLE) {
        if (t >= b->sideAt) {
            b->sideAt = t + 100;
            float yaw = soldier_yaw(soldier), c[3] = { pos[0], pos[1] + 1.2f, pos[2] }, h[3], nudge = 0;
            for (int s = -1; s <= 1; s += 2) {
                float a = yaw + s * 0.785398f, e[3] = { c[0] + sinf(a) * 1.0f, c[1], c[2] + cosf(a) * 1.0f };
                if (ray_cast(c, e, h, NULL)) nudge -= s * (1.0f - dist2d(c, h));
            }
            b->sideNudge = nudge;   // > 0: step towards the yaw - 45 deg side (a wall on the + side), < 0 the other way
        }
        if (b->sideNudge != 0 && t >= b->sideFloorAt) {   // floor 1 m out on the side it would step to? (every 300 ms)
            b->sideFloorAt = t + 300;
            float yaw = soldier_yaw(soldier), a = yaw + (b->sideNudge > 0 ? 1.5708f : -1.5708f), to[3] = { pos[0] + sinf(a), pos[1], pos[2] + cosf(a) };
            b->sideFloorOk = floor_continuous(pos, to);
        }
        if (b->sideNudge != 0 && b->sideFloorOk) b->strafe = fmaxf(-1, fminf(1, b->strafe + g_strafeSign * b->sideNudge * 1.5f));
    }
    // (Spacing steer, 2026-10-26: teammates within 4 m pushed it sideways and one ahead made it walk - taken out the same
    // day: the sideways push stopped the sprint (sprint needs strafe under 0.35) and the tester saw slow, hesitant bots. Bunching
    // is now handled where routes are picked: different learned routes per bot, busy ones cost more.)
    if (hold) { b->why = "waiting for a route"; b->fwd = 0; b->strafe = 0; b->turn = 0; b->progressAt = t; memcpy(b->progressPos, pos, 12); }   // (no staring at the target through a wall)
    if (b->targetFlag >= 0 && dist2d(pos, b->target) < 1.5f) b->fwd = 0;   // holding the spot
    camp_hold(i, b, soldier, pos);
    combat_tick(i, b, soldier, team);   // [combat.inc hook]
    movement_tick(i, b, soldier, aim);   // vault / jump low obstacles, sprint on long runs

    // Scraping along a wall: walking (forward held, not fighting, not vaulting) but under 1.4 m/s for a second - the bot
    // is pushing diagonally into a wall or a corner. Step off it (towards the side with more room) for 0.8 s; the
    // second time in a row the route is planned again from where it stands.
    // Someone right in front (The Block's playground: a knot of bots pushing into each other, each "stuck" and blocking
    // grid links for the rest): step round them instead - not a wall, not a stuck spot.
    int crowded = 0;
    // (mp_hills 2026-10-26: 12 cops in a 6 m knot at their spawn, every one strafing round the next - nobody got
    // anywhere for a minute. Stepping round someone for 1.5 s without getting 1 m further: 1.2 s of walking straight on,
    // sprinting - bodies shove past each other - and someone dead ahead with no side to it is passed on the right.)
    if (t < b->pushUntil) { b->strafe = 0; b->fwd = 1; b->buttons |= BTN_SPRINT; b->crowdSince = 0; }
    else if (b->fwd > 0.3f && t >= g_mx[i].vaultUntil) {
        float yaw = soldier_yaw(soldier), fx = sinf(yaw), fz = cosf(yaw);
        for (int j = 0; j < g_playerCount; j++) {
            void *s2 = bot_soldier(g_players[j]);
            if (!s2 || s2 == soldier) continue;
            const float *q = soldier_pos(s2);
            float rx = q[0] - pos[0], rz = q[2] - pos[2], along = rx * fx + rz * fz, side = rx * fz - rz * fx;
            if (along > 0 && along < 1.5f && fabsf(side) < 0.9f && fabsf(q[1] - pos[1]) < 1.5f) {
                // side > 0: they are towards yaw + 90 deg, which strafe +g_strafeSign moves to (see the wall slide) - go the other way
                b->strafe = (side > 0.3f ? -1.0f : side < -0.3f ? 1.0f : 1.0f) * g_strafeSign * 0.9f; b->fwd = 0.5f;   // (dead ahead: to the right, the yaw + 90 side, like they do)
                if (!b->crowdSince) { b->crowdSince = t; memcpy(b->crowdPos, pos, 12); }
                else if (t - b->crowdSince > 1500) {
                    if (dist2d(pos, b->crowdPos) < 1.0f) { b->pushUntil = t + 1200; static unsigned logged; if (logged++ < 20) H->log("move: %s stuck in a crowd at %.0f %.0f %.0f - pushes through", (char *)player + OFF_PLAYER_NAME, pos[0], pos[1], pos[2]); }
                    b->crowdSince = 0;
                }
                crowded = 1; b->slowSince = 0; b->progressAt = t; memcpy(b->progressPos, pos, 12);
                break;
            }
        }
    }
    if (crowded) {}
    else if (t < b->slideUntil) { b->strafe = b->slideStrafe; b->fwd = 0.3f; b->turn *= 0.3f; }   // (sidestep along the wall, barely pushing into it)
    else if (!hold && b->fwd > 0.8f && g_fight[i].state == CS_IDLE && t >= g_mx[i].vaultUntil) {
        const float *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
        float speed = sqrtf(v[0] * v[0] + v[2] * v[2]);
        if (speed > 1.4f) b->slowSince = 0;
        else if (!b->slowSince) b->slowSince = t;
        else if (t - b->slowSince > 1000) {
            float yaw = soldier_yaw(soldier), c[3] = { pos[0], pos[1] + 1.0f, pos[2] }, room[2];
            for (int s = 0; s < 2; s++) {
                float a = yaw + (s ? 1.0f : -1.0f) * 1.2f, e[3] = { c[0] + sinf(a) * 3, c[1], c[2] + cosf(a) * 3 }, h[3];
                room[s] = ray_cast(c, e, h, NULL) ? dist2d(c, h) : 3;
            }
            // the roomier side: s = 1 is yaw + 1.2 rad; moving that way needs strafe = g_strafeSign * sin(+1.2) > 0 side
            b->slideStrafe = (room[1] >= room[0] ? 1.0f : -1.0f) * g_strafeSign;
            b->slideUntil = t + 1400;
            b->slowSince = 0;
            b->npts = 0; b->planAt = 0;   // and a fresh route from where it ends up (the last one led into this wall)
            { float yaw2 = soldier_yaw(soldier); bot_block_towards(pos[0], pos[1], pos[2], pos[0] + sinf(yaw2) * 4, pos[2] + cosf(yaw2) * 4); }
            b->slides++;
            static unsigned logged;
            if (logged++ < 30) H->log("move: %s was scraping a wall at %.0f %.0f %.0f (%.1f m/s) - steps off", (char *)player + OFF_PLAYER_NAME, pos[0], pos[1], pos[2], speed);
        }
    }

    // Progress check: wanting to move but under 0.8 m in 2 s = stuck.
    if (t - b->progressAt >= 2000) {
        float moved = dist2d(pos, b->progressPos);
        if (b->stuckSpot >= 0 && g_learn && moved > 3) { bl_note_result(g_learn, b->stuckSpot, 1, 0, (long)time(NULL)); b->stuckSpot = -1; }
        // Frozen: turning hard but the heading did not change (seen on ladders / mid-vault: yaw locked at 0).
        int frozen = fabsf(b->turn) > 0.5f && fabsf(wrap_pi(soldier_yaw(soldier) - b->progressYaw)) < 0.05f && !g_fight[i].locked;   // [combat.inc hook] a locked heading is not a stuck spot
        // Not while fighting / in cover (side-steps and holding still are meant), not while waiting for a route.
        int busy = g_fight[i].state != CS_IDLE || b->enemy || hold;
        if (!busy && moved < 0.8f && (fabsf(b->fwd) + fabsf(b->strafe) > 0.5f || frozen) && dist2d(pos, b->target) > 3) {
            float yaw = soldier_yaw(soldier), h[2] = { sinf(yaw), cosf(yaw) };
            if (g_learn) {
                if (b->stuckSpot >= 0) bl_note_result(g_learn, b->stuckSpot, 0, 0, (long)time(NULL));
                b->stuckSpot = bl_note_stuck(g_learn, BL_FOOT, pos, h, (long)time(NULL));
            }
            bot_block_towards(pos[0], pos[1], pos[2], aim[0], aim[2]);
            // (nav_repair_around was tried here 2026-10-25: it dropped staircase links where crowds stood - stairs to
            // Code Blue's upper floors went, leaving 1100-node islands. Not used.)
            if (b->route >= 0) { route_note_stuck(b->route); b->route = -1; }
            // stuck here again and again (a learned spot): follow a route a human walked out of it, if one passes by
            if (g_learn && b->stuckSpot >= 0 && g_learn->spots[b->stuckSpot].seen >= 3) {
                int h = human_route_pick(pos, b->target, b->path, BRAIN_PATH_MAX);
                if (h) { b->planAt = t; b->npts = h; b->wp = 0; memcpy(b->planTo, b->target, 12); }
            }
            b->unstickUntil = t + 1300 + (b->seed % 500);
            movement_unstick(i, b, t);
            {   // the roomier side (3 m rays square to the heading): back up and step that way
                float c[3] = { pos[0], pos[1] + 1.0f, pos[2] }, room[2];
                for (int s = 0; s < 2; s++) {
                    float a = yaw + (s ? 1.5708f : -1.5708f), e[3] = { c[0] + sinf(a) * 3, c[1], c[2] + cosf(a) * 3 }, hh[3];
                    room[s] = ray_cast(c, e, hh, NULL) ? dist2d(c, hh) : 3;
                }
                b->unstickStrafe = (room[1] >= room[0] ? 1.0f : -1.0f) * g_strafeSign;
            }
            b->npts = 0; b->planAt = 0;
            // the third time within 4 m and 25 s: an escape point 8 - 12 m back the way it came, on the grid
            if (b->stuckRepeat && dist2d(pos, b->stuckAt) < 4 && t - b->stuckFirst < 25000) b->stuckRepeat++;
            else { b->stuckRepeat = 1; b->stuckFirst = t; memcpy(b->stuckAt, pos, 12); }
            if (b->stuckRepeat == 2) {
                // Second time here: jump / vault at it whatever the height test said (fences and gates it misjudged:
                // "they can just turn around or vault or jump"), and look for an opening - the clear way (knee and
                // chest rays 4 m, ground at the end) nearest the way it wants to go.
                movement_force_vault(i, b, t);
                float want = atan2f(aim[0] - pos[0], aim[2] - pos[2]), bestA = 9, gap[3];
                for (int k = 0; k < 16; k++) {
                    float a = k * 0.3927f, dx = sinf(a), dz = cosf(a), ok = 1;
                    for (int hgt = 0; hgt < 2 && ok; hgt++) {
                        float c1[3] = { pos[0], pos[1] + (hgt ? 1.3f : 0.5f), pos[2] }, c2[3] = { pos[0] + dx * 4, c1[1], pos[2] + dz * 4 };
                        if (ray_cast(c1, c2, NULL, NULL)) ok = 0;
                    }
                    if (!ok) continue;
                    float f1[3] = { pos[0] + dx * 4, pos[1] + 1.0f, pos[2] + dz * 4 }, f2[3] = { f1[0], pos[1] - 1.5f, f1[2] }, fh[3];
                    if (!ray_cast(f1, f2, fh, NULL)) continue;   // a drop
                    float da = fabsf(wrap_pi(a - want));
                    if (da < 0.5f) continue;   // (straight on is what it just failed)
                    if (da < bestA) { bestA = da; memcpy(gap, fh, 12); }
                }
                if (bestA < 9) {
                    memcpy(b->escape, gap, 12); b->escapeUntil = t + 4000;
                    static unsigned logged;
                    if (logged++ < 40) H->log("move: %s stuck twice at %.0f %.0f %.0f - jumps at it, then tries the opening towards %.0f %.0f", (char *)player + OFF_PLAYER_NAME, pos[0], pos[1], pos[2], gap[0], gap[2]);
                }
            }
            if (b->stuckRepeat >= 3 && g_nav && !g_navBuilding) {
                float dx = aim[0] - pos[0], dz = aim[2] - pos[2], dl = sqrtf(dx * dx + dz * dz) + 0.01f, out[3];
                for (int tries = 0; tries < 4; tries++) {
                    float back = 8 + tries * 1.5f, side = ((b->seed + tries) & 1 ? 1 : -1) * (float)(tries * 3);
                    float px = pos[0] - dx / dl * back - dz / dl * side, pz = pos[2] - dz / dl * back + dx / dl * side;
                    if (nav_snap(g_nav, px, pos[1], pz, out) && dist2d(out, pos) > 5) {
                        memcpy(b->escape, out, 12); b->escapeUntil = t + 10000; b->stuckRepeat = 0;
                        static unsigned logged;
                        if (logged++ < 40) H->log("move: %s keeps getting stuck at %.0f %.0f %.0f - backs off to %.0f %.0f first", (char *)player + OFF_PLAYER_NAME, pos[0], pos[1], pos[2], out[0], out[2]);
                        break;
                    }
                }
            }
        }
        memcpy(b->progressPos, pos, 12);
        b->progressYaw = soldier_yaw(soldier);
        b->progressAt = t;
    }
}

// Which way +strafe really moves, checked on every bot that is strafing (not walking forward) at speed: the velocity's
// heading relative to the soldier's tells. A wrong sign sends a bot that is facing an enemy (and walking its route
// sideways) the opposite way, into walls. Decided once 300 samples agree 4 to 1; the "strafesign" command overrides.
static unsigned g_strafeVotes[2], g_strafeDecided;
static float g_strafePrev[MAX_BOTS];
static void strafe_check(int i, Bot *b, void *soldier)
{
    float st = g_strafePrev[i];
    g_strafePrev[i] = b->strafe;
    if (g_strafeDecided || g_fight[i].locked || fabsf(st) < 0.8f || fabsf(b->fwd) > 0.3f || st * b->strafe <= 0) return;
    const float *r = (const float *)((char *)soldier + OFF_SOLDIER_ROT_FWD), *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
    if (r[0] == 0 && r[2] == 0) return;
    if (v[0] * v[0] + v[2] * v[2] < 1.5f * 1.5f) return;
    float d = wrap_pi(atan2f(v[0], v[2]) - soldier_yaw(soldier));
    if (fabsf(fabsf(d) - 1.5708f) > 0.6f) return;   // not moving sideways: something else is going on
    float sign = (d > 0) == (st > 0) ? 1.0f : -1.0f;   // the sign that maps +err to +strafe for this sample
    // steer_towards uses strafe = sign * sin(err) with err = want - yaw: moving at yaw + d needs sin(d) * sign = st.
    g_strafeVotes[sign > 0]++;
    unsigned n = g_strafeVotes[0] + g_strafeVotes[1];
    if (n >= 300) {
        int plus = g_strafeVotes[1] > 4 * g_strafeVotes[0], minus = g_strafeVotes[0] > 4 * g_strafeVotes[1];
        if (plus || minus) {
            float was = g_strafeSign;
            g_strafeSign = plus ? 1.0f : -1.0f;
            g_strafeDecided = 1;
            H->log("strafe: +strafe input moves %s (%u to %u samples) - strafe sign %+.0f%s", plus ? "right of the heading by our yaw convention" : "left of it",
                   g_strafeVotes[1], g_strafeVotes[0], g_strafeSign, was != g_strafeSign ? " (CHANGED: bots were strafing the wrong way)" : "");
        } else if (n >= 3000) { g_strafeDecided = 1; H->log("strafe: samples disagree (%u to %u) - keeping %+.0f", g_strafeVotes[1], g_strafeVotes[0], g_strafeSign); }
    }
}

#include "vehicles.inc"
#include "drive.inc"
#include "heli.inc"
#include "gunner.inc"
#include "lifts.inc"
#include "parachute.inc"
#include "pool.inc"
#include "editor.inc"

// Spawned into a vehicle (Conquest Large: one team starts in cars on Chinatown and the like - they sat there with
// forward held and never moved): for the first 20 s after spawning, a bot that hasn't got 1 m from where it appeared
// presses "get out" (EIAChangeVehicle) for 300 ms every 2 s. Only then: near a parked car the same key would get in.
static unsigned g_vehicleExits;
static void spawn_vehicle_exit(int i, Bot *b)
{
    void *s = bot_soldier(H->bots[i]);
    ULONGLONG t = now_ms();
    if (!g_vehicleStay || !s || !b->spawnAt || t - b->spawnAt < 1500 || t - b->spawnAt > 20000) return;   // (now only with "vehiclestay 0": drive.inc drives)
    if (dist2d(soldier_pos(s), b->spawnPos) > 1.0f) { b->spawnAt = 0; return; }   // on its way: done
    if ((t - b->spawnAt) % 2000 < 300) {
        b->buttons |= BTN_EXIT;
        if ((t - b->spawnAt) % 2000 < 40 && g_vehicleExits++ < 30) H->log("vehicle: %s hasn't moved since spawning - getting out", (char *)b->player + OFF_PLAYER_NAME);
    }
}

// "routetest <bot>": plan (synchronously) from that bot to its target and log the result - why a spot has no route
static volatile LONG g_routeTestBot = -1;
static float g_rtFrom[3], g_rtTo[3]; static volatile LONG g_rtPoints;   // "routepoints x y z x y z"
static void route_points_tick(void)
{
    if (!InterlockedExchange(&g_rtPoints, 0) || !g_nav || g_navBuilding) return;
    static float pts[BRAIN_PATH_MAX * 3];
    nav_set_path_owner(-1);
    int n = nav_find_path(g_nav, ray_cast, g_rtFrom[0], g_rtFrom[1], g_rtFrom[2], g_rtTo[0], g_rtTo[1], g_rtTo[2], pts, BRAIN_PATH_MAX);
    H->log("routepoints: %.1f %.1f %.1f -> %.1f %.1f %.1f: %d point(s), %s", g_rtFrom[0], g_rtFrom[1], g_rtFrom[2], g_rtTo[0], g_rtTo[1], g_rtTo[2], n, nav_last_error());
    for (int k = 0; k < n && k < 60; k++) H->log("routepoints:   %d: %.1f %.1f %.1f", k, pts[k * 3], pts[k * 3 + 1], pts[k * 3 + 2]);
}
static float g_rpArea[5]; static volatile LONG g_rpDo;   // "reprobe x0 z0 x1 z1 fromY"
static void reprobe_tick(void)
{
    if (!InterlockedExchange(&g_rpDo, 0) || !g_nav || g_navBuilding) return;
    int n = nav_reprobe_area(g_nav, ray_cast, g_rpArea[0], g_rpArea[1], g_rpArea[2], g_rpArea[3], g_rpArea[4]);
    H->log("reprobe: %.0f %.0f .. %.0f %.0f from %.1f: %d node(s) added", g_rpArea[0], g_rpArea[1], g_rpArea[2], g_rpArea[3], g_rpArea[4], n);
}
static float g_cdAt[2]; static volatile LONG g_cdDo;   // "cell x z"
static void cell_tick(void)
{
    if (!InterlockedExchange(&g_cdDo, 0) || !g_nav) return;
    char out[400]; nav_cell_dump(g_nav, g_cdAt[0], g_cdAt[1], out, sizeof out);
    H->log("cell %.1f %.1f:%s", g_cdAt[0], g_cdAt[1], out);
}
static float g_brAt[6]; static int g_brRef; static volatile LONG g_brDo;   // "bridge x y z"
static void bridge_tick(void)
{
    if (!InterlockedExchange(&g_brDo, 0) || !g_nav || g_navBuilding) return;
    float top[3], bot[3];
    int n = nav_island_bridge(g_nav, g_brAt[0], g_brAt[1], g_brAt[2], g_brRef ? g_brAt + 3 : NULL, top, bot);
    if (n) H->log("bridge: island of %d node(s) at %.1f %.1f %.1f: top %.1f %.1f %.1f, bottom %.1f %.1f %.1f", n, g_brAt[0], g_brAt[1], g_brAt[2], top[0], top[1], top[2], bot[0], bot[1], bot[2]);
    else H->log("bridge: nothing at %.1f %.1f %.1f", g_brAt[0], g_brAt[1], g_brAt[2]);
}
static void route_test_tick(void)
{
    LONG i = InterlockedExchange(&g_routeTestBot, -1);
    if (i < 0 || i >= H->botCount || !g_nav || g_navBuilding) return;
    void *s = bot_soldier(H->bots[i]);
    if (!s) { H->log("routetest: bot %ld is dead", i); return; }
    const float *p = soldier_pos(s);
    Bot *b = &g_bot[i];
    static float pts[BRAIN_PATH_MAX * 3];
    nav_set_path_owner(-1);
    int n = nav_find_path(g_nav, ray_cast, p[0], p[1], p[2], b->target[0], b->target[1], b->target[2], pts, BRAIN_PATH_MAX);
    float snapA[3], snapB[3];
    int sa = nav_snap(g_nav, p[0], p[1], p[2], snapA), sb = nav_snap(g_nav, b->target[0], b->target[1], b->target[2], snapB);
    H->log("routetest: %s at %.1f %.1f %.1f -> %.1f %.1f %.1f: %d point(s), %s; start node %s (%.1f %.1f %.1f), goal node %s (%.1f %.1f %.1f), clearance here %d",
           (char *)H->bots[i] + OFF_PLAYER_NAME, p[0], p[1], p[2], b->target[0], b->target[1], b->target[2], n, nav_last_error(),
           sa ? "ok" : "NONE", sa ? snapA[0] : 0, sa ? snapA[1] : 0, sa ? snapA[2] : 0, sb ? "ok" : "NONE", sb ? snapB[0] : 0, sb ? snapB[1] : 0, sb ? snapB[2] : 0,
           nav_node_clearance(g_nav, p[0], p[1], p[2]));
}
static void perf_part(int k, LARGE_INTEGER *since);   // core.c
static void brain_tick(void)
{
    LARGE_INTEGER pt; QueryPerformanceCounter(&pt);
    players_refresh();
    perf_part(0, &pt);
    {   // Soldiers prefer open ground: the wide-open node classes of the road map (drive.inc) cost x0.6 / x0.85 in the
        // planner (the tester's "easy outdoor routes"). nav.c had this since the vehicles work, but it was never switched on.
        static unsigned roadSerial;
        if (g_nav && !g_navBuilding && roadSerial != g_nav->serial) {
            const unsigned char *m = drive_mask();
            nav_set_road_mask(m ? g_nav : NULL, m);
            roadSerial = g_nav->serial;
            if (m) H->log("routes: soldiers prefer open ground on %s (road map as planner cost)", g_levelKey);
        }
        if (!g_nav || g_navBuilding) { nav_set_road_mask(NULL, NULL); roadSerial = 0; }
    }
    nav_tick();
    perf_part(1, &pt);
    learn_open();
    breach_load();
    survey_tick();
    camp_tick();
    if (InterlockedExchange(&g_ctfScanDo, 0)) ctf_scan(1, NULL, NULL);
    combat_frame();   // [combat.inc hook]
    vehicles_learn_soldier_vt();
    vehicles_track();
    heli_watch_humans();
    chute_watch_humans();
    perf_part(2, &pt);
    for (int i = 0; i < H->botCount; i++) {
        // (Every bot thinks every frame: thinking every other frame with 40+ bots made turning overshoot - the turn input
        // is a rate, held for two frames it turned twice as far - and movement jittered. the tester 2026-10-23.)
        bot_think(i);
        // (spawn vehicles: drive.inc drives them now - spawn_vehicle_exit() no longer runs)
        jumptest_bot(i, &g_bot[i]);
        drivetest_bot(i, &g_bot[i]);
        void *s = bot_soldier(H->bots[i]);
        if (s) strafe_check(i, &g_bot[i], s);
        {   // jitter meter (perf): hard turn or strafe input that flips direction from one frame to the next
            static float pt[MAX_BOTS], ps[MAX_BOTS];
            Bot *bb = &g_bot[i];
            if ((bb->turn > 0.3f && pt[i] < -0.3f) || (bb->turn < -0.3f && pt[i] > 0.3f)) { g_jitterTurn++; if (g_fight[i].state != CS_IDLE || bb->enemy) g_jitterTurnFight++; }
            if ((bb->strafe > 0.3f && ps[i] < -0.3f) || (bb->strafe < -0.3f && ps[i] > 0.3f)) g_jitterStrafe++;
            pt[i] = bb->turn; ps[i] = bb->strafe;
        }
    }
    perf_part(3, &pt);
    routes_tick();
    perf_part(4, &pt);
    water_learn();
    human_routes_tick();
    refine_tick();
    pit_tick(now_ms());
    editor_tick();
    route_test_tick();
    route_points_tick();
    reprobe_tick();
    bridge_tick();
    cell_tick();
    lifts_load(); lifts_link(); oneway_fix(); lifts_watch_humans();
    jumptest_frame();
    drivetest_frame();
    if (now_ms() - g_learnSavedAt > 60000 || InterlockedExchange(&g_reportNow, 0)) { learn_save(); routes_save(); report_write(); g_learnSavedAt = now_ms(); }
    perf_part(5, &pt);
}

// The input for a pseudo-player this frame (play mode), from its Bot.
static int brain_input(void *pp, float *fwd, float *strafe, float *turn, int *action)
{
    void *player = FIELD(pp, OFF_PP_PLAYER, void *);
    for (int i = 0; i < MAX_BOTS; i++)
        if (g_bot[i].player == player) { *fwd = g_bot[i].fwd; *strafe = g_bot[i].strafe; *turn = g_bot[i].turn; *action = g_bot[i].action; if (g_jumpUsed[i] == 2 && *action == 2 && now_ms() < g_mx[i].vaultUntil) *action = 0; return g_bot[i].inVehicle ? 2 : 1; }
    return 0;
}

// "bots": one line per bot - team, position, target, route progress.
static void brain_dump(void)
{
    int engaging = 0;
    for (int i = 0; i < H->botCount; i++) engaging += g_bot[i].enemy != NULL;
    H->log("brain: level %s, nav %s, %d flag(s), learned %d spot(s), %d player(s), %d bot(s) engaging, test action %d, %u stopgap route(s) while plans were searched",
           g_levelKey[0] ? g_levelKey : "(unknown)", !g_nav ? "none" : g_navBuilding ? "building" : "ready", conquest_flags(),
           g_learn ? g_learn->nSpots : 0, g_playerCount, engaging, g_testAction, g_stopgaps);
    H->log("brain: doors / panes: %d learned, %u tried, %u opened with E, %u broken", g_breachN, g_breachTried, g_breachOpened, g_breachBroken);
    H->log("brain: %u route(s) shared between teammates since the last dump", g_routesShared); g_routesShared = 0;
    H->log("brain: since last: %u enemy sighting(s) within 60 m, %u behind cover", g_seenInRange, g_seenBlocked);
    g_seenInRange = g_seenBlocked = 0;
    combat_dump();   // [combat.inc hook]
    doors_dump();
    movement_dump();
    objective_flags_dump();
    for (int i = 0; i < H->botCount; i++) {
        Bot *b = &g_bot[i];
        void *s = bot_soldier(H->bots[i]);
        if (!s) { H->log("bot %d %s: dead", i, (char *)H->bots[i] + OFF_PLAYER_NAME); continue; }
        const float *p = soldier_pos(s);
        const float *aim = b->npts && b->wp < b->npts ? &b->path[b->wp * 3] : b->target;
        float yaw = soldier_yaw(s), want = atan2f(aim[0] - p[0], aim[2] - p[2]);
        const float *v = (const float *)((char *)s + OFF_SOLDIER_VELOCITY);
        H->log("bot %d %s team %d at %.0f %.0f %.0f -> %s %.0f %.0f (%.0f m), route %d/%d, input %.1f %.1f %.1f, yaw %.2f want %.2f err %.2f, moving %.2f, %s", i,
               (char *)H->bots[i] + OFF_PLAYER_NAME, FIELD(H->bots[i], OFF_PLAYER_TEAM, int), p[0], p[1], p[2],
               b->targetFlag >= 0 ? "flag" : "enemy", b->target[0], b->target[2], dist2d(p, b->target), b->wp, b->npts, b->fwd, b->strafe, b->turn,
               yaw, want, wrap_pi(want - yaw), atan2f(v[0], v[2]), b->why ? b->why : "-");
        { double age; int st = nav_path_state(i, &age); if (!b->npts) H->log("bot %d: no route - request %s%s%.1f s, %d queued in all", i,
              st == 1 ? "queued" : st == 2 ? "searching" : st == 3 ? "done, not collected" : "none", st > 0 ? " for " : "", st > 0 ? age / 1000 : 0.0, nav_pending_paths()); }
    }
}

// The extra buttons / pitch for a pseudo-player this frame (play mode).
static int brain_buttons(void *pp, unsigned *buttons, float *pitch, float *roll, float *yaw, float *thr)
{
    void *player = FIELD(pp, OFF_PP_PLAYER, void *);
    for (int i = 0; i < MAX_BOTS; i++)
        if (g_bot[i].player == player) { *buttons = g_bot[i].buttons; if (g_jumpUsed[i] == 1 && now_ms() < g_mx[i].vaultUntil) *buttons &= ~BTN_JUMP; *pitch = g_bot[i].pitch; *roll = g_bot[i].roll; *yaw = g_bot[i].yawIn; *thr = g_bot[i].throttleIn; return 1; }
    return 0;
}
