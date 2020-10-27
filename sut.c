#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include "sut.h"
#include "queue.h"

#define THREAD_STACK_SIZE 1024*64
#define MAX_NUM_THREADS 32

// kernel threads
static pthread_t cexec_handle, iexec_handle;
// c_exec context and caller context
ucontext_t caller_context, cexec_context;
//queues
struct queue ready_queue, wait_queue, to_io_queue, from_io_queue;
// mutex lock
static pthread_mutex_t ready_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t shutdown_lock = PTHREAD_MUTEX_INITIALIZER;
atomic_int num_tasks_created= 0;
atomic_int num_tasks_completed = 0;

TCB tcb_arr[MAX_NUM_THREADS];
ucontext_t context_arr[MAX_NUM_THREADS];

int current_thread, num_threads;
bool shutdown_flag = false;

void * c_exec(void *arg);

void * i_exec(void *arg);

void * c_exec(void *arg){
    struct queue_entry *ptr;
    TCB *tcb;
    ucontext_t context_to_execute;
    while(true){
        pthread_mutex_lock(&ready_queue_lock);
        ptr = queue_peek_front(&ready_queue);
        pthread_mutex_unlock(&ready_queue_lock);
        while(!ptr){
            if (shutdown_flag && num_tasks_completed == num_tasks_created){
                printf("Received shutdown signal, no more tasks scheduled to run...terminating program.\n");
                exit(0);
            }
            usleep(100);
            pthread_mutex_lock(&ready_queue_lock);
            ptr = queue_peek_front(&ready_queue);
            pthread_mutex_unlock(&ready_queue_lock);
        }
        tcb = (TCB *)ptr->data;
        getcontext(&cexec_context);
        cexec_context.uc_stack.ss_sp = (char *)malloc(THREAD_STACK_SIZE);
        cexec_context.uc_stack.ss_size = THREAD_STACK_SIZE;
        cexec_context.uc_link = &caller_context;
        cexec_context.uc_stack.ss_flags = 0;
        swapcontext(&cexec_context, &(tcb->thread_context));
        usleep(100);
        pthread_mutex_lock(&ready_queue_lock);
        ptr = queue_pop_head(&ready_queue);
        pthread_mutex_unlock(&ready_queue_lock);

    }
    printf("exited the loop\n");
}

void sut_init(){
    getcontext(&caller_context);
    // declare and define queues
    ready_queue = queue_create();
    queue_init(&ready_queue);

    num_threads = 0;
    current_thread = 0;

    int cexec_rval = pthread_create(&cexec_handle, NULL, c_exec, NULL);
    if (cexec_rval !=0 ){
        fprintf(stderr, "Error when creating the C_EXEC kernel thread. Terminating program.\n");
        exit(-1);
    }
    printf("Initialization completed successfully.\n");
}

bool sut_create(sut_task_f fn){
    if (num_threads >= MAX_NUM_THREADS){
        fprintf(stderr, "ERROR: Exceeded maximum number of threads.\n");
        return false;
    }
    num_threads++;
    // task control block to enqueue
    TCB *tcb;
    tcb = &(tcb_arr[num_threads]);
    getcontext(&(tcb->thread_context));
    tcb->thread_id = num_threads;
    tcb->thread_stack = (char *)malloc(THREAD_STACK_SIZE);
    tcb->thread_context.uc_stack.ss_sp = tcb->thread_stack;
    tcb->thread_context.uc_stack.ss_size = THREAD_STACK_SIZE;
    tcb->thread_context.uc_stack.ss_flags = 0;
    tcb->thread_context.uc_link = &cexec_context;
    tcb->thread_function = *fn;

    makecontext(&(tcb->thread_context), *fn, 0);
    struct queue_entry *node = queue_new_node(tcb);
    pthread_mutex_lock(&ready_queue_lock);
    queue_insert_tail(&ready_queue, node);
    pthread_mutex_unlock(&ready_queue_lock);
    num_tasks_created++;

    
    printf("Task id %d created.\nNumber of threads: %d\n", tcb->thread_id, num_threads);
    return true;
}

void sut_yield(){
    int temp_thread, next_thread;
    next_thread = (current_thread + 1) % num_threads;
    temp_thread = current_thread;
    current_thread = next_thread;
    getcontext(&(tcb_arr[temp_thread].thread_context));
    struct queue_entry *node = queue_new_node(&tcb_arr[temp_thread]);
    pthread_mutex_lock(&ready_queue_lock);
    queue_insert_tail(&ready_queue, node);
    pthread_mutex_unlock(&ready_queue_lock);

    swapcontext(&(tcb_arr[temp_thread].thread_context), &cexec_context);
}

void sut_exit(){
    printf("Task exiting...\n");
    num_tasks_completed++;
    printf("Number of tasks created: %d\n", num_tasks_created);
    printf("Number of tasks completed: %d\n", num_tasks_completed);
    ucontext_t dummy_context;
    swapcontext(&dummy_context, &cexec_context);
}

void sut_open(char *dest, int port){}

void sut_write(char *buf, int size){}

void sut_close(){

}

void sut_shutdown(){
    printf("Sending shutdown signal...\n");
    pthread_mutex_lock(&shutdown_lock);
    shutdown_flag = true;
    pthread_join(cexec_handle, NULL);
    pthread_mutex_unlock(&shutdown_lock);
}