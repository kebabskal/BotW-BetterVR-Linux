#include "pch.h"

#include "cemu_hooks.h"
#include "instance.h"
#include "utils/debug_draw.h"
#include "utils/game_utils.h"

enum RoomscaleHookResult : uint32_t {
    RoomscaleHookResult_Skip = 0,
    RoomscaleHookResult_Cast = 1,
    RoomscaleHookResult_Warp = 2,
};

enum RoomscaleQueryType : uint32_t {
    RoomscaleQueryType_Sweep = 0,
    RoomscaleQueryType_GroundProbe = 1,
    RoomscaleQueryType_SupportProbe = 2,
};

enum RoomscaleResolvePhase : uint32_t {
    RoomscaleResolvePhase_ForwardSweep = 0,
    RoomscaleResolvePhase_DownSupportProbe = 1,
    RoomscaleResolvePhase_StepUpSweep = 2,
    RoomscaleResolvePhase_StepForwardSweep = 3,
    RoomscaleResolvePhase_StepDownSupportProbe = 4,
};

struct RoomscaleRaycastScratch {
    BEVec3 currentPos = {};
    BEVec3 castFrom = {};
    BEVec3 castDelta = {};
    BEVec3 hitPos = {};
    BEType<uint32_t> groundHit = 0;
    BEType<uint32_t> queryType = RoomscaleQueryType_Sweep;
    BEType<float> sweepRadius = 0.35f;
    BEVec3 hitNormal = {};
};

BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, currentPos) == 0x00, "Roomscale scratch current position offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, castFrom) == 0x0C, "Roomscale scratch cast from offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, castDelta) == 0x18, "Roomscale scratch cast delta offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, hitPos) == 0x24, "Roomscale scratch hit offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, groundHit) == 0x30, "Roomscale scratch ground-hit offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, queryType) == 0x34, "Roomscale scratch query-type offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, sweepRadius) == 0x38, "Roomscale scratch sweep-radius offset mismatch");
BVR_SIZE_CHECK(offsetof(RoomscaleRaycastScratch, hitNormal) == 0x3C, "Roomscale scratch hit-normal offset mismatch");
static_assert(sizeof(RoomscaleRaycastScratch) == 0x48, "Roomscale scratch size mismatch");

struct RoomscaleSupportState {
    bool valid = false;
    glm::fvec3 point = {};
    glm::fvec3 normal = {};
    glm::fvec3 bodyPos = {};
};

struct RoomscaleState {
    bool hasPreviousHeadPos = false;
    bool hasAppliedHeadPos = false;
    bool isResolving = false;
    bool isProbeOnly = false;
    bool isBlocked = false;
    bool isBlockedByCollision = false;
    bool hasActiveStep = false;
    bool currentStepBlockedByCollision = false;
    glm::fvec3 previousHeadPos = {};
    glm::fvec3 appliedHeadPos = {};
    glm::fvec3 trackedHeadPos = {};
    glm::fvec3 remainingRawDelta = {};
    glm::fvec3 remainingWorldDelta = {};
    glm::fvec3 currentStepRawDelta = {};
    glm::fvec3 currentStepWorldDelta = {};
    glm::fvec3 currentStepResolvedWorldDelta = {};
    glm::fvec3 currentStepCandidateWorldPos = {};
    RoomscaleSupportState support = {};
    RoomscaleSupportState currentStepSupport = {};
    uint32_t attemptIndex = 0;
    uint32_t maxAttempts = 0;
    RoomscaleResolvePhase resolvePhase = RoomscaleResolvePhase_ForwardSweep;
};

enum class RoomscaleBodyDriveMode : uint8_t {
    DisabledNotFirstPerson,
    DisabledNotInGame,
    DisabledNotStanding,
    DisabledNoRenderer,
    DisabledNoPositional,
    DisabledNoPlayer,
    DisabledSpecialMovement,
    Enabled,
};

static RoomscaleState s_roomscaleState;
static std::atomic<float> s_roomscaleFadeAmount = 0.0f;
static RoomscaleBodyDriveMode s_roomscaleBodyDriveMode = RoomscaleBodyDriveMode::DisabledNotInGame;

constexpr float kMaxRoomscaleJumpDistanceSq = 0.25f;
constexpr float kMinRoomscaleMoveDistanceSq = 0.000001f;
constexpr float kRoomscaleStepDistance = 0.0625f;
constexpr float kRoomscaleGroundSnapMaxRise = 0.10f;
constexpr float kRoomscaleGroundSnapJitterTolerance = 0.01f;
constexpr float kRoomscaleSupportRetainNormalDot = 0.98f;
constexpr float kRoomscaleStepUpHeight = 0.30f;
constexpr float kRoomscaleStandableSweepNormalY = 0.55f;
constexpr float kRoomscaleBlockedReturnTangentialAllowance = 0.5f;
constexpr float kRoomscaleDebugFallbackBodyRadius = 0.35f;
constexpr float kRoomscaleDebugFallbackBodyHalfHeight = 0.9f;
constexpr uint32_t kMaxRoomscaleRaycastAttempts = 8;
constexpr uint32_t kRoomscaleGroundHit = 15;

struct RoomscaleDebugBody {
    glm::fvec3 centerOffset = glm::fvec3(0.0f, kRoomscaleDebugFallbackBodyHalfHeight, 0.0f);
    float radius = kRoomscaleDebugFallbackBodyRadius;
    float halfHeight = kRoomscaleDebugFallbackBodyHalfHeight;
    float sweepRadius = kRoomscaleDebugFallbackBodyRadius;
    float groundProbeUp = 0.2f;
    float groundProbeDown = 0.75f;
};

struct RoomscaleDebugPalette {
    uint32_t pathColor = 0;
    uint32_t pathSecondaryColor = 0;
    uint32_t bodyColor = 0;
    uint32_t floorColor = 0;
    uint32_t markerColor = 0;
    uint32_t successColor = 0;
    uint32_t blockedColor = 0;
    uint32_t xrayBlockedColor = 0;
};

static glm::fmat3 GetRoomscaleWorldToRoomRotation();
static glm::fmat3 GetRoomscaleRoomToWorldRotation();
static glm::fvec3 GetRoomscaleResidual();
static void UpdateRoomscaleFade();
static void ClearRoomscaleResolveState();

