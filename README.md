# Hệ thống Đếm & Phân loại Ốc vít – Tán ốc bằng AI

Đồ án tốt nghiệp: hệ thống thị giác máy tính thời gian thực sử dụng **ESP32-S3 + camera OV2640** chạy mô hình **Edge Impulse FOMO** (Object Detection) để nhận diện và đếm ốc vít (screw) / tán ốc (nut) trực tiếp trên thiết bị (edge inference), đồng thời stream ảnh về **Web Dashboard** qua TCP với tốc độ **≥ 30 FPS**.

## ✨ Tính năng

- 🔴 **Nhận diện real-time ≥ 30 FPS** ngay trên ESP32-S3 (FOMO 96×96, tăng tốc phần cứng ESP-NN)
- 🎯 **Tracker gán ID theo khoảng cách** (dùng bình phương khoảng cách, bỏ `sqrt`) — mỗi vật chỉ được đếm đúng một lần, chống đếm trùng khi vật rung/nhấp nháy
- 📊 **Web Dashboard thời gian thực**: ảnh stream MJPEG, bộ đếm hiện tại/tổng, FPS của camera và của luồng web
- 📜 **Lịch sử đếm thời gian thực**: nút *"Lịch sử đếm"* ghi lại từng sự kiện đếm thêm ốc/tán và mỗi lần reset (kèm mốc thời gian), tự động cập nhật khi mở, hỗ trợ **xuất CSV**
- 🔄 **Reset bộ đếm từ Web** — đồng bộ hai chiều qua cơ chế epoch + ACK, ESP32 tự reset tracker và LCD
- 🖥️ **LCD 1602** hiển thị bộ đếm qua I2C, chạy song song trên core riêng, không ảnh hưởng tốc độ stream

## 🏗️ Kiến trúc

```
┌────────────────────────────────┐        TCP :5001         ┌───────────────────────────┐
│ ESP32-S3 + OV2640              │  header 20B + JPEG q45   │ Python Server (server.py) │
│ • Core 1: Camera 96×96 RGB565  │ ───────────────────────► │ • TCP worker  :5001       │
│ • Core 1: FOMO inference       │                          │ • Flask web   :5000       │
│ • Core 1: Tracker + vẽ box     │ ◄─────────────────────── │ • Lịch sử đếm (in-memory) │
│ • Core 0: Gửi TCP + LCD 1602   │   ACK reset 0x55AA|epoch │ • MJPEG stream + REST     │
└────────────────────────────────┘                          └────────────┬──────────────┘
                                                                         │ HTTP
                                                          Trình duyệt: Dashboard + /status
                                                          + /history + /video_feed
```

Luồng xử lý bất đồng bộ trên ESP32 (các task song song):
1. **Camera task** (Core 1, prio 3): lấy frame liên tục, gửi buffer cho AI khi worker rảnh, vẽ bounding box, đo FPS
2. **AI worker** (Core 1, prio 2): chạy `run_classifier` FOMO, xuất danh sách phát hiện
3. **TCP stream** (Core 0, prio 2): nén JPEG (q45), đóng gói và truyền; nhận lệnh reset từ server
4. **LCD task** (Core 0, prio 1): cập nhật màn hình khi bộ đếm thay đổi

## 📁 Cấu trúc dự án

```
├── platformio.ini          # Cấu hình PlatformIO (ESP32-S3, build flags ESP-NN)
├── post_upload.py          # Script tự khởi động server sau khi nạp firmware
├── run_server.bat          # Chạy server nhanh trên Windows
├── server.py               # Web dashboard + TCP stream server + lịch sử đếm
├── requirements.txt        # Thư viện Python cần thiết
└── src/
    ├── main.cpp            # Firmware ESP32-S3 (camera, AI, tracker, TCP, LCD)
    ├── wifi_config.h       # Cấu hình WiFi của bạn (gitignored — KHÔNG push)
    └── wifi_config.h.example   # File mẫu, sao chép thành wifi_config.h
```

## 🚀 Cài đặt & Chạy

### 1. Firmware (ESP32-S3)

```bash
# Cài PlatformIO extension cho VSCode, mở thư mục dự án, sau đó:
# PlatformIO: Build  →  Upload
```

Cấu hình WiFi — sao chép file mẫu rồi điền thông tin mạng của bạn:

```bash
cp src/wifi_config.h.example src/wifi_config.h
```

Sửa `server_host` trong `src/main.cpp` thành IP của máy chạy server (mặc định `192.168.137.1` — hotspot Windows; khi kết nối thất bại ≥ 5 lần firmware tự thử Gateway IP).

### 2. Server

```bash
pip install -r requirements.txt
python server.py        # hoặc chạy run_server.bat trên Windows
```

Truy cập dashboard tại `http://localhost:5000`. Nếu nạp firmware bằng PlatformIO, server **tự khởi động** sau khi upload xong (xem `post_upload.py`).

### 3. Sử dụng

- Mở dashboard → xem stream + bộ đếm thời gian thực
- Nút **📜 Lịch sử đếm**: bảng sự kiện trực tiếp (mỗi lần +1 ốc/tán, mỗi lần reset), nút **⬇ CSV** tải lịch sử về máy
- Nút **🔄 Reset Bộ Đếm**: về 0 cả trên Web, LCD và bộ tracker của ESP32

## 🔌 Giao thức TCP (cổng 5001)

Header 20 bytes (little-endian, packed) + payload JPEG:

| Offset | Trường | Kiểu | Ý nghĩa |
|-------:|-----------|---------|--------------------------------|
| 0 | magic | uint32 | `0xAA55AA55` |
| 4 | epoch | uint16 | vòng đếm reset (đồng bộ server ↔ ESP32) |
| 6 | cs | uint16 | số Screws hiện tại |
| 8 | cn | uint16 | số Nuts hiện tại |
| 10 | ts | uint16 | tổng Screws đã đếm |
| 12 | tn | uint16 | tổng Nuts đã đếm |
| 14 | fps | uint16 | FPS × 10 (vd: 302 = 30.2 FPS) |
| 16 | jpg_len | uint32 | độ dài payload JPEG |

Server gửi về ESP32 gói ACK 4 bytes `(0x55AA << 16) | epoch` khi cần ESP32 reset bộ đếm (sau khi người dùng bấm Reset trên Web).

## 🛠️ Công nghệ

- **Phần cứng**: ESP32-S3 DevKitC-1 (PSRAM OPI), OV2640, LCD 1602 I2C
- **AI**: [Edge Impulse](https://edgeimpulse.com/) — FOMO Object Detection 96×96, lớp cuối FOMO, 2 nhãn: `screw`, `nuts`
- **Firmware**: Arduino framework trên PlatformIO, FreeRTOS đa nhân, esp32-camera
- **Server**: Python Flask, socket TCP thuần, MJPEG streaming

## 📄 Ghi chú

- Lịch sử đếm lưu trong RAM của server (tối đa 300 sự kiện gần nhất) — khởi động lại server sẽ xóa lịch sử
- Mô hình đã huấn luyện nằm trong `lib/Detect_inferencing/` (thư viện Edge Impulse xuất ra dạng Arduino)
