#pragma once

#include <entt/entt.hpp>

class Camera;

namespace CameraSystem
{
	// FollowCamera==true のプレイヤーがいれば追従、いなければ camera->Update(dt)（自由視点）
	void Update(Camera* camera, float dt, entt::registry& registry);
}
