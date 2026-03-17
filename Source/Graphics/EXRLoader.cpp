// OpenEXR のみをインクルードする独立した TU（Windows/D3D ヘッダと分離してコンパイル）
// half→float 変換で Imath の巨大ルックアップテーブルを使うと imath_half_to_float_table にリンクします。
// ここではビット演算版を使うため、テーブルを無効化してリンクエラーを回避します。
#define NOMINMAX
#define IMATH_HALF_NO_LOOKUP_TABLE
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImathBox.h>
#include <string>
#include <cstdlib>
#include <stdexcept>

bool LoadEXRToFloatRgba(const char* pathUtf8, int* outW, int* outH, float** outRgba)
{
	if (pathUtf8 == nullptr || outW == nullptr || outH == nullptr || outRgba == nullptr)
		return false;
	*outW = 0;
	*outH = 0;
	*outRgba = nullptr;

	try
	{
		Imf::RgbaInputFile file(pathUtf8);
		Imath::Box2i dw = file.dataWindow();
		const int w = dw.max.x - dw.min.x + 1;
		const int h = dw.max.y - dw.min.y + 1;
		if (w <= 0 || h <= 0)
			return false;

		Imf::Array2D<Imf::Rgba> pixels;
		pixels.resizeErase(h, w);
		file.setFrameBuffer(&pixels[0][0] - dw.min.x - dw.min.y * w, 1, w);
		file.readPixels(dw.min.y, dw.max.y);

		float* rgba = static_cast<float*>(malloc(static_cast<size_t>(w) * h * 4 * sizeof(float)));
		if (rgba == nullptr)
			return false;

		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				const size_t idx = static_cast<size_t>(y * w + x) * 4u;
				rgba[idx + 0] = static_cast<float>(pixels[y][x].r);
				rgba[idx + 1] = static_cast<float>(pixels[y][x].g);
				rgba[idx + 2] = static_cast<float>(pixels[y][x].b);
				rgba[idx + 3] = static_cast<float>(pixels[y][x].a);
			}

		*outW = w;
		*outH = h;
		*outRgba = rgba;
		return true;
	}
	catch (...)
	{
		return false;
	}
}
