// 2s2h/Network/HiveShock/HiveShock.cpp
//
// HiveShock: servidor TCP nativo, compilado dentro de 2ship.exe.
// Escucha comandos JSON (uno por línea) en 0.0.0.0:<Port> (todas las interfaces;
// 127.0.0.1 sigue válido en la misma PC) y aplica efectos en el hilo principal.
//
#include "HiveShockSocket.h"
#include "HiveShockAdmission.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/NameTag/NameTag.h"
#include "2s2h/SaveManager/SaveManager.h"
#include "2s2h/ShipUtils.h"
#include "2s2h/Enhancements/Cheats/PlayerRescue.h"
#include "2s2h/Network/HiveShock/HiveShock.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/ConsoleWindow.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64actor.h"
#include "z64game_over.h"
#include "z64horse.h"
#include "alignment.h"
#include "z64save.h"
void func_80833B18(PlayState* play, Player* thisx, s32 arg2, f32 speed, f32 velocityY, s16 arg5,
                   s32 invincibilityTimer);
void Interface_StartMoonCrash(PlayState* play);
void Sram_SaveEndOfCycle(PlayState* play);
void func_8014546C(SramContext* sramCtx);
void Interface_NewDay(PlayState* play, s32 day);
void Environment_NewDay(EnvironmentContext* envCtx);
void TransitionFade_SetColor(void* thisx, u32 color);
Actor* Actor_Delete(ActorContext* actorCtx, Actor* actor, PlayState* play);
}

extern void UpdateGameTime(u16 gameTime);

#define CVAR_NAME "gCheats.HiveShockEnabled"
#define CVAR_NAME_LEGACY "gCheats.TikTokBridgeEnabled"

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

using namespace HiveShockNet;

// libultraship declara CVarExists pero no la define, y este repo no modifica el submódulo.
bool HiveShockCVarExists(const char* name) {
    return Ship::Context::GetRawInstance()->GetConsoleVariables()->Get(name) != nullptr;
}

constexpr int kDefaultPort = 43000;
constexpr int kPollIntervalMs = 200;
#define CVAR_PORT_NAME "gRemote.HiveShock.Port"
#define CVAR_TOKEN_NAME "gRemote.HiveShock.Token"

bool IsHiveShockEnabled() {
    if (HiveShockCVarExists(CVAR_NAME)) {
        return CVarGetInteger(CVAR_NAME, 1) != 0;
    }
    return CVarGetInteger(CVAR_NAME_LEGACY, 1) != 0;
}

#define CVAR IsHiveShockEnabled()

void MigrateHiveShockCVar() {
    if (HiveShockCVarExists(CVAR_NAME)) {
        return;
    }
    if (!HiveShockCVarExists(CVAR_NAME_LEGACY)) {
        return;
    }
    CVarSetInteger(CVAR_NAME, CVarGetInteger(CVAR_NAME_LEGACY, 1));
    CVarSave();
}

std::atomic<uint16_t> gPort{ kDefaultPort };

uint16_t ConfiguredPort() {
    return static_cast<uint16_t>(std::clamp(CVarGetInteger(CVAR_PORT_NAME, kDefaultPort), 1025, 65534));
}

// La IP LAN se consulta como mucho cada pocos segundos: el menú pide el endpoint en cada frame y resolverla abre un
// socket UDP.
std::string FormatListenEndpoint() {
    static std::mutex sMutex;
    static std::string sLan;
    static Clock::time_point sNextRefresh{};
    std::string lan;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const auto now = Clock::now();
        if (now >= sNextRefresh) {
            sLan = ResolvePreferredLanIpv4();
            sNextRefresh = now + std::chrono::seconds(5);
        }
        lan = sLan;
    }
    if (lan.empty()) {
        lan = "127.0.0.1";
    }
    return lan + ":" + std::to_string(gPort.load());
}
constexpr uint16_t kEventPort = 43002;
constexpr size_t kMaxLineBytes = 8192;
// Los comandos y spawns nunca se descartan para hacer sitio: un espectador los pagó. Estos topes solo acotan la
// memoria y, al superarlos, se rechaza la entrada NUEVA (con log), nunca una anterior.
constexpr size_t kMaxQueueSize = 1024;
constexpr size_t kMaxPendingSpawns = 1000;
constexpr size_t kMaxNameTagChars = 18;
constexpr f32 kEnemySpawnDistance = 80.0f;
constexpr s32 kActorHardCap = 255 - 24; // límite duro del motor; dejar holgura para el resto de la escena
constexpr s32 kMinFreeObjectSlots = 3;  // no agotar el banco de objects de la escena
constexpr u32 kCuccoArmyFrameGap = 2; // ticks entre cada cucco del army
// El juego corre a 20 ticks/s. Los spawns se liberan como mucho uno cada ~0.4 s (los ligeros, cada 0.1 s) para que un
// relleno tras varias muertes no llegue de golpe.
constexpr u32 kSpawnGapFrames = 8;
constexpr u32 kSpawnGapFramesLight = 2;
// Enemigos con este peso o más son "élite": pasan por delante de los ordinarios en la cola (Iron Knuckle, Garo,
// Garo Master, Eyegore, Dinolfos, Floormaster). Se desactiva con gRemote.HiveShock.ElitePriority.
constexpr u8 kEliteWeight = 3;
// Ventaja de élite: puntos reservados a los élite sobre MaxLoad para que los primeros entren de inmediato
// (gRemote.HiveShock.EliteReserve, 0 la desactiva). Es solo una ventaja, no un techo; ver HiveShockAdmission.h.
constexpr s32 kDefaultEliteReserve = 10;
constexpr s32 kMaxEliteReserve = 40;
constexpr s32 kOrdinaryFloor = 0;
constexpr u32 kEliteWaitLogTicks = 100;
// Cadencia de reintento de un spawn retenido (no permitido ahora) o que no pudo colocar un actor.
constexpr u32 kHeldRecheckFrames = 10;
constexpr u32 kFailedRetryFrames = 20;
// Tras un cambio de sala, los enemigos inyectados que desaparecen aún vivos los quitó el cambio de sala (no murieron),
// así que se reencolan.
constexpr u32 kRoomGraceTicks = 40;
// Un enemigo inyectado así de lejos de Link durante este tiempo se considera atascado y se mueve junto a él.
constexpr f32 kStuckDistance = 700.0f;
constexpr u32 kStuckTicks = 20 * 20;
constexpr u32 kStuckCheckTicks = 20;
// Los spawns pendientes se escriben a disco (con límite de frecuencia) para que un crash o reinicio no pierda lo
// que pagaron los espectadores.
constexpr u32 kBacklogSaveTicks = 100;
constexpr s64 kBacklogMaxAgeSeconds = 12 * 60 * 60;
constexpr u32 kQueueReportTicks = 10;
constexpr const char* kBacklogFileName = "hiveshock_pending.json";
constexpr f32 kDefaultSlowMult = 0.5f;
constexpr s32 kDefaultMaxAlive = 32;
constexpr s32 kDefaultMaxSceneEnemies = 40;
// Presupuesto blando: suma de SpawnDef::weight de los enemigos inyectados vivos (gRemote.HiveShock.MaxLoad).
constexpr s32 kDefaultMaxLoad = 14;
#define CVAR_HS(name) "gRemote.HiveShock." name
constexpr s32 kMaxItemPickupsPerCommand = 6; // rupee_rain: actores visuales por comando (ver ApplyRupeeRain)
constexpr float kCapacitySnapshotIntervalSeconds = 2.0f; // cadencia de eventos "capacity" hacia HiveShock
/** Espacio entre efectos de la cola (reloj real; no usar gameplayFrames — se resetea al cargar). */
constexpr float kSecondsBetweenQueuedCommands = 0.18f;
/** Bit en params: enemigos Bridge salen aggro a Link (lo leen y limpia su Init). */
constexpr s32 kBridgeForceAggroParam = 0x4000;

// CVar anterior (cantidad de entidades vivas); se lee como alternativa si MaxLoad aún no existe.
#define CVAR_MAX_ENTITIES_NAME "gCheats.HiveShockMaxEntities"

// Applied: hecho. Deferred: ahora no es posible para nadie (cutscene, sala cargando); parar y reintentar luego.
// LaneFull: este carril (élite u ordinario) no tiene sitio; se deja de mirar ESE carril (su primer enemigo conserva el
// turno) pero el otro sigue. Held: no es posible solo para ESTA entrada; se conserva y pasan las demás. Skipped:
// falló esta vez (p. ej. no se pudo colocar el actor), reintentar luego. Rejected: comando desconocido.
enum class ApplyResult { Applied, Deferred, LaneFull, Held, Skipped, Rejected };

std::mutex gQueueMutex;
std::deque<json> gCommandQueue;
std::atomic<bool> gServerRun{ false };
std::atomic<bool> gListening{ false };
// Une el hilo del servidor al destruirse: un std::thread joinable destruido durante el teardown estático llama a
// std::terminate() y abortaba el juego al salir con HiveShock activo.
struct ServerThreadHolder {
    std::thread thread;
    ~ServerThreadHolder() {
        gServerRun = false;
        if (thread.joinable()) {
            thread.join();
        }
    }
};
ServerThreadHolder gServerThreadHolder;
std::atomic<int> gDeathCount{ 0 };

// Contadores de enemigos que lee la UI (ver HiveShock::Stats). Los escribe el hilo del juego.
std::atomic<int> gStatTotal{ 0 };
std::atomic<int> gStatSpawned{ 0 };
std::atomic<int> gStatDefeated{ 0 };
std::atomic<int> gStatAlive{ 0 };
std::atomic<int> gStatWaiting{ 0 };
std::atomic<int> gStatLoad{ 0 };
std::atomic<int> gStatEliteBank{ 0 };
std::atomic<int> gStatEliteBankMax{ 0 };
std::atomic<int> gStatEliteWaiting{ 0 };
bool gDeathCountedThisCycle = false;

s32 gSavedTimeSpeed = 0;
bool gTimeIsFrozen = false;
bool gTimeFreezeHasDeadline = false;
Clock::time_point gTimeFrozenUntil{};
bool gInvertControls = false;
Clock::time_point gInvertUntil{};
bool gPendingMoonCrash = false;

enum class BridgeBuff : u8 {
    Speed = 0,
    Invincible,
    InfiniteMagic,
    DamageUp,
    Count,
};

struct BuffState {
    bool active = false;
    Clock::time_point until{};
    f32 strength = 1.0f;
};

BuffState gBuffs[static_cast<size_t>(BridgeBuff::Count)]{};

float GetNumber(const json& cmd, float defaultValue);
float GetDuration(const json& cmd);
std::string GetStringField(const json& cmd, const char* key);
bool IsHealPriorityAction(const std::string& action);

constexpr f32 kDefaultSpeedMult = 2.0f;
constexpr f32 kDefaultDamageMult = 3.0f;
constexpr float kDefaultBuffSeconds = 20.0f;

// Invencible = oro, speed = cian, magia = violeta, damage = naranja.
constexpr Color_RGBA8 kBuffColorSpeed = { 40, 220, 255, 255 };
constexpr Color_RGBA8 kBuffColorInvincible = { 255, 220, 40, 255 };
constexpr Color_RGBA8 kBuffColorMagic = { 190, 70, 255, 255 };
constexpr Color_RGBA8 kBuffColorDamage = { 255, 70, 25, 255 };

const char* BuffName(BridgeBuff buff) {
    switch (buff) {
        case BridgeBuff::Speed:
            return "speed";
        case BridgeBuff::Invincible:
            return "invincible";
        case BridgeBuff::InfiniteMagic:
            return "infinite_magic";
        case BridgeBuff::DamageUp:
            return "damage_up";
        default:
            return "buff";
    }
}

bool IsBuffActive(BridgeBuff buff) {
    const BuffState& state = gBuffs[static_cast<size_t>(buff)];
    return state.active && Clock::now() < state.until;
}

f32 GetBuffStrength(BridgeBuff buff, f32 fallback) {
    if (!IsBuffActive(buff)) {
        return fallback;
    }
    return gBuffs[static_cast<size_t>(buff)].strength;
}

void ActivateBuff(BridgeBuff buff, float durationSeconds, f32 strength) {
    durationSeconds = std::clamp(durationSeconds, 1.0f, 120.0f);
    BuffState& state = gBuffs[static_cast<size_t>(buff)];
    state.active = true;
    state.until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(durationSeconds));
    state.strength = strength;
    if (gPlayState != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        if (player != nullptr) {
            Player_PlaySfx(player, NA_SE_SY_TRANSFORM_MASK_FLASH);
        }
    }
    SPDLOG_INFO("HiveShock: buff '{}' {:.1f}s (strength={:.2f})", BuffName(buff), durationSeconds, strength);
}

void ClearExpiredBuffs() {
    const auto now = Clock::now();
    for (size_t i = 0; i < static_cast<size_t>(BridgeBuff::Count); i++) {
        if (gBuffs[i].active && now >= gBuffs[i].until) {
            gBuffs[i].active = false;
            SPDLOG_INFO("HiveShock: buff '{}' ended", BuffName(static_cast<BridgeBuff>(i)));
        }
    }
}

bool AnyVisualBuffActive() {
    return IsBuffActive(BridgeBuff::Speed) || IsBuffActive(BridgeBuff::Invincible) ||
           IsBuffActive(BridgeBuff::InfiniteMagic) || IsBuffActive(BridgeBuff::DamageUp);
}

bool GetCycledBuffColor(Color_RGBA8* out) {
    Color_RGBA8 colors[static_cast<size_t>(BridgeBuff::Count)];
    size_t count = 0;
    if (IsBuffActive(BridgeBuff::Invincible)) {
        colors[count++] = kBuffColorInvincible;
    }
    if (IsBuffActive(BridgeBuff::Speed)) {
        colors[count++] = kBuffColorSpeed;
    }
    if (IsBuffActive(BridgeBuff::DamageUp)) {
        colors[count++] = kBuffColorDamage;
    }
    if (IsBuffActive(BridgeBuff::InfiniteMagic)) {
        colors[count++] = kBuffColorMagic;
    }
    if (count == 0 || out == nullptr) {
        return false;
    }
    u32 frame = (gPlayState != nullptr) ? gPlayState->gameplayFrames : 0;
    *out = colors[(frame / 8) % count];
    return true;
}

bool ShouldBlockHealthLoss() {
    return IsBuffActive(BridgeBuff::Invincible);
}

