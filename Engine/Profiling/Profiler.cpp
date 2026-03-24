#include "Profiler.h"

#include "Engine/ECS/Systems/RenderSystem.h"
#include "Engine.h"

#include <algorithm>

std::chrono::high_resolution_clock::time_point Profiler::s_frameStartTime = {};
std::array<ProfilerFrameStats, Profiler::kHistorySize> Profiler::s_history{};
size_t Profiler::s_head = 0;
size_t Profiler::s_count = 0;
uint32_t Profiler::s_currentMiscDrawCalls = 0;
float Profiler::s_currentRenderCpuTimeMs = 0.0f;
float Profiler::s_currentRenderCpuBeginMs = 0.0f;
float Profiler::s_currentRenderCpuSceneDrawMs = 0.0f;
float Profiler::s_currentRenderCpuEndMs = 0.0f;
float Profiler::s_currentPreRenderCpuTimeMs = 0.0f;
bool Profiler::s_gpuProfilerReady = false;
uint64_t Profiler::s_gpuTimestampFrequency = 0;
UINT Profiler::s_gpuWriteFrameIndex = 0;
ComPtr<ID3D12QueryHeap> Profiler::s_gpuTimestampHeap = nullptr;
ComPtr<ID3D12Resource> Profiler::s_gpuTimestampReadback = nullptr;

void Profiler::BeginFrame()
{
	s_frameStartTime = std::chrono::high_resolution_clock::now();
	s_currentMiscDrawCalls = 0;
	s_currentRenderCpuTimeMs = 0.0f;
	s_currentRenderCpuBeginMs = 0.0f;
	s_currentRenderCpuSceneDrawMs = 0.0f;
	s_currentRenderCpuEndMs = 0.0f;
	s_currentPreRenderCpuTimeMs = 0.0f;
}

void Profiler::EndFrame()
{
	const auto endTime = std::chrono::high_resolution_clock::now();
	const float timeMs = std::chrono::duration<float, std::milli>(endTime - s_frameStartTime).count();

	ProfilerFrameStats stats{};
	stats.frameTimeMs = timeMs;
	stats.renderCpuTimeMs = s_currentRenderCpuTimeMs;
	stats.renderCpuBeginMs = s_currentRenderCpuBeginMs;
	stats.renderCpuSceneDrawMs = s_currentRenderCpuSceneDrawMs;
	stats.renderCpuEndMs = s_currentRenderCpuEndMs;
	stats.preRenderCpuTimeMs = s_currentPreRenderCpuTimeMs;
	stats.fps = (timeMs > 0.0f) ? (1000.0f / timeMs) : 0.0f;
	stats.gpuDrawMainMs = 0.0f;
	stats.gpuTerrainDepthPrepassMs = 0.0f;
	stats.gpuTerrainColorMs = 0.0f;
	stats.gpuHiZBuildMs = 0.0f;
	stats.gpuPostProcessMs = 0.0f;

	if (s_gpuProfilerReady && s_gpuTimestampReadback && s_gpuTimestampFrequency > 0 && g_Engine)
		{
			void* map = nullptr;
			if (SUCCEEDED(s_gpuTimestampReadback->Map(0, nullptr, &map)) && map)
			{
			// Present のあと CurrentBackBufferIndex は「次に描く」側に進んでいる。Resolve は LastSubmitted と一致するスロット。
			const UINT readFrame = g_Engine->LastSubmittedBackBufferIndex();
			const UINT64* t = reinterpret_cast<const UINT64*>(map) + (readFrame * kGpuStampCountPerFrame);
			const double toMs = 1000.0 / static_cast<double>(s_gpuTimestampFrequency);
			const UINT64 drawMainBegin = t[0], drawMainEnd = t[1];
			const UINT64 terrDepthBegin = t[2], terrDepthEnd = t[3];
			const UINT64 terrColorBegin = t[4], terrColorEnd = t[5];
			const UINT64 hizBegin = t[6], hizEnd = t[7];
			const UINT64 postBegin = t[8], postEnd = t[9];
			if (drawMainEnd >= drawMainBegin)
				stats.gpuDrawMainMs = static_cast<float>(static_cast<double>(drawMainEnd - drawMainBegin) * toMs);
			if (terrDepthEnd >= terrDepthBegin)
				stats.gpuTerrainDepthPrepassMs = static_cast<float>(static_cast<double>(terrDepthEnd - terrDepthBegin) * toMs);
			if (terrColorEnd >= terrColorBegin)
				stats.gpuTerrainColorMs = static_cast<float>(static_cast<double>(terrColorEnd - terrColorBegin) * toMs);
			if (hizEnd >= hizBegin)
				stats.gpuHiZBuildMs = static_cast<float>(static_cast<double>(hizEnd - hizBegin) * toMs);
			if (postEnd >= postBegin)
				stats.gpuPostProcessMs = static_cast<float>(static_cast<double>(postEnd - postBegin) * toMs);
			s_gpuTimestampReadback->Unmap(0, nullptr);
		}
	}

	// A案: RenderSystemからPull
	const RenderSystem::GpuDrawStats gpu = RenderSystem::GetLastGpuDrawStats();
	stats.render.totalDrawCalls = gpu.drawIndexedTotal + s_currentMiscDrawCalls;
	stats.render.pbrInstancesDrawn = gpu.pbrInstancesDrawn;

	s_history[s_head] = stats;
	s_head = (s_head + 1) % kHistorySize;
	s_count = std::min(s_count + 1, kHistorySize);
}

