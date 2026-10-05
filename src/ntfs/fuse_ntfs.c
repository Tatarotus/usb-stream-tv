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
static volatile int g_running = 1;
static uint64_t g_prev_Fend = (uint64_t)-1;
static uint64_t g_last_file_read_ms = 0;
static uint64_t g_last_tv_stream_pos = 0;
static uint64_t g_anchor_foff = (uint64_t)-1;
static uint64_t g_anchor_stream_pos = 0;

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

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(80);
        inet_pton(AF_INET, "129.146.5.64", &sin.sin_addr);

        if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
            char req[256];
            int req_len = snprintf(req, sizeof(req),
                "GET /api/switch_channel?id=%s&pin=1233&no_reconnect=1 HTTP/1.1\r\n"
                "Host: tv.smre.run.place\r\n"
                "Connection: close\r\n\r\n", sa->id);
            if (req_len > 0) {
                (void)write(s, req, (size_t)req_len);
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
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&g_cv, &g_mu, &ts);
}

static void fill_null(uint8_t *dst, size_t n) {
    size_t off = 0, frag = n % 188;
    if (frag) { memset(dst, 0, frag); off = frag; }
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

static void __attribute__((unused)) randomize_volume_serial(void) {
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
        pwrite(g_meta_fd, g_current_volume_serial, 8, 0x48);
        if (g_total_sectors > 0) {
            off_t backup_vbr_off = (off_t)((g_total_sectors - 1) * 512 + 0x48);
            pwrite(g_meta_fd, g_current_volume_serial, 8, backup_vbr_off);
        }
    }
}


static void on_open(void) {
    pthread_mutex_lock(&g_mu);
    if (g_test_pattern_mode) {
        g_base_valid = 1;
        pthread_mutex_unlock(&g_mu);
        return;
    }

    /* Wait up to 5s for initial stream feeder data to reach MIN_STREAM_START */
    uint64_t t0 = now_ms();
    while (g_s_write < MIN_STREAM_START && now_ms() - t0 < 5000 && g_running) {
        wait_step();
    }

    g_anchor_foff = (uint64_t)-1;
    g_anchor_stream_pos = 0;
    g_base_valid = 0;
    g_prev_Fend = (uint64_t)-1;
    g_last_file_read_ms = now_ms();
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
    static uint64_t g_probe_seq_end = (uint64_t)-1;
    static int g_probe_consecutive_count = 0;

    int is_seq_read = (g_prev_Fend != (uint64_t)-1 &&
                       (foff == g_prev_Fend ||
                        (foff > g_prev_Fend && foff - g_prev_Fend <= 262144) ||
                        (foff < g_prev_Fend && g_prev_Fend - foff <= 262144)));

    /* Track consecutive reads at a new seek/jump location */
    if (!is_seq_read) {
        if (g_probe_seq_end != (uint64_t)-1 && foff == g_probe_seq_end) {
            g_probe_consecutive_count++;
            g_probe_seq_end = foff + c;
        } else {
            g_probe_consecutive_count = 1;
            g_probe_seq_end = foff + c;
        }
    } else {
        g_probe_consecutive_count = 0;
        g_probe_seq_end = (uint64_t)-1;
    }

    int is_confirmed_jump = (!is_seq_read && g_probe_consecutive_count >= 3);

    uint64_t s_probe = foff_to_stream_pos(foff);
    int is_future_probe = (s_probe >= g_s_write + 2ULL * 1024 * 1024 && !is_seq_read);
    int is_behind_probe = (s_probe < ring_old && !is_seq_read);

    if (g_base_valid && foff >= 8ULL * 1024 * 1024 && (is_future_probe || is_behind_probe) && !is_confirmed_jump) {
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
        /* Confirmed seek / bookmark jump (3+ consecutive blocks): re-anchor to live target */
        is_reopen = 1;
        g_probe_consecutive_count = 0;
        g_probe_seq_end = (uint64_t)-1;
    }

    if (is_reopen) {
        /* Pre-buffering protection: Ensure feeder has buffered at least MIN_STREAM_START */
        if (g_s_write < MIN_STREAM_START) {
            uint64_t wait_t0 = now_ms();
            while (g_s_write < MIN_STREAM_START && now_ms() - wait_t0 < 5000 && g_running) {
                wait_step();
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
                        pthread_mutex_unlock(&g_mu);
                        usleep((useconds_t)sleep_us);
                        pthread_mutex_lock(&g_mu);
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
                pthread_mutex_unlock(&g_mu);
                usleep((useconds_t)(pace_ms * 1000ULL));
                pthread_mutex_lock(&g_mu);
                fo += cc;
                continue;
            }
        }

        size_t avail = (size_t)(g_s_write - start);
        if (avail > cc) avail = cc;
        ring_copy(dst + fo, start, avail);
        fo += avail;
    }

    if (g_base_valid) {
        g_last_tv_stream_pos = foff_to_stream_pos(foff + c);
        g_prev_Fend = foff + c;
        g_probe_consecutive_count = 0;
        g_probe_seq_end = (uint64_t)-1;
    }
}

