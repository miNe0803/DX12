#ifndef SCENE_H
#define SCENE_H

#include <entt/entt.hpp>
#include <DirectXMath.h>
#include <vector>
#include "ModelSpawnOptions.h"
#include "SharedStruct.h"

class VertexBuffer;
class IndexBuffer;
struct ModelBounds;

class Scene
{
public:
	~Scene();
	bool Init();
	void Update();
	void Draw();

	entt::registry& GetRegistry() { return m_registry; }

private:
	bool InitDescriptorHeap();
	bool InitCameraAndFrameBuffers();
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
};

extern Scene* g_Scene;

#endif
