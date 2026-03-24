// 細いミップから 2x2 の最大深度で粗いミップを構築（Forward-Z / far=1 向け）。
// max-depth は遠景側に倒れるため、遮蔽判定は保守的（誤カリングしにくい）。
Texture2D<float> gFine : register(t0);
RWTexture2D<float> gCoarse : register(u0);

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
	uint x0 = tid.x * 2;
	uint y0 = tid.y * 2;
	uint x1 = min(x0 + 1, SrcSize.x - 1);
	uint y1 = min(y0 + 1, SrcSize.y - 1);
	float z00 = gFine.Load(int3(int2(x0, y0), 0));
	float z10 = gFine.Load(int3(int2(x1, y0), 0));
	float z01 = gFine.Load(int3(int2(x0, y1), 0));
	float z11 = gFine.Load(int3(int2(x1, y1), 0));
	gCoarse[tid.xy] = max(max(z00, z10), max(z01, z11));
}
