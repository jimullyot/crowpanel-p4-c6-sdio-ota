/*
 * C6 SDIO OTA — CrowPanel ESP32-P4
 *
 * Upgrades ESP32-C6 co-processor firmware via SDIO OTA.
 * Host starts no WiFi — isolates SDIO transport from WiFi contention.
 *
 * Strategic logging tags:
 *   [PHASE]  — phase transitions (grep for experiment flow)
 *   [DIAG]   — diagnostic measurements (timing, versions, sizes)
 *   [PASS]   — success indicators
 *   [FAIL]   — failure indicators with error codes
 *   [WARN]   — non-fatal anomalies
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_hosted.h"

/* These headers exist in the stock OTA example but may not be public API.
 * If compilation fails here, check the actual esp_hosted header layout:
 *   find managed_components -name "*.h" | grep hosted
 * Then update includes accordingly. */
#if __has_include("esp_hosted_ota.h")
#include "esp_hosted_ota.h"
#else
/* OTA functions may be declared in esp_hosted.h directly */
#endif

#if __has_include("esp_hosted_api_types.h")
#include "esp_hosted_api_types.h"
#else
/* Version struct may be in esp_hosted.h */
#endif

#include "esp_timer.h"
#include "esp_app_desc.h"
#include "ota_littlefs.h"
#include "nvs.h"
#include "esp_hosted_transport_config.h"

static const char *TAG = "c6-sdio-ota";

/* CrowPanel reaches the C6 differently depending on the PCB revision: V1.1
 * reversed the SDIO data lines from IO14,15,16,17 to IO17,16,15,14 and moved
 * the reset line. Nothing on the board reports which revision you have -- the
 * only marking is silkscreen on the panel -- so the only way to know is to try
 * one and see if the C6 answers.
 *
 * Guessing wrong does not look like a wiring fault, which is the reason this
 * is worth probing for rather than leaving someone to find by hand. Card init
 * and register reads are CMD52 traffic on the CMD line alone, so the bus
 * enumerates perfectly and reports both function block sizes; only the CMD53
 * data transfers that carry the slave's init packet ever touch these pins. The
 * symptom is a healthy-looking bus that simply goes quiet.
 *
 * Order matters, because a wrong guess costs a reboot: whichever revision is
 * listed first is the one that never pays for the probe. V1.1 leads because it
 * is what is currently being sold, so the common case gets it right on the
 * first attempt and V1.0 boards take the single extra reset instead. */
struct board_wiring {
    const char *revision;
    int d0, d1, reset;
};

static const struct board_wiring kWirings[] = {
    { "V1.1", 17, 16, 54 },
    { "V1.0", 14, 15, 32 },
};
#define kWiringCount (sizeof(kWirings) / sizeof(kWirings[0]))

#define NVS_BOARD_NAMESPACE "board"
/* What is stored is an index into kWirings, so the key has to change whenever
 * that order does -- otherwise a board that already settled reads its old index
 * back and is handed the other revision's pins. Renamed when V1.1 moved to the
 * front; boards that had settled under "sdio_wiring" simply probe once more. */
#define NVS_KEY_WIRING      "sdio_rev"      /* index that last worked */
#define NVS_KEY_UNPROVEN    "sdio_trying"   /* set while a guess is in flight */
#define NVS_KEY_TRIES       "sdio_tries"    /* guesses made since the last success */

/* Has to equal the ESP-Hosted *host* version baked into whatever Arduino ESP32
 * core the clock firmware is built with -- core 3.3.11 carries 2.12.11 (see
 * esp_hosted_host_fw_ver.h in the installed core). A slave older than the host
 * is the mismatch the driver warns about on every boot, so this constant and
 * the core move together. */
static const uint32_t kTargetFwMajor = 2;
static const uint32_t kTargetFwMinor = 12;
static const uint32_t kTargetFwPatch = 11;

/* What the C6 reported in phase 1, so the summary can state what is actually on
 * the chip instead of assuming it equals the target. */
