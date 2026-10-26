// Thugbot objectives (included by brain.inc after combat.inc): where each bot goes when it is not fighting.
//
//   Flags (Conquest, Turf War - any mode with takeable ServerCapturePointEntity flags):
//     defenders (DefendShare of each team, a fixed set of bots) hold the owned flag under the most threat (enemies
//     within 45 m), patrolling inside it; with nothing owned they attack like everyone else.
//     attackers go for flags their team doesn't own: nearest first, spread over the flags (a flag that already has its
//     share of the team's attackers costs more), neutral flags preferred, lightly guarded ones preferred. They stay in
//     the capture area until it turns, then pick again. A bot keeps its flag unless another is clearly better (40 m),
//     so they don't dither between two.
//     team owns every flag: patrol a random one (changes every 30 s per bot).
//   No flags (Team Deathmatch, or modes whose objectives aren't mapped yet - Heist, Blood Money, Hotwire, Crosshair,
//   Rescue): hunt - the nearest live enemy (humans too), spread by a per-bot jitter; with none alive, the learned
//   death heat (where fights happen).
// Returns 1 when it set b->target (and b->targetFlag: flag index, or -1).

typedef struct FlagInfo { int ok, owner; float pos[3], radius; int enemies[3], friends[3], attackers[3]; } FlagInfo;
static FlagInfo g_fi[MAX_CPS];
static unsigned g_fiFrame = ~0u;
static void *g_flagGroundCp[MAX_CPS];
static int g_flagRaw[MAX_CPS], g_flagStable[MAX_CPS], g_flagStableSet[MAX_CPS];
static ULONGLONG g_flagRawAt[MAX_CPS];
static float g_flagGroundY[MAX_CPS];

static int g_flagOwnerSeen[MAX_CPS];
static unsigned g_flagChanges;
static void objective_flags_dump(void);
static void flags_survey(void)
{
    if (g_fiFrame == H->frame) return;
    g_fiFrame = H->frame;
    memset(g_fi, 0, sizeof g_fi);
    for (int f = 0; f < g_cpCount; f++) {
        FlagInfo *fi = &g_fi[f];
        if (!flag_takeable(f)) continue;
        fi->ok = 1;
        // The owner field flaps while a flag is contested (team -> neutral -> team, frame to frame: "flag 2: team 2 ->
        // team 0" a dozen times in a row). Bots re-chose their flag every frame and twitched left / right between two
        // (the tester: "movement is glitching out"). Only a value that holds for 1.5 s counts.
        {
            int raw = FIELD(g_cps[f], OFF_CP_OWNER_TEAM, int);
            ULONGLONG now = now_ms();
            if (raw != g_flagRaw[f]) { g_flagRaw[f] = raw; g_flagRawAt[f] = now; }
            if (now - g_flagRawAt[f] >= 1500 || !g_flagStableSet[f]) { g_flagStable[f] = raw; g_flagStableSet[f] = 1; }
            fi->owner = g_flagStable[f];
        }
        memcpy(fi->pos, (char *)g_cps[f] + OFF_CP_POSITION, 12);
        // The flag's position is the centre of its capture sphere, often metres above the floor (Museum: no grid node
        // within 3 m of it, so no route led there). Use the floor under it: a ray straight down, once per flag.
        if (g_flagGroundCp[f] != g_cps[f]) {
            float a[3] = { fi->pos[0], fi->pos[1] + 1.0f, fi->pos[2] }, b[3] = { fi->pos[0], fi->pos[1] - 40.0f, fi->pos[2] }, h[3];
            g_flagGroundY[f] = ray_cast(a, b, h, NULL) ? h[1] : fi->pos[1];
            if (g_rayCaster) g_flagGroundCp[f] = g_cps[f];
        }
        fi->pos[1] = g_flagGroundY[f];
        fi->radius = FIELD(g_cps[f], OFF_CP_RADIUS, float);
        if (!(fi->radius > 2 && fi->radius < 80)) fi->radius = 15;
        for (int j = 0; j < g_playerCount; j++) {
            int t = FIELD(g_players[j], OFF_PLAYER_TEAM, int);
            void *s = bot_soldier(g_players[j]);
            if (!s || t < 1 || t > 2 || dist2d(soldier_pos(s), fi->pos) > 45) continue;
            fi->friends[t]++;                 // per team: players of team t within 45 m
        }
        fi->enemies[1] = fi->friends[2];
        fi->enemies[2] = fi->friends[1];
        if (fi->owner != g_flagOwnerSeen[f]) {
            if (g_flagChanges++ < 200) H->log("flag %d at %.0f %.0f: team %d -> team %d (players near: team 1 %d, team 2 %d)", f, fi->pos[0], fi->pos[2],
                                              g_flagOwnerSeen[f], fi->owner, fi->friends[1], fi->friends[2]);
            g_flagOwnerSeen[f] = fi->owner;
        }
    }
    for (int i = 0; i < H->botCount; i++) {
        int f = g_bot[i].targetFlag, t = FIELD(H->bots[i], OFF_PLAYER_TEAM, int);
        if (f >= 0 && f < g_cpCount && t >= 1 && t <= 2 && bot_soldier(H->bots[i])) g_fi[f].attackers[t]++;
    }
}

