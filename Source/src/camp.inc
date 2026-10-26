// Thugbot window campers (included by brain.inc after movement.inc). the tester, The Block 2026-10-25: "I went to the 2nd
// floor of some buildings and took cover and shot enemies through the window. A good place for some bots to camp."
//
//   Spots: once a level's grid is ready, the grid nodes 12 - 55 m from each flag and 2.5 - 15 m above its floor are
//          looked at (a few dozen rays a frame): a roof over them (ray up hits within 4 m: indoors / under cover), a clear
//          line from standing eye height to the flag area, and best of all something waist-high between them and the
//          flag (a window sill / parapet to crouch behind). The best 16 per flag, 3 m apart, are kept.
//   Campers: g_campShare % of each team (by seed, never defenders) go to a free spot looking over a flag their team
//          wants (not owned, or owned with enemies near), crouch behind it facing the flag and stand up to look / shoot
//          every few seconds; combat does the shooting. 60 - 120 s there, then 45 s playing normally, then again.
//   Commands: "camp" (spots per flag, who is camping), "camp <percent>".

#define CAMP_SPOTS 16
typedef struct CampSpot { float p[3], score; int bad, reachChecked; } CampSpot;
static int g_campFlagReach[MAX_CPS];   // nodes reachable from the flag's floor (0 = not counted yet)
static CampSpot g_campSpot[MAX_CPS][CAMP_SPOTS];
static int g_campN[MAX_CPS], g_campFlag = -1, g_campCursor, g_campDone;
static const NavGrid *g_campGrid;
static int g_campShare = 15;
typedef struct Camp { int flag, spot, arrived; float p[3]; ULONGLONG until, restUntil, arrivedAt, goneAt; } Camp;
static Camp g_camp[MAX_BOTS];
static unsigned g_campArrivals, g_campSeen[4];   // scan: elevated nodes, with a roof, with a view, kept

static void camp_rescan(void) { g_campGrid = NULL; g_campDone = 0; }
static void camp_level_reset(void) { memset(g_campFlagReach, 0, sizeof g_campFlagReach); memset(g_campN, 0, sizeof g_campN); g_campGrid = NULL; g_campFlag = -1; g_campDone = 0; memset(g_camp, 0, sizeof g_camp); }

static void camp_add(int f, const float *p, float score)
{
    int n = g_campN[f], worst = -1;
    for (int k = 0; k < n; k++)
        if (dist2d(g_campSpot[f][k].p, p) < 3 && fabsf(g_campSpot[f][k].p[1] - p[1]) < 2) {   // near one we have: keep the better
            if (score > g_campSpot[f][k].score) { memcpy(g_campSpot[f][k].p, p, 12); g_campSpot[f][k].score = score; }
            return;
        }
    if (n < CAMP_SPOTS) { worst = n; g_campN[f]++; }
    else { for (int k = 0; k < n; k++) if (worst < 0 || g_campSpot[f][k].score < g_campSpot[f][worst].score) worst = k; if (g_campSpot[f][worst].score >= score) return; }
    memcpy(g_campSpot[f][worst].p, p, 12); g_campSpot[f][worst].score = score; g_campSpot[f][worst].bad = 0; g_campSpot[f][worst].reachChecked = 0;
}

// Clear line from eye to tgt, looking through window glass near the eye (2nd-floor windows are mostly glazed: the plain
// ray stopped at the pane - 8 of 388 roofed spots on The Block "saw" a flag). Counts its rays into *rays.
static int camp_view(const float *eye, const float *tgt, int *rays)
{
    float from[3] = { eye[0], eye[1], eye[2] }, h[3];
    for (int k = 0; k < 3; k++) {
        (*rays)++;
        if (!ray_cast(from, tgt, h, NULL)) return 1;
        uintptr_t body = *(uintptr_t *)g_lastHit.rest, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
        float dx = h[0] - eye[0], dy = h[1] - eye[1], dz = h[2] - eye[2];
        if (survey_kind(vt) != 'b' || dx * dx + dy * dy + dz * dz > 16) return 0;   // not a pane by the window: blocked
        float l = sqrtf((tgt[0] - h[0]) * (tgt[0] - h[0]) + (tgt[1] - h[1]) * (tgt[1] - h[1]) + (tgt[2] - h[2]) * (tgt[2] - h[2]));
        if (l < 0.5f) return 1;
        for (int c = 0; c < 3; c++) from[c] = h[c] + (tgt[c] - h[c]) / l * 0.3f;
    }
    return 0;
}

