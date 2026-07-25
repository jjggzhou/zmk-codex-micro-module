#include <zephyr/ztest.h>
#include <codex/descriptor.h>

ZTEST(codex_smoke, test_module_header_is_visible) {
    zassert_equal(CODEX_VENDOR_REPORT_ID, 0x06);
}

ZTEST_SUITE(codex_smoke, NULL, NULL, NULL, NULL, NULL);
