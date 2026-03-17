#include "TransformSystem.h"
#include "../Components.h"
#include <DirectXMath.h>

using namespace DirectX;

void TransformSystem::Update(entt::registry& registry)
{
	auto view = registry.view<TransformComponent>();

	for (auto entity : view)
	{
		auto& transform = view.get<TransformComponent>(entity);
		XMMATRIX base = XMLoadFloat4x4(&transform.BaseMatrix);
		XMMATRIX world = XMMatrixScaling(transform.UniformScale, transform.UniformScale, transform.UniformScale)
			* base * XMMatrixRotationY(transform.RotationY);
		transform.WorldMatrix = XMMatrixTranspose(world);
	}
}
