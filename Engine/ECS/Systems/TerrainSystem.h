#pragma once

#include <entt/entt.hpp>

namespace TerrainSystem
{
	float GetHeight(const entt::registry& registry, float worldX, float worldZ);
}
