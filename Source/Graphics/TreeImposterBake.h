#pragma once

#include "ComPtr.h"
#include <cstdint>
#include <d3d12.h>

class DescriptorHeap;
class DescriptorHandle;
class VertexBuffer;
class IndexBuffer;
struct ModelBounds;
struct TreeSpeciesMaterials;

namespace TreeImposterBake
{
	/// インポスター用の 2 三角四角（Vertex レイアウト 84B）
	bool CreateQuadMeshes(VertexBuffer** outVb, IndexBuffer** outIb);

	/// マージ LOD0 メッシュを種ごとに 8 方向ベイクし、2048×256 RGBA8 アトラス（横 8 スライス）を生成。
	/// heap に各アトラス用 SRV を 6 連続登録（t0..t5 同一テクスチャ、既存 PBR ルートと整合）。
	bool BakeAtlases(
		DescriptorHeap* heap,
		ID3D12RootSignature* rootSig,
		D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,
		D3D12_GPU_DESCRIPTOR_HANDLE iblTable,
		VertexBuffer* meshVB,
		IndexBuffer* meshIB,
		uint32_t meshIndexCount,
		const ModelBounds& meshLocalBounds,
		const TreeSpeciesMaterials* sm0,
		const TreeSpeciesMaterials* sm1,
		const TreeSpeciesMaterials* sm2,
		ComPtr<ID3D12Resource>& outAtlas0,
		ComPtr<ID3D12Resource>& outAtlas1,
		ComPtr<ID3D12Resource>& outAtlas2,
		DescriptorHandle* outMatTableStart[3]);
}
