// ============================================================
//  SSRResolvePS.hlsl — 平面反射による「濡れた水たまり」。
//  フルスクリーン。深度から水たまり領域(XZ楕円＋水平面ゲート)を判定し、
//  平面反射パスが描いた町の反射カラーRT(g_Reflection)を「自分の画面UV」で
//  サンプルして濡れ面に映す。平面はメイン視点とミラー視点で同じ画面座標に
//  射影されるためカメラ角度に依存しない（見下ろしでも建物が正しく映る）。
//  反射RTに町が無い所は空グラデにフォールバック。水のFresnelで鏡面度を制御。
//  出力は線形HDR（トーンマップは PostProcess）。VS は ToneMap_VS を流用。
// ============================================================

Texture2D        g_SceneColor : register(t0, space0);   // HDR コピー（濡れ下地用）
Texture2D<float> g_Depth      : register(t1, space0);   // シーン深度 R32_FLOAT
TextureCube      g_Prefilter  : register(t2, space0);   // 環境キューブ（互換で保持, 現状未使用）
Texture2D        g_Reflection : register(t3, space0);   // 平面反射カラー(フル解像度, rgb=町, a=被覆)
Texture2D        g_Noise      : register(t4, space0);   // UE5 T_blend_noise_a（水たまり分布マスク）
SamplerState     g_Lin        : register(s0, space0);   // linear clamp
SamplerState     g_Pt         : register(s1, space0);   // point clamp
SamplerState     g_Wrap       : register(s2, space0);   // linear wrap（ノイズのタイル用）

cbuffer SceneCB : register(b0, space0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;   // .xyz world pos
    float4 SunDirection;
    float4 SunColor;
    matrix InvViewProj;
};

cbuffer SSRParams : register(b1, space0)
{
    float4 P0;   // xz=水たまり中心(world), y=groundY(未使用/参考), w=有効フラグ(0=無効)
    float4 P1;   // x=halfX, y=halfZ, z=edgeFalloff(m), w=wetDarken(0..1)
    float4 P2;   // x=stride(m), y=steps, z=thicknessMult, w=edgeFade
    float4 Sky;  // rgb=空フォールバック色, w=反射強度
};

struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 ReconW(float2 uv, float d)
{
    float2 n = uv * 2.0f - 1.0f; n.y = -n.y;
    float4 w = mul(float4(n, d, 1.0f), InvViewProj);
    return w.xyz / w.w;
}
float ViewZ(float3 wp) { return mul(float4(wp, 1.0f), View).z; }

// --- 手続き的ノイズ（水たまりの有機的な形用, world XZ で安定）---
float hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}
float vnoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = hash21(i), b = hash21(i + float2(1, 0));
    float c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float fbm(float2 p)
{
    // 各オクターブで座標を回転させる（格子=ダイヤ状アーティファクトを分散させ、
    // 閾値処理しても角張らないようにする）。5 オクターブで細部まで。
    float v = 0.0f, a = 0.5f;
    const float2x2 rot = float2x2(0.80f, -0.60f, 0.60f, 0.80f); // ~37°
    [unroll] for (int i = 0; i < 5; ++i) { v += a * vnoise(p); p = mul(p, rot) * 2.03f; a *= 0.5f; }
    return v;
}

