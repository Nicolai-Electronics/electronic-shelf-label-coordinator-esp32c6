#include <esp_err.h>
#include <esp_ieee802154.h>
#include <esp_log.h>
#include <esp_phy_init.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <stdio.h>
#include <string.h>

#include "esl_proto.h"
#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"
#include "mbedtls/cipher.h"

uint8_t  esl_key[] = {0xD3, 0x06, 0xD9, 0x34, 0x8E, 0x29, 0xE5, 0xE3, 0x58, 0xBF, 0x29, 0x34, 0x81, 0x20, 0x02, 0xC1};
uint16_t esl_pan   = 0x4447;

#define FRAME_TYPE_BEACON       (0)
#define FRAME_TYPE_DATA         (1)
#define FRAME_TYPE_ACK          (2)
#define FRAME_TYPE_MAC_COMMAND  (3)
#define FRAME_TYPE_RESERVED     (4)
#define FRAME_TYPE_MULTIPURPOSE (5)
#define FRAME_TYPE_FRAGMENT     (6)
#define FRAME_TYPE_EXTENDED     (7)

#define ADDR_MODE_NONE     (0)  // PAN ID and address fields are not present
#define ADDR_MODE_RESERVED (1)  // Reseved
#define ADDR_MODE_SHORT    (2)  // Short address (16-bit)
#define ADDR_MODE_LONG     (3)  // Extended address (64-bit)

#define FRAME_TYPE_BEACON  (0)
#define FRAME_TYPE_DATA    (1)
#define FRAME_TYPE_ACK     (2)
#define FRAME_TYPE_MAC_CMD (3)

typedef struct mac_fcs {
    uint8_t frameType                  : 3;
    uint8_t secure                     : 1;
    uint8_t framePending               : 1;
    uint8_t ackReqd                    : 1;
    uint8_t panIdCompressed            : 1;
    uint8_t rfu1                       : 1;
    uint8_t sequenceNumberSuppression  : 1;
    uint8_t informationElementsPresent : 1;
    uint8_t destAddrType               : 2;
    uint8_t frameVer                   : 2;
    uint8_t srcAddrType                : 2;
} mac_fcs_t;

static const char* RADIO_TAG = "802.15.4 radio";

#define AES_CCM_MIC_SIZE   4
#define AES_CCM_NONCE_SIZE 13
#define MACFMT             "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x"
#define MACCVT(x)                                                                                                                          \
    ((const uint8_t*) (x))[0], ((const uint8_t*) (x))[1], ((const uint8_t*) (x))[2], ((const uint8_t*) (x))[3], ((const uint8_t*) (x))[4], \
        ((const uint8_t*) (x))[5], ((const uint8_t*) (x))[6], ((const uint8_t*) (x))[7]

mbedtls_ccm_context ctx;

