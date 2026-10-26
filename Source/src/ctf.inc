// Thugbot Capture the Flag (included by brain.inc after the routes). the tester 2026-10-25: "make bots play ... capture the flag".
//
// What the game knows (decompiled, RVAs in this server build):
//   CTF scoring handler onMessage (vtable slot RVA 0x23a4908 -> 0x418e70, `this` is the handler + 0x50) gets
//   ServerGameplayCTFFlag{PickedUp, Dropped, Returned, Captured}Message (type ids at msg+8 / +0xc). PickedUp: msg+0x28 =
//   the ServerPlayer who took it. The others: msg+0x28 = int team index.
//   The handler keeps one record per team index t (1 / 2) at [handler+0x60] + (t-1)*0x2c0: +0 carried (byte),
//   +8 carrier (weak handle: **(rec+8) - 8 = ServerPlayer), +0x2a0 dropped flag position. Record t = the flag team t
//   is after (indexed by the carrier's team, OFF_PLAYER_TEAM).
// What we learn on top (per level, Thugbot_learned\<level>.ctf):
//   base[t] = where team t's own flag sits = where team 3-t picks it up while it is at home, or where team t captures.
//   Until both are known: the centre of each team's spawn spots.
//
// Bots: the carrier runs home. Defenders (DefendShare, at least a fifth): own flag carried -> chase the carrier; dropped
// -> go to it (touching returns it); else hold the base. Attackers: their team's carrier is out -> a third escort him,
// the rest go on; enemy flag dropped -> pick it up; else the enemy base. Own flag carried -> half of the attackers hunt the
// carrier too.

#define CTF_MSG_VT_SLOT 0x23a4908
#define CTF_MSG_FN      0x418e70
typedef void (*CtfOnMsg_t)(void *self, void *msg);
static CtfOnMsg_t o_ctfOnMsg;
static uintptr_t *g_ctfSlot;
static char *g_ctfHandler;                 // CTFScoringHandler (from the last message)
static float g_ctfBase[3][3]; static int g_ctfBaseKnown[3], g_ctfBaseLoaded;
static float g_ctfScanBase[3][3], g_ctfZone[3][3]; static int g_ctfScanFound[3], g_ctfScanned;   // from the game's objects (ctf_scan)
static float g_ctfFlagPos[3][3], g_ctfStillPos[3][3]; static int g_ctfFlagSeen[3]; static ULONGLONG g_ctfFlagAt, g_ctfStillSince[3];   // team t's flag, where it is now (scanned every 500 ms)
static int g_ctfHome[3] = { 0, 0, 0 };   // (unknown after a hot reload; a level load, return or capture sets it)     // per record t: the flag team t is after sits at home (no pickup since return / capture)
static unsigned g_ctfEvents[4];            // picked up, dropped, returned, captured

static void *ctf_weak_player(char *rec)
{
    void **h = *(void ***)(rec + 8);
    if (!h || IsBadReadPtr(h, 8) || !*h) return NULL;
    void *pl = (char *)*h - 8;
    for (int j = 0; j < g_playerCount; j++) if (g_players[j] == pl) return pl;
    return NULL;
}
static char *ctf_rec(int t)
{
    if (!g_ctfHandler || t < 1 || t > 2 || IsBadReadPtr(g_ctfHandler + 0x60, 8)) return NULL;
    char *recs = *(char **)(g_ctfHandler + 0x60);
    if (!recs || IsBadReadPtr(recs + (t - 1) * 0x2c0, 0x2c0)) return NULL;
    return recs + (t - 1) * 0x2c0;
}

