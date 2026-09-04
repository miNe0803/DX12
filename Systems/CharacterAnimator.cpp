#include "Systems/CharacterAnimator.h"
#include "Engine/ECS/Components.h"

#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

using namespace DirectX;
namespace fs = std::filesystem;

namespace
{
	struct Clip
	{
		std::string name;
		uint32_t N = 0, F = 0, fps = 30;
		float fwdSpeed = 0.f;                 // モデル単位/秒（前進正味速度）
		std::vector<int> gid;                 // per-clip-bone -> global gid (-1=対象外)
		std::vector<XMFLOAT4X4> mats;          // F*N, 行優先 skinM_engine
		std::vector<XMFLOAT3>  root;           // F, エンジンmodel空間の水平ルート変位(frame0基準)
	};

	std::vector<std::string> g_gidToName;
	std::unordered_map<std::string, int> g_nameToGid;
	uint32_t g_gidCount = 0;

	std::vector<Clip> g_clips;             // [0..g_coreCount) = コア常駐 + [g_coreCount..) = ロード中グループ
	std::vector<std::string> g_clipNames;
	std::unordered_map<std::string, int> g_clipIndex;
	bool g_ready = false;
	float g_worldScale = 1.0f;
	// グループ（種類別オンデマンドロード）
	int g_coreCount = 0;
	std::vector<std::string> g_groupList;  // clips/groups のサブフォルダ名
	std::string g_groupRoot;               // clips/groups パス
	std::string g_curGroup;                // ロード中グループ名（空=無し）

	// 再生
	int   g_curClip = -1;
	float g_time = 0.f;
	const char* g_stateName = "idle";

	// オーバーライド
	bool g_hasOverride = false;
	std::string g_overrideName;
	bool g_overrideLoop = true;

	// ルートモーション
	bool g_applyRootMotion = false;
	int  g_prevRootFrame = -1;

	int findClip(const std::string& n)
	{
		auto it = g_clipIndex.find(n);
		return it == g_clipIndex.end() ? -1 : it->second;
	}

