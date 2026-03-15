#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <DirectXMath.h>
#include "SharedStruct.h"

//
// Assimp forward declarations
//
struct aiMesh;
struct aiScene;

//
// =============================
// Animation
// =============================
//
struct AnimChannel
{
    std::string name;
};

struct AnimationClip
{
    std::string name;
    float duration = 0.0f;
    float ticksPerSecond = 0.0f;

    std::unordered_map<std::string, AnimChannel> channels;
};

//
// =============================
// Import settings (YUP/ZUP auto from FBX metadata)
// =============================
//
struct ImportSettings
{
    const wchar_t* filename;
    std::vector<Mesh>& meshes;
    bool inverseU;
    bool inverseV;
    float globalScale;
    std::vector<AnimationClip>* outClips = nullptr;
    DirectX::XMFLOAT4X4 outBaseTransform;  // YUP/ZUP base transform, filled by Load()

    ImportSettings(
        const wchar_t* file,
        std::vector<Mesh>& out,
        bool invU = false,
        bool invV = false,
        float scale = 1.0f)
        : filename(file), meshes(out), inverseU(invU), inverseV(invV), globalScale(scale)
    {
        DirectX::XMStoreFloat4x4(&outBaseTransform, DirectX::XMMatrixIdentity());
    }
};

//
// =============================
// Assimp Loader
// =============================
//
class AssimpLoader
{
public:

    bool Load(ImportSettings& settings);

private:

    //
    // bone weight
    //
    void AddBoneWeights(
        Mesh& dst,
        const aiMesh* src);

    //
    // bone load
    //
    void LoadBones(
        Mesh& dst,
        const aiMesh* src,
        const aiScene* scene,
        const DirectX::XMMATRIX& axisFix);

    //
    // animation
    //
    void LoadAnimations(
        const aiScene* scene,
        std::vector<AnimationClip>& clips);
};