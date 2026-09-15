# SubCue · 自动字幕打轴工具

Windows 10/11 桌面端自动字幕打轴工具。界面使用 Qt 6 Quick/QML，采用接近 Adobe Audition 的深色专业音频编辑布局。用户字幕文案始终作为最终文本（一行 = 一格字幕），ASR 提供词级毫秒时间戳，本地算法和可选 AI 复核只接受有真实音频证据的连续词区间；文案中存在但音频中不存在的行会保留为“音频未检出”，不会生成或导出虚构时间段。

AI 辅助支持通用 OpenAI-compatible Provider，也可在设置中直接“添加 Qwen”。百炼新版 `sk-ws-` API Key 使用 Bearer 认证，连接测试会自动加载 Qwen 模型；Qwen 仅复核低置信度字幕，语音时间戳仍由所选 ASR Provider 生成。

编辑器运行路径是原生 C++（Qt + FFmpeg Library API）。仓库里不再包含 Python/PySide6/Nuitka 应用或打包脚本。

## 界面布局

```
┌───────────────────────────────────────────────────────┐
│ 命令栏: 打开  自动打轴  取消  导出  设置  任务状态     │
├──────────┬──────────────────┬─────────────────────────┤
│ 素材/文稿│ 视频预览         │ 字幕列表               │
│ 拖放媒体 │ 按片源比例信箱   │ 一行一格，置信度着色   │
│ 文案编辑 │ 字幕叠加         │ 点选定位播放头         │
├──────────┴──────────────────┴─────────────────────────┤
│ 传输栏: 时间码 │ J/K/L │ 倍率 │ 吸附 │ 缩放          │
├───────────────────────────────────────────────────────┤
│ 时间轴: 尺标 │ S1 可拖/修剪字幕块 │ A1 音频波形     │
└───────────────────────────────────────────────────────┘
```

- 播放头、预览当前帧、列表选中行、时间轴块四者双向同步。
- 字幕块悬停显示覆盖范围；中间拖动位置，左右沿修剪入出点，最短 250ms，禁止重叠和跨邻块。
- 支持播放头、相邻字幕、入出点和整帧吸附；支持静音倒放与逐帧移动。
- 未加载媒体时提供默认 60 秒时间域，仍可拖动播放头和底部滑块；点击缩放数字可直接输入百分比，`+/-` 每次精确变化 10%，此时“适应”恢复 100%。
- 支持从资源管理器直接拖入媒体文件和字幕文案（TXT/SRT）。
- 底部 Navigator 中间拖动只浏览，左右手柄调整可见范围；播放时自动翻页，拖动 Navigator 或连续滚轮浏览期间暂停跟随。
- 双击字幕块或列表正文编辑文字；列表时间可以定位播放头。待确认字幕保留可编辑时间，未定位行通过“在播放头创建”建立人工时间段。
- 自动打轴在工具栏下显示阶段与分块进度，无法计量的阶段显示不定进度；取消保留已有字幕。

## 核心快捷键

- 播放：`Space` 播放/暂停，`J/K/L` 倒放/停止/正放，`←/→` 前后 1 帧，`Shift+←/→` 前后 5 帧。
- 打轴：`3/4` 设置播放头后/前字幕边界，`T` 设置两条字幕衔接点，`[/]` 设置当前字幕入/出点，`C` 切开字幕。
- 编辑：`Enter` 新建或编辑字幕，`Shift+Enter` 从文稿取下一句创建字幕，`↑/↓/Tab` 导航字幕。
- 撤销与删除：`Ctrl+Z` 撤销，`Ctrl+Y` 重做，`Delete` 删除选中字幕；文本编辑框内 Enter 提交、Esc 取消。
- 范围：`I/O` 设置入/出点，`Alt+I/Alt+O` 清除入/出点。
- 时间轴：`S` 切换吸附，`=` 放大 10%，`-` 缩小 10%，`\` 适应整个时间轴，`Ctrl+滚轮` 每次缩放 10%。

## 环境

- Windows 10/11 x64
- MSVC x64、CMake 3.28+、Ninja
- Qt 6.8+（当前验证为 6.10.3 MSVC 2022 x64；目标 6.11.2）
- 项目内 `vcpkg_installed/x64-windows` 动态 FFmpeg（avcodec/avformat/avfilter/swresample/swscale）
- 云端 ASR 使用阿里云 DashScope API Key；AI 复核可配置支持 `GET /models` 的 OpenAI-compatible Provider

不需要 Python、PySide6、Nuitka，也不需要外部 `ffmpeg.exe` / `ffprobe.exe`。

## 构建与运行

`CMakePresets.json` 通过 `$env{Qt6_ROOT}` 定位 Qt，建议一次性持久化：

```powershell
setx Qt6_ROOT "C:\Qt\6.10.3\msvc2022_64"
```

日常迭代用 `tools\build-windows.bat`（默认 Debug）或 `tools\build-windows.bat release`。脚本会进入 MSVC x64 开发者环境、按需配置、增量构建并跑 CTest；**已有构建目录时不会重新配置**，只有加 `fresh` 参数才清空重建：

```powershell
tools\build-windows.bat            # 增量构建 Debug + 全量 CTest
tools\build-windows.bat release    # 增量构建 Release + 全量 CTest
tools\build-windows.bat release fresh   # 工具链变更或缓存损坏时才需要
```

手动等价命令（需自行设好 `Qt6_ROOT` 并进入 MSVC 环境）：

```powershell
cmake --preset windows-debug        # 仅首次或 CMakeLists 变更后
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