	bool loadClipFile(const std::string& path, Clip& c)
	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) return false;
		char magic[4] = {}; ifs.read(magic, 4);
		if (memcmp(magic, "SKCL", 4) != 0) return false;
		uint32_t ver = 0, N = 0, F = 0, fps = 0;
		ifs.read((char*)&ver, 4); ifs.read((char*)&N, 4); ifs.read((char*)&F, 4); ifs.read((char*)&fps, 4);
		float fwd = 0.f; if (ver >= 2) ifs.read((char*)&fwd, 4);
		if (!ifs || N == 0 || F == 0) return false;
		c.N = N; c.F = F; c.fps = fps ? fps : 30; c.fwdSpeed = fwd;
		c.gid.assign(N, -1);
		for (uint32_t i = 0; i < N; ++i)
		{
			uint16_t len = 0; ifs.read((char*)&len, 2);
			std::string nm(len, '\0'); if (len) ifs.read(&nm[0], len);
			auto it = g_nameToGid.find(nm);
			c.gid[i] = (it != g_nameToGid.end()) ? it->second : -1;
		}
		c.mats.assign((size_t)F * N, {});
		ifs.read((char*)c.mats.data(), (std::streamsize)sizeof(XMFLOAT4X4) * (size_t)F * N);
		if (!ifs) return false;
		c.root.assign(F, {});
		if (ver >= 2) ifs.read((char*)c.root.data(), (std::streamsize)sizeof(XMFLOAT3) * F);  // 短くても mats は有効
		return true;
	}

	// ---- 列ベクトル 4x4 ヘルパ（XMFLOAT4X4=行優先, 意味は M·v, 並進は最終列）----
	//   XMMatrixMultiply(A,B) は標準行列積 A·B, XMMatrixInverse は真の逆行列（規約非依存）なのでそのまま使える。
	static XMFLOAT4X4 mul4(const XMFLOAT4X4& A, const XMFLOAT4X4& B)
	{ XMFLOAT4X4 r; XMStoreFloat4x4(&r, XMMatrixMultiply(XMLoadFloat4x4(&A), XMLoadFloat4x4(&B))); return r; }
	static XMFLOAT4X4 inv4(const XMFLOAT4X4& A)
	{ XMFLOAT4X4 r; XMStoreFloat4x4(&r, XMMatrixInverse(nullptr, XMLoadFloat4x4(&A))); return r; }
	static XMFLOAT3 origin4(const XMFLOAT4X4& M) { return { M._14, M._24, M._34 }; }
	static XMFLOAT3 xform4(const XMFLOAT4X4& M, const XMFLOAT3& p)
	{ return { M._11*p.x+M._12*p.y+M._13*p.z+M._14, M._21*p.x+M._22*p.y+M._23*p.z+M._24, M._31*p.x+M._32*p.y+M._33*p.z+M._34 }; }

	// スプリングボーン
	std::vector<XMFLOAT4X4> g_offset;      // gid -> offsetMatrix (列ベクトル)
	std::vector<XMFLOAT4X4> g_bindWorld;   // gid -> inv(offset) = バインド時ボーンのモデル空間変換
	std::vector<XMFLOAT4X4> g_bakedSkinM;  // gid -> 現フレームのベイク skinM

	struct SpringBone
	{
		int gid = -1, parentGid = -1;
		bool parentIsSpring = false;
		int  depth = 0;
		int  category = 0;
		float boneLen = 0.f;
		XMFLOAT4X4 restLocal;              // offset[parent]·inv(offset[gid])  (parentPosed·restLocal = posedWorld[gid])
		XMFLOAT3   restTailLocal{};        // 親バインド空間での rest 先端位置
		// 状態（R2 Verlet 用）
		XMFLOAT3 curTail{}, prevTail{};
		bool  inited = false;
	};
	std::vector<SpringBone> g_spring;      // depth 昇順
	std::unordered_map<int,int> g_gidToSpringIdx;
	std::vector<XMFLOAT4X4> g_springWorld; // spring index -> 計算した posedWorld
	std::vector<XMFLOAT4X4> g_springSkinM; // spring index -> 上書き skinM
	bool g_springValid = false;            // 今フレーム spring を計算したか
	bool  g_springEnabled = true;          // 既定ON（髪/リボン/袖/スカート）
	float g_stiffness = 0.5f, g_drag = 1.0f, g_gravity = 0.8f;   // 落ち着いた追従（暴れない）
	// カテゴリ別有効: [1]=hair, [2]=skirt, [3]=rigid(胸/肩飾等)
	bool  g_enableCat[4] = { false, true, true, true };

	// 衝突コライダー（VRM式カプセル）: 2ボーンの posed 原点を結ぶ線分＋半径。gidB<0 で球。tailを押し出す。
	struct Collider { int gidA = -1; int gidB = -1; float radius = 0.f; };
	std::vector<Collider> g_colliders;
	bool g_collisionOn = true;
	float g_colliderScale = 1.0f;          // 全コライダー半径の倍率（膨らみ調整）
	bool g_springDebugDraw = false;        // デバッグ可視化

	// デバッグ可視化用（毎フレーム, モデル空間）: チェーン線分 + コライダー
	std::vector<CharacterAnimator::DebugSeg> g_dbgSegs;
	std::vector<CharacterAnimator::DebugCollider> g_dbgCols;

	int catFromName(const std::string& c)
	{
		// 0=none/off, 1=hair-like(spring既定ON), 2=skirt(既定OFF, B用), 3=rigid-ish(既定OFF)
		if (c=="longhair"||c=="sidehair"||c=="hair"||c=="banghair"||c=="strayhair"||
			c=="ribbon"||c=="tail"||c=="sleeve"||c=="earring"||c=="collar"||c=="necklace") return 1;
		if (c=="skirt") return 2;
		return 3; // shoulder/chest/hat/other
	}
}

void CharacterAnimator::Init(const std::vector<std::string>& gidToName, const std::vector<XMFLOAT4X4>& gidOffset)
{
	g_gidToName = gidToName;
	g_gidCount = (uint32_t)gidToName.size();
	g_nameToGid.clear();
	for (uint32_t i = 0; i < g_gidCount; ++i) g_nameToGid[gidToName[i]] = (int)i;
	g_offset = gidOffset;
	g_bindWorld.assign(g_gidCount, {});
	for (uint32_t i = 0; i < g_gidCount; ++i) g_bindWorld[i] = inv4(g_offset[i]);
	g_bakedSkinM.assign(g_gidCount, {});
	g_spring.clear(); g_gidToSpringIdx.clear(); g_springWorld.clear();
}

