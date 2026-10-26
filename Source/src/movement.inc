// Thugbot movement extras (included by brain.inc after objectives.inc): sprinting and getting over low obstacles.
//
//   Sprint: on a long open run to a far objective (a flag or enemy over 30 m off, the next waypoints in a near-straight
//           line), in bursts that differ per bot so a team doesn't move in lock step; always when running for cover.
//           Never while shooting, aiming down sights, crouched or turning hard.
//   Vault / jump: every 150 ms a walking bot looks 1.3 m ahead along its way at knee (0.45 m), waist (0.9 m) and head
//           (1.7 m) height. Something low in the way (a top 0.3 - 1.25 m above its feet, head height clear, floor on the
//           far side) gets a jump with forward held for 0.7 s: Hardline vaults when you jump into a low obstacle. Learned
//           stuck spots that were passed with a jump (learn.c) get a jump on the way in too.

// Jump through the pseudo-player action table (id 2): "jumptest" showed 15 of 16 bots jumping that way, 4 of 16 with
// EIAJump pressed through the input hook.
#define PP_ACT_JUMP 2
typedef struct MoveX { float vaultLand[3], tpTo[3]; ULONGLONG tpCheckAt, duckLook, duckUntil; int hadZoom; ULONGLONG vaultUntil, jumpUntil, jumpFrom, nextLook, sprintSlot; int sprinting; unsigned vaults; float vaultFrom[3], vaultTop, vaultDir[2];
                       int blind; float jumpDist, jumpSpeed, jumpTop; ULONGLONG runupUntil; int runups; float runupAt[3]; int waterJump, squareTries; } MoveX;   // blind: a stuck / forced jump (no low obstacle seen) - a failure says nothing about the spot
static float g_lastHitDist;   // low_obstacle: how far ahead the obstacle's face is
static float g_vaultMaxH = 1.05f;   // low_obstacle: tallest top it takes (swimming: higher - the body sits low in the water)
static MoveX g_mx[MAX_BOTS];
static unsigned g_vaultCount, g_sprintFrames, g_vaultOk, g_vaultFail, g_vaultLogged, g_tpOk, g_tpFail;
static float g_lastLand[3];   // low_obstacle: where it would land on the far side
// Which jump input a vault sends (the tester's clip, 2026-10-26: "their model appears on top but they get put back"): 0 both
// the pseudo-player jump action and the EIAJump button, 1 the action only, 2 the button only, 3 alternating per vault -
// "jumpmode N"; "vaults" logs the over / came-back counts per input so the one the server keeps can be picked.
static int g_jumpMode = 1, g_jumpUsed[MAX_BOTS];   // (5 min on mp_hills, alternating: both 13% came back, action only 10%,
                                                  // button only 16% - the input is not the main cause; action only kept)
