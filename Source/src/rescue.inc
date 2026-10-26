// Thugbot Rescue / Hostage0 (included by brain.inc after ctf.inc). the tester 2026-10-25: "make bots play Rescue".
//
// The game's objects (same entity walk as ctf_scan, class RVA 0x3a1d8f0 and subclasses), Snowcrash Rescue: two of kind
// 0, team 0 / 0 = the hostages; one of kind 14 (class 0x398d3d0), team 0 / t = the extraction, shown to team t, the
// rescuers. Positions every 500 ms.
//
// Rescuers: a third of the team per hostage (by seed) goes to it and holds E there (untie / pick up); a hostage that has
// left its spot by 3 m is being carried - the rescuer within 2.5 m of it takes it to the extraction, the others escort
// it. The rest hunt. Defenders: 60% guard the hostages (spots 4 - 12 m round them, changed every 8 s), the rest hunt; a
// moving hostage - everyone after it.

#define MAX_HOSTAGES 4
static float g_hostage[MAX_HOSTAGES][3], g_hostageHome[MAX_HOSTAGES][3], g_extract[3];
static int g_hostageN, g_extractTeam, g_rescueSeen;
static ULONGLONG g_rescueAt;
static unsigned g_rescueCarries;

static int rescue_mode(void) { return strstr(g_levelKey, "Hostage") != NULL; }

