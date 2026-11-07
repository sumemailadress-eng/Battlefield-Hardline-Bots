// (the tester 2026-10-30: "chopper mounted guns aren't shooting") the line from the gun stopped on the helicopter's own body
// (2.5 m out is still inside a transport's cabin / under its rotor) or on the target vehicle's hull: a hit within 4 m of
// the target counts as clear, and from a helicopter the line starts 6 m out.
static int gun_line_ok(const float *a, const float *q)
{
    float h[3];
    if (!ray_cast(a, q, h, NULL)) return 1;
    float dx = h[0] - q[0], dy = h[1] - q[1], dz = h[2] - q[2];
    return dx * dx + dy * dy + dz * dz < 16.0f;
}
// The same for a target in a vehicle: the ray stops on the hull, up to 10 m from a big helicopter's centre
static int air_line_ok(const float *a, const float *q)
{
    float h[3];
    if (!ray_cast(a, q, h, NULL)) return 1;
    float dx = h[0] - q[0], dy = h[1] - q[1], dz = h[2] - q[2];
    return dx * dx + dy * dy + dz * dz < 100.0f;
}
// Thugbot vehicle gunners (FloodBots drive_test version, THUGBOT_HELI_PORT) (included by brain.inc after heli.inc): bots in a seat whose controllable is the vehicle
// (helicopter door guns, boat turrets ...), and attack-helicopter pilots' nose guns (heli.inc).
//
//   The gun's direction isn't mapped, so it is found per seat: on getting in, every plausible matrix of the vehicle
//   (in it or one pointer away, within 10 m of it) is a candidate; turn +1 for 1.2 s - the matrix that turned most
//   against the body is the gun's yaw (and which way +1 turns it); pitch +1 for 1.2 s - the one whose forward row rose
//   or fell most is the barrel. Then: nearest enemy in sight within 150 m, turn / pitch toward it, fire when on.
//   Firing 6 s without one hit: the forward row is taken to point backwards (as on most cars' bodies) and flipped.

#define GUN_CANDS 512   // (48 filled up on the vehicle's own parts on 109 of 111 seats: the search never reached the objects it points to)
typedef struct Gunner {
    void *veh; int seat, phase; ULONGLONG at, fireSince, flipAt; unsigned hits0;
    int nc, po[GUN_CANDS], off[GUN_CANDS]; float yaw0[GUN_CANDS], fy0[GUN_CANDS], body0, d1[GUN_CANDS], prevY[GUN_CANDS], prevF[GUN_CANDS]; unsigned char dead[GUN_CANDS]; int pitchDone;
    int yawM, pitchM; float yawSign, pitchSign; int flip;
    float estYaw; ULONGLONG estAt; int estOk;   // blind aiming (no gun matrix found): the gun's heading kept by dead reckoning
    ULONGLONG scanAt; int scanFound, scanAir; float scanAim[3];   // (lag fix 0.3.1; a helicopter being aimed at: looked at more often)
    ULONGLONG pChkAt; float pChkErr; int pChkFlips;   // barrel sign check
    unsigned alHits; ULONGLONG alAt;   // learned aim: hits so far, last frame
    void *scanPl; ULONGLONG rowChkAt; unsigned rowHits; int rowFlips;   // who it aims at; is the gun's forward row the right way round
    int pitchBlind; float estPitch, lastPitch; ULONGLONG pitchAt;   // no barrel matrix found: its elevation kept by dead reckoning
    int trigOn, pressed; ULONGLONG trigSeen, burstEnd, pauseEnd;   // the trigger (gun_trigger)
    ULONGLONG daAt; float daY, daP, daHave, daDist; unsigned daN, daIn, daPin, daFire;   // gunaim: log
    ULONGLONG pinSince, pPinSince, banUntil, seenAt; void *banPl, *daTgt; float lastRel, lastHaveP, fireS;   // the seat's arc; passengers
} Gunner;
static Gunner g_gun[MAX_BOTS];
static unsigned g_vehHitsBy[MAX_BOTS];   // vehicle hits each bot made (core.c)
static unsigned g_hitsBy[MAX_BOTS];   // soldier hits each bot made (core.c's damage hook)

static const float *gun_m(void *v, int po, int off)
{
    char *o = po < 0 ? (char *)v : FIELD(v, po, char *);
    return o && !((uintptr_t)o & 7) && (uintptr_t)o > 0x10000 && !IsBadReadPtr(o + off, 64) ? (const float *)(o + off) : NULL;
}
static int gun_ortho(const float *m)
{
    float l0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], l1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], l2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
    float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6], d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
    return fabsf(l0 - 1) < 0.02f && fabsf(l1 - 1) < 0.02f && fabsf(l2 - 1) < 0.02f && fabsf(d01) < 0.02f && fabsf(d02) < 0.02f && m[13] == m[13] && fabsf(m[12]) < 5000 && fabsf(m[14]) < 5000;
}
// a matrix in the vehicle's own space (a turret's: near 0 0 0) rather than the world's
static int gun_local(const float *m) { return fabsf(m[12]) + fabsf(m[14]) < 12 && fabsf(m[13]) < 6; }
static void gun_collect(Gunner *g, void *v, const float *vp)
{
    g->nc = 0;
    for (int po = -1; po < 0x1000 && g->nc < GUN_CANDS; po += (po < 0 ? 1 : 8)) {
        char *o = po < 0 ? (char *)v : FIELD(v, po, char *);
        int span = po < 0 ? 0x2400 : 0x400;
        if (!o || ((uintptr_t)o & 7) || (uintptr_t)o < 0x10000 || IsBadReadPtr(o, span)) continue;
        for (int off = 0; off + 64 <= span && g->nc < GUN_CANDS; off += 16) {
            const float *m = (const float *)(o + off);
            if (!gun_ortho(m) || (!gun_local(m) && (dist2d(m + 12, vp) > 10 || fabsf(m[13] - vp[1]) > 8))) continue;
            int c = g->nc++; g->po[c] = po; g->off[c] = off; g->yaw0[c] = atan2f(m[8], m[10]); g->fy0[c] = m[9];
        }
    }
}
static float gun_body_yaw(void *v) { return veh_yaw(v); }

