#ifndef DITHER_CROSSFADE_HLSLI
#define DITHER_CROSSFADE_HLSLI

// ============================================================
// Alpha Dither Crossfade for LOD transitions
// Eliminates hard pop between LOD levels by rendering both
// adjacent LODs with complementary dither patterns.
// ============================================================

// 4x4 Bayer ordered dithering matrix (values 0..15 / 16)
static const float kBayerMatrix[4][4] = {
    { 0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0 },
    { 12.0/16.0, 4.0/16.0, 14.0/16.0,  6.0/16.0 },
    { 3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0 },
    { 15.0/16.0, 7.0/16.0, 13.0/16.0,  5.0/16.0 }
};

/// Apply dither-based alpha clip for LOD crossfade.
/// fadeFactor: 0.0 = fully transparent (fading out), 1.0 = fully opaque (fading in).
/// screenPos: SV_POSITION.xy
void ApplyDitherCrossfade(float fadeFactor, float2 screenPos)
{
    // No crossfade needed if fully opaque
    if (fadeFactor >= 1.0)
        return;

    uint2 px = uint2(screenPos) % 4;
    float threshold = kBayerMatrix[px.y][px.x];

    // Clip pixel if fadeFactor is below the dither threshold
    clip(fadeFactor - threshold - 0.001);
}

/// Compute the fade factor for an instance in a LOD transition zone.
/// dist: XZ distance from camera
/// lodNearEnd: the distance where the near LOD starts fading out
/// lodFarStart: the distance where the far LOD starts fading in
/// Returns: 1.0 outside transition zone, 0.0-1.0 inside
float ComputeLodFadeFactor(float dist, float lodBoundary, float transitionHalf)
{
    // Transition zone: [boundary - half, boundary + half]
    float fadeStart = lodBoundary - transitionHalf;
    float fadeEnd   = lodBoundary + transitionHalf;

    if (dist <= fadeStart) return 1.0; // fully near LOD
    if (dist >= fadeEnd)   return 0.0; // fully far LOD

    return 1.0 - saturate((dist - fadeStart) / (fadeEnd - fadeStart));
}

#endif // DITHER_CROSSFADE_HLSLI
