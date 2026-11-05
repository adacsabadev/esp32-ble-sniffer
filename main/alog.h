#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ALOG_HCI_CMD = 1,
    ALOG_HCI_EVT,
    ALOG_HCI_ACL,
    ALOG_HCI_HEX,
} alog_kind_t;

/* Installs a vprintf hook so every ESP_LOGx call only formats the line and queues it,
 * and starts a low priority task on core 1 that does the (blocking) UART output. */
void alog_start(void);

/* Queues a packet for decoding/printing by the log task. Never blocks; drops when full.
 * "dir" must be a string literal / static string. */
void alog_submit_hci(alog_kind_t kind, const char *dir, const uint8_t *data, size_t len);

/* Sequence number (assigned at submit time) of the record currently being printed. Gaps mean drops. */
uint32_t alog_current_seq(void);
