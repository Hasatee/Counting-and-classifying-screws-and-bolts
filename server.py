# 2. CHƯƠNG TRÌNH SERVER & WEB (TỐI ƯU HÓA CAO TỐC >= 30 FPS)
# Chức năng: Nhận stream TCP trực tiếp từ ESP32-S3 ở cổng 5001 (đạt 30+ FPS),
#            phục vụ Web Dashboard giám sát thời gian thực với độ trễ cực thấp.

from flask import Flask, request, jsonify, render_template_string, Response
import threading
import time
import socket
import struct
import sys
import io
import datetime
from collections import deque

# Cấu hình UTF-8 cho console terminal
if hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

app = Flask(__name__)

latest_frame = None
latest_frame_time = 0
current_data = {"cs": 0, "cn": 0, "ts": 0, "tn": 0, "fps": 0.0}
server_epoch = 0
esp32_connected = False
frame_generation = 0

data_lock = threading.Lock()
frame_cond = threading.Condition(data_lock)

# Lịch sử đếm theo thời gian thực: mỗi lần tổng Screws/Nuts tăng hoặc reset
history_events = deque(maxlen=300)
history_seq = 0
reset_grace_until = 0.0  # sau khi reset từ Web, tạm không ghi sự kiện "về 0" từ ESP32

def add_history(kind, text, ts_total, tn_total):
    """Ghi một sự kiện vào lịch sử đếm.
    CHỈ được gọi khi đang giữ data_lock / frame_cond."""
    global history_seq
    history_seq += 1
    history_events.append({
        "id": history_seq,
        "time": datetime.datetime.now().strftime("%H:%M:%S"),
        "kind": kind,  # "screw" | "nut" | "reset"
        "text": text,
        "ts": ts_total,
        "tn": tn_total,
    })

# Tạo ảnh placeholder mặc định khi camera chưa kết nối
def create_placeholder_jpeg():
    try:
        from PIL import Image, ImageDraw
        img = Image.new('RGB', (320, 320), color=(15, 23, 42))
        d = ImageDraw.Draw(img)
        d.rectangle([(10, 10), (310, 310)], outline=(245, 158, 11), width=3)
        d.text((60, 130), "AI CAMERA STREAM", fill=(245, 158, 11))
        d.text((45, 160), "Dang cho ket noi ESP32...", fill=(148, 163, 184))
        d.text((70, 190), "(TCP Port: 5001)", fill=(100, 116, 139))
        buf = io.BytesIO()
        img.save(buf, format='JPEG', quality=75)
        return buf.getvalue()
    except Exception:
        # Fallback JPEG 1x1 pixel
        return b'\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x01\x00`\x00`\x00\x00\xff\xdb\x00C\x00\x08\x06\x06\x07\x06\x05\x08\x07\x07\x07\t\t\x08\n\x0c\x14\r\x0c\x0b\x0b\x0c\x19\x12\x13\x0f\x14\x1d\x1a\x1f\x1e\x1d\x1a\x1c\x1c $.\' ",#\x1c\x1c(7),01444\x1f\'9=82<.342\xff\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00\xff\xc4\x00\x1f\x00\x00\x01\x05\x01\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\xff\xda\x00\x08\x01\x01\x00\x00?\x00\xbf\x00\xff\xd9'

PLACEHOLDER_FRAME = create_placeholder_jpeg()

# Lấy danh sách IP nội bộ của máy chủ để hiển thị hướng dẫn
def get_local_ips():
    ips = []
    try:
        import psutil
        for iface, addrs in psutil.net_if_addrs().items():
            for addr in addrs:
                if addr.family == socket.AF_INET and not addr.address.startswith("127."):
                    ips.append(f"{addr.address} ({iface})")
    except Exception:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ips.append(s.getsockname()[0])
            s.close()
        except Exception:
            ips.append("127.0.0.1")
    return ips

