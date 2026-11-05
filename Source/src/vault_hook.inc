// ThugBot: the vault carry for pseudo-player bots (included by core.c after brain.inc; vault_tick runs every server frame).
// Ported from FloodBots 2026-11-05, which took the idea from the Chinese bot mod.
//
// A jump at an obstacle starts the game's vault, but the server never animates pseudo-players, so it hangs and the bot is put
// back. So a bot in a vault jump is carried from where it jumped to the landing spot the movement code found, through the
// character's own velocity setter (positions written from outside are undone by the game).
//
// Hardline differs from BF4 here: the vault part is a heap object with no reliable way back to its soldier (FloodBots' hook
// on the part's tick faulted at level changes and left the carry dead on a whole map, 2026-11-05: 39 carries, 0 landed). But
// soldier+0x70 IS the character entity, seen on every bot, so no hook on the vault part is needed at all.
//
// the tester 2026-11-05: "make sure the vaulting never fails and they dont get placed back". Beyond the FloodBots version:
//  - the arc climbs above the obstacle's top first, then crosses, then comes down (it used to rise only 0.4 m above the
//    higher end, straight at the wall: a 1.3 m wall stopped it);
//  - a bot found short of the landing after the carry (put back, or stopped) is carried again, higher, VAULT_RETRIES times.
// "vaultstat" gives the counts, "vaultmove 0/1" turns the carry off / on.
#define VAULT_CARRY_MS 650
#define VAULT_RETRIES  4
#define OFF_SOLDIER_CPE 0x70        // CharacterPhysicsEntity*
#define VT_CPE          0x280a510   // its vtable (checked before every use)
typedef void (*SetVel_t)(void *, float *);
static int g_vaultMove = 1;
static unsigned g_vhCarries, g_vhKept, g_vhRetried, g_vhGaveUp, g_vhNoEntity, g_vhFaults;
static void extern_vault_window(ULONGLONG until) { (void)until; }
static struct {
    void *soldier; float from[3], to[3], apex; int moving, retries; ULONGLONG since, checkAt;
} g_vs[MAX_BOTS];

// Where the carry should be at fraction f: up first (a little way across), over at the top, then down onto the landing.
// On a retry straight up first: it did not get over, so it is against something.
static void vault_arc(int i, float f, float *q)
{
    const float *a = g_vs[i].from, *b = g_vs[i].to; float apex = g_vs[i].apex, h0 = !g_vs[i].retries ? 0.2f : g_vs[i].retries == 1 ? 0.0f : -0.3f, h, y;   // (from the 2nd retry: away from the edge while rising - under a dock's lip straight up hits the lip, Offshore)
   
    if (f > 1) f = 1;
    if (f < 0.4f) { float u = f / 0.4f; h = h0 * u; y = a[1] + (apex - a[1]) * u; }
    else if (f < 0.75f) { h = h0 + (1 - h0) * (f - 0.4f) / 0.35f; y = apex; }
    else { h = 1; y = apex + (b[1] + 0.05f - apex) * (f - 0.75f) / 0.25f; }
    q[0] = a[0] + (b[0] - a[0]) * h; q[1] = y; q[2] = a[2] + (b[2] - a[2]) * h;
}

static void vault_carry_start(int i, const float *p, const float *land, float climb, ULONGLONG t)
{
    float to[3]; memcpy(to, land, 12);   // (land may be this bot's own g_vs[i].to)
    memcpy(g_vs[i].from, p, 12); memcpy(g_vs[i].to, to, 12);
    g_vs[i].apex = fmaxf(fmaxf(p[1], to[1]), p[1] + climb) + 0.4f;
    g_vs[i].moving = 1; g_vs[i].since = t;
}

