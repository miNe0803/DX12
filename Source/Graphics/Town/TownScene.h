/*==============================================================================
   [TownScene.h]  SP31 の Unreal T3D 町シーンを DX12 へ移植した自己完結サブシステム。

   Downtown_West/Demo_Environment.t3d を解析し StaticMeshActor を配置、
   FBX を assimp で読み込み、独自ルートシグネチャ/PSO/シェーダ(TownVS/TownPS)で
   PBR 描画する。DX12 の cubemap IBL・CSM 影・PostProcess を再利用する。
==============================================================================*/
#pragma once

#include <d3dx12.h>
#include <DirectXMath.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "ComPtr.h"
#include "SharedStruct.h"   // Vertex

class DescriptorHeap;
struct DescriptorHandle;
class VertexBuffer;
class IndexBuffer;
class ConstantBuffer;
class PipelineState;
class Texture2D;
class ShadowSystem;

// 段階的サブセット読込・機能ゲートの設定
struct TownConfig
{
    std::string rootDir      = "assets\\town\\Downtown_West\\";
    uint32_t    maxActors    = 20000; // 配置する StaticMeshActor 上限
    uint32_t    maxUniqueMeshes = 500;// 読み込む FBX 種類の上限 ( 建物メッシュも含めるため拡大 )
    bool        enableLandscape = true;
    bool        enableRoads     = true;
    bool        enableDecals     = true;
    bool        enableLamps      = false;  // 昼シーン: 街灯オフ(UE5と一致)。点光源は無遮蔽で壁を透過し室内に光漏れするため昼は必ずオフ
    bool        enableGlass      = true;
    bool        enablePOM        = false;  // 重い（32回ループ）ため既定OFF。/POM で任意有効化
    float       cullMinScreenRatio = 0.004f; // 境界球半径/距離 がこれ未満なら描画しない（小物カリング）
    bool        cheapFoliage     = true;   // 植栽を軽量PSで描画（性能）
    float       foliageMaxDist   = 350.0f; // この距離を超える植栽は描かない（VRAM化後は余裕。0=無効）
    float       globalScale     = 0.01f;  // cm -> m ( SP31 GScale )。町のサイズ調整。
    bool        negX            = false;
    bool        negDepth        = true;
    // 町の配置（位置調整はここ）:
    bool        autoCenterToOrigin = true;         // 建物重心をワールド原点に合わせる
    DirectX::XMFLOAT3 worldOffset = { 0, 0, 0 };    // 追加の手動オフセット（ワールド単位, m）
};

// 平面反射パスのターゲット指定。Draw に渡すと「反射モード」で描画する
// （指定 RTV/DSV/ビューポートへ不透明インスタンスのみ、視錐台平面カリング無効）。
struct TownReflectionPass
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    D3D12_VIEWPORT              vp;
    D3D12_RECT                  scissor;
};

class TownScene
{
public:
    bool Init(ID3D12Device* device, DescriptorHeap* heap, const TownConfig& cfg);
    void Update();
    // メイン HDR パスへ描画。IBL/影リソースは Scene から受け取る ( 疎結合 )。
    // refl != nullptr で平面反射パス（ミラー化した sceneCBAddr を渡して呼ぶ）。
    void Draw(ID3D12GraphicsCommandList* cmd,
              D3D12_GPU_VIRTUAL_ADDRESS      sceneCBAddr,
              D3D12_GPU_DESCRIPTOR_HANDLE    iblTableBase,   // t6-t8 ( prefilter,irradiance,brdfLut )
              D3D12_GPU_DESCRIPTOR_HANDLE    csmSrv,         // t0 space2 ( CSM 配列 )
              D3D12_GPU_VIRTUAL_ADDRESS      shadowCBAddr,   // b1 space2
              const DirectX::XMMATRIX&       viewProj,       // CPU 視錐台カリング用 ( math, row-vector )
              const DirectX::XMFLOAT3&       camPos,         // 画面サイズカリング用
              const TownReflectionPass*      refl = nullptr);// 非nullで平面反射モード
    // CSM 影パス: 町インスタンスを光源視点で深度のみ描画（ShadowSystem の深度PSO/rootsig を流用）。
    // lightVP は math(row-vector: clip = worldPos * lightVP)。cascade 0 のみ呼ぶ想定。
    void DrawDepth(ID3D12GraphicsCommandList* cmd, const DirectX::XMMATRIX& lightVP, ShadowSystem* shadow,
                   const DirectX::XMFLOAT3& camPos, float cascadeFar);
    ~TownScene();

