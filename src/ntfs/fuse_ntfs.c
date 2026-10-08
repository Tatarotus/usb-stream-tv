/* fuse_ntfs.c — FUSE daemon serving an NTFS disk with Live TV and Multi-File VOD.
 *
 * Architecture:
 *   SCSI READ (LBA) -> FUSE disk offset -> virtual_file_lookup(lba):
 *     - METADATA (VBR, MFT, B-tree index, bitmap) -> pread(g_meta_fd)
 *     - LIVE_FIFO (TV AO VIVO.ts)                 -> serve_live_backend() [D13.1 Pacing & Anchor]
 *     - VOD_HTTP_RANGE (Movies, Series, Animes)   -> serve_vod_backend()  [HTTP Range & PROCESSING/READY]
 *
 * Compilation:
 *   Host:  gcc -Wall -Wextra -Wconversion -Wsign-conversion -Werror -O2 -lpthread src/ntfs/fuse_ntfs.c -o fuse_ntfs_host
 *   ARM64: aarch64-linux-gnu-gcc -Wall -Wextra -Wconversion -Wsign-conversion -Werror -O2 -lpthread -static src/ntfs/fuse_ntfs.c -o fuse_ntfs_arm64
 */

#include "fuse_ntfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/fuse.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <ucontext.h>

static inline uint64_t get_le64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t get_le32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

#define FILE_NAME "tv_stream.img"
#define FILE_INO 2
#define MIN_STREAM_START (6ULL * 1024 * 1024)      /* 6 MiB initial buffer (~9-10s, fast open <4s) */
#define LEADBACK (16ULL * 1024 * 1024)             /* 16 MiB leadback (~26s steady cushion) */
#define FLOW_CONTROL_LIMIT (64ULL * 1024 * 1024)    /* 64 MiB flow control limit (~100-120s) */
#define HDRCACHESZ 65536

static uint64_t g_live_starve_near_count = 0;
static uint64_t g_live_fill_null_count = 0;

/* Global filesystem geometry (dynamically updated from template VBR if present) */
static uint64_t g_disk_size = NTFS_DISK_SIZE;
static uint64_t g_total_sectors = NTFS_TOTAL_SECTORS;

/* Multi-File Catalog state */
static struct virtual_file g_files[MAX_VIRTUAL_FILES];
static int g_num_files = 0;
static pthread_rwlock_t g_catalog_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* Live TV Ring Buffer state (D13.1 strictly isolated) */
static uint8_t g_ring[RINGSZ];
static uint64_t g_s_write = 0;
static uint64_t g_base = 0;
static uint64_t g_epoch = 0;
static int g_base_valid = 0;
static int g_have_data = 0;
static int g_test_pattern_mode = 0;
static atomic_bool g_running = true;
static uint64_t g_prev_Fend = (uint64_t)-1;
static uint64_t g_last_file_read_ms = 0;
static uint64_t g_last_tv_stream_pos = 0;
static uint64_t g_anchor_foff = (uint64_t)-1;
static uint64_t g_anchor_stream_pos = 0;
static uint64_t g_anchor_birth_ms = 0;
static uint64_t g_probe_seq_end = (uint64_t)-1;
static int g_probe_consecutive_count = 0;
static uint64_t g_probe_accumulated_bytes = 0;

static inline uint64_t foff_to_stream_pos(uint64_t foff) {
    if (g_anchor_foff == (uint64_t)-1) {
        return g_anchor_stream_pos;
    }
    if (foff >= g_anchor_foff) {
        return g_anchor_stream_pos + (foff - g_anchor_foff);
    }
    uint64_t back = g_anchor_foff - foff;
    if (g_anchor_stream_pos >= back) {
        return g_anchor_stream_pos - back;
    }
    return 0;
}

static uint8_t g_hcache[HDRCACHESZ];
static uint64_t g_hcache_len = 0;

/* Flow control monitoring */
#define FLOW_CONTROL_ARM_BYTES (10ULL * 1024 * 1024)   /* Arm flow control after streaming >= 10 MB */

static volatile uint64_t g_last_video_read_ms = 0;
static volatile uint64_t g_video_bytes_streamed = 0;
static volatile int g_playback_armed = 0;
static uint8_t g_current_volume_serial[8];
static int g_serial_initialized = 0;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static int g_is_favorites_mode = 0;
static int g_active_channel_idx = 0;
static uint64_t g_last_channel_switch_ms = 0;

struct switch_arg {
    char id[32];
};

static void *switch_worker_thread(void *arg) {
    struct switch_arg *sa = (struct switch_arg *)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        const char *vps_ip = getenv("VPS_SWITCH_IP");
        if (!vps_ip) vps_ip = "129.146.5.64";
        const char *vps_pin = getenv("VPS_SWITCH_PIN");
        if (!vps_pin) vps_pin = "1233";
        const char *vps_host = getenv("VPS_SWITCH_HOST");
        if (!vps_host) vps_host = "tv.smre.run.place";

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(80);
        inet_pton(AF_INET, vps_ip, &sin.sin_addr);

        if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
            char req[256];
            int req_len = snprintf(req, sizeof(req),
                "GET /api/switch_channel?id=%s&pin=%s&no_reconnect=1 HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Connection: close\r\n\r\n", sa->id, vps_pin, vps_host);
            if (req_len > 0) {
                ssize_t w = send(s, req, (size_t)req_len, MSG_NOSIGNAL);
                (void)w;
            }
        }
        close(s);
    }
    free(sa);
    return NULL;
}

static void trigger_vps_channel_switch(const char *channel_id) {
    if (!channel_id || !channel_id[0]) return;
    struct switch_arg *sa = malloc(sizeof(struct switch_arg));
    if (!sa) return;
    strncpy(sa->id, channel_id, sizeof(sa->id) - 1);
    sa->id[sizeof(sa->id) - 1] = '\0';
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, switch_worker_thread, sa) != 0) {
        free(sa);
    }
    pthread_attr_destroy(&attr);
}

static int g_meta_fd = -1;
static int g_fuse_fd = -1;

static const uint8_t NULL_PKT[188] = {
    [0] = 0x47, [1] = 0x1F, [2] = 0xFF, [3] = 0x10,
    [4 ... 187] = 0xFF
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t g_tel_start_ms = 0;

static void wait_step(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&g_cv, &g_mu, &ts);
}

static void fill_null(uint8_t *dst, size_t n) {
    size_t off = 0, frag = n % 188;
    if (frag) {
        memcpy(dst, NULL_PKT, frag);
        off = frag;
    }
    for (; off + 188 <= n; off += 188) memcpy(dst + off, NULL_PKT, 188);
}

static inline uint64_t snap188(uint64_t v) { return v - (v % 188ULL); }

static void ring_write(const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t idx = (size_t)((g_s_write + (uint64_t)off) % RINGSZ);
        size_t c = n - off;
        size_t room = (size_t)RINGSZ - idx;
        if (c > room) c = room;
        memcpy(g_ring + idx, p + off, c);
        off += c;
    }
    g_s_write += (uint64_t)n;
    g_have_data = 1;
}

static void ring_copy(uint8_t *dst, uint64_t abs_pos, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t idx = (size_t)((abs_pos + (uint64_t)off) % RINGSZ);
        size_t c = n - off;
        size_t room = (size_t)RINGSZ - idx;
        if (c > room) c = room;
        memcpy(dst + off, g_ring + idx, c);
        off += c;
    }
}

static void hcache_feed(const uint8_t *p, size_t n, uint64_t abs_at) {
    if (abs_at >= HDRCACHESZ) return;
    size_t c = n;
    if (abs_at + (uint64_t)c > HDRCACHESZ) c = (size_t)(HDRCACHESZ - abs_at);
    memcpy(g_hcache + abs_at, p, c);
    if (abs_at + (uint64_t)c > g_hcache_len) g_hcache_len = abs_at + (uint64_t)c;
}

static int ring_has_nal(uint64_t p, uint64_t end, uint8_t target_type) {
    uint64_t q = snap188(p);
    while (q + 188 <= end) {
        uint8_t pkt[188];
        ring_copy(pkt, q, 188);
        q += 188;
        if (pkt[0] != 0x47) continue;
        uint16_t pid = (uint16_t)(((pkt[1] & 0x1F) << 8) | pkt[2]);
        if (pid != 0x100) continue;
        uint8_t af = (uint8_t)((pkt[3] >> 4) & 3);
        size_t off = 4;
        if (af == 2 || af == 0) continue;
        if (af == 3) off += 1 + (size_t)pkt[4];
        if (off >= 188) continue;
        const uint8_t *pay = pkt + off;
        size_t plen = 188 - off;
        for (size_t i = 0; i + 4 <= plen; i++) {
            if (pay[i] == 0 && pay[i+1] == 0 && pay[i+2] == 1) {
                if ((pay[i+3] & 0x1F) == target_type) return 1;
            } else if (i + 5 <= plen && pay[i] == 0 && pay[i+1] == 0 && pay[i+2] == 0 && pay[i+3] == 1) {
                if ((pay[i+4] & 0x1F) == target_type) return 1;
            }
        }
    }
    return 0;
}

static uint64_t find_pat_in_window(uint64_t start_pos, size_t window_sz, uint64_t min_pos) {
    uint64_t p = snap188(start_pos);
    uint64_t end = p + (uint64_t)window_sz;
    if (end > g_s_write) end = g_s_write;
    for (; p + 188 <= end; p += 188) {
        if (p < min_pos) continue;
        uint8_t pkt[188];
        ring_copy(pkt, p, 188);
        if (pkt[0] == 0x47 && (pkt[1] & 0x1F) == 0 && pkt[2] == 0) {
            uint8_t af = (uint8_t)((pkt[3] >> 4) & 3);
            size_t off = 4;
            if (af == 2 || af == 0) continue;
            if (af == 3) off += 1 + (size_t)pkt[4];
            if (off >= 188) continue;
            uint8_t ptr = pkt[off];
            if (off + 1 + (size_t)ptr < 188 && pkt[off + 1 + (size_t)ptr] == 0x00) {
                return p;
            }
        }
    }
    return (uint64_t)-1;
}

static uint64_t find_transport_lock(uint64_t start_pos, size_t window_sz, uint64_t min_pos) {
    uint64_t p = snap188(start_pos);
    uint64_t end = p + (uint64_t)window_sz;
    if (end > g_s_write) end = g_s_write;
    for (; p + 3 * 188 <= end; p += 188) {
        if (p < min_pos) continue;
        uint8_t p0[188], p1[188], p2[188];
        ring_copy(p0, p, 188);
        ring_copy(p1, p + 188, 188);
        ring_copy(p2, p + 376, 188);
        if (p0[0] == 0x47 && p1[0] == 0x47 && p2[0] == 0x47) {
            return p;
        }
    }
    return (uint64_t)-1;
}

