# 液态玻璃与 HDR 桌面

Windows 上液态玻璃的后景来自桌面实时捕获。桌面是 HDR 时，系统用 FP16 的 scRGB 合成，一个 8 位 UNORM 的截图装不下这个信号；上游因此直接拒绝在 HDR 桌面上运行。本文说明这个限制的成因、绕不开的取舍，以及现在的实现。

适用于 Windows 11 + SPW。Linux 合成器路径与此无关，见[开发指南](development.md#液态玻璃)。

## 结论

HDR 桌面现在可以用液态玻璃，但**输出仍是 SDR**：后景按系统参考白映射回 sRGB 编码后再参与玻璃光学。这不是真 HDR 输出，是刻意的选择，原因见下文「为什么不做真 HDR 输出」。

## 问题是怎么出现的

上游 `native/glass/DesktopCapture.cpp` 里有一个主动的拒绝：

```cpp
void requireSdr(com_ptr<IDXGIOutput6> const& output) {
    DXGI_OUTPUT_DESC1 color{};
    check_hresult(output->GetDesc1(&color));
    if (color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        throw hresult_error(E_NOTIMPL, L"Liquid Glass currently supports SDR displays only");
}
```

它有两个调用点：建立采集会话时一次，之后每秒一次。建立会话那一次排在创建帧池之前，所以在 HDR 桌面上采集线程一开始就抛出，一帧都拿不到，玻璃从未出现，直接退回经典背景。日志里留下的是 `capture.worker HRESULT=0x80004001`（`E_NOTIMPL`）。

去掉这个检查只是第一步。真正的约束在后面。

## 绕不开的约束：Skia 不能包装 FP16 共享纹理

很自然的想法是让捕获端直接输出 `R16G16B16A16_FLOAT`，把 FP16 通过共享纹理交给 Skia，再在 GPU 上做色调映射。

这条路走不通。`WindowsGpuTextureImport` 的交接是这样的：

```kotlin
BackendRenderTarget.makeDirect3D(width, height, resource, 87, 1, 1).use { target ->
    checkNotNull(Surface.makeFromBackendRenderTarget(context, target, SurfaceOrigin.TOP_LEFT,
        SurfaceColorFormat.BGRA_8888, null)) { "Direct3D 共享纹理无法包装为 Skia 表面" }
}
```

`Surface.makeFromBackendRenderTarget` 对 `R16G16B16A16_FLOAT` 直接返回 null。这不是 HDR 才触发的分支——只要格式是 FP16 就会失败，SDR 机器上一样。于是所有机器都退到经典背景，日志里的第一条错误是包装失败，随后才出现底层「操作成功完成（HRESULT 0）」这种看起来自相矛盾的报错：失败点在 Kotlin 侧的包装，不在原生采集。

结论：**FP16 不能跨越共享纹理这道交接**。任何把宽格式塞进现有传输链的方案都会失败，不管显示器是什么。

## 方案

把转换放在交接**之前**，而不是之后：

```
WGC 帧池 (FP16 scRGB, HDR)
  └─ HdrToneMap：D3D11 像素着色器 → B8G8R8A8_UNORM 纹理
       └─ DesktopReadback / DesktopSharedTexture / Skia   ← 与上游完全一致
```

好处是 `DesktopReadback.h`、`DesktopSharedTexture.h`、`DesktopViewport.h`、`WindowsGpuTextureImport.kt`、`IslandGlassImage.kt` 全部保持上游原样。这几处是整条链上最容易踩驱动差异的部分，改它们等于把「已验证可用」换成「理论上更好」。

### 显示器色彩探测

`native/glass/DisplayColor.h` 回答两个问题：这块屏是不是 HDR，以及它的参考白是多少。

`DXGI_OUTPUT_DESC1.ColorSpace` 判断模式（`RGB_FULL_G2084_NONE_P2020` 或 `G10_NONE_P709` 视为 HDR）。参考白走 `DISPLAYCONFIG_SDR_WHITE_LEVEL`：它是 80 nit 的倍数再乘 1000，所以除以 1000 再乘 80 就是 Windows 把 sRGB 白渲染成的实际亮度，也就是用户在「SDR 内容亮度」里调的那个值。

`QueryDisplayConfig` 拿到的路径只能靠 GDI 设备名和 `HMONITOR` 对上，所以要先 `GetMonitorInfoW` 取 `szDevice`，再用 `DISPLAYCONFIG_SOURCE_DEVICE_NAME` 逐条比对。探测失败时不会抛异常，退回 `MaxLuminance * 0.1`——用 80 nit 当参考白会把桌面上每个 SDR 窗口都读成高光。

### 色调映射

`native/glass/HdrToneMap.h`，一次 `Draw(3, 0)` 的全屏三角形：

```hlsl
float3 linear = max(raw.rgb / paperWhiteScale, 0.0);   // paperWhiteScale = 参考白 / 80
float peak = max(max(linear.r, linear.g), linear.b);
float ratio = 1.0;
if (peak > 1.0) {
    float headroom = ceiling - 1.0;                     // ceiling = 1.25
    ratio = (1.0 + headroom * (1.0 - 1.0 / (1.0 + (peak - 1.0) / headroom))) / peak;
}
return float4(srgbEncode(linear.r * ratio), srgbEncode(linear.g * ratio),
              srgbEncode(linear.b * ratio), 1.0);
```

三个要点：

**SDR 内容精确还原。** scRGB 1.0 是 80 nit 的 D65 白，而 Windows 把 sRGB 白放在参考白（常见 200–500 nit）。SDR 像素在 scRGB 里的值正好是 `linear_srgb × 参考白/80`，除以同一个数就回到 `linear_srgb`，再 `srgbEncode` 就回到原始 sRGB 值。数值验证在 80 / 203 / 480 nit 三档下最大误差 2.2e-16，即完全恒等。所以 HDR 桌面上玻璃和周围 SDR 窗口的亮度是对齐的，这是「不做真 HDR 输出」能成立的前提。

**按最亮通道压缩，而不是逐通道截断。** 逐通道 clamp 会毁掉色相：一个 800 nit 的橙色高光，三个通道分别超限的程度不同，夹到 1.0 之后变成一个偏黄的色块。按 peak 统一求比例则保留通道间关系：

| 800 nit 橙 `(800, 400, 100)`，参考白 203 nit | R | G | B |
| --- | --- | --- | --- |
| 逐通道截断 | 1.000 | 1.000 | 0.730 |
| 本实现 | 1.095 | 0.807 | 0.429 |

（编码后的值，未做 8 位夹断。R 通道两边都会在最终写入时夹到 1.0，差别在 G：截断把它拉到饱和，本实现留在 0.807，所以橙色还是橙色。）

**C1 连续。** 压缩在 `peak = 1.0` 处的斜率与不压缩一侧相同（数值验证 0.439583 / 0.439583），所以参考白附近不会出现一条可见的接缝。渐近线是 `srgbEncode(1.25) = 1.1028`，即高光向白色收敛而不是无限变亮。

输出纹理是 8 位，编码值超过 1.0 会被夹掉；`ceiling` 的作用不是留出可见的超白，而是决定参考白到纯白之间那一段怎么分布。

### 会话管理

像素格式是帧池的属性，所以 HDR 开关一变就必须重建：

```cpp
if (mode != sessionColor.mode) {
    closeSession();
    monitor = nullptr;   // 下一轮走会话建立分支
    continue;
}
```

这里没有用 `pool.Recreate` 单独换格式，而是整段关掉再走一遍建立流程：重建路径本来就要重新校验显示器句柄、采集项和窗口排除列表，复用它比维护一条只换格式的旁路更不容易漏掉状态。代价是切换时有一次几百毫秒的黑屏间隙，只发生在用户手动开关 HDR 的时候。

每秒用 `queryDisplayColor` 复检一次，和上游原来检查 `requireSdr` 的频率相同。用户在 Windows 设置里切 HDR 后，玻璃会在约一秒内自行恢复，不需要重开插件。

顺带把 `MinUpdateInterval` 从 `milliseconds(0)`（每个 vblank）改成 33 ms（30 fps）。后景是一张大幅模糊加折射的画面，30 fps 与屏幕刷新率在视觉上无差别，但少掉的是每帧一次全屏拷贝。这条独立于 HDR，在 SDR 上同样生效。

## 为什么不做真 HDR 输出

词岛是浮在其他窗口之上的一个窗口。真 HDR 输出意味着让它以 scRGB/HDR10 参与桌面合成，而它旁边全是 SDR 窗口——玻璃会明显比周围亮，因为参考白被抬到了 HDR 区间。用户要的是「玻璃看起来像周围的一部分」，不是「玻璃是唯一正确显示 HDR 的窗口」。

要让真 HDR 有意义，需要整个桌面一起进入 HDR 工作流，这超出插件能决定的范围。所以选择映射回 SDR：视觉上和环境一致，实现上也不需要 Skia 支持 HDR 交换链——而它现在连 FP16 共享纹理都包装不了。

## 降级行为

HDR 相关的每一步都只影响 HDR，不会带走玻璃本身：

| 失败点 | 行为 |
| --- | --- |
| 着色器编译失败 | 记录诊断，会话以 SDR 模式建立（8 位路径不需要转换） |
| 转换调用抛错 | 记录诊断，置 `forceSdr`，重建为 SDR 会话 |
| `DISPLAYCONFIG` 探测失败 | 参考白退回 `MaxLuminance * 0.1`，仍按 HDR 处理 |

早期版本在转换失败时直接抛穿帧循环，会结束采集线程并把玻璃退回经典背景。现在不会。

## 诊断

设置里的运行时状态新增一行「桌面色彩」：

- `SDR` — 桌面是 SDR 输出
- `HDR · 参考白 203 nit` — 参考白来自 `DISPLAYCONFIG_SDR_WHITE_LEVEL`
- `HDR · 参考白 480 nit（估算）` — 探测失败，用的是 `MaxLuminance * 0.1`

这行能出现，说明原生侧的探测和映射链路是通的。如果玻璃仍然不出现，看日志里 `capture.native` / `WindowsDesktopBackdrop` 的记录：`HRESULT=0x80004001` 说明还在跑旧版本 DLL；`Direct3D 共享纹理无法包装为 Skia 表面` 说明格式又变成了宽格式。

## 开销

相比上游的 SDR 路径，HDR 每帧多出来两件事：一次 FP16 裁切拷贝（`CopySubresourceRegion`，GPU 到 GPU）和一次全屏三角形的像素着色器。两者都在采集设备上异步完成，不经过 CPU，也不占主屏的绘制队列。

另一处改动方向相反：上游用 `MinUpdateInterval(0)`，让帧池以显示器刷新率投递。提交本身有去重（桌面内容没变就不拷贝），所以静态桌面下两者都几乎不做事；差别出现在**副屏跑动态壁纸**这种持续变化的场景——每个 vblank 都会触发一次拷贝，现在限到 30 fps，拷贝次数减半。后景是一张大幅模糊加折射的画面，30 fps 与屏幕刷新率在视觉上没有区别。

所以在持续变化的桌面上，SDR 的整体开销低于上游；HDR 多了着色器，但拷贝次数同样减半，量级相当。

这些是设计上的推断，没有做帧时间测量。如果实测下来副屏动态壁纸 + 玻璃仍有掉帧，先看「桌面捕获」那一行显示的 fps 是否稳定在 30——不稳说明瓶颈在传输而非着色器。

## 已知限制

- 8 位输出，高光在 `srgbEncode(1.25)` 之后夹掉，暗部仍是 sRGB 精度。
- 参考白每秒复检一次，切换后有约一秒的延迟。
- 只在单显示器内工作（沿用上游的视口约束）；跨屏拖动时按所在显示器重新建立会话。
- 只在 AMD Radeon RX 9070 XT + Windows 11 上验证过，Intel 集显与 NVIDIA 未测。

## 构建

本机没有 MSVC 和 Windows SDK 时，用 GitHub Actions 出包。`.github/workflows/build.yml` 在 `windows-latest` 上分两个 job：`kotlin` 只编译 Kotlin（快速暴露类型错误），`full` 编译原生 DLL 并打包 `.spmod`，产物在 Actions 页面的 Artifacts 里。

`.spmod` 只能在 Windows 上打包（`build.gradle.kts` 里有硬断言），因为包里同时含 Windows 和 Linux 组件。本机想快速验证 Kotlin 改动可以用：

```powershell
.\gradlew.bat --no-daemon compileKotlin -x processResources
```

原生改动必须走完整构建。`native/glass/HdrToneMap.h` 需要 `d3dcompiler.lib`，已加进 `tools/build-glass.ps1` 的链接列表；`DisplayColor.h` 需要 `/DNTDDI_VERSION=0x0A000002` 才能看到 `DISPLAYCONFIG_SDR_WHITE_LEVEL`。

`.github/workflows/build.yml` 会随每次推送到 `main` 触发，也可以在 Actions 页面手动 `workflow_dispatch`。产物保留 90 天，过期后重新跑一次即可。

## 相关文件

| 文件 | 作用 |
| --- | --- |
| `native/glass/DisplayColor.h` | 显示器色彩模式与参考白探测 |
| `native/glass/HdrToneMap.h` | D3D11 色调映射通道 |
| `native/glass/DesktopCapture.cpp` | 会话生命周期、格式选择、降级 |
| `platform/WindowsDesktopBackdrop.kt` | 读取色彩信息、状态展示 |
| `ui/IslandRuntimeStatus.kt` | 「桌面色彩」状态行 |

参考：[DISPLAYCONFIG_SDR_WHITE_LEVEL](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level)
