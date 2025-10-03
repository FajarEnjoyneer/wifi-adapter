/* main.c
 * ESP32-S3 WiFi client -> USB ECM/RNDIS dongle example (ESP-IDF v5.5)
 *
 * Notes:
 * - Provide tusb descriptors in main/tusb_desc.c (desc_device, desc_fs_configuration)
 * - This file sets those descriptors into tinyusb_config_t before installing driver.
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_err.h"

#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_netif_ip_addr.h"   // esp_ip4addr_aton (returns uint32_t)
#include "esp_wifi.h"


#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"   // optional, for tcpip_callback if needed
#include "esp_netif_net_stack.h" // for esp_netif_get_netif_impl

#include "tinyusb.h"
#include "tusb.h"

/* Externs: ensure these are implemented in main/tusb_desc.c and compiled into the project */
extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_fs_configuration[];

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "Angela"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "angela99ma88"
#endif

static const char *TAG = "usb_wifi_dongle";

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *usb_netif = NULL;

/* --- WiFi handlers --- */
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

/* --- WiFi init --- */
static void init_wifi_sta(void)
{
    ESP_LOGI(TAG, "Initializing WiFi STA");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default wifi STA netif and store handle
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

/* TinyUSB initialization and creation of USB netif.
 * IMPORTANT: we populate tusb_cfg.descriptor.* with pointers to our descriptors
 * (desc_device, desc_fs_configuration) so the esp_tinyusb wrapper will register them.
 */
static void tinyusb_init_and_create_usb_netif(void)
{
    ESP_LOGI(TAG, "Installing TinyUSB driver (with custom descriptors)");

    tinyusb_config_t tusb_cfg;
    memset(&tusb_cfg, 0, sizeof(tusb_cfg));

    /* Default port & PHY settings */
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    tusb_cfg.phy.skip_setup = false;
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;

    /* Task config: must be > 0 */
    tusb_cfg.task.size = 4096;
    tusb_cfg.task.priority = 5;
    tusb_cfg.task.xCoreID = 0;

    /* --- IMPORTANT: provide descriptor pointers from tusb_desc.c --- */
    tusb_cfg.descriptor.device = &desc_device;
    tusb_cfg.descriptor.qualifier = NULL;
    tusb_cfg.descriptor.full_speed_config = desc_fs_configuration;
    tusb_cfg.descriptor.high_speed_config = NULL;
    tusb_cfg.descriptor.string = NULL;
    tusb_cfg.descriptor.string_count = 0;

    tusb_cfg.event_cb = NULL;
    tusb_cfg.event_arg = NULL;

    /* Install driver (will call tinyusb_descriptors_set internally using provided descriptors) */
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s (%d)", esp_err_to_name(err), err);
        return;
    }
    ESP_LOGI(TAG, "TinyUSB Driver installed on port %d", tusb_cfg.port);

    // Give TinyUSB task & driver a short moment to attach netif/backend
    vTaskDelay(pdMS_TO_TICKS(200));

    /* === create esp-netif for USB (use ETH template) === */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    usb_netif = esp_netif_new(&cfg);
    if (!usb_netif) {
        ESP_LOGE(TAG, "Failed to create USB netif");
        return;
    }

    /* debug: print underlying lwIP netif (if any) */
    struct netif *maybe_netif = esp_netif_get_netif_impl(usb_netif);
    if (maybe_netif) {
        ESP_LOGI(TAG, "Underlying lwIP netif found: name='%c%c' num=%d flags=0x%08x",
                 maybe_netif->name[0], maybe_netif->name[1], maybe_netif->num, maybe_netif->flags);
    } else {
        ESP_LOGI(TAG, "No underlying lwIP netif attached yet (will continue and try to set IP/fallback).");
    }

    /* === prepare ip info (192.168.42.1/24) === */
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr      = esp_ip4addr_aton("192.168.42.1");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ip_info.gw.addr      = esp_ip4addr_aton("192.168.42.1");

    /* === stop DHCP server with retries (some netif take time to be ready) === */
    esp_err_t rc = ESP_FAIL;
    const int max_retries = 10;
    for (int i = 0; i < max_retries; ++i) {
        rc = esp_netif_dhcps_stop(usb_netif);
        if (rc == ESP_OK) {
            ESP_LOGI(TAG, "esp_netif_dhcps_stop OK on attempt %d", i+1);
            break;
        }
        // If DHCP not started yet or not stoppable, keep trying briefly
        ESP_LOGW(TAG, "esp_netif_dhcps_stop attempt %d returned %s (%d), retrying...", i+1, esp_err_to_name(rc), rc);
        vTaskDelay(pdMS_TO_TICKS(100)); // cumulative up to ~1s
    }

    if (rc != ESP_OK) {
        // Convert common DHCP_NOT_STOPPED into a non-fatal condition: try lwIP fallback
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
            ESP_LOGW(TAG, "Fallback: could not obtain lwIP netif for usb_netif (will still try to start dhcp server)");
        }
    } else {
        /* DHCP stopped successfully — set IP via esp-netif API */
        rc = esp_netif_set_ip_info(usb_netif, &ip_info);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_set_ip_info failed: %s (%d). Attempting lwIP fallback.", esp_err_to_name(rc), rc);
            struct netif *lwip_netif = esp_netif_get_netif_impl(usb_netif);
            if (lwip_netif) {
                ip4_addr_t ip, nm, gw;
                IP4_ADDR(&ip, 192,168,42,1);
                IP4_ADDR(&nm, 255,255,255,0);
                IP4_ADDR(&gw, 192,168,42,1);
                netif_set_addr(lwip_netif, &ip, &nm, &gw);
                ESP_LOGI(TAG, "Fallback: set lwIP netif addr to 192.168.42.1/24");
            }
        } else {
            ESP_LOGI(TAG, "esp_netif_set_ip_info OK");
        }
    }

    /* === start DHCP server on USB interface === */
    rc = esp_netif_dhcps_start(usb_netif);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_dhcps_start returned %s (%d). Host may still be able to use static IP.", esp_err_to_name(rc), rc);
    } else {
        ESP_LOGI(TAG, "USB DHCP server started");
    }

    ESP_LOGI(TAG, "TinyUSB initialized and USB netif ready. Host should see a USB ethernet interface (usb0/enx...).");
}

