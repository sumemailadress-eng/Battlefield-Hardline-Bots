// Thugbot route editor in the game (included by brain.inc after human_routes.inc). Players type in chat:
//   !wp        add a point where you are (on foot, in a car, or flying a helicopter - the point drops to the ground)
//   !rec       record a point every 3 m as you move (walk it, drive it or fly over it); !stop ends recording
//   !undo      remove the last point          !clear   start over
//   !save      save the points as a route bots use (with the routes they learned from players, and - when an end is
//              near a flag - as a saved route to that flag)
//   !delete    delete the route nearest to you (one you saved or one learned from players)
//   !routes    how many routes this map has
// Chat reaches the server as RCON events; Thugbot-Chat.ps1 (started with the server) passes "!" lines here as
// "chat <name>|<text>" commands and sends the replies back (Thugbot_say.txt) as admin.say to that player.

#define ED_MAX 240
static struct { char player[40]; float pts[ED_MAX][3]; int n, recording; } g_ed;
#define CHAT_QUEUE 16
static char g_chatQ[CHAT_QUEUE][160];
static volatile LONG g_chatHead, g_chatTail;

static void chat_enqueue(const char *line)   // (command thread)
{
    LONG h = g_chatHead;
    if (h - g_chatTail >= CHAT_QUEUE) return;
    snprintf(g_chatQ[h % CHAT_QUEUE], sizeof g_chatQ[0], "%s", line);
    InterlockedExchange(&g_chatHead, h + 1);
}

static void say_to(const char *player, const char *fmt, ...)
{
    char msg[200];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    FILE *f = fopen("Thugbot_say.txt", "a");
    if (f) { fprintf(f, "%s|%s\n", player, msg); fclose(f); }
    H->log("editor: to %s: %s", player, msg);
}

static void *player_by_name(const char *name)
{
    for (int j = 0; j < g_playerCount; j++) if (!strcmp((char *)g_players[j] + OFF_PLAYER_NAME, name)) return g_players[j];
    return NULL;
}

// Where the player is, on the ground under them (a helicopter's position dropped to the ground below).
static int player_ground(void *pl, float *out)
{
    const float *p = NULL;
    int seat; void *v = player_vehicle(pl, &seat);
    if (v) p = veh_pos(v);
    else { void *s = bot_soldier(pl); if (s) p = soldier_pos(s); }
    if (!p) return 0;
    float a[3] = { p[0], p[1] + 1.0f, p[2] }, b[3] = { p[0], p[1] - 300.0f, p[2] }, h[3];
    if (ray_cast(a, b, h, NULL)) { out[0] = h[0]; out[1] = h[1]; out[2] = h[2]; }
    else memcpy(out, p, 12);
    return 1;
}

static void editor_add(const float *g)
{
    if (g_ed.n < ED_MAX) memcpy(g_ed.pts[g_ed.n++], g, 12);
}

static void editor_save(const char *who)
{
    if (g_ed.n < 2) { say_to(who, "Thugbot: need at least 2 points (you have %d)", g_ed.n); return; }
    float len = 0;
    for (int k = 1; k < g_ed.n; k++) len += dist2d(g_ed.pts[k - 1], g_ed.pts[k]);
    // 1) with the routes learned from players (lost bots follow these)
    int r = g_learn ? bl_add_route(g_learn, BL_FOOT, (const float (*)[3])g_ed.pts, g_ed.n, (long)time(NULL)) : -1;
    if (r >= 0) g_learn->routes[r].uses += 9;   // (a route drawn by hand is kept over anything learned)
    if (g_learn) g_learn->dirty = 1;
    // 2) a saved route when one end is near a flag (bots going for that flag join it)
    int toFlag = 0;
    flags_survey();
    for (int f = 0; f < g_cpCount; f++) {
        if (!g_fi[f].ok) continue;
        for (int e = 0; e < 2; e++) {
            const float *end = e ? g_ed.pts[g_ed.n - 1] : g_ed.pts[0], *other = e ? g_ed.pts[0] : g_ed.pts[g_ed.n - 1];
            if (dist2d(end, g_fi[f].pos) > 15) continue;
            char from[40], to[40];
            snprintf(from, sizeof from, "manual@%.0f,%.0f", other[0], other[2]);
            snprintf(to, sizeof to, "flag@%.0f,%.0f", g_fi[f].pos[0], g_fi[f].pos[2]);
            static float pts[ROUTE_PTS * 3];
            int n = 0;
            for (int k = 0; k < g_ed.n && n < ROUTE_PTS; k++) { const float *q = e ? g_ed.pts[k] : g_ed.pts[g_ed.n - 1 - k]; memcpy(&pts[n++ * 3], q, 12); }
            if (route_store(from, to, other, g_fi[f].pos, pts, n) >= 0) toFlag++;
        }
    }
    routes_save(); learn_save();
    say_to(who, "Thugbot: route saved - %d points, %.0f m%s%s", g_ed.n, len, toFlag ? ", leads to a flag" : "", r < 0 && g_learn ? " (it matched a known route: that one counts another use)" : "");
    g_ed.n = 0; g_ed.recording = 0;
}

