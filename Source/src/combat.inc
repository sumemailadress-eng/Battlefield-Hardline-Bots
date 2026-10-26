// Thugbot combat (included by brain.inc after the movement helpers) - AltBotMod V34's infantry combat, adapted to the
// Hardline pseudo-players.
//
//   Seeing:  enemies (bots and humans) within the profile's detect range, inside its field of view (or very close, or
//            whoever just hurt us), with a clear eye-to-chest (or eye-to-head) physics ray.
//   Aiming:  yaw by the turn input (as steering does), pitch by the pitch input once the view vector is known (see
//            "pitch discovery"), leading the target by its velocity. Fire only after the profile's reaction time and
//            when the aim error at the target is inside the profile's fire window; bursts and pauses by profile.
//   Moving:  far fights keep walking the objective route while shooting; close fights stop and side-step; aggressive
//            bots push in and knife at arm's length.
//   Cover:   hurt below CoverBelowHealth (Aggression scales it), losing an exchange, or (cautious) outnumbered: find a
//            spot within 22 m where a ray from the threat's eyes hits something within 2.5 m of it (V34 find_cover),
//            run there, crouch, wait for health to come back, pop up to shoot (Aggression 1+), leave when healed or
//            when the cover is not working.
//
// Engine values that were not mapped yet are found while the server runs, and kept in Thugbot_learned\engine.txt:
//   health      - a float that reads 100 on every fresh spawn, drops while alive and is low when soldiers die
//                 (inline in the soldier or one pointer away - Frostbite 3 keeps it in a health component)
//   pitch       - a unit view vector in the soldier whose y changes with the pitch input (its sign tells up from down)
//   locked yaw  - some pseudo-player soldiers ignore turn input (identity rotation; seen 2026-10-23). We try weapon
//                 switch / jump / reload actions and remember which one frees them.

// ---------------------------------------------------------------- settings (Thugbot.ini, [Thugbot])
typedef struct Profile {
    float reaction;              // s from first sight to first shot
    int burstMs, pauseMs;
    float aimMargin;             // m: fire once the aim is this close to the aim point (scaled by distance)
    float turnGain;
    float engageRange, detectRange, fov;   // m, m, degrees
    float coverBelow;            // health below which a hurt bot looks for cover (before Aggression)
    float advanceStop;           // m: closer than this a bot stops walking and side-steps
    float coverHold;             // s in cover before peeking
    float strafe;                // side-step level (0 none)
    float farCrouch;             // m: beyond this a stationary shooter crouches (0 never)
} Profile;
// AltBotMod V34 difficulty table (enhanced.c kLevels), frames converted at 30 fps; ranges trimmed for Hardline's
// smaller urban maps (V34's elite 300 / 350 m was for Caspian Border).
static const Profile kProfiles[5] = {
    /* easy    */ { 0.80f, 270, 530, 0.30f, 1.5f,  60,  70,  60, 55, 15, 6.0f, 0.0f,  0 },
    /* normal  */ { 0.50f, 400, 330, 0.35f, 2.0f, 110, 140, 100, 40, 15, 6.0f, 0.4f,  0 },
    /* hard    */ { 0.30f, 470, 200, 0.40f, 3.0f, 150, 190, 130, 30, 12, 4.0f, 0.6f, 35 },   // (2026-10-23: engage / detect / fov widened - the tester)
    /* veteran */ { 0.15f, 600, 130, 0.42f, 4.0f, 150, 170, 130, 20, 10, 3.0f, 0.8f, 28 },
    /* elite   */ { 0.05f, 870,  70, 0.45f, 6.0f, 180, 200, 170, 10,  8, 2.0f, 1.0f, 22 },
};
static const char *kProfileNames[5] = { "easy", "normal", "hard", "veteran", "elite" };

typedef struct Settings {
    int difficulty;              // 0..4
    int aggression;              // 0 cautious (cover early, retreats), 1 normal, 2 aggressive (rarely hides, pushes)
    float defendShare;           // part of each team that defends owned flags (objectives.inc)
    int combat;                  // 0 = old engage() (no cover, no pitch)
    int peek;                    // bots in cover pop up to shoot (Aggression 1+)
    int vehicles;                // bots get in / drive vehicles (the tester 2026-10-25: a toggle in the UI, off by default)
    int supplies;                // bots drop ammo / first aid for teammates close by (the tester 2026-10-26)
} Settings;
static Settings g_set = { 2, 1, 0.3f, 1, 1, 0, 1 };
static const Profile *P = &kProfiles[2];

static void settings_load(void)
{
    char path[MAX_PATH], v[64];
    GetFullPathNameA("Thugbot.ini", MAX_PATH, path, NULL);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {   // first run: write the defaults, commented
        FILE *f = fopen(path, "w");
        if (f) {
            fputs("[Thugbot]\n"
                  "; Bot gunfight level: easy, normal, hard, veteran, elite (AltBotMod V34 profiles)\n"
                  "Difficulty=hard\n"
                  "; 0 = cautious: takes cover early when hurt or outnumbered, falls back when there is no cover\n"
                  "; 1 = normal: fights back, covers when badly hurt, pops up from cover to shoot\n"
                  "; 2 = aggressive: covers only when nearly dead, pushes in close and knifes\n"
                  "Aggression=2\n"
                  "; Part of each team that stays to defend flags it owns (0 - 0.8)\n"
                  "DefendShare=0.15\n"
                  "; 0 = the simple first-version combat (no cover, no pitch aim)\n"
                  "Combat=1\n"
                  "; Bots in cover stand up to shoot back now and then (Aggression 1 and 2)\n"
                  "Peek=1\n"
                  "; Bots get in and drive vehicles (1) or stay on foot and leave vehicles they spawn in (0)\n"
                  "Vehicles=0\n"
                  "; Bots drop ammo / first aid kits for teammates close by\n"
                  "Supplies=1\n", f);
            fclose(f);
        }
    }
    GetPrivateProfileStringA("Thugbot", "Difficulty", "hard", v, sizeof v, path);
    g_set.difficulty = 2;
    for (int k = 0; k < 5; k++) if (!_stricmp(v, kProfileNames[k]) || atoi(v) == k + 1) g_set.difficulty = k;
    g_set.aggression = GetPrivateProfileIntA("Thugbot", "Aggression", 2, path);
    if (g_set.aggression < 0) g_set.aggression = 0;
    if (g_set.aggression > 2) g_set.aggression = 2;
    GetPrivateProfileStringA("Thugbot", "DefendShare", "0.15", v, sizeof v, path);
    g_set.defendShare = fmaxf(0, fminf(0.8f, (float)atof(v)));
    g_set.combat = GetPrivateProfileIntA("Thugbot", "Combat", 1, path);
    g_set.peek = GetPrivateProfileIntA("Thugbot", "Peek", 1, path);
    g_set.vehicles = GetPrivateProfileIntA("Thugbot", "Vehicles", 0, path);
    g_set.supplies = GetPrivateProfileIntA("Thugbot", "Supplies", 1, path);
    P = &kProfiles[g_set.difficulty];
    H->log("settings: %s - difficulty %s, aggression %d, defend share %.2f, combat %s, peek %s, vehicles %s, supplies %s", path, kProfileNames[g_set.difficulty],
           g_set.aggression, g_set.defendShare, g_set.combat ? "on" : "off (simple)", g_set.peek ? "on" : "off", g_set.vehicles ? "on" : "off", g_set.supplies ? "on" : "off");
}

// The health below which a hurt bot goes for cover.
static float cover_below(void)
{
    if (g_set.aggression == 0) return fmaxf(P->coverBelow, 65);
    if (g_set.aggression == 2) return P->coverBelow * 0.5f;
    return P->coverBelow;
}

// ---------------------------------------------------------------- safe reads
static int rd_ok(const void *p, size_t n) { return p && (uintptr_t)p > 0x10000 && !IsBadReadPtr(p, n); }
static int heap_ptr(uintptr_t p)
{
    return p > 0x100000 && p < 0x7fff00000000ull && !(p & 7) && !(p >= H->image && p < H->image + 0x6000000);
}

// ---------------------------------------------------------------- engine.txt (discovered offsets)
static int g_hpOff = -1, g_hpSub = -1;           // health: soldier + off (sub -1) or *(soldier + off) + sub
static int g_pvSub = -1;         // pitch vector / angle: inline at soldier + g_pvOff (sub -1), or *(soldier + g_pvOff) + g_pvSub
static int g_pvAngle;            // 1 = a plain angle (radians), 0 = a unit vector
static int g_pvOff = -1, g_pvBack, g_pvUp;       // pitch view vector at soldier + off; 1 = it points backwards;
                                                 // up = the sign of pitch input that raises the view
