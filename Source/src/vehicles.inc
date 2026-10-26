// Thugbot vehicles (included by brain.inc before brain_tick): finding out who sits in what.
//
// ServerPlayer + 0x1688 (OFF_PLAYER_SOLDIER) is really the player's *attached controllable*: the soldier on foot, the
// vehicle when seated (+0x1680 is the entry / seat index; the vehicle's entries vector is at +0x1b0 - RVA 0x10f58d0,
// "ServerEntryComponent::getEntry"). So a bot in a car has a controllable whose class (vtable) is not the soldier's.
// The soldier class is learned as the most common controllable class among all players.

static uintptr_t g_soldierVt;          // the soldier entity's vtable (most common attached-controllable class)
static int g_vehicleStay;              // "vehiclestay": don't get out of spawn vehicles (for probing)

static void vehicles_learn_soldier_vt(void)
{
    static ULONGLONG at;
    ULONGLONG t = now_ms();
    if (t - at < 5000) return;
    at = t;
    uintptr_t vts[64]; int counts[64], n = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *c = bot_soldier_raw(g_players[j]);
        if (!c || IsBadReadPtr(c, 8)) continue;
        uintptr_t vt = *(uintptr_t *)c;
        int k = 0;
        while (k < n && vts[k] != vt) k++;
        if (k == n) { if (n == 64) continue; vts[n] = vt; counts[n++] = 0; }
        counts[k]++;
    }
    int best = -1;
    for (int k = 0; k < n; k++) if (best < 0 || counts[k] > counts[best]) best = k;
    if (best >= 0 && counts[best] >= 4 && vts[best] != g_soldierVt) {
        g_soldierVt = vts[best];
        H->log("vehicles: soldier class vtable %#llx (%d of the players on foot)", (unsigned long long)(g_soldierVt - H->image), counts[best]);
    }
}

// The vehicle a player sits in (its attached controllable, when that isn't a soldier), else NULL. seatOut: entry index.
static void *player_vehicle(void *pl, int *seatOut)
{
    void *c = bot_soldier_raw(pl);
    if (!c || !g_soldierVt || IsBadReadPtr(c, 8) || *(uintptr_t *)c == g_soldierVt) return NULL;
    if (seatOut) *seatOut = FIELD(pl, OFF_PLAYER_ENTRY_INDEX, int);
    return c;
}

