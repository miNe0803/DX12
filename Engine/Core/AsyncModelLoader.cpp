#include "Engine/Core/AsyncModelLoader.h"

#include <assimpLoader.h>
#include "DebugLog.h"
#include <filesystem>
#include <exception>

namespace fs = std::filesystem;

namespace
{
	/// TreeVegetation 等と同様: CWD が exe 直下でもプロジェクトルートでも解決できるようにする
	std::wstring ResolveAssetPath(const std::wstring& path)
	{
		if (path.empty())
			return path;
		if (fs::exists(path))
			return path;
		const fs::path p2 = fs::path(L"..\\..") / path;
		if (fs::exists(p2))
			return p2.wstring();
		const fs::path p3 = fs::path(L"..\\..\\..") / path;
		if (fs::exists(p3))
			return p3.wstring();
		return path;
	}

	struct WorkerBusyGuard
	{
		std::atomic<bool>& flag;
		explicit WorkerBusyGuard(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
		~WorkerBusyGuard() { flag.store(false, std::memory_order_release); }
	};
}

AsyncModelLoader* g_AsyncModelLoader = nullptr;

void AsyncModelLoader::ReportSpawnResult(std::wstring path, bool ok, size_t entityCount, const wchar_t* detail,
	float wx, float wy, float wz)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_lastSpawn.hasValue = true;
	m_lastSpawn.ok = ok;
	m_lastSpawn.entityCount = entityCount;
	m_lastSpawn.path = std::move(path);
	m_lastSpawn.detail = detail ? detail : L"";
	m_lastSpawn.worldX = wx;
	m_lastSpawn.worldY = wy;
	m_lastSpawn.worldZ = wz;
}

AsyncModelLoader::AsyncModelLoader()
{
	m_worker = std::thread([this] { WorkerLoop(); });
}

AsyncModelLoader::~AsyncModelLoader()
{
	Stop();
	std::lock_guard<std::mutex> lock(m_mutex);
	m_completed.clear();
	m_pending.clear();
}

void AsyncModelLoader::Stop()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_cv.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

void AsyncModelLoader::RequestLoad(std::wstring path, ModelSpawnOptions opt)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stop)
			return;
		m_pending.push_back(PendingJob{ std::move(path), opt });
	}
	m_cv.notify_one();
}

void AsyncModelLoader::DrainCompleted(std::function<void(AsyncModelLoadResult&&)> fn)
{
	std::deque<AsyncModelLoadResult> done;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		done.swap(m_completed);
	}
	for (auto& r : done)
		fn(std::move(r));
}

void AsyncModelLoader::DrainCompleted(size_t maxCount, std::function<void(AsyncModelLoadResult&&)> fn)
{
	if (maxCount == 0)
		return;

	std::deque<AsyncModelLoadResult> done;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (size_t i = 0; i < maxCount && !m_completed.empty(); ++i)
		{
			done.push_back(std::move(m_completed.front()));
			m_completed.pop_front();
		}
	}
	for (auto& r : done)
		fn(std::move(r));
}

void AsyncModelLoader::WorkerLoop()
{
	for (;;)
	{
		PendingJob job;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] {
				return m_stop || !m_pending.empty();
			});
			if (m_stop && m_pending.empty())
				return;
			if (m_pending.empty())
				continue;
			job = std::move(m_pending.front());
			m_pending.pop_front();
		}

		WorkerBusyGuard busy(m_workerBusy);

		AsyncModelLoadResult out;
		const std::wstring resolved = ResolveAssetPath(job.path);
		out.filePath = resolved;
		out.options = job.opt;

		if (!fs::exists(resolved))
		{
			out.success = false;
			DebugLog("[AsyncLoad] FAIL file not found (resolved): %ls (input was %ls)\n",
				resolved.c_str(), job.path.c_str());
		}
		else
		{
			std::vector<Mesh> meshes;
			ImportSettings imp(resolved.c_str(), meshes, false, true, 1.0f);
			imp.outClips = nullptr;
			AssimpLoader loader;
			try
			{
				out.success = loader.Load(imp);
				out.baseTransform = imp.outBaseTransform;
				if (out.success)
				{
					out.meshes = std::move(meshes);
					out.bounds = ComputeModelBounds(out.meshes);
				}
			}
			catch (const std::exception& e)
			{
				out.success = false;
				DebugLog("[AsyncLoad] exception (std::exception): %s  path=%ls\n", e.what(), resolved.c_str());
			}
			catch (...)
			{
				out.success = false;
				DebugLog("[AsyncLoad] exception (unknown) path=%ls\n", resolved.c_str());
			}
		}

		DebugLog("[AsyncLoad] %ls %s meshes=%zu\n",
			out.filePath.c_str(),
			out.success ? "OK" : "FAIL",
			out.success ? out.meshes.size() : 0u);

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stop)
				return;
			m_lastStatus.hasValue = true;
			m_lastStatus.success = out.success;
			m_lastStatus.meshCount = out.success ? out.meshes.size() : 0u;
			m_lastStatus.path = out.filePath;
			m_completed.push_back(std::move(out));
		}
	}
}
