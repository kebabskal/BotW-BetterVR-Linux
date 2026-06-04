#include "pch.h"
#include "entity_debugger.h"
#include "instance.h"
#include "utils/mod_settings.h"

#include "utils/debug_draw.h"

std::mutex g_actorListMutex;
std::unordered_map<uint32_t, std::pair<std::string, uint32_t>> s_knownActors;
glm::fvec3 CemuHooks::s_playerPos = {};
uint32_t CemuHooks::s_playerMtxAddress = 0;
uint32_t CemuHooks::s_cameraMtxAddress = 0;

void CemuHooks::hook_UpdateActorList(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    std::scoped_lock lock(g_actorListMutex);

    // r7 holds actor list size
    // r5 holds current actor index
    // r6 holds current actor* list entry

    // clear actor list when reiterating actor list again
    if (hCPU->gpr[5] == 0) {
        s_knownActors.clear();
    }

    uint32_t actorLinkPtr = hCPU->gpr[6] + offsetof(ActorWiiU, name) + offsetof(sead::FixedSafeString40, c_str);
    uint32_t actorNamePtr = 0;
    readMemoryBE(actorLinkPtr, &actorNamePtr);
    if (actorNamePtr == 0)
        return;

    char* actorName = (char*)s_memoryBaseAddress + actorNamePtr;

    if (actorName[0] != '\0') {
        // Log::print("Updating actor list [{}/{}] {:08x} - {}", hCPU->gpr[5], hCPU->gpr[7], hCPU->gpr[6], actorName);
        uint32_t actorId = hCPU->gpr[6] + stringToHash(actorName);
        s_knownActors.emplace(actorId, std::make_pair(actorName, hCPU->gpr[6]));
    }

    // if (strcmp(actorName, "Weapon_Sword_056") == 0) {
    //     // Log::print("Updating actor list [{}/{}] {:08x} - {}", hCPU->gpr[5], hCPU->gpr[7], hCPU->gpr[6], actorName);
    //     // float velocityY = 0.0f;
    //     // readMemoryBE(hCPU->gpr[6] + offsetof(ActorWiiU, velocity.y), &velocityY);
    //     // velocityY = velocityY * 1.5f;
    //     // writeMemoryBE(hCPU->gpr[6] + offsetof(ActorWiiU, velocity.y), &velocityY);
    //     s_currActorPtrs.emplace_back(hCPU->gpr[6]);
    // }
     if (strcmp(actorName, "GameROMPlayer") == 0) {
         BEMatrix34 mtx = {};
         uint32_t actorMtxPtr = hCPU->gpr[6] + offsetof(ActorWiiU, mtx);
         readMemory(actorMtxPtr, &mtx);
         s_playerPos = mtx.getPos().getLE();
         s_playerMtxAddress = actorMtxPtr;
         s_playerAddress = hCPU->gpr[6];
         //uint32_t vtableAddr = getMemory<BEType<uint32_t>>(hCPU->gpr[6] + offsetof(ActorWiiU, vtable)).getLE();
         //Log::print<INFO>("VTABLE = {:08X}", vtableAddr);
     }
     else if (strcmp(actorName, "GameRomCamera") == 0) {
         uint32_t actorMtxPtr = hCPU->gpr[6] + offsetof(ActorWiiU, mtx);
         s_cameraMtxAddress = actorMtxPtr;
     }
}
// ksys::phys::RigidBodyFromShape::create to create a RigidBody from a shape
// use Actor::getRigidBodyByName

std::unordered_map<uint32_t, std::pair<std::string, uint32_t>> s_alreadyAddedActors;

