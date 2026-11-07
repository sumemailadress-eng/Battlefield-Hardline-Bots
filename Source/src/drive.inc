// Thugbot driving (included by brain.inc after vehicles.inc): bots that sit in a car.
//
//   Where the car is: the vehicle's physics part at [vehicle + 0x208]: rows right / up / forward at +0x80 / +0x90 / +0xa0,
//   position at +0xb0 (found with "vehprobe" on Chinatown's cars, 2026-10-23; +0x1c50 only held it on some models).
//   Controls: the same pseudo-player inputs as on foot - forward = throttle, backward = brake / reverse, turn = steering
//   (positive turns to a lower heading; "drivetest 1" turned a car from 1.24 to -1.65 rad in 4 s).
//   Road map: grid nodes at least 2 m (1 cell) from any wall, pillar or ledge (the clearance map) - open streets and
//   squares; 4 m and more is preferred. Routes for cars are planned on that only (nav.c's vehicle mode), body 2.6 m wide.
//   Driver (seat 0): drives the road map to its flag, slows for bends, backs up and tries again when it hits something;
//   35 m from the flag (or with no road there, or stuck three times) it stops and everyone gets out.
//   Passengers: ride along and get out when the car has stopped for 3 s; the driver's arrival makes them get out too.

#define VEH_ARRIVE_M 35.0f
static int g_heliMode = THUGBOT_HELI_PORT ? 2 : 0;   // heli.inc: 0 pilots get out, 1 control probe, 2 fly (ini HeliFly, command "heli N")
static int name_is_air(const char *nm);
static int heli_think(int i, Bot *b, void *v, int seat, int team);
static int gunner_think(int i, Bot *b, void *v, int seat, int team);
static const char *vehicle_name(void *v);
static unsigned char *g_driveMask;
static const NavGrid *g_driveMaskGrid;
static unsigned g_driveMaskSerial;
static unsigned g_drives, g_driveArrivals, g_driveGiveUps;

typedef struct Drive { ULONGLONG noMaskSince, navWaitAt; int checkedFront, yield; ULONGLONG startAt, lookAt, edgeAt; float freeC, freeL, freeR; void *veh; ULONGLONG progressAt, reverseUntil, stoppedSince, exitFrom, noRouteSince, traceAt; float progressPos[3], reverseTurn, roadGoal[3], planGoal[3]; int stuck; ULONGLONG turnAroundAt; float stuckYaw; int xfRejected;
                       float motionYawPos[3], stYaw, stTurn; int lastSign, xfKey, stN; ULONGLONG signAt, stAt; float lastFwd, motionPos[3], motionYaw, matrixOff, avoidTurn; ULONGLONG yieldSince; ULONGLONG motionPosAt, motionAt; int motionN; int onNet, onLine, queueing, turningAround; float netGoal[3], cross, queueGap, footAhead, speed, speedPos[3], prevErr; ULONGLONG speedAt, queueAt, queueSince, blockStillSince, passUntil, laneUntil; } Drive;
static Drive g_drv[MAX_BOTS];
// (On foot: the next time it sits in any vehicle is a new drive. Getting back into the same car kept exitFrom set: it got straight out again.)
static void vehicle_onfoot_reset(int i) { g_drv[i].veh = NULL; }
static struct { void *v; ULONGLONG at; int flag; } g_crewCall[MAX_BOTS];   // a driver stopped for a flag: its passengers get out too

static char *vehicle_body(void *v) { char *o = v && !IsBadReadPtr((char *)v + 0x208, 8) ? FIELD(v, 0x208, char *) : NULL; return o && !IsBadReadPtr(o, 0xc0) ? o : NULL; }
// Where each vehicle keeps its world transform differs by model (Chinatown: [+0x208]+0x80 on most cars; Hollywood
// Heights: [+0xcf0]+0x40 ...). So it is found per vehicle: the first matrix, in the vehicle or one pointer away, that is
// orthonormal, upright (a car's up row points up; a spinning wheel's doesn't) and sits on the nav grid's ground (within
// 2.5 m). Remembered per vehicle and checked again on every read; a vehicle that no longer passes is searched again.
#define MAX_VEH_XF 256   /* (more than MAX_KNOWN_VEH: a flown helicopter's transform must not be pushed out) */
#define VEH_CANDS 64
static struct { void *v; int po, off; ULONGLONG retryAt; float last[3]; int flip, flipVotes, keepVotes; float samplePos[3]; int skip;
                int nc, cpo[VEH_CANDS], coff[VEH_CANDS]; float c0[VEH_CANDS][4]; ULONGLONG candAt; int candDone; } g_vehXf[MAX_VEH_XF];
static int g_vehXfNext;
static int g_xfAir;   // searching an aircraft's transform: it needn't sit on the ground
static int vehicle_matrix_ok(const float *m)
{
    if (!plausible_matrix(m) || m[5] < 0.75f) return 0;   // (0.75: motorcycles lean on their stand)
    if (m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f) return 0;   // an exact identity rotation: a local / spawn-point matrix, not a placed vehicle
    if (!g_nav || g_navBuilding) return 1;   // (no grid yet: upright and plausible will do)
    float out[3];
    if (g_xfAir) {   // an aircraft: anywhere with ground below it (part-local matrices sit near 0 0 0, under the whole map)
        float a[3] = { m[12], m[13] + 0.5f, m[14] }, e[3] = { m[12], m[13] - 400, m[14] };
        return ray_cast(a, e, out, NULL);
    }
    return nav_snap(g_nav, m[12], m[13], m[14], out) && fabsf(out[1] - m[13]) < 2.5f;
}
static float g_xfHint[3]; static int g_xfHintOn;
static int veh_xf_index(void *v);
// The entity's own world transform, as the engine reads it (vehicle vtable slot 26, RVA 0xfebf00 - the same getter
// every spatial entity uses): [this + 0x38] holds a small ring of transforms, the current one picked by its bytes
// +9 and +10: [+0x38] + (b9 + 2 * b10) * 0x20 + 0x10, rows right / up / forward / position. Found 2026-10-25 by
// decompiling the vehicle class's vtable (fun-bots on BF3 reads the same thing as controlledControllable.transform).
static const float *vehicle_engine_xf(void *v)
{
    char *o = !IsBadReadPtr((char *)v + 0x38, 8) ? FIELD(v, 0x38, char *) : NULL;
    if (!o || ((uintptr_t)o & 7) || IsBadReadPtr(o, 16)) return NULL;
    int idx = (unsigned char)o[9] + (unsigned char)o[10] * 2;
    const float *m = (const float *)(o + idx * 0x20 + 0x10);
    // (The getter takes the index as it is. The old "idx > 8" limit was never in the engine: FloodBots found it threw every
    // BF4 car's real transform away, 2026-10-31. Limit as there: b9 + 2 * b10 can't pass 765. Logged once per vehicle type.)
    if (idx > 765 || IsBadReadPtr(m, 64) || !plausible_matrix(m)) {
        static char seen[32][40]; static int seenN; const char *nm = vehicle_name(v); int k = 0;
        while (k < seenN && strcmp(seen[k], nm)) k++;
        if (k == seenN && seenN < 32) {
            strncpy(seen[seenN], nm, 39); seen[seenN++][39] = 0;
            H->log("drive: engine transform of %s unusable - idx %d (b9 %d b10 %d)%s", nm, idx, (unsigned char)o[9], (unsigned char)o[10], idx > 765 ? " too big" : IsBadReadPtr(m, 64) ? " unreadable" : " not a matrix");
        }
        return NULL;
    }
    {
        static char okN_[32][40]; static int okN; const char *nm = vehicle_name(v); int k = 0;
        while (k < okN && strcmp(okN_[k], nm)) k++;
        if (k == okN && okN < 32) {
            strncpy(okN_[okN], nm, 39); okN_[okN++][39] = 0;
            H->log("drive: engine transform of %s in use - idx %d (b9 %d b10 %d), at %.1f %.1f %.1f, forward %.2f %.2f %.2f", nm, idx, (unsigned char)o[9], (unsigned char)o[10], m[12], m[13], m[14], m[8], m[9], m[10]);
        }
    }
    return m;
}
static int g_xfEngine = 1;   // "xfengine 0": back to the matrix search
static const float *vehicle_xf(void *v)
{
    if (!v) return NULL;
    if (g_xfEngine) { const float *m = vehicle_engine_xf(v); if (m) return m; }
    int k = 0;
    while (k < MAX_VEH_XF && g_vehXf[k].v != v) k++;
    if (k < MAX_VEH_XF && g_vehXf[k].po >= -1) {
        char *o = g_vehXf[k].po < 0 ? (char *)v : FIELD(v, g_vehXf[k].po, char *);
        const float *m = o && !IsBadReadPtr(o + g_vehXf[k].off, 64) ? (const float *)(o + g_vehXf[k].off) : NULL;
        if (m && plausible_matrix(m) && m[5] > 0.3f) {   // (the one in use: leaning bikes, a car on a slope or tipping still count)
            // a car can't cover 60 m between two reads a few frames apart: that's another object's matrix now
            if (g_vehXf[k].last[0] == 0 || dist2d(g_vehXf[k].last, m + 12) < 60) { memcpy(g_vehXf[k].last, m + 12, 12); return m; }
            g_vehXf[k].last[0] = 0;
        }
    }
    ULONGLONG t = now_ms();
    if (k < MAX_VEH_XF && t < g_vehXf[k].retryAt) return NULL;
    g_xfAir = name_is_air(vehicle_name(v));
    if (k == MAX_VEH_XF) { k = g_vehXfNext++ % MAX_VEH_XF; memset(&g_vehXf[k], 0, sizeof g_vehXf[k]); g_vehXf[k].v = v; }
    int skip = g_vehXf[k].skip, hintPo = -3, hintOff = 0, anyN = 0, anyPo[VEH_CANDS], anyOff[VEH_CANDS]; float hintBest = 10;
    g_vehXf[k].po = -2; g_vehXf[k].retryAt = t + 2000;
    for (int po = -1; po < 0x1000; po += (po < 0 ? 1 : 8)) {
        char *o = po < 0 ? (char *)v : FIELD(v, po, char *);
        int span = po < 0 ? 0x2400 : 0x400;
        if (!o || ((uintptr_t)o & 7) || (uintptr_t)o < 0x10000 || IsBadReadPtr(o, span)) continue;
        for (int off = 0; off + 64 <= span; off += 16)
            if (g_xfHintOn && vehicle_matrix_ok((const float *)(o + off))) {   // with a hint: the one nearest to it
                float dd = dist2d((const float *)(o + off) + 12, g_xfHint);
                if (anyN < VEH_CANDS) { anyPo[anyN] = po; anyOff[anyN++] = off; }
                if (dd < hintBest) { hintBest = dd; hintPo = po; hintOff = off; }
                if (dd < 10 && g_vehXf[k].nc < VEH_CANDS) {   // every matrix near the driver: the live one is picked by which moves
                    int c = g_vehXf[k].nc++; const float *cm = (const float *)(o + off);
                    g_vehXf[k].cpo[c] = po; g_vehXf[k].coff[c] = off; memcpy(g_vehXf[k].c0[c], cm + 12, 12); g_vehXf[k].c0[c][3] = atan2f(cm[8], cm[10]);
                }
            }
            else if (vehicle_matrix_ok((const float *)(o + off)) && skip-- <= 0) {
                g_vehXf[k].po = po; g_vehXf[k].off = off;
                static unsigned logged;
                if (logged++ < 60) H->log("drive: vehicle %p (vt %#llx) keeps its transform at %s%#x]+%#x%s", v, (unsigned long long)(*(uintptr_t *)v - H->image),
                                          po < 0 ? "[self" : "[+", po < 0 ? 0 : po, off, g_vehXf[k].skip ? " (after a frozen one)" : "");
                return (const float *)(o + off);
            }
    }
    if (hintPo > -3) {
        g_vehXf[k].po = hintPo; g_vehXf[k].off = hintOff;
        char *o = hintPo < 0 ? (char *)v : FIELD(v, hintPo, char *);
        H->log("drive: vehicle %p (%s) keeps its transform at %s%#x]+%#x - %.1f m from where its driver got in", v, vehicle_name(v), hintPo < 0 ? "[self" : "[+", hintPo < 0 ? 0 : hintPo, hintOff, hintBest);
        return (const float *)(o + hintOff);
    }
    if (g_xfHintOn && anyN) {   // nothing near the hint (a bot that spawned in the car: the hint is where it died): all of them are candidates
        for (int c = 0; c < anyN; c++) {
            char *o = anyPo[c] < 0 ? (char *)v : FIELD(v, anyPo[c], char *); const float *cm = (const float *)(o + anyOff[c]);
            g_vehXf[k].cpo[c] = anyPo[c]; g_vehXf[k].coff[c] = anyOff[c]; memcpy(g_vehXf[k].c0[c], cm + 12, 12); g_vehXf[k].c0[c][3] = atan2f(cm[8], cm[10]);
        }
        // the vehicle's own transform is kept in several of its parts (physics, render ...): the position most of them
        // agree on; the odd ones out are entry points, spawn points and other objects' matrices
        int pick = 0, pickN = -1;
        for (int c = 0; c < anyN; c++) {
            int same = 0;
            for (int e = 0; e < anyN; e++) if (e != c && dist2d(g_vehXf[k].c0[c], g_vehXf[k].c0[e]) < 0.5f && fabsf(g_vehXf[k].c0[c][1] - g_vehXf[k].c0[e][1]) < 0.5f) same++;
            if (same > pickN) { pickN = same; pick = c; }
        }
        g_vehXf[k].nc = anyN; g_vehXf[k].po = anyPo[pick]; g_vehXf[k].off = anyOff[pick];
        char *o = anyPo[pick] < 0 ? (char *)v : FIELD(v, anyPo[pick], char *);
        return (const float *)(o + anyOff[pick]);
    }
    return NULL;
}
// The car is where its driver got in: of all the matrices that pass, the one nearest there (10 m) - the first one found
// was often another copy (Riptide: SUVs "frozen" 30 - 900 m from where they really were).
static void vehicle_xf_locate(void *v, const float *at)
{
    int k = 0, oldPo = -9, oldOff = 0;
    while (k < MAX_VEH_XF && g_vehXf[k].v != v) k++;
    if (k < MAX_VEH_XF && g_vehXf[k].po >= -1) {
        char *o = g_vehXf[k].po < 0 ? (char *)v : FIELD(v, g_vehXf[k].po, char *);
        const float *m = o && !IsBadReadPtr(o + g_vehXf[k].off, 64) ? (const float *)(o + g_vehXf[k].off) : NULL;
        (void)m;   // (even when the one we have is right there: it may be a frozen copy - the candidates decide)
        oldPo = g_vehXf[k].po; oldOff = g_vehXf[k].off;
        g_vehXf[k].po = -2; g_vehXf[k].retryAt = 0; g_vehXf[k].last[0] = 0;
    }
    if (k < MAX_VEH_XF) { g_vehXf[k].nc = 0; g_vehXf[k].candDone = 0; g_vehXf[k].candAt = 0; }
    memcpy(g_xfHint, at, 12); g_xfHintOn = 1;
    vehicle_xf(v);
    k = veh_xf_index(v);
    if (k >= 0 && (g_vehXf[k].po != oldPo || g_vehXf[k].off != oldOff)) { g_vehXf[k].flip = g_vehXf[k].flipVotes = g_vehXf[k].keepVotes = 0; g_vehXf[k].samplePos[0] = 0; }
    if (k >= 0) { g_vehXf[k].candAt = now_ms(); static unsigned logged; if (logged++ < 40) H->log("drive: %s %p - %d candidate transform(s) within 10 m of the driver", vehicle_name(v), v, g_vehXf[k].nc); }
    g_xfHintOn = 0;
}
// The matrix found never changed while the car was driven (position and heading frozen to the last bit): a copy - the
// spawn point's, or the wreck's - not the car's. Use the next one the search finds.
static void vehicle_xf_reject(void *v)
{
    for (int k = 0; k < MAX_VEH_XF; k++) if (g_vehXf[k].v == v) {
        int skip = g_vehXf[k].skip + 1;
        memset(&g_vehXf[k], 0, sizeof g_vehXf[k]); g_vehXf[k].v = v; g_vehXf[k].po = -2; g_vehXf[k].skip = skip > 6 ? 0 : skip;
        H->log("drive: %s %p - its transform never moved: trying the next one (%d)", vehicle_name(v), v, g_vehXf[k].skip);
        return;
    }
}
static const float *veh_pos(void *v) { const float *m = vehicle_xf(v); return m ? m + 12 : NULL; }
// A second after the driver got in (throttle on): of the candidate matrices near it, the one that moved or turned most
// (and stayed plausible) is the car's - the others are frozen copies (spawn point, wreck, other parts). Several copies
// move with the car (body, chassis): any of them will do. None moved: the car really is standing still.
static int vehicle_xf_pick_moving(void *v, int throttleOn)
{
    int k = veh_xf_index(v);
    if (k >= 0 && !throttleOn && g_vehXf[k].candAt) { g_vehXf[k].candAt = now_ms(); return 0; }   // (only a car under throttle tells)
    if (k < 0 || g_vehXf[k].candDone || g_vehXf[k].nc < 2 || !g_vehXf[k].candAt || now_ms() - g_vehXf[k].candAt < 1200) return 0;
    g_vehXf[k].candDone = 1;
    int best = -1, curC = -1; float bestScore = 0.3f, curScore = 0;
    for (int c = 0; c < g_vehXf[k].nc; c++) {
        int po = g_vehXf[k].cpo[c], off = g_vehXf[k].coff[c];
        char *o = po < 0 ? (char *)v : FIELD(v, po, char *);
        if (!o || IsBadReadPtr(o + off, 64)) continue;
        const float *m = (const float *)(o + off);
        if (!vehicle_matrix_ok(m)) continue;
        float mv = dist2d(m + 12, g_vehXf[k].c0[c]), turn = fabsf(wrap_pi(atan2f(m[8], m[10]) - g_vehXf[k].c0[c][3]));
        if (mv > 40 || fabsf(m[13] - g_vehXf[k].c0[c][1]) > 40) continue;
        float score = mv + fabsf(m[13] - g_vehXf[k].c0[c][1]) + turn * 4;   // (a helicopter lifts off straight up)
        if (po == g_vehXf[k].po && off == g_vehXf[k].off) { curC = c; curScore = score; }
        if (score > bestScore) { bestScore = score; best = c; }
    }
    static unsigned logged;
    if (best >= 0 && best != curC && curScore < 0.1f) {
        if (logged++ < 40) H->log("drive: %s %p - the transform in use stood still (%.2f), [+%#x]+%#x moved %.1f: using that", vehicle_name(v), v, curScore, g_vehXf[k].cpo[best], g_vehXf[k].coff[best], bestScore);
        g_vehXf[k].po = g_vehXf[k].cpo[best]; g_vehXf[k].off = g_vehXf[k].coff[best]; g_vehXf[k].last[0] = 0;
        g_vehXf[k].flipVotes = g_vehXf[k].keepVotes = 0; g_vehXf[k].samplePos[0] = 0; return 1;
    } else if (best < 0 && logged++ < 40) H->log("drive: %s %p - none of %d candidate transforms moved with full throttle: the car is really standing", vehicle_name(v), v, g_vehXf[k].nc);
    return 0;
}
// Heading: the matrix's forward row - but on some models the part we found faces backwards ("drivetest": cars with
// throttle on went the opposite way to that row). Checked while driving (veh_calibrate) and flipped per vehicle.
static int veh_xf_index(void *v) { for (int k = 0; k < MAX_VEH_XF; k++) if (g_vehXf[k].v == v) return k; return -1; }
static float veh_yaw(void *v)
{
    const float *m = vehicle_xf(v);
    if (!m) return 0;
    int k = veh_xf_index(v);
    return wrap_pi(atan2f(m[8], m[10]) + (k >= 0 && g_vehXf[k].flip ? 3.14159265f : 0));
}
// (Ported from FloodBots 2026-11-06.) The engine's transform is the hull: its forward row holds standing still and when
// sliding, and which way it points is voted per vehicle while driving forwards.
static int veh_xf_trusted(void *v) { return g_xfEngine && vehicle_engine_xf(v) != NULL; }
#define FLIP_MAX 128
static void *g_flipV[FLIP_MAX]; static signed char g_flipVotes[FLIP_MAX]; static int g_flipNext;
static int veh_flip_slot(void *v, int make)
{
    for (int k = 0; k < FLIP_MAX; k++) if (g_flipV[k] == v) return k;
    if (!make) return -1;
    int k = g_flipNext++ % FLIP_MAX; g_flipV[k] = v; g_flipVotes[k] = 0; return k;
}
static int veh_row_flipped(void *v) { int k = veh_flip_slot(v, 0); return k >= 0 && g_flipVotes[k] > 0; }
static void veh_flip_vote(void *v, int flipped)
{
    int k = veh_flip_slot(v, 1);
    int n = g_flipVotes[k] + (flipped ? 1 : -1); g_flipVotes[k] = (signed char)(n > 5 ? 5 : n < -5 ? -5 : n);
}
static float veh_yaw_raw(void *v) { const float *m = vehicle_xf(v); return m ? wrap_pi(atan2f(m[8], m[10]) + (veh_xf_trusted(v) && veh_row_flipped(v) ? 3.14159265f : 0)) : 0; }
static int veh_xf_key(void *v)
{
    if (veh_xf_trusted(v)) return -1;
    int k = veh_xf_index(v);
    return k < 0 ? -2 : (g_vehXf[k].po + 2) * 0x10000 + g_vehXf[k].off;
}
// While the driver holds the throttle and drives straight: which way the car went vs. the forward row (3 votes decide).
static void veh_calibrate(void *v, float fwd, float turn)
{
    int k = veh_xf_index(v);
    const float *p = veh_pos(v);
    if (k < 0 || !p) return;
    float moved = dist2d(p, g_vehXf[k].samplePos);
    if (moved < 1.5f) return;
    if (fwd > 0.5f && fabsf(turn) < 0.3f && moved < 15 && g_vehXf[k].samplePos[0] != 0) {
        const float *m = vehicle_xf(v);
        if (!m) return;
        float d = fabsf(wrap_pi(atan2f(p[0] - g_vehXf[k].samplePos[0], p[2] - g_vehXf[k].samplePos[2]) - atan2f(m[8], m[10])));
        if (d > 2.4f) g_vehXf[k].flipVotes++; else if (d < 0.7f) g_vehXf[k].keepVotes++;
        int flip = g_vehXf[k].flipVotes >= 3 && g_vehXf[k].flipVotes > g_vehXf[k].keepVotes * 2;
        if (flip != g_vehXf[k].flip) { g_vehXf[k].flip = flip; H->log("drive: %s %p - its forward row points %s", vehicle_name(v), v, flip ? "backwards (flipped)" : "forwards"); }
    }
    memcpy(g_vehXf[k].samplePos, p, 12);
}