static uint32_t ScaleRoomscaleDebugColor(uint32_t color, float rgbScale, float alphaScale = 1.0f) {
    auto scaleChannel = [&](int shift, float scale) {
        uint8_t value = (uint8_t)((color >> shift) & 0xFFu);
        return (uint8_t)glm::clamp((int)std::lround((float)value * scale), 0, 255);
    };

    return DebugDrawColor(scaleChannel(0, rgbScale), scaleChannel(8, rgbScale), scaleChannel(16, rgbScale), scaleChannel(24, alphaScale));
}

static RoomscaleDebugPalette GetRoomscaleDebugPalette(bool isGroundProbe, bool isProbeOnly) {
    if (isGroundProbe) {
        return {
            .pathColor = DebugDrawColor(196, 104, 255, 255),
            .pathSecondaryColor = DebugDrawColor(220, 160, 255, 168),
            .bodyColor = DebugDrawColor(188, 112, 255, 118),
            .floorColor = DebugDrawColor(212, 144, 255, 156),
            .markerColor = DebugDrawColor(255, 84, 220, 255),
            .successColor = DebugDrawColor(172, 132, 255, 232),
            .blockedColor = DebugDrawColor(255, 72, 220, 210),
            .xrayBlockedColor = DebugDrawColor(255, 72, 220, 128),
        };
    }

    if (isProbeOnly) {
        return {
            .pathColor = DebugDrawColor(96, 255, 136, 255),
            .pathSecondaryColor = DebugDrawColor(156, 255, 184, 180),
            .bodyColor = DebugDrawColor(96, 255, 136, 156),
            .floorColor = DebugDrawColor(120, 255, 164, 148),
            .markerColor = DebugDrawColor(72, 255, 128, 255),
            .successColor = DebugDrawColor(188, 255, 168, 228),
            .blockedColor = DebugDrawColor(72, 220, 104, 220),
            .xrayBlockedColor = DebugDrawColor(72, 220, 104, 136),
        };
    }

    return {
        .pathColor = DebugDrawColor(88, 212, 255, 255),
        .pathSecondaryColor = DebugDrawColor(144, 232, 255, 180),
        .bodyColor = DebugDrawColor(88, 212, 255, 144),
        .floorColor = DebugDrawColor(128, 224, 255, 144),
        .markerColor = DebugDrawColor(120, 255, 255, 255),
        .successColor = DebugDrawColor(96, 255, 136, 228),
        .blockedColor = DebugDrawColor(255, 88, 88, 224),
        .xrayBlockedColor = DebugDrawColor(255, 88, 88, 132),
    };
}

static RoomscaleDebugBody GetRoomscaleDebugBody() {
    RoomscaleDebugBody body = {};

    PlayerBase player = {};
    if (!GameUtils::TryReadPlayerBase(player)) {
        return body;
    }

    const glm::fvec3 localMin = player.aabb.min.getLE();
    const glm::fvec3 localMax = player.aabb.max.getLE();
    const glm::fvec3 localHalfExtents = (localMax - localMin) * 0.5f;
    const bool hasFiniteBounds = std::isfinite(localMin.x) && std::isfinite(localMin.y) && std::isfinite(localMin.z) && std::isfinite(localMax.x) && std::isfinite(localMax.y) && std::isfinite(localMax.z);
    if (!hasFiniteBounds || glm::any(glm::lessThanEqual(localHalfExtents, glm::fvec3(0.0f)))) {
        return body;
    }

    body.centerOffset = (localMin + localMax) * 0.5f;
    body.radius = std::max(localHalfExtents.x, localHalfExtents.z);
    body.halfHeight = std::max(localHalfExtents.y, body.radius * 0.5f);
    body.sweepRadius = glm::clamp(body.radius, 0.2f, 0.45f);

    float footOffset = std::max(-localMin.y, 0.0f);
    body.groundProbeUp = std::max(footOffset + 0.05f, 0.2f);
    body.groundProbeDown = std::max(footOffset + 0.25f, 0.5f);
    return body;
}

static bool IsRoomscaleSweepPhase(RoomscaleResolvePhase phase) {
    return phase == RoomscaleResolvePhase_ForwardSweep || phase == RoomscaleResolvePhase_StepUpSweep || phase == RoomscaleResolvePhase_StepForwardSweep;
}

static bool IsRoomscaleSupportProbePhase(RoomscaleResolvePhase phase) {
    return phase == RoomscaleResolvePhase_DownSupportProbe || phase == RoomscaleResolvePhase_StepDownSupportProbe;
}

static bool HasRoomscaleValidSweepNormal(const glm::fvec3& normal) {
    return glm::all(glm::isfinite(normal)) && glm::dot(normal, normal) > 0.000001f;
}

static bool HasRoomscaleValidSupportState(const RoomscaleSupportState& support) {
    return support.valid && glm::all(glm::isfinite(support.point)) && glm::all(glm::isfinite(support.normal)) && glm::all(glm::isfinite(support.bodyPos)) && HasRoomscaleValidSweepNormal(support.normal);
}

static glm::fvec3 NormalizeRoomscaleSweepHitNormal(const RoomscaleRaycastScratch& scratch) {
    glm::fvec3 hitNormal = scratch.hitNormal.getLE();
    if (!HasRoomscaleValidSweepNormal(hitNormal)) {
        return glm::fvec3(0.0f);
    }

    hitNormal = glm::normalize(hitNormal);
    glm::fvec3 castDelta = scratch.castDelta.getLE();
    if (glm::dot(castDelta, castDelta) > 0.000001f && glm::dot(hitNormal, castDelta) > 0.0f) {
        hitNormal = -hitNormal;
    }

    return hitNormal;
}

static bool IsRoomscaleSweepNormalFloorLike(const glm::fvec3& normal) {
    return HasRoomscaleValidSweepNormal(normal) && normal.y >= kRoomscaleStandableSweepNormalY;
}

static glm::fvec3 GetRoomscaleGroundProbeDelta(const RoomscaleDebugBody& body) {
    return glm::fvec3(0.0f, -(body.groundProbeUp + body.groundProbeDown), 0.0f);
}

static glm::fvec3 GetRoomscaleBodyCapsuleCenter(const RoomscaleDebugBody& body, const glm::fvec3& bodyPos) {
    return bodyPos + body.centerOffset;
}

