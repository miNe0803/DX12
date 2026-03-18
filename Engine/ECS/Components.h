#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <entt/entt.hpp>
#include <string>
#include <vector>

#include "Components/PlayerComponent.h"

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
	DescriptorHandle*    MaterialHandle; // t0～t3 の先頭（albedo, normal, metallic, roughness）
	bool                 CastShadow = true;
};

// 広域パフォーマンス管理（建物、地形、木、草など）
struct LODComponent
{
	int   CurrentLODLevel;   // 0=ハイポリ, 1=ミドル, 2=ローポリ, 3=カリング(非表示)
	float DistanceToCamera;
};

// 地形：CPU 側の高さデータを保持（GetHeight / Flatten 用）
struct TerrainComponent
{
	std::vector<float> HeightData;
	UINT  GridWidth   = 0;
	UINT  GridDepth   = 0;
	float CellSpacing = 1.0f;
	float MaxHeight   = 100.0f;
};

// Editor / Hierarchy 表示用（非同期ロード等でスポーンしたメッシュに自動付与）
struct EditorHierarchyLabelComponent
{
	std::wstring displayName;
};

// 1ファイルロード＝1親 + 複数子メッシュ（親で位置・回転・スケール一括）
struct ModelGroupRootComponent
{
	std::vector<entt::entity> children;
};

struct ModelGroupChildComponent
{
	entt::entity parent{};
};
