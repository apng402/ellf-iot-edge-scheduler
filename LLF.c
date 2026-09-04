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
// Resolusi 1 detik (skala manufaktur)
#define SCHEDULER_TICK_US 1000000

// [PATCH] Nama algoritma -> tag InfluxDB "algo" (pembeda LLF vs ELLF).
#define ALGO_NAME "LLF"

// --- INFLUXDB CONFIG ---
// [PATCH] Host (IP/hostname PC Windows) dan token TIDAK di-hardcode.
// Keduanya dibaca dari environment variable saat runtime (lihat main()):
//     export INFLUX_HOST="10.195.240.206"   # IP Windows; ganti saat demo, TANPA compile ulang
//     export INFLUX_TOKEN="token_anda"
//     ./llf
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

    long long total_cycles;
    long long missed_deadlines;
    long long context_switches;

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
        curl_easy_perform(curl);
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

// --- SINGLE CORE SCHEDULER (PURE LLF) ---
void* scheduler_routine(void* arg) {
    (void)arg;
    long long start_time_us = get_time_us();
    (void)start_time_us;  // disimpan untuk konsistensi struktur; tidak dipakai di LLF
    long long last_tick_time_us = get_time_us();
    int current_running_task = -1;
    long long current_task_start_time = get_time_us();

    printf("[Job Floor] Scheduler Real-Time Periodik (PURE LLF - 1s Tick) Started...\n");

    while (system_running && batch_running) {
        long long now_us = get_time_us();
        long long dt_us = now_us - last_tick_time_us;
        last_tick_time_us = now_us;

        for (int i = 0; i < num_tasks_loaded; i++) {
            if (tasks[i].is_complete && tasks[i].is_active && now_us >= tasks[i].next_release_time_us) {
                tasks[i].release_time_us = tasks[i].next_release_time_us;
                tasks[i].absolute_deadline_us = tasks[i].release_time_us + tasks[i].deadline_rel_us;
                tasks[i].remaining_exec_us = tasks[i].execution_cost_us;
                tasks[i].next_release_time_us += tasks[i].period_us;
                tasks[i].is_complete = false;
                tasks[i].is_first_run_in_cycle = true;
            }
        }

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
                }

                if (tasks[current_running_task].total_cycles >= tasks[current_running_task].max_loops) {
                    tasks[current_running_task].final_finish_time_us = now_us;
                    tasks[current_running_task].is_active = false;

                    printf("\n 🔔 [NOTIFIKASI] Task %lld telah menyelesaikan %d siklus penuh (SELESAI)!\n",
                           tasks[current_running_task].id, tasks[current_running_task].max_loops);

                    send_notification_to_influx(tasks[current_running_task].id);

                    pthread_mutex_lock(&util_lock);
                    double freed_util = (double)tasks[current_running_task].execution_cost_us / (double)tasks[current_running_task].period_us;
                    current_total_utilization -= freed_util;
                    if (current_total_utilization < 0.0) current_total_utilization = 0.0;
                    pthread_mutex_unlock(&util_lock);
                }

                tasks[current_running_task].is_complete = true;
                tasks[current_running_task].allowed_to_run = false;

                if (!tasks[current_running_task].is_active) {
                    pthread_cond_signal(&tasks[current_running_task].cond);
                }

                current_running_task = -1;
                current_task_start_time = now_us;
            }
        }

        for(int i=0; i<num_tasks_loaded; i++) {
            if(!tasks[i].is_complete && tasks[i].is_active) {
                if (now_us < tasks[i].release_time_us) { tasks[i].current_laxity_us = LLONG_MAX; }
                else { tasks[i].current_laxity_us = (tasks[i].absolute_deadline_us - now_us) - tasks[i].remaining_exec_us; }
            }
        }

        int candidate = -1;
        long long min_lax = LLONG_MAX; long long min_deadline = LLONG_MAX;

        for (int i = 0; i < num_tasks_loaded; i++) {
            if (!tasks[i].is_complete && tasks[i].is_active && now_us >= tasks[i].release_time_us) {
                long long lax = tasks[i].current_laxity_us; long long dl = tasks[i].absolute_deadline_us;
                if (lax < min_lax) { min_lax = lax; min_deadline = dl; candidate = i; }
                else if (lax == min_lax) { if (dl < min_deadline) { min_lax = lax; min_deadline = dl; candidate = i; } }
            }
        }

        int selected = candidate;

        if (selected != -1) {
            if (selected != current_running_task) {
                if (current_running_task != -1) {
                    log_interval_to_influx(tasks[current_running_task].id, current_task_start_time, now_us);
                    pthread_mutex_lock(&tasks[current_running_task].lock);
                    tasks[current_running_task].allowed_to_run = false;
                    pthread_mutex_unlock(&tasks[current_running_task].lock);
                }

                if (tasks[selected].is_first_run_in_cycle) {
                    tasks[selected].total_wait_time_us += (now_us - tasks[selected].release_time_us);
                    tasks[selected].is_first_run_in_cycle = false;
                }

                tasks[selected].context_switches++;

                pthread_mutex_lock(&tasks[selected].lock);
                tasks[selected].allowed_to_run = true;
                pthread_cond_signal(&tasks[selected].cond);
                pthread_mutex_unlock(&tasks[selected].lock);

                current_running_task = selected;
                current_task_start_time = now_us;
            }
        } else if (current_running_task != -1) {
             log_interval_to_influx(tasks[current_running_task].id, current_task_start_time, now_us);
             pthread_mutex_lock(&tasks[current_running_task].lock);
             tasks[current_running_task].allowed_to_run = false;
             pthread_mutex_unlock(&tasks[current_running_task].lock);
             current_running_task = -1;
             current_task_start_time = now_us;
        }

        // Kalkulasi nanosleep yang aman dan presisi
        struct timespec sleep_ts;
        long long total_ns = SCHEDULER_TICK_US * 1000LL;
        sleep_ts.tv_sec = total_ns / 1000000000LL;
        sleep_ts.tv_nsec = total_ns % 1000000000LL;
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

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

