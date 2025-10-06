/* main.c
 * ESP32-S3 WiFi STA -> USB ECM/RNDIS dongle (robust, IDF v5.x)
 *
 * Requirements:
 *  - main/tusb_desc.c exports:
 *      extern const tusb_desc_device_t desc_device;
 *      extern const uint8_t desc_fs_configuration[];
 *  - In menuconfig: enable LWIP IPv4, DHCPS, NAPT if you want NAT; disable TinyUSB auto-descriptors if using custom tusb_desc.c.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_err.h"

#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_net_stack.h" // esp_netif_get_netif_impl

#include "esp_wifi.h"

#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include "tinyusb.h"
#include "tusb.h"

/* Descriptors provided by main/tusb_desc.c */
extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_fs_configuration[];

/* Some TinyUSB wrappers provide tud_network_xmit(void *pbuf, uint16_t arg).
   If your wrapper exposes another API, adapt usb_driver_transmit. */
extern void tud_network_xmit(void *pbuf_ptr, uint16_t arg);

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

/* TinyUSB strings (index 0 reserved) */
static const char *tusb_strings[] = {
    "", /* lang placeholder */
    "Espressif", "ESP32-S3 ECM Dongle", "esp32s3-001", "001122334455"
};
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

/* USB MAC (locally administered) */
static uint8_t s_usb_mac[6] = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };

/* Helper type to deliver pbuf to tcpip thread */
typedef struct {
    struct pbuf *p;
    struct netif *n;
} recv_arg_t;

/* Dump lwIP netif diagnostic info */
static void dump_lwip_netif_info(struct netif *n)
{
    if (!n) {
        ESP_LOGW(TAG, "dump_lwip_netif_info: netif=NULL");
        return;
    }
    char hwbuf[32] = {0};
    for (int i = 0; i < n->hwaddr_len && i < (int)sizeof(n->hwaddr); ++i) {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%02x", (uint8_t)n->hwaddr[i]);
        strcat(hwbuf, tmp);
        if (i+1 < n->hwaddr_len) strcat(hwbuf, ":");
    }
    ESP_LOGI(TAG, "lwIP netif: name='%c%c' num=%d flags=0x%08x mtu=%d hwaddr=%s output=%p linkoutput=%p input=%p",
             n->name[0], n->name[1], n->num, n->flags, n->mtu, hwbuf, (void*)n->output, (void*)n->linkoutput, (void*)n->input);
}

/* Wait for lwIP netif to be attached and backend callbacks ready (output/linkoutput non-NULL).
   Returns lwip netif pointer (may be attached with callbacks still NULL if timed out). */
