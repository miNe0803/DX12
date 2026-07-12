/*==============================================================================
   [TownScene.cpp]  SP31 Unreal T3D 町シーンの DX12 移植 ( Phase 1: 静的メッシュ )
==============================================================================*/
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>

#include "TownScene.h"
#include "Engine.h"
#include "DescriptorHeap.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "ConstantBuffer.h"
#include "PipelineState.h"
#include "Texture2D.h"
#include "SharedStruct.h"
#include "DebugLog.h"
#include "Graphics/ShadowSystem.h"

#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cfloat>
#include <utility>

using namespace DirectX;

namespace
{
    constexpr uint32_t kWorldSlotsPerFrame = 16384u;
    constexpr size_t   kWorldSlotBytes = 64u;   // StructuredBuffer<float4x4> の要素サイズ

    std::string Lower(std::string s)
    {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }
    std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    // "...'/Game/Downtown_West/Assets/foo/Name.Name'" -> "<rootDir>Assets\foo\Name"
    bool ResolveAssetBase(const std::string& line, const std::string& rootDir, std::string& outBase)
    {
        size_t q0 = line.find('\'');
        if (q0 == std::string::npos) return false;
        size_t q1 = line.find('\'', q0 + 1);
        if (q1 == std::string::npos) return false;
        std::string ref = line.substr(q0 + 1, q1 - q0 - 1);
        const std::string prefix = "/Game/Downtown_West/";
        size_t p = ref.find(prefix);
        if (p == std::string::npos) return false;
        std::string rel = ref.substr(p + prefix.size());
        size_t slash = rel.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "" : rel.substr(0, slash);
        std::string name = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        size_t dot = name.find('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        std::string path = rootDir;
        if (!dir.empty()) path += dir + "\\";
        path += name;
        for (auto& c : path) if (c == '/') c = '\\';
        outBase = path;
        return true;
    }

    std::string StripColorVariant(const std::string& mat)
    {
        size_t cp = Lower(mat).rfind("_color");
        if (cp == std::string::npos || cp + 6 >= mat.size()) return mat;
        for (size_t k = cp + 6; k < mat.size(); k++)
            if (!isdigit((unsigned char)mat[k])) return mat;
        return mat.substr(0, cp);
    }

    const char* const g_BaseSuffix[]   = { "_BaseColor","_EditorSphere_BaseColor","_Diffuse","_EditorSphere_Diffuse","_Albedo","_EditorSphere_Albedo","_albedo","_D" };
    const char* const g_NormSuffix[]   = { "_Normal","_EditorSphere_Normal","_N","_NormalMap","_normal" };
    const char* const g_MRSuffix[]     = { "_MetallicRoughness","_EditorSphere_MetallicRoughness","_RoughnessMetallic","_ORM","_MR","_orm" };
    // UE5 は結合 MR/ORM でなく単体ラフネスを出荷する。グレースケール _rough を MR スロットへ
    // 入れると G チャンネル=roughness になり TownPS がそのまま読める（metallic は PS 側で 0 固定）。
    const char* const g_RoughSuffix[]  = { "_rough","_Roughness","_roughness","_R","_Rough" };
    const char* const g_AOSuffix[]     = { "_Occlusion","_EditorSphere_Occlusion","_AO","_Occ","_ao" };
    const char* const g_HeightSuffix[] = { "_Height","_EditorSphere_Height","_height","_H","_Displacement","_disp" };

    bool PathUnderMaterials(const std::string& p)
    {
        std::string lp = Lower(p);
        return lp.find("\\materials\\") != std::string::npos;
    }
}

//=============================================================================
DirectX::XMMATRIX TownScene::BuildLocal(const XMFLOAT3& loc, const XMFLOAT3& rot, const XMFLOAT3& scl) const
{
    float P = XMConvertToRadians(rot.x);
    float Y = XMConvertToRadians(rot.y);
    float R = XMConvertToRadians(rot.z);
    float SP = sinf(P), CP = cosf(P);
    float SY = sinf(Y), CY = cosf(Y);
    float SR = sinf(R), CR = cosf(R);

    XMMATRIX Rue(
        CP * CY, CP * SY, SP, 0.0f,
        SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP, 0.0f,
        -(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    XMMATRIX Sue = XMMatrixScaling(scl.x, scl.y, scl.z);
    XMMATRIX Tue = XMMatrixTranslation(loc.x, loc.y, loc.z);
    XMMATRIX Lue = Sue * Rue * Tue;

    float sx = m_cfg.negX ? -1.0f : 1.0f;
    float sd = m_cfg.negDepth ? -1.0f : 1.0f;
    XMMATRIX C(
        sx,   0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, sd,   0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    XMMATRIX Cinv = XMMatrixTranspose(C);
    return Cinv * Lue * C;
}

//=============================================================================
DirectX::XMMATRIX TownScene::GlobalG() const
{
    return XMMatrixScaling(m_cfg.globalScale, m_cfg.globalScale, m_cfg.globalScale)
         * XMMatrixTranslation(m_worldOffset.x, m_worldOffset.y, m_worldOffset.z);
}

// 建物重心/手動オフセットから m_worldOffset を確定し、全インスタンスの
// worldT / worldCenter / 境界 / 重心をワールド空間へ再計算する。
void TownScene::ApplyWorldOffset()
{
    if (m_cfg.autoCenterToOrigin && m_buildingCount > 0)
        m_worldOffset = XMFLOAT3(-m_buildingCenter.x + m_cfg.worldOffset.x,
                                 -m_buildingCenter.y + m_cfg.worldOffset.y,
                                 -m_buildingCenter.z + m_cfg.worldOffset.z);
    else
        m_worldOffset = m_cfg.worldOffset;

    const float gs = m_cfg.globalScale;
    const XMMATRIX G = GlobalG();
    XMFLOAT3 mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (auto& inst : m_instances)
    {
        XMMATRIX W = BuildLocal(inst.loc, inst.rot, inst.scl) * G;
        XMStoreFloat4x4(&inst.worldT, XMMatrixTranspose(W));
        inst.worldCenter = XMFLOAT3(inst.localPos.x * gs + m_worldOffset.x,
                                    inst.localPos.y * gs + m_worldOffset.y,
                                    inst.localPos.z * gs + m_worldOffset.z);
        mn.x = std::min(mn.x, inst.worldCenter.x); mx.x = std::max(mx.x, inst.worldCenter.x);
        mn.y = std::min(mn.y, inst.worldCenter.y); mx.y = std::max(mx.y, inst.worldCenter.y);
        mn.z = std::min(mn.z, inst.worldCenter.z); mx.z = std::max(mx.z, inst.worldCenter.z);
    }
    if (!m_instances.empty()) { m_boundsMin = mn; m_boundsMax = mx; }
    m_buildingCenter = XMFLOAT3(m_buildingCenter.x + m_worldOffset.x,
                                m_buildingCenter.y + m_worldOffset.y,
                                m_buildingCenter.z + m_worldOffset.z);
    printf("[Town] world offset=(%.1f,%.1f,%.1f) buildingCenter->(%.1f,%.1f,%.1f)\n",
        m_worldOffset.x, m_worldOffset.y, m_worldOffset.z,
        m_buildingCenter.x, m_buildingCenter.y, m_buildingCenter.z);
    fflush(stdout);
}

//=============================================================================
void TownScene::ScanTextures(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    std::string pattern = dir + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ScanTextures(full);
        else
        {
            size_t dot = name.find_last_of('.');
            if (dot != std::string::npos && Lower(name.substr(dot)) == ".png")
                m_texIndex[Lower(name.substr(0, dot))] = full;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

//=============================================================================
void TownScene::ResolveTextures(const std::string& matName, SubTexPaths& out)
{
    auto find = [&](const std::string& m, const char* const* suf, int n) -> std::string {
        for (int i = 0; i < n; i++)
        {
            auto it = m_texIndex.find(Lower(m + suf[i]));
            if (it != m_texIndex.end()) return it->second;
        }
        return std::string();
    };
    auto tryAll = [&](const std::string& m) {
        if (out.base.empty())   out.base   = find(m, g_BaseSuffix,   _countof(g_BaseSuffix));
        if (out.normal.empty()) out.normal = find(m, g_NormSuffix,   _countof(g_NormSuffix));
        if (out.mr.empty())     out.mr     = find(m, g_MRSuffix,     _countof(g_MRSuffix));
        if (out.mr.empty())     out.mr     = find(m, g_RoughSuffix,  _countof(g_RoughSuffix)); // 単体rough→MRスロット(G=rough)
        if (out.ao.empty())     out.ao     = find(m, g_AOSuffix,     _countof(g_AOSuffix));
        if (out.height.empty()) out.height = find(m, g_HeightSuffix, _countof(g_HeightSuffix));
    };
    tryAll(matName);
    if (out.base.empty() || out.normal.empty() || out.mr.empty() || out.ao.empty() || out.height.empty())
    {
        std::string strip = StripColorVariant(matName);
        if (strip != matName) tryAll(strip);
    }

    // 縁石 / 地面のタイリング差し替え ( EditorSphere プレビューは誤り )
    std::string ln = Lower(matName);
    const std::string tx = m_cfg.rootDir + "Textures\\";
    if (ln.find("concrete_curb_a") != std::string::npos)
    {
        std::string cd = tx + "Concrete_Curb\\";
        out.base = cd + "T_curb_a_concrete_albedo.PNG";
        out.normal = cd + "T_curb_a_concrete_normal.PNG";
        out.mr = cd + "T_curb_a_concrete_orm.PNG";
        out.ao = cd + "T_curb_a_concrete_ao.PNG";
        out.height.clear();
    }
    else if (ln.find("concrete_curb_b") != std::string::npos)
    {
        std::string cd = tx + "Concrete_Curb\\";
        out.base = cd + "T_curb_straight_concrete_albedo.PNG";
        out.normal = cd + "T_curb_straight_concrete_normal.PNG";
        out.mr = cd + "T_curb_straight_concrete_rough.PNG";  // 単体rough→MRスロット
        out.ao = cd + "T_curb_straight_concrete_ao.PNG";
        out.height.clear();
    }
    else
    {
        std::string dir, pre;
        if      (ln.find("bricks_ground_a_herringbone") != std::string::npos) { dir = tx + "Bricks_Ground\\"; pre = "T_brick_ground_herringbone"; }
        else if (ln.find("bricks_ground_a_straight")    != std::string::npos) { dir = tx + "Bricks_Ground\\"; pre = "T_brick_ground_straight"; }
        else if (ln.find("bricks_ground_a")             != std::string::npos) { dir = tx + "Bricks_Ground\\"; pre = "T_brick_ground_straight"; }
        else if (ln.find("concrete_ground_tiles")       != std::string::npos) { dir = tx + "Concrete_Ground\\"; pre = "T_concrete_a"; }
        if (!pre.empty())
        {
            std::string b = dir + pre;
            out.base = b + "_albedo.PNG";
            out.normal = b + "_normal.PNG";
            out.ao = b + "_ao.PNG";
            out.height = b + "_height.PNG";
            out.mr = b + "_rough.PNG";   // 単体rough→MRスロット(G=rough)
        }
    }

    // Materials フォルダ ( EditorSphere ) の MR/AO はベイク破損なので既定へ
    if (!out.mr.empty() && PathUnderMaterials(out.mr)) out.mr.clear();
    if (!out.ao.empty() && PathUnderMaterials(out.ao)) out.ao.clear();
    if (!m_cfg.enablePOM) out.height.clear();
}

//=============================================================================
// フォールバック用 1x1 テクスチャ ( UMA CPU 書込み )
static ComPtr<ID3D12Resource> MakeSolid1x1(ID3D12Device* dev, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1);
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0);
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    uint8_t px[4] = { r, g, b, a };
    res->WriteToSubresource(0, nullptr, px, 4, 4);
    return res;
}

bool TownScene::CreateFallbackTextures()
{
    m_fbWhite    = MakeSolid1x1(m_device, 255, 255, 255, 255);
    m_fbNormal   = MakeSolid1x1(m_device, 128, 128, 255, 255); // 接空間 (0,0,1)
    m_fbMR       = MakeSolid1x1(m_device, 0, 204, 0, 255);     // G=rough0.8, B=metal0
    m_fbRoadGrey = MakeSolid1x1(m_device, 58, 56, 56, 255);    // アスファルト下地
    return m_fbWhite && m_fbNormal && m_fbMR && m_fbRoadGrey;
}

// path が空/無効ならフォールバックを、有効なら実テクスチャを 1 記述子登録。
// first==true のとき登録先の GPU ハンドルを outBase に格納。
D3D12_GPU_DESCRIPTOR_HANDLE TownScene::RegisterFromPath(const std::string& path, int fbKind, bool first, D3D12_GPU_DESCRIPTOR_HANDLE& outBase)
{
    DescriptorHandle* h = nullptr;
    Texture2D* tex = nullptr;
    if (!path.empty())
    {
        tex = Texture2D::Get(Widen(path));
        if (tex && !tex->IsValid()) tex = nullptr;
    }
    if (tex)
    {
        h = m_heap->Register(tex);
    }
    else
    {
        ID3D12Resource* fb = (fbKind == 1) ? m_fbNormal.Get() : (fbKind == 2) ? m_fbMR.Get()
            : (fbKind == 3) ? m_fbRoadGrey.Get() : m_fbWhite.Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        h = m_heap->RegisterResource(fb, d);
    }
    if (first && h) outBase = h->HandleGPU;
    return h ? h->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };
}

D3D12_GPU_DESCRIPTOR_HANDLE TownScene::RegisterMaterialTable(const SubTexPaths& p, int baseFbKind)
{
    D3D12_GPU_DESCRIPTOR_HANDLE base{ 0 };
    RegisterFromPath(p.base,   baseFbKind, true, base);  // t0 base   ( fallback white/road-grey )
    RegisterFromPath(p.normal, 1, false, base);  // t1 normal ( fallback flat )
    RegisterFromPath(p.mr,     2, false, base);  // t2 MR     ( fallback default )
    RegisterFromPath(p.ao,     0, false, base);  // t3 AO     ( fallback white )
    RegisterFromPath(p.height, 0, false, base);  // t4 height ( fallback white=平坦 )
    return base;
}

//=============================================================================
// デカール base テクスチャの兄弟マップ ( _normal / _rough ) を導出。既知の base/mask
// 接尾辞を剥がして newSfx を付け、実在（白fallbackでない）なら返す。無ければ空。
static std::string DeriveDecalSibling(const std::string& basePath, const char* newSfx)
{
    size_t dot = basePath.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? basePath : basePath.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? ".PNG"    : basePath.substr(dot);
    const char* sfx[] = { "_BaseColor","_RGB_mask","_albedo","_Albedo","_opacity","_mask","_D" };
    std::string low = Lower(stem);
    for (const char* x : sfx)
    {
        std::string lx = Lower(x);
        if (low.size() > lx.size() && low.compare(low.size() - lx.size(), lx.size(), lx) == 0)
        { stem = stem.substr(0, stem.size() - lx.size()); break; }
    }
    std::string p = stem + newSfx + ext;
    Texture2D* t = Texture2D::Get(Widen(p));
    if (t && t->IsValid() && t != Texture2D::GetWhite()) return p;
    return std::string();
}

//=============================================================================
// DecalActor: 投影クアッド ( UE-local Y/Z 平面、半径 256cm ) を our-local へ。
// テクスチャ無しは配置しない（白四角回避）。z-fighting 回避に法線方向へ +1.5cm。
void TownScene::AddDecal(const XMFLOAT3& loc, const XMFLOAT3& rot, const XMFLOAT3& scl, const std::string& texPath)
{
    Texture2D* tex = texPath.empty() ? nullptr : Texture2D::Get(Widen(texPath));
    // Texture2D::Get は読込失敗時に不透明白 4x4 (g_white) を返し IsValid() を通過するため、
    // それを弾く（弾かないと _BaseColor.PNG 未解決のデカールがベタ白の四角として描かれる）。
    if (!tex || !tex->IsValid() || tex == Texture2D::GetWhite()) return;

    // 高所（UE Z>150cm）のデカールは壁/awning など非平面に貼られる。平面クアッドでは
    // 曲面に沿わず浮くため、深度投影のデファードデカール経路へ回す（メッシュに巻き付く）。
    if (loc.z > 150.0f) { AddDecalBox(loc, rot, scl, texPath); return; }

    XMMATRIX M = BuildLocal(loc, rot, scl);
    const float H = 256.0f;
    uint32_t base = (uint32_t)m_decalVerts.size();
    XMVECTOR corner[4]; XMFLOAT2 uv[4]; int k = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
        {
            float ey = i ? H : -H;   // UE-local Y ( 幅 )
            float ez = j ? H : -H;   // UE-local Z ( 長さ )
            corner[k] = XMVector3TransformCoord(XMVectorSet(0.0f, ez, -ey, 1.0f), M);
            // UV は U↔V を入れ替える: 線テクスチャの帯は U 方向に走るので、U を「長さ軸」
            // (ez→UE-Z, 大きい scl.z) に、V を「幅軸」(ey→UE-Y, 小さい scl.y) に対応させる。
            // 従来は逆で、線が短く太く潰れていた（横断歩道も同じ規約で正しく縞が長辺へ並ぶ）。
            uv[k] = XMFLOAT2(j ? 1.0f : 0.0f, i ? 1.0f : 0.0f);
            k++;
        }
    XMVECTOR e1 = XMVectorSubtract(corner[1], corner[0]);
    XMVECTOR e2 = XMVectorSubtract(corner[2], corner[0]);
    XMVECTOR nrm = XMVector3Normalize(XMVector3Cross(e1, e2));
    if (XMVectorGetY(nrm) < 0.0f) nrm = XMVectorNegate(nrm);
    // 路面より確実に上へ ( our +Y )。向きに依らずリフト。深度バイアスと併用。
    XMVECTOR off = XMVectorSet(0.0f, 3.0f, 0.0f, 0.0f);
    for (int c = 0; c < 4; c++)
    {
        Vertex v{};
        XMStoreFloat3(&v.Position, XMVectorAdd(corner[c], off));
        XMStoreFloat3(&v.Normal, nrm);
        v.UV = uv[c]; v.Tangent = XMFLOAT3(0, 0, 0); v.Color = XMFLOAT4(1, 1, 1, 1);
        for (int b = 0; b < 4; b++) { v.BoneIndex[b] = 0; v.BoneWeight[b] = 0.0f; }
        m_decalVerts.push_back(v);
    }
    uint32_t o = (uint32_t)m_decalIdx.size();
    m_decalIdx.push_back(base + 0); m_decalIdx.push_back(base + 2); m_decalIdx.push_back(base + 1);
    m_decalIdx.push_back(base + 1); m_decalIdx.push_back(base + 2); m_decalIdx.push_back(base + 3);
    SubTexPaths mp; mp.base = texPath;
    // B5: デカールの兄弟法線/ラフネスマップを解決（横断歩道/白線/ひび/汚れは凹凸+光沢を持つ）。
    // 見つからなければ空のまま（RegisterMaterialTable が flat 法線 / rough=0.8 の fallback を割当）。
    mp.normal = DeriveDecalSibling(texPath, "_normal");
    mp.mr     = DeriveDecalSibling(texPath, "_rough");
    if (mp.mr.empty()) mp.mr = DeriveDecalSibling(texPath, "_roughness");
    if (mp.mr.empty()) mp.mr = DeriveDecalSibling(texPath, "_Roughness");
    DecalDraw dd; dd.indexOffset = o; dd.indexCount = 6; dd.matTable = RegisterMaterialTable(mp);
    m_decalDraws.push_back(dd);

    // 検証用: 最初の横断歩道デカール中心 ( our-local ) を記録。
    if (!m_hasCrosswalk)
    {
        std::string low = texPath;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("crosswalk") != std::string::npos)
        {
            XMVECTOR ctr = XMVectorScale(
                XMVectorAdd(XMVectorAdd(corner[0], corner[1]), XMVectorAdd(corner[2], corner[3])), 0.25f);
            XMStoreFloat3(&m_crosswalkLocal, ctr);
            m_hasCrosswalk = true;
        }
    }
    // 検証用: 最初の「壁/awning 用」水滴デカール ( UE Z>300cm ) 中心を記録。
    if (!m_hasDrip)
    {
        std::string low = texPath;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("drip") != std::string::npos && loc.z > 300.0f)
        {
            XMVECTOR ctr = XMVectorScale(
                XMVectorAdd(XMVectorAdd(corner[0], corner[1]), XMVectorAdd(corner[2], corner[3])), 0.25f);
            XMStoreFloat3(&m_dripLocal, ctr);
            m_hasDrip = true;
        }
    }
}

bool TownScene::FirstCrosswalkWorld(XMFLOAT3& out) const
{
    if (!m_hasCrosswalk) return false;
    XMVECTOR w = XMVector3TransformCoord(XMLoadFloat3(&m_crosswalkLocal), GlobalG());
    XMStoreFloat3(&out, w);
    return true;
}

bool TownScene::FirstDripWorld(XMFLOAT3& out) const
{
    if (!m_hasDrip) return false;
    XMVECTOR w = XMVector3TransformCoord(XMLoadFloat3(&m_dripLocal), GlobalG());
    XMStoreFloat3(&out, w);
    return true;
}

//=============================================================================
// デファードデカール: 投影ボックス（単位キューブを DecalSize×scale へ）を構築。
// 深度からワールド座標を復元しこのボックス内に入る面へ投影するため、平面クアッドと
// 違い曲面（円塔/ドーム屋根）にも巻き付く。局所 X が投影/法線軸。
void TownScene::AddDecalBox(const XMFLOAT3& loc, const XMFLOAT3& rot, const XMFLOAT3& scl, const std::string& texPath)
{
    const float H = 256.0f;          // UE 既定 DecalSize の面内半径 (Y,Z)
    const float projDepth = 300.0f;  // 投影方向(X)の半深度。受け面をまたぐよう十分に。
    XMMATRIX M    = BuildLocal(loc, rot, scl);
    XMMATRIX Sbox = XMMatrixScaling(projDepth, H, H);   // 局所 X=投影/法線軸, Y/Z=footprint
    XMMATRIX BW   = Sbox * M * GlobalG();                // 単位キューブ[-1,1]^3 → world
    XMMATRIX WB   = XMMatrixInverse(nullptr, BW);        // world → box 局所

    XMFLOAT4X4 bwT, wbT;
    XMStoreFloat4x4(&bwT, XMMatrixTranspose(BW));        // VS 用（StructuredBuffer は列優先で読む）
    XMStoreFloat4x4(&wbT, XMMatrixTranspose(WB));
    XMVECTOR ax = XMVector3Normalize(BW.r[0]);           // 局所 +X 軸の world 方向（受け面法線の向き）

    DecalGpu g; g.worldToBox = wbT;
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&g.projAxisWorld), ax);
    g.projAxisWorld.w = 0.0f;

    m_decalBoxWorlds.push_back(bwT);
    m_decalGpu.push_back(g);
    SubTexPaths mp; mp.base = texPath;
    m_decalBoxes.push_back({ RegisterMaterialTable(mp) });

    // 検証用: 最初の「drip」ボックスデカール中心 ( our-local, pre-G ) を記録（DX12_LOOK_DRIPS 用）。
    if (!m_hasDrip)
    {
        std::string low = texPath;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("drip") != std::string::npos)
        {
            XMStoreFloat3(&m_dripLocal, M.r[3]);
            m_hasDrip = true;
        }
    }
}