bool OverrideColorFilter(Actor* actor, Color_RGBA8* color) {
    if (actor == nullptr || actor->id != ACTOR_PLAYER || color == nullptr) {
        return false;
    }
    if (COLORFILTER_GET_COLORFLAG(actor->colorFilterParams) == COLORFILTER_COLORFLAG_RED) {
        return false;
    }
    return GetCycledBuffColor(color);
}

void TickBuffVisuals() {
    if (gPlayState == nullptr || !AnyVisualBuffActive()) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }

    // Parpadeo: 4 frames tinte / 4 frames normal. No pisar el flash rojo de un golpe.
    if (player->actor.colorFilterTimer != 0 &&
        COLORFILTER_GET_COLORFLAG(player->actor.colorFilterParams) == COLORFILTER_COLORFLAG_RED) {
        return;
    }

    u32 frame = gPlayState->gameplayFrames;
    if (((frame / 4) % 2) == 0) {
        Actor_SetColorFilter(&player->actor, COLORFILTER_COLORFLAG_GRAY, COLORFILTER_INTENSITY_FLAG | 200,
                             COLORFILTER_BUFFLAG_OPA, 8);
    }
}

void TickInfiniteMagic() {
    if (!IsBuffActive(BridgeBuff::InfiniteMagic) || gSaveContext.magicCapacity <= 0) {
        return;
    }
    gSaveContext.save.saveInfo.playerData.magic = gSaveContext.magicCapacity;
}

float BuffDurationFromCmd(const json& cmd) {
    float duration = GetDuration(cmd);
    if (duration <= 0.0f) {
        duration = kDefaultBuffSeconds;
    }
    return duration;
}

void ApplySpeedBuff(const json& cmd) {
    f32 mult = std::clamp(GetNumber(cmd, kDefaultSpeedMult), 1.25f, 4.0f);
    ActivateBuff(BridgeBuff::Speed, BuffDurationFromCmd(cmd), mult);
}

void ApplyInvincibleBuff(const json& cmd) {
    ActivateBuff(BridgeBuff::Invincible, BuffDurationFromCmd(cmd), 1.0f);
}

void ApplyInfiniteMagicBuff(const json& cmd) {
    ActivateBuff(BridgeBuff::InfiniteMagic, BuffDurationFromCmd(cmd), 1.0f);
    TickInfiniteMagic();
}

void ApplyDamageUpBuff(const json& cmd) {
    f32 mult = std::clamp(GetNumber(cmd, kDefaultDamageMult), 1.5f, 8.0f);
    ActivateBuff(BridgeBuff::DamageUp, BuffDurationFromCmd(cmd), mult);
}

// Comparte el slot de Speed: frenar a Link reemplaza un speed-up (y al revés); al expirar vuelve la velocidad normal.
void ApplySlowDebuff(const json& cmd) {
    f32 mult = std::clamp(GetNumber(cmd, kDefaultSlowMult), 0.2f, 0.9f);
    ActivateBuff(BridgeBuff::Speed, BuffDurationFromCmd(cmd), mult);
}
void ApplyHasteBuff(const json& cmd) {
    float duration = BuffDurationFromCmd(cmd);
    ActivateBuff(BridgeBuff::Speed, duration, kDefaultSpeedMult);
    ActivateBuff(BridgeBuff::DamageUp, duration, kDefaultDamageMult);
}

struct OwlWarpAlias {
    const char* name;
    s32 owlId;
};

constexpr OwlWarpAlias kOwlWarpAliases[] = {
    { "great_bay", OWL_WARP_GREAT_BAY_COAST },
    { "great_bay_coast", OWL_WARP_GREAT_BAY_COAST },
    { "zora_cape", OWL_WARP_ZORA_CAPE },
    { "snowhead", OWL_WARP_SNOWHEAD },
    { "mountain_village", OWL_WARP_MOUNTAIN_VILLAGE },
    { "clock_town", OWL_WARP_CLOCK_TOWN },
    { "milk_road", OWL_WARP_MILK_ROAD },
    { "woodfall", OWL_WARP_WOODFALL },
    { "southern_swamp", OWL_WARP_SOUTHERN_SWAMP },
    { "swamp", OWL_WARP_SOUTHERN_SWAMP },
    { "ikana", OWL_WARP_IKANA_CANYON },
    { "ikana_canyon", OWL_WARP_IKANA_CANYON },
    { "stone_tower", OWL_WARP_STONE_TOWER },
};

s32 OwlIdFromName(const std::string& name) {
    std::string key = name;
    if (key.rfind("warp_", 0) == 0) {
        key = key.substr(5);
    }
    for (const OwlWarpAlias& alias : kOwlWarpAliases) {
        if (key == alias.name) {
            return alias.owlId;
        }
    }
    return -1;
}

void CollectActivatedOwls(s32* outIds, s32* outCount) {
    *outCount = 0;
    for (s32 i = 0; i < OWL_WARP_ENTRANCE; i++) {
        if (GET_OWL_STATUE_ACTIVATED(i)) {
            outIds[(*outCount)++] = i;
        }
    }
}

void DismountForWarp(Player* player) {
    if (gHorseIsMounted && (player->stateFlags1 & PLAYER_STATE1_800000) && player->rideActor != nullptr) {
        if (CHECK_QUEST_ITEM(QUEST_SONG_EPONA)) {
            gSaveContext.save.saveInfo.horseData.sceneId = gPlayState->sceneId;
            gSaveContext.save.saveInfo.horseData.pos.x = player->rideActor->world.pos.x;
            gSaveContext.save.saveInfo.horseData.pos.y = player->rideActor->world.pos.y;
            gSaveContext.save.saveInfo.horseData.pos.z = player->rideActor->world.pos.z;
            gSaveContext.save.saveInfo.horseData.yaw = player->rideActor->shape.rot.y;
        }
        gHorseIsMounted = false;
    }
}

ApplyResult ApplyOwlWarp(Player* player, const json& cmd, const std::string& action) {
    s32 requested = -1;
    std::string target = GetStringField(cmd, "target");
    if (target.empty()) {
        target = GetStringField(cmd, "owl");
    }
    if (!target.empty()) {
        requested = OwlIdFromName(target);
    }
    if (requested < 0 && action != "warp" && action != "warp_random") {
        requested = OwlIdFromName(action);
    }

    s32 activated[OWL_WARP_ENTRANCE];
    s32 activatedCount = 0;
    CollectActivatedOwls(activated, &activatedCount);
    if (activatedCount == 0) {
        SPDLOG_INFO("HiveShock: warp skipped (no owl statues activated)");
        return ApplyResult::Skipped;
    }

    s32 owlId = requested;
    if (owlId < 0 || owlId >= OWL_WARP_ENTRANCE) {
        s32 pick = static_cast<s32>(Rand_ZeroOne() * activatedCount);
        if (pick >= activatedCount) {
            pick = activatedCount - 1;
        }
        owlId = activated[pick];
    } else if (!GET_OWL_STATUE_ACTIVATED(owlId)) {
        SPDLOG_INFO("HiveShock: warp skipped (owl {} not activated)", owlId);
        return ApplyResult::Skipped;
    }

    DismountForWarp(player);
    gPlayState->nextEntrance = sOwlWarpEntrancesForMods[owlId];
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_WHITE;
    Player_PlaySfx(player, NA_SE_SY_TRANSFORM_MASK_FLASH);
    SPDLOG_INFO("HiveShock: owl warp id={}", owlId);
    return ApplyResult::Applied;
}

bool IsOwlWarpAction(const std::string& action) {
    return action == "warp" || action == "warp_random" || OwlIdFromName(action) >= 0;
}

// Persist a vanilla Song-of-Time cycle save to MAIN flash, then let moon crash
// reload it (Sram_ResetSaveFromMoonCrash). Dawn then does 0 → 1 (72 hours).
// Skip on first-cycle Deku so Clock Town MAIN stays and EnOsn teaches Healing.
void PersistSongOfTimeSaveForMoonCrash() {
    if (gPlayState == nullptr) {
        return;
    }

    bool pastDekuIntro = CHECK_QUEST_ITEM(QUEST_SONG_HEALING) ||
                         (gSaveContext.save.saveInfo.playerData.threeDayResetCount > 0);
    if (!pastDekuIntro) {
        SPDLOG_INFO("HiveShock: moon crash keeps existing MAIN save (Deku intro)");
        return;
    }

    if (gSaveContext.fileNum == 0xFF || !gSaveContext.flashSaveAvailable) {
        SPDLOG_WARN("HiveShock: moon crash SoT persist skipped (no flash file)");
        return;
    }

    if (CVarGetInteger("gEnhancements.Cycle.SaveOnMoonCrash", 0)) {
        return;
    }

    Sram_SaveEndOfCycle(gPlayState);
    func_8014546C(&gPlayState->sramCtx);
    SysFlashrom_WriteDataSync(gPlayState->sramCtx.saveBuf,
                              gFlashSaveStartPages[gSaveContext.fileNum * FLASH_SAVE_MAIN_MULTIPLIER],
                              gFlashSaveNumPages[gSaveContext.fileNum * FLASH_SAVE_MAIN_MULTIPLIER]);
    SPDLOG_INFO("HiveShock: persisted SoT cycle save to MAIN (day={})", (int)gSaveContext.save.day);
}

struct SpawnDef {
    const char* name;
    s16 actorId;
    s16 objectId; // -1 = already loaded (GAMEPLAY_KEEP)
    s32 params;
    s16 rotX;
    f32 yOffset;
    u8 weight; // cuánto del presupuesto MaxLoad usa una instancia viva (0 = gratis: fairy, bombchu)
};

constexpr SpawnDef kSpawnTable[] = {
    { "enemy", ACTOR_EN_RR, OBJECT_RR, 1, 0, 0.0f, 2 },
    { "like_like", ACTOR_EN_RR, OBJECT_RR, 1, 0, 0.0f, 2 },
    { "guay", ACTOR_EN_CROW, OBJECT_CROW, 0, 0, 50.0f, 1 },
    { "keese", ACTOR_EN_FIREFLY, OBJECT_FIREFLY, 2, 0, 50.0f, 1 },      // KEESE_NORMAL_FLY
    { "fire_keese", ACTOR_EN_FIREFLY, OBJECT_FIREFLY, 0, 0, 50.0f, 1 }, // KEESE_FIRE_FLY
    { "ice_keese", ACTOR_EN_FIREFLY, OBJECT_FIREFLY, 4, 0, 50.0f, 1 },  // KEESE_ICE_FLY
    { "chuchu", ACTOR_EN_SLIME, OBJECT_SLIME, 0, 0, 0.0f, 2 },
    { "redead", ACTOR_EN_RD, OBJECT_RD, 1, 0, 0.0f, 2 },
    { "wolfos", ACTOR_EN_WF, OBJECT_WF, 0, 0, 0.0f, 2 },
    { "stalchild", ACTOR_EN_SKB, OBJECT_SKB, 0, 0, 0.0f, 1 },
    { "bubble", ACTOR_EN_BB, OBJECT_BB, 0, 0, 40.0f, 1 },
    { "skulltula", ACTOR_EN_ST, OBJECT_ST, 0x3F, 0, 80.0f, 1 },
    { "bat", ACTOR_EN_BAT, OBJECT_BAT, 0, 0, 50.0f, 1 },
    { "tektite", ACTOR_EN_TITE, OBJECT_TITE, 0, 0, 0.0f, 1 },
    { "freezard", ACTOR_EN_FZ, OBJECT_FZ, 0, 0, 0.0f, 1 },
    { "cucco", ACTOR_EN_ATTACK_NIW, OBJECT_NIW, 0, 0, 50.0f, 1 },
    // Etapa 1 — enemigos fuertes / semi-jefes
    { "dinolfos", ACTOR_EN_DINOFOS, OBJECT_DINOFOS, 0, 0, 0.0f, 3 },
    { "iron_knuckle", ACTOR_EN_IK, OBJECT_IK, 1, 0, 0.0f, 4 }, // params 1 = silver
    { "garo", ACTOR_EN_JSO, OBJECT_JSO, 0, 0, 0.0f, 3 },
    { "garo_master", ACTOR_EN_JSO2, OBJECT_JSO, 1, 0, 0.0f, 4 }, // 1 = NORMAL (0 = cutscene light arrow)
    { "wallmaster", ACTOR_EN_WALLMAS, OBJECT_WALLMASTER, 0, 0, 0.0f, 2 },
    { "floormaster", ACTOR_EN_FLOORMAS, OBJECT_WALLMASTER, 0, 0, 0.0f, 3 },
    { "eyegore", ACTOR_EN_EGOL, OBJECT_EG, 0x1FFF, 0, 0.0f, 4 }, // sin path/switch (parche en EnEgol)
    { "fairy", ACTOR_EN_ELF, -1, 1, 0, 30.0f, 0 },
    { "bombchu", ACTOR_EN_BOM_CHU, -1, 0, 0, 0.0f, 0 },
};

constexpr const char* kRandomEnemies[] = {
    "guay",       "keese",        "fire_keese", "ice_keese",    "chuchu", "redead",
    "wolfos",     "stalchild",    "bubble",     "skulltula",    "bat",    "tektite",
    "freezard",   "like_like",    "dinolfos",   "iron_knuckle", "garo",   "garo_master",
    "floormaster", "eyegore",
};

// Pool reducido: enemigos frecuentes del random pool. Solo se precarga con el Bridge conectado.
constexpr s16 kHotPreloadObjectIds[] = {
    OBJECT_CROW, OBJECT_FIREFLY, OBJECT_SLIME, OBJECT_RD, OBJECT_RR, OBJECT_NIW,
};
constexpr u32 kHotPreloadFrameGap = 2; // 1 object cada N frames en idle (menos presión de memoria)

struct PendingSpawn {
    std::string name;
    std::string user;
    s16 yawOffset = 0;   // solo para spawns en círculo (cucco army); no se persiste
    f32 radius = 0.0f;   // 0 = distancia habitual del tipo de enemigo
    u32 earliestTick = 0; // se compara con gTick
    u32 attempts = 0;    // colocaciones fallidas, solo para limitar el log
    s64 createdAt = 0;   // segundos unix, solo para caducar entradas restauradas del disco
    bool requeued = false; // enemigo que ya se contó como spawneado y vuelve tras un cambio de escena
};

struct TrackedEnemy {
    Actor* actor = nullptr;
    const SpawnDef* def = nullptr;
    std::string user;
    u32 spawnTick = 0;
    u8 weight = 0;
    bool alive = true;      // false cuando se vio muerto (salud 0): un enemigo muriendo no se reencola
    bool sawHealth = false; // algunos actores (cuccos) no tienen salud, así que "salud == 0" solo vale si antes fue > 0
    u32 farSinceTick = 0;   // 0 = suficientemente cerca de Link
};

