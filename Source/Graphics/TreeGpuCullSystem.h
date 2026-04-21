#pragma once

// 大量植生（数十万本）向けパイプライン:
// - 共有ジオメトリ（種×LOD×パートの VB/IB）× インスタンスバッファ
// - TreeFrustumHiZCull_CS: 視錐+Hi-Z → 可視インスタンスインデックスをコンパクト化
// - バッチごとに 1 回の ExecuteIndirect（最大 27 = 3 species × 3 LOD × 3 part）
// - 材質: 現状はルートのディスクリプタテーブル（種×パート×LOD）を DrawIndirectLods で差し替え。
//   次段の bindless: 大きな SRV ヒープ + インスタンス／ドローごとの材質インデックス（HLSL NonUniformResourceIndex）。

#include "ComPtr.h"
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include <DirectXMath.h>

/// マスク全セル植生の上限（2^18 ≒ 26万本）。Init / Scene 再初期化で揃える。
inline constexpr uint32_t kTreeGpuMaxMaskInstances = 262144u;

class DescriptorHeap;
struct DescriptorHandle;
struct SceneConstants;
class RootSignature;
class PipelineState;
class VertexBuffer;
class IndexBuffer;

/// 木インスタンス用: GPU 視錐台 + Hi-Z オクルージョン → ExecuteIndirect
/// 現在は (species×LOD×part) に分割した間接コマンドを生成し、材質/パート/LOD を正しく切り替えて描画する。
class TreeGpuCullSystem
{
public:
	~TreeGpuCullSystem();
	void Shutdown();

	struct TreeInstanceCpu
	{
		DirectX::XMMATRIX worldGpuT; // TransformComponent::WorldMatrix（GPU用転置済み）
		uint8_t speciesIndex = 0;    // 0=R tree1, 1=G moss, 2=B sakura
	};

	/// 最大本数でバッファ確保。descriptorHeap に SRV/UAV を登録。失敗時 IsValid()==false
	bool Init(
		ID3D12Device* device,
		DescriptorHeap* descriptorHeap,
		ID3D12RootSignature* pbrRootSignature,
		uint32_t maxInstances);

	bool IsValid() const { return m_valid; }
	uint32_t GetMaxInstances() const { return m_maxInstances; }
	uint32_t GetInstanceCount() const { return m_instanceCount; }
	void SetHiZResources(DescriptorHandle* hizSrv, uint32_t hizWidth, uint32_t hizHeight, uint32_t hizMipCount, bool enabled);

	/// カメラとの水平距離 (XZ) で LOD 閾値を設定（TreeFrustumHiZCull_CS の LodParams と一致）。lod2Start は lod1Start より大きくクランプされる。
	void SetTreeLodDistanceTuning(float lod1StartMeters, float lod2StartMeters);
	void GetTreeLodDistanceTuning(float& outLod1StartMeters, float& outLod2StartMeters) const;

	/// 最大描画距離 (m)。0 = 無制限。これ以遠のインスタンスは全 LOD で描画しない。
	void SetMaxDrawDistance(float meters) { m_maxDrawDistance = (meters > 0.0f) ? meters : 0.0f; }
	float GetMaxDrawDistance() const { return m_maxDrawDistance; }

	/// デバッグ: LOD0 描画スキップ（LOD1+LOD2 のみ描画して LOD0 のコストを孤立測定）
	void SetDebugSkipLod0(bool skip) { m_debugSkipLod0 = skip; }
	bool GetDebugSkipLod0() const { return m_debugSkipLod0; }

	/// 直近フレームのカウンターreadback（LOD0/1/2 合計）
	void GetLastCounterReadback(uint32_t& outLod0, uint32_t& outLod1, uint32_t& outLod2) const
	{
		outLod0 = m_lastReadbackLod0; outLod1 = m_lastReadbackLod1; outLod2 = m_lastReadbackLod2;
	}