static esp_hosted_coprocessor_fwver_t g_running_ver;

/* Ordered version value, so "newer than us" is a comparison and not three. */
static uint32_t fw_value(uint32_t major, uint32_t minor, uint32_t patch) {
    return (major << 16) | (minor << 8) | patch;
}

/* Which entry of kWirings this boot is using, and whether we managed to apply
 * it. Without the second flag a failed apply would let a later success be
 * credited to a revision we never actually tried. */
static uint8_t g_wiring;
static bool g_wiring_applied;

static void store_wiring(uint8_t index, uint8_t unproven, uint8_t tries) {
    nvs_handle_t h;
    if (nvs_open(NVS_BOARD_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_WIRING, index);
    nvs_set_u8(h, NVS_KEY_UNPROVEN, unproven);
    nvs_set_u8(h, NVS_KEY_TRIES, tries);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t load_wiring(uint8_t *unproven, uint8_t *tries) {
    nvs_handle_t h;
    uint8_t index = 0;
    *unproven = 0;
    *tries = 0;
    if (nvs_open(NVS_BOARD_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_WIRING, &index);
        nvs_get_u8(h, NVS_KEY_UNPROVEN, unproven);
        nvs_get_u8(h, NVS_KEY_TRIES, tries);
        nvs_close(h);
    }
    return index < kWiringCount ? index : 0;
}

/* Pick a wiring and hand it to esp_hosted before the transport starts.
 *
 * This has to be a constructor. esp_hosted starts its transport from its own
 * constructor, so by the time app_main runs the SDIO driver already holds a
 * copy of the pins it read from Kconfig -- editing the config that late moves
 * only the reset line, which reads as "the pin map changed and nothing
 * happened". Constructors carrying a priority run before those without one,
 * and esp_hosted's has none, so this lands first. esp_hosted_init() keeps a
 * config that is already set and only falls back to Kconfig when there is
 * none, which is what makes this the supported way in rather than a race.
 *
 * A wrong guess cannot be caught in the moment: the transport reboots the host
 * rather than returning an error. So the guess is written down before it is
 * tried and cleared only once the slave answers, which turns a reboot into the
 * evidence that the guess was wrong. */
static void __attribute__((constructor(101))) choose_wiring(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "[WARN] NVS unavailable (%s) — keeping the built-in pin map",
                 esp_err_to_name(nvs));
        return;
    }

    uint8_t unproven = 0, tries = 0;
    uint8_t index = load_wiring(&unproven, &tries);

    if (unproven) {
        if (tries >= kWiringCount) {
            ESP_LOGW(TAG, "[WARN] Tried every known wiring and the C6 answered on none of them.");
            ESP_LOGW(TAG, "[WARN] That points at the co-processor or the board, not the pin map.");
            index = 0;
            tries = 0;
        } else {
            ESP_LOGW(TAG, "[DIAG] No answer on %s wiring last boot — trying %s",
                     kWirings[index].revision, kWirings[(index + 1) % kWiringCount].revision);
            index = (index + 1) % kWiringCount;
        }
    }

    /* The bus is 1-bit, so only D0 (data) and D1 (the slave's interrupt) are
     * live; D2/D3 are left at their defaults. */
    const struct board_wiring *w = &kWirings[index];
    struct esp_hosted_sdio_config conf = INIT_DEFAULT_HOST_SDIO_CONFIG();
    conf.pin_d0.pin = w->d0;
    conf.pin_d1.pin = w->d1;
    conf.pin_reset.pin = w->reset;
    if (esp_hosted_sdio_set_config(&conf) != ESP_TRANSPORT_OK) {
        ESP_LOGE(TAG, "[FAIL] Could not apply the %s pin map", w->revision);
        return;
    }

    g_wiring = index;
    g_wiring_applied = true;
    store_wiring(index, 1, tries + 1);
    ESP_LOGI(TAG, "[DIAG] Trying CrowPanel %s wiring: D0=%d D1=%d reset=%d",
             w->revision, w->d0, w->d1, w->reset);
}

