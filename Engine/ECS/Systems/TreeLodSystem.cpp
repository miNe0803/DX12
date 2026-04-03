#include "TreeLodSystem.h"
#include "../Components.h"
#include "TreeVegetation.h"

void TreeLodSystem::Update(::entt::registry& registry)
{
	for (auto entity : registry.view<TreeInstanceTag, LODComponent, MeshRendererComponent>())
	{
		// 幹/葉/枝は ModelGroupChild 上のパート別マテリアル（g_partMat）を維持する
		if (registry.all_of<ModelGroupChildComponent>(entity))
			continue;
		const auto& tag = registry.get<TreeInstanceTag>(entity);
		const auto& lod = registry.get<LODComponent>(entity);
		auto& mesh = registry.get<MeshRendererComponent>(entity);
		const TreeSpeciesMaterials* mats = TreeVegetation::GetSpeciesMaterials(tag.SpeciesIndex);
		if (!mats)
			continue;
		DescriptorHandle* pick = mats->matLod0;
		if (lod.CurrentLODLevel <= 0 && mats->matLod0)
			pick = mats->matLod0;
		else if (lod.CurrentLODLevel == 1 && mats->matLod1)
			pick = mats->matLod1;
		else if (lod.CurrentLODLevel == 2 && mats->matLod2)
			pick = mats->matLod2;
		else if (mats->matLod0)
			pick = mats->matLod0;
		if (pick && mesh.MaterialHandle != pick)
			mesh.MaterialHandle = pick;
	}
}
