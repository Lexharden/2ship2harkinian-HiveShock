#include "PlayerRescue.h"

#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace {

constexpr u32 kSnapshotIntervalFrames = 10;
constexpr u32 kStuckFramesNeeded = 60;       // ~3 s embebido/void
constexpr u32 kGrabStuckFramesNeeded = 20;   // ~1 s grab + geometría
constexpr u32 kGrabTimeoutFrames = 140;      // vanilla Like Like traga 100 frames; si no suelta, se quedó bugeado
constexpr u32 kGrabberDeadFrames = 8;        // el que te tiene ya no actualiza
constexpr u32 kRescueCooldownFrames = 40;
constexpr f32 kEmbedRadius = 8.0f;
constexpr f32 kChestHeight = 35.0f;
constexpr f32 kNearbyGrabberDist = 200.0f;

struct RescueSnapshot {
    Vec3f pos{};
    s16 yaw = 0;
    s16 sceneId = -1;
    s8 room = -1;
    bool valid = false;
};

RescueSnapshot gSnapshot;
u32 gStuckFrames = 0;
u32 gRescueCooldown = 0;
u32 gLastRescueTickFrame = 0xFFFFFFFF;
s16 gStuckSceneId = -1;
s8 gStuckRoom = -1;

bool IsPlayerOnSwampBoat(PlayState* play) {
    if (play == nullptr) {
        return false;
    }
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_BG].first; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_BG_INGATE && actor->update != nullptr &&
            DynaPolyActor_IsPlayerOnTop(reinterpret_cast<DynaPolyActor*>(actor))) {
            return true;
        }
    }
    return false;
}

bool InExcludedMovement(PlayState* play, Player* player) {
    if (player->stateFlags1 &
        (PLAYER_STATE1_4 | PLAYER_STATE1_800000 | PLAYER_STATE1_8000000 | PLAYER_STATE1_DEAD)) {
        return true;
    }
    if (player->stateFlags2 & PLAYER_STATE2_80000000) {
        return true;
    }
    if (player->stateFlags3 & (PLAYER_STATE3_100 | PLAYER_STATE3_FLYING_WITH_HOOKSHOT | PLAYER_STATE3_8000)) {
        return true;
    }
    if (play->soaringCsOrSoTCsPlaying) {
        return true;
    }
    if (IsPlayerOnSwampBoat(play)) {
        return true;
    }
    return false;
}

bool IsPlayerEmbedded(PlayState* play, Player* player) {
    Vec3f chest = player->actor.world.pos;
    chest.y += kChestHeight;
    return BgCheck_SphVsFirstWall(&play->colCtx, &chest, kEmbedRadius) != 0;
}

bool IsVoided(Player* player) {
    return player->actor.floorHeight <= BGCHECK_Y_MIN + 1.0f;
}

bool IsStuckInGeometry(PlayState* play, Player* player) {
    return IsPlayerEmbedded(play, player) || IsVoided(player);
}

// Enemies that swallow/grab Link and can pin him in collision. Add IDs here.
bool IsTrapGrabber(s16 actorId) {
    return actorId == ACTOR_EN_RR || actorId == ACTOR_EN_WALLMAS || actorId == ACTOR_EN_FLOORMAS;
}

bool IsEnemyGrab(Player* player) {
    Actor* parent = player->actor.parent;
    return parent != nullptr && (parent->category == ACTORCAT_ENEMY || IsTrapGrabber(parent->id));
}

bool IsGrabberDead(Player* player) {
    Actor* parent = player->actor.parent;
    return IsEnemyGrab(player) && parent->update == nullptr;
}

// 0 = no está atascado. Si no, frames seguidos necesarios para auto-rescate.
u32 AutoRescueThreshold(PlayState* play, Player* player) {
    if (IsGrabberDead(player)) {
        return kGrabberDeadFrames;
    }
    bool geometry = IsStuckInGeometry(play, player);
    bool grab = IsEnemyGrab(player);
    if (grab && geometry) {
        return kGrabStuckFramesNeeded;
    }
    if (geometry) {
        return kStuckFramesNeeded;
    }
    if (grab) {
        // Tragado que nunca termina (Like Like en pared, CS colgada, etc.)
        return kGrabTimeoutFrames;
    }
    return 0;
}

bool SphereFitsAt(PlayState* play, Vec3f pos) {
    pos.y += kChestHeight;
    return BgCheck_SphVsFirstWall(&play->colCtx, &pos, kEmbedRadius) == 0;
}

void KillNearbyGrabbers(PlayState* play, Vec3f dest) {
    Actor* actor = play->actorCtx.actorLists[ACTORCAT_ENEMY].first;
    while (actor != nullptr) {
        Actor* next = actor->next;
        if (IsTrapGrabber(actor->id) && Math_Vec3f_DistXZ(&actor->world.pos, &dest) < kNearbyGrabberDist) {
            Actor_Kill(actor);
        }
        actor = next;
    }
}