static void ctf_path(char *out, size_t n) { CreateDirectoryA("Thugbot_learned", NULL); snprintf(out, n, "Thugbot_learned\\%s.ctf", g_levelKey); }
static void ctf_save(void)
{
    char path[160]; ctf_path(path, sizeof path);
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int t = 1; t <= 2; t++) if (g_ctfBaseKnown[t]) fprintf(f, "base %d %.2f %.2f %.2f\n", t, g_ctfBase[t][0], g_ctfBase[t][1], g_ctfBase[t][2]);
    fclose(f);
}
static void ctf_load(void)
{
    if (g_ctfBaseLoaded || !g_levelKey[0]) return;
    g_ctfBaseLoaded = 1; g_ctfBaseKnown[1] = g_ctfBaseKnown[2] = 0;
    char path[160], line[128]; ctf_path(path, sizeof path);
    FILE *f = fopen(path, "r"); if (!f) return;
    int t; float p[3];
    while (fgets(line, sizeof line, f)) if (sscanf(line, "base %d %f %f %f", &t, &p[0], &p[1], &p[2]) == 4 && t >= 1 && t <= 2) { memcpy(g_ctfBase[t], p, 12); g_ctfBaseKnown[t] = 1; }
    fclose(f);
    H->log("ctf: %s - bases known: team 1 %s, team 2 %s", path, g_ctfBaseKnown[1] ? "yes" : "no", g_ctfBaseKnown[2] ? "yes" : "no");
}
static void ctf_learn_base(int team, const float *p, const char *how)
{
    if (team < 1 || team > 2 || !p || (!p[0] && !p[2])) return;
    if (g_ctfBaseKnown[team] && dist2d(g_ctfBase[team], p) < 6) return;
    memcpy(g_ctfBase[team], p, 12); g_ctfBaseKnown[team] = 1;
    H->log("ctf: team %d's flag base at %.0f %.0f %.0f (%s)", team, p[0], p[1], p[2], how);
    ctf_save();
}
static const float *player_pos(void *pl) { void *s = pl ? bot_soldier(pl) : NULL; return s ? soldier_pos(s) : NULL; }