/* ---------------- TinyUSB network callbacks (glue to lwIP) -----------------*/
/* These callbacks are required by esp_tinyusb's net class implementation.
   They bridge TinyUSB <-> lwIP (esp-netif). */

static uint8_t s_usb_mac[6] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 }; // locally administered MAC (change if desired)

/* Called by TinyUSB when network interface is initialized (link up). */
void tud_network_init_cb(void)
{
    ESP_LOGI(TAG, "tud_network_init_cb called (USB network interface initialized)");
    // Optional: you could set link flags, trigger DHCP server start, etc.
}

/* Return pointer to 6-byte MAC address used for the USB network interface.
   TinyUSB will read this to provide MAC to host (RNDIS/ECM). */
const uint8_t* tud_network_mac_address(void)
{
    return s_usb_mac;
}

/* Called by TinyUSB when a frame has been received from host (USB -> device).
   This function should deliver packet to lwIP. Return true on success. */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (!src || size == 0) return false;

    // Obtain underlying lwIP netif for the USB esp_netif we created earlier.
    if (!usb_netif) {
        ESP_LOGW(TAG, "tud_network_recv_cb: usb_netif is NULL");
        return false;
    }

    struct netif *lwip_netif = esp_netif_get_netif_impl(usb_netif);
    if (!lwip_netif) {
        ESP_LOGW(TAG, "tud_network_recv_cb: underlying lwIP netif not ready");
        return false;
    }

    // Allocate a pbuf from pool to hand to lwIP
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) {
        ESP_LOGW(TAG, "tud_network_recv_cb: pbuf_alloc failed for size %u", size);
        return false;
    }

    // copy packet data into pbuf chain (p->payload guaranteed for single pbuf)
    // If packet larger than first pbuf, should iterate; typical USB net frames fit single pbuf.
    if (p->len >= size) {
        memcpy(p->payload, src, size);
    } else {
        // If pbuf chain, copy in segments
        uint16_t copied = 0;
        struct pbuf *q = p;
        while (q && copied < size) {
            uint16_t copy_len = (size - copied) > q->len ? q->len : (size - copied);
            memcpy(q->payload, src + copied, copy_len);
            copied += copy_len;
            q = q->next;
        }
    }

    // Hand packet to lwIP input. Note: in some lwIP configs netif->input expects to run
    // in tcpip_thread. If you experience crashes, switch to tcpip_callback to call this
    // inside tcpip_thread context.
#if 1
    // Direct call (works for many esp-idf/lwip builds)
    err_t res = lwip_netif->input(p, lwip_netif);
    if (res != ERR_OK) {
        ESP_LOGW(TAG, "tud_network_recv_cb: netif->input returned %d", res);
        pbuf_free(p);
        return false;
    }
    return true;
#else
    // Safer (if tcpip_callback is available): post to tcpip thread
    struct pbuf *p_copy = p; // transfer ownership to callback
    err_t post = tcpip_try_callback( (tcpip_callback_fn)lwip_netif->input, (void*)p_copy );
    if (post != ERR_OK) {
        // fallback: try direct input or free pbuf
        if (lwip_netif->input(p, lwip_netif) != ERR_OK) {
            pbuf_free(p);
            return false;
        }
    }
    return true;
#endif
}




/* --- main --- */
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