    size_t InstanceCount() const { return m_instances.size(); }
    size_t LoadedMeshCount() const { return m_loadedMeshes; }
    DirectX::XMFLOAT3 BoundsMin() const { return m_boundsMin; }
    DirectX::XMFLOAT3 BoundsMax() const { return m_boundsMax; }
    bool HasBuildingCenter() const { return m_buildingCount > 0; }
    DirectX::XMFLOAT3 BuildingCenter() const { return m_buildingCenter; }
    // 検証用: 最初の横断歩道デカールのワールド中心 ( G 適用済み )。無ければ false。
    bool FirstCrosswalkWorld(DirectX::XMFLOAT3& out) const;
    // 検証用: 最初の壁/awning 用水滴デカール ( UE Z>300cm ) のワールド中心。
    bool FirstDripWorld(DirectX::XMFLOAT3& out) const;

private:
    struct SubMesh
    {
        ComPtr<ID3D12Resource> vbRes, ibRes;   // DEFAULT ヒープ ( VRAM )
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW  ibv{};
        UINT indexCount = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE matTable{ 0 }; // t0-t4 の先頭 ( base,normal,MR,AO,height )
        bool glass = false;
    };
    struct TownModel
    {
        std::vector<SubMesh> subs;
        float radius = 0.0f;   // メッシュ原点からの最大頂点距離 ( ローカル cm )
        bool  hasGlass = false; // ガラスのサブメッシュを含むか
    };
    struct Instance
    {
        TownModel* model = nullptr;
        DirectX::XMFLOAT3 loc, rot, scl;
        DirectX::XMFLOAT4X4 worldT;          // XMMatrixTranspose(BuildLocal * G) ( シェーダ b0 用 )
        DirectX::XMFLOAT3 localPos;         // BuildLocal の平行移動 ( 距離カリング用 )
        DirectX::XMFLOAT3 worldCenter{ 0,0,0 }; // ワールド中心 ( 視錐台カリング用 )
        float worldRadius = 0.0f;           // ワールド境界球半径
        bool  isFoliage = false;            // 植栽（診断/カリング用）
        D3D12_GPU_DESCRIPTOR_HANDLE overrideTable{ 0 }; // slot0 上書き ( 無ければ .ptr==0 )
        bool overrideGlass = false;
        bool castShadow = true;
    };
    // テクスチャ解決結果 ( パス。空=フォールバック )
    struct SubTexPaths { std::string base, normal, mr, ao, height; };
    // /Engine/BasicShapes/Plane を使う道路/地面プレーン ( 共有 VB/IB のマテリアル別範囲 )
    struct RoadDraw { UINT indexOffset = 0, indexCount = 0; D3D12_GPU_DESCRIPTOR_HANDLE matTable{ 0 }; };
    // T3D Landscape アクターから生成する地形メッシュ ( 1 アクター = 1 メッシュ )
    struct LandMesh { ComPtr<ID3D12Resource> vbRes, ibRes; D3D12_VERTEX_BUFFER_VIEW vbv{}; D3D12_INDEX_BUFFER_VIEW ibv{}; UINT indexCount = 0; D3D12_GPU_DESCRIPTOR_HANDLE matTable{ 0 }; };
    // DecalActor: 地面/壁へ投影する RGBA クアッド ( 横断歩道/白線/ひび/汚れ/水たまり )
    struct DecalDraw { UINT indexOffset = 0, indexCount = 0; D3D12_GPU_DESCRIPTOR_HANDLE matTable{ 0 }; };
    // デファードデカール（ボックス投影, 深度からメッシュへ巻き付く。曲面 awning 等）
    struct DecalGpu { DirectX::XMFLOAT4X4 worldToBox; DirectX::XMFLOAT4 projAxisWorld; };
    struct DecalBox { D3D12_GPU_DESCRIPTOR_HANDLE matTable{ 0 }; };

