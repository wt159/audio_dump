#ifndef DUMP_PCM_QUEUE_H
#define DUMP_PCM_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "../include/dump_pcm_data.h"
#include "platform_config.h"

typedef struct {
    char key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char key2[DUMP_PCM_KEY_MAX_LEN + 1];
    uint32_t generation;
    char *buffer;
    uint32_t buffer_size;
    uint64_t enqueue_time;
} data_block_t;

typedef struct queue_node {
    data_block_t block;
    struct queue_node *next;
} queue_node_t;

typedef struct {
    queue_node_t *head;
    queue_node_t *tail;
    uint32_t count;
    uint64_t total_bytes;
    uint32_t max_blocks;
    uint64_t max_bytes;
} queue_t;

void queue_init(queue_t *q, uint32_t max_blocks, uint64_t max_bytes);
void queue_destroy(queue_t *q);
int queue_push(queue_t *q, const data_block_t *block);
int queue_pop(queue_t *q, data_block_t *block);
int queue_is_full(const queue_t *q);
int queue_is_empty(const queue_t *q);
uint32_t queue_count(const queue_t *q);
uint64_t queue_bytes(const queue_t *q);

#endif
