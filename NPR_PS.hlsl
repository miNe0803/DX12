// NPR toon shading pixel shader（ルートテーブルは PBR と同じ t0-t5 を連番バインド）
// t0 Albedo, t1 Normal, t2 Metallic, t3 Roughness, t4 Ramp, t5 Sphere（未使用スロットは白テクスチャ）

struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL; // world-space normal
    float3 tangent : TANGENT; // world-space tangent
    float3 worldPos : TEXCOORD1;
    float4 nprPerMesh : TEXCOORD2;
};

SamplerState smp : register(s0);
Texture2D _AlbedoMap : register(t0, space0);
Texture2D _NormalMap : register(t1, space0);
Texture2D _MetallicMap : register(t2, space0);
Texture2D _RoughnessMap : register(t3, space0);
Texture2D _RampTex : register(t4, space0);
Texture2D _SphereMap : register(t5, space0);

cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams; // RimParams.y = NormalScale, RimParams.z = RimPower
    float4 CameraPos;
    float4 NprTuning;
    float4 NprTuning2;
    float4 NprDebugHdr; // 予約（定数バッファサイズ合わせ、未使用）
};

float4 main(VSOutput input) : SV_TARGET
{
    // 4: テクスチャ・clip より前に出す = NPR_PS が動いていれば必ず真緑（PBR では出ない）
    float dbgRamp = NprTuning2.w;
    if (dbgRamp > 3.5f && dbgRamp < 4.5f)
        return float4(0.0f, 1.0f, 0.0f, 1.0f);

    // 5=t0 生サンプル（sRGB テクスチャの値そのまま。RenderDoc の t0 サムネと同系で全身を確認）
    // 6=t0×頂点カラー（pow 前。PMX Diffuse 焼き込み後）
    // 7=リニア化後×頂点まで（スフィア前。シェーディングに入るベースに近い）
    float4 albedoTex = _AlbedoMap.Sample(smp, input.uv);
    if (dbgRamp > 4.5f && dbgRamp < 5.5f)
        return float4(albedoTex.rgb, 1.0f);

    float4 albedo = albedoTex;
    // PMX マテリアル Diffuse は頂点カラーに焼き込み。不透明パスでは AssimpLoader が Color.a=1 を保証（Early-Z のため clip は使わない）
    albedo *= input.color;
    if (dbgRamp > 5.5f && dbgRamp < 6.5f)
        return float4(albedo.rgb, 1.0f);

    // 1. sRGB -> Linear デコード（既存PBRと整合）
    albedo.rgb = pow(max(albedo.rgb, 1e-5), 2.2f);
    if (dbgRamp > 6.5f && dbgRamp < 7.5f)
        return float4(albedo.rgb, 1.0f);

    // PMX スフィア（InstanceData.NprPerMesh.y）
    int sphMode = (int)floor(input.nprPerMesh.y + 0.5f);
    if (sphMode > 0)
    {
        float4 sph = _SphereMap.Sample(smp, input.uv);
        sph.rgb = pow(max(sph.rgb, 1e-5), 2.2f);
        if (sphMode == 1)
            albedo.rgb *= sph.rgb;
        else
            albedo.rgb += sph.rgb;
    }

    // 未使用 SRV の最適化除去対策（メタル/ラフは NPR で未参照）
    albedo.rgb += (_MetallicMap.Sample(smp, input.uv).r + _RoughnessMap.Sample(smp, input.uv).r) * 0.0;

    // 2. NormalMapのデコードとTBNワールド法線化
    float4 nSample = _NormalMap.Sample(smp, input.uv);
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    float3 decodedNormal = nSample.rgb * 2.0f - 1.0f;
    float nScale = (RimParams.y > 0.001f) ? RimParams.y : 1.0f;
    float3 normalTS = normalize(float3(0, 0, 1) + (decodedNormal - float3(0, 0, 1)) * nScale);
    float3 worldNormal = normalize(mul(normalTS, TBN));

    // 3. ライト計算
    float3 L = normalize(float3(0.5f, 0.7f, -1.0f));
    float NdotL = dot(worldNormal, L);
    // halfLambert は 0.0(完全な影) 〜 0.5(明暗の境界) 〜 1.0(完全な日向) となる
    float halfLambert = saturate(NdotL * 0.5f + 0.5f);

    // 4. 量子化によるトゥーン化（NprTuning2.y = celShadeSharpness を反映。未使用だと常に 5 段で toon3 の差が見えにくい）
    float celSharp = saturate(NprTuning2.y);
    float steps = lerp(8.0f, 3.0f, celSharp);
    float q = clamp(floor(halfLambert * steps), 0.0f, steps - 1.0f);
    float u = (q + 0.5f) / steps;

    // Rampからの階調サンプリング (Mip0強制)
    float3 rampColor = _RampTex.SampleLevel(smp, float2(u, 0.5f), 0).rgb;
    // toon3 が「明るい帯」寄りだと R が高く、lerp が常に日向側に寄って平坦に見える → コントラストを付ける
    float rampR = saturate(rampColor.r);
    float rampGamma = lerp(1.0f, 1.9f, celSharp);
    rampR = pow(max(rampR, 1e-5f), rampGamma);

    // 1=ランプRGB 2=u（5段灰） 3=halfLambert（滑らかな灰）
    if (dbgRamp > 0.5f && dbgRamp < 1.5f)
        return float4(rampColor.r, rampColor.g, rampColor.b, albedo.a);
    if (dbgRamp > 1.5f && dbgRamp < 2.5f)
        return float4(u, u, u, albedo.a);
    if (dbgRamp > 2.5f && dbgRamp < 3.5f)
        return float4(halfLambert, halfLambert, halfLambert, albedo.a);

    float3 baseColor = albedo.rgb;

    // ==========================================
    // 7. 全身用・疑似SSS（一旦オフ）
    // ==========================================
    /*
    // ★ 全身用・疑似SSS（ターミネーター血色・強烈版）★
    // halfLambert(0.0=影, 1.0=光) のうち、明暗の境界線(0.45付近)を広めに狙い撃ち
    float sssMask = smoothstep(0.30f, 0.45f, halfLambert) - smoothstep(0.45f, 0.60f, halfLambert);
    float3 sssTint = float3(1.0f, 0.15f, 0.3f);
    float sssIntensity = 1.0f;
    baseColor += sssTint * sssMask * albedo.rgb * sssIntensity;
    */

    // ==========================================
    // 8. Rimライト 【プラスチック感の解消】
    // ==========================================
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float rimPower = (RimParams.z > 0.001f) ? RimParams.z : 5.0f;
    float rim = pow(1.0f - saturate(dot(worldNormal, V)), rimPower);
    
    // 純白(1.0)を足すのではなく、アルベドの色を残したまま明るくする
    baseColor += rim * albedo.rgb * 0.4f;

    // ==========================================
    // 9. 最終出力（線形 HDR）
    // ==========================================
    float3 finalColor = baseColor;

    // premultiplied RGB（Composite と Transparent と整合）
    return float4(finalColor * albedo.a, albedo.a);
}
