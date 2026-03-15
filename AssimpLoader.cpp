#include "AssimpLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <filesystem>
#include <algorithm>
#include <Windows.h> // WideCharToMultiByte
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

//
// Load scene
//

bool AssimpLoader::Load(ImportSettings& settings)
{
    Assimp::Importer importer;

    std::wstring targetFilePath = settings.filename;

    bool isLargeModel = (targetFilePath.find(L"sakura") != std::wstring::npos);

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

    fs::path modelDir = fs::path(targetFilePath).parent_path();

    // PMX (MMD): UV convention differs; V=0 at bottom -> flip V for DirectX. Avoid double-flip from settings.
    const std::wstring pathLower = [&]() {
        std::wstring s = targetFilePath;
        for (auto& c : s) if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
        return s;
    }();
    const bool isPmx = (pathLower.size() >= 4 && pathLower.compare(pathLower.size() - 4, 4, L".pmx") == 0);

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
    {
        const aiMesh* src = scene->mMeshes[i];
        Mesh& dst = settings.meshes[i];

        dst.Vertices.resize(src->mNumVertices);

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
                    if ((upAxis == 2) || settings.inverseV) v_ = 1.0f - v_;
                }
                vOut.UV = { u, v_ };
            }
            vOut.Color = { 1,1,1,1 };
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
        const aiMaterial* mat = scene->mMaterials[src->mMaterialIndex];
        auto Resolve = [&](aiTextureType type, std::wstring& outPath) {
            aiString aPath;
            if (mat->GetTexture(type, 0, &aPath) == AI_SUCCESS) {
                fs::path texPathInFBX = UTF8ToWide(aPath.C_Str());
                outPath = (modelDir / texPathInFBX).wstring();
            }
            };

        Resolve(aiTextureType_DIFFUSE, dst.DiffuseMap);
        Resolve(aiTextureType_NORMALS, dst.NormalMap);
        Resolve(aiTextureType_METALNESS, dst.MetallicMap);
        Resolve(aiTextureType_DIFFUSE_ROUGHNESS, dst.RoughnessMap);
    }

    if (scene->HasAnimations() && settings.outClips)
    {
        LoadAnimations(scene, *settings.outClips);
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