int CharacterAnimator::LoadSpringRig(const char* path)
{
	g_spring.clear(); g_gidToSpringIdx.clear(); g_springWorld.clear();
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) { printf("[Spring] rig not found: %s\n", path); fflush(stdout); return 0; }
	std::string line;
	std::vector<SpringBone> tmp;
	while (std::getline(ifs, line))
	{
		if (line.empty() || line[0] == '#') continue;
		// engine_name\tparent_engine_name\tcategory\tboneLen
		size_t a = line.find('\t'); if (a == std::string::npos) continue;
		size_t b = line.find('\t', a + 1); if (b == std::string::npos) continue;
		size_t c = line.find('\t', b + 1); if (c == std::string::npos) continue;
		std::string nm = line.substr(0, a);
		std::string pn = line.substr(a + 1, b - a - 1);
		std::string cat = line.substr(b + 1, c - b - 1);
		float blen = (float)atof(line.substr(c + 1).c_str());
		auto itg = g_nameToGid.find(nm); if (itg == g_nameToGid.end()) continue;
		int gid = itg->second;
		int pgid = -1; auto itp = g_nameToGid.find(pn); if (itp != g_nameToGid.end()) pgid = itp->second;
		SpringBone sb; sb.gid = gid; sb.parentGid = pgid; sb.category = catFromName(cat); sb.boneLen = blen;
		if (pgid >= 0) sb.restLocal = mul4(g_offset[pgid], g_bindWorld[gid]);      // parentPosed·restLocal=posedWorld
		else { XMStoreFloat4x4(&sb.restLocal, XMMatrixIdentity()); }
		tmp.push_back(sb);
	}
	// depth = 親を辿ってスプリング骨が何段続くか（アンカーは非スプリング）
	std::unordered_map<int,int> isSpring;
	for (int i = 0; i < (int)tmp.size(); ++i) isSpring[tmp[i].gid] = i;
	// 子（毛先側）の bind head を集める（親がスプリング骨なら、その骨の子として登録）
	std::unordered_map<int, XMFLOAT3> childHead;
	for (auto& sb : tmp)
		if (sb.parentGid >= 0 && isSpring.count(sb.parentGid)) childHead[sb.parentGid] = origin4(g_bindWorld[sb.gid]);
	for (auto& sb : tmp)
	{
		sb.parentIsSpring = (sb.parentGid >= 0 && isSpring.count(sb.parentGid) > 0);
		int d = 0, pg = sb.parentGid;
		while (pg >= 0 && isSpring.count(pg)) { d++; pg = tmp[isSpring[pg]].parentGid; }
		sb.depth = d;
		// rest 先端の向き: 子（毛先）方向。子が無い先端は親からの延長。← 根本を髪の生え際向きにしない
		if (sb.parentGid >= 0)
		{
			XMFLOAT3 headB = origin4(g_bindWorld[sb.gid]);
			XMFLOAT3 dir;
			auto ch = childHead.find(sb.gid);
			if (ch != childHead.end()) dir = { ch->second.x - headB.x, ch->second.y - headB.y, ch->second.z - headB.z };
			else { XMFLOAT3 hp = origin4(g_bindWorld[sb.parentGid]); dir = { headB.x - hp.x, headB.y - hp.y, headB.z - hp.z }; }
			float L = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z); if (L < 1e-6f) { dir = { 0.f,-1.f,0.f }; L = 1.f; }
			float bl = (sb.boneLen > 1e-4f) ? sb.boneLen : L;
			XMFLOAT3 tailW{ headB.x + dir.x/L*bl, headB.y + dir.y/L*bl, headB.z + dir.z/L*bl };
			sb.restTailLocal = xform4(g_offset[sb.parentGid], tailW);   // world -> 親バインド空間
		}
	}
	std::sort(tmp.begin(), tmp.end(), [](const SpringBone& a, const SpringBone& b){ return a.depth < b.depth; });
	g_spring = std::move(tmp);
	g_springWorld.assign(g_spring.size(), {});
	g_springSkinM.assign(g_spring.size(), {});
	for (int i = 0; i < (int)g_spring.size(); ++i) g_gidToSpringIdx[g_spring[i].gid] = i;
	// コライダー（VRM式カプセル）: 脚/胴は線分カプセルで太もも全長をカバー（球だと中間が貫通する）。頭は球。
	g_colliders.clear();
	struct CapDef { const char* a; const char* b; float r; };   // b=nullptr で球
	const CapDef CAPS[] = {
		{ "頭", nullptr, 0.9f },                 // 頭（球）
		{ "上半身2", "下半身", 1.05f },          // 胴（カプセル）
		{ "右足", "右ひざ", 0.85f }, { "左足", "左ひざ", 0.85f },   // 太もも
		{ "右ひざ", "右足首", 0.6f }, { "左ひざ", "左足首", 0.6f }, // すね
	};
	auto gidOf = [&](const char* nm) -> int { auto it = g_nameToGid.find(nm); return it != g_nameToGid.end() ? (int)it->second : -1; };
	for (auto& cp : CAPS)
	{
		int ga = gidOf(cp.a); if (ga < 0) continue;
		int gb = cp.b ? gidOf(cp.b) : -1;
		g_colliders.push_back({ ga, gb, cp.r });
	}
	int c1 = 0, c2 = 0, c3 = 0; for (auto& sb : g_spring) { if (sb.category == 1)++c1; else if (sb.category == 2)++c2; else ++c3; }
	printf("[Spring] rig loaded: %zu bones (hair=%d skirt=%d rigid=%d) colliders=%zu\n",
		g_spring.size(), c1, c2, c3, g_colliders.size());
	fflush(stdout);
	return (int)g_spring.size();
}

