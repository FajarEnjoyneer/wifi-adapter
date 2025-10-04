/* main.c
 * ESP32-S3 WiFi client -> USB ECM/RNDIS dongle example (ESP-IDF v5.5)
 *
 * - Provides TinyUSB descriptor pointers (desc_device, desc_fs_configuration) via externs
 * - Supplies string table to tinyusb via tusb_cfg.descriptor.string
 * - Robust esp-netif DHCP stop + set IP with retries and lwIP fallback
 * - TinyUSB <-> lwIP glue callbacks (tud_network_init_cb, tud_network_mac_address, tud_network_recv_cb)
 *
 * Requirements:
 * - main/tusb_desc.c must export const tusb_desc_device_t desc_device;
 *   and const uint8_t desc_fs_configuration[]; (no tud_descriptor_* functions here)
 * - main/CMakeLists.txt must include "tusb_desc.c" in SRCS
 * - In menuconfig ensure TinyUSB auto-descriptor is disabled if present
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_err.h"

#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_netif_ip_addr.h"   // esp_ip4addr_aton
#include "esp_wifi.h"

#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "esp_netif_net_stack.h" // esp_netif_get_netif_impl

#include "tinyusb.h"
#include "tusb.h"

/* Externs: descriptors provided in main/tusb_desc.c (descriptor-only, no tud_* callbacks) */
extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_fs_configuration[];

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "OPT-WIFII"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "qwertyyu"
#endif

static const char *TAG = "usb_wifi_dongle";

/* esp-netif handles */
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *usb_netif = NULL;

/* ------------------------------ TinyUSB string table ------------------------------
   Index 0 is reserved (language ID). Fill manufacturer/product/serial and MAC string.
   The wrapper expects 'const char *string_table[]' style; assign to tusb_cfg.descriptor.string.
   ---------------------------------------------------------------------------------*/
static const char *tusb_strings[] = {
    "",                            /* 0: reserved - language (wrapper will add langid) */
    "Espressif",                   /* 1: iManufacturer */
    "ESP32-S3 ECM Dongle",         /* 2: iProduct */
    "esp32s3-001",                 /* 3: iSerialNumber */
    "001122334455"                 /* 4: iMAC string used by Ethernet functional descriptor (12 hex digits) */
};

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ------------------------------ WiFi event handlers ------------------------------ */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> connecting");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

static void got_ip_handler(void* arg, esp_event_base_t event_base,
                           int32_t event_id, void* event_data)
{
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));

#if CONFIG_LWIP_IPV4_NAPT
    if (sta_netif) {
        esp_err_t rc = esp_netif_napt_enable(sta_netif);
        ESP_LOGI(TAG, "esp_netif_napt_enable returned %d (%s)", rc, esp_err_to_name(rc));
    } else {
        ESP_LOGW(TAG, "STA netif not available to enable NAPT");
    }
#endif
}

/* ------------------------------ WiFi init (STA) ------------------------------ */
static void init_wifi_sta(void)
{
    ESP_LOGI(TAG, "Initializing WiFi STA");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create default wifi STA netif and store handle */
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGW(TAG, "Failed to create default wifi STA netif");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password)-1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started: connecting to SSID '%s'", CONFIG_WIFI_SSID);
}

/* ------------------------------ TinyUSB <-> netif glue callbacks ------------------------------ */

/* Local MAC for USB network (locally administered) */
static uint8_t s_usb_mac[6] = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };

/* Called by TinyUSB when network interface initialized (link up) */
void tud_network_init_cb(void)
{
    ESP_LOGI(TAG, "tud_network_init_cb called (USB network interface initialized)");
}

/* Return pointer to MAC address */
const uint8_t* tud_network_mac_address(void)
{
    return s_usb_mac;
}

