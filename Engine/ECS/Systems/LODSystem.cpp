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

static float DistanceSq(const XMFLOAT3& a, const XMFLOAT3& b)
{
	float dx = a.x - b.x;
	float dy = a.y - b.y;
	float dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz;
}

// CPU 側 WorldMatrix は XMMatrixTranspose(S*B*R*T)。転置後は (tx,ty,tz) が _14,_24,_34（モデル原点のワールド座標）
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

		if (registry.all_of<TreeInstanceTag>(entity))
		{
			// sqrt を回避: 二乗距離で LOD 判定
			const float d2 = DistanceSq(pos, cameraPos);
			lod.DistanceToCamera = sqrtf(d2); // UI 表示用のみ sqrt（低頻度）

			const float dist2 = floorf(sqrtf(d2) * 2.f) * 0.5f; // 量子化には一回だけ sqrt
			static constexpr float h = 14.0f;
			const int prev = lod.CurrentLODLevel;
			// 閾値を二乗に変換して比較（55²=3025, 160²=25600, 350²=122500）
			static constexpr float t0 = 55.0f, t1 = 160.0f, t2 = 350.0f;
			if (prev <= 0)
			{
				if (d2 >= (t0+h)*(t0+h)) lod.CurrentLODLevel = 1;
				else lod.CurrentLODLevel = 0;
			}
			else if (prev == 1)
			{
				if (d2 < (t0-h)*(t0-h)) lod.CurrentLODLevel = 0;
				else if (d2 >= (t1+h)*(t1+h)) lod.CurrentLODLevel = 2;
				else lod.CurrentLODLevel = 1;
			}
			else if (prev == 2)
			{
				if (d2 < (t1-h)*(t1-h)) lod.CurrentLODLevel = 1;
				else if (d2 >= (t2+h)*(t2+h)) lod.CurrentLODLevel = 3;
				else lod.CurrentLODLevel = 2;
			}
			else
			{
				if (d2 < (t2-h)*(t2-h)) lod.CurrentLODLevel = 2;
				else lod.CurrentLODLevel = 3;
			}
			continue;
		}

		const float distRaw = Distance(pos, cameraPos);
		lod.DistanceToCamera = distRaw;

		//if (dist < 5000.0f)
		//	lod.CurrentLODLevel = 0;
		//else if (dist < 15000.0f)
		//	lod.CurrentLODLevel = 1;
		//else
		//	lod.CurrentLODLevel = 3; // 遠景はカリング
	}

	// 木のルート（TreeInstanceTag）の LOD を子メッシュへ複製（DrawMain は子の LOD を参照）
	registry.view<ModelGroupChildComponent, LODComponent>().each(
		[&registry](entt::entity, ModelGroupChildComponent& ch, LODComponent& childLod) {
			if (!registry.valid(ch.parent))
				return;
			if (!registry.all_of<TreeInstanceTag, LODComponent>(ch.parent))
				return;
			const auto& pLod = registry.get<LODComponent>(ch.parent);
			childLod.CurrentLODLevel = pLod.CurrentLODLevel;
			childLod.DistanceToCamera = pLod.DistanceToCamera;
		});
}
