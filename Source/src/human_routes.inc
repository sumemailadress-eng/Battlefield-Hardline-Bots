static unsigned g_plansOwn, g_learnedFar, g_planWhy[3];   // brain.inc plan(): own plans (the 5%), joins of a learned route further off
static unsigned g_learnedFirst;   // plans that took a learned route first (brain.inc plan)
// Thugbot: routes learned from human players (included by brain.inc after routes.inc).
//
// Every human is followed (a point every 3 m of walking). A trail ends when the player dies, leaves, stands still for
// 8 s, moves 25 m between two looks (a vehicle, a teleport) or reaches the record's point limit; trails of 40 m or more
// go into the level's learned file as a "route foot" record (learn.c merges one that is mostly the way of a known
// route into that one: it counts another use). A bot that is lost - the planner found no route, or it keeps getting
// stuck at the same spot - follows the learned route that passes near it and takes it closest to where it is going.
#define MAX_TRAILS 32
typedef struct Trail { void *player; int n; float pts[BL_ROUTE_PTS][3]; ULONGLONG movedAt; float last[3]; int hasLast; } Trail;
static unsigned g_humanVaults, g_humanDoors;
static Trail g_trails[MAX_TRAILS];
static unsigned g_humanRoutes, g_humanRoutesUsed;
static ULONGLONG g_trailAt;

// Picking a learned route (the tester 2026-10-26: "make sure bots don't all group together"): every bot near a learned route
// used to take the same best one, so a spawn wave walked it as one knot. Each bot sees each route up to 60% longer
// (fixed per bot and route), and every bot already walking a route adds 25 m to it - the next ones take another way in.
static unsigned g_lrSeed;   // the bot picking (set by plan())
static int g_lrPick = -1;
static unsigned g_farLegs;   // grid-then-route joins onto a leg (not reaching the flag)   // the learned route the last human_route_to_flag / human_route_far took
static int lr_users(int r);   // (brain.inc: bots walking learned route r)
static float lr_spread(int r) { return 1.0f + 0.6f * (float)(((g_lrSeed * 2654435761u) ^ ((unsigned)r * 2246822519u)) % 1000) / 1000.0f; }

static int is_bot_player(void *pl) { for (int i = 0; i < H->botCount; i++) if (H->bots[i] == pl) return 1; return 0; }

static void trail_finish(Trail *tr, const char *why)
{
    if (tr->n >= 2 && g_learn) {
        float len = 0;
        for (int k = 1; k < tr->n; k++) len += dist2d(tr->pts[k - 1], tr->pts[k]);
        int r = bl_add_route(g_learn, BL_FOOT, (const float (*)[3])tr->pts, tr->n, (long)time(NULL));
        if (r >= 0) {
            g_learn->routes[r].uses += 4;   // (a player's walk outweighs bots' trips when the store is full)
            g_humanRoutes++;
            H->log("learn: route from %s, %d points, %.0f m (%s) - %d learned route(s)", (char *)tr->player + OFF_PLAYER_NAME, tr->n, len, why, g_learn->nRoutes);
        }
    }
    tr->n = 0;
}

// Between two looks 250 ms apart a human moved from a to b:
//  - up 0.45 m or more over under 3 m (no staircase rises that fast): a vault / climb. Learned as a spot passed with a
//    jump (bots jump there on the way in - movement.inc) and as an extra grid link so routes may go over it.
//  - through a door / pane the bots learned as shut (or tried and failed once): it opens - learned as one that does.
static void human_moves(void *pl, const float *a, const float *b)
{
    float d = dist2d(a, b), rise = b[1] - a[1];
    if (d > 0.3f && d < 3.0f && rise > 0.45f && rise < 2.2f && g_learn) {
        float h[2] = { (b[0] - a[0]) / d, (b[2] - a[2]) / d };
        int k = bl_note_stuck(g_learn, BL_FOOT, a, h, (long)time(NULL));
        if (k >= 0) bl_note_result(g_learn, k, 1, 1, (long)time(NULL));
        if (g_nav && !g_navBuilding) nav_add_link(g_nav, a, b, 6.0f);
        if (g_humanVaults++ < 40) H->log("learn: %s vaulted / climbed %.1f m at %.0f %.0f %.0f", (char *)pl + OFF_PLAYER_NAME, rise, a[0], a[1], a[2]);
    }
    if (d > 0.2f && d < 6.0f) {
        float mid[3] = { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2 + 1.2f, (a[2] + b[2]) / 2 };
        for (int k = 0; k < g_breachN; k++) {
            Breach *e = &g_breach[k];
            if (e->kind != 'n' && e->kind != '?') continue;
            if (dist2d(e->p, mid) > 1.5f || fabsf(e->p[1] - mid[1]) > 1.5f) continue;
            e->kind = '?'; e->fails = 0;
            breach_learn(e->p, 'o');   // (becomes 'o' and rewrites the file)
            if (g_nav && !g_navBuilding) nav_add_link(g_nav, a, b, 8.0f);
            if (g_humanDoors++ < 40) H->log("learn: %s went through the door / panel at %.0f %.0f %.0f - bots will try it again", (char *)pl + OFF_PLAYER_NAME, e->p[0], e->p[1], e->p[2]);
        }
    }
}

