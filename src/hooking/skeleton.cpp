#include "pch.h"

#include "instance.h"
#include "cemu_hooks.h"
#include "rendering/openxr.h"

struct Bone {
    std::string name;
    glm::vec3 localPos;
    glm::vec3 localRotEuler; // in radians
    glm::mat4 localMatrix;
    glm::mat4 worldMatrix;
    int parentIndex = -1;
    std::vector<int> childrenIndices;
    int indentLevel = 0;
};

class Skeleton {
public:
    void Parse(const std::string& data) {
        m_bones.clear();
        m_boneNameMap.clear();

        std::stringstream ss(data);
        std::string line;

        // this parses the bone hierarchy using the indentation levels to calculate parent-child relationships and model space transforms
        std::vector<std::pair<int, int>> parentStack;
        parentStack.push_back({ -1, -1 }); // Root parent is -1

        while (std::getline(ss, line)) {
            if (line.empty()) continue;

            int indent = 0;
            while (indent < line.length() && line[indent] == ' ') indent++;

            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;

            std::string content = line.substr(start);
            size_t p1 = content.find('|');
            if (p1 == std::string::npos) continue;

            std::string currentName = content.substr(0, p1);
            size_t lastChar = currentName.find_last_not_of(' ');
            if (lastChar != std::string::npos) currentName = currentName.substr(0, lastChar + 1);

            size_t p2 = content.find('|', p1 + 1);
            if (p2 == std::string::npos) continue;

            glm::vec3 pos, rotEuler;
            std::stringstream ssPos(content.substr(p1 + 1, p2 - p1 - 1));
            ssPos >> pos.x >> pos.y >> pos.z;
            std::stringstream ssRot(content.substr(p2 + 1));
            ssRot >> rotEuler.x >> rotEuler.y >> rotEuler.z;

            Bone bone;
            bone.name = currentName;
            bone.localPos = pos;
            bone.localRotEuler = rotEuler;
            bone.indentLevel = indent;
            bone.localMatrix = glm::translate(glm::identity<glm::mat4>(), pos) * glm::eulerAngleZYX(rotEuler.x, rotEuler.y, rotEuler.z);

            while (parentStack.size() > 1 && parentStack.back().first >= indent) {
                parentStack.pop_back();
            }

            bone.parentIndex = parentStack.back().second;

            int newIndex = (int)m_bones.size();
            if (bone.parentIndex != -1) {
                m_bones[bone.parentIndex].childrenIndices.push_back(newIndex);
            }

            m_bones.push_back(bone);
            m_boneNameMap[bone.name] = newIndex;

            parentStack.push_back({ indent, newIndex });
        }

        UpdateWorldMatrices();
    }

    void UpdateWorldMatrices() {
        for (auto& bone : m_bones) {
            if (bone.parentIndex == -1) {
                bone.worldMatrix = bone.localMatrix;
            }
            else {
                bone.worldMatrix = m_bones[bone.parentIndex].worldMatrix * bone.localMatrix;
            }
        }
    }

    glm::mat4 CalculateLocalMatrixFromWorld(int boneIndex, const glm::mat4& targetWorldMatrix) {
        if (boneIndex < 0 || boneIndex >= m_bones.size()) return glm::identity<glm::mat4>();

        const Bone& bone = m_bones[boneIndex];
        if (bone.parentIndex == -1) {
            return targetWorldMatrix;
        }

        const glm::mat4& parentWorldMatrix = m_bones[bone.parentIndex].worldMatrix;
        return glm::inverse(parentWorldMatrix) * targetWorldMatrix;
    }

