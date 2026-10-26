// Thugbot vehicle gunners (included by brain.inc after heli.inc): bots in a seat whose controllable is the vehicle
// (helicopter door guns, boat turrets ...), and attack-helicopter pilots' nose guns (heli.inc).
//
//   The gun's direction isn't mapped, so it is found per seat: on getting in, every plausible matrix of the vehicle
//   (in it or one pointer away, within 10 m of it) is a candidate; turn +1 for 1.2 s - the matrix that turned most
//   against the body is the gun's yaw (and which way +1 turns it); pitch +1 for 1.2 s - the one whose forward row rose
//   or fell most is the barrel. Then: nearest enemy in sight within 150 m, turn / pitch toward it, fire when on.
//   Firing 6 s without one hit: the forward row is taken to point backwards (as on most cars' bodies) and flipped.

#define GUN_CANDS 48
typedef struct Gunner {
    void *veh; int seat, phase; ULONGLONG at, fireSince, flipAt; unsigned hits0;
    int nc, po[GUN_CANDS], off[GUN_CANDS]; float yaw0[GUN_CANDS], fy0[GUN_CANDS], body0;
    int yawM, pitchM; float yawSign, pitchSign; int flip;
} Gunner;
static Gunner g_gun[MAX_BOTS];
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
static int gunner_think(int i, Bot *b, void *v, int seat, int team)
{
    ULONGLONG t = now_ms();
    Gunner *g = &g_gun[i];
    const float *vp = veh_pos(v);
    if (!vp) return 0;
    if (g->veh != v || g->seat != seat) { memset(g, 0, sizeof *g); g->veh = v; g->seat = seat; g->yawM = g->pitchM = -1; g->at = t; }
    if (g->phase == 0) { gun_collect(g, v, vp); g->body0 = gun_body_yaw(v); g->phase = 1; g->at = t; return 1; }
    if (g->phase == 1) {   // turning
        b->turn = 1;
        if (t - g->at < 1200) return 1;
        float body = wrap_pi(gun_body_yaw(v) - g->body0), best = 0.15f;
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            if (!m || !gun_ortho(m)) continue;
            float d = wrap_pi(atan2f(m[8], m[10]) - g->yaw0[c]);
            if (!gun_local(m)) d = wrap_pi(d - body);   // (a world matrix turns with the body too)
            if (fabsf(d) > best) { best = fabsf(d); g->yawM = c; g->yawSign = d > 0 ? 1.0f : -1.0f; }
        }
        for (int c = 0; c < g->nc; c++) { const float *m = gun_m(v, g->po[c], g->off[c]); if (m) g->fy0[c] = m[9]; }
        H->log("gunner: %s in %s seat %d - %d matrices, gun yaw %s (turned %.2f, +1 turn %s%s)", (char *)b->player + OFF_PLAYER_NAME, vehicle_name(v), seat, g->nc,
               g->yawM >= 0 ? "found" : "not found", best, g->yawSign > 0 ? "raises the heading" : "lowers the heading",
               g->yawM >= 0 && gun_m(v, g->po[g->yawM], g->off[g->yawM]) && gun_local(gun_m(v, g->po[g->yawM], g->off[g->yawM])) ? ", vehicle space" : "");
        g->phase = 2; g->at = t; return 1;
    }
    if (g->phase == 2) {   // pitching
        b->pitch = 1;
        if (t - g->at < 1200) return 1;
        float best = 0.05f;
        for (int c = 0; c < g->nc; c++) {
            const float *m = gun_m(v, g->po[c], g->off[c]);
            if (!m || !gun_ortho(m)) continue;
            float d = m[9] - g->fy0[c];
            if (fabsf(d) > best) { best = fabsf(d); g->pitchM = c; g->pitchSign = d > 0 ? 1.0f : -1.0f; }
        }
        H->log("gunner: %s - barrel %s (forward row moved %.2f, +1 pitch %s)", (char *)b->player + OFF_PLAYER_NAME, g->pitchM >= 0 ? "found" : "not found", best, g->pitchSign > 0 ? "raises it" : "lowers it");
        if (g->pitchM < 0) { g->pitchM = g->yawM; g->pitchSign = 1; }
        g->phase = 3; g->at = t; g->hits0 = g_hitsBy[i]; return 1;
    }
    if (g->yawM < 0) return 0;
    const float *ym = gun_m(v, g->po[g->yawM], g->off[g->yawM]), *pm = gun_m(v, g->po[g->pitchM], g->off[g->pitchM]);
    if (!ym || !pm) return 0;
    float fx = ym[8], fz = ym[10], fy = pm[9];
    float eye[3] = { ym[12], ym[13] + 0.3f, ym[14] };
    if (gun_local(ym)) {   // vehicle space -> world: through the vehicle's rows (right, up, forward)
        const float *vm = vehicle_xf(v);
        if (!vm) return 0;
        float lf[3] = { ym[8], pm[9], ym[10] };
        fx = lf[0] * vm[0] + lf[1] * vm[4] + lf[2] * vm[8];
        fy = lf[0] * vm[1] + lf[1] * vm[5] + lf[2] * vm[9];
        fz = lf[0] * vm[2] + lf[1] * vm[6] + lf[2] * vm[10];
        eye[0] = vm[12] + ym[12] * vm[0] + ym[13] * vm[4] + ym[14] * vm[8];
        eye[1] = vm[13] + ym[12] * vm[1] + ym[13] * vm[5] + ym[14] * vm[9] + 0.3f;
        eye[2] = vm[14] + ym[12] * vm[2] + ym[13] * vm[6] + ym[14] * vm[10];
    }
    if (g->flip) { fx = -fx; fz = -fz; fy = -fy; }
    // the nearest enemy in sight (soldiers; and the vehicles enemies sit in)
    float best = 150, aim[3]; int found = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *pl = g_players[j];
        int pt = FIELD(pl, OFF_PLAYER_TEAM, int);
        if (!pt || pt == team) continue;
        float q[3];
        void *ev = player_vehicle(pl, NULL);
        if (ev) { const float *p = veh_pos(ev); if (!p) continue; memcpy(q, p, 12); q[1] += 1.0f; }
        else { void *s = bot_soldier(pl); if (!s) continue; memcpy(q, soldier_pos(s), 12); q[1] += 1.1f; }
        float d = sqrtf((q[0] - eye[0]) * (q[0] - eye[0]) + (q[1] - eye[1]) * (q[1] - eye[1]) + (q[2] - eye[2]) * (q[2] - eye[2]));
        if (d >= best || d < 3) continue;
        // (the ray starts 2.5 m out along the line so the vehicle's own body doesn't block it)
        float s0[3] = { eye[0] + (q[0] - eye[0]) / d * 2.5f, eye[1] + (q[1] - eye[1]) / d * 2.5f, eye[2] + (q[2] - eye[2]) / d * 2.5f };
        if (!clear_line(s0, q)) continue;
        best = d; memcpy(aim, q, 12); found = 1;
    }
    if (!found) { g->fireSince = 0; return 1; }
    float want = atan2f(aim[0] - eye[0], aim[2] - eye[2]), have = atan2f(fx, fz), err = wrap_pi(want - have);
    float wantP = atan2f(aim[1] - eye[1], dist2d(aim, eye)), haveP = asinf(fmaxf(-1, fminf(1, fy))), perr = wantP - haveP;
    b->turn = fmaxf(-1, fminf(1, g->yawSign * err * 3));
    b->pitch = fmaxf(-1, fminf(1, g->pitchSign * perr * 4));
    if (fabsf(err) < 0.1f && fabsf(perr) < 0.12f) {
        b->buttons |= BTN_FIRE;
        if (!g->fireSince) { g->fireSince = t; g->hits0 = g_hitsBy[i]; }
        if (t - g->fireSince > 6000 && g_hitsBy[i] == g->hits0 && t > g->flipAt) {
            g->flip = !g->flip; g->flipAt = t + 8000; g->fireSince = 0;
            H->log("gunner: %s - 6 s of fire, no hits: the gun's forward row taken the other way (%d)", (char *)b->player + OFF_PLAYER_NAME, g->flip);
        }
    } else if (fabsf(err) > 0.4f) g->fireSince = 0;
    return 1;
}
