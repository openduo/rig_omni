/*
 * [ambient-mod] duoduo ambient — always-on microphone uplink.
 *
 * The device is I/O only: it pushes PCM into the room's /live socket.
 * VAD / ASR / speaker-id / understanding live on the server. There is
 * no on-device wake-word concept.
 *
 * This directory is ours. Upstream edits are listed in /MODS.md.
 */
#ifndef AMBIENT_WS_H
#define AMBIENT_WS_H

#include <cstdint>
#include <vector>

/*
 * /live URI for this edge.
 *
 * Compile-time — changing the host or room means a rebuild. Same class
 * of constant as CONFIG_AUDIO_DEBUG_UDP_SERVER: the device has no
 * config surface.
 *
 * `room` is required. Ambient does not guess rooms (a wrong guess
 * attributes one room's audio to another).
 *
 * Over the tailnet, not the LAN. The device gets no LAN exposure.
 * AMBIENT_SERVER_TAILNET_IP is injected at configure time
 * (CMakeLists.txt, same path as TS_AUTH_KEY). It must be an IP, not a
 * MagicDNS name: device DNS is the Wi-Fi router.
 *
 * The host publishes that IP with `tailscale serve --http=38090` onto
 * loopback ambient. Room `office` is the physical room this hover sits
 * in; the browser page is the other ear of the same room.
 */
#ifndef AMBIENT_SERVER_TAILNET_IP
#error "AMBIENT_SERVER_TAILNET_IP must be injected at build time (see CMakeLists.txt)"
#endif
#define AMBIENT_ROOM "office"
#define AMBIENT_WS_URI "ws://" AMBIENT_SERVER_TAILNET_IP ":38090/live?room=" AMBIENT_ROOM

namespace ambient {

/* Start the always-on uplink task. Idempotent. */
void StartWsUplink();

/*
 * Feed one microphone PCM block (Int16LE @ 16 kHz mono).
 *
 * Called from the audio input task, so this never blocks: a full
 * buffer drops the block. Dropping audio is better than stalling the
 * input task (that stalls the whole capture path).
 */
void FeedPcm(const std::vector<int16_t>& pcm);

}  // namespace ambient

#endif  // AMBIENT_WS_H
