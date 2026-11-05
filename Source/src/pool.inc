// Thugbot pools (included by brain.inc after parachute.inc): a bot in water that isn't getting anywhere swims for the
// nearest rim (dry ground 0.2 - 2.2 m above it, next to the water or the pit's floor, within 12 m) and jumps / vaults out there. Pools on
// Hollywood Heights have walls the planner can't step up, so bots swam in circles (the tester, 2026-10-25).

// rim spots that failed (15 s and still in) this level: avoided next time
static int g_vaultHigh = 1;   // (vaults are carried, vault_hook.inc: docks and ledges up to 1.4 m over the water count as a way out)
static float g_rimFail[32][2]; static int g_rimFailN;
static int b_failNearR(float x, float z, float r) { for (int k = 0; k < g_rimFailN && k < 32; k++) if (fabsf(g_rimFail[k][0] - x) < r && fabsf(g_rimFail[k][1] - z) < r) return 1; return 0; }
static int b_failNear(float x, float z) { for (int k = 0; k < g_rimFailN && k < 32; k++) if (fabsf(g_rimFail[k][0] - x) < 2.5f && fabsf(g_rimFail[k][1] - z) < 2.5f) return 1; return 0; }
static int pool_rim_scan(const float *pos, float *out, int shore, const float *goal);
// BF4 (the tester 2026-10-27: "they can swim to shore but no, they try to magically get on the dock"): out of the water only
// where it is low - a beach, a ramp, a step 0.9 m or less over the surface (every swim vault at 1.1 m+ was put back).
// Up to 45 m away, the one that leaves the least walk to the target. Only with none of those the old nearest rim.
static int pool_rim(const float *pos, float *out) { return pool_rim_scan(pos, out, 1, NULL) || pool_rim_scan(pos, out, 0, NULL); }
static int pool_rim_goal(const float *pos, float *out, const float *goal) { return pool_rim_scan(pos, out, 1, goal) || pool_rim_scan(pos, out, 0, goal); }
static int pool_rim_scan(const float *pos, float *out, int shore, const float *goal)
{
    int wetHere = g_nav && nav_is_water(g_nav, pos[0], pos[1], pos[2]);
    NavGrid *g = g_nav;
    int c = nav_cell_index(g, pos[0], pos[2]);
    if (c < 0) return 0;
    if (shore && !wetHere) return 0;
    float surf = pos[1];   // the water surface here (the grid's water node): the bank must be level with it, not with the feet
    for (int l = 0; l < NAV_LAYERS; l++) { int n = c * NAV_LAYERS + l; if ((g->flags[n] & NAV_WATER) && fabsf(g->height[n] - pos[1]) < 2.0f) { surf = g->height[n]; break; } }
    int cx = c % g->w, cz = c / g->w, r = (int)((shore ? 45 : 12) / g->cell) + 1, found = 0;
    float best = 1e9f;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= g->w || z >= g->h) continue;
            for (int l = 0; l < NAV_LAYERS; l++) {
                int node = (z * g->w + x) * NAV_LAYERS + l;
                if (!(g->flags[node] & NAV_WALKABLE) || (g->flags[node] & NAV_WATER)) continue;
                float y = g->height[node];
                if (shore ? (y < surf - 0.3f || y > surf + (g_vaultHigh ? 1.4f : 0.3f)) : (y < pos[1] + 0.2f || y > pos[1] + 2.2f)) continue;
                if (shore && (x - cx) * (x - cx) + (z - cz) * (z - cz) < 9 / (g->cell * g->cell)) continue;   // (at least 3 m on: the spot it stands at already got it nowhere)
                int wet = 0;
                for (int d = 0; d < 8 && !wet; d++) { int nb = nav_step_node(g, node, d); if (nb >= 0 && (g->flags[nb] & NAV_WATER)) wet = 1; }
                if (!wet) {   // (the step links may not join a wall top to the water: look at the cells around too)
                    for (int d2 = 0; d2 < 4 && !wet; d2++) {
                        int x2 = x + (d2 == 0) - (d2 == 1), z2 = z + (d2 == 2) - (d2 == 3);
                        if (x2 < 0 || z2 < 0 || x2 >= g->w || z2 >= g->h) continue;
                        for (int l2 = 0; l2 < NAV_LAYERS; l2++) { int n2 = (z2 * g->w + x2) * NAV_LAYERS + l2; if (((g->flags[n2] & NAV_WATER) && fabsf(g->height[n2] - pos[1]) < 2) || ((g->flags[n2] & NAV_WALKABLE) && fabsf(g->height[n2] - pos[1]) < 0.4f)) wet = 1; }   // (or the pit's own floor)
                    }
                }
                if (!wet) continue;
                // nearest, but a low rim counts much nearer: 2 m walls can't be climbed (a pit at 158 298 3 on Hollywood
                // Heights: every try at its 2 m rim failed), 1.5 m and less vault
                float px = g->x0 + x * g->cell, pz = g->z0 + z * g->cell, dh = y - pos[1];
                float d = sqrtf((px - pos[0]) * (px - pos[0]) + (pz - pos[2]) * (pz - pos[2])) + (dh > 1.5f ? 30 : dh > 1.1f ? 6 : 0) + (wetHere && dh > 0.7f ? 25 : 0) + b_failNear(px, pz) * 40;   // (out of water: a beach / a low step - a jump does nothing while swimming)
                if (shore) {   // a real way up: walkable land goes on inland at about that height (not a lone post / pier edge)
                    int inland = 0;
                    for (int d2 = 0; d2 < 8 && !inland; d2++) { int nb = nav_step_node(g, node, d2); if (nb >= 0 && !(g->flags[nb] & NAV_WATER) && (g->flags[nb] & NAV_WALKABLE)) { int nb2 = nav_step_node(g, nb, d2); if (nb2 >= 0 && !(g->flags[nb2] & NAV_WATER) && (g->flags[nb2] & NAV_WALKABLE)) inland = 1; } }
                    if (!inland) continue;
                    d = sqrtf((px - pos[0]) * (px - pos[0]) + (pz - pos[2]) * (pz - pos[2])) + b_failNearR(px, pz, 8) * 80;   // (a failed bank: 8 m of it, not the next cell along it)
                    if (goal) d += 0.3f * sqrtf((px - goal[0]) * (px - goal[0]) + (pz - goal[2]) * (pz - goal[2]));
                }
                if (d < best) { best = d; out[0] = px; out[1] = y; out[2] = pz; found = 1; }
            }
        }
    return found;
}

