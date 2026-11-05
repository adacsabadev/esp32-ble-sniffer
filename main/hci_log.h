#pragma once

#include <stddef.h>
#include <stdint.h>

/* The "pkt" arguments below never include the H4 type byte. */
void hci_log_cmd(const char *dir, const uint8_t *pkt, size_t len);
void hci_log_evt(const char *dir, const uint8_t *pkt, size_t len);
void hci_log_acl(const char *dir, const uint8_t *pkt, size_t len);
void hci_log_hex(const char *tag, const uint8_t *data, size_t len);

/* Synchronous decoders, only called from the async log task (alog.c).
 * "data" holds at least min(len, 8, hex max) valid bytes. */
void hci_log_cmd_now(const char *dir, const uint8_t *pkt, size_t len);
void hci_log_evt_now(const char *dir, const uint8_t *pkt, size_t len);
void hci_log_acl_now(const char *dir, const uint8_t *pkt, size_t len);
void hci_log_hex_now(const char *tag, const uint8_t *data, size_t len);