static unsigned g_jmStat[3][2];
static int g_vaultTeleport = 0;   // "vaulttp 0/1": a vault that didn't get over puts the bot where it would have landed (the tester) - off: writing +0x640 / +0x7d0 was undone by the game 8 of 8 times
// Vaults that didn't get over (Chinatown 2026-10-23: three bots jumped at the same 0.9 m barrier by their spawn for 25 s).
// Two failures at a spot (1.5 m, same way) and it isn't tried again for 3 minutes; the grid link that way is blocked so
// the route goes round.
#define MAX_BAD_VAULTS 64
static struct { float p[3], h[2]; int fails; ULONGLONG until; } g_badVault[MAX_BAD_VAULTS];
static int g_badVaultNext;
// A spot is written off after 3 failed vaults (any bots, same way) within 2 minutes, for 2 minutes. One failure used to
// write it off for 5 minutes: on The Block the criminals' fence at x -88 by their spawn went "can't be vaulted" after a
// single fumble, routes that still crossed it had bots walk into it without jumping, and they barely reached a flag
// (the tester 2026-10-25). Never where a player has been seen vaulting: it can be done.
// (2 until 2026-10-26: the tester, "force vaulting until it works" - a wall written off after 2 bounce-backs left bots stuck
// behind it with no other way. Now 6 tries, each different - jump distance 0.85 / 0.6 / 1.1 / 0.45 m from the face, with
// a run-up from under 2.5 m/s - and a write-off lasts 2 minutes.)
#define BAD_VAULT_FAILS 6
static int human_vaulted_near(const float *p)
{
    if (!g_learn) return 0;
    for (int k = 0; k < g_learn->nSpots; k++) {
        BLSpot *sp = &g_learn->spots[k];
        if (sp->kind == BL_FOOT && bl_spot_jump(sp) && dist2d(p, sp->p) < 2.5f && fabsf(sp->p[1] - p[1]) < 1.5f) return 1;
    }
    return 0;
}
static int bad_vault(const float *p, float dx, float dz)
{
    ULONGLONG t = now_ms();
    for (int k = 0; k < MAX_BAD_VAULTS; k++)
        if (g_badVault[k].fails >= BAD_VAULT_FAILS && g_badVault[k].until > t && dist2d(g_badVault[k].p, p) < 1.5f &&
            g_badVault[k].h[0] * dx + g_badVault[k].h[1] * dz > 0.5f) return !human_vaulted_near(p);
    return 0;
}
static void note_bad_vault(const float *p, float dx, float dz)
{
    ULONGLONG t = now_ms();
    for (int k = 0; k < MAX_BAD_VAULTS; k++)
        if (g_badVault[k].until > t && dist2d(g_badVault[k].p, p) < 1.5f && g_badVault[k].h[0] * dx + g_badVault[k].h[1] * dz > 0.5f) {
            if (++g_badVault[k].fails == BAD_VAULT_FAILS && !human_vaulted_near(p)) {
                H->log("move: the obstacle at %.0f %.0f %.0f can't be vaulted (%d failures) - routes go round it", p[0], p[1], p[2], BAD_VAULT_FAILS);
                bot_block_towards(p[0], p[1], p[2], p[0] + dx * 4, p[2] + dz * 4);
            }
            g_badVault[k].until = t + 120000;
            return;
        }
    int k = g_badVaultNext++ % MAX_BAD_VAULTS;
    memcpy(g_badVault[k].p, p, 12); g_badVault[k].h[0] = dx; g_badVault[k].h[1] = dz; g_badVault[k].fails = 1; g_badVault[k].until = t + 120000;
}
static int vault_fails(const float *p, float dx, float dz)   // failures so far at this spot, this way
{
    ULONGLONG t = now_ms();
    for (int k = 0; k < MAX_BAD_VAULTS; k++)
        if (g_badVault[k].until > t && dist2d(g_badVault[k].p, p) < 1.5f && g_badVault[k].h[0] * dx + g_badVault[k].h[1] * dz > 0.5f) return g_badVault[k].fails;
    return 0;
}

static unsigned g_vaultBlind, g_vaultWait, g_swimLooks, g_swimVaults;
static float g_vStat[3][4]; static unsigned g_runups;   // worked / rose but came back / never rose: sums of speed, distance, top height, count
// Another player (either team) within `range` ahead of p along (dx, dz), less than 0.9 m off the line.
static int player_ahead(const float *p, float dx, float dz, float range)
{
    for (int j = 0; j < g_playerCount; j++) {
        void *s = bot_soldier(g_players[j]);
        if (!s) continue;
        const float *q = soldier_pos(s);
        float rx = q[0] - p[0], rz = q[2] - p[2], along = rx * dx + rz * dz, side = fabsf(rx * dz - rz * dx);
        if (along > 0.2f && along < range && side < 0.9f && fabsf(q[1] - p[1]) < 1.5f) return 1;
    }
    return 0;
}

// The top of an obstacle `dist` ahead along (dx, dz) from feet position p, if it is a low one we can get over.
static int low_obstacle(const float *p, float dx, float dz, float dist, float *topOut)
{
    float knee[3] = { p[0], p[1] + 0.45f, p[2] }, kneeTo[3] = { p[0] + dx * dist, p[1] + 0.45f, p[2] + dz * dist };
    float waist[3] = { p[0], p[1] + 0.9f, p[2] }, waistTo[3] = { p[0] + dx * dist, p[1] + 0.9f, p[2] + dz * dist };
    float head[3] = { p[0], p[1] + 1.7f, p[2] }, headTo[3] = { p[0] + dx * (dist + 0.8f), p[1] + 1.7f, p[2] + dz * (dist + 0.8f) };
    float hit[3];
    int kneeHit = ray_cast(knee, kneeTo, hit, NULL), waistHit = 0;
    float hx = hit[0], hz = hit[2];
    if (!kneeHit) { waistHit = ray_cast(waist, waistTo, hit, NULL); hx = hit[0]; hz = hit[2]; }
    if (!kneeHit && !waistHit) return 0;
    if (ray_cast(head, headTo, NULL, NULL)) return 0;            // a wall, not a low obstacle
    g_lastHitDist = sqrtf((hx - p[0]) * (hx - p[0]) + (hz - p[2]) * (hz - p[2]));
    // The obstacle's top: straight down just past where the ray met it.
    float over[3] = { hx + dx * 0.25f, p[1] + 1.6f, hz + dz * 0.25f }, down[3] = { over[0], p[1] - 0.5f, over[2] }, top[3];
    if (!ray_cast(over, down, top, NULL)) return 0;
    float h = top[1] - p[1];
    if (h < 0.3f || h > g_vaultMaxH) return 0;                         // a step the walk handles / too tall to vault (1.05+: failed, snapped back)
    // Somewhere to land: floor within 2 m below our level on the far side.
    float far1[3] = { hx + dx * 1.2f, p[1] + 1.6f, hz + dz * 1.2f }, far2[3] = { far1[0], p[1] - 2.0f, far1[2] }, land[3];
    if (!ray_cast(far1, far2, land, NULL)) return 0;
    // A railing with the floor more than a metre below on the far side is a balcony / stair edge, not a vault: the game
    // lifts the bot onto it and puts it back. Most of the "came back" vaults on Code Blue were on its upper floors (y 96 -
    // 100), at railings over the drops.
    if (land[1] < p[1] - 1.0f && g_vaultMaxH < 1.5f) return 0;
    memcpy(g_lastLand, land, 12);
    *topOut = h;
    return 1;
}

