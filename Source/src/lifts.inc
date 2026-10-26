// Thugbot elevators (included by brain.inc after editor.inc). Downtown's towers: an elevator is called / sent with E.
//
//   Where they are: learned from players - someone who rises (or drops) 8 m+ while moving under 3 m sideways within
//   20 s rode an elevator: the bottom and top spots become a lift - and from the editor ("!lift" at the bottom, then
//   "!lift" at the top). Kept in Thugbot_learned\<level>.lifts ("lift x y z x y z").
//   Using them: each lift is a grid link both ways (cost 25 m) so routes may go through it; a bot at one end whose
//   route goes to the other presses E every 1.5 s and waits (up to 25 s) until it has been carried there.

#define MAX_LIFTS 32
typedef struct Lift { float a[3], b[3]; } Lift;   // a: bottom, b: top
static Lift g_lifts[MAX_LIFTS];
static int g_liftN, g_liftsLoaded, g_liftLinked;
static float g_liftPending[3]; static int g_liftPendingSet;   // editor: first "!lift"

static void lifts_path(char *out, size_t n) { map_path(out, n, "Thugbot_learned", ".lifts"); }
static void lifts_save(void)
{
    char path[160]; lifts_path(path, sizeof path);
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < g_liftN; i++) fprintf(f, "lift %.1f %.1f %.1f %.1f %.1f %.1f\n", g_lifts[i].a[0], g_lifts[i].a[1], g_lifts[i].a[2], g_lifts[i].b[0], g_lifts[i].b[1], g_lifts[i].b[2]);
    fclose(f);
}
static void lifts_load(void)
{
    if (g_liftsLoaded || !g_levelKey[0]) return;
    g_liftsLoaded = 1; g_liftN = 0; g_liftLinked = 0;
    char path[160], line[160]; lifts_path(path, sizeof path);
    FILE *f = fopen(path, "r"); if (!f) return;
    while (fgets(line, sizeof line, f) && g_liftN < MAX_LIFTS) {
        Lift *l = &g_lifts[g_liftN];
        if (sscanf(line, "lift %f %f %f %f %f %f", &l->a[0], &l->a[1], &l->a[2], &l->b[0], &l->b[1], &l->b[2]) == 6) g_liftN++;
    }
    fclose(f);
    if (g_liftN) H->log("lifts: %d elevator(s) on %s", g_liftN, g_levelKey);
}
static int g_owDone;   // one-way fix done for this level
static void lifts_level_reset(void) { g_owDone = 0; g_liftsLoaded = 0; g_liftN = 0; g_liftLinked = 0; g_liftPendingSet = 0; }

static int lift_floor(const float *p)
{
    float a[3] = { p[0], p[1] + 1.0f, p[2] }, b[3] = { p[0], p[1] - 2.5f, p[2] }, h[3];
    return ray_cast(a, b, h, NULL);
}
static void lift_add(const float *p, const float *q, const char *who)
{
    if (!lift_floor(p) || !lift_floor(q)) { H->log("lifts: %s rose / fell %.0f m in the air (no floor under) - not an elevator", who, fabsf(p[1] - q[1])); return; }
    const float *lo = p[1] < q[1] ? p : q, *hi = p[1] < q[1] ? q : p;
    for (int i = 0; i < g_liftN; i++) if (dist2d(g_lifts[i].a, lo) < 4 && fabsf(g_lifts[i].a[1] - lo[1]) < 3 && fabsf(g_lifts[i].b[1] - hi[1]) < 3) return;   // known
    if (g_liftN >= MAX_LIFTS) return;
    memcpy(g_lifts[g_liftN].a, lo, 12); memcpy(g_lifts[g_liftN].b, hi, 12); g_liftN++;
    g_liftLinked = 0;   // (link the new one)
    lifts_save();
    H->log("lifts: %s from %.0f %.0f %.0f up to %.0f m (from %s)", hi[1] - lo[1] < 8 ? "ladder" : "elevator / ladder", lo[0], lo[1], lo[2], hi[1], who);
}

