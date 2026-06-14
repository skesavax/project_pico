#include "picos.h"
#include <stdio.h>
#define PICOS_IDLE_STACK_SIZE 100
// Define the declared variables from the header
picos_thread_t picos_threads[PICOS_MAX_THREADS];
picos_thread_t *picos_current[PICOS_CORES];
pico_core_stats_t pico_core_stats[PICOS_CORES];

#define MAX_PRIORITY_QUEUE 4
#ifdef PICO_SCHED_RR_EQUALSHARE
static uint8_t priority_ready_count[MAX_PRIORITY_QUEUE];
#endif //PICO_SCHED_RR_EQUALSHARE
picos_ready_queue_t priority_queue[MAX_PRIORITY_QUEUE];
uint32_t priority_queue_bitmap = 0;
#define SET_BIT(p)   (priority_queue_bitmap |=  (1U << (p)))
#define CLR_BIT(p)   (priority_queue_bitmap &= ~(1U << (p)))
#define HAS_BIT(p)   (priority_queue_bitmap &   (1U << (p)))

static picos_run_log_entry_t picos_log_buffer[PICOS_CORES][PICOS_LOG_BUFFER_SIZE];
static volatile uint32_t picos_log_head[PICOS_CORES] = {0};
static volatile uint32_t picos_log_tail[PICOS_CORES] = {0};
static uint32_t picos_log_overflow_count[PICOS_CORES] = {0};
static uint32_t picos_log_underflow_count = 0;
static uint8_t picos_log_rr_core = 0;
static picos_thread_t *picos_sleep_list[PICOS_CORES] = {0};

static void picos_enqueue_thread(picos_thread_t *t);
void isr_systick(void);

static void picos_sleep_enqueue(uint8_t core, picos_thread_t *thread){
    thread->next = picos_sleep_list[core];
    picos_sleep_list[core] = thread;
}

static void picos_wake_expired_sleepers(uint8_t core, uint64_t now){
    picos_thread_t *prev = NULL;
    picos_thread_t *node = picos_sleep_list[core];

    while (node){
        picos_thread_t *next = node->next;
        if (node->sleepUtilsUS <= now ){
            if (prev){
                prev->next = next;
            } else {
                picos_sleep_list[core] = next;
            }
            node->next = NULL;
            printf("WAKE pid=%u\n", node->pid);
            picos_enqueue_thread(node);
        } else {
            prev = node;
        }
        node = next;
    }
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

void picos_enqueue_thread(picos_thread_t *t) {
    uint8_t priority = t->priority;
    picos_ready_queue_t *q = &priority_queue[priority];
    t->next = NULL; //last entry is pointed to NULL
#ifdef PICO_SCHED_RR_EQUALSHARE
    t->timeQuantumUsed = 0; //Reset time quantum for this thread
#endif//PICO_SCHED_RR_EQUALSHARE
    if (q->tail == NULL){
        q->head = q->tail = t;
        SET_BIT(priority);
    }
    else {
       q->tail->next = t; //add element to last entry of circle linked list
       q->tail = t;
    }
#ifdef PICO_SCHED_RR_EQUALSHARE
    priority_ready_count[priority]++;
#endif//PICO_SCHED_RR_EQUALSHARE
    t->state = PICOS_READY;
}

picos_thread_t* picos_dequeue_thread(uint8_t priority) {
    picos_ready_queue_t *q = &priority_queue[priority];
    if (q->head == NULL) {
        return NULL;
    }
    picos_thread_t *t = q->head;
    q->head = t->next;
    if (q->head == NULL){
        q->tail = NULL;
        CLR_BIT(priority);
    }
    t->next = NULL;
#ifdef PICO_SCHED_RR_EQUALSHARE
    if (priority_ready_count[priority] > 0)
        priority_ready_count[priority]--;
#endif//PICO_SCHED_RR_EQUALSHARE
    return t;
}

static inline int32_t picos_get_high_priority(){
    if (priority_queue_bitmap == 0){
        return -1;
    }
    return 31 - __builtin_clz(priority_queue_bitmap);
}
#ifdef PICO_SCHED_RR_EQUALSHARE
static uint32_t picos_count_threads_of_priority(uint8_t priority){
    uint32_t count = priority_ready_count[priority];
    for (uint8_t cpu=0; cpu < PICOS_CORES; cpu++){
        picos_thread_t *current = picos_current[cpu];
        if (current && current->pid >= PICOS_CORES && 
            current->state == PICOS_RUNNING && current->priority == priority){
                count++;
            }
    }
    return (count == 0)? 1:count;
}

static uint64_t picos_compute_quantum_us(uint8_t priority){
    uint64_t budget_us = PICOS_TIME_QUANTUM_US;
    if (budget_us > PICOS_SCHEDULER_INTERVAL_US){
        budget_us = PICOS_SCHEDULER_INTERVAL_US;
    }
    uint64_t quantum_us = budget_us / picos_count_threads_of_priority(priority);
    return (quantum_us == 0)? 1: quantum_us;
}
#endif //PICO_SCHED_RR_EQUALSHARE
// Declare functions that will be later defined
void picos_setup_idle();
void picos_suicide();
void picos_scheduler_main();

// Functions from our assembler code
void picos_exec_stack(uint32_t sp);
void picos_set_psp(uint32_t sp, uint32_t ctrl);

void picos_init() { picos_setup_idle(); }

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

    slot->execTime = 0;
#ifdef PICO_SCHED_RR_EQUALSHARE
    slot->timeQuantumUsed = 0;
#endif
    slot->lastStartTime = 0;
    slot->sleepUtilsUS = 0;

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
    picos_enqueue_thread(slot);

    spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);

    return thread_slot;
}

