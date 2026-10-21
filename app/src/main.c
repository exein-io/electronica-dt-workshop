/*
 * Exein Runtime Security Monitor on the nRF9151 DK.
 *
 * Runs the monitor (precompiled runtime-zephyr blob, HEAP_LOW policy: the
 * only finding the platform accepts today) beside a button-driven demo
 * workload (see app_workload.c). The monitor registers with the Exein platform over LTE and
 * posts incidents there; TLS runs in the modem, credentials live in the
 * modem's key store under SEC_TAG.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/storage/flash_map.h>
#include <pm_config.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>

#include <runtime_zephyr.h>
#include "app_workload.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* The blob's kernel objects (heap_msgq: 52 bytes) are laid out for the kernel
 * config it was built with, CONFIG_ASSERT=y among it (SPIN_VALIDATE grows
 * struct k_spinlock). A mismatch links fine and faults at runtime.
 */
BUILD_ASSERT(sizeof(struct k_msgq) == 52,
	     "struct k_msgq differs from the runtime-zephyr blob: set CONFIG_ASSERT=y");

/* ponytail: fixed modem sec_tag, make it a Kconfig if another app shares the modem. */
#define SEC_TAG 4242

/* Modem key-store writes need LTE off (CFUN=0 at boot, CFUN=4 later). */
static int write_cred(enum modem_key_mgmt_cred_type type, const uint8_t *buf, size_t len)
{
	return modem_key_mgmt_write(SEC_TAG, type, buf, len);
}

static int install_ca(const uint8_t *pem, size_t len)
{
	/* cmp() returns 0 when the stored CA equals pem. */
	if (modem_key_mgmt_cmp(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, pem, len) == 0) {
		return 0;
	}
	return write_cred(MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, pem, len);
}

static int install_identity(const uint8_t *cert, size_t cert_len,
			    const uint8_t *key, size_t key_len)
{
	int rc = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);

	if (rc == 0) {
		rc = write_cred(MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT, cert, cert_len);
	}
	if (rc == 0) {
		rc = write_cred(MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT, key, key_len);
	}
	/* Back online either way; the net_if reconnects (connection persistence). */
	int rc2 = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL);

	return rc ? rc : rc2;
}

static bool has_identity(void)
{
	bool exists = false;

	(void)modem_key_mgmt_exists(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT, &exists);
	return exists;
}

/* LTE status on LED1: blinking = no IPv4 connectivity, solid = connected.
 * (The nRF9151 DK LEDs are all green, so blink vs solid is the signal.)
 */
static const struct gpio_dt_spec lte_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static atomic_t lte_up;

static void lte_led_blink(struct k_work *work)
{
	if (atomic_get(&lte_up)) {
		gpio_pin_set_dt(&lte_led, 1);
		return;
	}
	gpio_pin_toggle_dt(&lte_led);
	k_work_schedule(k_work_delayable_from_work(work), K_MSEC(250));
}
static K_WORK_DELAYABLE_DEFINE(lte_led_work, lte_led_blink);

/* net_mgmt events are 64-bit since Zephyr 5a9a39caf3b (v4.2.0), 32-bit
 * before (NCS v3.1); NET_MGMT_MAX_COMMANDS came with that change.
 */
#ifdef NET_MGMT_MAX_COMMANDS
typedef uint64_t mgmt_event_t;
#else
typedef uint32_t mgmt_event_t;
#endif

static void l4_handler(struct net_mgmt_event_callback *cb, mgmt_event_t event,
		       struct net_if *iface)
{
	atomic_set(&lte_up, event == NET_EVENT_L4_IPV4_CONNECTED);
	k_work_reschedule(&lte_led_work, K_NO_WAIT);
}
static struct net_mgmt_event_callback l4_cb;

static void lte_led_init(void)
{
	if (gpio_pin_configure_dt(&lte_led, GPIO_OUTPUT_INACTIVE)) {
		return;
	}
	net_mgmt_init_event_callback(&l4_cb, l4_handler,
				     NET_EVENT_L4_IPV4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);
	k_work_schedule(&lte_led_work, K_NO_WAIT);
}

