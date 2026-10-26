// Thugbot saved routes and the learned-data map (included by brain.inc after movement.inc).
//
//   Routes: for each map, a walking route from every team spawn to every flag and between every pair of flags, planned
//   on the raycast grid once (in the background, one at a time) and saved in Thugbot_routes\<level>.routes next to the
//   server. Spawn spots are where bots appear (clustered within 25 m, counted; kept in the same file). A bot going for
//   a flag joins the saved route that ends there at its nearest point (within 10 m, with a clear straight line to it)
//   instead of planning its own. A route bots keep getting stuck on (3 times) is planned again, round the trouble.
//   File lines: "spawn <team> x y z <seen>" and "route <from> <to> <length m> <n> x,y,z x,y,z ...", from / to being
//   "spawn<team>#<n>" (the nth spawn line) or "flag@x,z". Delete a line (or the file) to have it made again.
//
//   Map page: Thugbot_report\<level>.html (and latest.html), rewritten every minute and on "report": the walkable grid,
//   flags, spawns, saved routes, learned stuck spots, doors / glass, where bots die, and where every bot is now.

#define MAX_SPAWNS 24
#define MAX_ROUTES 800   // (the tester: "maps need many many routes" - every pair three ways: straight, left and right flank)
#define ROUTE_PTS BRAIN_PATH_MAX
#define ROUTE_OWNER 120   // the async planner slot used for building routes (bots use 0..MAX_BOTS-1)
typedef struct Spawn { int team; float p[3]; unsigned seen; } Spawn;
typedef struct SRoute {
    char from[40], to[40];
    float a[3], b[3];        // endpoints (the spawn / flag positions the route was planned between)
    int n, fails;
    float len;
    float pts[ROUTE_PTS][3];
} SRoute;
static Spawn g_spawns[MAX_SPAWNS];
static int g_spawnN;
static SRoute g_routes[MAX_ROUTES];
static int g_routeN, g_routesLoaded, g_routesDirty;
static int g_routeBuild = -1;                 // what is being planned: pair index (see routes_tick)
static float g_routeBuildA[3], g_routeBuildB[3];
static char g_routeBuildFrom[40], g_routeBuildTo[40];
static int g_routeBuildReversed;
static float g_routeBuildVia[3]; static int g_routeBuildHasVia;   // a flank variant: planned A -> via -> B
static unsigned g_routePickSeed;   // the bot picking (spreads bots over a pair's variants)   // second try, planned from the other end (one end can sit on a small island of the grid)
static unsigned g_routesUsed, g_routesBuilt, g_routesFailed;
static int g_routesFull;   // no room for more: stop building (it re-planned the same route every frame - Museum 2026-10-23)
static ULONGLONG g_routeNextTry;
static volatile LONG g_routesRebuild, g_reportNow;

static void routes_path(char *out, size_t n) { map_path(out, n, "Thugbot_routes", ".v8.routes"); }   // (v7: made on grids without crossings through thin walls)

static void routes_save(void)
{
    if (!g_routesDirty || !g_levelKey[0]) return;
    char path[160];
    routes_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Thugbot routes for %s: spawn <team> x y z <seen> / route <from> <to> <length> <points> x,y,z ...\n", g_levelKey);
    for (int i = 0; i < g_spawnN; i++) fprintf(f, "spawn %d %.1f %.1f %.1f %u\n", g_spawns[i].team, g_spawns[i].p[0], g_spawns[i].p[1], g_spawns[i].p[2], g_spawns[i].seen);
    for (int i = 0; i < g_routeN; i++) {
        SRoute *r = &g_routes[i];
        fprintf(f, "route %s %s %.0f %d", r->from, r->to, r->len, r->n);
        for (int k = 0; k < r->n; k++) fprintf(f, " %.1f,%.1f,%.1f", r->pts[k][0], r->pts[k][1], r->pts[k][2]);
        fputc('\n', f);
    }
    fclose(f);
    g_routesDirty = 0;
}

static int parse_label_pos(const char *label, float *out)   // "flag@x,z" / "spawn1@x,z" -> x, z (y unknown: 0)
{
    const char *at = strchr(label, '@');
    out[1] = 0;
    return at && sscanf(at + 1, "%f,%f", &out[0], &out[2]) == 2;
}

static void routes_load(void)
{
    if (g_routesLoaded || !g_levelKey[0]) return;
    g_routesLoaded = 1;
    g_spawnN = g_routeN = 0;
    char path[160];
    routes_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char line[ROUTE_PTS * 24 + 256];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "spawn ", 6) && g_spawnN < MAX_SPAWNS) {
            Spawn *s = &g_spawns[g_spawnN];
            if (sscanf(line + 6, "%d %f %f %f %u", &s->team, &s->p[0], &s->p[1], &s->p[2], &s->seen) == 5) g_spawnN++;
        } else if (!strncmp(line, "route ", 6) && g_routeN < MAX_ROUTES) {
            SRoute *r = &g_routes[g_routeN];
            memset(r, 0, sizeof *r);
            int used = 0, n = 0;
            if (sscanf(line + 6, "%39s %39s %f %d%n", r->from, r->to, &r->len, &n, &used) != 4) continue;
            char *p = line + 6 + used;
            for (int k = 0; k < n && k < ROUTE_PTS; k++) {
                int u = 0;
                if (sscanf(p, " %f,%f,%f%n", &r->pts[k][0], &r->pts[k][1], &r->pts[k][2], &u) != 3) break;
                p += u; r->n++;
            }
            if (r->n < 2) continue;
            parse_label_pos(r->from, r->a); parse_label_pos(r->to, r->b);
            r->a[1] = r->pts[0][1]; r->b[1] = r->pts[r->n - 1][1];
            g_routeN++;
        }
    }
    fclose(f);
    H->log("routes: %s - %d spawn spot(s), %d saved route(s)", path, g_spawnN, g_routeN);
    for (int k = 0; k < g_breachN; k++) if (g_breach[k].kind == 'n') routes_cut_near(g_breach[k].p);   // (saved before those walls were written off)
}

