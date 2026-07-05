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
// Hold a full telemetry window's burst (see TELEMETRY_TX_BUF_BYTES) so the
// batched write is absorbed in one go and drains async — a window larger than
// this FIFO forces IO_serial_write into 1 ms backpressure yields per overflow.
#define CFG_TUD_CDC_TX_BUFSIZE  2048

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