/* Settings page (web flasher or scripts/settings-hex.py): a 4 KB page at the start of the
 * (unused) MCUboot secondary slot. The signed app image can't carry them
 * (MCUboot checks the signature on boot). Layout: "EXEINCFG", then three
 * NUL-terminated strings, each empty = keep the build's value:
 *   token      bootstrap token, used for one registration attempt and then
 *              cleared (the platform consumes a token even when the device
 *              fails to read the reply)
 *   custom id  SystemInfo custom_id (default nRF9151-DK_<device id>)
 *   host       platform host for registration (:8004) and telemetry (:8003)
 */
#define CFG_MAGIC "EXEINCFG"
#define CFG_MAGIC_LEN (sizeof(CFG_MAGIC) - 1)

static struct {
	char token[RUNTIME_ZEPHYR_TOKEN_MAX + 1];
	char custom_id[RUNTIME_ZEPHYR_CUSTOM_ID_MAX + 1];
	char host[RUNTIME_ZEPHYR_HOST_MAX + 1];
} cfg;

static const struct flash_area *cfg_area(void)
{
	const struct flash_area *fa;

	return flash_area_open(PM_MCUBOOT_SECONDARY_ID, &fa) ? NULL : fa;
}

/* Copies the next NUL-terminated string of buf[*off..len) into out. */
static int cfg_field(const char *buf, size_t len, size_t *off, char *out, size_t out_size)
{
	size_t n = strnlen(buf + *off, len - *off);

	if (n >= out_size || *off + n >= len) {
		return -EINVAL;
	}
	memcpy(out, buf + *off, n + 1);
	*off += n + 1;
	return 0;
}

/* 1 = settings loaded, 0 = no settings page, < 0 = error. */
static int read_cfg(void)
{
	const struct flash_area *fa = cfg_area();
	char buf[CFG_MAGIC_LEN + sizeof(cfg)];
	size_t off = CFG_MAGIC_LEN;

	if (fa == NULL || flash_area_read(fa, 0, buf, sizeof(buf))) {
		return -EIO;
	}
	if (memcmp(buf, CFG_MAGIC, CFG_MAGIC_LEN) != 0) {
		return 0;
	}
	if (cfg_field(buf, sizeof(buf), &off, cfg.token, sizeof(cfg.token)) ||
	    cfg_field(buf, sizeof(buf), &off, cfg.custom_id, sizeof(cfg.custom_id)) ||
	    cfg_field(buf, sizeof(buf), &off, cfg.host, sizeof(cfg.host))) {
		memset(&cfg, 0, sizeof(cfg));
		return -EINVAL;
	}
	return 1;
}

/* Rewrites the page without the token; custom id and host stay. */
static void clear_cfg_token(void)
{
	const struct flash_area *fa = cfg_area();
	uint8_t buf[ROUND_UP(CFG_MAGIC_LEN + sizeof(cfg), 4)];
	size_t off = CFG_MAGIC_LEN + 1; /* magic + empty token */

	memset(cfg.token, 0, sizeof(cfg.token));
	memset(buf, 0xff, sizeof(buf));
	memcpy(buf, CFG_MAGIC, CFG_MAGIC_LEN);
	buf[CFG_MAGIC_LEN] = '\0';
	strcpy((char *)buf + off, cfg.custom_id);
	off += strlen(cfg.custom_id) + 1;
	strcpy((char *)buf + off, cfg.host);
	off += strlen(cfg.host) + 1;
	if (fa == NULL || flash_area_erase(fa, 0, 4096) ||
	    flash_area_write(fa, 0, buf, ROUND_UP(off, 4))) {
		LOG_ERR("could not clear the token from the settings page");
	}
}

static const struct runtime_zephyr_cred_backend creds = {
	.install_ca = install_ca,
	.install_identity = install_identity,
	.has_identity = has_identity,
};

