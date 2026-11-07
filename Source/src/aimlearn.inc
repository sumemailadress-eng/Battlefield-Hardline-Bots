// Learned aim for gun seats and soldiers (included by combat.inc). Soldiers: one key per difficulty ("soldier_2"), not per weapon: the weapon in hand can't be read here.
// Per vehicle type + seat and range band (0-50, 50-100, 100-200, 200+ m): how much to lead, how high to
// aim, a yaw bias; and which way round the gun's forward row and elevation are, once a hit has proved it. One setting at a time gets a small trial: 10 s of firing with
// it against the rate before it, kept only if it clearly gives more hits. Thugbot_learned\aim.txt, shared by all maps. Ini: AimLearn=0 off, AimLearnMaxSkill 0..1 scales
// how far the settings may move (the gun still has to get on target first: this only trims the aim point).
#define AL_BANDS 4
#define AL_PARAMS 4
#define AL_MAX 64
typedef struct AimBand {
    float v[AL_PARAMS];            // lead multiple (1 = plain), height offset m, yaw bias rad, burst length multiple (soldiers)
    float base;                    // hits per second of fire with the settings kept
    float fireS; unsigned hits;    // this window
    int param, dir, trying; float saved;
} AimBand;
#define ARC_BINS 36
typedef struct AimLearn { char key[48]; int nparams; int known, flip; float pitchSign; AimBand b[AL_BANDS]; ULONGLONG logAt; float logFrom; unsigned char arc[ARC_BINS]; float pLo, pHi; } AimLearn;   // arc: directions from the nose, 10 degrees each: 0 not known, 1 the gun has been there, 2 it stopped short of there
static AimLearn g_al[AL_MAX]; static int g_alN, g_alLoaded, g_alDirty; static ULONGLONG g_alSavedAt;
static const float kAlStep[AL_PARAMS] = { 0.1f, 0.4f, 0.006f, 0.15f }, kAlMax[AL_PARAMS] = { 0.4f, 2.5f, 0.04f, 0.5f };
// How far the settings may move: AimLearnMaxSkill, and lower on the lower difficulties (easy 20% ... elite 100%) so they stay what they are
static float al_cap(int param) { return kAlMax[param] * fmaxf(0, fminf(1, g_aimLearnMax)) * (g_set.difficulty + 1) / 5.0f; }