std::deque<PendingSpawn> gPendingSpawns;
std::vector<TrackedEnemy> gTrackedEnemies;
std::vector<s32> gBridgeObjectSlots;
// Contador monótono de updates del juego. A diferencia de gameplayFrames no se reinicia en cada carga de escena, así
// que los retrasos y espaciados calculados con él siguen valiendo entre escenas.
u32 gTick = 0;
u32 gNextSpawnTick = 0;
u32 gSpawnUnblockTick = 0;
u32 gLastEliteWaitLogTick = 0;
u32 gRoomGraceUntilTick = 0;
s16 gLastRoom = -1;
bool gBacklogDirty = false;
bool gBacklogLoaded = false;
u32 gLastBacklogSaveTick = 0;
u32 gLastQueueReportTick = 0;
bool gQueueReportForce = true;
std::string gLastQueueReport;
std::atomic<bool> gBridgeClientConnected{ false };
// Puesto en true por el hilo del socket al aceptar cliente; el hilo principal lo consume y manda
// un snapshot de capacidad. Evita tocar gTrackedEnemies/gPendingSpawns fuera del hilo principal.
std::atomic<bool> gCapacityResyncPending{ false };
size_t gHotPreloadNext = 0;
bool gHotPreloadComplete = false;
u32 gHotPreloadCooldown = 0;
Clock::time_point gNextQueuedCommandTime{};
u32 gSlotsFreedThisTick = 0; // coalesca "slot_freed" para mandar como máximo 1 evento por frame
Clock::time_point gNextCapacitySnapshot{};

float GetNumber(const json& cmd, float defaultValue) {
    if (cmd.contains("strength") && cmd["strength"].is_number()) {
        return cmd["strength"].get<float>();
    }
    if (cmd.contains("value") && cmd["value"].is_number()) {
        return cmd["value"].get<float>();
    }
    return defaultValue;
}

float GetDuration(const json& cmd) {
    if (cmd.contains("duration") && cmd["duration"].is_number()) {
        return std::max(0.0f, cmd["duration"].get<float>());
    }
    return 0.0f;
}

std::string GetStringField(const json& cmd, const char* key) {
    if (cmd.contains(key) && cmd[key].is_string()) {
        return cmd[key].get<std::string>();
    }
    return "";
}

void LogUser(const json& cmd, const std::string& action) {
    std::string user = GetStringField(cmd, "user");
    if (!user.empty()) {
        SPDLOG_INFO("HiveShock: action '{}' from '{}'", action, user);
    }
}

std::string TruncateDonorName(const std::string& user) {
    if (user.empty()) {
        return "";
    }
    std::string out;
    out.reserve(kMaxNameTagChars);
    for (unsigned char c : user) {
        if (out.size() >= kMaxNameTagChars) {
            break;
        }
        // Font-friendly ASCII; NameTag also strips unsupported glyphs.
        if (c >= 32 && c < 127) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

void AttachDonorNameTag(Actor* actor, const std::string& user) {
    if (actor == nullptr) {
        return;
    }
    std::string label = TruncateDonorName(user);
    if (label.empty()) {
        return;
    }
    NameTag_RegisterForActor(actor, label.c_str());
}

void SendEventToBridge(const json& event) {
    // Best-effort: no bloquear el hilo del juego si el bridge no escucha.
    std::string line = event.dump() + "\n";
    std::thread([line = std::move(line)]() {
        NetSession session;
        SocketHandle sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) {
            return;
        }
        ConfigureSocket(sock);
        sockaddr_in addr = LoopbackAddress(kEventPort);
        SetNonBlocking(sock, true);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            const int err = WSAGetLastError();
            const bool inProgress = err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
            const bool inProgress = errno == EINPROGRESS || errno == EINTR;
#endif
            if (!inProgress || !WaitForSocket(sock, false, 200)) {
                CloseSocket(sock);
                return;
            }
        }
        SetNonBlocking(sock, false);
        SendAll(sock, line);
        CloseSocket(sock);
    }).detach();
}

void NotifyDeathCount() {
    json event = { { "event", "player_death" }, { "deaths", gDeathCount.load() } };
    SendEventToBridge(event);
}

void NotifyDeathsReset(const char* reason) {
    json event = { { "event", reason }, { "deaths", 0 } };
    SendEventToBridge(event);
}

/** Suma 1 al contador del bridge (muerte / luna) y notifica. */
void RegisterDeath(const char* reason) {
    int deaths = ++gDeathCount;
    gDeathCountedThisCycle = true;
    SPDLOG_INFO("HiveShock: {} #{}", reason, deaths);
    NotifyDeathCount();
}

void PushCommand(json cmd) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gCommandQueue.size() >= kMaxQueueSize) {
        SPDLOG_WARN("HiveShock: queue full ({}), rejecting the new command", kMaxQueueSize);
        return;
    }

    // heal / hearts van delante del resto (tras otras curas ya en cola).
    const std::string action = GetStringField(cmd, "action");
    if (IsHealPriorityAction(action)) {
        auto it = gCommandQueue.begin();
        while (it != gCommandQueue.end()) {
            if (!IsHealPriorityAction(GetStringField(*it, "action"))) {
                break;
            }
            ++it;
        }
        gCommandQueue.insert(it, std::move(cmd));
        return;
    }

    gCommandQueue.push_back(std::move(cmd));
}

void PushCommandFront(json cmd) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    gCommandQueue.push_front(std::move(cmd));
}

// Reencola un Deferred detrás de heal/hearts (no pisa curas nuevas que entraron al vaciar el lote).
void PushDeferredCommand(json cmd) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    auto it = gCommandQueue.begin();
    while (it != gCommandQueue.end()) {
        if (!IsHealPriorityAction(GetStringField(*it, "action"))) {
            break;
        }
        ++it;
    }
    gCommandQueue.insert(it, std::move(cmd));
}

bool PopCommand(json& out) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gCommandQueue.empty()) {
        return false;
    }
    out = std::move(gCommandQueue.front());
    gCommandQueue.pop_front();
    return true;
}

bool IsHealPriorityAction(const std::string& action) {
    return action == "heal" || action == "hearts";
}

bool IsDangerousAction(const std::string& action) {
    return action == "bomb" || action == "enemy" || action == "like_like" || action == "knockback" ||
           action == "impulse" || action == "damage" || action == "insta_kill" || action == "instakill" ||
           action == "kill" || action == "guay" || action == "keese" || action == "fire_keese" ||
           action == "ice_keese" || action == "chuchu" || action == "redead" || action == "wolfos" ||
           action == "stalchild" || action == "bubble" || action == "skulltula" || action == "bat" ||
           action == "tektite" || action == "freezard" || action == "random_enemy" || action == "cucco" ||
           action == "cucco_army" || action == "dinolfos" || action == "iron_knuckle" || action == "garo" ||
           action == "garo_master" || action == "wallmaster" || action == "floormaster" || action == "eyegore" ||
           action == "bombchu" || action == "keg" || action == "blast" || action == "explode" ||
           action == "bomb_explode" || action == "rupee_rain" ||
           action == "time_up" || action == "times_up" || action == "time_zero" || action == "freeze" ||
           action == "shock" || action == "fire" || action == "delete_save" || action == "new_run" ||
           action == "clear_enemies";
}

bool IsSceneChangeInProgress() {
    if (gPlayState == nullptr) {
        return true;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return true;
    }
    if (gPlayState->transitionMode != TRANS_MODE_OFF) {
        return true;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && (player->stateFlags1 & (PLAYER_STATE1_1 | PLAYER_STATE1_DEAD))) {
        return true;
    }
    return false;
}

bool IsPlayerOnSwampBoat() {
    if (gPlayState == nullptr) {
        return false;
    }
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_BG].first; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_BG_INGATE && actor->update != nullptr &&
            DynaPolyActor_IsPlayerOnTop(reinterpret_cast<DynaPolyActor*>(actor))) {
            return true;
        }
    }
    return false;
}

// Link cannot fight: boat, shops, minigames, horse, swim, talk, ocarina, CS, fade.
// Dangerous gifts stay queued until this returns false.
bool IsPlayerUnableToFight() {
    if (gPlayState == nullptr) {
        return true;
    }
    if (IS_PAUSED(&gPlayState->pauseCtx)) {
        return true;
    }
    if (IsSceneChangeInProgress()) {
        return true;
    }

    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return true;
    }
    if (Player_InBlockingCsMode(gPlayState, player)) {
        return true;
    }
    if (player->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_800000 | PLAYER_STATE1_8000000)) {
        return true;
    }
    if (player->stateFlags2 & PLAYER_STATE2_USING_OCARINA) {
        return true;
    }
    if (gSaveContext.minigameStatus == MINIGAME_STATUS_ACTIVE) {
        return true;
    }
    if (gPlayState->interfaceCtx.minigameState != MINIGAME_STATE_NONE) {
        return true;
    }
    if (gPlayState->interfaceCtx.restrictions.bButton != 0) {
        return true;
    }
    if (IsPlayerOnSwampBoat()) {
        return true;
    }
    return false;
}

bool ShouldDeferDangerousEffects() {
    return IsPlayerUnableToFight();
}

void ResumeTime() {
    if (gSavedTimeSpeed != 0) {
        R_TIME_SPEED = gSavedTimeSpeed;
    } else if (gPlayState != nullptr && gPlayState->envCtx.sceneTimeSpeed != 0) {
        R_TIME_SPEED = gPlayState->envCtx.sceneTimeSpeed;
    } else {
        R_TIME_SPEED = 3;
    }
    gTimeIsFrozen = false;
    gTimeFreezeHasDeadline = false;
    SPDLOG_INFO("HiveShock: time resumed (R_TIME_SPEED={})", (int)R_TIME_SPEED);
}

void FreezeTime(float durationSeconds) {
    if (!gTimeIsFrozen) {
        gSavedTimeSpeed = R_TIME_SPEED;
    }
    R_TIME_SPEED = 0;
    gTimeIsFrozen = true;
    if (durationSeconds > 0.0f) {
        gTimeFrozenUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                              std::chrono::duration<float>(durationSeconds));
        gTimeFreezeHasDeadline = true;
        SPDLOG_INFO("HiveShock: time frozen for {:.1f}s", durationSeconds);
    } else {
        gTimeFreezeHasDeadline = false;
        SPDLOG_INFO("HiveShock: time frozen until resume_time");
    }
}

void ResumeTime();
void TryStartMoonCrash();

void TickTimedEffects() {
    if (gTimeIsFrozen && gTimeFreezeHasDeadline && Clock::now() >= gTimeFrozenUntil) {
        ResumeTime();
    }
    if (gInvertControls && Clock::now() >= gInvertUntil) {
        gInvertControls = false;
        SPDLOG_INFO("HiveShock: controls restored");
    }

    ClearExpiredBuffs();
    TickInfiniteMagic();
    TickBuffVisuals();

    if (gPendingMoonCrash) {
        TryStartMoonCrash();
    }
}

void TryStartMoonCrash() {
    if (gPlayState == nullptr) {
        gPendingMoonCrash = false;
        return;
    }

    if (gPlayState->transitionMode != TRANS_MODE_OFF) {
        return;
    }
    if (IS_PAUSED(&gPlayState->pauseCtx)) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && Player_InBlockingCsMode(gPlayState, player)) {
        return;
    }

    gPendingMoonCrash = false;

    if (gTimeIsFrozen) {
        ResumeTime();
    }

    PersistSongOfTimeSaveForMoonCrash();
    Interface_StartMoonCrash(gPlayState);
    RegisterDeath("moon crash (fin del mundo)");
    SPDLOG_INFO("HiveShock: moon crash started (vanilla Interface_StartMoonCrash)");
}

void ResetHotPreload() {
    gHotPreloadNext = 0;
    gHotPreloadComplete = false;
    gHotPreloadCooldown = 0;
}

void LoadBacklog();
void SaveBacklog();

// El token se lee de la CVar en el hilo principal (RefreshToken) y el hilo del servidor solo usa esta copia.
std::mutex gTokenMutex;
std::string gToken;

void RefreshToken() {
    const char* token = CVarGetString(CVAR_TOKEN_NAME, "");
    std::string value = token != nullptr ? std::string(token) : std::string();
    std::lock_guard<std::mutex> lock(gTokenMutex);
    if (gToken != value) {
        gToken = std::move(value);
    }
}

std::string ConfiguredToken() {
    std::lock_guard<std::mutex> lock(gTokenMutex);
    return gToken;
}

void HandleClient(SocketHandle clientSocket, SocketHandle listenSocket) {
    SPDLOG_INFO("HiveShock: client connected");
    ConfigureSocket(clientSocket);
    // Con token configurado, la primera línea debe ser {"action":"auth","token":"..."}; si no se corta la conexión.
    const std::string token = ConfiguredToken();
    bool authenticated = token.empty();
    if (authenticated) {
        gBridgeClientConnected = true;
        gCapacityResyncPending = true; // el hilo principal manda el snapshot (no tocar tracking desde aquí)
        ResetHotPreload();
    }
    std::string buffer;
    char recvBuf[1024];
    bool closeClient = false;

    auto handleLine = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            return;
        }
        if (line.size() > kMaxLineBytes) {
            SPDLOG_WARN("HiveShock: dropping oversized JSON line ({} bytes)", line.size());
            return;
        }
        try {
            json parsed = json::parse(line);
            if (!authenticated) {
                if (GetStringField(parsed, "action") == "auth" && GetStringField(parsed, "token") == token) {
                    authenticated = true;
                    gBridgeClientConnected = true;
                    gCapacityResyncPending = true;
                    ResetHotPreload();
                    SPDLOG_INFO("HiveShock: client authenticated");
                } else {
                    SPDLOG_WARN("HiveShock: client rejected (missing or wrong token)");
                    closeClient = true;
                }
                return;
            }
            if (GetStringField(parsed, "action") == "auth") {
                return; // sin token configurado, un cliente puede mandar auth igualmente; se ignora
            }
            PushCommand(std::move(parsed));
        } catch (const std::exception& e) {
            SPDLOG_WARN("HiveShock: invalid JSON: {}", e.what());
        }
    };

    while (gServerRun.load() && !closeClient) {
        // Despierta periódicamente para que Disable() nunca tenga que interrumpir un recv() bloqueante.
        const int ready = WaitForEitherReadable(clientSocket, listenSocket, kPollIntervalMs);
        if (ready & 2) {
            // Hay otro cliente esperando: se descarta este (puede estar muerto) para no dejar al nuevo sin turno.
            SPDLOG_INFO("HiveShock: new connection pending, replacing current client");
            break;
        }
        if (!(ready & 1)) {
            continue;
        }

        auto bytesReceived = recv(clientSocket, recvBuf, sizeof(recvBuf), 0);
        if (bytesReceived == 0) {
            break;
        }
        if (bytesReceived < 0) {
            if (LastSocketErrorIsTransient()) {
                continue;
            }
            break;
        }

        buffer.append(recvBuf, static_cast<size_t>(bytesReceived));

        size_t newlinePos;
        while (!closeClient && (newlinePos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newlinePos);
            buffer.erase(0, newlinePos + 1);
            handleLine(std::move(line));
        }

        // Un cliente que nunca manda salto de línea no puede hacer crecer el buffer sin límite.
        if (buffer.size() > kMaxLineBytes) {
            SPDLOG_WARN("HiveShock: dropping oversized line without newline ({} bytes)", buffer.size());
            buffer.clear();
        }
    }

    // Algunos clientes mandan un objeto JSON y cierran sin salto de línea: no se pierde.
    if (!closeClient && !buffer.empty()) {
        handleLine(std::move(buffer));
    }

    CloseSocket(clientSocket);
    gBridgeClientConnected = false;
    SPDLOG_INFO("HiveShock: client disconnected");
}

