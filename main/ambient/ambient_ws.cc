/* [ambient-mod] Always-on ambient /live client. See ambient_ws.h. */
#include "ambient_ws.h"

#include <freertos/FreeRTOS.h>
#include <freertos/message_buffer.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

#include <cJSON.h>

#include "ambient_face_state.h"
#include "ambient_rebind_gate.h"
#include "application.h"
#include "display.h"  /* board.h only forward-declares Display */
#include "assets/lang_config.h"
#include "board.h"
#include "microlink.h"
#include "web_socket.h"

#include "esp_audio_enc.h"
#include "esp_opus_enc.h"

#define TAG "AmbientWs"

namespace ambient {
namespace {

/* 1 s of 16 kHz int16. Must be PSRAM — xStreamBufferCreate follows
 * pvPortMalloc and can land in the ~45 KB internal pool. */
constexpr size_t kPcmBufferBytes = 32000;

/* 60 ms / packet. 20 ms is 50 WG packets/s and saturates software
 * WireGuard on this chip; the decoder does not care about duration. */
constexpr int kUplinkFrameMs = 60;

constexpr size_t kTxBufferBytes = 16 * 1024;

/* Same 24 KB PSRAM stack as the vendor opus codec task. Must not touch NVS. */
constexpr size_t kEncStackBytes = 2048 * 12;

/* RFC 6716: one frame ≤ 1275 B, one packet ≤ 120 ms ⇒ up to 6 frames.
 * 1275 is not the packet cap; TTS multi-frame packets exceed it. */
constexpr size_t kMaxOpusPacketBytes = 7656;

/* ~30 s of 16 kHz opus at ~5.7 KB/s, plus burst headroom. Non-zero
 * g_opus_dropped means raise this, not chase another cause. */
constexpr size_t kOpusBufferBytes = 192 * 1024;

constexpr TickType_t kReconnectDelay = pdMS_TO_TICKS(2000);
constexpr TickType_t kSntpTimeout = pdMS_TO_TICKS(15000);
/* Control-plane reconnect can take two backoff rounds (>60 s). */
constexpr int kTailnetWaitSeconds = 90;

StreamBufferHandle_t g_pcm_buffer = nullptr;
TaskHandle_t g_task = nullptr;

void* g_opus_enc = nullptr;
int g_enc_frame_bytes = 0;
int g_enc_outbuf_bytes = 0;
MessageBufferHandle_t g_tx_buffer = nullptr;
StaticMessageBuffer_t g_tx_buffer_ctrl;
TaskHandle_t g_enc_task = nullptr;
StaticTask_t g_enc_tcb;
unsigned g_tx_dropped = 0;

/* `played.ms` is a decode-admission watermark (FeedTask, at
 * PushPacketToDecodeQueue), not acoustic progress. The decode queue
 * is 40 packets deep, so this can lead the speaker by ~2400 ms. G3
 * (played >= audio_ms) has no timeout — without these frames the
 * channel stays in SPEAKING. Do not use this as "mouth is sounding". */
portMUX_TYPE g_played_mux = portMUX_INITIALIZER_UNLOCKED;
char g_speech_id[32] = {0};
uint32_t g_played_ms = 0;

MessageBufferHandle_t g_opus_buffer = nullptr;
StaticMessageBuffer_t g_opus_buffer_ctrl;
TaskHandle_t g_feed_task = nullptr;
/* PSRAM stack: this task must not touch NVS / flash. TCB stays internal
 * (xPortCheckValidTCBMem). UplinkTask touches NVS — its stack is internal. */
StaticTask_t g_feed_tcb;
constexpr size_t kFeedStackBytes = 4096;
/* From server audio_params. Do not guess a rate. */
int g_opus_rate = 0;
int g_opus_frame_ms = 0;
unsigned g_opus_dropped = 0;
StaticStreamBuffer_t g_pcm_buffer_ctrl;
microlink_t* g_ml = nullptr;
WifiRebindGate g_wifi_rebind;

static void OnWifiDisconnected(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
    g_wifi_rebind.OnDisconnected();
    ESP_LOGW(TAG, "wifi lost — will rebind microlink after the next address");
}

static void OnStaGotIp(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
    if (g_wifi_rebind.OnGotIp()) {
        ESP_LOGW(TAG, "wifi got an address after a loss — rebind armed");
    }
}

void MaybeRebindMicrolink() {
    if (!g_wifi_rebind.Consume() || g_ml == nullptr) {
        return;
    }
    /* Do not call this from the Wi-Fi / IP event task: microlink_rebind
     * sleeps (~300 ms) while it closes sockets. UplinkTask is the owner. */
    ESP_LOGW(TAG, "rebinding microlink (keep the session, do not re-register)");
    const esp_err_t err = microlink_rebind(g_ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microlink_rebind failed: %s", esp_err_to_name(err));
    }
    /* The station came back with IDF defaults; modem sleep would return. */
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_ps(NONE) after rebind failed — downlink will stutter");
    }
}

/* Face facts go through Application::Schedule so only the main task
 * touches the display. Remote facts carry the connection epoch. */
ambient::face::State g_face;
const char* g_face_rendered = nullptr;
std::atomic<uint64_t> g_face_epoch{0};

uint64_t FaceEpoch() { return g_face_epoch.load(std::memory_order_acquire); }

void RenderFaceIfChanged() {
    const char* expression = ambient::face::Expression(g_face);
    if (g_face_rendered != nullptr && strcmp(g_face_rendered, expression) == 0) return;
    g_face_rendered = expression;
    /* apply without a matching expression= means SetEmotion blocked. */
    ESP_LOGW(TAG, "[face] apply %s", expression);
    Board::GetInstance().GetDisplay()->SetEmotion(expression);
    ESP_LOGW(TAG, "[face] expression=%s", expression);
}

void ScheduleFaceConnected(uint64_t epoch) {
    Application::GetInstance().Schedule([epoch]() {
        const bool began = ambient::face::BeginConnection(g_face, epoch);
        const bool online = ambient::face::SetConnected(g_face, epoch);
        if (began || online || g_face_rendered == nullptr) RenderFaceIfChanged();
    });
}

void ScheduleFaceDisconnected(uint64_t epoch) {
    Application::GetInstance().Schedule([epoch]() {
        if (ambient::face::SetDisconnected(g_face, epoch) || g_face_rendered == nullptr) {
            RenderFaceIfChanged();
        }
    });
}

void ScheduleFaceSpeaker(bool active) {
    Application::GetInstance().Schedule([active]() {
        if (ambient::face::SetSpeakerActive(g_face, active)) RenderFaceIfChanged();
    });
}

void ScheduleFaceHearing(bool hearing) {
    Application::GetInstance().Schedule([hearing]() {
        if (ambient::face::SetHearing(g_face, hearing)) RenderFaceIfChanged();
    });
}

/* Local energy: listen above 500 ac_rms, rest after ~0.5 s below 250.
 * Measured quiet-room vs speech bands; the hold avoids flicker. */
void DriveHearingFace(const std::vector<int16_t>& pcm) {
    static bool hearing = false;
    static unsigned quiet_batches = 0;
    if (pcm.empty()) return;

    int64_t sum = 0;
    for (int16_t v : pcm) sum += v;
    const double mean = (double)sum / (double)pcm.size();
    double var = 0;
    for (int16_t v : pcm) {
        const double d = v - mean;
        var += d * d;
    }
    const double ac_rms = sqrt(var / (double)pcm.size());

    if (ac_rms > 500.0) {
        quiet_batches = 0;
        if (!hearing) {
            hearing = true;
            ScheduleFaceHearing(true);
        }
        return;
    }
    if (hearing && ac_rms < 250.0 && ++quiet_batches >= 8) {
        hearing = false;
        quiet_batches = 0;
        ScheduleFaceHearing(false);
    }
}

void ScheduleFaceMuted(uint64_t epoch, bool muted) {
    Application::GetInstance().Schedule([epoch, muted]() {
        if (ambient::face::SetMuted(g_face, epoch, muted)) RenderFaceIfChanged();
    });
}

void ScheduleFaceSenses(uint64_t epoch, bool enabled) {
    Application::GetInstance().Schedule([epoch, enabled]() {
        if (ambient::face::SetSensesEnabled(g_face, epoch, enabled)) RenderFaceIfChanged();
    });
}

void ScheduleFacePipeline(uint64_t epoch, ambient::face::Pipeline pipeline) {
    Application::GetInstance().Schedule([epoch, pipeline]() {
        if (ambient::face::SetPipeline(g_face, epoch, pipeline)) RenderFaceIfChanged();
    });
}

void ScheduleFaceMetaState(uint64_t epoch, ambient::face::MetaState meta) {
    Application::GetInstance().Schedule([epoch, meta]() {
        if (ambient::face::ApplyMetaState(g_face, epoch, meta)) RenderFaceIfChanged();
    });
}

void ScheduleFaceTranscript(uint64_t epoch) {
    Application::GetInstance().Schedule([epoch]() {
        if (ambient::face::ApplyTranscript(g_face, epoch)) RenderFaceIfChanged();
    });
}

/* mute_started.until is Unix-ms. Server mute_ended is lazy; this timer
 * is the local deadline. Epoch is atomic: the callback is on esp_timer. */
esp_timer_handle_t g_mute_timer = nullptr;
std::atomic<uint64_t> g_mute_timer_epoch{0};

void EnsureMuteTimer() {
    if (g_mute_timer != nullptr) return;
    esp_timer_create_args_t args = {};
    args.callback = [](void*) {
        ScheduleFaceMuted(g_mute_timer_epoch.load(std::memory_order_acquire), false);
    };
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "face_mute";
    args.skip_unhandled_events = true;
    if (esp_timer_create(&args, &g_mute_timer) != ESP_OK) g_mute_timer = nullptr;
}

void ArmMuteTimer(uint64_t epoch, double until_unix_ms) {
    EnsureMuteTimer();
    if (g_mute_timer == nullptr) return;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const double now_ms = tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
    const double delay_ms = until_unix_ms > now_ms ? until_unix_ms - now_ms : 0.0;
    g_mute_timer_epoch.store(epoch, std::memory_order_release);
    esp_timer_stop(g_mute_timer); /* not-armed returns an error; only the restart matters */
    esp_timer_start_once(g_mute_timer, (uint64_t)(delay_ms * 1000.0));
}

void CancelMuteTimer() {
    if (g_mute_timer != nullptr) esp_timer_stop(g_mute_timer);
}

/* Wall clock before microlink. WG TAI64N is wall time; a zero RTC
 * dates handshakes ~55 years early (control plane up, data plane dead). */
bool EnsureWallClock() {
    time_t now = 0;
    struct tm ti = {};
    time(&now);
    localtime_r(&now, &ti);
    /* Vendor OTA server_time already sets the clock on the normal path. */
    if (ti.tm_year > (2020 - 1900)) {
        ESP_LOGW(TAG, "wall clock already set (epoch=%ld), skip SNTP", (long)now);
        return true;
    }

    ESP_LOGW(TAG, "wall clock is %ld (RTC lost power) — syncing SNTP", (long)now);
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed — WG handshakes will be rejected");
        return false;
    }
    esp_err_t err = esp_netif_sntp_sync_wait(kSntpTimeout);
    time(&now);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync timed out (epoch=%ld) — WG handshakes will be rejected", (long)now);
        return false;
    }
    ESP_LOGW(TAG, "SNTP synced, epoch=%ld", (long)now);
    return true;
}