int main(void)
{
	k_thread_name_set(k_current_get(), "main");
	LOG_INF("Exein Runtime Monitor — nRF9151");

	int rc = nrf_modem_lib_init();
	if (rc) {
		LOG_ERR("nrf_modem_lib_init: %d", rc);
		return 0;
	}

	/* The CA must be in the modem before LTE comes up. */
	size_t ca_len;
	const uint8_t *ca = runtime_zephyr_ca_pem(&ca_len);

	rc = install_ca(ca, ca_len);
	if (rc) {
		LOG_ERR("CA provisioning: %d", rc);
	}

	lte_led_init();

	/* LTE via the modem's Zephyr net_if: the monitor waits for its L4 event. */
	conn_mgr_all_if_up(true);
	conn_mgr_all_if_connect(true);

	int rc_cfg = read_cfg();

	if (rc_cfg < 0) {
		LOG_WRN("settings page unreadable or invalid: %d", rc_cfg);
	}

	static struct runtime_zephyr_sysinfo info = RUNTIME_ZEPHYR_SYSINFO_INIT;
	/* Custom id: from the settings page, else "nRF9151-DK_<FICR device id>"
	 * so the workshop boards tell apart on the platform. The library keeps
	 * the pointer, hence static.
	 */
	static char custom_id[RUNTIME_ZEPHYR_CUSTOM_ID_MAX + 1];

	if (cfg.custom_id[0]) {
		strcpy(custom_id, cfg.custom_id);
	} else {
		uint8_t devid[8];
		ssize_t n = hwinfo_get_device_id(devid, sizeof(devid));
		int off = snprintk(custom_id, sizeof(custom_id), "nRF9151-DK_");

		for (ssize_t i = 0; i < n; i++) {
			off += snprintk(custom_id + off, sizeof(custom_id) - off, "%02X", devid[i]);
		}
		if (n <= 0) {
			LOG_WRN("hwinfo_get_device_id: %d", (int)n);
			custom_id[0] = '\0';
		}
	}
	if (custom_id[0]) {
		info.custom_id = custom_id;
		LOG_INF("custom id %s", custom_id);
	}

	struct runtime_zephyr_connection conn = RUNTIME_ZEPHYR_CONNECTION_INIT;

	if (cfg.host[0]) {
		conn.host = cfg.host;
		conn.registration_host = cfg.host;
	}
	LOG_INF("platform %s (registration %s)", conn.host,
		conn.registration_host && conn.registration_host[0] ? conn.registration_host : conn.host);

	/* Token: from the settings page if the board has no identity yet,
	 * else CONFIG_RUNTIME_MONITOR_BOOTSTRAP_TOKEN (empty in this repo).
	 * The library still wants a non-empty token; without an identity the
	 * registration then fails and the monitor runs offline.
	 */
	bool token_from_flash = false;

	if (cfg.token[0] && has_identity()) {
		LOG_INF("board already has an identity; clearing the unused token");
		clear_cfg_token();
	} else if (cfg.token[0]) {
		LOG_INF("registering with the token from the settings page (%u chars)",
			(unsigned int)strlen(cfg.token));
		conn.bootstrap_token = cfg.token;
		token_from_flash = true;
	}
	if (conn.bootstrap_token == NULL || conn.bootstrap_token[0] == '\0') {
		conn.bootstrap_token = "no-token";
	}
	conn.creds = &creds;
	conn.sec_tag = SEC_TAG;

	/* Blocks until LTE + SNTP + (first boot) registration are done. */
	rc = runtime_zephyr_start(&info, &conn, NULL);
	if (rc) {
		LOG_ERR("runtime_zephyr_start: %d", rc);
	}
	if (token_from_flash) {
		clear_cfg_token();
		if (has_identity()) {
			LOG_INF("registered with the platform; token cleared");
		} else {
			LOG_ERR("registration failed; token cleared (tokens are single-use): "
				"flash a new settings page with a token");
		}
	}
	workload_start();

	k_sleep(K_FOREVER);
	return 0;
}
