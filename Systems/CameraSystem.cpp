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

		// Camera::LookAt はピッチが上下反転している（エンジン全体が 2*camY-targetY の鏡像で補正）。
		// TPS 追従でも同じ補正を掛けないと、プレイヤーより下を狙うと逆に空を見上げてしまう。
		const float camY = XMVectorGetY(camPos);
		const float desiredY = transform.Position.y + player.Height * 0.8f;
		XMFLOAT3 lookAt = transform.Position;
		lookAt.y = 2.0f * camY - desiredY;
		camera->LookAt(XMLoadFloat3(&lookAt));
		return;
	}

	camera->Update(dt);
}