/* Join the tailnet once. microlink_is_connected is control-plane only;
 * the WS loop retries until the data plane is up. */
bool EnsureTailnet() {
    if (g_ml != nullptr) {
        return true;
    }
    /* Reset identity only when the compiled auth key changes. A
     * "run once" flag in a boot loop minted a new tailnet node every
     * 17 s. Write the fingerprint before reset: a crash after reset
     * and before the write would reset on every boot. Missing marker
     * is first install — record, do not erase. */
    {
        uint32_t want = 2166136261u;
        for (const char* c = TS_AUTH_KEY; *c; ++c) {
            want = (want ^ (uint8_t)*c) * 16777619u;
        }
        nvs_handle_t nvs;
        if (nvs_open("ambient", NVS_READWRITE, &nvs) == ESP_OK) {
            uint32_t seen = 0;
            esp_err_t got = nvs_get_u32(nvs, "ts_key_fp", &seen);
            if (got != ESP_OK) {
                ESP_LOGW(TAG, "no key marker yet (%08lx): recording, NOT resetting identity",
                         (unsigned long)want);
                nvs_set_u32(nvs, "ts_key_fp", want);
                nvs_commit(nvs);
                nvs_close(nvs);
            } else if (seen != want) {
                ESP_LOGW(TAG, "auth key changed (%08lx -> %08lx): resetting tailnet identity ONCE",
                         (unsigned long)seen, (unsigned long)want);
                nvs_set_u32(nvs, "ts_key_fp", want);
                nvs_commit(nvs);
                nvs_close(nvs);
                microlink_factory_reset();
            } else {
                ESP_LOGW(TAG, "auth key unchanged (%08lx): keeping tailnet identity", (unsigned long)want);
                nvs_close(nvs);
            }
        } else {
            ESP_LOGE(TAG, "nvs_open failed — skipping identity reset (safe direction)");
        }
    }
    microlink_config_t mc = {};
    mc.auth_key = TS_AUTH_KEY;
    mc.device_name = "rig-hover";
    mc.enable_derp = true;
    mc.enable_stun = true;
    mc.enable_disco = true;
    /* Default 4 peers fills before the ambient host on this tailnet
     * (EHOSTUNREACH to AMBIENT_SERVER_TAILNET_IP). */
    mc.max_peers = 8;
    mc.wifi_tx_power_dbm = 13;
    mc.priority_peer_ip = microlink_parse_ip(AMBIENT_SERVER_TAILNET_IP);

    g_ml = microlink_init(&mc);
    if (g_ml == nullptr) {
        ESP_LOGE(TAG, "microlink_init failed");
        return false;
    }
    if (microlink_start(g_ml) != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed");
        microlink_destroy(g_ml);
        g_ml = nullptr;
        return false;
    }
    /* Register after start so the first GOT_IP (already delivered) does not
     * arm a rebind. ml_net_switch is not used: it owns Wi-Fi + cellular, and
     * this board already has WifiBoard. */
    if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                   OnWifiDisconnected, nullptr) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   OnStaGotIp, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "failed to watch wifi/ip events — microlink will not rebind on reconnect");
    }
    for (int waited = 0; waited < kTailnetWaitSeconds; ++waited) {
        if (microlink_is_connected(g_ml)) {
            char ip[16] = {};
            microlink_ip_to_str(microlink_get_vpn_ip(g_ml), ip);
            ESP_LOGW(TAG, "tailnet up, vpn ip=%s, free internal=%u B", ip,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "tailnet not up in %ds — check auth key / DERP", kTailnetWaitSeconds);
    return false;
}

/* IDF uxTaskGetStackHighWaterMark is bytes, not words. */
void LogStackHighWaterMarks() {
    static const char* kTasks[] = {"ml_net_io", "ml_derp_tx", "ml_coord", "ml_wg_mgr",
                                   "tcp_receive"};
    static const int kSizes[] = {8 * 1024, 14 * 1024, 12 * 1024, 8 * 1024, 4 * 1024};
    for (size_t i = 0; i < sizeof(kTasks) / sizeof(kTasks[0]); ++i) {
        TaskHandle_t h = xTaskGetHandle(kTasks[i]);
        if (h == nullptr) {
            ESP_LOGW(TAG, "stack hwm: %s not found", kTasks[i]);
            continue;
        }
        unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(h);
        ESP_LOGW(TAG, "stack hwm: %-10s min free %5u B / %5d B (%.0f%% used)", kTasks[i],
                 free_b, kSizes[i], 100.0 * (kSizes[i] - (int)free_b) / kSizes[i]);
    }
    ESP_LOGW(TAG, "heap after tailnet: internal=%u B psram=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* One binary WS frame = one raw Opus packet. Queue only — this is the
 * socket task; decode happens on FeedTask. */
void HandleDownlinkAudio(const char* data, size_t len) {
    if (g_opus_buffer == nullptr) return;
    if (len == 0 || len > kMaxOpusPacketBytes) {
        ESP_LOGW(TAG, "[dl] bad opus packet: %u B (max %u)", (unsigned)len,
                 (unsigned)kMaxOpusPacketBytes);
        return;
    }
    if (xMessageBufferSend(g_opus_buffer, data, len, 0) == 0) {
        if ((++g_opus_dropped % 50) == 1) {
            ESP_LOGW(TAG, "[dl] opus buffer full, dropped %u packets", g_opus_dropped);
        }
    }
}

void HandleDownlink(const char* data, size_t len) {
    ESP_LOGI(TAG, "downlink %u B: %.*s", (unsigned)len,
             (int)(len < 96 ? len : 96), data);

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) return;

    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "audio_params") == 0) {
        const cJSON* rate = cJSON_GetObjectItem(root, "rate");
        const cJSON* fms = cJSON_GetObjectItem(root, "frame_ms");
        if (cJSON_IsNumber(rate) && cJSON_IsNumber(fms)) {
            g_opus_rate = rate->valueint;
            /* Latch the max: a new audio_params can arrive while the
             * previous utterance is still queued. Undersizing
             * decoder_frame_size_ is an overflow. */
            if (fms->valueint > g_opus_frame_ms) g_opus_frame_ms = fms->valueint;
            ESP_LOGW(TAG, "[dl] audio_params: rate=%d frame_ms=%d (latched max, told %d)",
                     g_opus_rate, g_opus_frame_ms, fms->valueint);
        } else {
            ESP_LOGW(TAG, "[dl] audio_params missing rate/frame_ms — ignored");
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "speech") == 0) {
        const cJSON* sid = cJSON_GetObjectItem(root, "speech_id");
        if (cJSON_IsString(sid)) {
            taskENTER_CRITICAL(&g_played_mux);
            strlcpy(g_speech_id, sid->valuestring, sizeof(g_speech_id));
            g_played_ms = 0;
            taskEXIT_CRITICAL(&g_played_mux);
            ESP_LOGW(TAG, "[dl] speech %s — watermark reset", sid->valuestring);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "stop_audio") == 0) {
        /* Drain the feed buffer before ResetDecoder, or FeedTask
         * refills the queue. Clear speech_id after, or leftover
         * packets raise the next watermark. */
        ESP_LOGW(TAG, "stop_audio — dropping queued playback");
        if (g_opus_buffer != nullptr) xMessageBufferReset(g_opus_buffer);
        Application::GetInstance().GetAudioService().ResetDecoder();
        taskENTER_CRITICAL(&g_played_mux);
        g_speech_id[0] = 0;
        taskEXIT_CRITICAL(&g_played_mux);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "meta") == 0) {
        const cJSON* state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state)) {
            using ambient::face::MetaState;
            if (strcmp(state->valuestring, "thinking") == 0) {
                ScheduleFaceMetaState(FaceEpoch(), MetaState::kThinking);
            } else if (strcmp(state->valuestring, "speaking") == 0) {
                ScheduleFaceMetaState(FaceEpoch(), MetaState::kSpeaking);
            } else if (strcmp(state->valuestring, "listening") == 0) {
                ScheduleFaceMetaState(FaceEpoch(), MetaState::kListening);
            } else if (strcmp(state->valuestring, "unowned") == 0) {
                ScheduleFaceMetaState(FaceEpoch(), MetaState::kUnowned);
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "transcript") == 0) {
        ScheduleFaceTranscript(FaceEpoch());
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "turn") == 0) {
        const cJSON* phase = cJSON_GetObjectItem(root, "phase");
        if (cJSON_IsString(phase)) {
            using ambient::face::Pipeline;
            const char* p = phase->valuestring;
            if (strcmp(p, "received") == 0) {
                ScheduleFacePipeline(FaceEpoch(), Pipeline::kReceived);
            } else if (strcmp(p, "thinking") == 0) {
                ScheduleFacePipeline(FaceEpoch(), Pipeline::kThinking);
            } else if (strcmp(p, "tool") == 0) {
                ScheduleFacePipeline(FaceEpoch(), Pipeline::kTool);
            } else if (strcmp(p, "speaking") == 0) {
                ScheduleFacePipeline(FaceEpoch(), Pipeline::kGenerating);
            } else if (strcmp(p, "done") == 0) {
                ScheduleFacePipeline(FaceEpoch(), Pipeline::kDone);
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "duoduo_said") == 0) {
        ScheduleFacePipeline(FaceEpoch(), ambient::face::Pipeline::kReply);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "mute_started") == 0) {
        const uint64_t epoch = FaceEpoch();
        ScheduleFaceMuted(epoch, true);
        const cJSON* until = cJSON_GetObjectItem(root, "until");
        if (cJSON_IsNumber(until)) ArmMuteTimer(epoch, until->valuedouble);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "mute_ended") == 0) {
        CancelMuteTimer();
        ScheduleFaceMuted(FaceEpoch(), false);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "senses_changed") == 0) {
        const cJSON* mic = cJSON_GetObjectItem(root, "mic");
        ScheduleFaceSenses(FaceEpoch(), !cJSON_IsFalse(mic));
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
}