static uint64_t find_anchor(uint64_t target, uint64_t min_pos, int prefer_sps, const char **out_type, const char **out_win) {
    uint64_t ring_old = (g_s_write > RINGSZ) ? g_s_write - RINGSZ : 0;

    if (prefer_sps && target >= min_pos) {
        uint64_t window_sz = 2097152;
        uint64_t window_start = (target > window_sz) ? target - window_sz : 0;
        if (window_start < ring_old) window_start = ring_old;
        if (window_start < min_pos) window_start = min_pos;
        uint64_t p = snap188(target);
        while (p >= window_start) {
            uint8_t pkt[188];
            ring_copy(pkt, p, 188);
            if (pkt[0] == 0x47 && (pkt[1] & 0x1F) == 0 && pkt[2] == 0) {
                uint8_t af = (uint8_t)((pkt[3] >> 4) & 3);
                size_t off = 4;
                if (af == 3) off += 1 + (size_t)pkt[4];
                if (off < 188) {
                    uint8_t ptr = pkt[off];
                    if (off + 1 + (size_t)ptr < 188 && pkt[off + 1 + (size_t)ptr] == 0x00) {
                        uint64_t scan_end = p + 262144; // 256KB forward scan for NAL 7 (SPS)
                        if (scan_end > g_s_write) scan_end = g_s_write;
                        if (ring_has_nal(p, scan_end, 7)) {
                            *out_type = "PAT_SPS";
                            *out_win = "2M_REV";
                            return p;
                        }
                    }
                }
            }
            if (p < 188) break;
            p -= 188;
        }
    }

    uint64_t pat = find_pat_in_window(target, 2 * 1024 * 1024, min_pos);
    if (pat != (uint64_t)-1) {
        *out_type = "PAT";
        *out_win = "2M";
        return pat;
    }

    uint64_t lock = find_transport_lock(target, 2 * 1024 * 1024, min_pos);
    if (lock != (uint64_t)-1) {
        *out_type = "TRANSPORT";
        *out_win = "2M_LOCK";
        return lock;
    }

    *out_type = "NONE";
    *out_win = "none";
    return (uint64_t)-1;
}

static int snap_open_base_target(uint64_t target, uint64_t *out_base) {
    uint64_t ring_old = (g_s_write > RINGSZ) ? g_s_write - RINGSZ : 0;
    if (target < ring_old) target = ring_old;

    const char *atype = "NONE";
    const char *awin = "none";
    uint64_t p_anchor = find_anchor(target, 0, 1, &atype, &awin);

    if (p_anchor != (uint64_t)-1) {
        *out_base = p_anchor;
        fprintf(stderr, "[D12_OPEN] anchor=%llu anchor_type=%s anchor_window=%s base=%llu\n",
                (unsigned long long)p_anchor, atype, awin, (unsigned long long)*out_base);
        return 1;
    }

    if (g_s_write > 0) {
        *out_base = snap188(target);
        fprintf(stderr, "[D12_OPEN] fallback snap188 target=%llu base=%llu\n",
                (unsigned long long)target, (unsigned long long)*out_base);
        return 1;
    }

    fprintf(stderr, "[D12_OPEN] target=%llu anchor=NONE recovery=FAIL (no valid TS anchor found)\n",
            (unsigned long long)target);
    return 0;
}

static int __attribute__((unused)) snap_open_base(uint64_t *out_base) {
    uint64_t target = (g_s_write > LEADBACK) ? g_s_write - LEADBACK : 0;
    return snap_open_base_target(target, out_base);
}

static void randomize_volume_serial(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, g_current_volume_serial, 8) != 8) {
            for (int i = 0; i < 8; i++) g_current_volume_serial[i] = (uint8_t)(rand() & 0xFF);
        }
        close(fd);
    } else {
        for (int i = 0; i < 8; i++) g_current_volume_serial[i] = (uint8_t)(rand() & 0xFF);
    }
    g_serial_initialized = 1;

    uint64_t s_val = 0;
    memcpy(&s_val, g_current_volume_serial, 8);
    fprintf(stderr, "[SERIAL] New NTFS Volume Serial Number: 0x%016llx\n", (unsigned long long)s_val);

    if (g_meta_fd >= 0) {
        /* Only patch primary VBR at sector 0. Backup VBR at 128GB is intercepted and
         * dynamically patched in-memory by ntfs_serve_disk() without extending sparse templates. */
        if (pwrite(g_meta_fd, g_current_volume_serial, 8, 0x48) < 0) {
            /* ignored */
        }
    }
}


static inline void reset_probe_state(void) {
    g_probe_seq_end = (uint64_t)-1;
    g_probe_consecutive_count = 0;
    g_probe_accumulated_bytes = 0;
}

static void on_open(void) {
    pthread_mutex_lock(&g_mu);
    if (g_test_pattern_mode) {
        g_base_valid = 1;
        pthread_mutex_unlock(&g_mu);
        return;
    }

    g_anchor_foff = (uint64_t)-1;
    g_anchor_stream_pos = 0;
    g_base_valid = 0;
    g_prev_Fend = (uint64_t)-1;
    g_last_file_read_ms = now_ms();
    g_anchor_birth_ms = now_ms();
    reset_probe_state();
    pthread_mutex_unlock(&g_mu);
}

/* =========================================================================
 *  D13.1 LIVE STREAM BACKEND (Universal Dynamic Floating Anchor & Pacing)
 *  g_mu MUST be held by caller.
 * ========================================================================= */
static uint64_t s_last_pace_ms = 0;

