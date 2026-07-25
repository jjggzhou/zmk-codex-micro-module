#include <codex/usb.h>

#include <stdbool.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/toolchain.h>
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
extern struct usb_desc_header __usb_descriptor_start[];
extern struct usb_desc_header __usb_descriptor_end[];

__weak uint8_t *codex_usb_descriptor_start(void)
{
    return (uint8_t *)__usb_descriptor_start;
}

__weak uint8_t *codex_usb_descriptor_end(void)
{
    return (uint8_t *)__usb_descriptor_end;
}

static void set_utf16le(uint8_t output[22], const char ascii[12])
{
    for (size_t index = 0U; index < 11U; index++) {
        output[index * 2U] = (uint8_t)ascii[index];
        output[index * 2U + 1U] = 0U;
    }
}

bool codex_usb_apply_identity(uint8_t *descriptor_block, size_t descriptor_block_size)
{
    struct usb_device_descriptor *device = NULL;
    struct codex_usb_string_descriptor *manufacturer = NULL;
    struct codex_usb_string_descriptor *product = NULL;
    uint8_t *cursor = descriptor_block;
    uint8_t *end;
    size_t string_index = 0U;

    if (descriptor_block == NULL ||
        descriptor_block_size < sizeof(struct usb_desc_header) ||
        descriptor_block_size > UINTPTR_MAX - (uintptr_t)descriptor_block) {
        return false;
    }
    end = descriptor_block + descriptor_block_size;

    while (cursor < end) {
        size_t remaining = (size_t)(end - cursor);
        struct usb_desc_header *header;

        if (remaining < sizeof(struct usb_desc_header)) {
            return false;
        }
        header = (struct usb_desc_header *)cursor;

        if (header->bLength == 0U) {
            if (header->bDescriptorType != 0U || remaining != sizeof(*header)) {
                return false;
            }
            break;
        }
        if (header->bLength < sizeof(*header) || header->bLength > remaining) {
            return false;
        }

        if (header->bDescriptorType == USB_DESC_DEVICE) {
            if (cursor != descriptor_block ||
                header->bLength < sizeof(struct usb_device_descriptor) ||
                device != NULL) {
                return false;
            }
            device = (struct usb_device_descriptor *)header;
        } else if (header->bDescriptorType == USB_DESC_STRING) {
            struct codex_usb_string_descriptor *string =
                (struct codex_usb_string_descriptor *)header;

            if (string_index == 1U) {
                if (header->bLength != sizeof(struct codex_usb_string_descriptor)) {
                    return false;
                }
                manufacturer = string;
            } else if (string_index == 2U) {
                if (header->bLength != sizeof(struct codex_usb_string_descriptor)) {
                    return false;
                }
                product = string;
            }
            string_index++;
        }

        cursor += header->bLength;
    }

    if (cursor >= end || device == NULL || manufacturer == NULL || product == NULL) {
        return false;
    }

    device->idVendor = sys_cpu_to_le16(CODEX_USB_VID);
    device->idProduct = sys_cpu_to_le16(CODEX_USB_PID);
    device->bcdDevice = sys_cpu_to_le16(CODEX_USB_BCD_DEVICE);
    set_utf16le(manufacturer->bString, "Work Louder");
    set_utf16le(product->bString, "Codex Micro");

    return true;
}

uint16_t codex_usb_vid(void) { return CODEX_USB_VID; }

uint16_t codex_usb_pid(void) { return CODEX_USB_PID; }

uint16_t codex_usb_bcd_device(void) { return CODEX_USB_BCD_DEVICE; }

const char *codex_usb_manufacturer(void) { return "Work Louder"; }

const char *codex_usb_product(void) { return "Codex Micro"; }

uint8_t *__wrap_usb_get_device_descriptor(void)
{
    uint8_t *descriptor = __real_usb_get_device_descriptor();
    uint8_t *descriptor_start = codex_usb_descriptor_start();
    uint8_t *descriptor_end = codex_usb_descriptor_end();
    uintptr_t descriptor_start_address = (uintptr_t)descriptor_start;
    uintptr_t descriptor_end_address = (uintptr_t)descriptor_end;
    size_t descriptor_block_size;

    if (descriptor == NULL || descriptor != descriptor_start ||
        descriptor_end_address <= descriptor_start_address) {
        return NULL;
    }
    descriptor_block_size = (size_t)(descriptor_end_address - descriptor_start_address);

    if (!codex_usb_apply_identity(descriptor, descriptor_block_size)) {
        return NULL;
    }

    return descriptor;
}