static void PrepareRoomscaleSupportProbe(RoomscaleRaycastScratch& scratch, const RoomscaleDebugBody& body, const glm::fvec3& bodyPos) {
    scratch.queryType = RoomscaleQueryType_SupportProbe;
    scratch.sweepRadius = body.radius;
    scratch.castFrom = GetRoomscaleBodyCapsuleCenter(body, bodyPos) + glm::fvec3(0.0f, body.groundProbeUp, 0.0f);
    scratch.castDelta = GetRoomscaleGroundProbeDelta(body);
    scratch.hitPos = scratch.castFrom.getLE() + scratch.castDelta.getLE();
    scratch.hitNormal = glm::fvec3(0.0f);
}

static bool TryBuildRoomscaleSupportState(const RoomscaleDebugBody& body, const glm::fvec3& referenceBodyPos, float maxRise, const RoomscaleRaycastScratch& scratch, RoomscaleSupportState& outSupport) {
    outSupport = {};
    glm::fvec3 supportNormal = NormalizeRoomscaleSweepHitNormal(scratch);
    if (HasRoomscaleValidSweepNormal(supportNormal) && supportNormal.y < 0.0f) {
        supportNormal = -supportNormal;
    }

    if (!IsRoomscaleSweepNormalFloorLike(supportNormal)) {
        return false;
    }

    glm::fvec3 castDelta = scratch.castDelta.getLE();
    float castLengthSq = glm::dot(castDelta, castDelta);
    if (castLengthSq < 0.000001f) {
        return false;
    }

    const float cylinderHalfHeight = std::max(body.halfHeight - body.radius, 0.0f);
    const glm::fvec3 hitPos = scratch.hitPos.getLE();
    const glm::fvec3 bottomSphereCenter = hitPos + (supportNormal * body.radius);
    const glm::fvec3 capsuleCenter = bottomSphereCenter + glm::fvec3(0.0f, cylinderHalfHeight, 0.0f);
    float alongCast = glm::dot(capsuleCenter - scratch.castFrom.getLE(), castDelta) / castLengthSq;
    if (alongCast < 0.0f || alongCast > 1.0f) {
        return false;
    }

    glm::fvec3 bodyPos = capsuleCenter - body.centerOffset;
    if (!glm::all(glm::isfinite(bodyPos)) || (bodyPos.y - referenceBodyPos.y) > maxRise) {
        return false;
    }

    outSupport.valid = true;
    outSupport.point = hitPos;
    outSupport.normal = supportNormal;
    outSupport.bodyPos = bodyPos;
    return true;
}

static RoomscaleSupportState StabilizeRoomscaleSupportState(const RoomscaleSupportState& previousSupport, const RoomscaleSupportState& nextSupport) {
    if (!HasRoomscaleValidSupportState(nextSupport)) {
        return {};
    }

    if (!HasRoomscaleValidSupportState(previousSupport)) {
        return nextSupport;
    }

    RoomscaleSupportState resolvedSupport = nextSupport;
    if (std::abs(nextSupport.bodyPos.y - previousSupport.bodyPos.y) <= kRoomscaleGroundSnapJitterTolerance && glm::dot(nextSupport.normal, previousSupport.normal) >= kRoomscaleSupportRetainNormalDot) {
        resolvedSupport.bodyPos.y = previousSupport.bodyPos.y;
        resolvedSupport.point.y = previousSupport.point.y;
        resolvedSupport.normal = previousSupport.normal;
    }

    return resolvedSupport;
}

static glm::fvec3 ProjectRoomscaleDeltaOntoSupport(const glm::fvec3& worldDelta, const RoomscaleSupportState& support) {
    if (!HasRoomscaleValidSupportState(support)) {
        return worldDelta;
    }

    glm::fvec3 supportNormal = glm::normalize(support.normal);
    if (std::abs(supportNormal.y) < 0.000001f) {
        return worldDelta;
    }

    glm::fvec3 projectedDelta = worldDelta;
    projectedDelta.y = -(supportNormal.x * worldDelta.x + supportNormal.z * worldDelta.z) / supportNormal.y;
    return projectedDelta;
}

static void BeginRoomscaleResolvePhase(RoomscaleResolvePhase phase) {
    s_roomscaleState.resolvePhase = phase;
}

static glm::fvec3 ConvertRoomscaleWorldDeltaToRoomDelta(const glm::fvec3& worldDelta) {
    glm::fvec3 roomDelta = GetRoomscaleWorldToRoomRotation() * worldDelta;
    roomDelta.y = 0.0f;
    return roomDelta;
}

static bool WouldRoomscaleResidualImproveWithAppliedDelta(glm::fvec3 appliedHeadDelta) {
    if (!s_roomscaleState.isProbeOnly) {
        return true;
    }

    appliedHeadDelta.y = 0.0f;
    glm::fvec3 oldResidual = GetRoomscaleResidual();
    glm::fvec3 newResidual = oldResidual - appliedHeadDelta;
    newResidual.y = 0.0f;
    return glm::dot(newResidual, newResidual) + kMinRoomscaleMoveDistanceSq < glm::dot(oldResidual, oldResidual);
}

static bool CommitRoomscaleBodyMove(uint32_t scratchPtr, RoomscaleRaycastScratch& scratch, const glm::fvec3& worldPos, glm::fvec3 appliedHeadDelta) {
    appliedHeadDelta.y = 0.0f;
    if (!WouldRoomscaleResidualImproveWithAppliedDelta(appliedHeadDelta)) {
        return false;
    }

    scratch.currentPos = worldPos;
    s_roomscaleState.appliedHeadPos += appliedHeadDelta;
    CemuHooks::writeMemory(scratchPtr, &scratch);
    return true;
}

static bool CommitRoomscaleBodyMoveWithSupport(uint32_t scratchPtr, RoomscaleRaycastScratch& scratch, const RoomscaleSupportState& support, glm::fvec3 appliedHeadDelta) {
    if (!HasRoomscaleValidSupportState(support)) {
        return false;
    }

    if (!CommitRoomscaleBodyMove(scratchPtr, scratch, support.bodyPos, appliedHeadDelta)) {
        return false;
    }

    s_roomscaleState.support = support;
    return true;
}