/* =========================================================================
 *  VOD HTTP RANGE CLIENT & PROCESSING/READY STATE MACHINE
 * ========================================================================= */
static int http_connect(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &sin.sin_addr) <= 0) {
        /* Fallback to hardcoded Oracle VPS IP */
        inet_pton(AF_INET, "129.146.5.64", &sin.sin_addr);
    }

    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static ssize_t http_fetch_range(struct virtual_file *vf, uint64_t start, size_t len, uint8_t *dst) {
    if (start >= vf->file_size) return 0;
    if (start + (uint64_t)len > vf->file_size) {
        len = (size_t)(vf->file_size - start);
    }

    uint64_t end = start + (uint64_t)len - 1;

    for (int retry = 0; retry < 2; retry++) {
        if (vf->http_sock < 0) {
            vf->http_sock = http_connect(vf->http_host, vf->http_port);
            if (vf->http_sock < 0) {
                fprintf(stderr, "[HTTP_RANGE_ERR] connect failed to %s:%d\n", vf->http_host, vf->http_port);
                return -1;
            }
        }

        char req[512];
        int req_len;
        if (vf->http_port == 80) {
            req_len = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Range: bytes=%" PRIu64 "-%" PRIu64 "\r\n"
                "Connection: keep-alive\r\n"
                "User-Agent: USBStreamTV-VOD/2.0\r\n\r\n",
                vf->http_path, vf->http_host, start, end);
        } else {
            req_len = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Range: bytes=%" PRIu64 "-%" PRIu64 "\r\n"
                "Connection: keep-alive\r\n"
                "User-Agent: USBStreamTV-VOD/2.0\r\n\r\n",
                vf->http_path, vf->http_host, vf->http_port, start, end);
        }

        if (write(vf->http_sock, req, (size_t)req_len) != req_len) {
            fprintf(stderr, "[HTTP_RANGE_ERR] write failed for %s\n", vf->http_path);
            close(vf->http_sock);
            vf->http_sock = -1;
            continue;
        }

        char hdr_buf[1024];
        size_t hdr_bytes = 0;
        int hdr_done = 0;
        while (hdr_bytes < sizeof(hdr_buf) - 1) {
            char c;
            ssize_t r = read(vf->http_sock, &c, 1);
            if (r <= 0) break;
            hdr_buf[hdr_bytes++] = c;
            hdr_buf[hdr_bytes] = '\0';
            if (hdr_bytes >= 4 && memcmp(hdr_buf + hdr_bytes - 4, "\r\n\r\n", 4) == 0) {
                hdr_done = 1;
                break;
            }
        }

        int status_code = 0;
        if (sscanf(hdr_buf, "HTTP/1.%*d %d", &status_code) != 1) {
            status_code = -1;
        }

        if (!hdr_done || (status_code != 206 && status_code != 200)) {
            fprintf(stderr, "[HTTP_RANGE_ERR] bad status=%d or header incomplete (hdr_bytes=%zu, retry=%d)\n",
                    status_code, hdr_bytes, retry);
            close(vf->http_sock);
            vf->http_sock = -1;
            continue;
        }

        size_t read_bytes = 0;
        while (read_bytes < len) {
            ssize_t r = read(vf->http_sock, dst + read_bytes, len - read_bytes);
            if (r <= 0) break;
            read_bytes += (size_t)r;
        }

        fprintf(stderr, "[HTTP_RANGE] %s Range=bytes=%" PRIu64 "-%" PRIu64 " (%zu B) -> HTTP %d (got %zu B)\n",
                vf->http_path, start, end, len, status_code, read_bytes);

        if (read_bytes == len) {
            return (ssize_t)read_bytes;
        }

        fprintf(stderr, "[HTTP_RANGE_ERR] short read %zu < %zu\n", read_bytes, len);
        close(vf->http_sock);
        vf->http_sock = -1;
    }
    return -1;
}