/* Opus encode on a PSRAM stack. UplinkTask cannot hold 24 KB internally
 * (it touches NVS). Packets go to g_tx_buffer; only UplinkTask writes WS. */
void EncodeTask(void* /*arg*/) {
    std::vector<uint8_t> pcm(g_enc_frame_bytes);
    std::vector<uint8_t> out(g_enc_outbuf_bytes);
    size_t have = 0;
    unsigned enc_errors = 0;
    while (true) {
        size_t n = xStreamBufferReceive(g_pcm_buffer, pcm.data() + have,
                                        (size_t)g_enc_frame_bytes - have, portMAX_DELAY);
        have += n;
        if (have < (size_t)g_enc_frame_bytes) continue;
        have = 0;

        esp_audio_enc_in_frame_t in = {
            .buffer = pcm.data(),
            .len = (uint32_t)g_enc_frame_bytes,
        };
        esp_audio_enc_out_frame_t o = {
            .buffer = out.data(),
            .len = (uint32_t)g_enc_outbuf_bytes,
            .encoded_bytes = 0,
        };
        auto ret = esp_opus_enc_process(g_opus_enc, &in, &o);
        if (ret != ESP_AUDIO_ERR_OK) {
            if ((++enc_errors % 50) == 1) {
                ESP_LOGE(TAG, "[uplink] opus encode failed: %d (x%u)", (int)ret, enc_errors);
            }
            continue;
        }
        if (o.encoded_bytes == 0) continue;
        if (xMessageBufferSend(g_tx_buffer, out.data(), o.encoded_bytes, 0) == 0) {
            if ((++g_tx_dropped % 250) == 1) {
                ESP_LOGW(TAG, "[uplink] tx buffer full, dropped %u opus packets", g_tx_dropped);
            }
        }
    }
}