static AimLearn *al_new(const char *key)
{
    if (g_alN >= AL_MAX) return NULL;
    AimLearn *a = &g_al[g_alN++];
    memset(a, 0, sizeof *a); snprintf(a->key, sizeof a->key, "%s", key); a->pitchSign = 1; a->nparams = 3; a->pLo = -3; a->pHi = 3;
    for (int k = 0; k < AL_BANDS; k++) { a->b[k].v[0] = 1; a->b[k].v[3] = 1; a->b[k].dir = 1; }
    return a;
}
static AimLearn *al_find(const char *key, int make)
{
    for (int k = 0; k < g_alN; k++) if (!strcmp(g_al[k].key, key)) return &g_al[k];
    return make ? al_new(key) : NULL;
}
static void aimlearn_load(void)
{
    if (g_alLoaded) return;
    g_alLoaded = 1;
    FILE *f = fopen("Thugbot_learned\\aim.txt", "r");
    if (!f) return;
    char line[256], key[48]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        int band, known, flip; float a, b, c, d, e = 1; char arcs[ARC_BINS + 2];
        if (sscanf(line, "arc2 %47s %37s %f %f", key, arcs, &a, &b) == 4 && strlen(arcs) == ARC_BINS) { AimLearn *al = al_find(key, 1); if (al) { for (int q = 0; q < ARC_BINS; q++) al->arc[q] = (unsigned char)(arcs[q] - '0') > 2 ? 0 : (unsigned char)(arcs[q] - '0'); al->pLo = a; al->pHi = b; n++; } continue; }
        if (sscanf(line, "seat %47s %d %d %f", key, &known, &flip, &a) == 4) { AimLearn *al = al_find(key, 1); if (al) { al->known = known; al->flip = flip; al->pitchSign = a < 0 ? -1.0f : 1.0f; n++; } }
        else if (sscanf(line, "band %47s %d %f %f %f %f %f", key, &band, &a, &b, &c, &d, &e) >= 6 && band >= 0 && band < AL_BANDS) { AimLearn *al = al_find(key, 1); if (al) { al->b[band].v[0] = a; al->b[band].v[1] = b; al->b[band].v[2] = c; al->b[band].v[3] = e; al->b[band].base = d; n++; } }
    }
    fclose(f);
    H->log("aimlearn: %d record(s) from Thugbot_learned\\aim.txt", n);
}
static void aimlearn_save(void)
{
    if (!g_alDirty) return;
    g_alDirty = 0; g_alSavedAt = now_ms();
    CreateDirectoryA("Thugbot_learned", NULL);
    FILE *f = fopen("Thugbot_learned\\aim.txt", "w");
    if (!f) return;
    for (int k = 0; k < g_alN; k++) {
        AimLearn *a = &g_al[k];
        fprintf(f, "seat %s %d %d %.0f\n", a->key, a->known, a->flip, a->pitchSign);
        { char arcs[ARC_BINS + 1]; int any = a->pLo > -3 || a->pHi < 3; for (int q = 0; q < ARC_BINS; q++) { arcs[q] = (char)('0' + a->arc[q]); any |= a->arc[q]; } arcs[ARC_BINS] = 0; if (any) fprintf(f, "arc2 %s %s %.3f %.3f\n", a->key, arcs, a->pLo, a->pHi); }
        for (int q = 0; q < AL_BANDS; q++) {
            AimBand *b = &a->b[q];
            float v[AL_PARAMS]; memcpy(v, b->v, sizeof v);
            if (b->trying) v[b->param] = b->saved;   // (what is kept, not a trial)
            fprintf(f, "band %s %d %.3f %.3f %.4f %.4f %.3f\n", a->key, q, v[0], v[1], v[2], b->base, v[3]);
        }
    }
    fclose(f);
}
static int al_band(float d) { return d < 50 ? 0 : d < 100 ? 1 : d < 200 ? 2 : 3; }
static AimLearn *al_seat(void *v, int seat)
{
    if (!g_aimLearn) return NULL;
    aimlearn_load();
    char key[48]; snprintf(key, sizeof key, "%s_seat%d", vehicle_name(v), seat);
    return al_find(key, 1);
}
// Every frame a seat is aiming: firing or not, and the hits it made since the last call
static AimLearn *al_soldier(void)
{
    static AimLearn *c; static int cDiff = -1;
    if (!g_aimLearn) return NULL;
    if (c && cDiff == g_set.difficulty) return c;
    aimlearn_load();
    char key[48]; snprintf(key, sizeof key, "soldier_%d", g_set.difficulty);
    c = al_find(key, 1); cDiff = g_set.difficulty;
    if (c) c->nparams = 4;
    return c;
}
static void al_tick(AimLearn *a, int band, int firing, float dt, unsigned hits, float window)
{
    if (!a) return;
    AimBand *b = &a->b[band];
    if (firing) b->fireS += dt;
    b->hits += hits;
    if (b->fireS < window) return;
    float rate = b->hits / b->fireS; unsigned got = b->hits;
    b->fireS = 0; b->hits = 0;
    g_alDirty = 1;
    if (b->trying) {
        int better = rate > b->base * 1.2f && got >= 2;   // (clearly better, on a real count)
        if (better) {
            ULONGLONG t = now_ms();
            if (t >= a->logAt) { a->logAt = t + 60000; H->log("aimlearn: %s %s hits/s %.3f -> %.3f, lead x%.2f, height %+.1f m, yaw %+.3f, burst x%.2f", a->key, band == 0 ? "0-50 m" : band == 1 ? "50-100 m" : band == 2 ? "100-200 m" : "200+ m", b->base, rate, b->v[0], b->v[1], b->v[2], b->v[3]); }
            b->base = rate; b->trying = 0;
        } else {
            b->v[b->param] = b->saved; b->trying = 0; b->base = b->base * 0.8f + rate * 0.2f;
            b->dir = -b->dir;
            if (b->dir > 0) b->param = (b->param + 1) % a->nparams;   // (both ways tried: the next setting)
        }
        return;
    }
    b->base = b->base > 0 ? b->base * 0.7f + rate * 0.3f : rate;
    if (b->param >= a->nparams) b->param = 0;
    float cap = al_cap(b->param), mid = b->param == 0 || b->param == 3 ? 1.0f : 0.0f;
    if (cap <= 0) return;
    b->saved = b->v[b->param]; b->trying = 1;
    b->v[b->param] = fmaxf(mid - cap, fminf(mid + cap, b->saved + b->dir * kAlStep[b->param]));
}
