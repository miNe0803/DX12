#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <entt/entt.hpp>
#include <string>
#include <vector>

#include "Components/PlayerComponent.h"
#include "Core/ModelBounds.h"

struct DescriptorHandle;

// 位置・回転・スケールの元データと、計算済みワールド行列
// 地形などは Position で配置。木などは BaseMatrix + UniformScale + RotationY のまま（Position は 0 でよい）
struct TransformComponent
{
	// NOTE: 未初期化だと TransformSystem の合成で「全部動かない」になり得るため、
	// デフォルトは Identity / scale=1 / rot=0 に揃える。
	DirectX::XMFLOAT4X4 BaseMatrix = {
		1.f, 0.f, 0.f, 0.f,
		0.f, 1.f, 0.f, 0.f,
		0.f, 0.f, 1.f, 0.f,
		0.f, 0.f, 0.f, 1.f
	};
	DirectX::XMFLOAT3   Position = { 0.0f, 0.0f, 0.0f }; // ワールド位置（地形エンティティなどで使用）
	float               UniformScale = 1.0f;
	float               RotationY = 0.0f;
	// XMMATRIX は 16 バイト境界必須。ここまでのオフセットが 84 のままだと未定義動作になり Position が反映されない
	alignas(16) DirectX::XMMATRIX WorldMatrix;
};

// 描画に必要な D3D12 ラッパーとマテリアル（PBR テクスチャ 4 つの先頭ハンドル）、影フラグ
struct MeshRendererComponent
{
	class VertexBuffer*  pVB;
	class IndexBuffer*   pIB;
	UINT                 IndexCount;
	/// 共有 IB のときチャンク先頭（DrawIndexed の StartIndexLocation）。通常 0。
	UINT                 StartIndexLocation = 0;
	/// false のとき Scene が共有 VB/IB を破棄（地形チャンク等）
	bool                 OwnsGpuBuffers = true;
	DescriptorHandle*    MaterialHandle; // t0 先頭（albedo, normal, metallic, roughness, ramp, sphere）
	bool                 CastShadow = true;
	/// ローカル空間 AABB（スポーン時に設定）。視錐台カリング用。地形など未設定なら false。
	bool                 HasLocalBounds = false;
	ModelBounds          LocalBounds{};
	/// true のとき CPU 視錐台カリングをしない（スキン: バインドポーズ AABB が枝先を表さないため）
	bool                 SkipCpuFrustumCull = false;
	/// NPR 透明パス用（親に NPRTag かつ Assimp 透明ルール）
	bool                 NprTransparent = false;
	/// NPR セル影: 頂点法線ブレンド上書き（>=0 でインスタンスごと）。顔パーツはロード時に設定
	float                NprCelVertexBlendOverride = -1.f;
	/// PMX スフィアモード（InstanceData.NprPerMesh.y へ渡す）
	uint8_t              NprSphereMode = 0;
	/// PMX 等のマテリアル Opacity（NPR 透明パスで NprPerMesh.z に乗算）
	float                NprOpacity = 1.f;
	/// Bindless material buffer index (set during model registration). 0 = default PBR.
	uint32_t             BindlessMaterialIndex = 0;
};

/// NPR（トゥーン）描画対象のモデルルートに付与
struct NPRTag {};

// 広域パフォーマンス管理（建物、地形、木、草など）
struct LODComponent
{
	int   CurrentLODLevel;   // 0=ハイポリ, 1=ミドル, 2=ローポリ, 3=カリング(非表示)
	float DistanceToCamera;
};

/// 地形ツリーマスクから配置されたインスタンス（種ごとに Mat LOD を差し替え）
struct TreeInstanceTag
{
	uint8_t SpeciesIndex = 0; // 0=R tree1, 1=G moss, 2=B sakura
};

// 地形：CPU 側の高さデータを保持（GetHeight / Flatten 用）。描画メッシュは子の TerrainMeshTag 側。
struct TerrainComponent
{
	std::vector<float> HeightData;
	UINT  GridWidth   = 0;
	UINT  GridDepth   = 0;
	float CellSpacing = 1.0f;
	float MaxHeight   = 100.0f;
};

/// 地形チャンク描画エンティティ（共有 VB/IB の範囲描画）
struct TerrainMeshTag {};

// Editor / Hierarchy 表示用（非同期ロード等でスポーンしたメッシュに自動付与）
struct EditorHierarchyLabelComponent
{
	std::wstring displayName;
};

// 1ファイルロード＝1親 + 複数子メッシュ（親で位置・回転・スケール一括）
struct ModelGroupRootComponent
{
	std::vector<entt::entity> children;
	/// 全サブメッシュを含むモデル空間 AABB（スポーン時）。子の CPU 視錐台は各パーツ AABB ではなくこれを使う。
	ModelBounds combinedModelBounds{};
	bool hasCombinedModelBounds = false;
};

struct ModelGroupChildComponent
{
	entt::entity parent{};
	/// 同一親の子メッシュの描画順（小さいほど先）。木は幹→枝→葉で重なり部の Z 競合を抑える
	uint8_t siblingDrawOrder = 0;
};
