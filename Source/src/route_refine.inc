// Thugbot: learned routes pulled straight (included by brain.inc after human_routes.inc).
//
// the tester 2026-10-26 (a drawing: a red wandering line from spawn to a flag, a clean blue one beside it): "Red line is where
// the bots go and eventually find a route to a flag - that route should then be intelligently adapted to be like the
// blue line". A walked trip wanders: it hugs walls, weaves round other players, takes the detours of exploring. Pull it
// tight: from each kept point, the furthest later point a soldier can walk to in a straight line (the grid says walkable
// all the way, body-wide knee / chest rays clear, floor under every metre, rising no more than 1 in 2) is the next kept
// point. Where the trail climbed or dropped steeply (stairs, a vault, a jump down) and at doors / panes / learned jump
// spots the walked points stay as they are. The straight legs are filled in with a point every 4 m (route matching and
// joining work on points). A few checks a frame: new trips are straightened before they are learned, and learned routes
// already on disk (players' walks, earlier trips) are worked through in the background - so a route found by exploring
// turns into the direct one. "refine 0/1" switches it, "refine" logs what it did.
#define RF_QUEUE 8
#define RF_LEG_MAX 45.0f
typedef struct RefineJob {
    int n, m, a, j, best, fails, route; unsigned hash;
    float in[BL_ROUTE_PTS][3], out[BL_ROUTE_PTS][3];
    unsigned char keep[BL_ROUTE_PTS];
} RefineJob;
static RefineJob g_rf;
static int g_rfBusy, g_refineOn = 1;
static struct { int n; float pts[BL_ROUTE_PTS][3]; } g_rfQueue[RF_QUEUE];
static int g_rfQn;
static unsigned g_rfDone[BL_MAX_ROUTES * 2]; static int g_rfDoneN;   // learned routes already straightened this run (hashes)
static unsigned g_rfTrips, g_rfRoutes, g_rfChecks, g_rfWhy[4], g_rfKept; static float g_rfBefore, g_rfAfter;
static ULONGLONG g_rfScanAt;

static int floor_continuous(const float *a, const float *b);   // (brain.inc, below)

static unsigned rf_hash(const BLRoute *r)
{
    unsigned h = 2166136261u ^ (unsigned)r->n;
    for (int k = 0; k < r->n; k += r->n > 8 ? r->n / 8 : 1)
        for (int c = 0; c < 3; c += 2) { h ^= (unsigned)(int)(r->pts[k][c] * 4); h *= 16777619u; }
    h ^= (unsigned)(int)(r->pts[r->n - 1][0] * 4); h *= 16777619u;
    return h;
}
static int rf_done(unsigned h) { for (int k = 0; k < g_rfDoneN; k++) if (g_rfDone[k] == h) return 1; return 0; }
static void rf_mark(unsigned h) { if (g_rfDoneN < (int)(sizeof g_rfDone / sizeof g_rfDone[0])) g_rfDone[g_rfDoneN++] = h; }

static float rf_len(const float (*p)[3], int n) { float l = 0; for (int k = 1; k < n; k++) l += dist2d(p[k - 1], p[k]); return l; }

// New trip (a bot's clean one): straightened before it is learned.
static void refine_trip(const float (*pts)[3], int n)
{
    if (!g_refineOn || g_rfQn >= RF_QUEUE || n < 3) { if (g_learn) bl_add_route(g_learn, BL_FOOT, pts, n, (long)time(NULL)); return; }
    g_rfQueue[g_rfQn].n = n; memcpy(g_rfQueue[g_rfQn].pts, pts, (size_t)n * 12); g_rfQn++;
}

static void rf_start(const float (*pts)[3], int n, int route)
{
    RefineJob *j = &g_rf;
    j->n = n > BL_ROUTE_PTS ? BL_ROUTE_PTS : n; j->route = route;
    memcpy(j->in, pts, (size_t)j->n * 12);
    memset(j->keep, 0, sizeof j->keep);
    j->keep[0] = j->keep[j->n - 1] = 1;
    for (int k = 1; k < j->n; k++) {   // steep: stairs, a vault, a drop - keep the walked points on both sides
        float d = dist2d(j->in[k - 1], j->in[k]), dy = fabsf(j->in[k][1] - j->in[k - 1][1]);
        if (dy > 0.4f + 0.5f * d) j->keep[k - 1] = j->keep[k] = 1;
    }
    for (int k = 1; k < j->n - 1; k++) {
        const float *p = j->in[k];
        for (int e = 0; e < g_breachN && !j->keep[k]; e++)   // a door / pane (not one written off)
            if (g_breach[e].kind != 'n' && dist2d(g_breach[e].p, p) < 2.5f && fabsf(g_breach[e].p[1] - 1.2f - p[1]) < 2.0f) j->keep[k] = 1;
        for (int s = 0; g_learn && s < g_learn->nSpots && !j->keep[k]; s++) {   // a learned jump spot
            const BLSpot *sp = &g_learn->spots[s];
            if (sp->kind == BL_FOOT && bl_spot_jump(sp) && dist2d(sp->p, p) < 2.5f && fabsf(sp->p[1] - p[1]) < 1.5f) j->keep[k] = 1;
        }
    }
    for (int k = 0; k < j->n; k++) g_rfKept += j->keep[k];
    memcpy(j->out[0], j->in[0], 12); j->m = 1;
    j->a = 0; j->j = 2; j->best = 1; j->fails = 0;
    g_rfBusy = 1;
}