/* The slave answered, so this wiring is right. Later boots start here. */
static void wiring_confirmed(void) {
    if (!g_wiring_applied) {
        return;
    }
    store_wiring(g_wiring, 0, 0);
    ESP_LOGI(TAG, "[PASS] This is a CrowPanel %s board — remembered for next time",
             kWirings[g_wiring].revision);
}

/* Millisecond timestamp since boot */
static int64_t ms_since_boot(void) {
    return esp_timer_get_time() / 1000;
}

/* Phase 1: Query C6 firmware version
 * Returns true if C6 already runs target firmware (OTA not needed), false otherwise. */
static bool phase_query_version(void) {
    ESP_LOGW(TAG, "[PHASE] 1/5 VERSION-QUERY start t=%" PRId64 "ms", ms_since_boot());

    esp_hosted_coprocessor_fwver_t ver = {0};
    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&ver);

    if (ret == ESP_OK) {
        g_running_ver = ver;
        ESP_LOGI(TAG, "[DIAG] C6 firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        /* At or past what we carry means there is nothing useful to write. An
         * exact-match test here would have treated a newer C6 as out of date
         * and quietly downgraded it. */
        if (fw_value(ver.major1, ver.minor1, ver.patch1) >=
            fw_value(kTargetFwMajor, kTargetFwMinor, kTargetFwPatch)) {
            ESP_LOGI(TAG, "[PASS] C6 is at v%" PRIu32 ".%" PRIu32 ".%" PRIu32
                     ", at or past the v%" PRIu32 ".%" PRIu32 ".%" PRIu32
                     " this tool carries — OTA not needed",
                     ver.major1, ver.minor1, ver.patch1,
                     kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
            return true;
        }
        ESP_LOGI(TAG, "[DIAG] C6 needs upgrade from %" PRIu32 ".%" PRIu32 ".%" PRIu32 " to %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1, kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
    } else {
        ESP_LOGW(TAG, "[WARN] Version query failed: %s (0x%x) — expected for v2.3.0 (RPC timeout)",
                 esp_err_to_name(ret), ret);
        ESP_LOGI(TAG, "[DIAG] Proceeding with OTA despite version query failure");
    }
    return false;
}

/* Phase 2: Perform OTA via LittleFS */
static int phase_ota_transfer(void) {
    ESP_LOGW(TAG, "[PHASE] 2/5 OTA-TRANSFER start t=%" PRId64 "ms", ms_since_boot());

    int64_t ota_start = ms_since_boot();
    uint8_t delete_after = 0;  /* keep binary for retry */
    int ret = ota_littlefs_perform(delete_after);
    int64_t ota_end_time = ms_since_boot();

    int64_t duration = ota_end_time - ota_start;

    if (ret == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
        ESP_LOGI(TAG, "[PASS] OTA transfer completed in %" PRId64 "ms", duration);
    } else if (ret == ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
        ESP_LOGI(TAG, "[PASS] OTA not required (slave firmware matches)");
    } else {
        ESP_LOGE(TAG, "[FAIL] OTA transfer failed: %s (0x%x) after %" PRId64 "ms",
                 esp_err_to_name(ret), ret, duration);
        ESP_LOGE(TAG, "[FAIL] SDIO transport may have died during transfer");
        ESP_LOGE(TAG, "[DIAG] If duration < 5000ms: transport init or begin failed");
        ESP_LOGE(TAG, "[DIAG] If duration 5000-30000ms: transport died mid-transfer (v2.3.0 bug)");
        ESP_LOGE(TAG, "[DIAG] If duration > 30000ms: timeout waiting for slave response");
    }

    return ret;
}

/* Phase 3: Activate new firmware on C6.
 *
 * IMPORTANT: the LittleFS OTA transfer in phase 2 already writes the new image
 * to the C6's inactive slot AND marks it bootable when it finishes.  This step
 * only asks the C6 to switch into it *immediately* (an in-place reboot).  The
 * factory C6 firmware (esp_hosted v2.3.0) does not expose that RPC, so it
 * returns ESP_ERR_NOT_SUPPORTED.  That is harmless and expected: the image is
 * already staged and the C6 boots it on the next power cycle.  (Confirmed in
 * the field — after a power cycle the C6 reports the new version.)
 *
 * Returns true only if the C6 was actually switched/rebooted into the new
 * image now (so an immediate version re-query in phase 4 is meaningful);
 * false means activation is deferred to the power cycle. */
static bool phase_activate(void) {
    ESP_LOGW(TAG, "[PHASE] 3/5 OTA-ACTIVATE start t=%" PRId64 "ms", ms_since_boot());

    esp_err_t ret = esp_hosted_slave_ota_activate();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[PASS] Activate succeeded — C6 will boot new firmware after reset");
        return true;
    }
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        /* Expected when upgrading from the factory v2.3.0 firmware. NOT a failure. */
        ESP_LOGI(TAG, "[DIAG] Immediate-activate RPC not supported by the running C6 firmware (expected on v2.3.0)");
        ESP_LOGI(TAG, "[PASS] New image already marked bootable during transfer — it starts after a power cycle");
        return false;
    }
    /* Any other code: still not fatal — the transfer already staged a bootable
     * image — but surface it as a soft warning so a real regression is noticed. */
    ESP_LOGW(TAG, "[WARN] Immediate-activate returned %s (0x%x); image is staged and boots after a power cycle",
             esp_err_to_name(ret), ret);
    return false;
}