static void routes_level_reset(void)
{
    routes_save();
    g_routesLoaded = 0; g_spawnN = g_routeN = 0; g_routeBuild = -1; g_routesFull = 0;
    nav_cancel_path(ROUTE_OWNER);
}

// A bot (re)spawned at p: count it in its team's spawn spot (a new spot once it is 25 m from every known one).
static void routes_note_spawn(int team, const float *p)
{
    if (team < 1 || team > 2) return;
    for (int i = 0; i < g_spawnN; i++) {
        Spawn *s = &g_spawns[i];
        if (s->team != team || dist2d(s->p, p) > 40) continue;
        // a running mean, so the spot settles in the middle of where bots appear
        float w = 1.0f / (float)(s->seen < 50 ? s->seen + 1 : 50);
        for (int k = 0; k < 3; k++) s->p[k] += (p[k] - s->p[k]) * w;
        s->seen++;
        if (s->seen == 2) g_routesDirty = 1;   // confirmed: routes from it get built
        return;
    }
    if (g_spawnN >= MAX_SPAWNS) return;
    Spawn *s = &g_spawns[g_spawnN++];
    s->team = team; memcpy(s->p, p, 12); s->seen = 1;
}

static int route_find(const char *from, const char *to)
{
    for (int i = 0; i < g_routeN; i++) if (!strcmp(g_routes[i].from, from) && !strcmp(g_routes[i].to, to)) return i;
    return -1;
}

// Endpoints of the routes wanted now: every confirmed spawn spot to every flag, and flag to flag (one direction; bots
// use a route either way). Fills pair `k` (0..); returns 0 past the last.
static int route_pair_base(int k, char *from, char *to, float *a, float *b)
{
    int flags[MAX_CPS], nf = 0;
    flags_survey();   // (g_fi: positions on the floor under each flag)
    for (int f = 0; f < g_cpCount; f++) if (flag_takeable(f)) flags[nf++] = f;
    if (!nf) return 0;
    for (int s = 0; s < g_spawnN; s++) {
        if (g_spawns[s].seen < 2) continue;
        int better = 0;   // spots of the same team seen more often
        for (int o = 0; o < g_spawnN; o++) if (o != s && g_spawns[o].team == g_spawns[s].team && (g_spawns[o].seen > g_spawns[s].seen || (g_spawns[o].seen == g_spawns[s].seen && o < s))) better++;
        if (better >= 6) continue;
        for (int j = 0; j < nf; j++) {
            if (k-- > 0) continue;
            const float *fp = g_fi[flags[j]].pos;
            snprintf(from, 40, "spawn%d#%d", g_spawns[s].team, s);   // by index: the spot's position drifts as spawns are averaged
            snprintf(to, 40, "flag@%.0f,%.0f", fp[0], fp[2]);
            memcpy(a, g_spawns[s].p, 12); memcpy(b, fp, 12);
            return 1;
        }
    }
    for (int i = 0; i < nf; i++)
        for (int j = i + 1; j < nf; j++) {
            if (k-- > 0) continue;
            const float *p = g_fi[flags[i]].pos, *q = g_fi[flags[j]].pos;
            snprintf(from, 40, "flag@%.0f,%.0f", p[0], p[2]);
            snprintf(to, 40, "flag@%.0f,%.0f", q[0], q[2]);
            memcpy(a, p, 12); memcpy(b, q, 12);
            return 1;
        }
    return 0;
}

// Pairs first straight, then each again by a flank: via a point off to the left / right of the straight line's middle
// (30 - 80 m out, on walkable ground of the same level, not water), so bots spread over different ways in.
static int g_basePairs = -1;
static int route_pair(int k, char *from, char *to, float *a, float *b, float *via, int *hasVia)
{
    *hasVia = 0;
    if (route_pair_base(k, from, to, a, b)) return 1;
    int nb = 0; { char f2[40], t2[40]; float a2[3], b2[3]; while (route_pair_base(nb, f2, t2, a2, b2)) nb++; }
    k -= nb;
    if (k >= nb * 2) return 0;
    int side = k % 2; k /= 2;
    if (!route_pair_base(k, from, to, a, b)) return 0;
    float mx = (a[0] + b[0]) / 2, mz = (a[2] + b[2]) / 2, dx = b[0] - a[0], dz = b[2] - a[2], l = sqrtf(dx * dx + dz * dz);
    if (l < 40) { from[0] = 0; return 1; }   // (too short for a flank: skipped)
    float off = fminf(80, fmaxf(30, l * 0.3f)) * (side ? -1.0f : 1.0f), px = -dz / l, pz = dx / l;
    int ok = 0;
    for (float f = 1.0f; f > 0.3f && !ok; f -= 0.25f) {
        float q[3] = { mx + px * off * f, (a[1] + b[1]) / 2, mz + pz * off * f }, sq[3];
        if (g_nav && nav_snap(g_nav, q[0], q[1], q[2], sq) && dist2d(sq, q) < 8 && !nav_is_water(g_nav, sq[0], sq[1], sq[2])) { memcpy(via, sq, 12); ok = 1; }
    }
    if (!ok) { from[0] = 0; return 1; }   // (no ground there: a pair with no route - skipped by the caller)
    size_t ln = strlen(to); snprintf(to + ln, 40 - ln, side ? "~R" : "~L");
    *hasVia = 1;
    return 1;
}

