/* Write a snapshot only while the local route editor renews its viewing lease. */
static void live_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (int i = 0; s && s[i] && i < 100; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') fputc('\\', f);
        fputc(c >= 32 && c < 127 ? c : '?', f);
    }
    fputc('"', f);
}

static int live_valid_point(const float *p)
{
    return p && isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

static void live_point(FILE *f, const float *p)
{
    if (live_valid_point(p)) fprintf(f, "[%.1f,%.1f,%.1f]", p[0], p[1], p[2]);
    else fputs("null", f);
}

static void live_snapshot(void)
{
    static ULONGLONG next;
    ULONGLONG t = now_ms();
    if (t < next) return;
    next = t + 1000;
    WIN32_FILE_ATTRIBUTE_DATA lease;
    if (!g_levelKey[0] || !GetFileAttributesExA("Thugbot_live.watch", GetFileExInfoStandard, &lease)) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER at, current;
    at.LowPart = lease.ftLastWriteTime.dwLowDateTime; at.HighPart = lease.ftLastWriteTime.dwHighDateTime;
    current.LowPart = ft.dwLowDateTime; current.HighPart = ft.dwHighDateTime;
    if (current.QuadPart < at.QuadPart || current.QuadPart - at.QuadPart > 150000000ULL) return;

    char tmp[48], file[48];
    snprintf(tmp, sizeof tmp, "Thugbot_live.%d.tmp", H->gamePort);
    snprintf(file, sizeof file, "Thugbot_live.%d.json", H->gamePort);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    char key[64]; map_key(key, sizeof key);
    fputs("{\"map\":", f); live_string(f, key);
    fputs(",\"round\":", f); live_string(f, g_levelKey);
    fputs(",\"flags\":[", f);
    int count = 0;
    for (int k = 0; k < g_cpCount; k++) {
        if (!g_fi[k].ok || !live_valid_point(g_fi[k].pos)) continue;
        fprintf(f, "%s{\"id\":%d,\"team\":%d,\"p\":", count++ ? "," : "", k, g_fi[k].owner);
        live_point(f, g_fi[k].pos); fputc('}', f);
    }
    fputs("],\"vehicles\":[", f); count = 0;
    for (int k = 0; k < g_knownVehN; k++) {
        const KnownVeh *kv = &g_knownVeh[k];
        if (!kv->seenAt || t - kv->seenAt > 3000 || !live_valid_point(kv->pos)) continue;
        fprintf(f, "%s{\"occupied\":%s,\"name\":", count++ ? "," : "", kv->occupied ? "true" : "false");
        live_string(f, kv->name); fputs(",\"p\":", f); live_point(f, kv->pos); fputc('}', f);
    }
    fputs("],\"bots\":[", f); count = 0;
    for (int i = 0; i < H->botCount; i++) {
        const Bot *b = &g_bot[i];
        if (!b->alive || !b->player || b->player != H->bots[i]) continue;
        int seat = -1;
        void *vehicle = player_in_vehicle(b->player);
        if (vehicle) player_vehicle(b->player, &seat);  // Soldier-controlled passengers have no reported seat index.
        void *soldier = vehicle ? NULL : bot_soldier(b->player);
        const float *p = vehicle ? veh_pos(vehicle) : soldier ? soldier_pos(soldier) : NULL;
        if (!live_valid_point(p)) continue;
        fprintf(f, "%s{\"id\":%d,\"team\":%d,\"name\":", count++ ? "," : "", i, FIELD(b->player, OFF_PLAYER_TEAM, int));
        live_string(f, (char *)b->player + OFF_PLAYER_NAME);
        fputs(",\"state\":", f); live_string(f, b->why);
        fputs(",\"vehicle\":", f); live_string(f, vehicle ? vehicle_name(vehicle) : NULL);
        fprintf(f, ",\"seat\":%d,\"flag\":%d,\"p\":", seat, b->targetFlag); live_point(f, p);
        fputs(",\"path\":[", f);
        int points = 0;
        if ((!vehicle || seat == 0) && b->wp >= 0 && b->npts <= BRAIN_PATH_MAX) {
            for (int k = b->wp; k < b->npts && k < b->wp + 32; k++) {
                const float *pt = b->path + k * 3;
                if (!live_valid_point(pt)) continue;
                if (points++) fputc(',', f);
                live_point(f, pt);
            }
        }
        fputs("]}", f);
    }
    fputs("]}", f);
    int ok = !ferror(f);
    if (fclose(f) == 0 && ok) MoveFileExA(tmp, file, MOVEFILE_REPLACE_EXISTING);
}