// A few cells a frame (ray budget ~80).
static void camp_tick(void)
{
    if (!g_nav || g_navBuilding || !g_rayCaster || g_campDone) return;
    flags_survey();
    if (g_campGrid != g_nav) {   // (wait until the level's flags are all there: the first scan ran before they were)
        int ok = 0; for (int f = 0; f < g_cpCount; f++) ok += g_fi[f].ok;
        if (!ok || ok < conquest_flags()) return;
        g_campGrid = g_nav; g_campFlag = 0; g_campCursor = 0; memset(g_campN, 0, sizeof g_campN); memset(g_campSeen, 0, sizeof g_campSeen);
    }
    NavGrid *g = g_nav;
    int rays = 0;
    while (rays < 80) {
        if (g_campFlag >= g_cpCount) {
            g_campDone = 1;
            int tot = 0; for (int f = 0; f < g_cpCount; f++) tot += g_campN[f];
            H->log("camp: %d window / balcony spot(s) found over %d flag(s) (nodes 2.5 m+ up: %u, roofed: %u, view of the flag: %u)", tot, g_cpCount, g_campSeen[0], g_campSeen[1], g_campSeen[2]);
            return;
        }
        FlagInfo *fi = &g_fi[g_campFlag];
        if (!fi->ok) { g_campFlag++; g_campCursor = 0; continue; }
        int span = (int)(55.0f / g->cell) + 1, side = span * 2 + 1;
        if (g_campCursor >= side * side) { g_campFlag++; g_campCursor = 0; continue; }
        int cx = g_campCursor % side - span, cz = g_campCursor / side - span;
        g_campCursor++;
        float x = fi->pos[0] + cx * g->cell, z = fi->pos[2] + cz * g->cell, d = sqrtf((x - fi->pos[0]) * (x - fi->pos[0]) + (z - fi->pos[2]) * (z - fi->pos[2]));
        if (d < 12 || d > 55) continue;
        int cell = nav_cell_index(g, x, z);
        if (cell < 0) continue;
        for (int l = 0; l < NAV_LAYERS; l++) {
            int node = cell * NAV_LAYERS + l;
            if (!(g->flags[node] & NAV_WALKABLE) || (g->flags[node] & NAV_WATER)) continue;
            float p[3]; nav_node_pos(g, node, p);
            float up = p[1] - fi->pos[1];
            if (up < 2.5f || up > 15) continue;
            g_campSeen[0]++;
            float ux = (fi->pos[0] - p[0]) / d, uz = (fi->pos[2] - p[2]) / d;
            float c0[3] = { p[0], p[1] + 1.0f, p[2] }, c1[3] = { p[0], p[1] + 4.0f, p[2] };
            rays++;
            int roofed = ray_cast(c0, c1, NULL, NULL);                        // no roof: a rooftop / balcony - only with a parapet
            g_campSeen[1] += roofed;
            // a clear line from standing eye height to the flag area (centre, or 5 m either side of it)
            float eye[3] = { p[0], p[1] + 1.6f, p[2] };
            int see = 0;
            for (int k = 0; k < 5 && see < 2; k++) {   // the centre and 4 points 8 m round it at chest height: 2 needed
                float tgt[3] = { fi->pos[0], fi->pos[1] + 1.3f, fi->pos[2] };
                if (k) { tgt[0] += sinf(k * 1.5708f) * 8; tgt[2] += cosf(k * 1.5708f) * 8; }
                see += camp_view(eye, tgt, &rays);
            }
            see = see >= 2;
            if (!see) continue;
            g_campSeen[2]++;
            // waist-high cover towards the flag (sill / parapet) within 1.8 m, chest height clear
            float w0[3] = { p[0], p[1] + 0.6f, p[2] }, w1[3] = { p[0] + ux * 1.8f, p[1] + 0.6f, p[2] + uz * 1.8f };
            rays++;
            int sill = ray_cast(w0, w1, NULL, NULL);
            if (!roofed && !sill) continue;
            if (nav_node_clearance(g, p[0], p[1], p[2]) < 0) continue;
            camp_add(g_campFlag, p, (sill ? 10.0f : 0.0f) + (roofed ? 4.0f : 0.0f) + (40 - fabsf(d - 30)) / 4 + fminf(up, 8) * 0.5f);
        }
    }
}

static int is_camper(const Bot *b) { return (int)((b->seed >> 7) % 100) < g_campShare && !is_defender((Bot *)b); }