# ------ GIAO DIỆN WEB DASHBOARD HIỆN ĐẠI -------
INDEX_HTML = """
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>AI Object Counter Dashboard (>= 30 FPS)</title>
    <style>
        * {
            box-sizing: border-box;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background: #f8fafc;
            color: #1e293b;
            text-align: center;
            margin: 0;
            padding: 16px;
        }
        .header {
            margin-bottom: 12px;
        }
        h2 {
            margin: 0 0 6px 0;
            color: #b45309;
            font-size: 24px;
        }
        .conn-bar {
            display: inline-block;
            margin-bottom: 10px;
            font-size: 13px;
            font-weight: 600;
            padding: 4px 14px;
            border-radius: 20px;
            transition: all 0.3s;
        }
        .conn-online {
            background: #dcfce7;
            color: #15803d;
            border: 1px solid #86efac;
        }
        .conn-offline {
            background: #fef3c7;
            color: #b45309;
            border: 1px solid #fcd34d;
        }
        .badge-bar {
            display: flex;
            justify-content: center;
            gap: 12px;
            margin-bottom: 12px;
        }
        .fps-badge {
            display: inline-flex;
            align-items: center;
            gap: 6px;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 13px;
            font-weight: 600;
        }
        .fps-camera {
            background: #dcfce7;
            color: #15803d;
            border: 1px solid #86efac;
        }
        .fps-stream {
            background: #dbeafe;
            color: #1d4ed8;
            border: 1px solid #93c5fd;
        }
        .img-box {
            width: 100%;
            max-width: 420px;
            aspect-ratio: 1 / 1;
            margin: 0 auto 14px auto;
            border: 3px solid #f59e0b;
            border-radius: 14px;
            overflow: hidden;
            background: #0f172a;
            box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.15);
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .img-box img {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: block;
            image-rendering: pixelated;
        }
        .stats-grid {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 10px;
            max-width: 420px;
            margin: 0 auto 12px auto;
        }
        .card {
            background: #ffffff;
            padding: 12px 10px;
            border-radius: 10px;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.05);
            border: 1px solid #e2e8f0;
        }
        .card .val {
            font-size: 28px;
            font-weight: 700;
            color: #d97706;
            line-height: 1.1;
        }
        .card .lbl {
            font-size: 13px;
            color: #64748b;
            margin-top: 4px;
            font-weight: 500;
        }
        .card.highlight .val {
            color: #2563eb;
        }
        button {
            background: #f59e0b;
            color: #fff;
            border: none;
            padding: 12px 28px;
            font-size: 15px;
            font-weight: 600;
            border-radius: 8px;
            cursor: pointer;
            box-shadow: 0 4px 12px rgba(245, 158, 11, 0.3);
            transition: all 0.15s ease-in-out;
        }
        button:hover {
            background: #d97706;
            transform: translateY(-1px);
        }
        button:active {
            transform: scale(0.97);
        }
        .btn-row {
            display: flex;
            gap: 12px;
            justify-content: center;
        }
        .btn-history {
            background: #2563eb;
            box-shadow: 0 4px 12px rgba(37, 99, 235, 0.3);
        }
        .btn-history:hover {
            background: #1d4ed8;
        }
        .modal-overlay {
            display: none;
            position: fixed;
            inset: 0;
            background: rgba(15, 23, 42, 0.55);
            z-index: 50;
            align-items: center;
            justify-content: center;
            padding: 16px;
        }
        .modal {
            background: #ffffff;
            width: 100%;
            max-width: 520px;
            max-height: 80vh;
            border-radius: 14px;
            display: flex;
            flex-direction: column;
            box-shadow: 0 20px 50px rgba(0, 0, 0, 0.3);
            overflow: hidden;
        }
        .modal-head {
            display: flex;
            align-items: center;
            gap: 8px;
            padding: 14px 16px;
            border-bottom: 1px solid #e2e8f0;
        }
        .modal-head h3 {
            margin: 0;
            font-size: 17px;
            color: #1e293b;
            flex: 1;
            text-align: left;
        }
        .modal-body {
            overflow-y: auto;
            padding: 0 16px 16px 16px;
        }
        .hist-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 13px;
        }
        .hist-table th {
            position: sticky;
            top: 0;
            background: #f8fafc;
            color: #64748b;
            text-align: left;
            padding: 8px 6px;
            border-bottom: 1px solid #e2e8f0;
        }
        .hist-table td {
            padding: 7px 6px;
            border-bottom: 1px solid #f1f5f9;
            text-align: left;
            color: #334155;
        }
        .ev-screw { color: #dc2626; font-weight: 600; }
        .ev-nut { color: #16a34a; font-weight: 600; }
        .ev-reset { color: #b45309; font-weight: 600; }
        .btn-mini {
            padding: 7px 14px;
            font-size: 13px;
            border-radius: 8px;
        }
        .btn-ghost {
            background: #f1f5f9;
            color: #334155;
            box-shadow: none;
        }
        .btn-ghost:hover {
            background: #e2e8f0;
            transform: none;
        }
        .hist-empty {
            color: #94a3b8;
            font-size: 13px;
            padding: 18px 0;
            text-align: center;
        }
        .ip-info {
            font-size: 12px;
            color: #94a3b8;
            margin-top: 15px;
        }
    </style>
</head>
<body>
    <div class="header">
        <h2>Hệ Thống Phân Loại & Đếm Đối Tượng AI</h2>
        <div>
            <span id="conn_status" class="conn-bar conn-offline">
                🟡 Đang chờ kết nối từ ESP32-S3 (Cổng 5001)...
            </span>
        </div>
        <div class="badge-bar">
            <span class="fps-badge fps-camera">
                <span>⚡ Camera AI:</span>
                <span id="cam_fps">--</span> FPS
            </span>
            <span class="fps-badge fps-stream">
                <span>📡 Web Stream:</span>
                <span id="web_fps">--</span> FPS
            </span>
        </div>
    </div>

    <div class="img-box">
        <img id="cam" src="/video_feed" alt="Đang chờ luồng camera...">
    </div>

    <div class="stats-grid">
        <div class="card">
            <div class="val" id="cs">0</div>
            <div class="lbl">Screws hiện tại</div>
        </div>
        <div class="card">
            <div class="val" id="cn">0</div>
            <div class="lbl">Nuts hiện tại</div>
        </div>
        <div class="card highlight">
            <div class="val" id="ts">0</div>
            <div class="lbl">Tổng Screws đã đếm</div>
        </div>
        <div class="card highlight">
            <div class="val" id="tn">0</div>
            <div class="lbl">Tổng Nuts đã đếm</div>
        </div>
    </div>

    <div class="btn-row">
        <button class="btn-history" onclick="toggleHistory()">📜 Lịch sử đếm</button>
        <button onclick="handleReset()">🔄 Reset Bộ Đếm</button>
    </div>

    <div class="ip-info">
        Địa chỉ IP máy chủ: {{ server_ips }} | Cổng Stream: 5001
    </div>

    <div id="history_modal" class="modal-overlay" onclick="if (event.target === this) toggleHistory();">
        <div class="modal">
            <div class="modal-head">
                <h3>📜 Lịch sử đếm theo thời gian thực</h3>
                <button class="btn-mini btn-ghost" onclick="exportHistoryCSV()">⬇ CSV</button>
                <button class="btn-mini btn-ghost" onclick="toggleHistory()">✕ Đóng</button>
            </div>
            <div class="modal-body">
                <table class="hist-table">
                    <thead>
                        <tr>
                            <th>Thời gian</th>
                            <th>Sự kiện</th>
                            <th>Tổng Screws</th>
                            <th>Tổng Nuts</th>
                        </tr>
                    </thead>
                    <tbody id="hist_body"></tbody>
                </table>
                <div id="hist_empty" class="hist-empty">Chưa có sự kiện nào. Mỗi lần đếm thêm ốc/tán và mỗi lần reset sẽ được ghi lại tại đây.</div>
            </div>
        </div>
    </div>

    <script>
        let activeEpoch = 0;
        let isResetting = false;

        // Đo FPS hiển thị trên Web trực tiếp từ thẻ img
        let lastFrameTime = performance.now();
        let frameCount = 0;
        const camImg = document.getElementById('cam');

        camImg.onload = () => {
            frameCount++;
            const now = performance.now();
            if (now - lastFrameTime >= 1000) {
                const fps = ((frameCount * 1000) / (now - lastFrameTime)).toFixed(1);
                document.getElementById('web_fps').innerText = fps;
                frameCount = 0;
                lastFrameTime = now;
            }
        };

        // Tự động kết nối lại nếu luồng hình ảnh bị gián đoạn
        camImg.onerror = () => {
            document.getElementById('web_fps').innerText = "--";
            setTimeout(() => {
                camImg.src = '/video_feed?t=' + Date.now();
            }, 1000);
        };

        // Polling trạng thái đếm số với tần số cao (60ms)
        setInterval(() => {
            if (isResetting) return;

            fetch('/status')
                .then(r => r.json())
                .then(d => {
                    // Cập nhật trạng thái kết nối ESP32
                    const connBadge = document.getElementById('conn_status');
                    if (d.connected) {
                        connBadge.className = 'conn-bar conn-online';
                        connBadge.innerText = '🟢 ESP32-S3 Đã kết nối (Truyền cao tốc)';
                    } else {
                        connBadge.className = 'conn-bar conn-offline';
                        connBadge.innerText = '🟡 Đang chờ kết nối từ ESP32-S3 (Cổng 5001)...';
                    }

                    if (d.epoch >= activeEpoch) {
                        activeEpoch = d.epoch;
                        document.getElementById('cs').innerText = d.cs;
                        document.getElementById('cn').innerText = d.cn;
                        document.getElementById('ts').innerText = d.ts;
                        document.getElementById('tn').innerText = d.tn;
                        if (d.fps !== undefined && d.connected) {
                            document.getElementById('cam_fps').innerText = Number(d.fps).toFixed(1);
                        } else if (!d.connected) {
                            document.getElementById('cam_fps').innerText = "--";
                        }
                    }
                })
                .catch(() => {});
        }, 60);

        function handleReset() {
            isResetting = true;

            document.getElementById('cs').innerText = 0;
            document.getElementById('cn').innerText = 0;
            document.getElementById('ts').innerText = 0;
            document.getElementById('tn').innerText = 0;

            fetch('/reset')
                .then(r => r.json())
                .then(data => {
                    activeEpoch = data.epoch;
                    setTimeout(() => {
                        isResetting = false;
                    }, 500);
                })
                .catch(() => {
                    isResetting = false;
                });
        }

        // ----- LỊCH SỬ ĐẾM THEO THỜI GIAN THỰC -----
        let histOpen = false;
        let histLastId = 0;
        let histTimer = null;

        function toggleHistory() {
            histOpen = !histOpen;
            document.getElementById('history_modal').style.display = histOpen ? 'flex' : 'none';
            if (histOpen) {
                document.getElementById('hist_body').innerHTML = '';
                document.getElementById('hist_empty').style.display = 'block';
                histLastId = 0;
                fetchHistory();
                histTimer = setInterval(fetchHistory, 700);
            } else {
                clearInterval(histTimer);
            }
        }

        function fetchHistory() {
            fetch('/history?after=' + histLastId)
                .then(r => r.json())
                .then(d => {
                    for (const ev of d.events) {
                        addHistoryRow(ev);
                        histLastId = Math.max(histLastId, ev.id);
                    }
                })
                .catch(() => {});
        }

        function addHistoryRow(ev) {
            document.getElementById('hist_empty').style.display = 'none';
            const cls = ev.kind === 'screw' ? 'ev-screw' : (ev.kind === 'nut' ? 'ev-nut' : 'ev-reset');
            const tbody = document.getElementById('hist_body');
            const tr = document.createElement('tr');
            tr.innerHTML =
                '<td>' + ev.time + '</td>' +
                '<td class="' + cls + '">' + ev.text + '</td>' +
                '<td>' + ev.ts + '</td>' +
                '<td>' + ev.tn + '</td>';
            tbody.insertBefore(tr, tbody.firstChild);
        }

        function exportHistoryCSV() {
            fetch('/history?after=0')
                .then(r => r.json())
                .then(d => {
                    let csv = '\uFEFFThoi gian,Su kien,Tong Screws,Tong Nuts\n';
                    for (const ev of [...d.events].reverse()) {
                        csv += ev.time + ',' + String(ev.text).replace(/,/g, ';') + ',' + ev.ts + ',' + ev.tn + '\n';
                    }
                    const blob = new Blob([csv], { type: 'text/csv;charset=utf-8;' });
                    const a = document.createElement('a');
                    a.href = URL.createObjectURL(blob);
                    a.download = 'lich_su_dem.csv';
                    a.click();
                    URL.revokeObjectURL(a.href);
                })
                .catch(() => {});
        }
    </script>
</body>
</html>
"""