// (Thugbot port 2026-10-29 from FloodBots: Hardline names are blueprint paths - HELI_..., BOAT...)
static int name_is_air(const char *nm) { return (strstr(nm, "HELI") || strstr(nm, "Heli") || strstr(nm, "heli") || strstr(nm, "JET") || strstr(nm, "PLANE")) && !strstr(nm, "JETSKI") && !strstr(nm, "jetski"); }   // (a JETSKI matched "JET": it would have been flown)
static int is_boat_name(const char *nm) { return strstr(nm, "BOAT") || strstr(nm, "Boat") || strstr(nm, "boat") || strstr(nm, "CAT_") || strstr(nm, "JETSKI"); }
// ---- driven roads (the tester 2026-10-27: "any map I've driven or there was a successful vehicle route - use it, they need
// to learn") ----
// Every ground vehicle's driver - a player, or a bot whose drive got to its flag - is followed once a second; a trail of
// 60 m+ is saved to Thugbot_learned\<map>.roads (one line per trail: x y z ...) and painted into the road map as class 6
// (the cheapest road there is, nav.c kDriveCost) along a 3 m wide band, so bots' car routes follow what was driven.
#define MAX_ROAD_PTS 60000
static float (*g_roadPts)[3]; static int g_roadN;   // all trails of this map, back to back (x > 1e8: a break)
static char g_roadsKey[64];
static void roads_paint_seg(unsigned char *m, const float *a, const float *b)
{
    NavGrid *g = g_nav; float len = dist2d(a, b); int steps = (int)(len / (g->cell * 0.5f)) + 1;
    for (int k = 0; k <= steps; k++) {
        float f = (float)k / steps, x = a[0] + (b[0] - a[0]) * f, y = a[1] + (b[1] - a[1]) * f, z = a[2] + (b[2] - a[2]) * f;
        for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
            int cell = nav_cell_index(g, x + dx * 1.5f, z + dz * 1.5f);
            if (cell < 0) continue;
            for (int l = 0; l < NAV_LAYERS; l++) { int node = cell * NAV_LAYERS + l; if ((g->flags[node] & NAV_WALKABLE) && !(g->flags[node] & NAV_WATER) && fabsf(g->height[node] - y) < 2.5f) m[node] = 6; }
        }
    }
}
// Road materials (the tester 2026-10-29: "a way to scan streets and large roads"): the ground's material under a driven road
// (RayHit.rest +8: a material word - "matscan") is counted; the materials that make up 15%+ of a map's driven roads are
// its road surfaces, and a background sweep (300 rays a second) marks every wide-enough spot of them as road (class 6).
static unsigned g_rmId[32], g_rmN[32], g_rmTotal, g_rmSweep; static int g_rmSweepDone;
static volatile LONG g_roadRescan;   // "roadscan" (Control's Vehicle scan button): forget this map's scan and run it again
static unsigned *g_rmMarked; static unsigned g_rmMarkedN, g_rmMarkedCap;   // the nodes this scan made road (saved per map)
// Thugbot_learned<map>.roadscan: "w h x0 z0 cell" then node numbers - loaded with the map so the scan runs once a map
static void roadscan_save(void)
{
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".roadscan");
    FILE *f = fopen(path, "w"); if (!f || !g_nav) { if (f) fclose(f); return; }
    fprintf(f, "%d %d %.3f %.3f %.3f\n", g_nav->w, g_nav->h, g_nav->x0, g_nav->z0, g_nav->cell);
    for (unsigned k = 0; k < g_rmMarkedN; k++) fprintf(f, "%u\n", g_rmMarked[k]);
    fclose(f);
}
static int roadscan_load(void)
{
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".roadscan");
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int w, h; float x0, z0, cell; unsigned node, n = 0, total = (unsigned)(g_nav->w * g_nav->h * NAV_LAYERS);
    if (fscanf(f, "%d %d %f %f %f", &w, &h, &x0, &z0, &cell) != 5 || w != g_nav->w || h != g_nav->h || fabsf(x0 - g_nav->x0) > 0.01f || fabsf(z0 - g_nav->z0) > 0.01f) { fclose(f); return 0; }   // (another grid: scan again)
    while (fscanf(f, "%u", &node) == 1) if (node < total && g_driveMask[node]) { g_driveMask[node] = 6; n++; }
    fclose(f);
    H->log("drive: road scan of %s loaded - %u road spot(s)", g_levelKey, n);
    return 1;
}
static unsigned ground_material(const float *p)
{
    float a[3] = { p[0], p[1] + 1.5f, p[2] }, e[3] = { p[0], p[1] - 2.5f, p[2] }, h[3];
    if (!ray_cast(a, e, h, NULL)) return 0;
    return ((unsigned *)g_lastHit.rest)[2];
}
static void roadmat_note(const float *p)
{
    unsigned id = ground_material(p);
    if (!id) return;
    g_rmTotal++;
    for (int k = 0; k < 32; k++) { if (g_rmId[k] == id) { g_rmN[k]++; return; } if (!g_rmId[k]) { g_rmId[k] = id; g_rmN[k] = 1; return; } }
}
static int roadmat_is(unsigned id)
{
    if (!id || g_rmTotal < 20) return 0;
    for (int k = 0; k < 32 && g_rmId[k]; k++) if (g_rmId[k] == id) return g_rmN[k] >= 5 && g_rmN[k] * 100 >= g_rmTotal * 15;
    return 0;
}
static void roadmat_sweep(void)   // 300 grid nodes a call (once a second): wide-enough ground of a road material -> class 6
{
    if (!g_driveMask || !g_nav || g_navBuilding || g_rmTotal < 20 || g_rmSweepDone) return;
    NavGrid *g = g_nav; unsigned n = (unsigned)(g->w * g->h * NAV_LAYERS); int rays = 0, marked = 0;
    if (!g_rmSweep) { g_rmMarkedN = 0; if (InterlockedExchange(&g_roadRescan, 0)) { char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".roadscan"); remove(path); } else if (roadscan_load()) { g_rmSweepDone = 1; return; } int nm = 0; for (int k = 0; k < 32 && g_rmId[k]; k++) if (roadmat_is(g_rmId[k])) nm++; H->log("drive: road scan of %s starts - %d road surface(s) from %u samples", g_levelKey, nm, g_rmTotal); }
    while (g_rmSweep < n && rays < 300) {
        unsigned node = g_rmSweep++;
        unsigned cell = node / NAV_LAYERS, x = cell % g->w, z = cell / g->w;
        if ((x & 1) || (z & 1) || (g_driveMask[node] != 1 && g_driveMask[node] != 2)) continue;   // (every other cell, the wide ground only)
        float q[3]; nav_node_pos(g, node, q); rays++;
        if (!roadmat_is(ground_material(q))) continue;
        for (int dz = 0; dz <= 1; dz++) for (int dx = 0; dx <= 1; dx++) {
            int c2 = nav_cell_index(g, q[0] + dx * g->cell, q[2] + dz * g->cell);
            if (c2 < 0) continue;
            for (int l = 0; l < NAV_LAYERS; l++) { int nd = c2 * NAV_LAYERS + l; if ((g_driveMask[nd] == 1 || g_driveMask[nd] == 2) && fabsf(g->height[nd] - q[1]) < 1.5f) {
                g_driveMask[nd] = 6; marked++;
                if (g_rmMarkedN == g_rmMarkedCap) { unsigned nc = g_rmMarkedCap ? g_rmMarkedCap * 2 : 4096; unsigned *nm = realloc(g_rmMarked, nc * sizeof *nm); if (!nm) continue; g_rmMarked = nm; g_rmMarkedCap = nc; }
                g_rmMarked[g_rmMarkedN++] = (unsigned)nd; } }
        }
    }
    static unsigned totalMarked; totalMarked += marked;
    if (g_rmSweep >= n) { g_rmSweepDone = 1; roadscan_save(); H->log("drive: road scan of %s done - %u spot(s) of road surface marked (from %u material samples on driven roads)", g_levelKey, totalMarked, g_rmTotal); totalMarked = 0; }
}
static void roads_paint_trail(unsigned char *m, float (*pts)[3], int n) { for (int k = 0; k + 1 < n; k++) if (dist2d(pts[k], pts[k + 1]) < 40) roads_paint_seg(m, pts[k], pts[k + 1]); for (int k = 0; k < n; k++) roadmat_note(pts[k]); }
static void roads_load_paint(void)
{
    if (!g_driveMask) return;
    if (strcmp(g_roadsKey, g_levelKey)) {   // a new map: read its file
        snprintf(g_roadsKey, sizeof g_roadsKey, "%s", g_levelKey);
        memset(g_rmId, 0, sizeof g_rmId); memset(g_rmN, 0, sizeof g_rmN); g_rmTotal = 0;
        if (!g_roadPts) g_roadPts = malloc(sizeof *g_roadPts * MAX_ROAD_PTS);
        g_roadN = 0;
        char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".roads");
        FILE *f = fopen(path, "r"); int trails = 0;
        if (f && g_roadPts) {
            static char line[1 << 16];
            while (fgets(line, sizeof line, f)) {
                char *c = line; float x, y, z; int used, start = g_roadN;
                while (g_roadN < MAX_ROAD_PTS - 1 && sscanf(c, "%f %f %f%n", &x, &y, &z, &used) == 3) { g_roadPts[g_roadN][0] = x; g_roadPts[g_roadN][1] = y; g_roadPts[g_roadN][2] = z; g_roadN++; c += used; }
                if (g_roadN - start >= 2) { roads_paint_trail(g_driveMask, g_roadPts + start, g_roadN - start); trails++; }
                if (g_roadN > start && g_roadN < MAX_ROAD_PTS) { g_roadPts[g_roadN][0] = 1e9f; g_roadN++; }
            }
        }
        if (f) fclose(f);
        H->log("drive: %d driven road(s) (%d points) loaded for %s", trails, g_roadN, g_levelKey);
        return;
    }
    int start = 0;   // the same map, a rebuilt road map: paint again what is held
    for (int k = 0; k <= g_roadN; k++) if (k == g_roadN || g_roadPts[k][0] > 1e8f) { if (k - start >= 2) roads_paint_trail(g_driveMask, g_roadPts + start, k - start); start = k + 1; }
}
static unsigned g_roadsSaved;
static int roads_save_trail(float (*pts)[3], int n, const char *who)
{
    float len = 0; for (int k = 0; k + 1 < n; k++) len += dist2d(pts[k], pts[k + 1]);
    if (n < 2 || !isfinite(len) || len < 15.0f) return 0;   // (a few metres of creeping is not a road)
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".roads");
    FILE *f = fopen(path, "a");
    if (!f) { H->log("drive: FAILED to save road to %s",path);return 0; }
    int ok=1;
    for (int k=0;k<n;k++) if (fprintf(f,"%.1f %.1f %.1f ",pts[k][0],pts[k][1],pts[k][2])<0) ok=0;
    if (fprintf(f,"\n")<0) ok=0;
    if (fclose(f)!=0) ok=0;
    if (!ok) { H->log("drive: FAILED writing road to %s",path);return 0; }
    if (g_driveMask) roads_paint_trail(g_driveMask, pts, n);
    if (g_roadPts && g_roadN + n + 1 < MAX_ROAD_PTS) { memcpy(g_roadPts + g_roadN, pts, sizeof *pts * n); g_roadN += n; g_roadPts[g_roadN][0] = 1e9f; g_roadN++; }
    g_roadsSaved++;
    H->log("drive: learned a road %s drove - %.0f m, %d points (%u this session)", who, len, n, g_roadsSaved);
    return 1;
}
#define MAX_TRACKS 24
static struct { void *v; float pts[400][3]; int n; ULONGLONG lastAt, movedAt; int bot; char who[32]; } g_track[MAX_TRACKS];
static void roads_track(void *v, const float *p, int isBot, const char *who, ULONGLONG t)
{
    int k = 0, freeK = -1;
    for (; k < MAX_TRACKS; k++) { if (g_track[k].v == v) break; if (!g_track[k].v && freeK < 0) freeK = k; }
    if (k == MAX_TRACKS) { if (freeK < 0) return; k = freeK; g_track[k].v = v; g_track[k].n = 0; g_track[k].bot = isBot; snprintf(g_track[k].who, sizeof g_track[k].who, "%s", who); }
    g_track[k].lastAt = t;
    int n = g_track[k].n;
    if (n && dist2d(p, g_track[k].pts[n - 1]) > 60) { g_track[k].n = 0; n = 0; }   // (a jump: a respawn / a wrong read - start over)
    if (n && dist2d(p, g_track[k].pts[n - 1]) < (isBot ? 4.0f : 1.0f)) {
        if (!isBot && n>=2 && t-g_track[k].movedAt>=5000 && roads_save_trail(g_track[k].pts,n,g_track[k].who)) {
            memcpy(g_track[k].pts[0],g_track[k].pts[n-1],12);g_track[k].n=1;
        }
        return;
    }
    if (n == 400) { if (!g_track[k].bot) roads_save_trail(g_track[k].pts, n, g_track[k].who); g_track[k].n = 0; n = 0; }
    memcpy(g_track[k].pts[n], p, 12); g_track[k].n = n + 1;g_track[k].movedAt=t;
    // Publish driven corridors during a drive instead of waiting for dismount/death.
    if (!isBot && g_track[k].n>=16 && roads_save_trail(g_track[k].pts,g_track[k].n,g_track[k].who)) {
        memcpy(g_track[k].pts[0],p,12);g_track[k].n=1;
    }
}
// a track whose driver is gone (out, dead, the vehicle wrecked): players' are saved; bots' only when they arrived
static void roads_track_end(void *v, int save)
{
    for (int k = 0; k < MAX_TRACKS; k++) if (g_track[k].v == v) { if (save) roads_save_trail(g_track[k].pts, g_track[k].n, g_track[k].who); g_track[k].v = NULL; g_track[k].n = 0; }
}
static void roads_track_expire(ULONGLONG t)
{
    for (int k = 0; k < MAX_TRACKS; k++) if (g_track[k].v && t - g_track[k].lastAt > 3000) roads_track_end(g_track[k].v, !g_track[k].bot);
}
static const unsigned char *drive_mask(void)
{
    if (!g_nav || g_navBuilding) return NULL;
    if (g_driveMask && g_driveMaskGrid == g_nav && g_driveMaskSerial == g_nav->serial) return g_driveMask;
    free(g_driveMask); g_driveMask = NULL;
    int n = g_nav->w * g_nav->h * NAV_LAYERS, open = 0;
    const unsigned char *clear = nav_clearance_table(g_nav);
    if (!clear) return NULL;
    unsigned char *m = calloc((size_t)n, 1);
    if (!m) return NULL;
    for (int k = 0; k < n; k++) {
        if (!(g_nav->flags[k] & NAV_WALKABLE) || (g_nav->flags[k] & NAV_WATER)) continue;
        m[k] = clear[k] >= 2 ? 1 : clear[k] == 1 ? 2 : 0;   // (nav.c drive costs: class 1 x0.5, class 2 x1.0; 0 = not for cars)
        open += m[k] != 0;
    }
    // Keep only the big connected road areas (300+ nodes, 1200 m2): a car parked in a pocket of open ground (Hollywood
    // Heights: 28 nodes) found no way out of it; its route now starts on the real road network next to it.
    {
        int *comp = malloc((size_t)n * sizeof *comp), *queue = malloc((size_t)n * sizeof *queue), kept = 0;
        if (comp && queue) {
            for (int k = 0; k < n; k++) comp[k] = -1;
            int ncomp = 0;
            for (int k = 0; k < n; k++) {
                if (!m[k] || comp[k] >= 0) continue;
                int head = 0, tail = 0; queue[tail++] = k; comp[k] = ncomp;
                while (head < tail) {
                    int cur = queue[head++];
                    for (int d = 0; d < 8; d++) {
                        int nb = nav_step_node(g_nav, cur, d);
                        if (nb < 0 || !m[nb] || comp[nb] >= 0) continue;
                        comp[nb] = ncomp; queue[tail++] = nb;
                    }
                }
                if (tail < 300) for (int q = 0; q < tail; q++) m[queue[q]] = 0;
                else kept += tail;
                ncomp++;
            }
            open = kept;
        }
        free(comp); free(queue);
    }
    g_driveMask = m; g_driveMaskGrid = g_nav; g_driveMaskSerial = g_nav->serial;
    roads_load_paint();
    H->log("drive: road map for %s - %d of %d grid nodes wide enough for a car (in road areas of 300+ nodes)", g_levelKey, open, n);
    return m;
}

