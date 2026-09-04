#!/usr/bin/env bash
# ============================================================
#  cek_reliability_pi.sh
#  Diagnostik akses Grafana & InfluxDB di Raspberry Pi (TA ELLF).
#  Tujuan: memilah kenapa Grafana tak kebuka dari Windows di sidang
#          -> service mati? bind salah? IP berubah? firewall?
#
#  Cara pakai (di Pi):
#     bash cek_reliability_pi.sh
#  Skrip ini READ-ONLY: hanya MEMERIKSA & memberi saran perintah,
#  tidak mengubah apa pun. Jalankan perintah saran secara manual.
# ============================================================
set -u

GRAFANA_PORT=3000
INFLUX_PORT=8086

echo "============================================================"
echo " Diagnostik Reliability Dashboard TA  -  Raspberry Pi"
echo " $(date)"
echo "============================================================"
echo

# ---------- 1. Status service ----------
for svc in grafana-server influxdb; do
  echo "--- Service: $svc ---"
  if systemctl list-unit-files 2>/dev/null | grep -q "^$svc"; then
    active=$(systemctl is-active   "$svc" 2>/dev/null)
    enabled=$(systemctl is-enabled "$svc" 2>/dev/null)
    echo "  aktif sekarang  : $active"
    echo "  auto-start boot : $enabled"
    [ "$active"  != "active"  ] && echo "  >> BELUM JALAN. Jalankan: sudo systemctl start $svc"
    [ "$enabled" != "enabled" ] && echo "  >> TIDAK auto-start saat boot. Aktifkan: sudo systemctl enable $svc"
    [ "$active" = "active" ] && [ "$enabled" = "enabled" ] && echo "  OK."
  else
    echo "  >> service '$svc' tidak ditemukan. Terinstall? (cek paket)"
  fi
  echo
done

# ---------- 2. Port yang listening + alamat bind ----------
echo "--- Port listening (penting: harus 0.0.0.0 / *, bukan 127.0.0.1) ---"
check_port() {
  local port=$1 name=$2
  local line
  line=$(ss -tlnp 2>/dev/null | grep ":$port ")
  if [ -n "$line" ]; then
    local bind
    bind=$(echo "$line" | awk '{print $4}' | head -1)
    echo "  $name (:$port) LISTEN di $bind"
    case "$bind" in
      127.0.0.1:*|"[::1]:"*)
        echo "    >> MASALAH: $name cuma bind ke localhost -> device lain TIDAK bisa akses."
        echo "       Perbaiki di config: http_addr = (kosong)  utk Grafana, atau bind-address utk Influx." ;;
      *) echo "    OK: bisa diakses dari device lain." ;;
    esac
  else
    echo "  $name (:$port) TIDAK listening (kemungkinan service mati)."
  fi
}
check_port "$GRAFANA_PORT" "Grafana"
check_port "$INFLUX_PORT"  "InfluxDB"
echo

# ---------- 3. Self-test HTTP dari Pi sendiri ----------
echo "--- Self-test lokal (dari Pi) ---"
if command -v curl >/dev/null 2>&1; then
  g=$(curl -s -o /dev/null -w "%{http_code}" --max-time 4 "http://localhost:$GRAFANA_PORT/api/health" 2>/dev/null)
  echo "  Grafana  /api/health -> HTTP ${g:-gagal}   (200 = sehat)"
  i=$(curl -s -o /dev/null -w "%{http_code}" --max-time 4 "http://localhost:$INFLUX_PORT/ping" 2>/dev/null)
  echo "  InfluxDB /ping       -> HTTP ${i:-gagal}   (204 = sehat)"
else
  echo "  curl tidak ada (sudo apt install curl) -> lewati."
fi
echo

# ---------- 4. Identitas jaringan ----------
echo "--- Identitas jaringan Pi ---"
host=$(hostname)
firstip=$(hostname -I 2>/dev/null | awk '{print $1}')
echo "  hostname : $host"
echo "  IP aktif :"
hostname -I 2>/dev/null | tr ' ' '\n' | grep -v '^$' | sed 's/^/      /'
echo
if systemctl is-active avahi-daemon >/dev/null 2>&1; then
  echo "  avahi-daemon AKTIF -> akses via nama: http://$host.local:$GRAFANA_PORT"
  echo "  (lebih tahan banting daripada IP, karena IP bisa berubah tiap konek hotspot)"
else
  echo "  >> avahi-daemon TIDAK aktif. Agar bisa akses via nama .local (anti IP berubah):"
  echo "     sudo apt install -y avahi-daemon && sudo systemctl enable --now avahi-daemon"
fi
echo

# ---------- 5. Firewall ----------
echo "--- Firewall ---"
if command -v ufw >/dev/null 2>&1; then
  st=$(sudo ufw status 2>/dev/null | head -1)
  echo "  ufw: ${st:-tidak diketahui}"
  echo "  (jika 'active', izinkan: sudo ufw allow $GRAFANA_PORT/tcp)"
else
  echo "  ufw tidak terpasang. Default Raspberry Pi OS = tanpa firewall -> kemungkinan bukan ini."
fi
echo

# ---------- Ringkasan ----------
echo "============================================================"
echo " RINGKASAN AKSES DARI WINDOWS"
echo "------------------------------------------------------------"
echo "  URL utama        : http://${firstip:-<IP-PI>}:$GRAFANA_PORT"
echo "  URL anti-berubah : http://$host.local:$GRAFANA_PORT"
echo "  Datasource Influx di Grafana TETAP: http://localhost:$INFLUX_PORT"
echo
echo " Jika dari Pi self-test (#3) SEHAT tapi Windows tetap gagal,"
echo " penyebabnya JARINGAN (IP berubah / AP isolation hotspot),"
echo " BUKAN Grafana. Lihat catatan AP isolation dari Claude."
echo "============================================================"