// Returns 1 when it set the bot's inputs.
static unsigned g_gunNoTarget, g_gunAiming, g_gunFiring, g_gunPresses;
// (the tester 2026-11-07: "a round every 2-3 seconds, they should rain down fire"; fire on 18 - 26% of aiming frames) The trigger stays down: on inside the cone, off only outside twice
// the cone or 0.5 s after losing the target. Bursts of 3 - 6 s with 0.5 s between them. Load shedding doesn't release it: a skipped frame keeps the bot's buttons.
static int gun_burst(Gunner *g, int i, ULONGLONG t)
{
    if (t >= g->burstEnd && t >= g->pauseEnd) { g->burstEnd = t + 3000 + ((unsigned)(i * 7 + t / 100) % 31) * 100; g->pauseEnd = g->burstEnd + 500; }
    int down = t < g->burstEnd;
    if (down && !g->pressed) g_gunPresses++;
    g->pressed = down;
    return down;
}
static int gun_trigger(Gunner *g, int i, ULONGLONG t, float errY, float errP, float tolY, float tolP)
{
    float y = fabsf(errY), p = fabsf(errP);
    if (y < tolY && p < tolP) g->trigOn = 1;
    else if (y >= 2 * tolY || p >= 2 * tolP) g->trigOn = 0;
    if (!g->trigOn) { g->pressed = 0; return 0; }
    g->trigSeen = t;
    return gun_burst(g, i, t);
}
static int gun_trigger_lost(Gunner *g, int i, ULONGLONG t)   // no target this frame
{
    if (g->trigOn && t - g->trigSeen < 500) return gun_burst(g, i, t);
    g->trigOn = 0; g->pressed = 0;
    return 0;
}
static void gun_stats(int i, ULONGLONG t)
{
    static ULONGLONG at; static unsigned hits0; unsigned hits = 0;
    if (t < at) return;
    for (int k = 0; k < H->botCount; k++) hits += g_hitsBy[k];
    if (at) H->log("gunstat: last 30 s, seat frames: no target %u, aiming %u, firing %u (%u%% of aiming), %u trigger presses; hits on soldiers by all bots %u", g_gunNoTarget, g_gunAiming, g_gunFiring, g_gunAiming ? g_gunFiring * 100 / g_gunAiming : 0, g_gunPresses, hits - hits0);
    at = t + 30000; hits0 = hits; g_gunNoTarget = g_gunAiming = g_gunFiring = g_gunPresses = 0; (void)i;
    if (t - g_alSavedAt > 180000) aimlearn_save();
}
// (the tester 2026-11-07: "Depending on where the chopper is aiming it can affect where it can see/aim/shoot") A gun holds its aim in the world, but only inside an arc of the body. Each seat learns it:
// where the gun has been counts as reachable; where it stopped short under full input (0.4 s, not moving against the body) counts as out of reach. Targets out of reach aren't picked and a
// target leaving the arc is dropped. A wrong reading can't silence a seat for good: 3 s in every 30 the filter is off, and reaching a direction clears it. Seats with no gun matrix learn nothing.
static float gun_body(void *v) { const float *m = name_is_air(vehicle_name(v)) ? vehicle_engine_xf(v) : NULL; return m ? atan2f(m[8], m[10]) : veh_yaw(v); }
static int arc_bin(float rel) { int b = (int)((wrap_pi(rel) + 3.14159265f) / 6.2831853f * ARC_BINS); return b < 0 ? 0 : b >= ARC_BINS ? ARC_BINS - 1 : b; }
static int arc_blocked(const AimLearn *a, float rel)
{
    if (!a) return 0;
    int k = arc_bin(rel);
    return a->arc[k] == 2 && a->arc[(k + 1) % ARC_BINS] != 1 && a->arc[(k + ARC_BINS - 1) % ARC_BINS] != 1;   // (the bins beside the reachable ones are the margin)
}
static int gun_arc_retry(int i, ULONGLONG t) { return ((t + (unsigned)i * 1700) % 30000) < 3000; }
static void gun_arc_tick(Gunner *g, AimLearn *a, Bot *b, void *v, int seat, ULONGLONG t, float have, float err, float haveP, float perr, int usePitch)
{
    float rel = wrap_pi(have - gun_body(v)), moved = fabsf(wrap_pi(rel - g->lastRel)); g->lastRel = rel;
    float movedP = fabsf(haveP - g->lastHaveP); g->lastHaveP = haveP;
    if (!a) return;
    if (a->arc[arc_bin(rel)] != 1) { a->arc[arc_bin(rel)] = 1; g_alDirty = 1; }
    if (fabsf(b->turn) >= 0.8f && moved < 0.002f) {
        if (!g->pinSince) g->pinSince = t;
        else if (t - g->pinSince > 400) {
            float span = wrap_pi(err); int steps = (int)(fabsf(span) / (6.2831853f / ARC_BINS)) + 1;
            for (int k = 1; k <= steps; k++) { int q = arc_bin(rel + (span > 0 ? 1 : -1) * k * (6.2831853f / ARC_BINS)); if (a->arc[q] != 1) a->arc[q] = 2; }
            g_alDirty = 1; g->pinSince = 0; g->banPl = g->scanPl; g->banUntil = t + 4000; g->scanAt = 0; g->scanFound = 0;
            { static unsigned l; if (l++ < 300) { int r = 0, o = 0; for (int q = 0; q < ARC_BINS; q++) { r += a->arc[q] == 1; o += a->arc[q] == 2; } H->log("gunarc: %s seat %d: the gun stops at %+.2f rad from the nose, its target was %+.2f further: out of reach. Directions reached %d, out of reach %d of %d", vehicle_name(v), seat, rel, span, r, o, ARC_BINS); } }
        }
    } else g->pinSince = 0;
    if (usePitch && !g->pitchBlind) {
        if (fabsf(b->pitch) >= 0.8f && fabsf(perr) > 0.2f && movedP < 0.002f) {
            if (!g->pPinSince) g->pPinSince = t;
            else if (t - g->pPinSince > 400) {
                if (perr < 0) a->pLo = haveP; else a->pHi = haveP;
                g_alDirty = 1; g->pPinSince = 0; g->banPl = g->scanPl; g->banUntil = t + 4000; g->scanAt = 0; g->scanFound = 0;
                { static unsigned l; if (l++ < 100) H->log("gunarc: %s seat %d: elevation stops at %+.2f, target %+.2f further: limits now %+.2f to %+.2f", vehicle_name(v), seat, haveP, perr, a->pLo, a->pHi); }
            }
        } else g->pPinSince = 0;
        if (a->pLo > -3 && haveP < a->pLo) { a->pLo = haveP; g_alDirty = 1; }
        if (a->pHi < 3 && haveP > a->pHi) { a->pHi = haveP; g_alDirty = 1; }
    }
}
// Which way should a transport show its side? +1 / -1: the side of the nose its gunners' guns have been (the same sign as a bearing minus the heading); 0: no bot on a gun seat
static int gun_seat_side(void *v, int prev)
{
    int n = 0; float sum = 0;
    for (int k = 0; k < H->botCount; k++) {
        int s = -1; if (!H->bots[k] || player_vehicle(H->bots[k], &s) != v || s <= 0) continue;
        n++;
        AimLearn *a = al_seat(v, s);
        if (a) for (int q = 0; q < ARC_BINS; q++) if (a->arc[q] == 1) sum += sinf(((q + 0.5f) / ARC_BINS - 0.5f) * 6.2831853f);
    }
    return !n ? 0 : sum > 0.5f ? 1 : sum < -0.5f ? -1 : prev ? prev : 1;
}
// (the tester 2026-11-07: "I don't think they're aiming at anything in particular") Every 5 s per seat: is the gun's reported direction reaching the target? "pinned": full turn input and the
// reported direction doesn't move (the gun at the end of its arc, or the matrix isn't the gun). A seat on "estimate" reports its own arithmetic: its error always shrinks and proves nothing.
static void gun_diag(Gunner *g, Bot *b, void *v, int seat, ULONGLONG t, float errY, float errP, float have, float dist, float cone, int estimate)
{
    g->daY += fabsf(errY); g->daP += fabsf(errP); g->daN++; g->daDist = dist;
    if (fabsf(errY) < cone && fabsf(errP) < cone) g->daIn++;
    if (!estimate && g->daN > 1 && fabsf(b->turn) >= 0.8f && fabsf(wrap_pi(have - g->daHave)) < 0.002f) g->daPin++;
    g->daHave = have;
    if (b->buttons & BTN_FIRE) g->daFire++;
    if (!g->daAt) { g->daAt = t + 5000; return; }
    if (t < g->daAt) return;
    g->daAt = t + 5000;
    static unsigned lines; if (lines++ < 6000) {
        unsigned n = g->daN ? g->daN : 1;
        H->log("gunaim: %s in %s seat %d (yaw %s %#x+%#x, pitch %s, flip %d): target %.0f m%s, average error yaw %.2f pitch %.2f, now %+.2f %+.2f rad, on target %u%%, turn pinned %u%%, firing %u%%",
               (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat, estimate ? "estimate" : "matrix", g->yawM >= 0 ? g->po[g->yawM] : 0, g->yawM >= 0 ? g->off[g->yawM] : 0, g->pitchBlind ? "estimate" : "barrel", g->flip,
               dist, g->scanAir ? " (aircraft)" : "", g->daY / n, g->daP / n, errY, errP, g->daIn * 100 / n, g->daPin * 100 / n, g->daFire * 100 / n);
    }
    g->daY = g->daP = 0; g->daN = g->daIn = g->daPin = g->daFire = 0;
}
static int gunner_think(int i, Bot *b, void *v, int seat, int team)
{
    ULONGLONG t = now_ms();
    gun_stats(i, t);
    Gunner *g = &g_gun[i];
    const float *vp = veh_pos(v);
    if (!vp) return 0;
    if (g->veh != v || g->seat != seat) { memset(g, 0, sizeof *g); g->veh = v; g->seat = seat; g->yawM = g->pitchM = -1; g->at = t; }
    // (log 2026-11-07, 0.3.12: every seat of a helicopter "found" the same part, 2 - 3 rad in 1.2 s: the rotor. Average error 1.57 rad, i.e. no relation to the target.) A gun is what stays still with
    // no input, moves on +1 and comes back on -1: anything else (rotors, wheels, other seats' guns) is out.
    if (g->phase == 0) { gun_collect(g, v, vp); g->body0 = gun_body_yaw(v); for (int c = 0; c < g->nc; c++) { g->prevY[c] = g->yaw0[c]; g->prevF[c] = g->fy0[c]; } g->phase = 4; g->at = t; return 1; }
    if (g->phase == 4) {   // hands off
        float body = gun_body_yaw(v), db = wrap_pi(body - g->body0); g->body0 = body;
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            if (!m || !gun_ortho(m)) { g->dead[c] = 1; continue; }
            float y = atan2f(m[8], m[10]), d = wrap_pi(y - g->prevY[c]);
            if (!gun_local(m)) d = wrap_pi(d - db);
            if (fabsf(d) > 0.01f || fabsf(m[9] - g->prevF[c]) > 0.01f) g->dead[c] = 1;
            g->prevY[c] = y; g->prevF[c] = m[9];
        }
        if (t - g->at < 800) return 1;
        for (int c = 0; c < g->nc; c++) { g->yaw0[c] = g->prevY[c]; g->fy0[c] = g->prevF[c]; }
        g->body0 = gun_body_yaw(v); g->phase = 1; g->at = t; return 1;
    }
    if (g->phase == 1) {   // turning
        b->turn = 1;
        if (t - g->at < 1200) return 1;
        float body = wrap_pi(gun_body_yaw(v) - g->body0);
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            g->d1[c] = 0;
            if (!m || !gun_ortho(m) || g->dead[c]) continue;
            float d = wrap_pi(atan2f(m[8], m[10]) - g->yaw0[c]);
            if (!gun_local(m)) d = wrap_pi(d - body);   // (a world matrix turns with the body too)
            g->d1[c] = d;
        }
        g->phase = 5; g->at = t; return 1;
    }
    if (g->phase == 5) {   // and back
        b->turn = -1;
        if (t - g->at < 1200) return 1;
        float body = wrap_pi(gun_body_yaw(v) - g->body0), best = 0.15f; int nd = 0;
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            nd += g->dead[c];
            if (!m || !gun_ortho(m) || g->dead[c]) continue;
            float d = wrap_pi(atan2f(m[8], m[10]) - g->yaw0[c]);
            if (!gun_local(m)) d = wrap_pi(d - body);
            if (fabsf(d) > 0.4f) continue;   // (it didn't come back)
            if (fabsf(g->d1[c]) > best) { best = fabsf(g->d1[c]); g->yawM = c; g->yawSign = g->d1[c] > 0 ? 1.0f : -1.0f; }
        }
        for (int c = 0; c < g->nc; c++) { const float *m = gun_m(v, g->po[c], g->off[c]); if (m) g->fy0[c] = m[9]; }
        H->log("gunner: %s in %s seat %d - %d matrices, %d moving on their own, gun yaw %s (turned %.2f, +1 turn %s%s)", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat, g->nc, nd,
               g->yawM >= 0 ? "found" : "not found", best, g->yawSign > 0 ? "raises the heading" : "lowers the heading",
               g->yawM >= 0 && gun_m(v, g->po[g->yawM], g->off[g->yawM]) && gun_local(gun_m(v, g->po[g->yawM], g->off[g->yawM])) ? ", vehicle space" : "");
        g->phase = 2; g->at = t; return 1;
    }
    if (g->phase == 2) {   // pitching
        b->pitch = 1;
        if (t - g->at < 1200) return 1;
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            g->d1[c] = m && gun_ortho(m) && !g->dead[c] ? m[9] - g->fy0[c] : 0;
        }
        g->phase = 3; g->at = t; g->hits0 = g_hitsBy[i];
        return 1;
    }
    if (g->phase == 3 && t - g->at < 1200) {   // The test above held the gun up for 1.2 s: back down the same time, or "level" for a seat with no barrel reading was really a long way up
        b->pitch = -1; g->lastPitch = 0; return 1;
    }
    if (g->phase == 3 && !g->pitchDone) {
        g->pitchDone = 1;
        float best = 0.15f;
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            if (!m || !gun_ortho(m) || g->dead[c] || fabsf(m[9] - g->fy0[c]) > 0.1f) continue;   // (back where it was?)
            if (fabsf(g->d1[c]) > best) { best = fabsf(g->d1[c]); g->pitchM = c; g->pitchSign = g->d1[c] > 0 ? 1.0f : -1.0f; }
        }
        H->log("gunner: %s - barrel %s (forward row moved %.2f, +1 pitch %s)", (char *)b->player + OFF_PLAYER_NAME, g->pitchM >= 0 ? "found" : "not found", best, g->pitchSign > 0 ? "raises it" : "lowers it");
        // (the tester 2026-10-31: "buggy gunners were aiming at the sky but in the direction of the enemy buggy") no barrel found: elevation is dead-reckoned from the input, +1 raises, starting level.
        if (g->pitchM < 0) { g->pitchM = g->yawM; g->pitchSign = 1; g->pitchBlind = 1; g->estPitch = 0; g->pitchAt = t; }
        // +1 raises a gun (game convention: 2228 of 2872 barrels). A row that goes down on +1 points backwards: its forward is the other way, and that fixes it once, here.
        if (!g->pitchBlind && g->pitchSign < 0) { g->flip = 1; g->pitchSign = 1; }
        // No flips on "no hits" (the tester 2026-11-07: "point erratically upwards, or back or front")
        g->rowFlips = 2; g->pChkFlips = 1;
    }
    if (g->yawM < 0) {
        // (the tester 2026-10-29: "mounted guns on choppers don't seem to be used" - most seats never found their gun's matrix
        // and the gunner just sat there) Blind: the gun starts facing the vehicle's nose; its heading is followed by
        // adding up the turn input (about 2 rad/s at full input); fire in bursts once roughly on the nearest enemy.
        if (!g->estOk) { g->estYaw = veh_yaw(v); g->estAt = t; g->estOk = 1; g->body0 = gun_body(v); }
        if (!name_is_air(vehicle_name(v))) { float bn = gun_body(v); g->estYaw = wrap_pi(g->estYaw + wrap_pi(bn - g->body0)); g->body0 = bn; }   // (a car's seat turns with the car, assumed; an aircraft's gun keeps its aim in the world)
        float dt = (t - g->estAt) / 1000.0f; g->estAt = t; if (dt > 0.2f) dt = 0.2f;
        float eye0[3] = { vp[0], vp[1] + 1.5f, vp[2] }, best0 = name_is_air(vehicle_name(v)) ? 280.0f : 220.0f, aim0[3]; int found0 = 0;
        if (t >= g->scanAt) { g->scanAt = t + 250; g->scanAir = 0;
        for (int j = 0; j < g_playerCount; j++) {
            void *pl = g_players[j]; int pt = FIELD(pl, OFF_PLAYER_TEAM, int);
            if (!pt || pt == team) continue;
            float q[3]; void *ev = player_vehicle(pl, NULL);
            float heavy0 = 0;
            if (ev) { const float *pp = veh_pos(ev); if (!pp) continue; memcpy(q, pp, 12); q[1] += 1.0f; const char *en = vehicle_name(ev); if (strstr(en, "tank")) heavy0 = 120; else if (name_is_air(en)) heavy0 = -20; }   /* (the tester 2026-11-07: "no one shoots at helicopters": aircraft first) */
            else { void *ss = bot_soldier(pl); if (!ss) continue; memcpy(q, soldier_pos(ss), 12); q[1] += 1.1f; }
            float d = sqrtf((q[0] - eye0[0]) * (q[0] - eye0[0]) + (q[1] - eye0[1]) * (q[1] - eye0[1]) + (q[2] - eye0[2]) * (q[2] - eye0[2]));
            if (d + heavy0 >= best0 || d < 3) continue;
            float o0 = name_is_air(vehicle_name(v)) ? 6.0f : 3.0f;
            float s0[3] = { eye0[0] + (q[0] - eye0[0]) / d * o0, eye0[1] + (q[1] - eye0[1]) / d * o0, eye0[2] + (q[2] - eye0[2]) / d * o0 };
            if (!gun_line_ok(s0, q)) continue;
            best0 = d + heavy0; memcpy(aim0, q, 12); found0 = 1; g->scanAir = ev && name_is_air(vehicle_name(ev)); if (g->scanAir) veh_lead(ev, aim0, d, 900);
        }
        g->scanFound = found0; if (found0) memcpy(g->scanAim, aim0, 12); if (g->scanAir) g->scanAt = t + 120; } else { found0 = g->scanFound; if (found0) memcpy(aim0, g->scanAim, 12); }
        if (!found0) { g_gunNoTarget++; if (gun_trigger_lost(g, i, t)) { g_gunFiring++; b->buttons |= BTN_FIRE; } return 1; }
        float err0 = wrap_pi(atan2f(aim0[0] - eye0[0], aim0[2] - eye0[2]) - g->estYaw);
        float u = fmaxf(-1, fminf(1, -err0 * 2));
        b->turn = u; g->estYaw = wrap_pi(g->estYaw - u * 2.0f * dt);
        {   // a helicopter's seat, or a helicopter as the target: elevation by dead reckoning, +1 raises at about 1 rad/s
            int inAir0 = name_is_air(vehicle_name(v)), usePitch0 = inAir0 || g->scanAir;
            float wantP0 = atan2f(aim0[1] - eye0[1], fmaxf(1, dist2d(aim0, eye0))), perr0 = 0;
            if (usePitch0) {
                if (inAir0 && !g->scanAir && wantP0 > 0.1f) wantP0 = 0.1f;
                float dtp = (t - g->pitchAt) / 1000.0f; if (dtp > 0.2f || dtp < 0) dtp = 0.2f; g->pitchAt = t;
                g->estPitch = fmaxf(-1.2f, fminf(0.9f, g->estPitch + g->lastPitch * dtp));
                perr0 = wantP0 - g->estPitch;
                b->pitch = fmaxf(-1, fminf(1, perr0 * 3)); g->lastPitch = b->pitch;
            } else b->pitch = 0;   /* (no elevation input - see below) */
            g_gunAiming++;
            if (gun_trigger(g, i, t, err0, perr0, 0.35f, 0.25f)) { g_gunFiring++; b->buttons |= BTN_FIRE; }
            gun_diag(g, b, v, seat, t, err0, perr0, g->estYaw, dist2d(aim0, eye0), 0.35f, 1);
        }
        return 1;
    }
    const float *ym = gun_m(v, g->po[g->yawM], g->off[g->yawM]), *pm = gun_m(v, g->po[g->pitchM], g->off[g->pitchM]);
    if (!ym || !pm) return 0;
    float fx = ym[8], fz = ym[10], fy = pm[9];
    float eye[3] = { ym[12], ym[13] + 0.3f, ym[14] };
    if (gun_local(ym)) {   // vehicle space -> world: through the vehicle's rows (right, up, forward)
        const float *vm = vehicle_xf(v);
        if (!vm) return 0;
        // (the tester 2026-11-07: "the MG doesn't turn with the helicopter, it has its own independent aim") an aircraft's gun direction is already in world terms: rotating it by the body again pointed
        // the "reported" gun somewhere else whenever the helicopter wasn't facing +z. The position still goes through the body. Ground vehicles as before (unverified either way).
        if (!name_is_air(vehicle_name(v))) {
            float lf[3] = { ym[8], pm[9], ym[10] };
            fx = lf[0] * vm[0] + lf[1] * vm[4] + lf[2] * vm[8];
            fy = lf[0] * vm[1] + lf[1] * vm[5] + lf[2] * vm[9];
            fz = lf[0] * vm[2] + lf[1] * vm[6] + lf[2] * vm[10];
        }
        eye[0] = vm[12] + ym[12] * vm[0] + ym[13] * vm[4] + ym[14] * vm[8];
        eye[1] = vm[13] + ym[12] * vm[1] + ym[13] * vm[5] + ym[14] * vm[9] + 0.3f;
        eye[2] = vm[14] + ym[12] * vm[2] + ym[13] * vm[6] + ym[14] * vm[10];
    }
    if (g->flip) { fx = -fx; fz = -fz; fy = -fy; }
    // the nearest enemy in sight (soldiers; and the vehicles enemies sit in)
    float best = name_is_air(vehicle_name(v)) ? 280 : 220, aim[3]; int found = 0;
    AimLearn *alArc = al_seat(v, seat); float bodyY = gun_body(v); int arcRetry = gun_arc_retry(i, t);
    if (t >= g->scanAt) { g->scanAt = t + 250; g->scanAir = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        int pt = FIELD(pl, OFF_PLAYER_TEAM, int);
        if (!pt || pt == team) continue;
        float q[3];
        void *ev = player_vehicle(pl, NULL);
        float heavy = 0;
        if (ev) { const float *p = veh_pos(ev); if (!p) continue; memcpy(q, p, 12); q[1] += 1.0f; const char *en = vehicle_name(ev); if (strstr(en, "tank")) heavy = 120; else if (name_is_air(en)) heavy = -20; }   /* (aircraft first) */
        else { void *s = bot_soldier(pl); if (!s) continue; memcpy(q, soldier_pos(s), 12); q[1] += 1.1f; }
        float d = sqrtf((q[0] - eye[0]) * (q[0] - eye[0]) + (q[1] - eye[1]) * (q[1] - eye[1]) + (q[2] - eye[2]) * (q[2] - eye[2]));
        if (d + heavy >= best || d < 3) continue;   /* (the tester 2026-10-29: "target infantry and light vehicles") armour / aircraft count 120 m further */
        if (pl == g->banPl && t < g->banUntil) continue;
        if (alArc && !arcRetry) { float wp = atan2f(q[1] - eye[1], fmaxf(1, dist2d(q, eye))); if (arc_blocked(alArc, atan2f(q[0] - eye[0], q[2] - eye[2]) - bodyY) || wp < alArc->pLo - 0.1f || wp > alArc->pHi + 0.1f) continue; }
        // (the ray starts 2.5 m out along the line so the vehicle's own body doesn't block it)
        float o1 = name_is_air(vehicle_name(v)) ? 6.0f : 2.5f;
        float s0[3] = { eye[0] + (q[0] - eye[0]) / d * o1, eye[1] + (q[1] - eye[1]) / d * o1, eye[2] + (q[2] - eye[2]) / d * o1 };
        if (!gun_line_ok(s0, q)) continue;
        best = d + heavy; memcpy(aim, q, 12); found = 1; g->scanPl = pl; g->scanAir = ev && name_is_air(vehicle_name(ev)); if (g->scanAir) veh_lead(ev, aim, d, 900);
    }
    g->scanFound = found; if (found) memcpy(g->scanAim, aim, 12); if (g->scanAir) g->scanAt = t + 120; } else { found = g->scanFound; if (found) memcpy(aim, g->scanAim, 12); }
    if (!found) { g->fireSince = 0; g_gunNoTarget++; if (gun_trigger_lost(g, i, t)) { g_gunFiring++; b->buttons |= BTN_FIRE; } return 1; }
    AimLearn *al = al_seat(v, seat); int alBand = al_band(dist2d(aim, eye));
    float alLead = al ? al->b[alBand].v[0] : 1, alUp = al ? al->b[alBand].v[1] : 0, alYaw = al ? al->b[alBand].v[2] : 0;
    if (g->scanPl) {   // (the tester 2026-11-07: "the MGs shoot but aren't hitting") the sight line is checked 4 times a second, but where the target is: every frame, led by the shot's flight time, with this vehicle's own movement taken off
        int there = 0; for (int j = 0; j < g_playerCount && !there; j++) there = g_players[j] == g->scanPl;
        void *ev2 = there ? player_vehicle(g->scanPl, NULL) : NULL; const float *q2 = NULL; float dy2 = 1.1f;
        if (ev2) { q2 = veh_pos(ev2); dy2 = 1.0f; } else if (there) { void *s2 = bot_soldier(g->scanPl); if (s2) q2 = soldier_pos(s2); }
        if (q2) {
            aim[0] = q2[0]; aim[1] = q2[1] + dy2; aim[2] = q2[2];
            float dl = sqrtf((aim[0] - eye[0]) * (aim[0] - eye[0]) + (aim[1] - eye[1]) * (aim[1] - eye[1]) + (aim[2] - eye[2]) * (aim[2] - eye[2]));
            float shot = 900.0f / fmaxf(0.3f, alLead);
            if (ev2) veh_lead(ev2, aim, dl, shot);
            { float self[3] = { 0, 0, 0 }; veh_lead(v, self, dl, shot); aim[0] -= self[0]; aim[1] -= self[1]; aim[2] -= self[2]; }
            aim[1] += alUp;
        }
    }
    float want = atan2f(aim[0] - eye[0], aim[2] - eye[2]), have = atan2f(fx, fz), err = wrap_pi(want - have + alYaw);
    if (al && !arcRetry && arc_blocked(al, want - bodyY)) { g->scanAt = 0; g->scanFound = 0; g_gunNoTarget++; if (gun_trigger_lost(g, i, t)) { g_gunFiring++; b->buttons |= BTN_FIRE; } return 1; }   // (it left the arc: the next one)
    float wantP = atan2f(aim[1] - eye[1], dist2d(aim, eye)), haveP = asinf(fmaxf(-1, fminf(1, fy))), perr;
    if (g->pitchBlind) {
        float dtp = (t - g->pitchAt) / 1000.0f; if (dtp > 0.2f) dtp = 0.2f; g->pitchAt = t;
        g->estPitch = fmaxf(-0.9f, fminf(0.5f, g->estPitch + g->lastPitch * 1.0f * dtp));   // (last frame's input: +1 raises, about 1 rad/s)
        haveP = g->estPitch;
    }
    perr = wantP - haveP;
    b->turn = fmaxf(-1, fminf(1, g->yawSign * err * 3));
    // (the tester 2026-11-01, twice: "gunners still shoot the sky" - the barrel matrix and the input sign are found wrong too often
    // and the loop then drives the gun all the way up) no elevation input: the gun stays level as it was when manned
    // (the tester 2026-11-07: "gunners on choppers are not shooting, they should be aiming downwards") that rule kept every helicopter gun level, and it only
    // fired at what was within 20 degrees of level: nothing, from the air. A helicopter's seat, or a helicopter as the target, does elevate. Below the
    // horizon unless it is aiming at an aircraft. The sign found is checked: the error growing under a firm input for 1.5 s flips it, once.
    int inAir = name_is_air(vehicle_name(v)), usePitch = inAir || g->scanAir || !g->pitchBlind;
    if (usePitch) {
        if (inAir && !g->scanAir && wantP > 0.1f) wantP = 0.1f;
        perr = wantP - haveP;
        b->pitch = fmaxf(-1, fminf(1, g->pitchSign * perr * 4)); g->lastPitch = g->pitchBlind ? g->pitchSign * b->pitch : 0;
        if (fabsf(b->pitch) > 0 && fabsf(b->pitch) < 0.08f && fabsf(perr) > 0.01f) b->pitch = b->pitch < 0 ? -0.08f : 0.08f;   // (a very small input may not move the gun)
        if (fabsf(b->turn) > 0 && fabsf(b->turn) < 0.08f && fabsf(err) > 0.01f) b->turn = b->turn < 0 ? -0.08f : 0.08f;
        if (!g->pitchBlind) {
            if (!g->pChkAt || fabsf(b->pitch) < 0.5f) { g->pChkAt = t; g->pChkErr = fabsf(perr); }
            else if (t - g->pChkAt > 1500) {
                if (fabsf(perr) > g->pChkErr + 0.1f && g->pChkFlips < 1) { g->pitchSign = -g->pitchSign; g->pChkFlips++; static unsigned pf; if (pf++ < 20) H->log("gunner: %s - the barrel went the wrong way: pitch sign flipped", (char *)b->player + OFF_PLAYER_NAME); }
                g->pChkAt = t; g->pChkErr = fabsf(perr);
            }
        }
        float dd = fmaxf(1, dist2d(aim, eye)), cone = fmaxf(0.12f, fminf(0.42f, atanf(30.0f / dd))), tolP = g->pitchBlind ? 0.5f : cone;   // (30 m at the target, 7 to 24 degrees)
        g_gunAiming++;
        // Near the target by its own transform: 14 m off, 4.6 to 11 degrees (8 m stopped them firing: 1 - 2% of frames) (0.22 rad was 44 m off at 200 m). No firing on yaw alone.
        float tolY = cone;
        // Is the gun's forward row the right way round? 10 s of real firing within 150 m with no hit on anyone: the other way, twice at most
        if (dd < 150 && fabsf(err) < tolY && fabsf(perr) < tolP) {
            if (!g->rowChkAt) { g->rowChkAt = t; g->rowHits = g_hitsBy[i] + g_vehHitsBy[i]; }
            else if (g_hitsBy[i] + g_vehHitsBy[i] != g->rowHits) { g->rowChkAt = 0; g->rowFlips = 2; if (al && !al->known) { al->known = 1; al->flip = g->flip; al->pitchSign = g->pitchSign; g_alDirty = 1; } }
            else if (t - g->rowChkAt > 10000 && g->rowFlips < 2) {
                if (g->pitchBlind && g->rowFlips == 0) { g->pitchSign = -g->pitchSign; H->log("gunner: %s in %s - 10 s of aimed fire, no hits, no barrel reading: elevation sign now %+.0f", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), g->pitchSign); }
                else { g->flip = !g->flip; H->log("gunner: %s in %s - 10 s of aimed fire, no hits: the gun's forward row taken the other way", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v)); }
                g->rowFlips++; g->rowChkAt = 0;
            }
        }
        if (gun_trigger(g, i, t, err, perr, tolY, tolP)) { g_gunFiring++; b->buttons |= BTN_FIRE; static ULONGLONG fa; if (t >= fa) { fa = t + 5000; H->log("gunner: %s in %s fires at %s, %.0f m away, %.2f rad", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), g->scanAir ? "a helicopter" : "a target", dd, wantP); } }
        gun_arc_tick(g, al, b, v, seat, t, have, err, haveP, perr, 1);
        gun_diag(g, b, v, seat, t, err, perr, wrap_pi(have - bodyY), dd, cone, 0);
        {   // learned aim: what this seat's shots did
            unsigned now = g_hitsBy[i] + g_vehHitsBy[i], got = now >= g->alHits ? now - g->alHits : 0;
            float dtA = g->alAt ? (t - g->alAt) / 1000.0f : 0; if (dtA > 0.2f) dtA = 0.2f;
            g->alAt = t; g->alHits = now;
            al_tick(al, alBand, (b->buttons & BTN_FIRE) != 0, dtA, got, 10);
        }
        return 1;
    }
    b->pitch = 0; g->lastPitch = 0; (void)perr;
    g_gunAiming++;
    gun_arc_tick(g, al, b, v, seat, t, have, err, haveP, 0, 0);
    { int fireNow = gun_trigger(g, i, t, err, wantP, 0.22f, 0.35f); if (fireNow) b->buttons |= BTN_FIRE; gun_diag(g, b, v, seat, t, err, wantP, wrap_pi(have - bodyY), dist2d(aim, eye), 0.22f, 0); }
    if (b->buttons & BTN_FIRE) {   /* (the gun level: fire at what is near its height) */   /* (was 0.1: they hardly ever fired) */
        g_gunFiring++;
        if (!g->fireSince) { g->fireSince = t; g->hits0 = g_hitsBy[i]; }
        if (0 && t - g->fireSince > 6000 && g_hitsBy[i] == g->hits0 && t > g->flipAt) {
            g->flip = !g->flip; g->flipAt = t + 8000; g->fireSince = 0;
            H->log("gunner: %s - 6 s of fire, no hits: the gun's forward row taken the other way (%d)", (char *)b->player + OFF_PLAYER_NAME, g->flip);
        }
    } else if (fabsf(err) > 0.4f) g->fireSince = 0;
    return 1;
}


