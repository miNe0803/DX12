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

struct alignas(256) Transform {
    DirectX::XMMATRIX World;
    DirectX::XMMATRIX View;
    DirectX::XMMATRIX Proj;
};

// PBR用: RimParams (NormalScale等) + カメラ位置（反射ベクトル計算用）
struct PBRConstants {
    DirectX::XMFLOAT4 RimParams;
    DirectX::XMFLOAT4 CameraPos;
};

// 地形用: 4レイヤー色 + カメラ位置（Terrain_PS で PBR 用）
struct TerrainConstants {
    DirectX::XMFLOAT4 LayerColor[4]; // 0=地面, 1=雪, 2=水, 3=木
    DirectX::XMFLOAT4 CameraPos;
};

struct Mesh {
    std::string Name; 
    std::vector<Vertex> Vertices;
    std::vector<uint32_t> Indices;
    std::wstring DiffuseMap;
    std::wstring NormalMap;   
    std::wstring MetallicMap; 
    std::wstring RoughnessMap;
    std::vector<Bone> Bones;  
};