static void AdvanceRoomscaleCommittedStep(glm::fvec3 appliedHeadDelta) {
    appliedHeadDelta.y = 0.0f;
    s_roomscaleState.remainingRawDelta -= appliedHeadDelta;
    s_roomscaleState.remainingWorldDelta = GetRoomscaleRoomToWorldRotation() * s_roomscaleState.remainingRawDelta;
    s_roomscaleState.remainingWorldDelta.y = 0.0f;
    s_roomscaleState.attemptIndex++;
    s_roomscaleState.hasActiveStep = false;
    BeginRoomscaleResolvePhase(RoomscaleResolvePhase_ForwardSweep);
}

static void FinishRoomscaleResolveWarp(PPCInterpreter_t* hCPU, bool blocked, bool blockedByCollision) {
    s_roomscaleState.isBlocked = blocked;
    s_roomscaleState.isBlockedByCollision = blocked && blockedByCollision;
    UpdateRoomscaleFade();
    ClearRoomscaleResolveState();
    hCPU->gpr[3] = RoomscaleHookResult_Warp;
}

static void DrawRoomscaleDebugPositionMarker(const glm::fvec3& pos, float radius, uint32_t color, bool xray = false) {
    DebugDraw::instance().Dot(pos, radius, color, xray);
}

static void DrawRoomscaleDebugBodyCapsule(const RoomscaleDebugBody& body, const glm::fvec3& bodyPos, uint32_t color, bool xray = false) {
    DebugDraw::instance().Capsule(GetRoomscaleBodyCapsuleCenter(body, bodyPos), body.radius, body.halfHeight, color, 1.0f, 0, xray);
}

static bool IsRoomscaleGroundProbe(const RoomscaleRaycastScratch& scratch) {
    return scratch.queryType.getLE() != RoomscaleQueryType_Sweep;
}

static void DrawRoomscaleDebugCast(const RoomscaleRaycastScratch& scratch, bool isProbeOnly) {
    if (!GetSettings().ShouldShowRoomPhysics() || IsRoomscaleGroundProbe(scratch)) {
        return;
    }

    const RoomscaleDebugBody body = GetRoomscaleDebugBody();
    const RoomscaleDebugPalette palette = GetRoomscaleDebugPalette(false, isProbeOnly);
    const glm::fvec3 castFrom = scratch.castFrom.getLE();
    const glm::fvec3 castTo = castFrom + scratch.castDelta.getLE();

    DebugDraw::instance().Line(castFrom, castTo, palette.pathColor, 1.0f, true);
    DrawRoomscaleDebugBodyCapsule(body, castFrom - body.centerOffset, ScaleRoomscaleDebugColor(palette.bodyColor, 0.85f, 0.28f), true);
    DrawRoomscaleDebugBodyCapsule(body, castTo - body.centerOffset, ScaleRoomscaleDebugColor(palette.bodyColor, 0.75f, 0.22f), true);
    DrawRoomscaleDebugPositionMarker(castFrom, 0.04f, palette.markerColor, true);
    DrawRoomscaleDebugPositionMarker(castTo, 0.04f, palette.pathSecondaryColor, true);
}

static void DrawRoomscaleDebugCastResult(const RoomscaleRaycastScratch& scratch, bool isProbeOnly, bool hadHit, bool acceptedHit) {
    if (!GetSettings().ShouldShowRoomPhysics()) {
        return;
    }

    const bool isGroundProbe = IsRoomscaleGroundProbe(scratch);
    const RoomscaleDebugPalette palette = GetRoomscaleDebugPalette(isGroundProbe, isProbeOnly);
    const glm::fvec3 castFrom = scratch.castFrom.getLE();
    const glm::fvec3 castTo = castFrom + scratch.castDelta.getLE();
    if (!hadHit) {
        return;
    }

    const glm::fvec3 hitPos = scratch.hitPos.getLE();
    glm::fvec3 hitNormal = NormalizeRoomscaleSweepHitNormal(scratch);
    if (isGroundProbe) {
        uint32_t hitColor = acceptedHit ? palette.successColor : palette.blockedColor;
        DebugDraw::instance().Line(castFrom, hitPos, hitColor, 1.0f, true);
        if (glm::dot(hitPos - castTo, hitPos - castTo) > 0.000001f) {
            DebugDraw::instance().Line(hitPos, castTo, ScaleRoomscaleDebugColor(hitColor, 0.45f, 0.35f), 1.0f, true);
        }
        if (HasRoomscaleValidSweepNormal(hitNormal)) {
            DebugDraw::instance().Line(hitPos, hitPos + (hitNormal * 0.18f), palette.floorColor, 1.0f, true);
        }
        DrawRoomscaleDebugPositionMarker(hitPos, 0.08f, hitColor, true);
        return;
    }

    const float sweepRadius = std::max(scratch.sweepRadius.getLE(), 0.01f);
    DebugDraw::instance().Line(castFrom, hitPos, palette.blockedColor, 1.0f, true);
    if (glm::dot(hitPos - castTo, hitPos - castTo) > 0.000001f) {
        DebugDraw::instance().Line(hitPos, castTo, ScaleRoomscaleDebugColor(palette.blockedColor, 0.45f, 0.35f), 1.0f, true);
    }
    if (HasRoomscaleValidSweepNormal(hitNormal)) {
        DebugDraw::instance().Line(hitPos, hitPos + (hitNormal * 0.18f), palette.markerColor, 1.0f, true);
    }
    DebugDraw::instance().Sphere(hitPos, sweepRadius, ScaleRoomscaleDebugColor(palette.blockedColor, 0.85f, 0.28f), 0, true);
    DrawRoomscaleDebugPositionMarker(hitPos, 0.08f, palette.blockedColor, true);
}

static void ResetRoomscaleActiveStep() {
    s_roomscaleState.hasActiveStep = false;
    s_roomscaleState.currentStepBlockedByCollision = false;
    s_roomscaleState.currentStepRawDelta = glm::fvec3(0.0f);
    s_roomscaleState.currentStepWorldDelta = glm::fvec3(0.0f);
    s_roomscaleState.currentStepResolvedWorldDelta = glm::fvec3(0.0f);
    s_roomscaleState.currentStepCandidateWorldPos = glm::fvec3(0.0f);
    s_roomscaleState.currentStepSupport = {};
    BeginRoomscaleResolvePhase(RoomscaleResolvePhase_ForwardSweep);
}