/* RFC 6716 TOC duration in 0.1 ms. Do not use audio_params frame_ms
 * (that is the 120 ms format cap); applying it to 60 ms packets runs
 * the watermark 2x fast and closes the echo window early. */
static uint32_t OpusPacketTenthMs(const uint8_t* p, size_t n) {
    if (n < 1) return 0;
    static const uint32_t kSilk[4] = {100, 200, 400, 600};  /* configs 0-11 */
    static const uint32_t kCelt[4] = {25, 50, 100, 200};    /* configs 16-31 */
    const uint8_t toc = p[0];
    const uint8_t config = toc >> 3;
    uint32_t per;
    if (config < 12)      per = kSilk[config & 3];
    else if (config < 16) per = (config & 1) ? 200 : 100;   /* hybrid 10/20 ms */
    else                  per = kCelt[config & 3];
    uint32_t frames;
    switch (toc & 3) {
        case 0: frames = 1; break;
        case 1:
        case 2: frames = 2; break;
        default: frames = (n >= 2) ? (uint32_t)(p[1] & 0x3F) : 0; break;
    }
    return per * frames;
}

/* May block on PushPacketToDecodeQueue so the WS task never waits on play. */
void FeedTask(void* /*arg*/) {
    std::vector<uint8_t> pkt(kMaxOpusPacketBytes);
    unsigned fed = 0;
    unsigned bytes = 0;
    while (true) {
        size_t n = xMessageBufferReceive(g_opus_buffer, pkt.data(), pkt.size(), portMAX_DELAY);
        if (n == 0) continue;
        if (g_opus_rate <= 0 || g_opus_frame_ms <= 0) {
            /* Guessing a rate undersizes the decode buffer. */
            if ((fed % 50) == 0) ESP_LOGW(TAG, "[dl] opus before audio_params — dropped");
            ++fed;
            continue;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = g_opus_rate;
        packet->frame_duration = g_opus_frame_ms;
        packet->timestamp = 0;
        packet->payload.assign(pkt.begin(), pkt.begin() + n);
        Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);

        {
            static uint32_t tenth_carry = 0;
            const uint32_t tenths = OpusPacketTenthMs(pkt.data(), n);
            taskENTER_CRITICAL(&g_played_mux);
            if (g_speech_id[0] != 0) {
                tenth_carry += tenths;
                g_played_ms += tenth_carry / 10;
                tenth_carry %= 10;
            } else {
                tenth_carry = 0;
            }
            taskEXIT_CRITICAL(&g_played_mux);
        }

        ++fed;
        bytes += n;
        if (fed == 1) {
            ESP_LOGW(TAG, "first downlink opus: %u B @%dHz/%dms — mouth is live",
                     (unsigned)n, g_opus_rate, g_opus_frame_ms);
        }
        if ((fed % 25) == 0) {
            ESP_LOGW(TAG, "[snd] fed=%u bytes=%u buf_fill=%u B dropped=%u",
                     fed, bytes,
                     (unsigned)(kOpusBufferBytes - xMessageBufferSpacesAvailable(g_opus_buffer)),
                     g_opus_dropped);
        }
    }
}

