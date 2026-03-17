#pragma once
#include <d3d12.h>
#ifdef ResourceBarrier
#undef ResourceBarrier
#endif
#ifdef barrier
#undef barrier
#endif
#ifdef BARRIER
#undef BARRIER
#endif
typedef D3D12_RESOURCE_BARRIER EngineResBarrier;

// Windows.h をインクルードしない .cpp で実装（ResourceBarrier マクロ衝突回避）
void EngineDoTransition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES fromState, D3D12_RESOURCE_STATES toState);
