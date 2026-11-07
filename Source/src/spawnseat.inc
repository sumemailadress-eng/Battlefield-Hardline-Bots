// Bots spawning into passenger and gun seats (the tester 2026-11-07: "Make sure bots can spawn as passengers"), through the game's own spawn only: no seating of a soldier already spawned (enterEntry crashed BF4).
// The game's pseudo-player spawn (RVA 0x461a50) picks a target, then calls the spawn (0x465fe0). That spawn reads two targets from the player's extent: +0x48 a soldier, +0x50 a vehicle, the second going
// to 0x465580 -> VeniceServerPlayerExtent::spawnOnVehicle (0x466a40), which finds the free seat. Here, just before that call, the game's own setters (0x462090 / 0x462310) are given a teammate's driven
// vehicle with room, using the same object the game passes for a player's vehicle (player +0x1668). The driver holds seat 0, so what's left is a passenger or gun seat. A spawn that fails: 30 s of
// the game's own choice for that bot. Ini: SpawnInVehiclePercent (default 0 = the game's choice only: in 0.3.12 the game refused every one of these, nothing was seated this way).
typedef char (*SpawnGo_t)(void *extent);
typedef void (*SpawnSet_t)(void *extent, void *target);
static SpawnGo_t o_spawnGo;
static CallHook g_hookSpawn;
static struct { void *v; ULONGLONG at; ULONGLONG noneUntil; void *lastCtl; } g_spawnBot[MAX_BOTS];
static unsigned g_spawnAsked, g_spawnFailed, g_spawnSeated, g_spawnGame;
#define g_spawnSeatPct g_spawnSeatPctIni

static void *spawn_pick(void *me, int team, ULONGLONG t)
{
    struct { void *v, *obj; int in, driver, enemy; } c[24]; int n = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j]; int seat = -1;
        if (pl == me) continue;
        void *v = player_vehicle(pl, &seat);
        if (!v) continue;
        int k = 0; while (k < n && c[k].v != v) k++;
        if (k == n) { if (n >= 24) continue; memset(&c[n], 0, sizeof c[n]); c[n].v = v; n++; }
        c[k].in++;
        if (FIELD(pl, OFF_PLAYER_TEAM, int) != team) c[k].enemy = 1;
        if (seat == 0) { c[k].driver = 1; c[k].obj = FIELD(pl, OFF_PLAYER_VEHICLE, void *); }
    }
    void *best = NULL; int bestFree = 0;
    for (int k = 0; k < n; k++) {
        if (c[k].driver && c[k].obj != c[k].v) { static unsigned l; if (l++ < 5) H->log("spawn: player +0x1668 is %p, not the vehicle %p: this vehicle is left to the game's choice", c[k].obj, c[k].v); }
        if (!c[k].driver || c[k].enemy || !c[k].obj || c[k].obj != c[k].v || c[k].v == g_apHoldV) continue;   // (the object the game would pass must be the vehicle we know)
        if (!rd_ok(c[k].obj, 0x208) || !rd_ok(FIELD(c[k].obj, 0x200, void *), 8)) continue;
        const char *nm = vehicle_name(c[k].v);
        if (!nm || veh_hp_frac(c[k].v) < 0.3f) continue;
        int seats = strstr(nm, "MOTORCYCLE") || strstr(nm, "ATTACK") ? 2 : strstr(nm, "TRANSPORT_HELI") ? 6 : 4, room = seats - c[k].in;
        for (int o = 0; o < H->botCount; o++) if (g_spawnBot[o].v == c[k].v && t - g_spawnBot[o].at < 3000) room--;   // (others on their way in)
        if (room <= 0) continue;
        if (room > bestFree || (room == bestFree && (rand() & 1))) { bestFree = room; best = c[k].v; }
    }
    return best;
}
static char h_spawnGo(void *extent)
{
    __try {
        unsigned off = *(unsigned *)(H->image + RVA_EXTENT_OFFSET);
        void *me = (char *)extent - off;
        ULONGLONG t = now_ms();
        int i = -1; for (int k = 0; k < H->botCount; k++) if (H->bots[k] == me) i = k;
        if (i >= 0 && g_spawnSeatPct > 0 && g_set.vehicles && t >= g_spawnBot[i].noneUntil && (rand() % 100) < g_spawnSeatPct) {
            players_refresh();
            void *v = spawn_pick(me, FIELD(me, OFF_PLAYER_TEAM, int), t);
            if (v) {
                ((SpawnSet_t)(H->image + RVA_SPAWN_SET_SOLDIER))(extent, NULL);
                ((SpawnSet_t)(H->image + RVA_SPAWN_SET_VEHICLE))(extent, v);
                g_spawnBot[i].v = v; g_spawnBot[i].at = t; g_spawnAsked++;
                char ok = o_spawnGo(extent);
                if (!ok) {
                    g_spawnBot[i].v = NULL; g_spawnBot[i].noneUntil = t + 30000; g_spawnFailed++;
                    ((SpawnSet_t)(H->image + RVA_SPAWN_SET_VEHICLE))(extent, NULL);
                    { static unsigned l; if (l++ < 30) H->log("spawn: %s couldn't spawn into a %s: the game's own choice for 30 s", (char *)me + OFF_PLAYER_NAME, vehicle_name(v)); }
                }
                return ok;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static unsigned l; if (l++ < 10) H->log("spawn: something unreadable while choosing a seat: left to the game");
    }
    return o_spawnGo(extent);
}
// Every spawn into a vehicle, the game's own too: no body a moment ago, now a vehicle (walking up to one goes from a soldier to a vehicle)
static void spawnseat_tick(void)
{
    static ULONGLONG at, statAt;
    ULONGLONG t = now_ms();
    if (t < at) return;
    at = t + 100;
    for (int i = 0; i < H->botCount && i < MAX_BOTS; i++) {
        void *pl = H->bots[i]; if (!pl) continue;
        void *ctl = bot_soldier_raw(pl), *was = g_spawnBot[i].lastCtl; int seat = -1;
        g_spawnBot[i].lastCtl = ctl;
        if (!ctl || was) continue;
        void *v = player_vehicle(pl, &seat);
        if (!v) continue;
        if (g_spawnBot[i].v == v) g_spawnSeated++; else g_spawnGame++;
        static unsigned l; if (l++ < 600) H->log("spawn: %s spawned into %s seat %d%s", (char *)pl + OFF_PLAYER_NAME, vehicle_name(v), seat, g_spawnBot[i].v == v ? "" : " (the game's choice)");
        g_spawnBot[i].v = NULL;
    }
    if (t >= statAt) { if (statAt && (g_spawnAsked || g_spawnSeated || g_spawnGame)) H->log("spawn: last minute: %u spawns aimed at a teammate's vehicle, %u refused; into a seat: %u aimed, %u the game's choice", g_spawnAsked, g_spawnFailed, g_spawnSeated, g_spawnGame); statAt = t + 60000; g_spawnAsked = g_spawnFailed = g_spawnSeated = g_spawnGame = 0; }
}
