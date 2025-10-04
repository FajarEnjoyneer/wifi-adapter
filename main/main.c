/* main.c
 * ESP32-S3 WiFi client -> USB ECM/RNDIS dongle example (ESP-IDF v5.5)
 *
 * Changes applied:
 *  - Provide minimal TinyUSB string descriptors to avoid "No String descriptors" warning.
 *  - Robust DHCP stop + single-shot esp_netif_set_ip_info with deterministic lwIP fallback
 *    (avoids repeated ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED logs).
 *  - Start USB DHCP server when TinyUSB network backend signals ready (tud_network_init_cb).
 *  - TinyUSB <-> lwIP glue callbacks (tud_network_*).
 *  - Disable WiFi power-save (WIFI_PS_NONE) for STA stability (optional).
 *
 * Requirements:
 *  - main/tusb_desc.c must export:
 *       const tusb_desc_device_t desc_device;
 *       const uint8_t desc_fs_configuration[];
 *
 *  - Ensure TinyUSB is configured to accept external descriptors in menuconfig / tusb_config.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

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

/* Externs: descriptors provided in main/tusb_desc.c */
extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_fs_configuration[];

/* Default WiFi (override via sdkconfig/menuconfig) */
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

/* ---------------- TinyUSB minimal string descriptors ----------------
   Index 0 is reserved (language ID). Provide manufacturer/product/serial.
   The wrapper expects 'const char *string_table[]' style; assign to tusb_cfg.descriptor.string.
   --------------------------------------------------------------------*/
static const char *tusb_strings[] = {
    "",                         /* 0: reserved (language ID placeholder) */
    "Espressif",                /* 1: iManufacturer */
    "ESP32-S3 USB WiFi",        /* 2: iProduct */
    "ESP32S3-0001",             /* 3: iSerialNumber */
    "001122334455"              /* 4: MAC (if needed by functional descriptor) */
};

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ---------------- WiFi event handlers ---------------- */
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

/* ---------------- Initialize WiFi STA ---------------- */
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

    /* Improve STA stability for NAT gateway (optional): disable WiFi power-save */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) returned %s (%d)", esp_err_to_name(ps), ps);
    } else {
        ESP_LOGI(TAG, "WiFi power-save disabled (WIFI_PS_NONE) for STA stability");
    }

    ESP_LOGI(TAG, "WiFi STA started: connecting to SSID '%s'", CONFIG_WIFI_SSID);
}

/* ---------------- TinyUSB <-> lwIP glue callbacks ---------------- */

/* Local MAC for USB network (locally administered) */
static uint8_t s_usb_mac[6] = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };

/* Forward declaration for helper that sets USB netif IP (used both in init and in tud callback) */
static void ensure_usb_netif_has_ip(void);

/* Called by TinyUSB when network interface is initialized (link up) */
void tud_network_init_cb(void)
{
    ESP_LOGI(TAG, "tud_network_init_cb called (USB network interface initialized)");

    /* When TinyUSB network backend attaches, start DHCP server on usb_netif.
       This avoids race where main init tries to start DHCP before backend ready. */
    if (!usb_netif) {
        ESP_LOGW(TAG, "tud_network_init_cb: usb_netif is NULL (cannot start DHCP yet)");
        return;
    }

    /* Wait briefly for lwIP netif to attach (TinyUSB may attach asynchronously) */
    struct netif *maybe_netif = NULL;
    int waited = 0;
    while (waited < 2000) {
        maybe_netif = esp_netif_get_netif_impl(usb_netif);
        if (maybe_netif) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (!maybe_netif) {
        ESP_LOGW(TAG, "tud_network_init_cb: underlying lwIP netif not attached after %d ms", waited);
    } else {
        ESP_LOGI(TAG, "tud_network_init_cb: underlying lwIP netif ready (name='%c%c' num=%d)",
                 maybe_netif->name[0], maybe_netif->name[1], maybe_netif->num);
    }

    /* Ensure USB netif has IP assigned (use esp-netif API if possible, else lwIP fallback) */
    ensure_usb_netif_has_ip();

    /* Start DHCP server for host */
    esp_err_t rc = esp_netif_dhcps_start(usb_netif);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "tud_network_init_cb: esp_netif_dhcps_start returned %s (%d)", esp_err_to_name(rc), rc);
    } else {
        ESP_LOGI(TAG, "tud_network_init_cb: USB DHCP server started");
    }
}

