#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE      1024
#define PRIO            5
#define INCREMENTS      1000000   /* each thread increments this many times */

/* Shared state - intentionally unprotected */
static volatile uint32_t counter;

static struct k_sem done_sem;

static K_MUTEX_DEFINE(counter_mtx);

void worker_fn(void *p1, void *p2, void *p3)
{
    const char *name = k_thread_name_get(k_current_get());

    for (int i = 0; i < INCREMENTS; i++) {

        k_mutex_lock(&counter_mtx, K_FOREVER);
        counter++;
        k_mutex_unlock(&counter_mtx);
    }

    LOG_INF("[%s] finished", name);
    k_sem_give(&done_sem);
}

K_THREAD_DEFINE(worker_a, STACK_SIZE, worker_fn, NULL, NULL, NULL,
                PRIO, 0, 0);
K_THREAD_DEFINE(worker_b, STACK_SIZE, worker_fn, NULL, NULL, NULL,
                PRIO, 0, 0);

int main(void)
{
    k_sem_init(&done_sem, 0, 2);

    LOG_INF("=== L2 Demo 1: Shared Counter Corruption ===");
    LOG_INF("Expected final value: %d", INCREMENTS * 2);

    /* Wait for both workers to complete */
    k_sem_take(&done_sem, K_FOREVER);
    k_sem_take(&done_sem, K_FOREVER);

    LOG_INF("Actual  final value: %u", counter);

    if (counter == INCREMENTS * 2) {
        LOG_WRN("No race this run - timing-dependent, try again");
    } else {
        LOG_ERR("Race condition confirmed: lost %d updates",
                (INCREMENTS * 2) - counter);
    }

    return 0;
}
