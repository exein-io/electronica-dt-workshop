/*
 * Copyright (c) 2026 Exein
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public API for the Exein Runtime Security Monitor.
 */

#ifndef RUNTIME_ZEPHYR_H
#define RUNTIME_ZEPHYR_H

#include <stdint.h>

#include <zephyr/kernel.h>

/**
 * Attach an application-owned heap to the monitor.
 *
 * The system heap (`_system_heap`, backing `k_malloc`) is registered
 * automatically at startup. Use this to also watch a private `K_HEAP_DEFINE`
 * heap: it maps the heap and installs ALLOC/FREE listeners on it, so its
 * events flow through the same collect → analyze → dispatch pipeline.
 *
 * Requirements and limits:
 *   - Only `sys_heap`-backed heaps work (`k_heap`, and the libc arena when it
 *     is a sys_heap). `k_mem_slab`, net_buf pools and mbedTLS's own heap have
 *     no heap-listener hook and cannot be attached.
 *   - Needs `CONFIG_SYS_HEAP_LISTENER=y`.
 *   - At most `HEAP_TYPE_COUNT - 1` user heaps (the system heap holds the
 *     remaining slot). All attached heaps share the `HEAP_TYPE_USER` class, so
 *     the ML extractor aggregates their live bytes into one balance.
 *   - Call BEFORE runtime_zephyr_start(), like the low-level
 *     runtime_monitor_register_heap().
 *
 * @param heap  Application heap to monitor (its inner sys_heap is the id)
 * @param name  Short name for alerts (max 7 chars, null-terminated)
 * @return 0 on success, -EINVAL on a NULL argument, -ENOMEM if the heap table
 *         or the user-heap listener pool is full
 */
int runtime_monitor_add_heap(struct k_heap *heap, const char *name);

/**
 * Device description reported in SystemInfoRequest for the fields Zephyr has
 * no runtime API for. In blob delivery the library cannot see the
 * integrator's Kconfig, so the integrator hands it over here — normally via
 * RUNTIME_ZEPHYR_SYSINFO_INIT, which expands in the integrator's build and so
 * reports the integrator's real configuration. Any field can be overridden.
 *
 * The struct is copied at start; the strings are NOT, so they must be
 * literals or otherwise outlive the monitor. NULL string = field absent.
 */
struct runtime_zephyr_sysinfo {
	const char *board;          /* CONFIG_BOARD */
	const char *soc;            /* CONFIG_SOC */
	const char *app_version;    /* APP_VERSION_STRING */
	const char *libc_version;   /* e.g. "picolibc 1.8.11" */
	const char *custom_id;      /* CONFIG_RUNTIME_MONITOR_CUSTOM_ID, 1..64 chars */
	bool        hw_stack_protection; /* CONFIG_HW_STACK_PROTECTION */
	bool        stack_sentinel;      /* CONFIG_STACK_SENTINEL */
	bool        stack_canaries;      /* any CONFIG_STACK_CANARIES* choice */
	bool        userspace;           /* CONFIG_USERSPACE */
	bool        llext;               /* CONFIG_LLEXT */
	bool        mcuboot;             /* CONFIG_BOOTLOADER_MCUBOOT */
};

#if __has_include(<app_version.h>)
#include <app_version.h>
#endif
#if defined(APP_VERSION_STRING)
#define RUNTIME_ZEPHYR_APP_VERSION APP_VERSION_STRING
#else
#define RUNTIME_ZEPHYR_APP_VERSION NULL
#endif

#if defined(CONFIG_PICOLIBC) && __has_include(<picolibc.h>)
#include <picolibc.h>
#define RUNTIME_ZEPHYR_LIBC_VERSION "picolibc " __PICOLIBC_VERSION__
#elif defined(CONFIG_NEWLIB_LIBC) && __has_include(<newlib.h>)
#include <newlib.h>
#define RUNTIME_ZEPHYR_LIBC_VERSION "newlib " _NEWLIB_VERSION
#elif defined(CONFIG_MINIMAL_LIBC)
#define RUNTIME_ZEPHYR_LIBC_VERSION "minimal"
#else
#define RUNTIME_ZEPHYR_LIBC_VERSION NULL
#endif

/* custom_id length bounds; runtime_zephyr_start() rejects others with -EINVAL. */
#define RUNTIME_ZEPHYR_CUSTOM_ID_MIN 1
#define RUNTIME_ZEPHYR_CUSTOM_ID_MAX 64