// "vehprobe": every bot in a vehicle - class, seat, the Vec3 at +0x640, and candidate transforms in the vehicle object
// (three orthonormal rows followed by a position near that Vec3).
static void vehicles_probe(void)
{
    int found = 0;
    for (int i = 0; i < H->botCount; i++) {
        int seat = -1;
        void *v = player_vehicle(H->bots[i], &seat);
        if (!v) continue;
        found++;
        uintptr_t vt = *(uintptr_t *)v;
        const float *p640 = (const float *)((char *)v + 0x640);
        H->log("vehprobe: %s in %p (vt %#llx) seat %d, +0x640 %.1f %.1f %.1f", (char *)H->bots[i] + OFF_PLAYER_NAME, v,
               (unsigned long long)(vt - H->image), seat, p640[0], p640[1], p640[2]);
        // the soldier itself: a pointer in the player to an object of the soldier class
        for (int off = 0; off < 0x2000; off += 8) {
            void **pp = (void **)((char *)H->bots[i] + off);
            if (IsBadReadPtr(pp, 8)) break;
            void *o = *pp;
            if (!o || o == v || ((uintptr_t)o & 7) || IsBadReadPtr(o, 0x700) || *(uintptr_t *)o != g_soldierVt) continue;
            const float *sp = (const float *)((char *)o + OFF_SOLDIER_POSITION), *sv = (const float *)((char *)o + OFF_SOLDIER_VELOCITY);
            H->log("vehprobe:   soldier at player+%#x: %p at %.1f %.1f %.1f, velocity %.1f %.1f %.1f, yaw %.2f", off, o, sp[0], sp[1], sp[2], sv[0], sv[1], sv[2], soldier_yaw(o));
        }
        {   // the sources that matched on the first cars: +0x1c50, [+0x208]+0xb0, [+0xe30]+0xb0 (and their matrices' fwd rows)
            static const int pOff[2] = { 0x208, 0xe30 };
            const float *q = (const float *)((char *)v + 0x1c50);
            char line[256]; int n = snprintf(line, sizeof line, "+0x1c50 %.1f %.1f %.1f", q[0], q[1], q[2]);
            for (int k = 0; k < 2; k++) {
                char *o = FIELD(v, pOff[k], char *);
                if (!o || IsBadReadPtr(o, 0xc0)) { n += snprintf(line + n, sizeof line - n, " | [%#x] -", pOff[k]); continue; }
                const float *w = (const float *)(o + 0xb0), *fw = (const float *)(o + 0xa0);
                n += snprintf(line + n, sizeof line - n, " | [%#x]+0xb0 %.1f %.1f %.1f fwd-row %.2f %.2f %.2f", pOff[k], w[0], w[1], w[2], fw[0], fw[1], fw[2]);
            }
            H->log("vehprobe:   %s", line);
        }
        // world-position candidates: Vec3s that look like a spot on this map (height 50 - 110, away from the origin), in
        // the vehicle itself and one pointer away (first 0x400 bytes of each object it points to)
        if (found > 1) continue;
        for (int off = 0; off < 0x2000; off += 4) {
            const float *f = (const float *)((char *)v + off);
            if (IsBadReadPtr(f, 12)) break;
            if (f[1] > 50 && f[1] < 110 && fabsf(f[0]) > 3 && fabsf(f[0]) < 600 && fabsf(f[2]) > 3 && fabsf(f[2]) < 600)
                H->log("vehprobe:   vec at +%#05x: %.2f %.2f %.2f", off, f[0], f[1], f[2]);
        }
        for (int po = 0; po < 0x1000; po += 8) {
            void **pp = (void **)((char *)v + po);
            if (IsBadReadPtr(pp, 8)) break;
            char *o = (char *)*pp;
            if (!o || ((uintptr_t)o & 7) || (uintptr_t)o < 0x10000 || IsBadReadPtr(o, 0x400)) continue;
            for (int off = 0; off < 0x400; off += 4) {
                const float *f = (const float *)(o + off);
                if (f[1] > 50 && f[1] < 110 && fabsf(f[0]) > 3 && fabsf(f[0]) < 600 && fabsf(f[2]) > 3 && fabsf(f[2]) < 600)
                    H->log("vehprobe:   vec at [+%#05x]+%#05x: %.2f %.2f %.2f", po, off, f[0], f[1], f[2]);
            }
        }
        continue;
        for (int off = 0; off < 0x1000; off += 16) {
            const float *m = (const float *)((char *)v + off);
            if (IsBadReadPtr(m, 64)) break;
            float l0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], l1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], l2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
            float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6], d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
            if (fabsf(l0 - 1) > 0.02f || fabsf(l1 - 1) > 0.02f || fabsf(l2 - 1) > 0.02f || fabsf(d01) > 0.02f || fabsf(d02) > 0.02f) continue;
            H->log("vehprobe:   matrix at +%#05x: right %.2f %.2f %.2f up %.2f %.2f %.2f fwd %.2f %.2f %.2f, then %.1f %.1f %.1f", off,
                   m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
        }
    }
    if (!found) H->log("vehprobe: no bot in a vehicle (soldier vt %#llx)", (unsigned long long)(g_soldierVt ? g_soldierVt - H->image : 0));
}

// The vehicle's world transform (a candidate: the first of the part matrices at +0x1c20 - right, up, forward rows,
// then the position at +0x1c50; Chinatown's getaway car read -40.6 73.8 -136.4 there). Checked by "drivetest".
#define OFF_VEH_TRANSFORM 0x1c20
static const float *veh_pos(void *v); static float veh_yaw(void *v);   // drive.inc (per-vehicle transform)
static const float *vehicle_pos(void *v) { const float *p = veh_pos(v); static const float zero[3]; return p ? p : zero; }
static float vehicle_yaw(void *v) { return veh_yaw(v); }