void UplinkTask(void* /*arg*/) {
    auto buffer = std::vector<uint8_t>(kMaxOpusPacketBytes);

    ESP_LOGW(TAG, "[boot] reset reason = %d", (int)esp_reset_reason());
    ESP_LOGW(TAG, "[mem] task start:       internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    EnsureWallClock();
    ESP_LOGW(TAG, "[mem] after wallclock:  internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    while (!EnsureTailnet()) {
        vTaskDelay(kReconnectDelay);
    }
    ESP_LOGW(TAG, "[mem] after tailnet:    internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Always-on audio cannot survive WIFI_PS_MIN_MODEM (DTIM batching). */
    if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) {
        ESP_LOGW(TAG, "wifi power save OFF (was IDF default MIN_MODEM)");
    } else {
        ESP_LOGE(TAG, "esp_wifi_set_ps(NONE) failed — downlink will stutter");
    }

    while (true) {
        MaybeRebindMicrolink();

        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            vTaskDelay(kReconnectDelay);
            continue;
        }

        /* Id 0: vendor protocol uses 1. */
        auto ws = network->CreateWebSocket(0);
        if (ws == nullptr) {
            ESP_LOGW(TAG, "CreateWebSocket failed");
            vTaskDelay(kReconnectDelay);
            continue;
        }

        /* OnData before Connect: later registration is ignored. */
        ws->OnData([](const char* d, size_t n, bool binary) {
            if (d == nullptr || n == 0) return;
            if (binary) { HandleDownlinkAudio(d, n); return; }
            if (!binary && d != nullptr && n > 0) HandleDownlink(d, n);
        });

        ESP_LOGI(TAG, "Connecting to %s", AMBIENT_WS_URI);
        /* Epoch before Connect: OnData can fire immediately. */
        const uint64_t face_epoch = g_face_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!ws->Connect(AMBIENT_WS_URI)) {
            ESP_LOGW(TAG, "Connect failed, code=%d", ws->GetLastError());
            vTaskDelay(kReconnectDelay);
            continue;
        }
        ESP_LOGW(TAG, "Connected to %s", AMBIENT_WS_URI);
        static bool hwm_logged = false;
        if (!hwm_logged) {
            hwm_logged = true;
            LogStackHighWaterMarks();
        }

        /* No audio or downlink until hello. conn is minted by the server. */
        ws->Send(std::string(
            "{\"type\":\"hello\",\"room\":\"" AMBIENT_ROOM "\",\"conn\":\"\","
            "\"edge\":\"device\",\"aec\":true}"));
        ScheduleFaceConnected(face_epoch);

        ESP_LOGW(TAG, "[mem] after ws connect: internal=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        /* Drop packets encoded while disconnected. Do not reset
         * g_pcm_buffer: EncodeTask may be blocked on it. */
        xMessageBufferReset(g_tx_buffer);

        char last_id[32] = {0};
        uint32_t last_ms = 0;

        while (ws->IsConnected()) {
            if (g_wifi_rebind.Pending()) {
                ESP_LOGW(TAG, "wifi came back under a live socket — drop it and rebind");
                break;
            }
            /* 100 ms so played still advances with no uplink packets. */
            size_t n = xMessageBufferReceive(g_tx_buffer, buffer.data(), buffer.size(),
                                             pdMS_TO_TICKS(100));
            if (n > 0) {
                if (!ws->Send(buffer.data(), n, true)) {
                    ESP_LOGW(TAG, "Send failed, reconnecting");
                    break;
                }
            }

            char id[32];
            uint32_t ms;
            taskENTER_CRITICAL(&g_played_mux);
            strlcpy(id, g_speech_id, sizeof(id));
            ms = g_played_ms;
            taskEXIT_CRITICAL(&g_played_mux);
            if (id[0] != 0 && ms > 0 && (ms != last_ms || strcmp(id, last_id) != 0)) {
                char frame[96];
                snprintf(frame, sizeof(frame),
                         "{\"type\":\"played\",\"speech_id\":\"%s\",\"ms\":%lu}",
                         id, (unsigned long)ms);
                if (!ws->Send(std::string(frame))) {
                    ESP_LOGW(TAG, "played send failed, reconnecting");
                    break;
                }
                strlcpy(last_id, id, sizeof(last_id));
                last_ms = ms;
            }
        }

        ESP_LOGI(TAG, "Disconnected");
        CancelMuteTimer();
        ScheduleFaceDisconnected(face_epoch);
        ws->Close();
        vTaskDelay(kReconnectDelay);
    }
}

}  // namespace