void RunServer(uint16_t port) {
    NetSession session;
    if (!session.ok) {
        SPDLOG_ERROR("HiveShock: network subsystem init failed ({})", LastSocketError());
        gServerRun = false;
        return;
    }

    SocketHandle listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == kInvalidSocket) {
        SPDLOG_ERROR("HiveShock: socket() failed ({})", LastSocketError());
        gServerRun = false;
        return;
    }
    ConfigureSocket(listenSocket);

#ifdef _WIN32
    // Bind exclusivo: SO_REUSEADDR en Windows dejaría que otro proceso secuestre el puerto.
    BOOL exclusive = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
#else
    // Permite re-bind inmediato tras Disable()/Enable() (TIME_WAIT); seguro en POSIX.
    int reuse = 1;
    if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        SPDLOG_WARN("HiveShock: SO_REUSEADDR failed ({})", LastSocketError());
    }
#endif

    // Todas las interfaces (LAN + loopback). 127.0.0.1 sigue conectando en la misma PC.
    sockaddr_in serverAddr = AnyAddress(port);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
        SPDLOG_ERROR("HiveShock: bind 0.0.0.0:{} failed ({})", port, LastSocketError());
        CloseSocket(listenSocket);
        gServerRun = false;
        return;
    }

    if (listen(listenSocket, 1) != 0) {
        SPDLOG_ERROR("HiveShock: listen() failed ({})", LastSocketError());
        CloseSocket(listenSocket);
        gServerRun = false;
        return;
    }

    gListening = true;
    SPDLOG_INFO("HiveShock: listening on 0.0.0.0:{} (LAN {} / also 127.0.0.1:{})", port, FormatListenEndpoint(), port);

    while (gServerRun.load()) {
        if (!WaitForSocket(listenSocket, true, kPollIntervalMs)) {
            continue;
        }
        SocketHandle clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == kInvalidSocket) {
            if (LastSocketErrorIsTransient()) {
                continue;
            }
            SPDLOG_ERROR("HiveShock: accept() failed ({})", LastSocketError());
            break;
        }
        HandleClient(clientSocket, listenSocket);
    }

    gListening = false;
    gBridgeClientConnected = false;
    CloseSocket(listenSocket);
    // Si el bucle terminó por un error y no por Disable(), IsEnabled() debe reflejarlo.
    gServerRun = false;
    SPDLOG_INFO("HiveShock: server stopped");
}

void StartServer() {
    if (gServerRun.exchange(true)) {
        return;
    }
    if (!gBacklogLoaded) {
        LoadBacklog();
    }
    RefreshToken();
    if (gServerThreadHolder.thread.joinable()) {
        gServerThreadHolder.thread.join();
    }
    gPort = ConfiguredPort();
    gServerThreadHolder.thread = std::thread(RunServer, gPort.load());
}

void StopServer() {
    gServerRun = false;
    // Todas las llamadas bloqueantes del hilo del servidor usan poll con timeout: el join vuelve rápido.
    if (gServerThreadHolder.thread.joinable()) {
        gServerThreadHolder.thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        gCommandQueue.clear();
    }
    // Desactivar es deliberado: se olvida todo lo que esperaba (y su copia en disco). Si el servidor nunca corrió en
    // este proceso, la cola guardada se deja intacta.
    gTrackedEnemies.clear();
    if (gBacklogLoaded) {
        gPendingSpawns.clear();
        SaveBacklog();
    }
    gStatTotal = gStatSpawned = gStatDefeated = gStatAlive = gStatWaiting = gStatLoad = 0;
    gStatEliteBank = gStatEliteBankMax = gStatEliteWaiting = 0;
    gListening = false;
    gBridgeClientConnected = false;
}

// Permite que un gift vuelva a pegar aunque Link aún tenga i-frames de un golpe anterior.
void ClearPlayerInvincibility(Player* player) {
    if (player->invincibilityTimer > 0) {
        player->invincibilityTimer = 0;
    }
}

s16 HitDamageFromCmd(const json& cmd, float defaultHeartsSixteenths) {
    return static_cast<s16>(std::clamp(GetNumber(cmd, defaultHeartsSixteenths), 1.0f, 64.0f));
}

// Reacción visual primero (daño 0): vanilla aborta el hit si HP llega a 0 en suelo.
// Luego Health_ChangeBy aplica el daño real y SÍ puede matar. Invencible Bridge sigue bloqueando.
void ApplyPlayerHit(Player* player, s32 hitType, s16 damage, f32 speed, f32 velocityY, s16 yaw,
                    s32 invincibilityTimer) {
    ClearPlayerInvincibility(player);
    player->actor.colChkInfo.damage = 0;
    func_80833B18(gPlayState, player, hitType, speed, velocityY, yaw, invincibilityTimer);
    if (damage > 0) {
        Health_ChangeBy(gPlayState, static_cast<s16>(-damage));
    }
}

void ApplyImpulse(Player* player, const json& cmd) {
    float strength = GetNumber(cmd, 6.34375f);
    strength = std::clamp(strength, -20.0f, 20.0f);
    player->actor.velocity.y = strength;
}

void ApplyKnockback(Player* player, const json& cmd) {
    float strength = 6.0f;
    if (cmd.contains("strength") && cmd["strength"].is_number()) {
        strength = cmd["strength"].get<float>();
    } else if (cmd.contains("value") && cmd["value"].is_number()) {
        // Compat: value era la fuerza del empujón
        strength = cmd["value"].get<float>();
    }
    strength = std::clamp(strength, 0.5f, 14.0f);

    // deal_damage false / damage 0 = solo lanzar.
    bool dealDamage = true;
    if (cmd.contains("deal_damage") && cmd["deal_damage"].is_boolean()) {
        dealDamage = cmd["deal_damage"].get<bool>();
    }

    s16 damage = 4;
    if (cmd.contains("damage") && cmd["damage"].is_number()) {
        damage = static_cast<s16>(std::clamp(cmd["damage"].get<float>(), 0.0f, 64.0f));
        if (!cmd.contains("deal_damage")) {
            dealDamage = damage > 0;
        }
    }
    if (!dealDamage) {
        damage = 0;
    }

    s16 yaw = player->actor.shape.rot.y + 0x8000;
    ApplyPlayerHit(player, 1, damage, strength * 1.5f, strength * 1.4f, yaw, 20);
}

void ApplyFreeze(Player* player, const json& cmd) {
    ApplyPlayerHit(player, 3, HitDamageFromCmd(cmd, 8.0f), 0.0f, 0.0f, 0, 20);
}

void ApplyShock(Player* player, const json& cmd) {
    ApplyPlayerHit(player, 4, HitDamageFromCmd(cmd, 16.0f), 0.0f, 0.0f, player->actor.shape.rot.y, 20);
}

void ApplyFire(Player* player, const json& cmd) {
    for (int i = 0; i < PLAYER_BODYPART_MAX; i++) {
        player->bodyFlameTimers[i] = static_cast<uint8_t>(Rand_S16Offset(0, 200));
    }
    player->bodyIsBurning = true;
    s16 yaw = player->actor.shape.rot.y + 0x8000;
    ApplyPlayerHit(player, 0, HitDamageFromCmd(cmd, 16.0f), 4.0f, 5.0f, yaw, 20);
}

void ApplyHeal(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, 16.0f), 1.0f, 80.0f));
    Health_ChangeBy(gPlayState, amount);
}

void ApplyDamage(Player* player, const json& cmd) {
    s16 yaw = player->actor.shape.rot.y + 0x8000;
    ApplyPlayerHit(player, 1, HitDamageFromCmd(cmd, 16.0f), 4.0f, 5.0f, yaw, 20);
}

void ApplyInstaKill() {
    // health == 0 dispara la cinemática de muerte del jugador en el siguiente update.
    // Un hada embotellada puede salvarlo, igual que en una muerte normal.
    gSaveContext.healthAccumulator = 0;
    gSaveContext.save.saveInfo.playerData.health = 0;
}

void ApplyRupees(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, 10.0f), -500.0f, 500.0f));
    Rupees_ChangeBy(amount);
}

void ApplyMagic(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, static_cast<float>(MAGIC_NORMAL_METER)), -128.0f, 128.0f));
    if (amount >= 0) {
        Magic_Add(gPlayState, amount);
    } else {
        Magic_Consume(gPlayState, -amount, MAGIC_CONSUME_NOW);
    }
}

void ApplyBomb(Player* player) {
    // rotX & 1 == 0 → bomb with a normal fuse (not a powder keg)
    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_BOM, player->actor.world.pos.x,
                                 player->actor.world.pos.y, player->actor.world.pos.z, 0, 0, 0, 0);
    if (spawned == nullptr) {
        SPDLOG_WARN("HiveShock: failed to spawn bomb (actors={})", gPlayState->actorCtx.totalLoadedActors);
    }
}

const SpawnDef* FindSpawnDef(const std::string& name) {
    for (const SpawnDef& def : kSpawnTable) {
        if (name == def.name) {
            return &def;
        }
    }
    return nullptr;
}

s32 CountFreeObjectSlots() {
    return static_cast<s32>(ARRAY_COUNT(gPlayState->objectCtx.slots)) - gPlayState->objectCtx.numEntries;
}

bool ActorStillLoaded(Actor* actor) {
    if (actor == nullptr || actor->update == nullptr || gPlayState == nullptr) {
        return false;
    }
    if (actor->category >= ACTORCAT_MAX) {
        return false;
    }
    for (Actor* it = gPlayState->actorCtx.actorLists[actor->category].first; it != nullptr; it = it->next) {
        if (it == actor) {
            return true;
        }
    }
    return false;
}

bool ObjectSlotInUse(s32 slot) {
    for (s32 cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[cat].first; actor != nullptr; actor = actor->next) {
            if (actor->update != nullptr && actor->objectSlot == slot) {
                return true;
            }
        }
    }
    return false;
}

bool IsObjectReady(s16 objectId) {
    if (objectId < 0) {
        return true;
    }
    ObjectContext* objectCtx = &gPlayState->objectCtx;
    s32 slot = Object_GetSlot(objectCtx, objectId);
    return slot > OBJECT_SLOT_NONE && Object_IsLoaded(objectCtx, slot);
}

// Carga transitoria: no sube numPersistentEntries, así un cambio de room puede
// recuperar los slots. Evita dejar el banco lleno hasta salir de la escena.
s32 SpawnTransientObject(ObjectContext* objectCtx, s16 objectId) {
    if (CountFreeObjectSlots() <= kMinFreeObjectSlots) {
        return OBJECT_SLOT_NONE;
    }

    s32 slot = objectCtx->numEntries;
    objectCtx->slots[slot].id = objectId;
    size_t size = gObjectTable[objectId].vromEnd - gObjectTable[objectId].vromStart;
    if (size != 0) {
        DmaMgr_SendRequest0(objectCtx->slots[slot].segment, gObjectTable[objectId].vromStart, size);
    }
    if (slot < static_cast<s32>(ARRAY_COUNT(objectCtx->slots)) - 1) {
        objectCtx->slots[slot + 1].segment =
            reinterpret_cast<void*>(ALIGN16(reinterpret_cast<uintptr_t>(objectCtx->slots[slot].segment) + size));
    }
    objectCtx->numEntries++;
    gBridgeObjectSlots.push_back(slot);
    SPDLOG_INFO("HiveShock: loaded object {} into transient slot {}", objectId, slot);
    return slot;
}

s32 FindReclaimableBridgeSlot(size_t neededSize) {
    ObjectContext* objectCtx = &gPlayState->objectCtx;
    for (s32 slot : gBridgeObjectSlots) {
        if (slot <= OBJECT_SLOT_NONE || slot >= objectCtx->numEntries) {
            continue;
        }
        if (ObjectSlotInUse(slot)) {
            continue;
        }
        size_t capacity = 0;
        if (slot + 1 < objectCtx->numEntries) {
            capacity = static_cast<size_t>(reinterpret_cast<uintptr_t>(objectCtx->slots[slot + 1].segment) -
                                           reinterpret_cast<uintptr_t>(objectCtx->slots[slot].segment));
        } else if (objectCtx->spaceEnd != nullptr) {
            capacity = static_cast<size_t>(reinterpret_cast<uintptr_t>(objectCtx->spaceEnd) -
                                           reinterpret_cast<uintptr_t>(objectCtx->slots[slot].segment));
        }
        if (capacity != 0 && neededSize > capacity) {
            continue;
        }
        return slot;
    }
    return OBJECT_SLOT_NONE;
}

bool ReclaimObjectSlot(s16 objectId) {
    size_t needed = gObjectTable[objectId].vromEnd - gObjectTable[objectId].vromStart;
    s32 slot = FindReclaimableBridgeSlot(needed);
    if (slot <= OBJECT_SLOT_NONE) {
        return false;
    }

    ObjectContext* objectCtx = &gPlayState->objectCtx;
    objectCtx->slots[slot].id = objectId;
    if (needed != 0) {
        DmaMgr_SendRequest0(objectCtx->slots[slot].segment, gObjectTable[objectId].vromStart, needed);
    }
    SPDLOG_INFO("HiveShock: reclaimed object slot {} for {}", slot, objectId);
    return Object_IsLoaded(objectCtx, slot);
}

