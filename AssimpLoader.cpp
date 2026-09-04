#include "AssimpLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <system_error>
#include <functional>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <Windows.h> // WideCharToMultiByte
#include <assimp/material.h>
#include "SharedStruct.h"

#include "Texture2D.h"

namespace fs = std::filesystem;
using namespace DirectX;

// =============================================================================
// COORDINATE SYSTEMS (single source of truth)
// =============================================================================
// [1] FILE (Assimp output with ConvertToLeftHanded)
//     - UpAxis from FBX/PMX: 1 = Y-up, 2 = Z-up. PMX often has no metadata (treated Y-up).
//     - Positions/normals/tangents: in file axis. We apply axisFix to get [2].
// [2] MODEL SPACE (after axisFix, stored in VertexBuffer)
//     - LH, +Y up. Forward = -Z (before baseTransform). Same for all formats.
//     - axisFix: Scale(1,1,-1) then, if Z-up, X(+90). So file +Z -> our +Y.
// [3] WORLD SPACE (Scene: World = Scale * baseTransform * RotY, passed transposed)
//     - baseTransform = Y(180) so model front faces +Z (camera). Same LH, +Y up.
// [4] UV
//     - No matrix. Range [0,1]. DirectX: V=0 top. FBX/Blender: often V=0 bottom -> flip V.
//     - PMX: Assimp may already output correct; we use raw UV (no flip) for PMX.
// =============================================================================

//
// Axis correction: file space -> our model space [2].
//

static XMMATRIX BuildAxisCorrection(const aiScene* scene)
{
    int upAxis = 1;
    if (scene->mMetaData)
        scene->mMetaData->Get("UpAxis", upAxis);

    // 1. LH: flip Z so we match DirectX (e.g. camera forward +Z)
    XMMATRIX m = XMMatrixScaling(1, 1, -1);

    // 2. Z-up file: file +Z (up) -> our +Y. Apply after scale: scale sends file (0,0,1)->(0,0,-1);
    //    then we need (0,0,-1)->(0,1,0) so up is +Y. Rotation X by +90: (0,0,-1)->(0,1,0). So X(+90).
    if (upAxis == 2)
        m = XMMatrixRotationX(XM_PIDIV2) * m;

    return m;
}

static XMFLOAT4X4 ToF44(const aiMatrix4x4& m)
{
    XMFLOAT4X4 r;

    // Assimp (Row-Major) to DirectX (Row-Major) copy
    r._11 = m.a1; r._12 = m.a2; r._13 = m.a3; r._14 = m.a4;
    r._21 = m.b1; r._22 = m.b2; r._23 = m.b3; r._24 = m.b4;
    r._31 = m.c1; r._32 = m.c2; r._33 = m.c3; r._34 = m.c4;
    r._41 = m.d1; r._42 = m.d2; r._43 = m.d3; r._44 = m.d4;

    return r;
}

//
// UTF conversion
//

