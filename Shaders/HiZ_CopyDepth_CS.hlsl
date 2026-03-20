// メイン深度バッファ（R32_FLOAT SRV）を Hi-Z ミップ0へコピー
Texture2D<float> gDepth : register(t0);
RWTexture2D<float> gHiZMip0 : register(u0);

cbuffer HiZParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= DstSize.x || tid.y >= DstSize.y)
		return;
	float z = gDepth.Load(int3(int2(tid.xy), 0));
	gHiZMip0[tid.xy] = z;
}