    void SolveTwoBoneIK(int rootIdx, int midIdx, int endIdx, const glm::vec3& targetPos, const glm::vec3& poleVector, float boneForwardSign, float upperArmStretchBias = 0.75f) {
        if (rootIdx < 0 || rootIdx >= m_bones.size() ||
            midIdx < 0 || midIdx >= m_bones.size() ||
            endIdx < 0 || endIdx >= m_bones.size()) {
            return;
        }

        Bone& rootBone = m_bones[rootIdx];
        Bone& midBone = m_bones[midIdx];
        Bone& endBone = m_bones[endIdx];

        // get parent world matrix (clavicle)
        glm::mat4 parentWorld = glm::identity<glm::mat4>();
        if (rootBone.parentIndex != -1) {
            parentWorld = m_bones[rootBone.parentIndex].worldMatrix;
        }

        glm::vec3 rootPos = glm::vec3(parentWorld * glm::vec4(rootBone.localPos, 1.0f));

        // get lengths
        float l1_orig = glm::length(midBone.localPos);
        float l2_orig = glm::length(endBone.localPos);
        float l1 = l1_orig;
        float l2 = l2_orig;
        float totalLength = l1 + l2;

        // solve IK
        glm::vec3 dir = targetPos - rootPos;
        float dist = glm::length(dir);

        // weighted stretch: upper arm absorbs more of the extra distance
        float epsilon = 0.001f;
        if (dist > totalLength - epsilon) {
            float extra = dist - totalLength + epsilon;
            l1 += extra * upperArmStretchBias;
            l2 += extra * (1.0f - upperArmStretchBias);
        }

        // clamp distance
        dist = glm::clamp(dist, epsilon, l1 + l2 - epsilon);

        // law of cosines for angle at shoulder (alpha)
        float cosAlpha = (l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist);
        float alpha = glm::acos(glm::clamp(cosAlpha, -1.0f, 1.0f));

        // plane construction
        glm::vec3 dirNorm = glm::normalize(dir);
        glm::vec3 planeNormal = glm::normalize(glm::cross(dirNorm, poleVector));
        glm::vec3 ortho = glm::normalize(glm::cross(planeNormal, dirNorm));

        // arm 1 direction (world)
        // glm vec * float is fine; glm vec * double trips overload resolution on Linux.
        glm::vec3 arm1Dir = glm::normalize(dirNorm * static_cast<float>(cos(alpha)) + ortho * static_cast<float>(sin(alpha)));

        // arm 2 direction (world)
        glm::vec3 elbowPos = rootPos + arm1Dir * l1;
        glm::vec3 arm2Dir = glm::normalize(targetPos - elbowPos);

        // construct rotation matrices
        glm::vec3 x1 = arm1Dir * boneForwardSign;
        glm::vec3 z1 = planeNormal;
        glm::vec3 y1 = glm::cross(z1, x1);
        glm::mat3 rot1World = glm::mat3(x1, -y1, -z1);

        glm::vec3 x2 = arm2Dir * boneForwardSign;
        glm::vec3 z2 = planeNormal;
        glm::vec3 y2 = glm::cross(z2, x2);
        glm::mat3 rot2World = glm::mat3(x2, -y2, -z2);

        // convert to local space, stretching the upper arm position more
        glm::mat4 arm1Local = glm::inverse(parentWorld) * glm::mat4(rot1World);
        arm1Local[3] = glm::vec4(rootBone.localPos, 1.0f);

        glm::mat4 arm1World = parentWorld * arm1Local;
        glm::mat4 arm2Local = glm::inverse(arm1World) * glm::mat4(rot2World);
        arm2Local[3] = glm::vec4(midBone.localPos * (l1 / l1_orig), 1.0f);

        // update skeleton
        rootBone.localMatrix = arm1Local;
        midBone.localMatrix = arm2Local;
        UpdateWorldMatrices();
    }

    int GetBoneIndex(const std::string& name) const {
        auto it = m_boneNameMap.find(name);
        if (it != m_boneNameMap.end()) return it->second;
        return -1;
    }

    Bone* GetBone(int index) {
        if (index < 0 || index >= m_bones.size()) return nullptr;
        return &m_bones[index];
    }

    Bone* GetBone(const std::string& name) {
        int idx = GetBoneIndex(name);
        if (idx == -1) return nullptr;
        return &m_bones[idx];
    }

private:
    std::vector<Bone> m_bones;
    std::map<std::string, int> m_boneNameMap;
};