static void serve_live_backend(uint8_t *dst, uint64_t foff, size_t c, uint64_t deadline_ms) {
    /* Isolate ConnectShare tail probe near EOF of 8GB virtual NTFS file */
    if (foff >= NTFS_FILE_SIZE - 10ULL * 1024 * 1024) {
        fill_null(dst, c);
        return;
    }

    if (g_test_pattern_mode) {
        for (size_t i = 0; i < c; i++) {
            dst[i] = (uint8_t)((foff + (uint64_t)i) & 0xFFULL);
        }
        return;
    }

    uint64_t now = now_ms();
    uint64_t ring_old = (g_s_write > RINGSZ) ? g_s_write - RINGSZ : 0;

    /* PROBE ISOLATION:
     * ConnectShare probes distant file offsets (e.g. 256MB, 600MB, 7GB) when reading directory
     * or checking for media container atoms.
     * Check if this read is a continuation of the active sequential playback.
     * Any distant non-sequential read (>= 8 MB) MUST return standard MPEG-TS NULL packets
     * immediately WITHOUT corrupting playback anchor or incrementing epoch! */
    int is_seq_read = (g_prev_Fend != (uint64_t)-1 &&
                       (foff == g_prev_Fend ||
                        (foff > g_prev_Fend && foff - g_prev_Fend <= 262144) ||
                        (foff < g_prev_Fend && g_prev_Fend - foff <= 262144)));

    /* Track consecutive reads at a new seek/jump location */
    if (!is_seq_read) {
        if (g_probe_seq_end != (uint64_t)-1 && foff == g_probe_seq_end) {
            g_probe_consecutive_count++;
            g_probe_accumulated_bytes += c;
            g_probe_seq_end = foff + c;
        } else {
            g_probe_consecutive_count = 1;
            g_probe_accumulated_bytes = c;
            g_probe_seq_end = foff + c;
        }
    } else {
        g_probe_consecutive_count = 0;
        g_probe_accumulated_bytes = 0;
        g_probe_seq_end = (uint64_t)-1;
    }

    /* Opening grace period: In the first 10 seconds of playback or while streaming near start (foff <= 4MB),
     * ConnectShare probes distant offsets (e.g. 800MB) for container atoms / index metadata.
     * Forward jumps to foff >= 8MB during grace period are ALWAYS probes and MUST NOT re-anchor! */
    int is_opening_grace = (g_anchor_birth_ms != 0 && (now - g_anchor_birth_ms < 10000ULL) && g_anchor_foff <= 4ULL * 1024 * 1024);

    uint64_t s_probe = foff_to_stream_pos(foff);
    int is_future_probe = (s_probe >= g_s_write + 2ULL * 1024 * 1024 && !is_seq_read);
    int is_behind_probe = (s_probe < ring_old && !is_seq_read);

    /* Volume check: A real user seek/jump reads multiple full blocks within the active stream window.
     * Probes into the unwritten future (e.g. 128GB file end, 12GB) or behind ring buffer are NEVER user seeks! */
    int is_confirmed_jump = (!is_seq_read &&
                             !is_opening_grace &&
                             !is_future_probe &&
                             !is_behind_probe &&
                             g_probe_consecutive_count >= 4 &&
                             g_probe_accumulated_bytes >= 1ULL * 1024 * 1024);

    if (g_base_valid && foff >= 8ULL * 1024 * 1024 && (is_future_probe || is_behind_probe)) {
        fill_null(dst, c);
        /* DO NOT update g_prev_Fend here! Keep active playback chain intact */
        return;
    }

    int is_reopen = 0;
    if (!g_base_valid || g_anchor_foff == (uint64_t)-1) {
        is_reopen = 1;
    } else if (foff == 0 && (now - g_last_file_read_ms > 1500 || g_prev_Fend > 131072)) {
        /* TV explicitly paused or restarted reading from beginning of file */
        is_reopen = 1;
    } else if (is_seq_read) {
        uint64_t s0 = foff_to_stream_pos(foff);
        if (s0 < ring_old) {
            /* Out of ring buffer bounds during sequential streaming: must re-anchor */
            is_reopen = 1;
        }
    } else if (is_confirmed_jump) {
        /* Confirmed seek / bookmark jump (>= 4 blocks AND >= 1 MB outside grace): re-anchor to live target */
        is_reopen = 1;
        g_probe_consecutive_count = 0;
        g_probe_accumulated_bytes = 0;
        g_probe_seq_end = (uint64_t)-1;
    }

    if (is_reopen) {
        s_last_pace_ms = 0; /* Reset pacing state on reopen / re-anchor (ALTO 3) */
        /* Pre-buffering protection: Ensure feeder has buffered at least MIN_STREAM_START */
        if (g_s_write < MIN_STREAM_START) {
            uint64_t wait_t0 = now_ms();
            uint64_t lim = deadline_ms;
            uint64_t t1 = wait_t0 + 1200; /* Never exceed SCSI timeout window (CRÍTICO 3) */
            if (t1 < lim) lim = t1;
            while (g_s_write < MIN_STREAM_START && now_ms() < lim && g_running) {
                wait_step();
            }
            if (g_s_write < MIN_STREAM_START) {
                fill_null(dst, c);
                return;
            }
        }
        if (g_have_data && g_s_write > 0) {
            uint64_t snapped_abs = 0;
            uint64_t eff_lead = (g_s_write > LEADBACK) ? LEADBACK : (g_s_write > 2ULL * 1024 * 1024 ? g_s_write - 2ULL * 1024 * 1024 : 0);
            uint64_t live_target = (g_s_write > eff_lead) ? g_s_write - eff_lead : 0;
            if (snap_open_base_target(live_target, &snapped_abs)) {
                g_anchor_foff = foff;
                g_anchor_stream_pos = snapped_abs;
                g_base = snapped_abs;
                g_epoch++;
                g_last_tv_stream_pos = snapped_abs;
                g_base_valid = 1;
                g_prev_Fend = (uint64_t)-1;
                g_anchor_birth_ms = now;
                s_last_pace_ms = 0;
                size_t c_h = 65536;
                if (c_h > HDRCACHESZ) c_h = HDRCACHESZ;
                uint64_t reader_p = snapped_abs;
                if (g_s_write > reader_p) {
                    size_t av = (size_t)(g_s_write - reader_p);
                    if (c_h > av) c_h = av;
                    for (size_t i = 0; i < c_h; i++) {
                        g_hcache[i] = g_ring[(size_t)((reader_p + i) % RINGSZ)];
                    }
                    g_hcache_len = c_h;
                }
                uint64_t lead_b = (g_s_write > snapped_abs) ? (g_s_write - snapped_abs) : 0;
                double lead_s = (double)lead_b / 617600.0;
                fprintf(stderr, "[FUSE] Fresh playback anchor at foff=%llu -> stream_pos=%llu! epoch=%llu S_write=%llu (lead_bytes=%llu, lead_sec=%.1fs)\n",
                        (unsigned long long)foff, (unsigned long long)snapped_abs, (unsigned long long)g_epoch, (unsigned long long)g_s_write,
                        (unsigned long long)lead_b, lead_s);
            }
        }
    }
    g_last_file_read_ms = now;

    size_t fo = 0;
    int delivered_real_video = 0;

    while (fo < c) {
        uint64_t F = foff + (uint64_t)fo;
        size_t cc = c - fo;
        ring_old = (g_s_write > RINGSZ) ? g_s_write - RINGSZ : 0;

        if (!g_have_data) {
            while (!g_have_data && now_ms() < deadline_ms && g_running) wait_step();
            if (!g_have_data) { fill_null(dst + fo, cc); fo += cc; continue; }
            continue;
        }

        if (F < g_hcache_len) {
            size_t hc = (size_t)(g_hcache_len - F);
            if (hc > cc) hc = cc;
            memcpy(dst + fo, g_hcache + F, hc);
            delivered_real_video = 1;
            fo += hc;
            continue;
        }

        if (!g_base_valid) {
            fill_null(dst + fo, cc);
            fo += cc;
            continue;
        }

        uint64_t start = foff_to_stream_pos(F);
        if (start < ring_old) {
            /* Ring buffer wrapped around: re-anchor to live_target! */
            uint64_t snapped_abs = 0;
            uint64_t live_target = (g_s_write > LEADBACK) ? g_s_write - LEADBACK : 0;
            if (snap_open_base_target(live_target, &snapped_abs)) {
                g_anchor_foff = F;
                g_anchor_stream_pos = snapped_abs;
                g_epoch++;
                g_hcache_len = 0;
                s_last_pace_ms = 0;
                start = snapped_abs;
                fprintf(stderr, "[FUSE] Re-anchored ring wrap at F=%llu -> stream_pos=%llu (S_write=%llu)\n",
                        (unsigned long long)F, (unsigned long long)snapped_abs, (unsigned long long)g_s_write);
            } else {
                fill_null(dst + fo, cc);
                fo += cc;
                continue;
            }
        }

        if (start + 131072 >= g_s_write) {
            g_live_starve_near_count++;
        }

        /* CLOSED-LOOP ADAPTIVE BUFFER GOVERNOR:
         * If TV is reading sequential blocks and lead margin is getting low (< 12 MB),
         * throttle read throughput based on remaining margin so TV read-ahead never depletes cushion. */
        if (start < g_s_write) {
            uint64_t lead_margin = g_s_write - start;
            if (lead_margin < 12ULL * 1024 * 1024 && g_prev_Fend != (uint64_t)-1 && F >= g_prev_Fend) {
                uint64_t pace_now = now_ms();
                if (s_last_pace_ms > 0 && pace_now >= s_last_pace_ms) {
                    uint64_t elapsed_ms = pace_now - s_last_pace_ms;
                    uint64_t target_rate_kbps;
                    uint64_t max_clamp_ms;
                    if (lead_margin < 4ULL * 1024 * 1024) {
                        target_rate_kbps = 480; /* ~3.8 Mbps: slower than 5.0 Mbps stream, forces margin expansion */
                    } else if (lead_margin < 8ULL * 1024 * 1024) {
                        target_rate_kbps = 600; /* ~4.8 Mbps: matches stream rate */
                    } else {
                        target_rate_kbps = 900; /* ~7.2 Mbps: gentle throttle */
                    }
                    max_clamp_ms = 50; /* Strictly clamped to <= 50ms (AGENTS.md §2.2) */
                    uint64_t target_ms = (uint64_t)cc * 1000ULL / (target_rate_kbps * 1024ULL);
                    if (target_ms > elapsed_ms) {
                        uint64_t diff_ms = target_ms - elapsed_ms;
                        if (diff_ms > max_clamp_ms) diff_ms = max_clamp_ms;
                        uint64_t sleep_us = diff_ms * 1000ULL;
                        uint64_t saved_epoch = g_epoch;
                        pthread_mutex_unlock(&g_mu);
                        usleep((useconds_t)sleep_us);
                        pthread_mutex_lock(&g_mu);
                        if (g_epoch != saved_epoch || !g_base_valid) {
                            continue;
                        }
                    }
                }
                s_last_pace_ms = now_ms();
            }
        }
        if (start >= g_s_write) {
            uint64_t s_frag = foff_to_stream_pos(F);
            if (s_frag >= g_s_write + 2ULL * 1024 * 1024 && !is_seq_read) {
                /* Distant probe into unwritten space: deliver NULLs without moving anchor */
                fill_null(dst + fo, cc);
                fo += cc;
                continue;
            }

            /* Feeder underrun on sequential playback: wait up to deadline_ms for feeder to supply data */
            while (start >= g_s_write && now_ms() < deadline_ms && g_running) {
                wait_step();
            }
            if (start >= g_s_write) {
                /* Feeder starved: do NOT re-anchor backwards!
                 * Deliver standard MPEG-TS NULL packets PACED at real-time rate (~600 KB/s, <= 50ms per clamp)
                 * so the TV maintains steady playback timeline and does not rush ahead into the future */
                g_live_fill_null_count++;
                fill_null(dst + fo, cc);
                uint64_t pace_ms = (uint64_t)cc * 1000ULL / (600ULL * 1024ULL);
                if (pace_ms > 50) pace_ms = 50; /* Strictly clamped to <= 50ms (AGENTS.md §2.2) */
                uint64_t saved_epoch = g_epoch;
                pthread_mutex_unlock(&g_mu);
                usleep((useconds_t)(pace_ms * 1000ULL));
                pthread_mutex_lock(&g_mu);
                if (g_epoch != saved_epoch || !g_base_valid) {
                    fo += cc;
                    continue;
                }
                fo += cc;
                continue;
            }
        }

        size_t avail = (size_t)(g_s_write - start);
        if (avail > cc) avail = cc;
        ring_copy(dst + fo, start, avail);
        delivered_real_video = 1;
        fo += avail;
    }

    if (g_base_valid && delivered_real_video) {
        g_last_tv_stream_pos = foff_to_stream_pos(foff + c);
        g_prev_Fend = foff + c;
        reset_probe_state();
    }
}

/* =========================================================================
 *  VOD DUAL-CACHE & PREFETCH ENGINE (Ported & Adapted from fuse_direct_v2)
 * ========================================================================= */
#define VOD_HEAD_SZ  (8ULL * 1024 * 1024)   /* 8 MB VOD Head Pinning Cache */
#define VOD_MEDIA_SZ (16ULL * 1024 * 1024)  /* 16 MB Dedicated VOD Media Buffer */
#define VOD_CHUNK_SZ (4ULL * 1024 * 1024)   /* 4 MB Chunk Fetch Size */

static uint8_t vod_media_buf1[VOD_MEDIA_SZ];
static uint8_t vod_media_buf2[VOD_MEDIA_SZ];
static uint8_t *vod_media_cache = vod_media_buf1;
static uint8_t *vod_prefetch_buf = vod_media_buf2;

static int vod_media_fi = -1;
static uint64_t vod_media_start = (uint64_t)-1;
static size_t vod_media_len = 0;

static int vod_prefetch_fi = -1;
static uint64_t vod_prefetch_start = (uint64_t)-1;
static size_t vod_prefetch_len = 0;
static int vod_prefetch_ready = 0;        /* 1 if vod_prefetch_buf has valid data */
static int vod_prefetch_in_progress = 0;  /* 1 if background thread is downloading */
static int vod_prefetch_target_fi = -1;
static uint64_t vod_prefetch_target = (uint64_t)-1;

static pthread_t vod_prefetch_tid;
static pthread_mutex_t vod_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vod_prefetch_cv;    /* wakes prefetch worker */
static pthread_cond_t vod_done_cv;        /* wakes thread waiting on prefetch */
static int vod_prefetch_requested = 0;
static volatile int vod_prefetch_abort = 0;
static int vod_prefetch_thread_started = 0;

static int vod_fg_sock = -1;
static char vod_fg_host[64] = {0};
static int vod_fg_port = 0;

static int vod_bg_sock = -1;
static char vod_bg_host[64] = {0};
static int vod_bg_port = 0;
static pthread_mutex_t vod_bg_sock_mu = PTHREAD_MUTEX_INITIALIZER;

static void vod_bg_sock_set(int s, const char *host, int port) {
    pthread_mutex_lock(&vod_bg_sock_mu);
    vod_bg_sock = s;
    if (host) strncpy(vod_bg_host, host, sizeof(vod_bg_host) - 1);
    vod_bg_port = port;
    pthread_mutex_unlock(&vod_bg_sock_mu);
}

static void vod_bg_sock_close(void) {
    pthread_mutex_lock(&vod_bg_sock_mu);
    int s = vod_bg_sock;
    vod_bg_sock = -1;
    vod_bg_host[0] = '\0';
    vod_bg_port = 0;
    pthread_mutex_unlock(&vod_bg_sock_mu);
    if (s >= 0) close(s);
}

static void vod_bg_sock_shutdown(void) {
    pthread_mutex_lock(&vod_bg_sock_mu);
    if (vod_bg_sock >= 0) {
        shutdown(vod_bg_sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&vod_bg_sock_mu);
}

static void vod_close_sock(int *sock_ptr) {
    if (!sock_ptr) return;
    if (sock_ptr == &vod_bg_sock) {
        vod_bg_sock_close();
    } else {
        if (*sock_ptr >= 0) {
            close(*sock_ptr);
            *sock_ptr = -1;
        }
        vod_fg_host[0] = '\0';
        vod_fg_port = 0;
    }
}

static int vod_resolve_host(const char *host, int port, struct sockaddr_in *out_sin) {
    memset(out_sin, 0, sizeof(*out_sin));
    out_sin->sin_family = AF_INET;
    out_sin->sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &out_sin->sin_addr) == 1) {
        return 0;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)res->ai_addr;
        out_sin->sin_addr = sin->sin_addr;
        freeaddrinfo(res);
        return 0;
    }
    if (res) freeaddrinfo(res);
    return -1;
}

