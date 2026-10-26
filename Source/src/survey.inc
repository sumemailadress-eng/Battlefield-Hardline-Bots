// Thugbot map survey (included by brain.inc after the breach code): as soon as a level's grid is ready (built or
// loaded), every door / pane / low-obstacle crossing the grid holds is looked at once, so bots know what each one is
// before anyone walks into it (the tester: "when a new map is loaded, vaultables and doors should be detected").
//
//   A crossing is a straight grid link the builder marked crossable (fences[] bit d): a ray at chest height along it
//   either hits a thin body - its class says door (opens with E) or glass (breaks) - or is clear (a low obstacle:
//   a vault). Doors and panes become breach spots ('o' / 'b'), saved with the learned ones; vaults are counted.
//   Runs in slices of 30000 nodes a frame.

static int g_surveyCursor = -1, g_surveyDoors, g_surveyGlass, g_surveyVaults, g_surveyOther, g_surveyNew;
static const NavGrid *g_surveyGrid;
static void survey_level_reset(void) { g_surveyCursor = -1; g_surveyGrid = NULL; }

// what breaching each kind of body did before (doors_dump: 0x2808310 / 0x2819780 open with E, 0x2819d40 breaks)
static char survey_kind(uintptr_t vt)
{
    if (vt == 0x2808310 || vt == 0x2819780) return 'o';
    if (vt == 0x2819d40) return 'b';
    for (int k = 0; k < g_panelClassN; k++) if (g_panelClass[k].vt == vt) {
        PanelClass *c = &g_panelClass[k];
        if (c->opened > c->broken && c->opened >= 2) return 'o';
        if (c->broken > c->opened && c->broken >= 2) return 'b';
    }
    return 0;
}
static void breach_save_all(void)
{
    char path[160]; breach_path(path, sizeof path);
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < g_breachN; i++) if (g_breach[i].kind != '?') fprintf(f, "%c %.2f %.2f %.2f\n", g_breach[i].kind, g_breach[i].p[0], g_breach[i].p[1], g_breach[i].p[2]);
    fclose(f);
}

static void survey_tick(void)
{
    if (!g_nav || g_navBuilding || !g_breachLoaded || !g_nav->fences) return;
    if (g_surveyGrid != g_nav) {
        g_surveyGrid = g_nav; g_surveyCursor = 0; g_surveyDoors = g_surveyGlass = g_surveyVaults = g_surveyOther = g_surveyNew = 0;
        // panes / walls written off on earlier rounds: no crossing there (the saved grid was built before they were)
        // (written-off spots at low walls are put back: explore used to write off any wall a bot was stuck at, vaultable
        // ones too - 394 on mp_hills, every vault crossing near them cut for good)
        int cut = 0, spots = 0, low = 0;
        for (int k = 0; k < g_breachN; k++) if (g_breach[k].kind == 'n') {
            if (low_wall_at(g_breach[k].p)) { g_breach[k].kind = '?'; g_breach[k].fails = 0; low++; continue; }
            spots++; cut += fence_cut_at(g_breach[k].p, "written off earlier");
        }
        if (low) { H->log("grid: %d written-off spot(s) are low walls - vaulting there again", low); breach_save_all(); }
        H->log("grid: %d crossing(s) cut at %d written-off spot(s)", cut, spots);
    }
    if (g_surveyCursor < 0) return;
    NavGrid *g = g_nav;
    int n = g->w * g->h * NAV_LAYERS, end = g_surveyCursor + 30000;
    if (end > n) end = n;
    for (int node = g_surveyCursor; node < end; node++) {
        unsigned fe = g->fences[node];
        if (!fe || !(g->flags[node] & NAV_WALKABLE)) continue;
        for (int d = 0; d < 8; d += 2) {   // straight directions; each crossing once (from its lower-numbered side)
            if (!(fe & (1u << d))) continue;
            int nb = nav_fence_step(g, node, d);
            if (nb < 0 || nb < node) continue;
            int c0 = node / NAV_LAYERS, c1 = nb / NAV_LAYERS;
            float p0[3] = { g->x0 + (c0 % g->w) * g->cell, g->height[node] + 1.2f, g->z0 + (c0 / g->w) * g->cell };
            float p1[3] = { g->x0 + (c1 % g->w) * g->cell, g->height[nb] + 1.2f, g->z0 + (c1 / g->w) * g->cell }, h[3];
            if (!ray_cast(p0, p1, h, NULL)) { g_surveyVaults++; continue; }   // clear at chest height: a low obstacle
            uintptr_t body = *(uintptr_t *)g_lastHit.rest, vt = body && !IsBadReadPtr((void *)body, 8) ? *(uintptr_t *)body - H->image : 0;
            char kind = survey_kind(vt);
            if (!kind) { g_surveyOther++; continue; }
            if (kind == 'o') g_surveyDoors++; else g_surveyGlass++;
            // Doors become spots (E first, no knife); glass and knifeable fences are left to the class rule (thousands
            // of grid crossings along Downtown's fences filled the table).
            if (0 && kind == 'o' && breach_find(h, 1.2f) < 0 && g_breachN < MAX_BREACH) {   // OFF: body classes don't tell doors from walls (0x2808310: 34 of 53 tries stayed shut) - bots pressed E at walls
                Breach *e = &g_breach[g_breachN++];
                memset(e, 0, sizeof *e); memcpy(e->p, h, 12); e->kind = kind;
                g_surveyNew++;
            }
        }
    }
    g_surveyCursor = end;
    if (end == n) {
        g_surveyCursor = -1;
        if (g_surveyNew) breach_save_all();
        H->log("survey: %s - %d door(s), %d glass / fence crossing(s), %d vault spot(s), %d other crossing(s); %d new door spot(s) (%d door / pane spots known now)",
               g_levelKey, g_surveyDoors, g_surveyGlass, g_surveyVaults, g_surveyOther, g_surveyNew, g_breachN);
    }
}