static int g_engineLoaded;
static void engine_save(void)
{
    CreateDirectoryA("Thugbot_learned", NULL);
    FILE *f = fopen("Thugbot_learned\\engine.txt", "w");
    if (!f) return;
    fprintf(f, "# Thugbot: engine values found at run time (delete a line to find it again)\n");
    if (g_hpOff >= 0) fprintf(f, "health %#x %d\n", g_hpOff, g_hpSub);
    if (g_pvOff >= 0) fprintf(f, "pitch2 %#x %d %d %d %d\n", g_pvOff, g_pvSub, g_pvBack, g_pvUp, g_pvAngle);
    fclose(f);
}
static void engine_load(void)
{
    if (g_engineLoaded) return;
    g_engineLoaded = 1;
    FILE *f = fopen("Thugbot_learned\\engine.txt", "r");
    if (!f) { H->log("engine: no Thugbot_learned\\engine.txt yet - health and pitch will be found during play"); return; }
    char line[128];
    while (fgets(line, sizeof line, f)) {
        unsigned a; int b, c;
        if (sscanf(line, "health %x %d", &a, &b) == 2) { g_hpOff = (int)a; g_hpSub = b; }
        else if (sscanf(line, "pitchvec %x %d %d", &a, &b, &c) == 3) { g_pvOff = (int)a; g_pvBack = b; g_pvUp = c; }
        else { int d, e; if (sscanf(line, "pitch2 %x %d %d %d %d", &a, &b, &c, &d, &e) == 5) { g_pvOff = (int)a; g_pvSub = b; g_pvBack = c; g_pvUp = d; g_pvAngle = e; } }
    }
    fclose(f);
    H->log("engine: health %s%#x/%d, pitch vector %s%#x", g_hpOff >= 0 ? "" : "unknown ", g_hpOff, g_hpSub, g_pvOff >= 0 ? "" : "unknown ", g_pvOff);
}

static int hp_at(void *soldier, int off, int sub, float *out)
{
    char *s = soldier;
    if (sub < 0) { if (!rd_ok(s + off, 4)) return 0; *out = *(float *)(s + off); return 1; }
    if (!rd_ok(s + off, 8)) return 0;
    uintptr_t p = *(uintptr_t *)(s + off);
    if (!heap_ptr(p) || !rd_ok((void *)(p + sub), 4)) return 0;
    *out = *(float *)(p + sub);
    return 1;
}
// Health 0..100, or -1 while unknown / unreadable.
static float soldier_health(void *soldier)
{
    float v;
    if (g_hpOff < 0 || !soldier || !hp_at(soldier, g_hpOff, g_hpSub, &v) || !(v >= 0 && v <= 1000)) return -1;
    return v;
}

// ---------------------------------------------------------------- health discovery
#define HP_CANDS 512
typedef struct HpCand { short off, sub; unsigned spawns, drops, lowDeaths, highDeaths; int dead; } HpCand;
static HpCand g_hpc[HP_CANDS];
static int g_hpcN, g_hpcBuilt;
static float g_hpcLast[MAX_BOTS][HP_CANDS];
static unsigned g_hpcSampled[MAX_BOTS];

static void hpc_add(int off, int sub)
{
    if (g_hpcN < HP_CANDS) { HpCand *c = &g_hpc[g_hpcN++]; memset(c, 0, sizeof *c); c->off = (short)off; c->sub = (short)sub; c->spawns = 1; }
}
// A soldier that has just spawned (full health): the first one lists every 100.0, the next ones strike off the rest.
static void hp_on_spawn(void *soldier)
{
    if (g_hpOff >= 0) return;
    char *s = soldier;
    if (!g_hpcBuilt) {
        g_hpcBuilt = 1;
        for (int off = 0; off < 0x2000; off += 4) if (rd_ok(s + off, 4) && *(float *)(s + off) == 100.0f) hpc_add(off, -1);
        for (int off = 0; off < 0x1800; off += 8) {
            if (!rd_ok(s + off, 8)) continue;
            uintptr_t p = *(uintptr_t *)(s + off);
            if (!heap_ptr(p) || p == (uintptr_t)s || !rd_ok((void *)p, 0x200)) continue;
            for (int sub = 0; sub < 0x200; sub += 4) if (*(float *)(p + sub) == 100.0f) hpc_add(off, sub);
        }
        H->log("health: %d candidate(s) on the first fresh spawn", g_hpcN);
        return;
    }
    int alive = 0;
    for (int k = 0; k < g_hpcN; k++) {
        HpCand *c = &g_hpc[k];
        if (c->dead) continue;
        float v;
        if (!hp_at(soldier, c->off, c->sub, &v) || v != 100.0f) { c->dead = 1; continue; }
        c->spawns++; alive++;
    }
    static int lastAlive = -1;
    if (alive != lastAlive && alive < 40) { lastAlive = alive; H->log("health: %d candidate(s) left after a spawn check", alive); }
}

static void hp_pick(void)
{
    int best = -1, bestScore = 0;
    for (int k = 0; k < g_hpcN; k++) {
        HpCand *c = &g_hpc[k];
        if (c->dead || c->spawns < 3 || c->drops < 3 || c->lowDeaths < 2 || c->highDeaths * 2 > c->lowDeaths) continue;
        int score = (int)(c->lowDeaths * 4 + c->drops) - (int)c->highDeaths * 6 + (c->sub < 0 ? 2 : 0);
        if (score > bestScore) { bestScore = score; best = k; }
    }
    if (best < 0) return;
    g_hpOff = g_hpc[best].off; g_hpSub = g_hpc[best].sub;
    H->log("health: FOUND at soldier %s%#x%s%#x (%u spawns at 100, %u drops, %u deaths low / %u high) - cover-when-hurt is on",
           g_hpSub < 0 ? "+" : "*(+", g_hpOff, g_hpSub < 0 ? "" : ") +", g_hpSub < 0 ? 0 : g_hpSub,
           g_hpc[best].spawns, g_hpc[best].drops, g_hpc[best].lowDeaths, g_hpc[best].highDeaths);
    engine_save();
}

// Every few frames per live bot: candidate values must stay within 0..100; dropping below 100 counts.
static void hp_sample(int i, void *soldier)
{
    if (g_hpOff >= 0 || !g_hpcBuilt || (H->frame + i) % 4) return;
    for (int k = 0; k < g_hpcN; k++) {
        HpCand *c = &g_hpc[k];
        if (c->dead) continue;
        float v;
        if (!hp_at(soldier, c->off, c->sub, &v) || !(v >= 0 && v <= 100.0f)) { c->dead = 1; continue; }
        if (v < 99.5f && v > 0 && g_hpcLast[i][k] >= 99.5f) c->drops++;
        g_hpcLast[i][k] = v;
    }
    g_hpcSampled[i] = H->frame;
}
static void hp_on_death(int i)
{
    if (g_hpOff >= 0 || !g_hpcBuilt || H->frame - g_hpcSampled[i] > 12) return;   // the last sample has to be recent
    for (int k = 0; k < g_hpcN; k++) {
        HpCand *c = &g_hpc[k];
        if (c->dead) continue;
        if (g_hpcLast[i][k] < 50) c->lowDeaths++; else c->highDeaths++;
    }
    hp_pick();
}