static int vod_connect_sock(int *sock_ptr, const char *host, int port) {
    if (!sock_ptr || !host) return -1;
    vod_close_sock(sock_ptr);

    struct sockaddr_in saddr;
    if (vod_resolve_host(host, port, &saddr) < 0) {
        fprintf(stderr, "[VOD] Failed to resolve host %s\n", host);
        return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(s);
        return -1;
    }

    int res = connect(s, (struct sockaddr *)&saddr, sizeof(saddr));
    if (res < 0) {
        if (errno != EINPROGRESS) {
            close(s);
            return -1;
        }

        struct pollfd pfd;
        pfd.fd = s;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 5000);
        if (pr <= 0) {
            close(s);
            return -1;
        }

        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
            close(s);
            return -1;
        }
    }

    if (fcntl(s, F_SETFL, flags) < 0) {
        close(s);
        return -1;
    }

    struct timeval tv = { .tv_sec = 6, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int keepalive = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    if (sock_ptr == &vod_bg_sock) {
        vod_bg_sock_set(s, host, port);
    } else {
        *sock_ptr = s;
        strncpy(vod_fg_host, host, sizeof(vod_fg_host) - 1);
        vod_fg_port = port;
    }
    return 0;
}

static int vod_http_fetch_chunk(int *sock_ptr, const char *host, int port,
                                const char *path, uint64_t file_size,
                                uint8_t *dst, uint64_t start_off, size_t chunk_sz,
                                size_t *out_len, volatile int *abort_flag,
                                uint64_t deadline_ms) {
    if (!sock_ptr || !host || !path || file_size == 0 || start_off >= file_size || chunk_sz == 0) return -1;

    if (start_off + (uint64_t)chunk_sz > file_size) {
        chunk_sz = (size_t)(file_size - start_off);
    }
    uint64_t fetch_end = start_off + (uint64_t)chunk_sz - 1;

    for (int retry = 0; retry < 2; retry++) {
        if (now_ms() >= deadline_ms) {
            vod_close_sock(sock_ptr);
            return -1;
        }

        if (abort_flag && *abort_flag) {
            vod_close_sock(sock_ptr);
            return -1;
        }

        int needs_connect = (*sock_ptr < 0);
        if (!needs_connect) {
            if (sock_ptr == &vod_bg_sock) {
                if (strcmp(vod_bg_host, host) != 0 || vod_bg_port != port) needs_connect = 1;
            } else {
                if (strcmp(vod_fg_host, host) != 0 || vod_fg_port != port) needs_connect = 1;
            }
        }
        if (needs_connect) {
            if (vod_connect_sock(sock_ptr, host, port) < 0) {
                usleep(100000);
                continue;
            }
        }

        char req[512];
        int req_len;
        if (port == 80) {
            req_len = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Range: bytes=%" PRIu64 "-%" PRIu64 "\r\n"
                "Connection: keep-alive\r\n"
                "User-Agent: USBStreamTV-VOD/2.0\r\n\r\n",
                path, host, start_off, fetch_end);
        } else {
            req_len = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Range: bytes=%" PRIu64 "-%" PRIu64 "\r\n"
                "Connection: keep-alive\r\n"
                "User-Agent: USBStreamTV-VOD/2.0\r\n\r\n",
                path, host, port, start_off, fetch_end);
        }

        ssize_t w = send(*sock_ptr, req, (size_t)req_len, MSG_NOSIGNAL);
        if (w != (ssize_t)req_len) {
            vod_close_sock(sock_ptr);
            continue;
        }

        char hbuf[4096];
        size_t hlen = 0;
        char *hdr_end = NULL;

        while (hlen < sizeof(hbuf) - 1) {
            if (now_ms() >= deadline_ms) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            if (abort_flag && *abort_flag) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            ssize_t r = recv(*sock_ptr, hbuf + hlen, sizeof(hbuf) - 1 - hlen, 0);
            if (r <= 0) break;
            size_t old_len = hlen;
            hlen += (size_t)r;
            hbuf[hlen] = '\0';
            size_t scan_start = (old_len >= 3) ? (old_len - 3) : 0;
            hdr_end = strstr(hbuf + scan_start, "\r\n\r\n");
            if (hdr_end) break;
        }

        if (!hdr_end) {
            vod_close_sock(sock_ptr);
            continue;
        }

        char *body_start = hdr_end + 4;
        *hdr_end = '\0';

        int status_code = 0;
        if (sscanf(hbuf, "HTTP/1.%*d %d", &status_code) != 1 || (status_code != 200 && status_code != 206)) {
            fprintf(stderr, "[VOD] Unexpected HTTP status: %d\n", status_code);
            vod_close_sock(sock_ptr);
            continue;
        }

        if (start_off != 0 && status_code != 206) {
            fprintf(stderr, "[VOD] Upstream server ignored Range request for offset %" PRIu64 " (status %d != 206)\n",
                    start_off, status_code);
            vod_close_sock(sock_ptr);
            continue;
        }

        if (status_code == 206) {
            char *cr_str = strcasestr(hbuf, "Content-Range:");
            if (cr_str) {
                uint64_t cr_start = 0;
                if (sscanf(cr_str + 14, " bytes %" PRIu64, &cr_start) == 1 ||
                    sscanf(cr_str + 14, " bytes=%" PRIu64, &cr_start) == 1 ||
                    sscanf(cr_str + 14, " %" PRIu64, &cr_start) == 1) {
                    if (cr_start != start_off) {
                        fprintf(stderr, "[VOD] Mismatched Content-Range start: got %" PRIu64 ", wanted %" PRIu64 "\n",
                                cr_start, start_off);
                        vod_close_sock(sock_ptr);
                        continue;
                    }
                }
            }
        }

        if (strcasestr(hbuf, "Transfer-Encoding: chunked")) {
            fprintf(stderr, "[VOD] Chunked transfer encoding not supported for VOD seek chunk\n");
            vod_close_sock(sock_ptr);
            continue;
        }

        int close_after_read = 0;
        size_t expected_len = chunk_sz;
        char *cl_str = strcasestr(hbuf, "Content-Length:");
        if (cl_str) {
            size_t val = 0;
            if (sscanf(cl_str + 15, "%zu", &val) == 1 && val > 0) {
                if (val > expected_len) {
                    close_after_read = 1;
                } else {
                    expected_len = val;
                }
            }
        } else {
            close_after_read = 1;
        }

        if (status_code == 200) {
            close_after_read = 1;
        }

        size_t leftover_body = hlen - (size_t)(body_start - hbuf);
        if (leftover_body > expected_len) leftover_body = expected_len;
        if (leftover_body > 0) {
            memcpy(dst, body_start, leftover_body);
        }
        size_t total_body = leftover_body;

        while (total_body < expected_len) {
            if (now_ms() >= deadline_ms) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            if (abort_flag && *abort_flag) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            ssize_t rd = recv(*sock_ptr, dst + total_body, expected_len - total_body, 0);
            if (rd <= 0) break;
            total_body += (size_t)rd;
        }

        if (total_body < expected_len) {
            vod_close_sock(sock_ptr);
            if (abort_flag && *abort_flag) return -1;
            continue;
        }

        *out_len = total_body;
        if (close_after_read) {
            vod_close_sock(sock_ptr);
        }
        return 0;
    }

    vod_close_sock(sock_ptr);
    return -1;
}

static int vod_init_head_cache_file(int fi) {
    if (fi < 0 || fi >= g_num_files) return -1;
    struct virtual_file *f = &g_files[fi];
    if (f->head_fetched) return 0;
    if (f->file_size == 0 || f->http_path[0] == '\0') return 0;
    if (now_ms() < f->head_failed_until) return -1;

    size_t max_head = VOD_HEAD_SZ;
    size_t to_fetch = (f->file_size < max_head) ? (size_t)f->file_size : (size_t)max_head;
    if (!f->head_cache) {
        f->head_cache = (uint8_t *)malloc(max_head);
        if (!f->head_cache) {
            fprintf(stderr, "[!] Memory allocation failure for VOD head_cache file %d\n", fi);
            return -1;
        }
    }

    fprintf(stderr, "[*] Preenchendo VOD Head Pinning Cache [file %d: %s]: 0 a %zu (%.2f MB)...\n",
           fi, f->path, to_fetch, (double)to_fetch / (1024.0 * 1024.0));
    size_t fetched = 0;
    volatile int abort_flag = 0;
    int res = vod_http_fetch_chunk(&vod_fg_sock, f->http_host, f->http_port, f->http_path,
                                  f->file_size, f->head_cache, 0, to_fetch, &fetched, &abort_flag, now_ms() + 10000);
    if (res == 0 && fetched > 0) {
        f->head_len = fetched;
        f->head_fetched = 1;
        f->head_failed_until = 0;
        fprintf(stderr, "[✓] VOD Head Pinning Cache pronto [file %d]: %zu bytes em RAM (0 ms latency, 0 rede)\n",
               fi, f->head_len);
        return 0;
    }
    f->head_failed_until = now_ms() + 15000;
    fprintf(stderr, "[!] Falha ao preencher VOD Head Pinning Cache [file %d] (tentara novamente em 15s)\n", fi);
    return -1;
}

static void *vod_prefetch_worker(void *arg) {
    (void)arg;
    while (g_running) {
        uint64_t target = (uint64_t)-1;
        int target_fi = -1;

        pthread_mutex_lock(&vod_mu);
        while (g_running && !vod_prefetch_requested) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&vod_prefetch_cv, &vod_mu, &ts);
        }
        if (!g_running) {
            pthread_mutex_unlock(&vod_mu);
            break;
        }

        target = vod_prefetch_target;
        target_fi = vod_prefetch_target_fi;
        vod_prefetch_requested = 0;
        vod_prefetch_ready = 0;
        vod_prefetch_fi = -1;
        vod_prefetch_start = (uint64_t)-1;
        vod_prefetch_len = 0;
        vod_prefetch_in_progress = 1;
        vod_prefetch_abort = 0;
        pthread_mutex_unlock(&vod_mu);

        size_t fetched = 0;
        int res = -1;
        if (target_fi >= 0 && target_fi < g_num_files) {
            struct virtual_file *vf = &g_files[target_fi];
            res = vod_http_fetch_chunk(&vod_bg_sock, vf->http_host, vf->http_port, vf->http_path,
                                      vf->file_size, vod_prefetch_buf, target, VOD_CHUNK_SZ,
                                      &fetched, &vod_prefetch_abort, now_ms() + 15000);
        }

        pthread_mutex_lock(&vod_mu);
        vod_prefetch_in_progress = 0;
        if (res == 0 && !vod_prefetch_abort && fetched > 0) {
            vod_prefetch_fi = target_fi;
            vod_prefetch_start = target;
            vod_prefetch_len = fetched;
            vod_prefetch_ready = 1;
        } else {
            vod_prefetch_ready = 0;
            vod_prefetch_fi = -1;
        }
        pthread_cond_broadcast(&vod_done_cv);
        pthread_mutex_unlock(&vod_mu);
    }
    vod_bg_sock_close();
    return NULL;
}