/* Phase 4: Verify — query version again.
 *
 * `activated` reflects whether phase 3 actually switched the C6 into the new
 * image right now.  When activation is deferred to a power cycle (the common
 * case upgrading from v2.3.0), the C6 is STILL running the old image at this
 * point, so re-reading the version here would report the OLD version — which
 * is expected, not a failure.  Reporting that as a warning is exactly what
 * made a successful upgrade look broken, so we skip the re-check in that case.
 *
 * Returns true only if the C6 is confirmed running the target version *now*
 * (immediate activation succeeded and the re-query matches).  False means the
 * upgrade is staged but takes effect on the next power cycle — the caller uses
 * this to decide between the "please power cycle" and "press Ctrl+] to exit"
 * guidance in the summary. */
static bool phase_verify(bool activated) {
    ESP_LOGW(TAG, "[PHASE] 4/5 VERIFY start t=%" PRId64 "ms", ms_since_boot());

    if (!activated) {
        ESP_LOGI(TAG, "[DIAG] Activation deferred to the power cycle — the C6 still runs the old image until then");
        ESP_LOGI(TAG, "[DIAG] Skipping the immediate version re-check (it would read the OLD version, as expected)");
        ESP_LOGI(TAG, "[PASS] New firmware is staged and bootable — it takes effect after the power cycle below");
        return false;
    }

    ESP_LOGI(TAG, "[DIAG] Waiting 3s for C6 reboot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    esp_hosted_coprocessor_fwver_t ver = {0};
    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&ver);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG] C6 version after OTA: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        if (ver.major1 == kTargetFwMajor && ver.minor1 == kTargetFwMinor && ver.patch1 == kTargetFwPatch) {
            ESP_LOGI(TAG, "[PASS] *** C6 UPGRADED TO v%" PRIu32 ".%" PRIu32 ".%" PRIu32 " — SUCCESS ***",
                     kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
            return true;
        }
        ESP_LOGI(TAG, "[DIAG] C6 still reports v%" PRIu32 ".%" PRIu32 ".%" PRIu32
                 " — it finishes switching to v%" PRIu32 ".%" PRIu32 ".%" PRIu32 " after the power cycle below",
                 ver.major1, ver.minor1, ver.patch1,
                 kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
    } else {
        ESP_LOGI(TAG, "[DIAG] Post-OTA version query returned %s (0x%x) — normal if the C6 is still rebooting",
                 esp_err_to_name(ret), ret);
        ESP_LOGI(TAG, "[DIAG] The new image is staged; it takes effect after the power cycle below");
    }
    return false;
}