static struct netif* wait_for_lwip_netif_ready(esp_netif_t *enet, int timeout_ms)
{
    const int step_ms = 100;
    int waited = 0;
    struct netif *lw = NULL;
    while (waited < timeout_ms) {
        lw = esp_netif_get_netif_impl(enet);
        if (lw) {
            if (lw->output && lw->linkoutput) {
                ESP_LOGI(TAG, "lwIP netif ready: name='%c%c' num=%d flags=0x%08x",
                         lw->name[0], lw->name[1], lw->num, lw->flags);
                dump_lwip_netif_info(lw);
                return lw;
            } else {
                ESP_LOGI(TAG, "lwIP attached but backend callbacks NULL; waiting...");
                dump_lwip_netif_info(lw);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    if (lw) {
        ESP_LOGW(TAG, "Timeout: lwIP attached but callbacks still NULL");
        dump_lwip_netif_info(lw);
        return lw;
    }
    ESP_LOGW(TAG, "Timeout waiting for lwIP netif attach");
    return NULL;
}

/* Runs in tcpip thread: hand pbuf to lwIP netif input */
static void netif_input_cb(void *arg)
{
    recv_arg_t *ra = (recv_arg_t*)arg;
    if (!ra) return;
    if (ra->n && ra->p) {
        err_t res = ra->n->input(ra->p, ra->n);
        if (res != ERR_OK) {
            ESP_LOGW(TAG, "netif_input_cb: netif->input returned %d", res);
            pbuf_free(ra->p);
        } else {
            /* lwIP took ownership */
        }
    } else {
        if (ra->p) pbuf_free(ra->p);
    }
    free(ra);
}

/* TinyUSB: called when backend attaches (link up). Start DHCP server (non-blocking). */
static void usb_dhcps_start_task(void *arg)
{
    esp_netif_t *enet = (esp_netif_t*)arg;

    /* Wait longer for backend to become ready (some host stacks take time) */
    struct netif *lw = wait_for_lwip_netif_ready(enet, 5000);
    if (!lw) {
        ESP_LOGW(TAG, "usb_dhcps_start_task: lwip backend not fully ready");
    }

    /* Prepare desired IP */
    esp_netif_ip_info_t ipinfo;
    memset(&ipinfo, 0, sizeof(ipinfo));
    ipinfo.ip.addr = esp_ip4addr_aton("192.168.42.1");
    ipinfo.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ipinfo.gw.addr = ipinfo.ip.addr;

    /* Robustly stop dhcp server first */
    esp_err_t rc = ESP_FAIL;
    for (int i = 0; i < 8; ++i) {
        rc = esp_netif_dhcps_stop(enet);
        if (rc == ESP_OK || rc == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGI(TAG, "esp_netif_dhcps_stop OK (attempt %d)", i+1);
            break;
        }
        ESP_LOGW(TAG, "esp_netif_dhcps_stop attempt %d returned %s (%d), retrying...", i+1, esp_err_to_name(rc), rc);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    /* Try to set IP info with retries, handle DHCP_NOT_STOPPED by stopping and retrying */
    bool set_ok = false;
    for (int j = 0; j < 8; ++j) {
        rc = esp_netif_set_ip_info(enet, &ipinfo);
        if (rc == ESP_OK) {
            ESP_LOGI(TAG, "esp_netif_set_ip_info OK on try %d", j+1);
            set_ok = true;
            break;
        }
        if (rc == ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED) {
            ESP_LOGW(TAG, "esp_netif_set_ip_info returned DHCP_NOT_STOPPED on try %d. Attempting dhcps_stop then retry...", j+1);
            esp_err_t s = esp_netif_dhcps_stop(enet);
            ESP_LOGI(TAG, "esp_netif_dhcps_stop returned %s (%d) during recovery", esp_err_to_name(s), s);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        ESP_LOGW(TAG, "esp_netif_set_ip_info attempt %d returned %s (%d), retrying...", j+1, esp_err_to_name(rc), rc);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (!set_ok) {
        ESP_LOGW(TAG, "esp_netif_set_ip_info failed after retries: %s (%d). Will attempt lwIP fallback.", esp_err_to_name(rc), rc);
        if (lw) {
            ip4_addr_t ip4, nm, gw;
            IP4_ADDR(&ip4, 192,168,42,1);
            IP4_ADDR(&nm, 255,255,255,0);
            IP4_ADDR(&gw, 192,168,42,1);
            netif_set_addr(lw, &ip4, &nm, &gw);
            ESP_LOGI(TAG, "usb_dhcps_start_task: lwIP fallback set IP 192.168.42.1/24");
        } else {
            ESP_LOGW(TAG, "usb_dhcps_start_task: cannot set lwIP fallback - no lwip netif");
        }
    }

    /* Start DHCP server with retries and diagnostics */
    for (int a = 0; a < 8; ++a) {
        rc = esp_netif_dhcps_start(enet);
        if (rc == ESP_OK) {
            ESP_LOGI(TAG, "esp_netif_dhcps_start OK on attempt %d", a+1);
            break;
        }
        ESP_LOGW(TAG, "esp_netif_dhcps_start attempt %d returned %s (%d)", a+1, esp_err_to_name(rc), rc);
        struct netif *nl = esp_netif_get_netif_impl(enet);
        if (nl) dump_lwip_netif_info(nl);
        if (rc == ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED) {
            esp_err_t s = esp_netif_dhcps_stop(enet);
            ESP_LOGI(TAG, "esp_netif_dhcps_stop during recovery returned %s (%d)", esp_err_to_name(s), s);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "usb_dhcps_start_task: dhcps_start failed permanently (%s). Host may need static IP.", esp_err_to_name(rc));
    } else {
        ESP_LOGI(TAG, "usb_dhcps_start_task: DHCP server started successfully");
    }

    vTaskDelete(NULL);
}

/* TinyUSB callback: network initialized */
void tud_network_init_cb(void)
{
    ESP_LOGI(TAG, "tud_network_init_cb called");
    if (!usb_netif) {
        ESP_LOGW(TAG, "tud_network_init_cb: usb_netif NULL");
        return;
    }
    /* create task to start DHCP non-blocking */
    BaseType_t rc = xTaskCreate(usb_dhcps_start_task, "usb_dhcps", 4096, usb_netif, 5, NULL);
    if (rc != pdPASS) {
        ESP_LOGW(TAG, "tud_network_init_cb: failed to create usb_dhcps task");
    } else {
        ESP_LOGI(TAG, "tud_network_init_cb: usb_dhcps task created");
    }
}

/* Provide MAC to host */
const uint8_t* tud_network_mac_address(void)
{
    return s_usb_mac;
}

/* TinyUSB -> device: schedule incoming frame into tcpip thread */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (!src || size == 0) return false;
    if (!usb_netif) return false;

    struct netif *lw = esp_netif_get_netif_impl(usb_netif);
    if (!lw) return false;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) {
        ESP_LOGW(TAG, "tud_network_recv_cb: pbuf_alloc failed for size %u", size);
        return false;
    }

    /* copy data into pbuf chain */
    uint16_t copied = 0;
    for (struct pbuf *q = p; q && copied < size; q = q->next) {
        uint16_t c = (size - copied) > q->len ? q->len : (size - copied);
        memcpy(q->payload, src + copied, c);
        copied += c;
    }

    recv_arg_t *ra = malloc(sizeof(*ra));
    if (!ra) { pbuf_free(p); return false; }
    ra->p = p;
    ra->n = lw;

    /* post to tcpip thread safely */
#if defined(tcpip_callback_with_block)
    if (tcpip_callback_with_block(netif_input_cb, ra, 0) != ERR_OK) {
        ESP_LOGW(TAG, "tud_network_recv_cb: tcpip_callback_with_block failed");
        pbuf_free(p); free(ra); return false;
    }
#elif defined(tcpip_callback)
    if (tcpip_callback(netif_input_cb, ra) != ERR_OK) {
        ESP_LOGW(TAG, "tud_network_recv_cb: tcpip_callback failed");
        pbuf_free(p); free(ra); return false;
    }
#else
    /* fallback: direct call (less safe) */
    err_t r = lw->input(p, lw);
    if (r != ERR_OK) {
        pbuf_free(p);
        free(ra);
        return false;
    }
    free(ra);
#endif

    return true;
}

/* TinyUSB expects a copy-style xmit callback in some wrappers; implement safe copy */
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    (void)arg;
    if (!dst || !ref) return 0;
    struct pbuf *p = (struct pbuf*)ref;
    uint16_t total = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (q->len) {
            memcpy(dst + total, q->payload, q->len);
            total += q->len;
        }
    }
    pbuf_free(p);
    return total;
}

/* ---------------- esp-netif driver glue (usb transmit/free rx) ---------------- */
/* Many esp-netif drivers expect transmit() to consume a pbuf pointer (driver-owned).
   We call tud_network_xmit(p, 0) which some TinyUSB wrappers implement. */
static esp_err_t usb_driver_transmit(void *handle, void *buffer, size_t len)
{
    (void)handle; (void)len;
    struct pbuf *p = (struct pbuf*)buffer;
    if (!p) return ESP_ERR_INVALID_ARG;

    if (!tud_ready()) {
        ESP_LOGW(TAG, "usb_driver_transmit: TinyUSB not ready, dropping");
        pbuf_free(p);
        return ESP_FAIL;
    }

    /* Call wrapper function to send to host. This hands pbuf to TinyUSB wrapper (it must copy/free). */
    tud_network_xmit(p, 0);
    return ESP_OK;
}

/* free rx buffer callback (esp-netif) */
static void usb_driver_free_rx_buffer(void *handle, void *buffer)
{
    (void)handle;
    if (!buffer) return;
    pbuf_free((struct pbuf*)buffer);
}

/* driver ifconfig for esp-netif driver callbacks */
static const esp_netif_driver_ifconfig_t s_usb_driver_ifconfig = {
    .handle = NULL,
    .transmit = usb_driver_transmit,
    .transmit_wrap = NULL,
    .driver_free_rx_buffer = usb_driver_free_rx_buffer
};

/* Ensure usb_netif has IP: first try esp-netif API, fallback to lwIP set_addr */
static void ensure_usb_has_ip(esp_netif_t *enet, const char *ip_s, const char *mask_s, const char *gw_s)
{
    if (!enet) return;
    esp_netif_ip_info_t ipinfo;
    memset(&ipinfo, 0, sizeof(ipinfo));
    ipinfo.ip.addr = esp_ip4addr_aton(ip_s);
    ipinfo.netmask.addr = esp_ip4addr_aton(mask_s);
    ipinfo.gw.addr = esp_ip4addr_aton(gw_s);

    /* stop dhcp server robustly */
    esp_err_t rc = ESP_FAIL;
    for (int i = 0; i < 6; ++i) {
        rc = esp_netif_dhcps_stop(enet);
        if (rc == ESP_OK || rc == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) break;
        ESP_LOGW(TAG, "ensure_usb_has_ip: dhcps_stop attempt %d returned %s (%d)", i+1, esp_err_to_name(rc), rc);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    rc = esp_netif_set_ip_info(enet, &ipinfo);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "ensure_usb_has_ip: esp_netif_set_ip_info OK for %s", ip_s);
        return;
    }

    if (rc == ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED) {
        ESP_LOGW(TAG, "ensure_usb_has_ip: esp_netif_set_ip_info returned DHCP_NOT_STOPPED -> trying lwIP fallback");
    } else {
        ESP_LOGW(TAG, "ensure_usb_has_ip: esp_netif_set_ip_info returned %s (%d) -> lwIP fallback", esp_err_to_name(rc), rc);
    }

    struct netif *lw = esp_netif_get_netif_impl(enet);
    if (lw) {
        ip4_addr_t ip4, nm, gw;
        /* use constants since parsing strings in-field is error-prone */
        IP4_ADDR(&ip4, 192,168,42,1);
        IP4_ADDR(&nm, 255,255,255,0);
        IP4_ADDR(&gw, 192,168,42,1);
        netif_set_addr(lw, &ip4, &nm, &gw);
        ESP_LOGI(TAG, "ensure_usb_has_ip: lwIP fallback set 192.168.42.1/24");
        dump_lwip_netif_info(lw);
    } else {
        ESP_LOGW(TAG, "ensure_usb_has_ip: no lwIP netif for fallback");
    }
}

/* ---------------- TinyUSB install and create usb_netif ---------------- */
static void tinyusb_init_and_create_usb_netif(void)
{
    ESP_LOGI(TAG, "Installing TinyUSB driver (with custom descriptors & strings)");

    /* prepare esp-netif config for ETH template */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.driver = &s_usb_driver_ifconfig;

    usb_netif = esp_netif_new(&netif_cfg);
    if (!usb_netif) {
        ESP_LOGE(TAG, "Failed to create USB netif");
        return;
    }

    tinyusb_config_t tusb_cfg;
    memset(&tusb_cfg, 0, sizeof(tusb_cfg));
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    tusb_cfg.phy.skip_setup = false;
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;
    tusb_cfg.task.size = 4096;
    tusb_cfg.task.priority = 5;
    tusb_cfg.task.xCoreID = 0;

    tusb_cfg.descriptor.device = &desc_device;
    tusb_cfg.descriptor.qualifier = NULL;
    tusb_cfg.descriptor.full_speed_config = desc_fs_configuration;
    tusb_cfg.descriptor.high_speed_config = NULL;
    tusb_cfg.descriptor.string = tusb_strings;
    tusb_cfg.descriptor.string_count = (uint8_t)ARRAY_SIZE(tusb_strings);

    tusb_cfg.event_cb = NULL;
    tusb_cfg.event_arg = NULL;

    esp_err_t rc = tinyusb_driver_install(&tusb_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s (%d)", esp_err_to_name(rc), rc);
        return;
    }
    ESP_LOGI(TAG, "TinyUSB driver installed");

    /* wait a short while for backend to attach; tud_network_init_cb will start DHCP later */
    struct netif *lw = wait_for_lwip_netif_ready(usb_netif, 2000);
    if (!lw) {
        ESP_LOGW(TAG, "tinyusb_init: lwIP backend not fully ready now (will retry in callbacks)");
    }

    /* ensure default USB IP (may be changed to follow WiFi later) */
    ensure_usb_has_ip(usb_netif, "192.168.42.1", "255.255.255.0", "192.168.42.1");

    ESP_LOGI(TAG, "TinyUSB initialized and USB netif created. DHCP will be started when USB backend attaches.");
}

/* ---------------- WiFi event handlers & init ---------------- */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> connecting");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t*) event_data;
        int reason = d ? d->reason : -1;
        ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED (reason=%d) -> reconnecting", reason);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    }
}

