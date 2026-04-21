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

	const float halfWidth = static_cast<float>(terrain.GridWidth) * terrain.CellSpacing * 0.5f;
	const float halfDepth = static_cast<float>(terrain.GridDepth) * terrain.CellSpacing * 0.5f;
	float gx = (worldX + halfWidth) / terrain.CellSpacing;
	float gz = (worldZ + halfDepth) / terrain.CellSpacing;

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

DirectX::XMFLOAT3 TerrainSystem::GetNormal(const entt::registry& registry, float worldX, float worldZ)
{
	auto view = registry.view<TerrainComponent>();
	if (view.begin() == view.end())
		return DirectX::XMFLOAT3(0.f, 1.f, 0.f);

	const auto& terrain = view.get<TerrainComponent>(*view.begin());
	const float eps = terrain.CellSpacing;

	const float hL = GetHeight(registry, worldX - eps, worldZ);
	const float hR = GetHeight(registry, worldX + eps, worldZ);
	const float hD = GetHeight(registry, worldX, worldZ - eps);
	const float hU = GetHeight(registry, worldX, worldZ + eps);

	DirectX::XMVECTOR n = DirectX::XMVector3Normalize(
		DirectX::XMVectorSet(hL - hR, 2.f * eps, hD - hU, 0.f));
	DirectX::XMFLOAT3 result;
	DirectX::XMStoreFloat3(&result, n);
	return result;
}