//=============================================================================
// UE の Plane ( ±50, +Z 上 ) を our-local ( cm ) の 2x2 クアッドとして蓄積。
void TownScene::AddRoadPlane(const XMFLOAT3& loc, const XMFLOAT3& rot, const XMFLOAT3& scl, const SubTexPaths& mat)
{
    XMMATRIX M = BuildLocal(loc, rot, scl);
    const float TILE = 400.0f;
    float uMax = fabsf(scl.x) * 100.0f / TILE;
    float vMax = fabsf(scl.y) * 100.0f / TILE;

    uint32_t base = (uint32_t)m_roadVerts.size();
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
        {
            float cx = i ? 50.0f : -50.0f;
            float cy = j ? 50.0f : -50.0f;
            XMVECTOR p = XMVector3TransformCoord(XMVectorSet(cx, 0.0f, -cy, 1.0f), M);
            XMVECTOR nv = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), M));
            Vertex v{};
            XMStoreFloat3(&v.Position, p);
            XMStoreFloat3(&v.Normal, nv);
            v.UV = XMFLOAT2(i * uMax, j * vMax);
            v.Tangent = XMFLOAT3(0, 0, 0);
            v.Color = XMFLOAT4(1, 1, 1, 1);
            for (int b = 0; b < 4; b++) { v.BoneIndex[b] = 0; v.BoneWeight[b] = 0.0f; }
            m_roadVerts.push_back(v);
        }
    uint32_t o = (uint32_t)m_roadIdx.size();
    m_roadIdx.push_back(base + 0); m_roadIdx.push_back(base + 2); m_roadIdx.push_back(base + 1);
    m_roadIdx.push_back(base + 1); m_roadIdx.push_back(base + 2); m_roadIdx.push_back(base + 3);

    RoadDraw rd;
    rd.indexOffset = o;
    rd.indexCount = 6;
    rd.matTable = RegisterMaterialTable(mat, /*road grey base*/3);
    m_roadDraws.push_back(rd);
}

