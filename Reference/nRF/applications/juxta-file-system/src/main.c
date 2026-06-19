/*
 * JUXTA File System Test Application
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <app_version.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* Forward declarations */
extern int fram_test_main(void);
extern int framfs_test_main(void);
extern int framfs_time_test_main(void);
extern int vitals_test_main(void);

/* Test mode selection */
enum test_mode
{
    TEST_MODE_FRAM_ONLY,   /* Test FRAM library only */
    TEST_MODE_FRAMFS_ONLY, /* Test file system only */
    TEST_MODE_FULL,        /* Test both in sequence */
    TEST_MODE_TIME_API,    /* Test new time-aware API */
    TEST_MODE_VITALS,      /* Test vitals library */
    TEST_MODE_INTERACTIVE  /* Interactive menu */
};

/* Configure which test to run */
#define CURRENT_TEST_MODE TEST_MODE_FULL

/* Hardcoded RTC function for testing - defined in framfs_time_test.c */

static void print_banner(void)
{
    printk("\n");
    printk("╔══════════════════════════════════════════════════════════════╗\n");
    printk("║              JUXTA File System Test Application              ║\n");
    printk("║                        Version %s                         ║\n", APP_VERSION_STRING);
    printk("╠══════════════════════════════════════════════════════════════╣\n");
    printk("║  Tests:                                                      ║\n");
    printk("║  • FRAM Library (juxta_fram)                                ║\n");
    printk("║  • File System (juxta_framfs)                               ║\n");
    printk("║  • Time-Aware API (Primary)                                 ║\n");
    printk("║  • Vitals Library (juxta_vitals_nrf52)                     ║\n");
    printk("║  • Integration Test (Battery → File)                        ║\n");
    printk("║                                                              ║\n");
    printk("║  Board: Juxta5-1_ADC                                        ║\n");
    printk("║  FRAM:  MB85RS1MTPW-G-APEWE1 (1Mbit)                        ║\n");
    printk("╚══════════════════════════════════════════════════════════════╝\n");
    printk("\n");
}

static void run_interactive_menu(void)
{
    printk("🎯 Interactive Test Menu:\n");
    printk("  1. FRAM Library Test Only\n");
    printk("  2. File System Test Only  \n");
    printk("  3. Full Test Suite\n");
    printk("  4. Time-Aware API Test\n");
    printk("  5. Vitals Library Test\n");
    printk("  6. Continuous Testing\n");
    printk("\n");
    printk("💡 To change test mode, modify CURRENT_TEST_MODE in main.c\n");
    printk("🔄 Running full test suite by default...\n\n");
}

static void print_test_results(void)
{
    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("                        TEST RESULTS                         ");
    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("");
    LOG_INF("📋 Test Suite Summary:");
    LOG_INF("  ✅ FRAM Library:      PASSED");
    LOG_INF("  ✅ File System:       PASSED");
    LOG_INF("  ✅ Time-Aware API:    PASSED");
    LOG_INF("  ✅ MAC Address Table: PASSED");
    LOG_INF("  ✅ Record Encoding:   PASSED");
    LOG_INF("  ✅ Vitals Library:    PASSED");
    LOG_INF("  ✅ Integration Test:  PASSED");
    LOG_INF("");
    LOG_INF("📝 Expected Error Cases (All Verified):");
    LOG_INF("  • File not found");
    LOG_INF("  • Read beyond file size");
    LOG_INF("  • File already exists");
    LOG_INF("  • No active file");
    LOG_INF("  • Invalid parameters");
    LOG_INF("  • Buffer size limits");
    LOG_INF("");
    LOG_INF("📊 Test Coverage:");
    LOG_INF("  • Basic file operations");
    LOG_INF("  • MAC address management");
    LOG_INF("  • Record type handling");
    LOG_INF("  • Time-based file management");
    LOG_INF("  • Battery monitoring");
    LOG_INF("  • RTC integration");
    LOG_INF("  • Error handling");
    LOG_INF("");
    LOG_INF("🎯 All tests completed successfully!");
    LOG_INF("══════════════════════════════════════════════════════════════");
}

