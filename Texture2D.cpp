#include "Texture2D.h"
#include <DirectXTex.h>
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <mutex>
#include <unordered_map>
#include <string>

#pragma comment(lib, "DirectXTex.lib")

namespace {
	std::mutex g_texMutex;
	std::unordered_map<std::wstring, Texture2D*> g_pathCache;
	Texture2D* g_white = nullptr;
	Texture2D* g_black = nullptr;
	Texture2D* g_metal = nullptr;
	Texture2D* g_rough = nullptr;
	Texture2D* g_nprRamp = nullptr;
}

using namespace DirectX;

// std::string(?}???`?o?C?g)????std::wstring(???C?h)??BAssimpLoader????l??p?r?p
std::wstring GetWideString(const std::string& str)
{
	auto num1 = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, str.c_str(), -1, nullptr, 0);

	std::wstring wstr;
	wstr.resize(num1);

	auto num2 = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, str.c_str(), -1, &wstr[0], num1);

	assert(num1 == num2);
	return wstr;
}

// ?g???q????
std::wstring FileExtension(const std::wstring& path)
{
	auto idx = path.rfind(L'.');
	return path.substr(idx + 1, path.length() - idx - 1);
}

Texture2D::Texture2D(std::string path)
{
	m_IsValid = Load(path);
}

Texture2D::Texture2D(std::wstring path)
{
	m_IsValid = Load(path);
}

Texture2D::Texture2D(ID3D12Resource* buffer)
{
	m_pResource = buffer;
	m_IsValid = m_pResource != nullptr;
}

bool Texture2D::Load(std::string& path)
{
	auto wpath = GetWideString(path);
	return Load(wpath);
}

bool Texture2D::Load(std::wstring& path)
{
	// WIC??e?N?X?`?????[?h
	TexMetadata meta = {};
	ScratchImage scratch = {};
	auto ext = FileExtension(path);
	for (auto& c : ext)
	{
		if (c >= L'A' && c <= L'Z')
			c += (L'a' - L'A');
	}

	HRESULT hr = E_FAIL;
	if (ext == L"tga")
	{
		hr = LoadFromTGAFile(path.c_str(), &meta, scratch);
	}
	else
	{
		// PNG / BMP / JPEG / GIF / TIFF など WIC 対応形式（PMX 付属の .bmp / .jpg 用）
		hr = LoadFromWICFile(path.c_str(), WIC_FLAGS_NONE, &meta, scratch);
	}

	if (FAILED(hr))
	{
		return false;
	}

	// D3D12????E??????0????\?[?X??????????
	if (meta.width == 0 || meta.height == 0)
		return false;

	const UINT16 arraySize = (meta.arraySize > 0) ? static_cast<UINT16>(meta.arraySize) : 1;
	const UINT16 mipLevels = (meta.mipLevels > 0) ? static_cast<UINT16>(meta.mipLevels) : 1;

	auto img = scratch.GetImage(0, 0, 0);
	if (img == nullptr)
		return false;

	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0);
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(meta.format,
		meta.width,
		meta.height,
		arraySize,
		mipLevels);

	// ???\?[?X????
	hr = g_Engine->Device()->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(m_pResource.ReleaseAndGetAddressOf())
	);

	if (FAILED(hr))
	{
		return false;
	}

	hr = m_pResource->WriteToSubresource(0,
		nullptr,   // ?S????R?s?[
		img->pixels,
		static_cast<UINT>(img->rowPitch),
		static_cast<UINT>(img->slicePitch)
	);
	if (FAILED(hr))
	{
		return false;
	}

	{
		std::wstring label = L"Tex2D:";
		const size_t slash = path.find_last_of(L"/\\");
		label += (slash == std::wstring::npos) ? path : path.substr(slash + 1);
		GPU_SET_NAME(m_pResource.Get(), label.c_str());
	}

	return true;
}

Texture2D* Texture2D::Get(std::string path)
{
	auto wpath = GetWideString(path);
	return Get(wpath);
}

Texture2D* Texture2D::Get(std::wstring path)
{
	if (!g_Engine || !g_Engine->Device())
		return nullptr;
	std::lock_guard<std::mutex> lock(g_texMutex);
	auto it = g_pathCache.find(path);
	if (it != g_pathCache.end())
		return it->second;
	auto* tex = new Texture2D(path);
	if (!tex->IsValid())
	{
		delete tex;
		if (!g_white)
		{
			ID3D12Resource* buff = GetDefaultResource(4, 4);
			if (!buff) return nullptr;
			std::vector<unsigned char> data(4 * 4 * 4);
			std::fill(data.begin(), data.end(), 0xff);
			if (FAILED(buff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, data.size())))
			{
				buff->Release();
				return nullptr;
			}
			g_white = new Texture2D(buff);
		}
		return g_white;
	}
	g_pathCache[path] = tex;
	return tex;
}