    // --- パース / 読込 ---
    void ScanTextures(const std::string& dir);
    bool ParseT3D();
    TownModel* LoadModel(const std::string& fbxPath);
    void ResolveTextures(const std::string& matName, SubTexPaths& out);
    DirectX::XMMATRIX BuildLocal(const DirectX::XMFLOAT3& loc, const DirectX::XMFLOAT3& rot,
                                 const DirectX::XMFLOAT3& scl) const;
    // グローバル変換 G = Scale(globalScale) * Translation(m_worldOffset)。町全体の配置。
    DirectX::XMMATRIX GlobalG() const;
    void ApplyWorldOffset();  // 重心/手動オフセットを確定し全インスタンスへ反映
    // 5 連続記述子 ( base,normal,MR,AO,height ) を登録しテーブル先頭 GPU ハンドルを返す
    // baseFbKind: base が欠落時のフォールバック ( 0=white, 3=road grey )
    D3D12_GPU_DESCRIPTOR_HANDLE RegisterMaterialTable(const SubTexPaths& p, int baseFbKind = 0);
    D3D12_GPU_DESCRIPTOR_HANDLE RegisterFromPath(const std::string& path, int fallbackKind, bool first, D3D12_GPU_DESCRIPTOR_HANDLE& outBase);
    void AddRoadPlane(const DirectX::XMFLOAT3& loc, const DirectX::XMFLOAT3& rot, const DirectX::XMFLOAT3& scl, const SubTexPaths& mat);
    void DrawRoads(ID3D12GraphicsCommandList* cmd, uint32_t baseInstance);
    void LoadLandscapes();
    void DrawLandscapes(ID3D12GraphicsCommandList* cmd, uint32_t baseInstance);
    void AddDecal(const DirectX::XMFLOAT3& loc, const DirectX::XMFLOAT3& rot, const DirectX::XMFLOAT3& scl, const std::string& texPath);
    void DrawDecals(ID3D12GraphicsCommandList* cmd, uint32_t baseInstance);
    // デファードデカール（曲面/壁に巻き付く）: ボックスを構築し深度投影で描画。
    void AddDecalBox(const DirectX::XMFLOAT3& loc, const DirectX::XMFLOAT3& rot, const DirectX::XMFLOAT3& scl, const std::string& texPath);
    void DrawDecalsDeferred(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv);

    // --- GPU セットアップ ---
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CreateFallbackTextures();

    ID3D12Device*   m_device = nullptr;
    DescriptorHeap* m_heap = nullptr;
    TownConfig      m_cfg;

    ComPtr<ID3D12RootSignature> m_rootSig;
    PipelineState* m_psoOpaque = nullptr;
    PipelineState* m_psoGlass = nullptr;
    PipelineState* m_psoFoliage = nullptr;  // 植栽用の軽量PS
    PipelineState* m_psoDecal = nullptr;    // デカール用（アルファブレンド, 平面）
    PipelineState* m_psoDecalDeferred = nullptr; // デファードデカール（ボックス投影）

    ConstantBuffer* m_worldCB = nullptr;     // per-draw World リング
    ConstantBuffer* m_paramsCB = nullptr;    // TownParams (b2)
    ConstantBuffer* m_lightCB = nullptr;     // TownLights (b9)
    uint32_t m_worldSlot = 0;

    std::unordered_map<std::string, TownModel*> m_cache;   // fbxPath -> model ( null も保持 )
    std::unordered_map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> m_overrideCache; // matName -> slot0 上書きテーブル
    std::vector<Instance> m_instances;
    std::unordered_map<std::string, std::string> m_texIndex; // 小文字basename -> フルパス
    size_t m_loadedMeshes = 0;
    size_t m_missingMeshes = 0;

    // フォールバック 1x1 リソース ( 0=white,1=flatNormal,2=defaultMR,3=road grey )
    ComPtr<ID3D12Resource> m_fbWhite, m_fbNormal, m_fbMR, m_fbRoadGrey;