// Walking up (or down) a staircase on its route: the next route points within 6 m are 0.25 m+ above / below it. A step
// looks like a "low obstacle" to the vault check - jumping at stairs is what made bots stumble on them.
static int on_stairs(const Bot *b, const float *p)
{
    for (int k = b->wp; k < b->npts && k < b->wp + 4; k++) {
        const float *q = &b->path[k * 3];
        if (dist2d(p, q) > 6) break;
        if (fabsf(q[1] - p[1]) > 0.25f) return 1;
    }
    return 0;
}

// After steering, before combat: may press jump (holding forward) or sprint. Returns 1 while a vault is under way.
static int movement_tick(int i, Bot *b, void *soldier, const float *aim)
{
    MoveX *m = &g_mx[i];
    ULONGLONG t = now_ms();
    const float *p = soldier_pos(soldier);
    if (m->vaultUntil && t >= m->vaultUntil) {   // how did it go: up and over (higher, or well past where it jumped)?
        float rose = m->vaultTop - m->vaultFrom[1], went = dist2d(p, m->vaultFrom);
        // Over it: it rose AND ended up 0.8 m further along the way it jumped. A rise that ends back where it started is
        // the server snapping it back (the tester 2026-10-23: "they go back in front of what they vault over" - and it lags).
        float along = (p[0] - m->vaultFrom[0]) * m->vaultDir[0] + (p[2] - m->vaultFrom[2]) * m->vaultDir[1];
        // (and high enough to be on top: mp_hills' half-empty pool, 2026-10-26 - bots "vaulted" a 1.15 m ledge, rose 0.6 m,
        // drifted 2 m along the wall and counted as over while still in the pool)
        int ok = rose > fmaxf(0.3f, m->jumpTop - 0.4f) && along > 0.8f;
        (void)went;
        int tp = 0;
        if (!ok && g_vaultTeleport && (m->vaultLand[0] || m->vaultLand[2]) && dist2d(p, m->vaultLand) < 4 && !IsBadWritePtr((char *)soldier + 0x7d0, 12)) {
            // the tester: "calculate where a bot would be after vaulting and place them there" - the far-side landing spot,
            // written to the soldier's position (and its mirror); checked 0.4 s later whether the game kept it.
            float to[3] = { m->vaultLand[0], m->vaultLand[1] + 0.05f, m->vaultLand[2] };
            memcpy((char *)soldier + OFF_SOLDIER_POSITION, to, 12); memcpy((char *)soldier + 0x7d0, to, 12);
            memcpy(m->tpTo, to, 12); m->tpCheckAt = t + 400; tp = 1;
        }
        // (a blind jump - stuck / forced, nothing low seen - or one with someone right in front: the spot isn't to blame.
        // At The Block's playground 528 spots were written off that way in 20 min, routes went round them and the
        // crowd got worse.)
        if (!ok && (m->blind || player_ahead(p, m->vaultDir[0], m->vaultDir[1], 1.6f))) { g_vaultFail++; g_vaultBlind++; }
        if (!m->blind) g_jmStat[g_jumpUsed[i]][ok ? 0 : 1]++;
        if (ok) g_vaultOk++; else if (!(!ok && (m->blind || player_ahead(p, m->vaultDir[0], m->vaultDir[1], 1.6f)))) { g_vaultFail++; if (!tp) { note_bad_vault(m->vaultFrom, m->vaultDir[0], m->vaultDir[1]); if (bad_vault(m->vaultFrom, m->vaultDir[0], m->vaultDir[1])) b->npts = 0; } }
        if (!m->blind) { int c = ok ? 0 : rose > 0.3f ? 1 : 2; g_vStat[c][0] += m->jumpSpeed; g_vStat[c][1] += m->jumpDist; g_vStat[c][2] += m->jumpTop; g_vStat[c][3] += 1; }
        if (g_vaultLogged++ < 25) H->log("move: %s vault %s - rose %.2f m, moved %.1f m, from %.0f %.0f %.0f (jumped %.2f m from a %.2f m top at %.1f m/s)%s%s%s", (char *)b->player + OFF_PLAYER_NAME, ok ? "worked" : "FAILED", rose, went,
                                        m->vaultFrom[0], m->vaultFrom[1], m->vaultFrom[2], m->jumpDist, m->jumpTop, m->jumpSpeed, g_nav && nav_is_water(g_nav, m->vaultFrom[0], m->vaultFrom[1], m->vaultFrom[2]) ? " (in water)" : "", m->hadZoom ? " (sights were up)" : "",
                                        m->blind ? " (blind jump)" : "");
        m->vaultUntil = 0; m->blind = 0;
    }
    if (m->tpCheckAt && t >= m->tpCheckAt) {   // did the placing stick?
        int kept = dist2d(p, m->tpTo) < 1.2f && fabsf(p[1] - m->tpTo[1]) < 1.5f;
        if (kept) g_tpOk++; else { g_tpFail++; note_bad_vault(m->vaultFrom, m->vaultDir[0], m->vaultDir[1]); }
        static unsigned logged; if (logged++ < 40) H->log("move: %s put over the obstacle at %.0f %.0f %.0f - %s (%u kept, %u undone)", (char *)b->player + OFF_PLAYER_NAME, m->tpTo[0], m->tpTo[1], m->tpTo[2], kept ? "kept" : "the game moved it back", g_tpOk, g_tpFail);
        m->tpCheckAt = 0;
    }
    if (t < m->vaultUntil) {
        // Straight through: forward + sprint, no turning, no aiming / crouching / firing. Combat runs before this and
        // left the sights up and the turn on (bots aim from 5 m now): the vault never started ("rose 0.00") or the
        // server snapped it back (the tester: "they start but end up back before where they were").
        b->fwd = 1; b->strafe = 0; b->turn = 0; b->pitch = 0;
        b->buttons &= ~(BTN_ZOOM | BTN_CROUCH | BTN_FIRE);
        b->buttons |= BTN_SPRINT;   // (vaults are a running move: forward + sprint all the way through)
        if (t >= m->jumpFrom && t < m->jumpUntil) { b->buttons |= BTN_JUMP; b->action = PP_ACT_JUMP; }   // held a moment: a one-frame tap was often missed
        // (in water the jump comes as taps, 150 ms on / off, over the longer window: one long press only ever gave a plain
        // 0.6 m hop at the pool ledge - a second press while rising is what climbs out)
        if (m->waterJump && t >= m->jumpFrom && t < m->jumpUntil && ((t - m->jumpFrom) / 150) % 2) { b->buttons &= ~BTN_JUMP; b->action = 0; }
        if (p[1] > m->vaultTop) m->vaultTop = p[1];
        return 1;
    }

    if (t < m->runupUntil) {   // backing off for a run-up: straight back, no turning, then the approach sprints in
        b->fwd = -1; b->strafe = 0; b->turn = 0; b->buttons &= ~(BTN_SPRINT | BTN_ZOOM | BTN_CROUCH | BTN_FIRE);
        return 1;
    }
    float dx = aim[0] - p[0], dz = aim[2] - p[2], dl = sqrtf(dx * dx + dz * dz);
    // Something low overhead ahead (blocked at 1.65 m, clear at 0.95 m, 1.2 m out the way it is going): crouch under it
    // (the tester: "they should crouch to get under some objects"). Held 900 ms after the last time it saw one.
    if (dl > 0.5f && b->fwd > 0.3f && t >= m->duckLook) {
        m->duckLook = t + 150;
        float ux = dx / dl, uz = dz / dl, h0[3] = { p[0], p[1] + 1.65f, p[2] }, h1[3] = { p[0] + ux * 1.2f, p[1] + 1.65f, p[2] + uz * 1.2f };
        float l0[3] = { p[0], p[1] + 0.95f, p[2] }, l1[3] = { p[0] + ux * 1.6f, p[1] + 0.95f, p[2] + uz * 1.6f };
        if (ray_cast(h0, h1, NULL, NULL) && !ray_cast(l0, l1, NULL, NULL)) m->duckUntil = t + 900;
    }
    if (t < m->duckUntil) { b->buttons |= BTN_CROUCH; b->buttons &= ~BTN_SPRINT; }
    if (g_learn && dl > 0.5f && b->fwd > 0.5f && t >= m->nextLook) {   // a learned jump spot just ahead, going its way
        for (int k = 0; k < g_learn->nSpots; k++) {
            BLSpot *sp = &g_learn->spots[k];
            if (sp->kind != BL_FOOT || !bl_spot_jump(sp) || dist2d(p, sp->p) > 1.6f || fabsf(sp->p[1] - p[1]) > 1.0f) continue;
            if ((sp->h[0] || sp->h[1]) && sp->h[0] * dx / dl + sp->h[1] * dz / dl < 0.5f) continue;
            { int zoomed = (b->buttons & BTN_ZOOM) != 0; m->hadZoom = zoomed; m->jumpFrom = t + (zoomed ? 220 : 0); if (!zoomed) { b->buttons |= BTN_JUMP; b->action = PP_ACT_JUMP; } }
            m->vaultUntil = m->jumpFrom + 1300; m->jumpUntil = m->jumpFrom + 300; b->buttons &= ~BTN_ZOOM; memcpy(m->vaultFrom, p, 12);
            { float tl; m->vaultLand[0] = m->vaultLand[2] = 0; if (low_obstacle(p, dx / dl, dz / dl, 1.6f, &tl)) memcpy(m->vaultLand, g_lastLand, 12); }   // (a learned jump spot: the far side if it is a low obstacle)
            // (these three used to sit inside the comment above: a learned-spot vault was judged on the last vault's
            // height and direction, failed, and wrote the spot off)
            m->vaultTop = p[1]; m->vaultDir[0] = dx / dl; m->vaultDir[1] = dz / dl; m->blind = 0; m->jumpTop = 0; m->waterJump = 0;
            g_jumpUsed[i] = g_jumpMode == 3 ? (int)(m->vaults % 3) : g_jumpMode; m->vaults++; g_vaultCount++;
            return 1;
        }
    }
    // Swimming: vault out at a ledge (the tester 2026-10-25: "there are many areas that vaulting can be used to get out of the
    // water"). The body sits low, so a ledge counts up to 1.9 m above the feet; the jump comes at the wall (0.85 m) like
    // on land. (It was switched off on 2026-10-25 morning because bots jumped at docks from 1.3 m and fell back - the
    // jump-at-the-face timing fixed that.)
    int swimming = g_nav && nav_is_water(g_nav, p[0], p[1], p[2]);
    if (dl > 0.5f && b->fwd > 0.5f && t >= m->nextLook) {
        m->nextLook = t + 150;
        g_vaultMaxH = swimming ? 1.9f : 1.05f;
        if (swimming) g_swimLooks++;
        dx /= dl; dz /= dl;
        float top;
        int justSpawned = b->spawnAt && t - b->spawnAt < 2000;   // (round start: 14 of 20 "rose 0.00" fails were bots still in the spawn pose)
        int lowAhead = !justSpawned && !on_stairs(b, p) && !bad_vault(p, dx, dz) && low_obstacle(p, dx, dz, 1.3f, &top);
        g_vaultMaxH = 1.05f;   // (back to the land value for every other caller)
        if (lowAhead) {
            // Seen 1.3 m out: keep running at it and jump when the face is under 0.85 m away. Jumping at 1.3 m landed
            // the bot short, against the obstacle, and the game snapped it back. Someone in the way: wait for them.
            // Someone in the way - in front of it, or standing where it would land (a vault onto a spot someone is
            // standing on is the one the server puts back: 120 of 155 failed vaults on mp_hills had a player there,
            // and the tester's clip of D shows a crowd at the ledge): step sideways along the wall to a free stretch instead of
            // queueing behind them (side by seed, flips after 1.5 s).
            {
                int busy = player_ahead(p, dx, dz, 1.6f);
                for (int j = 0; j < g_playerCount && !busy && (g_lastLand[0] || g_lastLand[2]); j++) {
                    void *s2 = bot_soldier(g_players[j]);
                    if (s2 && s2 != soldier && dist2d(soldier_pos(s2), g_lastLand) < 1.3f && fabsf(soldier_pos(s2)[1] - g_lastLand[1]) < 1.5f) busy = 1;
                }
                if (busy) {
                    int side = ((b->seed + (unsigned)(t / 1500)) & 1) ? 1 : -1;
                    b->strafe = g_strafeSign * side; b->fwd = 0.2f; b->buttons &= ~BTN_SPRINT;
                    m->nextLook = t + 120; g_vaultWait++;
                    return 0;
                }
            }
            // In water (the tester, mp_hills pool: "they cannot vault over the ledges... vaulting works for me 10 times out of
            // 10"): wading is slow, a jump 0.85 m out falls short. Walk right up to the ledge (0.45 m), then jump, held
            // longer; no run-up (backing off in water only loses the little speed there is).
            static const float kJumpAt[4] = { 0.85f, 0.6f, 1.1f, 0.45f };
            int tries = vault_fails(p, dx, dz);
            if (g_lastHitDist > (swimming ? 0.45f : kJumpAt[tries & 3])) {
                m->nextLook = t + 40;
                b->buttons |= BTN_SPRINT; b->buttons &= ~(BTN_ZOOM | BTN_CROUCH); b->fwd = 1; b->strafe = 0;
                return 0;
            }
            {   // Too slow at the face: back off and run in. 3 minutes of Code Blue, 2026-10-25: vaults that worked came in at
                // 3.5 m/s on average, the ones that rose and came back at 2.1 m/s (many from a standstill) - the tester: "bots vault
                // yet they get placed back". Two run-ups per spot, then it jumps anyway.
                const float *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY);
                float sp = v[0] * dx + v[2] * dz;
                if (dist2d(m->runupAt, p) > 3) m->runups = 0;
                if (sp < (tries ? 2.5f : 1.2f) && m->runups < 1 && !swimming) {   // (only from a near standstill: run-ups at 2.4 - 2.8 m/s did not change the come-back rate, 17 - 23%, and cost a second each)
                    m->runups++; memcpy(m->runupAt, p, 12); g_runups++;
                    m->runupUntil = t + 900; m->nextLook = m->runupUntil + 40;
                    b->fwd = -1; b->strafe = 0; b->turn = 0; b->buttons &= ~(BTN_SPRINT | BTN_ZOOM | BTN_CROUCH | BTN_FIRE);
                    return 1;
                }
                m->runups = 0;
            }
            if (swimming) {   // square on to the ledge first: the climb out took only when facing it (1 in 9 worked at an angle)
                float best = 1e9f, bestA = atan2f(dx, dz), a0 = bestA;
                for (int k = -6; k <= 6; k++) {
                    float a = a0 + k * 0.1745f, e0[3] = { p[0], p[1] + 0.6f, p[2] }, e1[3] = { p[0] + sinf(a) * 1.5f, p[1] + 0.6f, p[2] + cosf(a) * 1.5f }, h[3];
                    if (ray_cast(e0, e1, h, NULL)) { float d = dist2d(e0, h); if (d < best) { best = d; bestA = a; } }
                }
                float yaw = soldier_yaw(soldier), err = wrap_pi(bestA - yaw);
                if (fabsf(err) > 0.12f && m->squareTries++ < 25) {
                    b->turn = turn_input(yaw, bestA, 3.0f); b->fwd = 0.4f; b->strafe = 0; b->buttons &= ~(BTN_SPRINT | BTN_ZOOM);
                    m->nextLook = t + 40;
                    return 1;
                }
                m->squareTries = 0;
                dx = sinf(bestA); dz = cosf(bestA);
            }
            m->vaultDir[0] = dx; m->vaultDir[1] = dz; m->blind = 0;
            { const float *v = (const float *)((char *)soldier + OFF_SOLDIER_VELOCITY); m->jumpSpeed = v[0] * dx + v[2] * dz; m->jumpDist = g_lastHitDist; m->jumpTop = top; }
            { int zoomed = (b->buttons & BTN_ZOOM) != 0; m->hadZoom = zoomed; m->jumpFrom = t + (zoomed ? 220 : 0); if (!zoomed) { b->buttons |= BTN_JUMP; b->action = PP_ACT_JUMP; } }   // (sights up: they come down first - a jump meanwhile was ignored, "rose 0.00")
            m->vaultUntil = m->jumpFrom + (swimming ? 1800 : 1300); m->jumpUntil = m->jumpFrom + (swimming ? 900 : 300); m->waterJump = swimming; b->buttons &= ~BTN_ZOOM; memcpy(m->vaultLand, g_lastLand, 12);
            memcpy(m->vaultFrom, p, 12); m->vaultTop = p[1];
            g_jumpUsed[i] = g_jumpMode == 3 ? (int)(m->vaults % 3) : g_jumpMode; m->vaults++; if (swimming) g_swimVaults++;
            if (g_vaultCount++ < 40) H->log("move: %s vaults a %.2f m obstacle at %.0f %.0f %.0f%s", (char *)b->player + OFF_PLAYER_NAME, top, p[0], p[1], p[2], swimming ? " (out of the water)" : "");
            return 1;
        }
    }

    // Sprint.
    Fight *f = &g_fight[i];
    int fighting = f->state == CS_FIGHT || f->state == CS_COVER_HOLD || b->enemy;
    int wantRun = f->state == CS_COVER_RUN;
    if (!fighting && !wantRun && b->npts > b->wp + 1) {
        float farDist = dist2d(p, b->target);
        if (farDist > 8) {
            // Waypoints ahead in a near-straight line (within 25 deg) for 12 m or more.
            const float *w0 = &b->path[b->wp * 3];
            float h0 = atan2f(w0[0] - p[0], w0[2] - p[2]), run = dist2d(p, w0);
            for (int k = b->wp + 1; k < b->npts && run < 6; k++) {
                const float *a = &b->path[(k - 1) * 3], *c = &b->path[k * 3];
                if (fabsf(wrap_pi(atan2f(c[0] - a[0], c[2] - a[2]) - h0)) > 0.45f) break;
                run += dist2d(a, c);
            }
            // (the tester 2026-10-25: "bots walking slowly" - sprint whenever the way ahead is straight for 6 m; it used to be
            // 12 m, and only in two thirds of each bot's 5 s slots.)
            // (the tester 2026-10-26: "they should be running / sprinting a lot" - 4 m straight is enough, from 8 m out)
            wantRun = run >= 4;
        }
    }
    if (wantRun && b->fwd > 0.6f && fabsf(b->strafe) < 0.5f && fabsf(b->turn) < 0.8f && !(b->buttons & (BTN_CROUCH | BTN_ZOOM | BTN_FIRE))) {
        b->buttons |= BTN_SPRINT;
        b->strafe = 0;
        g_sprintFrames++;
    }
    return 0;
}