int CharacterAnimator::LoadClipLibrary(const char* folder, float worldScale)
{
	g_worldScale = (worldScale > 1e-4f) ? worldScale : 1.0f;
	g_clips.clear(); g_clipNames.clear(); g_clipIndex.clear();
	std::error_code ec;
	if (!fs::exists(folder, ec)) { printf("[Anim] clip folder not found: %s\n", folder); fflush(stdout); return 0; }
	std::vector<fs::path> files;
	for (auto& e : fs::directory_iterator(folder, ec))
		if (e.is_regular_file() && e.path().extension() == ".skcl") files.push_back(e.path());
	std::sort(files.begin(), files.end());
	for (auto& p : files)
	{
		Clip c; c.name = p.stem().string();
		if (loadClipFile(p.string(), c))
		{
			int idx = (int)g_clips.size();
			g_clips.push_back(std::move(c));
			g_clipNames.push_back(g_clips.back().name);
			g_clipIndex[g_clips.back().name] = idx;
		}
		else printf("[Anim] failed to load clip: %s\n", p.string().c_str());
	}
	g_ready = !g_clips.empty();
	g_coreCount = (int)g_clips.size();   // 先頭=コア常駐
	// 既定は idle（あれば）
	g_curClip = findClip("idle"); if (g_curClip < 0 && !g_clips.empty()) g_curClip = 0;
	g_time = 0.f;
	// グループ一覧（clips/groups/<group>/）をスキャン（中身は選択時にロード）
	g_groupList.clear(); g_curGroup.clear();
	g_groupRoot = std::string(folder) + "/groups";
	std::error_code ec2;
	if (fs::exists(g_groupRoot, ec2))
		for (auto& e : fs::directory_iterator(g_groupRoot, ec2))
			if (e.is_directory()) g_groupList.push_back(e.path().filename().string());
	std::sort(g_groupList.begin(), g_groupList.end());
	printf("[Anim] loaded %d core clips from %s (idle=%d), groups available=%zu\n",
		g_coreCount, folder, findClip("idle"), g_groupList.size());
	fflush(stdout);
	return (int)g_clips.size();
}