const std::string SKELETON_DATA = R"(
Root | 0 0 0 | 0 0 0
  Skl_Root | 0 0.99426 0 | 0 0 0
    Spine_1 | 0 0 0 | 1.5708 0 1.5708
      Spine_2 | 0.136 0 0 | 0 0 0
        Clavicle_L | 0.23961 -0.00002 0.03291 | 0 -1.5708 0
          Arm_1_L | 0.15 0 0.01074 | 0 0 0
            Arm_1_Assist_L | 0.06 0.00002 0 | 0 0 0
            Arm_2_L | 0.24 0 0 | 0 0 0
              Elbow_L | 0.04151 -0.02934 0.00021 | 0 0 0
              Wrist_Assist_L | 0.25809 0.00002 -0.00012 | 0 0 0
              Wrist_L | 0.27718 0 0 | 0 0 0
                Weapon_L | 0.1069 0.00002 0.02769 | 1.5708 0 3.14159
          Clavicle_Assist_L | 0.116 0 0.0107 | 0 0 0
        Clavicle_R | 0.2396 -0.00002 -0.03291 | 3.14159 -1.5708 0
          Arm_1_R | -0.15 0 -0.01074 | 0 0 0
            Arm_1_Assist_R | -0.06 -0.00002 0 | 0 0 0
            Arm_2_R | -0.24 0 0 | 0 0 0
              Elbow_R | -0.04151 0.02934 -0.0002 | 0 0 0
              Wrist_Assist_R | -0.25809 -0.00002 0.00012 | 0 0 0
              Wrist_R | -0.27718 0 0 | 0 0 0
                Weapon_R | -0.1069 -0.00002 -0.02769 | 1.5708 0 0
          Clavicle_Assist_R | -0.116 0 -0.0107 | 0 0 0
        Neck | 0.26326 0 0 | 0 0 0
          Head | 0.12447 0 0 | 0 0 0
            Face_Root | 0 0 0 | 0 0 0
              Chin | 0.04787 0.05757 0 | 0 0 2.53073
              Eyeball_L | 0.07017 0.12036 0.04815 | 0 0 0
              Eyeball_R | 0.07017 0.12036 -0.04815 | 0 0 0
)";

/*
    Waist | 0 0 0 | 1.5708 0 -1.5708
      Leg_1_L | 0.10854 0.0165 -0.11209 | 0 0 0
        Knee_L | 0.39619 0.0308 0 | 0 0 0
        Leg_2_L | 0.42 0 -0.08727 | 0 0 0
      Leg_1_R | 0.10854 0.0165 0.11209 | 0 0 3.14159
        Knee_R | -0.39619 -0.0308 0 | 0 0 0
        Leg_2_R | -0.42 0 -0.08727 | 0 0 0
 */

static bool isFaceBone(const std::string_view& boneName) {
    if (boneName.starts_with("Eye" /*lid*/) || boneName.starts_with("Cheek") || boneName.starts_with("Lip") || boneName.starts_with("Hair")) {
        return true;
    }
    if (boneName == "Nose" || boneName == "Ponytail_A_1" || boneName == "Neck" || boneName == "Head" || boneName.starts_with("Teeth_") || boneName.starts_with("Chin")) {
        return true;
    }
    return false;
}

static Skeleton s_skeleton;
static bool s_skeletonParsed = false;
static glm::vec3 s_manualBodyOffset = glm::vec3(0.0f, 0.0f, -0.075f);
static glm::mat4 s_handCorrectionRotationLeft = glm::mat4(1.0f);
static glm::mat4 s_handCorrectionRotationRight = glm::mat4(1.0f);