static ssize_t serve_vod_slice_file(int fi, uint8_t *dst, uint64_t foff, size_t to_read, uint64_t deadline_ms) {
    if (fi < 0 || fi >= g_num_files) return 0;
    struct virtual_file *f = &g_files[fi];
    if (f->file_size == 0 || to_read == 0 || foff >= f->file_size) return 0;
    if (now_ms() >= deadline_ms) return -ETIMEDOUT;
    if (foff + (uint64_t)to_read > f->file_size) to_read = (size_t)(f->file_size - foff);

    if (!f->head_fetched) {
        vod_init_head_cache_file(fi);
    }

    /* 1. Head Pinning Cache */
    if (foff < f->head_len && f->head_cache) {
        size_t avail = f->head_len - (size_t)foff;
        size_t take = (to_read < avail) ? to_read : avail;
        memcpy(dst, f->head_cache + foff, take);

        if (foff >= f->head_len * 3 / 4 && f->head_len < f->file_size) {
            pthread_mutex_lock(&vod_mu);
            uint64_t bridge_target = f->head_len;
            if (!vod_prefetch_in_progress &&
                !(vod_prefetch_ready && vod_prefetch_fi == fi && vod_prefetch_start == bridge_target) &&
                !(vod_media_fi == fi && vod_media_start == bridge_target && vod_media_len > 0)) {
                vod_prefetch_target_fi = fi;
                vod_prefetch_target = bridge_target;
                vod_prefetch_requested = 1;
                pthread_cond_signal(&vod_prefetch_cv);
            }
            pthread_mutex_unlock(&vod_mu);
        }

        return (ssize_t)take;
    }

    /* 2. Dedicated VOD Media Sliding Cache & Prefetch for foff >= head_len */
    pthread_mutex_lock(&vod_mu);

    /* Case A: HIT in vod_media_cache */
    if (vod_media_fi == fi && foff >= vod_media_start && foff < vod_media_start + vod_media_len) {
        size_t avail = vod_media_len - (size_t)(foff - vod_media_start);
        size_t take = (to_read < avail) ? to_read : avail;
        memcpy(dst, vod_media_cache + (foff - vod_media_start), take);

        if (foff >= vod_media_start + vod_media_len / 2) {
            uint64_t next_start = vod_media_start + vod_media_len;
            if (next_start < f->file_size) {
                if (!(vod_prefetch_ready && vod_prefetch_fi == fi && vod_prefetch_start == next_start) &&
                    !(vod_prefetch_in_progress && vod_prefetch_target_fi == fi && vod_prefetch_target == next_start)) {
                    vod_prefetch_target_fi = fi;
                    vod_prefetch_target = next_start;
                    vod_prefetch_requested = 1;
                    pthread_cond_signal(&vod_prefetch_cv);
                }
            }
        }
        pthread_mutex_unlock(&vod_mu);
        return (ssize_t)take;
    }

    /* Case B: HIT in vod_prefetch_buf */
    if (vod_prefetch_ready && vod_prefetch_fi == fi && foff >= vod_prefetch_start && foff < vod_prefetch_start + vod_prefetch_len) {
        uint8_t *tmp = vod_media_cache;
        vod_media_cache = vod_prefetch_buf;
        vod_prefetch_buf = tmp;

        vod_media_fi = fi;
        vod_media_start = vod_prefetch_start;
        vod_media_len = vod_prefetch_len;

        vod_prefetch_ready = 0;
        vod_prefetch_fi = -1;
        vod_prefetch_start = (uint64_t)-1;
        vod_prefetch_len = 0;

        size_t avail = vod_media_len - (size_t)(foff - vod_media_start);
        size_t take = (to_read < avail) ? to_read : avail;
        memcpy(dst, vod_media_cache + (foff - vod_media_start), take);

        if (foff >= vod_media_start + vod_media_len / 2) {
            uint64_t next_start = vod_media_start + vod_media_len;
            if (next_start < f->file_size &&
                !(vod_prefetch_in_progress && vod_prefetch_target_fi == fi && vod_prefetch_target == next_start)) {
                vod_prefetch_target_fi = fi;
                vod_prefetch_target = next_start;
                vod_prefetch_requested = 1;
                pthread_cond_signal(&vod_prefetch_cv);
            }
        }
        pthread_mutex_unlock(&vod_mu);
        return (ssize_t)take;
    }

    /* Case C: Currently downloading in background */
    if (vod_prefetch_in_progress && vod_prefetch_target_fi == fi &&
        foff >= vod_prefetch_target && foff < vod_prefetch_target + VOD_CHUNK_SZ) {
        while (vod_prefetch_in_progress && !vod_prefetch_abort && g_running) {
            uint64_t current_time = now_ms();
            if (current_time >= deadline_ms) break;

            uint64_t wait_ms = deadline_ms - current_time;
            if (wait_ms > 5000) wait_ms = 5000;

            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += (time_t)(wait_ms / 1000);
            ts.tv_nsec += (long)((wait_ms % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }

            int rc = pthread_cond_timedwait(&vod_done_cv, &vod_mu, &ts);
            if (rc == ETIMEDOUT && now_ms() >= deadline_ms) {
                break;
            }
        }
        if (vod_prefetch_ready && vod_prefetch_fi == fi &&
            foff >= vod_prefetch_start && foff < vod_prefetch_start + vod_prefetch_len) {
            uint8_t *tmp = vod_media_cache;
            vod_media_cache = vod_prefetch_buf;
            vod_prefetch_buf = tmp;

            vod_media_fi = fi;
            vod_media_start = vod_prefetch_start;
            vod_media_len = vod_prefetch_len;

            vod_prefetch_ready = 0;
            vod_prefetch_fi = -1;
            vod_prefetch_start = (uint64_t)-1;
            vod_prefetch_len = 0;

            size_t avail = vod_media_len - (size_t)(foff - vod_media_start);
            size_t take = (to_read < avail) ? to_read : avail;
            memcpy(dst, vod_media_cache + (foff - vod_media_start), take);

            if (foff >= vod_media_start + vod_media_len / 2) {
                uint64_t next_start = vod_media_start + vod_media_len;
                if (next_start < f->file_size &&
                    !(vod_prefetch_in_progress && vod_prefetch_target_fi == fi && vod_prefetch_target == next_start)) {
                    vod_prefetch_target_fi = fi;
                    vod_prefetch_target = next_start;
                    vod_prefetch_requested = 1;
                    pthread_cond_signal(&vod_prefetch_cv);
                }
            }
            pthread_mutex_unlock(&vod_mu);
            return (ssize_t)take;
        }
    }

    /* Case D: Cache Miss / Seek outside both buffers */
    if (now_ms() >= deadline_ms) {
        pthread_mutex_unlock(&vod_mu);
        return -ETIMEDOUT;
    }

    if (vod_prefetch_in_progress) {
        vod_prefetch_abort = 1;
        vod_bg_sock_shutdown();
        while (vod_prefetch_in_progress && g_running) {
            uint64_t current_time = now_ms();
            if (current_time >= deadline_ms) break;
            uint64_t wait_ms = deadline_ms - current_time;
            if (wait_ms > 1000) wait_ms = 1000;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += (time_t)(wait_ms / 1000);
            ts.tv_nsec += (long)((wait_ms % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&vod_done_cv, &vod_mu, &ts);
        }
        vod_prefetch_abort = 0;
    }
    vod_prefetch_requested = 0;
    vod_prefetch_target = (uint64_t)-1;
    vod_prefetch_target_fi = -1;
    vod_prefetch_ready = 0;
    vod_prefetch_fi = -1;
    vod_prefetch_start = (uint64_t)-1;
    vod_prefetch_len = 0;

    if (now_ms() >= deadline_ms) {
        pthread_mutex_unlock(&vod_mu);
        return -ETIMEDOUT;
    }

    vod_media_fi = -1;
    vod_media_start = (uint64_t)-1;
    vod_media_len = 0;

    pthread_mutex_unlock(&vod_mu);

    size_t fetched = 0;
    volatile int dummy_abort = 0;
    int res = vod_http_fetch_chunk(&vod_fg_sock, f->http_host, f->http_port, f->http_path,
                                  f->file_size, vod_media_cache, foff, VOD_CHUNK_SZ,
                                  &fetched, &dummy_abort, deadline_ms);

    pthread_mutex_lock(&vod_mu);

    if (res == 0 && fetched > 0) {
        vod_media_fi = fi;
        vod_media_start = foff;
        vod_media_len = fetched;

        size_t avail = vod_media_len;
        size_t take = (to_read < avail) ? to_read : avail;
        memcpy(dst, vod_media_cache, take);

        if (foff >= vod_media_start + vod_media_len / 2) {
            uint64_t next_start = vod_media_start + vod_media_len;
            if (next_start < f->file_size) {
                vod_prefetch_target_fi = fi;
                vod_prefetch_target = next_start;
                vod_prefetch_requested = 1;
                pthread_cond_signal(&vod_prefetch_cv);
            }
        }
        pthread_mutex_unlock(&vod_mu);
        return (ssize_t)take;
    }

    pthread_mutex_unlock(&vod_mu);
    return -EIO;
}

static int serve_vod_backend(int fi, uint8_t *dst, uint64_t foff, size_t c, uint64_t deadline_ms) {
    if (fi < 0 || fi >= g_num_files) {
        memset(dst, 0, c);
        return 0;
    }
    struct virtual_file *vf = &g_files[fi];
    if (foff >= vf->file_size) {
        memset(dst, 0, c);
        return 0;
    }

    if (vf->src_type == SRC_TEST_PATTERN) {
        for (size_t i = 0; i < c; i++) {
            dst[i] = (uint8_t)((foff + (uint64_t)i) & 0xFFULL);
        }
        return 0;
    }

    pthread_mutex_lock(&vf->vf_mu);
    enum vod_state st = vf->state;
    uint64_t avail = vf->bytes_available;
    pthread_mutex_unlock(&vf->vf_mu);

    size_t ok = c;
    if (st == VOD_STATE_PROCESSING) {
        if (foff >= avail) {
            pthread_mutex_lock(&vf->vf_mu);
            while (foff >= vf->bytes_available && now_ms() < deadline_ms && g_running) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                ts.tv_nsec += 50000000; /* 50ms step */
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&vf->vf_cv, &vf->vf_mu, &ts);
            }
            avail = vf->bytes_available;
            pthread_mutex_unlock(&vf->vf_mu);
            if (foff >= avail) {
                memset(dst, 0, c);
                return 0;
            }
        }
        if (foff + (uint64_t)c > avail) {
            ok = (size_t)(avail - foff);
        }
    }

    if (vf->src_type == SRC_VOD_HTTP_RANGE) {
        size_t done = 0;
        while (done < ok) {
            ssize_t rd = serve_vod_slice_file(fi, dst + done, foff + done, ok - done, deadline_ms);
            if (rd <= 0) {
                memset(dst + done, 0, c - done);
                return -EIO;
            }
            done += (size_t)rd;
        }
        if (c > ok) {
            memset(dst + ok, 0, c - ok);
        }
        return 0;
    }

    memset(dst, 0, c);
    return 0;
}

