#ifndef USB_H
#define USB_H

// Composite USB device: CDC ACM (debug serial) + MSC (read-only mass storage
// backed by the flash message store). printf output goes to the CDC link via
// _write()/putchar() defined in usb.c.

// Bring up the USB device (wraps USBFSSetup).
void usb_init(void);

// Service the MSC transfer state machine; call regularly from the main loop.
void usb_task(void);

// Return a pending CDC debug-input byte (0..255), or -1 if none.
int usb_cdc_getc(void);

// Reset the USB peripheral (needed before jumping to the bootloader).
void usb_reset(void);

#endif // USB_H
