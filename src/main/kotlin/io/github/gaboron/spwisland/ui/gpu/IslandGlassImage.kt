// SPDX-License-Identifier: GPL-3.0-only
package io.github.gaboron.spwisland.ui.gpu

import io.github.gaboron.spwisland.platform.WindowsDesktopBackdrop
import org.jetbrains.skia.*

/** Clear-rim, transition and soft-center filters of the same crop, cached per desktop frame and DPI. */
internal class IslandGlassImage : AutoCloseable {
    private var source: WindowsDesktopBackdrop.Pixels? = null
    private var scale = 0f to 0f
    /** Raster image this class owns; null when the desktop crop arrived as a borrowed GPU texture. */
    private var image: Image? = null
    private var toneShader: Shader? = null
    private var plainShader: Shader? = null
    private var filtered: Shader? = null
    private var filteredTransition: Shader? = null
    private var filteredEdge: Shader? = null
    private var effect: RuntimeEffect? = null
    private var builder: RuntimeShaderBuilder? = null
    private var mappingKey: Any? = null
    var shader: Shader? = null
        private set
    var edgeShader: Shader? = null
        private set
    var transitionShader: Shader? = null
        private set
    var generation = 0L
        private set

    fun update(sample: WindowsDesktopBackdrop.Sample) {
        if (source !== sample.pixels || scale != (sample.scaleX to sample.scaleY)) upload(sample)
        val pixels = sample.pixels
        val offsetX = sample.islandX - pixels.x
        val offsetY = sample.islandY - pixels.y
        val nextKey = listOf(pixels, sample.scaleX, sample.scaleY, offsetX, offsetY)
        if (mappingKey == nextKey) return
        val program = builder ?: run {
            val compiled = RuntimeEffect.makeForShader(IslandGlassShaders.DESKTOP_COORDINATES)
            effect = compiled
            RuntimeShaderBuilder(compiled).also { builder = it }
        }
        program.child("image", checkNotNull(filtered))
        program.uniform("deviceScale", sample.scaleX, sample.scaleY)
        program.uniform("captureOffset", offsetX, offsetY)
        val next = program.makeShader()
        val nextTransition = try {
            program.child("image", checkNotNull(filteredTransition))
            program.makeShader()
        } catch (error: Throwable) { next.close(); throw error }
        val nextEdge = try {
            program.child("image", checkNotNull(filteredEdge))
            program.makeShader()
        } catch (error: Throwable) { nextTransition.close(); next.close(); throw error }
        transitionShader?.close(); transitionShader = nextTransition
        edgeShader?.close(); edgeShader = nextEdge
        shader?.close(); shader = next; mappingKey = nextKey; generation++
    }