//=============================================================================
// 道路/地面プレーン描画。頂点は our-local なので World = transpose(G)。
void TownScene::DrawRoads(ID3D12GraphicsCommandList* cmd, uint32_t baseInstance)
{
    if (!m_cfg.enableRoads || !m_roadVBRes || !m_roadIBRes || m_roadDraws.empty()) return;
    cmd->SetGraphicsRoot32BitConstant(8, baseInstance, 0);   // g_Worlds[baseInstance] = G
    cmd->IASetVertexBuffers(0, 1, &m_roadVbv);
    cmd->IASetIndexBuffer(&m_roadIbv);
    for (const RoadDraw& rd : m_roadDraws)
    {
        if (rd.matTable.ptr) cmd->SetGraphicsRootDescriptorTable(3, rd.matTable);
        cmd->DrawIndexedInstanced(rd.indexCount, 1, rd.indexOffset, 0, 0);
    }
}

//=============================================================================
// デカール描画（TownVS 流用 World=G, 専用 PS + アルファブレンド）。
void TownScene::DrawDecals(ID3D12GraphicsCommandList* cmd, uint32_t baseInstance)
{
    if (!m_cfg.enableDecals || !m_psoDecal || !m_psoDecal->IsValid()
        || !m_decalVBRes || !m_decalIBRes || m_decalDraws.empty()) return;
    cmd->SetPipelineState(m_psoDecal->Get());
    cmd->SetGraphicsRootConstantBufferView(2, m_paramsCB->GetAddress() + 512); // デカール領域
    cmd->SetGraphicsRoot32BitConstant(8, baseInstance, 0);   // World = G
    cmd->IASetVertexBuffers(0, 1, &m_decalVbv);
    cmd->IASetIndexBuffer(&m_decalIbv);
    D3D12_GPU_DESCRIPTOR_HANDLE lastTable{ 0 };
    for (const DecalDraw& d : m_decalDraws)
    {
        if (d.matTable.ptr && d.matTable.ptr != lastTable.ptr)
        {
            cmd->SetGraphicsRootDescriptorTable(3, d.matTable);
            lastTable = d.matTable;
        }
        cmd->DrawIndexedInstanced(d.indexCount, 1, d.indexOffset, 0, 0);
    }
}

//=============================================================================
// デファードデカール描画（ボックス投影, 深度からメッシュへ巻き付く）。
// 深度を SRV としてバインドするため DSV を外し、深度テストは無効（範囲判定はシェーダ内）。
// 終了時に深度を DEPTH_WRITE へ戻し RTV+DSV を再バインド（後続のガラスパス用）。
void TownScene::DrawDecalsDeferred(ID3D12GraphicsCommandList* cmd,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    if (!m_cfg.enableDecals || !m_psoDecalDeferred || !m_psoDecalDeferred->IsValid()
        || m_decalBoxes.empty() || !m_depthSrv || !m_decalBoxWorldRes || !m_decalGpuRes
        || !m_cubeVBRes || !m_cubeIBRes) return;
    { char e[8]; if (GetEnvironmentVariableA("DX12_NO_DDECAL", e, sizeof(e)) > 0) return; } // 診断用スキップ

    ID3D12Resource* depthRes = g_Engine->GetDepthStencilResource();

    // 深度: DEPTH_WRITE -> PIXEL_SHADER_RESOURCE、DSV を外す（read/write ハザード回避）
    auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &toSrv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);   // HDR RTV, DSV=null

    cmd->SetPipelineState(m_psoDecalDeferred->Get());
    cmd->SetGraphicsRootShaderResourceView(0, m_decalBoxWorldRes->GetGPUVirtualAddress()); // param0: box world (VS)
    cmd->SetGraphicsRootConstantBufferView(2, m_paramsCB->GetAddress() + 512);             // param2: tint + 1/screen
    cmd->SetGraphicsRootShaderResourceView(9, m_decalGpuRes->GetGPUVirtualAddress());       // param9: worldToBox+projAxis (PS)
    cmd->SetGraphicsRootDescriptorTable(10, m_depthSrv->HandleGPU);                         // param10: 深度 SRV
    // param1(Scene)/4(IBL)/5(CSM)/6(ShadowCB) は不透明パスで既にバインド済み、そのまま有効。
    cmd->IASetVertexBuffers(0, 1, &m_cubeVbv);
    cmd->IASetIndexBuffer(&m_cubeIbv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_GPU_DESCRIPTOR_HANDLE cur{ 0 };
    for (UINT i = 0; i < (UINT)m_decalBoxes.size(); ++i)
    {
        const DecalBox& d = m_decalBoxes[i];
        if (d.matTable.ptr && d.matTable.ptr != cur.ptr)
        {
            cmd->SetGraphicsRootDescriptorTable(3, d.matTable);
            cur = d.matTable;
        }
        cmd->SetGraphicsRoot32BitConstant(8, i, 0);   // gBaseInstance = デカール index
        cmd->DrawIndexedInstanced(36, 1, 0, 0, 0);
    }

    // 深度を戻し、RTV+DSV を再バインド（後続ガラスパスが深度テストできるように）
    auto toDsv = CD3DX12_RESOURCE_BARRIER::Transition(
        depthRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &toDsv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // param0 をインスタンス World バッファへ戻す（ガラスパスが m_worldCB を参照するため）
    cmd->SetGraphicsRootShaderResourceView(0, m_worldCB->GetAddress());
}

//=============================================================================
// CSM 影パス: 町インスタンスを光源視点で深度のみ描画。ShadowSystem の深度PSO/rootsig
// （POSITION のみ読む非インスタンス VS、root CBV b0 = WorldLightVP）を流用する。
// 呼び出し側（Scene.cpp のカスケードループ）が事前に PSO/rootsig/topology を設定済み。
// lightVP は math(row-vector): clip = worldPos * lightVP。worldT は TownVS と同じく
// そのまま world として使う（mul(pos, worldT) = worldPos）。
void TownScene::DrawDepth(ID3D12GraphicsCommandList* cmd, const XMMATRIX& lightVP, ShadowSystem* shadow,
                          const XMFLOAT3& camPos, float cascadeFar)
{
    if (m_instances.empty() || !shadow) return;

    // BUG2 修正: キャスタを「カメラからの距離」でカリング（回転不変）。従来は回転する光源視錐台で
    // カリングしていたため、同じ位置でもカメラを回すと縁のビルが視錐台を出入りして影が点滅した。
    // 太陽が低い(23°)と影が長く、画面外のビルの長い影も画面に入るので far に余裕(+40m)を持たせ、
    // かつ 360° 全周のキャスタを含める（視錐台カリングと違い回転で集合が変わらない）。
    const float shellFar = cascadeFar + 60.0f;   // casterMargin(ShadowSystem) と一致。背の高い長影キャスタを距離カリングで落とさない

    // BUG1: 影PSOは CULL_NONE（ShadowSystem）なので、片面ガラス/窓枠/看板も全て深度を書き遮蔽する。
    // 呼び出し側(Scene.cpp)が GetShadowPSO() をバインド済み。ここでの PSO 切替は不要。
    uint32_t drawn = 0;
    const uint32_t kMaxCasters = 8000;   // 純粋な安全網（実キャスタ < これ）
    for (const Instance& inst : m_instances)
    {
        if (!inst.castShadow || inst.isFoliage) continue;       // 地面/植栽は落とさない
        if (!inst.model || inst.model->subs.empty()) continue;

        // カメラからの距離でシェルカリング（回転不変＝カメラ回転で影が点滅しない）
        float dx = inst.worldCenter.x - camPos.x;
        float dy = inst.worldCenter.y - camPos.y;
        float dz = inst.worldCenter.z - camPos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist - inst.worldRadius > shellFar) continue;

        // worldT は transpose(BuildLocal*G)（VS の StructuredBuffer 用）。WritePerDrawCB は
        // 内部で再度 transpose するため、ここでは「math のワールド行列」(BuildLocal*G)=transpose(worldT)
        // を渡す必要がある（ECS が tc.WorldMatrix を渡すのと同じ）。転置漏れが cascade0 の
        // 破綻＝カメラ中心の暗い円の原因だった。
        XMMATRIX world = XMMatrixTranspose(XMLoadFloat4x4(&inst.worldT)); // = BuildLocal*G (math)
        XMMATRIX wlvp = world * lightVP;
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = shadow->WritePerDrawCB(wlvp);
        if (cbAddr == 0) break;                                  // リング枯渇
        cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        for (const SubMesh& s : inst.model->subs)
        {
            if (!s.vbRes || !s.ibRes || s.indexCount == 0) continue;
            cmd->IASetVertexBuffers(0, 1, &s.vbv);
            cmd->IASetIndexBuffer(&s.ibv);
            cmd->DrawIndexedInstanced(s.indexCount, 1, 0, 0, 0);
        }
        if (++drawn >= kMaxCasters) break;
    }
}