#if defined(CONFIG_RUNTIME_MONITOR_CUSTOM_ID)
BUILD_ASSERT(sizeof(CONFIG_RUNTIME_MONITOR_CUSTOM_ID) - 1 <= RUNTIME_ZEPHYR_CUSTOM_ID_MAX,
	     "CONFIG_RUNTIME_MONITOR_CUSTOM_ID longer than 64 characters");
/* Empty string = not set. */
#define RUNTIME_ZEPHYR_CUSTOM_ID \
	(sizeof(CONFIG_RUNTIME_MONITOR_CUSTOM_ID) > 1 ? CONFIG_RUNTIME_MONITOR_CUSTOM_ID : NULL)
#else
#define RUNTIME_ZEPHYR_CUSTOM_ID NULL
#endif

/** Initializer for struct runtime_zephyr_sysinfo from the including build. */
#define RUNTIME_ZEPHYR_SYSINFO_INIT {                                        \
	.board               = CONFIG_BOARD,                                  \
	.soc                 = CONFIG_SOC,                                    \
	.app_version         = RUNTIME_ZEPHYR_APP_VERSION,                    \
	.libc_version        = RUNTIME_ZEPHYR_LIBC_VERSION,                   \
	.custom_id           = RUNTIME_ZEPHYR_CUSTOM_ID,                      \
	.hw_stack_protection = IS_ENABLED(CONFIG_HW_STACK_PROTECTION),        \
	.stack_sentinel      = IS_ENABLED(CONFIG_STACK_SENTINEL),             \
	.stack_canaries      = IS_ENABLED(CONFIG_STACK_CANARIES) ||           \
			       IS_ENABLED(CONFIG_STACK_CANARIES_STRONG) ||    \
			       IS_ENABLED(CONFIG_STACK_CANARIES_ALL) ||       \
			       IS_ENABLED(CONFIG_STACK_CANARIES_EXPLICIT),    \
	.userspace           = IS_ENABLED(CONFIG_USERSPACE),                  \
	.llext               = IS_ENABLED(CONFIG_LLEXT),                      \
	.mcuboot             = IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT),         \
}

/**
 * Runtime policy thresholds. A field left at 0 keeps the default the library
 * was built with (the matching Kconfig value); an out-of-range value also
 * falls back to it, with a warning. Only scalar thresholds are here: which
 * policies exist and ring/array depths stay build-time.
 */
struct runtime_zephyr_thresholds {
	uint8_t  heap_low_pct;           /* HEAP_LOW, 1..100 (CONFIG_HEAP_LOW_PCT) */
	uint8_t  stack_warn_pct;         /* STACK_LOW, 1..100 (CONFIG_THREAD_STACK_WARN_PCT) */
	uint16_t mem_leak_window_events; /* 4..1024 (CONFIG_MEM_LEAK_WINDOW_EVENTS) */
	uint32_t mem_leak_min_net_growth; /* bytes, 1..16777216 (CONFIG_MEM_LEAK_MIN_NET_GROWTH) */
	uint8_t  mem_leak_min_positive_windows; /* 1..32 (CONFIG_MEM_LEAK_MIN_POSITIVE_WINDOWS) */
};

/**
 * Where the monitor talks to the platform, and what authorises the first
 * registration (ADR-010 D9). Runtime parameters, so a precompiled library
 * bakes none of them. Fill it with RUNTIME_ZEPHYR_CONNECTION_INIT, which
 * expands in the integrator's build from its own Kconfig, or at runtime (e.g.
 * a per-device token from factory provisioning). The CA certificate is not
 * here: it is compiled into the library (lib/src/net/ssl/ca.cert.pem).
 *
 * The struct and its strings are copied at start (like the ESP32 agent's
 * vb_config_t), so the caller's buffers may be reused afterwards. Hosts up to
 * RUNTIME_ZEPHYR_HOST_MAX and the token up to RUNTIME_ZEPHYR_TOKEN_MAX
 * characters; longer ones are rejected, not truncated.
 */
#define RUNTIME_ZEPHYR_HOST_MAX  64
#define RUNTIME_ZEPHYR_TOKEN_MAX 128

/**
 * Credential backend supplied by the application (ADR-010 D11). With one set,
 * the library never calls tls_credential_add() or its PSA ITS store: the
 * backend owns where the CA and the client identity live (e.g. the nRF91
 * modem's key store via modem_key_mgmt, selected on sockets by sec_tag).
 * All three run on the thread calling runtime_zephyr_start(). PEM buffers are
 * NUL-terminated; @p len excludes the NUL.
 */