// Under a wide flat sheet 0.3 - 1.6 m above its feet (the same surface 2.5 m away on all four sides): no one can
// stand there - it is swimming. (Doesn't depend on the grid's water flags: pools' surface class differs per map.)
static int soldier_in_water(const float *p)
{
    float a[3] = { p[0], p[1] + 2, p[2] }, e[3] = { p[0], p[1] - 1, p[2] }, h[3];
    if (!ray_cast(a, e, h, NULL) || h[1] - p[1] < 0.3f || h[1] - p[1] > 1.6f) return 0;
    for (int d = 0; d < 4; d++) {
        float a2[3] = { p[0] + (d == 0 ? 2.5f : d == 1 ? -2.5f : 0), h[1] + 1.0f, p[2] + (d == 2 ? 2.5f : d == 3 ? -2.5f : 0) }, b2[3] = { a2[0], h[1] - 0.5f, a2[2] }, h2[3];
        if (!ray_cast(a2, b2, h2, NULL) || fabsf(h2[1] - h[1]) > 0.08f) return 0;
    }
    return 1;
}

// A pit: an empty pool, a ditch, a sunken yard - the ground 3 m away is 0.5 m+ higher in at least 5 of 8 directions.
static int soldier_in_pit(const float *p)
{
    int up = 0;
    for (int d = 0; d < 8; d++) {
        float a = d * 0.785398f, x = p[0] + sinf(a) * 3, z = p[2] + cosf(a) * 3;
        float s0[3] = { x, p[1] + 3.0f, z }, e[3] = { x, p[1] - 1, z }, h[3];
        if (ray_cast(s0, e, h, NULL) && h[1] > p[1] + 0.5f && h[1] < p[1] + 2.9f) up++;
    }
    return up >= 5;
}

