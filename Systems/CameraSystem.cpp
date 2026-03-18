#include "Systems/CameraSystem.h"
#include "Camera.h"
#include "Engine/ECS/Components.h"
#include <DirectXMath.h>

using namespace DirectX;

void CameraSystem::Update(Camera* camera, float dt, entt::registry& registry)
{
	if (!camera)
		return;

	auto view = registry.view<PlayerComponent, TransformComponent>();
	for (auto entity : view)
	{
		const auto& player = view.get<PlayerComponent>(entity);
		if (!player.FollowCamera)
			continue;

		const auto& transform = view.get<TransformComponent>(entity);
		XMVECTOR playerPos = XMLoadFloat3(&transform.Position);
		XMVECTOR off = XMLoadFloat3(&player.CameraOffset);
		XMVECTOR camPos = XMVectorAdd(playerPos, off);
		camera->SetPosition(camPos);

		XMFLOAT3 lookAt = transform.Position;
		lookAt.y += player.Height * 0.8f;
		camera->LookAt(XMLoadFloat3(&lookAt));
		return;
	}

	camera->Update(dt);
}
