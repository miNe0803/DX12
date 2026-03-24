#include "TransformSystem.h"
#include "../Components.h"
#include <DirectXMath.h>

using namespace DirectX;

void TransformSystem::Update(entt::registry& registry)
{
	auto view = registry.view<TransformComponent>();

	for (auto entity : view)
	{
		if (registry.all_of<ModelGroupChildComponent>(entity))
			continue;
		auto& transform = view.get<TransformComponent>(entity);
		XMVECTOR pos = XMLoadFloat3(&transform.Position);
		XMMATRIX T = XMMatrixTranslationFromVector(pos);
		XMMATRIX S = XMMatrixScaling(transform.UniformScale, transform.UniformScale, transform.UniformScale);
		XMMATRIX base = XMLoadFloat4x4(&transform.BaseMatrix);
		XMMATRIX R = XMMatrixRotationY(transform.RotationY);
		// Row-vector (mul(pos, World)) 前提: 平行移動は最後に適用しないと Position まで scale される。
		XMMATRIX world = S * base * R * T;
		transform.WorldMatrix = XMMatrixTranspose(world);
	}

	registry.view<ModelGroupChildComponent, TransformComponent>().each(
		[&registry](entt::entity entity, ModelGroupChildComponent& link, TransformComponent& tc) {
			if (!registry.valid(link.parent))
				return;
			const auto& parentTc = registry.get<TransformComponent>(link.parent);
			tc.WorldMatrix = parentTc.WorldMatrix;
		});
}
