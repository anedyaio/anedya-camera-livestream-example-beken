# Changes to the Beken SDK

This example needs **six changed files, 93 lines** in the Beken Armino AVDK.
That is the complete list — it was produced by diffing the working tree against
upstream `release/v3.0.1`, not written from memory.

Everything here is either **"compile a file Beken already ships"** or
**"enable a flag Beken already provides"**. Nothing changes the behaviour of
any SDK feature this example does not use.

**Baseline:** `bekencorp/bk_avdk_smp`, branch `release/v3.0.1` (the repository
default), commit `4ca3893`. Forked to `anedyaio/bk_avdk_smp`.

libpeer is a separate fork: `sepfy/libpeer` at commit
`9319aa434cb9e893faed0293ba9d2a21eca59c8b`, which is upstream HEAD — this
example is not built on a stale snapshot. Its changes are extensive and
structural rather than a small patch set, so they live as commits in the fork
rather than being listed here.

To see the changes yourself:

```bash
git -C sdk diff <upstream-commit>..HEAD
```

---

## Why these exist at all

Three of the six exist because **the SDK ships source files it never
compiles**. They sit on disk, their config flags can be enabled, and they are
simply absent from the component's explicit source list — so their symbols do
not exist at link time no matter what you configure.

That failure mode is worth internalising if you work on this SDK: *a config
flag being set does not mean the code is compiled.* Check with `nm` on the
object file, not by reading Kconfig.

---

## 1. `ap/components/psa_mbedtls/CMakeLists.txt` — 5 lines

**Added** `mbedtls/library/rsa.c`, `ecdh.c`, `ecdsa.c`, `ecp.c`, and
`mbedtls_port/src/timing_alt.c` to the source list. **Removed**
`mbedtls/library/timing.c`.

**Why.** The component uses an explicit, non-globbed `srcs` list, and these
files were missing from it. `mbedtls_rsa_gen_key` is undefined at link; the EC
files become necessary as soon as DTLS uses ECDSA certificates, which WebRTC
does.

`timing.c` is swapped for `timing_alt.c` because the stock file is
POSIX/Windows-only and `#error`s on bare metal. **The SDK already ships a
working `timing_alt.c` port** — it was simply never wired up.

**Defence.** Adding files to a build cannot change the behaviour of code that
does not call them. Anyone not using DTLS pays only a small amount of flash.

---

## 2. `ap/components/psa_mbedtls/mbedtls_port/configs/mbedtls_psa_crypto_config.h` — 4 lines

**Enabled** `MBEDTLS_TIMING_ALT` and `MBEDTLS_SSL_DTLS_SRTP` (both were
present, commented out).

**Why.** `MBEDTLS_TIMING_ALT` matches patch 1 — without it, `timing_alt.c` does
not see the type declarations it implements and the build fails outright.
`MBEDTLS_SSL_DTLS_SRTP` is what makes DTLS-SRTP exist; WebRTC media is
undecryptable without it.

**Defence.** Both are upstream mbedTLS options that Beken already lists.
Enabling them adds capability; it removes nothing.

> Verified by experiment: reverting this file breaks the build immediately with
> `unknown type name 'mbedtls_timing_delay_context'`. It is not optional.

---

## 3. `ap/components/psa_mbedtls/mbedtls/library/ecp.c` — 18 lines

**Renamed** the file-local `static inline mbedtls_mpi_mul_mod()` to
`ecp_mpi_mul_mod()`.

**Why.** It collides with a **public, differently-signatured**
`mbedtls_mpi_mul_mod()` that Beken declares in its own patched
`mbedtls_port/mbedtls/include/mbedtls/bignum.h`, for the Dubhe hardware crypto
engine. Upstream mbedTLS and Beken's patch disagree, and the file does not
compile.

**Defence.** The function is `static` — it has internal linkage, so the rename
is invisible outside this one file. Behaviour is identical.

**This is arguably Beken's bug**, introduced by their own `bignum.h` patch, and
has been reported upstream.

---

## 4. `ap/components/lwip_intf_v2_1/CMakeLists.txt` — 8 lines

**Added** `apps/mqtt/mqtt.c`, `core/altcp.c`, `altcp_alloc.c`, `altcp_tcp.c`,
`apps/altcp_tls/altcp_tls_mbedtls.c`, `altcp_tls_mbedtls_mem.c`, and
`psa_mbedtls` to `PRIV_REQUIRES`.

