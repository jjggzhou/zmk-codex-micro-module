# Companion Usage v1 — M1 模拟合同

**这是新增协议草案和 FakeDevice 的实现合同，不是现有固件接口或 OpenAI 官方方法。** 未分配 HID channel、Report ID 或 BLE UUID。禁止直接把本协议接到原有 `CODEX_CHANNEL_RPC = 0x02`。

## 分层

本合同处理完整 UTF-8 JSON 消息（最多 1024 字节），使用 JSON-RPC 2.0 envelope。原生传输后续负责分片、独立重组、超时、断线和定向回复。不得依赖前后加 CRLF 就能保证多个进程的消息不交错。

## 1. micro.capabilities

新连接首先发送 `method: micro.capabilities`、`params: null`。回复必须匹配 JSON-RPC id，并包含：

```json
{"schema_version":1,"device_id":"device-opaque-id","session":"0123456789abcdef0123456789abcdef","isolated":true,"max_message_bytes":1024}
```

session 为当前设备连接生成的 16 字节随机值，编码为 32 位十六进制。每次重新 Open 都更换 session，sequence 从 1 开始。它用于拒绝旧连接消息，**不是身份鉴权或加密**。USB 信任边界和 BLE 配对/绑定需另行设计。

如果 schema 不兼容、没有隔离能力、绑定的设备身份不一致、session 无效或消息预算不足，客户端关闭连接，不尝试在旧协议上降级发送。

`isolated: true` 是能力声明，不能替代平台端点与固件重组代码的实机审核。

## 2. micro.usage.set

```json
{
  "jsonrpc":"2.0",
  "id":2,
  "method":"micro.usage.set",
  "params":{
    "schema_version":1,
    "session":"0123456789abcdef0123456789abcdef",
    "sequence":1,
    "view":{
      "status":"ready",
      "observed_at":1790856000,
      "host_now":1790856002,
      "stale_after_s":1800,
      "weekly":{"remaining_bps":6300,"reset_at":1791115200}
    }
  }
}
```

数值均为示例。reset_at、observed_at 和 host_now 单位为 UTC Unix 秒；stale_after_s 是秒，不是毫秒。remaining_bps 范围 0..10000，6300 表示剩余 63%。字段缺失/null 不可解释成 0。

非 ready 状态必须 `weekly: null`，用同一个方法清除旧值，不合并旧百分比。状态集合为 ready、unavailable、unauthorized、unsupported、no_weekly、selection_required、stale、waiting_reset、clock_invalid。

完整快照不包含账号邮箱、scope、套餐名称、Token、模型名称和 credits。顶部键盘状态不通过此 RPC 下发。

## 3. 应用确认

成功 result：

```json
{"session":"0123456789abcdef0123456789abcdef","sequence":1,"applied":true}
```

确认必须发生在新 usage state 已被原子提交之后。Host 只在 id、session、sequence 和 applied 全部校验通过后记录已同步；USB 写成功或收到任意相同 id 的广播不算完成。

同一 session 下，小于已提交 sequence 的消息拒绝；相同 sequence 仅允许字节等价的 params 重放，回复确认但不重复应用。相同序号不同内容拒绝。序号间允许跳跃：丢失确认后 Host 用新序号和当前时间锚点重发当前完整快照，原 observed_at 不变。

缓存去重忽略 host_now 的普通推进，但不忽略新 observed_at、status、周数据或有效期变化。重连后清除去重缓存并全量恢复。

## 4. 时间和失效

固件从收到的 Host UTC 和接收后单调 elapsed 推算当前时间，不要求已有持久 RTC。新鲜度基于 observed_at + stale_after_s，而不是“上次收包时间”。达到 reset_at 只进入 waiting_reset，不能自行生成 100% 或新 reset_at。

生产原生后端必须限制排队/写入时间（目标 deadline 2 秒），不能把长期滞留报文当作新的时间同步。跨 System OFF/重启的时间锚点无效；需重新握手和同步。量产前还需测定传输延迟预算、校时策略与断线过期表现。

## 5. 必测负例

旧 session、乱序 sequence、同序号不同数据、错误 id、错误确认、重复 JSON 键、缺失数值、过长/过深 JSON、过期 ready、错误能力版本、没有隔离能力、设备身份不符、掉线后仍命中去重缓存。

M1 在假设备上覆盖这些关键合同；原生帧编码、分片丢失和多客户端交错测试属于 M2，不应标记为已完成。
