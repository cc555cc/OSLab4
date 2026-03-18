/*
 * Lab 4 - Scheduler
 *
 * IMPORTANT:
 * - Do NOT change print_log() format (autograder / TA diff expects exact output).
 * - Do NOT change the order of operations in the main tick loop.
 * - You may change internal implementations of the TODO functions freely,
 *   as long as behavior matches the lab requirements.
 * - compile: $make
 *   run testcase: $./groupX_scheduler < test_input.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "queue.h"

/*
 * Assumptions / Lab rules:
 * - user priority values are 0,1,2,3 (0 = Real-Time, 1..3 = user queues)
 * - all processes have mem_req > 0
 * - RT process memory is ALWAYS 64MB (reserved); user processes share 960MB
 * - user memory allocation range is [64, 1023], integer MB, contiguous blocks
 * - continuous allocation policy: First Fit (per modified handout)
 * - Processes are sorted by arrival_time in the test files
 */

/* ----------------------------
 * Global “hardware resources”
 * ---------------------------- */
int printers = 2;
int scanners = 1;
int modems = 1;
int cd_drives = 2;

/* Total user-available memory (excluding RT reserved region) */
int memory = 960;
int memory_real_time = 64;

/* ----------------------------
 * Ready queues (provided by queue.h / queue.c)
 * ---------------------------- */
queue_t rt_queue;       /* real-time queue */
queue_t sub_queue;      /* submission queue (user processes wait here until admitted) */
queue_t user_queue[3];  /* user queues: index 0 for priority 1, index 1 for priority 2, index 2 for priority 3 */


/* ----------------------------
 * Optional memory block-list type (NOT mandatory to use)
 * ----------------------------
 * You are NOT required to use a free-block list. You may implement memory using
 * any data structure (array/bitmap/list/etc.) as long as:
 *   - it behaves exactly like First Fit defined in the requirement document
 *   - it returns correct starting addresses (mem_start) for user processes
 */
typedef struct free_block free_block_t;
free_block_t *freelist; /* optional: head pointer if you choose block-list approach */

#define MAX_PROCESSES 128

/* ----------------------------
 * Process state and process struct
 * ---------------------------- */
typedef enum {
    NEW,        /* read in, but not yet arrived (time < arrival_time) */
    SUBMITTED,  /* arrived; in submission queue waiting for ADMIT */
    READY,      /* admitted; in a user queue or RT queue, waiting to run */
    RUNNING,    /* currently running for this tick */
    TERMINATED  /* finished execution */
} proc_state_t;

typedef struct process {
    /* identity */
    int pid;

    /* input fields*/
    int arrival_time;
    int init_prio;   /* 0 = RT, 1..3 = user priority */
    int cpu_total;   /* initial CPU time requested */
    int mem_req;     /* requested memory in MB */
    int printers;
    int scanners;
    int modems;
    int cds;

    /* runtime fields */
    int cpu_remain;      /* remaining CPU time */
    int current_prio;    /* current user priority (1..3); RT stays 0 */
    proc_state_t state;

    /* memory allocation result (must be set for user processes after admission) */
    int mem_start;       /* starting address (MB) of allocated contiguous memory block */
} process_t;

/* Optional free block struct (only used if you choose block-list approach) */
typedef struct free_block {
    int start;                  /* start address (MB) */
    int size;                   /* block size (MB) */
    struct free_block *next;    /* next free block in ascending start order */
} free_block_t;

/* =========================================================
 * REQUIRED FUNCTIONS (MUST IMPLEMENT)
 * =========================================================
 * These functions are called directly by main() in this skeleton.
 * You MUST provide implementations with the same signatures.
 */

void memory_init(void);                       /* Initialize your memory manager state. */
void admit_process(void);                     /* Admit user jobs from sub_queue to user queues when possible. */
process_t *dispatch(process_t **cur_running_rt); /* Select the process to run this tick. */
void run_process(process_t *p);               /* Run the selected process for exactly 1 tick. */
void post_run(process_t *p, process_t **cur_running_rt); /* Handle completion / re-queue after 1 tick. */
int termination_check(int processNo, int process_count, process_t *cur_running_rt); /* Return 1 if simulation ends. */


