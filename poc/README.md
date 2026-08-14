# Tailscale PoC

验证 ESP32-S3 通过 [microlink](https://github.com/CamM2325/microlink)(纯 C Tailscale 客户端)加入 tailnet,并访问内网部署的 AI 服务。

## PoC 验证什么

```
ESP32-S3 ──WiFi──┐
                 ├──WireGuard 隧道──> 100.94.32.60:39717 (Qwen API)
   microlink ────┘    (Tailscale)
```

三阶段验证(全过 = 链路成立):
1. ESP32 加入 tailnet,拿到 `100.x.x.x` IP
2. 通过 WireGuard 隧道 TCP 连到内网 Qwen 节点
3. 发送 OpenAI 兼容的 `/v1/chat/completions` 请求,收到 Qwen 回复

## 前置条件

### 1. Tailscale auth key
去 [login.tailscale.com/admin/settings/keys](https://login.tailscale.com/admin/settings/keys) 生成一把 **reusable + ephemeral** 的 key。

### 2. 凭证文件
```bash
cp poc/secrets.env.example ~/.tailscale-env
# 编辑 ~/.tailscale-env,填入真实的 auth key / WiFi / AI 服务地址
chmod 600 ~/.tailscale-env
```

### 3. 内网已有 tailnet 节点
AI 服务所在的内网,至少一台机器已在 tailnet 里(能从外部 `tailscale ping` 到它)。

## 构建

```bash
# 进入工程目录(direnv 自动激活 ESP-IDF + 加载 ~/.tailscale-env)
cd rig_omni

# 编译 PoC(独立固件,不影响主工程)
idf.py -C poc set-target esp32s3
idf.py -C poc build

# 烧录
idf.py -C poc -p /dev/cu.usbmodem* flash monitor
```

产物在 `poc/build/rig-poc-tailscale.bin`,与主工程的 `build/` 完全隔离。

## 验证成功

ESP32 串口日志依次出现:
```
[poc] WiFi 已连接
[poc] [microlink] state=4  (ML_STATE_CONNECTED)
[poc] ════════════════════════════════════════
[poc]   ✓ 已加入 tailnet,VPN IP = 100.x.x.x
[poc] ════════════════════════════════════════
[poc] ✓ TCP 连接已建立(经 WireGuard 隧道)
[poc] ════════════════════════════════════════
[poc]   ✓ Qwen API 响应(NNN 字节)— 链路完全打通!
[poc] ════════════════════════════════════════
```

PC 侧另开终端:
```bash
tailscale ping rig-hover-poc
# 应看到: pong from rig-hover-poc (100.x.x.x) via DERP(...) in XXXms
```

## 目录结构

```
poc/
├── CMakeLists.txt              # 独立顶层 project
├── extra.defaults              # microlink 追加配置(ChaCha20/HKDF/buffer)
├── partitions.csv              # 简单单 app 分区表
├── sdkconfig.defaults -> ..    # 软链复用主工程配置
├── main/
│   ├── CMakeLists.txt
│   └── poc_main.cc             # WiFi→tailnet→Qwen 三阶段验证
├── third_party/
│   └── microlink/              # git submodule → antmanler/microlink @ duoduo-edge (v2.1.0 + field fixes)
└── secrets.env.example         # 凭证模板(真实文件在 ~/.tailscale-env)
```

## 已知限制

- **DERP region 硬编码 9 (Dallas)**:国内访问延迟可能较高,但验证连通性够用
- **无 ICMP ping**:microlink 不提供原始 ICMP,只能用 DISCO ping(PC 侧)/ TCP 连接验证
- **内存占用**:microlink ~1MB PSRAM 峰值 + 116KB SRAM。PoC 已把 H2/JSON buffer 降到 128KB,与主工程音频栈共存时仍需监控 OOM
- **stability**: upstream #17/#20 (wireguardif pbuf double-free) is still unmerged upstream; our fork branch carries the fix

## 下一步(PoC 成功后)

把 `websocket_protocol.cc` 里连 AI 服务的 socket 调用改成 microlink TCP API,即可让主工程的对话流量走 Tailscale。详见主 README 的"AI 大脑 / 服务端实现"章节。
