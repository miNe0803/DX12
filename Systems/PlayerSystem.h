#pragma once

#include <entt/entt.hpp>

namespace PlayerSystem
{
	// WASD で移動＋進行方向へ向き＋接地。dt は Scene のフレーム時間。
	void Update(entt::registry& registry, float dt);
}