// The road-map node nearest (x, z) within `radius` m (height within 6 m of y): flags sit in plazas and buildings and
// cars park against walls - the route runs between the nearest road spots instead. Returns 0 if there is none.
static int road_point(float x, float y, float z, float radius, float *out)
{
    const unsigned char *m = drive_mask();
    if (!m) return 0;
    NavGrid *g = g_nav;
    int cx = (int)floorf((x - g->x0) / g->cell + 0.5f), cz = (int)floorf((z - g->z0) / g->cell + 0.5f), r = (int)(radius / g->cell) + 1;
    float best = radius * radius; int found = 0;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            int x2 = cx + dx, z2 = cz + dz;
            if (x2 < 0 || z2 < 0 || x2 >= g->w || z2 >= g->h) continue;
            float px = g->x0 + x2 * g->cell, pz = g->z0 + z2 * g->cell, d = (px - x) * (px - x) + (pz - z) * (pz - z);
            if (d >= best) continue;
            for (int l = 0; l < NAV_LAYERS; l++) {
                int node = (z2 * g->w + x2) * NAV_LAYERS + l;
                if ((m[node] != 1 && m[node] != 6) || fabsf(g->height[node] - y) > 6) continue;   // (class 1: 6 m clear - room to arrive; 6: a road someone drove / the road scan)
                best = d; out[0] = px; out[1] = g->height[node]; out[2] = pz; found = 1;
                break;
            }
        }
    return found;
}

// Heading from how the car moves: every 1.2 m of travel with the throttle on (under 1 s), the direction it went. The
// matrix found can be a part that turns with the steering (a bike's fork: it circled) or faces sideways; the matrix's
// heading is corrected by the average difference to the travelled direction (8+ samples), and while the car moves
// the travelled direction itself is used.
static void drive_motion(Drive *d, void *v, const float *vp, ULONGLONG t)
{
    if (!d->motionPosAt) { memcpy(d->motionPos, vp, 12); d->motionPosAt = t; return; }
    float mv = dist2d(vp, d->motionPos);
    if (mv < 1.2f) { if (t - d->motionPosAt > 1500) { memcpy(d->motionPos, vp, 12); d->motionPosAt = t; } return; }
    {   // another matrix in use: the offset learned against the old one no longer holds
        int key = veh_xf_key(v) * 2 + (veh_xf_trusted(v) && veh_row_flipped(v));
        if (key != d->xfKey) { d->xfKey = key; d->motionN = 0; }
    }
    if (t - d->motionPosAt < 1500 && mv < 40) {
        float dir = atan2f(vp[0] - d->motionPos[0], vp[2] - d->motionPos[2]);
        // Forwards or backwards: with a recent heading, whichever of the two is nearer it (a car can't turn 90 degrees in 1.2 m)
        int known = d->motionAt && t - d->motionAt < 5000;
        if (known) { if (fabsf(wrap_pi(dir - d->motionYaw)) > 1.5708f) dir = wrap_pi(dir + 3.14159265f); }
        else if (d->lastFwd < -0.3f) dir = wrap_pi(dir + 3.14159265f);   // backing up: the nose points the other way
        // throttle held forward 1.7 s, under way: which way the row points
        if (d->lastFwd > 0.3f && d->signAt && t - d->signAt > 1700 && d->speed > 3.0f && veh_xf_trusted(v)) {
            const float *m = vehicle_xf(v); float raw = m ? atan2f(m[8], m[10]) : 0, dd = fabsf(wrap_pi(atan2f(vp[0] - d->motionPos[0], vp[2] - d->motionPos[2]) - raw));
            if (m && (dd > 2.3f || dd < 0.8f)) veh_flip_vote(v, dd > 2.3f);
        }
        if (known || (fabsf(d->lastFwd) > 0.3f && d->signAt && t - d->signAt > 1700)) {
            d->motionYaw = dir; d->motionAt = t; memcpy(d->motionYawPos, vp, 12);
            float off = wrap_pi(dir - veh_yaw_raw(v));
            if (d->motionN < 8) d->matrixOff = d->motionN ? wrap_pi(d->matrixOff + wrap_pi(off - d->matrixOff) / (d->motionN + 1)) : off;
            else d->matrixOff = wrap_pi(d->matrixOff + wrap_pi(off - d->matrixOff) * 0.1f);
            d->motionN++;
        }
    }
    memcpy(d->motionPos, vp, 12); d->motionPosAt = t;
}
static float drive_yaw(Drive *d, void *v, ULONGLONG t)
{
    // The hull's own row, nothing from the motion. Log 2026-11-06: the old mix jumped more than 1.5 rad in 93 of 332 samples
    // under 25 m of travel (one SUV read 2.03, 0.06, 2.96 on a straight), so drivers steered at corners that weren't there.
    if (veh_xf_trusted(v)) return veh_yaw_raw(v);
    if (d->motionAt && t - d->motionAt < 3000) return d->motionYaw;
    if (d->motionN >= 4) return wrap_pi(veh_yaw_raw(v) + d->matrixOff);
    return veh_yaw(v);
}