// ---------------------------------------------------------------- pitch discovery
// One idle bot holds still and looks up (20 frames), down (40), up (20). Searched: the soldier itself and every object
// it points to (first 0x400 bytes each). Best: a unit vector whose horizontal part is the heading (or its opposite) and
// whose y goes up, down, up - its y gives the elevation and its sign tells which input looks up. Else: a plain angle
// that goes up, down, up (its "up" sign cannot be told from memory; assumed + input = up until a vector confirms it).
#define PC_SLOTS 1024            // pointer slots in the soldier (0..0x2000 step 8)
#define PC_SUB 256               // floats per pointed object
typedef struct PitchCal { int bot; void *soldier; int phase; unsigned at; int tries; ULONGLONG nextAt; } PitchCal;
static PitchCal g_pc = { -1 };
static float g_pcIn[4][0x800];
static float g_pcObj[4][PC_SLOTS][PC_SUB];
static uintptr_t g_pcPtr[4][PC_SLOTS];
static void pc_snap(void *soldier, int k)
{
    char *s = soldier;
    for (int j = 0; j < 0x800; j++) g_pcIn[k][j] = rd_ok(s + j * 4, 4) ? *(float *)(s + j * 4) : NAN;
    for (int j = 0; j < PC_SLOTS; j++) {
        uintptr_t p = rd_ok(s + j * 8, 8) ? *(uintptr_t *)(s + j * 8) : 0;
        if (!heap_ptr(p) || p == (uintptr_t)s || !rd_ok((void *)p, PC_SUB * 4)) p = 0;
        g_pcPtr[k][j] = p;
        if (p) memcpy(g_pcObj[k][j], (void *)p, PC_SUB * 4); else g_pcObj[k][j][0] = NAN;
    }
}
static int pc_unit(const float *s, int k) { float x = s[k], y = s[k + 1], z = s[k + 2]; float l = x * x + y * y + z * z; return l > 0.98f && l < 1.02f; }
// Up, down, up: the changes alternate and are big enough.
static int pc_wave(float a, float b, float c, float d, float minStep)
{
    float d1 = b - a, d2 = c - b, d3 = d - c;
    return fabsf(d1) >= minStep && fabsf(d2) >= minStep && d1 * d2 < 0 && d2 * d3 < 0;
}
static void pc_consider(const float *A, const float *B, const float *C, const float *D, int n, int off, int sub, float yaw,
                        int *vOff, int *vSub, int *vK, float *vBest, int *vBack, float *vD1, int *aOff, int *aSub, int *aK, float *aBest, float *aD1)
{
    for (int k = 0; k + 2 < n; k++) {
        if (pc_unit(A, k) && pc_unit(B, k) && pc_unit(C, k) && pc_unit(D, k) && pc_wave(A[k + 1], B[k + 1], C[k + 1], D[k + 1], 0.04f) && fabsf(A[k + 1]) < 0.9f) {
            float h = atan2f(A[k], A[k + 2]);
            int fwd = fabsf(wrap_pi(h - yaw)) < 0.35f, bk = fabsf(wrap_pi(h - yaw - 3.14159265f)) < 0.35f;
            float m = fabsf(B[k + 1] - A[k + 1]);
            if ((fwd || bk) && m > *vBest) { *vBest = m; *vOff = off; *vSub = sub < 0 ? -1 : k * 4; *vK = sub < 0 ? k * 4 : 0; *vBack = bk; *vD1 = B[k + 1] - A[k + 1]; }
        }
        float a = A[k];
        if (a == a && fabsf(a) < 1.6f && pc_wave(a, B[k], C[k], D[k], 0.05f) && fabsf(B[k] - A[k]) < 1.6f && fabsf(C[k]) < 1.6f) {
            float m = fabsf(B[k] - A[k]);
            if (m > *aBest) { *aBest = m; *aOff = off; *aSub = sub < 0 ? -1 : k * 4; *aK = sub < 0 ? k * 4 : 0; *aD1 = B[k] - A[k]; }
        }
    }
}
static void pc_finish(void *soldier)
{
    float yaw = soldier_yaw(soldier);
    int vOff = -1, vSub = -1, vK = 0, vBack = 0, aOff = -1, aSub = -1, aK = 0; float vBest = 0, aBest = 0, vD1 = 0, aD1 = 0;
    pc_consider(g_pcIn[0], g_pcIn[1], g_pcIn[2], g_pcIn[3], 0x800, -1, -1, yaw, &vOff, &vSub, &vK, &vBest, &vBack, &vD1, &aOff, &aSub, &aK, &aBest, &aD1);
    if (vOff == -1 && vBest > 0) vOff = vK;   // inline: the offset is the float's own
    if (aOff == -1 && aBest > 0) aOff = aK;
    for (int j = 0; j < PC_SLOTS; j++) {
        if (!g_pcPtr[0][j] || g_pcPtr[0][j] != g_pcPtr[1][j] || g_pcPtr[1][j] != g_pcPtr[2][j] || g_pcPtr[2][j] != g_pcPtr[3][j]) continue;
        float vb = vBest, ab = aBest;
        pc_consider(g_pcObj[0][j], g_pcObj[1][j], g_pcObj[2][j], g_pcObj[3][j], PC_SUB, j * 8, 0, yaw, &vOff, &vSub, &vK, &vBest, &vBack, &vD1, &aOff, &aSub, &aK, &aBest, &aD1);
        if (vBest > vb) vOff = j * 8;
        if (aBest > ab) aOff = j * 8;
    }
    g_pc.tries++;
    int isVec = vBest > 0, off = isVec ? vOff : aOff, sub = isVec ? vSub : aSub;
    if (!isVec && aBest <= 0) { H->log("pitch: nothing moved with the pitch input, in the soldier or one pointer away (try %d)", g_pc.tries); return; }
    float d1 = isVec ? (vBack ? -vD1 : vD1) : aD1;   // change of elevation (vector) / of the angle for + input
    int up = d1 > 0 ? 1 : -1;
    static int lastOff = -2, lastSub = -2, agree;
    if (off == lastOff && sub == lastSub) agree++; else { lastOff = off; lastSub = sub; agree = 1; }
    H->log("pitch: %s at soldier %s%#x%s%#x (+input %s, change %.3f) - %d agreeing run(s)", isVec ? "view vector" : "angle",
           sub < 0 ? "+" : "*(+", off, sub < 0 ? "" : ") +", sub < 0 ? 0 : sub, up > 0 ? "raises it" : "lowers it", d1, agree);
    if (agree >= 2) {
        g_pvOff = off; g_pvSub = sub; g_pvBack = isVec ? vBack : 0; g_pvUp = up; g_pvAngle = !isVec;
        engine_save();
        H->log("pitch: FOUND (%s) - bots aim up and down now", isVec ? "view vector" : "angle; up assumed to be the + input side");
    }
}
static int pc_bot_ok(int i);
static int pc_running(int i) { return g_pc.bot == i && g_pc.phase > 0; }
// Returns 1 while it controls bot i (the brain leaves it standing).
static int pc_tick(int i, Bot *b, void *soldier)
{
    if (g_pvOff >= 0) return 0;
    ULONGLONG t = now_ms();
    if (g_pc.phase == 0) {
        if (t < g_pc.nextAt || g_pc.tries >= 12) return 0;
        if (!pc_bot_ok(i)) return 0;   // an idle bot, not hurt lately
        g_pc.bot = i; g_pc.soldier = soldier; g_pc.phase = 1; g_pc.at = H->frame;
    }
    if (g_pc.bot != i) return 0;
    if (g_pc.soldier != soldier) { g_pc.phase = 0; g_pc.bot = -1; g_pc.nextAt = t + 5000; return 0; }
    unsigned f = H->frame - g_pc.at;
    b->fwd = b->strafe = b->turn = 0; b->buttons = 0;
    b->pitch = 0;
    if (f == 10) pc_snap(soldier, 0);
    else if (f > 10 && f <= 30) b->pitch = 0.6f;
    else if (f == 33) pc_snap(soldier, 1);
    else if (f > 33 && f <= 73) b->pitch = -0.6f;
    else if (f == 76) pc_snap(soldier, 2);
    else if (f > 76 && f <= 96) b->pitch = 0.6f;
    else if (f == 99) pc_snap(soldier, 3);
    else if (f > 99) { pc_finish(soldier); g_pc.phase = 0; g_pc.bot = -1; g_pc.nextAt = t + 20000; return 0; }
    return 1;
}
// Elevation of the soldier's view (radians, + up), or NAN while unknown.
static float view_elevation(void *soldier)
{
    if (g_pvOff < 0) return NAN;
    const float *v;
    if (g_pvSub < 0) v = (const float *)((char *)soldier + g_pvOff);
    else {
        if (!rd_ok((char *)soldier + g_pvOff, 8)) return NAN;
        uintptr_t p = *(uintptr_t *)((char *)soldier + g_pvOff);
        if (!heap_ptr(p)) return NAN;
        v = (const float *)(p + g_pvSub);
    }
    if (!rd_ok(v, 12)) return NAN;
    if (g_pvAngle) return v[0];   // angle (radians; taken as + up, the side the + input moves it, see g_pvUp)
    float y = fmaxf(-1, fminf(1, v[1]));
    return g_pvBack ? -asinf(y) : asinf(y);
}

// ---------------------------------------------------------------- per-bot combat state
enum { CS_IDLE, CS_FIGHT, CS_COVER_RUN, CS_COVER_HOLD, CS_CHASE };
static const char *kCsNames[] = { "idle", "fight", "to cover", "in cover", "chasing" };
typedef struct Fight {
    ULONGLONG lowCoverAt; int lowCover;   // a low wall toward the enemy (duck behind it)
    void *soldier;
    int state;
    void *target;                // enemy ServerPlayer*
    ULONGLONG seenAt, firstSeenAt, lookAt, burstEnd, pauseEnd, stateAt, dodgeEnd, peekEnd, peekNext, coverNext;
    float threat[3], goal[3];
    float hp, hpPrev;
    ULONGLONG hurtAt, alertUntil;
    void *hitBy;                 // who shot us last (from the server's damage handler, core.c)
    int hitsTaken, coverTries, dodgeDir, visible;
    float tgtHpAtHit;
    float aimH;                  // aim this high on the target: the part of it last seen (chest 1.15, head 1.5)
    ULONGLONG reloadAt;          // tactical reload done for the last fight
    // locked heading
    ULONGLONG lastFireAt;
    ULONGLONG lockSince, lockActAt; float lockYaw; int locked, lockTry, lockAct;
} Fight;
static Fight g_fight[MAX_BOTS];
static int pc_bot_ok(int i) { return g_fight[i].state == CS_IDLE && !g_fight[i].locked && g_fight[i].soldier && now_ms() - g_fight[i].hurtAt > 5000; }
static unsigned g_cStats[8];     // since the last "bots": shots begun, cover taken, cover not found, peeks, unlocks, chases
static int g_unlockOk[24];       // pseudo-player actions that freed a locked heading, and how often
static const int kUnlockActs[] = { 9, 10, 2, 8, 13 };   // switch weapon, switch inventory, jump, reload, change entry

static void fight_set(int i, Fight *f, int state)
{
    if (f->state != state) { f->state = state; f->stateAt = now_ms(); }
    (void)i;
}

static int player_alive_enemy(void *pl, int team)
{
    int t = FIELD(pl, OFF_PLAYER_TEAM, int);
    return t && t != team && bot_soldier(pl) != NULL;
}

// Eye-to-body line of sight (the ray ignores characters: any hit short of the target is in the way).
static int clear_line(const float *a, const float *b)
{
    float hit[3];
    if (!ray_cast(a, b, hit, NULL)) return 1;
    float dx = b[0] - hit[0], dy = b[1] - hit[1], dz = b[2] - hit[2];
    return dx * dx + dy * dy + dz * dz < 0.6f * 0.6f;
}
// 1 chest in sight, 2 only the head (behind a low wall / crouched behind cover), 0 neither
static int sees(const float *eye, const float *tp)
{
    float chest[3] = { tp[0], tp[1] + 1.2f, tp[2] }, head[3] = { tp[0], tp[1] + 1.6f, tp[2] };
    return clear_line(eye, chest) ? 1 : clear_line(eye, head) ? 2 : 0;
}