static int team_size(int team)
{
    int n = 0;
    for (int i = 0; i < H->botCount; i++) n += FIELD(H->bots[i], OFF_PLAYER_TEAM, int) == team;
    return n ? n : 1;
}

static int is_defender(Bot *b) { return (int)((b->seed >> 3) % 100) < (int)(g_set.defendShare * 100); }

// Inside the capture area: a new spot within it every few seconds (milling, so a bot doesn't stand in one place).
static void flag_mill(Bot *b, int f, ULONGLONG t, float spread)
{
    FlagInfo *fi = &g_fi[f];
    if (t > b->millAt || b->targetFlag != f) {
        // Spread over the capture area (the tester, The Block: "a lot of bots around each other at a playground" - every bot
        // milled within 6 m of the centre). Best of 8 spots up to 12 m out: furthest from other players and the spots
        // teammates already picked, a little bonus next to a wall / object (cover).
        float rmax = fminf(fi->radius * spread, 12.0f), best[3], bestScore = -1e9f;
        memcpy(best, fi->pos, 12);
        for (int k = 0; k < 8; k++) {
            unsigned rnd = b->seed * 2654435761u ^ (unsigned)(t / 7) * 40503u ^ (unsigned)k * 97u;
            float r = rmax * sqrtf((float)(rnd % 1000) / 1000.0f), a = (float)((rnd >> 10) % 628) / 100.0f;
            float pt[3] = { fi->pos[0] + r * sinf(a), fi->pos[1], fi->pos[2] + r * cosf(a) };
            if (g_nav && !g_navBuilding && !nav_snap(g_nav, pt[0], pt[1], pt[2], pt)) continue;
            // Close to the flag on another floor (Desert 2026-10-26: a knot of bots on the ground under flag D, its spots
            // snapped onto a roof 5 m up that the planner could not reach - 120 000-node searches, every bot "waiting for
            // a route" a few metres from the flag): the same spot on the bot's own floor, while that is still within
            // reach of the capture area (7 m of height).
            if (g_nav && !g_navBuilding && dist2d(b->lastPos, fi->pos) < rmax + 15 && fabsf(pt[1] - b->lastPos[1]) > 2.5f) {
                float alt[3];
                if (nav_snap(g_nav, pt[0], b->lastPos[1], pt[2], alt) && fabsf(alt[1] - b->lastPos[1]) < 2.5f && fabsf(alt[1] - fi->pos[1]) < 7) memcpy(pt, alt, 12);
            }
            if (soldier_in_pit(pt)) continue;   // (not down in a pit / an empty pool: mp_hills' D flag had 6 bots stuck in one)
            float nearest = 30;
            for (int j = 0; j < g_playerCount; j++) {
                void *s = bot_soldier(g_players[j]);
                if (s && g_players[j] != b->player) nearest = fminf(nearest, dist2d(soldier_pos(s), pt));
            }
            for (int q = 0; q < H->botCount; q++)
                if (&g_bot[q] != b && g_bot[q].targetFlag == f) nearest = fminf(nearest, dist2d(g_bot[q].target, pt));
            float score = fminf(nearest, 8.0f);
            if (g_nav && !g_navBuilding) { int c = nav_node_clearance(g_nav, pt[0], pt[1], pt[2]); if (c == 1) score += 1.5f; else if (c == 0) score -= 2; }
            if (score > bestScore) { bestScore = score; memcpy(best, pt, 12); }
        }
        memcpy(b->target, best, 12);
        b->millAt = t + 5000 + (b->seed % 4000);
        b->npts = 0;
    }
    b->targetFlag = f;
}