def recv_exact(sock, n):
    """Đọc chính xác n bytes từ socket TCP (xử lý an toàn phân mảnh mạng)"""
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        except (socket.timeout, TimeoutError):
            return None
        except Exception:
            return None
    return bytes(buf)

def tcp_stream_worker():
    """Luồng nhận dữ liệu TCP cao tốc (Port 5001) từ ESP32-S3"""
    global latest_frame, latest_frame_time, current_data, server_epoch, esp32_connected, frame_generation

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(('0.0.0.0', 5001))
    except Exception as e:
        print(f"[TCP Stream] Error binding to port 5001: {e}")
        return

    server_sock.listen(2)
    print("[TCP Stream] Server listening on port 5001 (High-speed stream >= 30 FPS)...", flush=True)

    # Header C++ StreamPacketHeader (20 bytes):
    # uint32_t magic (0xAA55AA55), uint16_t epoch, cs, cn, ts, tn, fps_x10, uint32_t jpg_len
    HEADER_FMT = "<IHHHHHHI"
    HEADER_SIZE = struct.calcsize(HEADER_FMT) # 20 bytes

    while True:
        conn = None
        try:
            conn, addr = server_sock.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.settimeout(5.0) # Timeout 5 giây chống treo luồng khi mất kết nối đột ngột
            print(f"[TCP Stream] ESP32 connected: {addr}", flush=True)
            esp32_connected = True
            acked_epoch = -1  # epoch đã ACK cho connection này (chống gửi ACK trùng)
            prev_counts = None  # (ts, tn) gần nhất của connection này, để ghi lịch sử

            while True:
                hdr_bytes = recv_exact(conn, HEADER_SIZE)
                if not hdr_bytes:
                    break

                magic, f1, f2, f3, f4, f5, f6, payload_len = struct.unpack(HEADER_FMT, hdr_bytes)
                if magic != 0xAA55AA55:
                    print(f"[TCP Stream] Invalid magic packet: {hex(magic)}, resetting connection.", flush=True)
                    break

                if payload_len > 500000: # Giới hạn an toàn 500KB
                    print(f"[TCP Stream] Abnormal payload size ({payload_len} bytes)", flush=True)
                    break

                payload = recv_exact(conn, payload_len)
                if not payload:
                    break

                # Tự động nhận diện cả 2 định dạng gói tin:
                # 1) Direct JPEG stream: payload bắt đầu bằng marker JPEG \xFF\xD8
                # 2) Codex stream: payload có 4 bytes epoch prefix phía trước JPEG
                expects_per_frame_ack = False
                if payload.startswith(b"\xff\xd8"):
                    ep = f1
                    cs, cn, ts, tn = f2, f3, f4, f5
                    fps_val = f6 / 10.0
                    jpg_bytes = payload
                elif len(payload) > 4 and payload[4:6] == b"\xff\xd8":
                    ep = struct.unpack_from("<I", payload, 0)[0] & 0xFFFF
                    cs, cn, ts, tn = f3, f4, f5, f6
                    fps_val = 25.0
                    jpg_bytes = payload[4:]
                    expects_per_frame_ack = True
                else:
                    ep = f1
                    cs, cn, ts, tn = f2, f3, f4, f5
                    fps_val = f6 / 10.0
                    jpg_bytes = payload

                with frame_cond:
                    latest_frame = jpg_bytes
                    latest_frame_time = time.time()
                    frame_generation += 1
                    current_data = {
                        "cs": cs,
                        "cn": cn,
                        "ts": ts,
                        "tn": tn,
                        "fps": fps_val,
                        "epoch": server_epoch
                    }

                    # Ghi lịch sử đếm thời gian thực (tổng tăng = đếm thêm,
                    # tổng giảm = ESP32 đã reset / kết nối lại)
                    if prev_counts is None:
                        prev_counts = (ts, tn)
                    else:
                        d_screw = ts - prev_counts[0]
                        d_nut = tn - prev_counts[1]
                        if d_screw > 0:
                            add_history("screw", "+%d Screw" % d_screw, ts, tn)
                        if d_nut > 0:
                            add_history("nut", "+%d Nut" % d_nut, ts, tn)
                        if (d_screw < 0 or d_nut < 0) and time.time() > reset_grace_until:
                            add_history("reset", "Bộ đếm về 0 (ESP32 reset/kết nối lại)", ts, tn)
                        prev_counts = (ts, tn)

                    # Nếu Client chưa cập nhật epoch mới từ Server (cần Reset bộ đếm)
                    # Hoặc client đòi hỏi ACK mỗi frame theo giao thức codex.
                    # Epoch trên giao thức là uint16 nên so sánh theo 16 bit,
                    # tránh lệch vĩnh viễn khi server_epoch vượt 65535.
                    current_epoch16 = server_epoch & 0xFFFF
                    if expects_per_frame_ack:
                        need_ack = True
                    else:
                        need_ack = (ep != current_epoch16 and acked_epoch != current_epoch16)
                    ack_epoch = current_epoch16
                    frame_cond.notify_all()

                # Gửi ACK bên NGOÀI lock: sendall có thể block tới 5s nếu ESP32
                # không đọc dữ liệu, giữ lock lúc đó sẽ đóng băng toàn bộ dashboard
                if need_ack:
                    if not expects_per_frame_ack:
                        acked_epoch = ack_epoch
                    command = (0x55AA << 16) | ack_epoch
                    ack_packet = struct.pack("!I" if expects_per_frame_ack else "<I", command)
                    try:
                        conn.sendall(ack_packet)
                    except Exception:
                        pass

            conn.close()
            print("[TCP Stream] ESP32 disconnected, waiting for reconnection...", flush=True)
            esp32_connected = False
        except socket.timeout:
            print("[TCP Stream] Socket timeout, resetting client connection...", flush=True)
            esp32_connected = False
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
        except Exception as e:
            print(f"[TCP Stream] Connection exception: {e}", flush=True)
            esp32_connected = False
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
            time.sleep(0.1)

