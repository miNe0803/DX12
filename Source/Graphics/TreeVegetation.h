#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <entt/entt.hpp>
#include <DirectXMath.h>

#include "SharedStruct.h"

class DescriptorHandle;
class VertexBuffer;
class IndexBuffer;
struct ModelBounds;
class DescriptorHeap;

/// 3 種（マスク R/G/B）× LOD 用マテリアル（ディスクリプタ先頭 = t0..）
struct TreeSpeciesMaterials
{
	DescriptorHandle* matLod0 = nullptr;
	DescriptorHandle* matLod1 = nullptr;
	DescriptorHandle* matLod2 = nullptr;
};

namespace TreeVegetation
{
	struct StreamedTreeInstance
	{
		DirectX::XMMATRIX worldGpuT{}; // TransformComponent::WorldMatrix（GPU用転置済み）
		uint8_t speciesIndex = 0;      // 0=R tree1, 1=G moss, 2=B sakura
	};

	/// 地形生成後・InitMainPipeline 後に呼ぶ。失敗しても true（木なしで継続）。
	bool Initialize(::entt::registry& registry, DescriptorHeap* heap,
		std::vector<VertexBuffer*>& outOwnedVB, std::vector<IndexBuffer*>& outOwnedIB);

	/// カメラ周辺優先のストリーミング木インスタンス生成（GPU描画用の WorldMatrix[転置済み] と species を返す）。
	/// - 近距離: 密度高
	/// - 遠距離: 距離に応じて間引き
	/// - 生成は決定的（セル座標ハッシュ）でフレーム間ちらつきを抑える
	bool BuildStreamedInstances(
		::entt::registry& registry,
		const DirectX::XMFLOAT3& cameraPos,
		std::vector<StreamedTreeInstance>& outInstances,
		uint32_t maxInstances,
		float nearRadius = 200.0f,
		float farRadius = 1600.0f,
		float cellSize = 16.0f);

	/// マスク上の植生セルを「全部」インスタンス化する（ストリーミング/間引き無し）。
	/// 内部キャッシュが有効なフレームでは再構築せず、かつ out へ大量コピーしない（GetAllMaskInstancesCached を使う）。
	/// 注意: 本数が多い場合は GPU カリング（ExecuteIndirect）で可視だけ描くのが前提。
	bool BuildAllMaskInstances(
		::entt::registry& registry,
		std::vector<StreamedTreeInstance>& outInstances,
		float cellSize = 8.0f);

	/// `BuildAllMaskInstances()` の内部キャッシュを参照（コピー無し）。
	/// 事前に `BuildAllMaskInstances()` を呼んでキャッシュを作っておくこと。
	const std::vector<StreamedTreeInstance>& GetAllMaskInstancesCached();

	/// マスクをフル再構築するたびに増える。本数が同じでも足元Y等で中身が変わるため GPU アップロード判定に使う。
	uint64_t GetMaskInstancesBuildSerial();

	const TreeSpeciesMaterials* GetSpeciesMaterials(size_t speciesIndex);
	/// デバッグ/高品質: LOD0 を幹/葉/枝に分割したパートメッシュ（存在しない場合 nullptr/0）
	VertexBuffer* GetPartVertexBuffer(int part);
	IndexBuffer* GetPartIndexBuffer(int part);
	uint32_t GetPartIndexCount(int part);
	DescriptorHandle* GetPartMaterialHandle(int part);
	VertexBuffer* GetMergedVertexBuffer(); // LOD0
	IndexBuffer* GetMergedIndexBuffer();   // LOD0
	uint32_t GetMergedIndexCount();        // LOD0
	VertexBuffer* GetMergedVertexBufferLod(int lod);
	IndexBuffer* GetMergedIndexBufferLod(int lod);
	uint32_t GetMergedIndexCountLod(int lod);
	const ModelBounds& GetMergedLocalBounds();

	/// LOD1 インポスター板: ベイクと同じ「足元＝AABB 底面中心」ローカル座標と、境界から取った板サイズ。
	struct ImposterBillboardParams
	{
		DirectX::XMFLOAT3 FootLocal{};
		float HalfWidth = 1.f;
		float Height = 1.f;
	};
	bool GetImposterBillboardParams(ImposterBillboardParams* out);

	/// マスクからスポーンした本数（Initialize 成功時）。0 のときは未初期化またはマスク無し。
	uint32_t GetSpawnedTreeCount();

	// --- LOD0 ジオメトリ（種0・近距離メッシュ）---
	/// GPU に LOD0 マージメッシュが登録されているか（マスク/インスタンス数とは独立）。
	bool IsLod0MeshReady();
	/// 実際に使った LOD0 ソース（解決済み .tmesh / "procedural"）。未設定時は空文字。
	const wchar_t* GetLod0SourcePath();
}