static void rescue_scan(void)
{
    ULONGLONG t = now_ms();
    if (t - g_rescueAt < 500) return;
    g_rescueAt = t;
    int n = 0, ext = 0;
    __try {
        void *root = (void *)(H->image + 0x3a1d8f0), *realm = *(void **)(H->image + 0x3590a58), *cls = root;
        EntFirst_t first = (EntFirst_t)(H->image + 0xd81f60); EntNextCls_t nextCls = (EntNextCls_t)(H->image + 0xd82b60);
        EntInt_t kind = (EntInt_t)(H->image + 0xf0bf50), teamB = (EntInt_t)(H->image + 0xf0ac40);
        for (int guard = 0; cls && guard < 64; guard++) {
            int seen = 0;
            for (void **node = (void **)first(cls, realm); node && seen < 400; node = (void **)*node, seen++) {
                void *e = (char *)node - 0xe8;
                float m[16] = { 0 };
                int k = kind(e), tb = teamB(e);
                (*(EntXf_t *)(*(char **)e + 0xd0))(e, m);
                if (!m[12] && !m[14]) continue;
                if (k == 0 && n < MAX_HOSTAGES) {
                    // keep the order stable: match to the nearest known hostage
                    int slot = n;
                    for (int h = 0; h < g_hostageN; h++) if (dist2d(g_hostage[h], &m[12]) < 6) { slot = h; break; }
                    if (slot >= g_hostageN) slot = g_hostageN < MAX_HOSTAGES ? g_hostageN : n;
                    memcpy(g_hostage[slot], &m[12], 12);
                    if (slot >= g_hostageN) { memcpy(g_hostageHome[slot], &m[12], 12); g_hostageN = slot + 1; }
                    n++;
                } else if (k == 14 && tb >= 1 && tb <= 2) { memcpy(g_extract, &m[12], 12); g_extractTeam = tb; ext = 1; }
            }
            cls = nextCls(root, cls);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!n) g_hostageN = 0;   // rescued / round over
    if (!g_rescueSeen && (n || ext)) { g_rescueSeen = 1; H->log("rescue: %d hostage(s), extraction %s for team %d", n, ext ? "found" : "not found", g_extractTeam); }
}

// The bot nearest a moved hostage (within 2.5 m) of the rescuing team is carrying it.
static int rescue_carrier_of(int h, int team, void **who)
{
    *who = NULL;
    if (dist2d(g_hostage[h], g_hostageHome[h]) < 3) return 0;
    float best = 2.5f;
    for (int j = 0; j < g_playerCount; j++) {
        if (FIELD(g_players[j], OFF_PLAYER_TEAM, int) != team) continue;
        const float *q = player_pos(g_players[j]);
        if (q && dist2d(q, g_hostage[h]) < best) { best = dist2d(q, g_hostage[h]); *who = g_players[j]; }
    }
    return 1;
}

static int rescue_target(int i, Bot *b, int team, const float *pos)
{
    (void)i;
    if (!rescue_mode() || team < 1 || team > 2) return 0;
    rescue_scan();
    if (!g_hostageN) return 0;   // nothing to do: hunt
    int rescuers = g_extractTeam ? g_extractTeam : 1;
    unsigned role = (b->seed >> 5) % 100;
    ULONGLONG t = now_ms();
    const float *goal = NULL; const char *why = NULL;
    int moving = -1; void *carrier = NULL;
    for (int h = 0; h < g_hostageN; h++) if (rescue_carrier_of(h, rescuers, &carrier)) { moving = h; break; }
    if (team == rescuers) {
        if (moving >= 0 && carrier == b->player && (g_extract[0] || g_extract[2])) {
            goal = g_extract; why = "carrying a hostage out";
            static ULONGLONG lastNote; if (t - lastNote > 10000) { lastNote = t; g_rescueCarries++; H->log("rescue: %s carries a hostage to the extraction", (char *)b->player + OFF_PLAYER_NAME); }
        } else if (moving >= 0 && role < 40) { goal = g_hostage[moving]; why = "escorting the hostage"; }
        else {
            int h = (int)(role / 34) < g_hostageN ? (int)(role / 34) : -1;   // 0-33 hostage 0, 34-67 hostage 1, rest hunt
            if (h >= 0) {
                goal = g_hostage[h]; why = "freeing a hostage";
                if (dist2d(pos, goal) < 2.2f && fabsf(pos[1] - goal[1]) < 2) { b->buttons |= BTN_INTERACT; b->fwd = 0; }
            }
        }
    } else {
        if (moving >= 0) { goal = g_hostage[moving]; why = "stopping the hostage rescue"; }
        else if (role < 60) {
            int h = (int)(b->seed % (unsigned)g_hostageN);
            static float spot[MAX_BOTS][3]; static ULONGLONG spotAt[MAX_BOTS];
            if (t > spotAt[i] || dist2d(spot[i], g_hostage[h]) > 14) {
                float a = (float)((b->seed + (unsigned)(t / 8000)) % 628) / 100.0f, r = 4 + (float)((b->seed >> 3) % 9);
                spot[i][0] = g_hostage[h][0] + r * sinf(a); spot[i][1] = g_hostage[h][1]; spot[i][2] = g_hostage[h][2] + r * cosf(a);
                if (g_nav && !g_navBuilding && !nav_snap(g_nav, spot[i][0], spot[i][1], spot[i][2], spot[i])) memcpy(spot[i], g_hostage[h], 12);
                spotAt[i] = t + 8000;
            }
            goal = spot[i]; why = "guarding a hostage";
        }
    }
    if (!goal) return 0;   // hunt
    if (dist2d(goal, b->target) > 8) b->npts = 0;
    memcpy(b->target, goal, 12);
    b->targetFlag = -1;
    b->why = why;
    return 1;
}

static void rescue_level_reset(void) { g_hostageN = 0; g_rescueSeen = 0; g_extractTeam = 0; memset(g_extract, 0, sizeof g_extract); }

static void rescue_dump(void)
{
    H->log("rescue: mode %s, %d hostage(s), extraction %.0f %.0f %.0f for team %d, carries seen %u", rescue_mode() ? "on" : "off", g_hostageN,
           g_extract[0], g_extract[1], g_extract[2], g_extractTeam, g_rescueCarries);
    for (int h = 0; h < g_hostageN; h++) H->log("rescue: hostage %d at %.0f %.0f %.0f (started at %.0f %.0f %.0f)", h, g_hostage[h][0], g_hostage[h][1], g_hostage[h][2],
                                                g_hostageHome[h][0], g_hostageHome[h][1], g_hostageHome[h][2]);
}
