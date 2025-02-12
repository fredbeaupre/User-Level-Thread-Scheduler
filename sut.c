#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "sut.h"
#include "queue.h"

#define BACKLOG_SIZE 10

#define THREAD_STACK_SIZE 1024 * 64
#define MAX_NUM_THREADS 32

// kernel threads
static pthread_t cexec_handle, iexec_handle;
// c_exec context and caller context
ucontext_t caller_context, cexec_context, iexec_context;
//queues
struct queue ready_queue, wait_queue, to_io_queue, from_io_queue;
// mutex locks
static pthread_mutex_t ready_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t shutdown_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t to_io_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_queue_lock = PTHREAD_MUTEX_INITIALIZER;

// trackers and flags for shutdown
atomic_int num_tasks_created = 0;
atomic_int num_tasks_completed = 0;
bool shutdown_flag = false;
bool open_flag = false;
// to make sure iexec exits before cexec can.
// See conditions for exiting in respective functions

bool iexec_flag = false;
char server_msg[1024];

// array of task control blocks (what we enqueue to ready_queue)
TCB tcb_arr[MAX_NUM_THREADS];

// for swapping contexts
int current_thread, num_threads;

// socket file descriptor for I/O
static int sockfd;

void *c_exec(void *arg);
void *i_exec(void *arg);
int create_server(const char *host, uint16_t port, int *sockfd);
int connect_to_server(const char *host, uint16_t port, int *sockfd);
ssize_t send_message(int sockfd, const char *buf, size_t len);
ssize_t recv_message(int sockfd, char *buf, size_t len);

void *c_exec(void *arg)
{
    // declare queue entry pointer
    struct queue_entry *ptr;
    // declare task control block
    TCB *tcb;
    while (true)
    {
        // peek queue_head
        pthread_mutex_lock(&ready_queue_lock);
        ptr = queue_peek_front(&ready_queue);
        pthread_mutex_unlock(&ready_queue_lock);
        // if queue is empty
        while (!ptr)
        {
            // if queue is empty AND all tasks have completed -> shutdown
            if (shutdown_flag && iexec_flag && num_tasks_completed == num_tasks_created)
            {
                exit(0);
            }
            // sleep for 100 microseconds then try again
            usleep(100);
            pthread_mutex_lock(&ready_queue_lock);
            ptr = queue_peek_front(&ready_queue);
            pthread_mutex_unlock(&ready_queue_lock);
        }
        // queue is non-empty
        tcb = (TCB *)ptr->data;
        // save current c_exec context, swap to tcb's context
        getcontext(&cexec_context);
        cexec_context.uc_stack.ss_sp = (char *)malloc(THREAD_STACK_SIZE);
        cexec_context.uc_stack.ss_size = THREAD_STACK_SIZE;
        cexec_context.uc_link = &caller_context;
        cexec_context.uc_stack.ss_flags = 0;
        swapcontext(&cexec_context, &(tcb->thread_context));
        // sleep for a while, check the queue again
        usleep(100);
        pthread_mutex_lock(&ready_queue_lock);
        ptr = queue_pop_head(&ready_queue);
        pthread_mutex_unlock(&ready_queue_lock);
    }
    printf("exited the loop\n");
} // c_exec

