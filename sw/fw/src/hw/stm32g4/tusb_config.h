#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* MCU / RTOS / speed --------------------------------------------------------*/
#define CFG_TUSB_MCU            OPT_MCU_STM32G4
#define CFG_TUSB_OS             OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

/* Memory placement / alignment (defaults are fine on the G4 single-core) -----*/
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

/* Device stack --------------------------------------------------------------*/
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

/* Classes — CDC (virtual COM) only for now ----------------------------------*/
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_CDC_RX_BUFSIZE  256
// Pipeline depth behind the endpoint buffer: the server's 1 ms pass writes
// into whatever is free here, and a pass catching up a stall pushes several
// milliseconds of records at once.
#define CFG_TUD_CDC_TX_BUFSIZE  2048
// Bytes handed to the endpoint per transfer; one packet per transfer costs a
// host poll interval per 64 B, so a multi-packet transfer carries the stream.
#define CFG_TUD_CDC_EP_BUFSIZE  512

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