	/// デバッグ: 全インスタンスを距離閾値だけで LOD0/1/2 に振り分けた本数（TreeFrustumHiZCull の LodParams と同一。視錐・Hi-Z は含まない）
	static void ComputeDebugDistanceLodCounts(
		const TreeInstanceCpu* instances,
		uint32_t count,
		const DirectX::XMFLOAT3& cameraWorldPos,
		float lod1StartMeters,
		float lod2StartMeters,
		uint32_t& outLod0,
		uint32_t& outLod1,
		uint32_t& outLod2);

	/// 毎フレーム: CPU から instance/world と材質を更新（アップロード→デフォルトへコピー）
	void UpdateInstances(ID3D12GraphicsCommandList* cmd, const TreeInstanceCpu* instances, uint32_t count);

	/// 毎フレーム先頭: カウンタクリア + カリング CS
	/// cameraWorldPos: LOD 距離計算用（inv(View) からの復元は View 行列の解釈で外れやすいため明示指定）
	/// 実際にディスパッチしたときだけ true（Hi-Z 未設定などでスキップしたら false）
	bool DispatchCull(
		ID3D12GraphicsCommandList* cmd,
		const SceneConstants* scene,
		const uint32_t indexCountByPartByLod[3][3],
		const DirectX::XMFLOAT3& cameraWorldPos);

	/// 木描画: VB/IB/PSO は呼び出し側で設定済み。ExecuteIndirect で可視だけ DrawIndexedInstanced(1)。
	void DrawIndirect(
		ID3D12GraphicsCommandList* cmd,
		RootSignature* pbrRootSig,
		PipelineState* pbrPso,
		D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
		D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
		D3D12_GPU_DESCRIPTOR_HANDLE treeMaterialTable,
		D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
		VertexBuffer* vb,
		IndexBuffer* ib,
		UINT indexCount);

	/// LOD0/1/2 を ExecuteIndirect で描画（CS 側で距離LOD分岐）。LOD1 はインポスター（四角 6 idx）を psoImposter で描く。
	void DrawIndirectLods(
		ID3D12GraphicsCommandList* cmd,
		RootSignature* pbrRootSig,
		PipelineState* psoOpaque,          // trunk/branches
		PipelineState* psoLeavesAlphaCut,  // leaves
		PipelineState* psoImposterLod1,    // LOD1: baked imposter quad + TreeImposterPS
		PipelineState* psoLod0DepthPrepass, // LOD0 depth-only prepass (can be nullptr to skip)
		D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
		D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
		const D3D12_GPU_DESCRIPTOR_HANDLE matBySpeciesByPartByLod[3][3][3],
		const D3D12_GPU_DESCRIPTOR_HANDLE imposterMatTableBySpecies[3],
		D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
		VertexBuffer* vbByPartByLod[3][3],
		IndexBuffer* ibByPartByLod[3][3],
		const uint32_t indexCountByPartByLod[3][3],
		VertexBuffer* vbImposterQuad,
		IndexBuffer* ibImposterQuad);

	/// デバッグ用: ExecuteIndirect を使わずに直接 instancing で描く（可視化/切り分け用）
	void DrawDirectInstancedDebug(
		ID3D12GraphicsCommandList* cmd,
		RootSignature* pbrRootSig,
		PipelineState* psoOpaque,
		D3D12_GPU_VIRTUAL_ADDRESS sceneCbGpu,
		D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
		D3D12_GPU_DESCRIPTOR_HANDLE treeMaterialTable,
		D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
		VertexBuffer* vb,
		IndexBuffer* ib,
		UINT indexCount,
		uint32_t instanceCountOverride = 0);

	uint32_t GetDebugLastGpuVisibleCount() const { return m_debugLastGpuVisibleCount; }
	/// 直近の DrawIndirectLods で発行した ExecuteIndirect 呼び出し数（デバッグ統計用）
	uint32_t GetLastDrawIndirectBatchCount() const { return m_lastDrawIndirectBatchCount; }

	/// GPU instance data buffer (all species, for shadow pass etc.)
	ID3D12Resource* GetInstanceDataResource() const { return m_instanceDataDefault.Get(); }

	/// GPU TreeInfo buffer (center/radius/world, for shadow frustum culling)
	ID3D12Resource* GetTreeInfoResource() const { return m_treeInfoDefault.Get(); }

private:
	bool CreatePipelines(ID3D12Device* device);
	bool CreateIndirectCommandSignature(ID3D12Device* device, ID3D12RootSignature* pbrRootSignature);