static int route_store(const char *from, const char *to, const float *a, const float *b, const float *pts, int n)
{
    int i = route_find(from, to);
    if (i < 0) { if (g_routeN >= MAX_ROUTES) return -1; i = g_routeN++; }
    SRoute *r = &g_routes[i];
    memset(r, 0, sizeof *r);
    snprintf(r->from, sizeof r->from, "%s", from); snprintf(r->to, sizeof r->to, "%s", to);
    memcpy(r->a, a, 12); memcpy(r->b, b, 12);
    r->n = n < ROUTE_PTS ? n : ROUTE_PTS;
    memcpy(r->pts, pts, (size_t)r->n * 12);
    for (int k = 1; k < r->n; k++) r->len += dist2d(r->pts[k - 1], r->pts[k]);
    g_routesDirty = 1;
    return i;
}

// Every frame once the grid is ready: plan the next missing route (async, its own planner slot), store it when done.
static void routes_tick(void)
{
    routes_load();
    if (InterlockedExchange(&g_routesRebuild, 0)) {
        g_routeN = 0; g_routeBuild = -1; g_routesDirty = 1; g_routesFull = 0; nav_cancel_path(ROUTE_OWNER);
        H->log("routes: rebuilding every route of %s", g_levelKey);
    }
    if (!g_nav || g_navBuilding || !g_levelKey[0] || g_routesFull) return;
    ULONGLONG t = now_ms();
    if (g_routeBuild < 0) {
        if (t < g_routeNextTry) return;
        g_routeNextTry = t + 1000;
        char from[40], to[40]; float a[3], b[3], via[3]; int hasVia = 0;
        for (int k = 0; route_pair(k, from, to, a, b, via, &hasVia); k++) {
            if (!from[0]) continue;
            int i = route_find(from, to);
            if (i >= 0 && (g_routes[i].n == 0 || g_routes[i].fails < 3)) continue;   // (n 0: no route exists - not until "routes rebuild")
            if (i < 0 && route_find(to, from) >= 0) continue;
            g_routeBuild = k; g_routeBuildReversed = hasVia;   // (a flank isn't retried the other way round)
            g_routeBuildHasVia = hasVia; if (hasVia) memcpy(g_routeBuildVia, via, 12);
            strcpy(g_routeBuildFrom, from); strcpy(g_routeBuildTo, to);
            float sa[3], sb[3];
            memcpy(g_routeBuildA, nav_snap(g_nav, a[0], a[1], a[2], sa) ? sa : a, 12);
            memcpy(g_routeBuildB, nav_snap(g_nav, b[0], b[1], b[2], sb) ? sb : b, 12);
            break;
        }
        if (g_routeBuild < 0) { routes_save(); return; }
    }
    static float pts[ROUTE_PTS * 3];
    static int leg1N = -1; static float leg1[ROUTE_PTS * 3];
    int n, deferred, partial;
    if (g_routeBuildHasVia && leg1N < 0) {   // flank: first leg to the via point
        nav_set_path_owner(ROUTE_OWNER);
        n = nav_find_path(g_nav, ray_cast, g_routeBuildA[0], g_routeBuildA[1], g_routeBuildA[2], g_routeBuildVia[0], g_routeBuildVia[1], g_routeBuildVia[2], leg1, ROUTE_PTS / 2);
        deferred = !n && nav_last_deferred(); partial = nav_last_partial();
        nav_set_path_owner(-1);
        if (deferred) return;
        if (n < 2 || partial) { leg1N = -1; n = 0; }
        else { leg1N = n; return; }   // (second leg next frame)
    } else {
        const float *from3 = g_routeBuildHasVia ? g_routeBuildVia : g_routeBuildA;
        nav_set_path_owner(ROUTE_OWNER);
        n = nav_find_path(g_nav, ray_cast, from3[0], from3[1], from3[2], g_routeBuildB[0], g_routeBuildB[1], g_routeBuildB[2], pts, g_routeBuildHasVia ? ROUTE_PTS - leg1N : ROUTE_PTS);
        deferred = !n && nav_last_deferred(); partial = nav_last_partial();
        nav_set_path_owner(-1);
        if (deferred) return;
        if (g_routeBuildHasVia) {
            if (n >= 2 && !partial && leg1N + n - 1 <= ROUTE_PTS) {
                memmove(&pts[(leg1N - 1) * 3], pts, (size_t)n * 12); memcpy(pts, leg1, (size_t)(leg1N - 1) * 12); n += leg1N - 1;
            } else n = 0;
            leg1N = -1;
        }
    }
    if (n >= 2 && !partial) {
        center_path(pts, n, 1);
        int k = route_store(g_routeBuildFrom, g_routeBuildTo, g_routeBuildA, g_routeBuildB, pts, n);
        if (k < 0) { g_routesFull = 1; g_routeBuild = -1; H->log("routes: no room for more routes (%d) - building stops for this level", g_routeN); return; }
        g_routesBuilt++;
        H->log("routes: %s -> %s, %d points, %.0f m", g_routeBuildFrom, g_routeBuildTo, g_routes[k].n, g_routes[k].len);
    } else if (!g_routeBuildReversed) {
        char tmp[40]; float v[3];
        strcpy(tmp, g_routeBuildFrom); strcpy(g_routeBuildFrom, g_routeBuildTo); strcpy(g_routeBuildTo, tmp);
        memcpy(v, g_routeBuildA, 12); memcpy(g_routeBuildA, g_routeBuildB, 12); memcpy(g_routeBuildB, v, 12);
        g_routeBuildReversed = 1;
        return;
    } else {
        g_routesFailed++;
        // Remember the failure as an empty route so it isn't retried every second (a later "routes rebuild" retries).
        // (an existing route being re-planned - stuck on 3 times, or through a cut crossing - that can't be re-made is
        // emptied too: it was kept with fails 3 and re-tried every frame, 91 times in 2 minutes on Code Blue)
        int had = route_find(g_routeBuildFrom, g_routeBuildTo);
        if (had < 0) had = route_find(g_routeBuildTo, g_routeBuildFrom);
        if (had >= 0) { g_routes[had].n = 0; g_routes[had].fails = 3; g_routesDirty = 1; }
        else if (g_routeN < MAX_ROUTES) {
            SRoute *r = &g_routes[g_routeN++];
            memset(r, 0, sizeof *r);
            strcpy(r->from, g_routeBuildFrom); strcpy(r->to, g_routeBuildTo); r->fails = 3; r->n = 0;
        }
        H->log("routes: no %sroute %s -> %s (%s)", partial ? "complete " : "", g_routeBuildFrom, g_routeBuildTo, nav_last_error());
    }
    g_routeBuild = -1;
}