void parse_esl_packet(uint8_t* data, uint8_t length, uint8_t* src_addr, uint8_t* dst_addr) {
    uint8_t packet_type = data[0];
    length -= 1;
    uint8_t* packet = &data[1];

    printf("[" MACFMT "] to [" MACFMT "]: ", MACCVT(src_addr), MACCVT(dst_addr));

    switch (packet_type) {
        case PKT_ASSOC_REQ:
            {
                if (length < sizeof(struct TagInfo)) {
                    printf("Assoc request, too short\n");
                    return;
                }
                struct TagInfo* tagInfo = (struct TagInfo*) packet;
                printf("Assoc request  proto v%u, sw v%llu, hw %04x, batt %u mV, w %u px (%u mm), h %u px (%u mm), c %04x, maxWait %u ms, screenType %u",
                       tagInfo->protoVer, tagInfo->state.swVer, tagInfo->state.hwType, tagInfo->state.batteryMv, tagInfo->screenPixWidth,
                       tagInfo->screenMmWidth, tagInfo->screenPixHeight, tagInfo->screenMmHeight, tagInfo->compressionsSupported, tagInfo->maxWaitMsec,
                       tagInfo->screenType);
                break;
            }
        case PKT_ASSOC_RESP:
            {
                if (length < sizeof(struct AssocInfo)) {
                    printf("Assoc response, too short\n");
                    return;
                }
                struct AssocInfo* assocInfo = (struct AssocInfo*) packet;
                printf(
                    "Assoc response: checkin delay %lu, retry delay %lu, failedCheckinsTillBlank %u, failedCheckinsTillDissoc %u, newKey %08lx %08lx %08lx "
                    "%08lx",
                    assocInfo->checkinDelay, assocInfo->retryDelay, assocInfo->failedCheckinsTillBlank, assocInfo->failedCheckinsTillDissoc,
                    assocInfo->newKey[0], assocInfo->newKey[1], assocInfo->newKey[2], assocInfo->newKey[3]);
                break;
            }
        case PKT_CHECKIN:
            {
                if (length < sizeof(struct CheckinInfo)) {
                    printf("Checkin, too short\n");
                    return;
                }
                struct CheckinInfo* checkinInfo = (struct CheckinInfo*) packet;
                printf("Checkin: sw v%llu, hw %04x, batt %u mV, LQI %u, RSSI %d, temperature %u *c", checkinInfo->state.swVer, checkinInfo->state.hwType,
                       checkinInfo->state.batteryMv, checkinInfo->lastPacketLQI, checkinInfo->lastPacketRSSI, checkinInfo->temperature - CHECKIN_TEMP_OFFSET);

                break;
            }
        case PKT_CHECKOUT:
            {
                if (length < sizeof(struct PendingInfo)) {
                    printf("Checkout, too short\n");
                    return;
                }
                struct PendingInfo* pendingInfo = (struct PendingInfo*) packet;
                printf("Checkout: image version %llu, image size %lu, os version %llu, os size %lu", pendingInfo->imgUpdateVer, pendingInfo->imgUpdateSize,
                       pendingInfo->osUpdateVer, pendingInfo->osUpdateSize);
                break;
            }
        case PKT_CHUNK_REQ:
            {
                if (length < sizeof(struct ChunkReqInfo)) {
                    printf("Chunk request, too short\n");
                    return;
                }
                struct ChunkReqInfo* chunkReqInfo = (struct ChunkReqInfo*) packet;
                printf("Chunk request: version %llu, offset %lu, len %u, os update %s", chunkReqInfo->versionRequested, chunkReqInfo->offset, chunkReqInfo->len,
                       chunkReqInfo->osUpdatePlz ? "yes" : "no");
                break;
            }
        case PKT_CHUNK_RESP:
            {
                if (length < sizeof(struct ChunkInfo)) {
                    printf("Chunk response, too short\n");
                    return;
                }
                struct ChunkInfo* chunkInfo = (struct ChunkInfo*) packet;
                printf("Chunk response: offset %lu, os update %s, ", chunkInfo->offset, chunkInfo->osUpdatePlz ? "yes" : "no");
                for (uint8_t idx = 0; idx < length - sizeof(struct ChunkInfo); idx++) {
                    printf("%02x", chunkInfo->data[idx]);
                }
                break;
            }
        default:
            {
                printf("Unknown ESL packet type (%u)", packet_type);
            }
    }
    printf("\n");
}

void decode_packet(uint8_t* header, uint8_t header_length, uint8_t* data, uint8_t data_length, uint8_t* src_addr, uint8_t* dst_addr) {
    uint8_t nonce[AES_CCM_NONCE_SIZE] = {0};
    memcpy(nonce, data + data_length - 4, 4);
    for (uint8_t idx = 0; idx < 8; idx++) {
        nonce[4 + idx] = src_addr[7 - idx];
    }
    uint8_t* ciphertext        = &data[0];
    uint8_t  ciphertext_length = data_length - 4 - 4;  // 4 bytes tag, 4 bytes nonce
    uint8_t* tag               = &data[ciphertext_length];
    uint8_t  tag_length        = 4;

    uint8_t decoded[256];

    int ret = mbedtls_ccm_auth_decrypt(&ctx, ciphertext_length, nonce, AES_CCM_NONCE_SIZE, header, header_length, ciphertext, decoded, tag, tag_length);

    if (ret != 0) {
        ESP_EARLY_LOGE(RADIO_TAG, "Failed to decrypt packet, rc = %d", ret);
        return;
    }

    parse_esl_packet(decoded, ciphertext_length, src_addr, dst_addr);
}

