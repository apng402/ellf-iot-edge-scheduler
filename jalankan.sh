#!/usr/bin/env bash
# jalankan.sh — menjalankan scheduler ELLF atau LLF dengan token otomatis.
#
# Pemakaian:
#   ./jalankan.sh ellf
#   ./jalankan.sh llf
#
# Token dibaca dari file lokal "influx_token.env" (lihat di bawah),
# jadi token tidak pernah ditulis di dalam kode .c maupun skrip ini.

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

# --- muat token dari file lokal (JANGAN commit file ini ke Git) ---
if [ -f "$DIR/influx_token.env" ]; then
    # shellcheck disable=SC1090
    source "$DIR/influx_token.env"
fi

if [ -z "$INFLUX_TOKEN" ]; then
    echo " ❌ INFLUX_TOKEN belum di-set."
    echo "    Buat file: $DIR/influx_token.env"
    echo "    Isinya   : export INFLUX_TOKEN=\"token_anda\""
    echo "    Lalu     : chmod 600 $DIR/influx_token.env"
    exit 1
fi

# --- pilih program ---
ALGO="${1:-ellf}"
case "$ALGO" in
    ellf) BIN="$DIR/ellf" ;;
    llf)  BIN="$DIR/llf"  ;;
    *) echo "Pemakaian: $0 [ellf|llf]"; exit 1 ;;
esac

if [ ! -x "$BIN" ]; then
    echo " ❌ Binary '$BIN' belum ada. Jalankan 'make' dulu."
    exit 1
fi

echo " ▶️  Menjalankan $ALGO (Ctrl+C untuk berhenti) ..."
exec "$BIN"