static inline void patch_serial_in_buffer(uint8_t *buf, uint64_t cur_disk, size_t len, uint64_t target_disk_off) {
    if (cur_disk < target_disk_off + 8 && cur_disk + (uint64_t)len > target_disk_off) {
        uint64_t start_overlap = (cur_disk > target_disk_off) ? cur_disk : target_disk_off;
        uint64_t end_overlap = (cur_disk + (uint64_t)len < target_disk_off + 8) ? cur_disk + (uint64_t)len : target_disk_off + 8;
        size_t buf_offset = (size_t)(start_overlap - cur_disk);
        size_t serial_offset = (size_t)(start_overlap - target_disk_off);
        size_t copy_bytes = (size_t)(end_overlap - start_overlap);
        memcpy(buf + buf_offset, g_current_volume_serial + serial_offset, copy_bytes);
    }
}

/* =========================================================================
 *  MASTER MULTI-FILE DISK SERVING FUNCTION
 * ========================================================================= */
int ntfs_serve_disk(uint8_t *dst, uint64_t disk_offset, size_t n, uint64_t deadline_ms) {
    size_t done = 0;
    int ret_status = 0;

    while (done < n) {
        uint64_t cur_disk = disk_offset + (uint64_t)done;
        uint64_t sec = cur_disk / BPS;
        size_t sec_off = (size_t)(cur_disk % BPS);
        int file_idx = -1;
        uint64_t foff = 0;
        uint64_t avail_in_ext = 0;

        pthread_rwlock_rdlock(&g_catalog_rwlock);
        int res = virtual_file_lookup(g_files, g_num_files, sec, sec_off, &file_idx, &foff, &avail_in_ext);

        if (res == VIRT_RES_EOF) {
            pthread_rwlock_unlock(&g_catalog_rwlock);
            memset(dst + done, 0, n - done);
            break;
        } else if (res >= 0) {
            struct virtual_file *vf = &g_files[file_idx];
            size_t c = n - done;
            if ((uint64_t)c > avail_in_ext) {
                c = (size_t)avail_in_ext;
            }

            uint64_t t_now = now_ms();
            if (g_tel_start_ms == 0) g_tel_start_ms = t_now;
            uint64_t t_rel = t_now - g_tel_start_ms;
            uint64_t t_sec = t_rel / 1000;
            uint64_t t_ms = t_rel % 1000;
            uint64_t dt = (vf->tel_last_t_ms == 0) ? 0 : (t_now - vf->tel_last_t_ms);
            double rate = (dt > 0) ? (((double)c / (1024.0 * 1024.0)) / ((double)dt / 1000.0)) : 0.0;

            const char *type_str = "SEQ";
            if (vf->tel_count == 0) {
                type_str = "PROBE";
            } else if (foff == vf->tel_last_foff + vf->tel_last_sz) {
                type_str = "SEQ";
            } else if ((vf->file_size > 131072 && foff + c >= vf->file_size - 131072) || (foff < 131072 && vf->tel_max_foff > 1048576)) {
                type_str = "PROBE";
            } else {
                type_str = "SEEK";
            }

            vf->tel_count++;
            vf->tel_last_t_ms = t_now;
            vf->tel_last_foff = foff;
            vf->tel_last_sz = c;
            if (foff + c > vf->tel_max_foff) {
                vf->tel_max_foff = foff + c;
            }

            if (vf->src_type == SRC_LIVE_FIFO) {
                pthread_rwlock_unlock(&g_catalog_rwlock);
                pthread_mutex_lock(&g_mu);

                if (g_is_favorites_mode && file_idx != g_active_channel_idx) {
                    /* Channel switch debounce: require sustained reads (>= 3 blocks AND >= 256KB) */
                    if (vf->tel_count >= 3 && vf->tel_max_foff >= 262144) {
                        uint64_t now_sw = now_ms();
                        if (now_sw - g_last_channel_switch_ms > 2000) {
                            g_last_channel_switch_ms = now_sw;
                            int prev_idx = g_active_channel_idx;
                            g_active_channel_idx = file_idx;
                            fprintf(stderr, "[FUSE_CHANNELS] TV selected channel switch: File %d (%s) -> File %d (%s, ID: %s)\n",
                                    prev_idx, g_files[prev_idx].path, file_idx, vf->path, vf->id);

                            trigger_vps_channel_switch(vf->id);

                            /* Invalidate old anchor to force re-anchor to new channel's PAT/SPS */
                            g_base_valid = 0;
                            g_anchor_foff = (uint64_t)-1;
                            g_anchor_stream_pos = 0;
                            g_prev_Fend = (uint64_t)-1;
                            g_anchor_birth_ms = now_sw;
                            reset_probe_state();
                            pthread_cond_broadcast(&g_cv);
                        }
                    }
                }

                serve_live_backend(dst + done, foff, c, deadline_ms);

                g_last_video_read_ms = t_now;
                g_video_bytes_streamed += c;
                if (!g_playback_armed && g_video_bytes_streamed >= FLOW_CONTROL_ARM_BYTES) {
                    g_playback_armed = 1;
                    fprintf(stderr, "[FLOW_CONTROL] Playback active (streamed %llu MB >= %llu MB threshold)\n",
                            (unsigned long long)(g_video_bytes_streamed / (1024 * 1024)),
                            (unsigned long long)(FLOW_CONTROL_ARM_BYTES / (1024 * 1024)));
                }

                uint64_t reader_pos = foff_to_stream_pos(foff);
                int64_t lead_bytes = (int64_t)g_s_write - (int64_t)reader_pos;
                double lead_sec = (double)lead_bytes / 617600.0;

                static uint64_t last_live_tel_ms = 0;
                if (t_now - last_live_tel_ms >= 1000 || g_live_starve_near_count > 0 || g_live_fill_null_count > 0) {
                    last_live_tel_ms = t_now;
                    fprintf(stderr, "[LIVE_TEL] #%" PRIu64 " t=%" PRIu64 ".%03" PRIu64 "ms LBA=%" PRIu64 " foff=%" PRIu64 " sz=%zu dt=%" PRIu64 "ms rate=%.2fMB/s lead_bytes=%" PRId64 " lead_sec=%.2fs epoch=%" PRIu64 " base=%" PRIu64 " S_write=%" PRIu64 " max_foff=%" PRIu64 " near_starve=%" PRIu64 " nulls=%" PRIu64 " type=%s file=%s\n",
                            vf->tel_count, t_sec, t_ms, sec, foff, c, dt, rate, lead_bytes, lead_sec,
                            g_epoch, g_anchor_stream_pos, g_s_write, vf->tel_max_foff, g_live_starve_near_count, g_live_fill_null_count, type_str, vf->path);
                }

                pthread_mutex_unlock(&g_mu);
            } else {
                static uint64_t last_vod_tel_ms = 0;
                if (t_now - last_vod_tel_ms >= 1000) {
                    last_vod_tel_ms = t_now;
                    fprintf(stderr, "[VOD_TEL] #%" PRIu64 " t=%" PRIu64 ".%03" PRIu64 "ms LBA=%" PRIu64 " foff=%" PRIu64 " sz=%zu dt=%" PRIu64 "ms rate=%.2fMB/s max_foff=%" PRIu64 " type=%s file=%s\n",
                            vf->tel_count, t_sec, t_ms, sec, foff, c, dt, rate, vf->tel_max_foff, type_str, vf->path);
                }

                pthread_rwlock_unlock(&g_catalog_rwlock);
                int vres = serve_vod_backend(file_idx, dst + done, foff, c, deadline_ms);
                if (vres < 0) {
                    ret_status = vres;
                }
            }
            done += c;
        } else {
            pthread_rwlock_unlock(&g_catalog_rwlock);
            /* Sector is NTFS filesystem metadata or unallocated sector */
            size_t c = n - done;
            /* Clamp c to the next virtual file extent start to avoid over-reading metadata into a file extent */
            uint64_t next_ext_disk = (uint64_t)-1;
            for (int fi = 0; fi < g_num_files; fi++) {
                const struct virtual_file *fv = &g_files[fi];
                if (!fv->active) continue;
                for (int ei = 0; ei < fv->num_extents; ei++) {
                    uint64_t ext_start_disk = fv->extents[ei].lba_start * BPS;
                    if (ext_start_disk > cur_disk && ext_start_disk < next_ext_disk) {
                        next_ext_disk = ext_start_disk;
                    }
                }
            }
            if (next_ext_disk != (uint64_t)-1 && cur_disk + (uint64_t)c > next_ext_disk) {
                c = (size_t)(next_ext_disk - cur_disk);
            }

            if (g_meta_fd >= 0) {
                ssize_t rd = pread(g_meta_fd, dst + done, c, (off_t)cur_disk);
                if (rd < (ssize_t)c) {
                    if (rd > 0) {
                        memset(dst + done + (size_t)rd, 0, c - (size_t)rd);
                    } else {
                        memset(dst + done, 0, c);
                    }
                }
            } else {
                memset(dst + done, 0, c);
            }

            /* Intercept and dynamically patch Volume Serial Number in memory */
            if (g_serial_initialized) {
                patch_serial_in_buffer(dst + done, cur_disk, c, 0x48);
                if (g_total_sectors > 0) {
                    patch_serial_in_buffer(dst + done, cur_disk, c, (g_total_sectors - 1) * BPS + 0x48);
                }
            }
            done += c;
        }
    }
    return ret_status;
}

static volatile int g_flush_requested = 0;
static void handle_sigusr1(int s) {
    (void)s;
    g_flush_requested = 1;
}

static uint8_t g_feeder_buf[65536];
static uint8_t g_feeder_sync_acc[131072];