// The enemy this bot fights now: the current one while it stays in sight; else the nearest visible one in the field of
// view (any direction within 8 m, or while just hurt). Rays are spread: each bot looks every 120 ms.
static void *pick_enemy(int i, Fight *f, void *soldier, int team, float *distOut)
{
    const float *p = soldier_pos(soldier);
    float eye[3] = { p[0], p[1] + 1.6f, p[2] }, yaw = soldier_yaw(soldier);
    ULONGLONG t = now_ms();
    int alert = t - f->hurtAt < 2500 || t < f->alertUntil;
    if (t < f->lookAt) {   // between looks: keep the current target while it is alive
        if (f->target && player_alive_enemy(f->target, team)) { *distOut = dist2d(p, soldier_pos(bot_soldier(f->target))); return f->visible ? f->target : NULL; }
        return NULL;
    }
    f->lookAt = t + (H->botCount > 40 ? 180 : 120) + (i % 4) * 10;   // (60 bots: looks a little less often - the server was 85% busy)
    void *best = NULL; float bestD = 0;
    // Candidates in range and view, nearest first; rays for the nearest 8. (The ray budget used to go to whoever came
    // first in the server's player list: with 32 players, a human standing right next to a bot was often never
    // looked at - "they ran past me".)
    struct { void *pl; float score, d; } c[64];
    int nc = 0;
    for (int j = 0; j < g_playerCount && nc < 64; j++) {
        void *pl = g_players[j];
        if (!player_alive_enemy(pl, team)) continue;
        const float *q = soldier_pos(bot_soldier(pl));
        float d = dist2d(p, q);
        if (d > P->detectRange * (alert ? 1.2f : 1.0f) || fabsf(q[1] - p[1]) > 40) continue;
        float ang = fabsf(wrap_pi(atan2f(q[0] - p[0], q[2] - p[2]) - yaw)) * 57.29578f;
        if (pl != f->target && d > 12 && !alert && ang > P->fov * 0.5f) continue;
        c[nc].pl = pl; c[nc].d = d; c[nc].score = d - (pl == f->target ? 25.0f : 0) - (pl == f->hitBy && t - f->hurtAt < 5000 ? 40.0f : 0); nc++;
    }
    for (int rays = 0; rays < 8 && rays < nc; rays++) {
        int m = rays;
        for (int k = rays + 1; k < nc; k++) if (c[k].score < c[m].score) m = k;
        if (m != rays) { void *tp = c[m].pl; float ts = c[m].score, td = c[m].d; c[m] = c[rays]; c[rays].pl = tp; c[rays].score = ts; c[rays].d = td; }
        g_seenInRange++;
        int part = sees(eye, soldier_pos(bot_soldier(c[rays].pl)));
        if (!part) { g_seenBlocked++; continue; }
        best = c[rays].pl; bestD = c[rays].d; f->aimH = part == 2 ? 1.5f : 1.15f;
        break;
    }
    f->visible = best != NULL;
    if (best) {
        if (best != f->target) { f->target = best; f->firstSeenAt = t; }
        f->seenAt = t;
        memcpy(f->threat, soldier_pos(bot_soldier(best)), 12);
    }
    *distOut = bestD;
    return best;
}

// From the server's damage handler (core.c): a soldier was hit by `giver` (a ServerPlayer, or NULL). If it is one of
// our bots, it knows at once that it was hit and by whom - no waiting for the health value to drop.
static int is_bot_player(void *pl);   // human_routes.inc
static unsigned g_botHits, g_fireFrames;   // hits bots landed / frames bots held fire ("combat" dump)
static void combat_note_hit(void *victim, void *giver)
{
    for (int i = 0; i < H->botCount; i++) {
        if (!victim || bot_soldier_raw(H->bots[i]) != victim) continue;
        Fight *f = &g_fight[i];
        ULONGLONG t = now_ms();
        if (t - f->hurtAt > 300) f->hitsTaken++;   // (a shotgun's pellets are one hit)
        f->hurtAt = t;
        if (giver && giver != H->bots[i]) f->hitBy = giver;
        break;
    }
    if (giver && is_bot_player(giver)) g_botHits++;
}

// "raydiff": does the ray flag set change what a bot can see? Every bot to every enemy within 150 m, eye to chest, with
// the usual flags and with other sets. (Black Friday: bots shot at walls - a wall the sight ray passes through?)
// "groundprobe": what is under each bot - a ray from 2 m above to 4 m below: the hit height and the physics body's
// class (vtable). On water maps the bots in the sea stand on a different class: that is the water surface.
static void ground_probe(void)
{
    for (int i = 0; i < H->botCount; i++) {
        void *s = bot_soldier(H->bots[i]);
        if (!s) continue;
        const float *p = soldier_pos(s), *v = (const float *)((char *)s + OFF_SOLDIER_VELOCITY);
        float a[3] = { p[0], p[1] + 2, p[2] }, b[3] = { p[0], p[1] - 4, p[2] }, h[3];
        int hit = ray_cast(a, b, h, NULL);
        uintptr_t body = hit ? *(uintptr_t *)g_lastHit.rest : 0, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body : 0;
        H->log("ground: %s at %.1f %.1f %.1f speed %.1f - %s %.1f m below, body vt %#llx", (char *)H->bots[i] + OFF_PLAYER_NAME, p[0], p[1], p[2],
               sqrtf(v[0] * v[0] + v[2] * v[2]), hit ? "hit" : "nothing", hit ? p[1] - h[1] : 0.0f, (unsigned long long)(vt ? vt - H->image : 0));
    }
}

static void humans_dump(void)
{
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        int bot = 0;
        for (int i = 0; i < H->botCount; i++) bot |= H->bots[i] == pl;
        if (bot) continue;
        void *s = bot_soldier_raw(pl);
        H->log("human: %s team %d soldier %p, takes input %d, health %.0f, counts as alive %d", (char *)pl + OFF_PLAYER_NAME, FIELD(pl, OFF_PLAYER_TEAM, int),
               s, s ? soldier_takes_input(s) : -1, s ? soldier_health(s) : -1.0f, bot_soldier(pl) != NULL);
    }
}
static void raydiff(void)
{
    static const int sets[4] = { 0x10 | 0x20 | 0x80, 0x10 | 0x20 | 0x80 | 0x1, 0x10 | 0x20, 0x20 };
    unsigned clear[4] = { 0 }, pairs = 0, differ[4] = { 0 };
    int saved = g_rayFlags;
    for (int i = 0; i < H->botCount && pairs < 400; i++) {
        void *s1 = bot_soldier(H->bots[i]);
        if (!s1) continue;
        int team = FIELD(H->bots[i], OFF_PLAYER_TEAM, int);
        const float *p = soldier_pos(s1);
        float eye[3] = { p[0], p[1] + 1.6f, p[2] };
        for (int j = 0; j < g_playerCount && pairs < 400; j++) {
            if (!player_alive_enemy(g_players[j], team)) continue;
            const float *q = soldier_pos(bot_soldier(g_players[j]));
            if (dist2d(p, q) > 150) continue;
            float chest[3] = { q[0], q[1] + 1.2f, q[2] };
            int vis[4];
            for (int k = 0; k < 4; k++) { g_rayFlags = sets[k]; vis[k] = clear_line(eye, chest); clear[k] += vis[k]; }
            for (int k = 1; k < 4; k++) differ[k] += vis[k] != vis[0];
            pairs++;
        }
    }
    g_rayFlags = saved;
    humans_dump();
    H->log("raydiff: %u pair(s); clear with %#x: %u, %#x: %u (%u differ), %#x: %u (%u differ), %#x: %u (%u differ)", pairs,
           sets[0], clear[0], sets[1], clear[1], differ[1], sets[2], clear[2], differ[2], sets[3], clear[3], differ[3]);
}

// ---------------------------------------------------------------- cover (AltBotMod V34 find_cover)
static int find_cover(int i, void *soldier, const float *threatEye, float *out)
{
    (void)i;
    if (!g_nav || g_navBuilding) return 0;
    const float *me = soldier_pos(soldier);
    float myD = dist2d(me, threatEye), best = 1e9f;
    int found = 0;
    static const float radii[6] = { 0, 4, 8, 12, 16, 22 };
    for (int r = 0; r < 6; r++)
        for (int a = 0; a < (r ? 12 : 1); a++) {
            float ang = a * 0.5235988f + r * 0.26f, pt[3];
            if (!r) memcpy(pt, me, 12);
            else if (!nav_snap(g_nav, me[0] + sinf(ang) * radii[r], me[1], me[2] + cosf(ang) * radii[r], pt)) continue;
            if (fabsf(pt[1] - me[1]) > 3) continue;
            float td = dist2d(pt, threatEye);
            if (td < 10 || (r && td < myD - 2)) continue;   // not towards the threat
            float hit[3], chest[3] = { pt[0], pt[1] + 1.0f, pt[2] }, low[3] = { pt[0], pt[1] + 0.6f, pt[2] };
            // Covered (crouched): the threat's ray to our crouched chest stops within 2.5 m in front of the spot.
            if (!ray_cast(threatEye, chest, hit, NULL) || dist2d(hit, pt) > 2.5f || !ray_cast(threatEye, low, NULL, NULL)) continue;
            float score = radii[r] + (myD - td) * 0.3f;
            if (r && !nav_direct_route(g_nav, ray_cast, me, pt)) score += 6;   // not a straight walk: a little dearer
            if (score < best) { best = score; memcpy(out, pt, 12); found = 1; }
        }
    return found;
}
// No cover near (cautious bots): fall back 15 m directly away from the threat, onto the grid.
static int find_fallback(void *soldier, const float *threat, float *out)
{
    if (!g_nav || g_navBuilding) return 0;
    const float *me = soldier_pos(soldier);
    float dx = me[0] - threat[0], dz = me[2] - threat[2], l = sqrtf(dx * dx + dz * dz);
    if (l < 0.5f) return 0;
    for (int k = 0; k < 3; k++) {
        float side = (k == 1 ? 0.6f : k == 2 ? -0.6f : 0), x = dx / l + side * dz / l, z = dz / l - side * dx / l;
        if (nav_snap(g_nav, me[0] + x * 15, me[1], me[2] + z * 15, out) && fabsf(out[1] - me[1]) < 3) return 1;
    }
    return 0;
}

