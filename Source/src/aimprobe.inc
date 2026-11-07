// Aim discovery (2026-11-07): where does the engine keep a gun seat's aim? READ ONLY: nothing here writes to the game or calls into it.
// A human sits in a non-driver seat: for 60 s every float of the vehicle, the player and the objects they point to (two levels) is
// watched 5 times a second. The server sees no human input, so the person follows a timetable from the moment they sit down:
//   0-10 s hands off, 10-30 s left and right only, 30-50 s up and down only, 50-60 s hands off.
// The log ("aimprobe:") ranks what moved in each part only; Thugbot_aimprobe.txt keeps the full history of the best 400, for matching offline.
// Safety: objects are accepted only from committed, readable, unguarded memory (VirtualQuery: IsBadReadPtr would trip a thread's stack guard);
// every read is under __try; each pointer is followed again each sample, so a freed or replaced object is dropped; slow samples stop the run.
// Thugbot_cmd.txt: "aimprobe" allows every seat again, "aimprobe off" / "aimprobe on" switch it.

#define AP_MAX_OBJ 2000
#define AP_MAX_FLOATS 300000
#define AP_MAX_VQ 6000
#define AP_HASH 16384
#define AP_SAMPLES 320
#define AP_SERIES 6000
#define AP_FILE_TOP 400
#define AP_MS 200
#define AP_RUN_MS 60000
#define AP_DONE 16
#define AP_HOLD_MS 75000

typedef struct ApObj { char *p; int parent, parentOff, span, first, dead; } ApObj;
typedef struct ApFloat { float last, lo, hi, tv[4]; int series; unsigned short moved0; unsigned char valid, pad; } ApFloat;
typedef struct ApTop { int f; float score; } ApTop;

static struct {
    int on; void *pl, *veh; int seat; ULONGLONG start, nextAt;
    ApObj *obj; int nObj; ApFloat *f; int nF;
    float *series; int *serF; int nSer, serDropped, n;
    float p0[3], moved, body0, turned; double msSum, msMax; int slow, dropped;
    char vehName[64], who[64];
} A;
static volatile LONG g_apOff, g_apReset;
static unsigned g_apDone[AP_DONE]; static int g_apDoneN, g_apRuns;

static unsigned ap_key(const char *s, int seat) { unsigned h = 5381 + seat * 131; for (; *s; s++) h = h * 33 ^ (unsigned char)*s; return h; }

