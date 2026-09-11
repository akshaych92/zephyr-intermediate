#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE       1024
#define PRIO             5
#define SENSOR_PERIOD_MS 100
#define RUN_SECONDS      3

/* Bonus debounce demo: 5 events fired within this window collapse to 1 run */
#define BURST_EVENTS      5
#define BURST_GAP_MS      4   /* 5 events * 4 ms = 20 ms burst window       */
#define DEBOUNCE_MS       20

/* Shared event flag between the "ISR-like" producer and the work handler */
static volatile bool data_ready;
static struct k_sem sensor_sem;
static uint32_t handler_runs;

/* --- k_work handler: only runs when (re)scheduled by a real event ------- */
static void event_work_handler(struct k_work *work)
{
    handler_runs++;

    if (data_ready) {
        data_ready = false;
        LOG_INF("event_work: handled event (handler run #%u)", handler_runs);
    } else {
        LOG_INF("event_work: debounced burst settled (handler run #%u)",
                handler_runs);
    }
}

static struct k_work_delayable event_work;

/* --- Producer: fires a "real event" every 100 ms ------------------------ */
static void sensor_sim_fn(void *p1, void *p2, void *p3)
{
    for (;;) {
        k_sleep(K_MSEC(SENSOR_PERIOD_MS));
        data_ready = true;
        k_sem_give(&sensor_sem);
        LOG_INF("sensor_sim: event raised");
        /* Submit immediately: no polling, the handler wakes on the event */
        k_work_reschedule(&event_work, K_NO_WAIT);
    }
}

/* --- Bonus: burst of 5 events inside 20 ms, debounced via reschedule --- */
static void burst_test_fn(void *p1, void *p2, void *p3)
{
    k_sleep(K_SECONDS(1));

    LOG_INF("burst_test: firing %d events %d ms apart (debounce=%d ms)",
            BURST_EVENTS, BURST_GAP_MS, DEBOUNCE_MS);

    for (int i = 0; i < BURST_EVENTS; i++) {
        LOG_INF("burst_test: raw event %d/%d", i + 1, BURST_EVENTS);
        /* Each new event pushes the deadline out, coalescing the burst */
        k_work_reschedule(&event_work, K_MSEC(DEBOUNCE_MS));
        k_sleep(K_MSEC(BURST_GAP_MS));
    }
}

K_THREAD_DEFINE(sensor_sim, STACK_SIZE, sensor_sim_fn, NULL, NULL, NULL,
                PRIO, 0, 0);
K_THREAD_DEFINE(burst_test, STACK_SIZE, burst_test_fn, NULL, NULL, NULL,
                PRIO, 0, 0);

int main(void)
{
    k_sem_init(&sensor_sem, 0, 1);
    k_work_init_delayable(&event_work, event_work_handler);

    LOG_INF("=== k_work version: sensor_sim (100 ms) drives event_work directly ===");
    LOG_INF("No polling thread: the handler only fires when work is (re)scheduled");

    return 0;
}