// Stuck: jump first (most "stuck" spots are something to get over), then the side step.
static void movement_unstick(int i, Bot *b, ULONGLONG t)
{
    // Jump only when something low is right in front (the way it faces or the way it was going); otherwise the side
    // step alone - hopping on the spot at walls looked broken.
    MoveX *m = &g_mx[i];
    void *s = bot_soldier(b->player);
    if (!s) return;
    const float *p = soldier_pos(s);
    float yaw = soldier_yaw(s), top, dx = b->target[0] - p[0], dz = b->target[2] - p[2], dl = sqrtf(dx * dx + dz * dz);
    int low = !on_stairs(b, p) && (low_obstacle(p, sinf(yaw), cosf(yaw), 1.2f, &top) || (dl > 0.5f && low_obstacle(p, dx / dl, dz / dl, 1.2f, &top)));
    if (!low) return;
    b->buttons |= BTN_JUMP; b->action = PP_ACT_JUMP;
    m->vaultUntil = t + 700; m->jumpUntil = t + 300; m->vaultDir[0] = sinf(yaw); m->vaultDir[1] = cosf(yaw);
    memcpy(m->vaultFrom, p, 12); m->vaultTop = p[1]; m->blind = 1; m->jumpTop = 0; m->jumpFrom = t;
}

