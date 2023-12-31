// Select desired board from the list below
// #define BOARD_WROVER_KIT 1
// #define BOARD_CAMERA_MODEL_ESP_EYE 1
#define BOARD_ESP32CAM_AITHINKER 1
// #define BOARD_CAMERA_MODEL_TTGO_T_JOURNAL 1

#include <nvs_flash.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>

#include "camera_pins.h"
#include "wifi_connection.h"
#include "sd_card_reader.h"

static const char *TAG = "camera_server";

// Web stream metadata
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"; 

static esp_err_t init_camera(void) {
    camera_config_t camera_config = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,

        .jpeg_quality = 10,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY
    };
    
    esp_err_t err = esp_camera_init(&camera_config);
    
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

// API handler
esp_err_t get_index_handler(httpd_req_t* req)
{
    /* Send a simple response */
    const char resp[] = "<html> \
                            <head> \
                                <link rel=\"stylesheet\" href=\"https://stackpath.bootstrapcdn.com/bootstrap/4.1.3/css/bootstrap.min.css\" integrity=\"sha384-MCw98/SFnGE8fJT3GXwEOngsV7Zt27NXFoaoApmYm81iuXoPkFOJwJ8ERdknLPMO\" crossorigin=\"anonymous\"> \
                                <title>ESP32-CAM</title> \
                            </head> \
                            <body> \
                                <div class=\"container\"> \
                                    <div class=\"row\"> \
                                        <div class=\"col-lg-8  offset-lg-2\"> \
                                            <h3 class=\"mt-5\">Live Streaming</h3> \
                                            <img src=\"/stream\" width=\"100%\"> \
                                        </div> \
                                    </div> \
                                </div> \
                            </body> \
                        </html>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Camera streamer handler
esp_err_t jpg_stream_httpd_handler(httpd_req_t *req) {
    camera_fb_t* fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t* _jpg_buf;
    char* part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        // Capture camera frame
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // Convert to jpeg if needed
        if(fb->format != PIXFORMAT_JPEG) {
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted) {
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } 
        else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        // Stream captured data
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }

        // Free captured data
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK) {
            break;
        }

        // Calculate frame rate
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
    }

    last_frame = 0;
    return res;
}

// Setup HTTP server
httpd_uri_t uri_get = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = jpg_stream_httpd_handler,
    .user_ctx = NULL
};

// Setup HTTP server
httpd_uri_t uri_index_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = get_index_handler,
    .user_ctx = NULL
};

httpd_handle_t setup_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t stream_httpd = NULL;

    // Start the httpd server and register handlers
    if (httpd_start(&stream_httpd , &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &uri_get);
        httpd_register_uri_handler(stream_httpd, &uri_index_get);
    }

    return stream_httpd;
}

void app_main() {
    esp_err_t err;

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    // Initialize SD card
    err = init_sd_card();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init error: %s", esp_err_to_name(err));
        return;
    }

    // Load wifi credentials from SD card
    char ssid[32];
    char password[64];
    load_wifi_credentials(ssid, password);

    // Connect to wifi
    connect_wifi(ssid, password);
    if (wifi_connect_status) {
        // Initialize camera
        err = init_camera();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init camera: %s", esp_err_to_name(err));
            return;
        }

        // Start web server
        setup_server();
        ESP_LOGI(TAG, "ESP32 Web Camera Streaming Server is up and running");
    }
    else {
        ESP_LOGI(TAG, "Failed to connected to Wifi, check your network credentials");
    }
}
