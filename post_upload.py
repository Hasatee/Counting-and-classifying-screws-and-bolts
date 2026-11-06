import os
import json
import socket
import subprocess
import sys
import time
import webbrowser
from pathlib import Path
from urllib.request import urlopen

Import("env")
if hasattr(env, "IsIntegrationDump") and env.IsIntegrationDump():
    Return()

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
SERVER_FILE = PROJECT_DIR / "server.py"
LOG_FILE = PROJECT_DIR / ".pio" / "server.log"
WEB_URL = "http://127.0.0.1:5000/"
WEB_PORT = 5000
TCP_PORT = 5001
SERVICE_ID = "esp32-screw-counter"
PROTOCOL_IDS = {"le20-dual-epoch-v1", "tcp-stream-v2"}


def port_is_open(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            return True
    except OSError:
        return False


def is_project_server_running():
    try:
        with urlopen(WEB_URL + "health", timeout=0.5) as response:
            status = json.loads(response.read().decode("utf-8"))
        return (status.get("service") == SERVICE_ID and
                status.get("protocol") in PROTOCOL_IDS and
                status.get("tcp_port") == TCP_PORT)
    except Exception:
        return False


def open_dashboard():
    try:
        if not webbrowser.open_new_tab(WEB_URL):
            print("Could not open a browser automatically. Open %s manually." % WEB_URL)
    except Exception as exc:
        print("Could not open browser automatically: %s" % exc)
        print("Open %s manually." % WEB_URL)


def after_upload(source, target, env):
    web_ready = port_is_open(WEB_PORT)
    tcp_ready = port_is_open(TCP_PORT)
    if web_ready and tcp_ready:
        if is_project_server_running():
            print("Screw-counter server is ready; opening %s" % WEB_URL)
            open_dashboard()
        else:
            print("Another or older server is occupying ports 5000/5001; not opening its dashboard.")
            print("Close that server, then upload again or run %s." % (PROJECT_DIR / "run_server.bat"))
        return
    if web_ready or tcp_ready:
        print("Server startup skipped: port 5000 or 5001 is already occupied by a partial/old server.")
        print("Stop the old server, then upload again. Server log: %s" % LOG_FILE)
        return

    LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
    flags = 0
    if os.name == "nt":
        flags = (subprocess.DETACHED_PROCESS |
                 subprocess.CREATE_NEW_PROCESS_GROUP |
                 subprocess.CREATE_NO_WINDOW)
    try:
        python_bin = "D:\\python\\python.exe" if Path("D:/python/python.exe").exists() else sys.executable
        log_file = LOG_FILE.open("ab")
        process = subprocess.Popen(
            [python_bin, "-u", str(SERVER_FILE)],
            cwd=str(PROJECT_DIR),
            stdin=subprocess.DEVNULL,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
    except Exception as exc:
        print("Could not start server after upload: %s" % exc)
        return

    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        if port_is_open(WEB_PORT) and port_is_open(TCP_PORT):
            print("Server ready; opening %s" % WEB_URL)
            open_dashboard()
            return
        if process.poll() is not None:
            print("Server exited with code %s. Check %s" % (process.returncode, LOG_FILE))
            return
        time.sleep(0.5)

    print("Server did not become ready within 20 seconds. Check %s" % LOG_FILE)


env.AddPostAction("upload", after_upload)