static void serve_vod_backend(struct virtual_file *vf, uint8_t *dst, uint64_t foff, size_t c, uint64_t deadline_ms) {
    if (foff >= vf->file_size) {
        memset(dst, 0, c);
        return;
    }

    /* PROCESSING state: Frontier pacing */
    if (vf->state == VOD_STATE_PROCESSING) {
        pthread_mutex_lock(&vf->vf_mu);
        while (foff >= vf->bytes_available && now_ms() < deadline_ms && g_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000; /* 50 ms step */
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&vf->vf_cv, &vf->vf_mu, &ts);
        }
        if (foff >= vf->bytes_available) {
            /* Frontier still not reached; fill zero without returning false EOF */
            memset(dst, 0, c);
            pthread_mutex_unlock(&vf->vf_mu);
            return;
        }
        pthread_mutex_unlock(&vf->vf_mu);
    }

    if (vf->src_type == SRC_TEST_PATTERN) {
        for (size_t i = 0; i < c; i++) {
            dst[i] = (uint8_t)((foff + (uint64_t)i) & 0xFFULL);
        }
        return;
    }

    /* READY state or within PROCESSING frontier: HTTP Range fetch */
    if (vf->src_type == SRC_VOD_HTTP_RANGE) {
        pthread_mutex_lock(&vf->vf_mu);
        ssize_t rd = http_fetch_range(vf, foff, c, dst);
        pthread_mutex_unlock(&vf->vf_mu);
        if (rd < 0) {
            memset(dst, 0, c);
        }
    } else {
        memset(dst, 0, c);
    }
}

/* =========================================================================
 *  MASTER MULTI-FILE DISK SERVING FUNCTION
 * ========================================================================= */