int CharacterAnimator::LoadGroup(const std::string& name)
{
	if (!g_ready) return 0;
	// コアだけ残して既存グループを破棄
	if ((int)g_clips.size() > g_coreCount) g_clips.erase(g_clips.begin() + g_coreCount, g_clips.end());
	int n = 0;
	std::string dir = g_groupRoot + "/" + name;
	std::error_code ec;
	if (fs::exists(dir, ec))
	{
		std::vector<fs::path> files;
		for (auto& e : fs::directory_iterator(dir, ec))
			if (e.is_regular_file() && e.path().extension() == ".skcl") files.push_back(e.path());
		std::sort(files.begin(), files.end());
		for (auto& p : files) { Clip c; c.name = p.stem().string(); if (loadClipFile(p.string(), c)) { g_clips.push_back(std::move(c)); ++n; } }
	}
	// 索引再構築
	g_clipNames.clear(); g_clipIndex.clear();
	for (int i = 0; i < (int)g_clips.size(); ++i) { g_clipNames.push_back(g_clips[i].name); g_clipIndex[g_clips[i].name] = i; }
	g_curGroup = name;
	g_curClip = findClip("idle"); if (g_curClip < 0 && !g_clips.empty()) g_curClip = 0;
	g_hasOverride = false; g_time = 0.f;
	printf("[Anim] group '%s' loaded: %d clips (total %zu)\n", name.c_str(), n, g_clips.size());
	fflush(stdout);
	return n;
}
const std::vector<std::string>& CharacterAnimator::GroupNames() { return g_groupList; }
const char* CharacterAnimator::CurrentGroup() { return g_curGroup.c_str(); }

bool CharacterAnimator::IsReady() { return g_ready; }