	uint32_t m_maxInstances = 0;
	uint32_t m_instanceCount = 0;
	bool m_valid = false;

	DescriptorHeap* m_descriptorHeap = nullptr;
	DescriptorHandle* m_hizSrv = nullptr;
	DescriptorHandle* m_hizFallbackSrv = nullptr;
	ComPtr<ID3D12Resource> m_hizFallbackResource;
	uint32_t m_hizWidth = 1;
	uint32_t m_hizHeight = 1;
	uint32_t m_hizMipCount = 1;
	bool m_hizEnabled = false;

	/// LOD: 水平距離 (XZ) < m_lod1Start → LOD0（フルメッシュ）; それ以上 → LOD1（インポスター or LOD1 メッシュ）。
	/// 既定 2m: カメラ周辺のみフルメッシュ、その外は LOD1。
	float m_lod1StartDistance = 40.0f;
	float m_lod2StartDistance = 120.0f;
	float m_maxDrawDistance = 250.0f; // 250m 以遠は 1px 未満 → カリング
	bool m_debugSkipLod0 = false;
	uint32_t m_lastReadbackLod0 = 0;
	uint32_t m_lastReadbackLod1 = 0;
	uint32_t m_lastReadbackLod2 = 0;
	// 前フレームのバッチ単位 InstanceCount（0 のバッチは描画スキップ）
	uint32_t m_lastBatchInstanceCount[27] = {}; // kBatchCount=27

	ComPtr<ID3D12RootSignature> m_computeRootSig;
	ComPtr<ID3D12PipelineState> m_computePso;
	ComPtr<ID3D12CommandSignature> m_cmdSig;

	ComPtr<ID3D12Resource> m_cullCBUpload;
	ComPtr<ID3D12Resource> m_treeInfoDefault;
	ComPtr<ID3D12Resource> m_treeInfoUpload;
	ComPtr<ID3D12Resource> m_instanceDataDefault;
	ComPtr<ID3D12Resource> m_instanceDataUpload;
	static constexpr int kSpeciesCount = 3;
	static constexpr int kLodCount = 3;
	static constexpr int kPartCount = 3;
	static constexpr int kBatchCount = kSpeciesCount * kLodCount * kPartCount;

	// Per batch:
	// - visible index buffer: uint32[maxInstances]
	// - indirect args buffer: single D3D12_DRAW_INDEXED_ARGUMENTS (20 bytes)
	ComPtr<ID3D12Resource> m_visibleIndexDefault[kBatchCount] = {};
	ComPtr<ID3D12Resource> m_indirectArgsDefault[kBatchCount] = {};
	ComPtr<ID3D12Resource> m_counterDefault[kBatchCount] = {};
	ComPtr<ID3D12Resource> m_indirectResetUpload; // sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) zero template
	ComPtr<ID3D12Resource> m_counterResetUpload;  // 4B zero

	DescriptorHandle* m_srvTreeInfo = nullptr;
	DescriptorHandle* m_uavVisible[kBatchCount] = {};
	DescriptorHandle* m_uavArgs[kBatchCount] = {};
	DescriptorHandle* m_uavCounter[kBatchCount] = {};

	D3D12_RESOURCE_STATES m_infoState = static_cast<D3D12_RESOURCE_STATES>(0);
	D3D12_RESOURCE_STATES m_instanceState = static_cast<D3D12_RESOURCE_STATES>(0);
	D3D12_RESOURCE_STATES m_visibleState[kBatchCount] = {};
	D3D12_RESOURCE_STATES m_indirectState[kBatchCount] = {};
	D3D12_RESOURCE_STATES m_counterState[kBatchCount] = {};

	uint32_t m_debugLastGpuVisibleCount = 0;
	uint32_t m_lastDrawIndirectBatchCount = 0;

	ComPtr<ID3D12Resource> m_counterReadback;
	ComPtr<ID3D12Resource> m_indirectArgsReadback;
	bool m_counterReadbackPending = false;
};

