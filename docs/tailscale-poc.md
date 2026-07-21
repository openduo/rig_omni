# rig_omni Tailscale 集成 — PoC 验证报告

> 验证日期: 2026-07-21
> 硬件: ESP32-S3 (RIG-Hover) + CH340 USB 串口
> 固件: microlink v2.1.0 (Tailscale ESP32 客户端)
> 目标: ESP32 通过 Tailscale 私有网络访问内网 Qwen LLM API

---

## 1. 验证结果

### 全链路打通 ✅

```
ESP32-S3 (100.124.124.111)
  → WiFi (<wifi-ssid>, WPA2)
    → Tailscale 控制面 (Noise_IK 握手, controlplane.tailscale.com)
      → 自建 DERP 中继 (<self-hosted-derp>:443, 北京)
        → WireGuard 隧道 (4 个 peer 全部握手成功)
          → TCP 100.94.32.60:39717 (186ms)
            → HTTP POST /v1/chat/completions
              → Qwen3.5-397B-A17B-FP8 回复 (838 字节) ✅
```

### 关键日志

```
[WG] *** HANDSHAKE COMPLETE! wg_idx=0 key=77fada75 from=8.130.211.171:41641 ***   ← al-h20-02 (Qwen)
[WG] *** HANDSHAKE COMPLETE! wg_idx=1 key=af7a078e from=1.119.195.58:34561 ***    ← mlserver-4x-a800
[WG] *** HANDSHAKE COMPLETE! wg_idx=2 key=99f9747b from=120.246.17.98:41641 ***   ← trader006
[WG] *** HANDSHAKE COMPLETE! wg_idx=3 key=e9940ef3 from=18.179.149.117:41641 ***  ← thesis-tky

ml_tcp: TCP connected to 100.124.67.30:22 (67 ms)       ← mlserver SSH 通了
poc: ✓✓✓ mlserver TCP 连接成功! WG 数据面完全打通!

ml_tcp: TCP connected to 100.94.32.60:39717 (186 ms)     ← Qwen API 通了
poc: ✓ Qwen API 响应(838 字节)— 链路完全打通!
{"model":"Qwen/Qwen3.5-397B-A17B-FP8","choices":[...]}   ← 模型真的回复了
```

---

## 2. 架构与流程

### 2.1 PoC 程序启动流程

```
app_main()
  ├── nvs_flash_init()              // NVS 初始化(microlink 缓存密钥)
  ├── esp_netif_init()              // 网络接口
  ├── wifi_start()                  // 连接 WiFi,等待 IP
  ├── SNTP 时间同步                  // ★ 关键!WireGuard 需要真实时间
  │     └── pool.ntp.org, 等待最多 15s
  ├── microlink_init(&config)       // 初始化 Tailscale 客户端
  │     ├── auth_key                // Tailscale 认证密钥
  │     ├── device_name             // 设备名 "rig-hover-poc"
  │     ├── enable_derp/stun/disco  // 启用 DERP/STUN/DISCO
  │     └── priority_peer_ip        // 优先保留 Qwen 节点的 WG 插槽
  ├── microlink_start()             // 启动 4 个 FreeRTOS task
  │     ├── coord_task (12KB)       // Tailscale 控制面 (Noise_IK + H2)
  │     ├── derp_tx (14KB)          // DERP 中继
  │     ├── net_io (8KB)            // UDP 路由
  │     └── wg_mgr (8KB)            // WireGuard + DISCO
  ├── 等待 tailnet 连接 (最多 120s)
  ├── verify: mlserver TCP (:22)    // 验证 WG 数据面
  └── verify: Qwen API (:39717)     // 验证 LLM 调用
```

### 2.2 Tailscale 连接阶段