void EntityDebugger::UpdateEntityMemory() {
    std::scoped_lock lock(g_actorListMutex);

    // remove actors in s_alreadyAddedActors that are no longer in s_knownActors
    for (const auto& hash : s_alreadyAddedActors | std::views::keys) {
        if (!s_knownActors.contains(hash)) {
            RemoveEntity(hash);
        }
    }

    s_alreadyAddedActors = s_knownActors;

    // find the current player (GameROMPlayer)
    BEMatrix34 playerPos = {};
    for (const auto& actorData : s_knownActors | std::views::values) {
        if (actorData.first == "GameROMPlayer") {
            CemuHooks::readMemory(actorData.second + offsetof(ActorWiiU, mtx), &playerPos);
            glm::fvec3 newPlayerPos = playerPos.getPos().getLE();
            m_playerPos = newPlayerPos;

            // // set invisibility flag
            // {
            //     BEType<int32_t> flags = 0;
            //     readMemory(actorData.second + offsetof(ActorWiiU, flags3), &flags);
            //     flags = flags.getLE() | 0x800;
            //     writeMemory(actorData.second + offsetof(ActorWiiU, flags3), &flags);
            // }
            // {
            //     BEType<int32_t> flags = 0;
            //     readMemory(actorData.second + offsetof(ActorWiiU, flags2), &flags);
            //     flags = flags.getLE() | 0x20;
            //     writeMemory(actorData.second + offsetof(ActorWiiU, flags2), &flags);
            //     writeMemory(actorData.second + offsetof(ActorWiiU, flags2Copy), &flags);
            // }
            // {
            //     float lodDrawDistanceMultiplier = 0;
            //     readMemory(actorData.second + offsetof(ActorWiiU, lodDrawDistanceMultiplier), &lodDrawDistanceMultiplier);
            //     lodDrawDistanceMultiplier = 0.0f;
            //     writeMemory(actorData.second + offsetof(ActorWiiU, lodDrawDistanceMultiplier), &lodDrawDistanceMultiplier);
            // }
            // {
            //     float startModelOpacity = 0;
            //     readMemory(actorData.second + offsetof(ActorWiiU, startModelOpacity), &startModelOpacity);
            //     startModelOpacity = 0.0f;
            //     writeMemory(actorData.second + offsetof(ActorWiiU, startModelOpacity), &startModelOpacity);
            // }
            // {
            //     BEType<float> modelOpacity = 1.0f;
            //     readMemory(actorData.second + offsetof(ActorWiiU, modelOpacity), &modelOpacity);
            //     modelOpacity = 1.0f;
            //     writeMemory(actorData.second + offsetof(ActorWiiU, modelOpacity), &modelOpacity);
            // }
            // {
            //     uint8_t opacityOrDoFlushOpacityToGPU = 0;
            //     writeMemory(actorData.second + offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU), &opacityOrDoFlushOpacityToGPU);
            //     writeMemory(actorData.second + offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU)+1, &opacityOrDoFlushOpacityToGPU);
            //     writeMemory(actorData.second + offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU)-1, &opacityOrDoFlushOpacityToGPU);
            //     writeMemory(actorData.second + offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU)-2, &opacityOrDoFlushOpacityToGPU);
            // }
        }
        else if (actorData.first == "GameRomCamera") {
            CemuHooks::readMemory(actorData.second + offsetof(ActorWiiU, mtx), &playerPos);
            glm::fvec3 newPlayerPos = playerPos.getPos().getLE();
        }
        else if (actorData.first.starts_with("Weapon_Sword")) {
            // BEType<float> modelOpacity = 1.0f;
            // writeMemory(actorData.second + offsetof(ActorWiiU, modelOpacity), &modelOpacity);
            // uint8_t opacityOrDoFlushOpacityToGPU = 1;
            // writeMemory(actorData.second + offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU), &opacityOrDoFlushOpacityToGPU);
        }
    }

    // add actors that aren't in the overlay already
    for (auto& [actorId, actorInfo] : s_knownActors) {
        uint32_t actorPtr = actorInfo.second;
        const std::string& actorName = actorInfo.first;

        auto addField = [&]<typename T>(const std::string& name, uint32_t offset) -> void {
            uint32_t address = actorPtr + offset;
            AddOrUpdateEntity(actorId, actorName, name, address, CemuHooks::getMemory<T>(address), true);
        };

        auto addMemoryRange = [&](const std::string& name, const uint32_t addressPtr, const uint32_t size) -> void {
            uint32_t address = 0;
            if (CemuHooks::readMemoryBE(addressPtr, &address); address != 0) {
                AddOrUpdateEntity(actorId, actorName, name, address, MemoryRange{ address, address + size, std::make_unique<MemoryEditor>() }, true);
            }
        };

        if (actorName.starts_with("Weapon")) {
            addField.operator()<BEVec3>("Weapon::originalScale", offsetof(Weapon, originalScale));
            addMemoryRange("Weapon::actorAtk.struct7Ptr", actorPtr + offsetof(Weapon, actorAtk.struct7Ptr), 0x2D8);
            addMemoryRange("Weapon::actorAtk.attackSensorStruct7Ptr", actorPtr + offsetof(Weapon, actorAtk.attackSensorPtr), 0x5C);
            addMemoryRange("Weapon::actorAtk.struct8Ptr", actorPtr + offsetof(Weapon, actorAtk.struct8Ptr), 0x714);
            addMemoryRange("Weapon::actorAtk.attackSensorStruct8Ptr", actorPtr + offsetof(Weapon, actorAtk.attackSensorStruct8Ptr), 0x28);
            addField.operator()<BEType<uint16_t>>("Weapon::weaponFlags", offsetof(Weapon, weaponFlags));
            addField.operator()<BEType<uint16_t>>("Weapon::otherFlags", offsetof(Weapon, otherFlags));
            addField.operator()<BEType<uint32_t>>("Weapon::heldIndex", offsetof(Weapon, field_5F4));
        }

        if (actorName.starts_with("GameROMPlayer")) {
            addMemoryRange("ActorWeapons", actorPtr + offsetof(PlayerOrEnemy, weapons), 0x68);

            //uint32_t hexFlags = CemuHooks::getMemory<BEType<uint32_t>>(actorPtr + 0x8DC).getLE();
            //Log::print<VERBOSE>("CanUseCamera = {:08X}", hexFlags);
        }

        BEMatrix34 mtx = CemuHooks::getMemory<BEMatrix34>(actorPtr + offsetof(ActorWiiU, mtx));
        AddOrUpdateEntity(actorId, actorName, "mtx", actorPtr + offsetof(ActorWiiU, mtx), mtx);
        if (playerPos.pos_x.getLE() != 0.0f) {
            SetPosition(actorId, playerPos.getPos(), mtx.getPos());
        }
        SetRotation(actorId, mtx.getRotLE());

        BEVec3 aabbMin = CemuHooks::getMemory<BEVec3>(actorPtr + offsetof(ActorWiiU, aabb.min));
        BEVec3 aabbMax = CemuHooks::getMemory<BEVec3>(actorPtr + offsetof(ActorWiiU, aabb.max));
        if (aabbMin.x.getLE() != 0.0f) {
            SetAABB(actorId, aabbMin.getLE(), aabbMax.getLE());
        }

        float distance = glm::distance(m_playerPos, mtx.getPos().getLE());
        glm::fvec3 pos = mtx.getPos().getLE();
        glm::fquat rot = mtx.getRotLE();

        glm::fvec3 localMin = aabbMin.getLE();
        glm::fvec3 localMax = aabbMax.getLE();
        glm::fvec3 localCenter = (localMin + localMax) * 0.5f;
        glm::fvec3 halfExtents = (localMax - localMin) * 0.5f;
        bool matchesFilter = m_filter.empty() || actorName.find(m_filter) != std::string::npos;

        if (GetSettings().ShouldShowEntityBoxesIn3DView() && matchesFilter && distance >= m_worldAABBMinDistance && distance <= m_worldAABBMaxDistance) {
            if (glm::all(glm::greaterThan(halfExtents, glm::vec3(0.0f)))) {
                glm::fvec3 worldCenter = pos + glm::mat3_cast(rot) * localCenter;
                DebugDraw::instance().Box(worldCenter, halfExtents, rot, IM_COL32(255, 255, 255, 255 / 1), 1.0f);
            }
            else {
                DebugDraw::instance().Dot(pos, 0.15f, IM_COL32(255, 255, 255, 255/1));
            }
        }

        // uint32_t physicsMtxPtr = 0;
        // if (readMemoryBE(actorPtr + offsetof(ActorWiiU, physicsMtxPtr), &physicsMtxPtr); physicsMtxPtr != 0) {
        //     overlay->AddOrUpdateEntity(actorId, actorName, "physicsMtx", physicsMtxPtr, getMemory<BEMatrix34>(physicsMtxPtr));
        // }
        addField.operator()<BEVec3>("velocity", offsetof(ActorWiiU, velocity));
        addField.operator()<BEVec3>("angularVelocity", offsetof(ActorWiiU, angularVelocity));
        addField.operator()<BEVec3>("scale", offsetof(ActorWiiU, scale));
        // addField.operator()<BEVec3>("previousPos", offsetof(ActorWiiU, previousPos));
        // addField.operator()<BEVec3>("previousPos2", offsetof(ActorWiiU, previousPos2));
        // addField.operator()<float>("dispDistSq", offsetof(ActorWiiU, dispDistSq));
        // addField.operator()<float>("deleteDistSq", offsetof(ActorWiiU, deleteDistSq));
        // addField.operator()<float>("loadDistP10", offsetof(ActorWiiU, loadDistP10));
        // addField.operator()<uint32_t>("modelBindInfoPtr", offsetof(ActorWiiU, modelBindInfoPtr));
        // addField.operator()<uint32_t>("gsysModelPtr", offsetof(ActorWiiU, gsysModelPtr));
        // addField.operator()<float>("startModelOpacity", offsetof(ActorWiiU, startModelOpacity));
        // addField.operator()<float>("modelOpacity", offsetof(ActorWiiU, modelOpacity));
        // addField.operator()<uint8_t>("opacityOrDoFlushOpacityToGPU", offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU));
        addField.operator()<BEVec3>("aabb_min", offsetof(ActorWiiU, aabb.min));
        addField.operator()<BEVec3>("aabb_max", offsetof(ActorWiiU, aabb.max));
        addField.operator()<uint32_t>("flags2", offsetof(ActorWiiU, flags2));
        addField.operator()<uint32_t>("flags2Copy", offsetof(ActorWiiU, flags2Copy));
        addField.operator()<uint32_t>("flags", offsetof(ActorWiiU, flags));
        addField.operator()<uint32_t>("flags3", offsetof(ActorWiiU, flags3));

        addField.operator()<uint32_t>("hashId", offsetof(ActorWiiU, hashId));
        addMemoryRange("physics", actorPtr + offsetof(ActorWiiU, actorPhysicsPtr), 0xE0);
        addMemoryRange("actorX6A0", actorPtr + offsetof(ActorWiiU, actorX6A0Ptr), 0x6C);
        addMemoryRange("chemicals", actorPtr + offsetof(ActorWiiU, chemicalsPtr), 0x64);
        addMemoryRange("reactions", actorPtr + offsetof(ActorWiiU, reactionsPtr), 0x0C);
        // addField.operator()<float>("lodDrawDistanceMultiplier", offsetof(ActorWiiU, lodDrawDistanceMultiplier));
    }

    // other systems might've added memory to the overlay, so hence this is a separate loop
    for (auto& entity : m_entities | std::views::values) {
        if (!entity.isEntity)
            continue;

        for (auto& value : entity.values) {
            if (!value.frozen)
                continue;

            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, BEType<uint32_t>>) {
                    CemuHooks::setMemory(value.value_address, arg);
                }
                else if constexpr (std::is_same_v<T, BEType<int32_t>>) {
                    CemuHooks::setMemory(value.value_address, arg);
                }
                else if constexpr (std::is_same_v<T, BEType<float>>) {
                    CemuHooks::setMemory(value.value_address, arg);
                }
                else if constexpr (std::is_same_v<T, BEVec3>) {
                    CemuHooks::setMemory(value.value_address, arg);
                }
                else if constexpr (std::is_same_v<T, BEMatrix34>) {
                    CemuHooks::setMemory(value.value_address, arg);
                }
                else if constexpr (std::is_same_v<T, uint8_t>) {
                    CemuHooks::setMemory(value.value_address, arg);
                }
            }, value.value);

        }
    }
}

