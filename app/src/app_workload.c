/*
 * Demo workload for the Exein Runtime Security Monitor: one button per
 * incident type.
 *
 *   Button 1  5 identical LOG_ERR            -> Log Event (one incident)
 *   Button 2  burst past 60% of the heap     -> Heap Exhaustion
 *   Button 3  thread "stack_hog" at ~90%     -> Free Stack Below Critical Threshold
 *   Button 4  slow sustained heap growth     -> Memory Leak
 *
 * Every trigger cleans up after itself (memory freed, thread exits), so the
 * demo is repeatable.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "app_workload.h"

LOG_MODULE_REGISTER(workload, LOG_LEVEL_INF);

/* ── Button 1: Log Event ─────────────────────────────────────────── */

/* Identical errors in one reporting interval become a single incident. */
#define LOG_REPEAT 5

static void log_burst_work(struct k_work *w)
{
	ARG_UNUSED(w);
	for (int i = 0; i < LOG_REPEAT; i++) {
		LOG_ERR("sensor read failed: -5");
	}
}
static K_WORK_DEFINE(log_burst, log_burst_work);

/* ── Button 2: Heap Exhaustion ───────────────────────────────────── */

/* The analyzer checks HEAP_LOW once per CONFIG_ANALYZER_PERIOD_MS, after
 * replaying the alloc/free events of that period: memory freed within the
 * same period never shows up. Holding it for two periods always does.
 */
#define HOLD_MS (2 * CONFIG_ANALYZER_PERIOD_MS)

static void *held[24];
static int held_n;

static void heap_free_work(struct k_work *w)
{
	ARG_UNUSED(w);
	while (held_n > 0) {
		k_free(held[--held_n]);
	}
}
static K_WORK_DELAYABLE_DEFINE(heap_free, heap_free_work);

static void heap_burst_work(struct k_work *w)
{
	ARG_UNUSED(w);
	if (held_n > 0) {
		return; /* previous burst still held */
	}
	while (held_n < ARRAY_SIZE(held) && (held[held_n] = k_malloc(512)) != NULL) {
		held_n++;
	}
	LOG_WRN("BTN2: burst-allocated %d x 512 B, freeing in %d ms (expect HEAP_LOW)",
		held_n, HOLD_MS);
	k_work_schedule(&heap_free, K_MSEC(HOLD_MS));
}
static K_WORK_DEFINE(heap_burst, heap_burst_work);

/* ── Button 3: Stack Overflow Risk ───────────────────────────────── */

/* A dedicated thread recurses until STACK_HOG_PCT of its stack is used, then
 * stays alive for STACK_HOG_HOLD_S so the collector (THREAD_COLLECT_PERIOD_MS)
 * samples it, then exits. Must stay above CONFIG_THREAD_STACK_WARN_PCT and
 * below 100: a real overflow halts the board. Its own thread, because the
 * high-water mark of the system workqueue would stay high forever.
 */
#define STACK_HOG_SIZE   1024
#define STACK_HOG_PCT    90
#define STACK_HOG_HOLD_S 20

K_THREAD_STACK_DEFINE(hog_stack, STACK_HOG_SIZE);
static struct k_thread hog_thread;
static atomic_t hog_busy;

static size_t stack_used(void)
{
	const struct k_thread *t = k_current_get();
	volatile char here;

	return t->stack_info.start + t->stack_info.size - (uintptr_t)&here;
}

__attribute__((noinline))
static void deep_recurse(size_t target)
{
	volatile char pad[64];

	pad[0] = 0;
	if (stack_used() < target) {
		deep_recurse(target);
	}
	pad[63] = pad[0];
}

static void stack_hog_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* One frame of headroom: the check runs before the next 64 B pad. */
	deep_recurse(k_current_get()->stack_info.size * STACK_HOG_PCT / 100 - 96);
	LOG_WRN("BTN3: stack_hog near its stack limit for %d s (expect STACK_LOW)",
		STACK_HOG_HOLD_S);
	k_sleep(K_SECONDS(STACK_HOG_HOLD_S));
	atomic_clear(&hog_busy);
}

