# 应用级云端连接配置

在“设置 → AI 配音 → 通义 Flash 云端连接”点击“新增”，填写连接名称、Workspace ID 和凭据，点击设置底部“保存”后生效。连接下拉列表用于查询和切换；名称、Workspace ID 和凭据引用可编辑；“删除”暂存删除操作，统一保存后写入数据库。取消或关闭时仍沿用设置草稿提醒。

当前选中的连接供 AI 配音工作区使用，重启后保留。新增连接分配独立凭据引用；存在未保存的密钥草稿时阻止切换或删除，避免将密钥写到另一条连接。删除连接不删除安全凭据，也不修改已有工程和生成历史。

数据库位于应用设置目录的 `connections.sqlite`；Windows 默认路径为 `%APPDATA%\SubCue\connections.sqlite`，与 `settings.json` 同目录。`cloud_connections` 表保存 `id`、`name`、`workspace_id`、`credential_ref` 和 `active`。API Key 不进入 SQLite 或 JSON，继续交由安全凭据库保存。

连接列表及当前选择通过同一 SQLite 事务保存；输入验证失败会回滚。JSON 中尚存的旧 `ttsCloudWorkspace` 在下次保存时迁移为默认连接；迁移失败保留原 JSON。无法读取连接数据库时阻止继续保存，避免空列表覆盖原数据。存储清理保护此数据库。

回归覆盖 SQLite 文件格式与重新读取、新增、编辑、删除、当前连接恢复、无效选择回滚、旧 JSON 迁移及迁移失败保护，以及设置内的连接切换和草稿撤销。真实云端合成不属于此项存储回归。

2026-10-16 验证：`tools\build-windows.bat cuda` 完整 CTest **34/34 通过（95.41 秒）**；SQLite 与凭据定向测试、Windows 原生 150% 设置交互测试通过。证据位于 `out/validation/workspace-connections-20261016/`。CUDA 暂存包 5,101,030,153 字节（4.75GiB），Torch 扣除 cuBLAS 为 3,235,925,188 字节，cuBLAS 合计 1,080,248,544 字节；未增加依赖。验证后清理打包暂存并保留当前构建。