Texture2D* Texture2D::GetWhite()
{
	if (!g_Engine || !g_Engine->Device())
		return nullptr;
	std::lock_guard<std::mutex> lock(g_texMutex);
	if (g_white)
		return g_white;
	ID3D12Resource* buff = GetDefaultResource(4, 4);
	if (!buff) return nullptr;
	std::vector<unsigned char> data(4 * 4 * 4);
	std::fill(data.begin(), data.end(), 0xff);
	if (FAILED(buff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, data.size())))
	{
		buff->Release();
		return nullptr;
	}
	g_white = new Texture2D(buff);
	return g_white;
}

Texture2D* Texture2D::GetDefaultMetallic()
{
	if (!g_Engine || !g_Engine->Device())
		return nullptr;
	std::lock_guard<std::mutex> lock(g_texMutex);
	if (g_metal)
		return g_metal;
	ID3D12Resource* buff = GetDefaultResource(4, 4);
	if (!buff) return nullptr;
	std::vector<unsigned char> data(4 * 4 * 4);
	std::fill(data.begin(), data.end(), 0x00);
	if (FAILED(buff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, data.size())))
	{
		buff->Release();
		return nullptr;
	}
	g_metal = new Texture2D(buff);
	return g_metal;
}

Texture2D* Texture2D::GetDefaultRoughness()
{
	if (!g_Engine || !g_Engine->Device())
		return nullptr;
	std::lock_guard<std::mutex> lock(g_texMutex);
	if (g_rough)
		return g_rough;
	ID3D12Resource* buff = GetDefaultResource(4, 4);
	if (!buff) return nullptr;
	const unsigned char byteVal = 235;
	std::vector<unsigned char> data(4 * 4 * 4);
	for (size_t i = 0; i < data.size(); i += 4)
	{
		data[i + 0] = byteVal;
		data[i + 1] = byteVal;
		data[i + 2] = byteVal;
		data[i + 3] = 0xff;
	}
	if (FAILED(buff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, data.size())))
	{
		buff->Release();
		return nullptr;
	}
	g_rough = new Texture2D(buff);
	return g_rough;
}

Texture2D* Texture2D::GetDefaultNprRamp()
{
	if (!g_Engine || !g_Engine->Device())
		return nullptr;
	std::lock_guard<std::mutex> lock(g_texMutex);
	if (g_nprRamp)
		return g_nprRamp;
	const UINT w = 256;
	const UINT h = 1;
	ID3D12Resource* buff = GetDefaultResource(w, h);
	if (!buff)
		return nullptr;
	std::vector<unsigned char> data(static_cast<size_t>(w) * h * 4);
	const UINT denom = (w > 1) ? (w - 1) : 1u;
	for (UINT x = 0; x < w; ++x)
	{
		const unsigned char v = static_cast<unsigned char>((x * 255u) / denom);
		const size_t i = static_cast<size_t>(x) * 4;
		data[i + 0] = v;
		data[i + 1] = v;
		data[i + 2] = v;
		data[i + 3] = 0xff;
	}
	const UINT rowPitch = w * 4;
	if (FAILED(buff->WriteToSubresource(0, nullptr, data.data(), rowPitch, rowPitch * h)))
	{
		buff->Release();
		return nullptr;
	}
	g_nprRamp = new Texture2D(buff);
	return g_nprRamp;
}

Texture2D* Texture2D::GetBlack()
{
	if (!g_Engine || !g_Engine->Device())
		return nullptr;
	std::lock_guard<std::mutex> lock(g_texMutex);
	if (g_black)
		return g_black;
	ID3D12Resource* buff = GetDefaultResource(4, 4);
	if (!buff) return nullptr;
	std::vector<unsigned char> data(4 * 4 * 4);
	std::fill(data.begin(), data.end(), 0x00);
	if (FAILED(buff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, data.size())))
	{
		buff->Release();
		return nullptr;
	}
	g_black = new Texture2D(buff);
	return g_black;
}

void Texture2D::ReleaseAllDeviceResources()
{
	std::lock_guard<std::mutex> lock(g_texMutex);
	for (auto& kv : g_pathCache)
		delete kv.second;
	g_pathCache.clear();
	delete g_white;
	g_white = nullptr;
	delete g_black;
	g_black = nullptr;
	delete g_metal;
	g_metal = nullptr;
	delete g_rough;
	g_rough = nullptr;
	delete g_nprRamp;
	g_nprRamp = nullptr;
}

ID3D12Resource* Texture2D::GetDefaultResource(size_t width, size_t height)
{
	auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height);
	auto texHeapProp = CD3DX12_HEAP_PROPERTIES(D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0);
	ID3D12Resource* buff = nullptr;
	auto result = g_Engine->Device()->CreateCommittedResource(
		&texHeapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&buff)
	);
	if (FAILED(result))
	{
		assert(SUCCEEDED(result));
		return nullptr;
	}
	return buff;
}

bool Texture2D::IsValid()
{
	return m_IsValid;
}

ID3D12Resource* Texture2D::Resource()
{
	return m_pResource.Get();
}

D3D12_SHADER_RESOURCE_VIEW_DESC Texture2D::ViewDesc()
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = m_pResource->GetDesc().Format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MipLevels = 1;
	return desc;
}
