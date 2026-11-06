// 1. CHƯƠNG TRÌNH ESP32-S3 CAMERA CAO TỐC (MỤC TIÊU >= 30 FPS ỔN ĐỊNH)
//     Chức năng: Xử lý AI nhận diện, đếm ốc & tán (screws/nuts) thời gian thực,
//                kiến trúc bất đồng bộ: Camera Stream 30+ FPS + AI Worker song song trên Core 1,
//                hiển thị LCD 1602 bất đồng bộ trên Core 0,
//                stream video TCP siêu tốc lên Web Server với khả năng tự phục hồi.

#include <Arduino.h>
#include <esp_camera.h>
#include <Detect_inferencing.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "wifi_config.h" // SSID & mật khẩu WiFi (gitignored — xem wifi_config.h.example)

// -----CẤU HÌNH WiFi & SERVER----

// Địa chỉ Server (Port 5001 cho luồng TCP Stream tốc độ cao, Port 5000 cho Web HTTP)
const char *server_host = "192.168.137.1";
const int server_stream_port = 5001;

// -----CẤU HÌNH LCD 1602----

#define I2C_SDA 14
#define I2C_SCL 21

LiquidCrystal_I2C lcd(0x27, 16, 2);
bool lcd_ok = false;

// ----CẤU HÌNH CAMERA ESP32-S3----

#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5
#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

#define CONFIDENCE_THRESHOLD 0.60f

#define FRAME_W 96
#define FRAME_H 96
#define RGB565_BUF_SIZE (FRAME_W * FRAME_H * 2)

// ----BIẾN TOÀN CỤC & BỘ ĐẾM----

int total_screw = 0;
int total_nuts = 0;
int currentScrew = 0;
int currentNuts = 0;

struct TrackedObject
{
    int id;
    float x;
    float y;
    int rx, ry, rw, rh;
    int disappeared;
    bool matched;
};

#define MAX_TRACKED 20
#define DIST_THRESHOLD 25.0f
#define DIST_THRESHOLD_SQ (DIST_THRESHOLD * DIST_THRESHOLD) // 625.0f - tối ưu hóa không dùng sqrt()
#define MAX_DISAPPEARED 8

TrackedObject tracked_screws[MAX_TRACKED];
int tracked_screws_count = 0;
int screw_id_counter = 0;

TrackedObject tracked_nuts[MAX_TRACKED];
int tracked_nuts_count = 0;
int nuts_id_counter = 0;

// Giao tiếp liên tác vụ (Core 1 <-> Core 0)
SemaphoreHandle_t xFrameMutex = NULL;
SemaphoreHandle_t xFrameReadySem = NULL;

static uint8_t shared_rgb_buf[RGB565_BUF_SIZE];
static bool shared_has_new_frame = false;
static int shared_cs = 0, shared_cn = 0, shared_ts = 0, shared_tn = 0;
static int shared_epoch = 0;
static float shared_fps = 0.0f;

int current_epoch = 0;
volatile bool reset_requested = false;

// Đo lường FPS thực tế trên Core 1
static float g_measured_fps = 0.0f;

// ----CẤU TRÚC GIAO TIẾP ASYNC AI & CAMERA STREAM TRÊN CORE 1----

struct Detection
{
    float x, y;
    int rx, ry, rw, rh;
    uint8_t type; // 1 = screw, 2 = nuts
};

#define MAX_DETECTIONS 10
static Detection g_latest_detections[MAX_DETECTIONS];
static int g_latest_detections_count = 0;
static volatile bool g_has_new_detections = false;

static SemaphoreHandle_t xAiMutex = NULL;
static SemaphoreHandle_t xAiTriggerSem = NULL;
static volatile bool g_ai_worker_busy = false;

// Buffer độc lập cho AI Worker (không bao giờ block camera frame)
static uint8_t ai_input_buf[RGB565_BUF_SIZE];

