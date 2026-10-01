# M1 验证记录

执行环境：Linux amd64，Go 1.23.2，2026-10-01 会话内执行。无网络依赖，GOPROXY=off。此工具链只用于开发验证，不是最终产品发布工具链。

| 检查 | 实际结果 |
|---|---|
| `go test -count=1 ./...` | 22 个顶层测试、53 个叶子测试用例通过 |
| `go test -race -count=20 ./...` | 20 轮通过，未报告数据竞争 |
| `go vet ./...` | 通过 |
| `gofmt` | 源码、命令和测试已格式化 |
| `go test -coverpkg=./src ...` | 核心语句覆盖率 86.8%；不含 CLI 壳层 |
| macOS arm64 / amd64 | 交叉编译成功，产物为 Mach-O |
| Windows arm64 / amd64 | 交叉编译成功，产物为 PE |
| 模拟主链路 | `WEEK 63%` → 无变化不重发 → 过期 `WEEK --` |
| 模拟重置边界 | 到期进入 waiting_reset，不自动生成 100% |

## 未做的验证

未在 macOS 或 Windows 本机运行这些二进制；未执行真实 USB/BLE、Codex 并发连接、Nice!View、nRF52840 或 ZMK 的实机测试。没有 Developer ID、Apple 公证、Authenticode 或安装器验证。

新增 CI 将在三种桌面/服务器 runner 上执行核心测试；**此文不宣称 CI 已运行或通过**。测试在 Linux 上通过与跨平台编译通过是两项独立事实，均不替代原生验收。

本次只新增 Host 模块和文档/独立 CI，没有修改固件原文件，因此没有将未执行的固件回归写成已通过。

## 复现

```sh
cd tools/companion
go test -count=1 ./...
go test -race -count=20 ./...
go vet ./...
go test -coverpkg=./src -coverprofile=coverage.out ./...
go run ./cmd/codex-micro-runtime demo --fixture tests/testdata/weekly.json --scope demo
go run ./cmd/codex-micro-runtime demo --scenario reset --offsets 0,29,30,32,62
```

交叉编译由开发机设置 GOOS、GOARCH、CGO_ENABLED=0 后执行 `go build -trimpath ./cmd/codex-micro-runtime`。本次组合为 darwin/arm64、darwin/amd64、windows/arm64、windows/amd64。