// ---------------------------------------------------------------- saved vehicle routes (ported from FloodBots 2026-11-05)
// Thugbot_learned\<map>.vroutes, "route ax ay az bx by bz n x y z ...": drawn in game (!rec, then !vsave) or in the 3D map
// (read again when the file changes). A driver takes a route that passes near the car and near its flag, or a way over all
// the routes as one network, and only without one the road map. the tester 2026-11-05: "make sure vehicles follow saved VEHICLE routes".
#define NAV_MAX_OUT_DRIVE 256
#define VNET_MAX 160
typedef struct VRoute { float a[3], b[3]; int n; float pts[NAV_MAX_OUT_DRIVE][3]; } VRoute;
static VRoute *g_vnet; static int g_vnetN, g_vgSerial = 1;
static const NavGrid *g_vnetGrid; static FILETIME g_vnetStamp; static ULONGLONG g_vnetCheckAt;
static void vnet_save(void)
{
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".vroutes");
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int k = 0; k < g_vnetN; k++) {
        const VRoute *v = &g_vnet[k];
        fprintf(f, "route %.1f %.1f %.1f %.1f %.1f %.1f %d", v->a[0], v->a[1], v->a[2], v->b[0], v->b[1], v->b[2], v->n);
        for (int q = 0; q < v->n; q++) fprintf(f, " %.1f %.1f %.1f", v->pts[q][0], v->pts[q][1], v->pts[q][2]);
        fputc('\n', f);
    }
    fclose(f);
}
static void vnet_load(void)
{
    g_vnetN = 0; g_vgSerial++;
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".vroutes");
    FILE *f = fopen(path, "r"); if (!f) return;
    static char line[65536];
    while (g_vnetN < VNET_MAX && fgets(line, sizeof line, f)) {
        VRoute *v = &g_vnet[g_vnetN]; int used = 0;
        if (sscanf(line, "route %f %f %f %f %f %f %d%n", &v->a[0], &v->a[1], &v->a[2], &v->b[0], &v->b[1], &v->b[2], &v->n, &used) < 7 || v->n < 2 || v->n > NAV_MAX_OUT_DRIVE) continue;
        char *c = line + used; int q = 0;
        for (; q < v->n; q++) { int u; if (sscanf(c, "%f %f %f%n", &v->pts[q][0], &v->pts[q][1], &v->pts[q][2], &u) != 3) break; c += u; }
        if (q == v->n) g_vnetN++;
    }
    fclose(f);
    H->log("vnet: %d vehicle route(s) loaded for %s", g_vnetN, g_levelKey);
}
#define VG_MAX 6000
static float (*g_vgP)[3]; static int g_vgN, g_vgBuiltFor = -1; static int *g_vgLinkStart, *g_vgLinks, g_vgLinkN;
static void vnet_graph_build(void)
{
    if (g_vgBuiltFor == g_vgSerial && g_vgP) return;
    g_vgBuiltFor = g_vgSerial; g_vgN = 0; g_vgLinkN = 0;
    if (!g_vgP) { g_vgP = malloc(sizeof *g_vgP * VG_MAX); g_vgLinkStart = malloc(sizeof(int) * (VG_MAX + 1)); g_vgLinks = malloc(sizeof(int) * VG_MAX * 8); }
    if (!g_vgP || !g_vgLinkStart || !g_vgLinks) return;
    static int routeOf[VG_MAX];
    for (int k = 0; k < g_vnetN; k++) for (int q = 0; q < g_vnet[k].n && g_vgN < VG_MAX; q++) { memcpy(g_vgP[g_vgN], g_vnet[k].pts[q], 12); routeOf[g_vgN] = k; g_vgN++; }
    for (int a = 0; a < g_vgN; a++) {
        g_vgLinkStart[a] = g_vgLinkN; int cnt = 0;
        if (a > 0 && routeOf[a - 1] == routeOf[a]) { g_vgLinks[g_vgLinkN++] = a - 1; cnt++; }   // (along its route first)
        if (a + 1 < g_vgN && routeOf[a + 1] == routeOf[a]) { g_vgLinks[g_vgLinkN++] = a + 1; cnt++; }
        for (int b = 0; b < g_vgN && cnt < 8; b++)
            if (routeOf[a] != routeOf[b] && dist2d(g_vgP[a], g_vgP[b]) < 15 && fabsf(g_vgP[a][1] - g_vgP[b][1]) < 4) { g_vgLinks[g_vgLinkN++] = b; cnt++; }
    }
    g_vgLinkStart[g_vgN] = g_vgLinkN;
    H->log("vnet: route network of %d point(s), %d link(s)", g_vgN, g_vgLinkN);
}
static int vnet_graph_path(const float *vp, const float *target, float *path, int maxPts)
{
    vnet_graph_build();
    if (g_vgN < 2) return 0;
    // the way to each flag is worked out once (from the flag back over the whole network, 8 flags kept): every point then
    // knows its next point towards it - a plan is a walk along those
    static struct { float t[3]; int built, forN; float *d; int *next; } C[8]; static int cNext;
    int c = 0;
    while (c < 8 && !(C[c].built && C[c].forN == g_vgSerial && dist2d(C[c].t, target) < 2)) c++;
    if (c == 8) {
        c = cNext++ % 8;
        if (!C[c].d) { C[c].d = malloc(sizeof(float) * VG_MAX); C[c].next = malloc(sizeof(int) * VG_MAX); }
        if (!C[c].d || !C[c].next) return 0;
        memcpy(C[c].t, target, 12); C[c].built = 1; C[c].forN = g_vgSerial;
        static unsigned char done[VG_MAX];
        for (int q = 0; q < g_vgN; q++) { C[c].d[q] = dist2d(g_vgP[q], target) < 35 ? 0 : 1e9f; C[c].next[q] = -1; done[q] = 0; }
        for (;;) {   // (links run both ways along a route and across: the same Dijkstra outwards from the flag)
            int u = -1; float bu = 1e9f;
            for (int q = 0; q < g_vgN; q++) if (!done[q] && C[c].d[q] < bu) { bu = C[c].d[q]; u = q; }
            if (u < 0) break;
            done[u] = 1;
            for (int l = g_vgLinkStart[u]; l < g_vgLinkStart[u + 1]; l++) { int w = g_vgLinks[l]; float nd = C[c].d[u] + dist2d(g_vgP[u], g_vgP[w]); if (nd < C[c].d[w]) { C[c].d[w] = nd; C[c].next[w] = u; } }
        }
    }
    int s0 = -1; float bs = 1e9f;
    for (int q = 0; q < g_vgN; q++) { float d = dist2d(vp, g_vgP[q]); if (d < 120 && fabsf(vp[1] - g_vgP[q][1]) < 8 && C[c].d[q] < 1e8f && d + C[c].d[q] < bs) { bs = d + C[c].d[q]; s0 = q; } }
    if (s0 < 0) return 0;
    static int chain[VG_MAX]; int cn = 0;
    for (int q = s0; q >= 0 && cn < VG_MAX; q = C[c].next[q]) chain[cn++] = q;
    int step = (cn + maxPts - 1) / maxPts; if (step < 1) step = 1;
    int n = 0;
    for (int k = 0; k < cn && n < maxPts; k += step) memcpy(&path[3 * n++], g_vgP[chain[k]], 12);
    if (n >= 2) { static unsigned logged; if (logged++ < 30) H->log("vnet: a way over the route network - %d point(s), %.0f m", n, C[c].d[s0]); }
    return n >= 2 ? n : 0;
}
static int g_vnetLead = 1;
// (FloodBots 2026-11-05, the tester: "they stick very very very strictly to routes", "if a vehicle is in front, they can wait their turn")
// "strictroutes 0": the old looser following. "vqueue 0": no waiting behind a vehicle on the route.
static int g_strictRoutes = 1, g_vehQueue = 1;
// A route out of an HQ or parking lot leads nowhere near a flag: it counts when it passes within 40 m of the car and its
// far end brings the car 30 m+ closer to the flag. Driven to its end, the road map takes over from there.
static int vnet_leg(const float *vp, const float *target, float *path, int maxPts)
{
    int bestK = -1, bestV = 0, bestE = 0; float bestGain = 30, base = dist2d(vp, target);
    for (int k = 0; k < g_vnetN; k++) {
        const VRoute *v = &g_vnet[k];
        int qv = -1; float dv = 40;
        for (int q = 0; q < v->n; q++) { float dq = dist2d(vp, v->pts[q]); if (dq < dv && fabsf(vp[1] - v->pts[q][1]) < 8) { dv = dq; qv = q; } }
        if (qv < 0) continue;
        for (int e = 0; e < 2; e++) {
            int q2 = e ? v->n - 1 : 0;
            if (abs(q2 - qv) < 2) continue;
            float gain = base - dist2d(v->pts[q2], target);
            if (gain > bestGain) { bestGain = gain; bestK = k; bestV = qv; bestE = q2; }
        }
    }
    if (bestK < 0) return 0;
    const VRoute *v = &g_vnet[bestK]; int n = 0, step = bestE > bestV ? 1 : -1;
    for (int q = bestV; n < maxPts; q += step) { memcpy(&path[3 * n++], v->pts[q], 12); if (q == bestE) break; }
    static unsigned logged; if (n >= 2 && logged++ < 30) H->log("vnet: a route part of the way - %d point(s), %.0f m closer to the flag", n, bestGain);
    return n >= 2 ? n : 0;
}
static int vnet_path(const float *vp, const float *target, float *path, int maxPts)
{
    // (the Fun-Bots roads imported for the Second Assault maps are road stretches, not flag-to-flag trips: any route that
    // passes within 60 m of the vehicle and 35 m of the flag counts, driven from the one point to the other)
    int bestK = -1, bestV = 0, bestT = 0; float bestCost = 1e9f;
    for (int k = 0; k < g_vnetN; k++) {
        const VRoute *v = &g_vnet[k];
        int qv = -1, qt = -1; float dv = g_vnetLead ? 120.0f : 60.0f, dt = 35;   /* (60 m: drivers further off than that never found the tester's drawn routes - 2026-10-31) */
        for (int q = 0; q < v->n; q++) {
            float d = dist2d(vp, v->pts[q]); if (d < dv && fabsf(vp[1] - v->pts[q][1]) < 8) { dv = d; qv = q; }
            float e = dist2d(target, v->pts[q]); if (e < dt) { dt = e; qt = q; }
        }
        if (qv < 0 || qt < 0 || abs(qt - qv) < 2) continue;
        if (dv + dt < bestCost) { bestCost = dv + dt; bestK = k; bestV = qv; bestT = qt; }
    }
    if (bestK < 0) { int gn = vnet_graph_path(vp, target, path, maxPts); return gn ? gn : g_strictRoutes ? vnet_leg(vp, target, path, maxPts) : 0; }
    const VRoute *v = &g_vnet[bestK]; int n = 0, step = bestT > bestV ? 1 : -1;
    for (int q = bestV; n < maxPts; q += step) { memcpy(&path[3 * n++], v->pts[q], 12); if (q == bestT) break; }
    return n >= 2 ? n : 0;
}
static int vnet_add_manual(const float (*pts)[3], int n)
{
    if (!g_vnet) g_vnet = calloc(VNET_MAX, sizeof *g_vnet);
    if (!g_vnet || n < 2) return -1;
    if (n > NAV_MAX_OUT_DRIVE) n = NAV_MAX_OUT_DRIVE;
    const float *a = pts[0], *b = pts[n - 1];
    int k = 0;
    while (k < g_vnetN && !((dist2d(g_vnet[k].a, a) < 30 && dist2d(g_vnet[k].b, b) < 30) || (dist2d(g_vnet[k].a, b) < 30 && dist2d(g_vnet[k].b, a) < 30))) k++;
    if (k == g_vnetN) { if (g_vnetN >= VNET_MAX) return -1; g_vnetN++; }
    VRoute *v = &g_vnet[k];
    memcpy(v->a, a, 12); memcpy(v->b, b, 12); v->n = n; memcpy(v->pts, pts, (size_t)n * 12);
    vnet_save(); g_vgSerial++;
    H->log("vnet: a vehicle route drawn in game - %d points, %.0f %.0f -> %.0f %.0f", n, a[0], a[2], b[0], b[2]);
    return g_vnetN;
}
// This map's routes: read when the level changes, and again when the file changes (a save in the 3D map, mid-round).
static void vnet_sync(ULONGLONG t)
{
    if (!g_nav || g_navBuilding) return;
    if (!g_vnet) { g_vnet = calloc(VNET_MAX, sizeof *g_vnet); if (!g_vnet) return; }
    int fresh = g_vnetGrid != g_nav;
    if (!fresh && t < g_vnetCheckAt) return;
    g_vnetCheckAt = t + 5000;
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".vroutes");
    WIN32_FILE_ATTRIBUTE_DATA fa; FILETIME stamp = { 0, 0 };
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) stamp = fa.ftLastWriteTime;
    if (fresh || CompareFileTime(&stamp, &g_vnetStamp)) { g_vnetGrid = g_nav; g_vnetStamp = stamp; vnet_load(); }
}
static void vehicle_exit(Bot *b, ULONGLONG t, Drive *d, const char *why)
{
    if (!d->exitFrom) {
        d->exitFrom = t;
        static unsigned logged;
        if (logged++ < 40) H->log("drive: %s gets out (%s)", (char *)b->player + OFF_PLAYER_NAME, why);
    }
    if ((t - d->exitFrom) % 1500 < 300) b->buttons |= (t - d->exitFrom) % 3000 < 1500 ? BTN_EXIT : BTN_INTERACT;   // (E is the exit key too)
}

