# Repository Guidelines

## Project Structure & Module Organization

SubCue is a C++20 desktop application built with Qt 6, QML, FFmpeg, and an isolated Python CUDA worker. Native code lives under `src/`: `app/` contains controllers and QML-facing types, `core/` contains media, playback, alignment, ASR, cache, and rough-cut logic, `inference/` builds the isolated Python worker executable, and `tools/` contains command-line programs. QML screens and reusable controls are in `src/app/qml/`. Tests are in `tests/cpp/`; fixtures belong in `tests/media/` and `tests/golden/`. Packaging logic is under `cmake/`, while maintenance scripts live in `tools/`. Treat `.venv-inference/`, `out/`, and `dist/` as generated or local-only content. ASR model weights live outside the repo (`SUBCUE_MODELS_ROOT`, default `E:/AIModels/ASR/models`).

## Build, Test, and Development Commands

Run commands from an MSVC x64 developer prompt on Windows:

```bat
tools\build-windows.bat            # 默认 CUDA Release + 全量 CTest
tools\build-windows.bat cuda
tools\build-windows.bat release    # 仅无 CUDA / 不用本地推理时
tools\build-windows.bat debug
ctest --preset windows-release-cuda --output-on-failure
tools\package-windows.bat          # 默认 CUDA 便携包
```

The build script configures when needed, builds all targets, and runs CTest. Add `fresh` only after toolchain changes or a damaged cache. Daily development keeps `out/build/windows-release-cuda` only. Launch with `SubCue.bat` from the project root (resolves the current CUDA build at start time). Use `--roughcut-workspace` to start in the rough-cut workspace and `--smoke-test` for a noninteractive startup check. Local models are downloaded with `tools\setup-models.ps1` into `SUBCUE_MODELS_ROOT`.

## Coding Style & Naming Conventions

Follow the surrounding Qt/C++ style: four-space indentation, braces on their own line for functions, `PascalCase` types, `camelCase` methods and locals, and trailing underscores for private members. Use `QStringLiteral` for UI strings and UTF-8 Chinese comments where a domain rule needs explanation. Keep QML object IDs in `camelCase`. Prefer focused changes that preserve existing structure. Do not modify IDE or user settings. MSVC builds enforce `/W4`, `/permissive-`, and `/utf-8`.

## Testing Guidelines

Tests use Qt Test. Name files `test_<feature>.cpp` and targets `SubCue<Feature>Tests`; register them through `subcue_register_test`. Add tests for behavioral rules, serialization, cancellation, cache invalidation, and boundary conversions. Run the relevant target first, then the full preset. Packaging changes must also pass `SubCuePackagingTests` and both startup tests.

## Commit & Pull Request Guidelines

Recent history uses short, imperative summaries, often written in Chinese. Keep each commit focused, for example: `Fix rough-cut project version migration`. PRs should explain the user-visible behavior, important implementation constraints, and exact validation commands. Link related issues when available and include screenshots for QML changes. Call out model/runtime requirements and any known pre-existing test failures.

## Security & Configuration

Never commit credentials, downloaded model weights, build output, or Python virtual environments. Keep Python and Torch confined to `.venv-inference/` and the packaged `inference/` directory; `SubCue.exe` must not import Python directly. Do not download models from the application; use `tools/setup-models.ps1` only.

## Project Size and Packaging Constraints

