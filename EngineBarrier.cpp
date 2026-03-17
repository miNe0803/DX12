// Windows.h を一切インクルードしないため、ResourceBarrier マクロが定義されず D3D12 API が正しく呼ばれる
#include "EngineBarrier.h"

void EngineDoTransition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES fromState, D3D12_RESOURCE_STATES toState)
{
	EngineResBarrier b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	b.Transition.pResource = res;
	b.Transition.StateBefore = fromState;
	b.Transition.StateAfter = toState;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}