static void editor_delete(const char *who, const float *g)
{
    int best = -1, bestKind = 0; float bd = 10;
    for (int i = 0; i < g_routeN; i++)
        for (int k = 0; k < g_routes[i].n; k++) { float d = dist2d(g, g_routes[i].pts[k]); if (d < bd) { bd = d; best = i; bestKind = 1; } }
    for (int i = 0; g_learn && i < g_learn->nRoutes; i++)
        for (int k = 0; k < g_learn->routes[i].n; k++) { float d = dist2d(g, g_learn->routes[i].pts[k]); if (d < bd) { bd = d; best = i; bestKind = 2; } }
    if (best < 0) { say_to(who, "Thugbot: no route within 10 m of you"); return; }
    if (bestKind == 1) { g_routes[best] = g_routes[--g_routeN]; g_routesDirty = 1; routes_save(); say_to(who, "Thugbot: deleted a saved route (%.0f m from you)", bd); }
    else { bl_delete(g_learn, 'r', best); g_learn->dirty = 1; learn_save(); say_to(who, "Thugbot: deleted a route learned from players (%.0f m from you)", bd); }
}

static void editor_command(const char *who, const char *text)
{
    void *pl = player_by_name(who);
    float g[3];
    int have = pl && player_ground(pl, g);
    if (!strncmp(text, "!wp", 3)) {
        if (!have) { say_to(who, "Thugbot: can't find where you are (alive?)"); return; }
        if (strcmp(g_ed.player, who)) { snprintf(g_ed.player, sizeof g_ed.player, "%s", who); g_ed.n = 0; }
        editor_add(g);
        say_to(who, "Thugbot: point %d at %.0f %.0f %.0f", g_ed.n, g[0], g[1], g[2]);
    } else if (!strncmp(text, "!rec", 4)) {
        snprintf(g_ed.player, sizeof g_ed.player, "%s", who); g_ed.recording = 1;
        if (have) editor_add(g);
        say_to(who, "Thugbot: recording - a point every 3 m. !stop to end, !save to keep it");
    } else if (!strncmp(text, "!stop", 5)) {
        g_ed.recording = 0; say_to(who, "Thugbot: stopped - %d point(s). !save to keep them", g_ed.n);
    } else if (!strncmp(text, "!undo", 5)) {
        if (g_ed.n) g_ed.n--; say_to(who, "Thugbot: %d point(s) left", g_ed.n);
    } else if (!strncmp(text, "!clear", 6)) {
        g_ed.n = 0; g_ed.recording = 0; say_to(who, "Thugbot: cleared");
    } else if (!strncmp(text, "!save", 5)) editor_save(who);
    else if (!strncmp(text, "!delete", 7)) { if (have) editor_delete(who, g); else say_to(who, "Thugbot: can't find where you are"); }
    else if (!strncmp(text, "!lift", 5)) {   // stand in an elevator at the bottom: !lift; ride up, step out: !lift
        if (!have) { say_to(who, "Thugbot: can't find where you are"); return; }
        if (!g_liftPendingSet) { memcpy(g_liftPending, g, 12); g_liftPendingSet = 1; say_to(who, "Thugbot: elevator start at %.0f m - ride it, then !lift again at the other end", g[1]); }
        else { g_liftPendingSet = 0; if (fabsf(g[1] - g_liftPending[1]) < 4) say_to(who, "Thugbot: that's the same floor - nothing saved"); else { lift_add(g_liftPending, g, who); say_to(who, "Thugbot: elevator saved (%.0f m to %.0f m)", fminf(g[1], g_liftPending[1]), fmaxf(g[1], g_liftPending[1])); } }
    }
    else if (!strncmp(text, "!out", 4)) {   // bots in your vehicle get out (and leave it alone for 30 s)
        void *v = pl ? player_vehicle(pl, NULL) : NULL;
        if (!v && have) { int k = soldier_seated(g); v = vehicle_known_ptr(k); }
        if (!v) { say_to(who, "Thugbot: you're not in a vehicle"); return; }
        g_kickVeh = v; g_kickUntil = now_ms() + 8000;
        say_to(who, "Thugbot: everyone out of your %s", vehicle_name(v));
    }
    else if (!strncmp(text, "!routes", 7)) say_to(who, "Thugbot: %d saved route(s), %d learned from players; editing %d point(s)", g_routeN, g_learn ? g_learn->nRoutes : 0, g_ed.n);
    else say_to(who, "Thugbot: !wp !rec !stop !undo !clear !save !delete !routes !lift !out");
}

static void editor_tick(void)
{
    while (g_chatTail != g_chatHead) {
        char line[160];
        snprintf(line, sizeof line, "%s", g_chatQ[g_chatTail % CHAT_QUEUE]);
        InterlockedIncrement(&g_chatTail);
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        editor_command(line, bar + 1);
    }
    if (g_ed.recording) {
        static ULONGLONG at;
        ULONGLONG t = now_ms();
        if (t - at < 250) return;
        at = t;
        void *pl = player_by_name(g_ed.player);
        float g[3];
        if (!pl || !player_ground(pl, g)) return;
        if (!g_ed.n || dist2d(g, g_ed.pts[g_ed.n - 1]) >= 3) {
            editor_add(g);
            if (g_ed.n == ED_MAX) { g_ed.recording = 0; say_to(g_ed.player, "Thugbot: %d points - recording stopped (full). !save", ED_MAX); }
        }
    }
}
