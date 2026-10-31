// SPDX-License-Identifier: GPL-3.0-only
package io.github.gaboron.spwisland.ui.gpu

/**
 * Paper-white reference for mapping an HDR desktop crop into the glass optics' encoded sRGB.
 *
 * The backdrop shaders ([IslandGlassShaders.LENS], [IslandGlassShaders.MATERIAL] and the WCAG
 * luminance test in `IslandLyricContrast`) all operate on sRGB-encoded values. Feeding them
 * linear scRGB would silently change every blend and shift the whole material, so the crop is
 * normalised back to encoded sRGB at its single entry point: the island must keep looking like
 * the SDR windows beside it, not like a light source.
 *
 * Windows composites HDR desktops in scRGB where 1.0 is D65 white at 80 nits, and renders SDR
 * content at the user's "SDR content brightness" (paper white). Everything at or below that
 * paper white is therefore reproduced exactly, which is what makes the glass agree with its
 * neighbours; only content brighter than every SDR window is compressed.
 *
 * The curve itself lives in [IslandGlassHdrConversion] as a shader, so the GPU shared-texture
 * transport and the CPU readback transport share one implementation.
 */
internal object IslandGlassToneMap {
    /** DirectX scene-referred reference white; scRGB 1.0 equals D65 white at this luminance. */
    const val REFERENCE_WHITE_NITS = 80f
    /**
     * Headroom above paper white, in normalised units. Encoded output may exceed 1.0 here on
     * purpose: the blur layers then see a real highlight falloff instead of a flat plateau, and
     * only the final 8-bit composition clips.
     */
    const val HIGHLIGHT_CEILING = 1.25f

    /**
     * Paper white used to normalise the capture, in nits.
     *
     * A missing DISPLAYCONFIG reading must not fall back to the 80-nit reference: on a display
     * whose SDR slider sits at 200 nits that would render every desktop window as a highlight.
     */
    fun paperWhite(sdrWhiteNits: Float, maxLuminanceNits: Float, measured: Boolean): Float {
        if (measured && sdrWhiteNits.isFinite() && sdrWhiteNits >= 1f) return sdrWhiteNits
        val estimate = maxLuminanceNits * 0.1f
        return if (estimate.isFinite() && estimate >= 1f) estimate else REFERENCE_WHITE_NITS
    }

    /**
     * How much brighter paper white is than scRGB's own 1.0, i.e. `paperWhite / 80`.
     *
     * SDR white therefore lands at this value in the scRGB buffer, and dividing by it is what
     * brings the desktop back to 1.0. Never returns zero or a non-finite value.
     */
    fun paperWhiteScale(paperWhiteNits: Float): Float =
        if (paperWhiteNits.isFinite() && paperWhiteNits > 0f) paperWhiteNits / REFERENCE_WHITE_NITS else 1f
}