struct runtime_zephyr_cred_backend {
	/** Make the platform CA available under sec_tag. 0 if already present. */
	int  (*install_ca)(const uint8_t *pem, size_t len);
	/** Persist and activate the registered identity under sec_tag. Called
	 *  once per device lifetime, after /register; may take the link down. */
	int  (*install_identity)(const uint8_t *cert, size_t cert_len,
				 const uint8_t *key, size_t key_len);
	/** Is an identity already provisioned (D6 "registered")? */
	bool (*has_identity)(void);
};

/**
 * The platform CA compiled into the library, as NUL-terminated PEM; *len
 * excludes the NUL. For a backend that must provision the CA itself before
 * the link is up (modem key stores refuse writes while LTE is active).
 */
const uint8_t *runtime_zephyr_ca_pem(size_t *len);

struct runtime_zephyr_connection {
	const char *host;                  /* telemetry host, DNS name or IPv4; required */
	uint16_t    port;                  /* telemetry port; required */
	const char *registration_host;     /* NULL/"" = host */
	uint16_t    registration_port;     /* 0 = port */
	const char *bootstrap_token;       /* required; never logged */
	uint16_t    net_connect_timeout_s; /* IPv4 wait at start; 0 = library default */
	const struct runtime_zephyr_cred_backend *creds; /* NULL = built-in (tls_credentials + ITS) */
	int         sec_tag;               /* TLS sec_tag for all sockets; 0 = library default (1) */
};

#define RUNTIME_ZEPHYR_CONNECTION_INIT {                                       \
	.host                  = CONFIG_THREAT_HTTP_DEST_ADDR,                     \
	.port                  = CONFIG_THREAT_HTTP_PORT,                          \
	.registration_host     = CONFIG_RUNTIME_MONITOR_REGISTRATION_ADDR,         \
	.registration_port     = CONFIG_RUNTIME_MONITOR_REGISTRATION_PORT,         \
	.bootstrap_token       = CONFIG_RUNTIME_MONITOR_BOOTSTRAP_TOKEN,           \
	.net_connect_timeout_s = CONFIG_RUNTIME_MONITOR_NET_CONNECT_TIMEOUT,       \
}

/**
 * Initialize and start the runtime security monitor.
 *
 * Performs the full startup sequence:
 *   1. Init ICircularBuffers (collector)
 *   2. Start collector work queue and schedule periodic collectors
 *   3. Reset ICircularBuffers (clean baseline per ADR-002)
 *   4. Init policies and start analyzer thread
 *   5. Start dispatcher thread
 *   6. Load LLEXT hook modules from flash (if enabled, ADR-004)
 *
 * Call this AFTER registering heaps with runtime_monitor_register_heap()
 * and BEFORE starting application threads that allocate.
 *
 * The monitor never brings the network up: the application does, over any
 * L2. This call BLOCKS the calling thread, so make it from one that can wait
 * (main): up to conn->net_connect_timeout_s for IPv4, then the SNTP sync (up
 * to ~19 s), then, only when no client identity is stored yet, the
 * registration (up to ~45 s with retries), then the SystemInfo POST
 * (3 attempts, each after an IPv4 wait; all failing fails the start).
 * Without IPv4 it skips SNTP, registration and SystemInfo and continues.
 *
 * @param info  Device description for SystemInfoRequest (copied), or NULL
 *              to report only what the library reads at runtime.
 * @param conn  Platform endpoint and bootstrap token (copied); required.
 * @param th    Policy thresholds (copied), or NULL for the build defaults.
 *
 * @return 0 on success, -EINVAL for a bad custom_id, a NULL @p conn, a
 *         missing host / port / bootstrap token or an over-long connection
 *         string, other negative errno on failure
 */
int runtime_zephyr_start(const struct runtime_zephyr_sysinfo *info,
			 const struct runtime_zephyr_connection *conn,
			 const struct runtime_zephyr_thresholds *th);

#ifdef CONFIG_USERSPACE

/**
 * Add the currently loaded LLEXT hook module's executable text partition to a
 * memory domain.
 *
 * User threads that can trigger hooked syscalls must execute the hook code in
 * their own active memory domain. Call this after za_llext_load_module() or
 * runtime_zephyr_start() has loaded the module, before moving user threads into
 * a custom domain.
 *
 * @param domain Memory domain that should be able to execute the LLEXT hook
 * @return 0 on success, negative errno on failure
 */
int za_llext_add_text_to_domain(struct k_mem_domain *domain);

#endif /* CONFIG_USERSPACE */



#endif /* RUNTIME_ZEPHYR_H */
