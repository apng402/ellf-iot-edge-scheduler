#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>

// --- CONFIGURATION ---
// Tick = 1 detik (skala manufaktur)
#define SCHEDULER_TICK_US 1000000

// [PATCH] Nama algoritma -> dipakai sebagai tag InfluxDB "algo" untuk
// membedakan data ELLF vs LLF di Grafana.
#define ALGO_NAME "ELLF"

// Tie window: dua task dianggap "tied" jika |slack_i - slack_j| <= window.
// Paper Hildebrandt menggunakan tick diskret sehingga tie selalu eksak.
// Pada implementasi software real-time, jitter sub-detik membuat slack
// jarang sama persis meski semantiknya tied. Window 1 tick = 1 detik
// menjaga semantik paper tanpa mengubah algoritma.
#define LAXITY_TIE_WINDOW_US 1000000LL

// --- INFLUXDB CONFIG ---
// [PATCH] Host (IP/hostname PC Windows) dan token TIDAK di-hardcode.
// Keduanya dibaca dari environment variable saat runtime (lihat main()):
//     export INFLUX_HOST="10.195.240.206"   # IP Windows; ganti saat demo, TANPA compile ulang
//     export INFLUX_TOKEN="token_anda"
//     ./ellf
// org & bucket jarang berubah, jadi tetap sebagai konstanta.
#define INFLUX_ORG    "polman"
#define INFLUX_BUCKET "TA2026"

char influx_auth_header[256];
char influx_url[512];   // URL lengkap, dibangun dari INFLUX_HOST saat runtime

// --- GLOBAL VARIABLES & SYSTEM STATUS ---
double current_total_utilization = 0.0;
pthread_mutex_t util_lock = PTHREAD_MUTEX_INITIALIZER;

// --- TASK STRUCTURE ---
typedef struct {
    long long id;
    long long execution_cost_us;
    long long deadline_rel_us;
    long long period_us;
    long long release_time_us;
    long long absolute_deadline_us;
    long long next_release_time_us;
    long long remaining_exec_us;
    long long current_laxity_us;

    int max_loops;
    bool is_active;
    bool is_complete;
    bool allowed_to_run;

    // [ELLF] Status exclusion shield (sesuai paper Hildebrandt et al.)
    // Sekali task ter-exclude, tetap ter-exclude sampai:
    //   (a) running task yang menyebabkan exclusion SELESAI siklusnya, ATAU
    //   (b) running task tersebut TER-PREEMPT oleh task lain.
    bool is_excluded;

    // SENSOR PENGHITUNG PERFORMA DASAR
    long long total_cycles;
    long long missed_deadlines;
    long long context_switches;

    // SENSOR ANALITIK METRIK FINAL
    long long total_wait_time_us;
    long long total_tardiness_us;
    long long first_release_time_us;
    long long final_finish_time_us;
    bool is_first_run_in_cycle;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} Task;

Task tasks[100];
int num_tasks_loaded = 0;
volatile sig_atomic_t system_running = 1;  // 0 = MATIKAN TOTAL program (Ctrl+C)
volatile sig_atomic_t batch_running  = 1;  // 0 = akhiri batch ini (scan 0 0 0)
int current_batch = 1;                      // nomor batch -> tag InfluxDB "batch"

// --- PENANGKAP CTRL+C (matikan TOTAL program) ---
// Hanya set flag (async-signal-safe). Pesan dicetak di main, bukan di sini.
void handle_sigint(int sig) {
    (void)sig;
    system_running = 0;
    batch_running  = 0;
}

// --- TIME FUNCTIONS ---
long long get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000000LL) + (ts.tv_nsec / 1000LL);
}

void burn_cpu_us(int us) {
    long long end = get_time_us() + us;
    while (get_time_us() < end) __asm__("nop");
}