static void h_ctfOnMsg(void *self, void *msg)
{
    __try {
        g_ctfHandler = (char *)self - 0x50;
        int hi = *(int *)((char *)msg + 8), lo = *(int *)((char *)msg + 0xc);
        if (hi == 0x50a2f3ea) {
            if (lo == (int)0xb6706a29) {          // picked up: msg+0x28 = the player
                void *pl = *(void **)((char *)msg + 0x28);
                int t = pl && !IsBadReadPtr(pl, OFF_PLAYER_TEAM + 4) ? FIELD(pl, OFF_PLAYER_TEAM, int) : 0;
                if (t >= 1 && t <= 2) {
                    g_ctfEvents[0]++;
                    if (g_ctfHome[t] && !(g_ctfScanFound[3 - t] & 2)) ctf_learn_base(3 - t, player_pos(pl), "taken from home");   // (only on maps without the base object)
                    g_ctfHome[t] = 0;
                    H->log("ctf: %s (team %d) has the enemy flag", (char *)pl + OFF_PLAYER_NAME, t);
                }
            } else {
                int t = *(int *)((char *)msg + 0x28);
                if (t >= 1 && t <= 2) {
                    if (lo == (int)0x8056797c) {       // captured: the carrier is home
                        char *rec = ctf_rec(t); void *pl = rec ? ctf_weak_player(rec) : NULL;
                        if (pl && !(g_ctfScanFound[t] & 2)) ctf_learn_base(t, player_pos(pl), "captured there");
                        g_ctfHome[t] = 1; g_ctfEvents[3]++;
                        H->log("ctf: team %d captured", t);
                    } else if (lo == (int)0xc4bb3c17) { g_ctfHome[t] = 1; g_ctfEvents[2]++; H->log("ctf: the flag team %d was after is back home", t); }
                    else if (lo == 0x720bf924) { g_ctfEvents[1]++; H->log("ctf: team %d dropped the flag", t); }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_ctfOnMsg(self, msg);
}

// The game's own way to the CTF objects (from the Dropped handler, RVA 0x4167f0): iterate the entities of the class at
// RVA 0x3a1d8f0 (and its subclasses) in the game realm ([RVA 0x3590a58]); entity = node - 0xe8; kind() (RVA 0xf0bf50)
// 6 with node byte +0x268 bit 3 are the flag objects, team via RVA 0x112b9a0 / 0xf0ac40; transform = vtable +0xd0
// (4x4, position in the last row). "ctfscan" logs every entity of that class; ctf_scan_bases() takes the kind-6 ones as
// each team's base when nothing better is known.
typedef void *(*EntFirst_t)(void *cls, void *realm);
typedef void *(*EntNextCls_t)(void *root, void *cls);
typedef int (*EntInt_t)(void *e);
typedef void (*EntXf_t)(void *e, float *m);
static int ctf_scan(int log, float bases[3][3], int found[3])
{
    int n = 0;
    __try {
        void *root = (void *)(H->image + 0x3a1d8f0), *realm = *(void **)(H->image + 0x3590a58), *cls = root;
        EntFirst_t first = (EntFirst_t)(H->image + 0xd81f60); EntNextCls_t nextCls = (EntNextCls_t)(H->image + 0xd82b60);
        EntInt_t kind = (EntInt_t)(H->image + 0xf0bf50), teamA = (EntInt_t)(H->image + 0x112b9a0), teamB = (EntInt_t)(H->image + 0xf0ac40);
        for (int guard = 0; cls && guard < 64; guard++) {
            for (void **node = (void **)first(cls, realm); node && n < 400; node = (void **)*node) {
                void *e = (char *)node - 0xe8;
                float m[16] = { 0 };
                int k = kind(e), fl = *((unsigned char *)node + 0x268), ta = teamA(e), tb = teamB(e);
                (*(EntXf_t *)(*(char **)e + 0xd0))(e, m);
                n++;
                if (log && n <= 60) H->log("ctfscan: entity %p class %#llx kind %d flags %#x team %d / %d at %.1f %.1f %.1f", e, (unsigned long long)((char *)cls - (char *)H->image), k, fl, ta, tb, m[12], m[13], m[14]);
                if (bases && tb >= 1 && tb <= 2 && (m[12] || m[14])) {
                    // kind 6 (owner team) is the flag stand - the tester took team 1's flag on Snowcrash right on it (-428 207 -91);
                    // kind 10 (class 0x398d3d0) sits 35 m off: the zone, tried by half the attackers until a pickup confirms
                    // kind 6 with node bit 3 and both team getters = t is team t's flag itself (Snowcrash: at -427 206 -91
                    // where the tester picked team 1's up, then moving with its carrier); kind 10 (class 0x398d3d0) sits by each base
                    // Snowcrash, scanned over several minutes: kind 5 (team t / t) never moves and sits where team t's flag is
                    // taken from (-426 206 -92 and -13 197 -48) - the base; kind 6 (t / t) is team t's flag, wherever it is.
                    if (k == 6 && ta == tb) { memcpy(bases[tb], &m[12], 12); found[tb] |= 1; }
                    else if (k == 5 && ta == tb) { memcpy(g_ctfZone[tb], &m[12], 12); found[tb] |= 2; }
                }
            }
            cls = nextCls(root, cls);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { H->log("ctfscan: fault after %d entities", n); return -1; }
    if (log) H->log("ctfscan: %d entities", n);
    return n;
}

static int ctf_scan(int log, float bases[3][3], int found[3]);
static int ctf_mode(void) { return strstr(g_levelKey, "CaptureTheFlag") != NULL; }
static int g_ctfAltPick;   // ctf_base: this bot tries the zone next to the stand (set per call)

// Where team t's flag base is: learned, else the centre of its spawn spots.
static void ctf_scan_flags(void)
{
    ULONGLONG t = now_ms();
    if (!ctf_mode() || t - g_ctfFlagAt < 500) return;
    g_ctfFlagAt = t;
    int found[3] = { 0 };
    if (ctf_scan(0, g_ctfFlagPos, found) < 0) return;
    for (int u = 1; u <= 2; u++) {
        g_ctfFlagSeen[u] = found[u] & 1;
        if (found[u] & 2) { g_ctfScanFound[u] |= 2; }
        if (found[u] & 2) ctf_learn_base(u, g_ctfZone[u], "the game's base object");
    }
}

// Where team t's flag base is: learned, else the object by the base (kind 10), else the centre of its spawn spots.
static int ctf_base(int t, float *out)
{
    if (g_ctfBaseKnown[t]) { memcpy(out, g_ctfBase[t], 12); return 1; }
    if (g_ctfScanFound[t] & 2) { memcpy(out, g_ctfZone[t], 12); if (g_nav && !g_navBuilding) nav_snap(g_nav, out[0], out[1], out[2], out); return 1; }
    float s[3] = { 0 }; int n = 0;
    for (int k = 0; k < g_spawnN; k++) if (g_spawns[k].team == t) { s[0] += g_spawns[k].p[0]; s[1] += g_spawns[k].p[1]; s[2] += g_spawns[k].p[2]; n++; }
    if (!n) return 0;
    out[0] = s[0] / n; out[1] = s[1] / n; out[2] = s[2] / n;
    if (g_nav && !g_navBuilding) nav_snap(g_nav, out[0], out[1], out[2], out);
    return 1;
}

// The state of the flag team t is after: 0 home, 1 carried (*carrier set), 2 dropped (*at set).
static int ctf_state(int t, void **carrier, float *at)
{
    char *rec = ctf_rec(t);
    *carrier = NULL;
    if (!rec) return 0;
    if (rec[0]) { *carrier = ctf_weak_player(rec); return *carrier ? 1 : 0; }
    if (!g_ctfHome[t]) { const float *d = (const float *)(rec + 0x2a0); if (d[0] || d[2]) { memcpy(at, d, 12); return 2; } }
    return 0;
}

static int ctf_target(int i, Bot *b, int team, const float *pos)
{
    (void)i;
    if (!ctf_mode() || team < 1 || team > 2) return 0;
    ctf_load();
    float mine[3], theirs[3], at[3];
    ctf_scan_flags();
    if (!ctf_base(team, mine) || !ctf_base(3 - team, theirs)) return 0;
    if (g_ctfFlagSeen[3 - team]) memcpy(theirs, g_ctfFlagPos[3 - team], 12);   // the enemy flag where it is right now
    void *ourCarrier, *theirCarrier;
    int ours = ctf_state(team, &ourCarrier, at);              // the enemy flag (we are after it)
    float dropOurs[3]; memcpy(dropOurs, at, 12);
    int their = ctf_state(3 - team, &theirCarrier, at);       // our flag (they are after it)
    const char *why;
    const float *goal = NULL;
    unsigned role = (b->seed >> 5) % 100;
    int defender = role < (unsigned)fmaxf(20, g_set.defendShare * 100);
    if (ourCarrier == b->player) { goal = mine; why = "carrying the flag home"; }
    else if (defender || (their == 1 && role < 60)) {
        if (their == 1 && player_pos(theirCarrier)) { goal = player_pos(theirCarrier); why = "chasing our flag's carrier"; }
        else if (their == 2 || (g_ctfFlagSeen[team] && dist2d(g_ctfFlagPos[team], mine) > 8 && their != 1)) { goal = their == 2 ? at : g_ctfFlagPos[team]; why = "returning our dropped flag"; }
        else if (defender) { goal = mine; why = "defending the flag"; }
    }
    if (!goal) {
        if (ours == 1 && player_pos(ourCarrier) && role % 3 == 0) { goal = player_pos(ourCarrier); why = "escorting our carrier"; }
        else if (ours == 2) { goal = g_ctfFlagSeen[3 - team] ? g_ctfFlagPos[3 - team] : dropOurs; why = "picking up the dropped flag"; }
        else { goal = theirs; why = "going for the enemy flag"; }
    }
    if (dist2d(goal, b->target) > 2) b->npts = dist2d(goal, b->target) > 8 ? 0 : b->npts;
    memcpy(b->target, goal, 12);
    if (goal == mine && defender && dist2d(pos, mine) < 10) {   // holding the base: spread round it
        float a = (float)((b->seed + (unsigned)(now_ms() / 6000)) % 628) / 100.0f, r = 3 + (float)(b->seed % 7);
        float pt[3] = { mine[0] + r * sinf(a), mine[1], mine[2] + r * cosf(a) };
        if (g_nav && !g_navBuilding && nav_snap(g_nav, pt[0], pt[1], pt[2], pt)) memcpy(b->target, pt, 12);
    }
    b->targetFlag = -1;
    b->why = why;
    return 1;
}

static void ctf_level_reset(void) { g_ctfFlagSeen[1] = g_ctfFlagSeen[2] = 0; g_ctfScanFound[1] = g_ctfScanFound[2] = 0; g_ctfScanned = 0; g_ctfBaseLoaded = 0; g_ctfHandler = NULL; g_ctfHome[1] = g_ctfHome[2] = 0; }

static void ctf_dump(void)
{
    void *c; float at[3];
    H->log("ctf: mode %s, handler %p, events: %u pickups, %u drops, %u returns, %u captures", ctf_mode() ? "on" : "off", (void *)g_ctfHandler,
           g_ctfEvents[0], g_ctfEvents[1], g_ctfEvents[2], g_ctfEvents[3]);
    for (int t = 1; t <= 2; t++) {
        float bp[3]; int ok = ctf_base(t, bp);
        int st = ctf_state(t, &c, at);
        H->log("ctf: team %d base %s %.0f %.0f %.0f (its flag now %.0f %.0f %.0f); the flag it is after: %s%s", t, g_ctfBaseKnown[t] ? "(learned)" : (g_ctfScanFound[t] & 2) ? "(object by the base)" : "(spawn guess)", ok ? bp[0] : 0, ok ? bp[1] : 0, ok ? bp[2] : 0, g_ctfFlagPos[t][0], g_ctfFlagPos[t][1], g_ctfFlagPos[t][2],
               st == 1 ? "carried by " : st == 2 ? "dropped" : "home", st == 1 ? (char *)c + OFF_PLAYER_NAME : "");
    }
}
