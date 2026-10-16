/*
 * Demo workload for the Exein Runtime Security Monitor: heap exhaustion.
 *
 * Button 2 burst-allocates past 60% of the 16 KB system heap, so the
 * HEAP_LOW policy fires and the monitor posts a heap_low finding over LTE;
 * the platform turns it into a "Heap Exhaustion" incident. The memory is
 * held for HOLD_MS, then freed, so the demo is repeatable.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "app_workload.h"

LOG_MODULE_REGISTER(workload, LOG_LEVEL_INF);

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

static const struct gpio_dt_spec btn2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct gpio_callback btn2_cb;

static void btn2_pressed(const struct device *d, struct gpio_callback *cb, uint32_t pins)
{
	k_work_submit(&heap_burst); /* k_malloc is not ISR-safe */
}

void workload_start(void)
{
	if (!gpio_is_ready_dt(&btn2)) {
		LOG_ERR("button 2 not ready");
		return;
	}
	gpio_pin_configure_dt(&btn2, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&btn2, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&btn2_cb, btn2_pressed, BIT(btn2.pin));
	gpio_add_callback(btn2.port, &btn2_cb);
	LOG_INF("Press Button 2 for a heap exhaustion (HEAP_LOW) incident");
}