// the tester 2026-11-07: "get as close to the flag, get out and walk". Everyone in the vehicle gets out and goes for that flag on foot; the
// vehicle is remembered, and vehicle_to_take walks them back to it once the flag is taken. Not counted as giving up.
static void crew_dismount(Bot *b, ULONGLONG t, Drive *d, void *v, const float *vp, int flag, float toFlag, const char *why)
{
    static unsigned logged;
    if (logged++ < 80) H->log("drive: %s dismount for flag %d at %.0f %.0f, %.0f m from it (%s, %s)", (char *)b->player + OFF_PLAYER_NAME, flag, vp[0], vp[2], toFlag, vehicle_name(v), why);
    b->crewVeh = v; b->crewFlag = flag; b->crewGo = 0; b->crewAt = t; memcpy(b->crewPos, vp, 12);
    b->npts = 0; b->wp = 0; b->planAt = 0;            // (the road route is no use on foot)
    b->attackFlag = flag + 1; b->attackChooseAt = t + 45000;
    vehicle_exit(b, t, d, "dismount for flag");
}
// Where the look-ahead rays start: past the vehicle's own nose, or they hit its own hull (FloodBots 2026-11-05: tanks and
// boats backed up "boxed in" most of the time).
static float veh_nose(const char *nm)
{
    if (strstr(nm, "TANKER") || strstr(nm, "MSP")) return 6.0f;
    if (strstr(nm, "VAN") || strstr(nm, "SUV") || strstr(nm, "PICKUP") || strstr(nm, "TRUCK")) return 3.2f;
    if (strstr(nm, "MOTORCYCLE")) return 1.5f;
    return 2.7f;
}
static void route_queue(int i, Bot *b, Drive *d, void *v, const float *vp, ULONGLONG t);   // below
// A bot whose attached controllable is a vehicle. Returns 1 when it handled the bot.
static int vehicle_think(int i, Bot *b, void *v, int seat, int team)
{
    ULONGLONG t = now_ms();
    Drive *d = &g_drv[i];
    if (d->veh != v) {
        if (b->crewVeh == v) { static unsigned rl; if (rl++ < 80) H->log("drive: %s reboard done - back in the %s seat %d", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat); }
        b->crewVeh = NULL; b->npts = 0; b->wp = 0; b->planAt = 0; b->takingAt = 0;   // (its walking route is no use in a car)
        memset(d, 0, sizeof *d); d->veh = v; d->progressAt = t; d->startAt = t; if (seat == 0) g_drives++;
        void *rs = FIELD(b->player, 0x1648, void *);   // the soldier stays where it got in: the car is there
        const float *sp = rs && !IsBadReadPtr(rs, 0x400) ? soldier_pos(rs) : NULL;
        if (sp && (sp[0] || sp[2])) vehicle_xf_locate(v, sp);
        else if (b->lastPos[0] || b->lastPos[2]) vehicle_xf_locate(v, b->lastPos);   // (soldier reads 0 0 0 on some seats: where the bot last stood)
        else { static const float nowhere[3] = { 1e6f, 0, 1e6f }; vehicle_xf_locate(v, nowhere); }   // (no idea: every matrix is a candidate)
    }
    d->lastFwd = b->fwd;
    { int sg = b->fwd > 0.3f ? 1 : b->fwd < -0.3f ? -1 : 0; if (sg != d->lastSign) { d->lastSign = sg; d->signAt = t; } }
    b->heliPilot = 0;
    b->fwd = b->strafe = b->turn = 0; b->buttons = 0; b->action = 0; b->pitch = 0;
    if (g_vehicleStay) return 1;   // ("vehiclestay 1": sit still, for probing)
    if (!g_set.vehicles) { vehicle_exit(b, t, d, "bots' vehicle use is off"); return 1; }   // (Vehicles=0 in Thugbot.ini / the UI toggle: spawned in one - get out)
    const float *vp = veh_pos(v);
    if (!vp && !(name_is_air(vehicle_name(v)) && g_heliMode && t - d->startAt < 8000)) { vehicle_exit(b, t, d, "no car position"); return 1; }   // (a pilot gets 8 s: FloodBots "stop abandoning helicopters")
    { const char *nm = vehicle_name(v); if (name_is_air(nm)) { if (heli_think(i, b, v, seat, team)) return 1; vehicle_exit(b, t, d, "it's an aircraft - bots don't fly yet"); return 1; }
      if (strstr(nm, "BOAT") || strstr(nm, "CAT_") || strstr(nm, "JETSKI") || strstr(nm, "Boat")) { vehicle_exit(b, t, d, "it's a boat - bots don't sail yet"); return 1; }
      if (!strncmp(nm, "MSP", 3) && seat == 0) { static unsigned msp; if (!d->exitFrom && msp++ < 5) H->log("drive: %s - a mobile spawn truck, not driven (it stayed put with any input)", nm); vehicle_exit(b, t, d, "spawn truck"); return 1; } }
    if (seat == 0) { if (vehicle_xf_pick_moving(v, fabsf(d->lastFwd) > 0.5f && !d->exitFrom)) d->progressPos[0] = d->progressPos[2] = 0; vp = veh_pos(v); if (!vp) { vehicle_exit(b, t, d, "no car position"); return 1; } }
    if (!d->progressPos[0] && !d->progressPos[2]) memcpy(d->progressPos, vp, 12);
    if (seat == 0) drive_motion(d, v, vp, t);
    if (seat == 0 && t - d->speedAt >= 300) { float ds = dist2d(vp, d->speedPos), dt = (t - d->speedAt) / 1000.0f; if (d->speedAt && dt < 2 && ds / dt < 80) d->speed = ds / dt; memcpy(d->speedPos, vp, 12); d->speedAt = t; }
    choose_target(i, b, team, vp);
    float toTarget = dist2d(vp, b->target);

    // everyone: a car that has stood still for 3 s is left (the driver arrived, or gave up)
    float moved = dist2d(vp, d->progressPos);
    if (seat != 0) {
        if (v == g_kickVeh && t < g_kickUntil) { b->noVeh = v; b->noVehUntil = t + 30000; vehicle_exit(b, t, d, "told to get out"); return 1; }
        if (d->exitFrom && b->crewVeh == v) { vehicle_exit(b, t, d, "dismount for flag"); return 1; }
        for (int o = 0; o < H->botCount; o++) if (o != i && g_crewCall[o].v == v && t - g_crewCall[o].at < 15000) { crew_dismount(b, t, d, v, vp, g_crewCall[o].flag, toTarget, "passenger"); return 1; }
        b->buttons |= BTN_LEAN;   // lean out of the window (Z): the seat then becomes a shooting one
        if (gunner_think(i, b, v, seat, team)) b->buttons |= BTN_LEAN;   // a gun seat (boat turret ...): aim and fire
        if (t - d->progressAt > 3000) { if (moved < 1.0f) { if (!d->stoppedSince) d->stoppedSince = t; } else d->stoppedSince = 0; memcpy(d->progressPos, vp, 12); d->progressAt = t; }
        for (int o = 0; o < H->botCount; o++) if (g_drv[o].veh == v && (g_drv[o].queueing || (g_drv[o].navWaitAt && t - g_drv[o].navWaitAt < 500))) { d->stoppedSince = 0; break; }   // (the driver waiting its turn, or for the road map: stay in)
        if ((d->stoppedSince && t - d->stoppedSince > 3000) || toTarget < VEH_ARRIVE_M * 0.7f) vehicle_exit(b, t, d, "the car stopped");
        return 1;
    }
    if (d->exitFrom) { vehicle_exit(b, t, d, "leaving"); return 1; }
    if (toTarget < VEH_ARRIVE_M || (d->roadGoal[0] != 0 && dist2d(vp, d->roadGoal) < 8)) { g_driveArrivals++; const char *why0 = toTarget < VEH_ARRIVE_M ? "near the flag" : "end of the road";
        if (b->targetFlag >= 0) { g_crewCall[i].v = v; g_crewCall[i].at = t; g_crewCall[i].flag = b->targetFlag; crew_dismount(b, t, d, v, vp, b->targetFlag, toTarget, why0); } else vehicle_exit(b, t, d, why0);
        return 1; }

    // route on the road map
    const unsigned char *mask = drive_mask();
    if (!mask) {   // (the grid is being rebuilt or grown mid-round, ~10 s: wait with everyone aboard, the route is planned again after. Only 30 s of it is giving up.)
        if (!d->noMaskSince) { d->noMaskSince = t; static unsigned wl; if (wl++ < 20) H->log("drive: %s waits for the road map (grid rebuilding)", (char *)b->player + OFF_PLAYER_NAME); }
        if (t - d->noMaskSince > 30000) { vehicle_exit(b, t, d, "no road map yet"); return 1; }
        d->navWaitAt = t; d->progressAt = t; memcpy(d->progressPos, vp, 12);   // (not judged as stuck)
        return 1;
    }
    d->noMaskSince = 0;
    int netKeep = g_strictRoutes && d->onNet && b->npts && b->wp < b->npts;   // (on a route: it is driven to its end)
    int roadKeep = b->npts && b->wp < b->npts && dist2d(d->planGoal, b->target) < 40 && t < b->planAt + 60000;   // (same flag: the route it has, not a new one every 6 s)
    if ((!b->npts || b->wp >= b->npts || (t > b->planAt + 6000 && !roadKeep && (!netKeep || dist2d(d->netGoal, b->target) > 40))) && t > b->planAt + 1000) {
        float from[3], to[3];
        if (!road_point(vp[0], vp[1], vp[2], 20, from)) memcpy(from, vp, 12);
        if (!road_point(b->target[0], b->target[1], b->target[2], 90, to)) memcpy(to, b->target, 12);
        memcpy(d->roadGoal, to, 12);
        nav_set_path_mask(mask); nav_set_vehicle_width(1.3f);
        nav_set_path_owner(i);
        vnet_sync(t);
        int vn, vnDeferred = 0;   // a saved vehicle route first
        if (netKeep) {   // (the flag changed: another route only from where the car is, or on along this one)
            static float tryPath[BRAIN_PATH_MAX * 3];
            vn = vnet_path(vp, b->target, tryPath, BRAIN_PATH_MAX);
            if (vn && dist2d(vp, tryPath) > 25) vn = 0;
            if (vn) memcpy(b->path, tryPath, (size_t)vn * 12);
        } else vn = vnet_path(vp, b->target, b->path, BRAIN_PATH_MAX);
        if (vn && g_vnetLead && dist2d(vp, b->path) > 20) {   // it starts away from the car: by road to its start, not straight at it through the walls
            static float lead[BRAIN_PATH_MAX * 3];
            int ln = nav_find_path(g_nav, ray_cast, from[0], from[1], from[2], b->path[0], b->path[1], b->path[2], lead, BRAIN_PATH_MAX);
            if (!ln && nav_last_deferred()) { vn = 0; vnDeferred = 1; }   // (still planning: asked again next time)
            else if (ln >= 1) {
                int keep = vn + ln > BRAIN_PATH_MAX ? BRAIN_PATH_MAX - ln : vn;
                memmove(b->path + ln * 3, b->path, (size_t)keep * 12); memcpy(b->path, lead, (size_t)ln * 12); vn = ln + keep;
            }
        }
        if (vn) { memcpy(d->netGoal, b->target, 12); d->onLine = 0; }
        if (!netKeep || vn) d->onNet = vn > 0;
        if (vn) { if (dist2d(&b->path[(vn - 1) * 3], b->target) < 40) memcpy(d->roadGoal, &b->path[(vn - 1) * 3], 12); else d->roadGoal[0] = 0;   /* (a route part of the way: the road map on from its end) */ static unsigned vl; if (vl++ < 20) H->log("drive: %s takes a saved vehicle route to its flag (%d points)", (char *)b->player + OFF_PLAYER_NAME, vn); }
        int n = vnDeferred || (netKeep && !vn) ? 0 : vn ? vn : nav_find_path(g_nav, ray_cast, from[0], from[1], from[2], to[0], to[1], to[2], b->path, BRAIN_PATH_MAX);
        int deferred = vnDeferred || (netKeep && !vn) || (!n && nav_last_deferred());
        if (netKeep && !vn) b->planAt = t;
        nav_set_path_owner(-1); nav_set_path_mask(NULL);
        if (!n && !deferred) { static unsigned logged; if (logged++ < 20) H->log("drive: %s no road from %.0f %.0f to %.0f %.0f (%s)", (char *)b->player + OFF_PLAYER_NAME, from[0], from[2], to[0], to[2], nav_last_error()); }
        if (n) {   // (the car moved on while the route was planned: pick it up at the nearest of its first points)
            b->npts = n; b->planAt = t; d->noRouteSince = 0; memcpy(d->planGoal, b->target, 12);
            int best = 0; float bd = 1e9f;
            for (int k = 0; k < n && k < 12; k++) { float dd = dist2d(vp, &b->path[k * 3]); if (dd < bd) { bd = dd; best = k; } }
            b->wp = best + 1 < n ? best + 1 : best;
        }
        else if (!deferred) { b->npts = 0; b->planAt = t; if (!d->noRouteSince) d->noRouteSince = t; }
    }
    if (!b->npts && d->noRouteSince && t - d->noRouteSince > 3000) {
        if (b->targetFlag >= 0 && toTarget < 150) { g_crewCall[i].v = v; g_crewCall[i].at = t; g_crewCall[i].flag = b->targetFlag; crew_dismount(b, t, d, v, vp, b->targetFlag, toTarget, "no road further"); return 1; }
        g_driveGiveUps++; vehicle_exit(b, t, d, "no road to the flag"); return 1;
    }
    // The flag a bit off the route: nothing left of the route gets nearer to it than here - this is as close as it goes
    if (b->targetFlag >= 0 && toTarget < 150 && b->npts && b->wp < b->npts) {
        float rest = 1e9f;
        for (int q = b->wp; q < b->npts; q++) { float dd = dist2d(&b->path[q * 3], b->target); if (dd < rest) rest = dd; }
        if (toTarget <= rest + 5.0f) { g_crewCall[i].v = v; g_crewCall[i].at = t; g_crewCall[i].flag = b->targetFlag; crew_dismount(b, t, d, v, vp, b->targetFlag, toTarget, "closest point of the route"); return 1; }
    }
    int strictNet = g_strictRoutes && d->onNet;
    if (strictNet && b->npts && b->wp < b->npts) {   // where the car is along the route (40 m at most: a loop in it is not skipped), and a point already beside or behind it is passed
        int nearK = b->wp; float nd = 1e9f, wAlong = 0;
        for (int k = b->wp; k < b->npts && k < b->wp + 40; k++) { if (k > b->wp) { wAlong += dist2d(&b->path[(k - 1) * 3], &b->path[k * 3]); if (wAlong > 40) break; } float dd = dist2d(vp, &b->path[k * 3]); if (dd < nd) { nd = dd; nearK = k; } }
        b->wp = nearK;
        while (b->wp + 1 < b->npts) {
            const float *p0 = &b->path[b->wp * 3], *p1 = &b->path[(b->wp + 1) * 3];
            if ((p1[0] - p0[0]) * (vp[0] - p0[0]) + (p1[2] - p0[2]) * (vp[2] - p0[2]) > 0) b->wp++; else break;
            if (b->wp > nearK + 1) break;
        }
    }
    while (b->npts && b->wp < b->npts && dist2d(vp, &b->path[b->wp * 3]) < (strictNet ? 4 : 6)) b->wp++;
    d->cross = 0;
    if (b->npts && b->wp < b->npts && b->wp > 0) {
        const float *a0 = &b->path[(b->wp - 1) * 3], *a1 = &b->path[b->wp * 3];
        float sx = a1[0] - a0[0], sz = a1[2] - a0[2], l2 = sx * sx + sz * sz, u = l2 > 0.01f ? ((vp[0] - a0[0]) * sx + (vp[2] - a0[2]) * sz) / l2 : 0; u = u < 0 ? 0 : u > 1 ? 1 : u;
        float px = a0[0] + sx * u - vp[0], pz = a0[2] + sz * u - vp[2]; d->cross = sqrtf(px * px + pz * pz);
    }
    if (d->cross < 10) d->onLine = 1;
    if (strictNet && d->onLine && d->cross > 40) { static unsigned cl; if (cl++ < 20) H->log("drive: %s is %.0f m off its route - picks one again", (char *)b->player + OFF_PLAYER_NAME, d->cross); b->npts = 0; b->planAt = t; d->onNet = 0; d->onLine = 0; return 1; }
    const float *aim = b->npts && b->wp < b->npts ? &b->path[b->wp * 3] : b->target;
    float pursuit[3];
    if (strictNet && b->npts && b->wp < b->npts) {   // aim close ahead on the route, stopping short of a real bend, so corners are taken round, not cut
        float left = fminf(15.0f, 7.0f + d->speed * 0.5f);
        const float *prev = vp; memcpy(pursuit, aim, 12);
        float dir0 = atan2f(aim[0] - vp[0], aim[2] - vp[2]);
        for (int k = b->wp; k < b->npts; k++) {
            const float *q = &b->path[k * 3];
            float seg = dist2d(prev, q);
            if (k > b->wp && fabsf(wrap_pi(atan2f(q[0] - prev[0], q[2] - prev[2]) - dir0)) > 0.61f) { memcpy(pursuit, prev, 12); break; }
            if (seg >= left) { float f = left / seg; pursuit[0] = prev[0] + (q[0] - prev[0]) * f; pursuit[1] = q[1]; pursuit[2] = prev[2] + (q[2] - prev[2]) * f; break; }
            left -= seg; memcpy(pursuit, q, 12); prev = q;
        }
        aim = pursuit;
        if (t < d->laneUntil) { float fx = pursuit[0] - vp[0], fz = pursuit[2] - vp[2], fl = sqrtf(fx * fx + fz * fz); if (fl > 1) { pursuit[0] += fz / fl * 3.5f; pursuit[2] -= fx / fl * 3.5f; } }   // someone coming the other way: keep right, both of them
    }
    if (t >= d->queueAt) { d->queueAt = t + 150; route_queue(i, b, d, v, vp, t); }

    float err = wrap_pi(atan2f(aim[0] - vp[0], aim[2] - vp[2]) - drive_yaw(d, v, t));
    // stuck: under 2 m in 3 s (forward or backing up - checked before either, the backing-up and boxed-in branches used to
    // return first: a car stood 40 s against a wall "stuck 0") - back up steering the other way; four times and it walks
    if (t - d->progressAt > 3000 && t - d->startAt > 2500) {
        float yawNow = veh_yaw(v);
        if (moved < 0.01f && fabsf(wrap_pi(yawNow - d->stuckYaw)) < 1e-4f && !d->xfRejected) {   // not one bit of movement: wrong matrix?
            d->xfRejected = 1; vehicle_xf_reject(v); d->progressAt = t; return 1;
        }
        d->stuckYaw = yawNow;
        if (moved < 2.0f) {
            d->stuck++;
            if (d->stuck >= 4) { g_driveGiveUps++; vehicle_exit(b, t, d, "stuck"); return 1; }
            int wasReversing = t < d->reverseUntil + 600;
            d->reverseUntil = wasReversing ? 0 : t + 2000 + d->stuck * 500;   // backing up didn't work either: try forwards
            d->reverseTurn = d->reverseTurn > 0 ? -1.0f : 1.0f;
            if (!strictNet) {   // (on a route: back up and on along it - nothing written off, no other way picked)
            if (g_nav && !g_navBuilding) nav_block_towards(g_nav, vp[0], vp[1], vp[2], aim[0], aim[2]);
            b->npts = 0;   // and plan again from where it ends up
            }
        } else if (moved > 8) d->stuck = 0;
        memcpy(d->progressPos, vp, 12); d->progressAt = t;
    }
    // backing up after a hit
    // (on a route it backs up nearly straight and stops 6 m off the line; time spent backing up is not judged as stuck)
    if (t < d->reverseUntil && strictNet && !d->turningAround && d->cross > 6) d->reverseUntil = 0;
    if (t < d->reverseUntil) { b->fwd = -1; b->turn = strictNet && !d->turningAround ? d->reverseTurn * 0.25f : d->reverseTurn; d->progressAt = t; memcpy(d->progressPos, vp, 12); return 1; }
    d->turningAround = 0;
    if (d->queueing) { b->fwd = d->speed > 2.5f && (t / 250) % 2 ? -0.5f : 0; b->turn = 0; return 1; }   // waiting its turn
    // The way on is behind the car: a three-point turn (back up turning, then forwards) rather than circling forwards
    // into the kerb (cars stood with the route behind them, err 2.2 - 2.3).
    if (fabsf(err) > 2.2f && t > d->turnAroundAt && t - d->startAt > 2500 && (veh_xf_trusted(v) || (d->motionAt && t - d->motionAt < 1500))) {   // (only with a fresh heading)
        d->turnAroundAt = t + 6000; d->reverseUntil = t + (strictNet ? 3500 : 1800); d->turningAround = strictNet; d->reverseTurn = err > 0 ? 1.0f : -1.0f;
        b->fwd = -1; b->turn = d->reverseTurn; return 1;
    }
    {   // a car whose heading isn't confirmed yet: 2 s straight ahead first, to see which way "forward" goes
        int k = veh_xf_index(v);
        if (k >= 0 && g_vehXf[k].flipVotes + g_vehXf[k].keepVotes < 3 && t - d->startAt < 2500) { b->fwd = 1; b->turn = 0; veh_calibrate(v, 1, 0); return 1; }
    }
    veh_calibrate(v, b->fwd, b->turn);
    err = wrap_pi(atan2f(aim[0] - vp[0], aim[2] - vp[2]) - drive_yaw(d, v, t));
    {   // driving trace: every 2 s per driver (first 400 lines a level), to see how drives go wrong
        static unsigned traced;
        if (t - d->traceAt > 2000 && traced++ < 400) {
            d->traceAt = t;
            H->log("drive: %s in %s at %.0f %.0f, route %d/%d, aim %.0f %.0f, heading %.2f err %.2f, moved %.1f m in %.1f s, stuck %d",
                   (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), vp[0], vp[2], b->wp, b->npts, aim[0], aim[2], drive_yaw(d, v, t), err,
                   dist2d(vp, d->progressPos), (t - d->progressAt) / 1000.0, d->stuck);
            void *rs = FIELD(b->player, 0x1648, void *);
            if (rs && !IsBadReadPtr(rs, 0x400)) { const float *sp = soldier_pos(rs); H->log("drive:   (driver's soldier at %.1f %.1f %.1f, car %.1f %.1f %.1f)", sp[0], sp[1], sp[2], vp[0], vp[1], vp[2]); }
        }
    }
    { float de = wrap_pi(err - d->prevErr); d->prevErr = err; b->turn = strictNet ? fmaxf(-1, fminf(1, -err * 1.5f - de * 2.0f)) : fmaxf(-1, fminf(1, -err * 2.0f)); }   // (damped on a route: no weaving)
    {   // does +turn really lower the heading? Counted while under way, logged every 50 samples.
        static unsigned stLow, stHigh, stLines; float yn = drive_yaw(d, v, t);
        if (!d->stAt) { d->stAt = t; d->stYaw = yn; d->stTurn = 0; d->stN = 0; }
        d->stTurn += b->turn; d->stN++;
        if (t - d->stAt >= 800) {
            float dh = wrap_pi(yn - d->stYaw), av = d->stN ? d->stTurn / d->stN : 0;
            if (d->speed > 3 && fabsf(av) > 0.3f && fabsf(dh) > 0.05f) { if ((av > 0) == (dh < 0)) stLow++; else stHigh++; }
            if (stLow + stHigh >= 50) { if (stLines++ < 60) H->log("drive: steer check: +turn lowered the heading %u times, raised it %u", stLow, stHigh); stLow = stHigh = 0; }
            d->stAt = t; d->stYaw = yn; d->stTurn = 0; d->stN = 0;
        }
    }
    b->fwd = 1.0f;   // full throttle: partial throttle (0.6 - 0.75) left most cars standing still (only "drivetest" at 1.0 moved them)
    if (d->motionAt && b->npts && b->wp < b->npts) {   // a top speed, less before a bend, behind someone, off the line (held by coasting and brake taps: partial throttle does nothing)
        const char *vn2 = vehicle_name(v);
        float vmax = strstr(vn2, "MOTORCYCLE") ? 20.0f : 16.0f;
        float bend = 0, along = dist2d(vp, &b->path[b->wp * 3]), prevA = atan2f(b->path[b->wp * 3] - vp[0], b->path[b->wp * 3 + 2] - vp[2]);
        for (int q = b->wp + 1; q < b->npts && along < 30; q++) {
            const float *p0 = &b->path[(q - 1) * 3], *p1 = &b->path[q * 3];
            float a1 = atan2f(p1[0] - p0[0], p1[2] - p0[2]), turnQ = fabsf(wrap_pi(a1 - prevA));
            if (along > 6 && turnQ > bend) bend = turnQ;
            prevA = a1; along += dist2d(p0, p1);
        }
        float vwant = bend > 1.0f ? 6.0f : bend > 0.5f ? 10.0f : vmax;
        if (fabsf(err) > 1.0f) vwant = fminf(vwant, 6.0f);
        if (strictNet && d->queueGap < 26) vwant = fminf(vwant, fmaxf(0.0f, (d->queueGap - 9) * 0.7f));
        if (strictNet && d->cross > 5) vwant = fminf(vwant, 7.0f);
        if (strictNet && d->footAhead < 10) vwant = fminf(vwant, 5.0f);
        if (d->freeC < 13.5f) vwant = fminf(vwant, d->freeC < 6 ? fmaxf(3.0f, d->freeC * 0.8f) : 8.0f);
        if (d->speed > vwant + 3.0f) b->fwd = (t / 200) % 2 && d->speed > 2.5f ? -0.6f : 0;
        else if (d->speed > vwant) b->fwd = 0;
    }
    // Ground check 6 and 10 m ahead, every 150 ms (from FloodBots; the tester 2026-11-05: "drove next to the bridge into the water").
    // The planner never routes over water, but a car drives straight from point to point and can leave a bridge between
    // them. A ray down from 2 m up must meet ground within reach of the car's level - water (rays go through it) or a
    // drop-off reads as none: back up turning away, block that way in the grid, plan again.
    if (t >= d->edgeAt && !(strictNet && d->cross < 4)) {   /* (right on the drawn line: the line is the ground) */
        d->edgeAt = t + 150;
        float yw = drive_yaw(d, v, t); int edge = 0;
        for (int r = 6; r <= 10 && !edge; r += 4) {
            float a[3] = { vp[0] + sinf(yw) * r, vp[1] + 2.0f, vp[2] + cosf(yw) * r }, e[3] = { a[0], vp[1] - 1.0f - 0.45f * r, a[2] }, h[3];   // (a 29 degree slope down is still road)
            if (!ray_cast(a, e, h, NULL)) edge = r;
        }
        if (edge) {
            d->reverseUntil = t + 1500; d->reverseTurn = d->reverseTurn > 0 ? -1.0f : 1.0f;
            if (!strictNet) { b->npts = 0; b->planAt = 0;
            if (g_nav && !g_navBuilding) nav_block_towards(g_nav, vp[0], vp[1], vp[2], vp[0] + sinf(yw) * 10, vp[2] + cosf(yw) * 10); }
            static unsigned el; if (el++ < 40) H->log("drive: %s - no ground %d m ahead at %.0f %.0f (water / drop): backs off", (char *)b->player + OFF_PLAYER_NAME, edge, vp[0], vp[2]);
            b->fwd = -1; b->turn = d->reverseTurn; return 1;
        }
    }
    // Look ahead (every 150 ms): three rays at bumper height from the front of the car - straight on and 20 degrees
    // either side, 14 m. Something straight ahead: steer for the side that's clearer and ease off; boxed in within 5 m:
    // back up (reversing steers the other way).
    if (t >= d->lookAt) {
        d->lookAt = t + 150;
        float yaw = drive_yaw(d, v, t), free[3];
        for (int r = 0; r < 3; r++) {
            float a = yaw + (r - 1) * 0.35f, nose = veh_nose(vehicle_name(v)), o[3] = { vp[0] + sinf(yaw) * nose, vp[1] + 0.7f, vp[2] + cosf(yaw) * nose }, e[3] = { o[0] + sinf(a) * 14, o[1], o[2] + cosf(a) * 14 }, h[3];
            free[r] = ray_cast(o, e, h, NULL) ? dist2d(o, h) : 14;
            // and at roof height: garage beams and low ceilings a bumper ray passes under (the tester: "garages with beams")
            o[1] = e[1] = vp[1] + 1.7f;
            if (ray_cast(o, e, h, NULL) && dist2d(o, h) < free[r]) free[r] = dist2d(o, h);
        }
        // Traffic: anyone ahead - a car (anyone's, driven or parked), a soldier on foot - within 14 m and 35 degrees of
        // the nose. Close (under 7 m): stop and wait (yield); further: steer to pass it on the side it isn't. Two bot
        // cars nose to nose: the lower-numbered one goes, the other waits; after 4 s of waiting it goes round.
        int yieldNow = 0; float avoid = 0, footAhead = 99;
        for (int j = 0; j < g_playerCount; j++) {
            void *pl = g_players[j];
            if (pl == b->player) continue;
            int seat2 = -1; void *v2 = player_vehicle(pl, &seat2);
            const float *q = NULL;
            if (v2) { if (v2 == v || seat2 != 0) continue; q = veh_pos(v2); }
            else { void *s2 = bot_soldier(pl); if (s2) q = soldier_pos(s2); }
            if (!q || fabsf(q[1] - vp[1]) > 4) continue;
            float dd = dist2d(vp, q);
            if (dd > 14 || dd < 0.5f) continue;
            float rel = wrap_pi(atan2f(q[0] - vp[0], q[2] - vp[2]) - yaw);
            if (fabsf(rel) > 0.6f) continue;
            if (!v2 && dd < footAhead) footAhead = dd;
            int otherBot = -1;
            for (int o = 0; o < H->botCount; o++) if (H->bots[o] == pl) otherBot = o;
            if (dd < 7 && !(v2 && otherBot >= 0 && otherBot > i)) yieldNow = 1;   // (a bot car with a higher number waits for us)
            float a2 = (rel > 0 ? -1.0f : 1.0f) * (1.0f - dd / 14);   // (positive turn input = lower heading: away from something on the right)
            if (fabsf(a2) > fabsf(avoid)) avoid = a2;
        }
        d->footAhead = footAhead;
        if (strictNet) yieldNow = 0;   // (on a route: route_queue waits)
        if (yieldNow) { if (!d->yieldSince) d->yieldSince = t; } else d->yieldSince = 0;
        d->yield = yieldNow && t - d->yieldSince < 4000;
        d->avoidTurn = avoid;
        d->freeC = free[1]; d->freeL = free[0]; d->freeR = free[2];
    }
    if (strictNet && t >= d->passUntil) {   // on a route: the route, and stop rather than swerve
        if (d->freeC < 4 && d->freeL < 5 && d->freeR < 5) { d->reverseUntil = t + 1500; d->reverseTurn = d->freeL > d->freeR ? 1.0f : -1.0f; d->freeC = 14; return 1; }
        if (d->freeC < 5 && (t / 250) % 2) b->fwd = d->speed > 2.5f ? -0.5f : 0.0f;
    }
    else if (d->yield) { b->fwd = (t / 300) % 2 ? -0.3f : 0; b->turn = 0; }
    else if (d->avoidTurn != 0 && d->freeC >= 4) b->turn = fmaxf(-1, fminf(1, b->turn + d->avoidTurn * 1.5f));
    else if (d->freeC < 10) {
        if (d->freeC < 4 && d->freeL < 5 && d->freeR < 5) { d->reverseUntil = t + 1500; d->reverseTurn = d->freeL > d->freeR ? 1.0f : -1.0f; d->freeC = 14; return 1; }
        // the side with more room: r = 0 is yaw - 0.35 (a lower heading: positive turn input), r = 2 is yaw + 0.35
        b->turn = d->freeL > d->freeR ? 1.0f : -1.0f;
        if (d->freeC < 5 && (t / 250) % 2) b->fwd = -0.5f;   // (brake taps close to something; partial throttle does nothing)
    }

    // Parked nose-in against something (a wall, a fence) with the way out behind: back out first.
    if (!d->checkedFront) {
        d->checkedFront = 1;
        float yaw = drive_yaw(d, v, t), nf = veh_nose(vehicle_name(v)), f[3] = { vp[0] + sinf(yaw) * nf, vp[1] + 0.8f, vp[2] + cosf(yaw) * nf }, e[3] = { f[0] + sinf(yaw) * 3, f[1], f[2] + cosf(yaw) * 3 };
        if (ray_cast(f, e, NULL, NULL) || fabsf(err) > 1.8f) { d->reverseUntil = t + 1800; d->reverseTurn = err > 0 ? 1.0f : -1.0f; return 1; }
    }

    return 1;
}