/* Enable NAPT on STA (if built-in) */
static void enable_napt_on_sta(void)
{
#if CONFIG_LWIP_IPV4_NAPT
    if (sta_netif) {
        esp_err_t rc = esp_netif_napt_enable(sta_netif);
        ESP_LOGI(TAG, "esp_netif_napt_enable returned %s (%d)", esp_err_to_name(rc), rc);
    } else {
        ESP_LOGW(TAG, "enable_napt_on_sta: sta_netif NULL");
    }
#else
    ESP_LOGW(TAG, "NAPT not enabled in sdkconfig (CONFIG_LWIP_IPV4_NAPT=n)");
#endif
}

/* Ganti fungsi set_usb_ip_from_wifi dengan ini */
/* Ganti fungsi set_usb_ip_from_wifi dengan implementasi ini */
static void set_usb_ip_from_wifi(const esp_netif_ip_info_t *wifi_ipinfo)
{
    if (!usb_netif || !wifi_ipinfo) return;

    /* derive network from WiFi IP (assume IPv4) */
    uint32_t w = ntohl(wifi_ipinfo->ip.addr);
    uint8_t a = (w >> 24) & 0xFF;
    uint8_t b = (w >> 16) & 0xFF;
    uint8_t c = (w >> 8)  & 0xFF;
    char usb_ip_s[16];
    snprintf(usb_ip_s, sizeof(usb_ip_s), "%u.%u.%u.%u", a, b, c, 253);

    ESP_LOGI(TAG, "Setting USB IP to %s/24 (following WiFi)", usb_ip_s);

    /* Check lwIP backend attachment & callbacks first */
    struct netif *lw = esp_netif_get_netif_impl(usb_netif);
    if (!lw || lw->output == NULL || lw->linkoutput == NULL) {
        ESP_LOGW(TAG, "set_usb_ip_from_wifi: lwIP backend not fully ready -> using lwIP fallback (avoid esp-netif race)");
        if (lw) {
            ESP_LOGI(TAG, "lwIP netif: name='%c%c' num=%d flags=0x%08x mtu=%d hwaddr_len=%d output=%p linkoutput=%p input=%p",
                     lw->name[0], lw->name[1], lw->num, lw->flags, lw->mtu, lw->hwaddr_len, (void*)lw->output, (void*)lw->linkoutput, (void*)lw->input);
            /* set address using lwIP directly */
            ip4_addr_t ip4, nm, gw;
            IP4_ADDR(&ip4, a, b, c, 253);
            IP4_ADDR(&nm, 255,255,255,0);
            IP4_ADDR(&gw, a, b, c, 253);
            netif_set_addr(lw, &ip4, &nm, &gw);
            ESP_LOGI(TAG, "set_usb_ip_from_wifi: lwIP fallback set %s", usb_ip_s);
        } else {
            ESP_LOGW(TAG, "set_usb_ip_from_wifi: no lwIP netif available even for fallback");
        }
        /* Don't try esp-netif in this case to avoid DHCP_NOT_STOPPED race */
        return;
    }

    /* If backend looks ready, attempt esp-netif path (preferred) */
    esp_err_t rc;

    rc = esp_netif_dhcps_stop(usb_netif);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "set_usb_ip_from_wifi: esp_netif_dhcps_stop OK");
    } else if (rc == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGI(TAG, "set_usb_ip_from_wifi: dhcps_stop indicates already stopped");
    } else {
        ESP_LOGW(TAG, "set_usb_ip_from_wifi: dhcps_stop returned %s (%d) - continuing", esp_err_to_name(rc), rc);
    }

    esp_netif_ip_info_t usb_ip;
    memset(&usb_ip, 0, sizeof(usb_ip));
    usb_ip.ip.addr = esp_ip4addr_aton(usb_ip_s);
    /* use same netmask as WiFi (keeps /24 if WiFi is /24) */
    usb_ip.netmask.addr = wifi_ipinfo->netmask.addr ? wifi_ipinfo->netmask.addr : esp_ip4addr_aton("255.255.255.0");
    usb_ip.gw.addr = usb_ip.ip.addr;

    /* Try esp_netif_set_ip_info once (no tight retry loop) */
    rc = esp_netif_set_ip_info(usb_netif, &usb_ip);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "set_usb_ip_from_wifi: esp_netif_set_ip_info OK");
    } else {
        ESP_LOGW(TAG, "set_usb_ip_from_wifi: esp_netif_set_ip_info returned %s (%d) -> using lwIP fallback", esp_err_to_name(rc), rc);
        /* lwIP fallback */
        if (lw) {
            ip4_addr_t ip4, nm, gw;
            IP4_ADDR(&ip4, a, b, c, 253);
            IP4_ADDR(&nm, 255,255,255,0);
            IP4_ADDR(&gw, a, b, c, 253);
            netif_set_addr(lw, &ip4, &nm, &gw);
            ESP_LOGI(TAG, "set_usb_ip_from_wifi: lwIP fallback set %s", usb_ip_s);
        }
    }

    /* Start DHCP server once; if it fails, log and continue (host can use static IP) */
    rc = esp_netif_dhcps_start(usb_netif);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "set_usb_ip_from_wifi: esp_netif_dhcps_start returned %s (%d) - host may need static IP", esp_err_to_name(rc), rc);
    } else {
        ESP_LOGI(TAG, "set_usb_ip_from_wifi: USB DHCP started on %s/24", usb_ip_s);
    }
}