// Cấu trúc gói tin truyền TCP siêu tốc (20 bytes header + JPEG)
struct __attribute__((packed)) StreamPacketHeader
{
    uint32_t magic;   // 0xAA55AA55
    uint16_t epoch;   // Epoch hiện tại
    uint16_t cs;      // Current Screw
    uint16_t cn;      // Current Nuts
    uint16_t ts;      // Total Screws
    uint16_t tn;      // Total Nuts
    uint16_t fps;     // FPS * 10 (ví dụ 302 = 30.2 fps)
    uint32_t jpg_len; // Chiều dài dữ liệu JPEG đi kèm
};

// ----HÀM RESET----

void doResetCounters()
{
    total_screw = 0;
    total_nuts = 0;
    currentScrew = 0;
    currentNuts = 0;
    tracked_screws_count = 0;
    tracked_nuts_count = 0;
    screw_id_counter = 0;
    nuts_id_counter = 0;

    if (xAiMutex && xSemaphoreTake(xAiMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        g_latest_detections_count = 0;
        g_has_new_detections = false;
        xSemaphoreGive(xAiMutex);
    }

    if (lcd_ok)
    {
        lcd.setCursor(0, 0);
        lcd.print("Scr: C:0   T:0   ");
        lcd.setCursor(0, 1);
        lcd.print("Nut: C:0   T:0   ");
    }
}

// -----KHỞI TẠO & CẬP NHẬT LCD (CHẠY TRÊN CORE 0)-----

void initLCD()
{
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000); // 400kHz I2C Fast Mode
    Wire.setTimeOut(20);

    Wire.beginTransmission(0x27);
    if (Wire.endTransmission() == 0)
    {
        lcd = LiquidCrystal_I2C(0x27, 16, 2);
        lcd.init();
        lcd.backlight();
        lcd.clear();
        lcd_ok = true;

        lcd.setCursor(0, 0);
        lcd.print("Scr: C:0   T:0   ");
        lcd.setCursor(0, 1);
        lcd.print("Nut: C:0   T:0   ");
    }
    else
    {
        Serial.println("[LCD] Không tìm thấy LCD 1602 I2C tại địa chỉ 0x27!");
        lcd_ok = false;
    }
}

void updateLCDDisplay()
{
    static int last_cs = -1, last_cn = -1, last_ts = -1, last_tn = -1;

    if (currentScrew == last_cs && currentNuts == last_cn &&
        total_screw == last_ts && total_nuts == last_tn)
    {
        return; // Không đổi số thì không ghi I2C để tiết kiệm chu kỳ bus
    }

    last_cs = currentScrew;
    last_cn = currentNuts;
    last_ts = total_screw;
    last_tn = total_nuts;

    char line1[17];
    char line2[17];

    lcd.setCursor(0, 0);
    snprintf(line1, sizeof(line1), "Scr: C:%-2d  T:%-3d", currentScrew, total_screw);
    lcd.print(line1);

    lcd.setCursor(0, 1);
    snprintf(line2, sizeof(line2), "Nut: C:%-2d  T:%-3d", currentNuts, total_nuts);
    lcd.print(line2);
}

// Task cập nhật LCD chạy trên Core 0 độc lập để không làm nghẽn Core 1
void lcd_task(void *pvParameters)
{
    while (1)
    {
        if (lcd_ok)
        {
            updateLCDDisplay();
        }
        vTaskDelay(pdMS_TO_TICKS(150)); // Cập nhật định kỳ 150ms (~6.6 Hz)
    }
}

// ------BOUNDING BOX & TRACKING (TỐI ƯU HÓA CAO TỐC)-----

