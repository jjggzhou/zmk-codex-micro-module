# Companion M1：无 Hook 额度同步核心

日期：2026-10-01。阶段：M1 实现，尚未完成量产集成。

## 实施位置与基线

本次仅在 `jjggzhou/zmk-codex-micro-module` 增加独立的 `tools/companion/`、文档、任务记录和独立 CI。基线为远端 `codex/firmware-implementation` 的 `c43d4ec93c1815a7f4899d74afd84f1b5bd3e7ca`。这个远端版本不代表用户 Mac 上尚未同步的 worktree。

不修改现有 `src/`、`include/`、CMake、Kconfig、USB 描述符、BLE Report Map、灯光 FSM、输入路由或 config 仓库。不把新增 Host 功能耦合进 ZMK 构建。

## 数据路径

```text
Provider → Normalize → SnapshotStore → Coordinator → DeviceClient
                                                        ↓
                                              Transport 接口
                                                        ↓
                                                 FakeDevice（M1）
```

真实账号授权、原生 HID 和固件服务属于后续阶段。Go 标准库实现是为了单文件原生分发和共享业务核心；macOS/Windows 的实际传输、GUI、权限和签名不能由“交叉编译通过”替代验收。

## 数据合同

`RawSnapshot` 是本项目内部接口。身份由不透明 scope 表达，至少区分账号和工作区。Provider 必须在同一次一致性读取中取得窗口使用比例与对应重置时间，不得拼接不同账号或不同窗口的数据。外部 API 的 primary/secondary 等字段必须由适配器转换，不能把位置当作语义。

周窗口目前严格识别 `duration_minutes == 10080`。有多个候选时要求显式选择 pool；单个 pool 内仍有多个周窗口时保持 selection_required。未知套餐名称不是错误条件。没有识别到周窗口时显示未知，不把短周期改名为 WEEK。

输入百分比范围为 0..100；NaN、Infinity、缺失和越界均拒绝，而非静默修正。Host 将剩余比例量化到 0..10000 的基点；不足一个基点的正数保留为 1，以免误报耗尽。0..1% 的正值显示 `<1%`。credits 只保留在原始 Provider 输入，不改变周百分比；M1 尚未提供 credits 详情 UI。

只有成功的新额度观测才改变 observed_at。fixture 重读、持久化缓存恢复和传输重试都保留它。持久化格式有 schema、scope 和可为空的 snapshot；空值是清空 tombstone。读取有 64 KiB 和 32 层嵌套边界，拒绝重复 JSON 键、未知顶层字段和尾随对象。

## 显示状态与时间

正常 ready 快照包含周剩余比例，reset_at 可为空。没有重置时间仍可以显示可信百分比，但倒计时未知。达到 reset_at 后显示 waiting_reset，不自行修改比例或将 reset_at 加七天。

ViewAt 每次从原始快照重新推导 ready、stale、waiting_reset 或 clock_invalid，绝不缓存时间派生状态。默认新鲜度 30 分钟，未来偏差超过 60 秒的观测被调度器拒绝；缓存 LoadAt 同样拒绝未来污染。

设备模型以收到的 Host UTC 加可信单调 elapsed 计算倒计时。MCU reboot/System OFF 后如单调时间连续性失效，必须丢弃时间锚点等待同步。M1 只在 FakeDevice 中建模，不表示现有屏幕代码已修改。

## 调度与一致性

默认参数：10 分钟查询，10 秒手动事件最小间隔，5 秒 Provider deadline，2 秒设备 deadline。重置点后 2 秒可查询；数据尚未变更时从 30 秒开始退避，上限为查询间隔。生产 Provider 和 Transport 必须响应 context 取消；不能用不受控、忽略取消的实现。

一次 Step 独占一次刷新，其他并发调用返回 ErrBusy，由正在执行的刷新服务该事件；这是进程内合并，不是持久化任务队列。客户端断开时暂停取额度，重新 Connect 后由下一次 Step 补发。自动发现/重连循环及 OS 唤醒监听尚未实现。

账号 SetScope 递增 generation 并清空缓存。生产壳层调用 Coordinator.SwitchScope，立即发布空状态；旧 generation 的查询结果不得提交。发送与 scope 切换之间有受 deadline 约束的读锁，避免 scope 切换已返回后仍发送旧账号结果。

授权失效覆盖旧缓存并禁止定时重试登录，只允许显式操作再次尝试。网络错误保留最后可信样本，但 TTL 和重置边界继续生效。系统时钟大幅回拨的自动检测、选择池的运行时切换和多进程实例锁尚需壳层实现；不得直接按 OS 当前时间改写历史观测时间。

## 不可变的固件边界

额度服务只有 Host usage state 的写权限；键盘本地 layer、活动连接、充电、电池和灯光不由它控制。主屏保留此前确定的顶部本地状态与底部周额度/重置时间，不新增账号名、5 小时额度、模型名或 RPC 状态。

生产传输必须隔离 Companion 和 Codex 的分片重组、回复关联与业务所有权。非独占 HID open 不等于消息级原子性。M1 只写出了隔离 transport 的合同；Report ID/channel/endpoint/UUID 的最终分配，要在用户当前固件和两套桌面系统上验证后决定。

## 发布门槛与下一阶段

M2：以用户当前工作树为基线，新增独立 Usage Service 与隔离传输；测试同一设备的 Codex/Companion 共存，不改灯光/输入所有权。确认原厂方法兼容与资源预算后才连接真硬件。

M3：确认数据源技术能力与授权适用范围，添加真实 Provider；不要把本地技术可用性等同于商业发行许可，也不要把身份登录自动等同于额度读取权限。本次未接入、复制、刷新任何真实 Token。

M4：菜单栏/托盘、系统事件、单实例、自动重连、用户级自启动、诊断、签名更新、Windows ACL、macOS Developer ID/公证和双平台实机验收。

## 参考与来源

参考的是分层、故障处理和模拟测试思路，未复制参考仓库代码或运行时依赖。

- Claude 会话状态参考： https://github.com/00cyre/claude-code-keypad/tree/b92bb2bfca48c5a05b740d2b477acda210e09f97
- 设备 SDK 参考： https://github.com/00cyre/creator-micro-kit/tree/a563b88c2dc251371dd4c670690a01f8980fc338
- Codex app-server 文档（未来适配器需重新验证）： https://developers.openai.com/codex/app-server
- Go JSON 行为： https://pkg.go.dev/encoding/json
- Go 构建环境： https://go.dev/doc/install/source#environment
