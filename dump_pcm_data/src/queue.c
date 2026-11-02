#include "queue.h"

#include <stdlib.h>
#include <string.h>

#include "key_utils.h"
#include "logger.h"

void queue_init(queue_t *q, uint32_t max_blocks, uint64_t max_bytes) {
    if (q == NULL) {
        return;
    }

    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    q->total_bytes = 0;
    q->max_blocks = max_blocks;
    q->max_bytes = max_bytes;
}

void queue_destroy(queue_t *q) {
    queue_node_t *node = NULL;

    if (q == NULL) {
        return;
    }

    node = q->head;
    while (node != NULL) {
        queue_node_t *next = node->next;
        free(node->block.buffer);
        free(node);
        node = next;
    }

    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    q->total_bytes = 0;
}

int queue_push(queue_t *q, const data_block_t *block) {
    queue_node_t *node = NULL;

    if (q == NULL || block == NULL || (block->buffer == NULL && block->buffer_size > 0)) {
        return -1;
    }

    if (q->count >= q->max_blocks || q->total_bytes + block->buffer_size > q->max_bytes) {
        LOGW("Queue full, dropping block: blocks=%u bytes=%lu", q->count, q->total_bytes);
        return -1;
    }

    node = malloc(sizeof(*node));
    if (node == NULL) {
        LOGE("Failed to allocate queue node");
        return -1;
    }

    memset(node, 0, sizeof(*node));
    dump_key_copy(node->block.key1, block->key1);
    dump_key_copy(node->block.key2, block->key2);
    node->block.generation = block->generation;
    node->block.buffer_size = block->buffer_size;
    node->block.enqueue_time = block->enqueue_time;
    if (block->buffer_size > 0) {
        node->block.buffer = malloc(block->buffer_size);
        if (node->block.buffer == NULL) {
            LOGE("Failed to allocate queue buffer");
            free(node);
            return -1;
        }
        memcpy(node->block.buffer, block->buffer, block->buffer_size);
    }

    if (q->tail == NULL) {
        q->head = node;
        q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }

    q->count++;
    q->total_bytes += block->buffer_size;
    return 0;
}

int queue_pop(queue_t *q, data_block_t *block) {
    queue_node_t *node = NULL;

    if (q == NULL || block == NULL || q->head == NULL) {
        return -1;
    }

    node = q->head;
    memset(block, 0, sizeof(*block));
    dump_key_copy(block->key1, node->block.key1);
    dump_key_copy(block->key2, node->block.key2);
    block->generation = node->block.generation;
    block->buffer = node->block.buffer;
    block->buffer_size = node->block.buffer_size;
    block->enqueue_time = node->block.enqueue_time;

    q->head = node->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }

    q->count--;
    q->total_bytes -= block->buffer_size;
    free(node);
    return 0;
}

int queue_is_full(const queue_t *q) {
    if (q == NULL) {
        return 0;
    }

    return q->count >= q->max_blocks || q->total_bytes >= q->max_bytes;
}

int queue_is_empty(const queue_t *q) {
    return (q == NULL || q->head == NULL) ? 1 : 0;
}

uint32_t queue_count(const queue_t *q) {
    return (q == NULL) ? 0 : q->count;
}

uint64_t queue_bytes(const queue_t *q) {
    return (q == NULL) ? 0 : q->total_bytes;
}