void ntfs_serve_disk(uint8_t *dst, uint64_t disk_offset, size_t n, uint64_t deadline_ms) {
    size_t done = 0;
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
            } else if (foff + c >= vf->file_size - 131072 || (foff < 131072 && vf->tel_max_foff > 1048576)) {
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
                    uint64_t now_sw = now_ms();
                    if (now_sw - g_last_channel_switch_ms > 800) {
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
                        pthread_cond_broadcast(&g_cv);
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

                fprintf(stderr, "[LIVE_TEL] #%" PRIu64 " t=%" PRIu64 ".%03" PRIu64 "ms LBA=%" PRIu64 " foff=%" PRIu64 " sz=%zu dt=%" PRIu64 "ms rate=%.2fMB/s lead_bytes=%" PRId64 " lead_sec=%.2fs epoch=%" PRIu64 " base=%" PRIu64 " S_write=%" PRIu64 " max_foff=%" PRIu64 " near_starve=%" PRIu64 " nulls=%" PRIu64 " type=%s file=%s\n",
                        vf->tel_count, t_sec, t_ms, sec, foff, c, dt, rate, lead_bytes, lead_sec,
                        g_epoch, g_anchor_stream_pos, g_s_write, vf->tel_max_foff, g_live_starve_near_count, g_live_fill_null_count, type_str, vf->path);

                pthread_mutex_unlock(&g_mu);
            } else {
                fprintf(stderr, "[VOD_TEL] #%" PRIu64 " t=%" PRIu64 ".%03" PRIu64 "ms LBA=%" PRIu64 " foff=%" PRIu64 " sz=%zu dt=%" PRIu64 "ms rate=%.2fMB/s max_foff=%" PRIu64 " type=%s file=%s\n",
                        vf->tel_count, t_sec, t_ms, sec, foff, c, dt, rate, vf->tel_max_foff, type_str, vf->path);

                pthread_rwlock_unlock(&g_catalog_rwlock);
                serve_vod_backend(vf, dst + done, foff, c, deadline_ms);
            }
            done += c;
        } else {
            pthread_rwlock_unlock(&g_catalog_rwlock);
            /* Sector is NTFS filesystem metadata or unallocated sector */
            size_t c = (size_t)BPS - sec_off;
            if (c > n - done) c = n - done;

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
                /* Primary VBR at sector 0, offset 0x48 */
                if (cur_disk <= 0x48 && cur_disk + (uint64_t)c > 0x48) {
                    size_t rel = (size_t)(0x48 - cur_disk);
                    size_t copy_len = c - rel;
                    if (copy_len > 8) copy_len = 8;
                    memcpy(dst + done + rel, g_current_volume_serial, copy_len);
                }
                /* Backup VBR at last sector */
                if (g_total_sectors > 0) {
                    uint64_t b_off = (g_total_sectors - 1) * BPS + 0x48;
                    if (cur_disk <= b_off && cur_disk + (uint64_t)c > b_off) {
                        size_t rel = (size_t)(b_off - cur_disk);
                        size_t copy_len = c - rel;
                        if (copy_len > 8) copy_len = 8;
                        memcpy(dst + done + rel, g_current_volume_serial, copy_len);
                    }
                }
            }
            done += c;
        }
    }
}

static volatile int g_flush_requested = 0;
static void handle_sigusr1(int s) {
    (void)s;
    g_flush_requested = 1;
}