/* =========================================================
 * OPTIONAL HELPER FUNCTIONS (OPTIONAL TO IMPLEMENT/USE)
 * =========================================================
 * You may implement these helpers, change their behavior, or ignore them entirely.
 * They exist only to suggest a clean decomposition; you can inline logic elsewhere.
 * (Especially for memory: any correct approach is allowed.)
 */

/* ---- Memory helpers (optional) ---- */
int memory_can_allocate(int req_size);        /* Optional: check if req_size can be allocated now. */
int memory_allocate(process_t *p);            /* Optional: allocate memory for a user process and set p->mem_start. */
int memory_free(process_t *p);                /* Optional: free a user process memory block. */

/* ---- Resource helpers (optional) ---- */
int resource_available(process_t *p);         /* Optional: check if resources are available. */
void resource_occupy(process_t *p);           /* Optional: reserve resources for an admitted user process. */
void resource_free(process_t *p);             /* Optional: release resources when a process terminates. */

/* ---- Arrival helper (optional) ---- */
void arrival(process_t *p);                   /* Optional: enqueue a newly arrived process. */


/* =========================================================
 * LOG OUTPUT (DO NOT MODIFY)
 * =========================================================
 * This output format is fixed for grading / diff.
 * Called after run_process() and before post_run().
 */
void print_log(process_t *ready_process, int time) {
    if (ready_process == NULL) {
        printf("[t=%d] IDLE\n", time);
    } else {
        printf(
            "[t=%d] RUN PID=%d PR=%d CPU=%d MEM_ST=%d MEM=%d P=%d S=%d M=%d C=%d\n",
            time,
            ready_process->pid,
            ready_process->current_prio,
            ready_process->cpu_remain,
            ready_process->mem_start,
            ready_process->mem_req,
            ready_process->printers,
            ready_process->scanners,
            ready_process->modems,
            ready_process->cds
        );
    }
}


/* =========================================================
 * REQUIRED FUNCTION STUBS (STUDENTS MUST COMPLETE)
 * ========================================================= */

void arrival(process_t *p) {
    if (p == NULL) {
        return;
    }

    /* RT jobs become ready immediately; user jobs wait for admission. */
    if (p->init_prio == 0) {
        p->state = READY;
        p->current_prio = 0;
        p->mem_req = memory_real_time;
        p->mem_start = 0;
        queue_push(&rt_queue, p);
    } else if (p->init_prio >= 1 && p->init_prio <= 3) {
        p->state = SUBMITTED;
        p->current_prio = p->init_prio;
        queue_push(&sub_queue, p);
    }
}

void admit_process(void) {
    while (!queue_empty(&sub_queue)) {
        process_t *p = queue_peek(&sub_queue);

        /*
         * The lab requires FIFO admission at the head of the submission queue.
         * If the head job cannot be fully admitted yet, later jobs must wait.
         */
        if (p == NULL ||
            !resource_available(p) ||
            !memory_can_allocate(p->mem_req)) {
            break;
        }

        p = queue_pop(&sub_queue);
        if (memory_allocate(p) != 0) {
            /* Allocation should match memory_can_allocate(); stop defensively. */
            queue_push(&sub_queue, p);
            break;
        }

        resource_occupy(p);
        p->state = READY;
        queue_push(&user_queue[p->current_prio - 1], p);
    }
}

process_t *dispatch(process_t **cur_running_rt) {
    process_t *next_p = NULL; //stores the pointer to the next process to be dispatched//

    //check whether there is an active real time process//
    if (*cur_running_rt != NULL) { 
        (*cur_running_rt)->state = RUNNING;
        return *cur_running_rt; //return pointer of the real time process: continue executing that process//
    }

    //checkeahc queue by priotrity and their emptiness//
    if (!queue_empty(&rt_queue)) {
        next_p = queue_pop(&rt_queue); 
        *cur_running_rt = next_p;
    } else if (!queue_empty(&user_queue[0])) {
        next_p = queue_pop(&user_queue[0]);
    } else if (!queue_empty(&user_queue[1])) {
        next_p = queue_pop(&user_queue[1]);
    } else if (!queue_empty(&user_queue[2])) {
        next_p = queue_pop(&user_queue[2]);
    }

    //change process state to RUNNING//
    if (next_p != NULL) {
        next_p -> state = RUNNING;
    }

    return next_p;
}

