// Thugbot helicopters (included by brain.inc after drive.inc): bots that sit in a helicopter's pilot seat.
//
//   "heli 0": pilots get out (the old behaviour). "heli 1": control probe - every pilot runs the same input script
//   (throttle, yaw, strafe, roll, pitch, brake: 2.5 s each) and the log shows what each input did to height, heading
//   and tilt, to learn which pseudo-player input is which helicopter control. "heli 2": fly (heli_fly).

// (g_heliMode: drive.inc)
static int gun_ortho(const float *m);   // gunner.inc
static void heli_deepscan(void *v, const float *ground, float agl);
typedef struct Heli { void *veh; ULONGLONG startAt, phaseAt; int phase; float p0[3], yaw0, up0[3], lastFwd; int spunUp; float lastP[3]; ULONGLONG logAt, checkAt; float logP[3], moved; } Heli;
static Heli g_heli[MAX_BOTS];

static int vehicle_is_aircraft(const char *nm) { return strstr(nm, "HELI") || strstr(nm, "Heli") || strstr(nm, "JET") || strstr(nm, "PLANE"); }

static const char *kHeliPhase[] = { "nothing", "fwd +1", "fwd +1 turn +1", "fwd +1 strafe +1", "fwd +1 yaw(hook) +1", "fwd +1 roll(hook) +1", "fwd +1 pitch(hook) +1", "fwd -1", "fwd +1 pitch(hook) -1", "throttle(hook) +1", "fwd +1 jump" };
#define HELI_PHASES 11

// What changed in the vehicle (its own 0x2400 bytes and the 0x400 behind each of its first 512 pointers) since the
// phase began: the count of changed dwords tells whether an input did anything at all (engine RPM, rotor angles ...).
#define HELI_SNAP_PTRS 512
static unsigned *g_heliSnap; static void *g_heliSnapVeh; static void *g_heliSnapPtr[HELI_SNAP_PTRS];
static void heli_snap_take(void *v)
{
    if (!g_heliSnap) g_heliSnap = malloc((0x2400 + HELI_SNAP_PTRS * 0x400));
    if (!g_heliSnap) return;
    g_heliSnapVeh = v;
    memcpy(g_heliSnap, v, 0x2400);
    for (int p = 0; p < HELI_SNAP_PTRS; p++) {
        char *o = FIELD(v, p * 8, char *);
        g_heliSnapPtr[p] = o && !((uintptr_t)o & 7) && (uintptr_t)o > 0x10000 && !IsBadReadPtr(o, 0x400) ? o : NULL;
        if (g_heliSnapPtr[p]) memcpy((char *)g_heliSnap + 0x2400 + p * 0x400, o, 0x400);
    }
}
static void heli_snap_diff(void *v, const char *phase)
{
    if (!g_heliSnap || g_heliSnapVeh != v || IsBadReadPtr(v, 0x2400)) return;
    int self = 0, deep = 0; char ex[256]; int exn = 0; ex[0] = 0;
    for (int o = 0; o < 0x2400; o += 4) if (*(unsigned *)((char *)v + o) != g_heliSnap[o / 4]) { self++; if (exn < 8) { exn++; snprintf(ex + strlen(ex), sizeof ex - strlen(ex), " +%#x", o); } }
    for (int p = 0; p < HELI_SNAP_PTRS; p++) {
        char *o = g_heliSnapPtr[p];
        if (!o || FIELD(v, p * 8, char *) != o || IsBadReadPtr(o, 0x400)) continue;
        int n = 0; const unsigned *old = (const unsigned *)((char *)g_heliSnap + 0x2400 + p * 0x400);
        for (int q = 0; q < 0x100; q++) n += ((const unsigned *)o)[q] != old[q];
        if (n) { deep += n; if (exn < 14) { exn++; snprintf(ex + strlen(ex), sizeof ex - strlen(ex), " [+%#x]:%d", p * 8, n); } }
    }
    H->log("heli:   %s - %d dword(s) changed in the vehicle, %d one pointer away:%s", phase, self, deep, ex);
}