// Pits and pools bots can't get out of (the tester, mp_hills 2026-10-26: "near C there is this half empty pool. They cannot
// vault over the ledges" - both teams had a knot of bots in a pool or pit there): once a bot has been stuck in one, the
// ways down into it are blocked in the grid - every rim node (0.6 - 3 m above the floor, within 12 m) loses its links
// that drop onto the floor - so routes go round it instead of through it. Kept for the level, put back every 2.5 min
// (grid blocks expire after 3).
#define MAX_PITS 16
static struct { float p[3]; } g_pit[MAX_PITS]; static int g_pitN; static ULONGLONG g_pitAt; static unsigned g_pitSerial;
static int pit_block(const float *c)
{
    NavGrid *g = g_nav;
    if (!g || g_navBuilding) return 0;
    int cell = nav_cell_index(g, c[0], c[2]);
    if (cell < 0) return 0;
    int cx = cell % g->w, cz = cell / g->w, r = (int)(12 / g->cell) + 1, n = 0;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= g->w || z >= g->h) continue;
            for (int l = 0; l < NAV_LAYERS; l++) {
                int node = (z * g->w + x) * NAV_LAYERS + l;
                if (!(g->flags[node] & NAV_WALKABLE)) continue;
                float y = g->height[node];
                if (y < c[1] + 0.6f || y > c[1] + 3.0f) continue;
                for (int d = 0; d < 8; d++) {
                    int nb = nav_step_node(g, node, d);
                    if (nb < 0 || g->height[nb] > c[1] + 0.4f) continue;
                    float a[3], q[3]; nav_node_pos(g, node, a); nav_node_pos(g, nb, q);
                    n += nav_block_towards(g, a[0], a[1], a[2], q[0], q[2]);
                }
            }
        }
    return n;
}
static void pit_note(const float *pos)
{
    for (int k = 0; k < g_pitN; k++) if (dist2d(g_pit[k].p, pos) < 10 && fabsf(g_pit[k].p[1] - pos[1]) < 1.5f) return;
    if (g_pitN >= MAX_PITS) return;
    memcpy(g_pit[g_pitN++].p, pos, 12);
    int n = pit_block(pos);
    H->log("pool: no way into the pit / pool at %.0f %.0f %.0f any more - %d drop link(s) into it blocked", pos[0], pos[1], pos[2], n);
}
// False floors: grid nodes 0.8 - 2.5 m above a soldier standing in the same cell, with no node at the soldier's own
// height (mp_hills' pools: the grid took the water surface / a cover for a floor at deck height, so routes went
// straight across the pool and bots dropped in). Every link into such a node is blocked (re-applied with the pits).
#define MAX_FALSE 512
static int g_falseNode[MAX_FALSE], g_falseN;
static void false_floor_block(int node)
{
    NavGrid *g = g_nav; float q[3]; nav_node_pos(g, node, q);
    for (int d = 0; d < 8; d++) {
        int nb = nav_step_node(g, node, d);
        if (nb < 0) continue;
        float a[3]; nav_node_pos(g, nb, a);
        nav_block_towards(g, a[0], a[1], a[2], q[0], q[2]);
    }
}
static void false_floor_check(const float *pos)
{
    NavGrid *g = g_nav;
    int cell = nav_cell_index(g, pos[0], pos[2]);
    if (cell < 0) return;
    int own = 0, over = -1;
    for (int l = 0; l < NAV_LAYERS; l++) {
        int node = cell * NAV_LAYERS + l;
        if (!(g->flags[node] & NAV_WALKABLE)) continue;
        float dy = g->height[node] - pos[1];
        if (fabsf(dy) < 0.6f) own = 1;
        else if (dy > 0.8f && dy < 2.5f) over = node;
    }
    if (own || over < 0) return;
    for (int k = 0; k < g_falseN; k++) if (g_falseNode[k] == over) return;
    if (g_falseN >= MAX_FALSE) return;
    g_falseNode[g_falseN++] = over;
    false_floor_block(over);
    static unsigned logged;
    if (logged++ < 20) H->log("grid: false floor %.1f m over a soldier at %.0f %.0f %.0f (a pool / pit the grid didn't see) - no way across it", g->height[over] - pos[1], pos[0], pos[1], pos[2]);
}

