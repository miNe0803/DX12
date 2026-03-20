#include "Editor/DebugUI.h"

#include <Windows.h>
#include <imgui.h>

#include "Core/ModelSpawnOptions.h"
#include "Core/Scene.h"
#include "Engine/Core/AsyncModelLoader.h"
#include "Engine/ECS/Systems/RenderSystem.h"
#include "Graphics/HiZSystem.h"

#include <cstdio>
#include <string>

void DebugUI::Draw()
{
	ImGui::Begin("Debug / Async load");
	const RenderSystem::GpuDrawStats gpu = RenderSystem::GetLastGpuDrawStats();
	ImGui::TextUnformatted("--- GPU (prev frame DrawMain) ---");
	bool frustumCull = RenderSystem::GetFrustumCullPbrEnabled();
	if (ImGui::Checkbox("PBR frustum cull (CPU / Hi-Z precursor)", &frustumCull))
		RenderSystem::SetFrustumCullPbrEnabled(frustumCull);
	ImGui::Text("PBR entities frustum-culled (prev): %u", gpu.pbrFrustumCulledEntities);
	ImGui::Text("DrawIndexed total: %u (terrain %u + PBR batches %u)", gpu.drawIndexedTotal, gpu.terrainDraws, gpu.pbrBatchDrawCalls);
	ImGui::Text("PBR instances drawn: %u", gpu.pbrInstancesDrawn);
	ImGui::Text("Player-model submesh draws: %u", gpu.playerModelSubmeshDraws);
	ImGui::TextDisabled("PlayerComponent is on parent; mesh draws are children. 0 => no DrawIndexed for that model.");
	if (g_Scene)
	{
		HiZSystem* hz = g_Scene->GetHiZSystem();
		if (hz && hz->IsValid())
		{
			bool hzOn = hz->GetEnabled();
			if (ImGui::Checkbox("Hi-Z pyramid (GPU, min-depth mips)", &hzOn))
				hz->SetEnabled(hzOn);
			ImGui::Text("Hi-Z mips: %u", hz->GetMipCount());
		}
	}
	ImGui::Separator();
	static bool showDemo = false;
	ImGui::Checkbox("ImGui Demo", &showDemo);
	if (showDemo)
		ImGui::ShowDemoWindow(&showDemo);

	static char pathUtf8[512] = "assets\\hibana\\hibana.pmx";
	ImGui::InputText("Model path (UTF-8)", pathUtf8, sizeof(pathUtf8));
	static float pos[3] = { 3.0f, 0.0f, 3.0f };
	ImGui::DragFloat3("Spawn position", pos, 0.1f);
	static float scale = 1.0f;
	ImGui::DragFloat("Uniform scale", &scale, 0.05f, 0.01f, 10.0f);
	static bool snapFeet = true;
	ImGui::Checkbox("Snap feet to terrain", &snapFeet);
	static bool offsetPlus15X = false;
	ImGui::Checkbox("Load with X +15m (avoid same spot as sakura tree)", &offsetPlus15X);
	static bool addPlayerOnLoad = false;
	ImGui::Checkbox("Add Player component (TPS / Movement panel)", &addPlayerOnLoad);
	ImGui::TextWrapped(
		"[Player][Draw] OK = DrawIndexed actually ran for an entity with PlayerComponent. "
		"[Player][Draw] SKIP = player mesh was not drawn (reason on line). "
		"Async PMX has no PlayerComponent unless you check the box above. Same coords as a big tree can still hide meshes (depth).");

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

			int n = MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, nullptr, 0);
			std::wstring wpath;
			if (n > 0)
			{
				// n は null 終端込みの文字数。まず n サイズで確保してから null を削る。
				wpath.resize(static_cast<size_t>(n));
				MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, wpath.data(), n);
				if (!wpath.empty() && wpath.back() == L'\0')
					wpath.pop_back();
			}
			if (!wpath.empty())
				g_AsyncModelLoader->RequestLoad(std::move(wpath), opt);
		}
		ImGui::Text("Async queue (pending): %zu", g_AsyncModelLoader->PendingLoadCount());
		AsyncModelLoader::LastResultStatus st = g_AsyncModelLoader->GetLastResultStatus();
		ImGui::Separator();
		ImGui::Text("Assimp: %s", st.hasValue ? (st.success ? "OK" : "FAIL") : "n/a");
		if (st.hasValue)
		{
			ImGui::Text("Meshes (file): %zu", st.meshCount);
			ImGui::Text("Path: %ls", st.path.c_str());
		}
		AsyncModelLoader::LastSpawnStatus sp = g_AsyncModelLoader->GetLastSpawnStatus();
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