static void stack_hog_work(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!atomic_cas(&hog_busy, 0, 1)) {
		return; /* previous stack_hog still running */
	}
	k_thread_create(&hog_thread, hog_stack, K_THREAD_STACK_SIZEOF(hog_stack),
			stack_hog_entry, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&hog_thread, "stack_hog");
}
static K_WORK_DEFINE(stack_hog, stack_hog_work);

/* ── Button 4: Memory Leak ───────────────────────────────────────── */

/* One unfreed block every LEAK_PACE_MS, so every leak window
 * (CONFIG_MEM_LEAK_WINDOW_EVENTS heap events) grows. LEAK_N blocks span more
 * than the 8 windows the policy needs and exceed MEM_LEAK_MIN_NET_GROWTH
 * (4096 B), while staying below the 60% HEAP_LOW threshold of the 16 KB heap.
 * Freed again after HOLD_MS.
 */
#define LEAK_N       160
#define LEAK_SIZE    32
#define LEAK_PACE_MS 100

static void *leaked[LEAK_N];
static int leak_n;
static bool leak_busy;

static void leak_free_work(struct k_work *w)
{
	ARG_UNUSED(w);
	while (leak_n > 0) {
		k_free(leaked[--leak_n]);
	}
	leak_busy = false;
}
static K_WORK_DELAYABLE_DEFINE(leak_free, leak_free_work);

static void leak_step_work(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(leak_step, leak_step_work);

static void leak_step_work(struct k_work *w)
{
	ARG_UNUSED(w);
	if (leak_n < LEAK_N && (leaked[leak_n] = k_malloc(LEAK_SIZE)) != NULL) {
		leak_n++;
		k_work_schedule(&leak_step, K_MSEC(LEAK_PACE_MS));
		return;
	}
	LOG_WRN("BTN4: leaked %d x %d B, freeing in %d ms (expect MEM_LEAK)",
		leak_n, LEAK_SIZE, HOLD_MS);
	k_work_schedule(&leak_free, K_MSEC(HOLD_MS));
}

static void leak_start_work(struct k_work *w)
{
	ARG_UNUSED(w);
	if (leak_busy) {
		return; /* previous leak still running or held */
	}
	leak_busy = true;
	LOG_WRN("BTN4: leaking %d x %d B over %d s", LEAK_N, LEAK_SIZE,
		LEAK_N * LEAK_PACE_MS / 1000);
	k_work_schedule(&leak_step, K_NO_WAIT);
}
static K_WORK_DEFINE(leak_start, leak_start_work);

/* ── Buttons ─────────────────────────────────────────────────────── */

static const struct gpio_dt_spec btns[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),
};
/* k_malloc and thread creation are not ISR work: hand off to the workqueue. */
static struct k_work *const actions[] = { &log_burst, &heap_burst, &stack_hog, &leak_start };
static struct gpio_callback btn_cb[ARRAY_SIZE(btns)];

static void btn_pressed(const struct device *d, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(d);
	ARG_UNUSED(pins);
	k_work_submit(actions[cb - btn_cb]);
}

void workload_start(void)
{
	for (int i = 0; i < ARRAY_SIZE(btns); i++) {
		if (!gpio_is_ready_dt(&btns[i])) {
			LOG_ERR("button %d not ready", i + 1);
			continue;
		}
		gpio_pin_configure_dt(&btns[i], GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&btns[i], GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&btn_cb[i], btn_pressed, BIT(btns[i].pin));
		gpio_add_callback(btns[i].port, &btn_cb[i]);
	}
	LOG_INF("Buttons: 1 Log Event, 2 Heap Exhaustion, 3 Stack Overflow Risk, 4 Memory Leak");
}