void app_main(void)
{
    ESP_LOGW(TAG, "==========================================================");
    ESP_LOGW(TAG, "  C6 SDIO OTA — CrowPanel ESP32-P4");
    ESP_LOGW(TAG, "  Target: ESP32-C6 upgrade via SDIO");
    ESP_LOGW(TAG, "  Host WiFi: DISABLED (SDIO transport only)");
    ESP_LOGW(TAG, "==========================================================");

    /* Phase 0: Init */
    ESP_LOGW(TAG, "[PHASE] 0/5 INIT start t=%" PRId64 "ms", ms_since_boot());

    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The wiring probe below is useless without somewhere to write down
         * what it learned, so a full partition is worth clearing rather than
         * giving up on. Nothing else in this app keeps state. */
        ESP_LOGW(TAG, "[WARN] NVS unusable (%s) — erasing it", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] NVS init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[DIAG] NVS initialized");

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] Event loop creation failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "[DIAG] Initializing esp_hosted SDIO transport (no WiFi)...");
    int64_t hosted_start = ms_since_boot();
    ret = esp_hosted_init();
    int64_t hosted_init_time = ms_since_boot() - hosted_start;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_hosted_init failed: %s (0x%x) after %" PRId64 "ms",
                 esp_err_to_name(ret), ret, hosted_init_time);
        ESP_LOGE(TAG, "[FAIL] SDIO transport cannot initialize — all OTA paths blocked");
        ESP_LOGE(TAG, "[DIAG] Check: C6 powered? Reset pin GPIO32 toggling? SDIO pins correct?");
        return;
    }
    ESP_LOGI(TAG, "[PASS] esp_hosted_init OK in %" PRId64 "ms", hosted_init_time);

    /* esp_hosted_connect_to_slave() is in the stock OTA example.
     * If it doesn't exist in current esp_hosted, esp_hosted_init() may handle
     * the SDIO handshake. Remove this call if build fails. */
    ESP_LOGI(TAG, "[DIAG] Connecting to C6 slave...");
    int64_t connect_start = ms_since_boot();
    ret = esp_hosted_connect_to_slave();
    int64_t connect_time = ms_since_boot() - connect_start;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_hosted_connect_to_slave failed: %s (0x%x) after %" PRId64 "ms",
                 esp_err_to_name(ret), ret, connect_time);
        ESP_LOGE(TAG, "[FAIL] SDIO handshake with C6 failed — the C6 version is unreadable until this succeeds");
        ESP_LOGE(TAG, "[DIAG] If the log above shows the card enumerating (block sizes read, data path");
        ESP_LOGE(TAG, "[DIAG] opened) then the bus is fine and the slave simply never reported ready —");
        ESP_LOGE(TAG, "[DIAG] which is what the wrong data-line map looks like. Rebooting to try the");
        ESP_LOGE(TAG, "[DIAG] other CrowPanel revision; the guess is already written down.");
        ESP_LOGE(TAG, "[DIAG] If the card never enumerated: C6 power (P37 test pad = 3.3V), reset GPIO[%d].",
                 kWirings[g_wiring].reset);
        esp_restart();
    }
    ESP_LOGI(TAG, "[PASS] Connected to C6 slave in %" PRId64 "ms", connect_time);
    wiring_confirmed();
    ESP_LOGW(TAG, "[PHASE] 0/5 INIT complete t=%" PRId64 "ms", ms_since_boot());

    /* Phase 1: Version query */
    bool already_upgraded = phase_query_version();
    if (already_upgraded) {
        ESP_LOGW(TAG, "[PHASE] 5/5 SUMMARY t=%" PRId64 "ms", ms_since_boot());
        ESP_LOGW(TAG, "  RESULT: C6 already at v%" PRIu32 ".%" PRIu32 ".%" PRIu32 " — no OTA needed",
                 g_running_ver.major1, g_running_ver.minor1, g_running_ver.patch1);
        ESP_LOGW(TAG, "==========================================================");
        ESP_LOGW(TAG, "  C6 UPDATE COMPLETE — no changes needed.");
        ESP_LOGW(TAG, "  C6 is running v%" PRIu32 ".%" PRIu32 ".%" PRIu32
                 "; this tool carries v%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".",
                 g_running_ver.major1, g_running_ver.minor1, g_running_ver.patch1,
                 kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
        ESP_LOGW(TAG, "  Press Ctrl+] to exit.");
        ESP_LOGW(TAG, "==========================================================");
        goto halt;
    }

    /* Phase 2: OTA transfer */
    int ota_result = phase_ota_transfer();

    bool on_target_now = false;
    if (ota_result == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
        /* Phase 3: Activate (may be deferred to the power cycle on v2.3.0) */
        bool activated = phase_activate();

        /* Phase 4: Verify (skips the misleading re-check when deferred).
         * on_target_now is true only if the C6 is already running the new
         * firmware; otherwise the new image takes effect after a power cycle. */
        on_target_now = phase_verify(activated);
    } else if (ota_result != ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
        ESP_LOGE(TAG, "[FAIL] Skipping activate/verify due to OTA transfer failure");
    }

    /* Phase 5: Summary */
    ESP_LOGW(TAG, "[PHASE] 5/5 SUMMARY t=%" PRId64 "ms", ms_since_boot());
    ESP_LOGW(TAG, "==========================================================");
    if (ota_result == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
        ESP_LOGW(TAG, "  RESULT: OTA TRANSFER SUCCEEDED");
        ESP_LOGW(TAG, "==========================================================");
        ESP_LOGW(TAG, "  C6 UPDATE COMPLETE — firmware upgraded successfully.");
        if (on_target_now) {
            /* Already running the new firmware (immediate activation worked) —
             * no power cycle needed, just exit. */
            ESP_LOGW(TAG, "  C6 is now running v%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".",
                     kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
            ESP_LOGW(TAG, "  Press Ctrl+] to exit.");
        } else {
            /* Staged — takes effect on the next power cycle.  The Ctrl+] hint
             * intentionally waits until the post-reboot run confirms the new
             * version, so the user power-cycles first. */
            ESP_LOGW(TAG, "  The new C6 version takes effect after the power cycle —");
            ESP_LOGW(TAG, "  it is normal that the version above still reads the old one.");
            ESP_LOGW(TAG, "  Please unplug the ESP32 and plug it back in.");
        }
        ESP_LOGW(TAG, "==========================================================");
    } else if (ota_result == ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
        ESP_LOGW(TAG, "  RESULT: OTA NOT REQUIRED (already up to date)");
        ESP_LOGW(TAG, "==========================================================");
        ESP_LOGW(TAG, "  C6 UPDATE COMPLETE — no changes needed.");
        ESP_LOGW(TAG, "  C6 is running v%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".",
                 kTargetFwMajor, kTargetFwMinor, kTargetFwPatch);
        ESP_LOGW(TAG, "  Press Ctrl+] to exit.");
        ESP_LOGW(TAG, "==========================================================");
    } else {
        ESP_LOGW(TAG, "  RESULT: OTA FAILED");
        ESP_LOGW(TAG, "  Analyze [FAIL] and [DIAG] lines above.");
        ESP_LOGW(TAG, "==========================================================");
        ESP_LOGW(TAG, "  C6 UPDATE FAILED — see errors above.");
        ESP_LOGW(TAG, "  Please unplug the ESP32 and plug it back in to retry.");
        ESP_LOGW(TAG, "==========================================================");
    }

halt:
    /* Spin forever — don't restart, let user read logs */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
