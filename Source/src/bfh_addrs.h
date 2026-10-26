// Engine addresses for bfh_server_Final.exe (HavanaXP4, built 2016-03-02, 64-bit). RVAs from the image base
// (0x140000000 when not relocated); found with Ghidra (re\dump). The counterpart of AltBotMod's ADDR_/OFF_ table.
#pragma once

// ---- PseudoPlayerManager (BF\BF\Server\PseudoPlayer\PseudoPlayerManager.cpp) ----
#define RVA_PPM_UPDATE          0x46ac70  // PseudoPlayerManager::update(this, int, float dt) - every server frame
#define RVA_PPM_UPDATE_CALL     0x18253b  // the E8 call to it in the server's frame update (rcx = owner+0x98)
#define RVA_PP_INPUT            0x46d090  // per pseudo-player input update(this, PseudoPlayer*, float dt) - "UpdateInput"
#define RVA_PP_INPUT_CALL       0x46e7bb  // the E8 call to it in the per-player update (RVA 0x46e5a0)

// The input update builds an EntryInputState on its stack, then copies it into the pseudo-player (+0x10). Hooking that
// copy lets us press any input action (not just the 20 the pseudo-player table maps) and set analog levels like pitch.
#define RVA_PP_INPUT_COPY_CALL  0x46d5fd  // E8 call: copy(rcx = pp + 0x10, rdx = the stack state)
#define RVA_INPUT_COPY          0x1012280
#define RVA_INPUT_TRACK         0x100ab30 // (state, hash, bool) - marks an action as driven by this state
#define RVA_INPUT_SET_DIGITAL   0x1012530 // (state, hash, bool pressed)
#define RVA_INPUT_SET_ANALOG    0x1012a10 // (state, hash, float level)

// Input action hashes: djb2-xor (h = h * 33 ^ c, seed 5381) of the EIA enum name, e.g. "EIAFire".
// Pseudo-player action ids (OFF_PP_ACTIONS) map to: 1 Sprint, 2 Jump, 4 Prone, 5/6 Fire, 7 Zoom, 8 Reload,
// 9 SwitchPrimaryWeapon, 10 SwitchPrimaryInventory, 11 MeleeAttack, 12 ThrowGrenade, 13 ChangeEntry, 15 ToggleCamera,
// 16 CycleFireMode, 19 FireCountermeasure, 20 ThrowDistraction (3 is looked up at runtime; 14, 17, 18 unidentified).
#define EIA_THROTTLE            0xe5a74ae0
#define EIA_BRAKE               0xd2620df7
#define EIA_YAW                 0x8d79a3e7
#define EIA_PITCH               0xd30e818e
#define EIA_VEHICLE_POSE        0x9722a0cb  // EIAVehiclePoseSwitch: lean out of a car window (passengers) - guessed for the tester's "hold Z"
#define EIA_PARACHUTE           0xc8a5f445  // djb2-xor "EIAParachute" (ConceptParachute): jump alone didn't open it
#define EIA_ROLL                0x3cb631f5  // djb2-xor "EIARoll" (not seen live yet)
#define EIA_STRAFE              0x403a2bdf
#define EIA_FIRE                0x3caf8530
#define EIA_SPRINT              0x3ff1a9ca
#define EIA_JUMP                0x3ca9660a
#define EIA_INTERACT            0x5dcb783a  // E: doors, pick-ups
#define EIA_MELEE               0x251695e4  // knife / melee: breaks glass panels
#define EIA_RELOAD              0x4370acd9
#define EIA_ZOOM                0x3cb210df
#define EIA_CHANGE_VEHICLE      0x5bdca916  // get in / out of a vehicle
#define EIA_CROUCH              0x4d40eba6  // EIACrouchOnHold: crouched while held (EIACrouch 0x1c6f5f08 toggles)
#define EIA_SWITCH_GL           0x87f58495  // EIASwitchToGrenadeLauncher
#define EIA_SWITCH_GADGET1      0xdd475fb1  // EIASwitchToDynamicGadget1
#define EIA_SWITCH_GADGET2      0xdd475fb2  // EIASwitchToDynamicGadget2
#define EIA_SWITCH_PRIMARY      0x7b48642f  // EIASwitchToPrimaryWeapon
#define EIA_SWITCH_STATIC       0xed46260d  // EIASwitchToStaticGadget (the class item: first aid / ammo kits - not yet confirmed live)

// ---- Damage: BFServerSoldierHealthComponent::onDamage(this, DamageResult *ret, DamageInfo *info) (virtual) ----
// With client-side hit detection on (a settings byte, +0x3b), a hit whose DamageInfo flags lack bit 0 (confirmed by
// the shooter's client) is dropped: blood shows, no health is lost. Bots have no client, so their bullets never count
// unless we set that bit for them.
#define RVA_SOLDIER_ON_DAMAGE    0x4570e0
#define RVA_SOLDIER_ON_DAMAGE_VT 0x23b2430 // the one vtable slot that holds it
#define OFF_DMG_GIVER_INFO       0xb0      // DamageGiverInfo*: +0 -> weak ref -> (ServerPlayer + 8)
#define OFF_DMG_FLAGS            0xbc      // bit 0: client-confirmed, bit 1: skips the check

#define OFF_PPM_PLAYERS_BEGIN   0x18      // PseudoPlayer** vector [begin, end)
#define OFF_PPM_PLAYERS_END     0x20