// --- WORKER THREAD ---
void* worker_routine(void* arg) {
    Task* self = (Task*)arg;
    pthread_mutex_lock(&self->lock);
    while (system_running && batch_running && self->is_active) {
        while (!self->allowed_to_run && system_running && batch_running && self->is_active) {
            pthread_cond_wait(&self->cond, &self->lock);
        }
        if (!system_running || !batch_running || !self->is_active) break;
        pthread_mutex_unlock(&self->lock);
        burn_cpu_us(500);
        pthread_mutex_lock(&self->lock);
    }
    pthread_mutex_unlock(&self->lock);
    return NULL;
}

// --- LOGGING HARIAN (GANTT CHART) ---
void log_interval_to_influx(long long task_id, long long start_mono, long long end_mono) {
    if ((end_mono - start_mono) < 1000) return;
    CURL *curl = curl_easy_init();
    if(curl) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        long long now_unix_ms = (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
        long long duration_ms = (end_mono - start_mono) / 1000LL;
        long long start_unix_ms = now_unix_ms - duration_ms;
        char data[512];
        // [PATCH] tambah tag algo
        snprintf(data, sizeof(data),
                 "scheduler_events,core=Lathe_1,algo=" ALGO_NAME ",batch=%d,task_id=Task_%lld startTime=%lld,endTime=%lld",
                 current_batch, task_id, start_unix_ms, now_unix_ms);
        struct curl_slist *headers = NULL; headers = curl_slist_append(headers, influx_auth_header);
        curl_easy_setopt(curl, CURLOPT_URL, influx_url); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data); curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 50);
        curl_easy_perform(curl); curl_slist_free_all(headers); curl_easy_cleanup(curl);
    }
}