// ---- vehicle names and the vehicles bots know about ----
// The name: the vehicle's data (+0x30) holds its blueprint path at +0x140 ("havana/Vehicles/UTILITY_VAN/UTILITY_VAN";
// found with "vehname" on Hollywood Heights) - the last part is shown.
static const char *vehicle_name(void *v)
{
    char *data = v && !IsBadReadPtr((char *)v + 0x30, 8) ? FIELD(v, 0x30, char *) : NULL;
    const char *path = data && !IsBadReadPtr(data + 0x140, 8) ? FIELD(data, 0x140, const char *) : NULL;
    if (!path || IsBadReadPtr(path, 8)) return "vehicle";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Every vehicle anyone has sat in this level: where it was last, whether someone is in it now, and which bot is walking
// to it. Bots on foot with a far flag take an empty one nearby (an abandoned car, or one that respawned where a car
// was taken before). (Brand-new vehicles nobody has used yet aren't known - the server's entity lists aren't mapped.)
#define MAX_KNOWN_VEH 192   /* (FloodBots: gadgets filled 48 slots and helicopters never got one) */
typedef struct KnownVeh { void *v; uintptr_t vt; float pos[3], lastPos[3]; char name[40]; int occupied, humanDriverTeam, still; int spotDone; ULONGLONG knownAt; int claimedBy; ULONGLONG claimUntil, seenAt, skipUntil; } KnownVeh;   // skipUntil: a helicopter whose pilot had no control is left alone that long
static KnownVeh g_knownVeh[MAX_KNOWN_VEH];
static int g_knownVehN;
static unsigned g_vehTaken;


#define TV_MAX 64
static void *g_tvV[TV_MAX]; static float g_tvP[TV_MAX][3], g_tvW[TV_MAX][2]; static ULONGLONG g_tvT[TV_MAX];
static void veh_velocity(void *v, const float *q, ULONGLONG t, float *w)
{
    int k = 0, free = -1;
    for (; k < TV_MAX && g_tvV[k] != v; k++) if (free < 0 && (!g_tvV[k] || t - g_tvT[k] > 5000)) free = k;
    if (k == TV_MAX) { k = free >= 0 ? free : (int)(t % TV_MAX); g_tvV[k] = v; memcpy(g_tvP[k], q, 12); g_tvT[k] = t; g_tvW[k][0] = g_tvW[k][1] = 0; }
    else if (t - g_tvT[k] >= 300) {
        float dt = (t - g_tvT[k]) / 1000.0f;
        if (dt < 2.0f) { g_tvW[k][0] = (q[0] - g_tvP[k][0]) / dt; g_tvW[k][1] = (q[2] - g_tvP[k][2]) / dt; } else g_tvW[k][0] = g_tvW[k][1] = 0;
        memcpy(g_tvP[k], q, 12); g_tvT[k] = t;
    }
    w[0] = g_tvW[k][0]; w[1] = g_tvW[k][1];
}
// How far along bot b's route, from point "from", point q lies; -1: not on it.
static float route_gap(const Bot *b, const float *from, const float *q, float side, float reach)
{
    float along = 0; const float *a = from;
    for (int k = b->wp; k < b->npts; k++) {
        const float *c = &b->path[k * 3];
        float sx = c[0] - a[0], sz = c[2] - a[2], l2 = sx * sx + sz * sz, sl = sqrtf(l2);
        float u = l2 > 0.01f ? ((q[0] - a[0]) * sx + (q[2] - a[2]) * sz) / l2 : 0; u = u < 0 ? 0 : u > 1 ? 1 : u;
        float px = a[0] + sx * u - q[0], pz = a[2] + sz * u - q[2], g = along + sl * u;
        if (px * px + pz * pz < side * side && g >= 2.5f) return g;
        along += sl; if (along > reach) break;
        a = c;
    }
    return -1;
}
// Waiting its turn (from FloodBots): the nearest vehicle on the route ahead within 30 m. The driver stops 9 m behind it.
// Two that each have the other on their route: the lower bot number goes, a human always. Coming straight at each other:
// both keep right and pass. One ahead that has not moved for 20 s (an empty parked one: 5 s) is passed by steering round it.
static void route_queue(int i, Bot *b, Drive *d, void *v, const float *vp, ULONGLONG t)
{
    float gap = 99, blockSpeed = 0; int parked = 0, wasQueueing = d->queueing;
    d->queueGap = 99; d->queueing = 0;
    if (!g_vehQueue || !d->onNet || !b->npts || b->wp >= b->npts || t < d->passUntil) { d->queueSince = d->blockStillSince = 0; return; }
    float side = 3.0f;
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        if (pl == b->player) continue;
        int seat2 = -1; void *v2 = player_vehicle(pl, &seat2);
        if (!v2 || v2 == v || seat2 != 0) continue;
        const float *q = veh_pos(v2);
        if (!q || fabsf(q[1] - vp[1]) > 4 || dist2d(vp, q) > 34) continue;
        float g = route_gap(b, vp, q, side, 30);
        if (g < 0) continue;
        int ob = -1; for (int o = 0; o < H->botCount; o++) if (H->bots[o] == pl) ob = o;
        float w[2]; veh_velocity(v2, q, t, w);
        float sp2 = sqrtf(w[0] * w[0] + w[1] * w[1]), dd = dist2d(vp, q);
        int mutual = ob >= 0 && g_drv[ob].veh == v2 && g_bot[ob].npts && g_bot[ob].wp < g_bot[ob].npts && route_gap(&g_bot[ob], q, vp, side, 30) >= 0;
        int comingAtMe = dd > 0.5f && sp2 > 1.0f && w[0] * (vp[0] - q[0]) / dd + w[1] * (vp[2] - q[2]) / dd > 0.7f * sp2;
        if (comingAtMe || (mutual && i < ob)) { if (g < 24) d->laneUntil = t + 1500; continue; }
        if (g < gap) { gap = g; blockSpeed = sp2; parked = 0; }
    }
    for (int k = 0; k < g_knownVehN; k++) {
        KnownVeh *kv = &g_knownVeh[k];
        if (kv->v == v || kv->occupied || t - kv->seenAt > 3000 || fabsf(kv->pos[1] - vp[1]) > 4 || dist2d(vp, kv->pos) > 26 || name_is_air(kv->name)) continue;
        float g = route_gap(b, vp, kv->pos, 2.2f, 22);
        if (g >= 0 && g < gap) { gap = g; blockSpeed = 0; parked = 1; }
    }
    d->queueGap = gap;
    if (gap >= 11.5f) { d->queueSince = d->blockStillSince = 0; return; }
    if (blockSpeed > 1.5f) d->blockStillSince = 0; else if (!d->blockStillSince) d->blockStillSince = t;
    if (d->blockStillSince && t - d->blockStillSince > (parked ? 5000u : 20000u)) {
        d->passUntil = t + 10000; d->queueSince = d->blockStillSince = 0; d->queueGap = 99;
        static unsigned pl; if (pl++ < 40) H->log("queue: %s has waited long enough behind a %s vehicle at %.0f %.0f - goes round it", (char *)b->player + OFF_PLAYER_NAME, parked ? "parked" : "stopped", vp[0], vp[2]);
        return;
    }
    d->queueing = 1; if (!d->queueSince) d->queueSince = t;
    d->progressAt = t; memcpy(d->progressPos, vp, 12);
    if (!wasQueueing) { static unsigned ql; if (ql++ < 60) H->log("queue: %s in a %s waits its turn at %.0f %.0f, %.0f m behind a %s vehicle", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), vp[0], vp[2], gap, parked ? "parked" : blockSpeed > 1.5f ? "moving" : "stopped"); }
}
static float veh_yaw_raw_spot(void *v) { const float *m = vehicle_xf(v); return m ? atan2f(m[8], m[10]) : 0; }
// ---- where vehicles stand when a round starts, saved for the 3D map (the tester 2026-11-05: "add the vehicles in the 3d lidar
// maps", to draw routes out of the HQs and parking lots). Thugbot_learned\<map>.vehicles, one line per spot:
// "veh <name> x y z yaw <rounds seen>" - grows round by round (random spawners show every model they gave).
#define VSPOT_MAX 128
typedef struct VSpot { char name[40]; float p[3], yaw; int seen; } VSpot;
static VSpot g_vspot[VSPOT_MAX]; static int g_vspotN; static char g_vspotFile[MAX_PATH]; static ULONGLONG g_vspotLevelAt; static char g_vspotLevel[128];
static void vspot_sync(ULONGLONG t)
{
    if (strcmp(g_vspotLevel, g_levelKey)) { snprintf(g_vspotLevel, sizeof g_vspotLevel, "%s", g_levelKey); g_vspotLevelAt = t; g_vspotFile[0] = 0; }
    if (g_vspotFile[0] || !g_levelKey[0]) return;
    char key[64]; map_key(key, sizeof key);
    CreateDirectoryA("Thugbot_learned", NULL);
    snprintf(g_vspotFile, sizeof g_vspotFile, "Thugbot_learned\\%s.vehicles", key);
    g_vspotN = 0;
    FILE *f = fopen(g_vspotFile, "r"); if (!f) return;
    char line[256];
    while (g_vspotN < VSPOT_MAX && fgets(line, sizeof line, f)) {
        VSpot *s = &g_vspot[g_vspotN];
        if (sscanf(line, "veh %39s %f %f %f %f %d", s->name, &s->p[0], &s->p[1], &s->p[2], &s->yaw, &s->seen) == 6) g_vspotN++;
    }
    fclose(f);
}
static void vspot_save(void)
{
    if (!g_vspotFile[0]) return;
    FILE *f = fopen(g_vspotFile, "w"); if (!f) return;
    for (int k = 0; k < g_vspotN; k++) fprintf(f, "veh %s %.1f %.1f %.1f %.2f %d\n", g_vspot[k].name, g_vspot[k].p[0], g_vspot[k].p[1], g_vspot[k].p[2], g_vspot[k].yaw, g_vspot[k].seen);
    fclose(f);
}
// A vehicle stands still where it was first found this round. (Gadgets the memory scan also finds are not vehicles.)
static void vspot_note(const char *nm, const float *p, float yaw)
{
    if (!nm[0] || !strcmp(nm, "vehicle")) return;
    for (const char *c = nm; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '-')) return;   // (an unreadable name: not a vehicle)
    if (strstr(nm, "stationary") || strstr(nm, "eodbot") || strstr(nm, "ucav") || strstr(nm, "mav") || strstr(nm, "Object") || strstr(nm, "Radar") || strstr(nm, "Turret") || strstr(nm, "TURRET") || strstr(nm, "beacon")) return;
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return;
    for (int k = 0; k < g_vspotN; k++) if (!strcmp(g_vspot[k].name, nm) && dist2d(g_vspot[k].p, p) < 5 && fabsf(g_vspot[k].p[1] - p[1]) < 4) { g_vspot[k].seen++; vspot_save(); return; }
    if (g_vspotN >= VSPOT_MAX) return;
    VSpot *s = &g_vspot[g_vspotN++];
    snprintf(s->name, sizeof s->name, "%s", nm); memcpy(s->p, p, 12); s->yaw = yaw; s->seen = 1;
    vspot_save();
    static unsigned logged; if (logged++ < 200) H->log("vspot: %s at %.1f %.1f %.1f (%d spot(s) on this map)", nm, p[0], p[1], p[2], g_vspotN);
}

