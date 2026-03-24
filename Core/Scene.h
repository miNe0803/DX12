#ifndef SCENE_H
#define SCENE_H

#include <entt/entt.hpp>
#include <DirectXMath.h>
#include <vector>
#include "ModelSpawnOptions.h"
#include "SharedStruct.h"
#include "Graphics/PostProcessSettings.h"

class VertexBuffer;
class IndexBuffer;
struct ModelBounds;
class HiZSystem;
class TerrainGpuCullSystem;

class Scene
{
public:
	~Scene();
	bool Init();
	void Update();
	/// ImGui で g_NprGpuTuning を変えた直後に呼ぶと、当フレームの Draw で反映（Update は ImGui より前のため）
	void SyncNprGpuTuningToMaterialCB();
	/// ポストプロセス（Bloom 閾値・露出・トーンマップ等）の編集用
	PostProcessSettings& GetPostProcessSettings();
	const PostProcessSettings& GetPostProcessSettings() const;
	/// NPR トゥーン経路が実際に使われるか（NPRTag 数・NPR PSO 成否）
	void GetNprPathDiagnostics(size_t& outNprTagEntityCount, bool& outNprOpaquePsoValid, bool& outWillUseNprDrawPass) const;
	void Draw();
	void SetAsyncSpawnBudgetPerFrame(size_t budget);
	size_t GetAsyncSpawnBudgetPerFrame() const;
	void SetTerrainPsDebugMode(int mode);
	int GetTerrainPsDebugMode() const { return m_terrainPsDebugMode; }
	void SetTerrainCheapPathEnabled(bool enabled) { m_terrainCheapPathEnabled = enabled; }
	bool GetTerrainCheapPathEnabled() const { return m_terrainCheapPathEnabled; }
	void SetTerrainCheapGrazingThresh(float grazing01) { m_terrainCheapGrazingThresh = grazing01; }
	float GetTerrainCheapGrazingThresh() const { return m_terrainCheapGrazingThresh; }
	/// この距離より手前では、視線がそれほど浅くない限りチープ経路を使わない（0 で無効）
	void SetTerrainCheapNearPreserveMeters(float meters) { m_terrainCheapNearPreserveMeters = meters; }
	float GetTerrainCheapNearPreserveMeters() const { return m_terrainCheapNearPreserveMeters; }

	entt::registry& GetRegistry() { return m_registry; }
	// Debug UI (Hi-Z toggle, etc.)
	HiZSystem* GetHiZSystem() const;
	TerrainGpuCullSystem* GetTerrainGpuCullSystem() const;

private:
	bool InitDescriptorHeap();
	bool InitCameraAndFrameBuffers();
	bool InitPbrInstanceRingBuffer();
	bool SpawnModelEntities(const wchar_t* path, const ModelSpawnOptions& opt);
	/// Assimp 済みメッシュからエンティティ生成（メインスレッド専用）。非同期ロード完了時に使用。
	/// @param outEntitiesSpawned スポーンしたエンティティ数（任意）
	/// @param outFinalPosition スポーンに使ったワールド基準位置（足元スナップ後など）
	/// @return 1体以上スポーンできたら true（Assimp 成功と一致しない場合あり）
	bool SpawnLoadedMeshes(const wchar_t* path, std::vector<Mesh>&& loadedMeshes,
		const DirectX::XMFLOAT4X4& baseMatrix, const ModelSpawnOptions& opt,
		const ModelBounds* precomputedBounds = nullptr, size_t* outEntitiesSpawned = nullptr,
		DirectX::XMFLOAT3* outFinalPosition = nullptr);
	void ProcessAsyncModelLoads();
	bool InitTerrain();
	bool InitMainPipeline();
	bool InitSkyboxAndIBL();
	bool InitPostProcess();

	entt::registry m_registry;
	std::vector<VertexBuffer*> m_ownedVertexBuffers;
	std::vector<IndexBuffer*> m_ownedIndexBuffers;
	/// InitTerrain の共有メッシュ（チャンクが OwnsGpuBuffers=false のためここで解放）
	VertexBuffer* m_terrainSharedVB = nullptr;
	IndexBuffer* m_terrainSharedIB = nullptr;
	size_t m_asyncSpawnBudgetPerFrame = 1;
	int m_terrainPsDebugMode = 3;
	bool m_terrainCheapPathEnabled = true;
	float m_terrainCheapGrazingThresh = 0.26f;
	float m_terrainCheapNearPreserveMeters = 85.0f;
};

extern Scene* g_Scene;

#endif
