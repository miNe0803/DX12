#include "Profiler.h"

#include "Engine/ECS/Systems/RenderSystem.h"

#include <algorithm>

std::chrono::high_resolution_clock::time_point Profiler::s_frameStartTime = {};
std::array<ProfilerFrameStats, Profiler::kHistorySize> Profiler::s_history{};
size_t Profiler::s_head = 0;
size_t Profiler::s_count = 0;
uint32_t Profiler::s_currentMiscDrawCalls = 0;

void Profiler::BeginFrame()
{
	s_frameStartTime = std::chrono::high_resolution_clock::now();
	s_currentMiscDrawCalls = 0;
}

void Profiler::EndFrame()
{
	const auto endTime = std::chrono::high_resolution_clock::now();
	const float timeMs = std::chrono::duration<float, std::milli>(endTime - s_frameStartTime).count();

	ProfilerFrameStats stats{};
	stats.frameTimeMs = timeMs;
	stats.fps = (timeMs > 0.0f) ? (1000.0f / timeMs) : 0.0f;

	// A案: RenderSystemからPull
	const RenderSystem::GpuDrawStats gpu = RenderSystem::GetLastGpuDrawStats();
	stats.render.totalDrawCalls = gpu.drawIndexedTotal + s_currentMiscDrawCalls;
	stats.render.pbrInstancesDrawn = gpu.pbrInstancesDrawn;

	s_history[s_head] = stats;
	s_head = (s_head + 1) % kHistorySize;
	s_count = std::min(s_count + 1, kHistorySize);
}

const ProfilerFrameStats& Profiler::GetLatest()
{
	// s_count == 0 の場合は先頭のデフォルト値を返す
	if (s_count == 0)
		return s_history[0];

	const size_t latestIdx = (s_head == 0) ? (kHistorySize - 1) : (s_head - 1);
	return s_history[latestIdx];
}

void Profiler::CopyHistory(
	std::vector<float>& outFrameTimeMs,
	std::vector<float>& outTotalDrawCalls,
	std::vector<float>& outPbrInstancesDrawn)
{
	outFrameTimeMs.clear();
	outTotalDrawCalls.clear();
	outPbrInstancesDrawn.clear();

	outFrameTimeMs.resize(s_count);
	outTotalDrawCalls.resize(s_count);
	outPbrInstancesDrawn.resize(s_count);

	// 古い→新しい順（PlotLinesの時系列そのまま）
	for (size_t i = 0; i < s_count; ++i)
	{
		const size_t idx = (s_head + kHistorySize - s_count + i) % kHistorySize;
		outFrameTimeMs[i] = s_history[idx].frameTimeMs;
		outTotalDrawCalls[i] = static_cast<float>(s_history[idx].render.totalDrawCalls);
		outPbrInstancesDrawn[i] = static_cast<float>(s_history[idx].render.pbrInstancesDrawn);
	}
}

void Profiler::AddMiscDrawCall(uint32_t count)
{
	s_currentMiscDrawCalls += count;
}

