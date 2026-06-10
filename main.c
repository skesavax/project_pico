#include "picos.h"

#include "pico/stdlib.h"
#include <stdio.h>

PICOS_STACK(thread1, 128);
void thread1() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(thread2, 128);
void thread2() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(thread3, 128);
void thread3() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(thread4, 128);
void thread4() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(thread5, 128);
void thread5() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(thread6, 128);
void thread6() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(thread7, 128);
void thread7() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}

PICOS_STACK(test2, 128);
void test2() {
    // Example from: https://wiki.segger.com/Cortex-M_Fault#Illegal_Memory_Write
    int r = 0;
    volatile unsigned int *p = (unsigned int *)0x00100000;

    *p = 0x00BADA55;

    for (;;) {
    }
}

PICOS_STACK(test3, 128);
void test3() {
    volatile uint8_t i = 0;
    for (;;) {
        i++;
    }
}
PICOS_STACK(test_report, 256);
void test_report() {

    uint64_t lastTime = time_us_64();

    static uint64_t lastThreadExec[PICOS_MAX_THREADS] = {0};
    static uint64_t lastCoreCtx[PICOS_CORES] = {0};

    while (true) {

        uint64_t now = time_us_64();
        uint64_t elapsed = now - lastTime;

        if (elapsed == 0) {
            sleep_ms(1);
            continue;
        }

        lastTime = now;

        uint64_t tm = now / 1000000;

        printf("\n\nWall time %llu:%02llu:%02llu\n",
               tm / 3600,
               (tm % 3600) / 60,
               tm % 60);

        // =========================
        // CORE STATS
        // =========================
        for (uint8_t c = 0; c < PICOS_CORES; c++) {

            uint64_t ctxNow = pico_core_stats[c].contextTime;
            uint64_t ctxDelta = ctxNow - lastCoreCtx[c];
            lastCoreCtx[c] = ctxNow;

            float ctx_pct = (elapsed > 0)
                ? (100.0f * ctxDelta) / elapsed
                : 0;

            printf("CPU%d Ctx=%6.3f%%\n", c, ctx_pct);
        }

        // =========================
        // THREAD STATS
        // =========================
        for (uint32_t a = PICOS_CORES; a < PICOS_MAX_THREADS; a++) {

            picos_thread_t *t = &picos_threads[a];

            uint64_t now_exec = t->execTime;
            uint64_t prev_exec = lastThreadExec[a];

            uint64_t delta = (now_exec >= prev_exec)
                ? (now_exec - prev_exec)
                : 0;

            lastThreadExec[a] = now_exec;

            float cpu_pct = (elapsed > 0)
                ? (100.0f * delta) / elapsed
                : 0;

            if (cpu_pct > 100.0f) cpu_pct = 100.0f;

            printf("%4u %-10s state=%d cpu=%6.3f%%\n",
                   (unsigned)(a - PICOS_CORES),
                   t->name ? t->name : "null",
                   t->state,
                   cpu_pct);
        }

        sleep_ms(500);
    }
}

int main() {
    stdio_init_all();

    picos_init();

    PICOS_THREAD(thread1, 3);
    PICOS_THREAD(thread2, 3);
    PICOS_THREAD(thread3, 2);
    PICOS_THREAD(thread4, 2);
    PICOS_THREAD(thread5, 1);
    PICOS_THREAD(thread6, 1);
    PICOS_THREAD(thread7, 0);
    PICOS_THREAD(test_report, 0);
    picos_start();
}