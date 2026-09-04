# ellf-iot-edge-scheduler
Soft real-time manufacturing scheduler implementing Enhanced Least Laxity First (ELLF) algorithm on a PREEMPT_RT Raspberry Pi edge server with InfluxDB &amp; Grafana telemetry.
# Panduan Instalasi dari Nol — Sistem Penjadwalan ELLF/LLF
Panduan ini mengasumsikan Raspberry Pi 4 dengan Raspberry Pi OS Bookworm yang benar-benar baru, belum ada konfigurasi apa pun.

Urutan pengerjaan penting. Ikuti dari atas ke bawah, jangan lompat, karena tahap belakang bergantung pada tahap sebelumnya.

---

## Yang perlu disiapkan sebelum mulai

Perangkat keras: Raspberry Pi 4, kartu microSD minimal 16 GB, kabel LAN untuk koneksi langsung ke laptop, barcode scanner USB, laptop dengan port Ethernet.

Akses sekali di awal: koneksi internet (WiFi atau kabel) hanya diperlukan saat instalasi paket dan sinkronisasi jam pertama kali. Setelah semua terpasang, sistem bisa berjalan tanpa internet lewat kabel LAN langsung.

File dari proyek: kode `ELLF.c` dan `LLF.c`, skrip `control_server.py`, `start_ellf.sh`, `start_llf.sh`, `control_server.service`, `influx_token.env`.

---

## Tahap 1. Sistem operasi

1. Pasang Raspberry Pi OS Bookworm (64-bit) ke microSD memakai Raspberry Pi Imager.
2. Saat proses flashing, aktifkan opsi kustomisasi: set hostname, aktifkan SSH, buat user `pi`, dan hubungkan ke WiFi rumah untuk tahap instalasi awal.
3. Boot Raspberry Pi, lalu perbarui paket dasar:
   ```
   sudo apt update && sudo apt full-upgrade -y
   ```

---

## Tahap 2. Jaringan statis untuk koneksi langsung ke laptop

Ini fondasi supaya alamat sistem selalu sama tiap dipakai, tidak berubah-ubah seperti saat memakai DHCP atau hotspot.

1. Cek metode jaringan yang dipakai (Bookworm memakai NetworkManager secara default):
   ```
   systemctl is-active NetworkManager
   nmcli con show
   ```
   Catat nama profil koneksi kabel, biasanya `Wired connection 1`.

2. Set IP statis pada Raspberry Pi:
   ```
   sudo nmcli con mod "Wired connection 1" ipv4.method manual ipv4.addresses 192.168.50.1/24
   sudo nmcli con up "Wired connection 1"
   hostname -I
   ```
   Baris terakhir harus menunjukkan `192.168.50.1`.

3. Di laptop Windows, set adapter Ethernet ke IP statis lewat Control Panel (`ncpa.cpl`) atau Settings, dengan IP `192.168.50.2`, subnet mask `255.255.255.0`, gateway dikosongkan.

4. Uji sambungan dari Windows:
   ```
   ping 192.168.50.1
   ```
   Harus menerima balasan (reply), bukan timeout.

Catatan: pakai port Ethernet dan kabel yang sama setiap kali. IP statis menempel ke adapter tertentu, jadi berganti port berarti harus mengulang langkah 3.

---

## Tahap 3. Jam sistem

Raspberry Pi tidak punya baterai RTC, sehingga jam bisa kembali ke tahun 1970 setelah mati listrik tanpa internet. Ini penyebab paling umum data tidak muncul di dashboard, jadi jangan dilewati.

1. Sambungkan Pi ke WiFi berinternet, lalu tunggu sinkronisasi:
   ```
   timedatectl
   ```
   Tunggu sampai `System clock synchronized: yes`.

2. Kunci waktunya agar bertahan tanpa internet:
   ```
   sudo fake-hwclock save
   sudo systemctl enable fake-hwclock
   ```

3. Sebelum tiap sesi pemakaian tanpa internet, verifikasi jam Pi sama dengan jam laptop pemantau. Kalau berbeda, set manual:
   ```
   sudo timedatectl set-ntp false
   sudo timedatectl set-time "YYYY-MM-DD HH:MM:SS"
   sudo fake-hwclock save
   ```

---

## Tahap 4. InfluxDB

1. Instal InfluxDB (ikuti repositori resmi InfluxDB untuk ARM64 sesuai versi yang dipakai proyek).
2. Aktifkan dan jalankan sebagai service:
   ```
   sudo systemctl enable --now influxdb
   ```
3. Buka wizard setup awal lewat `http://192.168.50.1:8086`, buat organisasi bernama `polman` dan bucket bernama `TA2026`.
4. Buat token akses. Load Data → API Tokens → Generate API Token → All Access Token. Salin tokennya sekarang karena hanya ditampilkan sekali.
5. Simpan token ke file environment yang dipakai scheduler:
   ```
   nano influx_token.env
   ```
   Isi dengan:
   ```
   INFLUX_TOKEN=isi_token_di_sini
   INFLUX_HOST=http://localhost:8086
   ```
6. Verifikasi InfluxDB hidup:
   ```
   curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8086/ping
   ```
   Harus keluar `204`.

---

## Tahap 5. Grafana

1. Instal Grafana OSS untuk ARM64 sesuai dokumentasi resmi.
2. Aktifkan sebagai service:
   ```
   sudo systemctl enable --now grafana-server
   ```
