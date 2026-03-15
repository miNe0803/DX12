#include "SharedStruct.h"

// é¿ëÃíËã`ÇÕÇ±Ç± 1 â”èäÇæÇØÇ…çiÇÈ
const D3D12_INPUT_ELEMENT_DESC Vertex::InputElements[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BONEINDEX", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BONEWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
};

const D3D12_INPUT_LAYOUT_DESC Vertex::InputLayout = {
    Vertex::InputElements,
    7
};


// éñåÃñhé~ÅiîCà”Åj
static_assert(offsetof(Vertex, Normal) == 12, "offset mismatch");
static_assert(offsetof(Vertex, UV) == 24, "offset mismatch");
static_assert(offsetof(Vertex, Tangent) == 32, "offset mismatch");
static_assert(offsetof(Vertex, Color) == 44, "offset mismatch");