// A bot at pos going to target: the saved route that ends (either way round) within 10 m of the target and passes
// within 10 m of the bot, joined at its nearest point. Copies the rest of it into path; returns the point count.
static int routes_pick(const float *pos, const float *target, float *path, int maxPts, int *routeOut)
{
    // candidates: the cheapest join per route and direction (getting on + the rest of the route)
    struct { float cost; int r, k, rev; } cand[16];
    int nc = 0;
    int best = -1, bestK = 0, bestRev = 0; float bestCost = 1e9f;
    for (int i = 0; i < g_routeN; i++) {
        SRoute *r = &g_routes[i];
        if (r->n < 2 || r->fails >= 3) continue;
        for (int rev = 0; rev < 2; rev++) {
            const float *end = rev ? r->pts[0] : r->pts[r->n - 1];
            if (dist2d(end, target) > 10) continue;
            best = -1; bestCost = 1e9f;
            for (int k = 0; k < r->n; k++) {
                const float *q = r->pts[rev ? r->n - 1 - k : k];
                float d = dist2d(pos, q);
                if (d > 35 || fabsf(q[1] - pos[1]) > 3) continue;   // (35 m: was 20 - more bots get on a saved route)
                // cost: getting on + the rest of the route
                float rest = 0;
                for (int m = k + 1; m < r->n; m++) rest += dist2d(r->pts[rev ? r->n - 1 - m : m], r->pts[rev ? r->n - m : m - 1]);
                // each bot sees each route up to 50% longer or shorter than it is (fixed per bot and route): bots spread over
                // a pair's straight / flank variants instead of all taking the cheapest
                float spread = 1.0f + 0.5f * (float)(((g_routePickSeed * 2654435761u) ^ ((unsigned)i * 40503u)) % 1000) / 1000.0f;
                if ((d * 2 + rest) * spread < bestCost) { bestCost = (d * 2 + rest) * spread; best = i; bestK = k; bestRev = rev; }
            }
            if (best >= 0 && nc < 16) { cand[nc].cost = bestCost; cand[nc].r = best; cand[nc].k = bestK; cand[nc].rev = bestRev; nc++; }
        }
    }
    // cheapest first; the first one we can walk onto in a straight line wins (at most 4 checked)
    best = -1;
    for (int tries = 0; tries < 8 && best < 0; tries++) {
        int m = -1;
        for (int c = 0; c < nc; c++) if (cand[c].r >= 0 && (m < 0 || cand[c].cost < cand[m].cost)) m = c;
        if (m < 0) break;
        SRoute *r = &g_routes[cand[m].r];
        const float *join = r->pts[cand[m].rev ? r->n - 1 - cand[m].k : cand[m].k];
        if (dist2d(pos, join) <= 2.5f || (g_nav && !g_navBuilding ? nav_direct_route(g_nav, ray_cast, pos, join) : dist2d(pos, join) < 6 && nav_segment_clear(ray_cast, pos, join)))
            { best = cand[m].r; bestK = cand[m].k; bestRev = cand[m].rev; }
        cand[m].r = -1;
    }
    if (best < 0) return 0;
    SRoute *r = &g_routes[best];
    int n = 0;
    for (int k = bestK; k < r->n && n < maxPts; k++, n++) memcpy(&path[n * 3], r->pts[bestRev ? r->n - 1 - k : k], 12);
    if (n < maxPts && dist2d(&path[(n - 1) * 3], target) > 1.5f) { memcpy(&path[n * 3], target, 12); n++; }
    *routeOut = best;
    g_routesUsed++;
    return n;
}

