#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE 1024

#define PRIO_LOW    7
#define PRIO_MED    5
#define PRIO_HIGH   3
#define PRIO_COOP  -1

void t_low_fun(void *p1, void *p2, void *p3)
{
    while (1) {
        LOG_INF("T_LOW running");
        k_msleep(300);
    }
}

void t_med_fun(void *p1, void *p2, void *p3)
{
    while (1) {
        LOG_INF("T_MED running");
        k_msleep(200);
    }
}

void t_high_fun(void *p1, void *p2, void *p3)
{
    while (1) {
        LOG_INF("T_HIGH running");
        k_msleep(100);
    }
}

void t_coop_fun(void *p1, void *p2, void *p3)
{
    LOG_INF("[COOP] starting - will run 5 steps without yielding");
    for(int loop=0; loop<5; loop++) {
        LOG_INF("T_COOP running");
    }
    LOG_INF("T_COOP finished");
    k_yield();

}

K_THREAD_DEFINE(thread_low, STACK_SIZE, t_low_fun,
                NULL, NULL, NULL, PRIO_LOW, 0, 0);
K_THREAD_DEFINE(thread_med, STACK_SIZE, t_med_fun,
                NULL, NULL, NULL, PRIO_MED, 0, 0);
K_THREAD_DEFINE(thread_high, STACK_SIZE, t_high_fun,
                NULL, NULL, NULL, PRIO_HIGH, 0, 0);
K_THREAD_DEFINE(thread_coop, STACK_SIZE, t_coop_fun,
                NULL, NULL, NULL, PRIO_COOP, 0, 0);

int main(void)
{
    return 0;
}