// Đặt vào IRAM để vẽ hộp giới hạn tức thì không chịu độ trễ Flash Cache
void IRAM_ATTR drawRectangle(uint8_t *img_data, int img_width, int img_height,
                             int x, int y, int width, int height,
                             uint8_t r, uint8_t g, uint8_t b)
{
    int x1 = max(0, x);
    int y1 = max(0, y);
    int x2 = min(img_width - 1, x + width);
    int y2 = min(img_height - 1, y + height);

    int thickness = 2;
    uint16_t color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    color = (color >> 8) | (color << 8); // Khớp thứ tự byte camera

    uint16_t *buf16 = (uint16_t *)img_data;

    for (int i = 0; i < thickness; i++)
    {
        int row_top = (y1 + i) * img_width;
        int row_bot = (y2 - i) * img_width;
        bool valid_top = (y1 + i < img_height);
        bool valid_bot = (y2 - i >= 0 && y2 - i < img_height);

        for (int px = x1; px <= x2; px++)
        {
            if (valid_top)
                buf16[row_top + px] = color;
            if (valid_bot)
                buf16[row_bot + px] = color;
        }
        for (int py = y1; py <= y2; py++)
        {
            int row_curr = py * img_width;
            if (x1 + i < img_width)
                buf16[row_curr + (x1 + i)] = color;
            if (x2 - i >= 0 && x2 - i < img_width)
                buf16[row_curr + (x2 - i)] = color;
        }
    }
}

void processTrackingDetections(const Detection *dets, int det_cnt)
{
    Detection det_screws[MAX_TRACKED];
    int det_screws_cnt = 0;

    Detection det_nuts[MAX_TRACKED];
    int det_nuts_cnt = 0;

    for (int i = 0; i < det_cnt; i++)
    {
        if (dets[i].type == 1 && det_screws_cnt < MAX_TRACKED)
            det_screws[det_screws_cnt++] = dets[i];
        else if (dets[i].type == 2 && det_nuts_cnt < MAX_TRACKED)
            det_nuts[det_nuts_cnt++] = dets[i];
    }

    currentScrew = det_screws_cnt;
    currentNuts = det_nuts_cnt;

    // Tracking Screws (Dùng bình phương khoảng cách - loại bỏ hoàn toàn sqrt)
    for (int i = 0; i < tracked_screws_count; i++)
        tracked_screws[i].matched = false;

    for (int d = 0; d < det_screws_cnt; d++)
    {
        float min_dist_sq = 999999.0f;
        int best_match = -1;

        for (int t = 0; t < tracked_screws_count; t++)
        {
            if (tracked_screws[t].matched)
                continue;
            float dx = det_screws[d].x - tracked_screws[t].x;
            float dy = det_screws[d].y - tracked_screws[t].y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq < DIST_THRESHOLD_SQ && dist_sq < min_dist_sq)
            {
                min_dist_sq = dist_sq;
                best_match = t;
            }
        }

        if (best_match != -1)
        {
            tracked_screws[best_match].x = det_screws[d].x;
            tracked_screws[best_match].y = det_screws[d].y;
            tracked_screws[best_match].rx = det_screws[d].rx;
            tracked_screws[best_match].ry = det_screws[d].ry;
            tracked_screws[best_match].rw = det_screws[d].rw;
            tracked_screws[best_match].rh = det_screws[d].rh;
            tracked_screws[best_match].disappeared = 0;
            tracked_screws[best_match].matched = true;
        }
        else
        {
            if (tracked_screws_count < MAX_TRACKED)
            {
                tracked_screws[tracked_screws_count] = {
                    .id = ++screw_id_counter,
                    .x = det_screws[d].x,
                    .y = det_screws[d].y,
                    .rx = det_screws[d].rx,
                    .ry = det_screws[d].ry,
                    .rw = det_screws[d].rw,
                    .rh = det_screws[d].rh,
                    .disappeared = 0,
                    .matched = true};
                tracked_screws_count++;
                total_screw++;
            }
        }
    }

    int new_screw_count = 0;
    for (int t = 0; t < tracked_screws_count; t++)
    {
        if (!tracked_screws[t].matched)
            tracked_screws[t].disappeared++;
        if (tracked_screws[t].disappeared <= MAX_DISAPPEARED)
        {
            tracked_screws[new_screw_count++] = tracked_screws[t];
        }
    }
    tracked_screws_count = new_screw_count;

    // Tracking Nuts (Dùng bình phương khoảng cách - loại bỏ hoàn toàn sqrt)
    for (int i = 0; i < tracked_nuts_count; i++)
        tracked_nuts[i].matched = false;

    for (int d = 0; d < det_nuts_cnt; d++)
    {
        float min_dist_sq = 999999.0f;
        int best_match = -1;

        for (int t = 0; t < tracked_nuts_count; t++)
        {
            if (tracked_nuts[t].matched)
                continue;
            float dx = det_nuts[d].x - tracked_nuts[t].x;
            float dy = det_nuts[d].y - tracked_nuts[t].y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq < DIST_THRESHOLD_SQ && dist_sq < min_dist_sq)
            {
                min_dist_sq = dist_sq;
                best_match = t;
            }
        }

        if (best_match != -1)
        {
            tracked_nuts[best_match].x = det_nuts[d].x;
            tracked_nuts[best_match].y = det_nuts[d].y;
            tracked_nuts[best_match].rx = det_nuts[d].rx;
            tracked_nuts[best_match].ry = det_nuts[d].ry;
            tracked_nuts[best_match].rw = det_nuts[d].rw;
            tracked_nuts[best_match].rh = det_nuts[d].rh;
            tracked_nuts[best_match].disappeared = 0;
            tracked_nuts[best_match].matched = true;
        }
        else
        {
            if (tracked_nuts_count < MAX_TRACKED)
            {
                tracked_nuts[tracked_nuts_count] = {
                    .id = ++nuts_id_counter,
                    .x = det_nuts[d].x,
                    .y = det_nuts[d].y,
                    .rx = det_nuts[d].rx,
                    .ry = det_nuts[d].ry,
                    .rw = det_nuts[d].rw,
                    .rh = det_nuts[d].rh,
                    .disappeared = 0,
                    .matched = true};
                tracked_nuts_count++;
                total_nuts++;
            }
        }
    }

    int new_nuts_count = 0;
    for (int t = 0; t < tracked_nuts_count; t++)
    {
        if (!tracked_nuts[t].matched)
            tracked_nuts[t].disappeared++;
        if (tracked_nuts[t].disappeared <= MAX_DISAPPEARED)
        {
            tracked_nuts[new_nuts_count++] = tracked_nuts[t];
        }
    }
    tracked_nuts_count = new_nuts_count;
}

