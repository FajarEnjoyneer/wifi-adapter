// main/tusb_desc.c
#include "tusb.h"
#include <stdint.h>

/* Device descriptor (exported symbol for main.c to reference) */
const tusb_desc_device_t desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,       // Espressif VID
    .idProduct          = 0x4003,       // Product/PID
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

/* Full-speed configuration descriptor (raw ECM descriptor bytes).
   IMPORTANT: wTotalLength set to 79 (0x4F) matching descriptor bytes below.
*/
const uint8_t desc_fs_configuration[] = {
    /* Configuration Descriptor */
    9, TUSB_DESC_CONFIGURATION,
    0x4F, 0x00,      /* wTotalLength = 79 (0x4F) */
    2,               /* bNumInterfaces */
    1,               /* bConfigurationValue */
    0,               /* iConfiguration */
    0x80,            /* bmAttributes (bus-powered) */
    50,              /* bMaxPower (100mA) */

    /* Interface Association Descriptor (IAD) */
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    0, 2,            /* first IF = 0, IF count = 2 */
    TUSB_CLASS_CDC, 6 /*CDC_COMM_SUBCLASS_ETHERNET_CONTROL_MODEL*/, 0,
    0,

    /* Communication Interface Descriptor */
    9, TUSB_DESC_INTERFACE,
    0, 0, 1,         /* if=0, alt=0, 1 endpoint */
    TUSB_CLASS_CDC, 6 /*ECM*/, 0,
    0,

    /* Header Functional Descriptor */
    5, 0x24, 0x00, 0x10, 0x01,

    /* Union Functional Descriptor */
    5, 0x24, 0x06, 0, 1,

    /* Ethernet Networking Functional Descriptor */
    13, 0x24, 0x0F,
    4,                  /* iMACAddress string index = 4 */
    0x00,0x00,0x00,0x00,/* bmEthernetStatistics (4 bytes) */
    0xEA,0x05,          /* wMaxSegmentSize = 1514 (0x05EA) little-endian */
    0x00,0x00,          /* wNumberMCFilters */
    0x00,               /* bNumberPowerFilters */

    /* Notification Endpoint (Interrupt IN) */
    7, TUSB_DESC_ENDPOINT,
    0x81, 0x03, 0x08, 0x00, 0x10,

    /* Data Interface Descriptor (CDC Data) */
    9, TUSB_DESC_INTERFACE,
    1, 0, 2,         /* if=1, alt=0, 2 endpoints */
    0x0A, 0, 0,      /* class = CDC Data (0x0A) */
    0,

    /* Endpoint OUT (Bulk OUT) */
    7, TUSB_DESC_ENDPOINT,
    0x02, 0x02, 0x40, 0x00, 0,

    /* Endpoint IN (Bulk IN) */
    7, TUSB_DESC_ENDPOINT,
    0x82, 0x02, 0x40, 0x00, 0
};