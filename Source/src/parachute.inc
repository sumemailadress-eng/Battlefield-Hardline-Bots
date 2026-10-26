// Thugbot: off the roof (included by brain.inc after lifts.inc). Downtown's towers: the quick way down is out of a
// window or off the edge, with a parachute.
//
//   Falling: any bot falling fast (over 7 m/s down) with 10 m+ of air under it presses jump - the parachute. Off an
//   edge it chose, 1.5 s after leaving it. Windows in the way of an edge are knifed / shot out.
//   Jumping off: a bot whose target is 15 m+ below it and over 25 m away sideways, with no route (or a route 2.5x+
//   longer than the straight line; always when 25 m+ below or the way down is an elevator), looks every 2 s for an edge within 25 m: a spot whose ground drops 10 m+ just past
//   it, nothing in the way at chest height. It walks there and keeps going over it; the falling rule then opens the
//   chute and it steers towards its target on the way down.

typedef struct Chute { ULONGLONG lookAt, until, jumpAt; float edge[3], dir[2]; int active, glass; float fallTop; int falling; ULONGLONG leftAt; } Chute;
static Chute g_chute[MAX_BOTS];
static unsigned g_chuteJumps, g_chuteOpens;

// Ways out learned from players (the tester: "learn from windows I broke and jumped out or vaulted out of"): where a human
// last stood on a floor before falling 10 m+, and which way they went - kept in Thugbot_learned\<level>.jumps. Bots
// that want down use the nearest one on their floor before looking for an edge of their own.
#define MAX_JUMPS 64
static struct { float p[3], dir[2]; } g_jumps[MAX_JUMPS];
static int g_jumpN, g_jumpsLoaded;
// edges that didn't work (walked there for the whole try and never fell): skipped for 3 minutes
static struct { float p[3]; ULONGLONG until; } g_badEdge[32]; static int g_badEdgeN;
static void jumps_path(char *out, size_t n) { map_path(out, n, "Thugbot_learned", ".jumps"); }
static void jumps_load(void)
{
    if (g_jumpsLoaded || !g_levelKey[0]) return;
    g_jumpsLoaded = 1; g_jumpN = 0; g_badEdgeN = 0;
    char path[160], line[128]; jumps_path(path, sizeof path);
    FILE *f = fopen(path, "r"); if (!f) return;
    while (fgets(line, sizeof line, f) && g_jumpN < MAX_JUMPS)
        if (sscanf(line, "jump %f %f %f %f %f", &g_jumps[g_jumpN].p[0], &g_jumps[g_jumpN].p[1], &g_jumps[g_jumpN].p[2], &g_jumps[g_jumpN].dir[0], &g_jumps[g_jumpN].dir[1]) == 5) g_jumpN++;
    fclose(f);
    if (g_jumpN) H->log("chute: %d way(s) out learned from players on %s", g_jumpN, g_levelKey);
}
static void jumps_level_reset(void) { g_jumpsLoaded = 0; g_jumpN = 0; g_badEdgeN = 0; }
static void jumps_add(const float *p, float dx, float dz, const char *who)
{
    for (int k = 0; k < g_jumpN; k++) if (dist2d(g_jumps[k].p, p) < 2.5f && fabsf(g_jumps[k].p[1] - p[1]) < 2) return;
    if (g_jumpN >= MAX_JUMPS) return;
    memcpy(g_jumps[g_jumpN].p, p, 12); g_jumps[g_jumpN].dir[0] = dx; g_jumps[g_jumpN].dir[1] = dz; g_jumpN++;
    char path[160]; jumps_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) { for (int k = 0; k < g_jumpN; k++) fprintf(f, "jump %.2f %.2f %.2f %.3f %.3f\n", g_jumps[k].p[0], g_jumps[k].p[1], g_jumps[k].p[2], g_jumps[k].dir[0], g_jumps[k].dir[1]); fclose(f); }
    H->log("chute: %s jumped out at %.0f %.0f %.0f - bots will use that way down", who, p[0], p[1], p[2]);
}
static int edge_bad(const float *p, ULONGLONG t) { for (int k = 0; k < g_badEdgeN && k < 32; k++) if (t < g_badEdge[k].until && dist2d(g_badEdge[k].p, p) < 2.5f && fabsf(g_badEdge[k].p[1] - p[1]) < 2) return 1; return 0; }
static float air_below(const float *p);
// humans: the last spot on solid floor (a sample every 250 ms); falling fast with 10 m+ under them = they jumped out there
static void chute_watch_humans(void)
{
    static struct { void *pl; float p[3], prev[3]; int falling; } w[16];
    static ULONGLONG at;
    ULONGLONG t = now_ms();
    if (t - at < 250) return;
    at = t;
    jumps_load();
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        if (is_bot_player(pl) || player_vehicle(pl, NULL)) continue;
        void *s = bot_soldier(pl);
        if (!s) continue;
        int k = 0, freeK = -1;
        while (k < 16 && w[k].pl != pl) { if (!w[k].pl && freeK < 0) freeK = k; k++; }
        if (k == 16) { if (freeK < 0) continue; k = freeK; memset(&w[k], 0, sizeof w[k]); w[k].pl = pl; }
        const float *p = soldier_pos(s), *v = (const float *)((char *)s + OFF_SOLDIER_VELOCITY);
        float air = air_below(p);
        if (air < 1.5f && v[1] > -2) { w[k].falling = 0; memcpy(w[k].prev, w[k].p, 12); memcpy(w[k].p, p, 12); continue; }
        if (!w[k].falling && v[1] < -7 && air > 10 && (w[k].p[0] || w[k].p[2]) && w[k].p[1] - p[1] < 25) {
            w[k].falling = 1;
            float dx = p[0] - w[k].prev[0], dz = p[2] - w[k].prev[2], l = sqrtf(dx * dx + dz * dz);
            if (l > 0.3f) jumps_add(w[k].p, dx / l, dz / l, (char *)pl + OFF_PLAYER_NAME);
        }
    }
}