//changes cpu_remain value in a process from argument//
void run_process(process_t *p) {
    if(p != NULL && p->cpu_remain > 0){
        p->cpu_remain--;
    }
}


void post_run(process_t *p, process_t **cur_running_rt) {
    if (p != NULL) {
        if (p->cpu_remain == 0) {
            p->state = TERMINATED;

            if (p->current_prio > 0) {
                memory_free(p);
                resource_free(p);
            }

            if (*cur_running_rt == p) {
                *cur_running_rt = NULL;
            }
        } else {
            p->state = READY;

            if (p->current_prio == 0) {
                *cur_running_rt = p;
            } else {
                if (p->current_prio < 3) {
                    p->current_prio++;
                }
                queue_push(&user_queue[p->current_prio - 1], p);
            }
        }
    }
}


int termination_check(int processNo, int process_count, process_t *cur_running_rt) {
    
    if(processNo == process_count  &&
        cur_running_rt == NULL      &&
        queue_empty(&rt_queue)      &&
        queue_empty(&sub_queue)     &&
        queue_empty(&user_queue[0]) &&
        queue_empty(&user_queue[1]) &&
        queue_empty(&user_queue[2])
    ){ return 1; 
    } else {
        return 0;
    }
}


/* =========================================================
 * OPTIONAL FUNCTION STUBS (YOU MAY USE OR IGNORE)
 * ========================================================= */

void memory_init(void) {
    /* Initialize freelist to single user-available block [64, 64+memory-1] */
    /* free list keeps blocks in ascending start order */
    freelist = (free_block_t *)malloc(sizeof(free_block_t));
    if (freelist == NULL) {
        fprintf(stderr, "memory_init: malloc failed\n");
        exit(1);
    }
    freelist->start = memory_real_time; /* 64 */
    freelist->size = memory; /* 960 */
    freelist->next = NULL;
}

int memory_can_allocate(int req_size) {
    if (req_size <= 0) {
        return 0;
    }

    free_block_t *cur = freelist;
    while (cur != NULL) {
        if (cur->size >= req_size) return 1;
        cur = cur->next;
    }
    return 0;
}

