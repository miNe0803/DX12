#include "Editor/DebugUI.h"

#include <Windows.h>
#include <imgui.h>

#include "Core/ModelSpawnOptions.h"
#include "Core/Scene.h"
#include "Engine/Core/AsyncModelLoader.h"
#include "Engine/ECS/Systems/RenderSystem.h"
#include "Graphics/HiZSystem.h"
#include "Graphics/PostProcessSettings.h"
#include "NprTuning.h"

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
	ImGui::Separator();
	ImGui::TextUnformatted("--- NPR (PBR hybrid) ---");
	ImGui::DragFloat("Opaque exposure", &g_NprGpuTuning.opaqueExposure, 0.02f, 0.1f, 2.0f);
	ImGui::DragFloat("NPR HDR view boost", &g_NprGpuTuning.nprHdrViewBoost, 0.05f, 0.25f, 16.0f);
	ImGui::TextDisabled("RenderDoc: NPR on R16F HDR looks black - try 2..8 (pre-tonemap). Reset to 1.0 for play.");
	ImGui::DragFloat("NPR linear HDR clamp (per ch)", &g_NprGpuTuning.nprHdrLinearClampMax, 0.1f, 0.0f, 32.0f);
	ImGui::TextDisabled("0=no clamp. Else caps NPR opaque before Bloom/ACES (reduces white blowout).");
	if (ImGui::IsItemDeactivatedAfterEdit() && g_Scene)
		g_Scene->SyncNprGpuTuningToMaterialCB();
	ImGui::DragFloat("Normal scale", &g_NprGpuTuning.normalScale, 0.05f, 0.0f, 4.0f);
	ImGui::DragFloat("Rim power", &g_NprGpuTuning.rimPower, 0.1f, 0.5f, 16.0f);
	ImGui::DragFloat("Rim strength", &g_NprGpuTuning.rimStrength, 0.02f, 0.0f, 2.0f);
	ImGui::DragFloat("Transparent virtual light", &g_NprGpuTuning.virtualLight, 0.02f, 0.0f, 2.0f);
	ImGui::DragFloat("Transparent exposure", &g_NprGpuTuning.transExposure, 0.02f, 0.1f, 2.0f);
	ImGui::DragFloat("Opaque alpha clip", &g_NprGpuTuning.opaqueAlphaClip, 0.01f, 0.0f, 0.99f);
	ImGui::DragFloat("Ambient shadow strength", &g_NprGpuTuning.ambientShadowStrength, 0.02f, 0.0f, 3.0f);
	ImGui::Separator();
	ImGui::TextUnformatted("Cel / face (vertex normal blend)");
	ImGui::DragFloat("Cel: vertex normal blend", &g_NprGpuTuning.celVertexNormalBlend, 0.02f, 0.0f, 1.0f);
	ImGui::TextDisabled("1=shadow from vertex normals only (clean face). 0=full normal map.");
	ImGui::DragFloat("Cel: ramp sharpness", &g_NprGpuTuning.celShadeSharpness, 0.02f, 0.0f, 1.0f);
	ImGui::DragFloat("Rim: vertex blend", &g_NprGpuTuning.rimVertexNormalBlend, 0.02f, 0.0f, 1.0f);
	ImGui::Separator();
	if (g_Scene)
	{
		size_t nprTags = 0;
		bool psoOk = false, willNpr = false;
		g_Scene->GetNprPathDiagnostics(nprTags, psoOk, willNpr);
		ImGui::Text("NPR path: NPRTag entities=%zu  NPR_PSO_OK=%s  -> draw NPR passes=%s",
			nprTags, psoOk ? "yes" : "NO", willNpr ? "YES" : "NO");
		if (!willNpr)
			ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1),
				"Toon/ramp debug only works when YES. NO => PBR only (often looks blown-out white).");
	}
	ImGui::TextUnformatted("NPR ramp (toon3 / t4) debug");
	const char* dbgItems[] = {
		"0: Off (normal shading)",
		"1: Show _RampTex RGB",
		"2: Show ramp coord u (5 grays)",
		"3: Show halfLambert (smooth gray)",
		"4: SOLID GREEN = NPR_PS runs (if still white, not NPR)",
		"5: Albedo t0 raw (RenderDoc / atlas check)",
		"6: t0 x vertex color (before linearize)",
		"7: Linear albedo x vertex (before sphere)"
	};
	ImGui::Combo("##npr_ramp_dbg", &g_NprGpuTuning.nprDebugRampView, dbgItems, IM_ARRAYSIZE(dbgItems));
	ImGui::TextDisabled("5–7: output to HDR like game; use RenderDoc on NPR draw to see texture on mesh.");
	ImGui::TextDisabled("If 4 is not bright green on body, NPR_PS is not drawing (PBR or clip).");
	if (ImGui::Checkbox("Log RampMap paths on register (Output window)", &g_NprGpuTuning.logNprRampPathsOnRegister))
	{ /* toggled */ }
	ImGui::TextDisabled("VS: View -> Output (Debug). Reload model after enabling to see paths.");

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
	if (g_Scene)
	{
		ImGui::Separator();
		ImGui::TextUnformatted("--- Post process (Bloom + ACES tonemap) ---");
		PostProcessSettings& pp = g_Scene->GetPostProcessSettings();
		ImGui::DragFloat("Bloom luma threshold", &pp.threshold, 0.02f, 0.1f, 4.0f);
		ImGui::DragFloat("Bloom soft knee", &pp.bloomKnee, 0.02f, 0.0f, 2.0f);
		ImGui::TextDisabled("Knee: larger = less bloom near threshold (skin). 0 = linear excess only.");
		ImGui::DragFloat("Bloom intensity", &pp.bloomIntensity, 0.02f, 0.0f, 2.0f);
		ImGui::DragFloat("Tonemap exposure", &pp.exposure, 0.02f, 0.1f, 4.0f);
		ImGui::DragFloat("Display gamma", &pp.gamma, 0.02f, 1.0f, 3.0f);
		ImGui::Separator();
		ImGui::TextUnformatted("NPR layer post (split path only)");
		ImGui::DragFloat("NPR post exposure", &pp.nprPostExposure, 0.02f, 0.1f, 4.0f);
		ImGui::DragFloat("NPR post gamma", &pp.nprPostGamma, 0.02f, 1.0f, 3.0f);
		ImGui::TextDisabled("Raise threshold / lower bloom if NPR skin glows white.");
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
	static bool addNprTagOnLoad = true;
	ImGui::Checkbox("NPR / toon path (NPRTag)", &addNprTagOnLoad);
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
			opt.addNprTag = addNprTagOnLoad;

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
