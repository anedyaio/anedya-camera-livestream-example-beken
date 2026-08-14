# Changes to the Beken SDK

This example needs **six changed files, 93 lines** in the Beken Armino AVDK.
That is the complete list — it was produced by diffing the working tree against
upstream `release/v3.0.1`, not written from memory.

They fall into three groups, and the distinction is the whole point of this
document:

| Group | Files | Lines | What it is |
|---|---|---|---|
| **A — Build lists** | 2 | 13 | Compile source files the SDK already ships but never builds. **Not one line of that source is modified.** |
| **B — Configuration** | 2 | 25 | Enable options the SDK already provides, and raise one buffer size. No logic changed. |
| **C — Bug fixes** | 2 | 55 | The only two places Beken's code is edited. Both files **do not compile as delivered.** |

Groups A and B are most of the files and none of the risk: nothing in them
alters the behaviour of any SDK feature. Group C is where we actually changed
Beken's source, and both are defects rather than preferences.

**Baseline:** `bekencorp/bk_avdk_smp`, branch `release/v3.0.1` (the repository
default), commit `4ca3893`. Forked to
[`anedyaio/bk_avdk_smp`](https://github.com/anedyaio/bk_avdk_smp), branch
`anedya/v3.0.1`. The upstream branch is mirrored untouched in the same
repository, so the comparison is one command away:

```bash
git -C sdk log  release/v3.0.1..HEAD
git -C sdk diff release/v3.0.1..HEAD
```

libpeer is a separate fork:
[`anedyaio/libpeer`](https://github.com/anedyaio/libpeer), from `sepfy/libpeer`
at commit `9319aa434cb9e893faed0293ba9d2a21eca59c8b`, which is upstream HEAD —
this example is not built on a stale snapshot. Its changes are extensive and
structural rather than a small patch set, so they live as commits in the fork
rather than being listed here.

---

# Group A — Build lists

**The SDK ships source files it never compiles.** They sit on disk, their config
flags can be enabled, and they are simply absent from the component's explicit
source list — so their symbols do not exist at link time no matter what you
configure.

Both changes here add existing filenames to a CMake list. **No `.c` or `.h` file
they reference is modified in any way.**

That failure mode is worth internalising if you work on this SDK: *a config flag
being set does not mean the code is compiled.* Check with `nm` on the object
file, not by reading Kconfig.

## A1. `ap/components/psa_mbedtls/CMakeLists.txt` — 5 lines

**Added** `mbedtls/library/rsa.c`, `ecdh.c`, `ecdsa.c`, `ecp.c`, and
`mbedtls_port/src/timing_alt.c` to the source list. **Removed**
`mbedtls/library/timing.c`.

**Why.** The component uses an explicit, non-globbed `srcs` list, and these
files were missing from it. `mbedtls_rsa_gen_key` is undefined at link; the EC
files become necessary as soon as DTLS uses ECDSA certificates, which WebRTC
does.

`timing.c` is swapped for `timing_alt.c` because the stock file is
POSIX/Windows-only and `#error`s on bare metal. **The SDK already ships a
working `timing_alt.c` port** — it was simply never wired up. We did not write
it and did not change it.

**Defence.** Adding files to a build cannot change the behaviour of code that
does not call them. Anyone not using DTLS pays only a small amount of flash.

## A2. `ap/components/lwip_intf_v2_1/CMakeLists.txt` — 8 lines

**Added** `apps/mqtt/mqtt.c`, `core/altcp.c`, `altcp_alloc.c`, `altcp_tcp.c`,
`apps/altcp_tls/altcp_tls_mbedtls.c`, `altcp_tls_mbedtls_mem.c`, and
`psa_mbedtls` to `PRIV_REQUIRES`.

**Why.** Same class as A1: lwIP's MQTT client and its TLS layer ship in the SDK
but are not in the build. Without them there is no MQTT-over-TLS, which is how
this example does signalling.

**Defence.** Purely additive. `PRIV_REQUIRES` is private to the component. Of
the six files named here, five compile exactly as Beken ships them; the sixth,
`altcp_tls_mbedtls.c`, is separately fixed in C2 because it does not compile
against this SDK's mbedTLS version.

---

# Group B — Configuration

Options that were **already present in Beken's own headers** — two of them
literally commented out — plus one buffer size. No new code.

## B1. `ap/components/psa_mbedtls/mbedtls_port/configs/mbedtls_psa_crypto_config.h` — 4 lines

**Uncommented** `MBEDTLS_TIMING_ALT` and `MBEDTLS_SSL_DTLS_SRTP`. Both were
already listed in this file, disabled.

**Why.** `MBEDTLS_TIMING_ALT` matches A1 — without it, `timing_alt.c` does not
see the type declarations it implements and the build fails outright.
`MBEDTLS_SSL_DTLS_SRTP` is what makes DTLS-SRTP exist; WebRTC media is
undecryptable without it.

**Defence.** Both are upstream mbedTLS options that Beken already lists.
Enabling them adds capability; it removes nothing.

> Verified by experiment: reverting this file breaks the build immediately with
> `unknown type name 'mbedtls_timing_delay_context'`. It is not optional.

## B2. `ap/components/lwip_intf_v2_1/lwip-2.1.2/port/lwipopts.h` — 21 lines

**Added** `MQTT_OUTPUT_RINGBUF_SIZE 2048`.

**Why.** The stock default is 256 bytes, and that buffer must hold topic plus
payload of the largest publish. A compressed SDP answer is around 720 bytes, so
every answer failed with `ERR_MEM (-1)` and no call could ever be established.

**Defence.** One buffer size, costing ~1.8 KB of RAM. It affects only MQTT
publishes, and only their size limit — no logic, no other subsystem.

---

# Group C — Bug fixes

**These two are the only files where Beken's source code is edited.** In both
cases the file cannot be compiled as delivered, so leaving it alone was not an
option.

## C1. `ap/components/psa_mbedtls/mbedtls/library/ecp.c` — 18 lines · symbol collision

**Renamed** the file-local `static inline mbedtls_mpi_mul_mod()` to
`ecp_mpi_mul_mod()`.

**The bug.** It collides with a **public, differently-signatured**
`mbedtls_mpi_mul_mod()` that Beken declares in its own patched
`mbedtls_port/mbedtls/include/mbedtls/bignum.h`, for the Dubhe hardware crypto
engine. Upstream mbedTLS and Beken's patch disagree on the same name, and the
file does not compile.

**Defence.** The function is `static` — it has internal linkage, so the rename
is invisible outside this one translation unit. Behaviour is identical.

**This is a defect introduced by the SDK's own `bignum.h` patch**, not by
upstream mbedTLS. Reported upstream.

## C2. `ap/components/lwip_intf_v2_1/lwip-2.1.2/src/apps/altcp_tls/altcp_tls_mbedtls.c` — 37 lines · mbedTLS 2.x vs 3.x

**Ported the file from mbedTLS 2.x to 3.x.**

**The bug.** lwIP's `altcp_tls` layer is written against **mbedTLS 2.x**. The SDK
ships **mbedTLS 3.x**. As delivered the two cannot compile together, so TLS over
lwIP is unbuildable out of the box.

What the 3.x port required:

- `mbedtls/certs.h`, `mbedtls/net.h` and `mbedtls/ssl_internal.h` no longer
  exist — includes removed
- The `MBEDTLS_ERR_NET_*` constants moved out of the public headers — the three
  used here are defined locally
- `mbedtls_pk_parse_key()` gained an RNG function and context parameter — the
  two calls now pass the config's existing CTR-DRBG
- `mbedtls_ssl_get_max_frag_len()` was removed — the max-fragment clamp in
  `altcp_mbedtls_sndbuf()` is dropped. The value returned is still bounded by
  the inner TCP connection's own `sndbuf`, which is far smaller in practice
- `mbedtls_ssl_context.out_left` became private — lwIP's flush-before-write
  block (its own comment calls it a "HACK") can no longer be expressed and is
  removed; `mbedtls_ssl_write()` is called directly

The last two are the only places where behaviour changes rather than just the
API. Both are consequences of mbedTLS 3.x making internals private, and both
concern a send path that this example exercises continuously — every MQTT
publish goes through it.

**Defence.** This is unambiguously a defect in the SDK, not a preference of
ours — Beken pairs two incompatible versions.

> **Already fixed in `release/v4.0.1`.** We checked. This example targets
> `release/v3.0.1` because it is the repository's default branch and because
> v4.0.1 is a 30,000-file jump that changes ~1,000 files in mbedTLS and ~950 in
> USB — the two subsystems this project depends on most. When this example
> migrates, this fix disappears on its own.

---

## What is *not* changed

Worth stating, because it bounds the risk:

- **No driver, RTOS, Wi-Fi, USB or media-pipeline source is modified.**
  Everything above is mbedTLS, lwIP, or a build list.
- **No SDK behaviour is altered for any feature this example does not use.**
- Two changes carried during development were **removed** once they proved
  unnecessary, rather than kept "just in case": a fix to `rtos_debug.c` (a
  genuine interrupt-leak bug, but nothing here calls the affected function) and
  a change to `opus_dec.c` (Opus is not enabled). Both were reported upstream
  instead of carried.

---

## Configuration, not changes

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
