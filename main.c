#include "picos.h"

#include "pico/stdlib.h"
#include <stdio.h>

PICOS_STACK(test1, 128);
void test1() {
    volatile uint32_t sum = 0;
    while (true) {
        for (uint32_t i = 0; i < 50000; i++)
            sum += i;
        uint8_t core = get_core_num();
        picos_thread_t *current = picos_current[core];
        if (current && current->pid >= PICOS_CORES){
            picos_log_thread_run(core, current->priority, current->pid, time_us_64());
        }
        delayms(100);
    }
}

PICOS_STACK(test1x, 128);
void test1x() {
    for (;;) {
        uint8_t core = get_core_num();
        picos_thread_t *current = picos_current[core];
        if (current && current->pid >= PICOS_CORES){
            ;//picos_log_thread_run(core, current->priority, current->pid, time_us_64());
        }
        //printf("thread-1\n\r");
        delayms(200);
    }
}

PICOS_STACK(test2, 128);
void test2() {
    volatile uint32_t sum = 0;
    while (true) {
        for (uint32_t i = 0; i < 100000; i++)
            sum += i;
        uint8_t core = get_core_num();
        picos_thread_t *current = picos_current[core];
        if (current && current->pid >= PICOS_CORES){
            picos_log_thread_run(core, current->priority, current->pid, time_us_64());
        }
        delayms(300);
    }
}
/*
PICOS_STACK(test2, 128);
void test2() {
    volatile uint32_t sum = 0;
    while (true) {
        uint8_t core = get_core_num();
        picos_thread_t *current = picos_current[core];
        if (current && current->pid >= PICOS_CORES){
            ;//picos_log_thread_run(core, current->priority, current->pid, time_us_64());
        }
        //printf("thread-1\n\r");
        delayms(5000);
    }
}
    */
/*
PICOS_STACK(test3, 128);
void test3() {
    volatile uint32_t sum = 0;
    static uint32_t lastOverflowCount = 0;
    static uint32_t lastUnderflowCount = 0;
    while (true) {
        for (uint32_t i = 0; i < 200000; i++)
            sum += i;
                picos_run_log_entry_t log_entry;
        bool has_log_entries = false;
        while (picos_log_pop(&log_entry)){
            has_log_entries = true;
            uint8_t *p = (uint8_t *)&log_entry;
            uint32_t lo = (uint32_t)log_entry.timestamp_us;
            uint32_t hi = (uint32_t)(log_entry.timestamp_us >> 32);

            printf("ts_hi=%u ts_lo=%u\n", hi, lo);

            for (int i = 0; i < sizeof(log_entry); i++) {
                printf("%02X ", p[i]);
            }
            printf("\n");
        }

        if (!has_log_entries){
            picos_log_note_underflow();
        }
        uint32_t overflowcount = picos_log_get_overflow_count();
        uint32_t underflowcount = picos_log_get_underflow_count();
        if (overflowcount != lastOverflowCount){
            printf("LOG ERROR: overflow detcted (total=%u, new=%u)\n",
                    overflowcount,
                    overflowcount - lastOverflowCount);
            lastOverflowCount = overflowcount;
        }
        if (underflowcount != lastUnderflowCount){
            printf("LOG ERROR: overflow detcted (total=%u, new=%u)\n",
                    underflowcount,
                    underflowcount - lastUnderflowCount);
            lastUnderflowCount = underflowcount;
        }
        delayms(300);
    }
}
*/

PICOS_STACK(test3, 128);
void test3() {
    volatile uint32_t sum = 0;
    while (true) {
        uint8_t core = get_core_num();
        picos_thread_t *current = picos_current[core];
        if (current && current->pid >= PICOS_CORES){
            picos_log_thread_run(core, current->priority, current->pid, time_us_64());
        }
        //printf("thread-1\n\r");
        delayms(500);
    }
}

PICOS_STACK(test_report, 1024);

void test_report() {

    uint64_t lastTime = time_us_64();

    static uint64_t lastThreadExec[PICOS_MAX_THREADS] = {0};
    static uint64_t lastCoreCtx[PICOS_CORES] = {0};
        static uint32_t lastOverflowCount = 0;
    static uint32_t lastUnderflowCount = 0;


    while (true) {
#if 0
        uint64_t now = time_us_64();
        uint64_t elapsed = now - lastTime;

        if (elapsed == 0) {
            sleep_ms(1);
            continue;
        }

        lastTime = now;

        /*elapsed time wall clock*/
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
#endif
                picos_run_log_entry_t log_entry;
        bool has_log_entries = false;
        while (picos_log_pop(&log_entry)){
            has_log_entries = true;
            uint8_t *p = (uint8_t *)&log_entry;
            
            for (int i = 0; i < sizeof(log_entry); i++) {
                printf("%02X ", p[i]);
            }
            printf("\n");
        }

        if (!has_log_entries){
            picos_log_note_underflow();
        }
        uint32_t overflowcount = picos_log_get_overflow_count();
        uint32_t underflowcount = picos_log_get_underflow_count();
        if (overflowcount != lastOverflowCount){
            printf("LOG ERROR: overflow detcted (total=%u, new=%u)\n",
                    overflowcount,
                    overflowcount - lastOverflowCount);
            lastOverflowCount = overflowcount;
        }
        if (underflowcount != lastUnderflowCount){
            printf("LOG ERROR: overflow detcted (total=%u, new=%u)\n",
                    underflowcount,
                    underflowcount - lastUnderflowCount);
            lastUnderflowCount = underflowcount;
        }


        sleep_ms(1000);
    }
}

int main() {
    stdio_init_all();

    picos_init();

    PICOS_THREAD(test1, 100);
    PICOS_THREAD(test2, 200);
    PICOS_THREAD(test3, 300);
    PICOS_THREAD(test_report, 50);

    picos_start();
}