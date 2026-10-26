// Thugbot pools (included by brain.inc after parachute.inc): a bot in water that isn't getting anywhere swims for the
// nearest rim (dry ground 0.2 - 2.2 m above it, next to the water or the pit's floor, within 12 m) and jumps / vaults out there. Pools on
// Hollywood Heights have walls the planner can't step up, so bots swam in circles (the tester, 2026-10-25).

// rim spots that failed (15 s and still in) this level: avoided next time
static float g_rimFail[32][2]; static int g_rimFailN;
static int b_failNear(float x, float z) { for (int k = 0; k < g_rimFailN && k < 32; k++) if (fabsf(g_rimFail[k][0] - x) < 2.5f && fabsf(g_rimFail[k][1] - z) < 2.5f) return 1; return 0; }
static int pool_rim(const float *pos, float *out)
{
    int wetHere = g_nav && nav_is_water(g_nav, pos[0], pos[1], pos[2]);
    NavGrid *g = g_nav;
    int c = nav_cell_index(g, pos[0], pos[2]);
    if (c < 0) return 0;
    int cx = c % g->w, cz = c / g->w, r = (int)(12 / g->cell) + 1, found = 0;
    float best = 1e9f;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= g->w || z >= g->h) continue;
            for (int l = 0; l < NAV_LAYERS; l++) {
                int node = (z * g->w + x) * NAV_LAYERS + l;
                if (!(g->flags[node] & NAV_WALKABLE) || (g->flags[node] & NAV_WATER)) continue;
                float y = g->height[node];
                if (y < pos[1] + 0.2f || y > pos[1] + 2.2f) continue;
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

static int pool_exit(Bot *b, void *soldier, const float *pos, ULONGLONG t)
{
    if (!g_nav || g_navBuilding) return 0;
    if (!b->poolSince) {
        // stuck (under 2 m in 2.5 s) and in water / a pit - or in a pit at all with its way going up out of it (a crowd
        // stepping round each other kept resetting "stuck": the D pit on mp_hills held 6 bots, the tester 2026-10-26)
        int stuck = t - b->progressAt >= 2500 && dist2d(pos, b->progressPos) <= 2;
        int wet = stuck && (nav_is_water(g_nav, pos[0], pos[1], pos[2]) || soldier_in_water(pos));
        int pit = 0;
        if (!wet && t >= b->pitLookAt) {
            b->pitLookAt = t + 1000;
            const float *w = b->npts && b->wp < b->npts ? &b->path[b->wp * 3] : b->target;
            if ((stuck && t - b->progressAt >= 5000) || (w[1] > pos[1] + 0.6f && dist2d(pos, w) < 8) || dist2d(pos, b->target) > 12) pit = soldier_in_pit(pos);
        }
        if (!wet && !pit) return 0;
        if (!pool_rim(pos, b->poolRim)) return 0;
        b->poolSince = b->poolTry = t;
        pit_note(pos);
        static unsigned logged;
        if (logged++ < 40) H->log("pool: %s stuck in %s at %.0f %.0f %.0f - climbs out at %.0f %.0f %.0f", (char *)b->player + OFF_PLAYER_NAME, wet ? "water" : "a pit (empty pool / ditch)", pos[0], pos[1], pos[2], b->poolRim[0], b->poolRim[1], b->poolRim[2]);
    }
    static unsigned g_poolOut, g_poolFailed;
    if (pos[1] > b->poolRim[1] - 0.4f || (t - b->poolSince > 1500 && dist2d(pos, b->poolRim) > 16)) {   // out (on the rim, or elsewhere)
        g_poolOut++; static unsigned logged; if (logged++ < 40) H->log("pool: %s got out in %.1f s (%u out, %u failed so far)", (char *)b->player + OFF_PLAYER_NAME, (t - b->poolSince) / 1000.0, g_poolOut, g_poolFailed);
        b->poolSince = 0; b->progressAt = t; memcpy(b->progressPos, pos, 12); return 0;
    }
    // the tester: "force vaulting until it works". No giving up: every 12 s another rim spot (the ones that failed count
    // further off), and it keeps going until it is out.
    if (t - b->poolTry > 12000) {
        g_poolFailed++; g_rimFail[g_rimFailN % 32][0] = b->poolRim[0]; g_rimFail[g_rimFailN % 32][1] = b->poolRim[2]; g_rimFailN++;
        static unsigned logged; if (logged++ < 40) H->log("pool: %s not out at %.0f %.0f %.0f after 12 s - tries another spot (%u out, %u spots failed so far)", (char *)b->player + OFF_PLAYER_NAME, b->poolRim[0], b->poolRim[1], b->poolRim[2], g_poolOut, g_poolFailed);
        if (!pool_rim(pos, b->poolRim)) { b->poolSince = 0; return 0; }
        b->poolTry = t;
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
    if (b->poolWall < 0.7f && !b->poolJumped) { b->poolJumped = 1; b->poolJumpAt = t; }
    if (b->poolJumped && t - b->poolJumpAt < 250) { b->action = 2; b->buttons |= BTN_JUMP; }
    if (e >= 3000) b->poolJumped = 0;   // (next go)
    return 1;
}