```
阶段 1: 控制面握手
  ESP32 ←Noise_IK→ controlplane.tailscale.com
  → 获取 tailnet 节点列表 (MapResponse)
  → 分配 VPN IP (100.124.124.111)
  → 获取 DERP Map (自建 DERP: region 904, bj02)

阶段 2: DERP 连接
  ESP32 ←TLS→ <self-hosted-derp>:443
  → 注册到 DERP 中继,准备转发 WG 包

阶段 3: DISCO 发现 + WG 握手
  对每个 peer:
    DISCO PING → 发现直连路径 (公网 IP:端口)
    WG INIT → WG RESPONSE → 握手完成 → 加密隧道建立

阶段 4: 数据传输
  TCP over WireGuard → 标准 socket API → 应用层 HTTP
```

### 2.3 凭证管理

凭证通过环境变量编译时注入,不写死在代码里:

```bash
# ~/.tailscale-env (不进 git)
export TS_AUTH_KEY="tskey-auth-..."
export TS_WIFI_SSID="<wifi-ssid>"
export TS_WIFI_PASS="..."
export TS_QWEN_HOST="100.94.32.60"
export TS_QWEN_PORT="39717"
export TS_QWEN_MODEL="Qwen/Qwen3.5-397B-A17B-FP8"
```

CMakeLists.txt 中:
```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TS_AUTH_KEY="$ENV{TS_AUTH_KEY}"
    ...
)
```

---

## 3. 修复的 Bug (正确经验)

### Bug 1: TAI64N 时间戳用了 uptime 而非真实时间

**文件**: `wireguard_lwip/src/wireguard-platform-esp32.c`

**问题**: `esp_timer_get_time()` 返回开机时长(微秒),不是 Unix 时间戳。WireGuard 的 TAI64N 时间戳 = `2^62 + Unix秒 + 10`。用 uptime 算出的时间戳比真实时间早 ~55 年,所有 peer 直接拒绝握手。

**修复**:
```c
// 错误:
uint64_t now_us = esp_timer_get_time();  // uptime,不是真实时间!

// 正确:
struct timeval tv;
gettimeofday(&tv, NULL);  // 真实 Unix 时间(需 SNTP 同步)
uint64_t seconds = (uint64_t)tv.tv_sec;
```

**前提**: 必须先 SNTP 同步时间,否则 `gettimeofday()` 返回 epoch 0 (1970)。

### Bug 2: DERP 服务器硬编码为官方地址

**文件**: `microlink_internal.h`

**问题**: microlink 硬编码 `derp9e.tailscale.com`(Tailscale 官方 DERP,达拉斯)。如果 tailnet 使用自建 DERP,ESP32 和其他节点不在同一个 DERP 上,WG 包无法转发。

**修复**:
```c
// 错误:
#define ML_DERP_REGION  9
#define ML_DERP_HOST    "derp9e.tailscale.com"
#define ML_STUN_PRIMARY_HOST "derp9.tailscale.com"

// 正确(改为自建 DERP):
#define ML_DERP_REGION  904
#define ML_DERP_HOST    "<self-hosted-derp>"
#define ML_STUN_PRIMARY_HOST "<self-hosted-derp>"
```

**诊断方法**: SSH 到 tailnet 内任一节点,跑 `tailscale debug derp-map` 查看 DERP 地址。

### Bug 3: 没有 SNTP 时间同步

**问题**: PoC 和 microlink 都没有在启动时同步时间。ESP32 断电后 RTC 归零,`gettimeofday()` 返回 1970 年。

