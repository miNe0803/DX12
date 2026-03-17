#include "TerrainSystem.h"
#include "../Components.h"
#include <cmath>
#include <algorithm>

float TerrainSystem::GetHeight(const entt::registry& registry, float worldX, float worldZ)
{
	auto view = registry.view<TerrainComponent>();
	if (view.begin() == view.end())
		return 0.0f;

	entt::entity entity = *view.begin();
	const auto& terrain = view.get<TerrainComponent>(entity);

	if (terrain.HeightData.empty() || terrain.GridWidth == 0 || terrain.GridDepth == 0 || terrain.CellSpacing <= 0.0f)
		return 0.0f;

	float gx = worldX / terrain.CellSpacing;
	float gz = worldZ / terrain.CellSpacing;

	int ix0 = static_cast<int>(std::floor(gx));
	int iz0 = static_cast<int>(std::floor(gz));
	int ix1 = ix0 + 1;
	int iz1 = iz0 + 1;

	const int w = static_cast<int>(terrain.GridWidth);
	const int h = static_cast<int>(terrain.GridDepth);
	ix0 = (std::max)(0, (std::min)(ix0, w - 1));
	ix1 = (std::max)(0, (std::min)(ix1, w - 1));
	iz0 = (std::max)(0, (std::min)(iz0, h - 1));
	iz1 = (std::max)(0, (std::min)(iz1, h - 1));

	float h00 = terrain.HeightData[ix0 + iz0 * terrain.GridWidth];
	float h10 = terrain.HeightData[ix1 + iz0 * terrain.GridWidth];
	float h01 = terrain.HeightData[ix0 + iz1 * terrain.GridWidth];
	float h11 = terrain.HeightData[ix1 + iz1 * terrain.GridWidth];

	float fx = gx - std::floor(gx);
	float fz = gz - std::floor(gz);
	float h0 = h00 + (h10 - h00) * fx;
	float h1 = h01 + (h11 - h01) * fx;
	float heightNorm = h0 + (h1 - h0) * fz;

	return heightNorm * terrain.MaxHeight;
}