static void human_routes_tick(void)
{
    ULONGLONG t = now_ms();
    if (t - g_trailAt < 250 || !g_learn) return;
    g_trailAt = t;
    for (int k = 0; k < MAX_TRAILS; k++) {   // players who left, or died
        Trail *tr = &g_trails[k];
        if (!tr->player) continue;
        int here = 0;
        for (int j = 0; j < g_playerCount; j++) if (g_players[j] == tr->player) { here = 1; break; }
        if (!here) { tr->n = 0; tr->player = NULL; continue; }   // (its memory may be gone: don't read its name)
        if (!bot_soldier(tr->player) && tr->n) trail_finish(tr, "died");
    }
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        if (is_bot_player(pl)) continue;
        void *s = bot_soldier(pl);
        if (!s) continue;
        Trail *tr = NULL, *freeTr = NULL;
        for (int k = 0; k < MAX_TRAILS; k++) {
            if (g_trails[k].player == pl) { tr = &g_trails[k]; break; }
            if (!g_trails[k].player && !freeTr) freeTr = &g_trails[k];
        }
        if (!tr) { if (!freeTr) continue; tr = freeTr; memset(tr, 0, sizeof *tr); tr->player = pl; }
        const float *p = soldier_pos(s);
        if (tr->hasLast) human_moves(pl, tr->last, p);
        memcpy(tr->last, p, 12); tr->hasLast = 1;
        if (!tr->n) { memcpy(tr->pts[tr->n++], p, 12); tr->movedAt = t; continue; }
        float d = dist2d(p, tr->pts[tr->n - 1]);
        if (d > 25) { trail_finish(tr, "drove / jumped"); memcpy(tr->pts[tr->n++], p, 12); tr->movedAt = t; continue; }
        if (d >= 3) {
            memcpy(tr->pts[tr->n++], p, 12); tr->movedAt = t;
            if (tr->n >= BL_ROUTE_PTS) { trail_finish(tr, "long"); memcpy(tr->pts[tr->n++], p, 12); }
        } else if (t - tr->movedAt > 8000 && tr->n > 1) { trail_finish(tr, "stopped"); memcpy(tr->pts[tr->n++], p, 12); tr->movedAt = t; }
    }
}

// A learned human route that passes within 8 m of pos and gets at least 15 m closer to target than we are: the rest of
// it from its nearest point (towards the end nearer the target) into path. Returns the point count.
static int human_route_pick(const float *pos, const float *target, float *path, int maxPts)
{
    if (!g_learn) return 0;
    float myD = dist2d(pos, target), bestScore = 1e9f;
    int best = -1, bestK = 0, bestDir = 1;
    for (int r = 0; r < g_learn->nRoutes; r++) {
        BLRoute *rt = &g_learn->routes[r];
        if (rt->kind != BL_FOOT || rt->n < 2) continue;
        int k = -1; float kd = 8;
        for (int m = 0; m < rt->n; m++) { float d = dist2d(pos, rt->pts[m]); if (d < kd && fabsf(rt->pts[m][1] - pos[1]) < 3) { kd = d; k = m; } }
        if (k < 0) continue;
        for (int dir = -1; dir <= 1; dir += 2) {
            float walked = 0, nearest = 1e9f;
            for (int m = k; m >= 0 && m < rt->n; m += dir) {
                if (m != k) walked += dist2d(rt->pts[m], rt->pts[m - dir]);
                float d = dist2d(rt->pts[m], target);
                if (d < nearest) nearest = d;
            }
            if (nearest > myD - 15) continue;
            float score = kd + walked * 0.25f + nearest;
            if (score < bestScore) { bestScore = score; best = r; bestK = k; bestDir = dir; }
        }
    }
    if (best < 0) return 0;
    BLRoute *rt = &g_learn->routes[best];
    float nearest = 1e9f; int stop = bestK, n = 0;
    for (int m = bestK; m >= 0 && m < rt->n; m += bestDir) { float d = dist2d(rt->pts[m], target); if (d < nearest) { nearest = d; stop = m; } }
    for (int m = bestK; n < maxPts; m += bestDir) { memcpy(&path[n++ * 3], rt->pts[m], 12); if (m == stop) break; }
    g_humanRoutesUsed++;
    return n;
}