// grid links for every lift, once the grid is ready
static void lifts_link(void)
{
    if (g_liftLinked || !g_nav || g_navBuilding) return;
    g_liftLinked = 1;
    int n = 0, dropped = 0;
    for (int i = 0; i < g_liftN; i++)   // learned from someone falling / parachuting / flying: no floor at an end
        if (!lift_floor(g_lifts[i].a) || !lift_floor(g_lifts[i].b)) { H->log("lifts: %.0f %.0f %.0f -> %.0f m has no floor at an end - dropped", g_lifts[i].a[0], g_lifts[i].a[1], g_lifts[i].a[2], g_lifts[i].b[1]); g_lifts[i--] = g_lifts[--g_liftN]; dropped++; }
    if (dropped) lifts_save();
    for (int i = 0; i < g_liftN; i++) {
        // The cab floor itself is often not on the grid (closed doors when it was scanned): link the walkable spots
        // nearest each end instead (the ride starts by walking into the cab - lift_ride).
        float a[3], b[3];
        if (!nav_snap(g_nav, g_lifts[i].a[0], g_lifts[i].a[1], g_lifts[i].a[2], a) && !nav_snap(g_nav, g_lifts[i].a[0], g_lifts[i].a[1] + 1.5f, g_lifts[i].a[2], a)) memcpy(a, g_lifts[i].a, 12);
        if (!nav_snap(g_nav, g_lifts[i].b[0], g_lifts[i].b[1], g_lifts[i].b[2], b) && !nav_snap(g_nav, g_lifts[i].b[0], g_lifts[i].b[1] + 1.5f, g_lifts[i].b[2], b)) memcpy(b, g_lifts[i].b, 12);
        int l1 = nav_add_link(g_nav, a, b, 25), l2 = nav_add_link(g_nav, b, a, 25);
        if (l1 < 0 || l2 < 0) {   // an end on a floor the grid doesn't hold (more floors stacked there than it keeps): probe it in
            if (l1 >= 0) nav_disable_link(l1);
            if (l2 >= 0) nav_disable_link(l2);
            const float *e2[2] = { g_lifts[i].a, g_lifts[i].b };
            for (int e = 0; e < 2; e++) nav_reprobe_area(g_nav, ray_cast, e2[e][0] - 5, e2[e][2] - 5, e2[e][0] + 5, e2[e][2] + 5, e2[e][1] + 1.5f);
            memcpy(a, g_lifts[i].a, 12); memcpy(b, g_lifts[i].b, 12);
            l1 = nav_add_link(g_nav, a, b, 25); l2 = nav_add_link(g_nav, b, a, 25);
        }
        n += (l1 >= 0) + (l2 >= 0);
        H->log("lifts: %.0f %.0f %.0f -> %.0f m: bottom on the grid at %.1f, top at %.1f - %s", g_lifts[i].a[0], g_lifts[i].a[1], g_lifts[i].a[2], g_lifts[i].b[1], a[1], b[1], l1 >= 0 && l2 >= 0 ? "linked" : "NOT linked");
    }
    if (g_liftN) H->log("lifts: %d grid link(s) for %d elevator(s)", n, g_liftN);
}

// Flags only reachable one way: tall buildings stack more floors than the grid keeps per spot, so a stairwell can lose
// the steps in between and only the drops down survive (Downtown's tower roof: the way down is on the grid, the way
// up is not). Once per level, a flag no route reaches from a bot far away but that has a route back gets every drop on
// that way back linked the other way too (bots walk up those steps).
static void oneway_fix(void)
{
    if (g_owDone || !g_nav || g_navBuilding || !g_liftLinked || !g_cpCount) return;
    float ref[3]; float farD = 0; int have = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *s = bot_soldier(g_players[j]);
        if (!s || player_vehicle(g_players[j], NULL)) continue;
        const float *p = soldier_pos(s);
        float d = 0; for (int f = 0; f < g_cpCount; f++) if (g_fi[f].ok) d += dist2d(p, g_fi[f].pos);
        if (d > farD && nav_node_clearance(g_nav, p[0], p[1], p[2]) >= 0) { farD = d; memcpy(ref, p, 12); have = 1; }
    }
    if (!have) return;
    g_owDone = 1;
    static float pts[BRAIN_PATH_MAX * 3];
    for (int f = 0; f < g_cpCount; f++) {
        if (!g_fi[f].ok) continue;
        const float *fp = g_fi[f].pos;
        nav_set_path_owner(-1);
        if (nav_find_path(g_nav, ray_cast, ref[0], ref[1], ref[2], fp[0], fp[1], fp[2], pts, BRAIN_PATH_MAX) > 0) continue;
        nav_set_path_owner(-1);
        int n = nav_find_path(g_nav, ray_cast, fp[0], fp[1], fp[2], ref[0], ref[1], ref[2], pts, BRAIN_PATH_MAX), links = 0;
        for (int k = 0; k + 1 < n && links < 8; k++) {
            const float *hi = &pts[k * 3], *lo = &pts[(k + 1) * 3];
            float drop = hi[1] - lo[1], side = dist2d(hi, lo);
            if (drop < 0.9f || drop > 4.0f || side > 4.5f) continue;
            if (nav_add_link(g_nav, lo, hi, side * 3 + drop * 2) >= 0) links++;
        }
        nav_set_path_owner(-1);
        int ok = links && nav_find_path(g_nav, ray_cast, ref[0], ref[1], ref[2], fp[0], fp[1], fp[2], pts, BRAIN_PATH_MAX) > 0;
        H->log("oneway: flag at %.0f %.0f %.0f has no way up: %d route point(s) down, %d step(s) linked up - %s", fp[0], fp[1], fp[2], n, links, ok ? "reachable now" : "still cut off");
        if (ok) target_mark_alive(fp);
    }
}

