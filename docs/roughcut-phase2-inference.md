# 自动粗剪 Phase 2：推理运行环境

## 固定运行环境

- Python：3.11.16，项目专用目录 `.venv-inference`
- PyTorch：2.6.0+cu124
- Qwen ASR：0.0.6
- 模型：直接引用 `models/qwen3-asr-0.6b` 与 `models/qwen3-forced-aligner-0.6b`
- 不修改系统 Python，不下载或复制模型权重

重新创建环境：

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup-inference-env.ps1
```

脚本从 PyTorch 官方 CDN 断点续传 CUDA wheel，并在安装前校验 SHA-256。环境完成后会执行真实 CUDA 张量运算。

## 2026-09-14 实机验证

- GPU：NVIDIA GeForce RTX 3060 Ti 8GB
- `torch.cuda.is_available()`：`True`
- CUDA 张量运算：通过
- Qwen3-ASR：成功识别 `models/fun-asr-nano-2512/example/zh.mp3`
- 识别文本：`開放時間：早上九點至下午五點。`
- Forced Aligner：成功返回 0.64–5.12 秒逐字时间
- 无对齐器模式：返回 0–5.622 秒真实分块覆盖范围，并标记 `timing=chunk`

Fun-ASR 目录中的 `model.pt.incomplete` 不是可用权重，本阶段不把它标记为可运行模型，也不触发隐式模型下载。

## Worker 协议

`SubCueInference.exe --stdio` 内嵌 Python 3.11，并从标准输入逐行接收 JSON。每个任务必须包含 `taskId`，标准输出返回同一任务 ID 的 `accepted`、`progress` 和 `result` 事件。进度区分模型加载、识别和对齐阶段；识别结果通过 `resultPath` 文件传递，诊断日志写入标准错误。

主程序中的 `InferenceManager` 串行化 GPU 推理，并在取消或 Worker 超时后结束子进程。字幕编辑器原有 Whisper 与本地 Qwen/Fun-ASR 调用共享同一门禁。

分析缓存读取完整媒体内容并计算 SHA-256，缓存键还包含模型、语言、参数和算法版本。修改媒体中间内容也会使缓存失效。
