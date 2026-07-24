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
class TreeGpuCullSystem;
class ShadowSystem;
class AtmosphereSystem;
struct AtmosphereParams;

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

	// VSM (Virtual Shadow Maps) ランタイムトグル（Debug UI 用。既定は環境変数 DX12_VSM で初期化）。
	// SetVsmEnabled(true) は有効化直後にアトラスを強制再描画する。
	void SetVsmEnabled(bool enabled);
	bool GetVsmEnabled() const;
	bool VsmAvailable() const;                 // VSM システムが Init 済みか（UIの活性判定）
	void SetVsmAtlasDebug(bool on);
	bool GetVsmAtlasDebug() const;
	void SetVsmShadowDebug(bool on);
	bool GetVsmShadowDebug() const;
	void SetVsmCache(bool on);                 // V5b 永続キャッシュ（移動時は新規ページのみ描画＝軽量）
	bool GetVsmCache() const;
	void SetVsmFootprintLod(bool on);          // Phase 1: フットプリントLOD（アイレベル擦過の崩壊を解消）
	bool GetVsmFootprintLod() const;
	uint32_t GetVsmLastPairCount() const;      // 直近フレームの (caster,page) ペア数（診断）
	uint32_t GetVsmResidentPages() const;      // V5b: 常駐ページ高水位（cap 4096）
	uint32_t GetVsmRequestedPages() const;     // Phase 0: 直近の要求ページ総数（フットプリントLOD効果の可視化）
	// DXR-GI F1 検証ビュー（primary ray ヒット距離 vs ラスタ深度の色分け）。TLAS構築(DX12_GI)が前提。
	void SetGiDebug(bool on);
	bool GetGiDebug() const;
	bool GetGiEnabled() const;                 // DX12_GI でTLAS構築済か（UI活性判定）
	// Phase R: レイトレースAO（RTAO）。ONでGTAOを置換。TLAS(DX12_GI)構築が前提。
	void SetRtaoEnabled(bool on);
	bool GetRtaoEnabled() const;
	bool RtaoAvailable() const;                // RtaoSystem生成済＋TLAS有効か（UI活性判定）
	// Phase G: DDGI 拡散GI（ONで町の偽ambientを実プローブirradiance＋太陽バウンスへ置換）。
	void SetDdgiEnabled(bool on);
	bool GetDdgiEnabled() const;
	bool DdgiAvailable() const;                // DdgiSystem生成済＋TLAS有効か
	bool GetDdgiReady() const;                 // ≥2フレーム蓄積済（町がサンプル開始）
	uint32_t GetDdgiProbeCount() const;
	uint32_t GetGiInstanceCount() const;       // TLAS インスタンス数（監視用）

	/// モデルグループ（親＋子メッシュ）の削除を予約。次フレームの Update 先頭で安全に削除される。
	void RequestDestroyEntity(entt::entity root);

	entt::registry& GetRegistry() { return m_registry; }
	// Debug UI (Hi-Z toggle, etc.)
	HiZSystem* GetHiZSystem() const;
	TerrainGpuCullSystem* GetTerrainGpuCullSystem() const;
	TreeGpuCullSystem* GetTreeGpuCullSystem() const;
	ShadowSystem* GetShadowSystem() const;
	AtmosphereParams& GetAtmosphereParams();
	const AtmosphereParams& GetAtmosphereParams() const;
	// Direct-instancing tree debug: actual per-frame LOD split counts (not ECS LODComponent).
	void GetDebugTreeDirectLodCounts(uint32_t& outLod0, uint32_t& outLod1, uint32_t& outLod2) const;

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
	bool InitShadowSystem();
	bool InitAtmosphereSystem();
	/// LOD0 が非同期で後から揃う場合に、初回だけインポスターアトラスをベイクする。
	void TryEnsureTreeImposterBake();
	/// LOD0 メッシュが揃った後に TreeGpuCullSystem を一度だけ確保する。
	void TryEnsureTreeGpuCullInit();

	entt::registry m_registry;
	std::vector<entt::entity> m_pendingDestroy;
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

	// Debug: updated in Draw() when using direct-instancing tree fallback.
	uint32_t m_debugTreeDirectLodCount[3] = { 0u, 0u, 0u };
};

extern Scene* g_Scene;

#endif