static void vehicles_track(void)
{
    static ULONGLONG at;
    ULONGLONG t = now_ms();
    if (t - at < 1000) return;
    at = t;
    vspot_sync(t);
    { static int seeded; if (!seeded) { seeded = 1; vehicle_class_note(H->image + 0x27a02e0); } }   /* (the memory scan needs one class: known, so it runs from the first map, vehicles on or off) */
    for (int k = 0; k < g_knownVehN; k++) g_knownVeh[k].occupied = g_knownVeh[k].humanDriverTeam = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *v = player_vehicle(g_players[j], NULL);
        if (!v) continue;
        int k = 0;
        while (k < g_knownVehN && g_knownVeh[k].v != v) k++;
        if (k == g_knownVehN) {
            if (g_knownVehN == MAX_KNOWN_VEH) continue;
            g_knownVehN++;
            memset(&g_knownVeh[k], 0, sizeof g_knownVeh[k]);
            g_knownVeh[k].v = v; g_knownVeh[k].vt = *(uintptr_t *)v; g_knownVeh[k].claimedBy = -1; g_knownVeh[k].knownAt = t;
            snprintf(g_knownVeh[k].name, sizeof g_knownVeh[k].name, "%s", vehicle_name(v));
            { static void *logged[64]; static int nl; int seenBefore = 0; for (int q = 0; q < nl; q++) seenBefore |= logged[q] == v; if (!seenBefore) { if (nl < 64) logged[nl++] = v; H->log("vehicles: %s (%p) on %s", g_knownVeh[k].name, v, g_levelKey); } }
            vehicle_class_note(*(uintptr_t *)v);
        }
        g_knownVeh[k].occupied = 1;
        int seat = -1; player_vehicle(g_players[j], &seat);
        if (seat == 0 && !is_bot_player(g_players[j])) g_knownVeh[k].humanDriverTeam = FIELD(g_players[j], OFF_PLAYER_TEAM, int);
        if (seat == 0) {   // driven roads: follow the driver (players always; bots' tracks are kept only if they arrive)
            const char *nm = g_knownVeh[k].name;
            const float *vp1 = !IsBadReadPtr(v, 0x300) ? veh_pos(v) : NULL;
            if (vp1 && !is_boat_name(nm) && !name_is_air(nm)) roads_track(v, vp1, is_bot_player(g_players[j]), (const char *)g_players[j] + OFF_PLAYER_NAME, t);
        }
    }
    // the memory scan's finds: those with a world transform on this map are vehicles nobody has used yet
    if (InterlockedExchange(&g_vehScanDone, 0)) {
        int added = 0;
        for (int f = 0; f < g_foundVehN && g_knownVehN < MAX_KNOWN_VEH; f++) {
            void *v = g_foundVeh[f];
            int k = 0;
            while (k < g_knownVehN && g_knownVeh[k].v != v) k++;
            const float *vp0 = k < g_knownVehN || IsBadReadPtr(v, 0x300) ? NULL : veh_pos(v);
            if (!vp0) continue;   // (one read only: a second read can fail - the 60 m jump check - and crashed the server)
            KnownVeh *kv = &g_knownVeh[g_knownVehN++];
            memset(kv, 0, sizeof *kv);
            kv->v = v; kv->vt = *(uintptr_t *)v; kv->claimedBy = -1; kv->seenAt = t; kv->knownAt = t;
            memcpy(kv->pos, vp0, 12);
            snprintf(kv->name, sizeof kv->name, "%s", vehicle_name(v));
            added++;
        }
        H->log("vehicles: memory scan found %d object(s) of a vehicle class, %d new vehicle(s) on the map - %d known", (int)g_foundVehN, added, g_knownVehN);
    }
    roads_track_expire(t);
    {   // seeds without anyone driving: ground vehicles spawn on roads / paved pads - the ground under every parked car, quad
        // and tank (each once; its spot and 4 points 3 m round it) counts as road surface
        static void *seeded[256]; static unsigned ns; static const void *seedGrid;
        if (seedGrid != g_nav) { seedGrid = g_nav; ns = 0; }
        for (int k = 0; k < g_knownVehN; k++) {
            KnownVeh *kv = &g_knownVeh[k];
            const char *nm = kv->name;
            if (kv->occupied || !nm[0] || !strcmp(nm, "vehicle") || is_boat_name(nm) || name_is_air(nm)) continue;
            int done = 0; for (unsigned q = 0; q < ns && q < 256; q++) if (seeded[q] == kv->v) done = 1;
            if (done || ns >= 256) continue;
            seeded[ns++] = kv->v;
            { static unsigned sl; if (sl++ < 10) H->log("drive: road seed from %s at %.0f %.0f (%u samples so far)", nm, kv->pos[0], kv->pos[2], g_rmTotal); }
            for (int d = 1; d < 5; d++) { float q2[3] = { kv->pos[0] + (d == 1 ? 4.5f : d == 2 ? -4.5f : 0), kv->pos[1], kv->pos[2] + (d == 3 ? 4.5f : d == 4 ? -4.5f : 0) }; roadmat_note(q2); }   /* (4.5 m out: the ray at the vehicle itself hits its body) */
        }
    }
    if (g_roadRescan && g_rmSweepDone) { g_rmSweep = 0; g_rmSweepDone = 0; g_driveMaskSerial = 0; }   /* (a fresh road map, then the scan) */
    { static unsigned sweepSerial; if (g_nav && sweepSerial != g_nav->serial) { sweepSerial = g_nav->serial; g_rmSweep = 0; g_rmSweepDone = 0; } if (g_rmTotal >= 20 && !g_rmSweepDone && drive_mask()) roadmat_sweep(); }   /* (builds the road map itself if no driver has yet) */
    {   // vehicle health watch: [+0x218]+0x30..0x44 of every known vehicle - logged when one changes (finding current health)
        static void *hv[64]; static float hvv[64][6]; static unsigned hlog;
        for (int k = 0; k < g_knownVehN && k < 64; k++) {
            float cur[6]; int ok = 1;
            for (int s2 = 0; s2 < 6; s2++) if (!hp_at(g_knownVeh[k].v, 0x218, 0x30 + s2 * 4, &cur[s2])) ok = 0;
            if (!ok) continue;
            if (hv[k] == g_knownVeh[k].v && memcmp(hvv[k], cur, sizeof cur) && hlog++ < 60)
                H->log("vehhp: %s [+0x218] +30..44: %.1f %.1f %.1f %.1f %.1f %.1f (was %.1f %.1f %.1f %.1f %.1f %.1f)", g_knownVeh[k].name, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], hvv[k][0], hvv[k][1], hvv[k][2], hvv[k][3], hvv[k][4], hvv[k][5]);
            hv[k] = g_knownVeh[k].v; memcpy(hvv[k], cur, sizeof cur);
        }
    }
    vehicle_scan_start(0);
    for (int k = 0; k < g_knownVehN; k++) {   // where each one is now (gone - destroyed, freed - when it stops reading as one)
        KnownVeh *kv = &g_knownVeh[k];
        const float *p = !IsBadReadPtr(kv->v, 8) && *(uintptr_t *)kv->v == kv->vt ? veh_pos(kv->v) : NULL;
        if (p) { kv->still = dist2d(p, kv->lastPos) < 1.0f; memcpy(kv->lastPos, p, 12); memcpy(kv->pos, p, 12); kv->seenAt = t; }
        // (a parked one: its first steady reading, up to 30 s for its name to appear; someone already in it: only straight away, it may be driving off)
        if (p && !kv->spotDone && (kv->occupied ? t - kv->knownAt > 4000 : t - kv->knownAt > 30000)) kv->spotDone = 1;
        if (p && !kv->spotDone && kv->still && strcmp(kv->name, "vehicle")) { kv->spotDone = 1; if (t - g_vspotLevelAt < 180000) vspot_note(kv->name, p, veh_yaw_raw_spot(kv->v)); }
        else if (t - kv->seenAt > 10000) { g_knownVeh[k] = g_knownVeh[--g_knownVehN]; k--; }
    }
}
static void vehicles_level_reset(void) { g_knownVehN = 0; g_vehScanAt = now_ms() - 75000; /* a scan 15 s after the load */ }

