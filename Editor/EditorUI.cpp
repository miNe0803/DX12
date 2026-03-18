#include "Editor/EditorUI.h"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "Core/ModelSpawnOptions.h"
#include "Engine/Core/AsyncModelLoader.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/TerrainSystem.h"

namespace
{
	constexpr float kPi = 3.14159265f;

	std::string WToUtf8(const std::wstring& w)
	{
		if (w.empty())
			return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (n <= 1)
			return {};
		std::string out(static_cast<size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
		return out;
	}

	// TransformSystem と同じ合成（T * S * base * R）を行い、
	// TransformComponent::WorldMatrix が保持している「転置済み行列」前提の平行移動成分を返す。
	static DirectX::XMFLOAT3 GetGPUWorldOriginFromTransformParams(const TransformComponent& tc)
	{
		const DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&tc.Position);
		const DirectX::XMMATRIX T = DirectX::XMMatrixTranslationFromVector(pos);
		const DirectX::XMMATRIX S = DirectX::XMMatrixScaling(tc.UniformScale, tc.UniformScale, tc.UniformScale);
		const DirectX::XMMATRIX base = DirectX::XMLoadFloat4x4(&tc.BaseMatrix);
		const DirectX::XMMATRIX R = DirectX::XMMatrixRotationY(tc.RotationY);
		const DirectX::XMMATRIX world = T * S * base * R;
		const DirectX::XMMATRIX worldT = DirectX::XMMatrixTranspose(world); // CPUが保持している形

		DirectX::XMFLOAT4X4 f;
		DirectX::XMStoreFloat4x4(&f, worldT);
		return DirectX::XMFLOAT3(f._14, f._24, f._34);
	}

	static DirectX::XMFLOAT3 GetGPUWorldOriginFromGPUWorldMatrix(const DirectX::XMMATRIX& worldT)
	{
		DirectX::XMFLOAT4X4 f;
		DirectX::XMStoreFloat4x4(&f, worldT);
		return DirectX::XMFLOAT3(f._14, f._24, f._34);
	}
}

void EditorUI::Draw(entt::registry& registry)
{
	DrawMainMenu();
	DrawWindows(registry);
}