bool RequestObjectLoad(s16 objectId) {
    if (objectId < 0) {
        return true;
    }

    ObjectContext* objectCtx = &gPlayState->objectCtx;
    s32 slot = Object_GetSlot(objectCtx, objectId);
    if (slot > OBJECT_SLOT_NONE && Object_IsLoaded(objectCtx, slot)) {
        return true;
    }

    if (slot <= OBJECT_SLOT_NONE) {
        slot = SpawnTransientObject(objectCtx, objectId);
        if (slot > OBJECT_SLOT_NONE) {
            return Object_IsLoaded(objectCtx, slot);
        }
        return ReclaimObjectSlot(objectId);
    }

    return Object_IsLoaded(objectCtx, slot);
}

ApplyResult EnsureObjectLoaded(s16 objectId) {
    if (objectId < 0) {
        return ApplyResult::Applied;
    }

    if (IsObjectReady(objectId)) {
        return ApplyResult::Applied;
    }

    if (!RequestObjectLoad(objectId)) {
        return ApplyResult::Skipped;
    }

    return IsObjectReady(objectId) ? ApplyResult::Applied : ApplyResult::Deferred;
}

bool TickSpawnQueuePreload() {
    constexpr size_t kPreloadLookahead = 8; // solo los próximos de la cola: no cargar objects de toda una cola larga
    size_t looked = 0;
    for (const PendingSpawn& pending : gPendingSpawns) {
        if (looked++ >= kPreloadLookahead) {
            break;
        }
        const SpawnDef* def = FindSpawnDef(pending.name);
        if (def == nullptr || def->objectId < 0) {
            continue;
        }
        if (IsObjectReady(def->objectId)) {
            continue;
        }
        RequestObjectLoad(def->objectId);
        return true;
    }
    return false;
}

void TickHotObjectPreload() {
    if (!gBridgeClientConnected.load() || gHotPreloadComplete || !gPendingSpawns.empty()) {
        return;
    }

    if (gHotPreloadCooldown > 0) {
        gHotPreloadCooldown--;
        return;
    }

    if (CountFreeObjectSlots() <= kMinFreeObjectSlots + 4) {
        gHotPreloadComplete = true;
        return;
    }

    while (gHotPreloadNext < ARRAY_COUNT(kHotPreloadObjectIds)) {
        s16 objectId = kHotPreloadObjectIds[gHotPreloadNext];
        gHotPreloadNext++;
        if (IsObjectReady(objectId)) {
            continue;
        }
        if (!RequestObjectLoad(objectId)) {
            gHotPreloadComplete = true;
            return;
        }
        gHotPreloadCooldown = kHotPreloadFrameGap;
        return;
    }

    gHotPreloadComplete = true;
    SPDLOG_INFO("HiveShock: hot object preload complete");
}

void TickObjectPreload() {
    if (gPlayState == nullptr || IsPlayerUnableToFight()) {
        return;
    }

    if (TickSpawnQueuePreload()) {
        return;
    }
    TickHotObjectPreload();
}

// ---------------------------------------------------------------------------------------------
// Seguimiento de enemigos inyectados, admisión por peso y cola de spawns (ver HiveShockAdmission.h).
// Nada se borra para hacer sitio: un spawn que no cabe espera en la cola hasta que algo muera.
// ---------------------------------------------------------------------------------------------

s64 UnixNow() {
    return static_cast<s64>(std::time(nullptr));
}

// Los enemigos "contados" son los que cuentan para presupuesto, contador y reencolado. fairy/bombchu son efectos de
// un solo uso: se siguen (para clear_enemies) pero no cuentan.
bool IsCountedSpawnName(const std::string& name) {
    return name != "fairy" && name != "bombchu";
}

s32 CountSceneEnemies() {
    if (gPlayState == nullptr) {
        return 0;
    }
    return gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].length;
}

// Pone un spawn al FRENTE de la cola, p. ej. un enemigo que seguía vivo cuando cambió la escena: el regalo del
// espectador sigue a Link en lugar de perderse. Ignora el tope de la cola a propósito.
void RequeueFront(const TrackedEnemy& enemy) {
    if (enemy.def == nullptr) {
        return;
    }
    PendingSpawn pending;
    pending.name = enemy.def->name;
    pending.user = enemy.user;
    pending.earliestTick = gTick;
    pending.createdAt = UnixNow();
    pending.requeued = true;
    gPendingSpawns.push_front(std::move(pending));
    gBacklogDirty = true;
}

// Reencola los supervivientes conservando su orden original (el más antiguo queda primero).
void RequeueSurvivors(const std::vector<TrackedEnemy>& survivors) {
    for (auto it = survivors.rbegin(); it != survivors.rend(); ++it) {
        RequeueFront(*it);
    }
    if (!survivors.empty()) {
        SPDLOG_INFO("HiveShock: {} enemies survived a scene/room change, queued again", survivors.size());
    }
}

bool TrackedStillLoaded(const TrackedEnemy& t) {
    // La comprobación del id protege contra un actor liberado cuya memoria fue reutilizada por otro.
    return ActorStillLoaded(t.actor) && t.def != nullptr && t.actor->id == t.def->actorId;
}

// Descarta los enemigos que ya no están. Una muerte normal simplemente desaparece; pero justo tras un cambio de
// sala, un enemigo que desaparece aún vivo lo quitó el cambio de sala, así que se reencola en vez de perderse.
// Solo llamar desde el update por frame (nunca mientras se recorre la cola).
void PruneTrackedEnemies() {
    std::vector<TrackedEnemy> survivors;
    const bool inRoomGrace = gTick < gRoomGraceUntilTick;
    for (auto it = gTrackedEnemies.begin(); it != gTrackedEnemies.end();) {
        if (TrackedStillLoaded(*it)) {
            ++it;
            continue;
        }
        const bool counted = it->def != nullptr && IsCountedSpawnName(it->def->name);
        if (counted) {
            if (inRoomGrace && it->alive) {
                survivors.push_back(std::move(*it));
            } else {
                gStatDefeated++;
            }
        }
        gSlotsFreedThisTick++;
        it = gTrackedEnemies.erase(it);
    }
    RequeueSurvivors(survivors);
}

s32 CountLiveTrackedEnemies() {
    s32 count = 0;
    for (const TrackedEnemy& t : gTrackedEnemies) {
        if (t.def != nullptr && IsCountedSpawnName(t.def->name)) {
            count++;
        }
    }
    return count;
}

s32 GetMaxAlive() {
    return std::max(1, CVarGetInteger(CVAR_HS("MaxAlive"), kDefaultMaxAlive));
}

s32 GetMaxSceneEnemies() {
    return std::max(1, CVarGetInteger(CVAR_HS("MaxSceneEnemies"), kDefaultMaxSceneEnemies));
}

s32 GetMaxLoad() {
    s32 value = kDefaultMaxLoad;
    if (HiveShockCVarExists(CVAR_HS("MaxLoad"))) {
        value = CVarGetInteger(CVAR_HS("MaxLoad"), kDefaultMaxLoad);
    } else if (HiveShockCVarExists(CVAR_MAX_ENTITIES_NAME)) {
        value = CVarGetInteger(CVAR_MAX_ENTITIES_NAME, kDefaultMaxLoad); // CVar anterior (cantidad de entidades)
    }
    return std::clamp(value, 1, 100);
}

s32 GetEliteReserve() {
    return std::clamp(CVarGetInteger(CVAR_HS("EliteReserve"), kDefaultEliteReserve), 0, kMaxEliteReserve);
}

// Presupuesto usado por los enemigos inyectados que siguen vivos.
s32 CurrentLoad() {
    s32 load = 0;
    for (const TrackedEnemy& t : gTrackedEnemies) {
        load += t.weight;
    }
    return load;
}

HiveShockAdmission::Lane LaneOf(const SpawnDef& def) {
    return def.weight >= kEliteWeight ? HiveShockAdmission::Lane::Elite : HiveShockAdmission::Lane::Ordinary;
}

HiveShockAdmission::Config AdmissionConfig() {
    return { GetMaxLoad(), GetEliteReserve(), kOrdinaryFloor };
}

// Carga de los enemigos inyectados vivos separada por carril (ver HiveShockAdmission.h).
HiveShockAdmission::Loads CurrentLoads() {
    HiveShockAdmission::Loads loads{ 0, 0 };
    for (const TrackedEnemy& t : gTrackedEnemies) {
        if (t.def != nullptr && t.def->weight >= kEliteWeight) {
            loads.elite += t.weight;
        } else {
            loads.ordinary += t.weight;
        }
    }
    return loads;
}

// Por qué un enemigo más de este tipo no puede entrar ahora, o nullptr si cabe. Nunca se mata nada para hacer sitio.
const char* NoRoomReason(const SpawnDef& def) {
    if (gPlayState->actorCtx.totalLoadedActors >= kActorHardCap) {
        return "the engine actor limit is reached";
    }
    if (!IsCountedSpawnName(def.name)) {
        return nullptr;
    }
    const HiveShockAdmission::Lane lane = LaneOf(def);
    const HiveShockAdmission::Config config = AdmissionConfig();
    const s32 eliteSlots = HiveShockAdmission::EliteSlots(config.eliteReserve, kEliteWeight);
    if (!HiveShockAdmission::CanAdmitSlots(lane, CountLiveTrackedEnemies(), CountSceneEnemies(), GetMaxAlive(),
                                           GetMaxSceneEnemies(), eliteSlots)) {
        return "too many enemies alive or in the scene";
    }
    if (!HiveShockAdmission::CanAdmit(lane, def.weight, CurrentLoads(), config)) {
        return lane == HiveShockAdmission::Lane::Elite ? "the elite bank and the free room in the pool are full"
                                                       : "the load limit is reached";
    }
    return nullptr;
}

void TrackEnemy(Actor* actor, const SpawnDef& def, const std::string& user) {
    if (actor == nullptr) {
        return;
    }
    TrackedEnemy tracked;
    tracked.actor = actor;
    tracked.def = &def;
    tracked.user = user;
    tracked.spawnTick = gTick;
    tracked.weight = def.weight;
    gTrackedEnemies.push_back(std::move(tracked));
}

// Acción explícita del streamer: mata los enemigos inyectados vivos (también fairy/bombchu). Los spawns que esperan
// en la cola no se tocan.
void ClearBridgeEnemies() {
    for (TrackedEnemy& t : gTrackedEnemies) {
        if (TrackedStillLoaded(t)) {
            Actor_Kill(t.actor);
        }
    }
    gSlotsFreedThisTick += static_cast<u32>(gTrackedEnemies.size());
    gTrackedEnemies.clear();
    SPDLOG_INFO("HiveShock: cleared bridge enemies");
}

// Quita los enemigos inyectados vivos y los cuenta como derrotados, para que una recarga de escena no los reencole y
// los spawns en espera sigan su curso. Se usa cuando Link muere y en un cambio de zona (opciones del jugador).
void DefeatAliveEnemies(const char* reason) {
    int defeated = 0;
    for (TrackedEnemy& t : gTrackedEnemies) {
        if (TrackedStillLoaded(t)) {
            Actor_Kill(t.actor);
        }
        if (t.def != nullptr && IsCountedSpawnName(t.def->name)) {
            gStatDefeated++;
            defeated++;
        }
    }
    gSlotsFreedThisTick += static_cast<u32>(gTrackedEnemies.size());
    gTrackedEnemies.clear();
    if (defeated > 0) {
        SPDLOG_INFO("HiveShock: {}: {} alive enemies marked as defeated", reason, defeated);
    }
}

// Acción explícita del streamer: olvida los spawns que siguen esperando.
void ClearPendingSpawns() {
    SPDLOG_INFO("HiveShock: cleared {} pending spawns", gPendingSpawns.size());
    gPendingSpawns.clear();
    gBacklogDirty = true;
}

// La escena se va (o estamos en el título): todo lo inyectado que seguía vivo se reencola para reaparecer junto a
// Link y el seguimiento empieza limpio. No toca los punteros viejos. Con "Enemies disappear on zone change" no se
// reencolan: se quedan atrás y cuentan como derrotados.
void ResetBridgeActorTracking() {
    if (!gTrackedEnemies.empty()) {
        if (CVarGetInteger(CVAR_HS("DespawnOnZoneChange"), 0)) {
            int left = 0;
            for (const TrackedEnemy& t : gTrackedEnemies) {
                if (t.def != nullptr && IsCountedSpawnName(t.def->name)) {
                    left++;
                }
            }
            gStatDefeated += left;
            SPDLOG_INFO("HiveShock: scene change: {} alive enemies left behind, marked as defeated", left);
        } else {
            std::vector<TrackedEnemy> survivors;
            for (TrackedEnemy& t : gTrackedEnemies) {
                if (t.alive && t.def != nullptr && IsCountedSpawnName(t.def->name)) {
                    survivors.push_back(std::move(t));
                }
            }
            RequeueSurvivors(survivors);
        }
        gTrackedEnemies.clear();
    }
    gBridgeObjectSlots.clear();
    ResetHotPreload();
    gNextQueuedCommandTime = {};
    gNextCapacitySnapshot = {}; // fuerza un snapshot inmediato: la escena nueva arranca en 0 vivos
    gQueueReportForce = true;
    gSpawnUnblockTick = gTick + 45;
}

// Devuelve false solo si la cola está llena: el spawn nuevo se rechaza (los anteriores se conservan).
bool EnqueueEnemySpawn(std::string name, std::string user, s16 yawOffset, f32 radius, u32 delayTicks) {
    const bool counted = IsCountedSpawnName(name);
    if (gPendingSpawns.size() >= kMaxPendingSpawns) {
        SPDLOG_WARN("HiveShock: spawn backlog full ({}), rejecting '{}'", kMaxPendingSpawns, name);
        SendEventToBridge({ { "event", "spawn_rejected" }, { "action", name }, { "user", user } });
        return false;
    }
    PendingSpawn pending;
    pending.name = std::move(name);
    pending.user = std::move(user);
    pending.yawOffset = yawOffset;
    pending.radius = radius;
    pending.earliestTick = gTick + delayTicks;
    pending.createdAt = UnixNow();
    gPendingSpawns.push_back(std::move(pending));
    gBacklogDirty = true;
    if (counted) {
        gStatTotal++;
    }
    return true;
}