void handle_packet(uint8_t* packet, uint8_t packet_length) {
    if (packet_length < sizeof(mac_fcs_t)) return;  // Can't be a packet if it's shorter than the frame control field

    uint8_t position = 0;

    mac_fcs_t* fcs = (mac_fcs_t*) &packet[position];
    position += sizeof(uint16_t);

    /*
    ESP_EARLY_LOGI(RADIO_TAG, "Frame type:                   %x", fcs->frameType);
    ESP_EARLY_LOGI(RADIO_TAG, "Security Enabled:             %s", fcs->secure ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "Frame pending:                %s", fcs->framePending ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "Acknowledge request:          %s", fcs->ackReqd ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "PAN ID Compression:           %s", fcs->panIdCompressed ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "Reserved:                     %s", fcs->rfu1 ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "Sequence Number Suppression:  %s", fcs->sequenceNumberSuppression ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "Information Elements Present: %s", fcs->informationElementsPresent ? "True" : "False");
    ESP_EARLY_LOGI(RADIO_TAG, "Destination addressing mode:  %x", fcs->destAddrType);
    ESP_EARLY_LOGI(RADIO_TAG, "Frame version:                %x", fcs->frameVer);
    ESP_EARLY_LOGI(RADIO_TAG, "Source addressing mode:       %x", fcs->srcAddrType);
    */

    if (fcs->panIdCompressed == false) {
        // ESP_EARLY_LOGE(RADIO_TAG, "PAN identifier not compressed, ignoring packet");
        // return;
    }

    if (fcs->rfu1) {
        ESP_EARLY_LOGE(RADIO_TAG, "Reserved field 1 is set, ignoring packet");
        return;
    }

    if (fcs->sequenceNumberSuppression) {
        ESP_EARLY_LOGE(RADIO_TAG, "Sequence number suppressed, ignoring packet");
        return;
    }

    if (fcs->informationElementsPresent) {
        ESP_EARLY_LOGE(RADIO_TAG, "Information elements present, ignoring packet");
        return;
    }

    if (fcs->frameVer != 0x0) {
        ESP_EARLY_LOGE(RADIO_TAG, "Unsupported frame version, ignoring packet");
        return;
    }

    switch (fcs->frameType) {
        case FRAME_TYPE_BEACON:
            {
                // ESP_EARLY_LOGI(RADIO_TAG, "Beacon");
                break;
            }
        case FRAME_TYPE_DATA:
            {
                uint8_t sequence_number = packet[position];
                position += sizeof(uint8_t);
                // ESP_EARLY_LOGI(RADIO_TAG, "Data (%u)", sequence_number);

                uint16_t pan_id         = 0;
                uint8_t  dst_addr[8]    = {0};
                uint8_t  src_addr[8]    = {0};
                uint16_t short_dst_addr = 0;
                uint16_t short_src_addr = 0;
                bool     broadcast      = false;

                switch (fcs->destAddrType) {
                    case ADDR_MODE_NONE:
                        {
                            // ESP_EARLY_LOGI(RADIO_TAG, "Without PAN ID or address field");
                            break;
                        }
                    case ADDR_MODE_SHORT:
                        {
                            pan_id = *((uint16_t*) &packet[position]);
                            position += sizeof(uint16_t);
                            short_dst_addr = *((uint16_t*) &packet[position]);
                            position += sizeof(uint16_t);
                            if (pan_id == 0xFFFF && short_dst_addr == 0xFFFF) {
                                broadcast = true;
                                pan_id    = *((uint16_t*) &packet[position]);  // srcPan
                                position += sizeof(uint16_t);
                                // ESP_EARLY_LOGI(RADIO_TAG, "Broadcast on PAN %04x", pan_id);
                            } else {
                                // ESP_EARLY_LOGI(RADIO_TAG, "On PAN %04x to short address %04x", pan_id, short_dst_addr);
                            }
                            break;
                        }
                    case ADDR_MODE_LONG:
                        {
                            pan_id = *((uint16_t*) &packet[position]);
                            position += sizeof(uint16_t);
                            for (uint8_t idx = 0; idx < sizeof(dst_addr); idx++) {
                                dst_addr[idx] = packet[position + sizeof(dst_addr) - 1 - idx];
                            }
                            position += sizeof(dst_addr);
                            // ESP_EARLY_LOGI(RADIO_TAG, "On PAN %04x to long address %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", pan_id, dst_addr[0],
                            // dst_addr[1], dst_addr[2], dst_addr[3], dst_addr[4], dst_addr[5], dst_addr[6], dst_addr[7]);
                            break;
                        }
                    default:
                        {
                            ESP_EARLY_LOGE(RADIO_TAG, "With reserved destination address type, ignoring packet");
                            return;
                        }
                }

                switch (fcs->srcAddrType) {
                    case ADDR_MODE_NONE:
                        {
                            // ESP_EARLY_LOGI(RADIO_TAG, "Originating from the PAN coordinator");
                            break;
                        }
                    case ADDR_MODE_SHORT:
                        {
                            short_src_addr = *((uint16_t*) &packet[position]);
                            position += sizeof(uint16_t);
                            // ESP_EARLY_LOGI(RADIO_TAG, "Originating from short address %04x", short_src_addr);
                            break;
                        }
                    case ADDR_MODE_LONG:
                        {
                            for (uint8_t idx = 0; idx < sizeof(src_addr); idx++) {
                                src_addr[idx] = packet[position + sizeof(src_addr) - 1 - idx];
                            }
                            position += sizeof(src_addr);
                            // ESP_EARLY_LOGI(RADIO_TAG, "Originating from long address %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", src_addr[0], src_addr[1],
                            // src_addr[2], src_addr[3], src_addr[4], src_addr[5], src_addr[6], src_addr[7]);
                            break;
                        }
                    default:
                        {
                            ESP_EARLY_LOGE(RADIO_TAG, "With reserved source address type, ignoring packet");
                            return;
                        }
                }

                uint8_t* header        = &packet[0];
                uint8_t  header_length = position;
                uint8_t* data          = &packet[position];
                uint8_t  data_length   = packet_length - position - sizeof(uint16_t);
                position += data_length;

                // ESP_EARLY_LOGI(RADIO_TAG, "Data length: %u", data_length);

                uint16_t checksum = *((uint16_t*) &packet[position]);

                // ESP_EARLY_LOGI(RADIO_TAG, "Checksum: %04x", checksum);

                ESP_EARLY_LOGI(RADIO_TAG, "PAN %04x S %04x " MACFMT " D %04x " MACFMT " %s", pan_id, short_src_addr, MACCVT(src_addr), short_dst_addr,
                               MACCVT(dst_addr), broadcast ? "BROADCAST" : "");

                if (broadcast)
                    for (uint8_t idx = 0; idx < 8; idx++) dst_addr[idx] = 0xFF;

                if (pan_id != esl_pan) return;  // Filter, only process electronic shelf label packets

                decode_packet(header, header_length, data, data_length, src_addr, dst_addr);
                break;
            }
        case FRAME_TYPE_ACK:
            {
                uint8_t sequence_number = packet[position++];
                // ESP_EARLY_LOGI(RADIO_TAG, "Ack (%u)", sequence_number);
                break;
            }
        default:
            {
                // ESP_EARLY_LOGE(RADIO_TAG, "Packet ignored because of frame type (%u)", fcs->frameType);
                break;
            }
    }
}