static int rf_ok(const float *a, const float *b)
{
    g_rfChecks++;
    // (a hillside rises steadily: up to 1 in 2 is fine, the floor check below follows the ground; a big step between
    // floors over a short distance is not - that is stairs / a ledge, walked as it was)
    float dy = fabsf(a[1] - b[1]);
    if (dy > 1.5f && dy > 0.5f * dist2d(a, b)) { g_rfWhy[0]++; return 0; }
    if (!nav_direct_route(g_nav, NULL, a, b)) { g_rfWhy[1]++; return 0; }
    if (!nav_segment_clear(ray_cast, a, b)) { g_rfWhy[2]++; return 0; }
    if (!floor_continuous(a, b)) { g_rfWhy[3]++; return 0; }
    return 1;
}

// Adds b (and points every 4 m on the straight leg from the last output point to it).
static void rf_emit(RefineJob *j, const float *b)
{
    const float *a = j->out[j->m - 1];
    float d = dist2d(a, b);
    int steps = (int)(d / 4.0f);
    for (int s = 1; s <= steps && j->m < BL_ROUTE_PTS - 1; s++) {
        float f = (float)s / (steps + 1), *o = j->out[j->m++];
        o[0] = a[0] + (b[0] - a[0]) * f; o[1] = a[1] + (b[1] - a[1]) * f; o[2] = a[2] + (b[2] - a[2]) * f;
    }
    if (j->m < BL_ROUTE_PTS) memcpy(j->out[j->m++], b, 12);
}

static void rf_finish(RefineJob *j)
{
    float before = rf_len((const float (*)[3])j->in, j->n), after = rf_len((const float (*)[3])j->out, j->m);
    g_rfBusy = 0;
    if (!g_learn) return;
    if (j->route < 0) {   // a new trip
        int r = bl_add_route(g_learn, BL_FOOT, (const float (*)[3])j->out, j->m, (long)time(NULL));
        g_rfTrips++; g_rfBefore += before; g_rfAfter += after;
        if (r >= 0) rf_mark(rf_hash(&g_learn->routes[r]));
        static unsigned logged;
        if (logged++ < 40) H->log("refine: trip %.0f m -> %.0f m (%d walked points, %d kept)%s", before, after, j->n, j->m, r >= 0 ? " - learned" : " - same way as a known route");
        return;
    }
    // A learned route: still the same record (the file can be reloaded meanwhile)?
    if (j->route >= g_learn->nRoutes || rf_hash(&g_learn->routes[j->route]) != j->hash) return;
    BLRoute *rt = &g_learn->routes[j->route];
    if (after < before - 2.0f) {
        memcpy(rt->pts, j->out, (size_t)j->m * 12); rt->n = j->m;
        g_learn->dirty = 1;
        g_rfRoutes++; g_rfBefore += before; g_rfAfter += after;
        static unsigned logged;
        if (logged++ < 40) H->log("refine: learned route %d straightened %.0f m -> %.0f m (%d -> %d points)", j->route, before, after, j->n, j->m);
    }
    rf_mark(rf_hash(rt));
}

// Every frame: up to `budget` straight-line checks.
static void refine_tick(void)
{
    if (!g_refineOn || !g_nav || g_navBuilding || !g_learn) return;
    if (!g_rfBusy) {
        if (g_rfQn) { rf_start((const float (*)[3])g_rfQueue[0].pts, g_rfQueue[0].n, -1); memmove(&g_rfQueue[0], &g_rfQueue[1], sizeof g_rfQueue[0] * (size_t)--g_rfQn); }
        else {   // the background pass: one learned route not done yet (looked for every 2 s)
            ULONGLONG t = now_ms();
            if (t - g_rfScanAt < 2000) return;
            g_rfScanAt = t;
            for (int r = 0; r < g_learn->nRoutes; r++) {
                BLRoute *rt = &g_learn->routes[r];
                if (rt->kind != BL_FOOT || rt->n < 3) continue;
                unsigned h = rf_hash(rt);
                if (rf_done(h)) continue;
                rf_start((const float (*)[3])rt->pts, rt->n, r); g_rf.hash = h;
                break;
            }
            if (!g_rfBusy) return;
        }
    }
    RefineJob *j = &g_rf;
    for (int budget = H->botCount > 40 ? 4 : 8; budget > 0 && g_rfBusy; ) {
        if (j->a >= j->n - 1) { rf_finish(j); break; }
        int stop = j->j >= j->n || j->fails >= 3 || (j->j - 1 > j->a && j->keep[j->j - 1]) || dist2d(j->in[j->a], j->in[j->j]) > RF_LEG_MAX;
        if (stop) {
            rf_emit(j, j->in[j->best]);
            j->a = j->best; j->j = j->a + 2; j->best = j->a + 1; j->fails = 0;
            continue;
        }
        if (rf_ok(j->in[j->a], j->in[j->j])) { j->best = j->j; j->fails = 0; } else j->fails++;
        j->j++; budget--;
    }
}

static void refine_dump(void)
{
    H->log("refine %s: %u trip(s) and %u learned route(s) straightened, %.0f m -> %.0f m in all, %u line checks (failed: height %u, grid %u, rays %u, floor %u), %u points pinned (steep / door / jump), %d queued, %s",
           g_refineOn ? "on" : "off", g_rfTrips, g_rfRoutes, g_rfBefore, g_rfAfter, g_rfChecks, g_rfWhy[0], g_rfWhy[1], g_rfWhy[2], g_rfWhy[3], g_rfKept, g_rfQn, g_rfBusy ? "working on one" : "idle");
}