void CharacterAnimator::Update(entt::registry& registry, float dt)
{
	if (!g_ready || g_clips.empty()) return;

	PlayerComponent* pc = nullptr;
	TransformComponent* tf = nullptr;
	for (auto e : registry.view<PlayerComponent, TransformComponent>())
	{
		pc = &registry.get<PlayerComponent>(e);
		tf = &registry.get<TransformComponent>(e);
		break;
	}

	// 移動速度をクリップ前進速度から設定（フットスライド低減）
	const int wi = findClip("walk"), ri = findClip("run");
	if (pc)
	{
		if (wi >= 0 && g_clips[wi].fwdSpeed > 0.1f) pc->WalkSpeed = g_clips[wi].fwdSpeed * g_worldScale;
		if (ri >= 0 && g_clips[ri].fwdSpeed > 0.1f) pc->RunSpeed  = g_clips[ri].fwdSpeed * g_worldScale;
	}

	// 望ましいクリップ
	std::string desired;
	if (g_hasOverride) { desired = g_overrideName; g_stateName = "override"; }
	else if (pc && pc->IsMoving) { desired = pc->RunMode ? "run" : "walk"; g_stateName = pc->RunMode ? "run" : "walk"; }
	else { desired = "idle"; g_stateName = "idle"; }

	int di = findClip(desired);
	if (di < 0) { di = findClip("idle"); if (di < 0) di = 0; }
	if (di != g_curClip) { g_curClip = di; g_time = 0.f; g_prevRootFrame = -1; }

	const Clip& c = g_clips[g_curClip];
	const float dur = (float)c.F / (float)c.fps;
	g_time += dt;
	const bool looping = g_hasOverride ? g_overrideLoop : true;
	if (g_time >= dur)
	{
		if (looping) g_time = (dur > 1e-4f) ? fmodf(g_time, dur) : 0.f;
		else { g_time = dur - 0.5f / (float)c.fps; g_hasOverride = false; }  // 単発は終端で状態機械へ復帰
	}

	int frame = (int)(g_time * (float)c.fps);
	if (frame < 0) frame = 0; if (frame >= (int)c.F) frame = (int)c.F - 1;

	// ルートモーション（任意）: クリップの水平変位を「向いている方向」へ加える。
	if (g_applyRootMotion && pc && tf && !c.root.empty())
	{
		if (g_prevRootFrame >= 0 && frame >= g_prevRootFrame)
		{
			const XMFLOAT3& a = c.root[g_prevRootFrame];
			const XMFLOAT3& b = c.root[frame];
			const float dxm = b.x - a.x, dzm = b.z - a.z;              // エンジンmodel空間の水平デルタ
			const float distWorld = sqrtf(dxm * dxm + dzm * dzm) * g_worldScale;
			const float yaw = tf->RotationY;                          // モデル前方 = -Z を yaw 回転
			tf->Position.x += -sinf(yaw) * distWorld;
			tf->Position.z += -cosf(yaw) * distWorld;
		}
		g_prevRootFrame = frame;
	}
	else g_prevRootFrame = -1;

	// --- 現フレームのベイク skinM を構築（gid索引） ---
	{
		XMFLOAT4X4 I; XMStoreFloat4x4(&I, XMMatrixIdentity());
		for (uint32_t g = 0; g < g_gidCount; ++g) g_bakedSkinM[g] = I;
		const XMFLOAT4X4* base = &c.mats[(size_t)frame * c.N];
		for (uint32_t i = 0; i < c.N; ++i) { int g = c.gid[i]; if (g >= 0 && (uint32_t)g < g_gidCount) g_bakedSkinM[g] = base[i]; }
	}
	// --- スプリングボーン（R2: Verlet 揺れ。剛体追従(rigid)を rest として、先端をVerlet積分し曲げる）---
	g_springValid = false;
	if (g_springEnabled && !g_spring.empty() && g_gidCount > 0)
	{
		const float sdt = (dt > 1e-4f && dt < 0.1f) ? dt : (1.f / 60.f);   // 異常dtをクランプ
		const XMVECTOR gravDown = XMVectorSet(0.f, -1.f, 0.f, 0.f);        // エンジン +Y up
		// カプセルコライダーの端点をベイクから復元
		struct ColW { XMVECTOR a, b; float r; };
		std::vector<ColW> cols; cols.reserve(g_colliders.size());
		if (g_springDebugDraw) g_dbgCols.clear();
		if (g_collisionOn || g_springDebugDraw)
			for (auto& cd : g_colliders)
			{
				if (cd.gidA < 0 || (uint32_t)cd.gidA >= g_gidCount) continue;
				XMFLOAT3 oa = origin4(mul4(g_bakedSkinM[cd.gidA], g_bindWorld[cd.gidA]));
				XMFLOAT3 ob = oa;
				if (cd.gidB >= 0 && (uint32_t)cd.gidB < g_gidCount) ob = origin4(mul4(g_bakedSkinM[cd.gidB], g_bindWorld[cd.gidB]));
				float r = cd.radius * g_colliderScale;
				if (g_collisionOn) cols.push_back({ XMLoadFloat3(&oa), XMLoadFloat3(&ob), r });
				if (g_springDebugDraw) g_dbgCols.push_back({ oa, ob, r });
			}
		if (g_springDebugDraw) g_dbgSegs.clear();
		for (int i = 0; i < (int)g_spring.size(); ++i)
		{
			SpringBone& sb = g_spring[i];
			if (sb.gid < 0 || sb.parentGid < 0 || sb.category < 1 || sb.category > 3 || !g_enableCat[sb.category]) continue;
			const float catStiff = (sb.category == 2) ? 2.2f : (sb.category == 3) ? 6.0f : 1.0f;  // skirt硬め, rigidさらに硬め
			const float effStiff = fminf(1.f, g_stiffness * catStiff);
			// 親の posedWorld（spring親なら計算済、そうでなければベイクから復元）
			XMFLOAT4X4 parentPosed;
			auto itp = g_gidToSpringIdx.find(sb.parentGid);
			if (sb.parentIsSpring && itp != g_gidToSpringIdx.end() && g_enableCat[g_spring[itp->second].category])
				parentPosed = g_springWorld[itp->second];
			else
				parentPosed = mul4(g_bakedSkinM[sb.parentGid], g_bindWorld[sb.parentGid]);
			// rest（剛体追従）の変換・head・rest先端
			XMFLOAT4X4 rigid = mul4(parentPosed, sb.restLocal);
			XMFLOAT3 headF = origin4(rigid);
			XMFLOAT3 rtailF = xform4(parentPosed, sb.restTailLocal);
			XMVECTOR vHead = XMLoadFloat3(&headF);
			XMVECTOR vRigidTail = XMLoadFloat3(&rtailF);
			XMVECTOR vRigidDir = XMVector3Normalize(XMVectorSubtract(vRigidTail, vHead));
			XMVECTOR vCur, vPrev;
			if (!sb.inited) { vCur = vRigidTail; vPrev = vRigidTail; sb.inited = true; }
			else { vCur = XMLoadFloat3(&sb.curTail); vPrev = XMLoadFloat3(&sb.prevTail); }
			// Verlet: 慣性(1-drag) + 剛性(rest先端へ) + 重力
			XMVECTOR next = XMVectorAdd(vCur, XMVectorScale(XMVectorSubtract(vCur, vPrev), (1.f - g_drag)));
			next = XMVectorAdd(next, XMVectorScale(XMVectorSubtract(vRigidTail, vCur), effStiff));
			next = XMVectorAdd(next, XMVectorScale(gravDown, g_gravity * sdt * sb.boneLen));
			// 骨長拘束（headから boneLen）
			XMVECTOR simDir = XMVector3Normalize(XMVectorSubtract(next, vHead));
			next = XMVectorAdd(vHead, XMVectorScale(simDir, sb.boneLen));
			// 衝突: カプセル（線分a-b + 半径）から押し出し。tail→線分の最近点方向へ。
			for (auto& cw : cols)
			{
				XMVECTOR ab = XMVectorSubtract(cw.b, cw.a);
				float abLen2 = XMVectorGetX(XMVector3LengthSq(ab));
				float t = (abLen2 > 1e-8f) ? XMVectorGetX(XMVector3Dot(XMVectorSubtract(next, cw.a), ab)) / abLen2 : 0.f;
				t = fmaxf(0.f, fminf(1.f, t));
				XMVECTOR closest = XMVectorAdd(cw.a, XMVectorScale(ab, t));
				XMVECTOR d = XMVectorSubtract(next, closest);
				float dist = XMVectorGetX(XMVector3Length(d));
				if (dist < cw.r && dist > 1e-5f)
					next = XMVectorAdd(closest, XMVectorScale(XMVectorScale(d, 1.f / dist), cw.r));
			}
			// 衝突後にもう一度骨長拘束（head基準）
			simDir = XMVector3Normalize(XMVectorSubtract(next, vHead));
			next = XMVectorAdd(vHead, XMVectorScale(simDir, sb.boneLen));
			XMStoreFloat3(&sb.prevTail, vCur); XMStoreFloat3(&sb.curTail, next);
			if (g_springDebugDraw) { XMFLOAT3 tF; XMStoreFloat3(&tF, next); g_dbgSegs.push_back({ headF, tF, sb.category }); }
			// aim: rigidDir -> simDir。列ベクトル標準の右手系ロドリゲス回転 R（R·u=v）で構築。
			//   XMMatrixRotationAxis は左手系で、右手系の軸 cross(u,v) と手系不一致→曲げが逆になるため使わない。
			XMFLOAT4X4 posed;
			XMVECTOR axisV = XMVector3Cross(vRigidDir, simDir);
			float s = XMVectorGetX(XMVector3Length(axisV));                 // sinθ
			float cth = XMVectorGetX(XMVector3Dot(vRigidDir, simDir));      // cosθ
			if (s < 1e-5f) posed = rigid;   // 平行（曲げ無し）
			else
			{
				XMFLOAT3 k; XMStoreFloat3(&k, XMVectorScale(axisV, 1.f / s));  // 単位軸
				const float oc = 1.f - cth;
				XMFLOAT4X4 R;
				R._11 = cth + k.x*k.x*oc;   R._12 = k.x*k.y*oc - k.z*s; R._13 = k.x*k.z*oc + k.y*s; R._14 = 0.f;
				R._21 = k.y*k.x*oc + k.z*s; R._22 = cth + k.y*k.y*oc;   R._23 = k.y*k.z*oc - k.x*s; R._24 = 0.f;
				R._31 = k.z*k.x*oc - k.y*s; R._32 = k.z*k.y*oc + k.x*s; R._33 = cth + k.z*k.z*oc;   R._34 = 0.f;
				R._41 = R._42 = R._43 = 0.f; R._44 = 1.f;
				XMFLOAT4X4 rigidRot = rigid;   // 回転部のみ
				rigidRot._14 = rigidRot._24 = rigidRot._34 = 0.f;
				rigidRot._41 = rigidRot._42 = rigidRot._43 = 0.f; rigidRot._44 = 1.f;
				posed = mul4(R, rigidRot);
				posed._14 = headF.x; posed._24 = headF.y; posed._34 = headF.z;   // 並進=head
			}
			g_springWorld[i] = posed;
			g_springSkinM[i] = mul4(posed, g_offset[sb.gid]);
		}
		g_springValid = true;
	}
}