/* Called when a frame arrives from USB host (USB -> device).
   We hand the frame into lwIP netif via netif->input() */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (!src || size == 0) return false;

    if (!usb_netif) {
        ESP_LOGW(TAG, "tud_network_recv_cb: usb_netif is NULL");
        return false;
    }

    struct netif *lwip_netif = esp_netif_get_netif_impl(usb_netif);
    if (!lwip_netif) {
        ESP_LOGW(TAG, "tud_network_recv_cb: underlying lwIP netif not ready");
        return false;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) {
        ESP_LOGW(TAG, "tud_network_recv_cb: pbuf_alloc failed for size %u", size);
        return false;
    }

    /* copy into pbuf chain */
    if (p->len >= size) {
        memcpy(p->payload, src, size);
    } else {
        uint16_t copied = 0;
        struct pbuf *q = p;
        while (q && copied < size) {
            uint16_t copy_len = (size - copied) > q->len ? q->len : (size - copied);
            memcpy(q->payload, src + copied, copy_len);
            copied += copy_len;
            q = q->next;
        }
    }

    /* Direct deliver to lwIP. If your port requires tcpip thread, replace with tcpip_try_callback/tcpip_callback_with_block */
    err_t res = lwip_netif->input(p, lwip_netif);
    if (res != ERR_OK) {
        ESP_LOGW(TAG, "tud_network_recv_cb: netif->input returned %d", res);
        pbuf_free(p);
        return false;
    }
    return true;
}

