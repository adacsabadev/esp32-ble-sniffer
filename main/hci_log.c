#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "hci_log.h"
#include "alog.h"

static const char *TAG = "HCI";

typedef struct {
    uint16_t code;
    const char *name;
} name_entry_t;

static const name_entry_t s_opcodes[] = {
    {0x0406, "Disconnect"},
    {0x041D, "Read Remote Version Information"},
    {0x0C01, "Set Event Mask"},
    {0x0C03, "Reset"},
    {0x0C13, "Write Local Name"},
    {0x0C14, "Read Local Name"},
    {0x0C2D, "Read Transmit Power Level"},
    {0x0C31, "Set Controller To Host Flow Control"},
    {0x0C33, "Host Buffer Size"},
    {0x0C35, "Host Number Of Completed Packets"},
    {0x0C63, "Set Event Mask Page 2"},
    {0x0C6C, "Read LE Host Support"},
    {0x0C6D, "Write LE Host Support"},
    {0x1001, "Read Local Version Information"},
    {0x1002, "Read Local Supported Commands"},
    {0x1003, "Read Local Supported Features"},
    {0x1004, "Read Local Extended Features"},
    {0x1005, "Read Buffer Size"},
    {0x1009, "Read BD_ADDR"},
    {0x1405, "Read RSSI"},
    {0x2001, "LE Set Event Mask"},
    {0x2002, "LE Read Buffer Size"},
    {0x2003, "LE Read Local Supported Features"},
    {0x2005, "LE Set Random Address"},
    {0x2006, "LE Set Advertising Parameters"},
    {0x2007, "LE Read Advertising Channel Tx Power"},
    {0x2008, "LE Set Advertising Data"},
    {0x2009, "LE Set Scan Response Data"},
    {0x200A, "LE Set Advertising Enable"},
    {0x200B, "LE Set Scan Parameters"},
    {0x200C, "LE Set Scan Enable"},
    {0x200D, "LE Create Connection"},
    {0x200E, "LE Create Connection Cancel"},
    {0x200F, "LE Read Filter Accept List Size"},
    {0x2010, "LE Clear Filter Accept List"},
    {0x2011, "LE Add Device To Filter Accept List"},
    {0x2012, "LE Remove Device From Filter Accept List"},
    {0x2013, "LE Connection Update"},
    {0x2014, "LE Set Host Channel Classification"},
    {0x2015, "LE Read Channel Map"},
    {0x2016, "LE Read Remote Features"},
    {0x2017, "LE Encrypt"},
    {0x2018, "LE Rand"},
    {0x2019, "LE Enable Encryption"},
    {0x201A, "LE Long Term Key Request Reply"},
    {0x201B, "LE Long Term Key Request Negative Reply"},
    {0x201C, "LE Read Supported States"},
    {0x2022, "LE Set Data Length"},
    {0x2023, "LE Read Suggested Default Data Length"},
    {0x2024, "LE Write Suggested Default Data Length"},
    {0x2027, "LE Add Device To Resolving List"},
    {0x2028, "LE Remove Device From Resolving List"},
    {0x2029, "LE Clear Resolving List"},
    {0x202A, "LE Read Resolving List Size"},
    {0x202D, "LE Set Address Resolution Enable"},
    {0x202E, "LE Set Resolvable Private Address Timeout"},
    {0x202F, "LE Read Maximum Data Length"},
    {0x2030, "LE Read PHY"},
    {0x2031, "LE Set Default PHY"},
    {0x2032, "LE Set PHY"},
    {0x2036, "LE Set Extended Advertising Parameters"},
    {0x2037, "LE Set Extended Advertising Data"},
    {0x2038, "LE Set Extended Scan Response Data"},
    {0x2039, "LE Set Extended Advertising Enable"},
    {0x203A, "LE Read Maximum Advertising Data Length"},
    {0x203B, "LE Read Number Of Supported Advertising Sets"},
    {0x203C, "LE Remove Advertising Set"},
    {0x203D, "LE Clear Advertising Sets"},
    {0x2041, "LE Set Extended Scan Parameters"},
    {0x2042, "LE Set Extended Scan Enable"},
    {0x2043, "LE Extended Create Connection"},
};