3. Edit konfigurasi agar bisa diakses lewat IP statis, bukan hanya localhost:
   ```
   sudo nano /etc/grafana/grafana.ini
   ```
   Ubah tiga bagian ini, pastikan tidak ada tanda `;` di depan baris yang diubah:
   ```
   [server]
   http_addr = 0.0.0.0
   root_url = http://192.168.50.1:3000/

   [security]
   cookie_secure = false

   [auth.anonymous]
   enabled = true
   org_name = Main Org.
   org_role = Viewer
   ```
4. Restart agar konfigurasi termuat:
   ```
   sudo systemctl restart grafana-server
   ```
5. Buka `http://192.168.50.1:3000` dari laptop lewat jendela Incognito supaya bebas dari cookie lama. Buat akun admin sesuai wizard awal.
6. Tambahkan datasource InfluxDB: Connections → Data sources → Add → InfluxDB. Isi URL `http://localhost:8086`, pilih query language Flux, isi organization `polman`, isi token mentah tanpa kata "Token" di depannya, isi default bucket `TA2026`. Klik Save & test, harus muncul pesan sukses.
7. Impor atau buat ulang dashboard beserta panel Gantt dan tabel metrik.

---

## Tahap 6. Kode scheduler (ELLF dan LLF)

1. Salin `ELLF.c`, `LLF.c`, dan file pendukung ke Raspberry Pi, misalnya ke `/home/pi/apingTA/ELLFCODEFIX/`.
2. Kompilasi:
   ```
   cd /home/pi/apingTA/ELLFCODEFIX
   gcc -Wall -Wextra -o ELLF ELLF.c -lpthread -lcurl
   gcc -Wall -Wextra -o LLF LLF.c -lpthread -lcurl
   ```
   Sesuaikan flag linking dengan pustaka yang benar-benar dipakai kode (`grep -n "curl\|pthread" ELLF.c` untuk memastikan).
3. Sambungkan barcode scanner, pastikan terbaca sebagai `/dev/ttyACM0`:
   ```
   ls /dev/ttyACM*
   ```
4. Uji jalan manual dari terminal sebelum melangkah ke backend otomatis:
   ```
   set -a; source influx_token.env; set +a
   ./ELLF
   ```
   Scan satu barcode, lihat program merespons, hentikan dengan Ctrl+C.

---

## Tahap 7. Backend kontrol Start/Stop

1. Buat virtual environment dan instal dependensi:
   ```
   python3 -m venv venv
   venv/bin/pip install flask flask-cors
   ```
2. Salin `control_server.py`, `start_ellf.sh`, `start_llf.sh` ke folder yang sama. Buka kedua skrip start, pastikan baris `exec ./ELLF` dan `exec ./LLF` sesuai nama binary hasil kompilasi. Jadikan executable:
   ```
   chmod +x start_ellf.sh start_llf.sh
   ```
3. Uji manual dulu sebelum dijadikan service:
   ```
   venv/bin/python control_server.py
   ```
   Di terminal lain:
   ```
   curl -X POST http://localhost:5000/start/ELLF
   curl http://localhost:5000/status
   curl -X POST http://localhost:5000/stop
   ```
   Ketiganya harus membalas JSON dengan `"ok": true`.
4. Pasang sebagai service permanen:
   ```
   sudo cp control_server.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now control_server
   ```
5. Verifikasi dari laptop, bukan dari Pi sendiri:
   ```
   curl.exe -X POST http://192.168.50.1:5000/status
   ```

---

## Tahap 8. Tombol kontrol di Grafana

1. Buka dashboard, tambahkan panel baru bertipe Canvas.
2. Aktifkan Inline editing, tambahkan tiga elemen Button dengan label Start ELLF, Start LLF, dan Stop.
3. Untuk tiap tombol, atur pemanggilan API dengan Method POST dan Endpoint mengarah ke `http://192.168.50.1:5000/start/ELLF`, `/start/LLF`, atau `/stop`.
4. Matikan Inline editing, simpan dashboard.
5. Uji klik tiap tombol, pastikan scheduler benar-benar berjalan (cek lewat `curl http://localhost:5000/status` di Pi).

---

## Tahap 9. Verifikasi akhir sebelum dipakai

Checklist singkat, semua harus lolos:

```
for s in influxdb grafana-server control_server NetworkManager; do
  printf "%-16s aktif:%s  boot:%s\n" "$s" "$(systemctl is-active $s)" "$(systemctl is-enabled $s)"
done
```
Keempat baris harus `aktif:active` dan `boot:enabled`.

Lalu lakukan uji reboot penuh: matikan Pi, nyalakan lagi, jangan sentuh apa pun di Pi, langsung buka `http://192.168.50.1:3000` dari laptop. Kalau dashboard muncul dan tombol berfungsi tanpa kamu mengetik satu perintah pun di Pi, instalasi selesai dan siap dipakai.

---

## Ringkasan alamat penting

| Layanan | Alamat |
|---|---|
| Dashboard Grafana | `http://192.168.50.1:3000` |
| Backend kontrol | `http://192.168.50.1:5000` |
| InfluxDB | `http://192.168.50.1:8086` |
| IP Raspberry Pi | `192.168.50.1` |
| IP laptop pemantau | `192.168.50.2` |

## Kesalahan yang paling sering terjadi

Jam Pi tidak sama dengan jam laptop, sehingga Gantt chart tampak kosong padahal data sebenarnya ada, hanya berada di luar rentang waktu yang ditampilkan.

Menekan Start padahal scheduler sebelumnya masih berjalan, yang akan ditolak dengan kode 409. Tekan Stop dahulu.

Grafana diakses lewat browser yang masih menyimpan cookie sesi lama, menimbulkan pesan Unauthorized. Buka lewat jendela Incognito untuk memastikan.

Token InfluxDB ditempel dengan tambahan kata "Token" di depannya pada datasource Grafana. Tempel token mentah saja.