// A bot on foot going to a flag over 120 m away: an empty known vehicle within 50 m (nobody else walking to it).
// Returns its index (and claims it), or -1.
static void vehicle_taken_note(void) { g_vehTaken++; }
static void *vehicle_known_ptr(int k) { return k >= 0 && k < g_knownVehN ? g_knownVeh[k].v : NULL; }
// A soldier sitting in a passenger seat you can shoot from: its attached controllable is still the soldier, but it
// stands still inside a known car. Returns the car's index, or -1.
static int soldier_seated(const float *pos)
{
    for (int k = 0; k < g_knownVehN; k++) if (dist2d(pos, g_knownVeh[k].pos) < 2.8f && fabsf(pos[1] - g_knownVeh[k].pos[1]) < 2.5f) return k;
    return -1;
}
static int vehicle_known_stopped_human(int k) { return k >= 0 && k < g_knownVehN && g_knownVeh[k].humanDriverTeam && g_knownVeh[k].still; }
static int vehicle_has_driver(int k)
{
    for (int j = 0; j < g_playerCount; j++) { int seat; if (player_vehicle(g_players[j], &seat) == g_knownVeh[k].v && seat == 0) return 1; }
    return 0;
}
// A car / bike a bot can drive (helicopters, boats and the mobile spawn truck it would only get out of again: 50 of the
// 400 trips on Downtown were walks to one of those).
static int vehicle_drivable_name(const char *nm)
{
    if (!nm || !nm[0]) return 1;
    if (name_is_air(nm)) return g_heliMode != 0;
    if (is_boat_name(nm)) return 0;
    if (!strncmp(nm, "MSP", 3)) return 0;
    if (strstr(nm, "Object") || strstr(nm, "Radar") || strstr(nm, "Turret") || strstr(nm, "TURRET")) return 0;   // (the vehicle scan also finds gadgets: "RadarSweep_Object")
    return 1;
}
static void known_veh_skip(void *v, ULONGLONG until) { for (int k = 0; k < g_knownVehN; k++) if (g_knownVeh[k].v == v) g_knownVeh[k].skipUntil = until; }
static int vehicle_to_take(int i, const float *pos, const float *target, float *vpOut, const char **nameOut)
{
    ULONGLONG t = now_ms();
    if (!g_set.vehicles) return -1;
    vnet_sync(t);
    int team = FIELD(H->bots[i], OFF_PLAYER_TEAM, int);
    {   // Left a vehicle for a flag: no vehicle until that flag is ours (or 90 s), then back to it with E if it is still there and free
        Bot *me = &g_bot[i];
        if (me->crewVeh && me->spawnAt > me->crewAt) me->crewVeh = NULL;   // (died since)
        if (me->crewVeh) {
            int f = me->crewFlag, taken = f >= 0 && f < g_cpCount && g_fi[f].ok && g_fi[f].owner == team;
            if (!taken && t - me->crewAt <= 90000) return -1;
            const char *off = NULL; int kk = -1;
            for (int k = 0; k < g_knownVehN; k++) if (g_knownVeh[k].v == me->crewVeh) kk = k;
            if (t - me->crewAt > 180000) off = "too long";
            else if (kk < 0 || t - g_knownVeh[kk].seenAt > 3000) off = "it is gone";
            else if (dist2d(pos, g_knownVeh[kk].pos) > 150) off = "too far";
            else if (me->noVeh == me->crewVeh && t < me->noVehUntil) off = "E did nothing";
            else if (!me->crewGo && dist2d(pos, target) < 60) off = "the next flag is close";
            else if (g_knownVeh[kk].occupied) {   // (a crewmate got there first: fine; anyone else, or it has driven off: no)
                if (dist2d(g_knownVeh[kk].pos, me->crewPos) > 25) off = "it has left";
                for (int j = 0; j < g_playerCount && !off; j++) if (player_vehicle(g_players[j], NULL) == me->crewVeh && FIELD(g_players[j], OFF_PLAYER_TEAM, int) != team) off = "the enemy has it";
            }
            static unsigned cl;
            if (off) { if (cl++ < 80) H->log("drive: %s no reboard (%s)", (char *)H->bots[i] + OFF_PLAYER_NAME, off); me->crewVeh = NULL; }
            else {
                if (!me->crewGo) { me->crewGo = 1; if (cl++ < 80) H->log("drive: %s reboard - walks back to the %s, %.0f m (%s)", (char *)H->bots[i] + OFF_PLAYER_NAME, g_knownVeh[kk].name, dist2d(pos, g_knownVeh[kk].pos), taken ? "flag taken" : "90 s up"); }
                memcpy(vpOut, g_knownVeh[kk].pos, 12); *nameOut = g_knownVeh[kk].name;
                return kk;
            }
        }
    }
    for (int k = 0; k < g_knownVehN; k++) {   // a teammate (human) driving, stopped: hop in
        KnownVeh *kv = &g_knownVeh[k];
        if (kv->humanDriverTeam != team || !kv->still || dist2d(pos, kv->pos) > 30) continue;
        if (g_bot[i].noVeh == kv->v && t < g_bot[i].noVehUntil) continue;
        int riders = 0;
        for (int o = 0; o < MAX_BOTS; o++) if (o != i && g_bot[o].takingAt && dist2d(g_bot[o].lastPos, kv->pos) < 5) riders++;
        if (riders >= 3) continue;
        memcpy(vpOut, kv->pos, 12); *nameOut = kv->name;
        return k;
    }
    float toFlag = dist2d(pos, target);
    if (toFlag < (g_heliMode ? 60 : 80)) return -1;
    int best = -1; float bd = 50;
    for (int k = 0; k < g_knownVehN; k++) {
        KnownVeh *kv = &g_knownVeh[k];
        if (kv->occupied || (kv->claimedBy >= 0 && kv->claimedBy != i && t < kv->claimUntil) || t - kv->seenAt > 3000) continue;
        if (!vehicle_drivable_name(kv->name)) continue;
        if (g_bot[i].noVeh == kv->v && t < g_bot[i].noVehUntil) continue;
        int air = name_is_air(kv->name);
        if (air && (t < kv->skipUntil || dist2d(pos, kv->pos) > 80)) continue;   // (helicopters at base: walk up to 80 m for one, FloodBots)
        if (!air && toFlag < 80) continue;
        if (dist2d(kv->pos, target) > toFlag + (air ? 30 : 0)) continue;   // not one that's further from the flag (a helicopter: 30 m of detour is fine)
        if (!air && g_vnetN > 0) { static float tmp[BRAIN_PATH_MAX * 3]; if (!vnet_path(kv->pos, target, tmp, BRAIN_PATH_MAX)) continue; }   // (this map has saved vehicle routes: only trips they cover)
        float d = dist2d(pos, kv->pos) - (air ? 40 : 0);   // (helicopters first)
        if (d < bd) { bd = d; best = k; }
    }
    if (best >= 0 && g_knownVeh[best].claimedBy != i) { g_knownVeh[best].claimedBy = i; g_knownVeh[best].claimUntil = t + 20000; }
    if (best >= 0) { memcpy(vpOut, g_knownVeh[best].pos, 12); *nameOut = g_knownVeh[best].name; }
    return best;
}

static void drive_dump(void)
{
    int drivers = 0, passengers = 0;
    for (int i = 0; i < H->botCount; i++) { int seat; if (player_vehicle(H->bots[i], &seat)) { if (seat == 0) drivers++; else passengers++; } }
    H->log("drive: %d bot(s) driving, %d riding; since start %u drive(s), %u reached a flag, %u gave up; %u car(s) taken on foot", drivers, passengers, g_drives, g_driveArrivals, g_driveGiveUps, g_vehTaken);
    for (int k = 0; k < g_knownVehN; k++) H->log("drive: known %s at %.0f %.0f %.0f%s", g_knownVeh[k].name, g_knownVeh[k].pos[0], g_knownVeh[k].pos[1], g_knownVeh[k].pos[2], g_knownVeh[k].occupied ? " (in use)" : "");
}

// "vnetsnap": test - the road route a bot is driving right now, saved as a vehicle route.
static void vnet_snap(void)
{
    for (int i = 0; i < H->botCount; i++) {
        Bot *b = &g_bot[i];
        if (!b->inVehicle || b->npts < 4) continue;
        float tmp[NAV_MAX_OUT_DRIVE][3]; int n = b->npts < NAV_MAX_OUT_DRIVE - 1 ? b->npts : NAV_MAX_OUT_DRIVE - 1;
        memcpy(tmp, b->path, (size_t)n * 12); memcpy(tmp[n++], b->target, 12);   // (ending at the flag, as a drawn route does)
        H->log("vnetsnap: %s's route saved: %d", (char *)b->player + OFF_PLAYER_NAME, vnet_add_manual((const float (*)[3])tmp, n));
        return;
    }
    H->log("vnetsnap: nobody driving a route");
}