// Trích xuất đặc trưng từ ai_input_buf (đặt tại IRAM)
static int IRAM_ATTR raw_feature_get_data(size_t offset, size_t length, float *out_ptr)
{
    const uint8_t *buf = ai_input_buf + (offset * 2);
    for (size_t i = 0; i < length; i++)
    {
        uint8_t b0 = buf[i * 2];
        uint8_t b1 = buf[i * 2 + 1];

        uint32_t r = b0 & 0xF8;
        uint32_t g = ((b0 & 0x07) << 5) | ((b1 & 0xE0) >> 3);
        uint32_t b = (b1 & 0x1F) << 3;

        out_ptr[i] = (float)((r << 16) | (g << 8) | b);
    }
    return 0;
}

// ------TASK AI WORKER BẤT ĐỒNG BỘ (Core 1, Priority 2)---------
void ai_worker_task(void *pvParameters)
{
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &raw_feature_get_data;

    while (1)
    {
        if (xSemaphoreTake(xAiTriggerSem, portMAX_DELAY) == pdTRUE)
        {
            ei_impulse_result_t result = {0};
            EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);

            if (res == EI_IMPULSE_OK)
            {
                Detection temp_dets[MAX_DETECTIONS];
                int det_cnt = 0;

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
                for (size_t ix = 0; ix < result.bounding_boxes_count && det_cnt < MAX_DETECTIONS; ix++)
                {
                    auto bb = result.bounding_boxes[ix];
                    if (bb.value < CONFIDENCE_THRESHOLD)
                        continue;

                    float cx = bb.x + bb.width / 2.0f;
                    float cy = bb.y + bb.height / 2.0f;

                    uint8_t t = 0;
                    if (strcmp(bb.label, "screw") == 0)
                        t = 1;
                    else if (strcmp(bb.label, "nuts") == 0 || strcmp(bb.label, "nut") == 0)
                        t = 2;

                    if (t != 0)
                    {
                        temp_dets[det_cnt].x = cx;
                        temp_dets[det_cnt].y = cy;
                        temp_dets[det_cnt].rx = (int)bb.x;
                        temp_dets[det_cnt].ry = (int)bb.y;
                        temp_dets[det_cnt].rw = (int)bb.width;
                        temp_dets[det_cnt].rh = (int)bb.height;
                        temp_dets[det_cnt].type = t;
                        det_cnt++;
                    }
                }
#endif

                if (xSemaphoreTake(xAiMutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    memcpy(g_latest_detections, temp_dets, sizeof(Detection) * det_cnt);
                    g_latest_detections_count = det_cnt;
                    g_has_new_detections = true;
                    xSemaphoreGive(xAiMutex);
                }
            }

            g_ai_worker_busy = false;
        }
    }
}