/* Feeder thread for live mode */
static void *feeder_thread(void *arg) {
    const char *fifo_path = (const char *)arg;
    int need_ts_sync = 0;
    size_t sync_acc_len = 0;
    size_t sync_total_scanned = 0;

    while (g_running) {
        int fd = open(fifo_path, O_RDONLY);
        if (fd < 0) {
            sleep(1);
            continue;
        }
        printf("[*] FIFO feeder connected to %s\n", fifo_path);

        while (g_running) {
            if (g_flush_requested || access("/data/local/tmp/fuse_flush", F_OK) == 0) {
                g_flush_requested = 0;
                unlink("/data/local/tmp/fuse_flush");
                fprintf(stderr, "[*] FLUSH requested! Draining pipe, resetting ring buffer and randomizing volume serial.\n");

                /* Drain Linux FIFO kernel buffer */
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                while (read(fd, g_feeder_buf, sizeof(g_feeder_buf)) > 0) {}
                fcntl(fd, F_SETFL, flags);

                /* Reset internal live stream state */
                pthread_mutex_lock(&g_mu);
                g_s_write = 0;
                g_base_valid = 0;
                g_anchor_foff = (uint64_t)-1;
                g_anchor_stream_pos = 0;
                g_have_data = 0;
                g_prev_Fend = (uint64_t)-1;
                g_hcache_len = 0;
                g_anchor_birth_ms = now_ms();
                g_epoch++;
                reset_probe_state();
                pthread_cond_broadcast(&g_cv);
                pthread_mutex_unlock(&g_mu);

                /* Randomize volume serial so TV sees a new filesystem */
                randomize_volume_serial();

                need_ts_sync = 1;
                sync_acc_len = 0;
                sync_total_scanned = 0;
                continue;
            }

            ssize_t n = read(fd, g_feeder_buf, sizeof(g_feeder_buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            size_t bytes_read = (size_t)n;
            if (bytes_read > sizeof(g_feeder_buf)) bytes_read = sizeof(g_feeder_buf);

            const uint8_t *data_to_write = g_feeder_buf;
            size_t bytes_to_write = bytes_read;

            if (need_ts_sync) {
                if (sync_acc_len + bytes_read > sizeof(g_feeder_sync_acc)) {
                    sync_acc_len = 0;
                }
                memcpy(g_feeder_sync_acc + sync_acc_len, g_feeder_buf, bytes_read);
                sync_acc_len += bytes_read;
                sync_total_scanned += bytes_read;

                size_t lock_idx = (size_t)-1;
                for (size_t i = 0; i + 3 * 188 <= sync_acc_len; i++) {
                    if (g_feeder_sync_acc[i] == 0x47 && g_feeder_sync_acc[i + 188] == 0x47 && g_feeder_sync_acc[i + 376] == 0x47) {
                        lock_idx = i;
                        break;
                    }
                }
                if (lock_idx == (size_t)-1) {
                    if (sync_total_scanned >= 64 * 1024) {
                        /* Fallback: force lock on first available 0x47 (ALTO 2) */
                        for (size_t i = 0; i + 188 <= sync_acc_len; i++) {
                            if (g_feeder_sync_acc[i] == 0x47) {
                                lock_idx = i;
                                break;
                            }
                        }
                    }
                }
                if (lock_idx == (size_t)-1) {
                    if (sync_acc_len > 3 * 188) {
                        size_t keep = 3 * 188 - 1;
                        memmove(g_feeder_sync_acc, g_feeder_sync_acc + sync_acc_len - keep, keep);
                        sync_acc_len = keep;
                    }
                    continue;
                }
                data_to_write = g_feeder_sync_acc + lock_idx;
                bytes_to_write = sync_acc_len - lock_idx;
                need_ts_sync = 0;
                sync_acc_len = 0;
                sync_total_scanned = 0;
                fprintf(stderr, "[FEEDER] TS stream re-aligned after flush (skipped %zu offset bytes)\n", lock_idx);
            }

            pthread_mutex_lock(&g_mu);
            uint64_t at = g_s_write;
            ring_write(data_to_write, bytes_to_write);
            hcache_feed(data_to_write, bytes_to_write, at);
            static uint64_t last_feed_log = 0;
            if (g_s_write - last_feed_log >= 2 * 1024 * 1024) {
                last_feed_log = g_s_write;
                fprintf(stderr, "[FEEDER] S_write=%lluMB\n", (unsigned long long)(g_s_write / (1024 * 1024)));
            }
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
        }
        close(fd);
        if (!g_running) break;
        sleep(1);
    }
    return NULL;
}

static void init_test_pattern(void) {
    g_test_pattern_mode = 1;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < (size_t)RINGSZ; i++) {
        g_ring[i] = (uint8_t)(i & 0xFFULL);
    }
    g_s_write = RINGSZ;
    g_base = 0;
    g_base_valid = 1;
    g_have_data = 1;
    pthread_mutex_unlock(&g_mu);
}

static void handle_sig(int s) {
    (void)s;
    g_running = false;
}

/* =========================================================================
 *  CATALOG AND GEOMETRY INITIALIZATION
 * ========================================================================= */
static void init_default_single_file_catalog(void) {
    pthread_rwlock_wrlock(&g_catalog_rwlock);
    g_num_files = 1;
    struct virtual_file *vf = &g_files[0];
    memset(vf, 0, sizeof(*vf));
    vf->file_id = 0;
    strncpy(vf->id, "live_tv", sizeof(vf->id) - 1);
    strncpy(vf->category, "LIVE", sizeof(vf->category) - 1);
    strncpy(vf->path, "TV AO VIVO.trp", sizeof(vf->path) - 1);
    vf->file_size = NTFS_FILE_SIZE;
    vf->src_type = SRC_LIVE_FIFO;
    vf->state = VOD_STATE_READY;
    vf->bytes_available = NTFS_FILE_SIZE;
    vf->num_extents = NTFS_NUM_EXTENTS;
    for (int i = 0; i < NTFS_NUM_EXTENTS; i++) {
        vf->extents[i] = NTFS_EXTENTS[i];
    }
    vf->active = 1;
    vf->http_sock = -1;
    pthread_mutex_init(&vf->vf_mu, NULL);
    pthread_cond_init(&vf->vf_cv, NULL);
    pthread_rwlock_unlock(&g_catalog_rwlock);
}

#include "catalog_hierarchy.h"
#include "catalog_favorites.h"

static void detect_vbr_geometry(int meta_fd) {
    if (meta_fd < 0) return;
    uint8_t vbr[512];
    if (pread(meta_fd, vbr, 512, 0) == 512) {
        if (vbr[0] == 0xEB && vbr[2] == 0x90 && memcmp(vbr + 3, "NTFS    ", 8) == 0) {
            uint64_t tot_sec = 0;
            memcpy(&tot_sec, vbr + 0x28, 8);
            if (tot_sec > 0) {
                /* In NTFS BPB, 0x28 is volume sectors EXCLUDING the backup boot sector.
                 * Total disk sectors = tot_sec + 1; backup VBR is at LBA tot_sec (g_total_sectors - 1). */
                g_total_sectors = tot_sec + 1;
                g_disk_size = g_total_sectors * BPS;
                fprintf(stderr, "[*] Detected NTFS VBR geometry: %" PRIu64 " sectors (%.2f GiB)\n",
                        g_total_sectors, (double)g_disk_size / (1024.0 * 1024.0 * 1024.0));
            }
            memcpy(g_current_volume_serial, vbr + 0x48, 8);
            g_serial_initialized = 1;
            uint64_t s_val = 0;
            memcpy(&s_val, g_current_volume_serial, 8);
            fprintf(stderr, "[*] Initial NTFS Volume Serial: 0x%016llx\n", (unsigned long long)s_val);
        }
    }
}

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    ucontext_t *uc = (ucontext_t *)ucontext;
    uintptr_t pc = 0;
#if defined(__arm__)
    pc = (uintptr_t)uc->uc_mcontext.arm_pc;
#elif defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#endif
    fprintf(stderr, "\n[FATAL CRASH] Signal %d (%s) at address %p (PC: %p)\n",
            sig,
            sig == SIGSEGV ? "SIGSEGV" :
            sig == SIGBUS ? "SIGBUS" :
            sig == SIGFPE ? "SIGFPE" :
            sig == SIGILL ? "SIGILL" :
            sig == SIGABRT ? "SIGABRT" : "OTHER",
            info ? info->si_addr : NULL,
            (void *)pc);
    fflush(stderr);
    _exit(128 + sig);
}