void CharacterAnimator::FillPalette(XMFLOAT4X4* dst, uint32_t gidCount)
{
	XMFLOAT4X4 I; XMStoreFloat4x4(&I, XMMatrixIdentity());
	if (g_curClip < 0 || g_gidCount == 0) { for (uint32_t g = 0; g < gidCount; ++g) dst[g] = I; return; }
	// ベイク skinM をコピー
	for (uint32_t g = 0; g < gidCount; ++g) dst[g] = (g < g_gidCount) ? g_bakedSkinM[g] : I;
	// スプリング上書き（有効カテゴリ）
	if (g_springValid)
		for (int i = 0; i < (int)g_spring.size(); ++i)
		{
			const SpringBone& sb = g_spring[i];
			if (sb.category >= 1 && sb.category <= 3 && g_enableCat[sb.category] && sb.gid >= 0 && (uint32_t)sb.gid < gidCount)
				dst[sb.gid] = g_springSkinM[i];
		}
}

const std::vector<std::string>& CharacterAnimator::ClipNames() { return g_clipNames; }
void CharacterAnimator::PlayOverride(const std::string& name, bool loop)
{
	if (findClip(name) < 0) return;
	g_hasOverride = true; g_overrideName = name; g_overrideLoop = loop;
	g_curClip = findClip(name); g_time = 0.f; g_prevRootFrame = -1;
}
void CharacterAnimator::ClearOverride() { g_hasOverride = false; }
bool CharacterAnimator::HasOverride() { return g_hasOverride; }
const char* CharacterAnimator::StateName() { return g_stateName; }
const char* CharacterAnimator::CurrentClipName() { return (g_curClip >= 0 && g_curClip < (int)g_clips.size()) ? g_clips[g_curClip].name.c_str() : "-"; }
float CharacterAnimator::CurrentClip01()
{
	if (g_curClip < 0) return 0.f;
	const Clip& c = g_clips[g_curClip];
	const float dur = (float)c.F / (float)c.fps;
	return (dur > 1e-4f) ? (g_time / dur) : 0.f;
}
bool CharacterAnimator::GetApplyRootMotion() { return g_applyRootMotion; }
void CharacterAnimator::SetApplyRootMotion(bool on) { g_applyRootMotion = on; g_prevRootFrame = -1; }
float CharacterAnimator::WalkSpeedWorld() { int i = findClip("walk"); return i >= 0 ? g_clips[i].fwdSpeed * g_worldScale : 0.f; }
float CharacterAnimator::RunSpeedWorld() { int i = findClip("run"); return i >= 0 ? g_clips[i].fwdSpeed * g_worldScale : 0.f; }