static void BeginRoomscaleActiveStep(uint32_t remainingSteps) {
    s_roomscaleState.hasActiveStep = true;
    s_roomscaleState.currentStepBlockedByCollision = false;
    s_roomscaleState.currentStepRawDelta = s_roomscaleState.remainingRawDelta / (float)remainingSteps;
    s_roomscaleState.currentStepWorldDelta = s_roomscaleState.remainingWorldDelta / (float)remainingSteps;
    s_roomscaleState.currentStepResolvedWorldDelta = ProjectRoomscaleDeltaOntoSupport(s_roomscaleState.currentStepWorldDelta, s_roomscaleState.support);
    s_roomscaleState.currentStepCandidateWorldPos = glm::fvec3(0.0f);
    s_roomscaleState.currentStepSupport = {};
    BeginRoomscaleResolvePhase(RoomscaleResolvePhase_ForwardSweep);
}

static glm::fmat3 GetRoomscaleRoomToWorldRotation() {
    return glm::fmat3(CemuHooks::s_lastCameraMtx);
}

static glm::fmat3 GetRoomscaleWorldToRoomRotation() {
    return glm::transpose(GetRoomscaleRoomToWorldRotation());
}

static void ClearRoomscaleResolveState() {
    BetterVRProfiler::EndSpan(BetterVRProfiler::Section::RoomscaleResolve);

    s_roomscaleState.isResolving = false;
    s_roomscaleState.isProbeOnly = false;
    s_roomscaleState.remainingRawDelta = glm::fvec3(0.0f);
    s_roomscaleState.remainingWorldDelta = glm::fvec3(0.0f);
    s_roomscaleState.attemptIndex = 0;
    s_roomscaleState.maxAttempts = 0;
    ResetRoomscaleActiveStep();
}

static void ResetRoomscaleState() {
    s_roomscaleState.hasPreviousHeadPos = false;
    s_roomscaleState.hasAppliedHeadPos = false;
    s_roomscaleState.isBlocked = false;
    s_roomscaleState.isBlockedByCollision = false;
    s_roomscaleState.support = {};
    s_roomscaleState.previousHeadPos = glm::fvec3(0.0f);
    s_roomscaleState.appliedHeadPos = glm::fvec3(0.0f);
    s_roomscaleState.trackedHeadPos = glm::fvec3(0.0f);
    ClearRoomscaleResolveState();
    s_roomscaleFadeAmount.store(0.0f, std::memory_order_relaxed);
}

static void UpdateRoomscaleFade() {
    // Room collision fading is handled separately from body driving now.
    s_roomscaleFadeAmount.store(0.0f, std::memory_order_relaxed);
}

static glm::fvec3 GetRoomscaleResidual() {
    if (!s_roomscaleState.hasAppliedHeadPos) {
        return glm::fvec3(0.0f);
    }

    glm::fvec3 residual = s_roomscaleState.trackedHeadPos - s_roomscaleState.appliedHeadPos;
    residual.y = 0.0f;
    return residual;
}

static glm::fvec3 RemoveResolvedRoomscaleDelta(glm::fvec3 roomDelta, glm::fvec3 residual) {
    residual.y = 0.0f;
    float residualDistance = glm::length(residual);
    if (residualDistance < 0.000001f) {
        return roomDelta;
    }

    glm::fvec3 residualDir = residual / residualDistance;
    float alongResidual = glm::dot(roomDelta, residualDir);
    if (alongResidual >= 0.0f) {
        return roomDelta;
    }

    glm::fvec3 tangentialDelta = roomDelta - (residualDir * alongResidual);
    if (s_roomscaleState.isBlockedByCollision) {
        float towardBodyDistance = -alongResidual;
        float tangentialDistance = glm::length(tangentialDelta);
        if (towardBodyDistance >= (tangentialDistance * kRoomscaleBlockedReturnTangentialAllowance)) {
            return glm::fvec3(0.0f);
        }
    }

    return tangentialDelta;
}

static std::optional<glm::fvec3> TryGetCurrentHeadPos() {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        return std::nullopt;
    }

    std::optional<glm::fmat4> middlePose = renderer->GetMiddlePose();
    if (!middlePose.has_value()) {
        return std::nullopt;
    }

    glm::fvec3 headPos = glm::fvec3(middlePose.value()[3]);
    if (!glm::all(glm::isfinite(headPos))) {
        return std::nullopt;
    }

    return headPos;
}

static const char* GetRoomscaleBodyDriveModeName(RoomscaleBodyDriveMode mode) {
    switch (mode) {
        case RoomscaleBodyDriveMode::DisabledNotFirstPerson:
            return "disabled (not first-person)";
        case RoomscaleBodyDriveMode::DisabledNotInGame:
            return "disabled (not in game)";
        case RoomscaleBodyDriveMode::DisabledNotStanding:
            return "disabled (play mode is not standing)";
        case RoomscaleBodyDriveMode::DisabledNoRenderer:
            return "disabled (renderer unavailable)";
        case RoomscaleBodyDriveMode::DisabledNoPositional:
            return "disabled (no positional tracking)";
        case RoomscaleBodyDriveMode::DisabledNoPlayer:
            return "disabled (player unavailable)";
        case RoomscaleBodyDriveMode::DisabledSpecialMovement:
            return "disabled (special movement state)";
        case RoomscaleBodyDriveMode::Enabled:
            return "enabled";
    }

    return "disabled (unknown)";
}

static void SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode mode) {
    if (s_roomscaleBodyDriveMode == mode) {
        return;
    }

    s_roomscaleBodyDriveMode = mode;
    Log::print<PPC>("Roomscale body drive mode switched: {}", GetRoomscaleBodyDriveModeName(mode));
}

static void PrimeRoomscaleBaseline(const glm::fvec3& headPos) {
    s_roomscaleState.previousHeadPos = headPos;
    s_roomscaleState.appliedHeadPos = headPos;
    s_roomscaleState.trackedHeadPos = headPos;
    s_roomscaleState.hasPreviousHeadPos = true;
    s_roomscaleState.hasAppliedHeadPos = true;
    s_roomscaleState.isBlocked = false;
    s_roomscaleState.isBlockedByCollision = false;
    s_roomscaleState.support = {};
    ClearRoomscaleResolveState();
    UpdateRoomscaleFade();
}

