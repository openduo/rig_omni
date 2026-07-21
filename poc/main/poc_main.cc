/**
 * rig_omni Tailscale PoC
 *
 * 验证三件事:
 *   1. ESP32-S3 通过 microlink 加入 tailnet,拿到 100.x.x.x IP
 *   2. 通过 Tailscale 隧道 TCP 连接到内网 Qwen API(100.94.32.60:39717)
 *   3. 发送 OpenAI 兼容的 chat/completions 请求,收到 Qwen 回复
 *
 * 三步全过 = "ESP32 能通过 Tailscale 访问内网 AI 服务"链路成立。
 *
 * 凭证从环境变量编译时注入(CMakeLists add_compile_definitions),不写死,不进 git。
 * 环境变量来源:~/.tailscale-env(由 .envrc source 到构建环境)。
 *
 * 构建: idf.py -C poc set-target esp32s3 && idf.py -C poc build
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "microlink.h"

static const char *TAG = "poc";

#define WIFI_CONNECTED_BIT BIT0

/* Qwen API 请求体(OpenAI 兼容格式)
 * 环境变量 TS_QWEN_MODEL 编译时注入 */
static const char *CHAT_REQUEST_TEMPLATE =
    "POST /v1/chat/completions HTTP/1.1\r\n"
    "Host: " TS_QWEN_HOST "\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "Content-Length: %d\r\n"
    "\r\n"
    "%s";

static const char *CHAT_BODY_TEMPLATE =
    "{\"model\":\"" TS_QWEN_MODEL "\",\"messages\":["
    "{\"role\":\"user\",\"content\":\"你是谁?用一句话回答。\"}"
    "],\"max_tokens\":64}";

static EventGroupHandle_t wifi_events;
static microlink_t *ml = nullptr;

/* ===== WiFi ===== */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 断开,重连...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start() {
    wifi_events = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any_id, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wc = {};
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)wc.sta.ssid, TS_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, TS_WIFI_PASS, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 关省电,降 WireGuard 抖动(microlink README 要求) */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "等待 WiFi 连接: %s ...", TS_WIFI_SSID);
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi 已连接");
}

/* ===== microlink 状态回调 ===== */
static void on_ml_state(microlink_t *m, microlink_state_t state, void *user) {
    ESP_LOGI(TAG, "[microlink] state=%d", state);
    if (state == ML_STATE_CONNECTED) {
        char ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(m), ip);
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "════════════════════════════════════════");
        ESP_LOGI(TAG, "  ✓ 已加入 tailnet,VPN IP = %s", ip);
        ESP_LOGI(TAG, "  ✓ PC 侧可执行: tailscale ping rig-hover-poc");
        ESP_LOGI(TAG, "════════════════════════════════════════");
        ESP_LOGI(TAG, "");
    } else if (state == ML_STATE_ERROR) {
        ESP_LOGE(TAG, "[microlink] 错误状态 — 检查 auth key / 网络");
    }
}