void EntityDebugger::DrawWorldSpaceOverlaySettings(bool* changed) {
    auto& settings = GetSettings();

    bool showEntityBoxes = settings.debugShowEntityBoxesIn3DView;
    if (ImGui::Checkbox("Show Entity Boxes In 3D View", &showEntityBoxes)) {
        settings.debugShowEntityBoxesIn3DView = showEntityBoxes;
        *changed = true;
    }

    if (showEntityBoxes) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Min 3D Box Distance", &m_worldAABBMinDistance, 1.0f, 0.0f, 10000.0f, "%.0f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Max 3D Box Distance", &m_worldAABBMaxDistance, 1.0f, 0.0f, 10000.0f, "%.0f");
    }

    bool showRaycastLines = settings.debugShowRaycastLines;
    if (ImGui::Checkbox("Show Raycast Lines", &showRaycastLines)) {
        settings.debugShowRaycastLines = showRaycastLines;
        *changed = true;
    }

    bool showRoomscalePhysics = settings.debugShowRoomscalePhysics;
    if (ImGui::Checkbox("Show Roomscale Physics", &showRoomscalePhysics)) {
        settings.debugShowRoomscalePhysics = showRoomscalePhysics;
        *changed = true;
    }
}

void EntityDebugger::DrawEntityInspectorContent() {
    static char buf[256];
    ImGui::InputText("Entity Filter", buf, std::size(buf));
    m_filter = buf;

    // display entities
    if (ImGui::CollapsingHeader("Entity List")) {
        // sort m_entities by priority
        std::multimap<float, std::reference_wrapper<Entity>> sortedEntities;
        for (auto& entity : m_entities | std::views::values) {
            if (m_filter.empty() || entity.name.find(m_filter) != std::string::npos) {
                bool isAnyValueFrozen = std::ranges::any_of(entity.values, [](auto& value) { return value.frozen; });
                // give priority to frozen entities
                sortedEntities.emplace(isAnyValueFrozen ? 0.0f - entity.priority : entity.priority, entity);
            }
        }

        for (auto& entity : sortedEntities | std::views::values) {
            std::string id = entity.get().name + "##" + std::to_string(entity.get().values[0].value_address);
            ImGui::Text(std::format("{}: dist={}", entity.get().name, std::abs(entity.get().priority)).c_str());
            ImGui::PushID(id.c_str());

            for (auto& value : entity.get().values) {
                ImGui::PushID(value.value_name.c_str());

                ImGui::Checkbox("##Frozen", &value.frozen);
                ImGui::SameLine();
                if (ImGui::Button("Copy")) {
                    ImGui::SetClipboardText(std::format("0x{:08x}", value.value_address).c_str());
                }
                ImGui::SameLine();

                ImGui::BeginDisabled(!value.frozen && false);

                std::visit([&]<typename T0>(T0&& arg) {
                    using T = std::decay_t<T0>;

                    if constexpr (std::is_same_v<T, BEType<uint32_t>>) {
                        uint32_t val = std::get<BEType<uint32_t>>(value.value).getLE();
                        if (ImGui::DragScalar(value.value_name.c_str(), ImGuiDataType_U32, &val)) {
                            std::get<BEType<uint32_t>>(value.value) = val;
                        }
                    }
                    else if constexpr (std::is_same_v<T, BEType<int32_t>>) {
                        int32_t val = std::get<BEType<int32_t>>(value.value).getLE();
                        if (ImGui::DragScalar(value.value_name.c_str(), ImGuiDataType_S32, &val)) {
                            std::get<BEType<int32_t>>(value.value) = val;
                        }
                    }
                    else if constexpr (std::is_same_v<T, BEType<float>>) {
                        float val = std::get<BEType<float>>(value.value).getLE();
                        if (ImGui::DragScalar(value.value_name.c_str(), ImGuiDataType_Float, &val)) {
                            std::get<BEType<float>>(value.value) = val;
                        }
                    }
                    else if constexpr (std::is_same_v<T, BEType<uint8_t>>) {
                        uint8_t val = std::get<BEType<uint8_t>>(value.value).getLE();
                        if (ImGui::DragScalar(value.value_name.c_str(), ImGuiDataType_U8, &val)) {
                            std::get<BEType<uint8_t>>(value.value) = val;
                        }
                    }
                    else if constexpr (std::is_same_v<T, BEType<uint16_t>>) {
                        uint16_t val = std::get<BEType<uint16_t>>(value.value).getLE();
                        if (ImGui::DragScalar(value.value_name.c_str(), ImGuiDataType_U16, &val)) {
                            std::get<BEType<uint16_t>>(value.value) = val;
                        }
                    }
                    else if constexpr (std::is_same_v<T, BEVec3>) {
                        float xyz[3] = { std::get<BEVec3>(value.value).x.getLE(), std::get<BEVec3>(value.value).y.getLE(), std::get<BEVec3>(value.value).z.getLE() };
                        if (ImGui::DragFloat3(value.value_name.c_str(), xyz)) {
                            std::get<BEVec3>(value.value).x = xyz[0];
                            std::get<BEVec3>(value.value).y = xyz[1];
                            std::get<BEVec3>(value.value).z = xyz[2];
                        }
                    }
                    else if constexpr (std::is_same_v<T, BEMatrix34>) {
                        if (value.expanded) {
                            auto mtx = std::get<BEMatrix34>(value.value).getLEMatrix();
                            ImGui::Indent(); bool row0Changed = ImGui::DragFloat4("Row 0", &mtx[0].x, 10.0f, 0, 0, nullptr, ImGuiSliderFlags_NoRoundToFormat); ImGui::Unindent();
                            ImGui::Indent(); bool row1Changed = ImGui::DragFloat4("Row 1", &mtx[1].x, 10.0f, 0, 0, nullptr, ImGuiSliderFlags_NoRoundToFormat); ImGui::Unindent();
                            ImGui::Indent(); bool row2Changed = ImGui::DragFloat4("Row 2", &mtx[2].x, 10.0f, 0, 0, nullptr, ImGuiSliderFlags_NoRoundToFormat); ImGui::Unindent();
                            if (row0Changed || row1Changed || row2Changed) {
                                std::get<BEMatrix34>(value.value).setLEMatrix(mtx);
                            }
                        }
                        else {
                            float xyz[3] = { std::get<BEMatrix34>(value.value).pos_x.getLE(), std::get<BEMatrix34>(value.value).pos_y.getLE(), std::get<BEMatrix34>(value.value).pos_z.getLE() };
                            if (ImGui::DragFloat3(value.value_name.c_str(), xyz)) {
                                std::get<BEMatrix34>(value.value).pos_x = xyz[0];
                                std::get<BEMatrix34>(value.value).pos_y = xyz[1];
                                std::get<BEMatrix34>(value.value).pos_z = xyz[2];
                            }
                        }

                        // allow matrix to be expandable to show more
                        ImGui::SameLine();
                        if (ImGui::Button("...")) {
                            value.expanded = !value.expanded;
                        }
                    }
                    else if constexpr (std::is_same_v<T, MemoryRange>) {
                        if (ImGui::Button(std::format("{} '{}' memory at {:08X}", value.expanded ? "Close" : "Open", value.value_name, value.value_address).c_str())) {
                            value.expanded = !value.expanded;
                        }
                        if (value.expanded) {
                            auto& mem_edit = std::get<MemoryRange>(value.value).editor;
                            uint32_t data = std::get<MemoryRange>(value.value).start;
                            uint32_t size = std::get<MemoryRange>(value.value).end - std::get<MemoryRange>(value.value).start;
                            if (ImGui::BeginChild("MemoryEditorInline", ImVec2(0.0f, 300.0f), true)) {
                                mem_edit->DrawContents((ImU8*)CemuHooks::s_memoryBaseAddress + (size_t)data, size, 0x0);
                            }
                            ImGui::EndChild();
                        }
                    }
                    else if constexpr (std::is_same_v<T, std::string>) {
                        std::string val = std::get<std::string>(value.value);
                        ImGui::Text( val.c_str());
                    }
                }, value.value);

                ImGui::EndDisabled();

                ImGui::PopID();
            }

            ImGui::PopID();
        }
    }
}