void *i_exec(void *arg)
{
    TCB *tcb;
    ICB *icb;

    // wait_queue and to_io_queue pointers when popping
    struct queue_entry *wq_ptr;
    struct queue_entry *io_ptr;
    while (true)
    {
        // peek front of to_io_queue
        pthread_mutex_lock(&to_io_queue_lock);
        io_ptr = queue_peek_front(&to_io_queue);
        pthread_mutex_unlock(&to_io_queue_lock);
        while (!io_ptr) // if queue is empty
        {
            // check shutdown condition
            if (shutdown_flag && num_tasks_completed == num_tasks_created)
            {
                iexec_flag = true;
                exit(0);
            }
            // if !(shutdown condition) => sleep for 100 microseconds and try again
            usleep(100);
            pthread_mutex_lock(&to_io_queue_lock);
            io_ptr = queue_peek_front(&to_io_queue);
            pthread_mutex_unlock(&to_io_queue_lock);
            // add a break here to test first three tests without the IO part
            // taking over the console
            // BREAK BELOW
        }
        // BREAK BELOW
        icb = (ICB *)io_ptr->data;

        if (icb->id == 0) // if function call is sut_open
        {
            // connect to server
            if (connect_to_server(icb->dest, icb->port, &sockfd) < 0)
            {
                fprintf(stderr, "Error connecting to the server\n");
            }
            // set open_flag to true so that other io calls can safely be performed
            open_flag = true;
            // now that server is created, move task from wait_queue to ready_queue
            // so that it can later be scheduled by c_exec
            pthread_mutex_lock(&wait_queue_lock);
            wq_ptr = queue_pop_head(&wait_queue);
            pthread_mutex_unlock(&wait_queue_lock);
            pthread_mutex_lock(&ready_queue_lock);
            queue_insert_tail(&ready_queue, wq_ptr);
            pthread_mutex_unlock(&ready_queue_lock);

            // pop next element in the queue
            pthread_mutex_lock(&to_io_queue_lock);
            io_ptr = queue_pop_head(&to_io_queue);
            pthread_mutex_unlock(&to_io_queue_lock);
        }
        else if (icb->id == 1) // if function call is sut_write
        {
            if (!open_flag) // checks that sut_open was called prior to this
            {
                // if not, print error and exit
                fprintf(stderr, "ERROR, no prior successful sut_open call.\n");
                exit(-1);
            }
            // send message to the server
            send_message(sockfd, icb->dest, strlen(icb->dest));
            pthread_mutex_lock(&to_io_queue_lock);
            io_ptr = queue_pop_head(&to_io_queue);
            pthread_mutex_unlock(&to_io_queue_lock);
        }
        else if (icb->id == 2) // function call is sut_read
        {
            if (!open_flag) // check sut_open was called prior to this
            {
                fprintf(stderr, "ERROR, no prior successful sut_open call.\n");
                exit(-1);
            }
            // received message back from the server, which corresponds to the message sent
            // in the previous sut_write call
            recv_message(sockfd, server_msg, sizeof(server_msg));

            // I/O is done with sut_read, so move task from wait_queue to ready_queue so it can later be
            // scheduled by C_exec
            pthread_mutex_lock(&wait_queue_lock);
            wq_ptr = queue_pop_head(&wait_queue);
            pthread_mutex_unlock(&wait_queue_lock);

            pthread_mutex_lock(&ready_queue_lock);
            queue_insert_tail(&ready_queue, wq_ptr);
            pthread_mutex_unlock(&ready_queue_lock);

            // pop next element in the io_queue
            pthread_mutex_lock(&to_io_queue_lock);
            io_ptr = queue_pop_head(&to_io_queue);
            pthread_mutex_unlock(&to_io_queue_lock);
        }
        else if (icb->id == 3) // if function call is sut_close
        {
            if (!open_flag) // checks if sut_open was called prior to this
            {
                fprintf(stderr, "ERROR, no prior successful sut_open call.\n");
                exit(-1);
            }
            // close socket connection
            close(sockfd);
            // pop next element in the queue
            pthread_mutex_lock(&to_io_queue_lock);
            io_ptr = queue_pop_head(&to_io_queue);
            pthread_mutex_unlock(&to_io_queue_lock);
        }
    }
} // i_exec

void sut_init()
{
    // get user's context
    getcontext(&caller_context);
    // declare and define queues
    ready_queue = queue_create();
    queue_init(&ready_queue);
    to_io_queue = queue_create();
    queue_init(&to_io_queue);
    wait_queue = queue_create();
    queue_init(&wait_queue);

    num_threads = 0;
    current_thread = 0;

    // create kernel threads
    int cexec_rval = pthread_create(&cexec_handle, NULL, c_exec, NULL);
    int iexec_rval = pthread_create(&iexec_handle, NULL, i_exec, NULL);
    if (cexec_rval != 0 || iexec_rval != 0)
    {
        fprintf(stderr, "Error when creating the kernel threads. Terminating program.\n");
        exit(-1);
    }
} // sut_init

bool sut_create(sut_task_f fn)
{
    // check max number of threads condition before doing anything
    if (num_threads >= MAX_NUM_THREADS)
    {
        fprintf(stderr, "ERROR: Exceeded maximum number of threads.\n");
        return false;
    }
    // we're good to create a new thread
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

    // make context and enqueue the whole struct (NOT just the context)
    makecontext(&(tcb->thread_context), *fn, 0);
    struct queue_entry *node = queue_new_node(tcb);
    pthread_mutex_lock(&ready_queue_lock);
    queue_insert_tail(&ready_queue, node);
    pthread_mutex_unlock(&ready_queue_lock);
    num_tasks_created++;

    return true;
} // sut_create

void sut_yield()
{
    // save current context, enqueue it, then swapcontext to c_exec so it can execute next task
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
} // sut_yield

void sut_exit()
{
    ucontext_t context_to_kill;
    num_tasks_completed++;
    swapcontext(&context_to_kill, &cexec_context);
} // sut_exit

void sut_open(char *dest, int port)
{
    TCB *tcb = (TCB *)malloc(THREAD_STACK_SIZE);
    ICB *icb = (ICB *)malloc(THREAD_STACK_SIZE);

    // get context of function which called sut_open, store in wait queue while io processes the request
    getcontext(&(tcb->thread_context));
    pthread_mutex_lock(&wait_queue_lock);
    struct queue_entry *node = queue_new_node(tcb);
    queue_insert_tail(&wait_queue, node);
    pthread_mutex_unlock(&wait_queue_lock);

    // struct with socket connection info to enqueue to to_io_queue
    icb->port = port;
    icb->id = 0;
    strcpy(icb->dest, dest);
    pthread_mutex_lock(&to_io_queue_lock);
    struct queue_entry *node2 = queue_new_node(icb);
    queue_insert_tail(&to_io_queue, node2);
    pthread_mutex_unlock(&to_io_queue_lock);

    // swap context to c_exec so that it runs concurrently to I/O being performed (no blocking)
    swapcontext(&(tcb->thread_context), &cexec_context);

} // sut_open

