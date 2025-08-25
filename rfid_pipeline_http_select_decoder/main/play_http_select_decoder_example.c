#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "esp_heap_caps.h"

#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"

#include "audio_idf_version.h"

// RC522 RFID includes
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

#define SELECT_MP3_DECODER 1

#if defined SELECT_AAC_DECODER
#include "aac_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "aac";
#elif defined SELECT_AMR_DECODER
#include "amr_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "amr";
#elif defined SELECT_FLAC_DECODER
#include "flac_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "flac";
#elif defined SELECT_MP3_DECODER
#include "mp3_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "mp3";
#elif defined SELECT_OGG_DECODER
#include "ogg_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "ogg";
#elif defined SELECT_OPUS_DECODER
#include "opus_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "opus";
#else
#include "wav_decoder.h"
static const char *TAG = "RFID_AUDIO_PLAYER";
static const char *selected_decoder_name = "wav";
#endif

// RC522 RFID Configuration
#define RC522_SPI_BUS_GPIO_MISO    (12)
#define RC522_SPI_BUS_GPIO_MOSI    (13)
#define RC522_SPI_BUS_GPIO_SCLK    (14)
#define RC522_SPI_SCANNER_GPIO_SDA (15)
#define RC522_SCANNER_GPIO_RST     (4) // soft-reset

// RFID to URL mapping structure
typedef struct {
    uint8_t uid[10];
    uint8_t uid_length;
    const char* url;
    const char* name;
} rfid_audio_mapping_t;

// RFID tag mappings
static const rfid_audio_mapping_t rfid_mappings[] = {
        {
        .uid = {0x04, 0x5F, 0xAC, 0xCA, 0x97, 0x69, 0x81},
        .uid_length = 7,
        .url = "http://littlecubbie.duckdns.org/uploads/LC_intro.mp3",
        .name = "LC_intro"
    },
    {
        .uid = {0x73, 0x51, 0xCE, 0x0D},
        .uid_length = 4,
        .url = "http://littlecubbie.duckdns.org/uploads/Krishna_intro.mp3",
        .name = "Krishna_intro"
    },
    {
        .uid = {0x43, 0x86, 0x49, 0x10},
        .uid_length = 4,
        .url = "http://littlecubbie.duckdns.org/uploads/Alex_intro.mp3",
        .name = "Alex_intro"
    }
};

#define NUM_RFID_MAPPINGS (sizeof(rfid_mappings) / sizeof(rfid_mappings[0]))

// Global variables for audio system
static audio_pipeline_handle_t pipeline = NULL;
static audio_element_handle_t http_stream_reader = NULL;
static audio_element_handle_t i2s_stream_writer = NULL;
static audio_element_handle_t selected_decoder = NULL;
static audio_event_iface_handle_t evt = NULL;
static bool audio_playing = false;
static const char* current_playing_url = NULL;

// Global variables for RFID system
static rc522_spi_config_t driver_config = {
    .host_id = SPI3_HOST,
    .bus_config = &(spi_bus_config_t){
        .miso_io_num = RC522_SPI_BUS_GPIO_MISO,
        .mosi_io_num = RC522_SPI_BUS_GPIO_MOSI,
        .sclk_io_num = RC522_SPI_BUS_GPIO_SCLK,
    },
    .dev_config = {
        .spics_io_num = RC522_SPI_SCANNER_GPIO_SDA,
    },
    .rst_io_num = RC522_SCANNER_GPIO_RST,
};

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

void log_memory_usage(const char *stage)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t min_ever_free = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    ESP_LOGI(TAG, "[MEM-%s] Free heap: %d | Largest block: %d | Min ever free: %d",
             stage, free_heap, largest_block, min_ever_free);
}

// Audio control functions
bool is_audio_playing(void)
{
    return audio_playing;
}

esp_err_t stop_current_audio(void)
{
    if (!audio_playing || !pipeline) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping current audio playback");

    esp_err_t ret = audio_pipeline_stop(pipeline);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop pipeline: %d", ret);
        return ret;
    }

    ret = audio_pipeline_wait_for_stop(pipeline);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wait for pipeline stop: %d", ret);
        return ret;
    }

    ret = audio_pipeline_reset_ringbuffer(pipeline);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset pipeline ringbuffer: %d", ret);
    }

    audio_playing = false;
    current_playing_url = NULL;
    ESP_LOGI(TAG, "Audio playback stopped successfully");

    return ESP_OK;
}

esp_err_t start_audio_for_url(const char* url)
{
    if (!url || !pipeline || !http_stream_reader) {
        ESP_LOGE(TAG, "Invalid parameters for starting audio");
        return ESP_ERR_INVALID_ARG;
    }

    // Stop current audio if playing
    if (audio_playing) {
        esp_err_t ret = stop_current_audio();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop current audio before starting new: %d", ret);
            return ret;
        }
    }

    ESP_LOGI(TAG, "Starting audio playback for URL: %s", url);

    // Set the new URI
    esp_err_t ret = audio_element_set_uri(http_stream_reader, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URI: %d", ret);
        return ret;
    }

    // Start the pipeline
    ret = audio_pipeline_run(pipeline);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start pipeline: %d", ret);
        return ret;
    }

    audio_playing = true;
    current_playing_url = url;
    ESP_LOGI(TAG, "Audio playback started successfully");

    return ESP_OK;
}