static int go_cover(int i, Fight *f, void *soldier, const char *why)
{
    ULONGLONG t = now_ms();
    if (t < f->coverNext) return 0;
    f->coverNext = t + 1500;
    static unsigned searchedAt;   // one cover search per frame across all bots (up to ~140 rays)
    if (searchedAt == H->frame) return 0;
    searchedAt = H->frame;
    float eye[3] = { f->threat[0], f->threat[1] + 1.6f, f->threat[2] }, spot[3];
    int fallback = 0;
    if (!find_cover(i, soldier, eye, spot)) {
        if (g_set.aggression > 0 || !find_fallback(soldier, f->threat, spot)) { g_cStats[2]++; return 0; }
        fallback = 1;
    }
    memcpy(f->goal, spot, 12);
    fight_set(i, f, CS_COVER_RUN);
    f->hitsTaken = 0;
    f->coverTries++;
    g_cStats[1]++;
    if (g_cStats[1] < 60)
        H->log("cover: %s %s %.0f m away (%s, health %.0f)", (char *)H->bots[i] + OFF_PLAYER_NAME, fallback ? "falls back" : "takes cover",
               dist2d(soldier_pos(soldier), spot), why, f->hp);
    return 1;
}

// combat owns the bot's destination (cover, fallback, chase): brain.inc's choose_target asks here first.
static int combat_target(int i, Bot *b)
{
    Fight *f = &g_fight[i];
    if (!g_set.combat || f->soldier != bot_soldier(b->player)) return 0;
    if (f->state != CS_COVER_RUN && f->state != CS_COVER_HOLD && f->state != CS_CHASE) return 0;
    if (dist2d(b->target, f->goal) > 1 || b->targetFlag != -2) b->npts = 0;
    memcpy(b->target, f->goal, 12);
    b->targetFlag = -2;
    return 1;
}

// ---------------------------------------------------------------- locked heading
static void lock_tick(int i, Bot *b, Fight *f, void *soldier)
{
    ULONGLONG t = now_ms();
    float yaw = soldier_yaw(soldier);
    if (yaw != f->lockYaw || fabsf(b->turn) < 0.5f) {
        if (f->locked && yaw != f->lockYaw) {
            f->locked = 0;
            H->log("lock: %s free after %.1f s", (char *)b->player + OFF_PLAYER_NAME, (t - f->lockSince) / 1000.0);
            if (f->lockAct) { g_unlockOk[f->lockAct]++; g_cStats[4]++; if (g_cStats[4] < 30) H->log("heading: %s turns again after action %d", (char *)b->player + OFF_PLAYER_NAME, f->lockAct); }
        }
        f->lockYaw = yaw; f->lockSince = t; f->lockAct = 0;
        return;
    }
    if (t - f->lockSince < 1500) return;
    if (!f->locked) {
        f->locked = 1; f->lockTry = 0; f->lockActAt = 0;
        static unsigned logged;
        if (logged++ < 80) {
            const float *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
            H->log("lock: %s stuck at yaw %.2f (takes input %d) - state %s, buttons %#x, pitch in %.2f, input %.2f %.2f %.2f, speed %.1f, health %.0f, action %d, last fire %.1f s ago",
                   (char *)b->player + OFF_PLAYER_NAME, yaw, soldier_takes_input(soldier), kCsNames[f->state], b->buttons, b->pitch, b->fwd, b->strafe, b->turn,
                   sqrtf(v[0] * v[0] + v[2] * v[2]), f->hp, b->action, f->lastFireAt ? (t - f->lastFireAt) / 1000.0 : -1.0);
        }
    }
    if (t < f->lockActAt) { if (t < f->lockActAt - 900) b->action = f->lockAct; return; }
    // The action that worked most often first, then the rest in turn.
    int order[8], n = 0, bestA = 0;
    for (int k = 0; k < (int)(sizeof kUnlockActs / sizeof *kUnlockActs); k++) if (g_unlockOk[kUnlockActs[k]] > g_unlockOk[bestA]) bestA = kUnlockActs[k];
    if (bestA) order[n++] = bestA;
    for (int k = 0; k < (int)(sizeof kUnlockActs / sizeof *kUnlockActs); k++) if (kUnlockActs[k] != bestA) order[n++] = kUnlockActs[k];
    f->lockAct = order[f->lockTry++ % n];
    f->lockActAt = t + 1200;
    b->action = f->lockAct;
}

// ---------------------------------------------------------------- the fight itself
static void aim_at(Bot *b, Fight *f, void *soldier, void *enemySoldier, float d, float *yawErr, float *pitchErr)
{
    const float *p = soldier_pos(soldier), *q = soldier_pos(enemySoldier);
    const float *v = (const float *)((char *)enemySoldier + OFF_SOLDIER_VELOCITY);
    float lead = d / 600.0f + 0.08f, aim[3] = { q[0] + v[0] * lead, q[1] + (f && f->aimH > 0 ? f->aimH : 1.15f), q[2] + v[2] * lead };
    float eye[3] = { p[0], p[1] + 1.6f, p[2] };
    float want = atan2f(aim[0] - eye[0], aim[2] - eye[2]), err = wrap_pi(want - soldier_yaw(soldier));
    b->turn = turn_input(soldier_yaw(soldier), want, P->turnGain);
    // Fine aim at range: the plain gain turns too slowly for the last half metre (aimlog, 2026-10-24: 0.5 - 0.9 m off at
    // 30 - 50 m, turn input 0.05) - a small error gets a firmer correction.
    if (fabsf(err) * d > 0.15f && fabsf(b->turn) > 1e-4f && fabsf(b->turn) < 0.12f) b->turn *= fminf(0.12f / fabsf(b->turn), 3.0f);
    *yawErr = err;
    float el = view_elevation(soldier);
    if (el == el) {
        float wantEl = atan2f(aim[1] - eye[1], fmaxf(0.5f, dist2d(eye, aim)));
        *pitchErr = wantEl - el;
        b->pitch = fmaxf(-1, fminf(1, g_pvUp * *pitchErr * 3.0f));
        // (a 0.02 dead band left 0.3 - 0.7 m pitch errors at 80 - 100 m uncorrected: scale small errors up instead)
        if (fabsf(*pitchErr) * d < 0.1f) b->pitch = 0;
        else if (fabsf(b->pitch) < 0.06f) b->pitch = b->pitch < 0 ? -0.06f : 0.06f;
    } else { *pitchErr = 0; b->pitch = 0; }
}

static void fire_control(Bot *b, Fight *f, float d, float yawErr, float pitchErr, int locked)
{
    ULONGLONG t = now_ms();
    if (locked || d > P->engageRange || t - f->firstSeenAt < (ULONGLONG)(P->reaction * 1000)) return;
    float margin = P->aimMargin * (d < 12 ? 2.0f : 1.0f);
    // (yaw allowance 2.5x margin = 1 m: at 30 m+ most of those shots missed a 0.5 m wide body - 1.7 hits per 100 frames)
    float yawK = d < 12 ? 2.5f : d < 30 ? 1.6f : 1.0f, pitchK = d < 12 ? 4.0f : 2.0f;
    if (fabsf(yawErr) * fmaxf(d, 1) > margin * yawK || fabsf(pitchErr) * fmaxf(d, 1) > margin * pitchK) { f->burstEnd = 0; return; }
    if (t < f->pauseEnd) return;
    if (!f->burstEnd) { f->burstEnd = t + (ULONGLONG)(P->burstMs * (d > 60 ? 0.5f : 1.0f)); g_cStats[0]++; }
    if (t < f->burstEnd) {
        // Release every 4th frame in a burst: semi-automatic guns fire on each press, automatic ones barely notice.
        if ((H->frame & 3) != 3) { b->buttons |= BTN_FIRE; f->lastFireAt = t; g_fireFrames++; }
        { static ULONGLONG logAt; static unsigned logged; if (t > logAt && logged < 200) { logAt = t + 1500; logged++; H->log("aimlog: %s d %.0f yawoff %.2f m pitchoff %.2f m fwd %.1f strafe %.1f turn %.2f pitch %.2f", (char *)b->player + OFF_PLAYER_NAME, d, yawErr * d, pitchErr * d, b->fwd, b->strafe, b->turn, b->pitch); } }
        return;
    }
    f->burstEnd = 0;
    f->pauseEnd = t + P->pauseMs + (d > 60 ? 150 : 0);
}