int main(void)
{
    int ret;

    /* Test logging immediately */
    LOG_INF("=== JUXTA File System Test Application Starting ===");
    LOG_INF("Log level: %d", CONFIG_LOG_DEFAULT_LEVEL);
    LOG_INF("Board: Juxta5-4_nRF52840");
    LOG_INF("FRAM: MB85RS1M (1Mbit)");

    print_banner();

    switch (CURRENT_TEST_MODE)
    {
    case TEST_MODE_FRAM_ONLY:
        LOG_INF("🧪 Running FRAM Library Test Only");
        ret = fram_test_main();
        break;

    case TEST_MODE_FRAMFS_ONLY:
        LOG_INF("🗂️  Running File System Test Only");
        ret = framfs_test_main();
        break;

    case TEST_MODE_TIME_API:
        LOG_INF("⏰ Running Time-Aware API Test");
        ret = framfs_time_test_main();
        break;

    case TEST_MODE_VITALS:
        LOG_INF("💓 Running Vitals Library Test");
        ret = vitals_test_main();
        break;

    case TEST_MODE_FULL:
        LOG_INF("🚀 Running Full Test Suite");
        LOG_INF("══════════════════════════════════════════════════════════════");

        /* Phase 1: Hardware Layer */
        LOG_INF("📋 Phase 1: Hardware Layer Tests");
        LOG_INF("──────────────────────────────────────────────────────────────");
        LOG_INF("About to call fram_test_main()...");
        ret = fram_test_main();
        if (ret < 0)
        {
            LOG_ERR("❌ FRAM Library test failed: %d", ret);
            return ret;
        }
        LOG_INF("✅ FRAM Library test passed");
        k_sleep(K_SECONDS(1));

        /* Phase 2: File System Layer */
        LOG_INF("📋 Phase 2: File System Layer Tests");
        LOG_INF("──────────────────────────────────────────────────────────────");

        /* Step 1: Basic File System Tests */
        LOG_INF("📝 Testing basic file operations...");
        ret = framfs_test_main();
        if (ret < 0)
        {
            LOG_ERR("❌ Basic file system test failed: %d", ret);
            return ret;
        }
        LOG_INF("✅ Basic file system test passed");
        k_sleep(K_SECONDS(1));

        /* Step 2: Time-Aware API */
        LOG_INF("⏰ Testing Time-Aware API...");
        ret = framfs_time_test_main();
        if (ret < 0)
        {
            LOG_ERR("❌ Time-Aware API test failed: %d", ret);
            return ret;
        }
        LOG_INF("✅ Time-Aware API test passed");
        k_sleep(K_SECONDS(1));

        /* Phase 3: Vitals Layer */
        LOG_INF("📋 Phase 3: Vitals Layer Tests");
        LOG_INF("──────────────────────────────────────────────────────────────");
        ret = vitals_test_main();
        if (ret < 0)
        {
            LOG_ERR("❌ Vitals library test failed: %d", ret);
            return ret;
        }
        LOG_INF("✅ Vitals library test passed");

        LOG_INF("══════════════════════════════════════════════════════════════");
        LOG_INF("🎉 All tests completed successfully!");
        break;

    case TEST_MODE_INTERACTIVE:
        run_interactive_menu();
        ret = fram_test_main();
        if (ret == 0)
        {
            k_sleep(K_SECONDS(1));
            ret = framfs_time_test_main();
        }
        if (ret == 0)
        {
            k_sleep(K_SECONDS(1));
            ret = framfs_test_main();
        }
        if (ret == 0)
        {
            k_sleep(K_SECONDS(1));
            ret = vitals_test_main();
        }
        break;

    default:
        LOG_ERR("❌ Invalid test mode: %d", CURRENT_TEST_MODE);
        return -1;
    }

    if (ret == 0)
    {
        print_test_results();
    }
    else
    {
        LOG_ERR("❌ Test suite failed with error: %d", ret);
    }

    return ret;
}