static void heli_probe(int i, Bot *b, void *v, ULONGLONG t, Heli *h)
{
    const float *m = vehicle_xf(v);
    if (!m) { static unsigned logged; if (logged++ < 10) H->log("heli: %s - no transform for %s", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v)); return; }
    if (!h->phaseAt || t - h->phaseAt > (h->phase == 1 && !h->spunUp ? 30000u : 2500u)) {
        if (h->phase == 1) h->spunUp = 1;   // (the first throttle phase lasts 30 s: rotors spin up for a long time)
        if (h->phaseAt) {
            float yaw = atan2f(m[8], m[10]);
            H->log("heli: %s %s phase \"%s\": moved %.1f %.1f %.1f (up %.1f), heading %.2f -> %.2f, up row %.2f %.2f %.2f -> %.2f %.2f %.2f",
                   (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), kHeliPhase[h->phase], m[12] - h->p0[0], m[13] - h->p0[1], m[14] - h->p0[2], m[13] - h->p0[1],
                   h->yaw0, yaw, h->up0[0], h->up0[1], h->up0[2], m[4], m[5], m[6]);
            heli_snap_diff(v, kHeliPhase[h->phase]);
            { const float *f = (const float *)v; const unsigned *u = (const unsigned *)v;
              H->log("heli:   +0x160.. %g %g %g %g | +0x240.. %g %g %g %g | +0x3e0.. %g %g %g %g | +0x4c0.. %08x %08x %08x %08x",
                     f[0x160/4], f[0x164/4], f[0x168/4], f[0x16c/4], f[0x240/4], f[0x244/4], f[0x248/4], f[0x24c/4], f[0x3e0/4], f[0x3e4/4], f[0x3e8/4], f[0x3ec/4], u[0x4c0/4], u[0x4c4/4], u[0x4c8/4], u[0x4cc/4]); }
            h->phase = (h->phase + 1) % HELI_PHASES;
        }
        heli_snap_take(v);
        h->phaseAt = t; memcpy(h->p0, m + 12, 12); h->yaw0 = atan2f(m[8], m[10]); memcpy(h->up0, m + 4, 12);
    }
    if (h->phase == 1 && !h->spunUp && t - h->phaseAt > 12000 && h->moved > 10) heli_deepscan(v, m + 12, FIELD(v, 0x168, float));
    if (h->phase == 1 && !h->spunUp && t - h->logAt > 2500) {
        h->logAt = t;
        H->log("heli: %s spinning up %.0f s: +0x168 %.1f, at %.1f %.1f %.1f", vehicle_name(v), (t - h->phaseAt) / 1000.0, FIELD(v, 0x168, float), m[12], m[13], m[14]);
        if (h->logP[0] || h->logP[2]) h->moved += dist2d(m + 12, h->logP);
        memcpy(h->logP, m + 12, 12);
    }
    switch (h->phase) {
    case 1: b->fwd = 1; break;
    case 2: b->fwd = 1; b->turn = 1; break;
    case 3: b->fwd = 1; b->strafe = 1; break;
    case 4: b->fwd = 1; b->yawIn = 1; break;
    case 5: b->fwd = 1; b->roll = 1; break;
    case 6: b->fwd = 1; b->pitch = 1; break;
    case 7: b->fwd = -1; break;
    case 8: b->fwd = 1; b->pitch = -1; break;
    case 9: b->throttleIn = 1; break;
    case 10: b->fwd = 1; b->buttons |= BTN_JUMP; break;
    }
    (void)i;
}

// "+0x168" looks like the height above the ground (0 parked, climbs with throttle, falls with brake), and the transform
// the search finds moves along the ground under the helicopter (a downwash / shadow point). While it is airborne
// (+0x168 > 20): every matrix two pointers away from the vehicle (first 0x800 of it, first 0x400 of each) that is
// upright-ish and sits within 20 m sideways of that point at about ground + height. Logged, once per helicopter.
static void heli_deepscan(void *v, const float *ground, float agl)
{
    static void *done[16]; static int nd;
    for (int k = 0; k < nd; k++) if (done[k] == v) return;
    if (nd < 16) done[nd++] = v;
    int hits = 0;
    for (int a = 0; a < 0x800 && hits < 24; a += 8) {
        char *o1 = FIELD(v, a, char *);
        if (!o1 || ((uintptr_t)o1 & 7) || (uintptr_t)o1 < 0x10000 || IsBadReadPtr(o1, 0x400)) continue;
        for (int b2 = -8; b2 < 0x400 && hits < 24; b2 += 8) {
            char *o2 = b2 < 0 ? o1 : FIELD(o1, b2, char *);
            if (!o2 || ((uintptr_t)o2 & 7) || (uintptr_t)o2 < 0x10000 || IsBadReadPtr(o2, 0x400)) continue;
            for (int off = 0; off + 64 <= 0x400; off += 16) {
                const float *m = (const float *)(o2 + off);
                if (!gun_ortho(m) || m[5] < 0.5f) continue;
                if (dist2d(m + 12, ground) > 40 || m[13] < ground[1] + 4 || m[13] > ground[1] + 400) continue;   // (+0x168 is the speed, not the height: anything above the ground point)
                H->log("heli: deep %s [[+%#x]+%#x]+%#x at %.1f %.1f %.1f (ground point %.1f %.1f %.1f, height %.1f)", vehicle_name(v), a, b2 < 0 ? 0 : b2, off,
                       m[12], m[13], m[14], ground[0], ground[1], ground[2], agl);
                if (++hits >= 24) break;
            }
        }
    }
    H->log("heli: deep scan of %s while %.0f m up: %d matrix(es) near where it should be", vehicle_name(v), agl, hits);
}

