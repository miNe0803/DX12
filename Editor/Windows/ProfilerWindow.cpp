#include "ProfilerWindow.h"

#include "Engine/Profiling/Profiler.h"

#include <imgui.h>

#include <vector>
#include <string>

void ProfilerWindow::Draw(bool* pOpen)
{
	if (!pOpen || !*pOpen)
		return;

	if (ImGui::Begin("Performance Profiler", pOpen))
	{
		const ProfilerFrameStats& latest = Profiler::GetLatest();

		// --- 1) 総合テキスト（全情報網羅） ---
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[ Engine Status ]");
		ImGui::Text("FPS: %.1f (%.2f ms)", latest.fps, latest.frameTimeMs);
		ImGui::Text("Total Draw Calls: %u", latest.render.totalDrawCalls);
		ImGui::Text("PBR Instances: %u", latest.render.pbrInstancesDrawn);
		ImGui::Separator();

		// --- 2) 履歴（連続配列） ---
		static std::vector<float> timeHist;
		static std::vector<float> totalDrawsHist;
		static std::vector<float> instHist;
		Profiler::CopyHistory(timeHist, totalDrawsHist, instHist);

		if (!timeHist.empty())
		{
			const ImVec2 graphSize(ImGui::GetContentRegionAvail().x, 60.0f);

			// --- Graph 1: Frame Time (ms) ---
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "[ CPU Frame Time (ms) ]");
			ImGui::PlotLines("##FrameTime", timeHist.data(), static_cast<int>(timeHist.size()), 0, nullptr, 0.0f, 33.3f, graphSize);
			ImGui::Spacing();

			// --- Graph 2: Total Draw Calls ---
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "[ Total Draw Calls ]");
			// スケール上限は固定（描画結果が見やすい）。必要なら後で max をサンプルして自動化。
			ImGui::PlotLines("##TotalDraws", totalDrawsHist.data(), static_cast<int>(totalDrawsHist.size()), 0, nullptr, 0.0f, 150.0f, graphSize);
			ImGui::Spacing();

			// --- Graph 3: PBR Instances ---
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[ PBR Instances Drawn ]");
			ImGui::PlotHistogram("##PbrInstances", instHist.data(), static_cast<int>(instHist.size()), 0, nullptr, 0.0f, 50000.0f, graphSize);
		}
	}
	ImGui::End();
}