// ------TASK CAMERA & STREAM MASTER (Core 1, Priority 3 - ĐẠT >= 30 FPS)---------
void camera_stream_task(void *pvParameters)
{
    uint32_t frame_counter = 0;
    uint32_t last_fps_time = millis();

    while (1)
    {
        if (reset_requested)
        {
            reset_requested = false;
            doResetCounters();
        }

        camera_fb_t *live_fb = esp_camera_fb_get();
        if (!live_fb)
        {
            taskYIELD();
            continue;
        }

        // 1. Nếu AI Worker đang rảnh, gửi frame mới cho AI suy luận song song
        if (!g_ai_worker_busy)
        {
            memcpy(ai_input_buf, live_fb->buf, RGB565_BUF_SIZE);
            g_ai_worker_busy = true;
            xSemaphoreGive(xAiTriggerSem);
        }

        // 2. Nếu AI Worker vừa có kết quả phát hiện mới, cập nhật tracker ngay lập tức
        if (g_has_new_detections)
        {
            Detection local_dets[MAX_DETECTIONS];
            int local_cnt = 0;
            bool got_dets = false;

            if (xSemaphoreTake(xAiMutex, 0) == pdTRUE)
            {
                memcpy(local_dets, g_latest_detections, sizeof(Detection) * g_latest_detections_count);
                local_cnt = g_latest_detections_count;
                g_has_new_detections = false;
                xSemaphoreGive(xAiMutex);
                got_dets = true;
            }

            // Chỉ chạy tracker khi đã lấy được dữ liệu thật. Nếu lấy mutex thất bại
            // mà vẫn gọi với 0 phát hiện thì bộ đếm sẽ nhảy về 0 và track bị xóa oan
            // (dẫn đến đếm tăng đúp khi vật xuất hiện lại)
            if (got_dets)
            {
                processTrackingDetections(local_dets, local_cnt);
            }
        }

        // 3. Vẽ bounding box liên tục trên mỗi frame ở 30 FPS
        for (int i = 0; i < tracked_screws_count; i++)
        {
            if (tracked_screws[i].disappeared <= 2 && tracked_screws[i].rw > 0 && tracked_screws[i].rh > 0)
            {
                drawRectangle((uint8_t *)live_fb->buf, live_fb->width, live_fb->height,
                              tracked_screws[i].rx, tracked_screws[i].ry,
                              tracked_screws[i].rw, tracked_screws[i].rh,
                              255, 0, 0); // Đỏ cho Screws
            }
        }

        for (int i = 0; i < tracked_nuts_count; i++)
        {
            if (tracked_nuts[i].disappeared <= 2 && tracked_nuts[i].rw > 0 && tracked_nuts[i].rh > 0)
            {
                drawRectangle((uint8_t *)live_fb->buf, live_fb->width, live_fb->height,
                              tracked_nuts[i].rx, tracked_nuts[i].ry,
                              tracked_nuts[i].rw, tracked_nuts[i].rh,
                              0, 255, 0); // Xanh lá cho Nuts
            }
        }

        // 4. Đo FPS thực tế tại cảm biến camera
        frame_counter++;
        uint32_t now_ms = millis();
        if (now_ms - last_fps_time >= 1000)
        {
            g_measured_fps = (frame_counter * 1000.0f) / (now_ms - last_fps_time);
            frame_counter = 0;
            last_fps_time = now_ms;
            Serial.printf("[FPS] %.1f | Screws: %d (cur %d) | Nuts: %d (cur %d)\n",
                          g_measured_fps, total_screw, currentScrew, total_nuts, currentNuts);
        }

        // 5. Chuyển giao dữ liệu sang Core 0 truyền TCP (Non-blocking)
        if (xSemaphoreTake(xFrameMutex, 0) == pdTRUE)
        {
            memcpy(shared_rgb_buf, live_fb->buf, RGB565_BUF_SIZE);
            shared_cs = currentScrew;
            shared_cn = currentNuts;
            shared_ts = total_screw;
            shared_tn = total_nuts;
            shared_epoch = current_epoch;
            shared_fps = g_measured_fps;
            shared_has_new_frame = true;

            xSemaphoreGive(xFrameMutex);
            xSemaphoreGive(xFrameReadySem); // Báo hiệu cho Core 0 truyền đi
        }

        esp_camera_fb_return(live_fb);

        // Nhường quyền CPU cho AI Worker hoàn tất phần việc
        taskYIELD();
    }
}