//=============================================================================
// VSM V3c: GPU駆動ページ描画用のキャスタレコードを構築（load時1回）。
// DrawDepth と同一条件でキャスタを収集し、96B レコード（transpose(world)+worldCenter/半径+modelId）を
// DEFAULT バッファへ。modelId はユニーク model へ密採番（scatter の per-model グループ化用）。
// worldCenter/worldRadius は既にスケール込みで算出済み（レビュー H1: ローカル半径を使わない）。
void TownScene::BuildCasterRecords()
{
    struct CasterRec { XMFLOAT4X4 world; XMFLOAT4 centerRadius; uint32_t meta[4]; };
    struct SubmeshGeo { uint32_t indexCount, startIndex, baseVertex, modelId; };
    static_assert(sizeof(CasterRec) == 96, "CasterRec must match VsmBinning.hlsli Caster (96B)");
    static_assert(sizeof(SubmeshGeo) == 16, "SubmeshGeo must be 16B");

    // ユニーク（キャスト）モデルへ密採番。CasterRecords と 描画バッチ/SubmeshGeoTable で共有。
    std::unordered_map<TownModel*, uint32_t> modelId;
    std::vector<TownModel*> uniqueModels;
    std::vector<CasterRec> recs;
    recs.reserve(m_instances.size());
    for (const Instance& inst : m_instances)
    {
        if (!inst.castShadow || inst.isFoliage) continue;         // DrawDepth と同条件
        if (!inst.model || inst.model->subs.empty()) continue;
        auto it = modelId.find(inst.model);
        uint32_t mid;
        if (it == modelId.end()) { mid = (uint32_t)uniqueModels.size(); modelId.emplace(inst.model, mid); uniqueModels.push_back(inst.model); }
        else mid = it->second;
        CasterRec r;
        r.world = inst.worldT;   // 既に transpose(BuildLocal*G)（描画 VS 用）
        r.centerRadius = XMFLOAT4(inst.worldCenter.x, inst.worldCenter.y, inst.worldCenter.z, inst.worldRadius);
        r.meta[0] = mid; r.meta[1] = 0; r.meta[2] = 0; r.meta[3] = 0;
        recs.push_back(r);
    }
    m_casterCount = (uint32_t)recs.size();
    m_casterModelCount = (uint32_t)uniqueModels.size();
    if (!recs.empty())
        m_casterRecRes = MakeGpuBuffer(recs.data(), recs.size() * sizeof(CasterRec));

    // m3 描画バッチ + SubmeshGeoTable（modelId 順に各モデルの全 submesh。startIndex/baseVertex=0=submesh毎VB）。
    std::vector<SubmeshGeo> geos;
    m_vsmBatches.clear();
    for (uint32_t mid = 0; mid < uniqueModels.size(); ++mid)
    {
        for (const SubMesh& s : uniqueModels[mid]->subs)
        {
            if (!s.vbRes || !s.ibRes || s.indexCount == 0) continue;
            VsmBatch b; b.vbv = s.vbv; b.ibv = s.ibv; b.indexCount = s.indexCount; b.modelId = mid;
            m_vsmBatches.push_back(b);
            SubmeshGeo g{ s.indexCount, 0u, 0u, mid };
            geos.push_back(g);
        }
    }
    if (!geos.empty())
        m_submeshTableRes = MakeGpuBuffer(geos.data(), geos.size() * sizeof(SubmeshGeo));

    printf("[VSM] caster records: %u casters, %u unique models, %zu draw batches (%.2f MB recs)\n",
        m_casterCount, m_casterModelCount, m_vsmBatches.size(), recs.size() * sizeof(CasterRec) / (1024.0 * 1024.0));
    fflush(stdout);
}

//=============================================================================
// T3D Landscape アクター ( CollisionHeightData ) から地形グリッドメッシュを生成。
void TownScene::LoadLandscapes()
{
    std::string t3d = m_cfg.rootDir + "Demo_Environment.t3d";
    std::ifstream fin(t3d.c_str());
    if (!fin.good()) return;

    SubTexPaths gp;
    std::string gl = m_cfg.rootDir + "Textures\\Ground_Landscape\\";
    gp.base = gl + "T_ground_tile_grass_alive_a_albedo.PNG";
    gp.normal = gl + "T_ground_tile_grass_alive_a_normal.PNG";
    D3D12_GPU_DESCRIPTOR_HANDLE grassTable = RegisterMaterialTable(gp);

    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    struct PendComp { int secX, secY, N; std::vector<unsigned short> h; };
    std::vector<PendComp> comps;
    bool inLandscape = false, haveXform = false, inRoot = false;
    XMFLOAT3 relLoc(0, 0, 0), relScale(1, 1, 1);
    int curSecX = 0, curSecY = 0;
    XMFLOAT3 vmin(FLT_MAX, FLT_MAX, FLT_MAX), vmax(-FLT_MAX, -FLT_MAX, -FLT_MAX); // 地形の our-local 境界

    auto AddComponent = [&](int secX, int secY, const unsigned short* h, int N)
    {
        if (N < 2) return;
        uint32_t base = (uint32_t)verts.size();
        const float ZSCALE = 1.0f / 128.0f;
        const float uvTile = 0.002f;
        for (int j = 0; j < N; j++) for (int i = 0; i < N; i++)
        {
            float localZ = ((float)h[j * N + i] - 32768.0f) * ZSCALE;
            float wx = relLoc.x + relScale.x * (float)(secX + i);
            float wy = relLoc.y + relScale.y * (float)(secY + j);
            float wz = relLoc.z + relScale.z * localZ;
            Vertex v{};
            v.Position = XMFLOAT3(wx, wz, -wy);   // UE(X,Y,Z) -> our(X, Z, -Y)（建物と同じ基底）
            vmin.x = std::min(vmin.x, wx); vmax.x = std::max(vmax.x, wx);
            vmin.y = std::min(vmin.y, wz); vmax.y = std::max(vmax.y, wz);
            vmin.z = std::min(vmin.z, -wy); vmax.z = std::max(vmax.z, -wy);
            v.Normal = XMFLOAT3(0, 0, 0);
            v.UV = XMFLOAT2(wx * uvTile, -wy * uvTile);
            v.Tangent = XMFLOAT3(0, 0, 0); v.Color = XMFLOAT4(1, 1, 1, 1);
            for (int b = 0; b < 4; b++) { v.BoneIndex[b] = 0; v.BoneWeight[b] = 0.0f; }
            verts.push_back(v);
        }
        for (int j = 0; j < N - 1; j++) for (int i = 0; i < N - 1; i++)
        {
            uint32_t v00 = base + j * N + i, v10 = base + j * N + (i + 1);
            uint32_t v01 = base + (j + 1) * N + i, v11 = base + (j + 1) * N + (i + 1);
            idx.push_back(v00); idx.push_back(v01); idx.push_back(v10);
            idx.push_back(v10); idx.push_back(v01); idx.push_back(v11);
        }
    };
    auto FlushActor = [&]()
    {
        if (haveXform) for (const PendComp& c : comps) AddComponent(c.secX, c.secY, c.h.data(), c.N);
        comps.clear();
        if (verts.empty() || idx.empty()) { verts.clear(); idx.clear(); return; }
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
        {
            uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
            XMVECTOR pa = XMLoadFloat3(&verts[a].Position), pb = XMLoadFloat3(&verts[b].Position), pc = XMLoadFloat3(&verts[c].Position);
            XMVECTOR n = XMVector3Cross(XMVectorSubtract(pb, pa), XMVectorSubtract(pc, pa));
            XMFLOAT3 nf; XMStoreFloat3(&nf, n);
            verts[a].Normal.x += nf.x; verts[a].Normal.y += nf.y; verts[a].Normal.z += nf.z;
            verts[b].Normal.x += nf.x; verts[b].Normal.y += nf.y; verts[b].Normal.z += nf.z;
            verts[c].Normal.x += nf.x; verts[c].Normal.y += nf.y; verts[c].Normal.z += nf.z;
        }
        for (auto& v : verts)
        {
            XMVECTOR n = XMLoadFloat3(&v.Normal);
            if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-12f) n = XMVectorSet(0, 1, 0, 0);
            XMStoreFloat3(&v.Normal, XMVector3Normalize(n));
        }
        LandMesh lm;
        lm.indexCount = (UINT)idx.size();
        lm.matTable = grassTable;
        const size_t vb = sizeof(Vertex) * verts.size();
        const size_t ib = sizeof(uint32_t) * idx.size();
        lm.vbRes = MakeGpuBuffer(verts.data(), vb);
        lm.ibRes = MakeGpuBuffer(idx.data(), ib);
        if (lm.vbRes && lm.ibRes)
        {
            lm.vbv.BufferLocation = lm.vbRes->GetGPUVirtualAddress();
            lm.vbv.SizeInBytes = (UINT)vb; lm.vbv.StrideInBytes = sizeof(Vertex);
            lm.ibv.BufferLocation = lm.ibRes->GetGPUVirtualAddress();
            lm.ibv.SizeInBytes = (UINT)ib; lm.ibv.Format = DXGI_FORMAT_R32_UINT;
            m_landscapes.push_back(lm);
        }
        verts.clear(); idx.clear();
    };

    std::vector<unsigned short> heights;
    std::string line;
    while (std::getline(fin, line))
    {
        if (line.find("Begin Actor Class=/Script/Landscape.Landscape ") != std::string::npos)
        {
            FlushActor(); inLandscape = true; haveXform = false; inRoot = false;
            relLoc = XMFLOAT3(0, 0, 0); relScale = XMFLOAT3(1, 1, 1); continue;
        }
        if (!inLandscape) continue;
        if (line.find("End Actor") != std::string::npos) { FlushActor(); inLandscape = false; continue; }
        if (line.find("Name=\"RootComponent0\"") != std::string::npos) inRoot = true;
        if (inRoot && !haveXform)
        {
            size_t pl = line.find("RelativeLocation=(X=");
            if (pl != std::string::npos) sscanf(line.c_str() + pl, "RelativeLocation=(X=%f,Y=%f,Z=%f)", &relLoc.x, &relLoc.y, &relLoc.z);
            size_t ps = line.find("RelativeScale3D=(X=");
            if (ps != std::string::npos) { sscanf(line.c_str() + ps, "RelativeScale3D=(X=%f,Y=%f,Z=%f)", &relScale.x, &relScale.y, &relScale.z); haveXform = true; }
        }
        if (inRoot && line.find("End Object") != std::string::npos) inRoot = false;
        { size_t p = line.find("SectionBaseX="); if (p != std::string::npos) curSecX = atoi(line.c_str() + p + 13);
          p = line.find("SectionBaseY="); if (p != std::string::npos) curSecY = atoi(line.c_str() + p + 13); }
        size_t hp = line.find("CollisionHeightData");
        if (hp != std::string::npos)
        {
            heights.clear();
            const char* s = line.c_str() + hp + 19; char* end = nullptr;
            for (;;) { long val = strtol(s, &end, 10); if (end == s) break; heights.push_back((unsigned short)val); s = end; }
            int N = (int)(sqrtf((float)heights.size()) + 0.5f);
            if (N >= 2 && (size_t)N * N == heights.size())
            {
                PendComp c; c.secX = curSecX; c.secY = curSecY; c.N = N; c.h.swap(heights);
                comps.push_back(std::move(c));
            }
        }
    }
    FlushActor();
    // our-local(cm) 境界 → ワールド(= *globalScale + worldOffset)
    const float gs = m_cfg.globalScale;
    printf("[Town] landscapes: %zu meshes | world bounds (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
        m_landscapes.size(),
        vmin.x * gs + m_worldOffset.x, vmin.y * gs + m_worldOffset.y, vmin.z * gs + m_worldOffset.z,
        vmax.x * gs + m_worldOffset.x, vmax.y * gs + m_worldOffset.y, vmax.z * gs + m_worldOffset.z);
    fflush(stdout);
}