/* Feeder thread for live mode */
static void *feeder_thread(void *arg) {
    const char *fifo_path = (const char *)arg;
    uint8_t buf[65536];

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
                while (read(fd, buf, sizeof(buf)) > 0) {}
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
                pthread_cond_broadcast(&g_cv);
                pthread_mutex_unlock(&g_mu);

                /* Randomize volume serial so TV sees a new filesystem */
                randomize_volume_serial();

                continue;
            }


            ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;

            pthread_mutex_lock(&g_mu);
            uint64_t at = g_s_write;
            ring_write(buf, (size_t)n);
            hcache_feed(buf, (size_t)n, at);
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
    g_running = 0;
    pthread_cond_broadcast(&g_cv);
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
    vf->num_extents = 3;
    for (int i = 0; i < 3; i++) {
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

#if 0


    /* Entry 2: FILMES/Matrix.mp4 (25 MB, Inode 71) */
    memset(&g_files[2], 0, sizeof(struct virtual_file));
    g_files[2].file_id = 2;
    strncpy(g_files[2].id, "vod_matrix", sizeof(g_files[2].id) - 1);
    strncpy(g_files[2].category, "FILMES", sizeof(g_files[2].category) - 1);
    strncpy(g_files[2].path, "FILMES/Matrix.mp4", sizeof(g_files[2].path) - 1);
    g_files[2].file_size = 26214400ULL;
    g_files[2].src_type = SRC_TEST_PATTERN;
    g_files[2].state = VOD_STATE_READY;
    g_files[2].bytes_available = 26214400ULL;
    g_files[2].num_extents = 1;
    g_files[2].extents[0].id = 0;
    g_files[2].extents[0].vcn_start = 0;
    g_files[2].extents[0].vcn_end = 6399;
    g_files[2].extents[0].lcn_start = 147704;
    g_files[2].extents[0].lcn_end = 154103;
    g_files[2].extents[0].file_start = 0;
    g_files[2].extents[0].file_end = 26214400ULL;
    g_files[2].extents[0].lba_start = 1181632ULL;
    g_files[2].extents[0].lba_end = 1232831ULL;
    g_files[2].extents[0].num_clusters = 6400;
    strncpy(g_files[2].http_host, "127.0.0.1", sizeof(g_files[2].http_host) - 1);
    g_files[2].http_port = 8089;
    strncpy(g_files[2].http_path, "/vod/matrix.mp4", sizeof(g_files[2].http_path) - 1);
    g_files[2].active = 1;
    g_files[2].http_sock = -1;
    pthread_mutex_init(&g_files[2].vf_mu, NULL);
    pthread_cond_init(&g_files[2].vf_cv, NULL);

    /* Entry 3: FILMES/Oppenheimer.mp4 (20 MB, Inode 72, PROCESSING) */
    memset(&g_files[3], 0, sizeof(struct virtual_file));
    g_files[3].file_id = 3;
    strncpy(g_files[3].id, "vod_oppenheimer", sizeof(g_files[3].id) - 1);
    strncpy(g_files[3].category, "FILMES", sizeof(g_files[3].category) - 1);
    strncpy(g_files[3].path, "FILMES/Oppenheimer.mp4", sizeof(g_files[3].path) - 1);
    g_files[3].file_size = 20971520ULL;
    g_files[3].src_type = SRC_TEST_PATTERN;
    g_files[3].state = VOD_STATE_PROCESSING;
    g_files[3].bytes_available = 4194304ULL; /* Starts at 4 MB */
    g_files[3].num_extents = 1;
    g_files[3].extents[0].id = 0;
    g_files[3].extents[0].vcn_start = 0;
    g_files[3].extents[0].vcn_end = 5119;
    g_files[3].extents[0].lcn_start = 154104;
    g_files[3].extents[0].lcn_end = 159223;
    g_files[3].extents[0].file_start = 0;
    g_files[3].extents[0].file_end = 20971520ULL;
    g_files[3].extents[0].lba_start = 1232832ULL;
    g_files[3].extents[0].lba_end = 1273791ULL;
    g_files[3].extents[0].num_clusters = 5120;
    strncpy(g_files[3].http_host, "127.0.0.1", sizeof(g_files[3].http_host) - 1);
    g_files[3].http_port = 8089;
    strncpy(g_files[3].http_path, "/vod/oppenheimer_processing.mp4", sizeof(g_files[3].http_path) - 1);
    g_files[3].active = 1;
    g_files[3].http_sock = -1;
    pthread_mutex_init(&g_files[3].vf_mu, NULL);
    pthread_cond_init(&g_files[3].vf_cv, NULL);

    /* Entry 4: SERIES/Breaking_Bad/S01E01.mp4 (15 MB, Inode 73) */
    memset(&g_files[4], 0, sizeof(struct virtual_file));
    g_files[4].file_id = 4;
    strncpy(g_files[4].id, "vod_bb_s01e01", sizeof(g_files[4].id) - 1);
    strncpy(g_files[4].category, "SERIES", sizeof(g_files[4].category) - 1);
    strncpy(g_files[4].path, "SERIES/Breaking_Bad/S01E01.mp4", sizeof(g_files[4].path) - 1);
    g_files[4].file_size = 15728640ULL;
    g_files[4].src_type = SRC_TEST_PATTERN;
    g_files[4].state = VOD_STATE_READY;
    g_files[4].bytes_available = 15728640ULL;
    g_files[4].num_extents = 1;
    g_files[4].extents[0].id = 0;
    g_files[4].extents[0].vcn_start = 0;
    g_files[4].extents[0].vcn_end = 3839;
    g_files[4].extents[0].lcn_start = 159224;
    g_files[4].extents[0].lcn_end = 163063;
    g_files[4].extents[0].file_start = 0;
    g_files[4].extents[0].file_end = 15728640ULL;
    g_files[4].extents[0].lba_start = 1273792ULL;
    g_files[4].extents[0].lba_end = 1304511ULL;
    g_files[4].extents[0].num_clusters = 3840;
    strncpy(g_files[4].http_host, "127.0.0.1", sizeof(g_files[4].http_host) - 1);
    g_files[4].http_port = 8089;
    strncpy(g_files[4].http_path, "/vod/breaking_bad_s01e01.mp4", sizeof(g_files[4].http_path) - 1);
    g_files[4].active = 1;
    g_files[4].http_sock = -1;
    pthread_mutex_init(&g_files[4].vf_mu, NULL);
    pthread_cond_init(&g_files[4].vf_cv, NULL);

    /* Entry 5: SERIES/Breaking_Bad/S01E02.mp4 (12 MB, Inode 74) */
    memset(&g_files[5], 0, sizeof(struct virtual_file));
    g_files[5].file_id = 5;
    strncpy(g_files[5].id, "vod_bb_s01e02", sizeof(g_files[5].id) - 1);
    strncpy(g_files[5].category, "SERIES", sizeof(g_files[5].category) - 1);
    strncpy(g_files[5].path, "SERIES/Breaking_Bad/S01E02.mp4", sizeof(g_files[5].path) - 1);
    g_files[5].file_size = 12582912ULL;
    g_files[5].src_type = SRC_TEST_PATTERN;
    g_files[5].state = VOD_STATE_READY;
    g_files[5].bytes_available = 12582912ULL;
    g_files[5].num_extents = 1;
    g_files[5].extents[0].id = 0;
    g_files[5].extents[0].vcn_start = 0;
    g_files[5].extents[0].vcn_end = 3071;
    g_files[5].extents[0].lcn_start = 163320;
    g_files[5].extents[0].lcn_end = 166391;
    g_files[5].extents[0].file_start = 0;
    g_files[5].extents[0].file_end = 12582912ULL;
    g_files[5].extents[0].lba_start = 1306560ULL;
    g_files[5].extents[0].lba_end = 1331135ULL;
    g_files[5].extents[0].num_clusters = 3072;
    strncpy(g_files[5].http_host, "127.0.0.1", sizeof(g_files[5].http_host) - 1);
    g_files[5].http_port = 8089;
    strncpy(g_files[5].http_path, "/vod/breaking_bad_s01e02.mp4", sizeof(g_files[5].http_path) - 1);
    g_files[5].active = 1;
    g_files[5].http_sock = -1;
    pthread_mutex_init(&g_files[5].vf_mu, NULL);
    pthread_cond_init(&g_files[5].vf_cv, NULL);

    /* Entry 6: ANIMES/Death_Note/S01E01.mp4 (8 MB, Inode 75) */
    memset(&g_files[6], 0, sizeof(struct virtual_file));
    g_files[6].file_id = 6;
    strncpy(g_files[6].id, "vod_dn_s01e01", sizeof(g_files[6].id) - 1);
    strncpy(g_files[6].category, "ANIMES", sizeof(g_files[6].category) - 1);
    strncpy(g_files[6].path, "ANIMES/Death_Note/S01E01.mp4", sizeof(g_files[6].path) - 1);
    g_files[6].file_size = 8388608ULL;
    g_files[6].src_type = SRC_TEST_PATTERN;
    g_files[6].state = VOD_STATE_READY;
    g_files[6].bytes_available = 8388608ULL;
    g_files[6].num_extents = 1;
    g_files[6].extents[0].id = 0;
    g_files[6].extents[0].vcn_start = 0;
    g_files[6].extents[0].vcn_end = 2047;
    g_files[6].extents[0].lcn_start = 167416;
    g_files[6].extents[0].lcn_end = 169463;
    g_files[6].extents[0].file_start = 0;
    g_files[6].extents[0].file_end = 8388608ULL;
    g_files[6].extents[0].lba_start = 1339328ULL;
    g_files[6].extents[0].lba_end = 1355711ULL;
    g_files[6].extents[0].num_clusters = 2048;
    strncpy(g_files[6].http_host, "127.0.0.1", sizeof(g_files[6].http_host) - 1);
    g_files[6].http_port = 8089;
    strncpy(g_files[6].http_path, "/vod/death_note_s01e01.mp4", sizeof(g_files[6].http_path) - 1);
    g_files[6].active = 1;
    g_files[6].http_sock = -1;
    pthread_mutex_init(&g_files[6].vf_mu, NULL);
    pthread_cond_init(&g_files[6].vf_cv, NULL);

    /* Entry 7: ANIMES/Death_Note/S01E02.mp4 (7 MB, Inode 76) */
    memset(&g_files[7], 0, sizeof(struct virtual_file));
    g_files[7].file_id = 7;
    strncpy(g_files[7].id, "vod_dn_s01e02", sizeof(g_files[7].id) - 1);
    strncpy(g_files[7].category, "ANIMES", sizeof(g_files[7].category) - 1);
    strncpy(g_files[7].path, "ANIMES/Death_Note/S01E02.mp4", sizeof(g_files[7].path) - 1);
    g_files[7].file_size = 7340032ULL;
    g_files[7].src_type = SRC_TEST_PATTERN;
    g_files[7].state = VOD_STATE_READY;
    g_files[7].bytes_available = 7340032ULL;
    g_files[7].num_extents = 1;
    g_files[7].extents[0].id = 0;
    g_files[7].extents[0].vcn_start = 0;
    g_files[7].extents[0].vcn_end = 1791;
    g_files[7].extents[0].lcn_start = 171512;
    g_files[7].extents[0].lcn_end = 173303;
    g_files[7].extents[0].file_start = 0;
    g_files[7].extents[0].file_end = 7340032ULL;
    g_files[7].extents[0].lba_start = 1372096ULL;
    g_files[7].extents[0].lba_end = 1386431ULL;
    g_files[7].extents[0].num_clusters = 1792;
    strncpy(g_files[7].http_host, "127.0.0.1", sizeof(g_files[7].http_host) - 1);
    g_files[7].http_port = 8089;
    strncpy(g_files[7].http_path, "/vod/death_note_s01e02.mp4", sizeof(g_files[7].http_path) - 1);
    g_files[7].active = 1;
    g_files[7].http_sock = -1;
    pthread_mutex_init(&g_files[7].vf_mu, NULL);
    pthread_cond_init(&g_files[7].vf_cv, NULL);

    pthread_rwlock_unlock(&g_catalog_rwlock);
}
#endif

