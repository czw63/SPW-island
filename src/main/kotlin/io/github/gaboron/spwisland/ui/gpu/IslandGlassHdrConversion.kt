// SPDX-License-Identifier: GPL-3.0-only
package io.github.gaboron.spwisland.ui.gpu

import io.github.gaboron.spwisland.diagnostics.IslandLog
import org.jetbrains.skia.*

/**
 * Wraps an FP16 scRGB desktop crop in a shader that emits encoded sRGB.
 *
 * The backdrop shaders ([IslandGlassShaders.LENS], [IslandGlassShaders.MATERIAL] and the WCAG
 * luminance test in `IslandLyricContrast`) all operate on sRGB-encoded values. Feeding them
 * linear scRGB would silently change every blend and shift the whole material, so the crop is
 * converted at its single entry point instead of at the end of the optics chain.
 *
 * Doing it as a shader rather than a pre-baked image means the GPU shared-texture transport and
 * the CPU readback transport share one curve and cannot drift apart, and no extra render target
 * or DirectContext plumbing is needed.
 */
internal object IslandGlassHdrConversion {
    private var program: RuntimeEffect? = null
    private var failed = false

    /**
     * Returns a shader over [image] that outputs encoded sRGB, or null when this runtime cannot
     * compile the program. The caller then keeps its previous frame rather than painting raw
     * scRGB values as if they were already sRGB.
     */
    fun shader(image: Image, paperWhiteScale: Float): Shader? {
        if (failed) return null
        return try {
            val effect = program ?: RuntimeEffect.makeForShader(PROGRAM).also { program = it }
            val builder = RuntimeShaderBuilder(effect)
            try {
                builder.child("source", image.makeShader(FilterTileMode.CLAMP, FilterTileMode.CLAMP,
                    FilterMode.NEAREST))
                builder.uniform("paperWhiteScale", paperWhiteScale)
                builder.uniform("ceiling", IslandGlassToneMap.HIGHLIGHT_CEILING)
                builder.makeShader()
            } finally { builder.close() }
        } catch (error: Throwable) {
            failed = true
            IslandLog.warn("rendering", "hdr.tone_map_unavailable",
                mapOf("reason" to (error.message ?: error.javaClass.simpleName)))
            null
        }
    }

    private val PROGRAM = """
        uniform shader source;
        uniform float paperWhiteScale;
        uniform float ceiling;

        float srgbEncode(float v) {
            return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
        }

        half4 main(float2 coord) {
            half4 raw = source.eval(coord);
            // scRGB 1.0 is 80 nits; SDR white sits at paperWhite nits, so dividing by that ratio
            // puts desktop white back at 1.0. Negative coordinates are out-of-gamut colours.
            float3 linear = max(float3(raw.rgb) / paperWhiteScale, float3(0.0));
            float peak = max(max(linear.r, linear.g), linear.b);
            float ratio = 1.0;
            if (peak > 1.0) {
                // Compress on the brightest channel so highlights desaturate towards white
                // instead of shifting hue, and keep the rolloff C1 at the join.
                float headroom = ceiling - 1.0;
                ratio = (1.0 + headroom * (1.0 - 1.0 / (1.0 + (peak - 1.0) / headroom))) / peak;
            }
            float3 encoded = float3(srgbEncode(linear.r * ratio),
                srgbEncode(linear.g * ratio), srgbEncode(linear.b * ratio));
            return half4(half3(encoded), clamp(raw.a, 0.0, 1.0));
        }
    """
}
