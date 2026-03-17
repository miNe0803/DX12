#include "TerrainGenerator.h"
#include "SharedStruct.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "Engine.h"
#include "EXRLoader.h"
#include <DirectXTex.h>
#include <DirectXMath.h>
#include <vector>
#include <algorithm>
#include <cassert>

#pragma comment(lib, "DirectXTex.lib")

using namespace DirectX;

namespace
{
	float SampleHeight(const uint8_t* pixels, UINT width, UINT height, size_t rowPitch, UINT i, UINT j)
	{
		UINT x = (std::min)(i, width > 0 ? width - 1u : 0u);
		UINT y = (std::min)(j, height > 0 ? height - 1u : 0u);
		const uint8_t* p = pixels + y * rowPitch + x * 4;
		float r = p[0] / 255.0f;
		float g = p[1] / 255.0f;
		float b = p[2] / 255.0f;
		return (r + g + b) / 3.0f;
	}

	void ComputeNormal(UINT i, UINT j, UINT w, UINT h, const std::vector<float>& heightData,
		float cellSpacing, float maxHeight, XMFLOAT3& outN)
	{
		float hC = heightData[i + j * w];
		float hL = (i > 0) ? heightData[(i - 1) + j * w] : hC;
		float hR = (i + 1 < w) ? heightData[(i + 1) + j * w] : hC;
		float hD = (j > 0) ? heightData[i + (j - 1) * w] : hC;
		float hU = (j + 1 < h) ? heightData[i + (j + 1) * w] : hC;

		float dx = 2.0f * cellSpacing;
		float dz = 2.0f * cellSpacing;
		XMVECTOR v1 = XMVectorSet(dx, (hR - hL) * maxHeight, 0.0f, 0.0f);
		XMVECTOR v2 = XMVectorSet(0.0f, (hU - hD) * maxHeight, dz, 0.0f);
		XMVECTOR n = XMVector3Cross(v1, v2);
		n = XMVector3Normalize(n);
		XMStoreFloat3(&outN, n);
	}
}

bool TerrainGenerator_GenerateFromFile(
	const wchar_t*     heightmapPath,
	float              cellSpacing,
	float              maxHeight,
	TerrainGenerateResult& out)
{
	DirectX::TexMetadata meta = {};
	DirectX::ScratchImage scratch = {};
	HRESULT hr = DirectX::LoadFromWICFile(heightmapPath, DirectX::WIC_FLAGS_NONE, &meta, scratch);
	if (FAILED(hr))
		return false;

	const DirectX::Image* img = scratch.GetImage(0, 0, 0);
	if (!img || !img->pixels)
		return false;

	UINT w = static_cast<UINT>(img->width);
	UINT h = static_cast<UINT>(img->height);
	if (w == 0 || h == 0)
		return false;

	size_t rowPitch = img->rowPitch;
	// HeightData: 1 sample per pixel (width * height)
	out.HeightData.resize(w * h);
	for (UINT j = 0; j < h; j++)
		for (UINT i = 0; i < w; i++)
			out.HeightData[i + j * w] = SampleHeight(img->pixels, w, h, rowPitch, i, j);

	out.GridWidth  = w;
	out.GridDepth  = h;

	// Vertices: (w+1) * (h+1) でグリッド頂点
	UINT numVerts = (w + 1) * (h + 1);
	std::vector<Vertex> vertices(numVerts);

	for (UINT j = 0; j <= h; j++)
	{
		for (UINT i = 0; i <= w; i++)
		{
			UINT ii = (std::min)(i, w - 1);
			UINT jj = (std::min)(j, h - 1);
			float height = out.HeightData[ii + jj * w] * maxHeight;

			Vertex v = {};
			v.Position = XMFLOAT3(
				static_cast<float>(i) * cellSpacing,
				height,
				static_cast<float>(j) * cellSpacing
			);
			v.UV = XMFLOAT2(
				(w > 0) ? static_cast<float>(i) / static_cast<float>(w) : 0.0f,
				(h > 0) ? static_cast<float>(j) / static_cast<float>(h) : 0.0f
			);
			// 法線: 隣接サンプルから計算（頂点はグリッド点なので ii,jj でサンプル参照）
			ComputeNormal(ii, jj, w, h, out.HeightData, cellSpacing, maxHeight, v.Normal);
			v.Tangent = XMFLOAT3(1.0f, 0.0f, 0.0f);
			v.Color   = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
			v.BoneIndex[0] = v.BoneIndex[1] = v.BoneIndex[2] = v.BoneIndex[3] = 0;
			v.BoneWeight[0] = 1.0f; v.BoneWeight[1] = v.BoneWeight[2] = v.BoneWeight[3] = 0.0f;

			vertices[i + j * (w + 1)] = v;
		}
	}

	// Indices: 2 triangles per cell
	std::vector<uint32_t> indices;
	indices.reserve(w * h * 6);
	for (UINT j = 0; j < h; j++)
	{
		for (UINT i = 0; i < w; i++)
		{
			UINT v00 = i + j * (w + 1);
			UINT v10 = (i + 1) + j * (w + 1);
			UINT v01 = i + (j + 1) * (w + 1);
			UINT v11 = (i + 1) + (j + 1) * (w + 1);
			indices.push_back(v00); indices.push_back(v10); indices.push_back(v01);
			indices.push_back(v10); indices.push_back(v11); indices.push_back(v01);
		}
	}

	out.IndexCount = static_cast<UINT>(indices.size());
	out.pVB = new VertexBuffer(numVerts * sizeof(Vertex), sizeof(Vertex), vertices.data());
	out.pIB = new IndexBuffer(indices.size() * sizeof(uint32_t), indices.data());
	if (!out.pVB->IsValid() || !out.pIB->IsValid())
	{
		delete out.pVB;
		delete out.pIB;
		out.pVB = nullptr;
		out.pIB = nullptr;
		return false;
	}
	return true;
}

