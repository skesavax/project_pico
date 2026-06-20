#include "picos.h"
#include <stdio.h>
#define PICOS_IDLE_STACK_SIZE 100
// Define the declared variables from the header
picos_thread_t picos_threads[PICOS_MAX_THREADS];
picos_thread_t *picos_current[PICOS_CORES];
pico_core_stats_t pico_core_stats[PICOS_CORES];
#define PICOS_LOG_BUFFER_SIZE 6400
static picos_run_log_entry_t picos_log_buffer[PICOS_CORES][PICOS_LOG_BUFFER_SIZE];
static volatile uint32_t picos_log_head[PICOS_CORES] = {0};
static volatile uint32_t picos_log_tail[PICOS_CORES] = {0};
static uint32_t picos_log_overflow_count[PICOS_CORES] = {0};
static uint32_t picos_log_underflow_count = 0;
static uint8_t picos_log_rr_core = 0;

// Declare functions that will be later defined
void picos_setup_idle();
void picos_suicide();
void picos_scheduler_main();

// Functions from our assembler code
void picos_exec_stack(uint32_t sp);
void picos_set_psp(uint32_t sp, uint32_t ctrl);

void picos_init() { picos_setup_idle(); }

uint8_t getPID() {
  uint8_t pid;
  asm("cpsid i");
  uint32_t cpu = get_core_num();
  pid = picos_current[cpu]->pid;
  asm("cpsie i");
  return pid;
}

void yield() {
  picos_thread_t *t = &picos_threads[getPID()];
  t->yielded = true;
  *(volatile uint32_t *)(0xe0000000|M0PLUS_ICSR_OFFSET) = (1L<<26); // SysTick pending
  // Wait for context switch
  __asm volatile ("isb");
}

uint64_t getTimeUs() {
  uint64_t tm = *(volatile uint32_t*)(TIMER_BASE + 0x0c);
  uint64_t htm = *(volatile uint32_t*)(TIMER_BASE + 0x08);
  return (htm<<32)|tm;
}

// Delay thread the specified number of microseconds.
// Guaranteed to be at least us delay.
void delayus(uint32_t us) {
  uint8_t pid = getPID();
  picos_threads[pid].waitExpires = getTimeUs() + us;
  picos_threads[pid].state = PICOS_WAIT;
  yield();
}

void picos_log_thread_run(uint8_t core, uint8_t priority, picos_pid thread_id,
                            uint64_t timestamp_us){
    if (thread_id < PICOS_CORES || core >= PICOS_CORES){
        return;
    }
    uint32_t head = picos_log_head[core];
    uint32_t next_head = (head + 1U) % PICOS_LOG_BUFFER_SIZE;
    if (next_head == picos_log_tail[core]){
        picos_log_overflow_count[core]++;
        return;
    }
    picos_log_buffer[core][head].core = core;
    picos_log_buffer[core][head].priority = priority;
    picos_log_buffer[core][head].thread_id = thread_id;
    picos_log_buffer[core][head].timestamp_us = timestamp_us;
    picos_log_head[core] = next_head;
}

bool picos_log_pop(picos_run_log_entry_t *entry){
    if (entry == NULL){
        return false;
    }
    uint8_t start_core = picos_log_rr_core;
    for (uint8_t i=0;i<PICOS_CORES;i++){
        uint8_t core = (uint8_t)((start_core+i)%PICOS_CORES);
        uint32_t tail = picos_log_tail[core];
        uint32_t head = picos_log_head[core];
        if (head == tail){
            continue;
        }
        *entry = picos_log_buffer[core][tail];
        picos_log_tail[core] = (tail+1U)%PICOS_LOG_BUFFER_SIZE;
        picos_log_rr_core = (uint8_t)((core+1U)%PICOS_CORES);
        return true;
    }
    return false;
}

uint32_t picos_log_get_overflow_count(void){
    uint32_t count = 0;
    for (uint8_t core = 0; core < PICOS_CORES; core++){
        count += picos_log_overflow_count[core];
    }
    return count;
}

uint32_t picos_log_get_underflow_count(void){
    return picos_log_underflow_count;
}

void picos_log_note_underflow(void){
    picos_log_underflow_count++;
}