static void pit_tick(ULONGLONG t)   // (from pool_exit's callers: brain_tick)
{
    if (g_nav && !g_navBuilding && g_pitSerial == g_nav->serial) {   // soldiers on the ground under the grid's floor, every 500 ms
        static ULONGLONG ffAt;
        if (t - ffAt >= 500) {
            ffAt = t;
            for (int j = 0; j < g_playerCount; j++) {
                void *s2 = bot_soldier(g_players[j]);
                if (!s2) continue;
                const float *q = soldier_pos(s2), *v = (const float *)((char *)s2 + OFF_SOLDIER_VELOCITY);
                if (fabsf(v[1]) > 0.5f) continue;   // (jumping / falling)
                if (air_below(q) > 0.4f) continue;   // (on the ground)
                if (soldier_in_water(q) || soldier_in_pit(q)) false_floor_check(q);   // (only pools / pits: a floor the grid missed under a walkway must not cut the walkway)
            }
        }
    }
    if (!g_nav || g_navBuilding) return;
    if (g_pitSerial != g_nav->serial) { g_pitSerial = g_nav->serial; g_pitN = 0; g_falseN = 0; g_pitAt = t; return; }   // (a new level)
    if (t - g_pitAt < 150000) return;
    g_pitAt = t;
    for (int k = 0; k < g_pitN; k++) pit_block(g_pit[k].p);
    for (int k = 0; k < g_falseN; k++) false_floor_block(g_falseNode[k]);
}