static const name_entry_t s_events[] = {
    {0x05, "Disconnection Complete"},
    {0x08, "Encryption Change"},
    {0x0C, "Read Remote Version Information Complete"},
    {0x0E, "Command Complete"},
    {0x0F, "Command Status"},
    {0x10, "Hardware Error"},
    {0x13, "Number Of Completed Packets"},
    {0x30, "Encryption Key Refresh Complete"},
    {0x3E, "LE Meta"},
    {0x57, "Authenticated Payload Timeout Expired"},
    {0xFF, "Vendor Specific"},
};

static const name_entry_t s_le_subevents[] = {
    {0x01, "LE Connection Complete"},
    {0x02, "LE Advertising Report"},
    {0x03, "LE Connection Update Complete"},
    {0x04, "LE Read Remote Features Complete"},
    {0x05, "LE Long Term Key Request"},
    {0x06, "LE Remote Connection Parameter Request"},
    {0x07, "LE Data Length Change"},
    {0x08, "LE Read Local P-256 Public Key Complete"},
    {0x09, "LE Generate DHKey Complete"},
    {0x0A, "LE Enhanced Connection Complete"},
    {0x0B, "LE Directed Advertising Report"},
    {0x0C, "LE PHY Update Complete"},
    {0x0D, "LE Extended Advertising Report"},
    {0x0E, "LE Periodic Advertising Sync Established"},
    {0x0F, "LE Periodic Advertising Report"},
    {0x10, "LE Periodic Advertising Sync Lost"},
    {0x11, "LE Scan Timeout"},
    {0x12, "LE Advertising Set Terminated"},
    {0x13, "LE Scan Request Received"},
    {0x14, "LE Channel Selection Algorithm"},
};

static const char *lookup(const name_entry_t *table, size_t count, uint16_t code)
{
    for (size_t i = 0; i < count; i++) {
        if (table[i].code == code) {
            return table[i].name;
        }
    }
    return NULL;
}

#define LOOKUP(tbl, code) lookup(tbl, sizeof(tbl) / sizeof(tbl[0]), code)

void hci_log_hex_now(const char *tag, const uint8_t *data, size_t len)
{
#if CONFIG_DONGLE_LOG_HCI_HEX
    char line[3 * CONFIG_DONGLE_LOG_HEX_MAX_BYTES + 8];
    size_t shown = len < CONFIG_DONGLE_LOG_HEX_MAX_BYTES ? len : CONFIG_DONGLE_LOG_HEX_MAX_BYTES;
    size_t pos = 0;
    for (size_t i = 0; i < shown; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", data[i]);
    }
    if (shown < len) {
        snprintf(line + pos, sizeof(line) - pos, "...");
    }
    ESP_LOGV(TAG, "%s #%lu [%u]: %s", tag, (unsigned long)alog_current_seq(), (unsigned)len, line);
#else
    (void)tag; (void)data; (void)len;
#endif
}

static void log_pkt_hex(const char *dir, const char *kind, const uint8_t *pkt, size_t len)
{
    char tag[24];
    snprintf(tag, sizeof(tag), "%s %s", dir, kind);
    hci_log_hex_now(tag, pkt, len);
}

void hci_log_cmd_now(const char *dir, const uint8_t *pkt, size_t len)
{
#if CONFIG_DONGLE_LOG_HCI_DECODE
    if (len < 3) {
        ESP_LOGW(TAG, "%s CMD truncated (%u bytes)", dir, (unsigned)len);
        return;
    }
    uint16_t opcode = pkt[0] | (pkt[1] << 8);
    const char *name = LOOKUP(s_opcodes, opcode);
    ESP_LOGD(TAG, "%s CMD 0x%04X (OGF 0x%02X OCF 0x%03X) %s, plen=%u%s", dir, opcode, opcode >> 10,
             opcode & 0x3FF, name ? name : "<unknown>", pkt[2],
             (size_t)pkt[2] + 3 != len ? " [LENGTH MISMATCH]" : "");
#endif
    log_pkt_hex(dir, "CMD", pkt, len);
}