// Flying (after fun-bots' VehicleChopperControl for BF3): four loops on the engine transform (drive.inc
// vehicle_engine_xf) - throttle holds a height above the target flag (and above the ground under it), yaw turns the
// nose to the flag, pitch holds a 20 degree nose-down tilt while far (less when close, level when there), roll stays
// level. Which way each input turns the helicopter isn't known for Hardline: every loop checks itself (input
// against the turn rate it caused, 1.5 s windows) and flips its sign when it made things worse.
// Attack helicopter pilots fire the nose guns at an enemy within 150 m, ahead and in line (and turn to one in 80 m).
#define HELI_CRUISE_UP 35.0f
typedef struct HeliCtl { float sign[3], acc[3], prev[3]; ULONGLONG winAt, lastAt; float lastY, vy; int flips[3]; } HeliCtl;
static HeliCtl g_hctl[MAX_BOTS];
static void heli_fly(int i, Bot *b, void *v, int team, ULONGLONG t, Heli *h)
{
    HeliCtl *c = &g_hctl[i];
    const float *m = vehicle_engine_xf(v);
    if (!m) m = vehicle_xf(v);
    if (!m) return;
    if (t - h->startAt < 50 || !c->winAt) { memset(c, 0, sizeof *c); c->sign[0] = c->sign[1] = c->sign[2] = 1; c->winAt = t; c->lastAt = t; c->lastY = m[13]; }
    const float *p = m + 12;
    float dt = (t - c->lastAt) / 1000.0f;
    if (dt > 0.05f) { c->vy = c->vy * 0.7f + 0.3f * (p[1] - c->lastY) / dt; c->lastY = p[1]; c->lastAt = t; }
    float yaw = atan2f(m[8], m[10]), pitch = asinf(fmaxf(-1, fminf(1, m[9]))), roll = asinf(fmaxf(-1, fminf(1, m[1])));
    choose_target(i, b, team, p);
    float aim[3]; memcpy(aim, b->target, 12);
    // an enemy near: attack helicopters go for it
    int attack = strstr(vehicle_name(v), "ATTACK") != NULL, firing = 0;
    float ground = p[1] - 200;
    { float a[3] = { p[0], p[1] - 2, p[2] }, e[3] = { p[0], p[1] - 300, p[2] }, hh[3]; if (ray_cast(a, e, hh, NULL)) ground = hh[1]; }
    float want = fmaxf(aim[1] + HELI_CRUISE_UP, ground + 25);
    float dist = dist2d(p, aim);
    if (attack) {
        float best = 150;
        for (int j = 0; j < g_playerCount; j++) {
            void *pl = g_players[j];
            int pt = FIELD(pl, OFF_PLAYER_TEAM, int);
            if (!pt || pt == team) continue;
            void *ev = player_vehicle(pl, NULL); float q[3];
            if (ev) { const float *qq = veh_pos(ev); if (!qq) continue; memcpy(q, qq, 12); }
            else { void *s = bot_soldier(pl); if (!s) continue; memcpy(q, soldier_pos(s), 12); q[1] += 1; }
            float d = sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]));
            if (d >= best) continue;
            float s0[3] = { p[0] + (q[0] - p[0]) / d * 4, p[1] + (q[1] - p[1]) / d * 4, p[2] + (q[2] - p[2]) / d * 4 };
            if (!clear_line(s0, q)) continue;
            best = d; memcpy(aim, q, 12); firing = 1;
        }
        if (firing) { dist = dist2d(p, aim); want = fmaxf(fminf(want, aim[1] + 25), ground + 15); }
    }
    float yawErr = wrap_pi(atan2f(aim[0] - p[0], aim[2] - p[2]) - yaw);
    float tiltWant = t - h->startAt < 5000 ? 0 : dist > 80 ? -0.35f : dist > 30 ? -0.35f * (dist - 30) / 50 : 0.05f;
    if (firing) tiltWant = fmaxf(-0.6f, fminf(0.3f, atan2f(aim[1] - p[1], fmaxf(1, dist)) * 0.8f));   // nose at it
    float rollWant = fabsf(yawErr) > 0.3f ? (yawErr > 0 ? 0.12f : -0.12f) : 0;
    float u[3] = { fmaxf(-1, fminf(1, yawErr * 2.0f)), fmaxf(-1, fminf(1, (tiltWant - pitch) * 4.0f)), fmaxf(-1, fminf(1, (rollWant - roll) * 4.0f)) };
    float now3[3] = { yaw, pitch, roll };
    // self-check: input u (wanting the angle to go up when positive) against how the angle went
    for (int k = 0; k < 3; k++) {
        if (c->prev[k] != 0 || k) c->acc[k] += u[k] * wrap_pi(now3[k] - c->prev[k]);
        c->prev[k] = now3[k];
    }
    if (t - c->winAt > 1500) {
        static const char *nm[3] = { "yaw", "pitch", "roll" };
        for (int k = 0; k < 3; k++) if (c->acc[k] < -0.05f && c->flips[k] < 6) { c->sign[k] = -c->sign[k]; c->flips[k]++; H->log("heli: %s - %s input turned it the wrong way: sign now %+.0f", vehicle_name(v), nm[k], c->sign[k]); }
        c->acc[0] = c->acc[1] = c->acc[2] = 0; c->winAt = t;
    }
    b->yawIn = c->sign[0] * u[0];
    b->pitch = c->sign[1] * u[1];
    b->roll = c->sign[2] * u[2];
    float dy = want - p[1];
    float thr = t - h->startAt < 4000 ? 1 : fmaxf(0, fminf(1, 0.45f + dy * 0.08f - c->vy * 0.25f));
    b->fwd = thr;   // EIAThrottle (collective)
    if (dy < -20 && c->vy > -3) b->fwd = -0.5f;   // far too high: brake (down)
    if (firing && fabsf(yawErr) < 0.12f && fabsf(tiltWant - pitch) < 0.15f) b->buttons |= BTN_FIRE;
    static unsigned traced;
    if (t - h->logAt > 3000 && traced++ < 300) {
        h->logAt = t;
        H->log("heli: %s %s at %.0f %.0f %.0f (want %.0f up, ground %.0f), to %.0f m, yaw err %.2f, pitch %.2f (want %.2f), roll %.2f, thr %.2f vy %.1f%s",
               (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), p[0], p[1], p[2], want, ground, dist, yawErr, pitch, tiltWant, roll, b->fwd, c->vy, firing ? ", attacking" : "");
    }
}
static int gunner_think(int i, Bot *b, void *v, int seat, int team);   // gunner.inc
static void combat_tick_vehicle(int i, Bot *b, void *v, int team, int seat) { gunner_think(i, b, v, seat, team); }

