// Thugbot elevators (included by brain.inc after editor.inc). Downtown's towers: an elevator is called / sent with E.
//
//   Where they are: learned from players - someone who rises (or drops) 8 m+ while moving under 3 m sideways within
//   20 s rode an elevator: the bottom and top spots become a lift - and from the editor ("!lift" at the bottom, then
//   "!lift" at the top). Kept in Thugbot_learned\<level>.lifts ("lift x y z x y z").
//   Using them: each lift is a grid link both ways (cost 25 m) so routes may go through it; a bot at one end whose
//   route goes to the other presses E every 1.5 s and waits (up to 25 s) until it has been carried there.

#define MAX_LIFTS 32
typedef struct Lift {
    float a[3], b[3];                // Cab centers at the bottom and top
    float landing[2][3];            // Connected grid points on the hall side
    float door[2][3];
    ULONGLONG scanAt[2], interactAt;
    int links[2], doorKnown[2], doorOpen[2];
} Lift;
typedef enum LiftPhase { LIFT_IDLE, LIFT_BOARD, LIFT_RIDE, LIFT_EXIT, LIFT_LADDER } LiftPhase;
typedef struct LiftUse {
    LiftPhase phase;
    int index, end, nextPoint;
    ULONGLONG spawnAt, since, cooldown;
} LiftUse;
static LiftUse g_liftUse[MAX_BOTS];
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
        memset(l, 0, sizeof *l);
        l->links[0] = l->links[1] = -1;
        if (sscanf(line, "lift %f %f %f %f %f %f", &l->a[0], &l->a[1], &l->a[2], &l->b[0], &l->b[1], &l->b[2]) != 6) continue;
        if (isfinite(l->a[0]) && isfinite(l->a[1]) && isfinite(l->a[2]) &&
            isfinite(l->b[0]) && isfinite(l->b[1]) && isfinite(l->b[2]) &&
            l->b[1] - l->a[1] >= 3 && dist2d(l->a, l->b) <= 8) g_liftN++;
    }
    fclose(f);
    if (g_liftN) H->log("lifts: %d elevator(s) on %s", g_liftN, g_levelKey);
}
static int g_owDone;   // one-way fix done for this level
static void lifts_level_reset(void) { g_owDone = 0; g_liftsLoaded = 0; g_liftN = 0; g_liftLinked = 0; g_liftPendingSet = 0; memset(g_liftUse, 0, sizeof g_liftUse); }

static int lift_floor(const float *p)
{
    float a[3] = { p[0], p[1] + 1.0f, p[2] }, b[3] = { p[0], p[1] - 2.5f, p[2] }, h[3];
    return ray_cast(a, b, h, NULL);
}
static void lift_add(const float *p, const float *q, const char *who)
{
    if (dist2d(p, q) > 8 || !lift_floor(p) || !lift_floor(q)) { H->log("lifts: %s rose / fell %.0f m in the air (no floor under) - not an elevator", who, fabsf(p[1] - q[1])); return; }
    const float *lo = p[1] < q[1] ? p : q, *hi = p[1] < q[1] ? q : p;
    for (int i = 0; i < g_liftN; i++) if (dist2d(g_lifts[i].a, lo) < 4 && fabsf(g_lifts[i].a[1] - lo[1]) < 3 && fabsf(g_lifts[i].b[1] - hi[1]) < 3) return;   // known
    if (g_liftN >= MAX_LIFTS) return;
    Lift *l = &g_lifts[g_liftN++];
    memset(l, 0, sizeof *l);
    l->links[0] = l->links[1] = -1;
    memcpy(l->a, lo, 12);
    memcpy(l->b, hi, 12);
    g_liftLinked = 0;   // (link the new one)
    lifts_save();
    H->log("lifts: %s from %.0f %.0f %.0f up to %.0f m (from %s)", hi[1] - lo[1] < 8 ? "ladder" : "elevator / ladder", lo[0], lo[1], lo[2], hi[1], who);
}