// Vanilla Actor_Spawn rechaza ACTORCAT_ENEMY cuando la sala ya está despejada (pueblos, salas de mazmorra
// terminadas, interiores "seguros"). Los efectos de multitud se saltan esa comprobación solo para el spawn; la sala
// sigue marcada como despejada después. El bit se alterna directamente (sin Flags_Set/UnsetClear) para que ningún
// hook de flags de escena (tracker del rando, etc.) vea un falso "sala despejada" por cada spawn.
Actor* SpawnActorInAnyRoom(s16 actorId, f32 x, f32 y, f32 z, s16 rotX, s16 rotY, s16 rotZ, s32 params) {
    s32 roomNum = gPlayState->roomCtx.curRoom.num;
    bool wasCleared = (roomNum >= 0) && Flags_GetClear(gPlayState, roomNum);
    if (wasCleared) {
        Flags_UnsetClear(gPlayState, roomNum);
    }

    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, x, y, z, rotX, rotY, rotZ, params);

    if (wasCleared) {
        Flags_SetClear(gPlayState, roomNum);
    }
    return spawned;
}

Vec3f SpawnPosInFront(Player* player, f32 distance, s16 yawOffset, f32 yOffset) {
    s16 yaw = player->actor.shape.rot.y + yawOffset;
    Vec3f pos;
    pos.x = player->actor.world.pos.x + (Math_SinS(yaw) * distance);
    pos.y = player->actor.world.pos.y + yOffset;
    pos.z = player->actor.world.pos.z + (Math_CosS(yaw) * distance);
    return pos;
}

// Elige un punto en el suelo alrededor de Link para este enemigo (delante de él primero). Siempre rellena pos;
// devuelve false solo si no se encontró suelo y hubo que usar la posición de Link. `radius` > 0 fuerza la distancia
// (cucco army en círculo); 0 = la habitual del tipo de enemigo.
bool FindSpawnPosition(const SpawnDef& def, Player* player, s16 yawOffset, f32 radius, Vec3f& pos) {
    pos = player->actor.world.pos;
    bool foundFloor = false;
    // Distancias de Link en orden de preferencia (gana la primera con suelo; las últimas son opciones cercanas para
    // salas pequeñas). Algunos enemigos solo despiertan cerca (Stalchild, Redead), así que aparecen cerca. Los
    // grandes (élite) empiezan más lejos y caminan hasta Link: lo detectan desde 200 unidades o más.
    static constexpr f32 kRadiiDefault[] = { 80.0f, 120.0f, 40.0f, 160.0f };
    static constexpr f32 kRadiiStalchild[] = { 45.0f, 35.0f, 50.0f, 30.0f };
    static constexpr f32 kRadiiRedead[] = { 135.0f, 115.0f, 150.0f, 100.0f, 80.0f };
    static constexpr f32 kRadiiElite[] = { 200.0f, 240.0f, 160.0f, 280.0f, 120.0f, 80.0f };
    f32 forced[3] = { radius, radius * 0.6f, radius * 1.5f };
    const f32* radii = kRadiiDefault;
    size_t radiiCount = ARRAY_COUNT(kRadiiDefault);
    if (radius > 0.0f) {
        radii = forced;
        radiiCount = ARRAY_COUNT(forced);
    } else if (def.actorId == ACTOR_EN_SKB) {
        radii = kRadiiStalchild;
        radiiCount = ARRAY_COUNT(kRadiiStalchild);
    } else if (def.actorId == ACTOR_EN_RD) {
        radii = kRadiiRedead;
        radiiCount = ARRAY_COUNT(kRadiiRedead);
    } else if (def.weight >= kEliteWeight) {
        radii = kRadiiElite;
        radiiCount = ARRAY_COUNT(kRadiiElite);
    }
    const s16 yawBase = player->actor.shape.rot.y + yawOffset; // delante de Link
    for (size_t r = 0; r < radiiCount && !foundFloor; r++) {
        const f32 dist = radii[r];
        for (s32 i = 0; i < 8 && !foundFloor; i++) {
            s16 yaw = yawBase + (s16)(i * 0x2000);
            Vec3f candidate;
            candidate.x = player->actor.world.pos.x + Math_SinS(yaw) * dist;
            candidate.y = player->actor.world.pos.y + 50.0f;
            candidate.z = player->actor.world.pos.z + Math_CosS(yaw) * dist;

            CollisionPoly* poly = nullptr;
            Vec3f rayPos = candidate;
            f32 floorY = BgCheck_EntityRaycastFloor1(&gPlayState->colCtx, &poly, &rayPos);
            if (floorY > BGCHECK_Y_MIN) {
                pos = candidate;
                pos.y = floorY;
                foundFloor = true;
            }
        }
    }
    if (!foundFloor) {
        // Último recurso: a los pies de Link para que el comando haga algo.
        pos = player->actor.world.pos;
    }
    pos.y += def.yOffset;
    return foundFloor;
}

ApplyResult SpawnDefNow(const SpawnDef& def, const std::string& user, s16 yawOffset, f32 radius,
                        const char** whyBlocked = nullptr) {
    if (gPlayState == nullptr) {
        return ApplyResult::Deferred;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return ApplyResult::Deferred;
    }
    // Sala cargándose: esperar en vez de spawnear en una escena a medio inicializar.
    if (gPlayState->roomCtx.status != 0) {
        return ApplyResult::Deferred;
    }
    // Sin sitio en este carril: esperar a que muera un enemigo en vez de quitar uno.
    if (const char* noRoom = NoRoomReason(def)) {
        if (whyBlocked != nullptr) {
            *whyBlocked = noRoom;
        }
        return ApplyResult::LaneFull;
    }

    ApplyResult objectResult = EnsureObjectLoaded(def.objectId);
    if (objectResult != ApplyResult::Applied) {
        return objectResult;
    }

    Vec3f pos;
    FindSpawnPosition(def, player, yawOffset, radius, pos);

    s16 faceYaw = Math_Vec3f_Yaw(&pos, &player->actor.world.pos);
    s32 params = def.params | kBridgeForceAggroParam;
    Actor* spawned = SpawnActorInAnyRoom(def.actorId, pos.x, pos.y, pos.z, def.rotX, faceYaw, 0, params);
    if (spawned == nullptr) {
        // Alternativa junto a Link si el punto elegido no vale para Actor_Spawn.
        pos = SpawnPosInFront(player, 40.0f, 0, def.yOffset);
        spawned = SpawnActorInAnyRoom(def.actorId, pos.x, pos.y, pos.z, def.rotX, faceYaw, 0, params);
    }
    if (spawned == nullptr) {
        SPDLOG_WARN("HiveShock: Actor_Spawn({}) returned NULL (actors={})", def.name,
                    gPlayState->actorCtx.totalLoadedActors);
        return ApplyResult::Skipped;
    }

    // Por si Init no reorientó: asegurar yaw hacia el jugador.
    faceYaw = Math_Vec3f_Yaw(&spawned->world.pos, &player->actor.world.pos);
    spawned->shape.rot.y = faceYaw;
    spawned->world.rot.y = faceYaw;

    AttachDonorNameTag(spawned, user);
    TrackEnemy(spawned, def, user);
    SPDLOG_INFO("HiveShock: spawned {} at ({:.1f}, {:.1f}, {:.1f})", def.name, pos.x, pos.y, pos.z);
    return ApplyResult::Applied;
}

// Mueve un enemigo atascado junto a Link. El enemigo NO se quita, así que el regalo sigue peleando.
void RelocateNearLink(TrackedEnemy& tracked, Player* player) {
    Vec3f pos;
    FindSpawnPosition(*tracked.def, player, 0, 0.0f, pos);
    Actor* actor = tracked.actor;
    actor->world.pos = pos;
    actor->prevPos = pos;
    actor->home.pos = pos;
    actor->velocity.x = 0.0f;
    actor->velocity.y = 0.0f;
    actor->velocity.z = 0.0f;
    actor->speed = 0.0f;
    SPDLOG_INFO("HiveShock: moved stuck {} next to Link", tracked.def->name);
}

