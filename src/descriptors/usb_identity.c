#include <codex/usb.h>

#include <stdbool.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

#if defined(CONFIG_USB_DEVICE_STACK)
BUILD_ASSERT(sizeof(CONFIG_USB_DEVICE_MANUFACTURER) == sizeof("Work Louder"),
             "Codex USB manufacturer descriptor storage must be exactly 11 characters");
BUILD_ASSERT(sizeof(CONFIG_USB_DEVICE_PRODUCT) == sizeof("Codex Micro"),
             "Codex USB product descriptor storage must be exactly 11 characters");
#endif

struct codex_usb_string_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bString[22];
} __packed;

extern uint8_t *__real_usb_get_device_descriptor(void);

static void set_utf16le(uint8_t output[22], const char ascii[12])
{
    for (size_t index = 0U; index < 11U; index++) {
        output[index * 2U] = (uint8_t)ascii[index];
        output[index * 2U + 1U] = 0U;
    }
}

static bool set_string_descriptors(uint8_t *descriptor_block)
{
    struct usb_desc_header *header = (struct usb_desc_header *)descriptor_block;
    size_t walked = 0U;
    size_t string_index = 0U;
    bool manufacturer_set = false;
    bool product_set = false;

    while (walked < 4096U && header->bLength != 0U) {
        if (header->bLength < sizeof(*header)) {
            return false;
        }

        if (header->bDescriptorType == USB_DESC_STRING) {
            struct codex_usb_string_descriptor *string =
                (struct codex_usb_string_descriptor *)header;

            if (string_index == 1U) {
                if (string->bLength != sizeof("Work Louder") * 2U) {
                    return false;
                }
                set_utf16le(string->bString, "Work Louder");
                manufacturer_set = true;
            } else if (string_index == 2U) {
                if (string->bLength != sizeof("Codex Micro") * 2U) {
                    return false;
                }
                set_utf16le(string->bString, "Codex Micro");
                product_set = true;
            }
            string_index++;
        }

        walked += header->bLength;
        header = (struct usb_desc_header *)(descriptor_block + walked);
    }

    return manufacturer_set && product_set;
}

uint16_t codex_usb_vid(void) { return CODEX_USB_VID; }

uint16_t codex_usb_pid(void) { return CODEX_USB_PID; }

uint16_t codex_usb_bcd_device(void) { return CODEX_USB_BCD_DEVICE; }

const char *codex_usb_manufacturer(void) { return "Work Louder"; }

const char *codex_usb_product(void) { return "Codex Micro"; }

uint8_t *__wrap_usb_get_device_descriptor(void)
{
    struct usb_device_descriptor *descriptor =
        (struct usb_device_descriptor *)__real_usb_get_device_descriptor();

    if (descriptor == NULL) {
        return NULL;
    }

    descriptor->idVendor = sys_cpu_to_le16(CODEX_USB_VID);
    descriptor->idProduct = sys_cpu_to_le16(CODEX_USB_PID);
    descriptor->bcdDevice = sys_cpu_to_le16(CODEX_USB_BCD_DEVICE);

    if (!set_string_descriptors((uint8_t *)descriptor)) {
        return NULL;
    }

    return (uint8_t *)descriptor;
}