Debug 启动用根目录下的 `run-debug.bat`，Release 启动用 `tools\run-release.bat`；二者会补全 Qt、FFmpeg，以及 Debug 版额外需要的调试 CRT 与 SDK ucrt（这些都不在默认 `PATH` 上），可透传参数如 `tools\run-release.bat --smoke-test`。

要交互式调试（断点、调用栈、QML 调试）请用 Visual Studio 打开本文件夹并选择 `windows-debug` 预设，或用 Qt Creator，它们会自动配好上述运行环境。

### CUDA 加速（可选）

本地 Qwen3 推理使用独立 Python 进程中的 PyTorch CUDA 运行时，需要兼容的 NVIDIA 显卡：

```powershell
tools\build-windows.bat cuda        # 配置并构建 out/build/windows-release-cuda
tools\package-windows.bat cuda      # 便携包 → dist/windows-x64-cuda（随带 CUDA 运行时 DLL）
tools\run-cuda.bat                  # 启动 CUDA 版 Release
```

本地识别的设备选择由 Python 推理运行时决定；CUDA 便携包同时保留 CUDA Toolkit 运行时 DLL。

只跑部分测试（全量约 45 秒，其中 `SubCueEditorIntegrationTests` 占大部分）：

```powershell
ctest --preset windows-debug -R SubCueTimelineTests
ctest --preset windows-debug -E SubCueEditorIntegrationTests
```

1. 拖入或选择媒体文件（支持窗口任意位置拖放）。
2. 在左侧面板粘贴文案，或导入 UTF-8 TXT/SRT；导入 SRT 时旧时间轴会被忽略。
3. 点击工具栏「设置」，分别配置并测试 ASR 与可选 AI Provider；密钥保存到 Windows Credential Manager。
4. 点击「自动打轴」。
5. 在时间轴上拖拽字幕块微调时间，或在字幕列表中查看低置信度和音频未检出项。
6. 点击「导出」，字幕写入 `{媒体名}_字幕文件/` 文件夹。

设置保存在 `%APPDATA%\SubCue\settings.json`，日志保存在 `%APPDATA%\SubCue\logs\app.log`。旧版 DPAPI 密钥会在回读验证成功后迁移到 Windows Credential Manager；迁移失败时保留旧文件。

## 项目结构

```
SubCue/
├── CMakeLists.txt              # C++20 / Qt Quick / FFmpeg
├── CMakePresets.json           # windows-debug / windows-release
├── cmake/PackageWindows.cmake  # 便携包：Qt + FFmpeg + VC Runtime
├── docs/                       # 架构与测试清单
├── licenses/                   # 随包第三方声明
├── src/
│   ├── app/                    # AppController、应用入口与原生主题
│   │   └── qml/                # 三栏编辑器、设置窗、关于与控件
│   ├── core/                   # 媒体、播放、对齐、ASR、AI、字幕、工程
│   └── platform/windows/       # DPAPI、WASAPI、D3D11 预览
├── tests/
│   ├── cpp/                    # 按模块命名的 Qt Test / CTest
│   ├── golden/                 # 对齐与字幕格式 fixtures
│   └── media/                  # 小型 CFR/VFR/音频/损坏样本
├── run-debug.bat               # 补齐运行时依赖后启动 Debug 版
└── tools/
    ├── build-windows.bat       # MSVC 环境中的增量 CMake/CTest
    ├── run-release.bat         # 补齐运行时依赖后启动 Release 版
    ├── run-cuda.bat            # 补齐运行时依赖后启动 CUDA 版 Release
    ├── package-windows.bat     # Release 便携包 → dist/windows-x64
    ├── generate_alignment_golden.py
    ├── requirements-golden.txt # 仅 golden 再生需要 rapidfuzz
    └── python_ref/             # 对齐算法对照实现，不是编辑器运行路径
```

模块划分与逐个测试目标的职责见 [docs/architecture.md](docs/architecture.md)。

## 测试

```powershell
ctest --preset windows-debug --output-on-failure
ctest --preset windows-release --output-on-failure
```

覆盖：时间码、TXT/SRT/ASS、Undo/Redo、媒体探针与解码、播放时钟/Seek、波形与缩略图、时间轴、对齐 golden、双 ASR、AI mock、QML 编辑器、打轴流水线、快捷键矩阵、A/V sync 与稳定性、Windows 便携包布局与隔离 PATH 冒烟。

若需要重新生成对齐 golden（可选，需本机 CPython 3.11 + rapidfuzz）：

```powershell
python -m pip install -r tools/requirements-golden.txt
python tools/generate_alignment_golden.py
```

## 发布

Release 便携目录由 `tools\package-windows.bat` 生成（默认 Release），输出到 `dist/windows-x64`。该目录随带 Qt、FFmpeg 动态库和 MSVC 运行库，不需要 Python、Visual Studio 或外部 FFmpeg 命令行工具。第三方许可证在包内 `licenses/`。

```powershell
tools\package-windows.bat
.\dist\windows-x64\SubCue.exe
```

CUDA 版由 `tools\package-windows.bat cuda` 打包到 `dist/windows-x64-cuda`，CUDA Toolkit DLL 位于包根目录，PyTorch 的 CUDA 运行时位于独立的 `inference/` 目录：

```powershell
tools\package-windows.bat cuda
.\dist\windows-x64-cuda\SubCue.exe
```

FFmpeg、ASR/LLM 权重不会被打包进源码树。本地模型保存在工作区，不随程序分发。
"# SubCue" 
"# SubCue"  