//=============================================================================
void TownScene::DrawLandscapes(ID3D12GraphicsCommandList* cmd, uint32_t baseInstance)
{
    if (!m_cfg.enableLandscape || m_landscapes.empty()) return;
    cmd->SetGraphicsRoot32BitConstant(8, baseInstance, 0);   // g_Worlds[baseInstance] = G
    for (const LandMesh& lm : m_landscapes)
    {
        if (!lm.vbRes || !lm.ibRes) continue;
        if (lm.matTable.ptr) cmd->SetGraphicsRootDescriptorTable(3, lm.matTable);
        cmd->IASetVertexBuffers(0, 1, &lm.vbv); cmd->IASetIndexBuffer(&lm.ibv);
        cmd->DrawIndexedInstanced(lm.indexCount, 1, 0, 0, 0);
    }
}

//=============================================================================
// データを DEFAULT ヒープ ( VRAM ) のバッファへ。コピーは 1 本のコマンドリストに
// 蓄積し、FlushUploads() でまとめて実行する（毎フレームの PCIe 読みを解消）。
ComPtr<ID3D12Resource> TownScene::MakeGpuBuffer(const void* data, size_t size)
{
    if (!data || size == 0) return nullptr;
    if (!m_upOpen)
    {
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_upAlloc)))) return nullptr;
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_upAlloc.Get(), nullptr, IID_PPV_ARGS(&m_upList)))) return nullptr;
        m_upOpen = true;
    }
    ComPtr<ID3D12Resource> def, up;
    auto rd = CD3DX12_RESOURCE_DESC::Buffer(size);
    auto hpD = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(m_device->CreateCommittedResource(&hpD, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&def)))) return nullptr;
    auto hpU = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(m_device->CreateCommittedResource(&hpU, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) return nullptr;
    void* p = nullptr;
    if (SUCCEEDED(up->Map(0, nullptr, &p))) { std::memcpy(p, data, size); up->Unmap(0, nullptr); }
    m_upList->CopyBufferRegion(def.Get(), 0, up.Get(), 0, size);
    auto bar = CD3DX12_RESOURCE_BARRIER::Transition(def.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    m_upList->ResourceBarrier(1, &bar);
    m_upStaging.push_back(up);
    return def;
}

void TownScene::FlushUploads()
{
    if (!m_upOpen) return;
    m_upList->Close();
    ID3D12CommandList* lists[] = { m_upList.Get() };
    g_Engine->Queue()->ExecuteCommandLists(1, lists);
    g_Engine->WaitForGpuIdle();
    m_upStaging.clear();
    m_upList.Reset();
    m_upAlloc.Reset();
    m_upOpen = false;
    printf("[Town] VRAM upload flushed\n"); fflush(stdout);
}

//=============================================================================
TownScene::TownModel* TownScene::LoadModel(const std::string& fbxPath)
{
    unsigned int flags = (aiProcessPreset_TargetRealtime_MaxQuality | aiProcess_ConvertToLeftHanded)
        & ~aiProcess_RemoveRedundantMaterials & ~aiProcess_OptimizeMeshes;
    const aiScene* scene = aiImportFile(fbxPath.c_str(), flags);
    if (!scene) return nullptr;

    TownModel* model = new TownModel();
    model->subs.reserve(scene->mNumMeshes);

    for (unsigned int m = 0; m < scene->mNumMeshes; m++)
    {
        const aiMesh* mesh = scene->mMeshes[m];
        const bool hasUV = mesh->HasTextureCoords(0);
        const bool hasN  = mesh->HasNormals();

        std::vector<Vertex> verts(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; v++)
        {
            Vertex& vt = verts[v];
            vt.Position = XMFLOAT3(mesh->mVertices[v].x, -mesh->mVertices[v].z, mesh->mVertices[v].y);
            { float r2 = vt.Position.x * vt.Position.x + vt.Position.y * vt.Position.y + vt.Position.z * vt.Position.z;
              if (r2 > model->radius) model->radius = r2; }
            vt.Normal   = hasN ? XMFLOAT3(mesh->mNormals[v].x, -mesh->mNormals[v].z, mesh->mNormals[v].y)
                               : XMFLOAT3(0.0f, 1.0f, 0.0f);
            vt.UV       = hasUV ? XMFLOAT2(mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y)
                                : XMFLOAT2(0.0f, 0.0f);
            vt.Tangent  = XMFLOAT3(0, 0, 0);   // PS は微分から TBN を組むので不要
            vt.Color    = XMFLOAT4(1, 1, 1, 1);
            for (int b = 0; b < 4; b++) { vt.BoneIndex[b] = 0; vt.BoneWeight[b] = 0.0f; }
        }

        std::vector<uint32_t> idx(mesh->mNumFaces * 3);
        for (unsigned int f = 0; f < mesh->mNumFaces; f++)
        {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices == 3)
            {
                idx[f * 3 + 0] = face.mIndices[0];
                idx[f * 3 + 1] = face.mIndices[1];
                idx[f * 3 + 2] = face.mIndices[2];
            }
            else { idx[f * 3 + 0] = idx[f * 3 + 1] = idx[f * 3 + 2] = 0; }
        }
        if (verts.empty() || idx.empty()) continue;

        SubMesh sub;
        const size_t vbytes = sizeof(Vertex) * verts.size();
        const size_t ibytes = sizeof(uint32_t) * idx.size();
        sub.vbRes = MakeGpuBuffer(verts.data(), vbytes);
        sub.ibRes = MakeGpuBuffer(idx.data(), ibytes);
        if (!sub.vbRes || !sub.ibRes) continue;
        sub.vbv.BufferLocation = sub.vbRes->GetGPUVirtualAddress();
        sub.vbv.SizeInBytes = (UINT)vbytes;
        sub.vbv.StrideInBytes = sizeof(Vertex);
        sub.ibv.BufferLocation = sub.ibRes->GetGPUVirtualAddress();
        sub.ibv.SizeInBytes = (UINT)ibytes;
        sub.ibv.Format = DXGI_FORMAT_R32_UINT;
        sub.indexCount = (UINT)idx.size();

        // マテリアル名 ( MI_ 以降 ) からテクスチャ解決
        aiString mn;
        scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, mn);
        std::string name = mn.C_Str();
        size_t pmi = name.find("MI_");
        if (pmi != std::string::npos) name = name.substr(pmi);
        SubTexPaths paths;
        ResolveTextures(name, paths);
        sub.matTable = RegisterMaterialTable(paths);
        sub.glass = Lower(name).find("glass") != std::string::npos;
        if (sub.glass) model->hasGlass = true;
        model->subs.push_back(sub);
    }

    aiReleaseImport(scene);
    model->radius = sqrtf(model->radius);  // 累積した最大 r^2 を半径へ
    return model;
}

