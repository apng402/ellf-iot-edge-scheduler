#!/usr/bin/env bash
# start_llf.sh
# Isi PERSIS seperti perintah yang kamu ketik di terminal untuk menjalankan LLF.
# Baris terakhir WAJIB pakai 'exec' supaya PID = proses scheduler -> Stop/SIGINT tepat sasaran.

set -a
source /home/pi/apingTA/ELLFCODEFIX/influx_token.env   # muat INFLUX_TOKEN / INFLUX_HOST
set +a

cd /home/pi/apingTA/ELLFCODEFIX

# GANTI './LLF' dengan nama binary LLF-mu yang sebenarnya:
exec ./jalankan.sh llf