// Going for a flag: a route a player walked that reaches within 15 m of it, joined at a point within 15 m of the bot on
// its floor with a clear straight line to it - used before the bot plans its own (the tester: "why do they not always use
// saved routes?"). Copies from the join to the point nearest the flag, then the flag itself.
static int human_route_to_flag(const float *pos, const float *target, float *path, int maxPts)
{
    if (!g_learn) return 0;
    float bestScore = 1e9f; int best = -1, bestK = 0, bestDir = 1, bestStop = 0;
    for (int r = 0; r < g_learn->nRoutes; r++) {
        BLRoute *rt = &g_learn->routes[r];
        if (rt->kind != BL_FOOT || rt->n < 2) continue;
        for (int dir = -1; dir <= 1; dir += 2) {
            int stop = -1; float sd = 15;   // the point nearest the flag
            for (int m = 0; m < rt->n; m++) { float d = dist2d(rt->pts[m], target); if (d < sd && fabsf(rt->pts[m][1] - target[1]) < 4) { sd = d; stop = m; } }
            if (stop < 0) continue;
            for (int k = 0; k < rt->n; k++) {   // a join before the stop in this direction
                if ((dir > 0 && k >= stop) || (dir < 0 && k <= stop)) continue;
                float kd = dist2d(pos, rt->pts[k]);
                if (kd > 15 || fabsf(rt->pts[k][1] - pos[1]) > 2.5f) continue;
                float walked = 0;
                for (int m = k; m != stop; m += dir) walked += dist2d(rt->pts[m], rt->pts[m + dir]);
                float score = (kd * 2 + walked + sd) * lr_spread(r) + 25.0f * lr_users(r) - 4.0f * (float)(rt->uses < 10 ? rt->uses : 10);   // (well used / player-walked routes first)
                if (score < bestScore) { bestScore = score; best = r; bestK = k; bestDir = dir; bestStop = stop; }
            }
        }
    }
    if (best < 0) return 0;
    BLRoute *rt = &g_learn->routes[best];
    if (dist2d(pos, rt->pts[bestK]) > 2 && !nav_segment_clear(ray_cast, pos, rt->pts[bestK])) return 0;
    g_lrPick = best;
    int n = 0;
    for (int m = bestK; n < maxPts - 1; m += bestDir) { memcpy(&path[n++ * 3], rt->pts[m], 12); if (m == bestStop) break; }
    memcpy(&path[n++ * 3], target, 12);
    g_humanRoutesUsed++;
    return n;
}

