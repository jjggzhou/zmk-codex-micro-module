# companion-001 — 无 Hook 同步核心（M1）

状态：M1 代码与本地验证已完成；等待 PR 审阅和原生 CI。整个量产 Companion 项目仍处于 active，不能作为完整交付关闭。

## 范围

建立独立 Host 模块和模拟协议合同；不改既有固件。基于远端 c43d4ec，不假定它等于用户当前 worktree。未来接入固件前重新确认最新源码。

## 本次验收

- [x] Provider/Normalizer/SnapshotStore/Coordinator/DeviceClient 分层实现。
- [x] 不同套餐名称不影响同样的真实窗口数据；支持未知模式、未授权、API Key、无周窗口、多池歧义和 credits 分离。
- [x] 缺失数值不转为 0；真实 0 和小于 1% 正值有区别。
- [x] 到期不自动恢复 100%；新观测到达后才恢复。
- [x] 缓存不刷新 observed_at，账号 generation 拒绝旧响应，SwitchScope 先清屏。
- [x] 假设备握手、回复校验、去重、重放、乱序和重连恢复测试。
- [x] Linux 下测试、并发检测、静态检查与四目标交叉编译。
- [x] 增加 macOS/Windows/Linux 原生 CI 定义（结果待执行确认）。
- [ ] PR 审阅与三平台原生 CI 通过。

本地测试细节见 `tools/companion/VALIDATION.md`。交叉编译不是原生系统运行测试，假设备不是实机 HID。

## 明确未实施

真实额度授权/查询、HID/BLE 后端、固件 Usage Service、Nice!View 集成、GUI、系统事件订阅、自动重连循环、单实例锁、自启动、签名安装器与更新。没有访问或修改用户 Codex 配置、Token、键盘参数或现有 Hook。

## 后续任务顺序

companion-002：确认当前固件基线，冻结隔离通道合同，再实现原生 USB 与固件 Usage Service；以 Codex/Companion 共存、输入/灯光/路由不回归为门槛。

companion-003：真实 Provider 的技术与授权确认、账号工作区切换、额度来源一致性；不在未确认前启用商业自动同步。

companion-004：双平台壳层与分发、安全存储、自动重连/自启动、诊断、签名、升级卸载与实际设备验收。

## 变更流程

需求与不变量 → 数据/协议合同 → 单元与模拟集成测试 → PR 审阅 → 当前固件集成 → 双平台实机与旧功能回归 → 验证记录 → 才能关闭对应任务。
