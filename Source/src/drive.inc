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
static int g_heliMode = 0;   // heli.inc: 0 pilots get out, 1 control probe, 2 fly ("heli 1" to test: flying not working yet)
static int heli_think(int i, Bot *b, void *v, int seat, int team);
static int gunner_think(int i, Bot *b, void *v, int seat, int team);
static const char *vehicle_name(void *v);
static unsigned char *g_driveMask;
static const NavGrid *g_driveMaskGrid;
static unsigned g_driveMaskSerial;
static unsigned g_drives, g_driveArrivals, g_driveGiveUps;

typedef struct Drive { int checkedFront, yield; ULONGLONG startAt, lookAt; float freeC, freeL, freeR; void *veh; ULONGLONG progressAt, reverseUntil, stoppedSince, exitFrom, noRouteSince, traceAt; float progressPos[3], reverseTurn, roadGoal[3]; int stuck; ULONGLONG turnAroundAt; float stuckYaw; int xfRejected;
                       float lastFwd, motionPos[3], motionYaw, matrixOff, avoidTurn; ULONGLONG yieldSince; ULONGLONG motionPosAt, motionAt; int motionN; } Drive;
static Drive g_drv[MAX_BOTS];

static char *vehicle_body(void *v) { char *o = v && !IsBadReadPtr((char *)v + 0x208, 8) ? FIELD(v, 0x208, char *) : NULL; return o && !IsBadReadPtr(o, 0xc0) ? o : NULL; }
// Where each vehicle keeps its world transform differs by model (Chinatown: [+0x208]+0x80 on most cars; Hollywood
// Heights: [+0xcf0]+0x40 ...). So it is found per vehicle: the first matrix, in the vehicle or one pointer away, that is
// orthonormal, upright (a car's up row points up; a spinning wheel's doesn't) and sits on the nav grid's ground (within
// 2.5 m). Remembered per vehicle and checked again on every read; a vehicle that no longer passes is searched again.
#define MAX_VEH_XF 128
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
    if (idx > 8 || IsBadReadPtr(m, 64) || !plausible_matrix(m)) return NULL;
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
    { const char *nm = vehicle_name(v); g_xfAir = strstr(nm, "HELI") || strstr(nm, "Heli") || strstr(nm, "JET") || strstr(nm, "PLANE"); }
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
                if (m[node] != 1 || fabsf(g->height[node] - y) > 6) continue;   // (class 1: 6 m clear - room to arrive)
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
    if (mv < 1.2f) { if (t - d->motionPosAt > 1000) { memcpy(d->motionPos, vp, 12); d->motionPosAt = t; } return; }
    if (t - d->motionPosAt < 1000 && mv < 40) {
        float dir = atan2f(vp[0] - d->motionPos[0], vp[2] - d->motionPos[2]);
        if (d->lastFwd < -0.3f) dir = wrap_pi(dir + 3.14159265f);   // backing up: the nose points the other way
        if (fabsf(d->lastFwd) > 0.3f) {
            d->motionYaw = dir; d->motionAt = t;
            float off = wrap_pi(dir - veh_yaw(v));
            if (d->motionN < 8) d->matrixOff = d->motionN ? wrap_pi(d->matrixOff + wrap_pi(off - d->matrixOff) / (d->motionN + 1)) : off;
            else d->matrixOff = wrap_pi(d->matrixOff + wrap_pi(off - d->matrixOff) * 0.1f);
            d->motionN++;
        }
    }
    memcpy(d->motionPos, vp, 12); d->motionPosAt = t;
}
static float drive_yaw(Drive *d, void *v, ULONGLONG t)
{
    if (d->motionAt && t - d->motionAt < 500) return d->motionYaw;
    return wrap_pi(veh_yaw(v) + (d->motionN >= 8 ? d->matrixOff : 0));
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

// A bot whose attached controllable is a vehicle. Returns 1 when it handled the bot.
static int vehicle_think(int i, Bot *b, void *v, int seat, int team)
{
    ULONGLONG t = now_ms();
    Drive *d = &g_drv[i];
    if (d->veh != v) {
        memset(d, 0, sizeof *d); d->veh = v; d->progressAt = t; d->startAt = t; if (seat == 0) g_drives++;
        void *rs = FIELD(b->player, 0x1648, void *);   // the soldier stays where it got in: the car is there
        const float *sp = rs && !IsBadReadPtr(rs, 0x400) ? soldier_pos(rs) : NULL;
        if (sp && (sp[0] || sp[2])) vehicle_xf_locate(v, sp);
        else if (b->lastPos[0] || b->lastPos[2]) vehicle_xf_locate(v, b->lastPos);   // (soldier reads 0 0 0 on some seats: where the bot last stood)
        else { static const float nowhere[3] = { 1e6f, 0, 1e6f }; vehicle_xf_locate(v, nowhere); }   // (no idea: every matrix is a candidate)
    }
    d->lastFwd = b->fwd;
    b->fwd = b->strafe = b->turn = 0; b->buttons = 0; b->action = 0; b->pitch = 0;
    if (g_vehicleStay) return 1;   // ("vehiclestay 1": sit still, for probing)
    if (!g_set.vehicles) { vehicle_exit(b, t, d, "bots' vehicle use is off"); return 1; }   // (Vehicles=0 in Thugbot.ini / the UI toggle: spawned in one - get out)
    const float *vp = veh_pos(v);
    if (!vp) { vehicle_exit(b, t, d, "no car position"); return 1; }
    { const char *nm = vehicle_name(v); if (strstr(nm, "HELI") || strstr(nm, "Heli") || strstr(nm, "JET") || strstr(nm, "PLANE")) { if (heli_think(i, b, v, seat, team)) return 1; vehicle_exit(b, t, d, "it's an aircraft - bots don't fly yet"); return 1; }
      if (strstr(nm, "BOAT") || strstr(nm, "CAT_") || strstr(nm, "JETSKI") || strstr(nm, "Boat")) { vehicle_exit(b, t, d, "it's a boat - bots don't sail yet"); return 1; }
      if (!strncmp(nm, "MSP", 3) && seat == 0) { static unsigned msp; if (!d->exitFrom && msp++ < 5) H->log("drive: %s - a mobile spawn truck, not driven (it stayed put with any input)", nm); vehicle_exit(b, t, d, "spawn truck"); return 1; } }
    if (seat == 0) { if (vehicle_xf_pick_moving(v, fabsf(d->lastFwd) > 0.5f && !d->exitFrom)) d->progressPos[0] = d->progressPos[2] = 0; vp = veh_pos(v); if (!vp) { vehicle_exit(b, t, d, "no car position"); return 1; } }
    if (!d->progressPos[0] && !d->progressPos[2]) memcpy(d->progressPos, vp, 12);
    if (seat == 0) drive_motion(d, v, vp, t);
    choose_target(i, b, team, vp);
    float toTarget = dist2d(vp, b->target);

    // everyone: a car that has stood still for 3 s is left (the driver arrived, or gave up)
    float moved = dist2d(vp, d->progressPos);
    if (seat != 0) {
        if (v == g_kickVeh && t < g_kickUntil) { b->noVeh = v; b->noVehUntil = t + 30000; vehicle_exit(b, t, d, "told to get out"); return 1; }
        b->buttons |= BTN_LEAN;   // lean out of the window (Z): the seat then becomes a shooting one
        if (gunner_think(i, b, v, seat, team)) b->buttons |= BTN_LEAN;   // a gun seat (boat turret ...): aim and fire
        if (t - d->progressAt > 3000) { if (moved < 1.0f) { if (!d->stoppedSince) d->stoppedSince = t; } else d->stoppedSince = 0; memcpy(d->progressPos, vp, 12); d->progressAt = t; }
        if ((d->stoppedSince && t - d->stoppedSince > 3000) || toTarget < VEH_ARRIVE_M * 0.7f) vehicle_exit(b, t, d, "the car stopped");
        return 1;
    }
    if (d->exitFrom) { vehicle_exit(b, t, d, "leaving"); return 1; }
    if (toTarget < VEH_ARRIVE_M || (d->roadGoal[0] != 0 && dist2d(vp, d->roadGoal) < 8)) { g_driveArrivals++; vehicle_exit(b, t, d, toTarget < VEH_ARRIVE_M ? "near the flag" : "end of the road"); return 1; }

    // route on the road map
    const unsigned char *mask = drive_mask();
    if (!mask) { vehicle_exit(b, t, d, "no road map yet"); return 1; }
    if ((!b->npts || b->wp >= b->npts || t > b->planAt + 6000) && t > b->planAt + 1000) {
        float from[3], to[3];
        if (!road_point(vp[0], vp[1], vp[2], 20, from)) memcpy(from, vp, 12);
        if (!road_point(b->target[0], b->target[1], b->target[2], 90, to)) memcpy(to, b->target, 12);
        memcpy(d->roadGoal, to, 12);
        nav_set_path_mask(mask); nav_set_vehicle_width(1.3f);
        nav_set_path_owner(i);
        int n = nav_find_path(g_nav, ray_cast, from[0], from[1], from[2], to[0], to[1], to[2], b->path, BRAIN_PATH_MAX);
        int deferred = !n && nav_last_deferred();
        nav_set_path_owner(-1); nav_set_path_mask(NULL);
        if (!n && !deferred) { static unsigned logged; if (logged++ < 20) H->log("drive: %s no road from %.0f %.0f to %.0f %.0f (%s)", (char *)b->player + OFF_PLAYER_NAME, from[0], from[2], to[0], to[2], nav_last_error()); }
        if (n) {   // (the car moved on while the route was planned: pick it up at the nearest of its first points)
            b->npts = n; b->planAt = t; d->noRouteSince = 0;
            int best = 0; float bd = 1e9f;
            for (int k = 0; k < n && k < 12; k++) { float dd = dist2d(vp, &b->path[k * 3]); if (dd < bd) { bd = dd; best = k; } }
            b->wp = best + 1 < n ? best + 1 : best;
        }
        else if (!deferred) { b->npts = 0; b->planAt = t; if (!d->noRouteSince) d->noRouteSince = t; }
    }
    if (!b->npts && d->noRouteSince && t - d->noRouteSince > 3000) { g_driveGiveUps++; vehicle_exit(b, t, d, "no road to the flag"); return 1; }
    while (b->npts && b->wp < b->npts && dist2d(vp, &b->path[b->wp * 3]) < 6) b->wp++;
    const float *aim = b->npts && b->wp < b->npts ? &b->path[b->wp * 3] : b->target;

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
            if (g_nav && !g_navBuilding) nav_block_towards(g_nav, vp[0], vp[1], vp[2], aim[0], aim[2]);
            b->npts = 0;   // and plan again from where it ends up
        } else if (moved > 8) d->stuck = 0;
        memcpy(d->progressPos, vp, 12); d->progressAt = t;
    }
    // backing up after a hit
    if (t < d->reverseUntil) { b->fwd = -1; b->turn = d->reverseTurn; return 1; }
    // The way on is behind the car: a three-point turn (back up turning, then forwards) rather than circling forwards
    // into the kerb (cars stood with the route behind them, err 2.2 - 2.3).
    if (fabsf(err) > 2.2f && t > d->turnAroundAt && t - d->startAt > 2500) {
        d->turnAroundAt = t + 6000; d->reverseUntil = t + 1800; d->reverseTurn = err > 0 ? 1.0f : -1.0f;
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
    b->turn = fmaxf(-1, fminf(1, -err * 2.0f));
    b->fwd = 1.0f;   // full throttle: partial throttle (0.6 - 0.75) left most cars standing still (only "drivetest" at 1.0 moved them)
    // Look ahead (every 150 ms): three rays at bumper height from the front of the car - straight on and 20 degrees
    // either side, 14 m. Something straight ahead: steer for the side that's clearer and ease off; boxed in within 5 m:
    // back up (reversing steers the other way).
    if (t >= d->lookAt) {
        d->lookAt = t + 150;
        float yaw = drive_yaw(d, v, t), free[3];
        for (int r = 0; r < 3; r++) {
            float a = yaw + (r - 1) * 0.35f, o[3] = { vp[0] + sinf(yaw) * 2.5f, vp[1] + 0.7f, vp[2] + cosf(yaw) * 2.5f }, e[3] = { o[0] + sinf(a) * 14, o[1], o[2] + cosf(a) * 14 }, h[3];
            free[r] = ray_cast(o, e, h, NULL) ? dist2d(o, h) : 14;
            // and at roof height: garage beams and low ceilings a bumper ray passes under (the tester: "garages with beams")
            o[1] = e[1] = vp[1] + 1.7f;
            if (ray_cast(o, e, h, NULL) && dist2d(o, h) < free[r]) free[r] = dist2d(o, h);
        }
        // Traffic: anyone ahead - a car (anyone's, driven or parked), a soldier on foot - within 14 m and 35 degrees of
        // the nose. Close (under 7 m): stop and wait (yield); further: steer to pass it on the side it isn't. Two bot
        // cars nose to nose: the lower-numbered one goes, the other waits; after 4 s of waiting it goes round.
        int yieldNow = 0; float avoid = 0;
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
            int otherBot = -1;
            for (int o = 0; o < H->botCount; o++) if (H->bots[o] == pl) otherBot = o;
            if (dd < 7 && !(v2 && otherBot >= 0 && otherBot > i)) yieldNow = 1;   // (a bot car with a higher number waits for us)
            float a2 = (rel > 0 ? -1.0f : 1.0f) * (1.0f - dd / 14);   // (positive turn input = lower heading: away from something on the right)
            if (fabsf(a2) > fabsf(avoid)) avoid = a2;
        }
        if (yieldNow) { if (!d->yieldSince) d->yieldSince = t; } else d->yieldSince = 0;
        d->yield = yieldNow && t - d->yieldSince < 4000;
        d->avoidTurn = avoid;
        d->freeC = free[1]; d->freeL = free[0]; d->freeR = free[2];
    }
    if (d->yield) { b->fwd = (t / 300) % 2 ? -0.3f : 0; b->turn = 0; }
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
        float f[3] = { vp[0], vp[1] + 0.8f, vp[2] }, yaw = drive_yaw(d, v, t), e[3] = { f[0] + sinf(yaw) * 5, f[1], f[2] + cosf(yaw) * 5 };
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
#define MAX_KNOWN_VEH 48
typedef struct KnownVeh { void *v; uintptr_t vt; float pos[3], lastPos[3]; char name[40]; int occupied, humanDriverTeam, still; int claimedBy; ULONGLONG claimUntil, seenAt; } KnownVeh;
static KnownVeh g_knownVeh[MAX_KNOWN_VEH];
static int g_knownVehN;
static unsigned g_vehTaken;