/* when WiFi gets IP */
static void got_ip_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t*) event_data;
    ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&evt->ip_info.ip));

    enable_napt_on_sta();
    set_usb_ip_from_wifi(&evt->ip_info);
}

/* initialize WiFi STA */
static void init_wifi_sta(void)
{
    ESP_LOGI(TAG, "Initializing WiFi STA");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGW(TAG, "Failed to create default wifi STA netif");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_handler, NULL, NULL));

    wifi_config_t wcfg = {0};
    strncpy((char*)wcfg.sta.ssid, CONFIG_WIFI_SSID, sizeof(wcfg.sta.ssid)-1);
    strncpy((char*)wcfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wcfg.sta.password)-1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* optional: disable wifi PS for stable NAT throughput */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps == ESP_OK) ESP_LOGI(TAG, "WiFi PS disabled (WIFI_PS_NONE)");
    else ESP_LOGW(TAG, "esp_wifi_set_ps returned %s", esp_err_to_name(ps));
}

/* ---------------- main ---------------- */
void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    ESP_LOGI(TAG, "Starting USB WiFi dongle");

    init_wifi_sta();

    tinyusb_init_and_create_usb_netif();

    /* main loop: run TinyUSB core (and let TinyUSB callbacks/tasks do network work) */
    while (1) {
        tud_task();      // TinyUSB core processing
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