**修复**: WiFi 连上后、microlink 启动前,加 SNTP:
```c
esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
esp_sntp_setservername(0, "pool.ntp.org");
esp_sntp_init();
// 等待最多 15 秒
while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && wait < 15) {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

---

## 4. 踩坑记录 (教训)

### 教训 1: ESP32 没有"模拟器"能验证网络链路

CrossMux 等项目的"模拟器"是把应用层代码用 host 编译器重编译,硬件相关代码全部 stub。WiFi/UDP/WireGuard 全是假的。**验证 Tailscale/WireGuard 链路只能上真机。**

### 教训 2: PoC 代码和 submodule 是两份独立副本

`/tmp/poc_ext/third_party/microlink/` 和 `rig_omni/poc/third_party/microlink/` 是**不同的文件**。改了 rig_omni 里的 microlink,PoC 编译不会生效。每次修复要确认改的是 PoC 实际编译的那份。

### 教训 3: DTR/RTS 软件复位对 CH340 不生效

CH340 USB 串口芯片不支持通过 DTR/RTS 信号触发 ESP32 重启。`esptool --before default_reset` 也无效。**必须物理断电**(拔 USB 线等 3 秒再插)。

### 教训 4: PoC 超时设太短 + 提前 return

控制面重连可能需要 2-3 次尝试(每次间隔指数退避),总耗时可能超过 60 秒。PoC 设了 60 秒超时后直接 `return`,后面的验证代码不执行。改为 120 秒 + 不提前退出。

### 教训 5: WG 握手是异步的,不能假设"控制面通了 = 数据面通了"

`microlink_is_connected()` 返回 true 只表示 Tailscale 控制面连上了(拿到了 VPN IP),不代表 WireGuard 隧道已建立。WG 握手是异步的,需要额外等待。`ml_tcp` 内部有 20 秒的 WG 隧道等待,但如果 peer 的握手一直失败,20 秒后强行 connect 也会失败。

### 教训 6: 对称 NAT 下直连不可行,必须走 DERP

ESP32 的 WiFi 路由器如果做了对称 NAT(每次 STUN 探测公网端口不同),DISCO 直连会失败。此时 WG 握手只能通过 DERP 中继。如果 DERP 配置错误(连了官方 DERP 而非自建),WG 包转发不到对端。

### 教训 7: DERP PeerGone 是关键诊断信号

`DERP PeerGone: <key>` 表示 DERP 服务器上没有该 peer 的连接。如果频繁出现,说明:
- 对端没连上这个 DERP(可能 DERP 地址不对)
- 或者 DERP 服务器本身有问题

### 教训 8: TAI64N 日志是最好的诊断入口

`[TAI64N] uptime=37s` vs `[TAI64N] epoch=1784642922s` 一眼就能看出时间戳是用 uptime 还是真实时间。如果 WG 握手一直失败,先查这个。

---

## 5. 环境搭建步骤

### 5.1 前置条件

- ESP-IDF v5.5.2+ (已验证)
- ESP32-S3 开发板 + USB 线
- Tailscale 账号 + auth key (reusable + ephemeral)
- 自建 DERP 服务器地址(如果用官方 Tailscale DERP 则不需要改)

### 5.2 构建 PoC

```bash
cd rig_omni/poc

# 配置凭证
cp secrets.env.example ~/.tailscale-env
# 编辑 ~/.tailscale-env 填入真实值

# 加载环境
source ~/.tailscale-env

# 编译
idf.py set-target esp32s3
idf.py build

# 烧录(需物理断电重启进入下载模式)
idf.py -p /dev/cu.wchusbserial* flash monitor
```

### 5.3 验证清单

- [ ] 串口日志出现 `✓ 已加入 tailnet, VPN IP = 100.x.x.x`
- [ ] 串口日志出现 `HANDSHAKE COMPLETE` (至少一个 peer)
- [ ] 串口日志出现 `✓ Qwen API 响应 — 链路完全打通!`
- [ ] 电脑端 `tailscale ping 100.124.124.111` 能通
- [ ] Tailscale 管理后台能看到 rig-hover-poc 在线

---

## 6. 后续集成方向

将 Tailscale 集成进 rig_omni 主工程,让 Hover 的 AI 对话走 Tailscale 隧道:

1. 新增板型 `RIG-Hover-TS`(Kconfig + CMakeLists + boards/hover_ts/)
2. microlink 作为可选 component(CONFIG_ENABLE_TAILSCALE)
3. WebSocket 的 AI 服务地址改为 tailnet 内网 IP
4. 3 个 microlink 修复(TAI64N/DERP/SNTP)提 PR 到上游