// Flanking: a third of the bots (by seed) attacking a flag over 70 m away first go to a point 35 m to the side of the
// straight line (halfway along, side by seed) - on the grid and not much further from the flag - then on to the flag.
static void flank_target(Bot *b, int f, const float *pos)
{
    ULONGLONG t = now_ms();
    const float *fp = g_fi[f].pos;
    if ((b->seed % 3) != 0) return;
    if (b->flankFlag != f + 1) {   // new flag: work out its flank point (once)
        b->flankFlag = f + 1; b->flankUntil = 0;
        float dx = fp[0] - pos[0], dz = fp[2] - pos[2], d = sqrtf(dx * dx + dz * dz);
        if (d < 70 || !g_nav || g_navBuilding) return;
        float side = (b->seed & 8) ? 1.0f : -1.0f, mx = pos[0] + dx * 0.55f, mz = pos[2] + dz * 0.55f;
        float px = mx - dz / d * 35 * side, pz = mz + dx / d * 35 * side, out[3];
        if (!nav_snap(g_nav, px, pos[1], pz, out) && !nav_snap(g_nav, px, NAN, pz, out)) return;
        memcpy(b->flank, out, 12); b->flankUntil = t + 45000;
    }
    if (!b->flankUntil || t > b->flankUntil) return;
    if (dist2d(pos, b->flank) < 10) { b->flankUntil = 0; b->npts = 0; return; }   // got there: on to the flag
    memcpy(b->target, b->flank, 12);
}

static int objective_flags(int i, Bot *b, int team, const float *pos)
{
    (void)i;
    ULONGLONG t = now_ms();
    flags_survey();
    int owned = 0, total = 0;
    for (int f = 0; f < g_cpCount; f++) if (g_fi[f].ok) { total++; owned += g_fi[f].owner == team; }
    if (!total) return 0;

    // Defend: the owned flag under most threat (ties: nearest).
    if (owned && owned < total && is_defender(b)) {
        int best = -1; float bestScore = 1e9f;
        for (int f = 0; f < g_cpCount; f++) {
            if (!g_fi[f].ok || g_fi[f].owner != team) continue;
            float score = dist2d(pos, g_fi[f].pos) * 0.2f - g_fi[f].enemies[team] * 40.0f + (f == b->targetFlag ? -15 : 0);
            if (score < bestScore) { bestScore = score; best = f; }
        }
        if (best >= 0 && b == g_stTraceBot && g_stTraceFrames > 0) H->log("steertrace: defend -> flag %d (owned %d of %d)", best, owned, total);
        if (best >= 0) {
            if (dist2d(pos, g_fi[best].pos) < g_fi[best].radius * 0.8f) flag_mill(b, best, t, 0.6f);
            else { if (b->targetFlag != best) b->npts = 0; b->targetFlag = best; memcpy(b->target, g_fi[best].pos, 12); }
            return 1;
        }
    }

    // Attack.
    if (owned < total) {
        // (a third of the team per flag at most before it costs: with 30 bots and two flags to take, half the team used
        // to pile onto each - the tester's playground crowd)
        int share = team_size(team) / (total - owned) + 1, cap = team_size(team) / 3 + 1;
        if (share > cap) share = cap;
        int best = -1, cur = -1; float bestScore = 1e9f, curScore = 1e9f;
        for (int f = 0; f < g_cpCount; f++) {
            FlagInfo *fi = &g_fi[f];
            if (!fi->ok || fi->owner == team) continue;
            if (f == b->avoidFlag && now_ms() < b->avoidUntil && total - owned > 1) continue;   // the watchdog gave up on it
            // "mine": the flag it chose last time it attacked (attackFlag, which nothing else resets). targetFlag is reset
            // in several places (spawn, the watchdog, hunting, cover); with it the bot's own vote on its old flag counted
            // against that flag, so it flipped between two flags every frame (Downtown, 63 bots: 8 flips a second).
            int mine = b->attackFlag == f + 1;
            int others = fi->attackers[team] - (b->targetFlag == f);
            float score = dist2d(pos, fi->pos) + (float)((b->seed * 2654435761u + f * 40503u) % 40)
                        + (others >= share ? 35.0f * (others - share + 1) : 0)
                        + (fi->owner == 0 ? -25.0f : 0)
                        + fi->enemies[team] * 6.0f
                        + (target_dead(fi->pos) ? 400.0f : 0);   // no route there right now (a rooftop flag only an elevator reaches): the others first
            if (mine) { cur = f; curScore = score; }
            if (score < bestScore) { bestScore = score; best = f; }
        }
        if (cur >= 0 && curScore < bestScore + 40) best = cur;   // keep the flag we're going for
        // Decide at most every 4 - 6 s per bot (staggered): deciding every frame on the previous frame's head-count, a
        // whole team of 28 swung from one flag to another and back 30 times a second (each saw the other flag empty).
        if (cur >= 0 && t < b->attackChooseAt) best = cur;
        else if (best >= 0) b->attackChooseAt = t + 4000 + (b->seed % 2000);
        if (best >= 0) b->attackFlag = best + 1;
        if (b == g_stTraceBot && g_stTraceFrames > 0) {
            char line[256]; int n = snprintf(line, sizeof line, "steertrace: attack -> flag %d (cur %d, owned %d of %d):", best, cur, owned, total);
            for (int f = 0; f < g_cpCount && n < 200; f++) if (g_fi[f].ok && g_fi[f].owner != team)
                n += snprintf(line + n, sizeof line - n, " f%d att%d en%d own%d", f, g_fi[f].attackers[team], g_fi[f].enemies[team], g_fi[f].owner);
            H->log("%s", line);
        }
        if (best >= 0) {
            if (dist2d(pos, g_fi[best].pos) < g_fi[best].radius * 0.6f) flag_mill(b, best, t, 0.45f);   // capturing: stay in
            else { if (b->targetFlag != best) b->npts = 0; b->targetFlag = best; memcpy(b->target, g_fi[best].pos, 12); flank_target(b, best, pos); }
            return 1;
        }
    }

    // Everything is ours: patrol.
    int k = (int)((b->seed + t / 30000) % (unsigned)total);
    for (int f = 0; f < g_cpCount; f++) if (g_fi[f].ok && k-- == 0) {
        if (dist2d(pos, g_fi[f].pos) < g_fi[f].radius) flag_mill(b, f, t, 0.8f);
        else { if (b->targetFlag != f) b->npts = 0; b->targetFlag = f; memcpy(b->target, g_fi[f].pos, 12); }
        return 1;
    }
    return 0;
}

