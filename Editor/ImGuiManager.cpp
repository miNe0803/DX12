#include "Editor/ImGuiManager.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <d3d12.h>
#include <Windows.h>

namespace {

constexpr int kSrvHeapSize = 64;

struct SrvHeapAllocator
{
	ID3D12DescriptorHeap* heap = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
	D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
	UINT increment = 0;
	std::vector<int> freeIndices;

	void Create(ID3D12Device* device, ID3D12DescriptorHeap* h)
	{
		heap = h;
		D3D12_DESCRIPTOR_HEAP_DESC d = h->GetDesc();
		cpuStart = h->GetCPUDescriptorHandleForHeapStart();
		gpuStart = h->GetGPUDescriptorHandleForHeapStart();
		increment = device->GetDescriptorHandleIncrementSize(d.Type);
		freeIndices.clear();
		for (int n = (int)d.NumDescriptors; n > 0; --n)
			freeIndices.push_back(n - 1);
	}
	void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
	{
		int idx = freeIndices.back();
		freeIndices.pop_back();
		outCpu->ptr = cpuStart.ptr + static_cast<SIZE_T>(idx) * increment;
		outGpu->ptr = gpuStart.ptr + static_cast<SIZE_T>(idx) * increment;
	}
	void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
	{
		int i = static_cast<int>((cpu.ptr - cpuStart.ptr) / increment);
		(void)gpu;
		freeIndices.push_back(i);
	}
};

static void SrvAllocFn(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
{
	static_cast<SrvHeapAllocator*>(info->UserData)->Alloc(outCpu, outGpu);
}
static void SrvFreeFn(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
	static_cast<SrvHeapAllocator*>(info->UserData)->Free(cpu, gpu);
}

} // namespace

ImGuiManager::~ImGuiManager()
{
	Shutdown();
}

bool ImGuiManager::Init(ID3D12Device* device, ID3D12CommandQueue* queue, void* hwnd, int numFramesInFlight, DXGI_FORMAT rtvFormat)
{
	if (m_initialized || !device || !queue || !hwnd)
		return false;

	m_device = device;
	m_queue = queue;
	m_numFrames = numFramesInFlight;
	m_rtvFormat = rtvFormat;

	D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = kSrvHeapSize;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap))))
		return false;

	auto* alloc = new SrvHeapAllocator();
	alloc->Create(device, m_srvHeap);
	m_srvAlloc = alloc;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::StyleColorsDark();

	// DPI（Windows の表示スケール）に追従して、UI が小さすぎる問題を回避
	// ※ DPI aware の有効化自体は「ウィンドウ生成前」に App 側で実施する。
	const float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(static_cast<HWND>(hwnd));
	if (dpiScale > 0.0f && dpiScale != 1.0f)
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style.ScaleAllSizes(dpiScale);
		io.FontGlobalScale = dpiScale;
	}

	ImGui_ImplWin32_Init(hwnd);

	ImGui_ImplDX12_InitInfo initInfo = {};
	initInfo.Device = device;
	initInfo.CommandQueue = queue;
	initInfo.NumFramesInFlight = numFramesInFlight;
	initInfo.RTVFormat = rtvFormat;
	initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
	initInfo.UserData = static_cast<SrvHeapAllocator*>(m_srvAlloc);
	initInfo.SrvDescriptorHeap = m_srvHeap;
	initInfo.SrvDescriptorAllocFn = SrvAllocFn;
	initInfo.SrvDescriptorFreeFn = SrvFreeFn;
	if (!ImGui_ImplDX12_Init(&initInfo))
	{
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		m_srvHeap->Release();
		m_srvHeap = nullptr;
		delete static_cast<SrvHeapAllocator*>(m_srvAlloc);
		m_srvAlloc = nullptr;
		return false;
	}

	m_initialized = true;
	return true;
}

void ImGuiManager::Shutdown()
{
	if (!m_initialized)
	{
		if (m_srvHeap)
		{
			m_srvHeap->Release();
			m_srvHeap = nullptr;
		}
		delete static_cast<SrvHeapAllocator*>(m_srvAlloc);
		m_srvAlloc = nullptr;
		return;
	}
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	if (m_srvHeap)
	{
		m_srvHeap->Release();
		m_srvHeap = nullptr;
	}
	delete static_cast<SrvHeapAllocator*>(m_srvAlloc);
	m_srvAlloc = nullptr;
	m_initialized = false;
}

void ImGuiManager::NewFrame()
{
	if (!m_initialized)
		return;
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

void ImGuiManager::Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
	UINT renderWidth, UINT renderHeight)
{
	if (!m_initialized || !commandList)
		return;

	ImGui::Render();
	ImDrawData* dd = ImGui::GetDrawData();
	if (!dd || dd->CmdListsCount == 0)
		return;

	D3D12_VIEWPORT vp = {};
	vp.Width = static_cast<float>(renderWidth);
	vp.Height = static_cast<float>(renderHeight);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	commandList->RSSetViewports(1, &vp);
	D3D12_RECT sr = { 0, 0, static_cast<LONG>(renderWidth), static_cast<LONG>(renderHeight) };
	commandList->RSSetScissorRects(1, &sr);

	commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { m_srvHeap };
	commandList->SetDescriptorHeaps(1, heaps);

	ImGui_ImplDX12_RenderDrawData(dd, commandList);
}
