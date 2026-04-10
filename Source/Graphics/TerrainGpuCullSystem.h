#pragma once

#include "ComPtr.h"
#include <d3d12.h>
#include <cstdint>
#include <vector>

#include "TerrainGenerator.h"

class DescriptorHeap;
struct DescriptorHandle;
class RootSignature;
class PipelineState;
class VertexBuffer;
class IndexBuffer;

struct SceneConstants;

//GPU 視錐台カリング + ExecuteIndirect（地形チャンク専用）
class TerrainGpuCullSystem
{
public:
	~TerrainGpuCullSystem();
	void Shutdown();


	bool Init(
		ID3D12Device* device,
		DescriptorHeap* descriptorHeap,
		const std::vector<TerrainChunkDesc>& chunks);

	bool IsValid() const { return m_valid; }
	uint32_t ChunkCount() const { return m_chunkCount; }
	D3D12_GPU_VIRTUAL_ADDRESS DrawPayloadBufferGpuAddress() const;
	void SetHiZResources(DescriptorHandle* hizSrv, uint32_t hizWidth, uint32_t hizHeight, uint32_t hizMipCount, bool enabled);
	void SetHiZOcclusionEnabled(bool enabled);
	bool GetHiZOcclusionEnabled() const { return m_hizUserEnabled; }
	void SetHiZOcclusionTuning(float nearDisableDistance, float depthBias, float maxPixelRadius);
	void GetHiZOcclusionTuning(float& outNearDisableDistance, float& outDepthBias, float& outMaxPixelRadius) const;
	void SetLodDistanceTuning(float lod0StartDistance, float lod1StartDistance);
	void GetLodDistanceTuning(float& outLod0StartDistance, float& outLod1StartDistance) const;
	void SetForceLod1(bool enabled);
	bool GetForceLod1() const { return m_forceLod1; }
	uint32_t GetDebugLastLod0VisibleCount() const { return m_debugLastLod0VisibleCount; }
	uint32_t GetDebugLastLod1VisibleCount() const { return m_debugLastLod1VisibleCount; }
	uint32_t GetDebugLastGpuVisibleCount() const { return m_debugLastGpuVisibleCount; }
	uint32_t GetDebugLastLod0IndexCount() const { return m_debugLastLod0IndexCount; }
	uint32_t GetDebugLastLod1IndexCount() const { return m_debugLastLod1IndexCount; }
	void SetEnableCpuDebugEstimation(bool enabled) { m_enableCpuDebugEstimation = enabled; }
	bool GetEnableCpuDebugEstimation() const { return m_enableCpuDebugEstimation; }

	/// フレーム先頭: カウンタクリア + 視錐台 CS（SceneConstants の View/Proj と同一の VP）
	void DispatchFrustumCull(ID3D12GraphicsCommandList* cmd, const SceneConstants* scene);

	/// 地形描画直前: バリア後 ExecuteIndirect（Terrain PSO/ルート/VB/IB は呼び出し側でバインド済みであること）
	void DrawIndirect(
		ID3D12GraphicsCommandList* cmd,
		RootSignature* terrainRootSig,
		PipelineState* terrainPso,
		D3D12_GPU_VIRTUAL_ADDRESS perDrawTransformGpu,
		D3D12_GPU_VIRTUAL_ADDRESS terrainMaterialGpu,
		D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskTable,
		D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
		VertexBuffer* vb,
		IndexBuffer* ib,
		D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSrvGpu = {},
		D3D12_GPU_VIRTUAL_ADDRESS shadowCBGpu = 0);

private:
	bool CreatePipelines(ID3D12Device* device);
	bool CreateIndirectCommandSignature(ID3D12Device* device);

	ComPtr<ID3D12Resource> m_chunkInfoDefault;
	ComPtr<ID3D12Resource> m_chunkUpload;
	ComPtr<ID3D12Resource> m_indirectArgsDefault;
	ComPtr<ID3D12Resource> m_counterDefault;
	ComPtr<ID3D12Resource> m_counterResetUpload;
	ComPtr<ID3D12Resource> m_cullCBUpload;
	ComPtr<ID3D12Resource> m_drawPayloadDefault;
	ComPtr<ID3D12Resource> m_hizFallbackResource;

	ComPtr<ID3D12RootSignature> m_computeRootSig;
	ComPtr<ID3D12PipelineState> m_computePso;
	ComPtr<ID3D12CommandSignature> m_drawIndexedSig;
	std::vector<ModelBounds> m_chunkBoundsCpu;
	std::vector<uint32_t> m_chunkLod0IndexCountCpu;
	std::vector<uint32_t> m_chunkLod1IndexCountCpu;
	std::vector<ComPtr<ID3D12Resource>> m_counterReadback;

	DescriptorHeap* m_descriptorHeap = nullptr;
	DescriptorHandle* m_srvChunk = nullptr;
	DescriptorHandle* m_uavArgs = nullptr;
	DescriptorHandle* m_uavCounter = nullptr;
	DescriptorHandle* m_uavPayload = nullptr;
	DescriptorHandle* m_hizFallbackSrv = nullptr;
	DescriptorHandle* m_hizSrv = nullptr;

	D3D12_CPU_DESCRIPTOR_HANDLE m_counterUavCpuForClear{};
	bool m_counterClearCpuValid = false;

	uint32_t m_chunkCount = 0;
	uint32_t m_maxIndirectCommands = 0;
	bool m_valid = false;

	// 初期状態は COMMON(0)。型名は D3D12_RESOURCE_STATES（SDK の列挙型名）
	D3D12_RESOURCE_STATES m_counterResState = static_cast<D3D12_RESOURCE_STATES>(0);
	D3D12_RESOURCE_STATES m_indirectResState = static_cast<D3D12_RESOURCE_STATES>(0);
	D3D12_RESOURCE_STATES m_payloadResState = static_cast<D3D12_RESOURCE_STATES>(0);

	bool m_hizEnabled = false;
	bool m_hizUserEnabled = true;
	uint32_t m_hizWidth = 1;
	uint32_t m_hizHeight = 1;
	uint32_t m_hizMipCount = 1;
	float m_hizNearDisableDistance = 200.0f;
	float m_hizDepthBias = 0.01f;
	float m_hizMaxPixelRadius = 96.0f;
	float m_lod0StartDistance = 300.0f;
	float m_lod1StartDistance = 900.0f;
	bool m_forceLod1 = false;
	uint32_t m_debugLastLod0VisibleCount = 0;
	uint32_t m_debugLastLod1VisibleCount = 0;
	uint32_t m_debugLastGpuVisibleCount = 0;
	uint32_t m_debugLastLod0IndexCount = 0;
	uint32_t m_debugLastLod1IndexCount = 0;
	bool m_enableCpuDebugEstimation = true;
};
