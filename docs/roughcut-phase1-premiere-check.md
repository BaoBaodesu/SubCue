# 自动粗剪 Phase 1：Premiere 验收

构建 `SubCueRoughCutXml` 后执行：

```powershell
out\build\windows-debug\SubCueRoughCutXml.exe --create-acceptance-fixture out\phase1-acceptance
```

目录中会生成完整源音频 `phase1_source.wav`、片段清单和 `roughcut.xml`。源音频依次包含六个不同音高的标记段，每段两秒声音加一秒静音：A、B1、B2、C、D1、D2。粗剪时间线只包含 A、B2、C、D2。

在 Premiere Pro 2026 中导入 `roughcut.xml`，完成以下检查：

1. 打开导入的 `SubCue Phase 1 Acceptance` 序列，确认粗剪轨依次播放 A、B2、C、D2。
2. 确认 `ORIGINAL REFERENCE` 所在参考轨处于禁用状态，启用后能连续播放全部六段。
3. 为 B2 左侧腾出空间，再向左拖动 B2 的左边缘，确认能够找到更早的 B1。
4. 对 D2 重复上述操作，确认能够找到 D1。
5. 对 B2、D2 分别执行 Match Frame 和 Slip Edit，确认源监视器仍可访问完整 `phase1_source.wav`。
6. 确认 Premiere 没有提示媒体离线，也没有生成或引用分句 WAV。

只有以上项目全部通过，Phase 1 才算通过，之后才能进入 ASR 阶段。