static void close_moves(int i, Bot *b, Fight *f, float d, const float *p, const float *q)
{
    ULONGLONG t = now_ms();
    (void)p; (void)q;
    if (g_set.aggression == 2 && d < 7) {   // push in, knife at arm's length
        b->fwd = 0.8f; b->strafe = 0;
        if (d < 2.0f) b->buttons |= BTN_MELEE;
        return;
    }
    b->fwd = d < 4 ? -0.4f : 0;
    if (P->strafe <= 0) { b->strafe = 0; return; }
    if (t >= f->dodgeEnd) {
        unsigned r = (unsigned)(t / 37) * 2654435761u + i * 40503u;
        f->dodgeDir = f->dodgeDir ? 0 : ((r >> 7) & 1 ? 1 : -1);   // step, rest, step...
        f->dodgeEnd = t + (f->dodgeDir ? 350 + (r >> 9) % 450 : 250 + (r >> 13) % 500);
    }
    b->strafe = f->dodgeDir * P->strafe;
}

// Replaces brain.inc's engage(): runs after steering every frame for a live bot. Sets b->enemy while fighting.
// ---------------------------------------------------------------- grenades
// A bot throws a grenade (pseudo-player action 12, ThrowGrenade) at an enemy 12 - 35 m away who just ducked out of
// sight, or at two or more enemies bunched within 6 m of each other. Faces the spot first; one every 25 s per bot (the
// loadout decides whether it has any - nothing happens when it doesn't).
#define PP_ACT_GRENADE 12
static ULONGLONG g_nadeNext[MAX_BOTS], g_nadeUntil[MAX_BOTS];
static float g_nadeAt[MAX_BOTS][3];
static unsigned g_nades;
static int grenade_tick(int i, Bot *b, void *soldier)
{
    ULONGLONG t = now_ms();
    if (t >= g_nadeUntil[i]) return 0;
    const float *p = soldier_pos(soldier);
    b->turn = turn_input(soldier_yaw(soldier), atan2f(g_nadeAt[i][0] - p[0], g_nadeAt[i][2] - p[2]), 3.0f);
    b->fwd = b->strafe = 0;
    if (g_nadeUntil[i] - t < 450) b->action = PP_ACT_GRENADE;   // after turning to face it
    return 1;
}
static void grenade_consider(int i, Bot *b, Fight *f, void *soldier, int team, int visible)
{
    ULONGLONG t = now_ms();
    if (t < g_nadeNext[i] || !f->target || !player_alive_enemy(f->target, team)) return;
    const float *p = soldier_pos(soldier), *q = visible ? soldier_pos(bot_soldier(f->target)) : f->threat;
    float d = dist2d(p, q);
    if (d < 12 || d > 35 || fabsf(q[1] - p[1]) > 6) return;
    int bunch = 0;
    for (int j = 0; j < g_playerCount; j++)
        if (player_alive_enemy(g_players[j], team) && dist2d(soldier_pos(bot_soldier(g_players[j])), q) < 6) bunch++;
    int ducked = !visible && t - f->seenAt < 3000;
    if (!ducked && bunch < 2) return;
    g_nadeNext[i] = t + 25000 + (i * 977) % 5000;
    g_nadeUntil[i] = t + 1100;
    memcpy(g_nadeAt[i], q, 12);
    if (g_nades++ < 30) H->log("grenade: %s throws at %.0f m (%s)", (char *)b->player + OFF_PLAYER_NAME, d, ducked ? "it ducked into cover" : "enemies bunched up");
    (void)b;
}

// ---------------------------------------------------------------- launchers and gadgets
// the tester: "use all kinds of equipment including launchers" / "if enemies are behind breakable walls, why not use a grenade
// launcher". Hardline's inputs EIASwitchToGrenadeLauncher / EIASwitchToDynamicGadget1/2 / EIASwitchToPrimaryWeapon
// (hashes from the Concept names, same scheme as EIAFire) select the item; fire uses it. The loadout decides what is
// there: a bot without a launcher just fires its gun once and switches back.
//   Launcher: the enemy it was fighting went out of sight in the last 5 s, 10 - 70 m away, and the line to him stops
//             within 4 m of him (he is just behind a wall / car / crate), or 2+ enemies bunched within 6 m at 20 - 70 m.
//             Every 20 s per bot. Aimed high for the drop (about d^2 / 1150 m).
//   Gadgets:  hurt and in cover, every 30 s: gadget 1 then gadget 2 next time, aimed at the feet (med / ammo kits).
//   Supplies: (the tester 2026-10-26: "make sure bots drop ammo and healthpacks") out of a fight with teammates close by - one
//             hurt within 12 m, or two or more within 10 m - every 35 - 50 s: the class item (static gadget), gadget 1,
//             gadget 2 in turn, dropped between it and them. Which slot holds the kit depends on the loadout, so all
//             three are tried in turn; a slot with something else in it just fires once at the floor.
enum { GS_NONE, GS_LAUNCHER, GS_GADGET };
typedef struct GearUse { int kind, slot; ULONGLONG at, next, gadgetNext, supplyNext; float to[3]; int supplySlot, onTheMove; } GearUse;
static GearUse g_gear[MAX_BOTS];
static unsigned g_launches, g_gadgetUses, g_supplyDrops, g_farIgnored;
static int gear_tick(int i, Bot *b, void *soldier)
{
    GearUse *g = &g_gear[i];
    if (g->kind == GS_NONE) return 0;
    ULONGLONG t = now_ms(), e = t - g->at;
    const float *p = soldier_pos(soldier);
    // (a supply drop is done on the move: it keeps walking its route, only the view dips to drop the kit at its feet)
    if (!g->onTheMove) b->fwd = b->strafe = 0;
    b->buttons &= ~(BTN_SPRINT | BTN_FIRE | BTN_ZOOM);
    if (e < 200) b->buttons |= g->kind == GS_LAUNCHER ? BTN_GL : g->slot == 1 ? BTN_GADGET1 : g->slot == 2 ? BTN_GADGET2 : BTN_STATIC;
    float eye[3] = { p[0], p[1] + 1.6f, p[2] }, d = dist2d(eye, g->to);
    float wantEl = g->kind == GS_GADGET ? -1.1f : atan2f(g->to[1] + 1.0f + d * d / 1150.0f - eye[1], fmaxf(0.5f, d));
    if (!g->onTheMove) b->turn = turn_input(soldier_yaw(soldier), atan2f(g->to[0] - p[0], g->to[2] - p[2]), 3.0f);
    float el = view_elevation(soldier);
    if (el == el) b->pitch = fmaxf(-1, fminf(1, g_pvUp * (wantEl - el) * 3.0f));
    if (e >= 1300 && e < 1600) b->buttons |= BTN_FIRE;            // the switch takes about a second
    if (e >= 2600 && e < 2800) b->buttons |= BTN_PRIMARY;         // and back to the gun
    if (e >= 3200) { g->kind = GS_NONE; g->onTheMove = 0; }
    b->why = g->kind == GS_LAUNCHER ? "grenade launcher" : "using a gadget";
    return 1;
}
static void launcher_consider(int i, Bot *b, Fight *f, void *soldier, int team, int visible)
{
    GearUse *g = &g_gear[i];
    ULONGLONG t = now_ms();
    if (g->kind != GS_NONE || t < g->next || !f->target || !player_alive_enemy(f->target, team)) return;
    const float *p = soldier_pos(soldier), *q = visible ? soldier_pos(bot_soldier(f->target)) : f->threat;
    float d = dist2d(p, q);
    if (d < 10 || d > 70 || fabsf(q[1] - p[1]) > 10) return;
    const char *why = NULL;
    if (!visible && t - f->seenAt > 300 && t - f->seenAt < 5000) {
        float eye[3] = { p[0], p[1] + 1.6f, p[2] }, chest[3] = { q[0], q[1] + 1.2f, q[2] }, hit[3];
        if (ray_cast(eye, chest, hit, NULL) && dist2d(hit, chest) < 4.0f) why = "behind a wall";
    }
    if (!why && d >= 20) {
        int bunch = 0;
        for (int j = 0; j < g_playerCount; j++)
            if (player_alive_enemy(g_players[j], team) && dist2d(soldier_pos(bot_soldier(g_players[j])), q) < 6) bunch++;
        if (bunch >= 2) why = "enemies bunched up";
    }
    if (!why) return;
    // (mp_hills 2026-10-26: 9 of 18 standing bots were standing for a launcher shot - each stops 3 s. One launch every
    // 60 - 80 s per bot, and at most one a second over all bots.)
    static ULONGLONG lastAny;
    if (t - lastAny < 1000) return;
    lastAny = t;
    g->kind = GS_LAUNCHER; g->at = t; g->next = t + 60000 + (b->seed % 20000);
    memcpy(g->to, q, 12);
    if (g_launches++ < 40) H->log("launcher: %s fires at %.0f m (%s)", (char *)b->player + OFF_PLAYER_NAME, d, why);
}
static void gadget_consider(int i, Bot *b, void *soldier)
{
    GearUse *g = &g_gear[i];
    ULONGLONG t = now_ms();
    if (g->kind != GS_NONE || t < g->gadgetNext) return;
    const float *p = soldier_pos(soldier);
    float yaw = soldier_yaw(soldier);
    g->kind = GS_GADGET; g->at = t; g->gadgetNext = t + 30000; g->slot = g->slot == 1 ? 2 : 1;
    g->to[0] = p[0] + sinf(yaw) * 1.5f; g->to[1] = p[1]; g->to[2] = p[2] + cosf(yaw) * 1.5f;
    if (g_gadgetUses++ < 30) H->log("gadget: %s uses gadget %d in cover", (char *)b->player + OFF_PLAYER_NAME, g->slot);
}

