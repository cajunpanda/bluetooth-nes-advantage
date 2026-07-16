# Vendored BTstack v1.8.2 subset (Classic-only host for the Switch Pro transport).
# See README.md in this directory for provenance, layout, and license.
idf_component_register(
    SRCS
        "upstream/src/hci.c"
        "upstream/src/hci_cmd.c"
        "upstream/src/hci_dump.c"
        "upstream/src/hci_event.c"
        "upstream/src/hci_event_builder.c"
        "upstream/src/l2cap.c"
        "upstream/src/l2cap_signaling.c"
        "upstream/src/btstack_memory.c"
        "upstream/src/btstack_memory_pool.c"
        "upstream/src/btstack_run_loop.c"
        "upstream/src/btstack_run_loop_base.c"
        "upstream/src/btstack_linked_list.c"
        "upstream/src/btstack_ring_buffer.c"
        "upstream/src/btstack_util.c"
        "upstream/src/btstack_tlv.c"
        "upstream/src/btstack_hid.c"
        "upstream/src/btstack_hid_parser.c"
        "upstream/src/ad_parser.c"
        "upstream/src/classic/hid_device.c"
        "upstream/src/classic/sdp_server.c"
        "upstream/src/classic/sdp_util.c"
        "upstream/src/classic/device_id_server.c"
        "upstream/src/classic/btstack_link_key_db_tlv.c"
        "upstream/platform/freertos/btstack_run_loop_freertos.c"
        "upstream/platform/embedded/hci_dump_embedded_stdout.c"
        "port/hci_transport_esp32_vhci.c"
        "port/btstack_tlv_esp32.c"
        "port/btstack_hal_esp32.c"
    INCLUDE_DIRS
        "include"
        "upstream/src"
        "upstream/platform/freertos"
        "upstream/platform/embedded"
        "port/include"
    PRIV_REQUIRES bt nvs_flash
)

# Upstream C sources are not clean under IDF's default -Werror=format set.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-format)