//=============================================================================
bool TownScene::ParseT3D()
{
    std::string t3d = m_cfg.rootDir + "Demo_Environment.t3d";
    std::ifstream fin(t3d.c_str());
    if (!fin.good())
    {
        printf("[Town] failed to open %s\n", t3d.c_str());
        fflush(stdout);
        return false;
    }

    std::string curMesh, curMatBase, curDecalPath;
    bool hasMesh = false, hasLoc = false, curIsPlane = false, hasDecal = false, sawRotation = false;
    float lx = 0, ly = 0, lz = 0, rp = 0, ryaw = 0, rr = 0, sx = 1, sy = 1, sz = 1;
    XMFLOAT3 mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    std::string line;
    while (std::getline(fin, line))
    {
        if (line.find("StaticMesh=") != std::string::npos &&
            line.find("StaticMeshComponent") == std::string::npos)
        {
            std::string base;
            if (ResolveAssetBase(line, m_cfg.rootDir, base)) { curMesh = base + ".FBX"; hasMesh = true; }
            else if (line.find("BasicShapes/Plane") != std::string::npos) curIsPlane = true;
        }
        else if (line.find("OverrideMaterials(0)=") != std::string::npos)
        {
            std::string base;
            if (ResolveAssetBase(line, m_cfg.rootDir, base)) curMatBase = base;
        }
        else if (line.find("DecalMaterial=") != std::string::npos)
        {
            // DecalActor: MI_Decal_* -> その _BaseColor.PNG ( RGBA, alpha=マスク )
            std::string base;
            if (ResolveAssetBase(line, m_cfg.rootDir, base)) { curDecalPath = base + "_BaseColor.PNG"; hasDecal = true; }
        }
        else if (line.find("RelativeLocation=") != std::string::npos)
        {
            size_t p = line.find("(X=");
            if (p != std::string::npos && sscanf(line.c_str() + p, "(X=%f,Y=%f,Z=%f)", &lx, &ly, &lz) == 3)
                hasLoc = true;
        }
        else if (line.find("RelativeRotation=") != std::string::npos)
        {
            size_t p = line.find("(Pitch=");
            if (p != std::string::npos && sscanf(line.c_str() + p, "(Pitch=%f,Yaw=%f,Roll=%f)", &rp, &ryaw, &rr) == 3)
                sawRotation = true;
        }
        else if (line.find("RelativeScale3D=") != std::string::npos)
        {
            size_t p = line.find("(X=");
            if (p != std::string::npos) sscanf(line.c_str() + p, "(X=%f,Y=%f,Z=%f)", &sx, &sy, &sz);
        }
        else if (line.find("End Object") != std::string::npos)
        {
            if (hasMesh && hasLoc)
            {
                TownModel* model = nullptr;
                auto it = m_cache.find(curMesh);
                if (it != m_cache.end()) model = it->second;
                else if (m_loadedMeshes < m_cfg.maxUniqueMeshes)
                {
                    model = LoadModel(curMesh);
                    m_cache[curMesh] = model;
                    if (model) m_loadedMeshes++; else m_missingMeshes++;
                }

                if (model)
                {
                    Instance inst;
                    inst.model = model;
                    inst.loc = XMFLOAT3(lx, ly, lz);
                    inst.rot = XMFLOAT3(rp, ryaw, rr);
                    inst.scl = XMFLOAT3(sx, sy, sz);

                    // slot0 上書きマテリアル ( matName でキャッシュし記述子の爆発を防ぐ )
                    if (!curMatBase.empty())
                    {
                        size_t sl = curMatBase.find_last_of('\\');
                        std::string matName = (sl == std::string::npos) ? curMatBase : curMatBase.substr(sl + 1);
                        auto oc = m_overrideCache.find(matName);
                        if (oc != m_overrideCache.end()) inst.overrideTable = oc->second;
                        else
                        {
                            SubTexPaths op;
                            ResolveTextures(matName, op);
                            inst.overrideTable = RegisterMaterialTable(op);
                            m_overrideCache[matName] = inst.overrideTable;
                        }
                        inst.overrideGlass = Lower(matName).find("glass") != std::string::npos;
                    }

                    XMMATRIX world = BuildLocal(inst.loc, inst.rot, inst.scl);
                    XMVECTOR tpos = world.r[3];
                    XMStoreFloat3(&inst.localPos, tpos);
                    std::string lm = Lower(curMesh);
                    inst.castShadow = !(lm.find("ground_mod") != std::string::npos ||
                        lm.find("ground_stone") != std::string::npos ||
                        lm.find("ground_tile") != std::string::npos);

                    // world = BuildLocal * G ( G = Scale(globalScale) )
                    XMMATRIX G = XMMatrixScaling(m_cfg.globalScale, m_cfg.globalScale, m_cfg.globalScale);
                    XMMATRIX Wworld = world * G;
                    XMStoreFloat4x4(&inst.worldT, XMMatrixTranspose(Wworld));

                    // ワールド境界球 ( 視錐台カリング用 )
                    XMFLOAT3 wp(inst.localPos.x * m_cfg.globalScale, inst.localPos.y * m_cfg.globalScale, inst.localPos.z * m_cfg.globalScale);
                    inst.worldCenter = wp;
                    float maxAbsScl = std::max(std::max(fabsf(sx), fabsf(sy)), fabsf(sz));
                    inst.worldRadius = model->radius * maxAbsScl * m_cfg.globalScale;
                    if (inst.worldRadius < 0.5f) inst.worldRadius = 0.5f;
                    mn.x = std::min(mn.x, wp.x); mx.x = std::max(mx.x, wp.x);
                    mn.y = std::min(mn.y, wp.y); mx.y = std::max(mx.y, wp.y);
                    mn.z = std::min(mn.z, wp.z); mx.z = std::max(mx.z, wp.z);

                    // 建物メッシュの重心 ( カメラ初期注視点用 )
                    std::string lmName = Lower(curMesh);
                    if (lmName.find("building") != std::string::npos)
                    {
                        m_buildingSum.x += wp.x; m_buildingSum.y += wp.y; m_buildingSum.z += wp.z;
                        m_buildingCount++;
                    }
                    // 植栽判定（診断用）
                    inst.isFoliage = (lmName.find("flower") != std::string::npos || lmName.find("shrub") != std::string::npos ||
                        lmName.find("foliage") != std::string::npos || lmName.find("tree") != std::string::npos ||
                        lmName.find("grass") != std::string::npos || lmName.find("leaves") != std::string::npos ||
                        lmName.find("plant") != std::string::npos || lmName.find("bush") != std::string::npos ||
                        lmName.find("ivy") != std::string::npos || lmName.find("hedge") != std::string::npos);

                    // 街灯/ランプ器具 → 点光源の配置位置（電球位置へ +4m）
                    if (lmName.find("tarppost") != std::string::npos || lmName.find("lamps_tree_light") != std::string::npos ||
                        lmName.find("lightpost") != std::string::npos || lmName.find("light_post") != std::string::npos ||
                        lmName.find("streetlight") != std::string::npos)
                    {
                        if (m_lampWorld.size() < 64) m_lampWorld.push_back(XMFLOAT3(wp.x, wp.y + 4.0f, wp.z));
                    }

                    m_instances.push_back(inst);
                }
            }
            else if (curIsPlane && hasLoc && m_cfg.enableRoads)
            {
                // UE 組み込み Plane を道路/地面として生成 ( マテリアルは OverrideMaterials(0) )
                SubTexPaths mp;
                if (!curMatBase.empty())
                {
                    size_t sl = curMatBase.find_last_of('\\');
                    std::string matName = (sl == std::string::npos) ? curMatBase : curMatBase.substr(sl + 1);
                    ResolveTextures(matName, mp);
                }
                AddRoadPlane(XMFLOAT3(lx, ly, lz), XMFLOAT3(rp, ryaw, rr), XMFLOAT3(sx, sy, sz), mp);
            }
            else if (hasDecal && hasLoc && m_cfg.enableDecals)
            {
                // UE DecalActor の archetype 既定は投影軸(-X)を真下へ向ける（≈Pitch=-90）。
                // T3D が RelativeRotation を省略したデカールはこの既定を継承する（＝差分だけ
                // シリアライズされるため）。省略時に Pitch=-90 を補い、路面へ「平ら」に寝かせる
                // （補わないと水平投影＝垂直クアッド＝カーテン状に立ってしまう）。
                // 明示回転（Yaw/Roll 含む）はそのまま尊重。既定補完はデカール枝のみ；
                // StaticMeshActor / 道路プレーンは従来どおり identity 既定。
                XMFLOAT3 decalRot = sawRotation ? XMFLOAT3(rp, ryaw, rr) : XMFLOAT3(-90.0f, 0.0f, 0.0f);
                AddDecal(XMFLOAT3(lx, ly, lz), decalRot, XMFLOAT3(sx, sy, sz), curDecalPath);
            }
            hasMesh = hasLoc = false;
            curIsPlane = false;
            hasDecal = false;
            curMesh.clear();
            curMatBase.clear();
            curDecalPath.clear();
            rp = ryaw = rr = 0; sx = sy = sz = 1; sawRotation = false;

            if (m_instances.size() >= m_cfg.maxActors) break;
        }
    }

    if (!m_instances.empty()) { m_boundsMin = mn; m_boundsMax = mx; }
    if (m_buildingCount > 0)
    {
        m_buildingCenter = XMFLOAT3(m_buildingSum.x / m_buildingCount, m_buildingSum.y / m_buildingCount, m_buildingSum.z / m_buildingCount);
        printf("[Town] building instances=%zu center=(%.1f,%.1f,%.1f)\n", m_buildingCount,
            m_buildingCenter.x, m_buildingCenter.y, m_buildingCenter.z);
    }

    // 道路/地面プレーンを VRAM バッファ化
    if (!m_roadVerts.empty() && !m_roadIdx.empty())
    {
        const size_t vb = sizeof(Vertex) * m_roadVerts.size();
        const size_t ib = sizeof(uint32_t) * m_roadIdx.size();
        m_roadVBRes = MakeGpuBuffer(m_roadVerts.data(), vb);
        m_roadIBRes = MakeGpuBuffer(m_roadIdx.data(), ib);
        if (m_roadVBRes && m_roadIBRes)
        {
            m_roadVbv.BufferLocation = m_roadVBRes->GetGPUVirtualAddress();
            m_roadVbv.SizeInBytes = (UINT)vb; m_roadVbv.StrideInBytes = sizeof(Vertex);
            m_roadIbv.BufferLocation = m_roadIBRes->GetGPUVirtualAddress();
            m_roadIbv.SizeInBytes = (UINT)ib; m_roadIbv.Format = DXGI_FORMAT_R32_UINT;
        }
    }

    // デカールを VRAM バッファ化
    if (!m_decalVerts.empty() && !m_decalIdx.empty())
    {
        const size_t vb = sizeof(Vertex) * m_decalVerts.size();
        const size_t ib = sizeof(uint32_t) * m_decalIdx.size();
        m_decalVBRes = MakeGpuBuffer(m_decalVerts.data(), vb);
        m_decalIBRes = MakeGpuBuffer(m_decalIdx.data(), ib);
        if (m_decalVBRes && m_decalIBRes)
        {
            m_decalVbv.BufferLocation = m_decalVBRes->GetGPUVirtualAddress();
            m_decalVbv.SizeInBytes = (UINT)vb; m_decalVbv.StrideInBytes = sizeof(Vertex);
            m_decalIbv.BufferLocation = m_decalIBRes->GetGPUVirtualAddress();
            m_decalIbv.SizeInBytes = (UINT)ib; m_decalIbv.Format = DXGI_FORMAT_R32_UINT;
        }
    }
    // デファードデカール: 共有ユニットキューブ + per-decal 静的 StructuredBuffer
    if (!m_decalBoxes.empty())
    {
        Vertex cv[8]{};
        const XMFLOAT3 pos[8] = {
            {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
            {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
        };
        for (int i = 0; i < 8; i++) { cv[i].Position = pos[i]; cv[i].Color = XMFLOAT4(1, 1, 1, 1); }
        const uint32_t ci[36] = {
            0,1,2, 0,2,3,   4,6,5, 4,7,6,
            0,4,5, 0,5,1,   3,2,6, 3,6,7,
            0,3,7, 0,7,4,   1,5,6, 1,6,2,
        };
        m_cubeVBRes = MakeGpuBuffer(cv, sizeof(cv));
        m_cubeIBRes = MakeGpuBuffer(ci, sizeof(ci));
        if (m_cubeVBRes && m_cubeIBRes)
        {
            m_cubeVbv = { m_cubeVBRes->GetGPUVirtualAddress(), (UINT)sizeof(cv), sizeof(Vertex) };
            m_cubeIbv = { m_cubeIBRes->GetGPUVirtualAddress(), (UINT)sizeof(ci), DXGI_FORMAT_R32_UINT };
        }
        m_decalBoxWorldRes = MakeGpuBuffer(m_decalBoxWorlds.data(), m_decalBoxWorlds.size() * sizeof(XMFLOAT4X4));
        m_decalGpuRes = MakeGpuBuffer(m_decalGpu.data(), m_decalGpu.size() * sizeof(DecalGpu));
    }

    printf("[Town] decals: %zu flat draws, %zu boxes(deferred), %zu verts\n",
        m_decalDraws.size(), m_decalBoxes.size(), m_decalVerts.size());
    printf("[Town] roads: %zu draws, %zu verts\n", m_roadDraws.size(), m_roadVerts.size());
    return true;
}

//=============================================================================
bool TownScene::CreateRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE rangeMat, rangeIBL, rangeCsm, rangeDepth;
    rangeMat.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0); // t0-t4 space0
    rangeIBL.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 6, 0); // t6-t8 space0
    rangeCsm.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2); // t0 space2
    rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 9, 0); // t9=シーン深度, t10=シーンカラーコピー(ガラスSSR)

    CD3DX12_ROOT_PARAMETER params[11];
    params[0].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_VERTEX); // t0 space1 InstanceWorlds (StructuredBuffer)
    params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);    // b1 Scene
    params[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);  // b2 TownParams
    params[3].InitAsDescriptorTable(1, &rangeMat, D3D12_SHADER_VISIBILITY_PIXEL);
    params[4].InitAsDescriptorTable(1, &rangeIBL, D3D12_SHADER_VISIBILITY_PIXEL);
    params[5].InitAsDescriptorTable(1, &rangeCsm, D3D12_SHADER_VISIBILITY_PIXEL);
    params[6].InitAsConstantBufferView(1, 2, D3D12_SHADER_VISIBILITY_PIXEL);  // b1 space2 ShadowCB
    params[7].InitAsConstantBufferView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL);  // b9 TownLights
    params[8].InitAsConstants(2, 0, 1, D3D12_SHADER_VISIBILITY_ALL);         // b0 space1: baseInstance + windEnable ( VS+PS: デファードPSも参照 )
    params[9].InitAsShaderResourceView(1, 1, D3D12_SHADER_VISIBILITY_PIXEL);  // t1 space1 デファードデカール per-decal データ
    params[10].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_PIXEL); // t9 space0 シーン深度

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].RegisterSpace = 2;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, _countof(samplers), samplers,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    {
        if (err) DebugLog("[Town] RootSig serialize error: %s\n", (const char*)err->GetBufferPointer());
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
        return false;
    return true;
}