    // 道路/地面プレーン ( 共有 VB/IB )
    std::vector<Vertex>   m_roadVerts;
    std::vector<uint32_t> m_roadIdx;
    std::vector<RoadDraw> m_roadDraws;
    ComPtr<ID3D12Resource> m_roadVBRes, m_roadIBRes;
    D3D12_VERTEX_BUFFER_VIEW m_roadVbv{};
    D3D12_INDEX_BUFFER_VIEW  m_roadIbv{};

    // VRAM アップロード ( バッチ ): ステージング→DEFAULT へ 1 コマンドリストでコピー
    ComPtr<ID3D12CommandAllocator> m_upAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_upList;
    std::vector<ComPtr<ID3D12Resource>> m_upStaging;
    bool m_upOpen = false;
    ComPtr<ID3D12Resource> MakeGpuBuffer(const void* data, size_t size); // DEFAULT ヒープへ
    void FlushUploads();

    // 地形メッシュ
    std::vector<LandMesh> m_landscapes;

    // デカール（共有 VB/IB + マテリアル別範囲, 平面デカール）
    std::vector<Vertex>    m_decalVerts;
    std::vector<uint32_t>  m_decalIdx;
    std::vector<DecalDraw> m_decalDraws;
    ComPtr<ID3D12Resource> m_decalVBRes, m_decalIBRes;
    D3D12_VERTEX_BUFFER_VIEW m_decalVbv{};
    D3D12_INDEX_BUFFER_VIEW  m_decalIbv{};

    // デファードデカール（ボックス投影, 壁/曲面 awning 用）
    std::vector<DirectX::XMFLOAT4X4> m_decalBoxWorlds; // transpose(BW), 1/decal ( VS param0 )
    std::vector<DecalGpu>            m_decalGpu;        // worldToBox + projAxis ( PS param9 )
    std::vector<DecalBox>            m_decalBoxes;      // matTable / decal
    ComPtr<ID3D12Resource> m_decalBoxWorldRes, m_decalGpuRes; // 静的 StructuredBuffer
    ComPtr<ID3D12Resource> m_cubeVBRes, m_cubeIBRes;   // 共有ユニットキューブ
    D3D12_VERTEX_BUFFER_VIEW m_cubeVbv{};
    D3D12_INDEX_BUFFER_VIEW  m_cubeIbv{};
    DescriptorHandle* m_depthSrv = nullptr;            // シーン深度 R32_FLOAT SRV (t9)
    // C1: ガラス SSR 用。不透明シーンの HDR コピー（ガラス描画前に CopyResource）。
    // SRV は m_depthSrv の直後に登録し t10 として連続テーブルを作る。
    ComPtr<ID3D12Resource> m_sceneColorCopy;           // フル解像度 HDR コピー（反射ソース）
    DescriptorHandle* m_sceneCopySrv = nullptr;        // シーンカラーコピー SRV (t10)
    UINT m_screenW = 0, m_screenH = 0;

    // 街灯の点光源（ワールド位置）
    std::vector<DirectX::XMFLOAT3> m_lampWorld;

    DirectX::XMFLOAT3 m_boundsMin{ 0,0,0 }, m_boundsMax{ 0,0,0 };
    DirectX::XMFLOAT3 m_worldOffset{ 0,0,0 };     // 町全体の平行移動（配置）
    DirectX::XMFLOAT3 m_buildingCenter{ 0,0,0 };  // "building" メッシュのワールド重心
    DirectX::XMFLOAT3 m_buildingSum{ 0,0,0 };
    size_t m_buildingCount = 0;

    DirectX::XMFLOAT3 m_crosswalkLocal{ 0,0,0 };  // 最初の横断歩道デカール中心 ( our-local, pre-G )
    bool m_hasCrosswalk = false;
    DirectX::XMFLOAT3 m_dripLocal{ 0,0,0 };       // 最初の壁/awning 水滴デカール中心 ( our-local, pre-G )
    bool m_hasDrip = false;
};
