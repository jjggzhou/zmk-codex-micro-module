# companion-001 — 无 Hook 同步核心（M1）

状态：M1 代码已实现，原生 CI 验证中，等待 PR 审阅。整个量产 Companion 项目仍为 active，不能作为完整交付关闭。

## 范围

建立独立 Host 模块和模拟协议合同；不改既有固件。基于远端 c43d4ec，不假定它等于用户当前 worktree。未来接入固件前重新确认最新源码。

## 实现与验收

- [x] Provider/Normalizer/SnapshotStore/Coordinator/DeviceClient 分层实现。
- [x] 不同套餐名称不影响同样的真实窗口数据；支持未授权、API Key、无周窗口、多池歧义和 credits 分离。
- [x] 缺失数值不转为 0；真实 0 与小于 1% 的正值区分。
- [x] 到期不自动恢复 100%；新观测到达后才恢复。
- [x] 缓存不更新 observed_at，账号 generation 拒绝旧响应，SwitchScope 清除旧状态。
- [x] 假设备握手、回复校验、去重、重放、乱序和重连恢复测试实现。
- [x] 首轮 Linux/macOS 原生 CI 的测试、静态检查、构建与模拟命令通过。
- [x] 增加仅针对 Companion 的 LF 规则，修复首轮 Windows 格式检查失败，并改进失败诊断。
- [ ] 修复后的三平台 CI 全部通过，以实际 checks 为准。
- [ ] 从新 CI 日志核对 20 轮 race、覆盖率与四目标交叉编译结果。
- [ ] PR 审阅。

证据范围见 `tools/companion/VALIDATION.md`；不要使用初始草稿里缺少可复核日志的本地数字作为验收证据。交叉编译不是实机运行，假设备不是 HID。

## 未实施

真实额度授权/查询、HID/BLE 后端、固件 Usage Service、Nice!View 集成、GUI、系统事件订阅、自动重连循环、单实例锁、自启动、签名安装器与更新。没有访问或修改用户 Codex 配置、Token、键盘参数或现有 Hook。

## 后续顺序

companion-002：确认用户当前固件基线，冻结隔离通道合同，再实现原生 USB 与固件 Usage Service；以 Codex/Companion 共存、输入/灯光/路由不回归为门槛。

companion-003：真实 Provider 的技术与授权确认、账号工作区切换、额度来源一致性；不在未确认前启用商业自动同步。

companion-004：双平台壳层与分发、安全存储、自动重连/自启动、诊断、签名、升级卸载与实际设备验收。

流程：需求与不变量 → 数据/协议合同 → 单元与模拟集成测试 → PR 审阅 → 当前固件集成 → 双平台实机与旧功能回归 → 验证记录 → 关闭对应任务。