**Why.** Same class as patch 1: lwIP's MQTT client and its TLS layer ship in
the SDK but are not in the build. Without them there is no MQTT-over-TLS, which
is how this example does signalling.

**Defence.** Purely additive. `PRIV_REQUIRES` is private to the component.

---

## 5. `ap/components/lwip_intf_v2_1/lwip-2.1.2/port/lwipopts.h` — 21 lines

**Added** `MQTT_OUTPUT_RINGBUF_SIZE 2048`.

**Why.** The stock default is 256 bytes, and that buffer must hold topic plus
payload of the largest publish. A compressed SDP answer is around 720 bytes, so
every answer failed with `ERR_MEM (-1)` and no call could ever be established.

**Defence.** One buffer size, costing ~1.8 KB of RAM. Affects only MQTT
publishes.

---

## 6. `ap/components/lwip_intf_v2_1/lwip-2.1.2/src/apps/altcp_tls/altcp_tls_mbedtls.c` — 37 lines

**Ported the file from mbedTLS 2.x to 3.x.** Removed includes of `certs.h`,
`net.h` and `ssl_internal.h` (all gone in 3.x), re-defined the
`MBEDTLS_ERR_NET_*` constants that moved, and updated `mbedtls_pk_parse_key()`,
which gained two RNG parameters in 3.x.

**Why.** lwIP's `altcp_tls` layer is written against mbedTLS 2.x. The SDK ships
mbedTLS 3.x. **As delivered these two cannot compile together**, so TLS over
lwIP is unbuildable out of the box.

**Defence.** This is unambiguously a defect in the SDK, not a preference of
ours — Beken pairs two incompatible versions. Reported upstream.

> **Already fixed in `release/v4.0.1`.** We checked. This example targets
> `release/v3.0.1` because it is the repository's default branch and because
> v4.0.1 is a 30,000-file jump that changes ~1,000 files in mbedTLS and ~950 in
> USB — the two subsystems this project depends on most. When this example
> migrates, this patch disappears on its own.

---

## What is *not* patched

Worth stating, because it bounds the risk:

- **No driver, RTOS, Wi-Fi or media-pipeline source is modified.** Everything
  above is mbedTLS, lwIP, or a build list.
- **No SDK behaviour is altered for features this example does not use.**
- Two patches carried during development were **removed** once they proved
  unnecessary: a fix to `rtos_debug.c` (a genuine interrupt-leak bug, but
  nothing here calls the affected function) and a change to `opus_dec.c` (Opus
  is not enabled). Both were reported upstream instead of carried.

---

## Configuration, not patches

The rest of this example's SDK tuning lives in
`app/ap/config/bk7258_ap/config` — ordinary Kconfig, no SDK files touched.
The non-obvious entries are documented inline there. The ones most likely to
matter to you:

| Option | Why |
|---|---|
| `CONFIG_FULL_MBEDTLS=y` | The board defconfig sets `n`, which gates off every DTLS-SRTP symbol |
| `CONFIG_CJSON_USE=y` | `cJSON.c` only compiles into the `json` component when set |
| `CONFIG_LWIP_MEM_MAX_TX_SIZE=125000` | **The** limiter for outbound video. At the 42666 default a single H.264 keyframe exceeds it and `sendto()` fails continuously |
| `CONFIG_JPEG_FRAME_SIZE=204800` | The UVC MJPEG assembly buffer. At the 100 KB default, detailed scenes overflow it and frames are discarded before reaching the encoder |
| `CONFIG_BLUETOOTH_AP=n` | Defaults to `y`; `bk_bluetooth_init()` runs at boot otherwise |

> ⚠️ `CONFIG_LWIP_MEM_*` have `range 1 150000` in Kconfig. **An out-of-range
> value is silently discarded** and falls back to the default. Always verify
> against the generated `sdkconfig.h`, never against the config file.

`app/partitions/bk7258/ram_regions.csv` also moves 448 KB of PSRAM from the
DISPLAY slab to ENCODE, to make room for the frame sizes above. DISPLAY holds
exactly one 720p YUV transcode frame and had spare capacity; the file documents
the arithmetic and how to revert it.