static int ap_mem_ok(const void *p, size_t n, int *vq)
{
    MEMORY_BASIC_INFORMATION mi;
    if (++*vq > AP_MAX_VQ || !VirtualQuery(p, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY))) return 0;
    return (const char *)p + n <= (const char *)mi.BaseAddress + mi.RegionSize;
}
static int ap_seen(uintptr_t *hash, uintptr_t p)
{
    unsigned h = (unsigned)((p >> 3) * 2654435761u) & (AP_HASH - 1);
    while (hash[h]) { if (hash[h] == p) return 1; h = (h + 1) & (AP_HASH - 1); }
    hash[h] = p; return 0;
}
static int ap_add(uintptr_t *hash, int *vq, uintptr_t p, int parent, int off, int span)
{
    if (A.nObj >= AP_MAX_OBJ || A.nF + span / 4 > AP_MAX_FLOATS) return -1;
    if (p < 0x10000 || p > 0x7fff00000000ull || (p & 7) || (p >= H->image && p < H->image + 0x6000000)) return -1;
    if (ap_seen(hash, p)) return -1;
    if (!ap_mem_ok((void *)p, span, vq)) return -1;
    ApObj *o = &A.obj[A.nObj]; o->p = (char *)p; o->parent = parent; o->parentOff = off; o->span = span; o->first = A.nF; o->dead = 0;
    A.nF += span / 4;
    return A.nObj++;
}
// __try here, with nothing to unwind: a freed object only costs that object
static void ap_links(int from, int to, int scan, int span, uintptr_t *hash, int *vq)
{
    for (int k = from; k < to; k++) {
        ApObj *o = &A.obj[k]; int lim = scan < o->span ? scan : o->span;
        for (int off = 0; off + 8 <= lim; off += 8) {
            uintptr_t q = 0;
            __try { q = *(uintptr_t *)(o->p + off); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            ap_add(hash, vq, q, k, off, span);
        }
    }
}
static int ap_fine(float v) { float a = fabsf(v); return v == v && a < 1e6f && (a == 0 || a > 1e-20f); }
static int ap_read_obj(ApObj *o, int win, float elapsed)
{
    __try {
        if (o->parent >= 0) { ApObj *par = &A.obj[o->parent]; if (par->dead || *(char **)(par->p + o->parentOff) != o->p) return 0; }
        const float *src = (const float *)o->p; ApFloat *pf = A.f + o->first; int nf = o->span / 4;
        for (int k = 0; k < nf; k++) {
            float v = src[k]; ApFloat *q = pf + k;
            if (!ap_fine(v)) { q->valid = 0; continue; }
            if (!q->valid) { q->valid = 1; q->last = v; if (q->lo == 0 && q->hi == 0) q->lo = q->hi = v; continue; }
            float d = fabsf(v - q->last);
            if (d <= 1e-5f) continue;
            float before = q->last;
            q->tv[win] += d; q->last = v;
            if (v < q->lo) q->lo = v;
            if (v > q->hi) q->hi = v;
            if (win == 0) { if (q->moved0 < 60000) q->moved0++; }
            else if (q->series < 0 && d > 1e-4f && q->moved0 < 15) {
                if (A.nSer < AP_SERIES) {
                    q->series = A.nSer; A.serF[A.nSer] = o->first + k;
                    float *s = A.series + A.nSer * AP_SAMPLES; for (int j = 0; j < AP_SAMPLES; j++) s[j] = before;   // (it hadn't moved until now)
                    A.nSer++;
                } else { A.serDropped++; q->series = -2; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static void ap_free(void)
{
    free(A.obj); free(A.f); free(A.series); free(A.serF);
    A.obj = NULL; A.f = NULL; A.series = NULL; A.serF = NULL; A.on = 0;
}
static int ap_start(void *pl, void *v, int seat, const char *name, ULONGLONG t)
{
    uintptr_t *hash = (uintptr_t *)calloc(AP_HASH, sizeof(uintptr_t));
    A.obj = (ApObj *)calloc(AP_MAX_OBJ, sizeof(ApObj));
    A.f = (ApFloat *)calloc(AP_MAX_FLOATS, sizeof(ApFloat));
    A.series = (float *)malloc(AP_SERIES * AP_SAMPLES * sizeof(float));
    A.serF = (int *)malloc(AP_SERIES * sizeof(int));
    if (!hash || !A.obj || !A.f || !A.series || !A.serF) { free(hash); ap_free(); H->log("aimprobe: no memory, not run"); return 0; }
    LARGE_INTEGER a, b, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
    A.nObj = A.nF = A.nSer = A.serDropped = A.n = A.slow = A.dropped = 0; A.msSum = A.msMax = 0; A.moved = A.turned = 0;
    int vq = 0;
    ap_add(hash, &vq, (uintptr_t)v, -1, 0, 0x2400);
    ap_add(hash, &vq, (uintptr_t)pl, -1, 0, 0x2000);
    int roots = A.nObj;
    if (roots < 2) { free(hash); ap_free(); H->log("aimprobe: vehicle or player memory not readable, not run"); return 0; }
    ap_links(0, roots, 0x2400, 0x800, hash, &vq);   // level 1
    int l1 = A.nObj;
    ap_links(roots, l1, 0x400, 0x400, hash, &vq);   // level 2 (a list's entries)
    for (int i = 0; i < A.nF; i++) A.f[i].series = -1;
    free(hash);
    QueryPerformanceCounter(&b);
    const float *vp = veh_pos(v);
    if (vp) memcpy(A.p0, vp, sizeof A.p0); else memset(A.p0, 0, sizeof A.p0);
    A.body0 = veh_yaw(v);
    A.on = 1; A.pl = pl; A.veh = v; A.seat = seat; A.start = t; A.nextAt = t;
    snprintf(A.vehName, sizeof A.vehName, "%s", name);
    snprintf(A.who, sizeof A.who, "%s", (char *)pl + OFF_PLAYER_NAME);
    g_apHoldV = v; g_apHoldUntil = t + AP_HOLD_MS; g_apHoldKick = 0;
    H->log("aimprobe: holding %s for the test", A.vehName);
    H->log("aimprobe: %s in %s seat %d: start. Watching %d floats in %d objects (%d in the first ring, %d memory checks, %.1f ms). 0-10 s still, 10-30 s left/right, 30-50 s up/down, 50-60 s still",
           A.who, A.vehName, seat, A.nF, A.nObj, l1 - roots, vq, (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart);
    return 1;
}
static void ap_path(int fi, char *out, size_t n)
{
    int k = 0;
    while (k + 1 < A.nObj && A.obj[k + 1].first <= fi) k++;
    int off = (fi - A.obj[k].first) * 4, chain[4], c = 0;
    for (int o = k; o >= 0 && c < 4; o = A.obj[o].parent) chain[c++] = o;
    int w = snprintf(out, n, "%s", chain[c - 1] == 0 ? "veh" : "player");
    for (int j = c - 1; j > 0 && w < (int)n - 12; j--) w += snprintf(out + w, n - w, "[%#x]", A.obj[chain[j - 1]].parentOff);
    uintptr_t vt = 0;
    __try { vt = *(uintptr_t *)A.obj[k].p; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; }
    if (vt >= H->image && vt < H->image + 0x6000000) snprintf(out + w, n - w, "+%#x (vt %#llx)", off, (unsigned long long)(vt - H->image));
    else snprintf(out + w, n - w, "+%#x", off);
}
static float ap_score(const ApFloat *q, int want)
{
    float a = q->tv[want], other = 3 * q->tv[0] + q->tv[want == 1 ? 2 : 1] + q->tv[3];
    return a < 0.05f ? 0 : a * a / (a + other);
}
// Angle- or direction-sized: in a flying vehicle world positions and counters moved far more and filled both the ranking and the file (first runs 2026-11-07: nothing else kept)
static int ap_small(const ApFloat *q) { return q->lo >= -10 && q->hi <= 10 && q->hi - q->lo >= 0.02f; }
static void ap_top(ApTop *top, int n, int fi, float s)
{
    if (s <= top[n - 1].score) return;
    int k = n - 1;
    while (k > 0 && top[k - 1].score < s) { top[k] = top[k - 1]; k--; }
    top[k].f = fi; top[k].score = s;
}
static int ap_cmp(const void *a, const void *b) { float x = ((const ApTop *)a)->score, y = ((const ApTop *)b)->score; return x < y ? 1 : x > y ? -1 : 0; }
static void ap_finish(const char *why, ULONGLONG t)
{
    float sec = (float)(t - A.start) / 1000.0f;
    H->log("aimprobe: %s in %s seat %d: %s after %.0f s. %d samples, %d floats moved after the still part (%d not kept), %d objects dropped, sample %.2f ms on average, %.2f ms at worst; the vehicle moved %.1f m and turned %.2f rad",
           A.who, A.vehName, A.seat, why, sec, A.n, A.nSer, A.serDropped, A.dropped, A.n ? A.msSum / A.n : 0.0, A.msMax, A.moved, A.turned);
    if (A.moved > 2 || A.turned > 0.2f) H->log("aimprobe: the vehicle itself moved: rankings are mixed with that (the file still helps)");
    for (int pass = 1; pass <= 2 && A.n > 50; pass++) {
        ApTop top[20]; memset(top, 0, sizeof top);
        for (int i = 0; i < A.nF; i++) { if (!ap_small(&A.f[i])) continue; float s = ap_score(&A.f[i], pass); if (s > 0) ap_top(top, 20, i, s); }
        for (int k = 0; k < 20 && top[k].score > 0; k++) {
            char path[128]; ApFloat *q = &A.f[top[k].f]; ap_path(top[k].f, path, sizeof path);
            H->log("aimprobe: %s #%d score %.2f %s: moved still %.2f, l/r %.2f, u/d %.2f, still %.2f; range %.3f to %.3f", pass == 1 ? "LEFT/RIGHT" : "UP/DOWN", k + 1, top[k].score, path, q->tv[0], q->tv[1], q->tv[2], q->tv[3], q->lo, q->hi);
        }
        if (top[0].score <= 0) H->log("aimprobe: nothing moved in the %s part only", pass == 1 ? "left/right" : "up/down");
    }
    // The history of the best movers (one write, at the end, when it is over)
    if (A.nSer && A.n > 50) {
        ApTop *rank = (ApTop *)malloc(A.nSer * sizeof(ApTop));
        FILE *fp = rank ? fopen("Thugbot_aimprobe.txt", "a") : NULL;
        if (fp) {
            for (int s = 0; s < A.nSer; s++) { ApFloat *q = &A.f[A.serF[s]]; float y = ap_score(q, 1), p = ap_score(q, 2); rank[s].f = s; rank[s].score = (y > p ? y : p) + (ap_small(q) ? 1e7f : 0); }
            qsort(rank, A.nSer, sizeof(ApTop), ap_cmp);
            fprintf(fp, "# %s seat %d, %d samples, every %d ms, %s, vehicle moved %.1f m, turned %.2f rad. Columns: path | moved in still, l/r, u/d, still | values\n", A.vehName, A.seat, A.n, AP_MS, why, A.moved, A.turned);
            for (int r = 0; r < A.nSer && r < AP_FILE_TOP; r++) {
                int s = rank[r].f; ApFloat *q = &A.f[A.serF[s]]; char path[128]; ap_path(A.serF[s], path, sizeof path);
                fprintf(fp, "%s | %.2f %.2f %.2f %.2f |", path, q->tv[0], q->tv[1], q->tv[2], q->tv[3]);
                const float *v = A.series + s * AP_SAMPLES;
                for (int j = 0; j < A.n; j++) fprintf(fp, " %.4g", v[j]);
                fputc('\n', fp);
            }
            fclose(fp);
            H->log("aimprobe: history of up to %d values added to Thugbot_aimprobe.txt", AP_FILE_TOP);
        }
        free(rank);
    }
    ap_free();
}
static void aimprobe_tick(void)
{
    if (InterlockedExchange(&g_apReset, 0)) { g_apDoneN = g_apRuns = 0; H->log("aimprobe: every seat can be measured again"); }
    ULONGLONG t = now_ms();
    if (g_apHoldV && t >= g_apHoldUntil) g_apHoldV = NULL;
    if (g_apHoldV) {   // No bot walks up to it. Bots already aboard get out, unless it is under way: nobody is thrown out of a flying helicopter (that run is reported as moved)
        for (int k = 0; k < MAX_BOTS; k++) { g_bot[k].noVeh = g_apHoldV; g_bot[k].noVehUntil = g_apHoldUntil; }
        if (A.on && A.n >= 3) { const float *hp = veh_pos(A.veh); g_apHoldKick = A.moved < 3 && hp && air_below(hp) < 8; }
    }
    if (!A.on) {
        static ULONGLONG lookAt;
        if (g_apOff || g_apRuns >= AP_DONE || t < lookAt) return;
        lookAt = t + 500;
        for (int j = 0; j < g_playerCount; j++) {
            void *pl = g_players[j]; int seat = -1;
            if (is_bot_player(pl)) continue;
            void *v = player_vehicle(pl, &seat);
            if (!v || seat <= 0) continue;
            const char *nm = vehicle_name(v);
            if (!nm) continue;
            unsigned key = ap_key(nm, seat); int done = 0;
            for (int k = 0; k < g_apDoneN; k++) done |= g_apDone[k] == key;
            if (done) continue;
            if (ap_start(pl, v, seat, nm, t)) g_apRuns++;
            return;
        }
        return;
    }
    if (t < A.nextAt) return;
    A.nextAt = t + AP_MS;
    int present = 0, seat = -1;
    for (int j = 0; j < g_playerCount; j++) if (g_players[j] == A.pl) present = 1;
    if (g_apOff) { ap_finish("switched off", t); return; }
    if (!present || player_vehicle(A.pl, &seat) != A.veh || seat != A.seat) { ap_finish("left the seat", t); g_apHoldV = NULL; return; }
    ULONGLONG el = t - A.start;
    if (el >= AP_RUN_MS || A.n >= AP_SAMPLES) {
        if (g_apDoneN < AP_DONE) g_apDone[g_apDoneN++] = ap_key(A.vehName, A.seat);
        ap_finish("done", t); return;
    }
    LARGE_INTEGER a, b, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
    int win = el < 10000 ? 0 : el < 30000 ? 1 : el < 50000 ? 2 : 3;
    for (int k = 0; k < A.nObj; k++) {
        ApObj *o = &A.obj[k];
        if (o->dead) continue;
        if (!ap_read_obj(o, win, 0)) { o->dead = 1; A.dropped++; }
    }
    for (int s = 0; s < A.nSer; s++) A.series[s * AP_SAMPLES + A.n] = A.f[A.serF[s]].last;
    A.n++;
    const float *vp = veh_pos(A.veh);
    if (vp) { float d = dist2d(vp, A.p0) + fabsf(vp[1] - A.p0[1]); if (d > A.moved) A.moved = d; }
    { float tr = fabsf(wrap_pi(veh_yaw(A.veh) - A.body0)); if (tr > A.turned) A.turned = tr; }
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
    A.msSum += ms; if (ms > A.msMax) A.msMax = ms;
    if (ms > 8 && ++A.slow >= 3) { ap_finish("stopped, samples too slow", t); g_apOff = 1; }
}