static void vault_bot_tick(int i, ULONGLONG t)
{
    char *s = H->bots[i] ? FIELD(H->bots[i], OFF_PLAYER_SOLDIER, char *) : NULL;
    if (s != g_vs[i].soldier) { g_vs[i].soldier = s; g_vs[i].moving = 0; }   // (dead, respawned, a new level)
    if (!s) return;
    MoveX *m = &g_mx[i];
    const float *p = (const float *)(s + OFF_SOLDIER_POSITION);
    if (!g_vs[i].moving) {   // 60 ms into the bot's own vault jump
        if (t < m->vaultUntil && t >= m->jumpFrom + 60 && (m->vaultLand[0] || m->vaultLand[2]) && dist2d(p, m->vaultLand) < 4.5f) {
            g_vs[i].retries = 0; vault_carry_start(i, p, m->vaultLand, m->jumpTop, t); g_vhCarries++;
        }
        return;
    }
    if (g_vs[i].moving == 1) {
        ULONGLONG el = t - g_vs[i].since;
        // Velocity that reaches the arc point 0.12 s ahead, from wherever the bot really is.
        float q[3]; vault_arc(i, (float)(el + 120) / VAULT_CARRY_MS, q);
        __declspec(align(16)) float v4[4] = { (q[0] - p[0]) / 0.12f, (q[1] - p[1]) / 0.12f, (q[2] - p[2]) / 0.12f, 0 };   // (the setter reads it with movaps: unaligned, every call faulted - 473 of 473, 2026-11-05)
        float vm = sqrtf(v4[0] * v4[0] + v4[1] * v4[1] + v4[2] * v4[2]);
        if (vm > 9) { v4[0] *= 9 / vm; v4[1] *= 9 / vm; v4[2] *= 9 / vm; }
        char *cpe = FIELD(s, OFF_SOLDIER_CPE, char *);
        if (!cpe || IsBadReadPtr(cpe, 0x150) || FIELD(cpe, 0, uintptr_t) != H->image + VT_CPE) { g_vhNoEntity++; g_vs[i].moving = 0; return; }
        if (isfinite(v4[0]) && isfinite(v4[1]) && isfinite(v4[2])) ((SetVel_t)(H->image + RVA_CHAR_SET_VELOCITY))(cpe, v4);
        if (m->vaultUntil < t + 200) m->vaultUntil = t + 200;   // (movement_tick holds forward through it, and judges it after)
        if (el >= VAULT_CARRY_MS) { g_vs[i].moving = 2; g_vs[i].checkAt = t + 300; }
        return;
    }
    if (t < g_vs[i].checkAt) return;
    const float *a = g_vs[i].from, *b = g_vs[i].to;
    float along = (p[0] - a[0]) * (b[0] - a[0]) + (p[2] - a[2]) * (b[2] - a[2]), len2 = (b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]);
    if (along > len2 * 0.6f && p[1] > b[1] - 0.5f) { g_vhKept++; g_vs[i].moving = 0; m->vaultLand[0] = m->vaultLand[2] = 0; return; }
    if (g_vs[i].retries < VAULT_RETRIES && dist2d(p, b) < 4.5f) {   // put back or stopped short: again, from where it is now, higher each go
        g_vs[i].retries++; g_vhRetried++;
        {   // a little further in each go, if there is floor there: a landing on the very edge catches the body's width
            float dl = dist2d(p, b), land[3]; memcpy(land, b, 12);
            if (dl > 0.1f) {
                float dx = (b[0] - p[0]) / dl, dz = (b[2] - p[2]) / dl, u[3] = { b[0] + dx * 0.5f, b[1] + 1.5f, b[2] + dz * 0.5f }, e[3] = { u[0], b[1] - 0.6f, u[2] }, h[3];
                if (ray_cast(u, e, h, NULL)) memcpy(land, h, 12);
            }
            vault_carry_start(i, p, land, fmaxf(0, g_vs[i].apex - 0.4f - p[1]) + 0.5f, t);
        }
        m->jumpFrom = t; m->jumpUntil = t + 300; m->vaultUntil = t + VAULT_CARRY_MS + 500;
        static unsigned rl; if (rl++ < 40) H->log("vault: %s short of the landing at %.1f %.1f %.1f (%.1f m off) - carried again (%d)", (char *)H->bots[i] + OFF_PLAYER_NAME, p[0], p[1], p[2], dist2d(p, b), g_vs[i].retries);
        return;
    }
    g_vhGaveUp++; g_vs[i].moving = 0; m->vaultLand[0] = m->vaultLand[2] = 0;
    static unsigned gl; if (gl++ < 40) H->log("vault: %s gave up at %.1f %.1f %.1f, landing %.1f %.1f %.1f, %d retries", (char *)H->bots[i] + OFF_PLAYER_NAME, p[0], p[1], p[2], b[0], b[1], b[2], g_vs[i].retries);
}

static void vault_tick(void)
{
    if (!g_vaultMove) return;
    ULONGLONG t = now_ms();
    for (int i = 0; i < H->botCount && i < MAX_BOTS; i++) {
        __try { vault_bot_tick(i, t); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_vs[i].moving = 0;
            if (g_vhFaults++ < 10) H->log("vault: fault %#x carrying bot %d", GetExceptionCode(), i);
        }
    }
}
static void vault_stat(void)
{
    H->log("vaultstat: carries %u, landed %u, carried again %u, gave up %u; no entity %u, faults %u", g_vhCarries, g_vhKept, g_vhRetried, g_vhGaveUp, g_vhNoEntity, g_vhFaults);
}