static void vehicles_track(void)
{
    static ULONGLONG at;
    ULONGLONG t = now_ms();
    if (t - at < 1000) return;
    at = t;
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
            g_knownVeh[k].v = v; g_knownVeh[k].vt = *(uintptr_t *)v; g_knownVeh[k].claimedBy = -1;
            snprintf(g_knownVeh[k].name, sizeof g_knownVeh[k].name, "%s", vehicle_name(v));
            { static void *logged[64]; static int nl; int seenBefore = 0; for (int q = 0; q < nl; q++) seenBefore |= logged[q] == v; if (!seenBefore) { if (nl < 64) logged[nl++] = v; H->log("vehicles: %s (%p) on %s", g_knownVeh[k].name, v, g_levelKey); } }
            vehicle_class_note(*(uintptr_t *)v);
        }
        g_knownVeh[k].occupied = 1;
        int seat = -1; player_vehicle(g_players[j], &seat);
        if (seat == 0 && !is_bot_player(g_players[j])) g_knownVeh[k].humanDriverTeam = FIELD(g_players[j], OFF_PLAYER_TEAM, int);
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
            kv->v = v; kv->vt = *(uintptr_t *)v; kv->claimedBy = -1; kv->seenAt = t;
            memcpy(kv->pos, vp0, 12);
            snprintf(kv->name, sizeof kv->name, "%s", vehicle_name(v));
            added++;
        }
        H->log("vehicles: memory scan found %d object(s) of a vehicle class, %d new vehicle(s) on the map - %d known", (int)g_foundVehN, added, g_knownVehN);
    }
    vehicle_scan_start(0);
    for (int k = 0; k < g_knownVehN; k++) {   // where each one is now (gone - destroyed, freed - when it stops reading as one)
        KnownVeh *kv = &g_knownVeh[k];
        const float *p = !IsBadReadPtr(kv->v, 8) && *(uintptr_t *)kv->v == kv->vt ? veh_pos(kv->v) : NULL;
        if (p) { kv->still = dist2d(p, kv->lastPos) < 1.0f; memcpy(kv->lastPos, p, 12); memcpy(kv->pos, p, 12); kv->seenAt = t; }
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
    if (strstr(nm, "HELI") || strstr(nm, "Heli") || strstr(nm, "JET") || strstr(nm, "PLANE")) return g_heliMode != 0;
    if (strstr(nm, "BOAT") || strstr(nm, "CAT_") || strstr(nm, "Boat")) return 0;
    if (!strncmp(nm, "MSP", 3)) return 0;
    if (strstr(nm, "Object") || strstr(nm, "Radar") || strstr(nm, "Turret") || strstr(nm, "TURRET")) return 0;   // (the vehicle scan also finds gadgets: "RadarSweep_Object")
    return 1;
}
static int vehicle_to_take(int i, const float *pos, const float *target, float *vpOut, const char **nameOut)
{
    ULONGLONG t = now_ms();
    if (!g_set.vehicles) return -1;
    int team = FIELD(H->bots[i], OFF_PLAYER_TEAM, int);
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
    if (dist2d(pos, target) < 80) return -1;
    int best = -1; float bd = 50;
    for (int k = 0; k < g_knownVehN; k++) {
        KnownVeh *kv = &g_knownVeh[k];
        if (kv->occupied || (kv->claimedBy >= 0 && kv->claimedBy != i && t < kv->claimUntil) || t - kv->seenAt > 3000) continue;
        if (!vehicle_drivable_name(kv->name)) continue;
        if (g_bot[i].noVeh == kv->v && t < g_bot[i].noVehUntil) continue;
        if (dist2d(kv->pos, target) > dist2d(pos, target)) continue;   // not one that's further from the flag
        float d = dist2d(pos, kv->pos);
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
