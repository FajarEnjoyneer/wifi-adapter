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
    .idVendor           = 0x303A,       // Espressif VID (change if needed)
    .idProduct          = 0x4003,       // Product/PID you want
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

/* Full-speed configuration descriptor (raw ECM descriptor bytes).
   Must match format expected by TinyUSB. Exported as symbol for main.c.
*/
const uint8_t desc_fs_configuration[] = {
    9, TUSB_DESC_CONFIGURATION,
    61, 0x00,         // wTotalLength
    2,                // bNumInterfaces
    1,                // bConfigurationValue
    0,                // iConfiguration
    0x80,             // bmAttributes (bus-powered)
    50,               // bMaxPower (100mA)

    // Interface Association Descriptor (IAD)
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    0, 2,
    TUSB_CLASS_CDC, 6 /*ECM subclass*/, 0,
    0,

    // Communication Interface Descriptor
    9, TUSB_DESC_INTERFACE,
    0, 0, 1,
    TUSB_CLASS_CDC, 6 /*ECM*/, 0,
    0,

    // Header Functional Descriptor
    5, 0x24, 0x00, 0x10, 0x01,

    // Union Functional Descriptor
    5, 0x24, 0x06, 0, 1,

    // Ethernet Networking Functional Descriptor
    13, 0x24, 0x0F,
    4, 0,0,0,0,
    0xEA,0x05, 0,0, 0,

    // Notification Endpoint
    7, TUSB_DESC_ENDPOINT,
    0x81, 0x03, 0x08, 0x00, 0x10,

    // Data Interface Descriptor
    9, TUSB_DESC_INTERFACE,
    1, 0, 2,
    0x0A, 0, 0,
    0,

    // Endpoint OUT
    7, TUSB_DESC_ENDPOINT,
    0x02, 0x02, 0x40, 0x00, 0,

    // Endpoint IN
    7, TUSB_DESC_ENDPOINT,
    0x82, 0x02, 0x40, 0x00, 0
};

/* Optionally, you can export a MAC string for the Ethernet functional descriptor,
   but the wrapper can work without it if tusb_cfg.descriptor.string is NULL.
   If you need strings passed via 'tusb_cfg.descriptor.string', you'll need to
   build the string table in format expected by the wrapper. For now we keep it
   simple and leave string pointer NULL in main.c.
*/
