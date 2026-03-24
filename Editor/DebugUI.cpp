#include "Editor/DebugUI.h"

#include <Windows.h>
#include <imgui.h>

#include "Core/ModelSpawnOptions.h"
#include "Core/Scene.h"
#include "Engine/Core/AsyncModelLoader.h"
#include "Engine/ECS/Systems/RenderSystem.h"
#include "Graphics/HiZSystem.h"
#include "Graphics/TerrainGpuCullSystem.h"
#include "Graphics/PostProcessSettings.h"
#include "NprTuning.h"
#include "Camera.h"

#include <cstdio>
#include <string>
#include <algorithm>
#include <DirectXMath.h>

extern Camera* g_Camera;

void DebugUI::Draw()
{
	ImGui::Begin("Debug / Async load");
	const RenderSystem::GpuDrawStats gpu = RenderSystem::GetLastGpuDrawStats();

	if (ImGui::BeginTabBar("##debug_tabs"))
	{
		if (ImGui::BeginTabItem("Overview"))
		{
			ImGui::TextUnformatted("--- GPU (prev frame DrawMain) ---");
			ImGui::Text("DrawIndexed total: %u (terrain %u + PBR batches %u)", gpu.drawIndexedTotal, gpu.terrainDraws, gpu.pbrBatchDrawCalls);
			ImGui::Text("PBR instances drawn: %u", gpu.pbrInstancesDrawn);
			ImGui::Text("PBR entities frustum-culled (prev): %u", gpu.pbrFrustumCulledEntities);
			ImGui::Text("Player-model submesh draws: %u", gpu.playerModelSubmeshDraws);
			ImGui::Separator();
			ImGui::TextUnformatted("--- Camera ---");
			if (g_Camera)
			{
				using namespace DirectX;
				XMFLOAT3 camPos{};
				XMStoreFloat3(&camPos, g_Camera->GetPosition());
				const XMMATRIX invView = XMMatrixInverse(nullptr, g_Camera->GetViewMatrix());
				XMFLOAT4 fwd4{};
				XMStoreFloat4(&fwd4, XMVector3Normalize(invView.r[2]));
				const float yawDeg = XMConvertToDegrees(atan2f(fwd4.x, fwd4.z));
				const float pitchDeg = XMConvertToDegrees(asinf((std::max)(-1.0f, (std::min)(1.0f, fwd4.y))));
				ImGui::Text("Position: (%.2f, %.2f, %.2f)", camPos.x, camPos.y, camPos.z);
				ImGui::Text("Forward : (%.3f, %.3f, %.3f)", fwd4.x, fwd4.y, fwd4.z);
				ImGui::Text("Yaw/Pitch: %.2f / %.2f deg", yawDeg, pitchDeg);
				if (g_Scene)
				{
					TerrainGpuCullSystem* terrainCull = g_Scene->GetTerrainGpuCullSystem();
					if (terrainCull && terrainCull->IsValid())
					{
						const uint32_t lod0Count = terrainCull->GetDebugLastLod0VisibleCount();
						const uint32_t lod1Count = terrainCull->GetDebugLastLod1VisibleCount();
						const uint32_t gpuVisible = terrainCull->GetDebugLastGpuVisibleCount();
						const uint32_t lod0Indices = terrainCull->GetDebugLastLod0IndexCount();
						const uint32_t lod1Indices = terrainCull->GetDebugLastLod1IndexCount();
						ImGui::Text("Terrain LOD visible: LOD0=%u  LOD1=%u", lod0Count, lod1Count);
						ImGui::Text("Terrain indices (est): LOD0=%u  LOD1=%u  total=%u", lod0Indices, lod1Indices, lod0Indices + lod1Indices);
						ImGui::Text("Terrain visible after Hi-Z: %u", gpuVisible);
					}
				}
			}
			else
			{
				ImGui::TextDisabled("Camera is not initialized.");
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Rendering"))
		{
			bool frustumCull = RenderSystem::GetFrustumCullPbrEnabled();
			if (ImGui::Checkbox("PBR frustum cull (CPU / Hi-Z precursor)", &frustumCull))
				RenderSystem::SetFrustumCullPbrEnabled(frustumCull);
			ImGui::TextDisabled("PlayerComponent is on parent; mesh draws are children. 0 => no DrawIndexed for that model.");
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Terrain / Hi-Z"))
		{
			if (g_Scene)
			{
				HiZSystem* hz = g_Scene->GetHiZSystem();
				if (hz && hz->IsValid())
				{
					bool hzOn = hz->GetEnabled();
					if (ImGui::Checkbox("Hi-Z pyramid (GPU, max-depth mips)", &hzOn))
						hz->SetEnabled(hzOn);
					ImGui::Text("Hi-Z mips: %u", hz->GetMipCount());
				}
				TerrainGpuCullSystem* terrainCull = g_Scene->GetTerrainGpuCullSystem();
				if (terrainCull && terrainCull->IsValid())
				{
					static const char* terrainPsModes[] = {
						"0: Ground texture only",
						"1: + simple lighting",
						"2: + tree mask blend",
						"3: Full (river/snow included)"
					};
					int terrainPsMode = g_Scene->GetTerrainPsDebugMode();
					if (ImGui::Combo("Terrain PS debug stage", &terrainPsMode, terrainPsModes, IM_ARRAYSIZE(terrainPsModes)))
						g_Scene->SetTerrainPsDebugMode(terrainPsMode);
					ImGui::Separator();
					ImGui::TextUnformatted("--- Terrain PS cheap path (shallow view / fill) ---");
					bool cheapOn = g_Scene->GetTerrainCheapPathEnabled();
					if (ImGui::Checkbox("Enable cheap path (stage 3)", &cheapOn))
						g_Scene->SetTerrainCheapPathEnabled(cheapOn);
					float gThresh = g_Scene->GetTerrainCheapGrazingThresh();
					float nearPr = g_Scene->GetTerrainCheapNearPreserveMeters();
					if (ImGui::DragFloat("Cheap: grazing threshold", &gThresh, 0.01f, 0.05f, 0.95f, "%.2f (higher=stricter)"))
						g_Scene->SetTerrainCheapGrazingThresh(gThresh);
					if (ImGui::DragFloat("Cheap: near preserve (m)", &nearPr, 1.0f, 0.0f, 400.0f, "%.0f (0=off)"))
						g_Scene->SetTerrainCheapNearPreserveMeters(nearPr);
					ImGui::TextDisabled("地平に寄せたカメラでマスク/ DISP 参照を省略。手前だけは閾値+0.14でフル品質寄り。");
					ImGui::Separator();
					ImGui::TextUnformatted("--- Terrain Hi-Z occlusion tuning ---");
					const bool globalHiZEnabled = (hz && hz->IsValid() && hz->GetEnabled());
					if (!globalHiZEnabled)
						ImGui::TextDisabled("Hi-Z pyramid is OFF. Terrain Hi-Z tuning has no effect.");
					bool terrainHiZOn = terrainCull->GetHiZOcclusionEnabled();
					if (ImGui::Checkbox("Terrain Hi-Z occlusion", &terrainHiZOn))
						terrainCull->SetHiZOcclusionEnabled(terrainHiZOn);

					float nearDisable = 0.0f, depthBias = 0.0f, maxPixelRadius = 0.0f;
					terrainCull->GetHiZOcclusionTuning(nearDisable, depthBias, maxPixelRadius);
					bool changed = false;
					changed |= ImGui::DragFloat("Terrain Hi-Z near disable dist", &nearDisable, 1.0f, 0.0f, 3000.0f, "%.1f m");
					changed |= ImGui::DragFloat("Terrain Hi-Z depth bias", &depthBias, 0.001f, 0.0f, 0.2f, "%.4f");
					changed |= ImGui::DragFloat("Terrain Hi-Z max pixel radius", &maxPixelRadius, 1.0f, 1.0f, 512.0f, "%.1f px");
					if (changed)
						terrainCull->SetHiZOcclusionTuning(nearDisable, depthBias, maxPixelRadius);

					ImGui::Separator();
					ImGui::TextUnformatted("--- Terrain LOD tuning ---");
					float lod0Start = 0.0f, lod1Start = 0.0f;
					terrainCull->GetLodDistanceTuning(lod0Start, lod1Start);
					bool lodChanged = false;
					lodChanged |= ImGui::DragFloat("Terrain LOD0 start dist", &lod0Start, 1.0f, 0.0f, 3000.0f, "%.1f m");
					lodChanged |= ImGui::DragFloat("Terrain LOD1 start dist", &lod1Start, 1.0f, 1.0f, 6000.0f, "%.1f m");
					if (lodChanged)
						terrainCull->SetLodDistanceTuning(lod0Start, lod1Start);

					bool forceLod1 = terrainCull->GetForceLod1();
					if (ImGui::Checkbox("Force Terrain LOD1 (perf debug)", &forceLod1))
						terrainCull->SetForceLod1(forceLod1);
					bool cpuDebugEst = terrainCull->GetEnableCpuDebugEstimation();
					if (ImGui::Checkbox("Enable terrain CPU debug estimation", &cpuDebugEst))
						terrainCull->SetEnableCpuDebugEstimation(cpuDebugEst);
					ImGui::TextDisabled("Off = skip per-frame CPU estimated LOD/index counting.");
				}
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("NPR"))
		{
			ImGui::DragFloat("Normal scale", &g_NprGpuTuning.normalScale, 0.05f, 0.0f, 4.0f);
			if (ImGui::IsItemDeactivatedAfterEdit() && g_Scene)
				g_Scene->SyncNprGpuTuningToMaterialCB();
			ImGui::DragFloat("Rim power", &g_NprGpuTuning.rimPower, 0.1f, 0.5f, 16.0f);
			ImGui::DragFloat("Rim strength", &g_NprGpuTuning.rimStrength, 0.02f, 0.0f, 2.0f);
			ImGui::DragFloat("Transparent virtual light", &g_NprGpuTuning.virtualLight, 0.02f, 0.0f, 2.0f);
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
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("PostProcess"))
		{
			if (g_Scene)
			{
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
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Async Load"))
		{
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
			{
				ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "AsyncModelLoader not initialized");
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("ImGui"))
		{
			static bool showDemo = false;
			ImGui::Checkbox("ImGui Demo", &showDemo);
			if (showDemo)
				ImGui::ShowDemoWindow(&showDemo);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
}