// No saved route close enough to walk onto: the nearest point (within 150 m, the same floor give or take 6 m) of the
// cheapest saved route that ends at the target - the bot plans its own way to there and follows the route from it
// (the tester: "make sure routes are always prioritized"). Returns the route, or -1.
static int routes_far_join(const float *pos, const float *target, int *kOut, int *revOut, float *joinOut)
{
    int best = -1; float bestCost = 1e9f;
    for (int i = 0; i < g_routeN; i++) {
        SRoute *r = &g_routes[i];
        if (r->n < 2 || r->fails >= 3) continue;
        for (int rev = 0; rev < 2; rev++) {
            const float *end = rev ? r->pts[0] : r->pts[r->n - 1];
            if (dist2d(end, target) > 10) continue;
            float rest = 0;
            for (int k = r->n - 1; k >= 0; k--) {   // from the end backwards, so `rest` is the length still to walk
                int kk = rev ? r->n - 1 - k : k;
                if (k < r->n - 1) { int kn = rev ? r->n - 2 - k : k + 1; rest += dist2d(r->pts[kk], r->pts[kn]); }
                float d = dist2d(pos, r->pts[kk]);
                if (d > 300 || fabsf(r->pts[kk][1] - pos[1]) > 6) continue;
                float cost = d * 1.5f + rest;
                if (cost < bestCost && cost < dist2d(pos, target) * 2.5f + 40) { bestCost = cost; best = i; *kOut = k; *revOut = rev; memcpy(joinOut, r->pts[kk], 12); }
            }
        }
    }
    return best;
}
static int routes_append(int ri, int k, int rev, float *path, int n, int maxPts, const float *target)
{
    SRoute *r = &g_routes[ri];
    for (int m = k + 1; m < r->n && n < maxPts; m++) memcpy(&path[n++ * 3], r->pts[rev ? r->n - 1 - m : m], 12);
    if (n < maxPts && dist2d(&path[(n - 1) * 3], target) > 1.5f) { memcpy(&path[n * 3], target, 12); n++; }
    g_routesUsed++;
    return n;
}

// A crossing was cut out of the grid at p (fence_cut_at): saved routes and bots' current routes through it are planned
// again (they were made when the grid still let them through a wall there).
static float seg_dist2d(const float *p, const float *a, const float *b)
{
    float ax = b[0] - a[0], az = b[2] - a[2], l2 = ax * ax + az * az, u = l2 > 1e-6f ? ((p[0] - a[0]) * ax + (p[2] - a[2]) * az) / l2 : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    float x = a[0] + ax * u - p[0], z = a[2] + az * u - p[2];
    return sqrtf(x * x + z * z);
}
static void routes_cut_near(const float *p)
{
    int n = 0;
    for (int k = 0; k < g_routeN; k++) {
        SRoute *r = &g_routes[k];
        if (r->fails >= 3) continue;
        for (int j = 1; j < r->n; j++)
            if (seg_dist2d(p, r->pts[j - 1], r->pts[j]) < 2.5f && fabsf(r->pts[j][1] - p[1]) < 2.5f) { r->fails = 3; g_routesDirty = 1; n++; break; }
    }
    int bots = 0;
    for (int i = 0; i < H->botCount; i++) {
        Bot *b = &g_bot[i];
        for (int j = b->wp > 0 ? b->wp - 1 : 0; j + 1 < b->npts; j++)
            if (seg_dist2d(p, &b->path[j * 3], &b->path[(j + 1) * 3]) < 2.5f && fabsf(b->path[j * 3 + 1] - p[1]) < 2.5f) { b->npts = 0; b->planAt = 0; b->route = -1; bots++; break; }
    }
    if (n || bots) { static unsigned logged; if (logged++ < 40) H->log("routes: %d saved route(s) and %d bot route(s) went through the cut at %.0f %.0f %.0f - planning them again", n, bots, p[0], p[1], p[2]); }
}

static void route_note_stuck(int k)
{
    if (k < 0 || k >= g_routeN) return;
    if (++g_routes[k].fails == 3) {
        H->log("routes: bots keep getting stuck on %s -> %s - planning it again", g_routes[k].from, g_routes[k].to);
        g_routesDirty = 1;
    }
}

static void routes_dump(void)
{
    int ok = 0;
    for (int i = 0; i < g_routeN; i++) ok += g_routes[i].n >= 2 && g_routes[i].fails < 3;
    H->log("routes: %d spawn spot(s), %d saved route(s) usable of %d, %u built / %u failed this run, bots joined one %u time(s)",
           g_spawnN, ok, g_routeN, g_routesBuilt, g_routesFailed, g_routesUsed);
}