// No learned route to walk onto within 15 m: plan over the grid to the best learned route further off (up to 250 m, its
// floor give or take 6 m) and follow it - its point in *join, the rest of it from there in tail. The route either reaches
// the flag (within 15 m, its floor: the flag is appended) or is a leg that brings the bot 30 m+ closer at no big detour;
// at a leg's end plan() runs again and links on to the next route the same way - route, grid, route (the tester 2026-10-26:
// "bots should use routes to get to other routes... route then grid then route"). The unknown rest after a leg counts
// 1.3x (a route that gets there is better than one that gets close). Per route: running lengths and, from each point,
// the point nearest the target ahead in each direction, so a route costs O(n).
static int human_route_far(const float *pos, const float *target, float *join, float *tail, int maxTail)
{
    if (!g_learn) return 0;
    static float cum[BL_ROUTE_PTS], dT[BL_ROUTE_PTS];
    static int nearFwd[BL_ROUTE_PTS], nearBack[BL_ROUTE_PTS];
    float myD = dist2d(pos, target), bestScore = 1e9f; int best = -1, bestK = 0, bestDir = 1, bestStop = 0, bestReach = 0;
    for (int r = 0; r < g_learn->nRoutes; r++) {
        BLRoute *rt = &g_learn->routes[r];
        if (rt->kind != BL_FOOT || rt->n < 2) continue;
        int n = rt->n;
        for (int m = 0; m < n; m++) {
            cum[m] = m ? cum[m - 1] + dist2d(rt->pts[m - 1], rt->pts[m]) : 0;
            dT[m] = dist2d(rt->pts[m], target) + (fabsf(rt->pts[m][1] - target[1]) < 4 ? 0 : 1000);   // (another floor: never "at" it)
        }
        nearFwd[n - 1] = n - 1; for (int m = n - 2; m >= 0; m--) nearFwd[m] = dT[m] < dT[nearFwd[m + 1]] ? m : nearFwd[m + 1];
        nearBack[0] = 0; for (int m = 1; m < n; m++) nearBack[m] = dT[m] < dT[nearBack[m - 1]] ? m : nearBack[m - 1];
        float usesBonus = 4.0f * (float)(rt->uses < 10 ? rt->uses : 10), spread = lr_spread(r), busy = 25.0f * lr_users(r);
        for (int k = 0; k < n; k++) {
            float kd = dist2d(pos, rt->pts[k]);
            if (kd > 250 || fabsf(rt->pts[k][1] - pos[1]) > 6) continue;
            for (int dir = -1; dir <= 1; dir += 2) {
                int stop = dir > 0 ? (k + 1 < n ? nearFwd[k + 1] : -1) : (k > 0 ? nearBack[k - 1] : -1);
                if (stop < 0) continue;
                float walked = fabsf(cum[stop] - cum[k]), rest = dT[stop] >= 1000 ? dT[stop] - 1000 : dT[stop];
                int reach = dT[stop] < 15;
                if (!reach && (rest > myD - 30 || kd + walked + rest > myD * 1.35f + 40)) continue;
                float score = (kd * 1.5f + walked + (reach ? rest : rest * 1.3f)) * spread + busy - usesBonus;
                if (score < bestScore) { bestScore = score; best = r; bestK = k; bestDir = dir; bestStop = stop; bestReach = reach; }
            }
        }
    }
    if (best < 0) return 0;
    BLRoute *rt = &g_learn->routes[best];
    g_lrPick = best;
    memcpy(join, rt->pts[bestK], 12);
    int n = 0;
    for (int m = bestK; n < maxTail - 1; m += bestDir) { memcpy(&tail[n++ * 3], rt->pts[m], 12); if (m == bestStop) break; }
    if (bestReach) memcpy(&tail[n++ * 3], target, 12);
    else g_farLegs++;
    return n;
}

// A learned route that does not reach the flag but is a good way towards it (the tester 2026-10-26: "I made a clear route of a
// basement and bots don't use it" - his walk ended 70 m short of the flag, and only routes ending within 15 m of a flag
// were ever taken for one). Joined within 15 m of the bot on its floor with a clear line, walked towards the end nearer
// the target, it must bring the bot 20 m+ closer, and the whole way (join + route + rest straight) at most 1.35x the
// straight line + 20 m. The bot follows it to its end and plans on from there. Well used / player routes first.
static unsigned g_legsTaken;   // (g_farLegs: above)
static int human_route_leg(const float *pos, const float *target, float *path, int maxPts)
{
    if (!g_learn) return 0;
    float myD = dist2d(pos, target), bestScore = 1e9f;
    int best = -1, bestK = 0, bestDir = 1, bestStop = 0;
    for (int r = 0; r < g_learn->nRoutes; r++) {
        BLRoute *rt = &g_learn->routes[r];
        if (rt->kind != BL_FOOT || rt->n < 2) continue;
        int k = -1; float kd = 15;
        for (int m = 0; m < rt->n; m++) { float d = dist2d(pos, rt->pts[m]); if (d < kd && fabsf(rt->pts[m][1] - pos[1]) < 2.5f) { kd = d; k = m; } }
        if (k < 0) continue;
        for (int dir = -1; dir <= 1; dir += 2) {
            float walked = 0, nearest = 1e9f; int stop = k;
            for (int m = k + dir; m >= 0 && m < rt->n; m += dir) {
                walked += dist2d(rt->pts[m], rt->pts[m - dir]);
                float d = dist2d(rt->pts[m], target);
                if (d < nearest) { nearest = d; stop = m; }
            }
            if (stop == k || nearest > myD - 20) continue;
            float w2 = 0; for (int m = k + dir; m != stop + dir; m += dir) w2 += dist2d(rt->pts[m], rt->pts[m - dir]);
            float total = kd + w2 + nearest;
            if (total > myD * 1.35f + 20) continue;
            float score = total * lr_spread(r) + 25.0f * lr_users(r) - 4.0f * (float)(rt->uses < 10 ? rt->uses : 10);
            if (score < bestScore) { bestScore = score; best = r; bestK = k; bestDir = dir; bestStop = stop; }
        }
    }
    if (best < 0) return 0;
    BLRoute *rt = &g_learn->routes[best];
    if (dist2d(pos, rt->pts[bestK]) > 2 && !nav_segment_clear(ray_cast, pos, rt->pts[bestK])) return 0;
    g_lrPick = best;
    int n = 0;
    for (int m = bestK; n < maxPts; m += bestDir) { memcpy(&path[n++ * 3], rt->pts[m], 12); if (m == bestStop) break; }
    g_legsTaken++;
    return n;
}

