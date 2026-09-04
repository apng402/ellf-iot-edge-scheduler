#!/usr/bin/env python3
"""
control_server.py — Backend kontrol Start/Stop scheduler ELLF/LLF untuk dashboard Grafana.

Dipanggil oleh tombol Canvas di Grafana via HTTP POST. TIDAK mengubah kode C sama sekali:
  - "start" menjalankan wrapper shell (start_ellf.sh / start_llf.sh)
  - "stop"  mengirim SIGINT == identik Ctrl+C -> memakai jalur shutdown bersih yang
            SUDAH ADA di kodemu (stop aman, kirim 6 metrik final, dst.)

Endpoint:
  POST /start/<algo>   algo = ELLF | LLF   -> jalankan scheduler
  POST /stop                                -> hentikan (SIGINT)
  GET  /status                              -> {running, algo, pid}
"""
import os
import signal
import subprocess
import threading
from flask import Flask, jsonify
from flask_cors import CORS

# ============ KONFIGURASI (sesuaikan sekali saja) ============
BASE_DIR = "/home/pi/apingTA/ELLFCODEFIX"
WRAPPERS = {
    "ELLF": os.path.join(BASE_DIR, "start_ellf.sh"),
    "LLF":  os.path.join(BASE_DIR, "start_llf.sh"),
}
LOG_FILE = os.path.join(BASE_DIR, "scheduler_run.log")
PORT = 5000
# =============================================================

app = Flask(__name__)
CORS(app)  # izinkan tombol Grafana (origin :3000) memanggil backend ini (:5000)

_lock = threading.Lock()
_proc = None    # subprocess.Popen yang sedang aktif
_algo = None    # "ELLF" / "LLF"


def _is_running():
    return _proc is not None and _proc.poll() is None


@app.post("/start/<algo>")
def start(algo):
    global _proc, _algo
    algo = algo.upper()
    if algo not in WRAPPERS:
        return jsonify(ok=False, error=f"algo tidak dikenal: {algo}"), 400
    with _lock:
        if _is_running():
            return jsonify(ok=False,
                           error=f"scheduler {_algo} masih jalan (pid {_proc.pid}). Stop dulu."), 409
        wrapper = WRAPPERS[algo]
        if not os.path.isfile(wrapper):
            return jsonify(ok=False, error=f"wrapper tidak ditemukan: {wrapper}"), 500
        logf = open(LOG_FILE, "ab", buffering=0)
        # start_new_session=True -> proses punya process-group sendiri,
        # sehingga SIGINT bisa dikirim ke group persis seperti Ctrl+C di terminal.
        _proc = subprocess.Popen(
            ["bash", wrapper],
            cwd=BASE_DIR,
            stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
            start_new_session=True,
        )
        _algo = algo
        return jsonify(ok=True, msg=f"{algo} started", pid=_proc.pid)


@app.post("/stop")
def stop():
    global _proc, _algo
    with _lock:
        if not _is_running():
            _proc, _algo = None, None
            return jsonify(ok=True, msg="tidak ada proses berjalan")
        pid = _proc.pid
        algo = _algo
        try:
            # SIGINT ke seluruh process-group = identik Ctrl+C -> shutdown bersih di kode C
            os.killpg(os.getpgid(pid), signal.SIGINT)
        except ProcessLookupError:
            pass
        try:
            _proc.wait(timeout=8)  # beri waktu kirim 6 metrik final & berhenti rapi
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
        _proc, _algo = None, None
        return jsonify(ok=True, msg=f"{algo} stopped (SIGINT)", pid=pid)


@app.get("/status")
def status():
    running = _is_running()
    return jsonify(
        running=running,
        algo=_algo if running else None,
        pid=_proc.pid if running else None,
    )


if __name__ == "__main__":
    # host 0.0.0.0 supaya bisa diakses dari browser Windows lewat IP Pi
    app.run(host="0.0.0.0", port=PORT)