// --- LOGGING FINAL (TABEL REKAPITULASI) ---
void send_final_report_to_influx(long long task_id, float miss_rate, float avg_wait, float cpu_util, float preemptions, float avg_tardy, float throughput) {
    CURL *curl = curl_easy_init();
    if(curl) {
        char data[512];
        // [PATCH] tambah tag algo
        snprintf(data, sizeof(data),
                 "final_performance,core=Lathe_1,algo=" ALGO_NAME ",batch=%d,task_id=Task_%lld miss_rate=%.2f,avg_wait=%.2f,cpu_util=%.2f,preemptions=%.2f,avg_tardy=%.2f,throughput=%.2f",
                 current_batch, task_id, miss_rate, avg_wait, cpu_util, preemptions, avg_tardy, throughput);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, influx_auth_header);
        curl_easy_setopt(curl, CURLOPT_URL, influx_url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000);

        CURLcode res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            printf(" ❌ [ERROR] Gagal mengirim metrik ke InfluxDB: %s\n", curl_easy_strerror(res));
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// --- NOTIFIKASI TASK SELESAI ---
void send_notification_to_influx(long long task_id) {
    CURL *curl = curl_easy_init();
    if(curl) {
        char data[256];
        // [PATCH] tambah tag algo
        snprintf(data, sizeof(data),
                 "task_notifications,core=Lathe_1,algo=" ALGO_NAME ",batch=%d,task_id=Task_%lld status=\"SELESAI\",value=1",
                 current_batch, task_id);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, influx_auth_header);
        curl_easy_setopt(curl, CURLOPT_URL, influx_url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 200);
        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// =============================================================================
// SCHEDULER ROUTINE — ELLF (Hildebrandt, Golatowski, Timmermann, 1999)
// =============================================================================
// Aturan utama dari paper Section 3.2:
//   1. Pilih task dengan slack terkecil. Jika tied, pilih yang deadline-nya
//      paling cepat (earliest deadline among ties).
//   2. Saat task dipilih dari grup tied, SEMUA task lain yang tied dengannya
//      otomatis menjadi "excluded". Excluded task tetap di-update slack-nya
//      (membentuk threshold), tapi tidak boleh dieksekusi.
//   3. Task hanya boleh preempt running task jika:
//        (a) ia TIDAK excluded, DAN
//        (b) slack-nya < slack minimum dari semua excluded task
//          (artinya: ia lebih genting dari threshold yang dibangun excluded).
//   4. Exclusion dilepas HANYA saat running task yang menyebabkan exclusion:
//        (a) selesai siklusnya, ATAU
//        (b) ter-preempt oleh task lain.
// =============================================================================
void* scheduler_routine(void* arg) {
    (void)arg;
    long long start_time_us = get_time_us();
    long long last_tick_time_us = get_time_us();
    int current_running_task = -1;
    long long current_task_start_time = get_time_us();

    printf("[Job Floor] Scheduler ELLF (paper-compliant, 1s tick) Started...\n");

    while (system_running && batch_running) {
        long long now_us = get_time_us();
        long long dt_us = now_us - last_tick_time_us;
        last_tick_time_us = now_us;

        // =====================================================================
        // BAGIAN 1 — PERIODIC REPLENISHMENT
        // =====================================================================
        for (int i = 0; i < num_tasks_loaded; i++) {
            if (tasks[i].is_complete && tasks[i].is_active && now_us >= tasks[i].next_release_time_us) {
                tasks[i].release_time_us = tasks[i].next_release_time_us;
                tasks[i].absolute_deadline_us = tasks[i].release_time_us + tasks[i].deadline_rel_us;
                tasks[i].remaining_exec_us = tasks[i].execution_cost_us;
                tasks[i].next_release_time_us += tasks[i].period_us;
                tasks[i].is_complete = false;
                tasks[i].is_first_run_in_cycle = true;
                // Task baru di-release tidak boleh ter-exclude
                tasks[i].is_excluded = false;
                printf("\n 🔄 [PERIODIC] Task %lld mulai Siklus %lld/%d!\n",
                       tasks[i].id, tasks[i].total_cycles + 1, tasks[i].max_loops);
            }
        }

        // =====================================================================
        // BAGIAN 2 — UPDATE RUNNING TASK PROGRESS & DETEKSI COMPLETION
        // =====================================================================
        if (current_running_task != -1) {
            tasks[current_running_task].remaining_exec_us -= dt_us;

            if (tasks[current_running_task].remaining_exec_us <= 0) {
                tasks[current_running_task].remaining_exec_us = 0;
                tasks[current_running_task].total_cycles++;

                log_interval_to_influx(tasks[current_running_task].id, current_task_start_time, now_us);

                long long tardiness_us = now_us - tasks[current_running_task].absolute_deadline_us;
                if (tardiness_us > 0) {
                    tasks[current_running_task].missed_deadlines++;
                    tasks[current_running_task].total_tardiness_us += tardiness_us;
                    printf(" ⚠️ [ALARM] Task %lld MISSED DEADLINE (Tardy: %.1f ms)!\n",
                           tasks[current_running_task].id, (float)tardiness_us/1000.0);
                }

                if (tasks[current_running_task].total_cycles >= tasks[current_running_task].max_loops) {
                    tasks[current_running_task].final_finish_time_us = now_us;
                    printf(" 🏁 [BATCH SELESAI] Task %lld telah menyelesaikan %d siklus penuh.\n",
                           tasks[current_running_task].id, tasks[current_running_task].max_loops);

                    tasks[current_running_task].is_active = false;

                    pthread_mutex_lock(&util_lock);
                    double freed_util = (double)tasks[current_running_task].execution_cost_us /
                                        (double)tasks[current_running_task].period_us;
                    current_total_utilization -= freed_util;
                    if (current_total_utilization < 0.0) current_total_utilization = 0.0;
                    printf(" 📉 [KAPASITAS KOSONG] Beban dilepaskan. Beban sistem saat ini: %.1f%%\n",
                           current_total_utilization * 100.0);
                    pthread_mutex_unlock(&util_lock);
                } else {
                    printf("   ✓ [Task %lld] Siklus %lld selesai. Menunggu periode berikutnya.\n",
                           tasks[current_running_task].id, tasks[current_running_task].total_cycles);
                }

                tasks[current_running_task].is_complete = true;
                tasks[current_running_task].allowed_to_run = false;

                if (!tasks[current_running_task].is_active) {
                    pthread_cond_signal(&tasks[current_running_task].cond);
                }

                // [ELLF Rule 4a] Running task SELESAI → lepas SEMUA exclusion.
                for (int i = 0; i < num_tasks_loaded; i++) {
                    tasks[i].is_excluded = false;
                }

                current_running_task = -1;
                current_task_start_time = now_us;
            }
        }

        // =====================================================================
        // BAGIAN 3 — UPDATE LAXITY (SLACK) UNTUK SEMUA TASK AKTIF
        // Penting (paper Section 3.2): excluded task TETAP update slack-nya.
        // =====================================================================
        for (int i = 0; i < num_tasks_loaded; i++) {
            if (!tasks[i].is_complete && tasks[i].is_active) {
                if (now_us < tasks[i].release_time_us) {
                    tasks[i].current_laxity_us = LLONG_MAX;
                } else {
                    tasks[i].current_laxity_us =
                        (tasks[i].absolute_deadline_us - now_us) - tasks[i].remaining_exec_us;
                }
            }
        }

        // =====================================================================
        // BAGIAN 4 — ELLF SELECTION
        // =====================================================================

        // (a) Threshold dari excluded tasks
        long long min_slack_excluded = LLONG_MAX;
        for (int i = 0; i < num_tasks_loaded; i++) {
            if (!tasks[i].is_complete && tasks[i].is_active && tasks[i].is_excluded) {
                if (tasks[i].current_laxity_us < min_slack_excluded) {
                    min_slack_excluded = tasks[i].current_laxity_us;
                }
            }
        }

        // (b) Kandidat terbaik dari non-excluded tasks
        long long min_slack_valid = LLONG_MAX;
        long long earliest_dl_valid = LLONG_MAX;
        int best_valid_task = -1;

        for (int i = 0; i < num_tasks_loaded; i++) {
            if (!tasks[i].is_complete && tasks[i].is_active &&
                !tasks[i].is_excluded && now_us >= tasks[i].release_time_us) {

                if (tasks[i].current_laxity_us < min_slack_valid) {
                    min_slack_valid = tasks[i].current_laxity_us;
                    earliest_dl_valid = tasks[i].absolute_deadline_us;
                    best_valid_task = i;
                } else if (llabs(tasks[i].current_laxity_us - min_slack_valid) <= LAXITY_TIE_WINDOW_US) {
                    if (tasks[i].absolute_deadline_us < earliest_dl_valid) {
                        earliest_dl_valid = tasks[i].absolute_deadline_us;
                        best_valid_task = i;
                    }
                }
            }
        }

        // (c) Keputusan scheduling
        int selected = current_running_task;

        if (best_valid_task == -1) {
            // Tidak ada kandidat valid → idle (atau tetap di running task kalau ada)
        } else if (current_running_task == -1) {
            selected = best_valid_task;
        } else if (best_valid_task == current_running_task) {
            selected = current_running_task;
        } else {
            // [ELLF Rule 3] Preempt HANYA jika kandidat lebih genting dari
            // BAIK running task MAUPUN threshold excluded.
            long long running_slack = tasks[current_running_task].current_laxity_us;
            if (min_slack_valid < running_slack && min_slack_valid < min_slack_excluded) {
                selected = best_valid_task;
            } else {
                selected = current_running_task;
            }
        }

        // =====================================================================
        // BAGIAN 5 — PASANG EXCLUSION SHIELD
        // =====================================================================
        if (selected != -1) {
            long long selected_slack = tasks[selected].current_laxity_us;
            for (int i = 0; i < num_tasks_loaded; i++) {
                if (i == selected) continue;
                if (tasks[i].is_complete || !tasks[i].is_active) continue;
                if (now_us < tasks[i].release_time_us) continue;

                if (llabs(tasks[i].current_laxity_us - selected_slack) <= LAXITY_TIE_WINDOW_US
                    && !tasks[i].is_excluded) {
                    tasks[i].is_excluded = true;
                    printf("   🛡️  [SHIELD] Task %lld di-exclude (tied dgn Task %lld, slack=%lld)\n",
                           tasks[i].id, tasks[selected].id, selected_slack);
                }
            }
        }

        // =====================================================================
        // BAGIAN 6 — EKSEKUSI CONTEXT SWITCH (jika selected berubah)
        // =====================================================================
        if (selected != current_running_task) {
            if (current_running_task != -1) {
                log_interval_to_influx(tasks[current_running_task].id,
                                       current_task_start_time, now_us);
                pthread_mutex_lock(&tasks[current_running_task].lock);
                tasks[current_running_task].allowed_to_run = false;
                pthread_mutex_unlock(&tasks[current_running_task].lock);

                // [ELLF Rule 4b] Running task TER-PREEMPT → lepas SEMUA exclusion,
                // lalu pasang ulang untuk task tied dgn selected yang baru.
                if (selected != -1) {
                    for (int i = 0; i < num_tasks_loaded; i++) {
                        tasks[i].is_excluded = false;
                    }
                    long long selected_slack = tasks[selected].current_laxity_us;
                    for (int i = 0; i < num_tasks_loaded; i++) {
                        if (i == selected) continue;
                        if (tasks[i].is_complete || !tasks[i].is_active) continue;
                        if (now_us < tasks[i].release_time_us) continue;
                        if (llabs(tasks[i].current_laxity_us - selected_slack) <= LAXITY_TIE_WINDOW_US) {
                            tasks[i].is_excluded = true;
                        }
                    }
                }
            }

            if (selected != -1) {
                if (tasks[selected].is_first_run_in_cycle) {
                    tasks[selected].total_wait_time_us +=
                        (now_us - tasks[selected].release_time_us);
                    tasks[selected].is_first_run_in_cycle = false;
                }

                printf("[t=%.1fs] Execute Task %lld (slack=%lld us)\n",
                       (double)(now_us - start_time_us)/1000000.0,
                       tasks[selected].id,
                       tasks[selected].current_laxity_us);
                tasks[selected].context_switches++;

                pthread_mutex_lock(&tasks[selected].lock);
                tasks[selected].allowed_to_run = true;
                pthread_cond_signal(&tasks[selected].cond);
                pthread_mutex_unlock(&tasks[selected].lock);

                current_running_task = selected;
                current_task_start_time = now_us;
            } else {
                current_running_task = -1;
                current_task_start_time = now_us;
            }
        }

        // =====================================================================
        // SLEEP SAMPAI TICK BERIKUTNYA
        // =====================================================================
        struct timespec sleep_ts;
        long long total_ns = SCHEDULER_TICK_US * 1000LL;
        sleep_ts.tv_sec = total_ns / 1000000000LL;
        sleep_ts.tv_nsec = total_ns % 1000000000LL;
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

// --- MAIN (RECEIVER & PARSER) ---
// --- HEALTH CHECK: cek koneksi ke InfluxDB saat start (GET /ping) ---
// Return 1 jika InfluxDB merespons (HTTP 204/200), 0 jika gagal/timeout.
int check_influx_connection(const char *host) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    char ping_url[512];
    snprintf(ping_url, sizeof(ping_url), "http://%s:8086/ping", host);
    curl_easy_setopt(curl, CURLOPT_URL, ping_url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);        // cukup header, tanpa body
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000);  // tunggu maksimal 3 detik
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && (http_code == 204 || http_code == 200));
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);

    // Ctrl+C: pakai sigaction TANPA SA_RESTART agar fgets() langsung keluar
    // (EINTR) saat sinyal datang, sehingga program bisa berhenti rapi.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // [PATCH] Baca token dari environment variable, bukan hardcode.
    const char *tok = getenv("INFLUX_TOKEN");
    if (!tok) {
        fprintf(stderr, " ❌ [ERROR] Environment variable INFLUX_TOKEN belum di-set.\n");
        fprintf(stderr, "      Jalankan dulu:  export INFLUX_TOKEN=\"token_anda\"\n");
        return 1;
    }
    snprintf(influx_auth_header, sizeof(influx_auth_header),
             "Authorization: Token %s", tok);

    // [PATCH] Baca host (IP PC Windows) dari environment variable INFLUX_HOST,
    // lalu bangun URL lengkap. IP berubah saat demo? cukup ubah env var, tanpa make ulang.
    const char *host = getenv("INFLUX_HOST");
    if (!host) {
        fprintf(stderr, " ❌ [ERROR] Environment variable INFLUX_HOST belum di-set.\n");
        fprintf(stderr, "      Contoh:  export INFLUX_HOST=\"10.195.240.206\"\n");
        return 1;
    }
    snprintf(influx_url, sizeof(influx_url),
             "http://%s:8086/api/v2/write?org=" INFLUX_ORG "&bucket=" INFLUX_BUCKET "&precision=us",
             host);
    printf(" 🔗 InfluxDB target: %s\n", influx_url);

    // [HEALTH CHECK] Cek koneksi ke InfluxDB sekali saat start.
    if (check_influx_connection(host)) {
        printf(" ✅ [INFLUXDB] Koneksi OK. Siap mengirim data.\n");
    } else {
        printf(" ⚠️  [INFLUXDB] TIDAK terhubung (gagal/timeout).\n");
        printf("      Cek: INFLUX_HOST cocok dgn ipconfig Windows, influxd jalan, hotspot aktif.\n");
        printf("      Program TETAP lanjut — perbaiki koneksi, data akan terkirim saat scan berikutnya.\n");
    }

    // Buka scanner SEKALI (dipakai lintas batch).
    int serial_port = open("/dev/ttyACM0", O_RDONLY | O_NOCTTY);
    if (serial_port < 0) { printf(" [ERROR] Gagal membuka port scanner.\n"); return 1; }
    FILE *scanner_stream = fdopen(serial_port, "r");
    char line[256];

    printf("===================================================\n");
    printf(" Edge Server ELLF Batch Production (1s Tick) Aktif.\n");
    printf(" Format QR Scan : [C] [D] [P]  ATAU  [C] [D] [P] [Loop]\n");
    printf(" Scan 0 0 0     : AKHIRI BATCH (kirim metrik + reset ke Task 1)\n");
    printf(" Ctrl+C         : MATIKAN program sepenuhnya\n");
    printf("===================================================\n");

    // ===================== OUTER LOOP: satu iterasi per BATCH =====================
    while (system_running) {
        // ---- mulai batch baru (fresh) ----
        batch_running = 1;
        num_tasks_loaded = 0;
        pthread_mutex_lock(&util_lock);
        current_total_utilization = 0.0;
        pthread_mutex_unlock(&util_lock);

        pthread_t sched_thread;
        pthread_create(&sched_thread, NULL, scheduler_routine, NULL);

        printf("\n========== BATCH %d AKTIF — silakan scan task (mulai Task 1) ==========\n",
               current_batch);

        // ---- INNER LOOP: baca scanner untuk batch ini ----
        while (system_running && batch_running) {
            if (fgets(line, sizeof(line), scanner_stream) != NULL) {
                long long f_cost = 0, f_deadline = 0, f_period = 0;
                int f_loops = 0;
                int parsed = sscanf(line, "%lld %lld %lld %d", &f_cost, &f_deadline, &f_period, &f_loops);

                if (parsed >= 3) {
                    if (f_cost == 0 && f_deadline == 0 && f_period == 0) {
                        // [RESET] Akhiri batch ini TANPA mematikan program.
                        printf("\n 🔁 [RESET] Barcode 0 0 0 — mengakhiri Batch %d, reset ke Task 1.\n",
                               current_batch);
                        batch_running = 0;
                        break;
                    }

                    int target_loops = (parsed == 4) ? f_loops : 5;

                    pthread_mutex_lock(&util_lock);
                    double new_task_utilization = (double)f_cost / (double)f_period;
                    current_total_utilization += new_task_utilization;

                    if (current_total_utilization > 1.0) {
                        printf("\n ⚠️ [WARNING] Overload Terdeteksi! Kapasitas: %.1f%%\n",
                               current_total_utilization * 100.0);
                        CURL *curl = curl_easy_init();
                        if(curl) {
                            char alert_data[256];
                            snprintf(alert_data, sizeof(alert_data),
                                     "system_warnings,core=Lathe_1,algo=" ALGO_NAME ",batch=%d,type=overload utilization=%.1f",
                                     current_batch, current_total_utilization * 100.0);
                            struct curl_slist *headers = NULL;
                            headers = curl_slist_append(headers, influx_auth_header);
                            curl_easy_setopt(curl, CURLOPT_URL, influx_url);
                            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, alert_data);
                            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 50);
                            curl_easy_perform(curl);
                            curl_slist_free_all(headers);
                            curl_easy_cleanup(curl);
                        }
                    } else {
                        printf("\n ✅ [KAPASITAS AMAN] Total Beban Naik Mjd: %.1f%%\n",
                               current_total_utilization * 100.0);
                    }
                    pthread_mutex_unlock(&util_lock);

                    long long now_us = get_time_us();
                    long long assigned_id = num_tasks_loaded + 1;
                    Task* t = &tasks[num_tasks_loaded];
                    t->id = assigned_id;
                    t->execution_cost_us = f_cost * 1000000LL;
                    t->deadline_rel_us = f_deadline * 1000000LL;
                    t->period_us = f_period * 1000000LL;
                    t->remaining_exec_us = t->execution_cost_us;
                    t->release_time_us = now_us;
                    t->absolute_deadline_us = now_us + t->deadline_rel_us;
                    t->next_release_time_us = now_us + t->period_us;
                    t->max_loops = target_loops;
                    t->is_active = true;
                    t->total_cycles = 0;
                    t->missed_deadlines = 0;
                    t->context_switches = 0;
                    t->is_complete = false;
                    t->allowed_to_run = false;
                    t->is_excluded = false;
                    t->total_wait_time_us = 0;
                    t->total_tardiness_us = 0;
                    t->first_release_time_us = now_us;
                    t->final_finish_time_us = 0;
                    t->is_first_run_in_cycle = true;

                    pthread_mutex_init(&t->lock, NULL);
                    pthread_cond_init(&t->cond, NULL);
                    pthread_create(&t->thread, NULL, worker_routine, t);
                    num_tasks_loaded++;

                    printf("    -> Task %lld Masuk (C: %llds, P: %llds, Target: %d Siklus)\n",
                           assigned_id, f_cost, f_period, target_loops);
                }
            } else {
                // fgets balik NULL.
                if (!system_running) break;   // Ctrl+C -> keluar program

                // Bukan Ctrl+C -> kemungkinan SCANNER TERPUTUS. Sambung ulang.
                // Device selalu kembali sebagai /dev/ttyACM0. Scheduler TIDAK
                // terpengaruh — ia terus memproses task yang sudah masuk & kirim ke InfluxDB.
                printf("\n 🔌 [SCANNER] Terputus / tidak terbaca. Menyambung ulang...\n");
                if (scanner_stream) { fclose(scanner_stream); scanner_stream = NULL; }
                while (system_running && scanner_stream == NULL) {
                    int fd = open("/dev/ttyACM0", O_RDONLY | O_NOCTTY);
                    if (fd >= 0) scanner_stream = fdopen(fd, "r");
                    if (scanner_stream == NULL) usleep(500000);  // 0.5s, lalu coba lagi
                }
                if (scanner_stream) {
                    usleep(200000);  // beri device waktu stabil
                    printf(" ✅ [SCANNER] Tersambung kembali. Silakan ulangi scan terakhir bila perlu.\n");
                }
            }
        }

        if (!system_running) {
            printf("\n\n 🛑 [SISTEM] Ctrl+C terdeteksi — mengirim metrik batch ini lalu mematikan program...\n");
        }

        // ---- AKHIR BATCH: hentikan & join semua thread dengan aman ----
        batch_running = 0;
        for (int i = 0; i < num_tasks_loaded; i++) {  // bangunkan worker yang menunggu
            pthread_mutex_lock(&tasks[i].lock);
            pthread_cond_signal(&tasks[i].cond);
            pthread_mutex_unlock(&tasks[i].lock);
        }
        pthread_join(sched_thread, NULL);
        for (int i = 0; i < num_tasks_loaded; i++) {
            pthread_join(tasks[i].thread, NULL);
        }

        // ---- kirim 6 metrik final untuk batch ini ----
        printf("\n [SISTEM] Mengirim metrik final Batch %d ke InfluxDB...\n", current_batch);
        for(int i = 0; i < num_tasks_loaded; i++) {
            Task *t = &tasks[i];
            long long cycles = t->total_cycles;

            float miss_rate = 0.0, avg_wait = 0.0, cpu_util = 0.0;
            float preemptions = 0.0, avg_tardy = 0.0, throughput = 0.0;

            if (cycles > 0) {
                miss_rate = ((float)t->missed_deadlines / cycles) * 100.0;
                avg_wait = ((float)t->total_wait_time_us / cycles) / 1000.0;
                avg_tardy = ((float)t->total_tardiness_us / cycles) / 1000.0;

                long long extra_cs = t->context_switches - cycles;
                if (extra_cs < 0) extra_cs = 0;
                preemptions = (float)extra_cs / cycles;

                long long elapsed_us = (t->final_finish_time_us > 0) ?
                                       (t->final_finish_time_us - t->first_release_time_us) :
                                       (get_time_us() - t->first_release_time_us);

                long long busy_time_us = cycles * t->execution_cost_us;
                if (elapsed_us > 0) {
                    cpu_util = ((float)busy_time_us / elapsed_us) * 100.0;
                    float elapsed_sec = (float)elapsed_us / 1000000.0;
                    throughput = (float)cycles / elapsed_sec;
                }
            }

            send_final_report_to_influx(t->id, miss_rate, avg_wait, cpu_util,
                                        preemptions, avg_tardy, throughput);

            printf(" ✅ [SENT] Batch %d Task %lld | Miss: %5.1f%% | Wait: %6.1fms | Util: %5.1f%% | Preempt: %4.1f | Tardy: %6.1fms | Thr: %4.1ft/s\n",
                   current_batch, t->id, miss_rate, avg_wait, cpu_util, preemptions, avg_tardy, throughput);
        }

        // ---- bersihkan objek sinkronisasi per-task ----
        for (int i = 0; i < num_tasks_loaded; i++) {
            pthread_mutex_destroy(&tasks[i].lock);
            pthread_cond_destroy(&tasks[i].cond);
        }

        current_batch++;  // batch berikutnya dapat nomor baru
        // Kalau Ctrl+C (system_running=0) -> outer loop berhenti.
        // Kalau reset 0 0 0 -> system_running tetap 1 -> lanjut batch berikutnya.
    }

    if (scanner_stream) fclose(scanner_stream);
    printf("\n ===================================================\n");
    printf(" SISTEM OFFLINE. Program dimatikan.\n");

    curl_global_cleanup();
    return 0;
}
