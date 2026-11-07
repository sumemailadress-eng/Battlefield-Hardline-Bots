// Thugbot helicopters (included by brain.inc after drive.inc): bots that sit in a helicopter's pilot seat.
// Ported from FloodBots drive_test\src\heli.inc 2026-11-06 (THUGBOT_HELI_PORT, ini HeliFly). Left out on purpose: remote seat
// switching / enterEntry (BF4 crashes, no control) - pilots only come from the game's spawn into a vehicle or walking up + E;
// transport drops (they need Hardline's exit call and entry list); the dead rocket picker; BF4's spawner hook.
//
//   "heli 0": pilots get out (the old behaviour). "heli 1": control probe - every pilot runs the same input script
//   (throttle, yaw, strafe, roll, pitch, brake: 2.5 s each) and the log shows what each input did to height, heading
//   and tilt, to learn which pseudo-player input is which helicopter control. "heli 2": fly (heli_fly).

// (g_heliMode: drive.inc)
static int gun_ortho(const float *m);   // gunner.inc
static void heli_deepscan(void *v, const float *ground, float agl);
typedef struct Heli { void *veh; ULONGLONG startAt, phaseAt; int phase; float p0[3], yaw0, up0[3], lastFwd; int spunUp; float lastP[3]; ULONGLONG logAt, checkAt; float logP[3], moved; } Heli;
static Heli g_heli[MAX_BOTS];
static int vehicle_is_aircraft(const char *nm) { return name_is_air(nm); }

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
static int g_heliFlip = 0;   // "heliflip 1": each axis may correct its sign once. Off: a bad reading flipped yaw 1 s into a flight and it spun in (2026-11-06)
// A transport over its flag calls the drop: passengers on the ledges and in the back seats get out and parachute down
static struct { void *v; ULONGLONG at; int flag; } g_dropCall[8];
static void heli_drop_call(void *v, ULONGLONG t, int flag) { int k = 0; while (k < 8 && g_dropCall[k].v != v) k++; if (k == 8) { static int nx; k = nx++ % 8; } g_dropCall[k].v = v; g_dropCall[k].at = t; g_dropCall[k].flag = flag; }
static int heli_drop_flag(void *v, ULONGLONG t) { for (int k = 0; k < 8; k++) if (g_dropCall[k].v == v && t - g_dropCall[k].at < 1500) return g_dropCall[k].flag; return -1; }
static int gun_seat_side(void *v, int prev);   // gunner_heli.inc
typedef struct HeliCtl { int orbit, orbitSide; ULONGLONG orbitAt; ULONGLONG breakUntil, gndAt, obsAt, scanAt; int gndHit, obsBlocked; float gndY, obsTop; void *tgt; float rateYaw, yawRate; ULONGLONG rateAt; float sign[3], acc[3], prev[3]; ULONGLONG winAt, lastAt; float lastY, vy, lastX, lastZ, vx, vz; int flips[3]; } HeliCtl;
static HeliCtl g_hctl[MAX_BOTS];
static int gun_line_ok(const float *a, const float *q);   // gunner_heli.inc
static int air_line_ok(const float *a, const float *q);
static void heli_fly(int i, Bot *b, void *v, int team, ULONGLONG t, Heli *h)
{
    HeliCtl *c = &g_hctl[i];
    // Only the engine's own transform is flown on. The matrix search can hand back a copy or a point on the ground under the
    // helicopter: flying on that gave full throttle and full yaw on phantom errors. No engine transform: stays on the ground,
    // controls neutral, and the no-control check lets the pilot out.
    const float *m = vehicle_engine_xf(v);
    if (!m) { static unsigned nl; if (nl++ < 20) H->log("heli: %s - no engine transform: stays on the ground", vehicle_name(v)); b->heliPilot = 1; return; }
    if (t - h->startAt < 50 || !c->winAt) { memset(c, 0, sizeof *c); c->sign[0] = -1; c->sign[1] = -1; c->sign[2] = 1;   /* (yaw -1: on Hardline +1 turn lowers the heading. Pitch -1: log 2026-11-06 18:01 - asked for nose up at +1, 32 of 43 flights went past 40 deg nose down. Roll +1: not settled by the log) */ c->winAt = t; c->lastAt = t; c->lastY = m[13]; c->lastX = m[12]; c->lastZ = m[14]; }
    const float *p = m + 12;
    {   // the reading jumped: not trusted this frame
        float jx = p[0] - c->lastX, jy = p[1] - c->lastY, jz = p[2] - c->lastZ;
        if (jx * jx + jy * jy + jz * jz > 3600) {
            static unsigned jl; if (jl++ < 20) H->log("heli: %s - its position jumped %.0f m: controls neutral this frame", vehicle_name(v), sqrtf(jx * jx + jy * jy + jz * jz));
            c->lastX = p[0]; c->lastY = p[1]; c->lastZ = p[2]; c->lastAt = t; b->heliPilot = 1; return;
        }
    }
    float dt = (t - c->lastAt) / 1000.0f;
    if (dt > 0.05f) { c->vy = c->vy * 0.7f + 0.3f * (p[1] - c->lastY) / dt; c->vx = c->vx * 0.7f + 0.3f * (p[0] - c->lastX) / dt; c->vz = c->vz * 0.7f + 0.3f * (p[2] - c->lastZ) / dt; c->lastX = p[0]; c->lastZ = p[2]; c->lastY = p[1]; c->lastAt = t; }
    float yaw = atan2f(m[8], m[10]), pitch = asinf(fmaxf(-1, fminf(1, m[9]))), roll = asinf(fmaxf(-1, fminf(1, m[1])));
    choose_target(i, b, team, p);
    float aim[3]; memcpy(aim, b->target, 12);
    // an enemy near: attack helicopters go for it
    const char *category = vehicle_name(v);
    int airTarget = 0;
    int attack = strstr(category, "ATTACK") || strstr(category, "attack") || strstr(category, "scout") || strstr(category,"SCOUT"), firing = 0;
    float ground = p[1] - 200;
    if (t >= c->gndAt) { c->gndAt = t + 150; c->gndHit = 0; float a[3] = { p[0], p[1] - 2, p[2] }, e[3] = { p[0], p[1] - 300, p[2] }, hh[3]; if (ray_cast(a, e, hh, NULL)) { c->gndHit = 1; c->gndY = hh[1]; } }   // (lag fix 0.3.1: rays a few times a second, not every frame)
    if (c->gndHit) ground = c->gndY;
    float want = fmaxf(aim[1] + HELI_CRUISE_UP, ground + 25);
    float dist = dist2d(p, aim);
    if (attack) {
        // (lag fix 0.3.1) who to attack is chosen 4 times a second, with its line-of-sight rays; where it is, every frame
        if (t >= c->scanAt) { c->scanAt = t + 250; c->tgt = NULL;
        float best = 1e9f;
        for (int j = 0; j < g_playerCount; j++) {
            void *pl = g_players[j];
            int pt = FIELD(pl, OFF_PLAYER_TEAM, int);
            if (!pt || pt == team) continue;
            void *ev = player_vehicle(pl, NULL); float q[3];
            if (ev) { const float *qq = veh_pos(ev); if (!qq) continue; memcpy(q, qq, 12); }
            else { void *s = bot_soldier(pl); if (!s) continue; memcpy(q, soldier_pos(s), 12); q[1] += 1; }
            float d = sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]));
            // (the tester 2026-10-29: "helicopters fight each other") an enemy aircraft out to 300 m comes first (scored 200 m
            // nearer), then anything within 150 m
            int air = ev && name_is_air(vehicle_name(ev));
            float range = air ? 500 : 220, score = d - (air ? 400 : 0);   /* (the tester 2026-10-30: "increase their range" - infantry / ground 300 m, was 150) */
            if (d < 0.5f || d > range || score >= best) continue;
            // (clear_line wants the ray to end within 0.6 m of the target's centre: on a vehicle or helicopter it stops on the hull, so they were never picked. the tester 2026-11-07)
            float st = fminf(8.0f, d * 0.5f), s0[3] = { p[0] + (q[0] - p[0]) / d * st, p[1] + (q[1] - p[1]) / d * st, p[2] + (q[2] - p[2]) / d * st };
            if (ev ? !air_line_ok(s0, q) : !clear_line(s0, q)) continue;
            best = score; c->tgt = pl;
        }
        }
        if (c->tgt) {
            int there = 0; for (int j = 0; j < g_playerCount && !there; j++) there = g_players[j] == c->tgt;
            if (there) {
                void *ev = player_vehicle(c->tgt, NULL); const float *qq = NULL; float dy = 0;
                if (ev) qq = veh_pos(ev); else { void *s = bot_soldier(c->tgt); if (s) { qq = soldier_pos(s); dy = 1; } }
                if (qq) {
                    memcpy(aim, qq, 12); aim[1] += dy; firing = 1;
                    if (ev) { veh_lead(ev, aim, dist2d(p, aim), 500); airTarget = name_is_air(vehicle_name(ev)); }
                }
            } else c->tgt = NULL;
        }
        if (firing) { dist = dist2d(p, aim); want = airTarget ? fmaxf(aim[1], ground + 15) : fmaxf(fminf(want, aim[1] + 25), ground + 15); }   // (a helicopter: at its height, to get the nose on it)
    }
    // Attack run (the tester 2026-11-07: "the attack chopper should pitch down and fire at people"): at a target on the ground it flies in 30 m over it, nose
    // on it, then breaks off and climbs when close or low
    float agl = c->gndHit ? p[1] - c->gndY : 200;
    int groundRun = attack && firing && !airTarget;
    if (groundRun) {
        if (t >= c->breakUntil && (dist < 45 || agl < 16)) c->breakUntil = t + 3500;
        want = t < c->breakUntil ? ground + 55 : fmaxf(aim[1] + 30, ground + 28);
    }
    // A transport goes low over its flag and calls the drop
    if (!attack && b->targetFlag >= 0 && dist < 70) want = fmaxf(aim[1] + 25, ground + 25);
    if (!attack && b->targetFlag >= 0 && b->targetFlag < MAX_CPS && dist < 60 && agl > 12 && g_fi[b->targetFlag].ok && g_fi[b->targetFlag].owner != team) heli_drop_call(v, t, b->targetFlag);
    float yawErr = wrap_pi(atan2f(aim[0] - p[0], aim[2] - p[2]) - yaw);
    // (the tester 2026-11-07: door guns only reach one side) A transport with gunners aboard circles a flag it doesn't hold, side on, 50 m out: nose along the circle, a little inward when outside
    // it and outward when inside. Lifting off, the tower check and the height loop are as before.
    int orbit = 0;
    if (!attack && b->targetFlag >= 0 && b->targetFlag < MAX_CPS && g_fi[b->targetFlag].ok && g_fi[b->targetFlag].owner != team && t - h->startAt > 8000 && dist < (c->orbit ? 150.0f : 110.0f)) {
        if (t >= c->orbitAt) { c->orbitAt = t + 1000; c->orbitSide = gun_seat_side(v, c->orbitSide); }
        if (c->orbitSide) {
            float off = 1.5708f + fmaxf(-0.7f, fminf(0.5f, (50.0f - dist) * 0.02f));
            yawErr = wrap_pi(atan2f(aim[0] - p[0], aim[2] - p[2]) - c->orbitSide * off - yaw);
            orbit = 1;
            if (!c->orbit) { static unsigned ol; if (ol++ < 60) H->log("heli: %s circles flag %d side on (%+d) with its gunners, %.0f m away", vehicle_name(v), b->targetFlag, c->orbitSide, dist); }
        }
    }
    c->orbit = orbit;
    // Towers, cranes, cliffs (the tester 2026-10-29: "pilots should avoid towers or anything really high"): three rays 80 m
    // ahead along the nose (level, and 8 m / 16 m below it) - something there: climb over it and hold the nose level
    // until clear; the climb aims 15 m over the highest point the rays found.
    int blockedAhead = 0;
    {
        // (the tester 2026-10-30: "choppers hit buildings instead of navigating between buildings" - only the nose line was
        // looked along; a helicopter sliding sideways or turning met the building beside it) also 25 degrees either side
        // of the nose and along the way it is actually moving
        float top = c->obsTop, sp = sqrtf(c->vx * c->vx + c->vz * c->vz);
        float dirs[4] = { yaw, yaw + 0.45f, yaw - 0.45f, sp > 3 ? atan2f(c->vx, c->vz) : yaw };
        if (t >= c->obsAt) { c->obsAt = t + 200; top = -1e9f;   // (lag fix 0.3.1: up to 24 rays a look, 5 looks a second)
        for (int k = 0; k < 12; k++) {
            float hx = sinf(dirs[k / 3]), hz = cosf(dirs[k / 3]), len = k / 3 == 3 ? fmaxf(60, sp * 5) : 80;
            float a[3] = { p[0] + hx * 10, p[1] - (k % 3) * 8.0f, p[2] + hz * 10 }, e[3] = { a[0] + hx * len, a[1], a[2] + hz * len }, hh[3];
            if (ray_cast(a, e, hh, NULL)) {
                blockedAhead = 1;
                float u0[3] = { hh[0] + hx * 2, hh[1] + 80, hh[2] + hz * 2 }, u1[3] = { u0[0], hh[1] - 1, u0[2] }, top0[3];
                if (ray_cast(u0, u1, top0, NULL) && top0[1] > top) top = top0[1];
                else if (hh[1] + 20 > top) top = hh[1] + 20;
            }
        }
        c->obsBlocked = blockedAhead; c->obsTop = top; }
        blockedAhead = c->obsBlocked;
        if (blockedAhead) { want = fmaxf(want, top + 20); static unsigned tl; if (tl++ < 20) H->log("heli: %s - something tall ahead (top %.0f): climbing to %.0f", vehicle_name(v), top, want); }
    }
    float tiltWant = t - h->startAt < 5000 ? 0 : dist > 80 ? -0.35f : dist > 30 ? -0.35f * (dist - 30) / 50 : 0.05f;
    if (!firing && t - h->startAt >= 5000) tiltWant = fmaxf(-0.5f, fminf(0.22f, -dist * 0.006f + (c->vx * sinf(yaw) + c->vz * cosf(yaw)) * 0.055f));
    if (!firing) tiltWant *= fmaxf(0, 1 - fabsf(yawErr) / 1.5f);   // (AltBotMod: no nose-down while turning hard)
    if (firing) tiltWant = fmaxf(-0.7f, fminf(0.3f, atan2f(aim[1] - p[1], fmaxf(1, dist))));   // nose at it (the full angle: x0.8 aimed short)
    if (groundRun) { if (t < c->breakUntil) tiltWant = -0.1f; else if (dist > 180) tiltWant = fminf(tiltWant, -0.35f); }   // (breaking off / still far: flying, not hanging there aiming)
    if (orbit) { float fw = c->vx * sinf(yaw) + c->vz * cosf(yaw); tiltWant = fmaxf(-0.22f, fminf(0.05f, (fw - 12) * 0.03f)); }   // (about 12 m/s round the circle)
    if (fabsf(yawErr) > 1.2f) tiltWant = 0.03f;
    // (the tester 2026-10-29: "helicopters crashed all around - they should go up vertically first") straight up off the pad
    // until 20 m clear of the ground (and near the cruise height) - no nose-down before that
    int climbFirst = 0;
    if (p[1] < fmaxf(ground + 20, groundRun ? -1e9f : want - 12)) { tiltWant = 0.02f; climbFirst = t - h->startAt < 20000 && !firing; }
    if (blockedAhead && want > p[1] + 3) tiltWant = 0.05f;   // (no flying into it while climbing over)   // (facing well away: hover and turn first - a transport flew off the map nose-first, yaw err 3.0, 2026-10-29)
    // Nose down is limited to 15 degrees, and past that it only pulls up (the tester 2026-11-06: "pitch forward enough to crash")
    // (log 2026-11-07: the flat 15 degree limit held helicopters to 14 m/s all the way across the map) 29 degrees cruising, 40 on an attack run, 15 when low
    float noseLimit = groundRun && agl >= 18 ? -0.7f : agl >= 25 ? -0.5f : -0.26f;
    tiltWant = fmaxf(noseLimit, tiltWant);
    if (pitch < noseLimit - 0.08f) tiltWant = 0.05f;
    int holdLevel = t - h->startAt < 5000 || climbFirst || p[1] < ground + 20;   // off the pad and until 20 m clear of the ground: straight up and level, no turning
    if (holdLevel) tiltWant = fmaxf(tiltWant, 0.02f);
    float rollWant = !holdLevel && fabsf(yawErr) > 0.3f ? (yawErr > 0 ? 0.12f : -0.12f) : 0;
    float yawRate = wrap_pi(yaw - c->prev[0]);   // (per frame - damping: AltBotMod's yaw PID)
    float u[3] = { fmaxf(-1, fminf(1, yawErr * 1.5f - yawRate * 2.0f)), fmaxf(-1, fminf(1, (tiltWant - pitch) * 4.0f)), fmaxf(-1, fminf(1, (rollWant - roll) * 4.0f)) };
    // limits: gentle inputs; spinning or tipped over: no more yaw until it settles
    if (!c->rateAt) { c->rateAt = t; c->rateYaw = yaw; }
    else if (t - c->rateAt >= 50) { c->yawRate = wrap_pi(yaw - c->rateYaw) / ((t - c->rateAt) / 1000.0f); c->rateYaw = yaw; c->rateAt = t; }
    u[0] = fmaxf(-0.6f, fminf(0.6f, u[0])); u[1] = fmaxf(-0.6f, fminf(0.6f, u[1])); u[2] = fmaxf(-0.25f, fminf(0.25f, u[2]));
    if (holdLevel || fabsf(c->yawRate) > 1.5f || fabsf(roll) > 0.6f || fabsf(pitch) > 0.9f) u[0] = 0;
    float now3[3] = { yaw, pitch, roll };
    // self-check: input u (wanting the angle to go up when positive) against how the angle went
    for (int k = 0; k < 3; k++) {
        if (c->prev[k] != 0 || k) c->acc[k] += u[k] * wrap_pi(now3[k] - c->prev[k]);
        c->prev[k] = now3[k];
    }
    if (t - c->winAt > 1500) {
        static const char *nm[3] = { "yaw", "pitch", "roll" };
        // (2026-10-29: "a chopper is just spinning in the air" - the yaw sign flipped back and forth on noise during attacks and
        // ended up wrong: a runaway spin) each axis may flip once, on clear evidence only - AltBotMod calibrates its signs once too
        { static unsigned sl; if (sl++ < 80) H->log("heli: %s axis check yaw %.2f pitch %.2f roll %.2f (negative: that axis has the wrong sign), yaw rate %.2f", vehicle_name(v), c->acc[0], c->acc[1], c->acc[2], c->yawRate); }
        for (int k = 0; g_heliFlip && k < 3; k++) if (c->acc[k] < -0.3f && c->flips[k] < 1) { c->sign[k] = -c->sign[k]; c->flips[k]++; H->log("heli: %s - %s input turned it the wrong way: sign now %+.0f", vehicle_name(v), nm[k], c->sign[k]); }
        // Pitch and roll may correct their sign once, only early in a flight on the engine's transform and only when already tilted that way: a wrong
        // sign is a crash within seconds. (Yaw never: a flip on a bad reading spun one in.)
        for (int k = 1; k <= 2; k++) if (c->acc[k] < -0.25f && c->flips[k] < 1 && t - h->startAt < 12000 && fabsf(now3[k]) > 0.15f) { c->sign[k] = -c->sign[k]; c->flips[k]++; H->log("heli: %s - %s input worked against it at %.2f: sign now %+.0f", vehicle_name(v), nm[k], now3[k], c->sign[k]); }
        c->acc[0] = c->acc[1] = c->acc[2] = 0; c->winAt = t;
    }
    b->yawIn = c->sign[0] * u[0];
    b->turn = b->yawIn;   // (the soldier turn input as well: on some helicopters the yaw axis alone did nothing. Same sign here: BF4's core flips turn in vehicles, Hardline's doesn't; both land on EIAYaw)
    b->pitch = c->sign[1] * u[1];
    b->roll = c->sign[2] * u[2];
    float dy = want - p[1];
    float thr = t - h->startAt < 4000 ? 1 : fmaxf(0, fminf(1, 0.45f + dy * 0.08f - c->vy * 0.25f));
    b->fwd = thr;   // EIAThrottle (collective)
    b->heliPilot = 1;
    if (climbFirst) b->fwd = 1;   /* (full collective until clear) */
    if (dy < -20 && c->vy > -3) b->fwd = -0.5f;   // far too high: brake (down)
    // Pilot weapons (simple, replacing the patch's rocket picker - the tester: "attack helis don't shoot rockets"): nose roughly on
    // the target (0.3 rad) - fire in bursts, 1.1 s on / 0.4 s off; the weapon swaps every 6 s between slot 1 (guns) and
    // slot 2 (rockets) with a select press at the start of each spell
    // (the tester 2026-10-30: "choppers can't hit the broad side of a barn") fire only with the nose within ~5 m of the target
    // sideways (0.3 rad was 60 m off at 200 m) and within 0.1 rad of its elevation
    float yawTol = fmaxf(0.03f, atanf((airTarget ? 8.0f : 5.0f) / fmaxf(dist, 1)));
    float aimEl = atan2f(aim[1] - p[1], fmaxf(1, dist));   // (the real angle to it: tiltWant is limited, and held level near the ground)
    if (attack && firing && fabsf(yawErr) < yawTol && fabsf(aimEl - pitch) < fmaxf(0.1f, atanf(6.0f / fmaxf(dist, 1)))) {
        { static ULONGLONG fa; if (t >= fa) { fa = t + 5000; H->log("heli: %s fires at %s %.0f m away", (char *)b->player + OFF_PLAYER_NAME, airTarget ? "a helicopter" : "a target", dist); } }
        ULONGLONG ph = t % 12000;
        if (ph < 200) b->buttons |= BTN_WEAPON1; else if (ph >= 6000 && ph < 6200) b->buttons |= BTN_WEAPON2;
        else if ((t % 1500) < 1100) b->buttons |= BTN_FIRE;
    }
    static unsigned traced;
    if (t - h->logAt > 3000 && traced++ < 300) {
        h->logAt = t;
        H->log("heli: %s %s at %.0f %.0f %.0f (want %.0f up, ground %.0f), to %.0f m, yaw err %.2f, pitch %.2f (want %.2f), roll %.2f, thr %.2f vy %.1f%s",
               (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), p[0], p[1], p[2], want, ground, dist, yawErr, pitch, tiltWant, roll, b->fwd, c->vy, firing ? ", attacking" : "");
    }
}
// A soldier on a helicopter's ledge: the seat keeps the soldier as the controllable, so it comes through here as if on foot, moving at the helicopter's
// speed (the "seated" check wants it standing still): it walked, and tried to jump off "edges" (log: chute jumps at 80 m up over the map).
// Here it stays put and shoots with its own weapons; over the flag it gets out and parachutes down (parachute.inc).
static int heli_ledge(int i, Bot *b, void *soldier, const float *pos, int team, ULONGLONG t)
{
    const float *vel = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
    if (vel[1] < -4) return 0;   // (falling: the parachute's business)
    int k = -1; float bd = 9;
    for (int q = 0; q < g_knownVehN; q++) {
        KnownVeh *kv = &g_knownVeh[q];
        if (!kv->occupied || !name_is_air(kv->name) || t - kv->seenAt > 3000) continue;
        const float *hp = veh_pos(kv->v);
        if (!hp || fabsf(hp[1] - pos[1]) > 6) continue;
        float d = dist2d(hp, pos);
        if (d < bd) { bd = d; k = q; }
    }
    if (k < 0 || air_below(pos) < 5) return 0;
    void *hv = g_knownVeh[k].v;
    b->why = "on a helicopter's ledge"; b->npts = 0;
    combat_tick(i, b, soldier, team);
    b->fwd = b->strafe = 0; b->buttons &= ~(BTN_SPRINT | BTN_JUMP); b->action = 0;
    int df = heli_drop_flag(hv, t);
    if (df >= 0) {
        b->attackFlag = df + 1; b->attackChooseAt = t + 45000;
        if ((t / 300) % 5 == 0) b->buttons |= ((t / 1500) & 1) ? BTN_EXIT : BTN_INTERACT;
        static ULONGLONG la; if (t >= la) { la = t + 3000; H->log("heli: %s gets off the ledge over flag %d", (char *)b->player + OFF_PLAYER_NAME, df); }
    }
    b->progressAt = t; memcpy(b->progressPos, pos, 12);
    return 1;
}
static int gunner_think(int i, Bot *b, void *v, int seat, int team);   // gunner.inc
static void combat_tick_vehicle(int i, Bot *b, void *v, int team, int seat) { gunner_think(i, b, v, seat, team); }