void EntityDebugger::AddOrUpdateEntity(uint32_t actorId, const std::string& entityName, const std::string& valueName, uint32_t address, ValueVariant&& value, bool isEntity) {
    const auto& [entityIt, _] = m_entities.try_emplace(actorId, Entity{ entityName, isEntity, 0.0f, {}, {}, {}, {} });

    const auto& valueIt = std::ranges::find_if(entityIt->second.values, [&](EntityValue& val) {
        return val.value_name == valueName;
    });

    if (valueIt == entityIt->second.values.end()) {
        entityIt->second.values.emplace_back(valueName, false, false, address, std::move(value));
    }
    else if (!valueIt->frozen && !std::holds_alternative<MemoryRange>(value)) {
        valueIt->value = std::move(value);
    }
}

void EntityDebugger::SetPosition(uint32_t actorId, const BEVec3& ws_playerPos, const BEVec3& ws_entityPos) {
    if (const auto it = m_entities.find(actorId); it != m_entities.end()) {
        it->second.priority = ws_playerPos.DistanceSq(ws_entityPos);
        it->second.position = ws_entityPos;
    }
}

void EntityDebugger::SetRotation(uint32_t actorId, const glm::fquat rotation) {
    if (const auto it = m_entities.find(actorId); it != m_entities.end()) {
        it->second.rotation = rotation;
    }
}

