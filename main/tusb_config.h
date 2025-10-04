#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// Enable network classes you want: CDC-ECM and/or RNDIS
// For ECM:
#define CFG_TUD_ECM             1
// For RNDIS (uncomment if you prefer RNDIS)
// #define CFG_TUD_RNDIS           1

// minimal stack sizes, etc. keep defaults for other CFGs
#endif /* TUSB_CONFIG_H_ */