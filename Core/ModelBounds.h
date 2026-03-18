#pragma once
#include <algorithm>
#include <DirectXMath.h>
#include <vector>
#include "SharedStruct.h"

struct ModelBounds
{
	DirectX::XMFLOAT3 Min = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 Max = { 0.0f, 0.0f, 0.0f };
};

inline ModelBounds ComputeModelBounds(const std::vector<Mesh>& modelMeshes)
{
	ModelBounds bounds{};
	bool initialized = false;
	for (const auto& mesh : modelMeshes)
	{
		for (const auto& v : mesh.Vertices)
		{
			if (!initialized)
			{
				bounds.Min = bounds.Max = v.Position;
				initialized = true;
				continue;
			}
			bounds.Min.x = (std::min)(bounds.Min.x, v.Position.x);
			bounds.Min.y = (std::min)(bounds.Min.y, v.Position.y);
			bounds.Min.z = (std::min)(bounds.Min.z, v.Position.z);
			bounds.Max.x = (std::max)(bounds.Max.x, v.Position.x);
			bounds.Max.y = (std::max)(bounds.Max.y, v.Position.y);
			bounds.Max.z = (std::max)(bounds.Max.z, v.Position.z);
		}
	}
	return bounds;
}