// Learned ways out of the water (the tester 2026-10-30, Siege: "they swim like ... there are stairs near C - make them use the
// stairs at all times when in the water"): every shore spot a swimmer really got out at is kept (Thugbot_learned// <map>.exits) and a swimmer heads for the nearest of those within 150 m first, the shore search only when there is none.
#define MAX_EXITS 64
// (the tester 2026-10-31: ways out placed by hand in the 3D map were overwritten) <map>.exits is the hand-placed list (3D map tool;
// whatever was in it before is kept as well) and is never written here; what swimmers learn goes to <map>.exits.learned,
// whose oldest entries make room. Both are read again when the .exits file changes (a save in the 3D map mid-round).
static float g_exits[MAX_EXITS][3]; static int g_exitN, g_exitPinN, g_exitNext; static char g_exitKey[96]; static FILETIME g_exitStamp;
static int exits_read(const char *path, int from)
{
    FILE *f = fopen(path, "r"); int n = from;
    if (!f) return n;
    char line[160];
    while (n < MAX_EXITS && fgets(line, sizeof line, f)) if (sscanf(line, "%f %f %f", &g_exits[n][0], &g_exits[n][1], &g_exits[n][2]) == 3) n++;
    fclose(f);
    return n;
}
static void exits_load(void)
{
    char path[MAX_PATH], lp[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".exits"); map_path(lp, sizeof lp, "Thugbot_learned", ".exits.learned");
    WIN32_FILE_ATTRIBUTE_DATA fa; FILETIME stamp = { 0, 0 };
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) stamp = fa.ftLastWriteTime;
    if (!strcmp(g_exitKey, g_levelKey) && !CompareFileTime(&stamp, &g_exitStamp)) return;
    snprintf(g_exitKey, sizeof g_exitKey, "%s", g_levelKey); g_exitStamp = stamp;
    g_exitPinN = exits_read(path, 0);
    g_exitN = exits_read(lp, g_exitPinN);
    H->log("pool: %d way(s) out of the water placed by hand, %d learned, on %s", g_exitPinN, g_exitN - g_exitPinN, g_levelKey);
}
static void exits_note(const float *p)
{
    exits_load();
    for (int k = 0; k < g_exitN; k++) if (dist2d(g_exits[k], p) < 4 && fabsf(g_exits[k][1] - p[1]) < 2) return;
    if (g_exitPinN >= MAX_EXITS) return;   // (all 64 placed by hand: nothing to learn into)
    int k = g_exitN < MAX_EXITS ? g_exitN++ : g_exitPinN + g_exitNext++ % (MAX_EXITS - g_exitPinN);   // (full: the learned ones in turn, never a placed one)
    memcpy(g_exits[k], p, 12);
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".exits.learned");
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int q = g_exitPinN; q < g_exitN; q++) fprintf(f, "%.1f %.1f %.1f\n", g_exits[q][0], g_exits[q][1], g_exits[q][2]);
    fclose(f);
    H->log("pool: way out of the water at %.0f %.0f %.0f learned (%d learned, %d placed on this map)", p[0], p[1], p[2], g_exitN - g_exitPinN, g_exitPinN);
}
static int pool_pick(const float *pos, float *out, const float *goal)
{
    exits_load();
    int best = -1; float bd = 150;
    for (int k = 0; k < g_exitN; k++) { float d = dist2d(pos, g_exits[k]); if (d < bd && !b_failNear(g_exits[k][0], g_exits[k][2])) { bd = d; best = k; } }
    if (best >= 0) { memcpy(out, g_exits[best], 12); return 1; }
    return pool_rim_goal(pos, out, goal);
}
// Over the rim on the vault carry (vault_hook.inc), from the water or a pit: the landing is the rim spot itself, and the bot
// is lifted whatever the game's vault makes of it (the tester 2026-11-05: docks vaultable, out of the Hollywood Heights pool and pit).
static void pool_carry(Bot *b, const float *pos, ULONGLONG t)
{
    MoveX *m = &g_mx[b - g_bot];
    float up = b->poolRim[1] - pos[1], d = dist2d(pos, b->poolRim);
    if (t < m->vaultUntil || up > 2.4f || d > 4.0f || d < 0.2f) return;
    memcpy(m->vaultLand, b->poolRim, 12); m->jumpTop = fmaxf(0, up); m->jumpFrom = t; m->jumpUntil = t + 300; m->vaultUntil = t + 1300;
    m->blind = 1; m->waterJump = 0; memcpy(m->vaultFrom, pos, 12); m->vaultTop = pos[1];
    m->vaultDir[0] = (b->poolRim[0] - pos[0]) / d; m->vaultDir[1] = (b->poolRim[2] - pos[2]) / d;
    extern_vault_window(m->vaultUntil);
}
// Really in the water: the feet under the surface. A bot standing on a dock is over a water cell as well (Offshore: bots on
// a pier at y 71, water at 70, "stuck in water" every second, "climbing out" to spots below their feet).
static int pool_swimming(const float *pos)
{
    NavGrid *g = g_nav;
    if (!nav_is_water(g, pos[0], pos[1], pos[2])) return 0;
    int c = nav_cell_index(g, pos[0], pos[2]);
    if (c < 0) return 0;
    for (int l = 0; l < NAV_LAYERS; l++) { int n = c * NAV_LAYERS + l; if ((g->flags[n] & NAV_WATER) && fabsf(g->height[n] - pos[1]) < 3.0f) return pos[1] < g->height[n] + 0.1f; }
    return 1;
}
static int pool_exit(Bot *b, void *soldier, const float *pos, ULONGLONG t)
{
    if (!g_nav || g_navBuilding) return 0;
    if (!b->poolSince) {
        int inW0 = pool_swimming(pos);
        if (t < b->poolCool && !inW0) return 0;   // (just got out: no new look for 4 s - the 45 m shore search is not for every frame)
        if (inW0 && t < b->poolCool - 3000) return 0;   // (in water: at most one new search a second)
        // stuck (under 2 m in 2.5 s) and in water / a pit - or in a pit at all with its way going up out of it (a crowd
        // stepping round each other kept resetting "stuck": the D pit on mp_hills held 6 bots, the tester 2026-10-26)
        int stuck = t - b->progressAt >= 2500 && dist2d(pos, b->progressPos) <= 2;
        int inWater = inW0;
        const float *w0 = b->npts && b->wp < b->npts ? &b->path[b->wp * 3] : b->target;
        int upAhead = inWater && w0[1] > pos[1] + 0.9f && dist2d(pos, w0) < 25;   // (its way goes up a dock / wall it can't climb from the water)
        int slow = inWater && t - b->progressAt >= 1500 && dist2d(pos, b->progressPos) <= 2;
        int wet = inWater || upAhead || slow || (stuck && soldier_in_water(pos));   // (in the water at all: one steerer only - the route's heading and the shore's took turns and swimmers spun, steerraw 2026-10-27)
        int pit = 0;
        if (!wet && t >= b->pitLookAt) {
            b->pitLookAt = t + 1000;
            const float *w = b->npts && b->wp < b->npts ? &b->path[b->wp * 3] : b->target;
            if ((stuck && t - b->progressAt >= 5000) || (w[1] > pos[1] + 0.6f && dist2d(pos, w) < 8 && t - b->progressAt >= 1500)) pit = soldier_in_pit(pos);
        }
        if (!wet && !pit) return 0;
        // A dry pit with a planned stair/ramp exit must follow that detour, not aim at its nearest rim.
        if (!wet && b->npts && b->wp < b->npts) {
            const float *q=&b->path[b->wp*3];
            if (fabsf(q[1]-pos[1])<0.65f && nav_segment_clear(ray_cast,pos,q) && floor_continuous(pos,q)) return 0;
        }

        if (!(wet ? pool_pick(pos, b->poolRim, b->target) : pool_rim(pos, b->poolRim))) { b->poolCool = t + 4000; return 0; }
        // "Out" again within 4 s and 3 m of the last time, heading for the same spot: it never left (Offshore: a bot stood at
        // -597 -27 for 40 s, "got out" every 2 s, feet level with the water, 5 m from a spot it could not reach). That spot failed.
        if (t - b->poolOutAt < 4000 && dist2d(pos, b->poolOutPos) < 3 && dist2d(b->poolRim, b->poolOutRim) < 2 && ++b->poolLoops >= 2) {
            g_rimFail[g_rimFailN % 32][0] = b->poolRim[0]; g_rimFail[g_rimFailN % 32][1] = b->poolRim[2]; g_rimFailN++; b->poolLoops = 0;
            if (!(wet ? pool_pick(pos, b->poolRim, b->target) : pool_rim(pos, b->poolRim))) { b->poolCool = t + 4000; return 0; }
        }
        b->poolSince = b->poolTry = t; b->poolBest = 1e9f; b->poolWet = wet;
        static unsigned logged;
        if (logged++ < 40) H->log("pool: %s stuck in %s at %.0f %.0f %.0f - climbs out at %.0f %.0f %.0f", (char *)b->player + OFF_PLAYER_NAME, wet ? "water" : "a pit (empty pool / ditch)", pos[0], pos[1], pos[2], b->poolRim[0], b->poolRim[1], b->poolRim[2]);
    }
    static unsigned g_poolOut, g_poolFailed;
    int wetNow = pool_swimming(pos);   // (a shore spot can sit below the swimmer's feet: out = on it, or dry)
    if ((pos[1] > b->poolRim[1] - 0.4f && (!wetNow || dist2d(pos, b->poolRim) < 1.5f)) || (t - b->poolSince > 1500 && dist2d(pos, b->poolRim) > 50)) {   // out (on the rim, or elsewhere)
        if (b->poolWet && pos[1] > b->poolRim[1] - 0.4f && dist2d(pos, b->poolRim) < 3) exits_note(b->poolRim);   // (out where it aimed: a real way out)
        g_poolOut++; static unsigned logged; if (logged++ < 40) H->log("pool: %s got out in %.1f s (%u out, %u failed so far)", (char *)b->player + OFF_PLAYER_NAME, (t - b->poolSince) / 1000.0, g_poolOut, g_poolFailed);
        b->poolOutAt = t; memcpy(b->poolOutPos, pos, 12); memcpy(b->poolOutRim, b->poolRim, 12);
        b->poolSince = 0; b->poolCool = t + 4000; b->progressAt = t; memcpy(b->progressPos, pos, 12); return 0;
    }
    // the tester: "force vaulting until it works". No giving up: every 12 s another rim spot (the ones that failed count
    // further off), and it keeps going until it is out.
    int swim = wetNow;   // (in water always swim straight at the spot: the wall dance never got anyone out on BF4)
    if (swim) {   // swimming to a shore spot: given up only once it stops getting nearer (6 s)
        float d = dist2d(pos, b->poolRim);
        if (d < b->poolBest - 1) { b->poolBest = d; b->poolTry = t - 6000; }
    }
    if (t - b->poolTry > 12000) {
        g_poolFailed++; g_rimFail[g_rimFailN % 32][0] = b->poolRim[0]; g_rimFail[g_rimFailN % 32][1] = b->poolRim[2]; g_rimFailN++;
        static unsigned logged; if (logged++ < 40) H->log("pool: %s not out at %.0f %.0f %.0f after 12 s - tries another spot (%u out, %u spots failed so far)", (char *)b->player + OFF_PLAYER_NAME, b->poolRim[0], b->poolRim[1], b->poolRim[2], g_poolOut, g_poolFailed);
        pit_note(pos);   // (12 s in and still here: a real trap - no way into it for the rest)
        if (!pool_pick(pos, b->poolRim, b->target)) { b->poolSince = 0; return 0; }
        b->poolTry = t; b->poolBest = 1e9f;
    }
    if (swim) {   // just swim at it, sprinting, jump taps at the edge (no squaring on to the nearest wall: that was the dock)
        float want = atan2f(b->poolRim[0] - pos[0], b->poolRim[2] - pos[2]);
        b->turn = turn_input(soldier_yaw(soldier), want, 3.0f); b->strafe = 0;
        b->buttons &= ~(BTN_ZOOM | BTN_CROUCH | BTN_FIRE);
        // (facing well off the spot: turn on the spot first - at full speed swimmers orbited a spot close by, turning
        // flat out for 4 s and more: the spin detector's "climbing out of water" circles)
        float offA = fabsf(wrap_pi(want - soldier_yaw(soldier)));
        b->fwd = offA > 1.2f ? 0.0f : offA > 0.6f ? 0.4f : 1.0f;
        if (offA < 0.6f) b->buttons |= BTN_SPRINT; else b->buttons &= ~BTN_SPRINT;
        if (dist2d(pos, b->poolRim) < 2.5f && b->poolRim[1] > pos[1] + 0.3f) { pool_carry(b, pos, t); if ((t / 400) % 2) { b->action = 2; b->buttons |= BTN_JUMP; } }
        return 1;
    }
    // Each go (3.2 s): square on to the wall at the rim, back off 0.7 s for a run-up, sprint in and jump when the wall
    // is 0.7 m away - the jumps that stuck came in at 3.5 m/s; from a standstill the server put them back (the tester's clip:
    // "their model appears on top but they get put back"). Then hold forward through the climb.
    ULONGLONG e = (t - b->poolTry) % 3200;
    float want = atan2f(b->poolRim[0] - pos[0], b->poolRim[2] - pos[2]);
    {   // square on: the shortest knee-height ray within 50 deg of the rim's direction
        float best = 1e9f, a0 = want;
        for (int k = -5; k <= 5; k++) {
            float a = a0 + k * 0.1745f, e0[3] = { pos[0], pos[1] + 0.6f, pos[2] }, e1[3] = { pos[0] + sinf(a) * 4, pos[1] + 0.6f, pos[2] + cosf(a) * 4 }, h[3];
            if (ray_cast(e0, e1, h, NULL)) { float d = dist2d(e0, h); if (d < best) { best = d; want = a; } }
        }
        b->poolWall = best;
    }
    b->turn = turn_input(soldier_yaw(soldier), want, 3.0f); b->strafe = 0;
    b->buttons &= ~(BTN_ZOOM | BTN_CROUCH | BTN_FIRE);
    if (e < 700 && b->poolWall < 2.5f) { b->fwd = -1; b->buttons &= ~BTN_SPRINT; return 1; }
    b->fwd = 1; b->buttons |= BTN_SPRINT;
    if (b->poolWall < 0.7f && !b->poolJumped) { b->poolJumped = 1; b->poolJumpAt = t; pool_carry(b, pos, t); }
    if (b->poolJumped && t - b->poolJumpAt < 250) { b->action = 2; b->buttons |= BTN_JUMP; }
    if (e >= 3000) b->poolJumped = 0;   // (next go)
    return 1;
}