// A bot in an aircraft. Returns 1 when handled (0: the caller makes it get out).
static int heli_think(int i, Bot *b, void *v, int seat, int team)
{
    if (!g_heliMode) return 0;
    ULONGLONG t = now_ms();
    Heli *h = &g_heli[i];
    if (h->veh != v) {
        memset(h, 0, sizeof *h); h->veh = v; h->startAt = t;
        int k = veh_xf_index(v);
        H->log("heli: %s gets in %s seat %d", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat);
        {   // the physics part cars keep their matrix in ([+0x208]+0x80): what a helicopter's holds (quaternion + position?)
            const float *pb = vehicle_body(v) ? (const float *)vehicle_body(v) : NULL;
            if (pb && !IsBadReadPtr(pb, 0x300)) for (int r = 0; r < 0x300 / 4; r += 8)
                H->log("heli: [+0x208]+%#05x %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f", r * 4, pb[r], pb[r + 1], pb[r + 2], pb[r + 3], pb[r + 4], pb[r + 5], pb[r + 6], pb[r + 7]);
            void *rs = FIELD(b->player, 0x1648, void *);
            if (rs && !IsBadReadPtr(rs, 0x400)) { const float *sp = soldier_pos(rs); H->log("heli: pilot soldier at %.1f %.1f %.1f, bot last stood at %.1f %.1f %.1f", sp[0], sp[1], sp[2], b->lastPos[0], b->lastPos[1], b->lastPos[2]); }
        }
        if (k >= 0) for (int c = 0; c < g_vehXf[k].nc; c++)
            H->log("heli: %s candidate %d [+%#x]+%#x at %.1f %.1f %.1f heading %.2f", vehicle_name(v), c, g_vehXf[k].cpo[c], g_vehXf[k].coff[c], g_vehXf[k].c0[c][0], g_vehXf[k].c0[c][1], g_vehXf[k].c0[c][2], g_vehXf[k].c0[c][3]);
    }
    b->fwd = b->strafe = b->turn = 0; b->buttons = 0; b->action = 0; b->pitch = 0; b->roll = 0; b->yawIn = 0; b->throttleIn = 0;
    if (seat != 0) { combat_tick_vehicle(i, b, v, team, seat); return 1; }   // gunner / passenger seats: shoot
    // +0x168 rises with the throttle (rotor speed?) and the helicopter does fly: if the transform in use stands still
    // while it reads 8+, it is the wrong one - every matrix becomes a candidate again and the one that moves wins.
    {
        const float *p = veh_pos(v);
        if (p && t - h->checkAt > 4000) {
            if (h->checkAt && FIELD(v, 0x168, float) > 8 && dist2d(p, h->lastP) < 1 && fabsf(p[1] - h->lastP[1]) < 1) {
                static const float nowhere[3] = { 1e6f, 0, 1e6f };
                vehicle_xf_locate(v, nowhere);
                H->log("heli: %s reads %.1f at +0x168 but its transform stood still: searching again", vehicle_name(v), FIELD(v, 0x168, float));
            }
            h->checkAt = t; memcpy(h->lastP, p, 12);
        }
    }
    if (vehicle_xf_pick_moving(v, h->lastFwd > 0.5f)) H->log("heli: %s - switched to the transform that moved", vehicle_name(v));
    if (g_heliMode == 1) { heli_probe(i, b, v, t, h); h->lastFwd = b->fwd; return 1; }
    heli_fly(i, b, v, team, t, h);
    return 1;
}

