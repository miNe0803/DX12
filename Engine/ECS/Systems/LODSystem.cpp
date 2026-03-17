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

static XMFLOAT3 GetPositionFromBaseMatrix(const XMFLOAT4X4& m)
{
	return XMFLOAT3(m._41, m._42, m._43);
}

void LODSystem::Update(entt::registry& registry, const DirectX::XMFLOAT3& cameraPos)
{
	auto view = registry.view<TransformComponent, LODComponent>();

	for (auto entity : view)
	{
		auto& transform = view.get<TransformComponent>(entity);
		auto& lod = view.get<LODComponent>(entity);

		XMFLOAT3 pos = GetPositionFromBaseMatrix(transform.BaseMatrix);
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
