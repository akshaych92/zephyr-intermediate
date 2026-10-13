#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(homework, LOG_LEVEL_INF);

#define STACK_SIZE            2048
#define CONTROL_PRIORITY         7
#define CORRECTED_PRIORITY       3
#define MAINTENANCE_PRIORITY     4
#define EVENT_PERIOD_MS        250
#define MAINTENANCE_LOAD_US  45000
#define RESPONSE_DEADLINE_US 10000
#define MEASUREMENT_SAMPLES     20

struct control_event {
    uint32_t seq;
    uint32_t ready_cycles;
};

K_MSGQ_DEFINE(control_queue, sizeof(struct control_event), 4, 4);
K_SEM_DEFINE(maintenance_start, 0, 1);

static atomic_t released;
static atomic_t processed;
static atomic_t deadline_misses;
static atomic_t dropped;
static atomic_t remaining;
static atomic_t max_response_us;

/* ================================================================== */
/*  Timer expiry: creates one control event                           */
/* ================================================================== */

static void event_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    struct control_event event = {
        .seq = (uint32_t)atomic_inc(&released) + 1U,
        .ready_cycles = k_cycle_get_32(),
    };

    sys_trace_named_event("control_release", event.seq, RESPONSE_DEADLINE_US);
    if (event.seq == MEASUREMENT_SAMPLES) {
        k_timer_stop(timer);
    }

    /* Timer expiry runs in interrupt context, so never wait here. */
    int ret = k_msgq_put(&control_queue, &event, K_NO_WAIT);

    if (ret != 0) {
        atomic_inc(&dropped);
        atomic_inc(&deadline_misses);
        sys_trace_named_event("control_drop", event.seq, (uint32_t)ret);
        atomic_dec(&remaining);
        return;
    }

    /* Both threads become ready when the timer interrupt returns. */
    k_sem_give(&maintenance_start);

}

K_TIMER_DEFINE(event_timer, event_timer_expiry, NULL);

/* ================================================================== */
/*  Control thread                                                   */
/* ================================================================== */

static void control_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (true) {
        struct control_event event;
        int ret = k_msgq_get(&control_queue, &event, K_FOREVER);

        if (ret != 0) {
            LOG_ERR("[CONTROL] receive failed: %d", ret);
            continue;
        }

        uint32_t elapsed_cycles = k_cycle_get_32() - event.ready_cycles;
        uint32_t response_us = (uint32_t)k_cyc_to_us_ceil64(elapsed_cycles);
        bool missed = response_us > RESPONSE_DEADLINE_US;

        sys_trace_named_event("control_start", event.seq, response_us);
        if (response_us > (uint32_t)atomic_get(&max_response_us)) {
            atomic_set(&max_response_us, response_us);
        }
        if (missed) {
            atomic_inc(&deadline_misses);
            LOG_WRN_RATELIMIT("[CONTROL] deadline miss seq=%u response_us=%u",
                              event.seq, response_us);
        }
        LOG_INF("[CONTROL] seq=%u response_us=%u deadline_us=%u missed=%u",
                event.seq, response_us, RESPONSE_DEADLINE_US, (unsigned int)missed);
        sys_trace_named_event("control_done", event.seq,
                             (uint32_t)atomic_get(&deadline_misses));
        atomic_inc(&processed);
        atomic_dec(&remaining);
    }
}

/* ================================================================== */
/*  Background maintenance thread                                    */
/* ================================================================== */

static void maintenance_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&maintenance_start, K_FOREVER);

        /* This work is important, but it has no short deadline. */
        k_busy_wait(MAINTENANCE_LOAD_US);
    }
}

K_THREAD_DEFINE(control, STACK_SIZE, control_fn,
                NULL, NULL, NULL, CONTROL_PRIORITY, 0, 0);

K_THREAD_DEFINE(maintenance, STACK_SIZE, maintenance_fn,
                NULL, NULL, NULL, MAINTENANCE_PRIORITY, 0, 0);

static int start_measurement(const struct shell *shell, int priority)
{
    if (atomic_get(&remaining) != 0) {
        shell_error(shell, "Measurement still running; use measure stats");
        return -EBUSY;
    }

    atomic_clear(&released);
    atomic_clear(&processed);
    atomic_clear(&deadline_misses);
    atomic_clear(&dropped);
    atomic_clear(&max_response_us);
    k_thread_priority_set(control, priority);
    atomic_set(&remaining, MEASUREMENT_SAMPLES);
    sys_trace_named_event("measure_begin", (uint32_t)priority, MEASUREMENT_SAMPLES);
    shell_print(shell, "samples=%u period_ms=%u deadline_us=%u control_prio=%d maintenance_prio=%d",
                MEASUREMENT_SAMPLES, EVENT_PERIOD_MS, RESPONSE_DEADLINE_US,
                priority, MAINTENANCE_PRIORITY);
    k_timer_start(&event_timer, K_MSEC(500), K_MSEC(EVENT_PERIOD_MS));
    return 0;
}

static int cmd_baseline(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    return start_measurement(shell, CONTROL_PRIORITY);
}

static int cmd_corrected(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    return start_measurement(shell, CORRECTED_PRIORITY);
}

static int cmd_stats(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(shell, "released=%ld processed=%ld dropped=%ld misses=%ld max_response_us=%ld remaining=%ld",
                (long)atomic_get(&released), (long)atomic_get(&processed),
                (long)atomic_get(&dropped), (long)atomic_get(&deadline_misses),
                (long)atomic_get(&max_response_us), (long)atomic_get(&remaining));
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(measure_commands,
    SHELL_CMD_ARG(baseline, NULL, "Run 20 events with control priority 7", cmd_baseline, 1, 0),
    SHELL_CMD_ARG(corrected, NULL, "Run 20 events with control priority 3", cmd_corrected, 1, 0),
    SHELL_CMD_ARG(stats, NULL, "Show deadline counters", cmd_stats, 1, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(measure, &measure_commands, "Response-time measurement", NULL);

int main(void)
{
    LOG_INF("=== L6 Homework: Runtime Investigation ===");
    LOG_INF("Control work must start within %u us of timer release", RESPONSE_DEADLINE_US);
    LOG_INF("Use measure baseline, measure stats, and measure corrected");

    return 0;
}