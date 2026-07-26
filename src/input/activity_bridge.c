#define DT_DRV_COMPAT codex_analog_stick

#include <codex/activity.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Codex activity filtering requires exactly one codex,analog-stick node");

static const struct device *const codex_analog_input_device =
    DEVICE_DT_GET(DT_INST_PHANDLE(0, input_device));

bool codex_activity_defer_input_device(const struct device *device)
{
    return device == codex_analog_input_device;
}
