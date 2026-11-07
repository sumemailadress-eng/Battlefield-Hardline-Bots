// Play area (included by brain.inc): the combat areas of this map and mode, from the game's own level files.
// Thugbot_learned\<map>.playarea (re\scripts\fb_playarea.py): "area <mode layer> <n> x z x z ...", one line per polygon. A mode has one or
// more polygons (one per team, an outer and an inner); their union is where a player may be. A route with a point more than 12 m
// outside it is dropped when it is loaded. No file, or no polygons for this mode: nothing is dropped.
#define PA_MAX_POLYS 32
#define PA_MAX_PTS 128
static struct { int n; float p[PA_MAX_PTS][2]; } g_pa[PA_MAX_POLYS];
static int g_paN; static char g_paKey[96];

// The layer names the game uses for a mode: same name, or these (Hardline's own Turf War layers are often still called conquest)
static int pa_mode_match(const char *layer, const char *mode)
{
    static const char *const kAlias[][4] = {
        { "turfwarlarge", "turfwarlarge", "conquestlarge", "" }, { "turfwarsmall", "turfwarsmall", "conquestsmall", "" },
        { "teamdeathmatch", "teamdeathmatch", "deathmatch", "tdm" }, { "hostage", "hostage", "rescue", "" }, { "rescue", "rescue", "hostage", "" },
        { "hotwire", "hotwire", "hotwireredux", "" }, { "capturetheflag", "ctf", "ctb", "capturetheflag" }, { "crosshair", "hit", "crosshair", "" } };
    char a[48], b[48]; int n = 0;
    for (const char *c = layer; *c && n < 47; c++) if (!isdigit((unsigned char)*c)) a[n++] = (char)tolower((unsigned char)*c);
    a[n] = 0; n = 0;
    for (const char *c = mode; *c && n < 47; c++) if (!isdigit((unsigned char)*c)) b[n++] = (char)tolower((unsigned char)*c);
    b[n] = 0;
    for (size_t k = 0; k < sizeof kAlias / sizeof kAlias[0]; k++)
        if (!strcmp(b, kAlias[k][0])) { for (int j = 1; j < 4; j++) if (kAlias[k][j][0] && !strcmp(a, kAlias[k][j])) return 1; return 0; }
    return !strcmp(a, b) || (a[0] && !strncmp(a, b, strlen(b)));
}
static void playarea_load(void)
{
    if (!g_levelKey[0] || !strcmp(g_paKey, g_levelKey)) return;
    snprintf(g_paKey, sizeof g_paKey, "%s", g_levelKey);
    g_paN = 0;
    const char *u = strrchr(g_levelKey, '_');
    if (!u) return;
    char path[MAX_PATH]; map_path(path, sizeof path, "Thugbot_learned", ".playarea");
    FILE *f = fopen(path, "r");
    if (!f) { H->log("playarea: no %s - routes are not checked", path); return; }
    static char line[16384];
    while (g_paN < PA_MAX_POLYS && fgets(line, sizeof line, f)) {
        char layer[64]; int n = 0, used = 0;
        if (sscanf(line, "area %63s %d%n", layer, &n, &used) != 2 || n < 3 || n > PA_MAX_PTS || !pa_mode_match(layer, u + 1)) continue;
        char *c = line + used; int q = 0;
        for (; q < n; q++) { int w; if (sscanf(c, "%f %f%n", &g_pa[g_paN].p[q][0], &g_pa[g_paN].p[q][1], &w) != 2) break; c += w; }
        if (q == n) g_pa[g_paN++].n = n;
    }
    fclose(f);
    H->log("playarea: %d polygon(s) for %s", g_paN, g_levelKey);
}
static int playarea_inside(float x, float z, float margin)
{
    playarea_load();
    if (!g_paN) return 1;
    for (int k = 0; k < g_paN; k++) {
        int c = 0, n = g_pa[k].n;
        for (int a = 0, b = n - 1; a < n; b = a++) {
            float x1 = g_pa[k].p[a][0], z1 = g_pa[k].p[a][1], x2 = g_pa[k].p[b][0], z2 = g_pa[k].p[b][1];
            if ((z1 > z) != (z2 > z) && x < (x2 - x1) * (z - z1) / (z2 - z1) + x1) c = !c;
            float ex = x2 - x1, ez = z2 - z1, l2 = ex * ex + ez * ez, s = l2 > 0.01f ? ((x - x1) * ex + (z - z1) * ez) / l2 : 0;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            float dx = x1 + ex * s - x, dz = z1 + ez * s - z;
            if (dx * dx + dz * dz < margin * margin) return 1;
        }
        if (c) return 1;
    }
    return 0;
}
static int playarea_route_ok(const float (*pts)[3], int n)
{
    for (int k = 0; k < n; k++) if (!playarea_inside(pts[k][0], pts[k][2], 12)) return 0;
    return 1;
}
// Most of a map's routes outside: the polygons are the wrong ones for it, keep the routes
static int playarea_may_drop(int drop, int total, const char *what, const char *path)
{
    if (!drop) return 0;
    if (total >= 4 && drop * 10 > total * 6) { H->log("playarea: %d of %d %s outside it - that looks wrong, keeping them", drop, total, what); return 0; }
    if (path && path[0]) { char bak[MAX_PATH + 20]; snprintf(bak, sizeof bak, "%s.bak-playarea", path); CopyFileA(path, bak, TRUE); }
    H->log("playarea: dropped %d of %d %s that leave the play area", drop, total, what);
    return 1;
}