void EntityDebugger::SetAABB(uint32_t actorId, glm::fvec3 min, glm::fvec3 max) {
    if (const auto it = m_entities.find(actorId); it != m_entities.end()) {
        it->second.aabbMin = min;
        it->second.aabbMax = max;
    }
}

void EntityDebugger::RemoveEntity(uint32_t actorId) {
    m_entities.erase(actorId);
}

void EntityDebugger::RemoveEntityValue(uint32_t actorId, const std::string& valueName) {
    if (const auto it = m_entities.find(actorId); it != m_entities.end()) {
        it->second.values.erase(std::ranges::remove_if(it->second.values, [&](const EntityValue& val) {
            return val.value_name == valueName;
        }).begin(), it->second.values.end());
    }
}
std::array<bool, 256> s_pressedKeyState = {};
std::array<bool, ImGuiKey_NamedKey_COUNT> s_pressedNamedKeyState = {};

#define IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN(key, isShiftPressed) { \
    bool isKeyDown = GetAsyncKeyState(key) & 0x8000; \
    bool wasKeyDown = s_pressedKeyState[key]; \
    s_pressedKeyState[key] = isKeyDown; \
    if (isKeyDown && !wasKeyDown) { \
        ImGui::GetIO().AddInputCharacter(isShiftPressed ? key : tolower(key)); \
    } \
}

