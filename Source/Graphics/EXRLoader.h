#pragma once

// EXR を float RGBA で読み込む。OpenEXR をこのヘッダではインクルードしない（Windows/D3D との衝突を避ける）。
// 成功時は *outRgba を free() すること。
bool LoadEXRToFloatRgba(const char* pathUtf8, int* outW, int* outH, float** outRgba);