picos_pid picos_exec(picos_thread_func func, picos_thread_stack_t *s, uint8_t priority) {
    picos_pid thread_slot;

    spin_lock_blocking(PICOS_SCHEDULE_SPINLOCK);

    // find free slot
    for (thread_slot = PICOS_CORES; thread_slot < PICOS_MAX_THREADS;
         thread_slot++) {
        if (picos_threads[thread_slot].state == PICOS_UNKNOWN)
            break;
    }

    // check if we found a free slot
    if (thread_slot > PICOS_MAX_THREADS - 1) {
        spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);
        return PICOS_INVALID_PID;
    }

    // initialize free slot
    picos_thread_t *slot = &picos_threads[thread_slot];
    slot->pid = thread_slot;
    slot->cpu = 0xFF;
    slot->priority = priority;
    // calculate given address to a stack address (switch direction)
    slot->sp = (uint32_t)(s->data + s->size - 16);
    s->data[s->size - 1] =
        0x01000000; // This will set the T-Bit in the EPSR Part of the xPSR
                    // Register. As mentioned in the Armv6m Datasheet, the
                    // architecture only supports thumb instruction mode. So
                    // we need to maintain this bit always wit hthe value 1.
    s->data[s->size - 2] = (uint32_t)func; // The PC position to start exectuing
    s->data[s->size - 3] =
        (uint32_t)&picos_suicide; // if the actual thread function should exit,
                                  // this is the function to call next.
    slot->state = PICOS_RUNNING;

    spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);

    return thread_slot;
}

void picos_start() {
    for (int i = 0; i < PICOS_CORES; i++) {
        pico_core_stats[i].lastContextTime = time_us_64();
        pico_core_stats[i].contextTime = 0;
    }
    // launch scheduler on second core
    multicore_launch_core1(picos_scheduler_main);
    // launch scheduler on this core
    picos_scheduler_main();

    // make sure we never return to main
    for (;;)
        ;
}

void isr_systick() {
    // Trigger the in assembly defined isr_pendsv via interrupt
    *(volatile uint32_t *)(0xe0000000 | M0PLUS_ICSR_OFFSET) = (1L << 28);
}

void isr_hardfault() {
    //uint8_t cpu = *(uint32_t *)(SIO_BASE);
    uint32_t cpu = get_core_num();

    // Update state to hardfault
    picos_thread_t *current = picos_current[cpu];
    current->state = PICOS_HARDFAULT;

    // Trigger scheduling routine (will store context)
    isr_systick();

    for (;;)
        ;
}

void picos_schedule() {

    uint32_t cpu = get_core_num();   // IMPORTANT FIX (do NOT use SIO_BASE)

    picos_thread_t *current = picos_current[cpu];
    pico_core_stats_t *stats = &pico_core_stats[cpu];

    uint64_t now = time_us_64();
    uint8_t cpriority = 255;
    current->yielded = false; // Yield waits for this to reset

    // =========================
    // 1. ACCOUNT CURRENT THREAD TIME
    // =========================
    if (current) {

        uint64_t last = current->lastStartTime;

        if (last != 0 && now >= last) {
            current->execTime += (now - last);
        }
    }

    // =========================
    // 2. UPDATE CONTEXT SWITCH STATS
    // =========================
    if (stats->lastContextTime != 0) {
        stats->contextTime += (now - stats->lastContextTime);
    }

    stats->lastContextTime = now;

    // =========================
    // 3. PICK NEXT THREAD
    // =========================
    spin_lock_blocking(PICOS_SCHEDULE_SPINLOCK);

    picos_thread_t *next = NULL;

    for (picos_pid i = PICOS_CORES; i < PICOS_MAX_THREADS; i++) {
        picos_thread_t *t =
            &picos_threads[(((current->pid - PICOS_CORES) + i) %
            PICOS_USER_THREADS) + PICOS_CORES];
        if (t->cpu != 0xff && t->cpu != cpu) continue;

        if (t->state == PICOS_WAIT && t->waitExpires <= now) {
             t->state = PICOS_RUNNING;
        }
        if (t->state == PICOS_RUNNING && t->priority < cpriority){
            cpriority = t->priority;
        }
    }

    for (picos_pid i = PICOS_CORES; i < PICOS_MAX_THREADS; i++) {

        picos_thread_t *t =
            &picos_threads[(((current->pid - PICOS_CORES) + i) %
            PICOS_USER_THREADS) + PICOS_CORES];

        if (t->state == PICOS_RUNNING &&
           (t->cpu == cpu || t->cpu == 0xFF) && 
           (t->priority == cpriority)) {

            next = t;

            if (t->cpu == 0xFF) {
                t->cpu = cpu;
            }

            break;
        }
    }

    if (!next) {
        next = &picos_threads[cpu]; // idle thread
    }

    picos_current[cpu] = next;

    spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);

    // =========================
    // 4. SET START TIME FOR NEW THREAD
    // =========================
    next->lastStartTime = time_us_64();
