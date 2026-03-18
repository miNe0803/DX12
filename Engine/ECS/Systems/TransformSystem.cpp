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
		XMMATRIX world = T * S * base * R;
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