static void supply_consider(int i, Bot *b, void *soldier, int team)
{
    GearUse *g = &g_gear[i];
    ULONGLONG t = now_ms();
    if (g->kind != GS_NONE || t < g->supplyNext) return;
    if (!g->supplyNext) { g->supplyNext = t + 10000 + (b->seed % 20000); return; }   // (not all at once after a map start)
    const float *p = soldier_pos(soldier);
    int close = 0; float hurtHp = 101, to[3] = { 0, 0, 0 };
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        if (pl == b->player || FIELD(pl, OFF_PLAYER_TEAM, int) != team) continue;
        void *s2 = bot_soldier(pl);
        if (!s2) continue;
        const float *q = soldier_pos(s2);
        float d = dist2d(p, q);
        if (d > 12 || fabsf(q[1] - p[1]) > 2.5f) continue;
        float hp = soldier_health(s2);
        if (d < 10) close++;
        if (hp >= 0 && hp < 70 && hp < hurtHp) { hurtHp = hp; memcpy(to, q, 12); }
        else if (!to[0] && !to[2] && d < 10) memcpy(to, q, 12);
    }
    if (!(hurtHp < 70 || close >= 2)) { g->supplyNext = t + 3000; return; }
    g->supplySlot = g->supplySlot % 3 + 1;   // 1 static, 2 gadget 1, 3 gadget 2
    g->slot = g->supplySlot == 1 ? 3 : g->supplySlot - 1;
    g->kind = GS_GADGET; g->at = t; g->supplyNext = t + 35000 + (b->seed % 15000); g->onTheMove = 1;
    float d = dist2d(p, to), f = d > 0.1f ? fminf(1.5f, d * 0.5f) / d : 0;   // between it and them, 1.5 m out at most
    g->to[0] = p[0] + (to[0] - p[0]) * f; g->to[1] = p[1]; g->to[2] = p[2] + (to[2] - p[2]) * f;
    g_supplyDrops++;
    static unsigned logged;
    if (logged++ < 40) H->log("supply: %s drops %s for %d teammate(s) close by%s", (char *)b->player + OFF_PLAYER_NAME,
                              g->slot == 3 ? "its class item" : g->slot == 1 ? "gadget 1" : "gadget 2", close, hurtHp < 70 ? " (one hurt)" : "");
}

static void combat_tick_inner(int i, Bot *b, void *soldier, int team)
{
    if (!g_set.combat) { engage(i, b, soldier, team); return; }
    Fight *f = &g_fight[i];
    ULONGLONG t = now_ms();
    b->action = g_testAction;
    b->enemy = NULL;
    b->pitch = 0;
    if (pc_tick(i, b, soldier)) return;
    if (grenade_tick(i, b, soldier)) return;
    if (gear_tick(i, b, soldier)) return;
    {   // not aiming: bring the view back level (aim_at overrides this)
        float el = view_elevation(soldier);
        if (el == el && fabsf(el) > 0.05f) b->pitch = fmaxf(-0.5f, fminf(0.5f, -g_pvUp * el * 2.0f));
    }
    lock_tick(i, b, f, soldier);

    // Health and hits.
    float hp = soldier_health(soldier);
    f->hp = hp;
    if (hp >= 0 && f->hpPrev >= 0 && hp < f->hpPrev - 0.5f) { f->hurtAt = t; f->hitsTaken++; }
    f->hpPrev = hp;
    int hurt = hp >= 0 && hp < cover_below();
    int justHit = t - f->hurtAt < 1500;

    float d = 0;
    void *enemy = pick_enemy(i, f, soldier, team, &d);
    void *es = enemy ? bot_soldier(enemy) : NULL;
    const float *p = soldier_pos(soldier);

    switch (f->state) {
    case CS_COVER_RUN:
        b->buttons &= ~BTN_FIRE;
        if (dist2d(p, f->goal) < 1.3f) { fight_set(i, f, CS_COVER_HOLD); f->peekNext = t + (ULONGLONG)(P->coverHold * 1000); break; }
        if (t - f->stateAt > 9000) { fight_set(i, f, CS_IDLE); b->npts = 0; break; }   // could not get there
        if (es && d < 10) { fight_set(i, f, CS_FIGHT); break; }   // cornered: fight it out
        if (es && g_set.aggression > 0) { float ye, pe; aim_at(b, f, soldier, es, d, &ye, &pe); b->buttons &= ~BTN_FIRE; }
        b->enemy = enemy;
        return;
    case CS_COVER_HOLD: {
        b->fwd = b->strafe = 0;
        if (es && d < 9) { fight_set(i, f, CS_FIGHT); f->coverTries = 0; break; }
        if (justHit && f->hitsTaken >= 2) {   // the cover is not working
            if (g_set.aggression == 0 && f->coverTries < 3 && go_cover(i, f, soldier, "cover not working")) return;
            fight_set(i, f, CS_FIGHT); break;
        }
        int healed = hp < 0 || hp >= 85;
        if ((healed && t - f->stateAt > 2500) || t - f->stateAt > 15000) {
            fight_set(i, f, CS_IDLE); f->coverTries = 0; b->npts = 0;
            if (g_cStats[1] < 60) H->log("cover: %s leaves cover (health %.0f)", (char *)b->player + OFF_PLAYER_NAME, hp);
            break;
        }
        // Face where the threat was; pop up to shoot now and then.
        float want = atan2f(f->threat[0] - p[0], f->threat[2] - p[2]);
        b->turn = turn_input(soldier_yaw(soldier), want, 2.5f);
        int peeking = t < f->peekEnd;
        if (!peeking && g_set.peek && g_set.aggression > 0 && t >= f->peekNext && hp >= cover_below() * 0.7f) {
            f->peekEnd = t + 1400; f->peekNext = t + 1400 + (ULONGLONG)(P->coverHold * 1000); peeking = 1; g_cStats[3]++;
        }
        if (!peeking && hurt) gadget_consider(i, b, soldier);
        if (!peeking) { b->buttons |= BTN_CROUCH; return; }
        if (es) { float ye, pe; aim_at(b, f, soldier, es, d, &ye, &pe); fire_control(b, f, d, ye, pe, 0); b->enemy = enemy; }
        return;
    }
    case CS_CHASE:
        if (es) { fight_set(i, f, CS_FIGHT); break; }
        if (dist2d(p, f->goal) < 2 || t - f->stateAt > 8000) { fight_set(i, f, CS_IDLE); b->npts = 0; }
        return;
    }

    if (!es) {
        if (f->state == CS_FIGHT) grenade_consider(i, b, f, soldier, team, 0);
        if (f->state == CS_FIGHT) { launcher_consider(i, b, f, soldier, team, 0); if (g_gear[i].kind) return; }
        if (f->state == CS_FIGHT) {
            // Lost sight: aggressive / normal bots go and look where it was (briefly), cautious ones carry on.
            // With flags to take, the flag comes first: only chase someone who was close (12 m), not across the map.
            if (g_set.aggression > 0 && t - f->seenAt < 4000 && f->target && player_alive_enemy(f->target, team) && dist2d(p, f->threat) > 4 &&
                (!conquest_flags() || dist2d(p, f->threat) < 12)) {
                memcpy(f->goal, f->threat, 12); fight_set(i, f, CS_CHASE); g_cStats[5]++; return;
            }
            fight_set(i, f, CS_IDLE);
        }
        // Between fights: reload (a full magazine for the next one rather than an empty click in the middle of it).
        if (f->state == CS_IDLE && f->lastFireAt && f->reloadAt < f->lastFireAt && t - f->lastFireAt > 2500 && t - f->seenAt > 2500 && !justHit) {
            f->reloadAt = t; b->buttons |= BTN_RELOAD;
        }
        // Between fights, teammates round it: drop a kit for them.
        if (f->state == CS_IDLE && !justHit && t - f->seenAt > 4000 && g_set.supplies) supply_consider(i, b, soldier, team);
        // Shot by someone we cannot see: turn to face whoever shot us (the damage handler tells who; else the nearest
        // enemy), look round for 3 s with the whole field of view, and get into cover when hurt or hit again.
        if (justHit) {
            void *who = f->hitBy && player_alive_enemy(f->hitBy, team) ? f->hitBy : NULL;
            float bd = 80;
            for (int j = 0; !who && j < g_playerCount; j++) {
                if (!player_alive_enemy(g_players[j], team)) continue;
                float dd = dist2d(p, soldier_pos(bot_soldier(g_players[j])));
                if (dd < bd) { bd = dd; who = g_players[j]; }
            }
            if (who) {
                const float *q = soldier_pos(bot_soldier(who));
                memcpy(f->threat, q, 12);
                b->turn = turn_input(soldier_yaw(soldier), atan2f(q[0] - p[0], q[2] - p[2]), 3.0f);
                f->alertUntil = t + 3000;
                f->lookAt = 0;   // look again now (pick_enemy) rather than at the next turn of the 120 ms cycle
                if (hurt || f->hitsTaken >= 2) go_cover(i, f, soldier, "shot from out of sight");
            }
        }
        return;
    }

    // On its way to a flag, an enemy far off (over 45 m) and not hitting it does not stop it: it keeps running its route
    // (the tester 2026-10-26: bots "stay still and do nothing" - on Desert 8 of a team stood at their spawn walking backwards at
    // half speed into a wall, aiming at an enemy 140 m behind them, their flag the other way). Ahead of it (within 60 deg
    // of where the route goes) it may still shoot on the move, at full pace.
    if (d > 45 && !justHit && t - f->hurtAt > 4000 && b->targetFlag >= 0 && dist2d(p, b->target) > 15 && b->npts && b->wp < b->npts) {
        const float *w = &b->path[b->wp * 3], *q0 = soldier_pos(es);
        float off = fabsf(wrap_pi(atan2f(q0[0] - p[0], q0[2] - p[2]) - atan2f(w[0] - p[0], w[2] - p[2])));
        if (off > 1.05f) {
            if (f->state == CS_FIGHT) fight_set(i, f, CS_IDLE);
            g_farIgnored++;
            return;
        }
        b->enemy = enemy;
        float ye, pe;
        aim_at(b, f, soldier, es, d, &ye, &pe);
        fire_control(b, f, d, ye, pe, 0);
        return;
    }

    // Fighting.
    b->enemy = enemy;
    if (f->state != CS_FIGHT) { fight_set(i, f, CS_FIGHT); f->hitsTaken = 0; f->tgtHpAtHit = soldier_health(es); }
    const float *q = soldier_pos(es);
    float ye, pe;
    aim_at(b, f, soldier, es, d, &ye, &pe);
    if (d <= P->advanceStop) close_moves(i, b, f, d, p, q);
    else {
        b->fwd *= 0.9f; b->strafe *= 0.9f;   // walking the route while shooting (the tester 2026-10-26: "make them use routes while still fighting" - was 0.75)
        // Under fire at range: side-step while shooting back (a standing target is an easy one).
        if (justHit || t - f->hurtAt < 2500) {
            if (t >= f->dodgeEnd) { unsigned r = (unsigned)(t / 53) * 2654435761u + i * 40503u; f->dodgeDir = (r >> 7) & 1 ? 1 : -1; f->dodgeEnd = t + 500 + (r >> 9) % 600; }
            b->strafe = f->dodgeDir * 0.5f;
        }
    }
    if (d > P->advanceStop && P->farCrouch > 0 && d > P->farCrouch && fabsf(b->fwd) + fabsf(b->strafe) < 0.1f) b->buttons |= BTN_CROUCH;
    int ducking = 0;
    // A low wall / car / crate between it and the enemy within 1.5 m (blocks at 0.6 m, clear at 1.5 m): use it - down
    // behind it, up to shoot over it (1.2 s down, 1.6 s up), still while it's there. Checked every 300 ms. (the tester: "crouch
    // ... to take cover")
    {
        if (t >= f->lowCoverAt) {
            f->lowCoverAt = t + 300;
            float dx = q[0] - p[0], dz = q[2] - p[2], l = sqrtf(dx * dx + dz * dz);
            f->lowCover = 0;
            if (l > 3) {
                dx /= l; dz /= l;
                float a0[3] = { p[0], p[1] + 0.6f, p[2] }, a1[3] = { p[0] + dx * 1.5f, p[1] + 0.6f, p[2] + dz * 1.5f };
                float c0[3] = { p[0], p[1] + 1.5f, p[2] }, c1[3] = { p[0] + dx * 1.5f, p[1] + 1.5f, p[2] + dz * 1.5f };
                f->lowCover = ray_cast(a0, a1, NULL, NULL) && !ray_cast(c0, c1, NULL, NULL);
            }
        }
        if (f->lowCover && fabsf(b->fwd) < 0.5f) {
            b->fwd = 0; b->strafe = 0;
            if ((t / 100) % 28 < 12) { b->buttons |= BTN_CROUCH; ducking = 1; }
        }
    }
    // Past 12 m: aim down the sights and slow down - hip fire on the move spreads too wide to hit (2 hits per 100 firing
    // frames with aim errors under 0.2 m, 2026-10-24).
    // (the tester: "firing from the hip" - sights from 5 m now; was 12 m. Walking while aiming only slows to 70%.)
    if (d > 5 && t - f->firstSeenAt > 150) {
        b->buttons |= BTN_ZOOM; b->buttons &= ~BTN_SPRINT;
        if (d > P->advanceStop) { b->fwd *= 0.85f; if (!(justHit || t - f->hurtAt < 2500)) b->strafe *= 0.85f; }   // (0.7 until 2026-10-26: with 0.75 above, bots fighting crawled along their routes at half speed)
    }
    fire_control(b, f, d, ye, pe, 0);
    if (ducking) b->buttons &= ~(BTN_FIRE | BTN_ZOOM);   // (down behind the cover: not shooting into it)
    grenade_consider(i, b, f, soldier, team, 1);
    launcher_consider(i, b, f, soldier, team, 1);

    // Going badly? (V34 cover_reason): badly hurt; losing the exchange; cautious and outnumbered.
    const char *why = NULL;
    if (hurt && justHit) why = "badly hurt";
    else if (justHit && f->hitsTaken >= 3 && g_set.aggression < 2) {
        float th = soldier_health(es);
        if (th < 0 || th >= f->tgtHpAtHit - 0.5f) why = "losing the exchange";
        else { f->tgtHpAtHit = th; f->hitsTaken = 0; }
    } else if (g_set.aggression == 0 && justHit) {
        int n = 0;
        float eye[3] = { p[0], p[1] + 1.6f, p[2] };
        for (int j = 0; j < g_playerCount && n < 2; j++)
            if (g_players[j] != enemy && player_alive_enemy(g_players[j], team) && dist2d(p, soldier_pos(bot_soldier(g_players[j]))) < 50 &&
                sees(eye, soldier_pos(bot_soldier(g_players[j])))) n++;
        if (n >= 1) why = "outnumbered";
    }
    if (why && !(g_set.aggression == 2 && d < 8)) go_cover(i, f, soldier, why);
}

