#include "LODSystem.h"
#include "../Components.h"
#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

static float Distance(const XMFLOAT3& a, const XMFLOAT3& b)
{
	float dx = a.x - b.x;
	float dy = a.y - b.y;
	float dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// CPU 側 WorldMatrix は XMMatrixTranspose(T*S*B*R)。転置後は行ベクトル用の「第4行」が列4に来るため _14,_24,_34 がワールド原点
static XMFLOAT3 GetWorldOriginFromGPUWorldMatrix(const DirectX::XMMATRIX& m)
{
	DirectX::XMFLOAT4X4 f;
	DirectX::XMStoreFloat4x4(&f, m);
	return XMFLOAT3(f._14, f._24, f._34);
}

void LODSystem::Update(entt::registry& registry, const DirectX::XMFLOAT3& cameraPos)
{
	auto view = registry.view<TransformComponent, LODComponent>();

	for (auto entity : view)
	{
		auto& transform = view.get<TransformComponent>(entity);
		auto& lod = view.get<LODComponent>(entity);

		XMFLOAT3 pos = GetWorldOriginFromGPUWorldMatrix(transform.WorldMatrix);
		float dist = Distance(pos, cameraPos);
		lod.DistanceToCamera = dist;

		if (dist < 5000.0f)
			lod.CurrentLODLevel = 0;
		else if (dist < 15000.0f)
			lod.CurrentLODLevel = 1;
		else
			lod.CurrentLODLevel = 3; // 遠景はカリング
	}
}