static int objective_hunt(int i, Bot *b, int team, const float *pos)
{
    (void)i;
    float bestScore = 1e9f; int found = 0;
    for (int j = 0; j < g_playerCount; j++) {
        if (!player_alive_enemy(g_players[j], team)) continue;
        const float *q = soldier_pos(bot_soldier(g_players[j]));
        float score = dist2d(pos, q) + (float)((b->seed * 2654435761u + (unsigned)j * 40503u) % 50);
        if (score < bestScore) { bestScore = score; memcpy(b->target, q, 12); found = 1; }
    }
    if (!found && g_learn && g_learn->nHeat) {   // nobody alive: where the fighting usually is
        int k = (int)((b->seed + now_ms() / 20000) % (unsigned)g_learn->nHeat);
        b->target[0] = g_learn->heat[k].cx * 10.0f + 5; b->target[2] = g_learn->heat[k].cz * 10.0f + 5; b->target[1] = pos[1];
        found = 1;
    }
    if (b->targetFlag != -1) b->npts = 0;
    b->targetFlag = -1;
    return found;
}

static int camp_target(int i, Bot *b, int team, const float *pos);   // camp.inc
static int ctf_target(int i, Bot *b, int team, const float *pos);    // ctf.inc
static int rescue_target(int i, Bot *b, int team, const float *pos); // rescue.inc
// brain.inc's choose_target asks here first.
static int objective_target(int i, Bot *b, int team, const float *pos)
{
    b->objRoute = 0;
    if (combat_target(i, b)) return 1;
    if (ctf_target(i, b, team, pos)) { b->objRoute = 1; return 1; }
    if (rescue_target(i, b, team, pos)) { b->objRoute = 1; return 1; }
    if (conquest_flags()) { flags_survey(); if (camp_target(i, b, team, pos)) return 1; return objective_flags(i, b, team, pos); }
    return objective_hunt(i, b, team, pos);
}

// "bots" adds a line per flag: owner, who is near, and how many bots of each team are going for it.
static void objective_flags_dump(void)
{
    flags_survey();
    for (int f = 0; f < g_cpCount; f++) {
        FlagInfo *fi = &g_fi[f];
        if (!fi->ok) continue;
        H->log("flag %d at %.0f %.0f (r %.0f): owner team %d, near: team 1 %d / team 2 %d, heading there: team 1 %d / team 2 %d", f, fi->pos[0], fi->pos[2],
               fi->radius, fi->owner, fi->friends[1], fi->friends[2], fi->attackers[1], fi->attackers[2]);
    }
}