static void detect_vbr_geometry(int meta_fd) {
    if (meta_fd < 0) return;
    uint8_t vbr[512];
    if (pread(meta_fd, vbr, 512, 0) == 512) {
        if (vbr[0] == 0xEB && vbr[2] == 0x90 && memcmp(vbr + 3, "NTFS    ", 8) == 0) {
            uint64_t tot_sec = 0;
            memcpy(&tot_sec, vbr + 0x28, 8);
            if (tot_sec > 0) {
                g_total_sectors = tot_sec;
                g_disk_size = tot_sec * BPS;
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

#ifndef TEST_SUITE
int main(int argc, char **argv) {
    setlinebuf(stdout);
    setlinebuf(stderr);

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGUSR1, handle_sigusr1);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mountpoint> [fifo_path] [metadata_img] [--multi]\n", argv[0]);
        fprintf(stderr, "       Pass fifo_path='test' for deterministic lab testing.\n");
        fprintf(stderr, "       Pass --multi to load the multi-file VOD hierarchy.\n");
        return 1;
    }

    const char *mnt = argv[1];
    const char *fifo = (argc > 2) ? argv[2] : "test";
    const char *meta = (argc > 3) ? argv[3] : NULL;
    int multi_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi") == 0) {
            multi_mode = 1;
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

    if (strcmp(fifo, "test") == 0) {
        printf("[*] Running in TEST PATTERN mode\n");
        init_test_pattern();
    } else {
        pthread_t th;
        pthread_create(&th, NULL, feeder_thread, (void *)fifo);
    }

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

    /* FUSE main event loop */
    uint8_t in_buf[131072 + 4096];
    uint8_t out_buf[131072 + 4096];

    while (g_running) {
        ssize_t n = read(g_fuse_fd, in_buf, sizeof(in_buf));
        if (n < (ssize_t)sizeof(struct fuse_in_header)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }

        const struct fuse_in_header *inh = (const struct fuse_in_header *)(const void *)in_buf;
        uint32_t opcode = inh->opcode;
        uint64_t unique = inh->unique;

        if (opcode == FUSE_INIT) {
            const struct fuse_init_in *ii = (const struct fuse_init_in *)(const void *)(in_buf + sizeof(*inh));
            struct fuse_out_header outh;
            memset(&outh, 0, sizeof(outh));
            outh.unique = unique;

            struct fuse_init_out init_out;
            memset(&init_out, 0, sizeof(init_out));
            init_out.major = 7;
            init_out.minor = (ii->minor < 26) ? ii->minor : 26;
            init_out.max_readahead = 131072;
            init_out.flags = ii->flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES);
            init_out.max_write = 131072;

            size_t out_payload_len;
            if (ii->minor < 23) {
                /* FUSE_COMPAT_22_INIT_OUT_SIZE = 24 bytes for Linux <= 3.13 (Samsung SM-T110 kernel 3.4.5) */
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
            writev(g_fuse_fd, iov, 2);
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
            att.attr.ino = inh->nodeid;
            att.attr.size = (inh->nodeid == 1) ? 4096ULL : (uint64_t)g_disk_size;
            att.attr.blocks = (inh->nodeid == 1) ? 8ULL : (uint64_t)g_total_sectors;
            att.attr.mode = (inh->nodeid == 1) ? (S_IFDIR | 0755U) : (S_IFREG | 0644U);
            att.attr.nlink = 1;

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = &att;
            iov[1].iov_len = sizeof(att);
            writev(g_fuse_fd, iov, 2);
            continue;
        }

        if (opcode == FUSE_LOOKUP) {
            const char *name = (const char *)(in_buf + sizeof(*inh));
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
                writev(g_fuse_fd, iov, 2);
                continue;
            }
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
            oout.open_flags = FOPEN_KEEP_CACHE;

            struct iovec iov[2];
            iov[0].iov_base = &outh;
            iov[0].iov_len = sizeof(outh);
            iov[1].iov_base = &oout;
            iov[1].iov_len = sizeof(oout);
            writev(g_fuse_fd, iov, 2);
            continue;
        }

        if (opcode == FUSE_READ) {
            const struct fuse_read_in *rin = (const struct fuse_read_in *)(const void *)(in_buf + sizeof(*inh));
            uint64_t offset = rin->offset;
            uint32_t req_size = rin->size;

            if (offset >= (uint64_t)g_disk_size) {
                /* Clean EOF */
                struct fuse_out_header outh;
                memset(&outh, 0, sizeof(outh));
                outh.unique = unique;
                outh.len = (uint32_t)sizeof(outh);
                write(g_fuse_fd, &outh, sizeof(outh));
                continue;
            }

            uint32_t to_read = req_size;
            if (offset + (uint64_t)to_read > (uint64_t)g_disk_size) {
                to_read = (uint32_t)((uint64_t)g_disk_size - offset);
            }

            static uint64_t total_read = 0;
            static uint64_t last_log_bytes = 0;
            total_read += (uint64_t)to_read;

            uint64_t deadline_ms = now_ms() + 1500;
            ntfs_serve_disk(out_buf, offset, (size_t)to_read, deadline_ms);

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
            writev(g_fuse_fd, iov, 2);
            continue;
        }

        if (opcode == FUSE_WRITE) {
            /* Absorb writes and return success */
            const struct fuse_write_in *win = (const struct fuse_write_in *)(const void *)(in_buf + sizeof(*inh));
            uint32_t written_size = win->size;

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
            writev(g_fuse_fd, iov, 2);
            continue;
        }

        /* Default fallback: ENOSYS */
        struct fuse_out_header outh;
        memset(&outh, 0, sizeof(outh));
        outh.unique = unique;
        outh.error = -ENOSYS;
        outh.len = (uint32_t)sizeof(outh);
        write(g_fuse_fd, &outh, sizeof(outh));
    }

    if (g_meta_fd >= 0) close(g_meta_fd);
    close(g_fuse_fd);
    umount2(mnt, MNT_FORCE);
    return 0;
}
#endif /* TEST_SUITE */