void sut_write(char *buf, int size)
{
    // signals i_exec to write buf with size size to the socket (non-blocking)
    ICB *icb = (ICB *)malloc(THREAD_STACK_SIZE);
    icb->id = 1;
    strcpy(icb->dest, buf);
    icb->port = size;
    pthread_mutex_lock(&to_io_queue_lock);
    struct queue_entry *node = queue_new_node(icb);
    queue_insert_tail(&to_io_queue, node);
    pthread_mutex_unlock(&to_io_queue_lock);

} // sut_write

char *sut_read()
{
    ICB *icb = (ICB *)malloc(THREAD_STACK_SIZE);
    TCB *tcb = (TCB *)malloc(THREAD_STACK_SIZE);

    // instruct i_exec to perform the read
    icb->id = 2;
    pthread_mutex_lock(&to_io_queue_lock);
    struct queue_entry *node = queue_new_node(icb);
    queue_insert_tail(&to_io_queue, node);
    pthread_mutex_unlock(&to_io_queue_lock);

    // then save current task context and swap context to c_exec
    getcontext(&(tcb->thread_context));
    pthread_mutex_lock(&wait_queue_lock);
    struct queue_entry *node2 = queue_new_node(tcb);
    queue_insert_tail(&wait_queue, node2);
    pthread_mutex_unlock(&wait_queue_lock);
    swapcontext(&(tcb->thread_context), &cexec_context);

    // when I/O has finished with the task, moved it back from wait_queue to ready_queue, and when
    // C_exec will pop it from the c_exec queue and execute it, the context of the task will restart here
    return server_msg;
} // sut_read

void sut_close()
{
    // signals I_exec to close the socket connection
    ICB *icb = (ICB *)malloc(THREAD_STACK_SIZE);
    icb->id = 3;
    pthread_mutex_lock(&to_io_queue_lock);
    struct queue_entry *node = queue_new_node(icb);
    queue_insert_tail(&to_io_queue, node);
    pthread_mutex_unlock(&to_io_queue_lock);
} // sut_close

void sut_shutdown()
{
    // notify c_exec user will be ready to shutdown once tasks have completed
    pthread_mutex_lock(&shutdown_lock);
    shutdown_flag = true;
    pthread_join(iexec_handle, NULL);
    pthread_join(cexec_handle, NULL);
    pthread_mutex_unlock(&shutdown_lock);
} // sut_shutdown

/////////////////////// CODE BELOW IS FROM ASSIGNMENT 1 (taken from a1_lib.c) //////////////////////////////////////////

int create_server(const char *host, uint16_t port, int *sockfd)
{
    struct sockaddr_in server_address = {0};

    // create TCP socket
    *sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (*sockfd < 0)
    {
        perror("Failed to create a new socket\n");
        return -1;
    }

    // set options
    int opt = 1;
    if (setsockopt(*sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("Failed to set options for socket\n");
        return -1;
    }

    // bind to an address
    server_address.sin_family = AF_INET;
    inet_pton(AF_INET, host, &(server_address.sin_addr.s_addr));
    server_address.sin_port = htons(port);
    if (bind(*sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Failed to bind to an address\n");
        return -1;
    }

    // start listening
    if (listen(*sockfd, BACKLOG_SIZE) < 0)
    {
        perror("Failed to listen to socket\n");
        return -1;
    }

    return 0;
}

int accept_connection(int sockfd, int *clientfd)
{
    struct sockaddr_in connection_address = {0};
    socklen_t addrlen = sizeof(connection_address);

    // wait for a new connection on the server socket and accept it
    *clientfd = accept(sockfd, (struct sockaddr *)&connection_address, &addrlen);
    if (*clientfd < 0)
    {
        perror("Failed to accept client connection\n");
        EXIT_FAILURE;
    }
    return 0;
}

int connect_to_server(const char *host, uint16_t port, int *sockfd)
{
    struct sockaddr_in server_address = {0};

    // create a new socket
    *sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (*sockfd < 0)
    {
        perror("Failed to create a new socket\n");
        return -1;
    }

    // connect to server
    server_address.sin_family = AF_INET;
    inet_pton(AF_INET, host, &(server_address.sin_addr.s_addr));
    server_address.sin_port = htons(port);
    if (connect(*sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Failed to connect to server\n");
        return -1;
    }
    return 0;
}

ssize_t send_message(int sockfd, const char *buf, size_t len)
{
    return send(sockfd, buf, len, 0);
}

ssize_t recv_message(int sockfd, char *buf, size_t len)
{
    return recv(sockfd, buf, len, 0);
}