// PseudoPlayer: +0 ServerPlayer*, +0x10 its persistent EntryInputState, then the random "manual" input it holds
// between re-rolls. The input update turns these into input levels (forward/back, strafe, yaw) and buttons.
#define OFF_PP_PLAYER           0x0
#define OFF_PP_REROLL_TIMER     0x620     // float seconds until it rolls new random input (we keep it high)
#define OFF_PP_ACTION_COUNT     0x624     // uint, 0..5 digital actions pressed this period
#define OFF_PP_ACTIONS          0x628     // uint[5], action ids 1..20 (the switch in RVA_PP_INPUT)
#define OFF_PP_FORWARD          0x63c     // float -1..1 (positive MoveForward, negative MoveBackward)
#define OFF_PP_TURN             0x640     // float, yaw rate (input hash 0x8d79a3e7)
#define OFF_PP_STRAFE           0x644     // float (input hash 0x403a2bdf; live test: 1.0 strafes at ~2.7 m/s)

// ---- Physics raycast ----
// GameWorld::physicsRayQuery(this, const char *ident, Vec4 *from, Vec4 *to, RayCastHit *hit, int flags, void *exclude)
// -> bool hit. Virtual: only referenced from vtables (one at 0x1427486f8). RayCastHit: +0 Vec4 position, +0x10 Vec4
// normal, +0x34 float fraction (0..1 along the ray).
#define RVA_PHYSICS_RAY_QUERY   0xfef160
#define RVA_GAME_WORLD          0x3590a58 // the server level's GameWorld* (cleared at level teardown, RVA 0x106bde0;
                                          // 0x35046b8 is the same class's other singleton, null on the server)
#define OFF_WORLD_RAYCASTER     0x30      // the IPhysicsRayCaster sub-object (vtable 0x1427486f8, slot 0 = ray query)
// (Not realm context + 0x48: passing that as the caster crashed the server - 2026-10-23.)

// ---- ServerCapturePointEntity (BF\BF\Server\Spawn\ServerCapturePointEntity.cpp) ----
#define RVA_CP_UPDATE           0x46b160  // ServerCapturePointEntity::update(this, UpdateInfo*) - per flag, throttled
#define RVA_CP_UPDATE_CALL      0x45eff3  // the E8 call to it in the capture point update loop (RVA 0x45ef40)
#define OFF_CP_DATA             0x30      // CapturePointEntityData*
#define OFF_CP_OWNER_TEAM       0x118     // int: 0 neutral, 1 / 2 (HQs show their team here from the start)
#define OFF_CP_TEAM_11C         0x11c     // int: equal to the owner on the HQs at round start (meaning TBD)
#define OFF_CP_TEAM_B           0x120
#define OFF_CP_POSITION         0x140     // Vec3 - centre of the capture area's bounding sphere (takeable flags only)
#define OFF_CP_RADIUS           0x14c     // float - that sphere's radius (MP_Bank A: 28 m); the real area is a volume inside
#define OFF_CP_COUNT_A          0x178     // int pair compared with the players-in-range counts each update
#define OFF_CP_COUNT_B          0x17c

// ---- ServerMapSequencer (BF\BF\Server\MapSequence\ServerMapSequencer.cpp) ----
#define RVA_SEQ_LEVEL_LOADED      0x398e70  // ServerMapSequencer::onLevelLoaded(this) - logs "After Level(..) GameMode(..)"
#define RVA_SEQ_LEVEL_LOADED_CALL 0x399a8a  // the E8 call to it in the sequencer's message handler (RVA 0x3998c0)
#define OFF_SEQ_LEVEL             0x38      // string: "havana/levels/mp/mp_bank" (read as char* - checked before use)
#define OFF_SEQ_MODE              0x58      // string: "TurfWarLarge0", "TeamDeathMatch0", ...

// ---- every player: ServerGameContext -> ServerPlayerManager -> vector of ServerPlayer* (RVA 0x4513e0 walks it) ----
#define RVA_SERVER_GAME_CONTEXT 0x3590a50
#define OFF_GC_PLAYER_MANAGER   0x68
#define OFF_PM_PLAYERS_BEGIN    0x270
#define OFF_PM_PLAYERS_END      0x278

// ---- ServerPlayer ----
#define OFF_PLAYER_NAME         0x40      // inline char[] (what RCON listPlayers shows)
#define OFF_PLAYER_SPECTATOR    0xce9     // byte
#define OFF_PLAYER_TEAM         0xcec     // int 1 / 2 (matched against RCON listPlayers for all 16 bots)
#define OFF_PLAYER_ENTRY_INDEX  0x1680    // int: seat (entry) index in the attached controllable
#define OFF_PLAYER_SOLDIER      0x1688    // ServerSoldierEntity* (0 while dead)

// ---- ServerSoldierEntity (found live with "probe": walking a bot forward moves these) ----
#define OFF_SOLDIER_POSITION    0x640     // Vec3 x, y (up), z - world position (also mirrored at +0x7d0)
#define OFF_SOLDIER_VELOCITY    0x660     // Vec3, m/s (about 4 walking, 2.7 strafing)
#define OFF_SOLDIER_YAW         0x58c     // float radians, unwrapped: walking direction = atan2(vx, vz) = yaw (mod 2 pi);
                                          // positive turn input raises it (about 1.5 rad/s at 1.0)
#define OFF_SOLDIER_ROT_FWD     0x550     // Vec3 row of the soldier's rotation: it points opposite the walking direction