bool TownScene::CreatePipelines()
{
    m_psoOpaque = new PipelineState();
    m_psoOpaque->SetInputLayout(Vertex::InputLayout);
    m_psoOpaque->SetRootSignature(m_rootSig.Get());
    m_psoOpaque->SetVS(L"TownVS.cso");
    m_psoOpaque->SetPS(L"TownPS.cso");
    m_psoOpaque->SetCullMode(D3D12_CULL_MODE_NONE);
    m_psoOpaque->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ALL);
    m_psoOpaque->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    m_psoOpaque->SetNumRenderTargets(1);
    m_psoOpaque->SetRenderTargetFormat(DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_psoOpaque->Create();
    if (!m_psoOpaque->IsValid()) return false;

    // ガラス: 深度テストのみ ( 書込み無し ) + アルファブレンド
    m_psoGlass = new PipelineState();
    m_psoGlass->SetInputLayout(Vertex::InputLayout);
    m_psoGlass->SetRootSignature(m_rootSig.Get());
    m_psoGlass->SetVS(L"TownVS.cso");
    m_psoGlass->SetPS(L"TownPS.cso");
    m_psoGlass->SetCullMode(D3D12_CULL_MODE_NONE);
    m_psoGlass->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ZERO);
    m_psoGlass->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    m_psoGlass->SetNumRenderTargets(1);
    m_psoGlass->SetRenderTargetFormat(DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_psoGlass->SetAlphaBlendPremultiplied();
    m_psoGlass->Create();
    if (!m_psoGlass->IsValid()) return false;

    // 植栽: 軽量PS（アルファテスト不透明。深度書込みON、両面）
    m_psoFoliage = new PipelineState();
    m_psoFoliage->SetInputLayout(Vertex::InputLayout);
    m_psoFoliage->SetRootSignature(m_rootSig.Get());
    m_psoFoliage->SetVS(L"TownVS.cso");
    m_psoFoliage->SetPS(L"TownFoliagePS.cso");
    m_psoFoliage->SetCullMode(D3D12_CULL_MODE_NONE);
    m_psoFoliage->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ALL);
    m_psoFoliage->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    m_psoFoliage->SetNumRenderTargets(1);
    m_psoFoliage->SetRenderTargetFormat(DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_psoFoliage->Create();
    if (!m_psoFoliage->IsValid()) return false;

    // デカール: TownVS + 専用PS、アルファブレンド、深度テストのみ（書込み無し）、両面
    m_psoDecal = new PipelineState();
    m_psoDecal->SetInputLayout(Vertex::InputLayout);
    m_psoDecal->SetRootSignature(m_rootSig.Get());
    m_psoDecal->SetVS(L"TownVS.cso");
    m_psoDecal->SetPS(L"TownDecalPS.cso");
    m_psoDecal->SetCullMode(D3D12_CULL_MODE_NONE);
    m_psoDecal->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ZERO);
    m_psoDecal->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    m_psoDecal->SetDepthBias(-50000, -3.0f, 0.0f);   // カメラ側へ寄せて路面との Z ファイティング回避
    m_psoDecal->SetNumRenderTargets(1);
    m_psoDecal->SetRenderTargetFormat(DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_psoDecal->SetAlphaBlendPremultiplied();
    m_psoDecal->Create();
    if (!m_psoDecal->IsValid()) return false;

    // デファードデカール: ユニットキューブ投影。深度は SRV バインドで DSV 未使用のため
    // 深度テスト無効（範囲判定はシェーダ内）。カメラがボックス内でも描けるよう前面カリング。
    m_psoDecalDeferred = new PipelineState();
    m_psoDecalDeferred->SetInputLayout(Vertex::InputLayout);
    m_psoDecalDeferred->SetRootSignature(m_rootSig.Get());
    m_psoDecalDeferred->SetVS(L"TownDeferredDecalVS.cso");
    m_psoDecalDeferred->SetPS(L"TownDeferredDecalPS.cso");
    m_psoDecalDeferred->SetCullMode(D3D12_CULL_MODE_FRONT);   // 背面を描く（カメラがボックス内でも可視）
    m_psoDecalDeferred->SetDepthEnable(false);                // DSV 未バインド → 深度テスト無効
    m_psoDecalDeferred->SetNumRenderTargets(1);
    m_psoDecalDeferred->SetRenderTargetFormat(DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_psoDecalDeferred->SetAlphaBlendPremultiplied();
    m_psoDecalDeferred->Create();
    return m_psoDecalDeferred->IsValid();
}

//=============================================================================
bool TownScene::Init(ID3D12Device* device, DescriptorHeap* heap, const TownConfig& cfg)
{
    m_device = device;
    m_heap = heap;
    m_cfg = cfg;
    { char ev[8]; if (GetEnvironmentVariableA("DX12_POM_OFF", ev, sizeof(ev)) > 0) m_cfg.enablePOM = false; }

    if (!CreateFallbackTextures()) { printf("[Town] fallback textures failed\n"); return false; }
    if (!CreateRootSignature())    { printf("[Town] root signature failed\n"); return false; }
    if (!CreatePipelines())        { printf("[Town] pipeline failed ( TownVS/TownPS .cso ? )\n"); return false; }

    // ×2: メインパスと平面反射パスが同一フレーム内で別領域を使う（ワールドCBリングのエイリアス回避）
    m_worldCB = new ConstantBuffer(kWorldSlotBytes * kWorldSlotsPerFrame * Engine::FRAME_BUFFER_COUNT * 2);
    m_paramsCB = new ConstantBuffer(768); // 3 領域: 0=不透明, 256=ガラス, 512=デカール
    m_lightCB = new ConstantBuffer(sizeof(XMFLOAT4) * (1 + 64 + 64));
    if (!m_worldCB->IsValid() || !m_paramsCB->IsValid() || !m_lightCB->IsValid()) return false;

    // 点光源 ( Phase 1: 0 灯 )
    if (auto* lp = m_lightCB->GetPtr<XMFLOAT4>()) lp[0] = XMFLOAT4(0, 0, 0, 0);

    ScanTextures(m_cfg.rootDir.substr(0, m_cfg.rootDir.size() - 1)); // 末尾 '\\' を除く
    printf("[Town] texture index: %zu png\n", m_texIndex.size());
    if (!ParseT3D()) return false;
    ApplyWorldOffset();   // 町を原点付近へ配置（worldT/境界を再計算）
    // GPU インスタンシング: 同一 (model, slot0上書き) を連続させ 1 ドローに束ねられるよう整列
    std::sort(m_instances.begin(), m_instances.end(), [](const Instance& a, const Instance& b) {
        if (a.model != b.model) return a.model < b.model;
        return a.overrideTable.ptr < b.overrideTable.ptr;
    });
    if (m_cfg.enableLandscape) LoadLandscapes();
    BuildCasterRecords();   // VSM V3c: 静的キャスタレコード（FlushUploads 前に MakeGpuBuffer へ蓄積）
    FlushUploads();   // 蓄積した VRAM コピーをまとめて実行

    // デファードデカール/ガラスSSR 用: シーン深度(R32_TYPELESS リソース)の R32_FLOAT SRV を
    // 共有ヒープへ 1 度だけ登録。深度リソースは Engine が scene init 前に作成し再作成しない。
    // 続けて（連続する）シーンカラーコピーの SRV を登録し、t9=深度 / t10=カラー の 2枚テーブルにする。
    if ((!m_decalBoxes.empty() || m_cfg.enableGlass) && g_Engine && g_Engine->GetDepthStencilResource())
    {
        ID3D12Resource* depthRes = g_Engine->GetDepthStencilResource();
        D3D12_RESOURCE_DESC dr = depthRes->GetDesc();
        m_screenW = (UINT)dr.Width; m_screenH = (UINT)dr.Height;

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        m_depthSrv = m_heap->RegisterResource(depthRes, sd);  // t9

        // C1: 不透明シーンの HDR コピー（ガラス反射のソース）。SsrSystem の m_hdrCopy と同様、
        // 生成時は PIXEL_SHADER_RESOURCE 状態。ガラス描画前に CopyResource で毎フレーム更新する。
        if (m_cfg.enableGlass && m_screenW && m_screenH)
        {
            auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, m_screenW, m_screenH,
                1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE);
            if (SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_sceneColorCopy))))
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC cd = {};
                cd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                cd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                cd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                cd.Texture2D.MipLevels = 1;
                m_sceneCopySrv = m_heap->RegisterResource(m_sceneColorCopy.Get(), cd);  // t10（m_depthSrv の直後＝連続）
            }
        }
    }

    printf("[Town] Init done. instances=%zu meshes=%zu missing=%zu bounds=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) descriptors=%u\n",
        m_instances.size(), m_loadedMeshes, m_missingMeshes, m_boundsMin.x, m_boundsMin.y, m_boundsMin.z,
        m_boundsMax.x, m_boundsMax.y, m_boundsMax.z, m_heap->AllocatedCount());
    fflush(stdout);
    return true;
}

void TownScene::Update() {}

