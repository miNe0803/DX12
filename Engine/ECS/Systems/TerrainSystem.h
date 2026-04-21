#pragma once

#include <entt/entt.hpp>
#include <DirectXMath.h>

namespace TerrainSystem
{
	float GetHeight(const entt::registry& registry, float worldX, float worldZ);

	/// Compute terrain surface normal at a world XZ position via finite differences.
	DirectX::XMFLOAT3 GetNormal(const entt::registry& registry, float worldX, float worldZ);
}
