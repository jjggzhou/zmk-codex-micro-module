# M1 验证记录

## 证据范围

验证以 GitHub Actions 的实际步骤和日志为准。初始草稿中的本地测试次数、覆盖率、交叉编译与本地/远端哈希一致性声明没有在本记录中附可复核输出，现撤下这些未验证声明。不要把初始提交说明中的数字当成独立验收证据。

本次没有在用户的 Mac、Windows 电脑或真实键盘上运行；编写测试、交叉编译、原生 CI 运行与实机验收是不同层级。

## 已核实的首轮 CI

- Run: https://github.com/jjggzhou/zmk-codex-micro-module/actions/runs/36877296383
- PR head: `3767eca69cb9f822eb21348b7b1a93b878e8ac0a`
- Linux job `110419877920`：格式、`go vet`、测试、race 检查、构建、模拟重置命令通过。
- macOS job `110419878431`：格式、`go vet`、测试、构建、模拟重置命令通过；race 步骤按配置未运行。
- Windows job `110419878378`：格式检查失败，后续业务测试被跳过。不能记为 Windows 测试通过。

首轮 Windows 日志显示 runner 为 Windows Server 2025 amd64，Go 工具链为 1.27.1。失败发生在 `gofmt -l` 检查，不是已经执行后的业务测试失败。

## 修复与可重复验证

本次修订增加 `tools/companion/.gitattributes`，只对新的 Companion 模块强制 LF，不改变既有固件文件的换行策略。格式检查失败时现在会打印文件清单及 diff。

CI 增加机器可读测试输出、Linux 20 轮 race 检查、实际覆盖率输出与 darwin/windows × amd64/arm64 四目标交叉编译，便于直接核对证据。依赖的 GitHub Actions 固定为首轮实际执行的 commit SHA。

**修复后的通过与否须以对应新 run 的实际结论为准；此处不预写通过结果。**

## 未做的验证

无真实额度数据源、无原生 HID/BLE 后端，因此没有真实 USB/BLE、多客户端共存、Nice!View、nRF52840 或 ZMK 的实机结果。没有 Developer ID、Apple 公证、Authenticode 或安装器验证。CI 的 macOS/Windows 是托管 runner，不是用户设备。

本 PR 只新增 Host 模块、文档与独立 CI，没有修改固件原文件，不将未执行的固件回归记为已通过。

## 本地复现命令

下列是复现入口，不表示已经在用户电脑运行：

```sh
cd tools/companion
go test -count=1 ./...
go test -race -count=20 ./...
go vet ./...
go test -coverpkg=./src -coverprofile=coverage.out ./...
go run ./cmd/codex-micro-runtime demo --fixture tests/testdata/weekly.json --scope demo
go run ./cmd/codex-micro-runtime demo --scenario reset --offsets 0,29,30,32,62
```