// Air under p (up to 120 m): metres to the ground below, 120 if none found.
static float air_below(const float *p)
{
    float a[3] = { p[0], p[1] + 0.4f, p[2] }, b[3] = { p[0], p[1] - 120, p[2] }, h[3];   // (from above the feet: an elevator cab floor going down counts)
    return ray_cast(a, b, h, NULL) ? fmaxf(p[1] - h[1], 0) : 120;
}

// Every frame for a live bot on foot, after steering. Returns 1 when it took over the inputs.
static int parachute_tick(int i, Bot *b, void *soldier, const float *pos, ULONGLONG t)
{
    Chute *c = &g_chute[i];
    const float *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
    // falling: open the chute (and steer for the target while coming down)
    // a fall that ended: how fast it came down (a chute: ~5 m/s, none: 20+), to see whether the chute opened
    if (c->falling && v[1] > -1.5f) {
        c->falling = 0;
        static unsigned logged;
        if (logged++ < 30) H->log("chute: %s landed - %.0f m down in %.1f s (%.1f m/s)", (char *)b->player + OFF_PLAYER_NAME, c->fallTop - pos[1], (t - c->jumpAt) / 1000.0, (c->fallTop - pos[1]) / fmaxf(0.1f, (t - c->jumpAt) / 1000.0f));
    }
    // Off the edge it chose: the chute opens 1.5 s after leaving it (the tester: "parachute a second and a half after jumping
    // out" - clear of the building first), sooner if the ground comes within 30 m.
    if (c->active && v[1] < -2.5f && air_below(pos) > 10) { c->active = 0; c->leftAt = t; }
    if (c->leftAt) {
        if (air_below(pos) < 2 || t - c->leftAt > 30000) c->leftAt = 0;
        else if (t - c->leftAt < 1500 && air_below(pos) > 30) { b->fwd = 1; b->strafe = 0; return 1; }   // (falling clear, still pushing away from the wall)
    }
    if (v[1] < -7 && air_below(pos) > 10) {
        if (!c->falling) { c->falling = 1; c->fallTop = pos[1]; c->jumpAt = 0; }
        // Jump and the parachute key, held 250 ms of every 700 (one-frame taps were missed: bots fell 120 m pressing
        // jump every 600 ms). Logged once per fall.
        if (!c->jumpAt || t - c->jumpAt > 5000) { c->jumpAt = t; if (g_chuteOpens++ < 30) H->log("chute: %s opens the parachute %.0f m up", (char *)b->player + OFF_PLAYER_NAME, air_below(pos)); }
        if ((t - c->jumpAt) % 700 < 250) { b->buttons |= BTN_JUMP | BTN_CHUTE; b->action = PP_ACT_JUMP; }
        b->fwd = 1; b->strafe = 0;
        b->turn = turn_input(soldier_yaw(soldier), atan2f(b->target[0] - pos[0], b->target[2] - pos[2]), 2.0f);
        c->active = 0;
        return 1;
    }
    // heading for an edge it picked
    if (c->active) {
        if (g_fight[i].state != CS_IDLE || b->enemy) { c->active = 0; return 0; }   // enemies first: no window-knifing in a fight
        if (t > c->until) {   // never got off: that edge is no good for a while
            c->active = 0;
            g_badEdge[g_badEdgeN % 32].until = t + 180000; memcpy(g_badEdge[g_badEdgeN % 32].p, c->edge, 12); g_badEdgeN++;
            static unsigned logged; if (logged++ < 30) H->log("chute: %s couldn't get off at %.0f %.0f %.0f - skipped for 3 min", (char *)b->player + OFF_PLAYER_NAME, c->edge[0], c->edge[1], c->edge[2]);
            return 0;
        }
        float d = dist2d(pos, c->edge);
        float want = d > 1.0f ? atan2f(c->edge[0] - pos[0], c->edge[2] - pos[2]) : atan2f(c->dir[0], c->dir[1]);
        b->turn = turn_input(soldier_yaw(soldier), want, 3.0f);
        b->fwd = 1; b->strafe = 0;
        if (d < 2.0f) { b->buttons |= BTN_JUMP; b->action = PP_ACT_JUMP; }   // jump off rather than step (clears railings / sills)
        if (c->glass) {   // a window pane between it and the drop: knife it when it's in reach
            float a1[3] = { pos[0], pos[1] + 1.2f, pos[2] }, a2[3] = { pos[0] + c->dir[0] * 1.6f, pos[1] + 1.2f, pos[2] + c->dir[1] * 1.6f };
            if (ray_cast(a1, a2, NULL, NULL)) { b->buttons |= BTN_MELEE; if (((t / 400) & 1)) b->buttons |= BTN_FIRE; }
        }
        return 1;
    }
    // should it jump? target far below and far away, and walking there is no good
    float drop = pos[1] - b->target[1], side = dist2d(pos, b->target);
    if (drop < 15 || side < 25 || t < c->lookAt || g_fight[i].state != CS_IDLE || b->enemy) return 0;
    c->lookAt = t + 2000;
    int pathLift = 0;   // the way down is an elevator ride: jumping is quicker
    for (int k = b->wp; k + 1 < b->npts; k++) if (b->path[k * 3 + 1] - b->path[(k + 1) * 3 + 1] > 15) pathLift = 1;
    if (b->npts > 1 && drop < 25 && !pathLift) {
        float len = 0;
        for (int k = b->wp; k + 1 < b->npts; k++) len += dist2d(&b->path[k * 3], &b->path[(k + 1) * 3]);
        if (len < side * 2.5f) return 0;   // a decent way down on foot
    }
    // an edge within 25 m: in 16 directions, step out 3 m at a time; the first spot with 10 m+ of air past it
    float best = 1e9f; int found = 0;
    // a way out a player used, on this floor within 40 m: that first
    for (int k = 0; k < g_jumpN; k++) {
        if (fabsf(g_jumps[k].p[1] - pos[1]) > 2.5f || edge_bad(g_jumps[k].p, t)) continue;
        float d = dist2d(pos, g_jumps[k].p);
        if (d < 40 && d < best) { best = d; memcpy(c->edge, g_jumps[k].p, 12); c->dir[0] = g_jumps[k].dir[0]; c->dir[1] = g_jumps[k].dir[1]; c->glass = 1; found = 2; }
    }
    for (int dI = 0; dI < 16 && found != 2; dI++) {
        float a = dI * 0.3927f, dx = sinf(a), dz = cosf(a);
        for (float r = 3; r <= 25; r += 3) {
            float at[3] = { pos[0] + dx * r, pos[1], pos[2] + dz * r }, past[3] = { pos[0] + dx * (r + 2.5f), pos[1] + 0.5f, pos[2] + dz * (r + 2.5f) };
            float c1[3] = { pos[0], pos[1] + 1.2f, pos[2] }, c2[3] = { past[0], pos[1] + 1.2f, past[2] };
            float gh[3]; int glass = 0;
            if (ray_cast(c1, c2, gh, NULL)) {                         // a wall / window frame in the way this direction - unless it's glass
                uintptr_t body = *(uintptr_t *)g_lastHit.rest, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
                float c3[3] = { gh[0] + dx * 0.4f, gh[1], gh[2] + dz * 0.4f };
                if (vt != 0x2819d40 || ray_cast(c3, c2, NULL, NULL)) break;   // (thin and clear behind it: a pane)
                glass = 1;   // a window: knife it on the way out
            }
            if (air_below(past) < 10) continue;                       // still roof
            float score = r + dist2d(past, b->target) * 0.5f;         // near, and towards the target
            if (edge_bad(at, t)) break;
            if (score < best) { best = score; memcpy(c->edge, at, 12); c->dir[0] = dx; c->dir[1] = dz; found = 1; c->glass = glass; }
            break;
        }
    }
    if (!found) return 0;
    c->active = 1; c->until = t + 9000;
    if (g_chuteJumps++ < 30) H->log("chute: %s jumps off at %.0f %.0f %.0f (%.0f m above its target)%s", (char *)b->player + OFF_PLAYER_NAME, c->edge[0], c->edge[1], c->edge[2], drop, found == 2 ? " - a way a player used" : "");
    return 1;
}