/*
    printf("CPU%d current=%d next=%d prio=%d\n",
       cpu,
       current->pid,
       next->pid,
       next->priority);
*/

}

// idle structure
PICOS_STACK(idle0, PICOS_IDLE_STACK_SIZE)
PICOS_STACK(idle1, PICOS_IDLE_STACK_SIZE)
void picos_idle() {
    for (;;)
        asm("wfi"); // wait for the next interrupt
}

static picos_thread_stack_t *picos_idle_stack[PICOS_CORES] = {
    &picos_stack_idle0, &picos_stack_idle1};

void picos_setup_idle() {
    for (uint8_t i = 0; i < PICOS_CORES; i++) {
        picos_thread_t *t = &picos_threads[i];
        picos_thread_stack_t *s = picos_idle_stack[i];
        t->pid = i;
        t->cpu = i;
        t->priority = 255;
        t->state = PICOS_RUNNING;

        // Setting the correct stack data and position
        s->data[s->size - 1] =
            0x01000000; // This will set the T-Bit in the EPSR Part of the xPSR
                        // Register. As mentioned in the Armv6m Datasheet, the
                        // architecture only supports thumb instruction mode. So
                        // we need to maintain this bit always wit hthe value 1.
        s->data[s->size - 2] =
            (uint32_t)picos_idle; // The PC position to start exectuing
        // NOTE: We never expect the idle process to finish. So keep that in
        // mind, if that is not the case the controller will eventually crash.

        // calculate given address to a stack address (switch direction)
        t->sp = (uint32_t)(s->data + s->size - 16);

        picos_current[i] = t;
    }
}

void picos_suicide() {
    uint8_t cpu = *(uint32_t *)(SIO_BASE);

    spin_lock_blocking(PICOS_SCHEDULE_SPINLOCK);

    // Cleanup the thread slot
    picos_thread_t *current = picos_current[cpu];
    current->state = PICOS_UNKNOWN;
    current->pid = 0;
    current->sp = 0;
    current->cpu = 0xFF;

    spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);

    // avoid continiung with an return which would cause a crash if we return to
    // here
    for (;;)
        ;
}

void picos_scheduler_main() {
    uint8_t cpu = *(uint32_t *)(SIO_BASE);

    // This will set the wanted counter value which defines the delays between
    // scheduler calls
    *(volatile unsigned int *)(0xe0000000 | M0PLUS_SYST_RVR_OFFSET) =
        (clock_get_hz(clk_sys) / 1000000) *
        PICOS_SCHEDULER_INTERVAL_US; // the counter value to set in CSR when 0
                                     // is eached
    // This will configure the systick timer so we have the behavior required
    // for scheduling
    *(volatile unsigned int *)(0xe0000000 | M0PLUS_SYST_CSR_OFFSET) =
        (1 << 0)    // enable counter
        | (1 << 1)  // counter at 0 causes systick exception status pending
        | (1 << 2); // use processor clock
    // This will change the priority of the system handlers we utilize for our
    // scheduler calls
    *(volatile unsigned int *)(0xe0000000 | M0PLUS_SHPR3_OFFSET) =
        (0 << 30) |
        (3 << 22); // priority systick=0(high), priority pendsv=3(low)

    // Start execution of the initial process (here idle process)
    picos_exec_stack(picos_current[cpu]->sp);
}

void picos_enter_critical() {
    // This will disable the systick counter
    *(volatile unsigned int *)(0xe0000000 | M0PLUS_SYST_CSR_OFFSET) &=
        ~(1 << 0);
}

void picos_leave_critical() {
    // This will enable the systick counter
    *(volatile unsigned int *)(0xe0000000 | M0PLUS_SYST_CSR_OFFSET) |= (1 << 0);
}