bool TerrainGenerator_GenerateFromExr(
	const char*        pathUtf8,
	float              cellSpacing,
	float              maxHeight,
	TerrainGenerateResult& out)
{
	int w = 0, h = 0;
	float* rgba = nullptr;
	if (!LoadEXRToFloatRgba(pathUtf8, &w, &h, &rgba) || w <= 0 || h <= 0 || !rgba)
		return false;

	const int total = w * h;
	out.HeightData.resize(static_cast<size_t>(total));
	for (int i = 0; i < total; i++)
	{
		const float r = rgba[i * 4 + 0];
		const float g = rgba[i * 4 + 1];
		const float b = rgba[i * 4 + 2];
		float heightNorm = (r + g + b) / 3.0f;
		if (heightNorm < 0.0f) heightNorm = 0.0f;
		if (heightNorm > 1.0f) heightNorm = 1.0f;
		out.HeightData[static_cast<size_t>(i)] = heightNorm;
	}
	free(rgba);

	out.GridWidth  = static_cast<UINT>(w);
	out.GridDepth  = static_cast<UINT>(h);
	const UINT uw = out.GridWidth, uh = out.GridDepth;

	UINT numVerts = (uw + 1) * (uh + 1);
	std::vector<Vertex> vertices(numVerts);

	for (UINT j = 0; j <= uh; j++)
	{
		for (UINT i = 0; i <= uw; i++)
		{
			UINT ii = (std::min)(i, uw - 1);
			UINT jj = (std::min)(j, uh - 1);
			float height = out.HeightData[ii + jj * uw] * maxHeight;

			Vertex v = {};
			v.Position = XMFLOAT3(
				static_cast<float>(i) * cellSpacing,
				height,
				static_cast<float>(j) * cellSpacing
			);
			v.UV = XMFLOAT2(
				(uw > 0) ? static_cast<float>(i) / static_cast<float>(uw) : 0.0f,
				(uh > 0) ? static_cast<float>(j) / static_cast<float>(uh) : 0.0f
			);
			ComputeNormal(ii, jj, uw, uh, out.HeightData, cellSpacing, maxHeight, v.Normal);
			v.Tangent = XMFLOAT3(1.0f, 0.0f, 0.0f);
			v.Color   = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
			v.BoneIndex[0] = v.BoneIndex[1] = v.BoneIndex[2] = v.BoneIndex[3] = 0;
			v.BoneWeight[0] = 1.0f; v.BoneWeight[1] = v.BoneWeight[2] = v.BoneWeight[3] = 0.0f;
			vertices[i + j * (uw + 1)] = v;
		}
	}

	std::vector<uint32_t> indices;
	indices.reserve(uw * uh * 6);
	for (UINT j = 0; j < uh; j++)
		for (UINT i = 0; i < uw; i++)
		{
			UINT v00 = i + j * (uw + 1);
			UINT v10 = (i + 1) + j * (uw + 1);
			UINT v01 = i + (j + 1) * (uw + 1);
			UINT v11 = (i + 1) + (j + 1) * (uw + 1);
			indices.push_back(v00); indices.push_back(v10); indices.push_back(v01);
			indices.push_back(v10); indices.push_back(v11); indices.push_back(v01);
		}

	out.IndexCount = static_cast<UINT>(indices.size());
	out.pVB = new VertexBuffer(numVerts * sizeof(Vertex), sizeof(Vertex), vertices.data());
	out.pIB = new IndexBuffer(indices.size() * sizeof(uint32_t), indices.data());
	if (!out.pVB->IsValid() || !out.pIB->IsValid())
	{
		delete out.pVB;
		delete out.pIB;
		out.pVB = nullptr;
		out.pIB = nullptr;
		return false;
	}
	return true;
}

void TerrainGenerator_ReleaseResult(TerrainGenerateResult& result)
{
	result.HeightData.clear();
	delete result.pVB;
	delete result.pIB;
	result.pVB = nullptr;
	result.pIB = nullptr;
	result.IndexCount = result.GridWidth = result.GridDepth = 0;
}