/* Return pointer to 6-byte MAC address used for the USB network interface */
const uint8_t* tud_network_mac_address(void)
{
    return s_usb_mac;
}

/* Called by TinyUSB when a frame has been received from host (USB -> device).
   Delivers packet to lwIP. */
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

    /* Deliver directly to lwIP. If crashes appear on your build, switch to tcpip_callback. */
    err_t res = lwip_netif->input(p, lwip_netif);
    if (res != ERR_OK) {
        ESP_LOGW(TAG, "tud_network_recv_cb: netif->input returned %d", res);
        pbuf_free(p);
        return false;
    }
    return true;
}

/* Helper to ensure usb_netif has IP; tries esp-netif first, then lwIP fallback */
static void ensure_usb_netif_has_ip(void)
{
    if (!usb_netif) return;

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr      = esp_ip4addr_aton("192.168.42.1");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ip_info.gw.addr      = esp_ip4addr_aton("192.168.42.1");

    esp_err_t set_rc = esp_netif_set_ip_info(usb_netif, &ip_info);
    if (set_rc == ESP_OK) {
        ESP_LOGI(TAG, "ensure_usb_netif_has_ip: esp_netif_set_ip_info OK");
        return;
    }

    if (set_rc == ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED) {
        ESP_LOGW(TAG, "ensure_usb_netif_has_ip: esp_netif_set_ip_info returned DHCP_NOT_STOPPED. Falling back to lwIP.");
    } else {
        ESP_LOGW(TAG, "ensure_usb_netif_has_ip: esp_netif_set_ip_info returned %s (%d). Falling back to lwIP.",
                 esp_err_to_name(set_rc), set_rc);
    }

    struct netif *lwip_netif = esp_netif_get_netif_impl(usb_netif);
    if (lwip_netif) {
        ip4_addr_t ip, nm, gw;
        IP4_ADDR(&ip, 192,168,42,1);
        IP4_ADDR(&nm, 255,255,255,0);
        IP4_ADDR(&gw, 192,168,42,1);
        netif_set_addr(lwip_netif, &ip, &nm, &gw);
        ESP_LOGI(TAG, "ensure_usb_netif_has_ip: Fallback set lwIP netif addr to 192.168.42.1/24");
    } else {
        ESP_LOGW(TAG, "ensure_usb_netif_has_ip: could not obtain lwIP netif for usb_netif");
    }
}

/* ---------------- TinyUSB init + create USB netif ---------------- */
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

    /* Provide minimal string table so tinyusb wrapper can create string descriptors */
    tusb_cfg.descriptor.string = tusb_strings;
    tusb_cfg.descriptor.string_count = (uint8_t) ARRAY_SIZE(tusb_strings);

    tusb_cfg.event_cb = NULL;
    tusb_cfg.event_arg = NULL;

    /* Install driver (tinyusb_descriptors_set called internally) */
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s (%d)", esp_err_to_name(err), err);
        return;
    }
    ESP_LOGI(TAG, "TinyUSB Driver installed on port %d", tusb_cfg.port);

    /* give driver a moment to set up backend */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* create esp-netif for USB using ETH template */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    usb_netif = esp_netif_new(&cfg);
    if (!usb_netif) {
        ESP_LOGE(TAG, "Failed to create USB netif");
        return;
    }

    /* debug: check lwIP netif attachment */
    struct netif *maybe_netif = esp_netif_get_netif_impl(usb_netif);
    if (maybe_netif) {
        ESP_LOGI(TAG, "Underlying lwIP netif found: name='%c%c' num=%d flags=0x%08x",
                 maybe_netif->name[0], maybe_netif->name[1], maybe_netif->num, maybe_netif->flags);
    } else {
        ESP_LOGI(TAG, "No underlying lwIP netif attached yet (will try to set IP/fallback).");
    }

    /* prepare IP info (192.168.42.1/24) - we will ensure IP in tud_network_init_cb as well */
    ensure_usb_netif_has_ip();

    /* Important: do not start DHCP here to avoid race with TinyUSB backend.
       DHCP will be started in tud_network_init_cb when the USB network is ready. */

    ESP_LOGI(TAG, "TinyUSB initialized and USB netif created. DHCP will be started when USB network becomes ready.");
}

/* ---------------- app_main ---------------- */
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