typedef struct {
    uint8_t length;
    uint8_t data[256];
} packet_t;

QueueHandle_t packet_rx_queue = NULL;

void esp_ieee802154_receive_done(uint8_t* frame, esp_ieee802154_frame_info_t* frame_info) {
    static packet_t packet;
    packet.length = frame[0];
    memcpy(packet.data, &frame[1], packet.length);
    xQueueSendFromISR(packet_rx_queue, (void*) &packet, pdFALSE);
}

void esp_ieee802154_energy_detect_done(int8_t power) { ESP_EARLY_LOGI(RADIO_TAG, "ed_scan_rss_value: %d dB", power); }

void esp_ieee802154_transmit_sfd_done(uint8_t* frame) { ESP_EARLY_LOGI(RADIO_TAG, "tx sfd done, Radio state: %d", esp_ieee802154_get_state()); }

void esp_ieee802154_receive_sfd_done(void) {
    // ESP_EARLY_LOGI(RADIO_TAG, "rx sfd done, Radio state: %d", esp_ieee802154_get_state());
}

void esp_ieee802154_transmit_failed(const uint8_t* frame, esp_ieee802154_tx_error_t error) {
    ESP_EARLY_LOGI(RADIO_TAG, "the Frame Transmission failed, Failure reason: %d", error);
}

// static void ieee802154_task(void *pvParameters) {
//     ESP_LOGW(RADIO_TAG, "Radio main loop returned, terminating radio task");
//     vTaskDelete(NULL);
// }

static const char* TAG = "main";

static void initialize_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting NVS...");
    initialize_nvs();

    ESP_LOGI(TAG, "Initializing mbedtls...");

    mbedtls_ccm_init(&ctx);
    int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, esl_key, sizeof(esl_key) * 8);

    if (ret != 0) {
        ESP_EARLY_LOGE(RADIO_TAG, "Failed to set key, rc = %d", ret);
        return;
    }

    ESP_LOGI(TAG, "Initializing radio...");

    packet_rx_queue = xQueueCreate(8, 257);

    esp_ieee802154_enable();
    esp_phy_enable();

    esp_ieee802154_set_coordinator(true);
    esp_ieee802154_set_channnel(11);
    esp_ieee802154_set_panid(esl_pan);
    esp_ieee802154_receive();
    esp_ieee802154_set_rx_when_idle(true);

    // ESP_LOGI(TAG, "Starting radio task...");
    // xTaskCreate(ieee802154_task, RADIO_TAG, 4096, NULL, 5, NULL);

    while (true) {
        static packet_t packet;
        xQueueReceive(packet_rx_queue, (void*) &packet, portMAX_DELAY);
        handle_packet(packet.data, packet.length);
    }

    mbedtls_ccm_free(&ctx);
}