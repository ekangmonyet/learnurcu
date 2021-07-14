#define _LGPL_SOURCE
#include <urcu.h>
#include <urcu/rculfqueue.h>
#include <threads.h>

enum { Q_OK, Q_ERROR };

typedef struct { /* Queue node */
    void *value;
    struct cds_lfq_node_rcu node;
    struct rcu_head head;
} node_t;

#include <stdlib.h>
#include <string.h>

inline static node_t *_con_node_init(void *value)
{
    node_t *node = malloc(sizeof(node_t));
    if (!node)
        return NULL;

    node->value = value;
    cds_lfq_node_init_rcu(&node->node);

    return node;
}

/* Add element to queue. The client is responsible for freeing elementsput into
 * the queue afterwards. Returns Q_OK on success or Q_ERROR on failure.
 */
int con_push(struct cds_lfq_queue_rcu *restrict queue,
        void *restrict new_element)
{
    /* Prepare new node */
    node_t *node = _con_node_init(new_element);
    if (!node)
        return Q_ERROR;

    urcu_memb_read_lock();
    cds_lfq_enqueue_rcu(queue, &node->node);
    urcu_memb_read_unlock();
    return Q_OK;
}

static
void free_node(struct rcu_head *head) {
    node_t *node = caa_container_of(head, node_t, head);
    free(node);
}

/* Retrieve element and remove it from the queue.
 * Returns a pointer to the element previously pushed in or NULL of the queue is
 * emtpy.
 */
void *con_pop(struct cds_lfq_queue_rcu *queue)
{
    node_t *node;
    struct cds_lfq_node_rcu *qnode;

    urcu_memb_read_lock();
    qnode = cds_lfq_dequeue_rcu(queue);
    urcu_memb_read_unlock();
    if (!qnode) {
        return NULL;
    }
    node = caa_container_of(qnode, node_t, node);
    void *return_value = node->value;
    urcu_memb_call_rcu(&node->head, free_node);

    /* Free removed node and return */
    return return_value;
}

#include <assert.h>
#include <stdio.h>

#define N_PUSH_THREADS 4
#define N_POP_THREADS 4
#define NUM 1000000

/* This thread writes integers into the queue */
int push_thread(void *queue_ptr)
{
    struct cds_lfq_queue_rcu *queue = (struct cds_lfq_queue_rcu *)queue_ptr;

    /* Push ints into queue */
    for (int i = 0; i < NUM; ++i) {
        int *pushed_value = malloc(sizeof(int));
        *pushed_value = i;
        if (con_push(queue, pushed_value) != Q_OK)
            printf("Error pushing element %i\n", i);
    }

    thrd_exit(0);
}

/* This thread reads ints from the queue and frees them */
int pop_thread(void *queue_ptr)
{
    struct cds_lfq_queue_rcu *queue = (struct cds_lfq_queue_rcu *)queue_ptr;

    /* Read values from queue. Break loop on -1 */
    while (1) {
        int *popped_value = con_pop(queue);
        if (popped_value) {
            if (*popped_value == -1) {
                printf("exited\n");
                free(popped_value);
                break;
            }

            free(popped_value);
        }
    }

    thrd_exit(0);
}

int main()
{
    thrd_t push_threads[N_PUSH_THREADS], pop_threads[N_POP_THREADS];

    struct cds_lfq_queue_rcu queue;
    cds_lfq_init_rcu(&queue, urcu_memb_call_rcu);

    urcu_memb_register_thread();
    for (int i = 0; i < N_PUSH_THREADS; ++i) {
        if (thrd_create(&push_threads[i], push_thread, &queue) != thrd_success)
            printf("Error creating push thread %i\n", i);
    }

    for (int i = 0; i < N_POP_THREADS; ++i) {
        if (thrd_create(&pop_threads[i], pop_thread, &queue) != thrd_success)
            printf("Error creating pop thread %i\n", i);
    }

    for (int i = 0; i < N_PUSH_THREADS; ++i) {
        if (thrd_join(push_threads[i], NULL) != thrd_success)
            continue;
    }

    /* Push kill signals */
    for (int i = 0; i < N_POP_THREADS; ++i) {
        int *kill_signal = malloc(sizeof(int)); /* signal pop threads to exit */
        *kill_signal = -1;
        con_push(&queue, kill_signal);
    }

    for (int i = 0; i < N_POP_THREADS; ++i) {
        if (thrd_join(pop_threads[i], NULL) != thrd_success)
            continue;
    }

    cds_lfq_destroy_rcu(&queue);
    urcu_memb_unregister_thread();
    return 0;
}
