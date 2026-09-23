#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE            4096
#define QUEUE_CAPACITY          10
#define PRODUCER_PERIOD_MS      50
#define STUCK_AFTER_ITEMS        5
#define STUCK_SLEEP_MS        3000
#define WDT_FEED_PERIOD_MS     500
#define HEALTH_CHECK_PERIOD_MS 200
#define QUEUE_WARN_PCT          75

/* ================================================================== */
/*  Producer/consumer pipeline                                        */
/* ================================================================== */

struct work_item {
    uint32_t seq;
    uint32_t timestamp_ms;
};

K_MSGQ_DEFINE(work_q, sizeof(struct work_item), QUEUE_CAPACITY, 4);

static int consumer_wdt_channel = -1;

/* ================================================================== */
/*  Task watchdog callback                                            */
/* ================================================================== */

static void wdt_callback(int channel_id, void *user_data)
{
    ARG_UNUSED(user_data);

    /* Fires when the consumer fails to feed its channel in time. */
    LOG_ERR("[WATCHDOG] channel %d starved - consumer appears stuck!", channel_id);
}

/* ================================================================== */
/*  Producer                                                          */
/* ================================================================== */

static void producer_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "producer");

    uint32_t seq = 0;

    while (1) {
        struct work_item item = {
            .seq = seq++,
            .timestamp_ms = k_uptime_get_32(),
        };

        int ret = k_msgq_put(&work_q, &item, K_NO_WAIT);
        if (ret != 0) {
            LOG_WRN("[PRODUCER] queue full, dropped item seq=%u", item.seq);
        } else {
            LOG_INF("[PRODUCER] produced seq=%u", item.seq);
        }

        k_msleep(PRODUCER_PERIOD_MS);
    }
}

/* ================================================================== */
/*  Consumer                                                           */
/* ================================================================== */

static void consumer_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "consumer");

    uint32_t processed = 0;

    while (1) {
        struct work_item item;

        int ret = k_msgq_get(&work_q, &item, K_MSEC(WDT_FEED_PERIOD_MS));
        if (ret == 0) {
            LOG_INF("[CONSUMER] processed seq=%u latency=%ums",
                    item.seq, k_uptime_get_32() - item.timestamp_ms);
            processed++;
        }

        /* Simulate a stuck consumer once, long enough to starve the watchdog. */
        if (processed == STUCK_AFTER_ITEMS) {
            LOG_WRN("[CONSUMER] simulating stall for %dms", STUCK_SLEEP_MS);
            k_msleep(STUCK_SLEEP_MS);
            processed++;
        }

        task_wdt_feed(consumer_wdt_channel);
    }
}

/* ================================================================== */
/*  Health-check thread                                                */
/* ================================================================== */

static void health_check_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "health_check");

    while (1) {
        uint32_t used = k_msgq_num_used_get(&work_q);
        uint32_t pct = (used * 100) / QUEUE_CAPACITY;

        if (pct >= QUEUE_WARN_PCT) {
            LOG_WRN("[HEALTH] queue fill %u%% (%u/%u) - approaching capacity",
                    pct, used, QUEUE_CAPACITY);
        } else {
            LOG_DBG("[HEALTH] queue fill %u%% (%u/%u)", pct, used, QUEUE_CAPACITY);
        }

        k_msleep(HEALTH_CHECK_PERIOD_MS);
    }
}

/* ================================================================== */
/*  Threads                                                           */
/* ================================================================== */

K_THREAD_DEFINE(producer_thread, STACK_SIZE, producer_thread_fn,
                NULL, NULL, NULL, 5, 0, 0);

K_THREAD_DEFINE(consumer_thread, STACK_SIZE, consumer_thread_fn,
                NULL, NULL, NULL, 5, 0, 0);

K_THREAD_DEFINE(health_check_thread, STACK_SIZE, health_check_thread_fn,
                NULL, NULL, NULL, 6, 0, 0);

/* ================================================================== */
/*  Main                                                              */
/* ================================================================== */

int main(void)
{
    LOG_INF("=== Producer/Consumer + Task Watchdog + Health Check ===");

    task_wdt_init(NULL);

    /* Consumer must feed this channel at least once per WDT_FEED_PERIOD_MS * 2. */
    consumer_wdt_channel = task_wdt_add(WDT_FEED_PERIOD_MS * 2, wdt_callback, NULL);
    if (consumer_wdt_channel < 0) {
        LOG_ERR("failed to register watchdog channel: %d", consumer_wdt_channel);
    }

    return 0;
}
