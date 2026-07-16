// btstack_config.h - BTstack configuration for the Classic Switch Pro Controller transport.
//
// Classic-only on the original ESP32: the BLE gameplay and config/OTA boot modes stay on
// Bluedroid; exactly one host stack is initialized per boot on the shared VHCI controller.
// See components/btstack/README.md for what is vendored and why.

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features (FreeRTOS run loop on ESP-IDF)
#define HAVE_ASSERT
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_FREERTOS_INCLUDE_PREFIX
#define HAVE_FREERTOS_TASK_NOTIFICATIONS
#define HAVE_MALLOC

// Do NOT define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL: this controller does not implement it
// (Read_Local_Supported_Commands octet 10 = 0xFC, with both command bits clear) and refuses
// Host_Buffer_Size with status 0x11 at any parameters, while stubbing the enable out as success.
// BTstack takes the stub at face value and then answers every inbound ACL packet with a
// Host_Number_Of_Completed_Packets, which preempts every other command in hci_run(). The VHCI
// transport's hci_ringbuffer is the real backpressure.

// Logging goes through printf -> the board's serial console (benchmux-visible)
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// BR/EDR only. Deliberately NOT defined:
//   ENABLE_BLE                                    - BLE boots use Bluedroid
//   ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE     - HID uses basic mode only
//   ENABLE_L2CAP_INFORMATION_REQUESTS_ON_CONNECT  - a real Pro Controller is silent on the ACL
//     data plane until the host authenticates it. Off is also BTstack's default.
#define ENABLE_CLASSIC

// Bonds: link keys via btstack_link_key_db_tlv on NVS (namespace "BTstack").
// One console + a couple of bench hosts is plenty.
#define NVM_NUM_LINK_KEYS 4

// Buffers: HID interrupt traffic is tiny (<= 50-byte reports, SDP answers < 500 B).
#define HCI_ACL_PAYLOAD_SIZE    (512 + 4)
#define HCI_HOST_ACL_PACKET_LEN 512
#define HCI_HOST_ACL_PACKET_NUM 8
// SCO is unused. These only size the transport's ring buffer, whose SCO term is multiplied by
// PACKET_NUM - so at 0 they cost nothing.
#define HCI_HOST_SCO_PACKET_LEN 255
#define HCI_HOST_SCO_PACKET_NUM 0

#endif