// --- MAIN (RECEIVER & PARSER) ---
int main() {
    curl_global_init(CURL_GLOBAL_ALL);

    // Ctrl+C: sigaction TANPA SA_RESTART agar fgets() langsung keluar (EINTR).
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
    printf(" Edge Server PURE LLF Batch Production (1s Tick) Aktif.\n");
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
                        printf("\n ⚠️ [WARNING] Overload Terdeteksi! Kapasitas: %.1f%%\n", current_total_utilization * 100.0);
                    } else {
                        printf("\n ✅ [KAPASITAS AMAN] Total Beban Naik Mjd: %.1f%%\n", current_total_utilization * 100.0);
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

                    t->total_wait_time_us = 0;
                    t->total_tardiness_us = 0;
                    t->first_release_time_us = now_us;
                    t->final_finish_time_us = 0;
                    t->is_first_run_in_cycle = true;

                    pthread_mutex_init(&t->lock, NULL);
                    pthread_cond_init(&t->cond, NULL);
                    pthread_create(&t->thread, NULL, worker_routine, t);
                    num_tasks_loaded++;

                    printf("    -> Task %lld Masuk (C: %llds, P: %llds, Target: %d Siklus)\n", assigned_id, f_cost, f_period, target_loops);
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
        for (int i = 0; i < num_tasks_loaded; i++) {
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

            send_final_report_to_influx(t->id, miss_rate, avg_wait, cpu_util, preemptions, avg_tardy, throughput);

            printf(" 📊 [PURE LLF SENT] Batch %d Task %lld | Miss: %5.1f%% | Preempt: %4.1f | Tardy: %6.1fms\n",
                   current_batch, t->id, miss_rate, preemptions, avg_tardy);
        }

        // ---- bersihkan objek sinkronisasi per-task ----
        for (int i = 0; i < num_tasks_loaded; i++) {
            pthread_mutex_destroy(&tasks[i].lock);
            pthread_cond_destroy(&tasks[i].cond);
        }

        current_batch++;
    }

    if (scanner_stream) fclose(scanner_stream);
    printf("\n ===================================================\n");
    printf(" SISTEM OFFLINE. Program dimatikan.\n");

    curl_global_cleanup();
    return 0;
}