void StartWsUplink() {
    if (g_task != nullptr) {
        return;
    }
    Application::GetInstance().Schedule([]() {
        ESP_LOGW(TAG, "[face] main loop consumed the boot callback");
        RenderFaceIfChanged();
    });
    /* Static buffer storage is size+1. */
    auto storage = static_cast<uint8_t*>(
        heap_caps_malloc(kPcmBufferBytes + 1, MALLOC_CAP_SPIRAM));
    if (storage == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u B PCM buffer in PSRAM", (unsigned)kPcmBufferBytes);
        return;
    }
    auto* opus_storage = static_cast<uint8_t*>(
        heap_caps_malloc(kOpusBufferBytes + 1, MALLOC_CAP_SPIRAM));
    if (opus_storage == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for opus buffer (%u B)", (unsigned)kOpusBufferBytes);
        return;
    }
    g_opus_buffer = xMessageBufferCreateStatic(kOpusBufferBytes, opus_storage, &g_opus_buffer_ctrl);
    if (g_opus_buffer == nullptr) {
        ESP_LOGE(TAG, "xMessageBufferCreateStatic failed");
        return;
    }
    ESP_LOGW(TAG, "opus buffer %u B at %p (%s)", (unsigned)kOpusBufferBytes, opus_storage,
             esp_ptr_external_ram(opus_storage) ? "PSRAM" : "INTERNAL");

    g_pcm_buffer = xStreamBufferCreateStatic(kPcmBufferBytes, 1, storage, &g_pcm_buffer_ctrl);
    if (g_pcm_buffer == nullptr) {
        ESP_LOGE(TAG, "xStreamBufferCreateStatic failed");
        heap_caps_free(storage);
        return;
    }

    /* DTX off: server clock counts samples; a silent gap would stall it. */
    {
        esp_opus_enc_config_t enc_cfg = {
            .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
            .channel = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .bitrate = ESP_OPUS_BITRATE_AUTO,
            .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
            .complexity = 0,
            .enable_fec = false,
            .enable_dtx = false,
            .enable_vbr = true,
        };
        auto ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &g_opus_enc);
        if (g_opus_enc == nullptr) {
            ESP_LOGE(TAG, "esp_opus_enc_open failed: %d — uplink stays deaf", (int)ret);
            return;
        }
        esp_opus_enc_get_frame_size(g_opus_enc, &g_enc_frame_bytes, &g_enc_outbuf_bytes);
        ESP_LOGW(TAG, "opus encoder open: frame=%d B out<=%d B, free internal=%u B psram=%u B",
                 g_enc_frame_bytes, g_enc_outbuf_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    auto* tx_storage = static_cast<uint8_t*>(
        heap_caps_malloc(kTxBufferBytes + 1, MALLOC_CAP_SPIRAM));
    if (tx_storage == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for tx buffer (%u B)", (unsigned)kTxBufferBytes);
        return;
    }
    g_tx_buffer = xMessageBufferCreateStatic(kTxBufferBytes, tx_storage, &g_tx_buffer_ctrl);
    if (g_tx_buffer == nullptr) {
        ESP_LOGE(TAG, "tx xMessageBufferCreateStatic failed");
        return;
    }
    auto* enc_stack = static_cast<StackType_t*>(
        heap_caps_malloc(kEncStackBytes, MALLOC_CAP_SPIRAM));
    if (enc_stack == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for encoder task stack");
        return;
    }
    /* xTaskCreateStatic stack size is words, not bytes. */
    g_enc_task = xTaskCreateStatic(EncodeTask, "ambient_enc",
                                   kEncStackBytes / sizeof(StackType_t),
                                   nullptr, 3, enc_stack, &g_enc_tcb);
    ESP_LOGW(TAG, "PCM buffer %u B at %p (%s), free internal=%u B psram=%u B",
             (unsigned)kPcmBufferBytes, storage,
             esp_ptr_external_ram(storage) ? "PSRAM" : "INTERNAL-!!",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    xTaskCreate(UplinkTask, "ambient_ws", 4096, nullptr, 3, &g_task);
    auto* feed_stack = static_cast<StackType_t*>(
        heap_caps_malloc(kFeedStackBytes, MALLOC_CAP_SPIRAM));
    if (feed_stack == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for feed task stack");
        return;
    }
    /* Priority 7, above AFE (6). At 3, feed ran at 0.43x realtime. */
    g_feed_task = xTaskCreateStatic(FeedTask, "ambient_feed",
                                    kFeedStackBytes / sizeof(StackType_t),
                                    nullptr, 7, feed_stack, &g_feed_tcb);
}

/* All-or-nothing. A partial odd-byte write permanently misaligns int16
 * (server RMS sticks at 1/√3 ≈ 0.577). Single producer: FeedPcm only. */
void SendAligned(const void* data, size_t bytes) {
    if ((bytes % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "[uplink] refusing odd-sized write (%u B)", (unsigned)bytes);
        return;
    }
    if (xStreamBufferSpacesAvailable(g_pcm_buffer) < bytes) {
        static unsigned dropped = 0;
        if ((++dropped % 50) == 1) {
            ESP_LOGW(TAG, "[uplink] buffer full, dropped %u whole chunks (alignment preserved)",
                     dropped);
        }
        return;
    }
    const size_t sent = xStreamBufferSend(g_pcm_buffer, data, bytes, 0);
    if (sent != bytes) {
        ESP_LOGE(TAG, "[uplink] partial write %u/%u — STREAM MAY BE MISALIGNED",
                 (unsigned)sent, (unsigned)bytes);
    }
}

static void LogMicShape(const std::vector<int16_t>& pcm, bool speaking) {
    static unsigned n = 0;
    if ((n++ % 100) != 0 || pcm.empty()) {
        return;
    }
    int64_t sum = 0;
    int16_t lo = INT16_MAX, hi = INT16_MIN;
    for (int16_t v : pcm) {
        sum += v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    const double mean = (double)sum / (double)pcm.size();
    double var = 0;
    for (int16_t v : pcm) { const double d = v - mean; var += d * d; }
    const double ac_rms = sqrt(var / (double)pcm.size());
    ESP_LOGW(TAG, "[mic] spk=%d n=%u mean=%.1f ac_rms=%.1f min=%d max=%d "
                  "| mean/full=%.3f ac/full=%.3f | int_free=%u dma_max=%u",
             speaking ? 1 : 0, (unsigned)pcm.size(), mean, ac_rms, (int)lo, (int)hi,
             mean / 32768.0, ac_rms / 32768.0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

void FeedPcm(const std::vector<int16_t>& pcm) {
    if (g_pcm_buffer == nullptr || pcm.empty()) {
        return;
    }

    DriveHearingFace(pcm);

    static bool was_speaking = false;
    const bool speaking = Application::GetInstance().GetAudioService().IsSpeakerActive();
    if (speaking != was_speaking) {
        was_speaking = speaking;
        ESP_LOGW(TAG, "[uplink] live, speaker %s", speaking ? "active" : "idle");
        ScheduleFaceSpeaker(speaking);
    }

    LogMicShape(pcm, speaking);
    SendAligned(pcm.data(), pcm.size() * sizeof(int16_t));
}

}  // namespace ambient
