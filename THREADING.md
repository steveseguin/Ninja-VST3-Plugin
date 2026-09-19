# Threading and lifetime model

Updated for the September 2026 hardening changes. See
[validation evidence](docs/developer/HARDENING_VALIDATION.md) for tested limits.

## Execution contexts

| Context | Responsibilities | Must not do |
|---|---|---|
| Host audio callback, `WebRTCProcessor::process` | Float32 dry passthrough/zeroing, finite-sample handling, numeric automation mailbox, bounded audio-bridge transfer | Wait for a lock, allocate/grow buffers, encode/decode, send signaling, log, notify the host/UI |
| Processor audio worker | Drain outgoing PCM, encode/send through the session, pull decoded audio/PLC into the bridge | Call controller/host UI APIs |
| Processor configuration worker | Validate complete configuration, start/stop/reconfigure sessions; poll numeric mode mailbox | Run work on the audio callback |
| IXWebSocket worker | Parse bounded signaling, announce roles, reconnect with cancellable backoff | Negotiate while holding the session lock |
| libdatachannel callbacks | Peer state, SDP/ICE, data channels, decode received Opus into per-peer queues | Assume a shutdown flag alone proves object lifetime |
| Controller UI timer | Poll processor status/configuration every 100 ms and update parameters | Be required for audio transport to work |
| Host control/lifecycle calls | State serialization, setup/activation, editor lifetime, terminate | Be assumed to run on a particular OS thread without checking the host contract |

## Audio boundary

`RealtimeAudioBridge` owns preallocated stereo queues (8192 frames each) and
512-frame worker chunks. Independent single-producer/single-consumer rings use
lock-free atomic cursors: the host owns TX writes/RX reads and the serialized
worker owns TX reads/RX writes. No try-lock contention drops host blocks.
Reset discards at consumer-owned cursors rather than rewriting live storage.
RX demand maintains four host blocks of scheduling headroom (minimum 1024 frames).
An empty RX queue still produces silence; a full TX queue drops excess input.
Bounded memory does not imply lossless operation under arbitrary starvation.

Receive demand follows frames actually requested by the host. It does not
free-run PLC ahead of the DAW. Mono input is duplicated to stereo; mono output
averages the two received channels. Network jitter and worker scheduling add
variable latency. Queue bounds prevent unbounded backlog; they do not guarantee
dropout-free audio under arbitrary CPU starvation.

The session's resampler, Opus, per-peer buffers and scratch vectors can still
allocate or lock, but execute off the host audio thread. `AudioRingBuffer` uses
its own synchronization; it must not be described as lock-free.

Offline-bounce blocks do not enqueue DAW audio for transmission. Their Seed
output is dry; Play output is silent. Prefetch is **not** an offline bounce:
`IPrefetchableSupport` requests real-time host processing for this live bridge,
but prefetch blocks remain audible if a host ignores that request. Hosts that
still anticipate the track may need their per-track anticipative processing
disabled for lowest latency. Signaling can remain connected during an offline
bounce, so this is not a network-isolation switch.

## Locks and ownership

- `configMutex_` protects the processor's configuration snapshot. Text changes
  travel as bounded JSON messages through `IConnectionPoint`, not a global
  process-local string registry or numeric audio automation.
- `audioWorkMutex_` serializes processor audio-worker session calls against
  start/stop. The host audio callback never takes it.
- The session `mutex_` protects peer maps and codec/session state. Per-peer
  audio contexts have their own lock and shared ownership. Keep lock scopes
  short and never wait on RTC worker futures while holding the session lock.
- IX callback storage has its own lock. Copy callbacks under it, then invoke
  outside it. Stop/join the WebSocket worker outside that lock.
- Status is queued internally and sent to the host from the controller's poll.
  Hosts without a UI event loop can explicitly send `PollStatus` for telemetry.
- Copy/QR side effects require a native editor gesture. Host parameter writes
  remain inert, even though hosts can write non-automatable parameter IDs.

Do not enable implicit RTC negotiation: `createDataChannel()` can otherwise
wait for certificate generation under the session lock while the entire RTC
pool waits for that lock. Create peer objects under the lock, retain a
`shared_ptr`, release the lock, then set descriptions/explicitly answer offers.
The peer-burst regression covers this previously reproduced deadlock.

## Reconnect

IX owns one retry worker, with failed-connect delays bounded to 1–30 seconds.
An additional cancellable delay bounds repeated successful-open/close loops.
There are no detached per-attempt reconnect threads.

Signaling failure does not immediately destroy active media. Reconnect restores
room/publishing intent; a healthy Play peer is retained without requesting a
duplicate. A replacement connection for the same stream retires the old peer;
distinct room streams remain independent. Failed/closed Play peers request
recovery. This is not full ICE restart or TURN support.

## Shutdown and remaining audit boundaries

Processor termination deactivates processing, joins its audio/config workers,
and explicitly stops the session **before member destruction**. The destructor
also performs this cleanup. Reverse declaration-order destruction is not a
substitute for explicit lifetime management.

Session stop marks shutdown/stopped, clears owned sinks, moves the signaling
client out under lock, then stops and joins its worker outside the lock. Peer
callbacks are detached, audio contexts marked inactive, peers closed, and codecs
released. Shared audio-context captures keep decoder storage alive while held.

An early shutdown-flag check does not cancel callbacks already in flight.
Likewise, copying a raw codec pointer and releasing its lock does not make later
use safe. Changes to callback ownership need live teardown stress and sanitizer
coverage; do not infer safety merely from short smoke tests. Current ASan/UBSan
and TSan coverage instruments settings/audio-bridge unit paths, **not the whole
plugin and third-party RTC stack**. Full-stack TSan/ASan remains an explicit gap.

## Regression commands

```bash
ctest --test-dir build/webrtc_vst_mac -C Release --output-on-failure
npm run test:hardening
WEBRTC_EXTENDED_SECONDS=1800 npm run test:extended
WEBRTC_SOAK_SECONDS=1800 WEBRTC_SOAK_RECONNECT=1 npm run test:audio-soak
```

The extended runner owns a loopback signaling fixture, records seeds/RSS and
checks that the plugin binary hash stays unchanged. The audio soak owns both
synthetic endpoints and measures one-second stereo level, pitch, finite-output,
silence and callback-duration windows. Neither test captures physical inputs.