static std::string WideToUTF8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(len - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring UTF8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

/// UTF-8 部分文字列（PMX マテリアル名の漢字用）
static bool Utf8Contains(const std::string& hay, const char* needleUtf8, size_t needleLen)
{
    if (hay.empty() || !needleUtf8 || needleLen == 0)
        return false;
    return hay.find(std::string(needleUtf8, needleLen)) != std::string::npos;
}

/// hibana 等: 肌・顔・目・口など（髪・衣は含めない）
static bool PmxMaterialNameSuggestsFacePart(const std::string& matNameUtf8)
{
    static const struct {
        const char* b;
        size_t n;
    } kParts[] = {
        { "\xe8\x82\x8c", 3 }, // 肌
        { "颜", 3 }, // 颜 (简)
        { "\xe9\xa1\x8f", 3 }, // 顏 (繁)
        { "\xe9\xa1\xb4", 3 }, // 顔 (JP 常用 U+9854)
        { "\xe7\x9b\xae", 3 }, // 目
        { "\xe5\x8f\xa3", 3 }, // 口
        { "\xe8\x88\x8c", 3 }, // 舌
        { "\xe9\xbd\x92", 3 }, // 齒
        { "\xe7\x89\x99", 3 }, // 牙
        { "\xe7\x99\xbd", 3 }, // 白
        { "\xe7\x9c\x89", 3 }, // 眉
        { "\xe7\x9d\xab", 3 }, // 睫
        { "\xe9\x9d\xa2", 3 }, // 面 (面纹など)
    };
    for (const auto& p : kParts)
    {
        if (Utf8Contains(matNameUtf8, p.b, p.n))
            return true;
    }
    return false;
}

static std::string MaterialPropertyKeyToLower(const char* k)
{
    if (!k)
        return {};
    std::string key(k);
    for (char& c : key)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

static int DiffusePropertyKeyScore(const std::string& keyLower)
{
    if (keyLower.find("$clr.diffuse") != std::string::npos)
        return 100;
    if (keyLower.find("diffuse") != std::string::npos)
        return 80;
    if (keyLower.find("clr") != std::string::npos && keyLower.find("color") != std::string::npos)
        return 60;
    if (keyLower.find("clr") != std::string::npos)
        return 40;
    if (keyLower.find("color") != std::string::npos)
        return 30;
    return 0;
}

static bool IsPlausibleDiffuseRgba(float r, float g, float b, float a)
{
    if (a < -0.02f || a > 1.02f)
        return false;
    if (r < -0.1f || r > 2.0f || g < -0.1f || g > 2.0f || b < -0.1f || b > 2.0f)
        return false;
    return true;
}

/// Assimp が Get(AI_MATKEY_COLOR_DIFFUSE) の A を 1 にしても、生プロパティに RGBA が残ることがある（PMX 目影など）
static bool TryReadDiffuseAlphaFromMaterialProperties(const aiMaterial* mat, float& outA)
{
    if (!mat)
        return false;
    for (unsigned i = 0; i < mat->mNumProperties; ++i)
    {
        const aiMaterialProperty* p = mat->mProperties[i];
        if (!p || p->mType != aiPTI_Float || p->mDataLength < 16)
            continue;
        const std::string key = MaterialPropertyKeyToLower(p->mKey.C_Str());
        if (DiffusePropertyKeyScore(key) == 0)
            continue;
        const float* f = reinterpret_cast<const float*>(p->mData);
        float a = f[3];
        if (a >= 0.f && a <= 1.0001f)
        {
            outA = (std::min)(1.f, a);
            return true;
        }
    }
    return false;
}

/// PMX 等: Get(DIFFUSE) が (1,1,1,1) のまま返るが、生プロパティに作者の Diffuse 色が残ることがある → 頂点カラー用に RGBA 全体を拾う
static bool TryReadDiffuseRgbaFromMaterialProperties(const aiMaterial* mat, aiColor4D& out)
{
    if (!mat)
        return false;
    int bestScore = 0;
    aiColor4D best(1.f, 1.f, 1.f, 1.f);
    for (unsigned i = 0; i < mat->mNumProperties; ++i)
    {
        const aiMaterialProperty* p = mat->mProperties[i];
        if (!p)
            continue;
        const std::string key = MaterialPropertyKeyToLower(p->mKey.C_Str());
        const int ks = DiffusePropertyKeyScore(key);
        if (ks < 40)
            continue;

        float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
        if (p->mType == aiPTI_Float && p->mDataLength >= 16)
        {
            const float* f = reinterpret_cast<const float*>(p->mData);
            r = f[0];
            g = f[1];
            b = f[2];
            a = f[3];
        }
        else if (p->mType == aiPTI_Float && p->mDataLength >= 12)
        {
            const float* f = reinterpret_cast<const float*>(p->mData);
            r = f[0];
            g = f[1];
            b = f[2];
            a = 1.f;
        }
        else if (p->mType == aiPTI_Double && p->mDataLength >= 32)
        {
            const double* d = reinterpret_cast<const double*>(p->mData);
            r = static_cast<float>(d[0]);
            g = static_cast<float>(d[1]);
            b = static_cast<float>(d[2]);
            a = static_cast<float>(d[3]);
        }
        else if (p->mType == aiPTI_Double && p->mDataLength >= 24)
        {
            const double* d = reinterpret_cast<const double*>(p->mData);
            r = static_cast<float>(d[0]);
            g = static_cast<float>(d[1]);
            b = static_cast<float>(d[2]);
            a = 1.f;
        }
        else
            continue;

        if (!IsPlausibleDiffuseRgba(r, g, b, a))
            continue;
        a = (std::min)(1.f, (std::max)(0.f, a));

        if (ks >= bestScore)
        {
            bestScore = ks;
            best.r = r;
            best.g = g;
            best.b = b;
            best.a = a;
        }
    }
    if (bestScore < 40)
        return false;
    out = best;
    return true;
}

static std::string ToLowerAsciiCopy(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

static bool PathWContainsInsensitive(const std::wstring& w, const wchar_t* tok)
{
    if (w.empty() || !tok)
        return false;
    std::wstring s = w;
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z')
            c = wchar_t(c - L'A' + L'a');
    std::wstring t = tok;
    for (wchar_t& c : t)
        if (c >= L'A' && c <= L'Z')
            c = wchar_t(c - L'A' + L'a');
    return s.find(t) != std::wstring::npos;
}

/// 齒・面纹は PMX 上 toon4 のことがある → toon3 強制から除外
static bool PmxExcludeForceToon3Skin(const std::string& matNameUtf8)
{
    return Utf8Contains(matNameUtf8, "\xe9\xbd\x92", 3) // 齒
        || Utf8Contains(matNameUtf8, "\xe9\x9d\xa2\xe7\xba\xb9", 6); // 面纹
}

/// 衣・髪・手套など toon4 優先（「手」より先に判定し、手套が肌扱いにならないようにする）
static bool PmxMatNameClothHairAccessoryToon4(const std::string& matNameUtf8)
{
    if (Utf8Contains(matNameUtf8, "\xe8\xa1\xa3", 3)) // 衣
        return true;
    if (Utf8Contains(matNameUtf8, "\xe9\xab\xaa", 3)) // 髪
        return true;
    if (Utf8Contains(matNameUtf8, "\xe6\x89\x8b\xe5\xa5\x97", 6)) // 手套
        return true;
    std::string lower = matNameUtf8;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("glove") != std::string::npos || lower.find("cloth") != std::string::npos || lower.find("dress") != std::string::npos
        || lower.find("hair") != std::string::npos)
        return true;
    return false;
}

/// toon3 ランプ: 肌・手足・顔系（ASCII 名のフォールバック含む）
static bool PmxMatNameSkinLikeToon3(const std::string& matNameUtf8)
{
    if (PmxExcludeForceToon3Skin(matNameUtf8))
        return false;
    if (Utf8Contains(matNameUtf8, "\xe8\x82\x8c", 3)) // 肌
        return true;
    if (Utf8Contains(matNameUtf8, "\xe6\x89\x8b", 3) && !Utf8Contains(matNameUtf8, "\xe5\xa5\x97", 3))
        return true;
    if (Utf8Contains(matNameUtf8, "\xe8\xb6\xb3", 3)) // 足
        return true;
    if (PmxMaterialNameSuggestsFacePart(matNameUtf8))
        return true;
    std::string lower = matNameUtf8;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kSkinAscii[] = { "skin", "body", "face", "hand", "foot", "leg", "lip", "mouth", "cheek", "hoho" };
    for (const char* k : kSkinAscii)
    {
        if (lower.find(k) != std::string::npos)
            return true;
    }
    return false;
}

/// PMX マテリアル番号（Assimp の materialIndex ≒ PMX エディタのマテ # と同じ 0 始まり）で toon を決めるテーブル。
/// 戻り値: 0=このファイル用テーブル無し, 1=toon3 系, 2=toon4 系（名前判定が失敗したときのフォールバック）
static int PmxRampKindByMaterialIndexTable(const std::wstring& modelPathLower, unsigned materialIndex)
{
    constexpr wchar_t kHibana[] = L"hiban1a.pmx";
    constexpr size_t kHibanaLen = sizeof(kHibana) / sizeof(kHibana[0]) - 1u;
    if (modelPathLower.size() < kHibanaLen
        || modelPathLower.compare(modelPathLower.size() - kHibanaLen, kHibanaLen, kHibana) != 0)
        return 0;
    // docs/hibana_pmx_materials.md: #8 齒=toon4、#12〜=衣装 toon4、#0-7 と #9-11 = 顔・肌・手足 toon3
    if (materialIndex == 8u)
        return 2;
    if (materialIndex >= 12u)
        return 2;
    return 1;
}

/// PMX: トゥーンが Emissive 等に載らない → パス名に "toon" を含むテクスチャを集め、マテ名で toon3/toon4 を優先
static void TryPmxResolveRampByToonFilename(
    const aiMaterial* mat,
    const fs::path& modelDir,
    const std::string& matNameUtf8,
    std::wstring& rampOut,
    const std::wstring& modelPathLower,
    unsigned pmxMaterialIndex)
{
    if (!mat || !rampOut.empty())
        return;
    std::vector<std::wstring> paths;
    for (int tt = 1; tt < 36; ++tt)
    {
        const aiTextureType t = static_cast<aiTextureType>(tt);
        const unsigned ntex = mat->GetTextureCount(t);
        for (unsigned ti = 0; ti < ntex; ++ti)
        {
            aiString aPath;
            if (mat->GetTexture(t, ti, &aPath) != AI_SUCCESS)
                continue;
            std::string rel(aPath.C_Str());
            if (ToLowerAsciiCopy(rel).find("toon") == std::string::npos)
                continue;
            paths.push_back((modelDir / UTF8ToWide(rel)).wstring());
        }
    }
    if (paths.empty())
        return;

    auto wlow = [](std::wstring s) {
        for (wchar_t& c : s)
        {
            if (c >= L'A' && c <= L'Z')
                c = wchar_t(c - L'A' + L'a');
        }
        return s;
    };
    auto hasTok = [&](const std::wstring& p, const wchar_t* tok) {
        return wlow(p).find(tok) != std::wstring::npos;
    };

    const int idxKindT = PmxRampKindByMaterialIndexTable(modelPathLower, pmxMaterialIndex);
    const bool nameSk = PmxMatNameSkinLikeToon3(matNameUtf8);

    if (PmxMatNameClothHairAccessoryToon4(matNameUtf8))
    {
        for (const auto& p : paths)
            if (hasTok(p, L"toon4"))
            {
                rampOut = p;
                return;
            }
        for (const auto& p : paths)
            if (hasTok(p, L"toon3"))
            {
                rampOut = p;
                return;
            }
        rampOut = paths[0];
        return;
    }
    // 番号表 toon4（齒・衣装番号）: マテ名が肌扱いでないときだけ（名前優先）
    if (idxKindT == 2 && !nameSk)
    {
        for (const auto& p : paths)
            if (hasTok(p, L"toon4"))
            {
                rampOut = p;
                return;
            }
        for (const auto& p : paths)
            if (hasTok(p, L"toon3"))
            {
                rampOut = p;
                return;
            }
        rampOut = paths[0];
        return;
    }
    if (nameSk || idxKindT == 1)
    {
        for (const auto& p : paths)
            if (hasTok(p, L"toon3"))
            {
                rampOut = p;
                return;
            }
        for (const auto& p : paths)
            if (hasTok(p, L"toon4"))
            {
                rampOut = p;
                return;
            }
        rampOut = paths[0];
        return;
    }
    rampOut = paths[0];
}

/// PMX ランプ確定: マテ名優先。名前が化ける場合は materialIndex テーブル（hibana.pmx）で toon3/toon4 を決定。
static void FinalizePmxRampMap(
    const fs::path& modelDir,
    const std::string& matNameUtf8,
    std::wstring& rampOut,
    const std::wstring& modelPathLower,
    unsigned pmxMaterialIndex)
{
    auto absPathStr = [](const fs::path& p) -> std::wstring {
        std::error_code ec;
        fs::path a = fs::absolute(p, ec);
        return (ec ? p : a).wstring();
    };

    auto pathHasToon = [](const std::wstring& w) -> bool {
        if (w.empty())
            return false;
        std::wstring s = w;
        for (wchar_t& c : s)
            if (c >= L'A' && c <= L'Z')
                c = wchar_t(c - L'A' + L'a');
        return s.find(L"toon") != std::wstring::npos;
    };

    const fs::path t3png = modelDir / L"toon3.png";
    const fs::path t3bmp = modelDir / L"toon3.bmp";
    const fs::path t4png = modelDir / L"toon4.png";
    const fs::path t4bmp = modelDir / L"toon4.bmp";

    if (PmxMatNameClothHairAccessoryToon4(matNameUtf8))
    {
        if (fs::exists(t4png))
            rampOut = absPathStr(t4png);
        else if (fs::exists(t4bmp))
            rampOut = absPathStr(t4bmp);
        else if (fs::exists(t3png))
            rampOut = absPathStr(t3png);
        else if (fs::exists(t3bmp))
            rampOut = absPathStr(t3bmp);
        return;
    }

    const int idxKind = PmxRampKindByMaterialIndexTable(modelPathLower, pmxMaterialIndex);
    const bool nameSkin = PmxMatNameSkinLikeToon3(matNameUtf8);

    if (nameSkin)
    {
        if (fs::exists(t3png))
        {
            rampOut = absPathStr(t3png);
            return;
        }
        if (fs::exists(t3bmp))
        {
            rampOut = absPathStr(t3bmp);
            return;
        }
    }

    if (idxKind == 2 && !nameSkin)
    {
        if (fs::exists(t4png))
            rampOut = absPathStr(t4png);
        else if (fs::exists(t4bmp))
            rampOut = absPathStr(t4bmp);
        else if (fs::exists(t3png))
            rampOut = absPathStr(t3png);
        else if (fs::exists(t3bmp))
            rampOut = absPathStr(t3bmp);
        return;
    }

    if (idxKind == 1 || nameSkin)
    {
        if (fs::exists(t3png))
        {
            rampOut = absPathStr(t3png);
            return;
        }
        if (fs::exists(t3bmp))
        {
            rampOut = absPathStr(t3bmp);
            return;
        }
        // toon3 が無い場合は下へ（肌マテなのに toon3 未同梱のとき Emissive の toon を残す）
    }

    if (!pathHasToon(rampOut))
    {
        if (fs::exists(t4png))
            rampOut = absPathStr(t4png);
        else if (fs::exists(t4bmp))
            rampOut = absPathStr(t4bmp);
        else if (fs::exists(t3png))
            rampOut = absPathStr(t3png);
        else if (fs::exists(t3bmp))
            rampOut = absPathStr(t3bmp);
        return;
    }

    // Assimp が肌系マテの Ramp に toon4 だけ載せた場合 → 同フォルダに toon3 があれば差し替え（番号表 toon3 も対象）
    if ((nameSkin || idxKind == 1) && PathWContainsInsensitive(rampOut, L"toon4")
        && !PathWContainsInsensitive(rampOut, L"toon3"))
    {
        if (fs::exists(t3png))
            rampOut = absPathStr(t3png);
        else if (fs::exists(t3bmp))
            rampOut = absPathStr(t3bmp);
    }
}

//
// Load scene
//

bool AssimpLoader::Load(ImportSettings& settings)
{
    Assimp::Importer importer;

    std::wstring targetFilePath = settings.filename;

    unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_CalcTangentSpace |
        aiProcess_GenSmoothNormals |
        aiProcess_PopulateArmatureData |
        aiProcess_LimitBoneWeights |
        aiProcess_ConvertToLeftHanded; // Assimp LH conversion

    // Read scene (UTF-8 path)
    const aiScene* scene = importer.ReadFile(WideToUTF8(targetFilePath), flags);
    if (!scene || !scene->mRootNode)
        return false;

    // Coordinate chain: FBX (UpAxis 1 or 2) -> axisFix (BuildAxisCorrection) -> mesh in "our" space (LH, +Y up).
    // Then Scene applies Scale * outBaseTransform * RotY as World; camera is +Z, so model front = +Z (Y(180) here).
    int upAxis = 1;
    if (scene->mMetaData)
        scene->mMetaData->Get("UpAxis", upAxis);

    // Face camera (+Z): Y(180). That also mirrors left-right; undo with Scale(-1,1,1).
    XMMATRIX baseMatrix = XMMatrixScaling(-1.f, 1.f, 1.f) * XMMatrixRotationY(XM_PI);
    XMStoreFloat4x4(&settings.outBaseTransform, baseMatrix);
    // ------------------------------------------------

    XMMATRIX axisFix = BuildAxisCorrection(scene);

    settings.meshes.clear();
    settings.meshes.resize(scene->mNumMeshes);

    // Map mesh index -> node path (for LOD grouping etc.)
    std::vector<std::string> meshNodePath(scene->mNumMeshes);
    {
        std::function<void(const aiNode*, const std::string&)> walk = [&](const aiNode* node, const std::string& parent) {
            if (!node) return;
            const std::string name = node->mName.C_Str();
            const std::string here = parent.empty() ? name : (parent + "/" + name);
            for (unsigned mi = 0; mi < node->mNumMeshes; ++mi)
            {
                const unsigned meshIndex = node->mMeshes[mi];
                if (meshIndex < meshNodePath.size())
                    meshNodePath[meshIndex] = here;
            }
            for (unsigned ci = 0; ci < node->mNumChildren; ++ci)
                walk(node->mChildren[ci], here);
            };
        walk(scene->mRootNode, std::string{});
    }

    fs::path modelDir = fs::path(targetFilePath).parent_path();
    {
        std::error_code ec;
        const fs::path absFile = fs::absolute(fs::path(targetFilePath), ec);
        if (!ec)
            modelDir = absFile.parent_path();
    }

    // PMX (MMD): UV convention differs; V=0 at bottom -> flip V for DirectX. Avoid double-flip from settings.
    const std::wstring pathLower = [&]() {
        std::wstring s = targetFilePath;
        for (auto& c : s) if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
        return s;
    }();
    const bool isPmx = (pathLower.size() >= 4 && pathLower.compare(pathLower.size() - 4, 4, L".pmx") == 0);
    // sakura 樹木 FBX: AsyncModelLoader は inverseV=true だが TreeVegetation 同期読みは false。
    // 非 PMX で V だけ反転すると UV がテクスチャとずれ PBR 表示が壊れるため、PMX 以外はパスで判定して無効化。
    const bool isSakuraTreeFbx = !isPmx && pathLower.find(L"sakura") != std::wstring::npos;

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
    {
        const aiMesh* src = scene->mMeshes[i];
        Mesh& dst = settings.meshes[i];

        // Use node path as mesh name so downstream can group by collection/node (e.g. *_LOD0 / *_LOD1).
        dst.Name = (!meshNodePath[i].empty()) ? meshNodePath[i] : std::string(src->mName.C_Str());

        dst.Vertices.resize(src->mNumVertices);

        // PMX/MMD 等: 肌色はテクスチャではなくマテリアル Diffuse に乗ることが多い → 頂点カラーに焼き込む（シェーダで albedo *= input.color）
        const aiMaterial* mat = scene->mMaterials[src->mMaterialIndex];
        aiColor4D matDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
        (void)mat->Get(AI_MATKEY_COLOR_DIFFUSE, matDiffuse);
        // PMX: Assimp の Get が常に白でも、生プロパティに PMX の Diffuse が残るモデルがある
        if (isPmx)
        {
            aiColor4D propDiffuse;
            if (TryReadDiffuseRgbaFromMaterialProperties(mat, propDiffuse))
            {
                const bool getIsWhiteRgb =
                    (std::fabs(matDiffuse.r - 1.0f) < 0.004f && std::fabs(matDiffuse.g - 1.0f) < 0.004f
                        && std::fabs(matDiffuse.b - 1.0f) < 0.004f);
                const bool propDiffersRgb =
                    (std::fabs(propDiffuse.r - matDiffuse.r) > 0.002f || std::fabs(propDiffuse.g - matDiffuse.g) > 0.002f
                        || std::fabs(propDiffuse.b - matDiffuse.b) > 0.002f);
                const bool propDiffersA = std::fabs(propDiffuse.a - matDiffuse.a) > 0.002f;
                if (getIsWhiteRgb || propDiffersRgb || propDiffersA)
                    matDiffuse = propDiffuse;
            }
        }

        for (unsigned int v = 0; v < src->mNumVertices; ++v)
        {
            auto& vOut = dst.Vertices[v];

            XMVECTOR pos = XMVectorSet(src->mVertices[v].x, src->mVertices[v].y, src->mVertices[v].z, 1.0f);
            pos = XMVector3Transform(pos, axisFix);
            XMStoreFloat3(&vOut.Position, pos);

            if (src->HasNormals())
            {
                XMVECTOR n = XMVectorSet(src->mNormals[v].x, src->mNormals[v].y, src->mNormals[v].z, 0);
                n = XMVector3TransformNormal(n, axisFix);
                XMStoreFloat3(&vOut.Normal, n);
            }

            if (src->HasTangentsAndBitangents())
            {
                XMVECTOR t = XMVectorSet(src->mTangents[v].x, src->mTangents[v].y, src->mTangents[v].z, 0);
                t = XMVector3TransformNormal(t, axisFix);
                XMStoreFloat3(&vOut.Tangent, t);
            }

            if (src->HasTextureCoords(0))
            {
                float u = src->mTextureCoords[0][v].x;
                float v_ = src->mTextureCoords[0][v].y;
                if (isPmx) {
                    // PMX: use Assimp UV as-is (importer often already DirectX-style)
                    (void)settings;
                } else {
                    if (settings.inverseU) u = 1.0f - u;
                    const bool flipVFromSettings = settings.inverseV && !isSakuraTreeFbx;
                    if ((upAxis == 2) || flipVFromSettings) v_ = 1.0f - v_;
                }
                vOut.UV = { u, v_ };
            }
            vOut.Color = { matDiffuse.r, matDiffuse.g, matDiffuse.b, matDiffuse.a };
        }

        // Faces: keep Assimp order (ConvertToLeftHanded already handles winding for LH)
        for (unsigned int f = 0; f < src->mNumFaces; ++f)
        {
            const aiFace& face = src->mFaces[f];
            if (face.mNumIndices == 3)
            {
                dst.Indices.push_back(face.mIndices[0]);
                dst.Indices.push_back(face.mIndices[1]);
                dst.Indices.push_back(face.mIndices[2]);
            }
        }

        if (src->HasBones())
        {
            AddBoneWeights(dst, src);
            LoadBones(dst, src, scene, axisFix);
        }

        // Texture paths: keep full relative path (e.g. sakura1.fbm/xxx.png) so .fbm folder is used
        auto Resolve = [&](aiTextureType type, std::wstring& outPath) {
            aiString aPath;
            if (mat->GetTexture(type, 0, &aPath) == AI_SUCCESS) {
                fs::path texPathInFBX = UTF8ToWide(aPath.C_Str());
                outPath = (modelDir / texPathInFBX).wstring();
            }
            };

        dst.MaterialName.clear();
        {
            aiString matNameEarly;
            if (mat->Get(AI_MATKEY_NAME, matNameEarly) == AI_SUCCESS)
                dst.MaterialName = matNameEarly.C_Str();
        }

        Resolve(aiTextureType_DIFFUSE, dst.DiffuseMap);
        Resolve(aiTextureType_NORMALS, dst.NormalMap);
        Resolve(aiTextureType_METALNESS, dst.MetallicMap);
        Resolve(aiTextureType_DIFFUSE_ROUGHNESS, dst.RoughnessMap);
        Resolve(aiTextureType_OPACITY, dst.OpacityMap);

        if (isPmx)
        {
            // PMX: トゥーンは Emissive / Ambient / Lightmap に載ることがある（hibana_pmx_materials.md の toon3/toon4）
            Resolve(aiTextureType_EMISSIVE, dst.RampMap);
            if (dst.RampMap.empty())
                Resolve(aiTextureType_AMBIENT, dst.RampMap);
            if (dst.RampMap.empty())
                Resolve(aiTextureType_LIGHTMAP, dst.RampMap);
            if (dst.RampMap.empty())
            {
                aiString ap2;
                if (mat->GetTexture(aiTextureType_DIFFUSE, 1, &ap2) == AI_SUCCESS)
                    dst.RampMap = (modelDir / UTF8ToWide(std::string(ap2.C_Str()))).wstring();
            }
            if (dst.RampMap.empty())
                TryPmxResolveRampByToonFilename(mat, modelDir, dst.MaterialName, dst.RampMap, pathLower, src->mMaterialIndex);
            FinalizePmxRampMap(modelDir, dst.MaterialName, dst.RampMap, pathLower, src->mMaterialIndex);
            // スフィア: Specular / Height / Reflection（Assimp PMX の割り当てに依存）
            Resolve(aiTextureType_SPECULAR, dst.SphereMap);
            if (dst.SphereMap.empty())
                Resolve(aiTextureType_HEIGHT, dst.SphereMap);
            if (dst.SphereMap.empty())
                Resolve(aiTextureType_REFLECTION, dst.SphereMap);
            if (!dst.SphereMap.empty() && dst.SphereMode == 0)
                dst.SphereMode = 2; // MMD spa 加算が多い（1=乗算, 2=加算）
        }
        else
        {
            // NPR ランプ用（DCC で Emissive にランプテクスチャを割り当てる想定。未設定なら Scene でデフォルト）
            Resolve(aiTextureType_EMISSIVE, dst.RampMap);
        }

        float opacity = 1.0f;
        if (mat->Get(AI_MATKEY_OPACITY, opacity) != AI_SUCCESS)
            opacity = matDiffuse.a;
        // PMX: 透明度は Diffuse A や生プロパティにだけ入り、OPACITY が 1 のままのことが多い
        if (isPmx)
        {
            float propA = 1.f;
            if (TryReadDiffuseAlphaFromMaterialProperties(mat, propA))
                opacity = (std::min)(opacity, propA);
            opacity = (std::min)(opacity, matDiffuse.a);
        }
        dst.Opacity = opacity;

        std::string lower = dst.MaterialName;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool nameSuggestsTransparent = (lower.find("_tr") != std::string::npos);
        dst.NprTransparentByRule = (dst.Opacity < 0.99f) || nameSuggestsTransparent;
        // hibana「目影」: Assimp が A を落とさない場合のフォールバック（PMX では Diffuse A=0.2）
        if (isPmx && Utf8Contains(dst.MaterialName, "\xe7\x9b\xae\xe5\xbd\xb1", 6))
        {
            dst.NprTransparentByRule = true;
            dst.Opacity = (std::min)(dst.Opacity, 0.2f);
        }

        // NPR: 顔・肌パーツはセル影を頂点法線寄りに（法線マップの凹凸影を抑えて「顔影固定」に近づける）
        dst.NprCelVertexBlendOverride = -1.f;
        if (!lower.empty())
        {
            if (lower.find("face") != std::string::npos || lower.find("skin") != std::string::npos
                || lower.find("hoho") != std::string::npos || lower.find("cheek") != std::string::npos
                || lower.find("mouth") != std::string::npos || lower.find("lip") != std::string::npos)
                dst.NprCelVertexBlendOverride = 0.88f;
        }
        if (dst.NprCelVertexBlendOverride < 0.f && isPmx && PmxMaterialNameSuggestsFacePart(dst.MaterialName))
            dst.NprCelVertexBlendOverride = 0.88f;

        // 不透明扱いメッシュ: PMX Diffuse の A=0 等が albedo *= vertexColor で全体を潰すのを防ぐ（NPR 不透明 / PBR 共通データ）
        if (!dst.NprTransparentByRule)
        {
            for (auto& vtx : dst.Vertices)
                vtx.Color.w = 1.0f;
        }
    }

    if (scene->HasAnimations() && settings.outClips)
    {
        LoadAnimations(scene, *settings.outClips);
    }

    // === TEMP (retarget bridge): dump unique bone names + offset matrices to a file.
    //     Enable with env DX12_DUMPBONES=<abs filepath>. Rows: name<TAB>16 floats (row-major offsetMatrix).
    {
        char dumpPath[1024] = {};
        if (GetEnvironmentVariableA("DX12_DUMPBONES", dumpPath, sizeof(dumpPath)) > 0 && dumpPath[0])
        {
            std::ofstream ofs(dumpPath, std::ios::binary);
            if (ofs)
            {
                std::unordered_map<std::string, XMFLOAT4X4> uniq;
                std::vector<std::string> order;
                for (const auto& mesh : settings.meshes)
                    for (const auto& bone : mesh.Bones)
                        if (uniq.find(bone.name) == uniq.end())
                        {
                            XMFLOAT4X4 f; XMStoreFloat4x4(&f, bone.offsetMatrix);
                            uniq.emplace(bone.name, f);
                            order.push_back(bone.name);
                        }
                ofs << "# unique_bones " << order.size() << "\n";
                for (const auto& nm : order)
                {
                    const float* p = &uniq[nm]._11;
                    ofs << nm << "\t";
                    for (int i = 0; i < 16; ++i) { ofs << p[i]; if (i < 15) ofs << ' '; }
                    ofs << "\n";
                }
                ofs.flush();
            }
        }
    }

    return true;
}

//
// LoadBones
//

void AssimpLoader::LoadBones(Mesh& dst, const aiMesh* src, const aiScene* scene, const XMMATRIX& axisFix)
{
    XMMATRIX invAxisFix = XMMatrixInverse(nullptr, axisFix);

    for (unsigned b = 0; b < src->mNumBones; ++b)
    {
        const aiBone* ab = src->mBones[b];
        Bone nb;
        nb.name = ab->mName.C_Str();

        XMFLOAT4X4 off = ToF44(ab->mOffsetMatrix);
        XMMATRIX m = XMLoadFloat4x4(&off);

        // Bone offset into axis-corrected space
        m = axisFix * m * invAxisFix;

        nb.offsetMatrix = m;
        dst.Bones.push_back(nb);
    }
}

void AssimpLoader::AddBoneWeights(Mesh& dst, const aiMesh* src)
{
    struct BW { uint32_t idx; float w; };
    std::vector<std::vector<BW>> perVertex(src->mNumVertices);

    for (unsigned int b = 0; b < src->mNumBones; ++b) {
        const aiBone* bone = src->mBones[b];
        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight& vw = bone->mWeights[w];
            if (vw.mVertexId < perVertex.size()) {
                perVertex[vw.mVertexId].push_back({ b, vw.mWeight });
            }
        }
    }

    for (size_t i = 0; i < dst.Vertices.size(); ++i) {
        auto& list = perVertex[i];
        if (list.empty()) continue;

        std::sort(list.begin(), list.end(), [](const BW& a, const BW& b) { return a.w > b.w; });

        float sum = 0.0f;
        size_t n = std::min<size_t>(4, list.size());
        for (size_t k = 0; k < n; ++k) sum += list[k].w;
        if (sum <= 0.0f) sum = 1.0f;

        for (size_t k = 0; k < n; ++k) {
            dst.Vertices[i].BoneIndex[k] = (uint16_t)list[k].idx;
            dst.Vertices[i].BoneWeight[k] = list[k].w / sum;
        }
    }
}

void AssimpLoader::LoadAnimations(const aiScene* scene, std::vector<AnimationClip>& clips)
{
    for (unsigned int i = 0; i < scene->mNumAnimations; ++i)
    {
        const aiAnimation* aa = scene->mAnimations[i];
        AnimationClip clip;
        clip.name = aa->mName.C_Str();
        clip.duration = (float)aa->mDuration;
        clip.ticksPerSecond = (aa->mTicksPerSecond != 0.0) ? (float)aa->mTicksPerSecond : 25.0f;

        for (unsigned int c = 0; c < aa->mNumChannels; ++c)
        {
            const aiNodeAnim* ch = aa->mChannels[c];
            AnimChannel channel;
            channel.name = ch->mNodeName.C_Str();
            clip.channels[channel.name] = channel;
        }
        clips.push_back(std::move(clip));
    }
}