static void combat_tick(int i, Bot *b, void *soldier, int team)
{
    combat_tick_inner(i, b, soldier, team);
    // Standing to fight, dodging or holding cover is not being stuck: keep brain.inc's progress check quiet.
    Fight *f = &g_fight[i];
    if (g_set.combat && (f->state == CS_COVER_HOLD || f->state == CS_FIGHT || pc_running(i))) {
        b->progressAt = now_ms(); memcpy(b->progressPos, soldier_pos(soldier), 12);
    }
}

// Per frame for every bot (alive or dead), before the bots think: discovery bookkeeping, deaths and spawns.
static void combat_frame(void)
{
    static int loaded;
    if (!loaded) { loaded = 1; settings_load(); }
    engine_load();
    // Sanity check on the "takes input" test: if it says no for most live soldiers it means something else here.
    {
        int raw = 0, no = 0;
        for (int j = 0; j < g_playerCount; j++) { void *s = bot_soldier_raw(g_players[j]); if (s) { raw++; no += !soldier_takes_input(s); } }
        static unsigned badFrames; static int warned;
        if (raw >= 6 && no * 10 > raw * 6) badFrames++; else if (badFrames) badFrames--;
        if (badFrames > 300 && g_downTestOk) { g_downTestOk = 0; if (!warned++) H->log("down test: says no for %d of %d soldiers - switched off (health only)", no, raw); }
        static unsigned at; static int downNow;
        if (H->frame - at > 1800) { at = H->frame; if (no != downNow) { downNow = no; H->log("down test: %d of %d soldier(s) down now", no, raw); } }
    }
    for (int i = 0; i < H->botCount; i++) {
        Fight *f = &g_fight[i];
        void *s = bot_soldier_raw(H->bots[i]);
        static unsigned char wasDead[MAX_BOTS];
        if (s != f->soldier) {
            if (f->soldier && !s) hp_on_death(i);
            memset(f, 0, sizeof *f);
            f->soldier = s;
            f->hpPrev = -1;
            if (s && wasDead[i]) hp_on_spawn(s);   // only a real (re)spawn is at full health
        }
        wasDead[i] = s == NULL;
        if (s) hp_sample(i, s);
        if (s && !soldier_up(s)) { f->state = CS_IDLE; f->target = NULL; }   // down: nothing to fight with
    }
}

static void combat_level_reset(void)
{
    memset(g_fight, 0, sizeof g_fight);
    g_pc.phase = 0; g_pc.bot = -1;
}

// "combat": the settings, what was discovered and each bot's fight state.
static void combat_dump(void)
{
    int live = 0;
    for (int k = 0; k < g_hpcN; k++) live += !g_hpc[k].dead;
    H->log("combat: %s, aggression %d (cover below %.0f), health %s (%d candidate(s) left), pitch %s, since last: %u bursts, %u cover, %u no cover, %u peeks, %u unlocks, %u chases",
           kProfileNames[g_set.difficulty], g_set.aggression, cover_below(), g_hpOff >= 0 ? "found" : "searching", live,
           g_pvOff >= 0 ? "found" : "searching", g_cStats[0], g_cStats[1], g_cStats[2], g_cStats[3], g_cStats[4], g_cStats[5]);
    H->log("combat: bots landed %u hit(s) in %u firing frame(s) (%.1f per 100)", g_botHits, g_fireFrames, g_fireFrames ? g_botHits * 100.0f / g_fireFrames : 0.0f);
    g_botHits = g_fireFrames = 0;
    memset(g_cStats, 0, sizeof g_cStats);
    for (int i = 0; i < H->botCount; i++) {
        Fight *f = &g_fight[i];
        if (!f->soldier) continue;
        H->log("combat: bot %d %s %s, health %.0f, %s%s", i, (char *)H->bots[i] + OFF_PLAYER_NAME, kCsNames[f->state], f->hp,
               f->target ? (char *)f->target + OFF_PLAYER_NAME : "no target", f->locked ? ", HEADING LOCKED" : "");
    }
}