- Keep ASR model weights outside the repository (`SUBCUE_MODELS_ROOT`, default `E:/AIModels/ASR/models`). Do not copy weights into `models/`, the build tree, or the release package. Protect the configured model root: record file count, total size, and SHA-256 before any change, then verify against `tools/models-manifest.json`. Download models only with `tools/setup-models.ps1`.
- Daily development keeps only `out/build/windows-release-cuda`. CPU/Debug presets are for machines without CUDA or when local inference is unused; delete them after use. Inference runtimes on disk: at most `.venv-inference/` plus one CUDA package.
- A self-contained CUDA release must retain `cublas64_13.dll`, `cublasLt64_13.dll`, and `cudart64_13.dll`. Do not sacrifice offline operation merely to reduce package size. CPU packages must not ship Python or Torch.
- Before adding a dependency, resource, or release file, confirm that the runtime actually uses it. Do not copy the source repository, test fixtures, CLI tools, Python environments, probe artifacts, or unused Qt Quick Controls styles into the release package. New runtime files must appear in `package-manifest.txt` and pass `packageRestrictsPythonAndCliTools`.
- The application uses the `Basic` style. Do not restore Material, Fusion, Universal, Imagine, Fluent, or Windows style resources unless product code explicitly switches to another style.
- Do not bundle `vc_redist.x64.exe` when the VC Runtime DLLs are already fully deployed.
- Do not keep duplicate staging and `dist` copies of the same release package after builds, tests, or packaging. Remove staging after copying and verification succeed.
- Place temporary probe targets, old build directories, root-level logs, and failed-download fragments in an ignored temporary directory, and remove them when the task ends.
- After changing CMake, packaging, or dependencies, report the final package size. Keep the CPU package at or below 610 MiB. Keep the CUDA package at or below 4.8 GiB and report the Torch and cuBLAS share; if it exceeds that target, explain the added content and why it is necessary.
- For cleanup, use only the preview-first workflow in `tools/clean-workspace.ps1` (default `-KeepBuild windows-release-cuda`) or the in-app `StorageCleaner`. Any new cleanup target must resolve to an absolute path, stay within the project or app data directories, and exclude the model root, `src/`, `tests/media`, `tests/golden`, `.git/`, and the current build directory except its `package/` staging folder.
- Before deleting `.git/lfs/objects`, run `git lfs ls-files --all`; retain the objects if any historical reference exists. Do not delete reachable Git objects or rewrite history to reduce repository size without explicit user authorization.
- When updating `.gitignore`, ensure models, logs, IDE caches, `CMakeUserPresets.json`, temporary downloads, and duplicate build artifacts neither trigger Git LFS clean nor enter version control.

## Memory and Resource Lifecycle Constraints

- Do not add `.detach()`, unmanaged threads, unmanaged network requests, or asynchronous callbacks that capture raw controller pointers. Background tasks must have a single owner and support cancellation and waiting for completion.
- Propagate cancellation flags to providers, HTTP requests, downloads, verification, decoding, and Worker processes. After a controller is destroyed, no signal or callback may be delivered to it or access its members.
- On application shutdown, request cancellation first and then wait for tasks to exit. Release FFmpeg, image, audio, D3D, network, and temporary-file resources promptly when media changes, closes, or fails.
- Do not hide ownership problems with periodic `gc.collect()`, scheduled cache clearing, or unconditional forced reclamation. Fix lifecycle, shared references, and cache limits first.
- For long media, do not retain intermediate PCM, floating-point audio, complete frame histories, or duplicate images whose size grows linearly with media duration. Use streaming aggregation and fixed-size conversion buffers.
- Retain only small conversion buffers and the final waveform pyramid. When media changes or closes, both the controller and timeline must release their shared references to the old waveform.
- Keep the combined per-frame GOP cache and fallback history at or below 128 MiB. Prioritize the current and recent frames; decode missing cache entries on demand.
- Playback packet queues, frame queues, and audio buffers must have explicit count or byte limits. New caches must expose queryable byte usage and eviction on overflow.
- Load Qwen ASR and Forced Aligner sequentially. On phase transitions, task completion, and cancellation, run Python GC and release CUDA cache; terminate the Worker and remove temporary audio directories.
- New background features require cancellation and destruction tests proving that no threads, Workers, network requests, temporary directories, or delayed callbacks remain after exit.
- New media caches or waveform algorithms require boundary tests. Temporary memory for long media must not grow with duration, and frame-cache tests must assert a limit of 128 MiB.
- For media lifecycle changes, retain a regression test covering 50 open, switch, play, and close cycles. After warm-up, target no more than 32 MiB of private-memory fluctuation near the end of the test.