#define IS_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN(key, imGuiKey) { \
    bool wasKeyDown = s_pressedNamedKeyState[imGuiKey - ImGuiKey_NamedKey_BEGIN]; \
    bool isKeyDown = GetAsyncKeyState(key) & 0x8000; \
    s_pressedNamedKeyState[imGuiKey - ImGuiKey_NamedKey_BEGIN] = isKeyDown; \
    if (isKeyDown && !wasKeyDown) { \
        ImGui::GetIO().AddKeyEvent(imGuiKey, true); \
    } \
    else if (!isKeyDown && wasKeyDown) { \
        ImGui::GetIO().AddKeyEvent(imGuiKey, false); \
    } \
}

void EntityDebugger::UpdateKeyboardControls() {
#if !BETTERVR_HAS_WIN32
    // Linux: ImGui receives keyboard events through its own platform backend
    // (X11/Wayland) when Cemu's desktop window has focus. The debug-tools
    // poll path below uses Win32's GetAsyncKeyState directly, which isn't
    // worth porting for a debug-only feature.
    return;
#else
    // capture keyboard input
    ImGui::GetIO().KeyAlt = GetAsyncKeyState(VK_MENU) & 0x8000;
    ImGui::GetIO().KeyCtrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
    ImGui::GetIO().KeyShift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    ImGui::GetIO().KeySuper = GetAsyncKeyState(VK_LWIN) & 0x8000;

    // get asciinum from key state
    bool isShift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN(' ', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('0', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('1', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('2', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('3', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('4', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('5', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('6', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('7', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('8', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('9', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('A', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('B', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('C', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('D', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('E', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('F', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('G', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('H', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('I', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('J', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('K', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('L', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('M', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('N', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('O', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('P', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('R', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('Q', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('S', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('T', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('U', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('V', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('W', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('X', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('Y', isShift);
    IS_ALPHANUMERIC_KEY_DOWN_AND_WASNT_PREVIOUSLY_DOWN('Z', isShift);

    // check underscore
    bool isKeyDown = GetAsyncKeyState(VK_OEM_MINUS) & 0x8000;
    bool wasKeyDown = s_pressedKeyState[VK_OEM_MINUS];
    s_pressedKeyState[VK_OEM_MINUS] = isKeyDown;
    if (isKeyDown && !wasKeyDown) {
        ImGui::GetIO().AddInputCharacter(isShift ? '_' : '-');
    }

    // check backspace
    bool isBackspaceKeyDown = GetAsyncKeyState(VK_BACK) & 0x8000;
    bool wasBackspaceKeyDown = s_pressedKeyState[VK_BACK];
    s_pressedKeyState[VK_BACK] = isBackspaceKeyDown;
    if (isBackspaceKeyDown && !wasBackspaceKeyDown) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
    }
    else if (!isBackspaceKeyDown && wasBackspaceKeyDown) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
    }
#endif // BETTERVR_HAS_WIN32
}