/* ------------------------------ TinyUSB init + USB netif create ------------------------------ */
static void tinyusb_init_and_create_usb_netif(void)
{
    ESP_LOGI(TAG, "Installing TinyUSB driver (with custom descriptors & strings)");

    tinyusb_config_t tusb_cfg;
    memset(&tusb_cfg, 0, sizeof(tusb_cfg));

    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    tusb_cfg.phy.skip_setup = false;
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;

    /* Task config */
    tusb_cfg.task.size = 4096;
    tusb_cfg.task.priority = 5;
    tusb_cfg.task.xCoreID = 0;

    /* Provide descriptor arrays from tusb_desc.c */
    tusb_cfg.descriptor.device = &desc_device;
    tusb_cfg.descriptor.qualifier = NULL;
    tusb_cfg.descriptor.full_speed_config = desc_fs_configuration;
    tusb_cfg.descriptor.high_speed_config = NULL;

    /* Provide string table to wrapper so it can create string descriptors */
    tusb_cfg.descriptor.string = tusb_strings;
    tusb_cfg.descriptor.string_count = (uint8_t) ARRAY_SIZE(tusb_strings);

    tusb_cfg.event_cb = NULL;
    tusb_cfg.event_arg = NULL;

    /* Install; wrapper will call tinyusb_descriptors_set internally */
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s (%d)", esp_err_to_name(err), err);
        return;
    }
    ESP_LOGI(TAG, "TinyUSB Driver installed on port %d", tusb_cfg.port);

    /* give driver a moment */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* create esp-netif for USB using ETH template */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    usb_netif = esp_netif_new(&cfg);
    if (!usb_netif) {
        ESP_LOGE(TAG, "Failed to create USB netif");
        return;
    }

    /* debug: print lwIP netif if attached */
    struct netif *maybe_netif = esp_netif_get_netif_impl(usb_netif);
    if (maybe_netif) {
        ESP_LOGI(TAG, "Underlying lwIP netif found: name='%c%c' num=%d flags=0x%08x",
                 maybe_netif->name[0], maybe_netif->name[1], maybe_netif->num, maybe_netif->flags);
    } else {
        ESP_LOGI(TAG, "No underlying lwIP netif attached yet (will try to set IP/fallback).");
    }

    /* prepare IP info */
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr      = esp_ip4addr_aton("192.168.42.1");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ip_info.gw.addr      = esp_ip4addr_aton("192.168.42.1");

    /* Robust DHCP stop + set IP sequence with retries */
    esp_err_t rc = ESP_FAIL;
    const int max_stop_retries = 8;
    for (int i = 0; i < max_stop_retries; ++i) {
        rc = esp_netif_dhcps_stop(usb_netif);
        if (rc == ESP_OK) {
            ESP_LOGI(TAG, "esp_netif_dhcps_stop OK on attempt %d", i+1);
            break;
        }
        ESP_LOGW(TAG, "esp_netif_dhcps_stop attempt %d returned %s (%d), retrying...", i+1, esp_err_to_name(rc), rc);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    if (rc == ESP_OK) {
        /* try set ip with retries; if DHCP_NOT_STOPPED appears, attempt dhcps_stop again and retry */
        const int max_set_retries = 8;
        bool set_ok = false;
        for (int j = 0; j < max_set_retries; ++j) {
            rc = esp_netif_set_ip_info(usb_netif, &ip_info);
            if (rc == ESP_OK) {
                ESP_LOGI(TAG, "esp_netif_set_ip_info OK on try %d", j+1);
                set_ok = true;
                break;
            }
            if (rc == ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED) {
                ESP_LOGW(TAG, "esp_netif_set_ip_info returned DHCP_NOT_STOPPED on try %d. Calling dhcps_stop and retrying...", j+1);
                esp_err_t s = esp_netif_dhcps_stop(usb_netif);
                ESP_LOGI(TAG, "esp_netif_dhcps_stop returned %s (%d) during recovery", esp_err_to_name(s), s);
                vTaskDelay(pdMS_TO_TICKS(150));
                continue;
            }
            ESP_LOGW(TAG, "esp_netif_set_ip_info attempt %d returned %s (%d), retrying...", j+1, esp_err_to_name(rc), rc);
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        if (!set_ok) {
            ESP_LOGW(TAG, "esp_netif_set_ip_info failed after retries: %s (%d). Will attempt lwIP fallback.", esp_err_to_name(rc), rc);
            struct netif *lwip_netif = esp_netif_get_netif_impl(usb_netif);
            if (lwip_netif) {
                ip4_addr_t ip, nm, gw;
                IP4_ADDR(&ip, 192,168,42,1);
                IP4_ADDR(&nm, 255,255,255,0);
                IP4_ADDR(&gw, 192,168,42,1);
                netif_set_addr(lwip_netif, &ip, &nm, &gw);
                ESP_LOGI(TAG, "Fallback: set lwIP netif addr to 192.168.42.1/24");
            } else {
                ESP_LOGW(TAG, "Fallback: could not obtain lwIP netif for usb_netif (will still try to start dhcp server)");
            }
        }
    } else {
        /* dhcps_stop never succeeded — fallback immediately to lwIP netif if possible */
        ESP_LOGW(TAG, "esp_netif_dhcps_stop failed after retries: %s (%d). Will attempt lwIP fallback.", esp_err_to_name(rc), rc);
        struct netif *lwip_netif = esp_netif_get_netif_impl(usb_netif);
        if (lwip_netif) {
            ip4_addr_t ip, nm, gw;
            IP4_ADDR(&ip, 192,168,42,1);
            IP4_ADDR(&nm, 255,255,255,0);
            IP4_ADDR(&gw, 192,168,42,1);
            netif_set_addr(lwip_netif, &ip, &nm, &gw);
            ESP_LOGI(TAG, "Fallback: set lwIP netif addr to 192.168.42.1/24");
        } else {
            ESP_LOGW(TAG, "Fallback: could not obtain lwIP netif for usb_netif");
        }
    }

    /* start DHCP server on USB interface */
    rc = esp_netif_dhcps_start(usb_netif);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_dhcps_start returned %s (%d). Host may still be able to use static IP.", esp_err_to_name(rc), rc);
    } else {
        ESP_LOGI(TAG, "USB DHCP server started");
    }

    ESP_LOGI(TAG, "TinyUSB initialized and USB netif ready. Host should see a USB ethernet interface (usb0/enx...).");
}

/* ------------------------------ app_main ------------------------------ */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting USB WiFi dongle app");

    /* init WiFi STA (this also creates sta_netif handle) */
    init_wifi_sta();

    /* init TinyUSB and create USB network interface (usb_netif) */
    tinyusb_init_and_create_usb_netif();

    /* Guidance about NAPT: enable in menuconfig if you want NAT from USB->WiFi */
    if (!sta_netif) {
        ESP_LOGW(TAG, "STA netif handle is NULL; NAPT cannot be enabled automatically here.");
    } else {
        ESP_LOGI(TAG, "STA netif acquired - you may enable NAPT on it if your IDF provides the API.");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Running main loop...");
    }
}
