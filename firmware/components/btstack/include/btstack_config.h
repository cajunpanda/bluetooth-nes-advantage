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

// HCI controller-to-host flow control is deliberately NOT enabled: this controller does not
// implement it. Read_Local_Supported_Commands octet 10 reads 0xFC - bit 0
// (Set_Controller_To_Host_Flow_Control) and bit 1 (Host_Buffer_Size) are both clear - and on the
// wire Host_Buffer_Size is refused ("0E 04 05 33 0C 11", status 0x11 Unsupported Feature or
// Parameter Value) at any parameters, while Set_Controller_To_Host_Flow_Control returns success as
// a stub. With the define on, BTstack ignored that refusal, believed flow control was live, and
// answered every inbound ACL packet with a Host_Number_Of_Completed_Packets the controller never
// asked for - taking priority over all other commands in hci_run(). The VHCI transport does its own
// backpressure in hci_ringbuffer, which is what actually protects us here.

// Logging goes through printf -> the board's serial console (benchmux-visible)
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// BR/EDR only. Deliberately NOT defined:
//   ENABLE_BLE                                    - BLE boots use Bluedroid
//   ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE     - HID uses basic mode only
//   ENABLE_L2CAP_INFORMATION_REQUESTS_ON_CONNECT  - a real Pro Controller is silent on the
//     ACL data plane until the console authenticates it; the Switch 2 drops pre-auth
//     chatterers. BTstack keeps this off by default - keep it that way.
#define ENABLE_CLASSIC

// Bonds: link keys via btstack_link_key_db_tlv on NVS (namespace "BTstack").
// One console + a couple of bench hosts is plenty.
#define NVM_NUM_LINK_KEYS 4

// Buffers: HID interrupt traffic is tiny (<= 50-byte reports, SDP answers < 500 B).
#define HCI_ACL_PAYLOAD_SIZE    (512 + 4)
#define HCI_HOST_ACL_PACKET_LEN 512
#define HCI_HOST_ACL_PACKET_NUM 8
// SCO is unused, but the length still has to be in the spec's 0x01..0xFF range: at 0 the
// controller rejects the whole Host_Buffer_Size command (seen on the wire: "0E 04 05 33 0C 11",
// status 0x11 Unsupported Feature or Parameter Value). BTstack ignores that failure and enables
// controller-to-host flow control anyway, leaving the controller believing we have no receive
// buffers at all. Costs no RAM: the transport's ring sizes the SCO term by PACKET_NUM, which is 0.
#define HCI_HOST_SCO_PACKET_LEN 255
#define HCI_HOST_SCO_PACKET_NUM 0

#endif
