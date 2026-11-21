// Hardline's selected-weapon handler (0x176f020) updates this customization
// table. Slots 2 and 5 match the two dynamic gadget inputs.
#define RVA_CUSTOMIZATION_EXTENT 0x2f3bea8
#define CUSTOMIZATION_WEAPONS 0x830
#define CUSTOMIZATION_WEAPON_STRIDE 0x88
#define CUSTOMIZATION_WEAPON_ASSET 0x30

static int kit_read(const void *from, void *to, size_t size)
{
    __try { memcpy(to, from, size); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static const char *kit_asset_name(const void *asset, char *out, size_t size)
{
    out[0] = 0;
    if (!asset) return out;
    static const int offsets[] = {0x10, 0x08, 0x18};
    for (int i = 0; i < 3; i++) {
        char text[128] = {0};
        char *name = NULL;
        if (!kit_read((const char *)asset + offsets[i], &name, sizeof name) || !name ||
            !kit_read((const void *)name, text, sizeof text - 1)) continue;
        size_t n = strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/.- ");
        if (n < 3 || text[n] || !strchr(text, '/')) continue;
        snprintf(out, size, "%s", text);
        break;
    }
    return out;
}

static int kit_customization(void *player, void **assets, int max)
{
    unsigned off;
    uintptr_t table[2];
    if (!player || !kit_read((void *)(H->image + RVA_CUSTOMIZATION_EXTENT), &off, sizeof off) ||
        off < 0x100 || off > 0x9000 ||
        !kit_read((const char *)player + off + CUSTOMIZATION_WEAPONS, table, sizeof table) ||
        !table[0] || table[1] < table[0]) return 0;
    size_t bytes = table[1] - table[0];
    if (bytes % CUSTOMIZATION_WEAPON_STRIDE || bytes > 16 * CUSTOMIZATION_WEAPON_STRIDE) return 0;
    int slots = (int)(bytes / CUSTOMIZATION_WEAPON_STRIDE);
    for (int k = 0; k < slots && k < max; k++)
        kit_read((const void *)(table[0] + k * CUSTOMIZATION_WEAPON_STRIDE + CUSTOMIZATION_WEAPON_ASSET),
                 &assets[k], sizeof assets[k]);
    return slots < max ? slots : max;
}

static void kits_dump(void)
{
    unsigned off = 0;
    kit_read((void *)(H->image + RVA_CUSTOMIZATION_EXTENT), &off, sizeof off);
    H->log("kits: customization extent at +%#x", off);
    for (int i = 0; i < H->botCount; i++) {
        void *player = H->bots[i];
        if (!player || player_vehicle(player, NULL) || !bot_soldier(player)) continue;
        void *assets[12] = {0};
        int count = kit_customization(player, assets, 12);
        char row[1024] = {0};
        size_t used = 0;
        for (int k = 0; k < count; k++) {
            char name[128];
            kit_asset_name(assets[k], name, sizeof name);
            if (!assets[k]) continue;
            int n = snprintf(row + used, sizeof row - used, " | %d: %s", k, name[0] ? name : "?");
            if (n < 0 || (size_t)n >= sizeof row - used) break;
            used += (size_t)n;
        }
        H->log("kits: %s, %d slots%s", (char *)player + OFF_PLAYER_NAME, count, row);
    }
}


enum { KIT_NONE, KIT_MED, KIT_AMMO, KIT_REPAIR };
static int kit_gadget_kind(const char *name)
{
    const char *base = strrchr(name, '/');
    if (!base) return KIT_NONE;
    char key[64];
    size_t len = 0;
    for (const char *p = base + 1; *p && len + 1 < sizeof key; p++)
        if (isalnum((unsigned char)*p)) key[len++] = (char)tolower((unsigned char)*p);
    key[len] = 0;
    if (key[0] == 'u') memmove(key, key + 1, len);
    if (!strcmp(key, "medkit") || !strcmp(key, "medicbag") || !strcmp(key, "firstaidpack") ||
        !strcmp(key, "firstaidkit") || !strcmp(key, "healthpack") ||
        !strcmp(key, "lemedicbag") || !strcmp(key, "crmedicbag") || !strcmp(key, "fumedicbag")) return KIT_MED;
    if (!strcmp(key, "ammobag") || !strcmp(key, "ammobox") || !strcmp(key, "ammopack") ||
        !strcmp(key, "ammocrate") || !strcmp(key, "ammosupply") ||
        !strcmp(key, "ammocancop") || !strcmp(key, "ammocancrim") || !strcmp(key, "fuammocan")) return KIT_AMMO;
    if (!strcmp(key, "repairtool") || !strcmp(key, "repairtorch")) return KIT_REPAIR;
    return KIT_NONE;
}

static const char *kit_gadget_label(int kind)
{
    return kind == KIT_AMMO ? "ammo" : kind == KIT_MED ? "first aid" : "a repair tool";
}

static void *kit_find_gadget(void *player, int kind, int *slot)
{
    static const int gadgetSlots[] = {2, 5};
    void *assets[6] = {0};
    int slots = kit_customization(player, assets, 6);
    for (int i = 0; i < 2; i++) {
        int k = gadgetSlots[i];
        char name[128];
        if (k < slots && kit_gadget_kind(kit_asset_name(assets[k], name, sizeof name)) == kind) {
            *slot = i + 1;
            return assets[k];
        }
    }
    *slot = 0;
    return NULL;
}

/* The pseudo-player spawn chooses a class first, then builds a temporary list
 * of the class's unlocks for each slot. Supply bots can favor a legal kit in
 * one dynamic slot before the game does its normal selection and spawn. */
#define RVA_ROLL_CUSTOMIZATION_CALL 0x4608e6
#define RVA_ROLL_CUSTOMIZATION      0x460980
#define RVA_BUILD_UNLOCKS_CALL      0x460b0b
#define RVA_BUILD_UNLOCKS           0x43e3b0
#define RVA_UNLOCK_AVAILABLE        0x100f590
typedef void (*RollCustomizationFn)(void *context, void *player, void *kit);
typedef void (*BuildUnlocksFn)(void *slotOwner, void *vector);
typedef char (*UnlockAvailableFn)(void *player, void *unlock);
static RollCustomizationFn o_rollCustomization;
static BuildUnlocksFn o_buildUnlocks;
static CallHook g_hookRollCustomization, g_hookBuildUnlocks;
static __declspec(thread) struct {
    void *player;
    int gameSlot, cops, allowRepair, picked;
} g_roll;
static int g_kitRollLogged[4];
static int kit_roll_enabled(void);

static void *kit_faction_item(void *unlock, int cops, int kind)
{
    // The two-faction unlock is a selector, with a table of faction ID and asset.
    char *variants = NULL;
    unsigned count = 0;
    if (!kit_read((const char *)unlock + 0x58, &variants, sizeof variants) || !variants ||
        !kit_read(variants - 4, &count, sizeof count) || count < 1 || count > 12) return NULL;
    for (unsigned k = 0; k < count; k++) {
        void *asset = NULL;
        char name[128];
        kit_read(variants + k * 0x10 + 8, &asset, sizeof asset);
        kit_asset_name(asset, name, sizeof name);
        if (kit_gadget_kind(name) != kind) continue;
        for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        const char *base = strrchr(name, '/');
        if (kind == KIT_MED && base && !strcmp(base + 1, cops ? "u_le_medicbag" : "u_cr_medicbag")) return asset;
        if (kind == KIT_AMMO && base && !strcmp(base + 1, cops ? "u_ammocancop" : "u_ammocancrim")) return asset;
    }
    return NULL;
}

static void *kit_slot_supply(void *slotOwner, int *kindOut)
{
    void *descriptor = NULL;
    void **assets = NULL;
    unsigned count = 0;
    if (!kit_read(slotOwner, &descriptor, sizeof descriptor) || !descriptor ||
        !kit_read(descriptor, &assets, sizeof assets) || !assets ||
        !kit_read((const char *)assets - 4, &count, sizeof count) || count > 128) return NULL;
    for (unsigned k = 0; k < count; k++) {
        void *asset = NULL;
        char name[128];
        kit_read(&assets[k], &asset, sizeof asset);
        int kind = kit_gadget_kind(kit_asset_name(asset, name, sizeof name));
        if (!kind || (kind == KIT_REPAIR && !g_roll.allowRepair)) continue;
        const char *base = strrchr(name, '/');
        if (base && !_strnicmp(base + 1, "fu_", 3))
            asset = kit_faction_item(asset, g_roll.cops, kind);
        if (!asset || !((UnlockAvailableFn)(H->image + RVA_UNLOCK_AVAILABLE))(g_roll.player, asset)) continue;
        *kindOut = kind;
        return asset;
    }
    return NULL;
}

static void h_buildUnlocks(void *slotOwner, void *vector)
{
    o_buildUnlocks(slotOwner, vector);
    if (!g_roll.player) return;
    int slot = g_roll.gameSlot++;
    if (g_roll.picked || (slot != 2 && slot != 5)) return;

    __try {
        void ***list = (void ***)vector;
        void **begin = list[0], **end = list[1];
        size_t bytes = (uintptr_t)end - (uintptr_t)begin;
        if (!begin || (uintptr_t)end < (uintptr_t)begin || bytes % sizeof(void *) ||
            bytes > 128 * sizeof(void *)) return;
        int kind = KIT_NONE;
        void *choice = kit_slot_supply(slotOwner, &kind);
        if (!choice) return;
        begin[0] = choice;
        list[1] = begin + 1;
        g_roll.picked = 1;
        if (g_kitRollLogged[kind]++ < 8)
            H->log("kits: %s can spawn with %s in dynamic gadget %d",
                   (char *)g_roll.player + OFF_PLAYER_NAME, kit_gadget_label(kind), slot == 2 ? 1 : 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void h_rollCustomization(void *context, void *player, void *kit)
{
    int index = -1;
    if (kit_roll_enabled()) {
        for (int i = 0; i < H->botCount; i++) {
            if (H->bots[i] == player) { index = i; break; }
        }
    }
    g_roll.player = NULL;
    if (index >= 0 && index % 5 != 4) {
        char name[128];
        kit_asset_name(kit, name, sizeof name);
        const char *base = strrchr(name, '/');
        if (base && (!_strnicmp(base + 1, "cops", 4) || !_strnicmp(base + 1, "crim", 4))) {
            g_roll.player = player;
            g_roll.cops = !_strnicmp(base + 1, "cops", 4);
            g_roll.gameSlot = 0;
            g_roll.allowRepair = index % 3 == 0;
            g_roll.picked = 0;
        }
    }
    o_rollCustomization(context, player, kit);
    g_roll.player = NULL;
}