    private fun upload(sample: WindowsDesktopBackdrop.Sample) {
        val pixels = sample.pixels
        val nextScale = sample.scaleX to sample.scaleY
        val owned = if (pixels.texture == null) Image.makeRaster(
            if (pixels.wide) ImageInfo(pixels.width, pixels.height, ColorType.RGBA_F16, ColorAlphaType.PREMUL)
            else ImageInfo(pixels.width, pixels.height, ColorType.BGRA_8888, ColorAlphaType.OPAQUE),
            checkNotNull(pixels.data), pixels.width * if (pixels.wide) 8 else 4) else null
        val crop = pixels.texture ?: checkNotNull(owned)
        // HDR crops arrive as scRGB; every downstream program expects encoded sRGB, so the
        // conversion happens once here instead of at each of the three blur layers.
        val tone = if (pixels.wide) IslandGlassHdrConversion.shader(crop, paperWhiteScale(sample)) else null
        if (pixels.wide && tone == null) {
            // Painting raw scRGB through the sRGB optics would clip every highlight to white, so
            // refuse the frame instead. The caller then falls back to the classic background
            // rather than showing a silently wrong material.
            owned?.close()
            throw IllegalStateException("HDR 桌面需要 tone map 着色器，当前运行时无法编译")
        }
        val plain = if (tone == null)
            crop.makeShader(FilterTileMode.CLAMP, FilterTileMode.CLAMP, SamplingMode.LINEAR) else null
        val cropShader = tone ?: checkNotNull(plain)
        fun abandon() {
            plain?.close(); tone?.close(); owned?.close()
        }
        val nextShader = try {
            filter(cropShader, pixels, sample, IslandGlassShaders.CENTER_BLUR_SIGMA)
        } catch (error: Throwable) { abandon(); throw error }
        val nextTransition = try {
            filter(cropShader, pixels, sample, IslandGlassShaders.TRANSITION_BLUR_SIGMA)
        } catch (error: Throwable) { nextShader.close(); abandon(); throw error }
        val nextEdge = try {
            filter(cropShader, pixels, sample, IslandGlassShaders.EDGE_BLUR_SIGMA)
        } catch (error: Throwable) { nextTransition.close(); nextShader.close(); abandon(); throw error }
        // The picture shaders retain their own references, but the inputs stay alive until the
        // next upload so a live picture can never observe a released image.
        filtered?.close(); filteredTransition?.close(); filteredEdge?.close()
        toneShader?.close(); plainShader?.close()
        image?.close()
        filteredTransition = nextTransition
        filtered = nextShader; filteredEdge = nextEdge; image = owned
        toneShader = tone; plainShader = plain
        source = pixels; scale = nextScale; mappingKey = null
    }

    /**
     * How far SDR paper white sits above scRGB's own 1.0 for this capture.
     *
     * Returns 1.0 for the 8-bit SDR transport, where values are already encoded sRGB.
     */
    private fun paperWhiteScale(sample: WindowsDesktopBackdrop.Sample): Float {
        val color = sample.color ?: return 1f
        if (!color.hdr) return 1f
        return IslandGlassToneMap.paperWhiteScale(IslandGlassToneMap.paperWhite(
            color.sdrWhiteNits, color.maxLuminance, color.whiteLevelMeasured))
    }

    private fun filter(cropShader: Shader, pixels: WindowsDesktopBackdrop.Pixels,
                       sample: WindowsDesktopBackdrop.Sample, sigma: Float): Shader =
        PictureRecorder().use { recorder ->
            val bounds = Rect.makeWH(pixels.width.toFloat(), pixels.height.toFloat())
            val canvas = recorder.beginRecording(bounds)
            Paint().use { paint ->
                // Backdrop effects/ColorFilter.kt: saturation 1.5, contrast 1, brightness 0.
                ColorFilter.makeMatrix(ColorMatrix(
                    1.3935f, -.3575f, -.036f, 0f, 0f,
                    -.1065f, 1.1425f, -.036f, 0f, 0f,
                    -.1065f, -.3575f, 1.464f, 0f, 0f,
                    0f, 0f, 0f, 1f, 0f
                )).use { color ->
                    ImageFilter.makeColorFilter(color, null, null).use { vibrancy ->
                        ImageFilter.makeBlur(sigma * sample.scaleX, sigma * sample.scaleY,
                            FilterTileMode.CLAMP, vibrancy).use { blur ->
                            paint.imageFilter = blur
                            paint.shader = cropShader
                            canvas.drawRect(bounds, paint)
                        }
                    }
                }
            }
            recorder.finishRecordingAsPicture().use {
                it.makeShader(FilterTileMode.CLAMP, FilterTileMode.CLAMP, FilterMode.LINEAR)
            }
        }

    override fun close() {
        shader?.close(); shader = null
        edgeShader?.close(); edgeShader = null
        transitionShader?.close(); transitionShader = null
        filtered?.close(); filtered = null
        filteredTransition?.close(); filteredTransition = null
        filteredEdge?.close(); filteredEdge = null
        toneShader?.close(); toneShader = null
        plainShader?.close(); plainShader = null
        image?.close(); image = null; source = null
        builder?.close(); builder = null
        effect?.close(); effect = null; mappingKey = null
    }
}