bool CharacterAnimator::SpringAvailable() { return !g_spring.empty(); }
int  CharacterAnimator::SpringBoneCount() { int n = 0; for (auto& s : g_spring) if (s.category >= 1 && s.category <= 3 && g_enableCat[s.category]) ++n; return n; }
bool CharacterAnimator::GetSpringEnabled() { return g_springEnabled; }
void CharacterAnimator::SetSpringEnabled(bool on) { g_springEnabled = on; }
void CharacterAnimator::GetSpringParams(float& stiffness, float& drag, float& gravity) { stiffness = g_stiffness; drag = g_drag; gravity = g_gravity; }
void CharacterAnimator::SetSpringParams(float stiffness, float drag, float gravity) { g_stiffness = stiffness; g_drag = drag; g_gravity = gravity; }
void CharacterAnimator::GetSpringCategories(bool& hair, bool& skirt, bool& rigid) { hair = g_enableCat[1]; skirt = g_enableCat[2]; rigid = g_enableCat[3]; }
void CharacterAnimator::SetSpringCategories(bool hair, bool skirt, bool rigid) { g_enableCat[1] = hair; g_enableCat[2] = skirt; g_enableCat[3] = rigid; }
bool CharacterAnimator::GetSpringCollision() { return g_collisionOn; }
void CharacterAnimator::SetSpringCollision(bool on) { g_collisionOn = on; }
float CharacterAnimator::GetColliderScale() { return g_colliderScale; }
void CharacterAnimator::SetColliderScale(float s) { g_colliderScale = (s > 0.05f) ? s : 0.05f; }
bool CharacterAnimator::GetSpringDebugDraw() { return g_springDebugDraw; }
void CharacterAnimator::SetSpringDebugDraw(bool on) { g_springDebugDraw = on; }
const std::vector<CharacterAnimator::DebugSeg>& CharacterAnimator::DebugSegments() { return g_dbgSegs; }
const std::vector<CharacterAnimator::DebugCollider>& CharacterAnimator::DebugColliders() { return g_dbgCols; }