//=============================================================================
void TownScene::Draw(ID3D12GraphicsCommandList* cmd,
    D3D12_GPU_VIRTUAL_ADDRESS sceneCBAddr,
    D3D12_GPU_DESCRIPTOR_HANDLE iblTableBase,
    D3D12_GPU_DESCRIPTOR_HANDLE csmSrv,
    D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddr,
    const XMMATRIX& viewProj,
    const XMFLOAT3& camPos,
    const TownReflectionPass* refl)
{
    if (m_instances.empty() || !m_psoOpaque || !m_psoOpaque->IsValid()) return;

    // 視錐台 6 平面を viewProj ( row-vector: clip = v*M ) から抽出し正規化。
    XMFLOAT4X4 m; XMStoreFloat4x4(&m, viewProj);
    XMFLOAT4 planes[6] = {
        { m._11 + m._14, m._21 + m._24, m._31 + m._34, m._41 + m._44 }, // left
        { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 }, // right
        { m._12 + m._14, m._22 + m._24, m._32 + m._34, m._42 + m._44 }, // bottom
        { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 }, // top
        { m._13,         m._23,         m._33,         m._43         }, // near (z>=0)
        { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 }, // far
    };
    for (auto& p : planes)
    {
        float inv = 1.0f / std::max(sqrtf(p.x * p.x + p.y * p.y + p.z * p.z), 1e-6f);
        p.x *= inv; p.y *= inv; p.z *= inv; p.w *= inv;
    }

    // TownParams 更新（0=不透明領域, 256=ガラス領域）
    if (auto* pp = m_paramsCB->GetPtr<XMFLOAT4>())
    {
        const XMFLOAT4 params2(1.0f, m_cfg.enablePOM ? 0.03f : 0.0f, 1.35f, 3.0f); // iblDiffuse, height, ambientBoost, sunScale
        pp[0]  = XMFLOAT4(0.0f, 0.4f, 1.0f, 1.0f);  // 不透明: glass=0, iblReflect, normalStr, flipG
        pp[1]  = params2;
        pp[16] = XMFLOAT4(1.0f, 0.4f, 1.0f, 1.0f);  // ガラス: glass=1 ( +256B )
        pp[17] = params2;
        pp[32] = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);  // デカール: tint.rgb=明るさ, a=不透明度 ( +512B )
        D3D12_VIEWPORT dvp = g_Engine->GetViewport();
        pp[33] = XMFLOAT4(dvp.Width > 0 ? 1.0f / dvp.Width : 0.0f,
                          dvp.Height > 0 ? 1.0f / dvp.Height : 0.0f, 0.0f, 0.0f); // デファード: 1/画面サイズ
    }
    // 街灯の点光源 (b9)
    if (auto* lp = m_lightCB->GetPtr<XMFLOAT4>())
    {
        int n = m_cfg.enableLamps ? (int)std::min<size_t>(m_lampWorld.size(), 64) : 0;
        lp[0] = XMFLOAT4((float)n, 0, 0, 0);
        for (int i = 0; i < n; i++)
        {
            lp[1 + i] = XMFLOAT4(m_lampWorld[i].x, m_lampWorld[i].y, m_lampWorld[i].z, 8.0f); // radius 8m
            lp[1 + 64 + i] = XMFLOAT4(6.0f, 4.5f, 2.4f, 0.0f); // 電球色×強度
        }
    }

    // レンダーターゲットをバインド（反射モードは反射RT、通常はメイン HDR）
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;
    if (refl) { rtv = refl->rtv; dsv = refl->dsv; vp = refl->vp; sc = refl->scissor; }
    else { rtv = g_Engine->GetHdrRtvCpuHandle(); dsv = g_Engine->GetDsvCpuHandle();
           vp = g_Engine->GetViewport(); sc = g_Engine->GetScissorRect(); }
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = { m_heap->GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_psoOpaque->Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd->SetGraphicsRootConstantBufferView(1, sceneCBAddr);
    cmd->SetGraphicsRootConstantBufferView(2, m_paramsCB->GetAddress());
    if (iblTableBase.ptr) cmd->SetGraphicsRootDescriptorTable(4, iblTableBase);
    if (csmSrv.ptr)       cmd->SetGraphicsRootDescriptorTable(5, csmSrv);
    if (shadowCBAddr)     cmd->SetGraphicsRootConstantBufferView(6, shadowCBAddr);
    cmd->SetGraphicsRootConstantBufferView(7, m_lightCB->GetAddress());

    const UINT frame = g_Engine->CurrentBackBufferIndex();
    // メイン(0)/反射(1)で別領域を使い、同フレーム2回の Draw がワールドCBを踏み合わないようにする。
    const uint32_t passSlot = refl ? 1u : 0u;
    const uint32_t frameBase = (frame * 2u + passSlot) * kWorldSlotsPerFrame;
    const uint32_t frameEnd = frameBase + kWorldSlotsPerFrame;
    uint32_t slot = frameBase;
    char* worldBase = reinterpret_cast<char*>(m_worldCB->GetPtr());

    // インスタンス World を StructuredBuffer<float4x4> として VS(t0,space1) にバインド
    cmd->SetGraphicsRootShaderResourceView(0, m_worldCB->GetAddress());
    cmd->SetGraphicsRoot32BitConstant(8, 0u, 1);   // windEnable=0（建物/道路/デカール/ガラスは揺れない）

    // 診断用トグル
    static char ev[8];
    static const bool noLand = (GetEnvironmentVariableA("DX12_NOLAND", ev, sizeof(ev)) > 0);
    static const bool noInst = (GetEnvironmentVariableA("DX12_NOINST", ev, sizeof(ev)) > 0);
    static const bool noFoliage = (GetEnvironmentVariableA("DX12_NOFOLIAGE", ev, sizeof(ev)) > 0);

    // 道路/地形用に G を 1 スロット書く
    const uint32_t gSlot = slot;
    {
        XMFLOAT4X4 gT; XMStoreFloat4x4(&gT, XMMatrixTranspose(GlobalG()));
        std::memcpy(worldBase + (size_t)slot * kWorldSlotBytes, &gT, sizeof(XMFLOAT4X4));
        ++slot;
    }

    // 地形 → 道路（World=G, 1 インスタンス）。反射パスでは地面(水面と同一平面)は映さない。
    if (!refl)
    {
        if (!noLand) DrawLandscapes(cmd, gSlot);
        DrawRoads(cmd, gSlot);
    }
    if (noInst) return;

    // ---- バッチ構築: ソート済み m_instances を (model, table0) 連続ランでまとめ、
    //      各ランの World を連続スロットへ書き込む（GPU インスタンシング）----
    struct Batch { const TownModel* model; D3D12_GPU_DESCRIPTOR_HANDLE table0; uint32_t base; uint32_t count; bool foliage; };
    std::vector<Batch> batches;
    batches.reserve(512);

    for (const Instance& inst : m_instances)
    {
        if (slot >= frameEnd) break;
        if (!inst.model || inst.model->subs.empty()) continue;
        if (noFoliage && inst.isFoliage) continue;
        // カリング
        float dx = inst.worldCenter.x - camPos.x, dy = inst.worldCenter.y - camPos.y, dz = inst.worldCenter.z - camPos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (inst.isFoliage && m_cfg.foliageMaxDist > 0.0f && dist > m_cfg.foliageMaxDist) continue;
        if (dist > inst.worldRadius && inst.worldRadius < m_cfg.cullMinScreenRatio * dist) continue;
        // 反射パスは視錐台平面カリングを行わない（ミラー行列は行列式が負で平面が反転し
        // 全カリング/全通過になる危険があるため）。距離カリングのみ有効。
        if (!refl)
        {
            bool outside = false;
            for (const XMFLOAT4& p : planes)
            {
                float d = p.x * inst.worldCenter.x + p.y * inst.worldCenter.y + p.z * inst.worldCenter.z + p.w;
                if (d < -inst.worldRadius) { outside = true; break; }
            }
            if (outside) continue;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE table0 = inst.overrideTable.ptr ? inst.overrideTable : inst.model->subs[0].matTable;
        if (batches.empty() || batches.back().model != inst.model || batches.back().table0.ptr != table0.ptr)
            batches.push_back(Batch{ inst.model, table0, slot, 0, inst.isFoliage });

        std::memcpy(worldBase + (size_t)slot * kWorldSlotBytes, &inst.worldT, sizeof(XMFLOAT4X4));
        ++slot;
        ++batches.back().count;
    }

    // 1 バッチ = 同一メッシュのインスタンス群を submesh 毎に 1 回の DrawIndexedInstanced
    auto drawBatch = [&](const Batch& b, bool glassPass)
    {
        cmd->SetGraphicsRoot32BitConstant(8, b.base, 0);
        const auto& subs = b.model->subs;
        for (size_t si = 0; si < subs.size(); ++si)
        {
            const SubMesh& s = subs[si];
            if (!s.vbRes || !s.ibRes || s.indexCount == 0) continue;
            const bool isGlass = m_cfg.enableGlass && s.glass;
            if (glassPass != isGlass) continue;
            D3D12_GPU_DESCRIPTOR_HANDLE table = (si == 0) ? b.table0 : s.matTable;
            if (table.ptr) cmd->SetGraphicsRootDescriptorTable(3, table);
            cmd->IASetVertexBuffers(0, 1, &s.vbv);
            cmd->IASetIndexBuffer(&s.ibv);
            cmd->DrawIndexedInstanced(s.indexCount, b.count, 0, 0, 0);
        }
    };

    // 不透明パス（非植栽バッチの非ガラス）
    for (const Batch& b : batches)
        if (!b.foliage) drawBatch(b, /*glassPass*/false);

    // 反射パスは不透明の建物のみ（植栽/デカール/ガラスは省略＝負荷減・アーティファクト回避）
    if (refl) return;

    // 植栽パス（軽量PS）。葉インスタンスだけ風で揺らす（windEnable=1）。
    if (m_cfg.cheapFoliage && m_psoFoliage && m_psoFoliage->IsValid())
    {
        cmd->SetPipelineState(m_psoFoliage->Get());
        cmd->SetGraphicsRootConstantBufferView(2, m_paramsCB->GetAddress());
        cmd->SetGraphicsRoot32BitConstant(8, 1u, 1);   // windEnable=1
        for (const Batch& b : batches)
            if (b.foliage) drawBatch(b, /*glassPass*/false);
        cmd->SetGraphicsRoot32BitConstant(8, 0u, 1);   // 後続（デカール/ガラス）は揺らさない
    }

    // デカールパス（不透明/植栽の後、ガラスの前。地面の上に投影）
    DrawDecals(cmd, gSlot);                 // 平面デカール（道路/横断歩道）
    DrawDecalsDeferred(cmd, rtv, dsv);      // 壁/曲面デカール（深度投影, awning 等）

    // ガラスパス（深度書込み無し + ブレンド）
    if (m_cfg.enableGlass && m_psoGlass && m_psoGlass->IsValid())
    {
        // C1 ガラスSSR: 不透明シーンを反射ソースとしてコピー。深度を SRV 化し、深度テストは
        // シェーダ内で手動化（DSV=null）＝デファードデカールと同じ深度ハンドリングを流用。
        const bool ssr = (m_sceneColorCopy && m_depthSrv && m_sceneCopySrv);
        ID3D12Resource* depthRes = ssr ? g_Engine->GetDepthStencilResource() : nullptr;
        if (ssr)
        {
            ID3D12Resource* hdrRes = g_Engine->GetHdrColorResource();
            // (1) 不透明 HDR -> コピー（反射ソース）
            D3D12_RESOURCE_BARRIER b[2];
            b[0] = CD3DX12_RESOURCE_BARRIER::Transition(hdrRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            b[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_sceneColorCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(2, b);
            cmd->CopyResource(m_sceneColorCopy.Get(), hdrRes);
            b[0] = CD3DX12_RESOURCE_BARRIER::Transition(hdrRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            b[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_sceneColorCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(2, b);
            // (2) 深度 -> SRV、DSV を外す（手動深度テスト）
            auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &toSrv);
            cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }

        cmd->SetPipelineState(m_psoGlass->Get());
        cmd->SetGraphicsRootConstantBufferView(2, m_paramsCB->GetAddress() + 256);
        if (ssr) cmd->SetGraphicsRootDescriptorTable(10, m_depthSrv->HandleGPU);  // t9=深度, t10=シーンカラー
        for (const Batch& b : batches)
            if (!b.foliage && b.model->hasGlass) drawBatch(b, /*glassPass*/true);

        if (ssr)
        {
            // (3) 深度を DEPTH_WRITE に戻す（後続の SSR水たまり/Atmosphere が前提）。DSV 再バインドは不要。
            auto back = CD3DX12_RESOURCE_BARRIER::Transition(depthRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmd->ResourceBarrier(1, &back);
        }
    }
}

//=============================================================================
TownScene::~TownScene()
{
    for (auto& kv : m_cache)
    {
        if (!kv.second) continue;
        delete kv.second;   // ComPtr メンバが VRAM リソースを解放
    }
    m_cache.clear();
    m_landscapes.clear();   // ComPtr が解放
    delete m_worldCB;
    delete m_paramsCB;
    delete m_lightCB;
    delete m_psoOpaque;
    delete m_psoGlass;
    delete m_psoFoliage;
    delete m_psoDecal;
}