// "drivetest [turn]": every bot in a driver's seat holds throttle (and turn, if given) for 4 s; logs where the car
// was and is, which way its forward row points and which way it actually went.
static ULONGLONG g_driveTestUntil;
static float g_driveTestTurn, g_driveTestStrafe, g_dtFrom[MAX_BOTS][3], g_dtYaw[MAX_BOTS];
// "drivetest [turn]" steers with the turn (mouse) input, "drivetest s<level>" with strafe (A / D in a car).
static void drivetest_start(float turn, float strafe)
{
    g_driveTestUntil = now_ms() + 4000; g_driveTestTurn = turn; g_driveTestStrafe = strafe;
    int n = 0;
    for (int i = 0; i < H->botCount; i++) {
        int seat; void *v = player_vehicle(H->bots[i], &seat);
        if (!v || seat != 0) { g_dtFrom[i][0] = 1e9f; continue; }
        memcpy(g_dtFrom[i], vehicle_pos(v), 12); g_dtYaw[i] = vehicle_yaw(v); n++;
    }
    H->log("drivetest: %d driver(s), throttle 1, turn %.1f, strafe %.1f for 4 s", n, turn, strafe);
}
static void drivetest_bot(int i, Bot *b)
{
    if (!g_driveTestUntil) return;
    int seat; void *v = player_vehicle(H->bots[i], &seat);
    if (!v || seat != 0 || g_dtFrom[i][0] > 1e8f) return;
    b->fwd = 1; b->strafe = g_driveTestStrafe; b->turn = g_driveTestTurn; b->buttons = 0; b->action = 0;
}
static void drivetest_frame(void)
{
    if (!g_driveTestUntil || now_ms() < g_driveTestUntil) return;
    g_driveTestUntil = 0;
    for (int i = 0; i < H->botCount; i++) {
        int seat; void *v = player_vehicle(H->bots[i], &seat);
        if (!v || seat != 0 || g_dtFrom[i][0] > 1e8f) continue;
        const float *p = vehicle_pos(v);
        float dx = p[0] - g_dtFrom[i][0], dz = p[2] - g_dtFrom[i][2];
        H->log("drivetest: %s %.1f %.1f -> %.1f %.1f (%.1f m, went %.2f rad), facing %.2f -> %.2f", (char *)H->bots[i] + OFF_PLAYER_NAME,
               g_dtFrom[i][0], g_dtFrom[i][2], p[0], p[2], sqrtf(dx * dx + dz * dz), atan2f(dx, dz), g_dtYaw[i], vehicle_yaw(v));
    }
}

// "vehscan": for every vehicle anyone sits in, the places that hold a world transform - three orthonormal rows and a
// position inside the nav grid - directly in the vehicle (+off) or one pointer away ([+po]+off). Prints the
// (po, off) pairs and on how many of the vehicles each one holds (the one that holds on all of them is the answer).
static int plausible_matrix(const float *m)
{
    float l0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], l1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], l2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
    float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6], d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
    if (fabsf(l0 - 1) > 0.02f || fabsf(l1 - 1) > 0.02f || fabsf(l2 - 1) > 0.02f || fabsf(d01) > 0.02f || fabsf(d02) > 0.02f) return 0;
    if (!(m[13] == m[13]) || fabsf(m[12]) + fabsf(m[14]) < 3 || fabsf(m[12]) > 5000 || fabsf(m[14]) > 5000) return 0;
    return !g_nav || nav_covers(g_nav, m[12], m[14], 0);
}
static void vehicles_scan(void)
{
    static struct { int po, off, hits; } c[512];
    int nc = 0, nv = 0;
    void *seen[64]; int ns = 0;
    for (int j = 0; j < g_playerCount; j++) {
        void *v = player_vehicle(g_players[j], NULL);
        if (!v) continue;
        int dup = 0; for (int k = 0; k < ns; k++) dup |= seen[k] == v;
        if (dup || ns >= 64) continue;
        seen[ns++] = v; nv++;
        for (int po = -1; po < 0x1000; po += (po < 0 ? 1 : 8)) {
            char *o = po < 0 ? (char *)v : FIELD(v, po, char *);
            int span = po < 0 ? 0x2400 : 0x400;
            if (!o || ((uintptr_t)o & 7) || (uintptr_t)o < 0x10000 || IsBadReadPtr(o, span)) continue;
            for (int off = 0; off + 64 <= span; off += 16) {
                if (!plausible_matrix((const float *)(o + off))) continue;
                int k = 0;
                while (k < nc && !(c[k].po == po && c[k].off == off)) k++;
                if (k == nc) { if (nc == 512) continue; c[nc].po = po; c[nc].off = off; c[nc].hits = 0; nc++; }
                c[k].hits++;
            }
        }
    }
    for (int k = 0; k < nc; k++)
        if (c[k].hits * 2 >= nv) H->log("vehscan: %s%#x]+%#x holds a world transform on %d of %d vehicle(s)", c[k].po < 0 ? "[self" : "[+", c[k].po < 0 ? 0 : c[k].po, c[k].off, c[k].hits, nv);
    H->log("vehscan: %d vehicle(s), %d candidate place(s)", nv, nc);
}