#ifndef TEST_SUITE
int main(int argc, char **argv) {
    setlinebuf(stdout);
    setlinebuf(stderr);

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa_crash;
    memset(&sa_crash, 0, sizeof(sa_crash));
    sigemptyset(&sa_crash.sa_mask);
    sa_crash.sa_sigaction = crash_handler;
    sa_crash.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa_crash, NULL);
    sigaction(SIGBUS, &sa_crash, NULL);
    sigaction(SIGILL, &sa_crash, NULL);
    sigaction(SIGFPE, &sa_crash, NULL);
    sigaction(SIGABRT, &sa_crash, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sa.sa_flags = 0; /* NO SA_RESTART so blocking read(/dev/fuse) exits on signal */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_usr;
    memset(&sa_usr, 0, sizeof(sa_usr));
    sa_usr.sa_handler = handle_sigusr1;
    sa_usr.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr, NULL);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mountpoint> [fifo_path] [metadata_img] [--multi]\n", argv[0]);
        fprintf(stderr, "       Pass fifo_path='test' for deterministic lab testing.\n");
        fprintf(stderr, "       Pass --multi to load the multi-file VOD hierarchy.\n");
        return 1;
    }

    const char *mnt = argv[1];
    const char *fifo = "test";
    const char *meta = NULL;
    int multi_mode = 0;

    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi") == 0) {
            multi_mode = 1;
        } else {
            if (pos == 0) mnt = argv[i];
            else if (pos == 1) fifo = argv[i];
            else if (pos == 2) meta = argv[i];
            pos++;
        }
    }

    if (meta && (strstr(meta, "hierarchy") || strstr(meta, "vod_hierarchy") || strstr(meta, "favorites"))) {
        multi_mode = 1;
    }

    if (meta) {
        g_meta_fd = open(meta, O_RDWR);
        if (g_meta_fd < 0) {
            g_meta_fd = open(meta, O_RDONLY);
        }
        if (g_meta_fd < 0) {
            fprintf(stderr, "[!] Warning: Failed to open metadata image %s: %s\n", meta, strerror(errno));
        } else {
            printf("[*] Metadata image loaded: %s\n", meta);
            detect_vbr_geometry(g_meta_fd);
            randomize_volume_serial();
        }
    }

    if (meta && strstr(meta, "favorites")) {
        printf("[*] Initializing MULTI-CHANNEL FAVORITES live catalog (9 channels)\n");
        init_favorites_catalog();
        g_is_favorites_mode = 1;
    } else if (multi_mode) {
        printf("[*] Initializing MULTI-FILE VOD hierarchy (8 files, FILMES/SERIES/ANIMES)\n");
        init_full_hierarchy_catalog();
    } else {
        printf("[*] Initializing SINGLE-FILE live baseline (TV AO VIVO.trp, 3 extents)\n");
        init_default_single_file_catalog();
    }

    /* Initialize monotonic condvars */
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &cattr);
    pthread_cond_init(&vod_prefetch_cv, &cattr);
    pthread_cond_init(&vod_done_cv, &cattr);
    pthread_condattr_destroy(&cattr);

    /* Initialize thread attributes with 2MB stack to prevent musl stack overflow */
    pthread_attr_t t_attr;
    pthread_attr_init(&t_attr);
    pthread_attr_setstacksize(&t_attr, 2 * 1024 * 1024);

    /* Start VOD prefetch background thread */
    if (pthread_create(&vod_prefetch_tid, &t_attr, vod_prefetch_worker, NULL) == 0) {
        vod_prefetch_thread_started = 1;
    }

    if (strcmp(fifo, "test") == 0) {
        printf("[*] Running in TEST PATTERN mode\n");
        init_test_pattern();
    } else {
        pthread_t th;
        if (pthread_create(&th, &t_attr, feeder_thread, (void *)fifo) != 0) {
            fprintf(stderr, "[!] Failed to spawn feeder thread for %s\n", fifo);
        }
    }
    pthread_attr_destroy(&t_attr);

    /* Stale mount cleanup before mount */
    umount2(mnt, MNT_DETACH);

    g_fuse_fd = open("/dev/fuse", O_RDWR);
    if (g_fuse_fd < 0) {
        fprintf(stderr, "[!] Cannot open /dev/fuse: %s\n", strerror(errno));
        return 1;
    }

    char opts[256];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=0040755,user_id=%u,group_id=%u,allow_other",
             g_fuse_fd, (unsigned int)getuid(), (unsigned int)getgid());

    if (mount("fuse_ntfs", mnt, "fuse", MS_NOSUID | MS_NODEV, opts) < 0) {
        fprintf(stderr, "[!] mount failed on %s: %s\n", mnt, strerror(errno));
        close(g_fuse_fd);
        return 1;
    }

    printf("[+] NTFS FUSE daemon mounted at %s (Disk Size: %" PRIu64 " bytes, Files: %d)\n",
           mnt, (uint64_t)g_disk_size, g_num_files);

    /* FUSE main event loop buffers (static BSS, 8-byte aligned for ARMv7) */
    static union {
        uint8_t raw[131072 + 4096];
        uint64_t align;
    } in_u __attribute__((aligned(8)));

    static union {
        uint8_t raw[131072 + 4096];
        uint64_t align;
    } out_u __attribute__((aligned(8)));

    #define in_buf  (in_u.raw)
    #define out_buf (out_u.raw)

    while (g_running) {
        ssize_t n = read(g_fuse_fd, in_buf, sizeof(in_buf) - 1);
        if (n < (ssize_t)sizeof(struct fuse_in_header)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == ENOENT)) continue;
            break;
        }
        if (n >= 0 && (size_t)n < sizeof(in_buf)) in_buf[n] = '\0';

        struct fuse_in_header inh;
        memcpy(&inh, in_buf, sizeof(inh));
        uint32_t opcode = inh.opcode;
        uint64_t unique = inh.unique;

        if (opcode == FUSE_INIT) {
            struct fuse_init_in ii;
            size_t avail = (size_t)n > sizeof(inh) ? (size_t)n - sizeof(inh) : 0;
            memset(&ii, 0, sizeof(ii));
            if (avail > sizeof(ii)) avail = sizeof(ii);
            if (avail > 0) memcpy(&ii, in_buf + sizeof(inh), avail);

            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;

            struct fuse_init_out init_out;
            memset(&init_out, 0, sizeof(init_out));
            init_out.major = 7;
            init_out.minor = (ii.minor < 26) ? ii.minor : 26;
            init_out.max_readahead = 131072;
            init_out.flags = ii.flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES);
            init_out.max_write = 131072;

            size_t out_payload_len;
            if (ii.minor < 23) {
                out_payload_len = 24;
            } else {
                out_payload_len = sizeof(struct fuse_init_out);
            }
            outh.len = (uint32_t)(sizeof(outh) + out_payload_len);

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = &init_out;
            iov[1].iov_len = out_payload_len;
            if (writev(g_fuse_fd, iov, 2) < 0) {}
            continue;
        }

        /* FUSE_FORGET and FUSE_BATCH_FORGET must NEVER receive a reply */
        if (opcode == FUSE_FORGET || opcode == 42 /* FUSE_BATCH_FORGET */) {
            continue;
        }

        if (opcode == FUSE_GETATTR) {
            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;
            outh.len = (uint32_t)(sizeof(outh) + sizeof(struct fuse_attr_out));

            struct fuse_attr_out att;
            memset(&att, 0, sizeof(att));
            att.attr_valid = 10;
            att.attr.ino = inh.nodeid;
            att.attr.size = (inh.nodeid == 1) ? 4096ULL : (uint64_t)g_disk_size;
            att.attr.blocks = (inh.nodeid == 1) ? 8ULL : (uint64_t)g_total_sectors;
            att.attr.mode = (inh.nodeid == 1) ? (S_IFDIR | 0755U) : (S_IFREG | 0644U);
            att.attr.nlink = 1;

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = &att;
            iov[1].iov_len = sizeof(att);
            if (writev(g_fuse_fd, iov, 2) < 0) {}
            continue;
        }

        if (opcode == FUSE_LOOKUP) {
            const char *name = (const char *)(in_buf + sizeof(inh));
            if (strcmp(name, FILE_NAME) == 0) {
                struct fuse_out_header outh;
                memset(&outh, 0, sizeof(outh));
                outh.unique = unique;
                outh.len = (uint32_t)(sizeof(outh) + sizeof(struct fuse_entry_out));

                struct fuse_entry_out entry;
                memset(&entry, 0, sizeof(entry));
                entry.nodeid = FILE_INO;
                entry.generation = 1;
                entry.entry_valid = 10;
                entry.attr_valid = 10;
                entry.attr.ino = FILE_INO;
                entry.attr.size = (uint64_t)g_disk_size;
                entry.attr.blocks = (uint64_t)g_total_sectors;
                entry.attr.mode = S_IFREG | 0644U;
                entry.attr.nlink = 1;

                struct iovec iov[2];
                iov[0].iov_base = &outh;
                iov[0].iov_len = sizeof(outh);
                iov[1].iov_base = &entry;
                iov[1].iov_len = sizeof(entry);
                if (writev(g_fuse_fd, iov, 2) < 0) {}
                continue;
            }
            /* Unknown filename: return -ENOENT */
            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;
            outh.error = -ENOENT;
            outh.len = (uint32_t)sizeof(outh);
            if (write(g_fuse_fd, &outh, sizeof(outh)) < 0) {}
            continue;
        }

        if (opcode == FUSE_OPEN) {
            on_open();
            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;
            outh.len = (uint32_t)(sizeof(outh) + sizeof(struct fuse_open_out));

            struct fuse_open_out oout;
            memset(&oout, 0, sizeof(oout));
            oout.fh = 1;
            oout.open_flags = FOPEN_DIRECT_IO; /* Direct I/O prevents stale kernel page cache hits */

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = &oout;
            iov[1].iov_len = sizeof(oout);
            if (writev(g_fuse_fd, iov, 2) < 0) {}
            continue;
        }

        if (opcode == FUSE_READ) {
            struct fuse_read_in rin;
            size_t r_avail = (size_t)n > sizeof(inh) ? (size_t)n - sizeof(inh) : 0;
            memset(&rin, 0, sizeof(rin));
            if (r_avail > sizeof(rin)) r_avail = sizeof(rin);
            if (r_avail > 0) memcpy(&rin, in_buf + sizeof(inh), r_avail);

            uint64_t offset = rin.offset;
            uint32_t req_size = rin.size;

            /* Defensive clamp */
            if (req_size > sizeof(out_buf) - sizeof(struct fuse_out_header)) {
                req_size = (uint32_t)(sizeof(out_buf) - sizeof(struct fuse_out_header));
            }

            if (offset >= (uint64_t)g_disk_size) {
                /* Clean EOF */
                struct fuse_out_header outh;
                memset(&outh, 0, sizeof(outh));
                outh.unique = unique;
                outh.len = (uint32_t)sizeof(outh);
                if (write(g_fuse_fd, &outh, sizeof(outh)) < 0) {}
                continue;
            }

            uint32_t to_read = req_size;
            if (offset + (uint64_t)to_read > (uint64_t)g_disk_size) {
                to_read = (uint32_t)((uint64_t)g_disk_size - offset);
            }

            static uint64_t total_read = 0;
            static uint64_t last_log_bytes = 0;
            total_read += (uint64_t)to_read;

            uint64_t deadline_ms = now_ms() + 2800;
            int sres = ntfs_serve_disk(out_buf, offset, (size_t)to_read, deadline_ms);
            if (sres < 0) {
                struct fuse_out_header outh;
                memset(&outh, 0, sizeof(outh));
                outh.unique = unique;
                outh.error = sres;
                outh.len = (uint32_t)sizeof(outh);
                if (write(g_fuse_fd, &outh, sizeof(outh)) < 0) {}
                continue;
            }

            if (total_read - last_log_bytes >= 2 * 1024 * 1024 || offset < 10000000ULL * BPS) {
                last_log_bytes = total_read;
                fprintf(stderr, "[FUSE_READ] total=%lluMB off=%llu sz=%u S_write=%lluMB base=%llu epoch=%llu\n",
                    (unsigned long long)(total_read / (1024*1024)),
                    (unsigned long long)offset, to_read,
                    (unsigned long long)(g_s_write / (1024*1024)),
                    (unsigned long long)g_base,
                    (unsigned long long)g_epoch);
            }

            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;
            outh.len = (uint32_t)(sizeof(outh) + (size_t)to_read);

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = out_buf;
            iov[1].iov_len = (size_t)to_read;
            if (writev(g_fuse_fd, iov, 2) < 0) {}
            continue;
        }

        if (opcode == FUSE_WRITE) {
            /* Absorb writes and return success */
            struct fuse_write_in win;
            size_t w_avail = (size_t)n > sizeof(inh) ? (size_t)n - sizeof(inh) : 0;
            memset(&win, 0, sizeof(win));
            if (w_avail > sizeof(win)) w_avail = sizeof(win);
            if (w_avail > 0) memcpy(&win, in_buf + sizeof(inh), w_avail);
            uint32_t written_size = win.size;

            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;
            outh.len = (uint32_t)(sizeof(outh) + sizeof(struct fuse_write_out));

            struct fuse_write_out wout;
            memset(&wout, 0, sizeof(wout));
            wout.size = written_size;

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = &wout;
            iov[1].iov_len = sizeof(wout);
            if (writev(g_fuse_fd, iov, 2) < 0) {}
            continue;
        }

        /* Default fallback: ENOSYS */
        struct fuse_out_header outh;
        memset(&outh, 0, sizeof(outh));
        outh.unique = unique;
        outh.error = -ENOSYS;
        outh.len = (uint32_t)sizeof(outh);
        if (write(g_fuse_fd, &outh, sizeof(outh)) < 0) {}
    }

    #undef in_buf
    #undef out_buf

    /* Clean shutdown of VOD prefetch worker */
    if (vod_prefetch_thread_started) {
        pthread_mutex_lock(&vod_mu);
        vod_prefetch_abort = 1;
        pthread_cond_broadcast(&vod_prefetch_cv);
        pthread_cond_broadcast(&vod_done_cv);
        pthread_mutex_unlock(&vod_mu);
        vod_bg_sock_shutdown();
        pthread_join(vod_prefetch_tid, NULL);
    }

    /* Free VOD head caches */
    for (int i = 0; i < g_num_files; i++) {
        if (g_files[i].head_cache) {
            free(g_files[i].head_cache);
            g_files[i].head_cache = NULL;
        }
    }

    if (g_meta_fd >= 0) close(g_meta_fd);
    close(g_fuse_fd);
    umount2(mnt, MNT_FORCE);
    return 0;
}
#endif /* TEST_SUITE */