/* ===== Qwen API 验证 ===== */
static void verify_qwen() {
    uint32_t qwen_ip = microlink_parse_ip(TS_QWEN_HOST);
    if (qwen_ip == 0) {
        ESP_LOGE(TAG, "解析 Qwen IP 失败: %s", TS_QWEN_HOST);
        return;
    }
    ESP_LOGI(TAG, "验证 Qwen API: %s:%d ...", TS_QWEN_HOST, TS_QWEN_PORT);

    /* TCP 连接(自动触发 WG 握手) */
    microlink_tcp_socket_t *sock = microlink_tcp_connect(
        ml, qwen_ip, TS_QWEN_PORT, 20000);
    if (!sock) {
        ESP_LOGE(TAG, "✗ TCP 连接失败 — WG 隧道可能未就绪");
        return;
    }
    ESP_LOGI(TAG, "✓ TCP 连接已建立(经 WireGuard 隧道)");

    /* 构造 HTTP POST */
    char body[256];
    snprintf(body, sizeof(body), CHAT_BODY_TEMPLATE);
    char request[768];
    int body_len = strlen(body);
    int req_len = snprintf(request, sizeof(request),
                           CHAT_REQUEST_TEMPLATE, body_len, body);

    if (microlink_tcp_send(sock, request, req_len) != ESP_OK) {
        ESP_LOGE(TAG, "✗ 发送失败");
        microlink_tcp_close(sock);
        return;
    }
    ESP_LOGI(TAG, "✓ 请求已发送(%d 字节),等待 Qwen 回复...", req_len);

    /* 读响应(可能分多包,循环读到连接关闭或超时) */
    char buf[2048];
    int total = 0;
    int n;
    while (total < (int)sizeof(buf) - 1 &&
           (n = microlink_tcp_recv(sock, buf + total,
                                    sizeof(buf) - 1 - total, 8000)) > 0) {
        total += n;
    }
    buf[total] = '\0';

    if (total == 0) {
        ESP_LOGE(TAG, "✗ 未收到响应");
    } else {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "════════════════════════════════════════");
        ESP_LOGI(TAG, "  ✓ Qwen API 响应(%d 字节)— 链路完全打通!", total);
        ESP_LOGI(TAG, "════════════════════════════════════════");
        /* 打印前 500 字节(HTTP 头 + 部分 body) */
        int show = total > 500 ? 500 : total;
        ESP_LOGI(TAG, "%.*s", show, buf);
        if (total > 500) ESP_LOGI(TAG, "... (%d 字节未显示)", total - 500);
    }
    microlink_tcp_close(sock);
}

/* ===== main ===== */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   rig_omni Tailscale PoC                 ║");
    ESP_LOGI(TAG, "║   目标: ESP32 → tailnet → 内网 Qwen API   ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    /* NVS(microlink 缓存密钥用) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 网络 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Step 1: WiFi */
    wifi_start();

    /* Step 2: microlink 加入 tailnet */
    ESP_LOGI(TAG, "启动 microlink,加入 tailnet ...");
    microlink_config_t mc = {};
    mc.auth_key = TS_AUTH_KEY;
    mc.device_name = "rig-hover-poc";
    mc.enable_derp = true;
    mc.enable_stun = true;
    mc.enable_disco = true;
    mc.max_peers = 4;
    mc.wifi_tx_power_dbm = 13;          /* 降热 */
    mc.priority_peer_ip = microlink_parse_ip(TS_QWEN_HOST); /* 保证 Qwen 有 WG 插槽 */

    ml = microlink_init(&mc);
    if (!ml) {
        ESP_LOGE(TAG, "microlink_init 失败");
        return;
    }
    microlink_set_state_callback(ml, on_ml_state, nullptr);
    ESP_ERROR_CHECK(microlink_start(ml));

    /* 等连上(最多等 60s) */
    int waited = 0;
    while (!microlink_is_connected(ml) && waited < 60) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
        if (waited % 10 == 0) {
            ESP_LOGI(TAG, "等待 tailnet 连接 ... %ds", waited);
        }
    }
    if (!microlink_is_connected(ml)) {
        ESP_LOGE(TAG, "✗ 60s 内未加入 tailnet — 检查 auth key / WiFi / DERP");
        return;
    }

    /* 给 WireGuard 握手一点时间稳定 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Step 3: 验证 Qwen API */
    verify_qwen();

    /* 完成,保持运行便于 PC 侧 tailscale ping */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "PoC 验证完成。保持运行,PC 侧可 tailscale ping rig-hover-poc");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (microlink_is_connected(ml)) {
            char ip[16];
            microlink_ip_to_str(microlink_get_vpn_ip(ml), ip);
            ESP_LOGI(TAG, "[heartbeat] VPN IP=%s peers=%d",
                     ip, microlink_get_peer_count(ml));
        }
    }
}
