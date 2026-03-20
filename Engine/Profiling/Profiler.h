#pragma once

#include <cstdint>
#include <vector>
#include <chrono>
#include <array>

namespace RenderSystem
{
	// Forward-declare to avoid including RenderSystem.h in this header.
	struct GpuDrawStats;
	GpuDrawStats GetLastGpuDrawStats();
}

struct RenderStats
{
	uint32_t totalDrawCalls = 0;      // RenderSystem::GpuDrawStats::drawIndexedTotal (+ misc future)
	uint32_t pbrInstancesDrawn = 0;   // RenderSystem::GpuDrawStats::pbrInstancesDrawn
};

struct ProfilerFrameStats
{
	float frameTimeMs = 0.0f;
	float renderCpuTimeMs = 0.0f; // BeginRender〜EndRender
	float fps = 0.0f; // derived from frameTimeMs (text only)
	RenderStats render{};
};

class Profiler
{
public:
	static constexpr size_t kHistorySize = 120;

	static void BeginFrame();
	static void EndFrame(); // Pull from RenderSystem here (A案)
	static void BeginRenderCpuSection();
	static void EndRenderCpuSection();

	static const ProfilerFrameStats& GetLatest();

	// UI用: Plotにそのまま渡せる連続配列（古い→新しい）
	static void CopyHistory(
		std::vector<float>& outFrameTimeMs,
		std::vector<float>& outTotalDrawCalls,
		std::vector<float>& outPbrInstancesDrawn);

	// 将来用: RenderSystem外の描画も合算したい場合の口（今は使わない）
	static void AddMiscDrawCall(uint32_t count = 1);

private:
	static std::chrono::high_resolution_clock::time_point s_frameStartTime;
	static std::chrono::high_resolution_clock::time_point s_renderStartTime;
	static std::array<ProfilerFrameStats, kHistorySize> s_history;
	static size_t s_head;
	static size_t s_count;

	static uint32_t s_currentMiscDrawCalls;
	static float s_currentRenderCpuTimeMs;
};