float4 main(PS_IN In) : SV_Target
{
    float3 scene = g_SceneColor.SampleLevel(g_Lin, In.uv, 0).rgb;
    if (P0.w < 0.5f) return float4(scene, 1);           // SSR 無効

    float d = g_Depth.SampleLevel(g_Pt, In.uv, 0);
    if (d >= 0.9999f) return float4(scene, 1);          // 空

    float3 P = ReconW(In.uv, d);

    // ============================================================
    //  濡れ地面＋水たまり（UE5「雨上がりの広場」）— 2段レイヤード方式。
    //  水は「有る/無い」の2値ではなく、深い水（鏡）→染み込んだ濡れ（グラデ）→乾き、
    //  という連続量。1つの連続ノイズ amount(=水の量) に対し、"広い" 2つの閾値で
    //    濡れ(wet): 低め・広い遷移 / 水たまり(pool): 高め・広い遷移
    //  を作り、単一閾値のパキッとした切れ目・欠けを根絶する。
    // ============================================================
    // 水平地面ゲート（壁/屋根の垂直面を除外）＋高さゲート（反射平面付近の地面のみ）
    float3 Ns = normalize(cross(ddy(P), ddx(P)));
    if (Ns.y < 0.0f) Ns = -Ns;
    float ground = smoothstep(0.62f, 0.90f, Ns.y);
    // 濡れは「道路面のみ」。歩道は縁石で一段高い(~15cm+)。平面反射は y=groundY 基準なので
    // 一段高い歩道に出すと像が浮いて破綻する（＝店舗が歩道に映る不具合）。groundY+3cm で満濡れ、
    // +12cm で完全に乾きにして縁石より上（歩道）を除外する。
    float hgate  = 1.0f - smoothstep(P0.y + 0.03f, P0.y + 0.12f, P.y);
    // 濡らす範囲（中心 P0.xz から半径 P1.xy、広く緩やかな縁で自然に乾きへ）
    float2 q = (P.xz - P0.xz) / float2(max(P1.x, 0.1f), max(P1.y, 0.1f));
    float region = 1.0f - smoothstep(0.55f, 1.10f, length(q));
    float gate = saturate(region) * ground * hgate;
    if (gate <= 0.003f) return float4(scene, 1);

    // 「水の量」場: UE5 の実ブレンドノイズ(T_blend_noise_a)を world XZ でサンプル。
    // M_Ground_Master と同じ「本物の有機的分布」を使うので、手続きノイズの格子/落書き感
    // が出ない。2 スケール合成でタイル反復を隠し、CheapContrast(UE5 Puddle_Blend_Constrast
    // 相当)で水際を締める。P2.z=タイル(1/m 相当), P2.w=コントラスト。
    float nt = (P2.z > 0.0001f) ? P2.z : 0.03f;
    // mip2〜3 の粗い階層をサンプル＝細かいスパークルを消し「大きな滑らかな溜まり形状」に。
    float n0 = g_Noise.SampleLevel(g_Wrap, P.xz * nt, 2.5f).r;
    float n1 = g_Noise.SampleLevel(g_Wrap, P.xz * nt * 0.41f + 0.37f, 2.5f).r; // 低周波で大きな粗密
    float amount = n0 * 0.70f + n1 * 0.30f;
    // CheapContrast 相当（0.5 中心にコントラスト）で水際をくっきり。強すぎるとバラける。
    float ctr = (P2.w > 0.0001f) ? P2.w : 2.0f;
    amount = saturate((amount - 0.5f) * ctr + 0.5f);
    // ★2段レイヤード。水は地面全体を一様に覆わない — 低い所に「水たまり(鏡)」があり、
    //   その周りだけ「濡れ(染み込み)」がグラデーションで滲む。それ以外は乾いた地面のまま。
    //   （前回は全面を濡らして暗く潰れ＝水に見えなかった。distinct pools + damp halo へ）
    float wet  = smoothstep(0.34f, 0.54f, amount);   // 段1: 濡れ縁（大きめに滲む）
    float pool = smoothstep(0.50f, 0.72f, amount);   // 段2: 水たまり（深い水=鏡, 連結した大溜まり）
    wet  *= gate;
    pool *= gate;
    if (wet <= 0.003f) return float4(scene, 1);       // 乾いた地面は一切変更しない

    // --- 平らな水面/濡れ面として反射（法線=世界上）---
    float3 N = float3(0, 1, 0);
    float3 V = normalize(P - CameraWorld.xyz);
    float3 R = reflect(V, N);

    // --- 平面反射RT（ミラー町＋本物の空）を画面UVでサンプル。濡れ=強めのぼかし(粗い面)、
    //     水たまり=シャープ(鏡)。pool で 2 段のぼかし半径を補間。---
    float2 rbW = float2(0.0018f, 0.0030f);   // 濡れ縁（少しだけボケる）
    float2 rbP = float2(0.0007f, 0.0012f);   // 水たまり（ほぼ鏡＝シャープ）
    float2 rb = lerp(rbW, rbP, pool);
    float4 reflGeo =
          g_Reflection.SampleLevel(g_Lin, In.uv, 0) * 0.250f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2( rb.x, 0), 0) * 0.125f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2(-rb.x, 0), 0) * 0.125f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2(0,  rb.y), 0) * 0.125f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2(0, -rb.y), 0) * 0.125f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2( rb.x,  rb.y), 0) * 0.0625f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2(-rb.x,  rb.y), 0) * 0.0625f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2( rb.x, -rb.y), 0) * 0.0625f
        + g_Reflection.SampleLevel(g_Lin, In.uv + float2(-rb.x, -rb.y), 0) * 0.0625f;
    float3 refl = min(reflGeo.rgb, 3.0f);   // 白飛び防止クランプ

    float NdotV = saturate(dot(-V, N));
    float fres = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);   // 水の Fresnel

    // 連続合成。濡れ縁は少し暗い＋弱い光沢。水たまりは「空を映して明るい鏡」＝
    // 周囲の地面より明るくなる（現実の空反射水たまりの最重要特性）。暗く潰さない。
    float  darkAmt  = lerp(1.0f, 0.88f, wet);             // 乾き1.0→濡れ0.88（軽い）
    darkAmt         = lerp(darkAmt, 0.62f, pool);         // →水たまり0.62（潰さない）
    float3 wetBase  = scene * darkAmt;
    float  glossy   = saturate(wet * 0.14f + pool * 1.0f);// 濡れ=弱, 水たまり=満
    // 水たまりは正面でも十分反射して空を映す（フロア高め）。濡れ縁は弱く。
    float  reflAmt  = saturate((0.30f + 0.70f * fres) * glossy * Sky.w);
    float3 outc = lerp(wetBase, refl, reflAmt);
    return float4(outc, 1);
}