// Mantenimiento por frame de los enemigos inyectados vivos: olvida los muertos (reencolando los perdidos por un
// cambio de sala), anota cuáles están muriendo y trae de vuelta a los que se quedaron lejos para que no retengan un
// hueco del presupuesto para siempre.
void UpdateTrackedEnemies() {
    PruneTrackedEnemies();
    if (gTrackedEnemies.empty() || gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    const bool checkStuck = player != nullptr && (gTick % kStuckCheckTicks) == 0 && !IsSceneChangeInProgress() &&
                            gPlayState->roomCtx.status == 0 && gPlayState->csCtx.state == CS_STATE_IDLE;

    for (TrackedEnemy& tracked : gTrackedEnemies) {
        Actor* actor = tracked.actor;
        if (actor->colChkInfo.health > 0) {
            tracked.sawHealth = true;
        } else if (tracked.sawHealth) {
            tracked.alive = false;
        }
        if (!checkStuck || !tracked.alive || !IsCountedSpawnName(tracked.def->name)) {
            continue;
        }

        // Se mide con las posiciones: xzDistToPlayer solo se refresca mientras el actor actualiza, cosa que un
        // enemigo lejano (descartado por distancia) no hace.
        const f32 dx = actor->world.pos.x - player->actor.world.pos.x;
        const f32 dy = actor->world.pos.y - player->actor.world.pos.y;
        const f32 dz = actor->world.pos.z - player->actor.world.pos.z;
        const bool tooFar = (dx * dx + dy * dy + dz * dz) > (kStuckDistance * kStuckDistance);
        if (!tooFar) {
            tracked.farSinceTick = 0;
        } else if (tracked.farSinceTick == 0) {
            tracked.farSinceTick = gTick;
        } else if (gTick - tracked.farSinceTick >= kStuckTicks) {
            RelocateNearLink(tracked, player);
            tracked.farSinceTick = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Persistencia de la cola: los spawns en espera se copian (con límite de frecuencia) a hiveshock_pending.json para
// que un crash o reinicio no pierda lo que pagaron los espectadores. Las entradas de más de kBacklogMaxAgeSeconds se
// descartan al cargar.
// ---------------------------------------------------------------------------------------------

std::filesystem::path BacklogPath() {
    return std::filesystem::path(Ship::Context::GetPathRelativeToAppDirectory(kBacklogFileName));
}

void SaveBacklog() {
    gBacklogDirty = false;
    gLastBacklogSaveTick = gTick;
    try {
        const std::filesystem::path path = BacklogPath();
        std::error_code ec;
        if (gPendingSpawns.empty()) {
            std::filesystem::remove(path, ec);
            return;
        }
        json spawns = json::array();
        for (const PendingSpawn& pending : gPendingSpawns) {
            spawns.push_back({ { "name", pending.name }, { "user", pending.user }, { "t", pending.createdAt } });
        }
        const json doc = { { "version", 1 }, { "spawns", spawns } };

        // Se escribe a un temporal y se renombra, así un crash a medias nunca deja una cola truncada.
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << doc.dump();
            if (!out.good()) {
                SPDLOG_WARN("HiveShock: could not write {}", tmp.string());
                return;
            }
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            SPDLOG_WARN("HiveShock: could not replace {}: {}", path.string(), ec.message());
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("HiveShock: saving the spawn backlog failed: {}", e.what());
    }
}

void LoadBacklog() {
    gBacklogLoaded = true;
    try {
        std::ifstream in(BacklogPath(), std::ios::binary);
        if (!in.good()) {
            return;
        }
        const json doc = json::parse(in);
        if (!doc.contains("spawns") || !doc["spawns"].is_array()) {
            return;
        }
        const s64 now = UnixNow();
        size_t restored = 0;
        for (const json& entry : doc["spawns"]) {
            if (gPendingSpawns.size() >= kMaxPendingSpawns || !entry.is_object()) {
                break;
            }
            PendingSpawn pending;
            pending.name = GetStringField(entry, "name");
            pending.user = GetStringField(entry, "user");
            pending.createdAt = entry.contains("t") && entry["t"].is_number_integer() ? entry["t"].get<s64>() : now;
            if (FindSpawnDef(pending.name) == nullptr || now - pending.createdAt > kBacklogMaxAgeSeconds) {
                continue;
            }
            pending.earliestTick = gTick;
            if (IsCountedSpawnName(pending.name)) {
                gStatTotal++;
            }
            gPendingSpawns.push_back(std::move(pending));
            restored++;
        }
        if (restored > 0) {
            SPDLOG_INFO("HiveShock: restored {} pending spawns from the previous session", restored);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("HiveShock: loading the spawn backlog failed: {}", e.what());
    }
    gBacklogDirty = false;
}

void TickBacklogPersistence() {
    if (gBacklogDirty && gTick - gLastBacklogSaveTick >= kBacklogSaveTicks) {
        SaveBacklog();
    }
}

// Mantiene al día los números vivos / en espera que lee la UI. La cola solo se recorre unas pocas veces por segundo.
void UpdateStatsSnapshot() {
    gStatAlive = CountLiveTrackedEnemies();
    gStatLoad = CurrentLoad();
    if (gTick % 5 != 0) {
        return;
    }
    int waiting = 0;
    int eliteWaiting = 0;
    for (const PendingSpawn& pending : gPendingSpawns) {
        if (IsCountedSpawnName(pending.name)) {
            waiting++;
            const SpawnDef* def = FindSpawnDef(pending.name);
            if (def != nullptr && def->weight >= kEliteWeight) {
                eliteWaiting++;
            }
        }
    }
    gStatWaiting = waiting;
    gStatEliteWaiting = eliteWaiting;

    const s32 reserve = GetEliteReserve();
    gStatEliteBankMax = reserve;
    gStatEliteBank = std::min(CurrentLoads().elite, reserve);
}

// Snapshot completo del presupuesto. Se manda al conectar un cliente (para resincronizar tras una caída de red) y
// cada kCapacitySnapshotIntervalSeconds mientras haya cliente conectado.
void SendCapacitySnapshot() {
    json event = { { "event", "capacity" },
                   { "used", CurrentLoad() },
                   { "max", GetMaxLoad() },
                   { "queued", static_cast<int>(gPendingSpawns.size()) } };
    SendEventToBridge(event);
}

// Junta todas las liberaciones de cupo de este frame (muertes, clear_enemies) en un solo evento, para no saturar el
// socket si mueren varios de golpe.
void FlushSlotFreedEvent() {
    if (gSlotsFreedThisTick == 0) {
        return;
    }
    json event = { { "event", "slot_freed" }, { "count", static_cast<int>(gSlotsFreedThisTick) } };
    SendEventToBridge(event);
    gSlotsFreedThisTick = 0;
}

void TickCapacityReporting() {
    if (!gBridgeClientConnected.load()) {
        gSlotsFreedThisTick = 0;
        return;
    }
    bool resync = gCapacityResyncPending.exchange(false);
    const auto now = Clock::now();
    if (resync || now >= gNextCapacitySnapshot) {
        SendCapacitySnapshot();
        gNextCapacitySnapshot = now + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<float>(kCapacitySnapshotIntervalSeconds));
    }
    FlushSlotFreedEvent();
}

// Avisa al companion de cuántos spawns esperan y cuánta carga hay (solo si cambia, como mucho cada ~0.5 s).
void TickQueueReport() {
    if (!gBridgeClientConnected.load()) {
        return;
    }
    if (!gQueueReportForce && gTick - gLastQueueReportTick < kQueueReportTicks) {
        return;
    }
    gLastQueueReportTick = gTick;
    const json report = { { "event", "spawn_queue" },
                          { "pending", gPendingSpawns.size() },
                          { "active", gTrackedEnemies.size() },
                          { "load", CurrentLoad() },
                          { "max", GetMaxLoad() },
                          { "eliteLoad", CurrentLoads().elite },
                          { "eliteReserve", GetEliteReserve() },
                          { "eliteWaiting", gStatEliteWaiting.load() } };
    const std::string serialized = report.dump();
    if (!gQueueReportForce && serialized == gLastQueueReport) {
        return;
    }
    gQueueReportForce = false;
    gLastQueueReport = serialized;
    SendEventToBridge(report);
}

void TickPendingSpawns() {
    if (gPlayState == nullptr || gPendingSpawns.empty()) {
        return;
    }
    if (IsPlayerUnableToFight() || gTick < gSpawnUnblockTick) {
        return;
    }
    // Un spawn por turno, separados kSpawnGapFrames, para que un relleno tras varias muertes no llegue de golpe.
    if (gTick < gNextSpawnTick) {
        return;
    }

    // Dos carriles. Con ElitePriority los élite (banco de ventaja, ver HiveShockAdmission.h) se miran primero y los
    // ordinarios después; sin ella ambos pasan por una sola cola FIFO. Dentro de un carril es primero en entrar,
    // primero en salir, y un carril lleno se detiene, así que su primer enemigo conserva el turno. Cuando un élite no
    // tiene sitio, el carril ordinario también se pausa: cada punto que se libera al morir enemigos ordinarios queda
    // reservado para el élite, acumulándose baja tras baja hasta que cabe, en lugar de rellenarse con el siguiente
    // lobo. Un bloqueo que afecta a todos (cutscene, sala cargando) termina el recorrido.
    using HiveShockAdmission::Lane;
    const bool elitePriority = CVarGetInteger(CVAR_HS("ElitePriority"), 1) != 0;
    bool laneBlocked[2] = { false, false };
    for (int pass = elitePriority ? 0 : 1; pass < 2; pass++) {
        if (pass == 1 && elitePriority && laneBlocked[static_cast<int>(Lane::Elite)]) {
            return; // el hueco liberado pertenece al élite que espera
        }

        for (auto it = gPendingSpawns.begin(); it != gPendingSpawns.end();) {
            if (gTick < it->earliestTick) {
                ++it;
                continue;
            }

            const SpawnDef* def = FindSpawnDef(it->name);
            if (def == nullptr) {
                SPDLOG_WARN("HiveShock: pending spawn unknown '{}', discarding", it->name);
                it = gPendingSpawns.erase(it);
                gBacklogDirty = true;
                continue;
            }
            const Lane lane = LaneOf(*def);
            if (elitePriority && lane != (pass == 0 ? Lane::Elite : Lane::Ordinary)) {
                ++it;
                continue;
            }
            if (laneBlocked[static_cast<int>(lane)]) {
                ++it;
                continue;
            }

            const char* whyBlocked = nullptr;
            switch (SpawnDefNow(*def, it->user, it->yawOffset, it->radius, &whyBlocked)) {
                case ApplyResult::Applied:
                    if (IsCountedSpawnName(def->name) && !it->requeued) {
                        gStatSpawned++;
                    }
                    gPendingSpawns.erase(it);
                    gBacklogDirty = true;
                    gNextSpawnTick = gTick + (def->weight <= 1 ? kSpawnGapFramesLight : kSpawnGapFrames);
                    return;
                case ApplyResult::Deferred:
                    return;
                case ApplyResult::LaneFull:
                    laneBlocked[static_cast<int>(lane)] = true;
                    if (lane == Lane::Elite && gTick - gLastEliteWaitLogTick >= kEliteWaitLogTicks) {
                        gLastEliteWaitLogTick = gTick;
                        SPDLOG_INFO("HiveShock: elite '{}' is waiting, freed room is reserved for it: {}", it->name,
                                    whyBlocked);
                    }
                    ++it;
                    break;
                case ApplyResult::Held:
                    it->earliestTick = gTick + kHeldRecheckFrames;
                    ++it;
                    break;
                case ApplyResult::Skipped:
                    // No se pudo colocar esta vez (sin suelo, límite de actores...). Se conserva y se reintenta pronto.
                    if (++it->attempts % 8 == 1) {
                        SPDLOG_WARN("HiveShock: could not place '{}' (attempt {}), will retry", it->name,
                                    it->attempts);
                    }
                    it->earliestTick = gTick + kFailedRetryFrames;
                    ++it;
                    break;
                case ApplyResult::Rejected:
                    it = gPendingSpawns.erase(it);
                    gBacklogDirty = true;
                    break;
            }
        }
    }
}

std::string NormalizeActionName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c == '-' || c == ' ') {
            out.push_back('_');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out.push_back(static_cast<char>(c));
        }
    }
    // Alias habituales del companion que no coinciden 1:1 con el nombre de la tabla.
    if (out == "likelike" || out == "like") {
        return "like_like";
    }
    if (out == "ironknuckle" || out == "knuckle") {
        return "iron_knuckle";
    }
    if (out == "garomaster") {
        return "garo_master";
    }
    if (out == "firekeese") {
        return "fire_keese";
    }
    if (out == "icekeese") {
        return "ice_keese";
    }
    if (out == "randomenemy" || out == "random") {
        return "random_enemy";
    }
    if (out == "cuccoarmy" || out == "cuccostorm" || out == "cucco_storm") {
        return "cucco_army";
    }
    if (out == "slowdown" || out == "slow_down") {
        return "slow";
    }
    if (out == "clear_pending") {
        return "clear_queue";
    }
    return out;
}

// Nombre canónico de lo que hay que spawnear si el comando es un spawn ("spawn" + enemy/name/actor/type incluido),
// o cadena vacía si no lo es.
std::string SpawnActionOf(const json& cmd) {
    std::string action = NormalizeActionName(GetStringField(cmd, "action"));
    if (action == "spawn" || action == "spawn_enemy" || action == "spawn_actor") {
        std::string enemy = GetStringField(cmd, "enemy");
        if (enemy.empty()) {
            enemy = GetStringField(cmd, "name");
        }
        if (enemy.empty()) {
            enemy = GetStringField(cmd, "actor");
        }
        if (enemy.empty()) {
            enemy = GetStringField(cmd, "type");
        }
        if (enemy.empty()) {
            return "";
        }
        action = NormalizeActionName(enemy);
    }
    if (action == "random_enemy" || action == "cucco_army" || FindSpawnDef(action) != nullptr) {
        return action;
    }
    return "";
}

ApplyResult SpawnNamed(const std::string& name, const std::string& user) {
    const SpawnDef* def = FindSpawnDef(name);
    if (def == nullptr) {
        return ApplyResult::Skipped;
    }
    return EnqueueEnemySpawn(def->name, user, 0, 0.0f, 0) ? ApplyResult::Applied : ApplyResult::Skipped;
}

ApplyResult SpawnRandomEnemy(const std::string& user) {
    s32 count = ARRAY_COUNT(kRandomEnemies);
    s32 index = (s32)(Rand_ZeroOne() * count);
    if (index >= count) {
        index = count - 1;
    }
    return SpawnNamed(kRandomEnemies[index], user);
}

ApplyResult SpawnCuccoArmy(const std::string& user) {
    if (FindSpawnDef("cucco") == nullptr) {
        return ApplyResult::Skipped;
    }
    constexpr s32 kCount = 6;
    for (s32 i = 0; i < kCount; i++) {
        s16 yawOffset = (s16)((0x10000 / kCount) * i);
        if (!EnqueueEnemySpawn("cucco", user, yawOffset, 70.0f, (u32)(i * kCuccoArmyFrameGap))) {
            return ApplyResult::Skipped;
        }
    }
    return ApplyResult::Applied;
}

// Comandos que no dependen de que Link esté jugando: los spawns solo entran en la cola (TickPendingSpawns decide
// cuándo y si entran al juego) y clear_queue la vacía. Así un spawn que debe esperar nunca bloquea a los comandos
// de detrás, y funcionan también en el título o en una cutscene. Devuelve false si el comando no es de este tipo.
bool ApplyEarlyCommand(const json& cmd, ApplyResult& result) {
    const std::string action = SpawnActionOf(cmd);
    if (!action.empty()) {
        LogUser(cmd, action);
        const std::string user = TruncateDonorName(GetStringField(cmd, "user"));
        if (action == "random_enemy") {
            result = SpawnRandomEnemy(user);
        } else if (action == "cucco_army") {
            result = SpawnCuccoArmy(user);
        } else {
            result = SpawnNamed(action, user);
        }
        return true;
    }
    if (NormalizeActionName(GetStringField(cmd, "action")) == "clear_queue") {
        ClearPendingSpawns();
        result = ApplyResult::Applied;
        return true;
    }
    return false;
}

// Saca de la cola de comandos los de ApplyEarlyCommand y los aplica, esté Link jugando o no.
void DrainEarlyCommands() {
    std::vector<json> early;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        for (auto it = gCommandQueue.begin(); it != gCommandQueue.end();) {
            const std::string action = NormalizeActionName(GetStringField(*it, "action"));
            if (action == "clear_queue" || !SpawnActionOf(*it).empty()) {
                early.push_back(std::move(*it));
                it = gCommandQueue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const json& cmd : early) {
        ApplyResult result = ApplyResult::Skipped;
        ApplyEarlyCommand(cmd, result);
    }
}

void ApplyKeg(Player* player, const std::string& user) {
    if (gSaveContext.powderKegTimer == 0) {
        gSaveContext.powderKegTimer = 200;
    }
    Vec3f pos = SpawnPosInFront(player, kEnemySpawnDistance, 0, 0.0f);
    Actor* spawned =
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_BOM, pos.x, pos.y, pos.z, 1, player->actor.shape.rot.y,
                    0, 0);
    if (spawned == nullptr) {
        SPDLOG_WARN("HiveShock: failed to spawn powder keg");
        return;
    }
    AttachDonorNameTag(spawned, user);
}

void ApplyDeleteSave() {
    if (gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2) {
        std::string fileName = SaveManager_GetFileName(gSaveContext.fileNum + 1, false);
        std::string backupFileName = SaveManager_GetFileName(gSaveContext.fileNum + 1, true);
        SaveManager_DeleteSaveFile(fileName);
        SaveManager_DeleteSaveFile(backupFileName);
        SPDLOG_INFO("HiveShock: deleted save slot {}", gSaveContext.fileNum + 1);
    } else {
        SPDLOG_WARN("HiveShock: delete_save ignored (invalid fileNum {})", gSaveContext.fileNum);
    }

    gDeathCount = 0;
    gDeathCountedThisCycle = false;
    NotifyDeathsReset("save_deleted");

    auto console = std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"));
    if (console != nullptr) {
        console->Dispatch("reset");
    } else {
        SPDLOG_WARN("HiveShock: Console window missing; could not reset to file select");
    }
}

void ApplyBlast(Player* player, const json& cmd) {
    s16 damage = HitDamageFromCmd(cmd, 16.0f);
    float strength = 6.0f;
    if (cmd.contains("strength") && cmd["strength"].is_number()) {
        strength = std::clamp(cmd["strength"].get<float>(), 0.5f, 14.0f);
    }
    // hitType 2 = blast; velocidad para que se vea el empujón de explosión.
    ApplyPlayerHit(player, 2, damage, strength * 1.2f, strength * 1.5f, player->actor.shape.rot.y + 0x8000, 20);
    // rotX & 1 with timer 0 → keg that detonates immediately
    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_BOM, player->actor.world.pos.x,
                                 player->actor.world.pos.y, player->actor.world.pos.z, 1, 0, 0, 0);
    if (spawned == nullptr) {
        SPDLOG_WARN("HiveShock: failed to spawn blast keg (actors={})", gPlayState->actorCtx.totalLoadedActors);
    }
}

void ApplyRupeeRain(Player* player) {
    int16_t currentRupees = gSaveContext.save.saveInfo.playerData.rupees;
    if (currentRupees <= 0) {
        SPDLOG_INFO("HiveShock: rupee_rain skipped, wallet empty");
        return;
    }

    Vec3f positional = player->actor.world.pos;
    positional.y += 100.0f;
    // Esto redistribuye la billetera del propio jugador como espectáculo visual (no agrega valor
    // nuevo), así que topar cuántas monedas caen no le cuesta nada al donante: lo que no llega a
    // caer se queda intacto en la billetera. Evita que 500 rupias spawneen 25 actores de golpe.
    s32 pickupsSpawned = 0;
    while (currentRupees > 0 && pickupsSpawned < kMaxItemPickupsPerCommand) {
        s16 drop;
        s16 amount;
        if (currentRupees >= 20) {
            drop = ITEM00_RUPEE_RED;
            amount = 20;
        } else if (currentRupees >= 5) {
            drop = ITEM00_RUPEE_BLUE;
            amount = 5;
        } else {
            drop = ITEM00_RUPEE_GREEN;
            amount = 1;
        }
        Rupees_ChangeBy(-amount);
        currentRupees -= amount;
        pickupsSpawned++;
        EnItem00* rupeeActor = (EnItem00*)Item_DropCollectible(gPlayState, &positional, drop);
        if (rupeeActor != nullptr) {
            rupeeActor->actor.speed = Rand_CenteredFloat(5.0f);
            rupeeActor->unk152 = 600;
        }
    }
}

void ApplyHearts(Player* player) {
    Vec3f positional = player->actor.world.pos;
    positional.y += 60.0f;
    for (s32 i = 0; i < 5; i++) {
        EnItem00* heart = (EnItem00*)Item_DropCollectible(gPlayState, &positional, ITEM00_RECOVERY_HEART);
        if (heart != nullptr) {
            heart->actor.speed = Rand_CenteredFloat(6.0f);
            heart->unk152 = 600;
        }
    }
}

void ApplyInvert(const json& cmd) {
    float duration = GetDuration(cmd);
    if (duration <= 0.0f) {
        duration = 20.0f;
    }
    duration = std::clamp(duration, 1.0f, 120.0f);
    gInvertControls = true;
    gInvertUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(duration));
    SPDLOG_INFO("HiveShock: inverted controls for {:.1f}s", duration);
}