// Jump / vault forwards without the height test (a bot stuck twice at the same spot): a fence or gate the rays misjudged.
static void movement_force_vault(int i, Bot *b, ULONGLONG t)
{
    MoveX *m = &g_mx[i];
    void *s = bot_soldier(b->player);
    if (!s) return;
    const float *p = soldier_pos(s);
    if (on_stairs(b, p)) return;
    float yaw = soldier_yaw(s);
    b->buttons |= BTN_JUMP | BTN_SPRINT; b->action = PP_ACT_JUMP;
    m->vaultUntil = t + 700; m->jumpUntil = t + 300; m->vaultDir[0] = sinf(yaw); m->vaultDir[1] = cosf(yaw);
    memcpy(m->vaultFrom, p, 12); m->vaultTop = p[1]; m->blind = 1; m->jumpTop = 0; m->jumpFrom = t;
}

static void movement_dump(void)
{
    H->log("move: by jump input - both: %u over / %u came back; action only: %u / %u; button only: %u / %u (jumpmode %d)",
           g_jmStat[0][0], g_jmStat[0][1], g_jmStat[1][0], g_jmStat[1][1], g_jmStat[2][0], g_jmStat[2][1], g_jumpMode);
    H->log("move: since last: %u vault(s) / jump(s) over low obstacles (%u got over, %u didn't, %u of those blind / blocked by a player), %u waited for a player in the way, %u out of the water, %u sprint frame(s)",
           g_vaultCount, g_vaultOk, g_vaultFail, g_vaultBlind, g_vaultWait, g_swimVaults, g_sprintFrames);
    g_swimVaults = 0;
    if (g_runups) H->log("move: %u run-up(s) before vaults (too slow at the obstacle)", g_runups);
    g_runups = 0;
    for (int c = 0; c < 3; c++) if (g_vStat[c][3] > 0) H->log("move: vaults that %s: %.0f, average approach %.1f m/s, jumped %.2f m out, top %.2f m", c == 0 ? "worked" : c == 1 ? "rose but came back" : "never rose",
        g_vStat[c][3], g_vStat[c][0] / g_vStat[c][3], g_vStat[c][1] / g_vStat[c][3], g_vStat[c][2] / g_vStat[c][3]);
    memset(g_vStat, 0, sizeof g_vStat);
    g_vaultOk = g_vaultFail = g_vaultBlind = g_vaultWait = 0; g_vaultLogged = 0;
    g_vaultCount = 0; g_sprintFrames = 0;
}

