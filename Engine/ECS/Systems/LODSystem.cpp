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

// ワールド行列の平行移動成分（Position と BaseMatrix のどちらで動かしても正しい距離計算になる）
static XMFLOAT3 GetPositionFromWorldMatrix(const DirectX::XMMATRIX& m)
{
	DirectX::XMFLOAT4X4 f;
	DirectX::XMStoreFloat4x4(&f, m);
	return XMFLOAT3(f._41, f._42, f._43);
}

void LODSystem::Update(entt::registry& registry, const DirectX::XMFLOAT3& cameraPos)
{
	auto view = registry.view<TransformComponent, LODComponent>();

	for (auto entity : view)
	{
		auto& transform = view.get<TransformComponent>(entity);
		auto& lod = view.get<LODComponent>(entity);

		XMFLOAT3 pos = GetPositionFromWorldMatrix(transform.WorldMatrix);
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
