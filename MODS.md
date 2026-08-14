# Ambient modifications

This tree is a downstream of [Xgorobot/RIG-Omni](https://github.com/Xgorobot/RIG-Omni)
(via `LuwuDynamics/rig_omni`). It adds one product: the hover as an
**always-on ear and mouth** of a duoduo ambient room.

The device does I/O only. VAD, ASR, speaker-id, and understanding live on
the server. There is no on-device wake word.

Our code lives under `main/ambient/`. Upstream files change only at the
sites tagged `[ambient-mod]`, so `git merge upstream/main` stays a
countable conflict surface.

Balance control, servos, the camera, and the expression panel are
untouched product features. Do not treat them as RAM levers.

---

## Build

```bash
git clone --recursive <this-repo>
source ~/.tailscale-env          # TS_AUTH_KEY + AMBIENT_SERVER_TAILNET_IP
idf.py build
```

`.envrc` sources `~/.tailscale-env` when direnv is on.

| Variable | Role |
| --- | --- |
| `TS_AUTH_KEY` | Tailnet auth. Injected at configure time. **Never commit.** Missing → `FATAL_ERROR`. |
| `AMBIENT_SERVER_TAILNET_IP` | Ambient host's tailnet IP (not a MagicDNS name — device DNS is the Wi-Fi router). Same injection path. Missing → `FATAL_ERROR`. |

Submodule: `poc/third_party/microlink` →
[antmanler/microlink](https://github.com/antmanler/microlink) branch
`duoduo-edge`. Edit there, commit, push, bump the gitlink here. No
patch-apply step.

Host tests (no IDF):

```bash
g++ -std=c++17 -I main/ambient main/ambient/ambient_face_state_test.cc -o /tmp/face_test && /tmp/face_test
g++ -std=c++17 -I main/ambient main/ambient/ambient_rebind_gate_test.cc -o /tmp/rebind_test && /tmp/rebind_test
```

Flash the **app partition only**. `idf.py flash` also rewrites the assets
partition; this tree's assets are not the factory set.

```bash
esptool.py --port <port> -b 921600 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x20000 build/rig-hover.bin
```

After any flash, confirm a fresh boot banner. `--after hard_reset` can
silently fail on this board; the old image then keeps running from cache.

`sdkconfig` is gitignored. Board-level defaults that a fresh clone needs
belong in `sdkconfig.defaults`.

---

## Runtime

```
Wi-Fi
  → microlink (WireGuard tailnet)
  → ws://<AMBIENT_SERVER_TAILNET_IP>:38090/live?room=office
       hello + Opus uplink + played watermark
       text frames drive the face
  → server: VAD / ASR / speaker-id / understanding / TTS
  → Opus downlink → vendor decoder → I2S
```

The host publishes that port with `tailscale serve` onto loopback ambient.
The device has no LAN exposure.

Room is compile-time (`AMBIENT_ROOM`, currently `office`). The device
and the browser page are two ears of the same room; capture mastership
is the server's job.

Audio format: **Int16LE @ 16 kHz mono** into the device's Opus encoder
for uplink. Downlink is one binary WebSocket frame per raw Opus packet.

---

## Our files

| Path | What |
| --- | --- |
| `main/ambient/ambient_ws.h` | Uplink API + compile-time URI |
| `main/ambient/ambient_ws.cc` | Full-duplex ambient link, tailnet bring-up, face adapters |
| `main/ambient/ambient_face_state.h` | Host-testable face reducer |
| `main/ambient/ambient_face_state_test.cc` | Face priority / epoch / seat honesty |
| `main/ambient/ambient_rebind_gate.h` | Rebind only after DISCONNECTED then GOT_IP |
| `main/ambient/ambient_rebind_gate_test.cc` | That gate |

---

## Face

Hover animations come from `main/boards/hover/240_240/emote.json` and
the packed `board_assets` partition. Call only names that exist there.
`connecting` is not packed; the reducer maps that state to `scanning`.

Precedence:

```
offline > senses-off > muted > unowned > tts > pipeline > listening
```

Mute and senses-off outrank an unowned seat: those are the user's own
actions. Unowned still outranks the pipeline. Offline outranks everything.

| Ambient state | Packed EAF |
| --- | --- |
| listening (speech-band energy) | `listen` |
| quiet, seated | `neutral` |
| heard | `surprised` |
| received | `winking` |
| thinking | `thinking` |
| tool | `cool` |
| generating | `confident` |
| reply | `happy` |
| tts | `laughing` |
| muted | `icon_speaker_zzz` |
| senses-off | `sleepy` |
| unowned (no capture seat) | `confused` |
| offline | `sad` |

The reducer owns `SetEmotion`. Remote facts carry a connection epoch so
a stale WebSocket callback cannot overwrite the live connection.
Disconnect clears remote facts and keeps the locally observed speaker
fact. `IsSpeakerActive()` supplies `tts`; playback never implies the
mic is off.

`mute_started.until` is an absolute Unix-ms timestamp. A one-shot timer
clears the mute face at that instant even if the server's lazy
`mute_ended` has not arrived.

Reconnect snapshot is a protocol gap: a new edge gets coarse
`meta.state`, not current mute / senses / turn. Lost connections reset
those remote facts rather than display stale state.

The emote renderer waits on `WAIT_FLUSH_DONE` while holding
`gfx_emote_lock`. A failed SPI queue never starts a transfer, so the
flush callback must call `emote_notify_flush_finished` on draw failure
or the main task wedges inside `SetEmotion`. Audio tasks do not take
that lock.

---

## Upstream touch points

All tagged `[ambient-mod]`.

### Version (`CMakeLists.txt`)

`PROJECT_VER` is `3.6.0-ambient`. Factory units run 3.6.0. Flashing
plain `3.5.0` makes the vendor OTA (`xl-api.xgorobot.com`) push 3.6.0
and overwrite this firmware (`HasNewVersion`). The suffix makes
`IsNewVersionAvailable` return false.

This does **not** stop a server-side `force:1`. Pointing `wifi.ota_url`
off the vendor is the durable fix; it needs an NVS wipe and a manual
provision. Keep the vendor OTA URL: a failed redirect sits in
`kDeviceStateActivating` with the mic down for the retry backoff
(~85 minutes). The vendor "already latest" reply returns in seconds.

### Sources (`main/CMakeLists.txt`)

Adds `ambient/ambient_ws.cc` and the `ambient` include dir.

### Audio callbacks (`main/audio/audio_service.h` / `.cc`)

- `on_raw_audio` — pre-AFE PCM, same tap as `CONFIG_USE_AUDIO_DEBUGGER`. Debug only.
- `on_processed_audio` — post-AFE frames, **not** VAD-gated. This is the uplink.
- Vendor Opus **encoder** is compiled out (`#if 0`). The decoder stays (`PlaySound`, downlink).
- Wake-word engine is never created (`SetModelsList` returns after
  `wake_word_ = nullptr`). The design is an always-open mic; a second
  esp-sr AFE would duplicate the dual-channel AEC working set. Mic
  liveness is `AUDIO_PROCESSOR_RUNNING`, not the wake-word event bit.

### Application (`main/application.cc`)

- Wires `on_processed_audio` → `ambient::FeedPcm`.
- Starts `ambient::StartWsUplink` only after the network is up.
- Does **not** call `InitializeProtocol()`. The vendor cloud is a second
  brain; this body has one. MQTT stays dark.
- Does **not** enable wake-word detection.

### Camera (`main/boards/hover/hover_board.cc`)

`InitializeCamera()` stays. It is a product requirement.

### mbedtls (`sdkconfig.defaults`)

`CHACHA20` / `CHACHAPOLY` / `POLY1305` / `HKDF` must be here, not in
`sdkconfig`. A fresh clone otherwise fails to link `mbedtls_chachapoly_*`.

### AEC (`sdkconfig.defaults`)

`CONFIG_USE_DEVICE_AEC=y`. Mutually exclusive with `USE_SERVER_AEC`
(`application.cc` `#error`). The reference is the hardware I2S loopback
on the codec (clock stays on). AFE type is `AFE_TYPE_VC` /
`AEC_MODE_VOIP_HIGH_PERF`. Neural-net NS is off (`ns_init = false`):
VAD/ASR live on the server; NS on the device starves the feed path.
Do not turn NS back on for "quality".

Uplink stays open while the speaker is active. Muting the uplink during
playback is half-duplex and is not an available workaround. Echo is
the device's problem (AEC) plus whatever the server already does for
every edge. The server does not grow a device-only echo channel.

### PCM buffer

The uplink PCM stream buffer is `xStreamBufferCreateStatic` in PSRAM.
Writes are all-or-nothing and 2-byte aligned. A partial or odd-byte
write permanently misaligns the int16 stream; the reader also drops a
trailing odd byte and logs. Failures must be loud.

---

## microlink

`CMakeLists.txt` adds `EXTRA_COMPONENT_DIRS` and injects `TS_AUTH_KEY`.
`main/CMakeLists.txt` `PRIV_REQUIRES microlink`.

`ml_wg_mgr` is a WireGuard lwIP netif. Standard sockets reach `100.x`.
The firmware reuses the vendor WebSocket client; it does not use
microlink's own TCP API.

### What the fork carries

Branch `duoduo-edge`, currently on top of upstream `216da33` (v2.1.0 +
`microlink_rebind` + switching debounce + PPP IPv6).

| Theme | Keep until |
| --- | --- |
| TAI64N from `gettimeofday`, PSRAM stacks for flash-free tasks, diagnostics that print lengths not keys | permanent |
| Upstream PR #20 (pbuf double-free on the WG RX path) | drop after upstream merges |
| Outbound handshake allowed (does not force `peers[idx].active = false`) | until upstream agrees |
| `wg_udp_output_cb` dispatched by caller thread | drop after upstream merges |
| `ML_CONFIG_HTTPD` gate (this board: `n`) | keep; re-provision needs it on |
| `ML_PEER_UPDATE_QUEUE_DEPTH` 32 | keep |

`ml_wg_mgr`'s stack stays in **internal** RAM. That task touches NVS
(flash cache disabled). A PSRAM stack there asserts
`esp_task_stack_is_sane_cache_disabled`. Other microlink stacks that
never touch flash may sit in PSRAM.

### Clock

WireGuard TAI64N is wall-clock. After a power cycle the RTC is zero, so
uptime-based timestamps date ~55 years early and every handshake is
refused (control plane up, data plane dead).
`ambient_ws.cc::EnsureWallClock()` runs SNTP if the clock is still
epoch; the vendor OTA `server_time` already sets it on the normal path.

### Rebind

`microlink_rebind()` is called only from `UplinkTask`, and only after
`WIFI_EVENT_STA_DISCONNECTED` followed by `IP_EVENT_STA_GOT_IP`. A DHCP
renew is GOT_IP alone and must not rebind. The event task must not
sleep inside `microlink_rebind`. A live `/live` socket is dropped so
the outer loop can rebind before `Connect`.

`ml_net_switch` is not used: that module owns Wi-Fi + cellular, and
hover already has `WifiBoard`.

### Identity

microlink keeps machine / WG / DISCO keys in NVS. Swapping `TS_AUTH_KEY`
does **not** swap tailnets; the old node identity is reused. A real
tailnet change needs `microlink_factory_reset()` **before** init.

`do_register` succeeds whenever a `Node` object is present. It does not
check `MachineAuthorized` / `NodeKeyExpired` / `AuthURL` / `Error`. A
true registration prints all three of: VPN IP, Home DERP region,
Self-Node Key.

This tree logs at WARN (`CONFIG_LOG_DEFAULT_LEVEL=2`). `ESP_LOGI` is
compiled out. Diagnostics that must be visible use `ESP_LOGW`.

### Cost

Linking microlink adds about **+1.7 KB DIRAM** and +86 KB flash versus
the same firmware without it. Runtime cost is the task stacks and
buffers, not the 116 KB SRAM figure in the upstream example README
(that number is the whole example program).

DERP region/host is a compile-time constant in the fork. Changing it
locks the binary to one tailnet. Dynamic DERP discovery exists
upstream; this tree does not use it.

---

## Constraints

- One brain. Do not re-init the vendor conversation protocol.
- Always-open mic. Do not re-arm the wake-word engine without
  re-auditing the internal-RAM budget (it is a second AFE).
- Camera stays.
- Do not mute uplink during playback.
- Do not enable AFE neural-net NS.
- `played` is a decode-admission watermark, up to one downlink buffer
  (~2400 ms) ahead of the speaker. It is not "the mouth is sounding
  now".
- `LOG_DEFAULT_LEVEL=2`. New diagnostics at WARN or they vanish.
- Internal-RAM tasks that touch flash/NVS stay on internal stacks.
- `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` silently migrates Wi-Fi TX buffers
  Dynamic → Static×16 and does **not** migrate them back when cleared.
  `sdkconfig` will not show up in git. After any sdkconfig edit, re-read
  the whole `ESP_WIFI_*_BUFFER` block against intent.

---

## Merge notes

| Site | When upstream moves |
| --- | --- |
| `PROJECT_VER` | take upstream, keep the `-ambient` suffix |
| `main/CMakeLists.txt` source list | keep the two ambient lines |
| `AudioServiceCallbacks` | keep the two extra fields |
| `application.cc` protocol / wake-word / uplink start | keep the skips and the ambient start |
| microlink gitlink | bump independently; rebase `duoduo-edge` on upstream as needed |