// A human flying a helicopter (seat 0): every 0.5 s its engine transform, how it is tilted, how fast it climbs, and the
// vehicle's fields that react to the pilot's input (+0x168 rises with the bots' throttle; +0x3e8 reads 120 with it),
// plus how many dwords of the vehicle changed since the last line - to compare a real flight with the bots' tries.
static void heli_watch_humans(void)
{
    static ULONGLONG at; static void *lastV; static float lastP[3];
    ULONGLONG t = now_ms();
    if (t - at < 500) return;
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        if (is_bot_player(pl)) continue;
        int seat = -1; void *v = player_vehicle(pl, &seat);
        if (!v || !vehicle_is_aircraft(vehicle_name(v))) continue;
        const float *m = vehicle_engine_xf(v);
        if (!m) continue;
        at = t;
        if (v != lastV) { lastV = v; memcpy(lastP, m + 12, 12); heli_snap_take(v); H->log("helirec: %s in %s seat %d", (char *)pl + OFF_PLAYER_NAME, vehicle_name(v), seat); return; }
        const float *f = (const float *)v;
        int self = 0;
        if (g_heliSnap && g_heliSnapVeh == v) for (int o = 0; o < 0x2400; o += 4) self += *(unsigned *)((char *)v + o) != g_heliSnap[o / 4];
        H->log("helirec: seat %d at %.1f %.1f %.1f (moved %.1f, up %.1f), heading %.2f pitch %.2f roll %.2f | +0x168 %.2f +0x16c %.2f | +0x3e0 %g %g %g %g | +0x240 %g %g %g %g | %d dwords changed",
               seat, m[12], m[13], m[14], dist2d(m + 12, lastP), m[13] - lastP[1], atan2f(m[8], m[10]), asinf(fmaxf(-1, fminf(1, m[9]))), asinf(fmaxf(-1, fminf(1, m[1]))),
               f[0x168 / 4], f[0x16c / 4], f[0x3e0 / 4], f[0x3e4 / 4], f[0x3e8 / 4], f[0x3ec / 4], f[0x240 / 4], f[0x244 / 4], f[0x248 / 4], f[0x24c / 4], self);
        memcpy(lastP, m + 12, 12);
        heli_snap_take(v);
        return;
    }
}