// players riding: per player, where they were over the last 20 s (a sample a second)
#define LIFT_SAMPLES 20
static struct { void *pl; float p[LIFT_SAMPLES][3]; int n; ULONGLONG at; } g_liftWatch[32];
static void lifts_watch_humans(void)
{
    ULONGLONG t = now_ms();
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        if (is_bot_player(pl)) continue;
        void *s = bot_soldier(pl);
        if (!s || player_vehicle(pl, NULL)) continue;
        int k = 0, freeK = -1;
        while (k < 32 && g_liftWatch[k].pl != pl) { if (!g_liftWatch[k].pl && freeK < 0) freeK = k; k++; }
        if (k == 32) { if (freeK < 0) continue; k = freeK; memset(&g_liftWatch[k], 0, sizeof g_liftWatch[k]); g_liftWatch[k].pl = pl; }
        if (t - g_liftWatch[k].at < 1000) continue;
        g_liftWatch[k].at = t;
        const float *p = soldier_pos(s);
        int n = g_liftWatch[k].n;
        if (n == LIFT_SAMPLES) { memmove(g_liftWatch[k].p[0], g_liftWatch[k].p[1], sizeof(float) * 3 * (LIFT_SAMPLES - 1)); n--; }
        memcpy(g_liftWatch[k].p[n], p, 12); g_liftWatch[k].n = n + 1;
        // a ride: the oldest sample within 3 m sideways and 8 m+ apart in height from now, the samples in between too.
        // Ladders: 3 m+ within 2 m sideways, over 2 s or more, no second faster than 5 m (a drop off a ledge is faster).
        for (int o = 0; o < n; o++) {
            const float *q = g_liftWatch[k].p[o];
            float rise = fabsf(p[1] - q[1]);
            int ladder = rise >= 3 && rise < 8 && dist2d(p, q) <= 2 && n - o >= 2;
            if ((rise < 8 && !ladder) || dist2d(p, q) > 3) continue;
            int side = 1;
            for (int m = o; m <= n; m++) {
                if (dist2d(g_liftWatch[k].p[m], q) > (ladder ? 2 : 3)) side = 0;
                if (ladder && m > o && fabsf(g_liftWatch[k].p[m][1] - g_liftWatch[k].p[m - 1][1]) > 5) side = 0;
            }
            if (!side) continue;
            lift_add(q, p, (char *)pl + OFF_PLAYER_NAME);
            g_liftWatch[k].n = 0;
            break;
        }
    }
}

// A bot standing at one end of a lift with its route going to the other: ride it. Returns 1 while riding.
static int lift_ride(Bot *b, const float *pos, ULONGLONG t)
{
    for (int i = 0; i < g_liftN; i++) {
        Lift *l = &g_lifts[i];
        for (int e = 0; e < 2; e++) {
            const float *here = e ? l->b : l->a, *there = e ? l->a : l->b;
            if (dist2d(pos, here) > 7 || fabsf(pos[1] - here[1]) > 2.5f) continue;
            int going = 0;   // the route (next few points) or the target is at the other end
            for (int k = b->wp; k < b->npts && k < b->wp + 3; k++) if (fabsf(b->path[k * 3 + 1] - there[1]) < 3 && dist2d(&b->path[k * 3], there) < 6) going = 1;
            if (!going && fabsf(b->target[1] - there[1]) < 3 && fabsf(b->target[1] - pos[1]) > 6) going = 1;
            if (!going) continue;
            if (!b->liftSince) b->liftSince = t;
            if (t - b->liftSince > 25000) { b->liftSince = 0; b->npts = 0; return 0; }   // it didn't come: give up (walk / re-plan)
            void *s = bot_soldier(b->player);
            if (s && dist2d(pos, here) > 1.0f) {   // into the cab first (pressing E at its door, the doors shut it out)
                b->turn = turn_input(soldier_yaw(s), atan2f(here[0] - pos[0], here[2] - pos[2]), 3.0f);
                b->fwd = 0.7f; b->strafe = 0;
                if ((t - b->liftSince) % 1500 < 250) b->buttons |= BTN_INTERACT;   // (opens / calls it)
                return 1;
            }
            // In the cab / at the foot of a ladder: keep pushing forward, facing the other end (a ladder's top is a little
            // in front of its foot; an elevator doesn't mind) - forward climbs a ladder, E calls an elevator.
            b->strafe = 0; b->fwd = 0;
            if (s && dist2d(here, there) > 0.25f) { b->turn = turn_input(soldier_yaw(s), atan2f(there[0] - here[0], there[2] - here[2]), 3.0f); b->fwd = 0.8f; }
            else if (s && t - b->liftSince > 3000) b->fwd = 0.8f;   // (straight up: forward the way it faces)
            if ((t - b->liftSince) % 1500 < 250) b->buttons |= BTN_INTERACT;
            return 1;
        }
    }
    b->liftSince = 0;
    return 0;
}