void Profiler::SetPreRenderCpuTimeMs(float ms)
{
	s_currentPreRenderCpuTimeMs = ms;
}

bool Profiler::EnsureGpuProfilerReady()
{
	if (s_gpuProfilerReady)
		return true;
	if (!g_Engine || !g_Engine->Device() || !g_Engine->Queue())
		return false;
	D3D12_QUERY_HEAP_DESC qd = {};
	qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	qd.Count = Engine::FRAME_BUFFER_COUNT * kGpuStampCountPerFrame;
	qd.NodeMask = 0;
	if (FAILED(g_Engine->Device()->CreateQueryHeap(&qd, IID_PPV_ARGS(s_gpuTimestampHeap.ReleaseAndGetAddressOf()))))
		return false;
	auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * qd.Count);
	auto rbHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
	if (FAILED(g_Engine->Device()->CreateCommittedResource(
		&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(s_gpuTimestampReadback.ReleaseAndGetAddressOf()))))
		return false;
	if (FAILED(g_Engine->Queue()->GetTimestampFrequency(&s_gpuTimestampFrequency)))
		return false;
	s_gpuProfilerReady = true;
	return true;
}

void Profiler::WriteGpuStamp(ID3D12GraphicsCommandList* cmd, UINT stampIndexInFrame)
{
	if (!cmd || !EnsureGpuProfilerReady() || !s_gpuTimestampHeap)
		return;
	const UINT queryIndex = s_gpuWriteFrameIndex * kGpuStampCountPerFrame + stampIndexInFrame;
	cmd->EndQuery(s_gpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
}

void Profiler::GpuMarkDrawMainBegin(ID3D12GraphicsCommandList* cmd)
{
	if (!g_Engine)
		return;
	s_gpuWriteFrameIndex = g_Engine->CurrentBackBufferIndex();
	WriteGpuStamp(cmd, 0);
}

void Profiler::GpuMarkDrawMainEnd(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 1);
}

void Profiler::GpuMarkHiZBuildBegin(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 6);
}

void Profiler::GpuMarkHiZBuildEnd(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 7);
}

void Profiler::GpuMarkPostProcessBegin(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 8);
}

void Profiler::GpuMarkPostProcessEndAndResolve(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 9);
	if (!cmd || !EnsureGpuProfilerReady() || !s_gpuTimestampHeap || !s_gpuTimestampReadback)
		return;
	const UINT begin = s_gpuWriteFrameIndex * kGpuStampCountPerFrame;
	const UINT64 dstOffset = static_cast<UINT64>(begin) * sizeof(UINT64);
	cmd->ResolveQueryData(
		s_gpuTimestampHeap.Get(),
		D3D12_QUERY_TYPE_TIMESTAMP,
		begin,
		kGpuStampCountPerFrame,
		s_gpuTimestampReadback.Get(),
		dstOffset);
}

void Profiler::GpuMarkTerrainDepthPrepassBegin(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 2);
}

void Profiler::GpuMarkTerrainDepthPrepassEnd(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 3);
}

void Profiler::GpuMarkTerrainColorBegin(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 4);
}

void Profiler::GpuMarkTerrainColorEnd(ID3D12GraphicsCommandList* cmd)
{
	WriteGpuStamp(cmd, 5);
}

void Profiler::SetRenderCpuBreakdown(float beginMs, float sceneDrawMs, float endMs)
{
	s_currentRenderCpuBeginMs = beginMs;
	s_currentRenderCpuSceneDrawMs = sceneDrawMs;
	s_currentRenderCpuEndMs = endMs;
	s_currentRenderCpuTimeMs = beginMs + sceneDrawMs + endMs;
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

