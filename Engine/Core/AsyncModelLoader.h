#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <DirectXMath.h>
#include "Core/ModelBounds.h"
#include "Core/ModelSpawnOptions.h"
#include "SharedStruct.h"

/// ワーカー完了後、メインスレッドで Scene::SpawnLoadedMeshes に渡す結果
struct AsyncModelLoadResult
{
	std::wstring filePath;
	ModelSpawnOptions options{};
	std::vector<Mesh> meshes;
	DirectX::XMFLOAT4X4 baseTransform{};
	ModelBounds bounds{};
	bool success = false;
};

/// Assimp パースのみワーカー。GPU / ECS はメインスレッド。
class AsyncModelLoader
{
public:
	AsyncModelLoader();
	~AsyncModelLoader();

	void RequestLoad(std::wstring path, ModelSpawnOptions opt);

	/// メインスレーム（例: Scene::Update 先頭）で呼ぶ。完了分をすべて fn に渡す。
	void DrainCompleted(std::function<void(AsyncModelLoadResult&&)> fn);
	/// 完了分のうち最大 maxCount 件だけ fn に渡す（残りはキューに保持）
	void DrainCompleted(size_t maxCount, std::function<void(AsyncModelLoadResult&&)> fn);

	void Stop();

	size_t PendingLoadCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_pending.size();
	}

	/// キューから取り出し済みで、ワーカーが Assimp 処理中（メインはまだ Drain していない）
	bool IsWorkerBusy() const { return m_workerBusy.load(std::memory_order_acquire); }

	struct LastResultStatus
	{
		bool hasValue = false;
		bool success = false;
		size_t meshCount = 0;
		std::wstring path;
	};
	LastResultStatus GetLastResultStatus() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_lastStatus;
	}

	/// メインスレッド: Scene スポーン後の実結果（Assimp 成功と別）
	struct LastSpawnStatus
	{
		bool hasValue = false;
		bool ok = false;
		size_t entityCount = 0;
		std::wstring path;
		std::wstring detail;
		float worldX = 0.f, worldY = 0.f, worldZ = 0.f;
	};
	void ReportSpawnResult(std::wstring path, bool ok, size_t entityCount, const wchar_t* detail,
		float wx = 0.f, float wy = 0.f, float wz = 0.f);
	LastSpawnStatus GetLastSpawnStatus() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_lastSpawn;
	}

private:
	void WorkerLoop();

	struct PendingJob
	{
		std::wstring path;
		ModelSpawnOptions opt;
	};

	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<PendingJob> m_pending;
	std::deque<AsyncModelLoadResult> m_completed;
	LastResultStatus m_lastStatus;
	LastSpawnStatus m_lastSpawn;

	std::thread m_worker;
	std::atomic<bool> m_stop{ false };
	std::atomic<bool> m_workerBusy{ false };
};

extern AsyncModelLoader* g_AsyncModelLoader;