static bool ShouldDrivePlayerBodyWithVR(const glm::fvec3& currentHeadPos, PlayerBase& player) {
    if (!CemuHooks::IsFirstPerson()) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNotFirstPerson);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!CemuHooks::IsInGame()) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNotInGame);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (GetSettings().GetPlayMode() != PlayMode::STANDING) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNotStanding);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNoRenderer);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!VRManager::instance().XR->m_capabilities.supportsPositional) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNoPositional);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!GameUtils::TryReadPlayerBase(player)) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNoPlayer);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    PlayerMoveBitFlags moveBits = player.moveBitFlags.getLE();
    OpenXR::GameState gameState = VRManager::instance().XR->m_gameState.load();
    bool shouldDisablePlayerBodyDrive = gameState.is_climbing || gameState.is_paragliding || gameState.is_riding_mount || HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_SWIMMING_OR_CLIMBING | PlayerMoveBitFlags::IS_SWIMMING);
    if (shouldDisablePlayerBodyDrive) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledSpecialMovement);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::Enabled);
    return true;
}

static bool TryGetRoomscaleMoveDelta(uint32_t controller, glm::fvec3& currentHeadPos, glm::fvec3& roomDelta, glm::fvec3& worldDelta) {
    currentHeadPos = glm::fvec3(0.0f);
    roomDelta = glm::fvec3(0.0f);
    worldDelta = glm::fvec3(0.0f);

    uint32_t playerController = 0;
    if (!GameUtils::TryGetPlayerCharacterController(playerController) || controller != playerController) {
        return false;
    }

    std::optional<glm::fvec3> currentHeadPosOpt = TryGetCurrentHeadPos();
    if (!currentHeadPosOpt.has_value()) {
        ResetRoomscaleState();
        return false;
    }

    currentHeadPos = currentHeadPosOpt.value();
    s_roomscaleState.trackedHeadPos = currentHeadPos;

    PlayerBase player = {};
    if (!ShouldDrivePlayerBodyWithVR(currentHeadPos, player)) {
        return false;
    }

    if (!s_roomscaleState.hasPreviousHeadPos) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    glm::fvec3 previousResidual = s_roomscaleState.previousHeadPos - s_roomscaleState.appliedHeadPos;
    previousResidual.y = 0.0f;
    roomDelta = currentHeadPos - s_roomscaleState.previousHeadPos;
    s_roomscaleState.previousHeadPos = currentHeadPos;

    roomDelta.y = 0.0f;
    if (glm::dot(roomDelta, roomDelta) < kMinRoomscaleMoveDistanceSq) {
        UpdateRoomscaleFade();
        return false;
    }

    if (glm::dot(roomDelta, roomDelta) > kMaxRoomscaleJumpDistanceSq) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    const glm::fmat3 roomToWorld = GetRoomscaleRoomToWorldRotation();
    worldDelta = roomToWorld * roomDelta;
    worldDelta.y = 0.0f;
    glm::fvec3 previousResidualWorld = roomToWorld * previousResidual;
    previousResidualWorld.y = 0.0f;
    worldDelta = RemoveResolvedRoomscaleDelta(worldDelta, previousResidualWorld);
    if (glm::dot(worldDelta, worldDelta) < kMinRoomscaleMoveDistanceSq) {
        UpdateRoomscaleFade();
        return false;
    }

    roomDelta = GetRoomscaleWorldToRoomRotation() * worldDelta;
    roomDelta.y = 0.0f;

    return true;
}

glm::fvec3 CemuHooks::GetAppliedRoomscaleHeadPosition() {
    if (s_roomscaleState.hasAppliedHeadPos) {
        return s_roomscaleState.appliedHeadPos;
    }

    std::optional<glm::fvec3> currentHeadPos = TryGetCurrentHeadPos();
    if (currentHeadPos.has_value()) {
        return currentHeadPos.value();
    }

    return glm::fvec3(0.0f);
}

float CemuHooks::GetRoomscaleFadeAmount() {
    return s_roomscaleFadeAmount.load(std::memory_order_relaxed);
}


void CemuHooks::hook_BeginRoomscaleMovement(PPCInterpreter_t* hCPU) {
    BetterVRProfiler::Scope profile(BetterVRProfiler::Section::RoomscaleBeginHook);

    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t controller = hCPU->gpr[3];
    uint32_t scratchPtr = hCPU->gpr[4];
    RoomscaleRaycastScratch scratch = {};
    writeMemory(scratchPtr, &scratch);
    hCPU->gpr[3] = RoomscaleHookResult_Skip;
    ClearRoomscaleResolveState();

    uint32_t playerController = 0;
    if (!GameUtils::TryGetPlayerCharacterController(playerController) || controller != playerController) {
        return;
    }

    glm::fvec3 currentHeadPos = {};
    glm::fvec3 roomDelta = {};
    glm::fvec3 worldDelta = {};
    if (!TryGetRoomscaleMoveDelta(controller, currentHeadPos, roomDelta, worldDelta)) {
        glm::fvec3 residual = GetRoomscaleResidual();
        glm::fvec3 residualWorldDelta = GetRoomscaleRoomToWorldRotation() * residual;
        residualWorldDelta.y = 0.0f;
        if (glm::dot(residualWorldDelta, residualWorldDelta) < kMinRoomscaleMoveDistanceSq) {
            s_roomscaleState.isBlocked = false;
            s_roomscaleState.isBlockedByCollision = false;
            UpdateRoomscaleFade();
            return;
        }

        float residualMoveDistance = glm::length(residualWorldDelta);
        uint32_t maxAttempts = std::min<uint32_t>(std::max<uint32_t>((uint32_t)std::ceil(residualMoveDistance / kRoomscaleStepDistance), 1u), kMaxRoomscaleRaycastAttempts);

        s_roomscaleState.remainingRawDelta = residual;
        s_roomscaleState.remainingWorldDelta = residualWorldDelta;
        s_roomscaleState.attemptIndex = 0;
        s_roomscaleState.maxAttempts = maxAttempts;
        s_roomscaleState.isResolving = true;
        s_roomscaleState.isProbeOnly = true;
        hCPU->gpr[3] = RoomscaleHookResult_Cast;
        BetterVRProfiler::BeginSpan(BetterVRProfiler::Section::RoomscaleResolve);
        return;
    }
    (void)currentHeadPos;

    float moveDistance = glm::length(worldDelta);
    uint32_t maxAttempts = std::min<uint32_t>(std::max<uint32_t>((uint32_t)std::ceil(moveDistance / kRoomscaleStepDistance), 1u), kMaxRoomscaleRaycastAttempts);

    s_roomscaleState.remainingRawDelta = roomDelta;
    s_roomscaleState.remainingWorldDelta = worldDelta;
    s_roomscaleState.attemptIndex = 0;
    s_roomscaleState.maxAttempts = maxAttempts;
    s_roomscaleState.isResolving = true;
    s_roomscaleState.isProbeOnly = false;
    hCPU->gpr[3] = RoomscaleHookResult_Cast;
    BetterVRProfiler::BeginSpan(BetterVRProfiler::Section::RoomscaleResolve);
}

