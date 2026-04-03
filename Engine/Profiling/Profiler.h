#pragma once

#include <cstdint>
#include <vector>
#include <chrono>
#include <array>
#include <d3d12.h>
#include "ComPtr.h"

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
	float renderCpuBeginMs = 0.0f;
	float renderCpuSceneDrawMs = 0.0f;
	float renderCpuEndMs = 0.0f;
	/// BeginFrame〜描画直前（Update / ImGui / Transform など）
	float preRenderCpuTimeMs = 0.0f;
	float gpuDrawMainMs = 0.0f;
	float gpuTerrainDepthPrepassMs = 0.0f;
	float gpuTerrainColorMs = 0.0f;
	float gpuHiZBuildMs = 0.0f;
	float gpuPostProcessMs = 0.0f;
	/// タイムスタンプで取っているパスの合計（地形〜Post＋木アップロード/カリング＋木描画）
	float gpuTaggedPassesSumMs = 0.0f;
	/// 木: アップロード + カリングCS（Scene で DispatchCull 前後）
	float gpuTreeUploadCullMs = 0.0f;
	/// 木: ExecuteIndirect（RenderSystem::DrawPostScenePbrTreesExecuteIndirect、Post CL）
	float gpuTreeDrawMs = 0.0f;
	float fps = 0.0f; // derived from frameTimeMs (text only)
	RenderStats render{};
};

class Profiler
{
public:
	static constexpr size_t kHistorySize = 120;

	static void BeginFrame();
	static void EndFrame(); // Pull from RenderSystem here (A案)
	static void GpuMarkDrawMainBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkDrawMainEnd(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTerrainDepthPrepassBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTerrainDepthPrepassEnd(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTerrainColorBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTerrainColorEnd(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkHiZBuildBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkHiZBuildEnd(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkPostProcessBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkPostProcessEndAndResolve(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTreeUploadCullBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTreeUploadCullEnd(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTreeDrawBegin(ID3D12GraphicsCommandList* cmd);
	static void GpuMarkTreeDrawEnd(ID3D12GraphicsCommandList* cmd);
	/// 木 GPU パスをスキップしたフレームでも、ResolveQueryData(0..13) 用に 10..13 の EndQuery を補う。
	static void EnsureTreeGpuStampPlaceholders(ID3D12GraphicsCommandList* cmd);
	static void SetRenderCpuBreakdown(float beginMs, float sceneDrawMs, float endMs);
	static void SetPreRenderCpuTimeMs(float ms);

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
	static std::array<ProfilerFrameStats, kHistorySize> s_history;
	static size_t s_head;
	static size_t s_count;

	static uint32_t s_currentMiscDrawCalls;
	static float s_currentRenderCpuTimeMs;
	static float s_currentRenderCpuBeginMs;
	static float s_currentRenderCpuSceneDrawMs;
	static float s_currentRenderCpuEndMs;
	static float s_currentPreRenderCpuTimeMs;
	static bool s_gpuProfilerReady;
	static uint64_t s_gpuTimestampFrequency;
	static UINT s_gpuWriteFrameIndex;
	static ComPtr<ID3D12QueryHeap> s_gpuTimestampHeap;
	static ComPtr<ID3D12Resource> s_gpuTimestampReadback;
	static constexpr UINT kGpuStampCountPerFrame = 14;
	static bool EnsureGpuProfilerReady();
	static void WriteGpuStamp(ID3D12GraphicsCommandList* cmd, UINT stampIndexInFrame);
};