// "vehname": text reachable from an occupied vehicle (its pointers, and theirs: 2 levels, the first 0x200 bytes of each
// object) that looks like a name - printable, 4+ characters, letters - to find where the vehicle's name lives.
static int looks_like_text(const char *p)
{
    if (IsBadReadPtr(p, 48)) return 0;
    int n = 0, letters = 0;
    while (n < 48 && p[n] >= 32 && p[n] < 127) { letters += isalpha((unsigned char)p[n]) != 0; n++; }
    return n >= 4 && n < 48 && p[n] == 0 && letters * 2 >= n ? n : 0;
}
static void vehicles_names(void)
{
    void *v = NULL;
    for (int j = 0; j < g_playerCount && !v; j++) v = player_vehicle(g_players[j], NULL);
    if (!v) { H->log("vehname: nobody is in a vehicle"); return; }
    int shown = 0;
    for (int a = 0; a < 0x400 && shown < 40; a += 8) {
        char *o1 = FIELD(v, a, char *);
        if (!o1 || ((uintptr_t)o1 & 7) || (uintptr_t)o1 < 0x10000 || IsBadReadPtr(o1, 0x200)) continue;
        if (looks_like_text(o1)) { H->log("vehname: [+%#x] \"%s\"", a, o1); shown++; }
        for (int b = 0; b < 0x200 && shown < 40; b += 8) {
            char *o2 = FIELD(o1, b, char *);
            if (!o2 || (uintptr_t)o2 < 0x10000 || IsBadReadPtr(o2, 8)) continue;
            if (looks_like_text(o2)) { H->log("vehname: [[+%#x]+%#x] \"%s\"", a, b, o2); shown++; continue; }
            if (((uintptr_t)o2 & 7) || IsBadReadPtr(o2, 0x80)) continue;
            for (int c = 0; c < 0x80 && shown < 40; c += 8) {
                char *o3 = FIELD(o2, c, char *);
                if (o3 && (uintptr_t)o3 > 0x10000 && looks_like_text(o3)) { H->log("vehname: [[[+%#x]+%#x]+%#x] \"%s\"", a, b, c, o3); shown++; }
            }
        }
    }
}

// Vehicles nobody has used yet aren't attached to any player, and the server's entity lists aren't mapped. So: every
// 90 s (and 15 s after a level loads), a background thread reads the server's private memory (ReadProcessMemory on
// itself: a page freed meanwhile fails the read instead of crashing) for objects whose first 8 bytes are a vehicle
// class vtable seen on a used vehicle. The game thread then keeps the ones that have a world transform on the map
// (drive.inc vehicle_xf), so parked cars, boats and respawned ones become known.
#define MAX_VEH_CLASSES 8
static uintptr_t g_vehClassVt[MAX_VEH_CLASSES];
static volatile LONG g_vehClassN;
#define MAX_FOUND_VEH 256
static void *g_foundVeh[MAX_FOUND_VEH];
static volatile LONG g_foundVehN, g_vehScanBusy, g_vehScanDone;
static ULONGLONG g_vehScanAt;
static void vehicle_class_note(uintptr_t vt)
{
    for (int k = 0; k < g_vehClassN; k++) if (g_vehClassVt[k] == vt) return;
    if (g_vehClassN < MAX_VEH_CLASSES) { g_vehClassVt[g_vehClassN] = vt; InterlockedIncrement(&g_vehClassN); H->log("vehicles: vehicle class %#llx", (unsigned long long)(vt - H->image)); }
}
static DWORD WINAPI vehicle_scan_thread(LPVOID unused)
{
    (void)unused;
    static unsigned char buf[1 << 16];
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    int n = 0, classes = g_vehClassN;
    while (VirtualQuery((void *)a, &mbi, sizeof mbi) == sizeof mbi && n < MAX_FOUND_VEH) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE)) && !(mbi.Protect & PAGE_GUARD))
            for (uintptr_t p = base; p < end && n < MAX_FOUND_VEH; p += sizeof buf) {
                SIZE_T want = end - p < sizeof buf ? end - p : sizeof buf, got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void *)p, buf, want, &got) || got < 8) continue;
                for (SIZE_T o = 0; o + 8 <= got; o += 8) {
                    uintptr_t v = *(uintptr_t *)(buf + o);
                    for (int c = 0; c < classes; c++) if (v == g_vehClassVt[c]) { g_foundVeh[n++] = (void *)(p + o); break; }
                    if (n >= MAX_FOUND_VEH) break;
                }
            }
        if (end <= a) break;
        a = end;
    }
    g_foundVehN = n;
    InterlockedExchange(&g_vehScanDone, 1);
    InterlockedExchange(&g_vehScanBusy, 0);
    return 0;
}
static void vehicle_scan_start(int force)
{
    ULONGLONG t = now_ms();
    if (!g_vehClassN || g_vehScanBusy || (!force && t - g_vehScanAt < 90000)) return;
    g_vehScanAt = t;
    InterlockedExchange(&g_vehScanBusy, 1);
    HANDLE th = CreateThread(NULL, 0, vehicle_scan_thread, NULL, 0, NULL);
    if (th) CloseHandle(th); else InterlockedExchange(&g_vehScanBusy, 0);
}