// "jumptest" / "jumptest act" / "jumptest crouch": every bot holds jump (our input hook / pseudo-player action 2) for 3 s
// standing still; logs how many of them left the ground. Tells whether jumping (and so vaulting) works at all.
static ULONGLONG g_jtUntil; static int g_jtMode; static float g_jtBase[MAX_BOTS], g_jtTop[MAX_BOTS], g_jtYaw0[MAX_BOTS], g_jtYawMove[MAX_BOTS];
static int g_jtLocked0[MAX_BOTS];
static void jumptest_start(int mode)
{
    g_jtMode = mode; g_jtUntil = now_ms() + 3000;
    for (int i = 0; i < H->botCount; i++) {
        void *s = bot_soldier(H->bots[i]);
        g_jtBase[i] = g_jtTop[i] = s ? soldier_pos(s)[1] : 0;
        g_jtYaw0[i] = s ? soldier_yaw(s) : 0; g_jtYawMove[i] = 0; g_jtLocked0[i] = g_fight[i].locked;
    }
    if (mode == 2) g_testYaw = 1.0f;
    if (mode >= 5) { H->log("jumptest: variant %d for 3 s", mode); return; }
    H->log("jumptest: %s for 3 s", mode == 1 ? "pseudo-player action 2" : mode == 2 ? "yaw 1.0 through the input hook (turn input 0)" :
           mode == 3 ? "turn input 1.0 (pseudo-player)" : mode == 4 ? "turn input -1.0 (pseudo-player)" : "EIAJump through the input hook");
}
// Called per bot after bot_think while a test runs: overrides its input.
static void jumptest_bot(int i, Bot *b)
{
    if (!g_jtUntil) return;
    void *s = bot_soldier(H->bots[i]);
    b->fwd = b->strafe = b->turn = 0;
    ULONGLONG left = g_jtUntil - now_ms();
    if (g_jtMode == 5) { b->fwd = 1; b->action = 2; }                                  // running forward, jump held
    else if (g_jtMode == 6) { if (left > 2700) b->action = 2; }                        // standing, jump for 300 ms
    else if (g_jtMode == 7) { b->fwd = 1; if (left > 2700) b->action = 2; }            // running, jump for 300 ms
    else if (g_jtMode == 8) { b->fwd = 1; b->action = (left / 150) & 1 ? 2 : 0; }      // running, jump tapped
    else if (g_jtMode == 1) b->action = 2; else if (g_jtMode == 0) b->buttons = BTN_JUMP; else if (g_jtMode == 3) b->turn = 1; else if (g_jtMode == 4) b->turn = -1;
    if (s && soldier_pos(s)[1] > g_jtTop[i]) g_jtTop[i] = soldier_pos(s)[1];
    if (s) { float d = fabsf(wrap_pi(soldier_yaw(s) - g_jtYaw0[i])); if (d > g_jtYawMove[i]) g_jtYawMove[i] = d; }
}
static void jumptest_frame(void)
{
    if (!g_jtUntil || now_ms() < g_jtUntil) return;
    int up = 0, n = 0; float best = 0;
    for (int i = 0; i < H->botCount; i++) { if (!g_jtBase[i]) continue; n++; float r = g_jtTop[i] - g_jtBase[i]; if (r > 0.3f) up++; if (r > best) best = r; g_bot[i].action = g_testAction; }
    if (g_jtMode >= 2 && g_jtMode <= 4) {
        int turned = 0, lockedTurned = 0, locked = 0;
        for (int i = 0; i < H->botCount; i++) { if (!g_jtBase[i]) continue; locked += g_jtLocked0[i]; if (g_jtYawMove[i] > 0.3f) { turned++; lockedTurned += g_jtLocked0[i]; } }
        H->log("yawtest: %d of %d bot(s) turned; of the %d with a locked heading, %d turned", turned, n, locked, lockedTurned);
        g_testYaw = 0;
    } else
    H->log("jumptest: %d of %d bot(s) left the ground (highest %.2f m)", up, n, best);
    g_jtUntil = 0;
}