// objectives.inc asks here first (flag modes). Returns 1 when it set the bot's target to a camp spot.
static int camp_target(int i, Bot *b, int team, const float *pos)
{
    Camp *c = &g_camp[i];
    ULONGLONG t = now_ms();
    if (!g_campDone || !is_camper(b) || t < c->restUntil) { c->flag = -1; return 0; }
    if (c->flag >= 0) {   // still wanted there?
        FlagInfo *fi = &g_fi[c->flag];
        int wanted = fi->ok && (fi->owner != team || fi->enemies[team] > 0);
        // (the planner gave up on it - target_dead: written off at once, not after the 90 s; Desert 2026-10-26 had campers
        // re-planning to unreachable upper floors, each a 120 000-node search that held up everyone's routes)
        if (!c->arrived && target_dead(c->p)) g_campSpot[c->flag][c->spot].bad = 1;
        if (g_campSpot[c->flag][c->spot].bad || (c->arrived && t > c->until) || (!c->arrived && t - c->goneAt > 90000) || (!wanted && c->arrived && t - c->arrivedAt > 20000)) {
            if (!c->arrived) g_campSpot[c->flag][c->spot].bad = 1;   // a minute and never got there
            c->flag = -1; c->restUntil = t + 45000; b->npts = 0;
            return 0;
        }
    }
    if (c->flag < 0) {   // pick: a spot over a flag the team wants, nearest, not taken
        int bf = -1, bs = -1; float best = 1e9f;
        for (int f = 0; f < g_cpCount; f++) {
            FlagInfo *fi = &g_fi[f];
            if (!fi->ok || !(fi->owner != team || fi->enemies[team] > 0)) continue;
            for (int k = 0; k < g_campN[f]; k++) {
                CampSpot *s = &g_campSpot[f][k];
                if (s->bad || target_dead(s->p)) continue;
                static unsigned reachFrame, reachN;   // (a flood fill each: at most 3 a frame, unchecked spots wait)
                if (reachFrame != H->frame) { reachFrame = H->frame; reachN = 0; }
                if (!s->reachChecked && reachN >= 3) continue;
                if (!s->reachChecked && g_nav && !g_navBuilding && ++reachN) {   // same walkable piece of the map as the flag? (once per spot)
                    s->reachChecked = 1; float np[3];
                    if (!g_campFlagReach[f]) g_campFlagReach[f] = nav_reach(g_nav, g_fi[f].pos[0], g_fi[f].pos[1], g_fi[f].pos[2], np);
                    int r = nav_reach(g_nav, s->p[0], s->p[1], s->p[2], np);
                    if (r < g_campFlagReach[f] / 2) { s->bad = 1; continue; }
                }
                int taken = 0;
                for (int q = 0; q < H->botCount && !taken; q++) if (q != i && g_camp[q].flag >= 0 && dist2d(g_camp[q].p, s->p) < 4) taken = 1;
                if (taken) continue;
                float score = dist2d(pos, s->p) - s->score * 3;
                if (score < best) { best = score; bf = f; bs = k; }
            }
        }
        if (bf < 0 || best > 150) { c->restUntil = t + 20000; return 0; }
        c->flag = bf; c->spot = bs; c->arrived = 0; c->goneAt = t;
        memcpy(c->p, g_campSpot[bf][bs].p, 12);
        b->npts = 0;
        static unsigned logged; if (logged++ < 30) H->log("camp: %s heads for a spot over flag %d at %.0f %.0f %.0f", (char *)b->player + OFF_PLAYER_NAME, bf, c->p[0], c->p[1], c->p[2]);
    }
    if (dist2d(b->target, c->p) > 0.5f) b->npts = 0;
    memcpy(b->target, c->p, 12);
    b->targetFlag = -1;
    return 1;
}

// After steering, before combat: at the spot, hold still facing the flag, crouched, standing up now and then to look.
static void camp_hold(int i, Bot *b, void *soldier, const float *pos)
{
    Camp *c = &g_camp[i];
    if (c->flag < 0 || dist2d(pos, c->p) > 2.6f || fabsf(pos[1] - c->p[1]) > 1.5f) return;
    ULONGLONG t = now_ms();
    // (the route ends up to 2.2 m short: walk the last bit straight, slowly)
    if (!c->arrived && dist2d(pos, c->p) > 0.9f && t - c->goneAt < 90000) {
        b->turn = turn_input(soldier_yaw(soldier), atan2f(c->p[0] - pos[0], c->p[2] - pos[2]), 3.0f); b->fwd = 0.5f; b->strafe = 0;
        b->progressAt = t; memcpy(b->progressPos, pos, 12); b->why = "going to a window"; return;
    }
    if (!c->arrived) { c->arrived = 1; c->arrivedAt = t; c->until = t + 60000 + (b->seed % 60000); g_campArrivals++; }
    const float *fp = g_fi[c->flag].pos;
    b->fwd = 0; b->strafe = 0;
    b->turn = turn_input(soldier_yaw(soldier), atan2f(fp[0] - pos[0], fp[2] - pos[2]), 3.0f);
    if (((t + b->seed) / 1000) % 7 < 4) b->buttons |= BTN_CROUCH;   // 4 s down behind the sill, 3 s up looking out
    b->why = "camping a window";
    b->progressAt = t; memcpy(b->progressPos, pos, 12);
}

static void camp_dump(void)
{
    int camping = 0, going = 0;
    for (int i = 0; i < H->botCount; i++) if (g_camp[i].flag >= 0) { if (g_camp[i].arrived) camping++; else going++; }
    H->log("camp: share %d%%, spots %s, %d bot(s) camping, %d on the way, %u arrival(s) so far", g_campShare, g_campDone ? "ready" : "being found", camping, going, g_campArrivals);
    for (int f = 0; f < g_cpCount; f++) if (g_campN[f]) {
        char line[512]; int n = snprintf(line, sizeof line, "camp: flag %d:", f);
        for (int k = 0; k < g_campN[f] && n < 460; k++) n += snprintf(line + n, sizeof line - n, " %.0f,%.0f,%.0f(%.0f%s)", g_campSpot[f][k].p[0], g_campSpot[f][k].p[1], g_campSpot[f][k].p[2], g_campSpot[f][k].score, g_campSpot[f][k].bad ? " bad" : "");
        H->log("%s", line);
    }
}