// Passengers who keep their soldier (cars' and bikes' back seats, side seats: the tester 2026-11-07 "my passenger was aiming the wrong way"). soldier_yaw reads the soldier's body rotation, which in a seat need not
// follow the aim. Each seat is measured: 0.5 s of +1 turn and back. Whichever follows the input is used: the body row (a world direction) or the soldier's heading field. The field may be relative to the vehicle
// or absolute, so it is tried as relative first (the row didn't move, and a world reading in a seat would have kept them on target) and the other way after 10 s of firing without a hit; a hit settles it (aim.txt).
#define PAX_ROW 1
#define PAX_FIELD 2
static float pax_row(void *s) { return soldier_yaw(s); }
static float pax_field(void *s) { return FIELD(s, OFF_SOLDIER_YAW, float); }
static int pax_think(int i, Bot *b, void *soldier, void *v, int team)
{
    ULONGLONG t = now_ms();
    int seat = FIELD(b->player, OFF_PLAYER_ENTRY_INDEX, int);
    Gunner *g = &g_gun[i];
    if (g->veh != v || g->seat != seat || !g->pitchDone) {
        if (g->veh != v || g->seat != seat) { memset(g, 0, sizeof *g); g->veh = v; g->seat = seat; g->at = t; g->phase = 20; g->body0 = gun_body(v); g->yaw0[0] = pax_row(soldier); g->yaw0[1] = pax_field(soldier); }
        float body = wrap_pi(gun_body(v) - g->body0);
        if (g->phase == 20) { b->turn = 1; if (t - g->at < 500) return 1; g->d1[0] = wrap_pi(pax_row(soldier) - g->yaw0[0] - body); g->d1[1] = wrap_pi(pax_field(soldier) - g->yaw0[1]); g->phase = 21; g->at = t; return 1; }
        b->turn = -1; if (t - g->at < 500) return 1;
        float r = g->d1[0], f = g->d1[1];
        g->yawM = fabsf(r) > 0.15f ? PAX_ROW : fabsf(f) > 0.15f ? PAX_FIELD : 0;
        g->yawSign = (g->yawM == PAX_ROW ? r : f) > 0 ? 1.0f : -1.0f;
        g->pitchDone = 1; g->phase = 22;
        { AimLearn *al0 = NULL; char key[48]; snprintf(key, sizeof key, "%s_pax%d", vehicle_name(v), seat); if (g_aimLearn) { aimlearn_load(); al0 = al_find(key, 1); } if (al0 && al0->known) { g->flip = al0->flip; g->rowFlips = 3; } }
        H->log("paxaim: %s in %s seat %d: aim %s (body row moved %+.2f, heading field %+.2f on 0.5 s of input)", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat, g->yawM == PAX_ROW ? "follows the body row" : g->yawM == PAX_FIELD ? "follows the heading field" : "not readable: this seat isn't aimed", r, f);
        return 1;
    }
    if (!g->yawM) return 0;
    AimLearn *al = NULL; { char key[48]; snprintf(key, sizeof key, "%s_pax%d", vehicle_name(v), seat); if (g_aimLearn) al = al_find(key, 1); }
    const float *p = soldier_pos(soldier); float eye[3] = { p[0], p[1] + 1.6f, p[2] }, bodyY = gun_body(v);
    float aim[3]; int found = 0, arcRetry = gun_arc_retry(i, t);
    if (t >= g->scanAt) {
        g->scanAt = t + 250; float best = 150; g->scanPl = NULL;
        for (int j = 0; j < g_playerCount; j++) {
            void *pl = g_players[j]; int pt = FIELD(pl, OFF_PLAYER_TEAM, int);
            if (!pt || pt == team || (pl == g->banPl && t < g->banUntil)) continue;
            float q[3]; void *ev = player_vehicle(pl, NULL);
            if (ev) { const float *pp = veh_pos(ev); if (!pp) continue; memcpy(q, pp, 12); q[1] += 1.0f; }
            else { void *ss = bot_soldier(pl); if (!ss) continue; memcpy(q, soldier_pos(ss), 12); q[1] += 1.15f; }
            float dx = q[0] - eye[0], dy = q[1] - eye[1], dz = q[2] - eye[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d >= best || d < 2) continue;
            if (al && !arcRetry && arc_blocked(al, atan2f(dx, dz) - bodyY)) continue;
            float s0[3] = { eye[0] + dx / d * 2.5f, eye[1] + dy / d * 2.5f, eye[2] + dz / d * 2.5f };
            if (ev ? !air_line_ok(s0, q) : !gun_line_ok(s0, q)) continue;
            best = d; g->scanPl = pl;
        }
        if (g->scanPl && g->scanPl != g->daTgt) { g->daTgt = g->scanPl; g->seenAt = t; }
    }
    if (g->scanPl) {
        int there = 0; for (int j = 0; j < g_playerCount && !there; j++) there = g_players[j] == g->scanPl;
        void *ev = there ? player_vehicle(g->scanPl, NULL) : NULL; const float *q2 = NULL; float dy2 = 1.15f; void *es = NULL;
        if (ev) { q2 = veh_pos(ev); dy2 = 1.0f; } else if (there && (es = bot_soldier(g->scanPl))) q2 = soldier_pos(es);
        if (q2) {
            aim[0] = q2[0]; aim[1] = q2[1] + dy2; aim[2] = q2[2]; found = 1;
            float d0 = sqrtf((aim[0] - eye[0]) * (aim[0] - eye[0]) + (aim[1] - eye[1]) * (aim[1] - eye[1]) + (aim[2] - eye[2]) * (aim[2] - eye[2]));
            if (ev) veh_lead(ev, aim, d0, 600);
            else { const float *ve = (const float *)((char *)es + OFF_SOLDIER_VELOCITY); float tf = d0 / 600.0f + 0.08f; aim[0] += ve[0] * tf; aim[1] += ve[1] * tf; aim[2] += ve[2] * tf; }
            { float self[3] = { 0, 0, 0 }; veh_lead(v, self, d0, 600); aim[0] -= self[0]; aim[1] -= self[1]; aim[2] -= self[2]; }
            float wob = (4 - g_set.difficulty) * 0.4f; aim[0] += wob * sinf(t / 700.0f); aim[2] += wob * cosf(t / 900.0f);   // (the lower difficulties miss more)
        }
    }
    if (!found) { g->scanPl = NULL; b->turn = 0; b->pitch = 0; return 1; }
    float d = fmaxf(1, dist2d(aim, eye)), want = atan2f(aim[0] - eye[0], aim[2] - eye[2]);
    float have = g->yawM == PAX_ROW ? pax_row(soldier) : wrap_pi(pax_field(soldier) + (g->flip ? 0 : bodyY));
    float err = wrap_pi(want - have);
    b->turn = fmaxf(-1, fminf(1, g->yawSign * err * 3));
    if (fabsf(b->turn) > 0 && fabsf(b->turn) < 0.06f && fabsf(err) * d > 0.3f) b->turn = b->turn < 0 ? -0.06f : 0.06f;
    float el = view_elevation(soldier), perr = 0;
    if (el == el) { perr = atan2f(aim[1] - eye[1], d) - el; b->pitch = fmaxf(-1, fminf(1, g_pvUp * perr * 3.0f)); } else b->pitch = 0;
    gun_arc_tick(g, al, b, v, seat, t, have, err, 0, 0, 0);
    if (!g->scanPl) return 1;   // (out of reach, just found: the next one)
    int on = fabsf(err) * d < 5 && fabsf(perr) * d < 5;
    if (on && t - g->seenAt > (ULONGLONG)(P->reaction * 1000) && (t % 1600) < 1100 && (H->frame & 3) != 3) { b->buttons |= BTN_FIRE; g->fireS += 1; }
    unsigned hits = g_hitsBy[i] + g_vehHitsBy[i];
    if (g->yawM == PAX_FIELD && g->rowFlips < 3) {   // relative or absolute? A hit settles it
        if (hits != g->rowHits && g->fireS > 0) { g->rowFlips = 3; if (al) { al->known = 1; al->flip = g->flip; g_alDirty = 1; } H->log("paxaim: %s seat %d: a hit - its aim is %s", vehicle_name(v), seat, g->flip ? "absolute" : "relative to the vehicle"); }
        else if (g->fireS > 300) { g->flip = !g->flip; g->rowFlips++; g->fireS = 0; H->log("paxaim: %s seat %d: 300 firing frames, no hit - trying its aim as %s", vehicle_name(v), seat, g->flip ? "absolute" : "relative to the vehicle"); }
        g->rowHits = hits;
    }
    g->daY += fabsf(err); g->daP += fabsf(perr); g->daN++; g->daIn += on; g->daFire += (b->buttons & BTN_FIRE) != 0;
    if (fabsf(b->turn) >= 0.8f && fabsf(wrap_pi(have - bodyY - g->daHave)) < 0.002f) g->daPin++;
    g->daHave = wrap_pi(have - bodyY);
    if (!g->daAt) { g->daAt = t + 5000; g->alHits = hits; }
    else if (t >= g->daAt) {
        g->daAt = t + 5000; unsigned n = g->daN ? g->daN : 1;
        static unsigned lines; if (lines++ < 4000) H->log("paxaim: %s in %s seat %d (%s, %s): target %.0f m, average error yaw %.2f pitch %.2f, on target %u%%, turn pinned %u%%, firing %u%%, hits %u",
            (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat, g->yawM == PAX_ROW ? "row" : "field", g->flip ? "absolute" : "relative", d, g->daY / n, g->daP / n, g->daIn * 100 / n, g->daPin * 100 / n, g->daFire * 100 / n, hits - g->alHits);
        g->alHits = hits; g->daY = g->daP = 0; g->daN = g->daIn = g->daPin = g->daFire = 0;
    }
    return 1;
}