def generate_mjpeg_stream():
    """Phát luồng MJPEG mượt mà thời gian thực tới trình duyệt"""
    with frame_cond:
        frame = latest_frame if latest_frame is not None else PLACEHOLDER_FRAME
        last_generation = frame_generation

    # Gửi ngay frame đầu tiên (hoặc placeholder) để mở luồng tức thì
    yield (b'--frame\r\n'
           b'Content-Type: image/jpeg\r\n'
           b'Content-Length: ' + str(len(frame)).encode('ascii') + b'\r\n\r\n' + frame + b'\r\n')

    while True:
        with frame_cond:
            frame_cond.wait_for(lambda: frame_generation != last_generation, timeout=0.04)
            frame = latest_frame
            gen = frame_generation

        if frame is not None and gen != last_generation:
            last_generation = gen
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n'
                   b'Content-Length: ' + str(len(frame)).encode('ascii') + b'\r\n\r\n' + frame + b'\r\n')

@app.route('/')
def index():
    server_ips = ", ".join(get_local_ips())
    return render_template_string(INDEX_HTML, server_ips=server_ips)

@app.route('/upload', methods=['POST'])
@app.route('/', methods=['POST'])
def upload():
    """Hỗ trợ tương thích ngược qua HTTP POST nếu cần"""
    global latest_frame, latest_frame_time, current_data, server_epoch, esp32_connected, frame_generation

    client_epoch = request.args.get('epoch', default=0, type=int)
    cs = request.args.get('cs', default=0, type=int)
    cn = request.args.get('cn', default=0, type=int)
    ts = request.args.get('ts', default=0, type=int)
    tn = request.args.get('tn', default=0, type=int)

    with frame_cond:
        latest_frame = request.data
        latest_frame_time = time.time()
        frame_generation += 1
        esp32_connected = True

        if client_epoch == server_epoch:
            current_data.update({"cs": cs, "cn": cn, "ts": ts, "tn": tn})
            should_reset = False
        else:
            should_reset = True

        resp_epoch = server_epoch
        frame_cond.notify_all()

    return jsonify({"status": "ok", "reset": should_reset, "epoch": resp_epoch}), 200

