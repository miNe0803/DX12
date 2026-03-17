#pragma once

#include <entt/entt.hpp>
#include <DirectXMath.h>

namespace LODSystem
{
	void Update(entt::registry& registry, const DirectX::XMFLOAT3& cameraPos);
}