static glm::fquat ExtractPlayerBodyYaw(const glm::fmat4& sourceMtx) {
    glm::fquat sourceRot = glm::quat_cast(sourceMtx);
    float yProj = glm::dot(glm::fvec3(sourceRot.x, sourceRot.y, sourceRot.z), glm::fvec3(0.0f, 1.0f, 0.0f));
    glm::fquat yawRot(sourceRot.w, 0.0f, yProj, 0.0f);
    float lenSq = glm::dot(yawRot, yawRot);
    yawRot = (lenSq > 0.000001f) ? yawRot * (1.0f / sqrtf(lenSq)) : glm::identity<glm::fquat>();

    // fix body inversion
    return yawRot * glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

static glm::fvec3 RemoveHeadsetHorizontalOffset(glm::fvec3 trackedPos) {
    glm::fvec3 appliedHeadPos = CemuHooks::GetAppliedRoomscaleHeadPosition();
    trackedPos.x -= appliedHeadPos.x;
    trackedPos.z -= appliedHeadPos.z;
    return trackedPos;
}

void CemuHooks::hook_ModifyBoneMatrix(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    const uint32_t gsysModelPtr = hCPU->gpr[3];
    const uint32_t matrixPtr = hCPU->gpr[4];
    const uint32_t scalePtr = hCPU->gpr[5];
    const uint32_t boneNamePtr = hCPU->gpr[6];
    if (!gsysModelPtr || !matrixPtr || !scalePtr || !boneNamePtr) return;

    const auto modelName = getMemory<sead::FixedSafeString100>(gsysModelPtr + 0x128);
    if (modelName.getLE() != "GameROMPlayer") return;

    std::string boneName((char*)(s_memoryBaseAddress + boneNamePtr));
    const bool isLeft = boneName.ends_with("_L");
    const OpenXR::EyeSide side = isLeft ? OpenXR::EyeSide::LEFT : OpenXR::EyeSide::RIGHT;

    // helpers to write back matrix and scale
    auto writeBoneMatrix = [&](const glm::mat4x3& mtx, const glm::fvec3& scale) {
        BEMatrix34 m{ mtx };
        setMemory(matrixPtr, m);
        setMemory(scalePtr, scale);
    };
    auto writeBoneQuat = [&](const glm::vec3& pos, const glm::quat& rot, const glm::fvec3& scale) {
        BEMatrix34 m{ pos, rot };
        setMemory(matrixPtr, m);
        setMemory(scalePtr, scale);
    };

    if (IsThirdPerson()) {
        if (isFaceBone(boneName)) {
            setMemory(scalePtr, glm::fvec3(1.0f));
        }
        return;
    }


    if (isFaceBone(boneName)) {
        writeBoneQuat(glm::fvec3(), glm::identity<glm::fquat>(), glm::fvec3(0.05f));
        return;
    }

    glm::fvec3 boneScale = getMemory<BEVec3>(scalePtr).getLE();
    const glm::fmat4 playerMtx4 = glm::fmat4(getMemory<BEMatrix34>(s_playerMtxAddress).getLEMatrix());
    const glm::mat4 cameraMtx = s_lastCameraMtx;

    const OpenXR::InputState inputs = VRManager::instance().XR->m_input.load();
    if (!inputs.shared.pose[side].isActive)
        return;

    const auto& pose = inputs.shared.poseLocation[side];
    glm::fvec3 controllerPos = glm::fvec3();
    glm::fquat controllerRot = glm::identity<glm::fquat>();
    if (pose.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        controllerPos = ToGLM(pose.pose.position);
        controllerPos = RemoveHeadsetHorizontalOffset(controllerPos);
    }
    if (pose.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
        controllerRot = ToGLM(pose.pose.orientation);

    // one-time skeleton and hand correction initialization
    if (!s_skeletonParsed) {
        s_skeleton.Parse(SKELETON_DATA);
        s_skeletonParsed = true;

        // left: 90 Y -> -90 Z -> 30 Z
        glm::fquat wristL = glm::identity<glm::fquat>();
        wristL *= glm::angleAxis(glm::radians(90.0f), glm::fvec3(0, 1, 0));
        wristL *= glm::angleAxis(glm::radians(-90.0f), glm::fvec3(0, 0, 1));
        wristL *= glm::angleAxis(glm::radians(30.0f), glm::fvec3(0, 0, 1));

        // right: -90 Z -> -180 Y -> 270 X -> 30 Z tweak
        glm::fquat wristR = glm::identity<glm::fquat>();
        wristR *= glm::angleAxis(glm::radians(-90.0f), glm::fvec3(0, 0, 1));
        wristR *= glm::angleAxis(glm::radians(-180.0f), glm::fvec3(0, 1, 0));
        wristR *= glm::angleAxis(glm::radians(270.0f), glm::fvec3(1, 0, 0));
        wristR *= glm::angleAxis(glm::radians(30.0f), glm::fvec3(0, 0, 1));

        s_handCorrectionRotationLeft = glm::mat4_cast(wristL);
        s_handCorrectionRotationRight = glm::mat4_cast(wristR);
    }

    int boneIndex = s_skeleton.GetBoneIndex(boneName);
    if (boneIndex == -1)
        return;

    // compute the controller target in model space
    auto calcControllerTargetModel = [&]() -> glm::mat4 {
        glm::mat4 handCorrectionMtx = isLeft ? s_handCorrectionRotationLeft : s_handCorrectionRotationRight;
        glm::mat4 controllerMat = glm::translate(glm::identity<glm::mat4>(), controllerPos) * glm::mat4_cast(controllerRot) * handCorrectionMtx;
        glm::mat4 targetWorld = cameraMtx * controllerMat;

        const char* weaponName = isLeft ? "Weapon_L" : "Weapon_R";
        if (Bone* weapon = s_skeleton.GetBone(weaponName)) {
            glm::vec3 weaponOffset = glm::vec3(weapon->localMatrix[3]);
            targetWorld = targetWorld * glm::translate(glm::identity<glm::mat4>(), -weaponOffset);
        }

        return glm::inverse(playerMtx4) * targetWorld;
    };

    glm::mat4 calculatedLocalMat = s_skeleton.GetBone(boneIndex)->localMatrix;

    // override the root transform so the body aligns with the headset yaw
    if (boneName == "Skl_Root") {
        auto headsetPose = VRManager::instance().XR->GetRenderer()->GetMiddlePose();
        glm::mat4 headsetMtx = headsetPose.value_or(ToMat4(glm::fvec3(0)));

        // calculate eye offset from eyeball bones (once)
        static glm::vec3 eyeOffset = glm::vec3(0.0f);
        static bool offsetCalculated = false;
        if (!offsetCalculated) {
            Bone* eyeL = s_skeleton.GetBone("Eyeball_L");
            Bone* eyeR = s_skeleton.GetBone("Eyeball_R");
            Bone* sklRoot = s_skeleton.GetBone("Skl_Root");
            if (eyeL && eyeR && sklRoot) {
                glm::vec3 eyePos = (glm::vec3(eyeL->worldMatrix[3]) + glm::vec3(eyeR->worldMatrix[3])) * 0.5f;
                eyeOffset = eyePos - glm::vec3(sklRoot->worldMatrix[3]);
                offsetCalculated = true;
            }
        }

        // headset in model space
        glm::mat4 headsetModel = glm::inverse(playerMtx4) * cameraMtx * headsetMtx;
        headsetModel[3].x = 0.0f;
        headsetModel[3].z = 0.0f;

        glm::fquat yawRot = ExtractPlayerBodyYaw(headsetModel);

        // position the root so that yawRot * eyeOffset lands at the headset position
        glm::vec3 targetPos = glm::vec3(headsetModel[3]) - (yawRot * eyeOffset) + (yawRot * s_manualBodyOffset);

        // update skeleton for children (hands)
        if (Bone* rootBone = s_skeleton.GetBone("Skl_Root")) {
            rootBone->localMatrix = glm::translate(glm::identity<glm::mat4>(), targetPos) * glm::mat4_cast(yawRot);
            s_skeleton.UpdateWorldMatrices();
        }

        writeBoneQuat(targetPos, yawRot, boneScale);
        return;
    }

    // solve upper arm IK so the arms reach the VR controllers
    if (boneName == "Arm_1_L" || boneName == "Arm_1_R" ||
        boneName == "Elbow_L" || boneName == "Elbow_R" ||
        boneName == "Wrist_Assist_L" || boneName == "Wrist_Assist_R") {

        int arm1Index = s_skeleton.GetBoneIndex(isLeft ? "Arm_1_L" : "Arm_1_R");
        int arm2Index = s_skeleton.GetBoneIndex(isLeft ? "Arm_2_L" : "Arm_2_R");
        int wristIndex = s_skeleton.GetBoneIndex(isLeft ? "Wrist_L" : "Wrist_R");

        if (arm1Index != -1 && arm2Index != -1 && wristIndex != -1) {
            glm::vec3 targetPos = glm::vec3(calcControllerTargetModel()[3]);

            // pole vector (elbow hint): left-down-back / right-down-back, rotated by body yaw
            glm::vec3 poleDir = isLeft ? glm::vec3(1.0f, -1.0f, -0.5f) : glm::vec3(-1.0f, -1.0f, -0.5f);
            if (Bone* rootBone = s_skeleton.GetBone("Skl_Root"))
                poleDir = glm::quat_cast(rootBone->localMatrix) * poleDir;

            s_skeleton.SolveTwoBoneIK(arm1Index, arm2Index, wristIndex, targetPos, poleDir, isLeft ? 1.0f : -1.0f);
            calculatedLocalMat = s_skeleton.GetBone(boneIndex)->localMatrix;
        }
    }

    // align the wrist and wrist assist directly with the controller pose
    if (boneName == "Wrist_L" || boneName == "Wrist_R" ||
        boneName == "Wrist_Assist_L" || boneName == "Wrist_Assist_R") {
        calculatedLocalMat = s_skeleton.CalculateLocalMatrixFromWorld(boneIndex, calcControllerTargetModel());
    }

    writeBoneMatrix(glm::mat4x3(calculatedLocalMat), boneScale);
}
