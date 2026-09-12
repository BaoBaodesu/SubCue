# 第三方依赖与许可证策略

本文件不是法律意见。发布前必须根据最终二进制和 vcpkg resolved graph 再生成一次清单。

## 目标原生版本

- Qt 6.11.2：动态链接；仅使用 Qt Core、Gui、Qml、Quick、QuickControls2、Network、Multimedia、Test 等 LGPL 可用模块，不链接 GPL-only 的 Qt Quick Timeline。Phase 1 暂以 6.10.3 验证，不能视为最终版本替代。
- FFmpeg 9.0.1#1：通过已锁定 baseline 的 vcpkg `x64-windows` 动态构建，只启用 avcodec、avformat、avfilter、swresample、swscale；禁止 `gpl`、`nonfree`、x264、x265。
- whisper.cpp v1.9.1：MIT；只链接核心库，不复制许可证存疑的示例源码。
- rapidfuzz-cpp v3.3.3：MIT。

## 发布要求

- 随包附带 Qt、FFmpeg 及其实际传递依赖的许可证文本。whisper.cpp 仅在实际链接核心库后再附其 MIT 文本。RapidFuzz 为 in-tree 实现，不链接 rapidfuzz-cpp。
- 保存 vcpkg baseline、resolved versions、FFmpeg configure options、对应源码下载地址及修改补丁。
- FFmpeg DLL 保持原名并提供相同源码；About 对话框和发布说明注明 FFmpeg LGPL 使用情况。
- 不将 Whisper 模型混同为程序源码依赖；模型清单独立记录来源、许可证、大小和 SHA-256。当前清单在 `src/core/asr/whisper_model_catalog.cpp`，URL 为 `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-{id}.bin`，SHA-256 取自 Hugging Face LFS oid。

## 当前锁定信息

- vcpkg builtin baseline：`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`。
- manifest 未启用 FFmpeg 的 `gpl`、`nonfree`、`x264` 或 `x265` feature。
- Phase 3 实际构建参数包含 `--disable-avdevice --disable-ffmpeg --disable-ffprobe --disable-static --enable-shared`；安装目录中不存在 `avdevice` DLL 和 FFmpeg CLI 程序。
- 本机 PATH 中若仍有 FFmpeg 4.4.1 GPL CLI 构建，不得进入 C++ Release 包或链接图。编辑器不再调用 `ffmpeg.exe`/`ffprobe.exe`。

## Phase 13 随包清单

Release 便携目录（`dist/windows-x64`）实际带上：

- Qt 6.10.3 动态库与 QML/平台/TLS 插件（LGPLv3 文本在 `licenses/qt/LICENSE.LGPLv3`）。
- FFmpeg 9.0.1#1 动态库原名：`avcodec-63.dll`、`avformat-63.dll`、`avutil-61.dll`、`swresample-7.dll`、`swscale-10.dll`；版权文件在 `licenses/FFmpeg/copyright`。不包含 avdevice、avfilter 或 FFmpeg CLI。
- MSVC x64 CRT DLL（`vcruntime140.dll` 等）以及 windeployqt 放入的 `vc_redist.x64.exe`。
- `licenses/NOTICE.txt` 与本文件副本。

未随包：Python、whisper.cpp 核心库、RapidFuzz 库（对齐用 in-tree 实现）、Whisper 模型文件。About 对话框指向上述许可证目录。