void hci_log_evt_now(const char *dir, const uint8_t *pkt, size_t len)
{
#if CONFIG_DONGLE_LOG_HCI_DECODE
    if (len < 2) {
        ESP_LOGW(TAG, "%s EVT truncated (%u bytes)", dir, (unsigned)len);
        return;
    }
    uint8_t code = pkt[0];
    uint8_t plen = pkt[1];
    const char *name = LOOKUP(s_events, code);
    if (code == 0x0E && len >= 6) {
        uint16_t opcode = pkt[3] | (pkt[4] << 8);
        const char *cn = LOOKUP(s_opcodes, opcode);
        ESP_LOGD(TAG, "%s EVT Command Complete: %s (0x%04X) status=0x%02X ncmd=%u plen=%u", dir,
                 cn ? cn : "<unknown>", opcode, pkt[5], pkt[2], plen);
    } else if (code == 0x0F && len >= 6) {
        uint16_t opcode = pkt[4] | (pkt[5] << 8);
        const char *cn = LOOKUP(s_opcodes, opcode);
        ESP_LOGD(TAG, "%s EVT Command Status: %s (0x%04X) status=0x%02X ncmd=%u", dir,
                 cn ? cn : "<unknown>", opcode, pkt[2], pkt[3]);
    } else if (code == 0x3E && len >= 3) {
        const char *sn = LOOKUP(s_le_subevents, pkt[2]);
        ESP_LOGD(TAG, "%s EVT LE Meta: %s (0x%02X) plen=%u", dir, sn ? sn : "<unknown>", pkt[2], plen);
    } else if (code == 0x05 && len >= 6) {
        ESP_LOGD(TAG, "%s EVT Disconnection Complete: status=0x%02X handle=0x%03X reason=0x%02X", dir, pkt[2],
                 (pkt[3] | (pkt[4] << 8)) & 0x0FFF, pkt[5]);
    } else {
        ESP_LOGD(TAG, "%s EVT 0x%02X %s plen=%u", dir, code, name ? name : "<unknown>", plen);
    }
#endif
    log_pkt_hex(dir, "EVT", pkt, len);
}

void hci_log_acl_now(const char *dir, const uint8_t *pkt, size_t len)
{
#if CONFIG_DONGLE_LOG_HCI_DECODE
    if (len < 4) {
        ESP_LOGW(TAG, "%s ACL truncated (%u bytes)", dir, (unsigned)len);
        return;
    }
    uint16_t hf = pkt[0] | (pkt[1] << 8);
    uint16_t dlen = pkt[2] | (pkt[3] << 8);
    ESP_LOGD(TAG, "%s ACL handle=0x%03X PB=%u BC=%u dlen=%u%s", dir, hf & 0x0FFF, (hf >> 12) & 3, hf >> 14, dlen,
             (size_t)dlen + 4 != len ? " [LENGTH MISMATCH]" : "");
#endif
    log_pkt_hex(dir, "ACL", pkt, len);
}

void hci_log_cmd(const char *dir, const uint8_t *pkt, size_t len) { alog_submit_hci(ALOG_HCI_CMD, dir, pkt, len); }
void hci_log_evt(const char *dir, const uint8_t *pkt, size_t len) { alog_submit_hci(ALOG_HCI_EVT, dir, pkt, len); }
void hci_log_acl(const char *dir, const uint8_t *pkt, size_t len) { alog_submit_hci(ALOG_HCI_ACL, dir, pkt, len); }
void hci_log_hex(const char *tag, const uint8_t *data, size_t len) { alog_submit_hci(ALOG_HCI_HEX, tag, data, len); }