// RFID helper functions
bool compare_uid(const rc522_picc_uid_t* uid1, const uint8_t* uid2, uint8_t length)
{
    if (uid1->length != length) {
        return false;
    }

    for (int i = 0; i < length; i++) {
        if (uid1->value[i] != uid2[i]) {
            return false;
        }
    }

    return true;
}

const char* find_url_for_uid(const rc522_picc_uid_t* uid)
{
    for (int i = 0; i < NUM_RFID_MAPPINGS; i++) {
        if (compare_uid(uid, rfid_mappings[i].uid, rfid_mappings[i].uid_length)) {
            ESP_LOGI(TAG, "Found mapping for UID: %s -> %s",
                     rfid_mappings[i].name, rfid_mappings[i].url);
            return rfid_mappings[i].url;
        }
    }

    ESP_LOGW(TAG, "No mapping found for UID");
    return NULL;
}

// RFID event handler
static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        ESP_LOGI(TAG, "RFID card detected");
        rc522_picc_print(picc);

        const char* url = find_url_for_uid(&picc->uid);
        if (url) {
            esp_err_t ret = start_audio_for_url(url);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start audio for detected card: %d", ret);
            }
        } else {
            ESP_LOGW(TAG, "Unknown RFID card detected, no audio mapping found");
        }
    }
    else if (picc->state == RC522_PICC_STATE_IDLE && event->old_state >= RC522_PICC_STATE_ACTIVE) {
        ESP_LOGI(TAG, "RFID card removed");
        esp_err_t ret = stop_current_audio();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop audio after card removal: %d", ret);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== RFID Audio Player Starting ===");

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[ 1 ] Initialize audio codec chip");
    log_memory_usage("START_CODEC");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    log_memory_usage("AFTER_CODEC_INIT");

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);
    log_memory_usage("PIPELINE_INIT");

    ESP_LOGI(TAG, "[ 2.1 ] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.out_rb_size = 1024 * 1024;
    http_stream_reader = http_stream_init(&http_cfg);
    log_memory_usage("HTTP_STREAM");

    ESP_LOGI(TAG, "[ 2.2 ] Create %s decoder", selected_decoder_name);
#if defined SELECT_AAC_DECODER
    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    selected_decoder = aac_decoder_init(&aac_cfg);
#elif defined SELECT_AMR_DECODER
    amr_decoder_cfg_t amr_cfg = DEFAULT_AMR_DECODER_CONFIG();
    selected_decoder = amr_decoder_init(&amr_cfg);
#elif defined SELECT_FLAC_DECODER
    flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    flac_cfg.out_rb_size = 500 * 1024;
    selected_decoder = flac_decoder_init(&flac_cfg);
#elif defined SELECT_MP3_DECODER
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    selected_decoder = mp3_decoder_init(&mp3_cfg);
#elif defined SELECT_OGG_DECODER
    ogg_decoder_cfg_t ogg_cfg = DEFAULT_OGG_DECODER_CONFIG();
    selected_decoder = ogg_decoder_init(&ogg_cfg);
#elif defined SELECT_OPUS_DECODER
    opus_decoder_cfg_t opus_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    selected_decoder = decoder_opus_init(&opus_cfg);
#else
    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    selected_decoder = wav_decoder_init(&wav_cfg);
#endif
    log_memory_usage("DECODER_INIT");

    ESP_LOGI(TAG, "[ 2.3 ] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    log_memory_usage("I2S_STREAM");

    ESP_LOGI(TAG, "[ 2.4 ] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, selected_decoder, selected_decoder_name);
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[ 2.5 ] Link pipeline http->%s->i2s", selected_decoder_name);
    const char *link_tag[3] = {"http", selected_decoder_name, "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    // Note: We don't set a URI here - it will be set dynamically when RFID cards are detected

    ESP_LOGI(TAG, "[ 3 ] Start Wi-Fi");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    periph_wifi_cfg_t wifi_cfg = {
        .wifi_config.sta.ssid = "You broadband",
        .wifi_config.sta.password = "0228503933",
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    log_memory_usage("WIFI_CONNECTED");

    ESP_LOGI(TAG, "[ 4 ] Set up audio event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Initialize RFID system");
    rc522_spi_create(&driver_config, &driver);
    rc522_driver_install(driver);

    rc522_config_t scanner_config = {
        .driver = driver,
    };

    rc522_create(&scanner_config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
    rc522_start(scanner);
    log_memory_usage("RFID_INITIALIZED");

    ESP_LOGI(TAG, "[ 6 ] System ready - waiting for RFID cards...");

    // Main event loop - runs continuously
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, 100); // 100ms timeout

        if (ret == ESP_OK) {
            // Handle audio events
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *) selected_decoder
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(selected_decoder, &music_info);
                ESP_LOGI(TAG, "[ * ] Music info: rate=%d bits=%d ch=%d",
                         music_info.sample_rates, music_info.bits, music_info.channels);
                i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                continue;
            }

            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                ESP_LOGI(TAG, "[ * ] Audio playback finished");
                audio_playing = false;
                current_playing_url = NULL;
                continue;
            }

            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) http_stream_reader
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && ((int)msg.data == AEL_STATUS_ERROR_OPEN)) {
                ESP_LOGE(TAG, "[ * ] HTTP stream error - failed to open URL");
                audio_playing = false;
                current_playing_url = NULL;
                continue;
            }
        } else if (ret != ESP_ERR_TIMEOUT) {
            // ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
        }

        // Small delay to prevent busy waiting
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // This point should never be reached in normal operation
    ESP_LOGE(TAG, "Main loop exited unexpectedly!");
}