// A bot in an aircraft. Returns 1 when handled (0: the caller makes it get out).
static int heli_think(int i, Bot *b, void *v, int seat, int team)
{
    if (!g_heliMode) return 0;
    { const char *nm0 = vehicle_name(v); if (strstr(nm0, "JET") || strstr(nm0, "PLANE") || strstr(nm0, "jet") || strstr(nm0, "plane")) return 0; }   // jets are never flown (the control probe flew one into a hill, 2026-10-27)
    ULONGLONG t = now_ms();
    Heli *h = &g_heli[i];
    if (h->veh != v) {
        memset(h, 0, sizeof *h); h->veh = v; h->startAt = t; veh_hp_note(v, 1, 1);   /* (a new vehicle can reuse an old one's address: its health reads again from the next hit) */
        int k = veh_xf_index(v);
        H->log("heli: %s gets in %s seat %d (%s)", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat, b->takingAt && t - b->takingAt < 8000 ? "walked up, E" : "spawned in");
        {   // the physics part cars keep their matrix in ([+0x208]+0x80): what a helicopter's holds (quaternion + position?)
            const float *pb = vehicle_body(v) ? (const float *)vehicle_body(v) : NULL;
            static unsigned dumps; if (pb && dumps++ < 4 && !IsBadReadPtr(pb, 0x300)) for (int r = 0; r < 0x300 / 4; r += 8)
                H->log("heli: [+0x208]+%#05x %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f", r * 4, pb[r], pb[r + 1], pb[r + 2], pb[r + 3], pb[r + 4], pb[r + 5], pb[r + 6], pb[r + 7]);
            void *rs = FIELD(b->player, 0x1648, void *);
            if (rs && !IsBadReadPtr(rs, 0x400)) { const float *sp = soldier_pos(rs); H->log("heli: pilot soldier at %.1f %.1f %.1f, bot last stood at %.1f %.1f %.1f", sp[0], sp[1], sp[2], b->lastPos[0], b->lastPos[1], b->lastPos[2]); }
        }
        if (k >= 0) for (int c = 0; c < g_vehXf[k].nc; c++)
            H->log("heli: %s candidate %d [+%#x]+%#x at %.1f %.1f %.1f heading %.2f", vehicle_name(v), c, g_vehXf[k].cpo[c], g_vehXf[k].coff[c], g_vehXf[k].c0[c][0], g_vehXf[k].c0[c][1], g_vehXf[k].c0[c][2], g_vehXf[k].c0[c][3]);
    }
    b->fwd = b->strafe = b->turn = 0; b->buttons = 0; b->action = 0; b->pitch = 0; b->roll = 0; b->yawIn = 0; b->throttleIn = 0;
    {   // Nearly wrecked (the tester 2026-11-07): everyone out, high enough for the parachute or on the ground. In between the pilot lets it down and the others hold on.
        float hpf = veh_hp_frac(v);
        if (hpf < 0.18f) {
            const float *hb = veh_pos(v); float ab = hb ? air_below(hb) : 0;
            if (ab >= 15 || ab < 3) {
                static ULONGLONG bl; if (t >= bl) { bl = t + 2000; H->log("heli bail: %s health %.0f%%, %.0f m up, %s gets out", vehicle_name(v), hpf * 100, ab, (char *)b->player + OFF_PLAYER_NAME); }
                vehicle_exit(b, t, &g_drv[i], "the helicopter is nearly wrecked");
                return 1;
            }
            if (seat == 0) { b->fwd = 0.25f; return 1; }
        }
    }
    // (the tester 2026-11-07: seated gunners stay and use the guns. Only bots riding on the ledge jump over the flag: heli_ledge.)
    if (seat != 0) { combat_tick_vehicle(i, b, v, team, seat); return 1; }   // gunner / passenger seats: shoot
    // +0x168 rises with the throttle (rotor speed?) and the helicopter does fly: if the transform in use stands still
    // while it reads 8+, it is the wrong one - every matrix becomes a candidate again and the one that moves wins.
    {
        const float *p = veh_pos(v);
        if (p && t - h->checkAt > 4000) {
            if (!vehicle_engine_xf(v) && h->checkAt && FIELD(v, 0x168, float) > 8 && dist2d(p, h->lastP) < 1 && fabsf(p[1] - h->lastP[1]) < 1) {
                static const float nowhere[3] = { 1e6f, 0, 1e6f };
                vehicle_xf_locate(v, nowhere);
                H->log("heli: %s reads %.1f at +0x168 but its transform stood still: searching again", vehicle_name(v), FIELD(v, 0x168, float));
            }
            h->checkAt = t; memcpy(h->lastP, p, 12);
        }
    }
    if (vehicle_xf_pick_moving(v, h->lastFwd > 0.5f)) H->log("heli: %s - switched to the transform that moved", vehicle_name(v));
    if (g_heliMode == 1) { heli_probe(i, b, v, t, h); h->lastFwd = b->fwd; return 1; }
    {   // on the pad: teammates walking up to it - wait for them (up to 15 s); the flight starts fresh after
        static ULONGLONG boardAt[MAX_BOTS]; static void *boardVeh[MAX_BOTS];
        if (boardVeh[i] != v) { boardVeh[i] = v; boardAt[i] = t; }
        if (t - boardAt[i] < 20000) {
            int coming = 0;
            const float *hp0 = veh_pos(v);
            for (int o = 0; o < H->botCount; o++) if (hp0 && o != i && !g_bot[o].inVehicle && ((g_bot[o].takingAt && t - g_bot[o].takingAt < 5000 && dist2d(g_bot[o].lastPos, hp0) < 12) || (g_bot[o].boardHeli == v && t - g_bot[o].boardHeliAt < 2000))) coming++;
            if (coming) { h->startAt = t; return 1; }
        }
    }
    {   // A pilot with no control (seated through enterEntry, the helicopter answers nothing): not 3 m up after 20 s of
        // full collective - it gets out and the helicopter is left for 60 s (Siege 2026-10-30: 3 of 6 sat on their pads)
        static float y0[MAX_BOTS], yMax[MAX_BOTS], p0[MAX_BOTS][3]; static void *v0[MAX_BOTS];
        const float *hp = veh_pos(v);
        if (hp) {
            if (v0[i] != v || t - h->startAt < 100) { v0[i] = v; y0[i] = hp[1]; yMax[i] = hp[1]; memcpy(p0[i], hp, 12); }
            if (hp[1] > yMax[i]) yMax[i] = hp[1];
            if (t - h->startAt > 20000 && yMax[i] - y0[i] < 3 && dist2d(hp, p0[i]) < 5) {   // (10 s, was 20: retried sooner - the tester "4 choppers in the air")   // (not moved at all - one flying level is fine)
                static unsigned fl; if (fl++ < 20) H->log("heli: %s has no control of the %s (not 3 m up in 20 s) - gets out", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v));
                known_veh_skip(v, t + 15000); vehicle_exit(b, t, &g_drv[i], "no control of the helicopter"); return 1;
            }
        }
    }
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
