#pragma once
#include <d3dx12.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

struct Bone
{
    std::string name;
    DirectX::XMMATRIX offsetMatrix;
};

enum class ModelUpAxis
{
    Y_UP,
    Z_UP,
    UNKNOWN
};
// -----------------



struct Vertex {
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT2 UV;
    DirectX::XMFLOAT3 Tangent;
    DirectX::XMFLOAT4 Color;
    uint16_t BoneIndex[4]; 
    float    BoneWeight[4];

    static const D3D12_INPUT_LAYOUT_DESC InputLayout;
    static const int InputElementCount = 7;
    static const D3D12_INPUT_ELEMENT_DESC InputElements[7];
};


static_assert(sizeof(Vertex) == 84, "Vertex size must be 84 bytes.");

/// PBR instancing: one row-major world matrix per instance (HLSL row_major + mul(pos, World)).
/// NprPerMesh: x=セル影用頂点ブレンド上書き(>=0 で有効), w=1 で x を使用（未設定は -1）
struct InstanceData {
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4 NprPerMesh;
};
static_assert(sizeof(InstanceData) == 80, "InstanceData must match HLSL StructuredBuffer stride");

/// Per-frame ring slices; keep in sync with Engine::FRAME_BUFFER_COUNT.
inline constexpr size_t kMaxPbrInstancesPerFrame = 16384u;
inline constexpr size_t kPbrInstanceRingFrameCount = 2u;

/// PBR path b0: camera only (View/Proj). 256-byte aligned for CBV.
struct alignas(256) SceneConstants {
    DirectX::XMMATRIX View;
    DirectX::XMMATRIX Proj;
};

struct alignas(256) Transform {
    DirectX::XMMATRIX World;
    DirectX::XMMATRIX View;
    DirectX::XMMATRIX Proj;
};

// Draw ごとに b0 を別スロットに分ける（同一アドレスだと GPU 実行時は最後の World だけが全メッシュに効く）
inline constexpr size_t kPerDrawTransformSlotCount = 16384u;
inline constexpr size_t kPerDrawTransformCBBytes = sizeof(Transform) * kPerDrawTransformSlotCount;
static_assert(sizeof(Transform) == 256u, "Transform CB stride must be 256 for root CBV offsets");

// PBR用: RimParams + CameraPos + NPR 調整（NPR シェーダで使用。PBR は主に RimParams.y のみ）
// RimParams: x=不透明NPR露出 y=法線スケール z=リムべき w=リム加算強度
// NprTuning: x=virtualLight(透明) y=透明露出 z=不透明clip閾値 w=影の環境色スケール
// NprTuning2: x=セル影の頂点法線ブレンド(0=法線マップ優先,1=頂点のみ) y=ランプ境界の急峻さ z=リムの頂点ブレンド w=予備
struct PBRConstants {
    DirectX::XMFLOAT4 RimParams;
    DirectX::XMFLOAT4 CameraPos;
    DirectX::XMFLOAT4 NprTuning;
    DirectX::XMFLOAT4 NprTuning2;
};

// 地形用: ベース地面 + 3種の木 + 雪 + 川 + カメラ位置
struct TerrainConstants {
    DirectX::XMFLOAT4 LayerColor[6]; // 0=地面, 1..3=木3種, 4=雪, 5=川
    DirectX::XMFLOAT4 CameraPos;
};

struct Mesh {
    std::string Name;
    /// Assimp マテリアル名（透明ルール `_tr` 判定など）
    std::string MaterialName;
    std::vector<Vertex> Vertices;
    std::vector<uint32_t> Indices;
    std::wstring DiffuseMap;
    std::wstring NormalMap;   
    std::wstring MetallicMap; 
    std::wstring RoughnessMap;
    /// 任意: NPR ランプ（空なら既定グラデ or assets/npr/default_ramp.png）
    std::wstring RampMap;
    /// NPR: セル影を頂点法線寄りにする係数（0..1）。-1=グローバル既定のみ（顔マテは Assimp で自動設定）
    float NprCelVertexBlendOverride = -1.f;
    std::vector<Bone> Bones;
    /// マテリアル不透明度（AI_MATKEY_OPACITY 等）
    float Opacity = 1.0f;
    /// マテリアル名 `_tr` または Opacity<1 に基づく「透明パス対象」フラグ（NPR タグ付きモデルのみ使用）
    bool NprTransparentByRule = false;
};

