#pragma once
#include <DirectXMath.h>
#include <d3d12.h>
#include "DescriptorHeap.h"

// --- [Data Components] ---

// 3Dモデルの描画に必要なリソース
struct MeshComponent {
    class VertexBuffer* pVB;
    class IndexBuffer* pIB;
    unsigned int        indexCount;
};

// PBRマテリアルのテクスチャデータ
struct MaterialComponent {
    DescriptorHandle* albedoHandle;
    DescriptorHandle* normalHandle;
    DescriptorHandle* metallicHandle;
    DescriptorHandle* roughnessHandle;
};

// 以前のこだわり：樹皮などの法線強度を制御するパラメータ
struct PBRPropertyComponent {
    DirectX::XMFLOAT4 RimParams; // RimParams.y = NormalScale 
};

// 座標・回転・スケール
struct TransformComponent {
    DirectX::XMMATRIX world;
};