// ---- the learned-data map page ----
static void report_write(void)
{
    if (!g_levelKey[0]) return;
    CreateDirectoryA("Thugbot_report", NULL);
    char path[160];
    snprintf(path, sizeof path, "Thugbot_report\\%s.html", g_levelKey);
    FILE *f = fopen(path, "w");
    if (!f) return;
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>Thugbot - %s</title><style>"
               ":root{--bg:#f6f5f2;--fg:#1d1d1f;--mut:#6b6b70;--panel:#fff;--line:#d9d7d2}"
               "@media (prefers-color-scheme:dark){:root{--bg:#141416;--fg:#ececef;--mut:#9a9aa2;--panel:#1d1d21;--line:#33333a}}"
               "body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}"
               "header{padding:14px 16px 6px}h1{font-size:18px;margin:0}p{margin:4px 0;color:var(--mut)}"
               ".wrap{display:flex;gap:16px;padding:8px 16px 24px;flex-wrap:wrap}"
               "canvas{background:var(--panel);border:1px solid var(--line);border-radius:8px;max-width:100%%;height:auto;touch-action:none}"
               ".side{min-width:240px;flex:1;max-width:420px}.side label{display:flex;gap:8px;align-items:center;padding:3px 0}"
               ".sw{width:12px;height:12px;border-radius:3px;display:inline-block}table{border-collapse:collapse;width:100%%;font-size:13px}"
               "td,th{text-align:left;padding:3px 6px;border-bottom:1px solid var(--line)}#tip{min-height:20px;color:var(--mut)}"
               "</style></head><body><header><h1>Thugbot learned map: %s</h1><p>Written %s. Refreshes every minute while the server runs "
               "(reload the page). Hover the map for details.</p></header><div class=\"wrap\"><canvas id=\"c\" width=\"900\" height=\"900\"></canvas>"
               "<div class=\"side\"><div id=\"layers\"></div><p id=\"tip\"></p><h3>Summary</h3><table id=\"sum\"></table></div></div><script>\nconst D=",
            g_levelKey, g_levelKey, when);
    // ---- data ----
    fprintf(f, "{\"level\":\"%s\"", g_levelKey);
    if (g_nav && !g_navBuilding) {
        NavGrid *g = g_nav;
        fprintf(f, ",\"grid\":{\"x0\":%.2f,\"z0\":%.2f,\"cell\":%.2f,\"w\":%d,\"h\":%d,\"cells\":\"", g->x0, g->z0, g->cell, g->w, g->h);
        // per cell: '.' nothing walkable, otherwise a letter for the number of walkable floors (a = 1 ... f = 6)
        for (int c = 0; c < g->w * g->h; c++) {
            int floors = 0;
            for (int l = 0; l < NAV_LAYERS; l++) floors += (g->flags[c * NAV_LAYERS + l] & NAV_WALKABLE) != 0;
            fputc(floors ? 'a' + floors - 1 : '.', f);
        }
        fputs("\"}", f);
    }
    fputs(",\"flags\":[", f);
    for (int k = 0, first = 1; k < g_cpCount; k++) {
        if (!flag_takeable(k)) continue;
        const float *p = (const float *)((char *)g_cps[k] + OFF_CP_POSITION);
        fprintf(f, "%s{\"x\":%.1f,\"z\":%.1f,\"r\":%.1f,\"owner\":%d}", first ? "" : ",", p[0], p[2], FIELD(g_cps[k], OFF_CP_RADIUS, float), FIELD(g_cps[k], OFF_CP_OWNER_TEAM, int));
        first = 0;
    }
    fputs("],\"spawns\":[", f);
    for (int i = 0; i < g_spawnN; i++) fprintf(f, "%s{\"team\":%d,\"x\":%.1f,\"z\":%.1f,\"seen\":%u}", i ? "," : "", g_spawns[i].team, g_spawns[i].p[0], g_spawns[i].p[2], g_spawns[i].seen);
    fputs("],\"routes\":[", f);
    for (int i = 0, first = 1; i < g_routeN; i++) {
        SRoute *r = &g_routes[i];
        if (r->n < 2) continue;
        fprintf(f, "%s{\"from\":\"%s\",\"to\":\"%s\",\"len\":%.0f,\"fails\":%d,\"pts\":[", first ? "" : ",", r->from, r->to, r->len, r->fails);
        for (int k = 0; k < r->n; k++) fprintf(f, "%s%.1f,%.1f", k ? "," : "", r->pts[k][0], r->pts[k][2]);
        fputs("]}", f);
        first = 0;
    }
    fputs("],\"human\":[", f);
    for (int i = 0, first = 1; g_learn && i < g_learn->nRoutes; i++) {
        BLRoute *r = &g_learn->routes[i];
        if (r->kind != BL_FOOT || r->n < 2) continue;
        fprintf(f, "%s{\"uses\":%u,\"pts\":[", first ? "" : ",", r->uses);
        for (int k = 0; k < r->n; k++) fprintf(f, "%s%.1f,%.1f", k ? "," : "", r->pts[k][0], r->pts[k][2]);
        fputs("]}", f);
        first = 0;
    }
    fputs("],\"stuck\":[", f);
    for (int i = 0, first = 1; g_learn && i < g_learn->nSpots; i++) {
        BLSpot *s = &g_learn->spots[i];
        if (s->kind != BL_FOOT) continue;
        fprintf(f, "%s{\"x\":%.1f,\"z\":%.1f,\"seen\":%u,\"passed\":%u,\"failed\":%u,\"jumps\":%u,\"bad\":%d}", first ? "" : ",", s->p[0], s->p[2], s->seen, s->passed, s->failed, s->jumps, bl_spot_bad(s));
        first = 0;
    }
    fputs("],\"heat\":[", f);
    for (int i = 0; g_learn && i < g_learn->nHeat; i++) fprintf(f, "%s[%d,%d,%.1f]", i ? "," : "", g_learn->heat[i].cx, g_learn->heat[i].cz, g_learn->heat[i].deaths);
    fputs("],\"doors\":[", f);
    for (int i = 0; i < g_breachN; i++) fprintf(f, "%s{\"x\":%.1f,\"z\":%.1f,\"kind\":\"%c\"}", i ? "," : "", g_breach[i].p[0], g_breach[i].p[2], g_breach[i].kind);
    fputs("],\"bots\":[", f);
    for (int i = 0, first = 1; i < H->botCount; i++) {
        void *s = bot_soldier(H->bots[i]);
        if (!s) continue;
        const float *p = soldier_pos(s);
        fprintf(f, "%s{\"name\":\"", first ? "" : ",");
        for (const char *c = (char *)H->bots[i] + OFF_PLAYER_NAME; *c && c < (char *)H->bots[i] + OFF_PLAYER_NAME + 32; c++) if (*c != '"' && *c != '\\' && (unsigned char)*c >= 32) fputc(*c, f);
        fprintf(f, "\",\"team\":%d,\"x\":%.1f,\"z\":%.1f,\"tx\":%.1f,\"tz\":%.1f}", FIELD(H->bots[i], OFF_PLAYER_TEAM, int), p[0], p[2], g_bot[i].target[0], g_bot[i].target[2]);
        first = 0;
    }
    fputs("]};\n", f);
    // ---- drawing ----
    fputs(
"const L=[['grid','Walkable ground','#9aa39a',1],['routes','Saved routes','#2f7de1',1],['human','Routes learned from players','#f08c00',1],['heat','Where bots die','#d9480f',1],"
"['stuck','Stuck spots (red = avoided)','#c92a2a',1],['doors','Doors / glass','#7048e8',1],['spawns','Spawns','#0b7285',1],['flags','Flags','#e8a400',1],['bots','Bots now','#1d1d1f',1]];\n"
"const on={};const lay=document.getElementById('layers');L.forEach(([k,n,c])=>{on[k]=true;const l=document.createElement('label');"
"l.innerHTML=`<input type=checkbox checked> <span class=sw style=\"background:${c}\"></span> ${n}`;l.querySelector('input').onchange=e=>{on[k]=e.target.checked;draw()};lay.appendChild(l)});\n"
"const cv=document.getElementById('c'),ctx=cv.getContext('2d');let pts=[];D.routes.forEach(r=>{for(let i=0;i<r.pts.length;i+=2)pts.push([r.pts[i],r.pts[i+1]])});"
"D.flags.forEach(f=>pts.push([f.x,f.z]));D.spawns.forEach(s=>pts.push([s.x,s.z]));D.bots.forEach(b=>pts.push([b.x,b.z]));\n"
"let b={x0:1e9,z0:1e9,x1:-1e9,z1:-1e9};if(D.grid){const g=D.grid;b={x0:g.x0,z0:g.z0,x1:g.x0+g.w*g.cell,z1:g.z0+g.h*g.cell}}"
"else pts.forEach(([x,z])=>{b.x0=Math.min(b.x0,x-20);b.z0=Math.min(b.z0,z-20);b.x1=Math.max(b.x1,x+20);b.z1=Math.max(b.z1,z+20)});\n"
"const S=Math.min(900/(b.x1-b.x0),900/(b.z1-b.z0));cv.width=Math.ceil((b.x1-b.x0)*S);cv.height=Math.ceil((b.z1-b.z0)*S);\n"
"const X=x=>(x-b.x0)*S,Z=z=>cv.height-(z-b.z0)*S;const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v);\n"
"function dot(x,z,r,fill,stroke){ctx.beginPath();ctx.arc(X(x),Z(z),r,0,7);if(fill){ctx.fillStyle=fill;ctx.fill()}if(stroke){ctx.strokeStyle=stroke;ctx.lineWidth=1.5;ctx.stroke()}}\n"
"function draw(){ctx.clearRect(0,0,cv.width,cv.height);const fg=css('--fg');\n"
" if(on.grid&&D.grid){const g=D.grid;for(let z=0;z<g.h;z++)for(let x=0;x<g.w;x++){const ch=g.cells[z*g.w+x];if(ch=='.')continue;const n=ch.charCodeAt(0)-96;"
"ctx.fillStyle=`rgba(120,135,120,${0.18+0.12*n})`;ctx.fillRect(X(g.x0+x*g.cell-g.cell/2),Z(g.z0+(z+1)*g.cell-g.cell/2),g.cell*S+0.5,g.cell*S+0.5)}}\n"
" if(on.heat)D.heat.forEach(([cx,cz,d])=>{ctx.fillStyle=`rgba(217,72,15,${Math.min(0.75,0.12+d*0.08)})`;ctx.fillRect(X(cx*10),Z(cz*10+10),10*S,10*S)});\n"
" if(on.routes)D.routes.forEach(r=>{ctx.beginPath();for(let i=0;i<r.pts.length;i+=2){const x=X(r.pts[i]),z=Z(r.pts[i+1]);i?ctx.lineTo(x,z):ctx.moveTo(x,z)}"
"ctx.strokeStyle=r.fails>=3?'#c92a2a':r.from.startsWith('spawn')?'#2f7de1':'#12b886';ctx.lineWidth=2;ctx.stroke()});\n"
" if(on.human)D.human.forEach(r=>{ctx.beginPath();for(let i=0;i<r.pts.length;i+=2){const x=X(r.pts[i]),z=Z(r.pts[i+1]);i?ctx.lineTo(x,z):ctx.moveTo(x,z)}"
"ctx.strokeStyle='#f08c00';ctx.setLineDash([6,4]);ctx.lineWidth=1.5+Math.min(3,r.uses*0.5);ctx.stroke();ctx.setLineDash([])});\n"
" if(on.stuck)D.stuck.forEach(s=>dot(s.x,s.z,2+Math.min(5,s.seen),s.bad?'#c92a2a':'rgba(201,42,42,0.35)'));\n"
" if(on.doors)D.doors.forEach(d=>dot(d.x,d.z,4,d.kind=='o'?'#7048e8':d.kind=='b'?'#3bc9db':'#868e96'));\n"
" if(on.spawns)D.spawns.forEach(s=>{ctx.fillStyle=s.team==1?'#1c7ed6':'#e03131';ctx.fillRect(X(s.x)-6,Z(s.z)-6,12,12);ctx.fillStyle=fg;ctx.font='12px system-ui';ctx.fillText('spawn T'+s.team,X(s.x)+8,Z(s.z)+4)});\n"
" if(on.flags)D.flags.forEach((f,i)=>{dot(f.x,f.z,Math.max(6,f.r*S*0.5),'rgba(232,164,0,0.25)','#e8a400');ctx.fillStyle=fg;ctx.font='bold 13px system-ui';"
"ctx.fillText(String.fromCharCode(65+i)+(f.owner?' (T'+f.owner+')':''),X(f.x)-6,Z(f.z)+5)});\n"
" if(on.bots)D.bots.forEach(o=>{ctx.beginPath();ctx.moveTo(X(o.x),Z(o.z));ctx.lineTo(X(o.tx),Z(o.tz));ctx.strokeStyle='rgba(128,128,128,0.35)';ctx.lineWidth=1;ctx.stroke();"
"dot(o.x,o.z,4,o.team==1?'#1c7ed6':'#e03131','#fff')});}\n"
"draw();matchMedia('(prefers-color-scheme: dark)').addEventListener('change',draw);\n"
"const tip=document.getElementById('tip');cv.addEventListener('mousemove',e=>{const r=cv.getBoundingClientRect(),k=cv.width/r.width,mx=(e.clientX-r.left)*k,mz=(e.clientY-r.top)*k;"
"const wx=mx/S+b.x0,wz=(cv.height-mz)/S+b.z0;let best=null,bd=10;const near=(x,z,t)=>{const d=Math.hypot(X(x)-mx,Z(z)-mz);if(d<bd){bd=d;best=t}};\n"
"D.bots.forEach(o=>near(o.x,o.z,`Bot ${o.name} (team ${o.team})`));D.stuck.forEach(s=>near(s.x,s.z,`Stuck spot: stuck ${s.seen}x, got through ${s.passed}x (${s.jumps} with a jump), failed ${s.failed}x${s.bad?' - avoided':''}`));"
"D.doors.forEach(d=>near(d.x,d.z,d.kind=='o'?'Door: opens with E':d.kind=='b'?'Glass / panel: breaks':'Panel: stays shut'));D.spawns.forEach(s=>near(s.x,s.z,`Team ${s.team} spawn, ${s.seen} spawns seen`));"
"D.flags.forEach((f,i)=>near(f.x,f.z,`Flag ${String.fromCharCode(65+i)}, owner ${f.owner?'team '+f.owner:'nobody'}`));"
"tip.textContent=best||`x ${wx.toFixed(0)}, z ${wz.toFixed(0)}`});\n"
"const rows=[['Saved routes',D.routes.length],['Routes learned from players',D.human.length],['Spawn spots',D.spawns.length],['Stuck spots',D.stuck.length+' ('+D.stuck.filter(s=>s.bad).length+' avoided)'],"
"['Doors (open with E)',D.doors.filter(d=>d.kind=='o').length],['Glass / panels that break',D.doors.filter(d=>d.kind=='b').length],['Panels that stay shut',D.doors.filter(d=>d.kind=='n').length],"
"['Death cells',D.heat.length],['Bots alive',D.bots.length]];document.getElementById('sum').innerHTML=rows.map(r=>`<tr><td>${r[0]}</td><td>${r[1]}</td></tr>`).join('');\n"
"</script></body></html>\n", f);
    fclose(f);
    CopyFileA(path, "Thugbot_report\\latest.html", FALSE);
}
