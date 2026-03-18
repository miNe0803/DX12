#include "Systems/PlayerSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/TerrainSystem.h"

void PlayerSystem::Update(entt::registry& registry)
{
	auto view = registry.view<PlayerComponent, TransformComponent>();
	for (auto entity : view)
	{
		auto& player = view.get<PlayerComponent>(entity);
		auto& transform = view.get<TransformComponent>(entity);
		float ground = TerrainSystem::GetHeight(registry, transform.Position.x, transform.Position.z);
		transform.Position.y = ground + player.GroundOffset;
	}
}