static const float *lift_stop(const Lift *l, int end) { return end ? l->b : l->a; }

static int lift_level_floor(const float *p)
{
    float a[3] = { p[0], p[1] + 0.65f, p[2] }, z[3] = { p[0], p[1] - 0.85f, p[2] }, h[3];
    return ray_cast(a, z, h, NULL);
}

// Find a level hallway through an open doorway or a thin closed door. A shaft wall
// has no opening on the other side of the panel and is excluded by the reverse ray.
static int lift_door(Lift *l, int end, ULONGLONG t)
{
    if (t < l->scanAt[end]) return l->doorKnown[end];
    const float *cab = lift_stop(l, end);
    l->scanAt[end] = t + 1300;
    l->doorOpen[end] = 0;
    float best = -1000.0f;
    for (int k = 0; k < 24; k++) {
        float yaw = k * 0.2617994f, dx = sinf(yaw), dz = cosf(yaw);
        float hall[3] = {cab[0] + dx * 3.6f, cab[1], cab[2] + dz * 3.6f};
        if (!lift_level_floor(hall)) continue;
        float from[3] = {cab[0], cab[1] + 1.15f, cab[2]};
        float to[3] = {hall[0], from[1], hall[2]}, hit[3], back[3], normal[3];
        int open = !ray_cast(from, to, hit, normal);
        if (!open && (!ray_cast(to, from, back, NULL) || dist2d(hit, back) > 0.28f ||
                      fabsf(dx * normal[0] + dz * normal[2]) < 0.82f)) continue;
        float score = open ? 12.0f : 0.0f;
        if (open) {
            // Both shoulders should fit. The score favors the middle of the opening.
            for (int side = -1; side <= 1; side += 2) {
                float a[3] = {from[0] + dz * side * 0.35f, from[1], from[2] - dx * side * 0.35f};
                float b[3] = {to[0] + dz * side * 0.35f, to[1], to[2] - dx * side * 0.35f};
                if (!ray_cast(a, b, NULL, NULL)) score += 2;
            }
        } else score += fabsf(dx * normal[0] + dz * normal[2]);
        if (l->doorKnown[end]) score -= fminf(3, dist2d(l->door[end], hall)) * 0.4f;
        if (score <= best) continue;
        best = score;
        memcpy(l->door[end], hall, 12);
        l->doorOpen[end] = open;
    }
    if (best > -1000) l->doorKnown[end] = 1;
    return l->doorKnown[end];
}

static int lift_landing(Lift *l, int end, float *out)
{
    const float *cab = lift_stop(l, end);
    if (l->doorKnown[end]) {
        const float *hall = l->door[end];
        if (nav_snap(g_nav, hall[0], hall[1], hall[2], out) && dist2d(out, hall) < 2.5f &&
            fabsf(out[1] - hall[1]) < 1.25f && nav_segment_clear(ray_cast, hall, out)) return 1;
    }
    // A closed cab may be absent from the grid, but a connected floor on this
    // landing is enough to keep the elevator discoverable when the doors open.
    return nav_snap(g_nav, cab[0], cab[1], cab[2], out) &&
        dist2d(out, cab) < 8 && fabsf(out[1] - cab[1]) < 1.6f;
}