void ApplyTimeSkip() {
    if (gPlayState == nullptr) {
        return;
    }

    // +4000 = +6 h de reloj del juego (misma cantidad que TRAP_TIME en Rando).
    constexpr u16 kTimeSkipInterval = 4000;
    u16 previousTime = gSaveContext.save.time;
    u16 newTime = previousTime + kTimeSkipInterval;

    // UpdateGameTime sincroniza En_Test4 y evita transiciones; hay que avanzar el día
    // manualmente al cruzar el amanecer (igual que Traps.cpp).
    if (previousTime < MORNING_TIME && newTime >= MORNING_TIME) {
        if (gSaveContext.save.day != 3) {
            gSaveContext.save.day++;
            gSaveContext.save.eventDayCount++;
            UpdateGameTime(newTime);
            Interface_NewDay(gPlayState, CURRENT_DAY);
            Environment_NewDay(&gPlayState->envCtx);
            gWeatherMode = WEATHER_MODE_CLEAR;
            gPlayState->envCtx.lightningState = LIGHTNING_OFF;
        } else {
            // Día 3: no saltar más allá del amanecer final (evita bucle / saltar la luna).
            UpdateGameTime(MORNING_TIME - (kTimeSkipInterval / 10));
        }
    } else {
        UpdateGameTime(newTime);
    }

    TransitionFade_SetColor(&gPlayState->unk_18E48, 0x000000);
    R_TRANS_FADE_FLASH_ALPHA_STEP = -1;
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr) {
        Player_PlaySfx(player, NA_SE_SY_TRANSFORM_MASK_FLASH);
    }
    SPDLOG_INFO("HiveShock: time skipped (day={}, time=0x{:04X})", gSaveContext.save.day, gSaveContext.save.time);
}

void ApplyTimeUp() {
    if (gPlayState == nullptr) {
        return;
    }

    gPendingMoonCrash = true;
    SPDLOG_INFO("HiveShock: time_up queued (moon CS -> salesman + cycle reset)");
}

void ApplyJinx(const json& cmd) {
    float duration = GetDuration(cmd);
    if (duration <= 0.0f) {
        duration = 60.0f;
    }
    duration = std::clamp(duration, 1.0f, 120.0f);
    gSaveContext.jinxTimer = static_cast<u16>(duration * 20.0f);
    if (gPlayState != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        if (player != nullptr) {
            Actor_PlaySfx(&player->actor, NA_SE_EN_BUBLE_BITE);
        }
    }
}

ApplyResult ApplyCommand(const json& cmd) {
    ApplyResult earlyResult = ApplyResult::Skipped;
    if (ApplyEarlyCommand(cmd, earlyResult)) {
        return earlyResult;
    }

    if (gPlayState == nullptr) {
        return ApplyResult::Deferred;
    }

    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return ApplyResult::Deferred;
    }

    std::string action = NormalizeActionName(GetStringField(cmd, "action"));
    if (action.empty()) {
        SPDLOG_WARN("HiveShock: command missing 'action'");
        return ApplyResult::Skipped;
    }

    // Rescue must apply during grab CS / pause-like combat-lock. Only wait on a real fade.
    if (action == "rescue") {
        if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF) {
            return ApplyResult::Deferred;
        }
        LogUser(cmd, action);
        RescuePlayer();
        return ApplyResult::Applied;
    }

    // Door/fade already set nextEntrance. Impulse, wallmaster grab, or void-out
    // would overwrite it with the last entrance (Deku balcony → Stock Pot Inn).
    if (IsSceneChangeInProgress()) {
        return ApplyResult::Deferred;
    }

    if (IsDangerousAction(action) && ShouldDeferDangerousEffects()) {
        return ApplyResult::Deferred;
    }

    LogUser(cmd, action);
    std::string user = GetStringField(cmd, "user");

    if (action == "impulse") {
        ApplyImpulse(player, cmd);
    } else if (action == "knockback") {
        ApplyKnockback(player, cmd);
    } else if (action == "freeze") {
        ApplyFreeze(player, cmd);
    } else if (action == "shock") {
        ApplyShock(player, cmd);
    } else if (action == "fire") {
        ApplyFire(player, cmd);
    } else if (action == "heal") {
        ApplyHeal(cmd);
    } else if (action == "damage") {
        ApplyDamage(player, cmd);
    } else if (action == "insta_kill" || action == "instakill" || action == "kill") {
        ApplyInstaKill();
    } else if (action == "rupees") {
        ApplyRupees(cmd);
    } else if (action == "magic") {
        ApplyMagic(cmd);
    } else if (action == "bomb") {
        ApplyBomb(player);
    } else if (action == "freeze_time") {
        FreezeTime(GetDuration(cmd));
    } else if (action == "resume_time") {
        ResumeTime();
    } else if (action == "jinx") {
        ApplyJinx(cmd);
    } else if (action == "slow") {
        ApplySlowDebuff(cmd);
    } else if (action == "keg") {
        ApplyKeg(player, user);
    } else if (action == "blast" || action == "explode" || action == "bomb_explode") {
        ApplyBlast(player, cmd);
    } else if (action == "rupee_rain") {
        ApplyRupeeRain(player);
    } else if (action == "hearts") {
        ApplyHearts(player);
    } else if (action == "invert") {
        ApplyInvert(cmd);
    } else if (action == "speed") {
        ApplySpeedBuff(cmd);
    } else if (action == "invincible" || action == "invencible") {
        ApplyInvincibleBuff(cmd);
    } else if (action == "infinite_magic") {
        ApplyInfiniteMagicBuff(cmd);
    } else if (action == "damage_up") {
        ApplyDamageUpBuff(cmd);
    } else if (action == "haste") {
        ApplyHasteBuff(cmd);
    } else if (IsOwlWarpAction(action)) {
        return ApplyOwlWarp(player, cmd, action);
    } else if (action == "time_skip") {
        ApplyTimeSkip();
    } else if (action == "time_up" || action == "times_up" || action == "time_zero") {
        ApplyTimeUp();
    } else if (action == "delete_save" || action == "new_run") {
        ApplyDeleteSave();
    } else if (action == "clear_enemies") {
        ClearBridgeEnemies();
    } else {
        SPDLOG_WARN("HiveShock: unknown action '{}'", action);
        return ApplyResult::Skipped;
    }

    return ApplyResult::Applied;
}

// Cola serial: 1 efecto cada ~0.18s (reloj real). En pausa solo se acumulan.
// Los Deferred se reencolan y se prueba el siguiente del lote (p.ej. heal tras knockback diferido).
void DrainQueuedCommands() {
    if (gPlayState == nullptr) {
        return;
    }
    if (IS_PAUSED(&gPlayState->pauseCtx)) {
        return;
    }

    const auto now = Clock::now();
    if (now < gNextQueuedCommandTime) {
        return;
    }

    std::deque<json> deferred;
    bool consumedSlot = false;

    json cmd;
    while (PopCommand(cmd)) {
        ApplyResult result = ApplyCommand(cmd);
        if (result == ApplyResult::Deferred) {
            deferred.push_back(std::move(cmd));
            continue;
        }
        // Applied o Skipped: cuenta como un slot de la cola serial.
        consumedSlot = true;
        break;
    }

    for (auto it = deferred.rbegin(); it != deferred.rend(); ++it) {
        PushDeferredCommand(std::move(*it));
    }

    if (consumedSlot) {
        gNextQueuedCommandTime =
            now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kSecondsBetweenQueuedCommands));
    } else if (!deferred.empty()) {
        // Solo diferidos: reintentar el próximo tick sin esperar el gap largo.
        gNextQueuedCommandTime = now;
    }
}

} // namespace

extern "C" int HiveShock_ShouldBlockHealthLoss(void) {
    return ShouldBlockHealthLoss() ? 1 : 0;
}

extern "C" int HiveShock_OverrideColorFilter(Actor* actor, void* colorRgba8) {
    return OverrideColorFilter(actor, static_cast<Color_RGBA8*>(colorRgba8)) ? 1 : 0;
}

extern "C" int HiveShock_GetListenEndpoint(char* out, int outSize) {
    std::string endpoint = FormatListenEndpoint();
    if (out != nullptr && outSize > 0) {
        std::snprintf(out, static_cast<size_t>(outSize), "%s", endpoint.c_str());
    }
    return gPort.load();
}

extern "C" void HiveShock_GetCapacityStatus(int* used, int* max, int* queued) {
    if (used != nullptr) {
        *used = gStatLoad.load();
    }
    if (max != nullptr) {
        *max = GetMaxLoad();
    }
    if (queued != nullptr) {
        *queued = gStatWaiting.load();
    }
}

HiveShock* HiveShock::Instance = nullptr;

void HiveShock::Enable() {
    CVarSetInteger(CVAR_NAME, 1);
    StartServer();
}

void HiveShock::Disable() {
    CVarSetInteger(CVAR_NAME, 0);
    StopServer();
}

bool HiveShock::IsEnabled() const {
    return gServerRun.load();
}

bool HiveShock::IsListening() const {
    return gListening.load();
}

bool HiveShock::HasClient() const {
    return gBridgeClientConnected.load();
}

std::string HiveShock::GetListenEndpoint() const {
    return FormatListenEndpoint();
}

HiveShock::Stats HiveShock::GetStats() const {
    Stats stats;
    stats.total = gStatTotal.load();
    stats.spawned = gStatSpawned.load();
    stats.defeated = gStatDefeated.load();
    stats.alive = gStatAlive.load();
    stats.waiting = gStatWaiting.load();
    stats.eliteBank = gStatEliteBank.load();
    stats.eliteBankMax = gStatEliteBankMax.load();
    stats.eliteWaiting = gStatEliteWaiting.load();
    return stats;
}

void HiveShock::ResetStats() {
    const int alive = gStatAlive.load();
    gStatDefeated = 0;
    gStatSpawned = alive;
    gStatTotal = alive + gStatWaiting.load();
}

void RegisterHiveShock() {
    static HiveShock sInstance;
    HiveShock::Instance = &sInstance;

    MigrateHiveShockCVar();
    if (CVAR) {
        StartServer();
    } else {
        StopServer();
        return;
    }

    // OnGameStateUpdate corre en pausa (la cola solo acumula) y al jugar drena 1 efecto por intervalo.
    // No usar OnActorUpdate: en pausa los actors no actualizan y al reanudar se vaciaba todo el lote.
    COND_HOOK(OnGameStateUpdate, CVAR, []() {
        gTick++;
        RefreshToken();
        TickTimedEffects();
        // Los spawns y clear_queue entran a la cola aunque Link no esté jugando (título, pausa, cutscene).
        DrainEarlyCommands();
        DrainQueuedCommands();

        if (gPlayState == nullptr) {
            // Título / selección de archivo: los enemigos vivos ya no están, pero ellos y los spawns que pagaron los
            // espectadores siguen en la cola.
            gLastRoom = -1;
            ResetBridgeActorTracking();
            UpdateStatsSnapshot();
            TickQueueReport();
            TickBacklogPersistence();
            return;
        }

        const s16 room = gPlayState->roomCtx.curRoom.num;
        if (room != gLastRoom) {
            if (gLastRoom != -1) {
                gRoomGraceUntilTick = gTick + kRoomGraceTicks;
                // Antes del mantenimiento de abajo, para que la ventana de gracia no los reencole.
                if (CVarGetInteger(CVAR_HS("DespawnOnZoneChange"), 0)) {
                    DefeatAliveEnemies("zone change");
                }
            }
            gLastRoom = room;
        }

        TickObjectPreload();
        UpdateTrackedEnemies();
        TickPendingSpawns();
        UpdateStatsSnapshot();
        TickCapacityReporting();
        TickQueueReport();
        TickBacklogPersistence();

        auto state = gPlayState->gameOverCtx.state;
        if (state == GAMEOVER_DEATH_WAIT_GROUND && !gDeathCountedThisCycle) {
            RegisterDeath("player death");
            if (CVarGetInteger(CVAR_HS("DefeatOnDeath"), 0)) {
                DefeatAliveEnemies("Link died");
            }
        } else if (state == GAMEOVER_INACTIVE) {
            gDeathCountedThisCycle = false;
        }
    });

    COND_HOOK(OnPassPlayerInputs, CVAR, [](Input* input) {
        if (!gInvertControls) {
            return;
        }
        input->cur.stick_x = static_cast<int8_t>(-input->cur.stick_x);
        input->cur.stick_y = static_cast<int8_t>(-input->cur.stick_y);
        input->rel.stick_x = static_cast<int8_t>(-input->rel.stick_x);
        input->rel.stick_y = static_cast<int8_t>(-input->rel.stick_y);
    });

    COND_VB_SHOULD(VB_SPEED_MODIFIER_WALK, CVAR, {
        if (!IsBuffActive(BridgeBuff::Speed)) {
            return;
        }
        f32* speedTarget = va_arg(args, f32*);
        *speedTarget *= GetBuffStrength(BridgeBuff::Speed, kDefaultSpeedMult);
    });

    COND_VB_SHOULD(VB_SPEED_MODIFIER_SWIM, CVAR, {
        if (!IsBuffActive(BridgeBuff::Speed)) {
            return;
        }
        f32* incrStep = va_arg(args, f32*);
        f32* maxSpeed = va_arg(args, f32*);
        f32* speed = va_arg(args, f32*);
        f32* speedTarget = va_arg(args, f32*);
        (void)incrStep;
        (void)speed;
        f32 mult = GetBuffStrength(BridgeBuff::Speed, kDefaultSpeedMult);
        *maxSpeed *= mult;
        *speedTarget *= mult;
    });

    COND_VB_SHOULD(VB_DAMAGE_MULTIPLIER, CVAR, {
        if (!IsBuffActive(BridgeBuff::DamageUp)) {
            return;
        }
        (void)va_arg(args, int);
        (void)va_arg(args, DamageTable*);
        f32* damage = va_arg(args, f32*);
        (void)va_arg(args, f32*);
        Actor* ac = va_arg(args, Actor*);
        if (ac == nullptr || ac->id == ACTOR_PLAYER) {
            return;
        }
        if (ac->category != ACTORCAT_ENEMY && ac->category != ACTORCAT_BOSS) {
            return;
        }
        *damage *= GetBuffStrength(BridgeBuff::DamageUp, kDefaultDamageMult);
    });

    COND_HOOK(OnSceneInit, CVAR, [](s8 /*sceneId*/, s8 /*spawnNum*/) {
        gLastRoom = -1;
        ResetBridgeActorTracking();
    });
}

static RegisterShipInitFunc initFunc(RegisterHiveShock, { CVAR_NAME, CVAR_NAME_LEGACY });
