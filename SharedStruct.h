#pragma once
#include <d3dx12.h>
#include <DirectXMath.h>
#include <cstdint>
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
/// materialIndex: index into StructuredBuffer<GpuMaterialData> (bindless material lookup).
struct InstanceData {
    DirectX::XMFLOAT4X4 World;       // 64 bytes
    uint32_t materialIndex;            // 4 bytes — bindless material buffer index
    uint32_t _pad[3];                  // 12 bytes
};
static_assert(sizeof(InstanceData) == 80, "InstanceData must match HLSL StructuredBuffer stride");

/// Legacy NprPerMesh layout (kept for reference during transition, will be removed).
struct InstanceDataLegacy {
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4 NprPerMesh;
};
static_assert(sizeof(InstanceDataLegacy) == 80, "Legacy InstanceData stride");

/// Per-frame ring slices; keep in sync with Engine::FRAME_BUFFER_COUNT.
inline constexpr size_t kMaxPbrInstancesPerFrame = 16384u;
inline constexpr size_t kPbrInstanceRingFrameCount = 2u;

/// PBR path b0: View/Proj + world camera + Forward+ cluster info.
struct alignas(256) SceneConstants {
    DirectX::XMMATRIX View;
    DirectX::XMMATRIX Proj;
    DirectX::XMFLOAT4 CameraWorld;   // .xyz = world position, .w = time
    DirectX::XMFLOAT4 SunDirection;  // .xyz = normalised direction TO the light, .w = intensity
    DirectX::XMFLOAT4 SunColor;      // .rgb = colour, .a = unused
    DirectX::XMMATRIX InvViewProj;    // depth → world reconstruction (column-major for HLSL)
    // Forward+ cluster grid (Step 4)
    DirectX::XMFLOAT4 ClusterGridParams;  // x=tileCountX, y=tileCountY, z=nearZ, w=farZ
    DirectX::XMFLOAT4 ClusterSliceParams; // x=logScale, y=logBias, z=sliceCount, w=unused
    uint32_t lightBufferSRVIdx;
    uint32_t clusterDataSRVIdx;
    uint32_t lightIndexListSRVIdx;
    uint32_t activeLightCount;
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
// RimParams: x=未使用(1.0 固定) y=法線スケール z=リムべき w=リム加算強度
// NprTuning: x=virtualLight(透明) y=未使用 z=不透明clip閾値 w=影の環境色スケール
// NprTuning2: x=セル影の頂点法線ブレンド y=ランプ境界の急峻さ z=リムの頂点ブレンド w=NPRデバッグ(0–7: Editor 参照 5=t0生 6=t0×頂点 7=線形×頂点)
// NprDebugHdr: 予備（0 クリア、シェーダ未使用）
struct PBRConstants {
    DirectX::XMFLOAT4 RimParams;
    DirectX::XMFLOAT4 CameraPos;
    DirectX::XMFLOAT4 NprTuning;
    DirectX::XMFLOAT4 NprTuning2;
    DirectX::XMFLOAT4 NprDebugHdr;
};

// 地形用: ベース地面 + 3種の木 + 雪 + 川 + カメラ位置 + ライティング
struct TerrainConstants {
    DirectX::XMFLOAT4 LayerColor[6]; // 0=地面, 1..3=木3種, 4=雪, 5=川
    DirectX::XMFLOAT4 CameraPos;
    /// x:Terrain PS debug stage 0..3 / y:cheap path on (1/0) / z:grazing threshold / w:near preserve dist (m, 0=off)
    DirectX::XMFLOAT4 DebugParams;
    DirectX::XMFLOAT4 SunDirection;  // .xyz = normalised dir TO light, .w = intensity
    DirectX::XMFLOAT4 SunColor;      // .rgb
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
    /// FBX 等: 不透明度マップ（葉カットアウト等）。ClassifyTreePart で葉判定に使う。
    std::wstring OpacityMap;
    /// 任意: NPR ランプ（空なら既定グラデ or assets/npr/default_ramp.png）
    std::wstring RampMap;
    /// PMX/MMD スフィアマップ（加算 spa 等）。空なら白テクスチャ。
    std::wstring SphereMap;
    /// PMX sphereMode: 0=無効, 1=乗算(sph), 2=加算(spa), 3+=加算扱い（サブテクスチャは未対応）
    uint8_t SphereMode = 0;
    /// NPR: セル影を頂点法線寄りにする係数（0..1）。-1=グローバル既定のみ（顔マテは Assimp で自動設定）
    float NprCelVertexBlendOverride = -1.f;
    std::vector<Bone> Bones;
    /// マテリアル不透明度（AI_MATKEY_OPACITY 等）
    float Opacity = 1.0f;
    /// マテリアル名 `_tr` または Opacity<1 に基づく「透明パス対象」フラグ（NPR タグ付きモデルのみ使用）
    bool NprTransparentByRule = false;
};