// -----TASK STREAMING CAO TỐC QUA TCP (Core 0, Priority 2)-------

void tcp_stream_task(void *pvParameters)
{
    WiFiClient client;
    client.setNoDelay(true);
    client.setTimeout(3); // 3 giây socket timeout

    static uint8_t local_rgb[RGB565_BUF_SIZE];
    camera_fb_t temp_fb;
    temp_fb.buf = local_rgb;
    temp_fb.len = RGB565_BUF_SIZE;
    temp_fb.width = FRAME_W;
    temp_fb.height = FRAME_H;
    temp_fb.format = PIXFORMAT_RGB565;

    uint32_t last_reconnect_try = 0;
    uint32_t last_wifi_retry = 0;
    int connect_fail_count = 0;

    while (1)
    {
        // Chờ tín hiệu có frame mới từ Core 1 (timeout 50ms)
        if (xSemaphoreTake(xFrameReadySem, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                uint32_t now = millis();
                if (now - last_wifi_retry >= 5000)
                {
                    last_wifi_retry = now;
                    Serial.println("[WiFi] Mất kết nối, đang thử kết nối lại...");
                    WiFi.reconnect();
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            int cs = 0, cn = 0, ts = 0, tn = 0, ep = 0;
            float fps_val = 0.0f;
            bool got_frame = false;

            if (xSemaphoreTake(xFrameMutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                if (shared_has_new_frame)
                {
                    memcpy(local_rgb, shared_rgb_buf, RGB565_BUF_SIZE);
                    cs = shared_cs;
                    cn = shared_cn;
                    ts = shared_ts;
                    tn = shared_tn;
                    ep = shared_epoch;
                    fps_val = shared_fps;
                    shared_has_new_frame = false;
                    got_frame = true;
                }
                xSemaphoreGive(xFrameMutex);
            }

            if (!got_frame)
                continue;

            // Kết nối TCP socket tới Server nếu chưa kết nối
            if (!client.connected())
            {
                uint32_t now = millis();
                if (now - last_reconnect_try >= 1500)
                {
                    last_reconnect_try = now;
                    client.stop();

                    IPAddress target_ip;
                    if (!target_ip.fromString(server_host)) {
                        target_ip = WiFi.gatewayIP();
                    }

                    if (connect_fail_count >= 5 && WiFi.gatewayIP() != target_ip && WiFi.gatewayIP() != IPAddress(0,0,0,0)) {
                        target_ip = WiFi.gatewayIP();
                    }

                    Serial.printf("[TCP Stream] Đang kết nối Server: %s:%d...\n", target_ip.toString().c_str(), server_stream_port);
                    if (client.connect(target_ip, server_stream_port, 2500))
                    {
                        client.setNoDelay(true);
                        connect_fail_count = 0;
                        Serial.println("[TCP Stream] Kết nối Server THÀNH CÔNG! Bắt đầu truyền hình ảnh...");
                    }
                    else
                    {
                        connect_fail_count++;
                        Serial.printf("[TCP Stream] Kết nối Server thất bại (lần %d). Sẽ thử lại...\n", connect_fail_count);
                    }
                }
                if (!client.connected())
                {
                    continue;
                }
            }

            // Nén ảnh RGB565 sang JPEG chất lượng 45 (~2.5ms, file nhỏ ~2KB)
            uint8_t *jpg_buf = NULL;
            size_t jpg_len = 0;

            if (frame2jpg(&temp_fb, 45, &jpg_buf, &jpg_len) && jpg_buf && jpg_len > 0)
            {
                StreamPacketHeader hdr;
                hdr.magic = 0xAA55AA55;
                hdr.epoch = (uint16_t)ep;
                hdr.cs = (uint16_t)cs;
                hdr.cn = (uint16_t)cn;
                hdr.ts = (uint16_t)ts;
                hdr.tn = (uint16_t)tn;
                hdr.fps = (uint16_t)(fps_val * 10.0f);
                hdr.jpg_len = (uint32_t)jpg_len;

                // Gộp header và JPEG vào một buffer liên tục để truyền trong 1 luồng TCP duy nhất
                // Loại bỏ hoàn toàn hiện tượng Delayed ACK (200ms) trên Windows
                static uint8_t tcp_tx_buf[sizeof(hdr) + 16384];
                size_t total_tx_len = sizeof(hdr) + jpg_len;
                bool ok = true;

                if (total_tx_len <= sizeof(tcp_tx_buf))
                {
                    memcpy(tcp_tx_buf, &hdr, sizeof(hdr));
                    memcpy(tcp_tx_buf + sizeof(hdr), jpg_buf, jpg_len);

                    size_t sent = 0;
                    while (sent < total_tx_len && client.connected())
                    {
                        size_t w = client.write(tcp_tx_buf + sent, total_tx_len - sent);
                        if (w == 0)
                        {
                            vTaskDelay(1);
                        }
                        sent += w;
                    }
                    if (sent != total_tx_len) ok = false;
                }
                else
                {
                    // Frame vượt bộ đệm TCP: chỉ bỏ qua frame này, giữ nguyên
                    // kết nối (không ngắt kết nối chỉ vì một frame quá lớn)
                    Serial.println("[TCP Stream] JPEG lớn hơn bộ đệm gửi, bỏ qua frame này.");
                }

                free(jpg_buf);
                jpg_buf = NULL;

                if (!ok)
                {
                    Serial.println("[TCP Stream] Gửi dữ liệu thất bại, ngắt kết nối để phục hồi.");
                    client.stop();
                }
            }
            else
            {
                if (jpg_buf)
                {
                    free(jpg_buf);
                    jpg_buf = NULL;
                }
            }

            // Đọc lệnh điều khiển từ Server (nếu có yêu cầu Reset)
            while (client.available() >= 4)
            {
                uint32_t cmd = 0;
                client.read((uint8_t *)&cmd, 4);
                if ((cmd >> 16) == 0x55AA)
                {
                    int new_epoch = (int)(cmd & 0xFFFF);
                    // Bỏ qua ACK trùng: Server có thể gửi ACK cho nhiều frame
                    // trước khi ESP32 kịp đọc, nhận trùng sẽ gây reset kép
                    if (new_epoch != current_epoch)
                    {
                        current_epoch = new_epoch;
                        reset_requested = true;
                        Serial.printf("[TCP Stream] Nhận lệnh Reset từ Server, epoch mới: %d\n", current_epoch);
                    }
                }
            }
        }
        else
        {
            taskYIELD();
        }
    }
}

// -----KHỞI TẠO HỆ THỐNG----

bool initCamera()
{
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000; // 20 MHz

    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_96X96;
    config.jpeg_quality = 12;
    config.fb_count = 2; // Double buffering
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_DRAM; // Lưu frame buffer vào SRAM nội 0-wait-state thay vì PSRAM

    esp_err_t err = esp_camera_init(&config);
    return (err == ESP_OK);
}

void connectWiFi()
{
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(200);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false); // Tắt chế độ ngủ WiFi để giảm tối đa độ trễ packet

    Serial.printf("\n[WiFi] Đang kết nối tới mạng '%s'...\n", ssid);
    WiFi.begin(ssid, password);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 40)
    {
        delay(500);
        Serial.print(".");
        timeout++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.printf("\n[WiFi] Kết nối THÀNH CÔNG!\n");
        Serial.printf("[WiFi] IP ESP32: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] Gateway IP: %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("[WiFi] Cường độ tín hiệu RSSI: %d dBm\n\n", WiFi.RSSI());
    }
    else
    {
        Serial.printf("\n[WiFi] Kết nối THẤT BẠI tới '%s' (Mã lỗi: %d)! Sẽ thử lại nền...\n\n", ssid, WiFi.status());
    }
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    initLCD();
    connectWiFi();

    if (!initCamera())
    {
        Serial.println("[Camera] Init Camera FAILED!");
        while (1)
            delay(1000);
    }
    Serial.println("[Camera] Init Camera OK (DRAM double-buffer, 96x96 RGB565)");

    xFrameMutex = xSemaphoreCreateMutex();
    xFrameReadySem = xSemaphoreCreateBinary();

    xAiMutex = xSemaphoreCreateMutex();
    xAiTriggerSem = xSemaphoreCreateBinary();

    // 1. Core 1 - Camera Stream Task: Chạy liên tục ở tốc độ cảm biến tối đa >= 30 FPS (Ưu tiên: 3)
    xTaskCreatePinnedToCore(
        camera_stream_task,
        "Cam_Stream_Task",
        1024 * 6,
        NULL,
        3,
        NULL,
        1);

    // 2. Core 1 - AI Worker Task: Chạy suy luận FOMO tăng tốc phần cứng bất đồng bộ (Ưu tiên: 2)
    xTaskCreatePinnedToCore(
        ai_worker_task,
        "AI_Worker_Task",
        1024 * 16,
        NULL,
        2,
        NULL,
        1);

    // 3. Core 0 - TCP Stream Task: Nén JPEG và truyền mạng socket TCP (Ưu tiên: 2)
    xTaskCreatePinnedToCore(
        tcp_stream_task,
        "TCP_Stream_Task",
        1024 * 8,
        NULL,
        2,
        NULL,
        0);

    // 4. Core 0 - LCD Task: Cập nhật màn hình LCD 1602 định kỳ (Ưu tiên thấp: 1)
    xTaskCreatePinnedToCore(
        lcd_task,
        "LCD_Task",
        2048,
        NULL,
        1,
        NULL,
        0);

    delay(200);
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