@app.route('/video_feed')
def video_feed():
    return Response(generate_mjpeg_stream(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/status')
def status():
    with data_lock:
        return jsonify({**current_data, "epoch": server_epoch, "connected": esp32_connected})

@app.route('/health')
def health():
    with data_lock:
        age_ms = None if latest_frame is None else int((time.time() - latest_frame_time) * 1000)
        return jsonify({
            "service": "esp32-screw-counter",
            "protocol": "tcp-stream-v2",
            "tcp_port": 5001,
            "frame_received": latest_frame is not None,
            "frame_age_ms": age_ms,
            "connected": esp32_connected,
            "fps": current_data.get("fps", 0.0),
            "frame_generation": frame_generation
        })

@app.route('/reset')
def reset():
    global server_epoch, current_data, reset_grace_until
    with frame_cond:
        server_epoch += 1
        current_data = {"cs": 0, "cn": 0, "ts": 0, "tn": 0, "fps": current_data.get("fps", 0.0)}
        reset_grace_until = time.time() + 3.0
        add_history("reset", "Reset từ Web Dashboard", 0, 0)
        resp_epoch = server_epoch
        frame_cond.notify_all()
    return jsonify({"status": "ok", "epoch": resp_epoch}), 200

@app.route('/history')
def history():
    """Lịch sử đếm: chỉ trả về các sự kiện mới hơn ?after=<id> để web poll tăng dần"""
    after = request.args.get('after', default=0, type=int)
    with data_lock:
        events = [e for e in history_events if e["id"] > after]
        return jsonify({
            "events": events[-200:],
            "last_id": history_seq,
            "total_screw": current_data.get("ts", 0),
            "total_nut": current_data.get("tn", 0)
        })

if __name__ == '__main__':
    # Khởi chạy luồng TCP Stream chuyên dụng ở chế độ daemon
    tcp_thread = threading.Thread(target=tcp_stream_worker, daemon=True)
    tcp_thread.start()

    # Chạy Web Flask Server trên cổng 5000
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)