void EditorUI::DrawMainMenu()
{
	if (!ImGui::BeginMainMenuBar())
		return;

	if (ImGui::BeginMenu("Character"))
	{
		ImGui::MenuItem("Model Hierarchy && Inspector", nullptr, &m_State.showCharacterHierarchy);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Player"))
	{
		ImGui::MenuItem("Movement && Physics", nullptr, &m_State.showPlayerMovement);
		ImGui::MenuItem("Action && Animation Blend (WIP)", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Enemy && Boss AI"))
	{
		ImGui::MenuItem("Boss Status && Phases", nullptr, &m_State.showBossStatus);
		ImGui::MenuItem("Timeline Action Editor", nullptr, &m_State.showTimelineEditor);
		ImGui::MenuItem("AOE Marker Setup", nullptr, &m_State.showAOESetup);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Foliage && Nature"))
	{
		ImGui::MenuItem("Sakura Forest Spawner", nullptr, &m_State.showForestSpawner);
		ImGui::MenuItem("LOD Distance Tuner", nullptr, &m_State.showLODTuner);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("World && Terrain"))
	{
		ImGui::MenuItem("Terrain Parameters", nullptr, &m_State.showTerrainParams);
		ImGui::MenuItem("Atmosphere && Skybox", nullptr, &m_State.showAtmosphere);
		ImGui::MenuItem("Splat Map Editor (WIP)", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Structures"))
	{
		ImGui::MenuItem("Prop Placer (WIP)", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("System && Debug"))
	{
		ImGui::MenuItem("ECS Hierarchy && Inspector", nullptr, &m_State.showHierarchy);
		ImGui::MenuItem("Performance Profiler", nullptr, &m_State.showProfiler);
		ImGui::MenuItem("Async Model Load", nullptr, &m_State.showAsyncModelLoad);
		ImGui::MenuItem("ImGui Demo", nullptr, &m_State.showImGuiDemo);
		ImGui::EndMenu();
	}

	ImGui::EndMainMenuBar();
}

void EditorUI::DrawWindows(entt::registry& registry)
{
	if (m_State.showImGuiDemo)
		ImGui::ShowDemoWindow(&m_State.showImGuiDemo);

	if (m_State.showCharacterHierarchy)
		DrawCharacterHierarchyAndInspector(registry);

	if (m_State.showPlayerMovement)
		DrawPlayerMovementPanel(registry);

	if (m_State.showBossStatus)
	{
		if (ImGui::Begin("Boss Status && Phases", &m_State.showBossStatus))
			ImGui::TextUnformatted("HP thresholds and phase transitions will go here.");
		ImGui::End();
	}

	if (m_State.showTimelineEditor)
	{
		if (ImGui::Begin("Timeline Action Editor", &m_State.showTimelineEditor))
			ImGui::TextUnformatted("Timeline UI for boss actions will go here.");
		ImGui::End();
	}

	if (m_State.showAOESetup)
	{
		if (ImGui::Begin("AOE Marker Setup", &m_State.showAOESetup))
			ImGui::TextUnformatted("Radius and angle setup for AOE telegraphs will go here.");
		ImGui::End();
	}

	if (m_State.showForestSpawner)
	{
		if (ImGui::Begin("Sakura Forest Spawner", &m_State.showForestSpawner))
			ImGui::TextUnformatted("Density and noise parameters for instance spawning will go here.");
		ImGui::End();
	}

	if (m_State.showLODTuner)
	{
		if (ImGui::Begin("LOD Distance Tuner", &m_State.showLODTuner))
			ImGui::TextUnformatted("Sliders for LOD0, LOD1, LOD2 distances will go here.");
		ImGui::End();
	}

	if (m_State.showTerrainParams)
	{
		if (ImGui::Begin("Terrain Parameters", &m_State.showTerrainParams))
			ImGui::TextUnformatted("CellSpacing and MaxHeight sliders will go here.");
		ImGui::End();
	}

	if (m_State.showAtmosphere)
	{
		if (ImGui::Begin("Atmosphere && Skybox", &m_State.showAtmosphere))
			ImGui::TextUnformatted("Fog distance and IBL intensity controls will go here.");
		ImGui::End();
	}

	if (m_State.showHierarchy)
	{
		if (ImGui::Begin("ECS Hierarchy && Inspector", &m_State.showHierarchy))
			ImGui::TextUnformatted("Full ECS entity list will go here (use Character window for mesh models).");
		ImGui::End();
	}

	if (m_State.showProfiler)
	{
		if (ImGui::Begin("Performance Profiler", &m_State.showProfiler))
			ImGui::TextUnformatted("FPS, draw calls, and VRAM usage will go here.");
		ImGui::End();
	}

	if (m_State.showAsyncModelLoad)
		DrawAsyncModelLoadPanel(registry);
}

void EditorUI::DrawPlayerMovementPanel(entt::registry& registry)
{
	if (!ImGui::Begin("Movement && Physics", &m_State.showPlayerMovement))
	{
		ImGui::End();
		return;
	}

	std::vector<entt::entity> players;
	for (auto e : registry.view<PlayerComponent, TransformComponent>())
		players.push_back(e);

	if (players.empty())
	{
		ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1),
			"No PlayerComponent in scene.");
		ImGui::TextWrapped(
			"Check \"Add Player component on load\" in Async Model Load, then Queue again, "
			"or spawn a player from code.");
		ImGui::End();
		return;
	}

	static int currentIdx = 0;
	currentIdx = std::min(currentIdx, static_cast<int>(players.size()) - 1);
	if (players.size() > 1)
	{
		ImGui::Text("Player entity (%d total)", static_cast<int>(players.size()));
		for (int i = 0; i < static_cast<int>(players.size()); ++i)
		{
			char buf[48];
			snprintf(buf, sizeof(buf), "Entity %u##pl%d",
				static_cast<unsigned>(static_cast<std::underlying_type_t<entt::entity>>(players[i])), i);
			if (ImGui::Selectable(buf, currentIdx == i))
				currentIdx = i;
		}
		ImGui::Separator();
	}

	const entt::entity ent = players[static_cast<size_t>(currentIdx)];
	auto& pl = registry.get<PlayerComponent>(ent);
	auto& tc = registry.get<TransformComponent>(ent);

	ImGui::PushID(static_cast<int>(static_cast<uint32_t>(
		static_cast<std::underlying_type_t<entt::entity>>(ent))));

	ImGui::Checkbox("Follow camera (TPS)", &pl.FollowCamera);
	ImGui::DragFloat("Walk speed", &pl.WalkSpeed, 0.05f, 0.1f, 30.f);
	ImGui::TextDisabled("(walk speed not used by input yet)");
	ImGui::DragFloat("Height (for camera look-at)", &pl.Height, 0.02f, 0.5f, 5.f);
	ImGui::DragFloat("Ground offset (feet vs terrain)", &pl.GroundOffset, 0.05f, -5.f, 15.f);
	ImGui::DragFloat3("Camera offset", &pl.CameraOffset.x, 0.05f, -30.f, 30.f);

	ImGui::Separator();
	ImGui::TextUnformatted("Transform (this entity)");
	ImGui::DragFloat3("Position", &tc.Position.x, 0.1f);
	ImGui::DragFloat("Uniform scale", &tc.UniformScale, 0.02f, 0.01f, 10.f);

	ImGui::PopID();
	ImGui::End();
}

entt::entity EditorUI::ResolveModelTransformEntity(entt::registry& registry, entt::entity selected)
{
	if (!registry.valid(selected))
		return selected;
	if (const auto* ch = registry.try_get<ModelGroupChildComponent>(selected))
	{
		if (registry.valid(ch->parent))
			return ch->parent;
	}
	return selected;
}

void EditorUI::DrawCharacterHierarchyAndInspector(entt::registry& registry)
{
	if (m_selectedSceneModel && !registry.valid(*m_selectedSceneModel))
		m_selectedSceneModel.reset();

	if (ImGui::Begin("Character Models", &m_State.showCharacterHierarchy))
	{
		ImGui::TextUnformatted("Parent = whole model (move/rotate/scale). Expand for mesh parts.");
		ImGui::Separator();

		const float w = ImGui::GetContentRegionAvail().x;
		const float leftW = std::max(220.0f, w * 0.38f);
		ImGui::BeginChild("HierarchyPane", ImVec2(leftW, 0.0f), ImGuiChildFlags_Borders);

		bool anyRoot = false;
		std::vector<entt::entity> roots;
		for (const auto root : registry.view<ModelGroupRootComponent, EditorHierarchyLabelComponent>())
		{
			if (registry.all_of<TerrainComponent>(root))
				continue;
			roots.push_back(root);
		}
		std::sort(roots.begin(), roots.end(), [](entt::entity a, entt::entity b) {
			return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
		});

		for (const entt::entity root : roots)
		{
			anyRoot = true;
			const auto& rootComp = registry.get<ModelGroupRootComponent>(root);
			std::string rootName = WToUtf8(registry.get<EditorHierarchyLabelComponent>(root).displayName);
			if (registry.all_of<PlayerComponent>(root))
				rootName += " [Player]";

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
				| ImGuiTreeNodeFlags_DefaultOpen;
			const bool selRoot = m_selectedSceneModel && *m_selectedSceneModel == root;
			if (selRoot)
				flags |= ImGuiTreeNodeFlags_Selected;

			const bool open = ImGui::TreeNodeEx(
				reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<std::underlying_type_t<entt::entity>>(root))),
				flags, "%s", rootName.c_str());
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				m_selectedSceneModel = root;

			if (open)
			{
				for (const entt::entity child : rootComp.children)
				{
					if (!registry.valid(child))
						continue;
					std::string part = WToUtf8(registry.get<EditorHierarchyLabelComponent>(child).displayName);
					const bool selChild = m_selectedSceneModel && *m_selectedSceneModel == child;
					ImGui::Indent();
					if (ImGui::Selectable(part.c_str(), selChild))
						m_selectedSceneModel = child;
					ImGui::Unindent();
				}
				ImGui::TreePop();
			}
		}

		std::vector<entt::entity> legacy;
		for (const auto e : registry.view<TransformComponent, MeshRendererComponent, LODComponent>())
		{
			if (registry.all_of<TerrainComponent>(e))
				continue;
			if (registry.all_of<ModelGroupChildComponent>(e))
				continue;
			legacy.push_back(e);
		}
		std::sort(legacy.begin(), legacy.end(), [](entt::entity a, entt::entity b) {
			return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
		});
		for (const auto e : legacy)
		{
			anyRoot = true;
			std::string line;
			if (const auto* lab = registry.try_get<EditorHierarchyLabelComponent>(e))
				line = WToUtf8(lab->displayName);
			else
				line = "Entity " + std::to_string(static_cast<unsigned>(
					static_cast<std::underlying_type_t<entt::entity>>(e)));
			const bool sel = m_selectedSceneModel && *m_selectedSceneModel == e;
			if (ImGui::Selectable(line.c_str(), sel))
				m_selectedSceneModel = e;
		}

		if (!anyRoot)
			ImGui::TextDisabled("No models (use Async Model Load).");

		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::BeginChild("InspectorPane", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		if (m_selectedSceneModel && registry.valid(*m_selectedSceneModel))
		{
			const entt::entity selected = *m_selectedSceneModel;
			const entt::entity transformEnt = ResolveModelTransformEntity(registry, selected);
			if (!registry.all_of<TransformComponent>(transformEnt))
			{
				ImGui::TextUnformatted("No Transform on resolved entity.");
			}
			else
			{
				auto& tc = registry.get<TransformComponent>(transformEnt);

				const uint32_t idKey = static_cast<uint32_t>(
					static_cast<std::underlying_type_t<entt::entity>>(transformEnt));
				ImGui::PushID(static_cast<int>(idKey));

				if (registry.all_of<ModelGroupChildComponent>(selected))
					ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
						"Transform applies to parent (whole model).");
				if (registry.all_of<PlayerComponent>(transformEnt))
					ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f),
						"Player: Y is forced to terrain+GroundOffset each frame (use Movement panel).");

				ImGui::Text("Selected: %u", static_cast<unsigned>(
					static_cast<std::underlying_type_t<entt::entity>>(selected)));
				if (transformEnt != selected)
					ImGui::Text("Parent entity: %u", static_cast<unsigned>(
						static_cast<std::underlying_type_t<entt::entity>>(transformEnt)));
				if (const auto* lab = registry.try_get<EditorHierarchyLabelComponent>(selected))
					ImGui::Text("Label: %s", WToUtf8(lab->displayName).c_str());

				ImGui::Separator();
				ImGui::TextUnformatted("Model transform (parent)");

				if (!std::isfinite(tc.Position.x)) tc.Position.x = 0.f;
				if (!std::isfinite(tc.Position.y)) tc.Position.y = 0.f;
				if (!std::isfinite(tc.Position.z)) tc.Position.z = 0.f;
				if (!std::isfinite(tc.UniformScale) || tc.UniformScale <= 0.f) tc.UniformScale = 1.f;
				if (!std::isfinite(tc.RotationY)) tc.RotationY = 0.f;

				ImGui::PushItemWidth(std::max(280.0f, ImGui::GetContentRegionAvail().x - 4.0f));
				ImGui::DragFloat3("Position", &tc.Position.x, 0.1f, -1.0e5f, 1.0e5f, "%.2f");
				float degY = tc.RotationY * (180.0f / kPi);
				if (!std::isfinite(degY)) degY = 0.f;
				if (ImGui::DragFloat("Rotation Y (deg)", &degY, 0.5f, -3600.f, 3600.f, "%.2f"))
					tc.RotationY = degY * (kPi / 180.0f);
				ImGui::DragFloat("Uniform scale", &tc.UniformScale, 0.02f, 0.01f, 100.0f, "%.3f");
				ImGui::PopItemWidth();

				// 描画で使われる座標を確認するためのデバッグ表示
				const auto predOrigin = GetGPUWorldOriginFromTransformParams(tc);
				const auto curOrigin = GetGPUWorldOriginFromGPUWorldMatrix(tc.WorldMatrix);
				ImGui::Separator();
				ImGui::TextUnformatted("Runtime Transform (debug)");
				ImGui::Text("Position   = (%.2f, %.2f, %.2f)", tc.Position.x, tc.Position.y, tc.Position.z);
				ImGui::Text("Scale      = %.3f", tc.UniformScale);
				ImGui::Text("RotationY  = %.2f deg", tc.RotationY * (180.0f / kPi));
				ImGui::Text("WorldOrigin(pred) = (%.2f, %.2f, %.2f)", predOrigin.x, predOrigin.y, predOrigin.z);
				ImGui::Text("WorldOrigin(cur)  = (%.2f, %.2f, %.2f)", curOrigin.x, curOrigin.y, curOrigin.z);

				// 見た目上選んでいる entity と、編集している entity が違うケースがあるため追加表示
				if (selected != transformEnt && registry.all_of<TransformComponent>(selected))
				{
					const auto& stc = registry.get<TransformComponent>(selected);
					const auto selCurOrigin = GetGPUWorldOriginFromGPUWorldMatrix(stc.WorldMatrix);
					ImGui::Text("SelectedEnt %u WorldOrigin(cur) = (%.2f, %.2f, %.2f)",
						static_cast<unsigned>(static_cast<std::underlying_type_t<entt::entity>>(selected)),
						selCurOrigin.x, selCurOrigin.y, selCurOrigin.z);
				}

				ImGui::PopID();

				if (const auto* mr = registry.try_get<MeshRendererComponent>(selected))
					ImGui::Text("Part indices: %u", mr->IndexCount);
				else if (registry.all_of<ModelGroupRootComponent>(selected))
					ImGui::Text("Submeshes: %zu", registry.get<ModelGroupRootComponent>(selected).children.size());
			}
		}
		else
			ImGui::TextDisabled("Select a model or part in the hierarchy.");

		ImGui::EndChild();
	}
	ImGui::End();
}

void EditorUI::DrawAsyncModelLoadPanel(entt::registry& registry)
{
	if (!ImGui::Begin("Async Model Load", &m_State.showAsyncModelLoad))
	{
		ImGui::End();
		return;
	}

	static char pathUtf8[512] = "assets\\hibana\\hibana.pmx";
	ImGui::InputText("Model path (UTF-8)", pathUtf8, sizeof(pathUtf8));
	static float pos[3] = { 3.0f, 0.0f, 3.0f };
	ImGui::DragFloat3("Spawn position", pos, 0.1f);
	static float scale = 1.0f;
	ImGui::DragFloat("Uniform scale", &scale, 0.05f, 0.01f, 10.0f);
	static bool snapFeet = true;
	ImGui::Checkbox("Snap feet to terrain", &snapFeet);
	static bool offsetPlus15X = false;
	ImGui::Checkbox("Load with X +15m", &offsetPlus15X);
	static bool addPlayerOnLoad = false;
	ImGui::Checkbox("Add Player component on load (TPS / Movement panel)", &addPlayerOnLoad);

	ImGui::Separator();
	ImGui::TextWrapped(
		"Spawn pos/scale apply only when you click \"Queue async load\" (next spawn). "
		"To move an already-loaded model with the fields above, select it in Character Models then:");
	if (m_selectedSceneModel && registry.valid(*m_selectedSceneModel))
	{
		if (ImGui::Button("Apply spawn pos && scale to selected model"))
		{
			const entt::entity root = ResolveModelTransformEntity(registry, *m_selectedSceneModel);
			if (registry.all_of<TransformComponent>(root))
			{
				auto& tc = registry.get<TransformComponent>(root);
				const float x = pos[0] + (offsetPlus15X ? 15.0f : 0.0f);
				const float z = pos[2];
				float y = pos[1];
				if (snapFeet)
				{
					const float gy = TerrainSystem::GetHeight(registry, x, z);
					if (registry.all_of<PlayerComponent>(root))
						y = gy + registry.get<PlayerComponent>(root).GroundOffset;
					else
						y = gy + 2.0f;
				}
				tc.Position.x = x;
				tc.Position.y = y;
				tc.Position.z = z;
				tc.UniformScale = scale;
			}
		}
	}
	else
		ImGui::TextDisabled("Select hibana.pmx parent in Character Models to enable apply.");

	if (g_AsyncModelLoader)
	{
		if (ImGui::Button("Queue async load"))
		{
			ModelSpawnOptions opt{};
			opt.position.x = pos[0] + (offsetPlus15X ? 15.0f : 0.0f);
			opt.position.y = pos[1];
			opt.position.z = pos[2];
			opt.uniformScale = scale;
			opt.foot = snapFeet ? ModelSpawnOptions::FootPlacement::SnapFeetToTerrain
			                  : ModelSpawnOptions::FootPlacement::None;
			opt.addPlayerComponent = addPlayerOnLoad;

			const int n = MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, nullptr, 0);
			std::wstring wpath;
			if (n > 0)
			{
				wpath.resize(static_cast<size_t>(n));
				MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, wpath.data(), n);
				if (!wpath.empty() && wpath.back() == L'\0')
					wpath.pop_back();
			}
			if (!wpath.empty())
				g_AsyncModelLoader->RequestLoad(std::move(wpath), opt);
		}
		ImGui::Text("Async queue (pending): %zu", g_AsyncModelLoader->PendingLoadCount());
		const AsyncModelLoader::LastResultStatus st = g_AsyncModelLoader->GetLastResultStatus();
		ImGui::Separator();
		ImGui::Text("Assimp: %s", st.hasValue ? (st.success ? "OK" : "FAIL") : "n/a");
		if (st.hasValue)
		{
			ImGui::Text("Meshes (file): %zu", st.meshCount);
			ImGui::Text("Path: %ls", st.path.c_str());
		}
		const AsyncModelLoader::LastSpawnStatus sp = g_AsyncModelLoader->GetLastSpawnStatus();
		ImGui::Text("Spawn: %s", sp.hasValue ? (sp.ok ? "OK" : "FAIL") : "n/a");
		if (sp.hasValue)
		{
			ImGui::Text("Entities: %zu", sp.entityCount);
			if (sp.ok)
				ImGui::Text("Spawn world: (%.2f, %.2f, %.2f)", sp.worldX, sp.worldY, sp.worldZ);
			if (!sp.detail.empty())
				ImGui::TextWrapped("%ls", sp.detail.c_str());
		}
	}
	else
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "AsyncModelLoader not initialized");

	ImGui::End();
}
