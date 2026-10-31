// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <dxgi1_6.h>
#include <vector>

// Desktop composition colour for one display. HDR desktops composite in FP16 scRGB, so an
// 8-bit UNORM crop is overclipped; the transport switches format instead of refusing to run.
enum class DisplayColorMode : int { SDR = 0, HDR = 1 };

struct DisplayColorInfo {
    DisplayColorMode mode = DisplayColorMode::SDR;
    // Luminance Windows renders sRGB white at inside the HDR signal (paper white).
    float sdrWhiteNits = 80.0f;
    float maxLuminance = 80.0f;
    float maxFullFrameLuminance = 80.0f;
    // False when DISPLAYCONFIG could not report the user's SDR content brightness.
    bool whiteLevelMeasured = false;
};

namespace displaycolor {

// DISPLAYCONFIG_SDR_WHITE_LEVEL is a multiplier of 80 nits, scaled by 1000.
inline bool sdrWhiteLevelNits(HMONITOR monitor, float& nits) noexcept {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
            modes.data(), nullptr) != ERROR_SUCCESS) return false;
    paths.resize(pathCount);
    for (auto const& path : paths) {
        // GDI device name is the only join key between a monitor handle and a display path.
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return false;
        if (!white.SDRWhiteLevel) return false;
        const auto value = static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
        if (!(value > 0.0f)) return false;
        nits = value;
        return true;
    }
    return false;
}

// DXGI reports the display's actual output space, so PQ/BT.2020 and scRGB both mean HDR.
inline bool isHdrColorSpace(DXGI_COLOR_SPACE_TYPE space) noexcept {
    return space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

} // namespace displaycolor

// Never throws: any probe failure degrades to the SDR transport that already works.
inline DisplayColorInfo queryDisplayColor(HMONITOR monitor, IDXGIOutput6* output) noexcept {
    DisplayColorInfo info;
    if (!output) return info;
    DXGI_OUTPUT_DESC1 color{};
    if (FAILED(output->GetDesc1(&color))) return info;
    info.mode = displaycolor::isHdrColorSpace(color.ColorSpace)
        ? DisplayColorMode::HDR : DisplayColorMode::SDR;
    if (color.MaxLuminance > 0.0f) info.maxLuminance = color.MaxLuminance;
    if (color.MaxFullFrameLuminance > 0.0f) info.maxFullFrameLuminance = color.MaxFullFrameLuminance;
    float white = 0.0f;
    if (displaycolor::sdrWhiteLevelNits(monitor, white)) {
        info.sdrWhiteNits = white;
        info.whiteLevelMeasured = true;
    } else if (info.mode == DisplayColorMode::HDR) {
        // Fall back to a plausible paper white rather than the 80-nit reference, which would
        // make every SDR window on the desktop read as a highlight.
        info.sdrWhiteNits = info.maxLuminance * 0.1f;
    }
    if (!(info.sdrWhiteNits > 0.0f)) info.sdrWhiteNits = 80.0f;
    return info;
}