int memory_allocate(process_t *p) {
    if (p == NULL) return -1;
    int req = p->mem_req;
    free_block_t *cur = freelist;
    free_block_t *prev = NULL;

    while (cur != NULL) {
        if (cur->size >= req) {
            /* allocate at cur->start */
            p->mem_start = cur->start;

            if (cur->size == req) {
                /* remove block from freelist */
                if (prev == NULL) {
                    free_block_t *tmp = cur->next;
                    free(cur);
                    freelist = tmp;
                } else {
                    prev->next = cur->next;
                    free(cur);
                }
            } else {
                /* shrink block */
                cur->start += req;
                cur->size -= req;
            }
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return -1; /* not found */
}

int memory_free(process_t *p) {
    if (p == NULL) return -1;
    int start = p->mem_start;
    int size = p->mem_req;

    /* create new free block */
    free_block_t *nb = (free_block_t *)malloc(sizeof(free_block_t));
    if (nb == NULL) return -1;
    nb->start = start;
    nb->size = size;
    nb->next = NULL;

    /* insert into freelist in ascending order */
    if (freelist == NULL || nb->start < freelist->start) {
        nb->next = freelist;
        freelist = nb;
    } else {
        free_block_t *cur = freelist;
        while (cur->next != NULL && cur->next->start < nb->start) {
            cur = cur->next;
        }
        nb->next = cur->next;
        cur->next = nb;
    }

    /* coalesce adjacent blocks */
    free_block_t *cur = freelist;
    while (cur != NULL && cur->next != NULL) {
        if (cur->start + cur->size == cur->next->start) {
            free_block_t *tmp = cur->next;
            cur->size += tmp->size;
            cur->next = tmp->next;
            free(tmp);
            continue; /* check again at same cur */
        }
        cur = cur->next;
    }

    p->mem_start = 0;
    return 0;
}

int resource_available(process_t *p) {
    if (p == NULL) {
        return 0;
    }

    return printers >= p->printers &&
           scanners >= p->scanners &&
           modems >= p->modems &&
           cd_drives >= p->cds;
}

void resource_occupy(process_t *p) {
    if (p == NULL) {
        return;
    }

    printers -= p->printers;
    scanners -= p->scanners;
    modems -= p->modems;
    cd_drives -= p->cds;
}

void resource_free(process_t *p) {
    if (p == NULL) {
        return;
    }

    printers += p->printers;
    scanners += p->scanners;
    modems += p->modems;
    cd_drives += p->cds;
}



/* =========================================================
 * MAIN (DO NOT CHANGE LOOP ORDER)
 * =========================================================
 * This main reads processes from stdin, then simulates 1-second ticks.
 *
 * Input format: each line has 8 integers:
 *   <arrival> <priority> <cpu> <mem> <printers> <scanners> <modems> <cds>
 *
 * IMPORTANT:
 * - For determinism with your current arrival loop, input should be sorted by arrival_time.
 *   (If unsorted, later arrivals may never be enqueued due to the break condition.)
 * - In all the test files, the inputs are sorted by arrival_time.
 */
int main(void) {
    /* Initialize queues (provided by queue.h) */
    queue_init(&rt_queue);
    queue_init(&sub_queue);
    for (int i = 0; i < 3; i++) {
        queue_init(&user_queue[i]);
    }

    /* Initialize memory manager (YOUR implementation) */
    memory_init();

    /* Read processes from stdin */
    process_t processes[MAX_PROCESSES];
    int process_count = 0;

    while (process_count < MAX_PROCESSES) {
        int a, p, cpu, mem, pr, sc, mo, cd;
        if (scanf("%d %d %d %d %d %d %d %d",
                  &a, &p, &cpu, &mem, &pr, &sc, &mo, &cd) != 8) {
            break; /* EOF or invalid input */
        }

        processes[process_count].arrival_time = a;
        processes[process_count].init_prio    = p;
        processes[process_count].cpu_total    = cpu;
        processes[process_count].mem_req      = mem;
        processes[process_count].printers     = pr;
        processes[process_count].scanners     = sc;
        processes[process_count].modems       = mo;
        processes[process_count].cds          = cd;

        processes[process_count].pid          = process_count;
        processes[process_count].cpu_remain   = cpu;
        processes[process_count].current_prio = p;     /* initial priority */
        processes[process_count].state        = NEW;
        processes[process_count].mem_start    = 0;     /* will be set when admitted (user processes) */

        process_count++;
    }

    /* Simulation state:
    * - cur_running_rt: holds the currently running RT job (if any). In this lab, an RT job,
    *   once dispatched, stays as the selected RT job across ticks until it terminates.
    *   (This is just a state variable kept by main; your dispatch/post_run can manage it.)
    * - ready_process: the process selected to run for THIS tick only (may be RT/user/NULL).
    * - Note: You are free to implement RT handling differently internally, as long as
    *   the external behavior matches the lab requirements and the main loop order is unchanged.
 
    */
    int processNo = 0;                 /* index of next process that has not arrived yet */
    process_t *cur_running_rt = NULL;  /* if an RT job is running, it persists until completion */

    /* Tick-by-tick simulation */
    for (int time = 0; ; time++) {
        /* 1) ARRIVAL: move any processes arriving at this tick into rt_queue or sub_queue */
        for (; processNo < process_count; processNo++) {
            if (processes[processNo].arrival_time == time) {
                arrival(&processes[processNo]);
            } else {
                break; /* important for determinism; assumes arrivals are sorted */
            }
        }

        /* 2) ADMIT: move as many from submission queue to user queues as possible */
        admit_process();

        /* 3) DISPATCH: pick the process to run for this tick
        * dispatch() returns the process that should run in the current tick.
        * It may also update cur_running_rt to remember a running RT job across ticks.
        */
        process_t *ready_process = dispatch(&cur_running_rt);

        /* 4) RUN: execute exactly 1 tick */
        run_process(ready_process);

        /* 5) PRINT: fixed log format for grading (after run, before post-run updates) */
        print_log(ready_process, time);

        /* 6) POST-RUN: terminate/requeue/demote as needed */
        post_run(ready_process, &cur_running_rt);

        /* Terminate when all work is done */
        if (termination_check(processNo, process_count, cur_running_rt)) {
            break;
        }
    }

    return 0;
}
