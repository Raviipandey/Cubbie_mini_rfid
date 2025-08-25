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

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

#define SELECT_MP3_DECODER 1

#if defined SELECT_AAC_DECODER
#include "aac_decoder.h"
static const char *TAG = "HTTP_SELECT_AAC_EXAMPLE";
static const char *selected_decoder_name = "aac";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac";
#elif defined SELECT_AMR_DECODER
#include "amr_decoder.h"
static const char *TAG = "HTTP_SELECT_AMR_EXAMPLE";
static const char *selected_decoder_name = "amr";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-1c-8000hz.amr";
#elif defined SELECT_FLAC_DECODER
#include "flac_decoder.h"
static const char *TAG = "HTTP_SELECT_FLAC_EXAMPLE";
static const char *selected_decoder_name = "flac";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.flac";
#elif defined SELECT_MP3_DECODER
#include "mp3_decoder.h"
static const char *TAG = "HTTP_SELECT_MP3_EXAMPLE";
static const char *selected_decoder_name = "mp3";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3";
#elif defined SELECT_OGG_DECODER
#include "ogg_decoder.h"
static const char *TAG = "HTTP_SELECT_OGG_EXAMPLE";
static const char *selected_decoder_name = "ogg";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.ogg";
#elif defined SELECT_OPUS_DECODER
#include "opus_decoder.h"
static const char *TAG = "HTTP_SELECT_OPUS_EXAMPLE";
static const char *selected_decoder_name = "opus";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.opus";
#else
#include "wav_decoder.h"
static const char *TAG = "HTTP_SELECT_WAV_EXAMPLE";
static const char *selected_decoder_name = "wav";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.wav";
#endif

void log_memory_usage(const char *stage)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t min_ever_free = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    ESP_LOGI(TAG, "[MEM-%s] Free heap: %d | Largest block: %d | Min ever free: %d",
             stage, free_heap, largest_block, min_ever_free);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_reader, i2s_stream_writer, selected_decoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    log_memory_usage("START_CODEC");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    log_memory_usage("AFTER_CODEC_INIT");

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);
    log_memory_usage("PIPELINE_INIT");

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.out_rb_size = 1024 * 1024;
    http_stream_reader = http_stream_init(&http_cfg);
    log_memory_usage("HTTP_STREAM");

    ESP_LOGI(TAG, "[2.2] Create %s decoder", selected_decoder_name);
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

    ESP_LOGI(TAG, "[2.3] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    log_memory_usage("I2S_STREAM");

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, selected_decoder, selected_decoder_name);
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.5] Link pipeline http->%s->i2s", selected_decoder_name);
    const char *link_tag[3] = {"http", selected_decoder_name, "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    audio_element_set_uri(http_stream_reader, selected_file_to_play);

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

    ESP_LOGI(TAG, "[ 4 ] Set up event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);
    log_memory_usage("PIPELINE_RUNNING");

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

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
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 6 ] Stop and release resources");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, http_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, selected_decoder);

    audio_pipeline_remove_listener(pipeline);
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);
    audio_event_iface_destroy(evt);

    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(selected_decoder);
    esp_periph_set_destroy(set);
    log_memory_usage("AFTER_CLEANUP");
}