bool PickRescueDest(PlayState* play, Player* player, Vec3f* outPos, s16* outYaw) {
    if (gSnapshot.valid && gSnapshot.sceneId == play->sceneId && gSnapshot.room == play->roomCtx.curRoom.num) {
        *outPos = gSnapshot.pos;
        *outYaw = gSnapshot.yaw;
        return true;
    }

    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f probe = player->actor.world.pos;
    f32 floorY = BgCheck_EntityRaycastFloor5(&play->colCtx, &poly, &bgId, &player->actor, &probe);
    if (floorY > BGCHECK_Y_MIN + 1.0f) {
        Vec3f candidate = probe;
        candidate.y = floorY + 1.0f;
        if (SphereFitsAt(play, candidate)) {
            *outPos = candidate;
            *outYaw = player->yaw;
            return true;
        }
    }

    RespawnData* down = &gSaveContext.respawn[RESPAWN_MODE_DOWN];
    if (down->entrance != 0xFFFF && Entrance_GetSceneIdAbsolute(down->entrance) == play->sceneId) {
        *outPos = down->pos;
        *outYaw = down->yaw;
        return true;
    }

    return false;
}

void TickSnapshot(PlayState* play, Player* player) {
    if ((play->gameplayFrames % kSnapshotIntervalFrames) != 0) {
        return;
    }
    if (!(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND)) {
        return;
    }
    if (IsVoided(player) || player->actor.parent != nullptr) {
        return;
    }
    if (Player_InBlockingCsMode(play, player) || InExcludedMovement(play, player)) {
        return;
    }
    if (IS_PAUSED(&play->pauseCtx)) {
        return;
    }
    if (IsPlayerEmbedded(play, player)) {
        return;
    }

    gSnapshot.pos = player->actor.world.pos;
    gSnapshot.yaw = player->yaw;
    gSnapshot.sceneId = play->sceneId;
    gSnapshot.room = play->roomCtx.curRoom.num;
    gSnapshot.valid = true;
}

void TickAutoRescue(PlayState* play, Player* player) {
    if (gRescueCooldown > 0) {
        gRescueCooldown--;
        gStuckFrames = 0;
        return;
    }
    if (IS_PAUSED(&play->pauseCtx) || play->transitionTrigger != TRANS_TRIGGER_OFF ||
        play->transitionMode != TRANS_MODE_OFF) {
        gStuckFrames = 0;
        return;
    }

    s16 sceneId = play->sceneId;
    s8 room = play->roomCtx.curRoom.num;
    if (sceneId != gStuckSceneId || room != gStuckRoom) {
        gStuckSceneId = sceneId;
        gStuckRoom = room;
        gStuckFrames = 0;
    }

    if (InExcludedMovement(play, player)) {
        gStuckFrames = 0;
        return;
    }

    u32 needed = AutoRescueThreshold(play, player);
    if (needed == 0) {
        gStuckFrames = 0;
        return;
    }

    gStuckFrames++;
    if (gStuckFrames >= needed) {
        SPDLOG_INFO("PlayerRescue: auto-rescue after {} frames (need {}, grab={}, geom={})", gStuckFrames, needed,
                    IsEnemyGrab(player), IsStuckInGeometry(play, player));
        gStuckFrames = 0;
        RescuePlayer();
    }
}

void TickPlayerRescue() {
    if (gPlayState == nullptr) {
        gSnapshot.valid = false;
        gStuckFrames = 0;
        gLastRescueTickFrame = 0xFFFFFFFF;
        return;
    }
    if (gPlayState->gameplayFrames == gLastRescueTickFrame) {
        return;
    }
    gLastRescueTickFrame = gPlayState->gameplayFrames;

    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }
    TickSnapshot(gPlayState, player);
    TickAutoRescue(gPlayState, player);
}

} // namespace

bool RescuePlayer() {
    if (gPlayState == nullptr) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return false;
    }

    Actor* parent = player->actor.parent;
    player->actor.parent = nullptr;
    Player_SetCsAction(gPlayState, &player->actor, PLAYER_CSACTION_END);
    player->speedXZ = 0.0f;
    player->actor.speed = 0.0f;
    player->actor.velocity.x = 0.0f;
    player->actor.velocity.y = 0.0f;
    player->actor.velocity.z = 0.0f;

    Vec3f dest;
    s16 yaw = player->yaw;
    if (!PickRescueDest(gPlayState, player, &dest, &yaw)) {
        SPDLOG_WARN("PlayerRescue: no safe destination in this scene");
        return false;
    }

    player->actor.world.pos = dest;
    player->actor.prevPos = dest;
    player->actor.shape.rot.y = yaw;
    player->actor.world.rot.y = yaw;
    player->yaw = yaw;
    player->actor.floorHeight = dest.y;
    player->actor.bgCheckFlags |= BGCHECKFLAG_GROUND;

    if (parent != nullptr && (parent->category == ACTORCAT_ENEMY || IsTrapGrabber(parent->id))) {
        Actor_Kill(parent);
    }
    KillNearbyGrabbers(gPlayState, dest);

    gStuckFrames = 0;
    gRescueCooldown = kRescueCooldownFrames;
    SPDLOG_INFO("PlayerRescue: rescued to ({:.1f}, {:.1f}, {:.1f})", dest.x, dest.y, dest.z);
    return true;
}

void RegisterPlayerRescue() {
    // OnGameStateUpdate corre aunque Link esté en CS de grab (el update del player a veces no).
    COND_HOOK(OnGameStateUpdate, true, []() { TickPlayerRescue(); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_PLAYER, true, [](Actor* /*actor*/) { TickPlayerRescue(); });
}

static RegisterShipInitFunc initFunc(RegisterPlayerRescue, {});
