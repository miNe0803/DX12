#pragma once

#include <DirectXMath.h>
#include <d3d12.h>

struct DescriptorHandle;

// 位置・回転・スケールの元データと、計算済みワールド行列（現行は Base + Scale + RotationY で 1 本の木と互換）
struct TransformComponent
{
	DirectX::XMFLOAT4X4 BaseMatrix;
	float               UniformScale;
	float               RotationY;
	DirectX::XMMATRIX   WorldMatrix;
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