static void human_routes_dump(void)
{
    H->log("routes: plans begun - %u with no route, %u at the end of one, %u on the re-plan timer; for flags: own %u (1 in 20 by design), learned route joined from afar %u",
           g_planWhy[0], g_planWhy[1], g_planWhy[2], g_plansOwn, g_learnedFar);
    H->log("routes: %u learned route legs taken from where the bot stood, %u planned to over the grid (part of the way to a flag)", g_legsTaken, g_farLegs); g_farLegs = 0;
    memset(g_planWhy, 0, sizeof g_planWhy); g_plansOwn = g_learnedFar = 0; g_learnedFirst = 0; g_legsTaken = 0;
    { double age = 0; int own = nav_active_path_owner(), st = own >= 0 ? nav_path_state(own, &age) : -1;
      H->log("planner: %d request(s) waiting, active owner %d (state %d, %.1f s), route builder state %d", nav_pending_paths(), own, st, age / 1000, nav_path_state(ROUTE_OWNER, NULL)); }
    H->log("routes: %d learned (players' walks and clean bot trips, %u new this run), followed %u time(s) (%u as the first choice for a flag); players' vaults %u, doors / panels %u",
           g_learn ? g_learn->nRoutes : 0, g_humanRoutes, g_humanRoutesUsed, g_learnedFirst, g_humanVaults, g_humanDoors);
}

// Learning mode (the tester: "a learning mode where bots try to learn routes and smartly adapt"): every bot's own trip is
// followed like a human's (a point every 3 m). A trip that reaches the flag it was going for without getting stuck
// once (no unstick, no watchdog, no pool / pit exit) and is 40 m+ long is learned as a walked route - the same records
// players' trails make, so routes_first (human_route_to_flag) hands it to the next bots going there. Trips that went
// wrong are dropped; routes bots keep getting stuck on are re-planned (route_note_stuck). "learnroutes 0/1".
static int g_learnRoutes = 1;
static void refine_trip(const float (*pts)[3], int n);   // (route_refine.inc)
static int g_refineOn;   // (route_refine.inc)
typedef struct BotTrail { int n, bad, flag; float pts[BL_ROUTE_PTS][3]; } BotTrail;
static BotTrail g_botTrail[MAX_BOTS];
static unsigned g_botRoutesLearned;
static void bot_trail_tick(int i, Bot *b, const float *pos, ULONGLONG t)
{
    if (!g_learnRoutes || !g_learn) return;
    BotTrail *tr = &g_botTrail[i];
    if (b->targetFlag != tr->flag) { tr->n = 0; tr->bad = 0; tr->flag = b->targetFlag; }
    if (tr->flag < 0) return;
    // A trip that had to unstick / back out on the way is learned too now that trips are straightened (route_refine.inc):
    // the wandering part is pulled tight before it is kept - the tester's "explore, find the flag, then make it direct".
    // Climbing out of a pool still spoils it.
    if (b->poolSince || (!g_refineOn && (t < b->unstickUntil || (b->escapeUntil && t < b->escapeUntil)))) tr->bad = 1;
    if (!tr->n || dist2d(pos, tr->pts[tr->n - 1]) >= 3 || fabsf(pos[1] - tr->pts[tr->n - 1][1]) > 1.5f) {
        if (tr->n && dist2d(pos, tr->pts[tr->n - 1]) > 20) { tr->n = 0; tr->bad = 0; }   // (respawned / teleported)
        if (tr->n < BL_ROUTE_PTS) memcpy(tr->pts[tr->n++], pos, 12); else tr->bad = 1;
    }
    if (dist2d(pos, b->target) < 12 && tr->n >= 2) {
        float len = 0;
        for (int k = 1; k < tr->n; k++) len += dist2d(tr->pts[k - 1], tr->pts[k]);
        if (!tr->bad && len >= 40 && tr->n >= 8) {
            // straightened first (route_refine.inc), then learned
            refine_trip((const float (*)[3])tr->pts, tr->n);
            g_botRoutesLearned++; static unsigned logged; if (logged++ < 40) H->log("learn: %s's trip to flag %d (%.0f m, %d points, no trouble) goes to be straightened and learned - %u from bots this run", (char *)b->player + OFF_PLAYER_NAME, tr->flag, len, tr->n, g_botRoutesLearned);
        }
        tr->n = 0; tr->bad = 0; tr->flag = -2;   // (until the next target)
    }
}
static void bot_trail_dead(int i) { g_botTrail[i].n = 0; g_botTrail[i].bad = 0; }
