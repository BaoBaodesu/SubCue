# 自动粗剪 Phase 3～8 开发记录

## 已完成核心模块

- Phase 3：UTF-8 TXT、DOCX 正文/显式换行/表格解析，以及不强迫录音服从文案的顺序模糊匹配。
- Phase 4：Forced Alignment 结果校验、原 WAV 采样坐标转换、重叠分块同词去重，以及能量静音证据。
- Phase 5：默认 90 秒邻域的 Retake 分组，保存每个 Take 的评分与理由；质量接近时进入 REVIEW。
- Phase 6：分别保存 `autoDecision` 与可空 `userDecision`。只有失败证据、可信边界和完整替代版本同时存在时自动 CUT。
- Phase 7：按录音原序生成非破坏性时间线，默认 PreRoll 80ms、PostRoll 120ms，并实现五类间距范围。

## Phase 8 开发入口

使用以下参数启动独立粗剪界面：

```powershell
out\build\phase2-nmake\SubCue.exe --roughcut-workspace
```

当前用于验证布局和 QML 生命周期，包含素材/文案区、源波形/播放器区、虚拟化结果列表、状态筛选和粗剪时间线区。正式 `Main.qml` 入口尚未切换，控制器、项目存储、试听和人工编辑连接仍属于 Phase 8 后续工作。

## 验证

针对性测试目标：

- `SubCueRoughCutScriptTests`
- `SubCueRoughCutAlignmentTests`
- `SubCueRetakeDetectorTests`
- `SubCueRoughCutDecisionTests`
- `SubCueRoughCutTimelineTests`

粗剪开发入口使用 `--roughcut-workspace --smoke-test` 验证 QML 能创建并正常退出。