void CemuHooks::hook_PrepareRoomscaleRaycast(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    uint32_t scratchPtr = hCPU->gpr[3];
    hCPU->gpr[3] = RoomscaleHookResult_Skip;

    if (!s_roomscaleState.isResolving) {
        return;
    }

    if (s_roomscaleState.attemptIndex >= s_roomscaleState.maxAttempts || glm::dot(s_roomscaleState.remainingWorldDelta, s_roomscaleState.remainingWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    RoomscaleRaycastScratch scratch = {};
    readMemory(scratchPtr, &scratch);

    uint32_t remainingSteps = s_roomscaleState.maxAttempts - s_roomscaleState.attemptIndex;
    if (remainingSteps == 0) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    if (!s_roomscaleState.hasActiveStep) {
        BeginRoomscaleActiveStep(remainingSteps);
    }

    if (glm::dot(s_roomscaleState.currentStepWorldDelta, s_roomscaleState.currentStepWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    const RoomscaleDebugBody body = GetRoomscaleDebugBody();
    const glm::fvec3 currentWorldPos = scratch.currentPos.getLE();
    const RoomscaleResolvePhase phase = s_roomscaleState.resolvePhase;

    scratch.groundHit = kRoomscaleGroundHit;
    scratch.sweepRadius = body.sweepRadius;
    scratch.hitNormal = glm::fvec3(0.0f);
    if (phase == RoomscaleResolvePhase_ForwardSweep) {
        scratch.queryType = RoomscaleQueryType_Sweep;
        scratch.castFrom = GetRoomscaleBodyCapsuleCenter(body, currentWorldPos);
        scratch.castDelta = s_roomscaleState.currentStepResolvedWorldDelta;
        scratch.hitPos = scratch.castFrom.getLE() + scratch.castDelta.getLE();
        s_roomscaleState.currentStepCandidateWorldPos = currentWorldPos + s_roomscaleState.currentStepResolvedWorldDelta;
    }
    else if (phase == RoomscaleResolvePhase_StepUpSweep) {
        const glm::fvec3 stepUpDelta = glm::fvec3(0.0f, kRoomscaleStepUpHeight, 0.0f);
        scratch.queryType = RoomscaleQueryType_Sweep;
        scratch.castFrom = GetRoomscaleBodyCapsuleCenter(body, currentWorldPos);
        scratch.castDelta = stepUpDelta;
        scratch.hitPos = scratch.castFrom.getLE() + stepUpDelta;
        s_roomscaleState.currentStepCandidateWorldPos = currentWorldPos + stepUpDelta;
    }
    else if (phase == RoomscaleResolvePhase_StepForwardSweep) {
        const glm::fvec3 steppedUpWorldPos = s_roomscaleState.currentStepCandidateWorldPos;
        scratch.queryType = RoomscaleQueryType_Sweep;
        scratch.castFrom = GetRoomscaleBodyCapsuleCenter(body, steppedUpWorldPos);
        scratch.castDelta = s_roomscaleState.currentStepResolvedWorldDelta;
        scratch.hitPos = scratch.castFrom.getLE() + scratch.castDelta.getLE();
        s_roomscaleState.currentStepCandidateWorldPos = steppedUpWorldPos + s_roomscaleState.currentStepResolvedWorldDelta;
    }
    else if (IsRoomscaleSupportProbePhase(phase)) {
        glm::fvec3 probeBodyPos = s_roomscaleState.currentStepCandidateWorldPos;
        PrepareRoomscaleSupportProbe(scratch, body, probeBodyPos);
    }

    DrawRoomscaleDebugCast(scratch, s_roomscaleState.isProbeOnly);
    writeMemory(scratchPtr, &scratch);
    hCPU->gpr[3] = RoomscaleHookResult_Cast;
}

void CemuHooks::hook_BuildRoomscaleWarpTransform(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t currentPosPtr = hCPU->gpr[3];
    uint32_t transformPtr = hCPU->gpr[4];

    BEVec3 currentPos = {};
    BEMatrix34 transform = {};
    readMemory(currentPosPtr, &currentPos);
    readMemory(transformPtr, &transform);

    transform.setPos(currentPos.getLE());
    writeMemory(transformPtr, &transform);
}

void CemuHooks::hook_ConsumeRoomscaleRaycast(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    uint32_t scratchPtr = hCPU->gpr[3];
    bool hadHit = hCPU->gpr[4] != 0;
    hCPU->gpr[3] = RoomscaleHookResult_Skip;

    if (!s_roomscaleState.isResolving) {
        return;
    }

    RoomscaleRaycastScratch scratch = {};
    readMemory(scratchPtr, &scratch);
    uint32_t remainingSteps = s_roomscaleState.maxAttempts - s_roomscaleState.attemptIndex;
    if (remainingSteps == 0) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    if (IsRoomscaleSweepPhase(s_roomscaleState.resolvePhase)) {
        DrawRoomscaleDebugCastResult(scratch, s_roomscaleState.isProbeOnly, hadHit, false);
        if (hadHit) {
            s_roomscaleState.currentStepBlockedByCollision = true;
            if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_ForwardSweep) {
                BeginRoomscaleResolvePhase(RoomscaleResolvePhase_StepUpSweep);
                hCPU->gpr[3] = RoomscaleHookResult_Cast;
                return;
            }

            FinishRoomscaleResolveWarp(hCPU, true, true);
            return;
        }

        if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_ForwardSweep) {
            BeginRoomscaleResolvePhase(RoomscaleResolvePhase_DownSupportProbe);
        }
        else if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_StepUpSweep) {
            BeginRoomscaleResolvePhase(RoomscaleResolvePhase_StepForwardSweep);
        }
        else {
            BeginRoomscaleResolvePhase(RoomscaleResolvePhase_StepDownSupportProbe);
        }

        hCPU->gpr[3] = RoomscaleHookResult_Cast;
        return;
    }

    const RoomscaleDebugBody body = GetRoomscaleDebugBody();
    const bool isStepProbe = s_roomscaleState.resolvePhase == RoomscaleResolvePhase_StepDownSupportProbe;
    const float maxGroundRise = isStepProbe ? (kRoomscaleStepUpHeight + kRoomscaleGroundSnapMaxRise) : kRoomscaleGroundSnapMaxRise;
    RoomscaleSupportState support = {};
    const bool hasSupport = hadHit && TryBuildRoomscaleSupportState(body, scratch.currentPos.getLE(), maxGroundRise, scratch, support);
    DrawRoomscaleDebugCastResult(scratch, s_roomscaleState.isProbeOnly, hadHit, hasSupport);

    if (!hasSupport) {
        if (!isStepProbe) {
            glm::fvec3 appliedHeadDelta = s_roomscaleState.currentStepRawDelta;
            if (!CommitRoomscaleBodyMove(scratchPtr, scratch, s_roomscaleState.currentStepCandidateWorldPos, appliedHeadDelta)) {
                FinishRoomscaleResolveWarp(hCPU, true, s_roomscaleState.currentStepBlockedByCollision);
                return;
            }

            s_roomscaleState.support = {};
            AdvanceRoomscaleCommittedStep(appliedHeadDelta);
            if (glm::dot(s_roomscaleState.remainingWorldDelta, s_roomscaleState.remainingWorldDelta) < kMinRoomscaleMoveDistanceSq) {
                FinishRoomscaleResolveWarp(hCPU, false, false);
                return;
            }

            if (s_roomscaleState.attemptIndex >= s_roomscaleState.maxAttempts) {
                FinishRoomscaleResolveWarp(hCPU, true, false);
                return;
            }

            hCPU->gpr[3] = RoomscaleHookResult_Cast;
            return;
        }

        FinishRoomscaleResolveWarp(hCPU, true, s_roomscaleState.currentStepBlockedByCollision);
        return;
    }

    s_roomscaleState.currentStepSupport = StabilizeRoomscaleSupportState(s_roomscaleState.support, support);
    glm::fvec3 appliedHeadDelta = s_roomscaleState.currentStepRawDelta;
    if (!CommitRoomscaleBodyMoveWithSupport(scratchPtr, scratch, s_roomscaleState.currentStepSupport, appliedHeadDelta)) {
        FinishRoomscaleResolveWarp(hCPU, true, s_roomscaleState.currentStepBlockedByCollision);
        return;
    }

    AdvanceRoomscaleCommittedStep(appliedHeadDelta);
    if (glm::dot(s_roomscaleState.remainingWorldDelta, s_roomscaleState.remainingWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        FinishRoomscaleResolveWarp(hCPU, false, false);
        return;
    }

    if (s_roomscaleState.attemptIndex >= s_roomscaleState.maxAttempts) {
        FinishRoomscaleResolveWarp(hCPU, true, false);
        return;
    }

    hCPU->gpr[3] = RoomscaleHookResult_Cast;
}


#define LOG_PLAYER_BODY(...)                     \
    do {                                         \
        if (GameUtils::IsTrackedPlayerBody(hCPU->gpr[3])) { \
            Log::print<PPC>(__VA_ARGS__);        \
        }                                        \
    } while (0)

void CemuHooks::hook_SetRigidBodyVelocity(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    BEVec3 velocity;
    readMemory(hCPU->gpr[4], &velocity);
    LOG_PLAYER_BODY("[{:08X}] SetVelocity {}", hCPU->gpr[0], velocity.getLE());
}

void CemuHooks::hook_SetRigidBodyTransform(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486CD8;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    BEMatrix34 transform;
    readMemory(hCPU->gpr[4], &transform);
    LOG_PLAYER_BODY("[{:08X}] SetTransform pos={} rot={}", hCPU->sprNew.LR, transform.getPos().getLE(), transform.getRotLE());
}

void CemuHooks::hook_SetRigidBodyScale(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486F50;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    LOG_PLAYER_BODY("[{:08X}] SetScale {}", hCPU->sprNew.LR, (float)hCPU->fpr[1].fp0);
}

void CemuHooks::hook_SetRigidBodyPosition(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486D84;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    BEVec3 position;
    readMemory(hCPU->gpr[4], &position);
    LOG_PLAYER_BODY("[{:08X}] SetPosition {}", hCPU->sprNew.LR, position.getLE());
}

void CemuHooks::hook_SetRigidBodyPositionAndRotation(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03489A84;
    hCPU->gpr[11] = hCPU->gpr[1];

    BEMatrix34 transform;
    readMemory(hCPU->gpr[4], &transform);
    LOG_PLAYER_BODY("[{:08X}] SetPositionAndRotation pos={} rot={}", hCPU->sprNew.LR, transform.getPos().getLE(), transform.getRotLE());
}


bool shouldAdjustArrowTarget = false;

void CemuHooks::hook_LoadDynamicBool(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    hCPU->sprNew.LR = hCPU->gpr[0];

    InlineParamBool boolParam;
    readMemory(hCPU->gpr[30], &boolParam);

    const char* paramString = (char*)(boolParam.keyPtr.getLE() + s_memoryBaseAddress);
    Log::print<PPC>("[{:08X}] Loaded dynamic bool parameter for {}: {}", hCPU->gpr[0], paramString, boolParam.value.getLE());

    if (std::string(paramString) == "IsShootByPlayer" && boolParam.value.getLE() == 1) {
        Log::print<PPC>("Arrow is being shot by the player!");
        shouldAdjustArrowTarget = true;
    }
}

void CemuHooks::hook_LoadDynamicVec3(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x030EC844;

    InlineParamVec3 vec3Param;
    readMemory(hCPU->gpr[31], &vec3Param);

    vec3Param.value.z = hCPU->fpr[0].fp0;
    writeMemory(hCPU->gpr[31], &vec3Param);
}
