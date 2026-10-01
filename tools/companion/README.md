# Codex Micro Companion — M1 无 Hook 同步核心

**开发验证版，不是可交付量产用户的 Companion。** 目前只能运行模拟额度与假设备，不能获取真实 Codex 额度，不能连接键盘。没有隐藏的 live 模式。

该模块独立于 ZMK 的 CMake/Kconfig 构建。采用 Go 标准库，无第三方运行时依赖。开发需要 Go 1.23 或更新版本；将来分发编译好的程序时，不要求用户安装 Go、Node 或 Python。正式发布前应使用受支持且已修补的 Go 工具链重新构建。

## 已实现

- Provider 接口，以及只读取显式指定文件的 JSON fixture Provider。
- 按实际窗口周期选择周额度，不按 Free、Plus、Pro 或工作区套餐名称推测。
- 未登录、API Key、无周窗口、多额度池、未知/缺失/非法数值的独立状态。
- 周额度与 credits 不混算；正数不足 1% 显示 `WEEK <1%`，真实耗尽才显示 `WEEK 0%`。
- 原始观测时间不因读缓存、重连、重试而更新；有效期与重置状态每次重新计算。
- 带账号 generation 的内存缓存、限长文件导入、可持久化快照和清空 tombstone。
- 调度单步接口：启动、定时、手动、唤醒、网络恢复、重连事件；请求合并、超时、退避和无设备暂停。
- DeviceClient 与假设备：能力握手、设备绑定、连接 session、递增序号、确认校验、去重、重放与旧消息拒绝。
- 账号切换时先清除设备状态，再拒绝旧账号在途结果。

所有套餐样本都是合成能力测试，**不代表相应套餐现在必然提供这些额度**。

## 本地运行

从仓库根目录：

```sh
cd tools/companion
go test ./...
go vet ./...
go run ./cmd/codex-micro-runtime demo --scenario weekly
go run ./cmd/codex-micro-runtime demo --scenario reset --offsets 0,29,30,32,62
go run ./cmd/codex-micro-runtime demo --scenario multi-pool --pool codex
```

同样的 `go` 命令可在 macOS 终端和 Windows PowerShell 使用。

演示默认使用固定模拟时间 `1790856000`，没有真实等待。`--offsets` 指定各次调度的模拟秒数；读取同一条样本不会使其重新变新。标准错误会输出 `SIMULATION ONLY`，标准输出是可解析的 JSON。

`weekly` 默认结果：第 0 秒显示 `WEEK 63%`；第 60 秒不重发相同额度；第 1801 秒显示 `WEEK --` / `stale`。

`reset` 到达模拟重置点后进入 `waiting_reset`，**不会自己恢复成 100%**。测试套件另有明确返回新周期数据后才恢复显示的用例。

场景：`weekly`、`free`、`pro`、`unknown-plan`、`exhausted`、`near-zero`、`no-weekly`、`missing-percent`、`missing-reset`、`reset`、`api-key`、`unauthorized`、`multi-pool`。

显式导入本项目格式的样本：

```sh
go run ./cmd/codex-micro-runtime demo --fixture tests/testdata/weekly.json --scope demo
```

这个 JSON 是本项目 Provider 合同，**不是 app-server 原始响应**。没有对用户目录进行自动扫描，不读取 `auth.json`、聊天记录或系统凭据。

## 源码结构

```text
src/quota.go       数据模型、周窗口选择、有效期、主屏显示状态
src/store.go       Provider、账号 generation、快照持久化、JSON 边界
src/sync.go        事件刷新、合并、退避、账号切换
src/device.go      传输接口、RPC client、假设备
cmd/codex-micro-runtime/main.go  明确标记的模拟命令
 tests/core_test.go             回归测试
```

## 当前边界

没有真实账号 Provider、自动账号检测、原生 USB/BLE 后端、菜单栏/托盘界面、系统事件订阅、后台自动启动、自动重连循环、安装器、签名、公证或自动更新。`Step` 是可调用的调度核心，不是已安装的常驻服务。

假设备声明隔离能力，只是测试合同，**不证明现有固件已具备隔离 channel**。本模块没有选择 HID Report ID、USB endpoint 或 BLE UUID，也不会向旧 `0x02` RPC 流发送任何数据。

文件缓存 API 供后续壳层显式调用；演示程序默认不写任何持久化缓存。生产壳层必须先确认账号身份，再按该 scope 恢复缓存；退出授权时要保存 tombstone。Windows 用户目录 ACL、崩溃恢复和多进程单实例锁仍需实现。包内互斥锁不是跨进程锁。

见 [架构与实施边界](../../docs/architecture/companion-m1.md)、[Usage 协议草案](../../docs/protocol/companion-usage-v1.md)、[任务记录](../../tasks/active/companion-001.md)。