void picos_thread_sleep(uint32_t ms){
    uint8_t cpu = get_core_num();
    uint64_t now = time_us_64();
    uint64_t elapsed = 0;
    spin_lock_blocking(PICOS_SCHEDULE_SPINLOCK);

    picos_thread_t *current = picos_current[cpu];
    printf("ENTER SLEEP pid=%u\n", current->pid);
    if (current == NULL || current->pid < PICOS_CORES ||
         current->state != PICOS_RUNNING){
        spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);
        sleep_ms(ms);
        return;
    }
    if (current->lastStartTime > 0){
        uint64_t elapsed = now - current->lastStartTime;
        current->execTime += elapsed;
#ifdef PICO_SCHED_RR_EQUALSHARE
        current->timeQuantumUsed +=elapsed;
#endif
    }
    current->state = PICOS_SLEEPING;
    current->sleepUtilsUS = now + ((uint64_t)ms * 1000ULL);
    current->lastStartTime = 0;
    picos_sleep_enqueue(cpu, current);
    spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);
    printf("SLEEP pid=%u wake=%llu\n",
       current->pid,
       current->sleepUtilsUS);
    // trigger a context s/w so another runnable thread can execute now.
    isr_systick();
    while(picos_current[cpu] == current){
        asm volatile("wfi");
    }
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
    uint8_t cpu = *(uint32_t *)(SIO_BASE);

    // Update state to hardfault
    picos_thread_t *current = picos_current[cpu];
    current->state = PICOS_HARDFAULT;

    // Trigger scheduling routine (will store context)
    isr_systick();

    for (;;)
        ;
}

void picos_schedule() {
    spin_lock_blocking(PICOS_SCHEDULE_SPINLOCK);
    uint32_t cpu = get_core_num();   // IMPORTANT FIX (do NOT use SIO_BASE)
    picos_thread_t *current = picos_current[cpu];
    picos_thread_t *next = NULL;
    uint64_t now = time_us_64();
    picos_wake_expired_sleepers((uint8_t)cpu, now);
#ifdef PICO_SCHED_RR_EQUALSHARE
    //Account execution time for current thread
    if (current->lastStartTime > 0){
        uint64_t elapsed = now - current->lastStartTime;
        current->execTime += elapsed;
        current->timeQuantumUsed += elapsed;
    }
    if (current->pid >= PICOS_CORES){
        uint32_t highest_priority = picos_get_high_priority();
        bool higher_prio_waiting = (highest_priority > (uint32_t)current->priority);
        uint64_t quantum_us = picos_compute_quantum_us(current->priority);
        if (!higher_prio_waiting && current->timeQuantumUsed < quantum_us){
            //keep running this thread until it consumes its equal share
            current->lastStartTime = now;
            spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);
            return;
        }
        current->state = PICOS_READY;
        picos_enqueue_thread(current);
    }
#else //#ifdef PICO_SCHED_RR_EQUALSHARE
    if (current && current->state == PICOS_RUNNING) {
        printf("CURRENT pid=%u state=%u\n",
       current->pid,
       current->state);
        uint64_t last = current->lastStartTime;
        current->execTime += (now - current->lastStartTime);
        current->state = PICOS_READY;
        picos_enqueue_thread(current);
    }
#endif
    //pick next thread
    uint8_t priority = picos_get_high_priority();
    if (priority < 0){
        next = &picos_threads[cpu]; // idle thread
    }
    else {
        next = picos_dequeue_thread(priority);
        if (!next)
            next = &picos_threads[cpu]; // idle thread
    }
    next->state = PICOS_RUNNING;
    next->lastStartTime = now;
    printf("CPU%u -> pid=%u state=%u prio=%u\n",
       cpu,
       next->pid,
       next->state,
       next->priority);
#ifdef PICO_SCHED_RR_EQUALSHARE
    next->timeQuantumUsed = 0;//Rest quantum for new thread
#endif
    picos_current[cpu] = next;
    spin_unlock_unsafe(PICOS_SCHEDULE_SPINLOCK);
}

#ifdef SIMPLE_RR_SCHED
void picos_schedule() {

    uint32_t cpu = get_core_num();   // IMPORTANT FIX (do NOT use SIO_BASE)

    picos_thread_t *current = picos_current[cpu];
    pico_core_stats_t *stats = &pico_core_stats[cpu];

    uint64_t now = time_us_64();

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

    for (picos_pid i = 1; i < PICOS_MAX_THREADS; i++) {

        picos_thread_t *t =
            &picos_threads[(((current->pid - PICOS_CORES) + i) %
            PICOS_USER_THREADS) + PICOS_CORES];

        if (t->state == PICOS_RUNNING &&
           (t->cpu == cpu || t->cpu == 0xFF)) {

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
}
#endif //SIMPLE_RR_SCHED
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
        t->state = PICOS_RUNNING;
        t->priority = 0;
        t->execTime = 0;
        t->sleepUtilsUS = 0;
        t->lastStartTime = 0;
        t->next = NULL;
#ifdef PICO_SCHED_RR_EQUALSHARE
        t->timeQuantumUsed = 0;
#endif

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