static void lifts_link(void)
{
    if (g_liftLinked || !g_nav || g_navBuilding) return;
    g_liftLinked = 1;
    for (int i = 0; i < g_liftN; i++) {
        Lift *l = &g_lifts[i];
        if (l->links[0] >= 0 && l->links[1] >= 0) continue;
        int tall = l->b[1] - l->a[1] >= 8;
        if (tall) {
            lift_door(l, 0, now_ms());
            lift_door(l, 1, now_ms());
        } else if (!lift_floor(l->a) || !lift_floor(l->b)) continue;
        if (!lift_landing(l, 0, l->landing[0]) || !lift_landing(l, 1, l->landing[1])) {
            H->log("lifts: no same-floor grid landing at %.1f %.1f %.1f -> %.1f", l->a[0], l->a[1], l->a[2], l->b[1]);
            continue;
        }
        l->links[0] = nav_add_link(g_nav, l->landing[0], l->landing[1], 25);
        l->links[1] = nav_add_link(g_nav, l->landing[1], l->landing[0], 25);
        if (l->links[0] < 0 || l->links[1] < 0) {
            nav_disable_link(l->links[0]);
            nav_disable_link(l->links[1]);
            l->links[0] = l->links[1] = -1;
        }
        H->log("lifts: %.1f %.1f %.1f -> %.1f, %s", l->a[0], l->a[1], l->a[2], l->b[1],
               l->links[0] >= 0 ? "both directions linked" : "could not link");
    }
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

static void lift_face(Bot *b, void *soldier, float yaw, float speed)
{
    float facing = soldier_yaw(soldier);
    b->turn = turn_input(facing, yaw, 3.0f);
    b->fwd = fabsf(wrap_pi(yaw - facing)) < 0.45f ? speed : 0;
    b->strafe = 0;
}

static void lift_walk(Bot *b, void *soldier, const float *pos, const float *to, float speed)
{
    lift_face(b, soldier, atan2f(to[0] - pos[0], to[2] - pos[2]), speed);
}

static void lift_interact(Bot *b, Lift *l, ULONGLONG t)
{
    if (t >= l->interactAt + 900) l->interactAt = t;
    if (t - l->interactAt < 200) b->buttons |= BTN_INTERACT;
}

static int lift_ride(Bot *b, void *soldier, const float *pos, ULONGLONG t, int activeOnly)
{
    LiftUse *use = &g_liftUse[b - g_bot];
    if (use->spawnAt != b->spawnAt) {
        memset(use, 0, sizeof *use);
        use->spawnAt = b->spawnAt;
    }
    if (use->phase == LIFT_IDLE) {
        if (activeOnly || t < use->cooldown || !g_liftLinked) return 0;
        for (int i = 0; i < g_liftN && use->phase == LIFT_IDLE; i++) {
            Lift *l = &g_lifts[i];
            if (l->links[0] < 0) continue;
            for (int end = 0; end < 2; end++) {
                const float *here = lift_stop(l, end), *there = lift_stop(l, 1 - end);
                if (fabsf(pos[1] - here[1]) > 1.8f || dist2d(pos, here) > 8) continue;
                for (int k = b->wp; k < b->npts && k <= b->wp + 1; k++) {
                    const float *q = b->path + k * 3;
                    if (fabsf(q[1] - there[1]) > 2 || dist2d(q, l->landing[1 - end]) > 4) continue;
                    if (k > b->wp && dist2d(pos, b->path + (k - 1) * 3) > 3.5f) continue;
                    if (dist2d(pos, l->landing[end]) > 1.6f &&
                        !nav_segment_clear(ray_cast, pos, l->landing[end])) continue;
                    use->phase = l->b[1] - l->a[1] >= 8 ? LIFT_BOARD : LIFT_LADDER;
                    use->index = i; use->end = end; use->nextPoint = k + 1; use->since = t;
                    H->log("lift: %s approaching %.1f %.1f %.1f for floor %.1f",
                           (char *)b->player + OFF_PLAYER_NAME, here[0], here[1], here[2], there[1]);
                    break;
                }
                if (use->phase != LIFT_IDLE) break;
            }
        }
        if (use->phase == LIFT_IDLE) return 0;
    }

    Lift *l = &g_lifts[use->index];
    const float *here = lift_stop(l, use->end), *there = lift_stop(l, 1 - use->end);
    float rise = fabsf(there[1] - here[1]), gap = dist2d(pos, here);
    b->fwd = b->strafe = b->turn = b->pitch = 0;
    b->action = 0;
    if (use->phase == LIFT_BOARD && gap < 2.1f && fabsf(pos[1] - here[1]) > 1.6f) {
        use->phase = LIFT_RIDE;
        use->since = t;
        H->log("lift: %s riding towards %.1f", (char *)b->player + OFF_PLAYER_NAME, there[1]);
    }
    if ((use->phase == LIFT_RIDE || use->phase == LIFT_LADDER) &&
        fabsf(pos[1] - there[1]) < 0.85f && dist2d(pos, there) < 3.2f) {
        use->phase = LIFT_EXIT;
        use->since = t;
        H->log("lift: %s arrived at %.1f", (char *)b->player + OFF_PLAYER_NAME, pos[1]);
    }
    if (use->phase == LIFT_EXIT && (rise < 8 || dist2d(pos, there) > 2.6f)) {
        if (rise >= 8) H->log("lift: %s cleared the cab at %.1f", (char *)b->player + OFF_PLAYER_NAME, pos[1]);
        b->wp = use->nextPoint;
        use->phase = LIFT_IDLE;
        use->cooldown = t + 3500;
        return 0;
    }
    ULONGLONG limit = use->phase == LIFT_RIDE ? (ULONGLONG)(25000 + rise * 450) :
                      use->phase == LIFT_EXIT ? 12000 : use->phase == LIFT_LADDER ? 14000 : 35000;
    if (t - use->since > limit || gap > 12) {
        H->log("lift: %s replanning from %.1f %.1f %.1f (phase %d)", (char *)b->player + OFF_PLAYER_NAME,
               pos[0], pos[1], pos[2], use->phase);
        use->phase = LIFT_IDLE;
        use->cooldown = t + 20000;
        b->npts = 0; b->planAt = 0; b->route = -1; b->lroute = -1;
        return 0;
    }

    if (use->phase == LIFT_RIDE) { b->why = "riding elevator"; return 1; }
    if (use->phase == LIFT_LADDER) {
        b->why = "climbing ladder";
        const float *aim = gap > 0.9f && fabsf(pos[1] - here[1]) < 1.5f ? here : there;
        lift_walk(b, soldier, pos, aim, 0.8f);
        return 1;
    }
    if (use->phase == LIFT_EXIT) {
        b->why = "leaving elevator";
        int end = 1 - use->end;
        if (lift_door(l, end, t) && l->doorOpen[end] && nav_segment_clear(ray_cast, pos, l->door[end]))
            lift_walk(b, soldier, pos, l->door[end], 0.65f);
        return 1;
    }

    b->why = "boarding elevator";
    int end = use->end;
    int haveDoor = lift_door(l, end, t);
    int cabPresent = lift_level_floor(here);
    if (gap < 1.15f && cabPresent) {
        b->why = "operating elevator";
        // Cycle the four cab walls for alternate panel placements without walking out.
        float base = haveDoor ? atan2f(l->door[end][0] - here[0], l->door[end][2] - here[2]) : 0;
        float yaw = base + 0.785398f + 1.570796f * (float)(((t - use->since) / 1700) % 4);
        lift_face(b, soldier, yaw, 0);
        if (fabsf(wrap_pi(yaw - soldier_yaw(soldier))) < 0.4f) lift_interact(b, l, t);
    } else if (haveDoor && l->doorOpen[end] && cabPresent && nav_segment_clear(ray_cast, pos, here)) {
        lift_walk(b, soldier, pos, here, 0.62f);
    } else {
        const float *hall = haveDoor ? l->door[end] : l->landing[end];
        if (dist2d(pos, hall) > 1.3f && nav_segment_clear(ray_cast, pos, hall) && floor_continuous(pos, hall)) {
            lift_walk(b, soldier, pos, hall, 0.65f);
        } else {
            static const float offsets[] = {0, -0.55f, 0.55f, -0.95f, 0.95f};
            float yaw = atan2f(here[0] - pos[0], here[2] - pos[2]) + offsets[((t - use->since) / 1300) % 5];
            lift_face(b, soldier, yaw, 0);
            if (gap < 5.5f && fabsf(wrap_pi(yaw - soldier_yaw(soldier))) < 0.45f) lift_interact(b, l, t);
        }
    }
    return 1;
}
