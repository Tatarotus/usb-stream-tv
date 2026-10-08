/* fuse_direct_v2.c — FUSE-served infinite live disk & cloud VOD for USB Stream TV.
 *
 * Supports three modes:
 *   1. MODE_LIVE_FIFO (default): Infinite live stream fed by FIFO into 32MB RAM ring
 *   2. MODE_VOD_HTTP (Cloud VOD v2): Virtual remote disk via Dual-Cache & Prefetch
 *      - VOD Head Pinning Cache (8MB RAM): Pinned bytes 0..8MB for instant headers & seek-to-zero (0ms latency)
 *      - VOD Media Sliding Cache (16MB RAM): 4MB chunk streaming with lookahead prefetch background thread
 *      - Buffered HTTP Parser: Bulk recv() header/body parser replacing 1-byte read loops
 *      - Persistent TCP Socket with TCP_NODELAY & auto-reconnect
 *   3. MODE_VOD_LOCAL: Direct pread from local static MP4/TS file
 *
 * Presents ONE file (whole virtual FAT32 disk, ~4.0GB virtual) to the USB
 * mass-storage gadget. FAT/boot/dir regions come from a static template or
 * are synthesized arithmetically; file-data sectors stream live from RAM or HTTP.
 *
 * Layout (must match gen_template.py):
 *   reserved=32, fats=2, spf=8192, root cluster 2, file cluster 3, spc=8.
 * Build (ARM32 / Tablet): arm-linux-gnueabihf-gcc -static -O2 -Wall -lpthread fuse_direct_v2.c -o fuse_direct_arm32_v2
 * Build (ARM64 / Phone):  aarch64-linux-gnu-gcc -static -O2 -Wall -lpthread fuse_direct_v2.c -o fuse_direct_arm64_v2
 * Usage: fuse_direct_v2 <mnt> <fifo_or_url_or_file> <template>
 */
#define _GNU_SOURCE
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
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dirent.h>
#include <poll.h>
#include <linux/fuse.h>

/* ---- geometry ---- */
#define BPS 512ULL
#define SPC 8ULL
#define RSV 32ULL
#define NFATS 2ULL
#define SPF 8192ULL
#define FILECLUS 3ULL
#define DEFAULT_FILE_SIZE 1800000000ULL  /* 1.8GB default, updated dynamically from template */
#define DATA_SEC (RSV + NFATS * SPF)     /* 16416: cluster 2 */
#define FILE_SEC (DATA_SEC + SPC)        /* 16424: cluster 3 */
#define TOTCLUS 1048576ULL               /* 4.00GB virtual disk, matches template total_sec */
#define TOTSEC (DATA_SEC + (TOTCLUS - 2ULL) * SPC)
#define DISK_SIZE (TOTSEC * BPS)

/* Dynamic geometry */
static uint64_t g_file_size = DEFAULT_FILE_SIZE;
static uint64_t g_nfileclus = ((DEFAULT_FILE_SIZE + SPC * BPS - 1) / (SPC * BPS));
static uint64_t g_lastclus = FILECLUS + ((DEFAULT_FILE_SIZE + SPC * BPS - 1) / (SPC * BPS)) - 1;

/* ---- modes ---- */
enum StreamMode {
    MODE_LIVE_FIFO = 0,
    MODE_VOD_HTTP  = 1,
    MODE_VOD_LOCAL = 2
};
static enum StreamMode g_mode = MODE_LIVE_FIFO;
static int g_local_fd = -1;

/* ---- VOD Dual-Cache & Prefetch Architecture ---- */
#define VOD_HEAD_SZ  (8ULL * 1024 * 1024)   /* 8 MB VOD Head Pinning Cache */
#define VOD_MEDIA_SZ (16ULL * 1024 * 1024)  /* 16 MB Dedicated VOD Media Buffer */
#define VOD_CHUNK_SZ (4ULL * 1024 * 1024)   /* 4 MB Chunk Fetch Size */

/* 2. Dedicated VOD Media Sliding Cache & Prefetch Double Buffers */
static uint8_t vod_media_buf1[VOD_MEDIA_SZ] __attribute__((aligned(8)));
static uint8_t vod_media_buf2[VOD_MEDIA_SZ] __attribute__((aligned(8)));
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
static uint64_t vod_prefetch_target = (uint64_t)-1; /* offset being fetched */

static pthread_t vod_prefetch_tid;
static pthread_mutex_t vod_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vod_prefetch_cv = PTHREAD_COND_INITIALIZER; /* wakes prefetch thread */
static pthread_cond_t vod_done_cv = PTHREAD_COND_INITIALIZER;     /* wakes FUSE thread waiting for prefetch */
static int vod_prefetch_requested = 0;
static atomic_bool vod_prefetch_abort = false;                    /* thread-safe atomic abort flag */
static int vod_prefetch_thread_started = 0;

static int vod_fg_sock = -1; /* Foreground FUSE thread socket (head cache & sync misses) */
static int vod_bg_sock = -1; /* Background prefetch worker socket */
static pthread_mutex_t vod_bg_sock_mu = PTHREAD_MUTEX_INITIALIZER;

static void vod_bg_sock_set(int s) {
    pthread_mutex_lock(&vod_bg_sock_mu);
    vod_bg_sock = s;
    pthread_mutex_unlock(&vod_bg_sock_mu);
}

static void vod_bg_sock_close(void) {
    pthread_mutex_lock(&vod_bg_sock_mu);
    int s = vod_bg_sock;
    vod_bg_sock = -1;
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
    }
}

static struct sockaddr_in vod_server_addr;
static int vod_addr_resolved = 0;
static char g_vod_host[128] = "127.0.0.1";
static int g_vod_port = 80;
static char g_vod_path[256] = "/vod/movie.mp4";

/* ---- Multi-File VOD Architecture ---- */
#define MAX_VOD_FILES 16
#define PER_FILE_HEAD_SZ (2ULL * 1024 * 1024) /* 2 MB pinned header per file */

struct vod_file_entry {
    uint32_t start_clus;
    uint32_t end_clus;
    uint64_t size;
    char path[512];
    uint8_t *head_cache;
    size_t head_len;
    int head_fetched;
    uint64_t head_failed_until;
};

static struct vod_file_entry g_vod_files[MAX_VOD_FILES];
static int g_num_vod_files = 0;

static inline int find_vod_file_by_cluster(uint64_t clus, int *out_idx, uint64_t *out_foff) {
    if (g_num_vod_files <= 0) return -1;
    for (int i = 0; i < g_num_vod_files; i++) {
        if (clus >= g_vod_files[i].start_clus && clus <= g_vod_files[i].end_clus) {
            if (out_idx) *out_idx = i;
            if (out_foff) *out_foff = (clus - (uint64_t)g_vod_files[i].start_clus) * (SPC * BPS);
            return i;
        }
    }
    return -1;
}

/* ---- ring buffer for Live TV ---- */
#define RINGSZ (32ULL * 1024 * 1024)     /* ~90s @ 2.9Mbps */
#define HDRCACHESZ (512ULL * 1024)
#define LEADBACK (12ULL * 1024 * 1024)   /* TV starts ~40s behind live for network jitter immunity */
#define BLOCK_S 4                        /* max block per read: Samsung MStar SCSI command timeout is ~4-5s */

#define MNT_POINT "/data/local/tmp/vfat_mnt"
#define FILE_NAME "tv_stream.img"
#define FILE_INO 2
#define DEF_FIFO "/data/local/tmp/live_pipe"
#define DEF_TMPL "/data/local/tmp/fat_template.bin"

static uint8_t ring[RINGSZ] __attribute__((aligned(8)));
static uint64_t S_write = 0;             /* absolute stream bytes written */
static int have_data = 0;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;  /* CLOCK_REALTIME */

static uint8_t hcache[HDRCACHESZ] __attribute__((aligned(8)));
static uint64_t hcache_len = 0;          /* first stream bytes ever cached */

static uint64_t base = 0;                /* file off F -> stream base+F */
static int base_valid = 0;
static uint64_t prev_Fend = (uint64_t)-1; /* end of last READ (sequential) */
static int consec_small = 0;             /* sequential stale reads <2MB */
static uint64_t last_file_read_ms = 0;    /* timestamp of last file cluster read */
static uint64_t last_tv_stream_pos = 0;   /* absolute stream pos read by TV */

static const char *g_fifo = DEF_FIFO;
static volatile sig_atomic_t running = 1;
static int fuse_fd = -1;

/* template sectors: secno -> 512 bytes (boot, fsinfo, rootdir) */
#define MAXTMPL 32
static uint32_t tmpl_sec[MAXTMPL];
static uint8_t tmpl_dat[MAXTMPL][512];
static int ntmpl = 0;

static const uint8_t NULL_PKT[188] = { [0] = 0x47, [1] = 0x1F, [2] = 0xFF, [3] = 0x10,
    [4 ... 187] = 0xFF };

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wait_step(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&cv, &mu, &ts);
}

/* ---- template ---- */
static void load_template(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open template"); exit(1); }
    uint8_t rec[516];
    ssize_t n;
    g_num_vod_files = 0;
    while ((n = read(fd, rec, sizeof rec)) == (ssize_t)sizeof rec) {
        if (ntmpl >= MAXTMPL) { fprintf(stderr, "template too big\n"); exit(1); }
        uint32_t sec;
        memcpy(&sec, rec, 4);
        tmpl_sec[ntmpl] = sec;
        memcpy(tmpl_dat[ntmpl], rec + 4, 512);

        /* Inspect root directory (cluster 2 / DATA_SEC .. DATA_SEC + SPC - 1) to extract file(s) */
        if (sec >= DATA_SEC && sec < DATA_SEC + SPC) {
            for (int off = 0; off <= 512 - 32; off += 32) {
                uint8_t first_byte = rec[4 + off];
                if (first_byte == 0x00) break;
                if (first_byte == 0xE5) continue;
                uint8_t attr = rec[4 + off + 11];
                if (attr == 0x0F || (attr & 0x18) != 0) continue; /* Skip LFN, volume label, subdirs */

                uint16_t highclus = (uint16_t)rec[4 + off + 20] | ((uint16_t)rec[4 + off + 21] << 8);
                uint16_t lowclus  = (uint16_t)rec[4 + off + 26] | ((uint16_t)rec[4 + off + 27] << 8);
                uint32_t start_clus = ((uint32_t)highclus << 16) | (uint32_t)lowclus;
                uint32_t sz;
                memcpy(&sz, &rec[4 + off + 28], sizeof(uint32_t));

                if (start_clus >= 3 && sz > 0 && g_num_vod_files < MAX_VOD_FILES) {
                    uint64_t n_clus = ((uint64_t)sz + SPC * BPS - 1) / (SPC * BPS);
                    if ((uint64_t)start_clus + n_clus > TOTCLUS) {
                        fprintf(stderr, "[!] FAT overflow no arquivo %d: start %u + n_clus %llu > TOTCLUS %llu\n",
                                g_num_vod_files, start_clus, (unsigned long long)n_clus, TOTCLUS);
                        exit(1);
                    }
                    if ((uint64_t)start_clus + n_clus >= 0x0FFFFFF7ULL) {
                        fprintf(stderr, "[!] FAT32 cluster reservado atingido no arquivo %d\n", g_num_vod_files);
                        exit(1);
                    }
                    int fi = g_num_vod_files++;
                    g_vod_files[fi].start_clus = start_clus;
                    g_vod_files[fi].size = sz;
                    g_vod_files[fi].end_clus = start_clus + (n_clus > 0 ? (uint32_t)(n_clus - 1) : 0);
                    g_vod_files[fi].head_cache = NULL;
                    g_vod_files[fi].head_len = 0;
                    g_vod_files[fi].head_fetched = 0;
                    g_vod_files[fi].head_failed_until = 0;
                    g_vod_files[fi].path[0] = '\0';

                    printf("[*] Template detectou arquivo %d: clus %u..%u, %llu bytes (%.2f MB)\n",
                           fi, g_vod_files[fi].start_clus, g_vod_files[fi].end_clus,
                           (unsigned long long)g_vod_files[fi].size,
                           (double)g_vod_files[fi].size / (1024.0 * 1024.0));
                }
            }
        }

        ntmpl++;
    }
    close(fd);

    if (g_num_vod_files > 0) {
        g_file_size = g_vod_files[0].size;
        g_nfileclus = (g_file_size + SPC * BPS - 1) / (SPC * BPS);
        g_lastclus = g_vod_files[g_num_vod_files - 1].end_clus;
    }
    printf("[*] template: %d sectors loaded, %d files found\n", ntmpl, g_num_vod_files);
}

static void vod_setup_file_paths(void) {
    if (g_num_vod_files <= 0) return;
    char base_path[256];
    snprintf(base_path, sizeof(base_path), "%s", g_vod_path);
    char *last_slash = strrchr(base_path, '/');
    if (last_slash) *(last_slash + 1) = '\0';
    else snprintf(base_path, sizeof(base_path), "/vod/");

    for (int i = 0; i < g_num_vod_files; i++) {
        if (g_num_vod_files == 1) {
            snprintf(g_vod_files[0].path, sizeof(g_vod_files[0].path), "%s", g_vod_path);
        } else {
            snprintf(g_vod_files[i].path, sizeof(g_vod_files[i].path), "%sfile_%d.mp4", base_path, i);
        }
        printf("[*] VOD File %d: %s (size: %llu bytes)\n",
               i, g_vod_files[i].path, (unsigned long long)g_vod_files[i].size);
    }
}

static const uint8_t *tmpl_lookup(uint64_t sec) {
    for (int i = 0; i < ntmpl; i++)
        if (tmpl_sec[i] == sec) return tmpl_dat[i];
    return NULL;
}

/* ---- VOD HTTP Range Engine (Dual-Cache + Prefetch + Buffered Parser) ---- */

static int vod_resolve_server(void) {
    memset(&vod_server_addr, 0, sizeof(vod_server_addr));
    vod_server_addr.sin_family = AF_INET;
    vod_server_addr.sin_port = htons(g_vod_port);

    if (inet_pton(AF_INET, g_vod_host, &vod_server_addr.sin_addr) == 1) {
        vod_addr_resolved = 1;
        return 0;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", g_vod_port);

    int rc = getaddrinfo(g_vod_host, port_str, &hints, &res);
    if (rc == 0 && res) {
        if (res->ai_addrlen <= sizeof(vod_server_addr)) {
            memcpy(&vod_server_addr, res->ai_addr, res->ai_addrlen);
        } else {
            memcpy(&vod_server_addr, res->ai_addr, sizeof(vod_server_addr));
        }
        freeaddrinfo(res);
        vod_addr_resolved = 1;
        return 0;
    }
    if (res) freeaddrinfo(res);
    fprintf(stderr, "[VOD] Falha ao resolver host: %s\n", g_vod_host);
    vod_addr_resolved = 0;
    return -1;
}

static int vod_connect_sock(int *sock_ptr) {
    if (!sock_ptr) return -1;
    vod_close_sock(sock_ptr);

    if (!vod_addr_resolved) {
        if (vod_resolve_server() < 0) return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    /* Set non-blocking mode for connect() with 5s timeout */
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(s);
        return -1;
    }

    int res = connect(s, (struct sockaddr *)&vod_server_addr, sizeof(vod_server_addr));
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

    /* Restore blocking mode */
    if (fcntl(s, F_SETFL, flags) < 0) {
        close(s);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int keepalive = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    if (sock_ptr == &vod_bg_sock) {
        vod_bg_sock_set(s);
    } else {
        *sock_ptr = s;
    }
    return 0;
}

/* Fetch chunk [start_off .. start_off + chunk_sz - 1] via HTTP Range with Buffered Parser.
 * Uses isolated sock_ptr per thread. Reconnects with TCP_NODELAY if dropped.
 * Supports early abort via abort_flag and deadline enforcement via deadline_ms. */
static int vod_http_fetch_chunk(int *sock_ptr, const char *path, uint64_t file_size,
                                uint8_t *dst, uint64_t start_off, size_t chunk_sz,
                                size_t *out_len, atomic_bool *abort_flag,
                                uint64_t deadline_ms) {
    if (!sock_ptr || !path || file_size == 0 || start_off >= file_size || chunk_sz == 0) return -1;

    if (start_off + chunk_sz > file_size) {
        chunk_sz = (size_t)(file_size - start_off);
    }
    uint64_t fetch_end = start_off + chunk_sz - 1;

    for (int retry = 0; retry < 2; retry++) {
        if (now_ms() >= deadline_ms) {
            vod_close_sock(sock_ptr);
            return -1;
        }

        if (abort_flag && atomic_load_explicit(abort_flag, memory_order_acquire)) {
            vod_close_sock(sock_ptr);
            return -1;
        }

        if (*sock_ptr < 0) {
            if (vod_connect_sock(sock_ptr) < 0) {
                usleep(100000);
                continue;
            }
        }

        char req[512];
        int req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Range: bytes=%llu-%llu\r\n"
            "Connection: keep-alive\r\n"
            "User-Agent: USBStreamTV-VOD/2.0\r\n\r\n",
            path, g_vod_host,
            (unsigned long long)start_off,
            (unsigned long long)fetch_end);

        ssize_t w = send(*sock_ptr, req, req_len, MSG_NOSIGNAL);
        if (w != (ssize_t)req_len) {
            vod_close_sock(sock_ptr);
            continue;
        }

        /* 3. Buffered HTTP Parser: static thread-local buffer to avoid 80KB musl stack overflow */
        static __thread char hbuf[4096];
        size_t hlen = 0;
        char *hdr_end = NULL;

        while (hlen < sizeof(hbuf) - 1) {
            if (now_ms() >= deadline_ms) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            if (abort_flag && atomic_load_explicit(abort_flag, memory_order_acquire)) {
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
        *hdr_end = '\0'; /* Terminate headers safely for strcasestr / sscanf */

        int status_code = 0;
        if (sscanf(hbuf, "HTTP/1.%*d %d", &status_code) != 1) {
            fprintf(stderr, "[VOD] HTTP status invalido no cabecalho\n");
            vod_close_sock(sock_ptr);
            continue;
        }

        if (status_code == 301 || status_code == 302) {
            fprintf(stderr, "[VOD] HTTP redirect %d (re-resolvendo servidor)\n", status_code);
            vod_close_sock(sock_ptr);
            vod_addr_resolved = 0;
            return -EAGAIN;
        }

        if (status_code == 401 || status_code == 403) {
            fprintf(stderr, "[VOD] HTTP %d (acesso negado/token expirado)\n", status_code);
            vod_close_sock(sock_ptr);
            sleep(1);
            return -EACCES;
        }

        if (status_code != 200 && status_code != 206) {
            fprintf(stderr, "[VOD] HTTP status inesperado: %d\n", status_code);
            vod_close_sock(sock_ptr);
            continue;
        }

        /* Check for server ignoring Range request */
        if (start_off != 0 && status_code != 206) {
            fprintf(stderr, "[VOD] Upstream server ignored Range request for offset %llu (status %d != 206)\n",
                    (unsigned long long)start_off, status_code);
            vod_close_sock(sock_ptr);
            continue;
        }

        /* If 206, verify Content-Range start if header is present (robust parsing) */
        if (status_code == 206) {
            char *cr_str = strcasestr(hbuf, "Content-Range:");
            if (cr_str) {
                char *p = cr_str + 14;
                while (*p == ' ' || *p == ':') p++;
                if (strncasecmp(p, "bytes", 5) == 0) p += 5;
                while (*p == ' ' || *p == '=' || *p == ':') p++;
                char *endp = NULL;
                unsigned long long cr_start = strtoull(p, &endp, 10);
                if (endp != p && (uint64_t)cr_start != start_off) {
                    fprintf(stderr, "[VOD] Mismatched Content-Range start: got %llu, wanted %llu\n",
                            (unsigned long long)cr_start, (unsigned long long)start_off);
                    vod_close_sock(sock_ptr);
                    continue;
                }
            }
        }

        /* Reject chunked encoding on VOD streams without chunk framing */
        if (strcasestr(hbuf, "Transfer-Encoding: chunked") ||
            strcasestr(hbuf, "transfer-encoding: chunked")) {
            fprintf(stderr, "[VOD] Chunked transfer encoding not supported for VOD seek chunk\n");
            vod_close_sock(sock_ptr);
            continue;
        }

        /* Prevent BSS buffer overflow & detect desynchronization */
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

        if (status_code == 200 || strcasestr(hbuf, "Connection: close") || strcasestr(hbuf, "connection: close")) {
            close_after_read = 1;
        }

        /* Copy already-received body payload directly into destination buffer */
        size_t leftover_body = hlen - (size_t)(body_start - hbuf);
        if (leftover_body > expected_len) leftover_body = expected_len;
        if (leftover_body > 0) {
            memcpy(dst, body_start, leftover_body);
        }
        size_t total_body = leftover_body;

        /* Read remaining payload directly into destination buffer */
        while (total_body < expected_len) {
            if (now_ms() >= deadline_ms) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            if (abort_flag && atomic_load_explicit(abort_flag, memory_order_acquire)) {
                vod_close_sock(sock_ptr);
                return -1;
            }
            ssize_t rd = recv(*sock_ptr, dst + total_body, expected_len - total_body, 0);
            if (rd <= 0) break;
            total_body += (size_t)rd;
        }

        if (total_body < expected_len) {
            /* Error, EOF, or abort before expected_len received */
            vod_close_sock(sock_ptr);
            if (abort_flag && atomic_load_explicit(abort_flag, memory_order_acquire)) return -1;
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

static pthread_mutex_t vod_head_mu = PTHREAD_MUTEX_INITIALIZER;

/* 1. VOD Head Pinning Cache Initialization (per-file) */
static int vod_init_head_cache_file(int fi) {
    if (fi < 0 || fi >= g_num_vod_files) return -1;
    struct vod_file_entry *f = &g_vod_files[fi];
    if (f->head_fetched) return 0;
    if (f->size == 0 || f->path[0] == '\0') return 0;
    if (now_ms() < f->head_failed_until) return -1;

    pthread_mutex_lock(&vod_head_mu);
    if (f->head_fetched) {
        pthread_mutex_unlock(&vod_head_mu);
        return 0;
    }
    if (now_ms() < f->head_failed_until) {
        pthread_mutex_unlock(&vod_head_mu);
        return -1;
    }

    size_t max_head = (g_num_vod_files > 1) ? PER_FILE_HEAD_SZ : VOD_HEAD_SZ;
    size_t to_fetch = (f->size < max_head) ? (size_t)f->size : max_head;
    if (!f->head_cache) {
        f->head_cache = (uint8_t *)malloc(max_head);
        if (!f->head_cache) {
            fprintf(stderr, "[!] Falha de memoria para head_cache do arquivo %d\n", fi);
            pthread_mutex_unlock(&vod_head_mu);
            return -1;
        }
    }

    printf("[*] Preenchendo VOD Head Pinning Cache [file %d: %s]: 0 a %zu (%.2f MB)...\n",
           fi, f->path, to_fetch, (double)to_fetch / (1024.0 * 1024.0));
    size_t fetched = 0;
    atomic_bool abort_flag = false;
    int res = vod_http_fetch_chunk(&vod_fg_sock, f->path, f->size, f->head_cache, 0, to_fetch, &fetched, &abort_flag, now_ms() + 10000);
    if (res == 0 && fetched > 0) {
        f->head_len = fetched;
        f->head_fetched = 1;
        f->head_failed_until = 0;
        printf("[✓] VOD Head Pinning Cache pronto [file %d]: %zu bytes em RAM (0 ms latency, 0 rede)\n",
               fi, f->head_len);
        pthread_mutex_unlock(&vod_head_mu);
        return 0;
    }
    f->head_failed_until = now_ms() + 15000;
    fprintf(stderr, "[!] Falha ao preencher VOD Head Pinning Cache [file %d] (tentara novamente em 15s)\n", fi);
    pthread_mutex_unlock(&vod_head_mu);
    return -1;
}

static int vod_init_head_cache(void) {
    if (g_num_vod_files > 0) {
        return vod_init_head_cache_file(0);
    }
    return 0;
}

/* 2. Asynchronous Look-Ahead Prefetch Worker Thread */
static void *vod_prefetch_worker(void *arg) {
    (void)arg;
    vod_init_head_cache();
    while (running) {
        uint64_t target = (uint64_t)-1;
        int target_fi = -1;

        pthread_mutex_lock(&vod_mu);
        while (running && !vod_prefetch_requested) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&vod_prefetch_cv, &vod_mu, &ts);
        }
        if (!running) {
            pthread_mutex_unlock(&vod_mu);
            break;
        }

        target = vod_prefetch_target;
        target_fi = vod_prefetch_target_fi;
        vod_prefetch_requested = 0;
        vod_prefetch_ready = 0; /* Invalidate prefetch buffer while download is actively writing into it */
        vod_prefetch_fi = -1;
        vod_prefetch_start = (uint64_t)-1;
        vod_prefetch_len = 0;
        vod_prefetch_in_progress = 1;
        atomic_store_explicit(&vod_prefetch_abort, false, memory_order_release);
        pthread_mutex_unlock(&vod_mu);

        size_t fetched = 0;
        int res = -1;
        if (target_fi >= 0 && target_fi < g_num_vod_files) {
            res = vod_http_fetch_chunk(&vod_bg_sock, g_vod_files[target_fi].path, g_vod_files[target_fi].size,
                                      vod_prefetch_buf, target, VOD_CHUNK_SZ, &fetched, &vod_prefetch_abort, now_ms() + 15000);
        }

        pthread_mutex_lock(&vod_mu);
        vod_prefetch_in_progress = 0;
        if (res == 0 && !atomic_load_explicit(&vod_prefetch_abort, memory_order_acquire) && fetched > 0) {
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

/* Serves a single slice of VOD media data from Dual-Cache + Prefetch architecture */
static ssize_t serve_vod_slice_file(int fi, uint8_t *dst, uint64_t foff, size_t to_read, uint64_t deadline_ms) {
    if (fi < 0 || fi >= g_num_vod_files) return 0;
    struct vod_file_entry *f = &g_vod_files[fi];
    if (f->size == 0 || to_read == 0 || foff >= f->size) return 0;
    if (now_ms() >= deadline_ms) return -ETIMEDOUT;
    if (foff + to_read > f->size) to_read = (size_t)(f->size - foff);

    /* Ensure head cache is ready */
    if (!f->head_fetched) {
        vod_init_head_cache_file(fi);
    }

    /* 1. VOD Head Pinning Cache: Any read where foff < head_len must be served
     * immediately via memcpy from head_cache (0 ms latency, 0 network requests, 0 eviction) */
    if (foff < f->head_len && f->head_cache) {
        size_t avail = f->head_len - (size_t)foff;
        size_t take = (to_read < avail) ? to_read : avail;
        memcpy(dst, f->head_cache + foff, take);

        /* Head Cache Prefetch Bridging: trigger prefetch when near end of head cache */
        if (foff >= f->head_len * 3 / 4 && f->head_len < f->size) {
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

        /* Look-ahead prefetch trigger: when sequential playback reaches > 50% of the current chunk */
        if (foff >= vod_media_start + vod_media_len / 2) {
            uint64_t next_start = vod_media_start + vod_media_len;
            if (next_start < f->size) {
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

    /* Case B: HIT in vod_prefetch_buf (already finished downloading) -> Instant 0-wait SWAP */
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
            if (next_start < f->size &&
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

    /* Case C: Currently downloading in background for this exact offset -> wait on condvar */
    if (vod_prefetch_in_progress && vod_prefetch_target_fi == fi &&
        foff >= vod_prefetch_target && foff < vod_prefetch_target + VOD_CHUNK_SZ) {
        while (vod_prefetch_in_progress && !atomic_load_explicit(&vod_prefetch_abort, memory_order_acquire) && running) {
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
                if (next_start < f->size &&
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

    /* Case D: Cache Miss / Seek outside both buffers -> abort obsolete prefetch and sync-fetch */
    if (now_ms() >= deadline_ms) {
        pthread_mutex_unlock(&vod_mu);
        return -ETIMEDOUT;
    }

    if (vod_prefetch_in_progress) {
        pthread_mutex_unlock(&vod_mu);
        atomic_store_explicit(&vod_prefetch_abort, true, memory_order_release);
        vod_bg_sock_shutdown();
        pthread_mutex_lock(&vod_mu);
        while (vod_prefetch_in_progress && running && now_ms() < deadline_ms) {
            uint64_t current_time = now_ms();
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
        atomic_store_explicit(&vod_prefetch_abort, false, memory_order_release);
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

    /* Invalidate vod_media_cache before releasing lock during network I/O */
    vod_media_fi = -1;
    vod_media_start = (uint64_t)-1;
    vod_media_len = 0;

    pthread_mutex_unlock(&vod_mu);

    size_t fetched = 0;
    atomic_bool dummy_abort = false;
    int res = vod_http_fetch_chunk(&vod_fg_sock, f->path, f->size, vod_media_cache, foff, VOD_CHUNK_SZ, &fetched, &dummy_abort, deadline_ms);

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
            if (next_start < f->size) {
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

/* Serves arbitrary byte length of VOD media, chaining slices across head/media/chunks */
static int serve_vod_file(int fi, uint8_t *dst, uint64_t foff, size_t to_serve, uint64_t deadline_ms) {
    size_t served_total = 0;
    while (served_total < to_serve) {
        if (now_ms() >= deadline_ms) {
            return -ETIMEDOUT;
        }
        size_t chunk = to_serve - served_total;
        ssize_t served = serve_vod_slice_file(fi, dst + served_total, foff + served_total, chunk, deadline_ms);
        if (served < 0) {
            return (int)served;
        }
        if (served == 0) {
            /* EOF reached */
            memset(dst + served_total, 0, chunk);
            break;
        }
        served_total += (size_t)served;
    }
    return 0;
}

/* ---- ring ops (mu held, Live mode) ---- */
static void ring_write(const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t idx = (size_t)((S_write + off) % RINGSZ);
        size_t c = n - off;
        size_t room = RINGSZ - idx;
        if (c > room) c = room;
        memcpy(ring + idx, p + off, c);
        off += c;
    }
    S_write += n;
    have_data = 1;
}

static void hcache_feed(const uint8_t *p, size_t n, uint64_t abs_at) {
    if (abs_at >= HDRCACHESZ) return;
    size_t c = n;
    if ((uint64_t)c > HDRCACHESZ - abs_at) c = (size_t)(HDRCACHESZ - abs_at);
    memcpy(hcache + abs_at, p, c);
    if (abs_at + c > hcache_len) hcache_len = abs_at + c;
}

static void ring_copy(uint8_t *dst, uint64_t a, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t idx = (size_t)((a + off) % RINGSZ);
        size_t c = n - off;
        size_t room = RINGSZ - idx;
        if (c > room) c = room;
        memcpy(dst + off, ring + idx, c);
        off += c;
    }
}

/* ---- fifo feeder (Live mode) ---- */
static void *feeder(void *arg) {
    (void)arg;
    static uint8_t buf[65536];
    for (;;) {
        if (!running) return NULL;
        int fd = open(g_fifo, O_RDONLY);
        if (fd < 0) { sleep(1); continue; }
        printf("[*] fifo reader connected\n");
        for (;;) {
            /* Flow control: Prevent writer from overrunning 32MB ring buffer */
            while (running && have_data && base_valid && last_tv_stream_pos > 0) {
                pthread_mutex_lock(&mu);
                uint64_t lead = (S_write > last_tv_stream_pos) ? (S_write - last_tv_stream_pos) : 0;
                pthread_mutex_unlock(&mu);
                if (lead > 24ULL * 1024 * 1024) {
                    usleep(50000);
                } else {
                    break;
                }
            }

            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;
            pthread_mutex_lock(&mu);
            uint64_t at = S_write;
            ring_write(buf, (size_t)n);
            hcache_feed(buf, (size_t)n, at);
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            if (!running) break;
        }
        close(fd);
        printf("[!] fifo EOF, waiting for writer...\n");
    }
    return NULL;
}

/* ---- rebase helpers (Live mode) ---- */
static uint64_t snap188(uint64_t v) { return v - (v % 188ULL); }

static uint64_t snap_pat(uint64_t p) {
    uint64_t end = p + 65536;
    if (end > S_write) end = S_write;
    uint64_t q = snap188(p);
    for (; q + 188 <= end; q += 188) {
        uint8_t pkt[188];
        ring_copy(pkt, q, 188);
        if (pkt[0] == 0x47 && (pkt[1] & 0x1F) == 0 && pkt[2] == 0) return q;
    }
    return p;
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
        uint8_t af = (pkt[3] >> 4) & 3;
        size_t off = 4;
        if (af == 2 || af == 0) continue;
        if (af == 3) off += 1 + pkt[4];
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

static uint64_t snap_open_base(void) {
    uint64_t target = (S_write > LEADBACK) ? S_write - LEADBACK : 0;
    uint64_t window_start = (target > 65536) ? target - 65536 : 0;
    uint64_t ring_old = (S_write > RINGSZ) ? S_write - RINGSZ : 0;
    if (window_start < ring_old) window_start = ring_old;
    uint64_t p = snap188(target);
    while (p >= window_start) {
        uint8_t pkt[188];
        ring_copy(pkt, p, 188);
        if (pkt[0] == 0x47 && (pkt[1] & 0x1F) == 0 && pkt[2] == 0) {
            uint64_t scan_end = p + 65536;
            if (scan_end > S_write) scan_end = S_write;
            if (ring_has_nal(p, scan_end, 7)) return p;
        }
        if (p < 188) break;
        p -= 188;
    }
    return snap_pat(target);
}

static void do_rebase(uint64_t cur_foff) {
    uint64_t target = (S_write > LEADBACK) ? S_write - LEADBACK : 0;
    uint64_t ring_old = (S_write > RINGSZ) ? S_write - RINGSZ : 0;
    if (target < ring_old) target = ring_old;
    uint64_t new_base = snap_pat(target);
    if (new_base > cur_foff) new_base -= cur_foff;
    else new_base = 0;
    base = new_base;
    last_tv_stream_pos = base + cur_foff;
    base_valid = 1;
    prev_Fend = (uint64_t)-1;
    consec_small = 0;
}

static void on_open(void) {
    pthread_mutex_lock(&mu);
    if (g_mode == MODE_VOD_HTTP) {
        printf("[FUSE] on_open: modo VOD CLOUD (%d arquivos, principal: %llu bytes)\n",
               g_num_vod_files, (unsigned long long)g_file_size);
        base_valid = 1;
        for (int i = 0; i < g_num_vod_files; i++) g_vod_files[i].head_failed_until = 0;
        pthread_mutex_unlock(&mu);
        vod_init_head_cache();
        return;
    }
    if (g_mode == MODE_VOD_LOCAL) {
        printf("[FUSE] on_open: modo VOD LOCAL (tamanho %llu bytes)\n", (unsigned long long)g_file_size);
        base_valid = 1;
        pthread_mutex_unlock(&mu);
        return;
    }
    /* wait for enough stream to choose a decodable start (≤5s) */
    {
        uint64_t t0 = now_ms();
        while (S_write < 512 * 1024 && now_ms() - t0 < 5000 && running) wait_step();
    }
    if (have_data && S_write > 0) {
        base = snap_open_base();
        last_tv_stream_pos = base;
        size_t c_h = 65536;
        if (c_h > HDRCACHESZ) c_h = HDRCACHESZ;
        if (S_write > base) {
            size_t av = (size_t)(S_write - base);
            if (c_h > av) c_h = av;
            for (size_t i = 0; i < c_h; i++) {
                hcache[i] = ring[(size_t)((base + i) % RINGSZ)];
            }
            hcache_len = c_h;
        }
    } else {
        base = 0;
    }
    fprintf(stderr, "[FUSE] on_open: base=%llu S_write=%llu (lead: %.1fs)\n",
            (unsigned long long)base, (unsigned long long)S_write,
            (double)(S_write - base) / (305.0 * 1024.0));
    base_valid = 1;
    prev_Fend = (uint64_t)-1;
    consec_small = 0;
    last_file_read_ms = now_ms();
    pthread_mutex_unlock(&mu);
}

static void fill_null(uint8_t *dst, size_t n) {
    size_t off = 0, frag = n % 188;
    if (frag) { memset(dst, 0, frag); off = frag; }
    for (; off + 188 <= n; off += 188) memcpy(dst + off, NULL_PKT, 188);
}

/* serve absolute disk range [disk, disk+n); mu HELD by caller. */
static int serve_disk(uint8_t *dst, uint64_t disk, size_t n, uint64_t deadline_ms) {
    size_t done = 0;
    while (done < n) {
        uint64_t sec = (disk + done) / BPS;
        size_t sec_off = (size_t)((disk + done) % BPS);
        size_t c = BPS - sec_off;
        if (c > n - done) c = n - done;
        union {
            uint32_t words[128];
            uint8_t bytes[BPS];
        } sbuf __attribute__((aligned(8)));

        const uint8_t *t = tmpl_lookup(sec);
        if (t) {
            memcpy(sbuf.bytes, t, BPS);
            memcpy(dst + done, sbuf.bytes + sec_off, c);
            done += c;
            continue;
        }

        if (sec < FILE_SEC) {
            if (sec >= RSV && sec < RSV + NFATS * SPF) {
                /* synthesized FAT mirror: 128 entries/sector */
                uint64_t sno = sec - RSV;
                if (sno >= SPF) sno -= SPF;
                for (int k = 0; k < 128; k++) {
                    uint64_t cl = sno * 128 + (uint64_t)k;
                    uint32_t v;
                    if (cl < 2) v = (cl == 0) ? 0x0FFFFFF8 : 0x0FFFFFFF;
                    else if (cl == 2) v = 0x0FFFFFFF;
                    else if (g_num_vod_files > 0) {
                        v = 0;
                        for (int i = 0; i < g_num_vod_files; i++) {
                            if (cl >= g_vod_files[i].start_clus && cl <= g_vod_files[i].end_clus) {
                                if (cl == g_vod_files[i].end_clus) v = 0x0FFFFFFF;
                                else v = (uint32_t)(cl + 1);
                                break;
                            }
                        }
                    } else if (cl < g_lastclus) v = (uint32_t)(cl + 1);
                    else if (cl == g_lastclus) v = 0x0FFFFFFF;
                    else v = 0;
                    sbuf.words[k] = v;
                }
            } else {
                memset(sbuf.bytes, 0, BPS);
            }
            memcpy(dst + done, sbuf.bytes + sec_off, c);
            done += c;
            continue;
        }

        uint64_t clus = 2 + (sec - DATA_SEC) / SPC;

        /* ---- MODE_VOD_HTTP (Cloud VOD: Multi-File Dual-Cache + Prefetch) ---- */
        if (g_mode == MODE_VOD_HTTP) {
            int fi = -1;
            uint64_t foff_base = 0;
            fi = find_vod_file_by_cluster(clus, &fi, &foff_base);
            if (fi >= 0) {
                uint64_t foff = foff_base + (sec - (DATA_SEC + (clus - 2) * SPC)) * BPS + sec_off;
                uint64_t file_data_end = (DATA_SEC + (g_vod_files[fi].end_clus - 2 + 1) * SPC) * BPS;
                size_t max_file_span = n - done;
                if (disk + done + max_file_span > file_data_end) {
                    max_file_span = (size_t)(file_data_end - (disk + done));
                }
                if (foff < g_vod_files[fi].size) {
                    size_t to_read = max_file_span;
                    if (foff + to_read > g_vod_files[fi].size) {
                        to_read = (size_t)(g_vod_files[fi].size - foff);
                    }
                    int vret = serve_vod_file(fi, dst + done, foff, to_read, deadline_ms);
                    if (vret < 0) return vret;
                    if (max_file_span > to_read) {
                        memset(dst + done + to_read, 0, max_file_span - to_read);
                    }
                } else {
                    memset(dst + done, 0, max_file_span);
                }
                done += max_file_span;
                continue;
            } else {
                memset(dst + done, 0, c);
                done += c;
                continue;
            }
        }

        if (clus >= FILECLUS && clus < FILECLUS + g_nfileclus) {
            uint64_t foff = (clus - FILECLUS) * (SPC * BPS)
                          + (sec - (DATA_SEC + (clus - 2) * SPC)) * BPS + sec_off;

            /* ---- MODE_VOD_LOCAL ---- */
            if (g_mode == MODE_VOD_LOCAL) {
                if (foff < g_file_size && g_local_fd >= 0) {
                    size_t to_read = c;
                    if (foff + to_read > g_file_size) to_read = (size_t)(g_file_size - foff);
                    ssize_t rd = pread(g_local_fd, dst + done, to_read, (off_t)foff);
                    if (rd < (ssize_t)to_read) {
                        memset(dst + done + (rd > 0 ? rd : 0), 0, to_read - (rd > 0 ? rd : 0));
                    }
                    if (c > to_read) memset(dst + done + to_read, 0, c - to_read);
                } else {
                    memset(dst + done, 0, c);
                }
                done += c;
                continue;
            }

            /* ---- MODE_LIVE_FIFO ---- */
            uint64_t now = now_ms();
            uint64_t ring_old_chk = (S_write > RINGSZ) ? S_write - RINGSZ : 0;
            if (foff == 0 && (!base_valid || base < ring_old_chk || now - last_file_read_ms > 1500)) {
                if (have_data && S_write > 0) {
                    base = snap_open_base();
                    last_tv_stream_pos = base;
                    base_valid = 1;
                    prev_Fend = (uint64_t)-1;
                    consec_small = 0;
                    size_t c_h = 65536;
                    if (c_h > HDRCACHESZ) c_h = HDRCACHESZ;
                    if (S_write > base) {
                        size_t av = (size_t)(S_write - base);
                        if (c_h > av) c_h = av;
                        for (size_t i = 0; i < c_h; i++) {
                            hcache[i] = ring[(size_t)((base + i) % RINGSZ)];
                        }
                        hcache_len = c_h;
                    }
                    fprintf(stderr, "[FUSE] Fresh playback at foff=0! base=%llu S_write=%llu (lead: %.1fs)\n",
                            (unsigned long long)base, (unsigned long long)S_write,
                            (double)(S_write - base) / (305.0 * 1024.0));
                }
            }
            last_file_read_ms = now;

            size_t fo = 0;
            while (fo < c) {
                uint64_t F = foff + fo;
                size_t cc = c - fo;
                uint64_t ring_old = (S_write > RINGSZ) ? S_write - RINGSZ : 0;

                if (!have_data) {
                    while (!have_data && now_ms() < deadline_ms && running) wait_step();
                    if (!have_data) { fill_null(dst + done + fo, cc); fo += cc; continue; }
                    continue;
                }
                if (F < hcache_len && (!base_valid || base + F < ring_old)) {
                    size_t hc = hcache_len - (size_t)F;
                    if (hc > cc) hc = cc;
                    memcpy(dst + done + fo, hcache + F, hc);
                    fo += hc;
                    continue;
                }
                if (!base_valid) { fill_null(dst + done + fo, cc); fo += cc; continue; }
                uint64_t start = base + F;
                if (start < ring_old) {
                    uint64_t safe_pos = snap188(ring_old + (F % (RINGSZ / 2)));
                    if (safe_pos + cc > S_write) safe_pos = snap188(S_write > cc ? S_write - cc : 0);
                    ring_copy(dst + done + fo, safe_pos, cc);
                    fo += cc;
                    continue;
                }
                if (start >= S_write) {
                    if (start < S_write + 4 * 1024 * 1024) {
                        while (start >= S_write && now_ms() < deadline_ms && running)
                            wait_step();
                    }
                    if (start >= S_write) {
                        int sequential = (prev_Fend != (uint64_t)-1 && foff >= prev_Fend &&
                                          foff - prev_Fend < 2 * 1024 * 1024);
                        if (!sequential || start >= S_write + 1024 * 1024) {
                            uint64_t safe_pos = snap188(ring_old + (F % (RINGSZ / 2)));
                            if (safe_pos + cc > S_write) safe_pos = snap188(S_write > cc ? S_write - cc : 0);
                            ring_copy(dst + done + fo, safe_pos, cc);
                            fo += cc;
                            continue;
                        }
                        while (start >= S_write && now_ms() < deadline_ms && running)
                            wait_step();
                        if (start >= S_write) { fill_null(dst + done + fo, cc); fo += cc; continue; }
                        continue;
                    }
                }
                size_t avail = (size_t)(S_write - start);
                if (avail > cc) avail = cc;
                ring_copy(dst + done + fo, start, avail);
                fo += avail;
            }
            if (base_valid) {
                uint64_t s0 = base + foff;
                last_tv_stream_pos = s0;
                uint64_t ring_old2 = (S_write > RINGSZ) ? S_write - RINGSZ : 0;
                int stale = (s0 < ring_old2);
                int seq = (prev_Fend != (uint64_t)-1 && foff >= prev_Fend && foff - prev_Fend < 256 * 1024);
                if (stale && foff < 2 * 1024 * 1024) {
                    if (foff == 0) consec_small = 1;
                    else if (seq) consec_small++;
                    else consec_small = 0;
                    if (consec_small >= 3 && foff != 0) do_rebase(foff);
                } else if (stale) {
                    do_rebase(foff);
                } else {
                    consec_small = 0;
                }
                prev_Fend = foff + c;
            }
            done += c;
            continue;
        }
        memset(dst + done, 0, c);
        done += c;
    }
    return 0;
}

/* ---- FUSE ---- */
static void handle_sig(int s) {
    (void)s;
    running = 0;
}

static inline int fuse_reply(int fd, const void *buf, size_t len) {
    ssize_t w = write(fd, buf, len);
    if (w < 0) {
        if (errno == ENOENT || errno == EINTR) return 0;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    const char *mnt = MNT_POINT;
    if (argc > 1) mnt = argv[1];
    if (argc > 2) g_fifo = argv[2];
    if (argc > 3) load_template(argv[3]);
    else load_template(DEF_TMPL);

    /* Detect mode from target data source */
    if (strncasecmp(g_fifo, "https://", 8) == 0) {
        fprintf(stderr, "[!] Erro: URLs com https:// (TLS/SSL) nao sao suportadas nativamente pelo FUSE estatico.\n"
                        "    Use http:// ou um proxy HTTP reverso local (ex: caddy/socat/nginx).\n");
        return 1;
    }
    if (strncmp(g_fifo, "http://", 7) == 0) {
        g_mode = MODE_VOD_HTTP;
        const char *p = g_fifo + 7;
        const char *slash = strchr(p, '/');
        if (slash) {
            char hostport[128];
            size_t hplen = (size_t)(slash - p);
            if (hplen >= sizeof(hostport)) hplen = sizeof(hostport) - 1;
            memcpy(hostport, p, hplen);
            hostport[hplen] = '\0';
            char *colon = strchr(hostport, ':');
            if (colon) {
                *colon = '\0';
                g_vod_port = atoi(colon + 1);
            } else {
                g_vod_port = 80;
            }
            snprintf(g_vod_host, sizeof(g_vod_host), "%s", hostport);
            snprintf(g_vod_path, sizeof(g_vod_path), "%s", slash);
        }
        printf("[✓] FUSE v2 iniciado em MODO VOD CLOUD: host=%s port=%d path=%s\n",
               g_vod_host, g_vod_port, g_vod_path);
        if (vod_resolve_server() < 0) {
            fprintf(stderr, "[!] Erro ao resolver endereco do servidor VOD: %s\n", g_vod_host);
        }
        vod_setup_file_paths();
    } else if (access(g_fifo, F_OK) == 0 && (strstr(g_fifo, ".mp4") || strstr(g_fifo, ".ts")) && !strstr(g_fifo, "pipe")) {
        struct stat st;
        if (stat(g_fifo, &st) == 0 && S_ISREG(st.st_mode)) {
            g_mode = MODE_VOD_LOCAL;
            g_local_fd = open(g_fifo, O_RDONLY);
            printf("[✓] FUSE v2 iniciado em MODO VOD LOCAL: %s\n", g_fifo);
        }
    } else {
        g_mode = MODE_LIVE_FIFO;
        printf("[✓] FUSE v2 iniciado em MODO TV AO VIVO (FIFO: %s)\n", g_fifo);
    }

    mkdir(mnt, 0755);
    umount2(mnt, MNT_DETACH);

    fuse_fd = open("/dev/fuse", O_RDWR);
    if (fuse_fd < 0) { perror("open /dev/fuse"); return 1; }

    char opts[256];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=040755,user_id=0,group_id=0,allow_other", fuse_fd);
    if (mount("fuse", mnt, "fuse", 0, opts) < 0) { perror("mount"); return 2; }
    printf("[✓] fuse_direct_v2 montado em %s\n", mnt);

    /* Initialize condition variables with CLOCK_MONOTONIC */
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&vod_prefetch_cv, &cattr);
    pthread_cond_init(&vod_done_cv, &cattr);
    pthread_cond_init(&cv, &cattr);
    pthread_condattr_destroy(&cattr);

    /* Block SIGINT and SIGTERM before creating worker threads */
    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigmask, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* NO SA_RESTART: allow EINTR on blocking read(fuse_fd) */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (g_mode == MODE_LIVE_FIFO) {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        if (pthread_create(&th, &attr, feeder, NULL) != 0) {
            perror("thread feeder");
            pthread_attr_destroy(&attr);
            return 3;
        }
        pthread_attr_destroy(&attr);
    } else if (g_mode == MODE_VOD_HTTP) {
        /* Start background look-ahead prefetch thread with 1MB stack */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        if (pthread_create(&vod_prefetch_tid, &attr, vod_prefetch_worker, NULL) != 0) {
            perror("thread prefetch");
            pthread_attr_destroy(&attr);
            return 3;
        }
        pthread_attr_destroy(&attr);
        vod_prefetch_thread_started = 1;
        printf("[✓] Thread de prefetch assincrono inicializada com sucesso (stack 1MB)\n");
    }

    /* Unblock signals in main thread so SIGINT/SIGTERM is delivered strictly here */
    pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

    static union {
        uint64_t align;
        char bytes[128 * 1024 + 4096];
    } in_u, out_u;
    char *in_buf = in_u.bytes;
    char *out_buf = out_u.bytes;

    while (running) {
        ssize_t n = read(fuse_fd, in_buf, sizeof(in_u.bytes));
        if (n < 0) {
            if (errno == EINTR) {
                if (!running) break;
                continue;
            }
            if (errno != ENODEV) perror("fuse read error");
            break;
        }
        if (n < (ssize_t)sizeof(struct fuse_in_header)) continue;
        struct fuse_in_header *inh = (struct fuse_in_header *)(void *)in_buf;
        void *payload = in_buf + sizeof(struct fuse_in_header);

        /* Never reply to FUSE_FORGET or FUSE_BATCH_FORGET (kernel drops them, reply yields ENOENT) */
        if (inh->opcode == FUSE_FORGET || inh->opcode == 42 /* FUSE_BATCH_FORGET */) {
            continue;
        }

        if (inh->opcode == FUSE_INIT) {
            if (n < (ssize_t)(sizeof(struct fuse_in_header) + 8)) continue;
            struct fuse_init_in ii;
            memset(&ii, 0, sizeof(ii));
            size_t copy_len = (size_t)n - sizeof(struct fuse_in_header);
            if (copy_len > sizeof(ii)) copy_len = sizeof(ii);
            memcpy(&ii, payload, copy_len);
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            struct fuse_init_out *io = (struct fuse_init_out *)(void *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out));
            oh->error = 0; oh->unique = inh->unique;
            io->major = 7;
            io->minor = (ii.minor < 26) ? ii.minor : 26;
            io->max_readahead = 128 * 1024;
            io->flags = ii.flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES);
            io->max_write = 128 * 1024;
            if (ii.minor < 23) {
                /* FUSE_COMPAT_22_INIT_OUT_SIZE = 24 bytes for Linux <= 3.13 */
                oh->len = sizeof(struct fuse_out_header) + 24;
            } else {
                oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
            }
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) {
                perror("write FUSE_INIT reply");
                break;
            }
            continue;
        } else if (inh->opcode == FUSE_GETATTR) {
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            struct fuse_attr_out *ao = (struct fuse_attr_out *)(void *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out));
            oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
            oh->error = 0; oh->unique = inh->unique;
            ao->attr_valid = 10;
            ao->attr.ino = inh->nodeid;
            if (inh->nodeid == 1) {
                ao->attr.mode = S_IFDIR | 0755;
                ao->attr.nlink = 2;
                ao->attr.size = 4096;
            } else if (inh->nodeid == FILE_INO) {
                ao->attr.mode = S_IFREG | 0644;
                ao->attr.nlink = 1;
                ao->attr.size = DISK_SIZE;
            } else {
                oh->error = -ENOENT;
                oh->len = sizeof(struct fuse_out_header);
            }
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_LOOKUP) {
            char *name = payload;
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            if (strncmp(name, FILE_NAME, sizeof(FILE_NAME)) == 0) {
                struct fuse_entry_out *eo = (struct fuse_entry_out *)(void *)(out_buf + sizeof(struct fuse_out_header));
                memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out));
                oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
                oh->error = 0; oh->unique = inh->unique;
                eo->nodeid = FILE_INO;
                eo->generation = 1;
                eo->entry_valid = 10;
                eo->attr_valid = 10;
                eo->attr.ino = FILE_INO;
                eo->attr.mode = S_IFREG | 0644;
                eo->attr.nlink = 1;
                eo->attr.size = DISK_SIZE;
            } else {
                oh->len = sizeof(struct fuse_out_header);
                oh->error = -ENOENT;
                oh->unique = inh->unique;
            }
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_OPEN) {
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            struct fuse_open_out *oo = (struct fuse_open_out *)(void *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out));
            on_open();
            oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
            oh->error = 0; oh->unique = inh->unique;
            oo->fh = 1;
            oo->open_flags = FOPEN_KEEP_CACHE;
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_OPENDIR) {
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            struct fuse_open_out *oo = (struct fuse_open_out *)(void *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out));
            oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
            oh->error = 0; oh->unique = inh->unique;
            oo->fh = 1;
            oo->open_flags = FOPEN_KEEP_CACHE;
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_READDIR) {
            if (n < (ssize_t)(sizeof(struct fuse_in_header) + 20)) continue;
            struct fuse_read_in rr;
            memset(&rr, 0, sizeof(rr));
            size_t copy_len = (size_t)n - sizeof(struct fuse_in_header);
            if (copy_len > sizeof(rr)) copy_len = sizeof(rr);
            memcpy(&rr, payload, copy_len);
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            char *dd = out_buf + sizeof(struct fuse_out_header);
            uint64_t off = rr.offset;
            size_t used = 0;
            struct { uint64_t ino; uint64_t next; const char *nm; } ents[] = {
                { 1, 1, "." }, { FILE_INO, 2, FILE_NAME },
            };
            for (int k = 0; k < 2; k++) {
                if ((uint64_t)(k + 1) <= off) continue;
                size_t nl = strlen(ents[k].nm);
                size_t esz = sizeof(struct fuse_dirent) + nl;
                esz = (esz + 7) & ~7ULL;
                if (used + esz > rr.size) break;
                struct fuse_dirent *de = (struct fuse_dirent *)(void *)(dd + used);
                memset(de, 0, esz);
                de->ino = ents[k].ino;
                de->off = k + 1;
                de->namelen = nl;
                de->type = (k == 0) ? DT_DIR : DT_REG;
                memcpy(de->name, ents[k].nm, nl);
                used += esz;
            }
            oh->len = sizeof(struct fuse_out_header) + used;
            oh->error = 0; oh->unique = inh->unique;
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_RELEASEDIR) {
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            oh->len = sizeof(struct fuse_out_header);
            oh->error = 0; oh->unique = inh->unique;
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_READ) {
            if (n < (ssize_t)(sizeof(struct fuse_in_header) + 20)) continue;
            struct fuse_read_in rin;
            memset(&rin, 0, sizeof(rin));
            size_t copy_len = (size_t)n - sizeof(struct fuse_in_header);
            if (copy_len > sizeof(rin)) copy_len = sizeof(rin);
            memcpy(&rin, payload, copy_len);
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            char *rdata = out_buf + sizeof(struct fuse_out_header);
            uint64_t offset = rin.offset;
            uint32_t size = rin.size;
            if (size > 128 * 1024) size = 128 * 1024;
            static uint64_t total_read = 0;
            static uint64_t last_log_bytes = 0;
            total_read += size;
            if (total_read - last_log_bytes >= 4 * 1024 * 1024 || offset < FILE_SEC * BPS) {
                last_log_bytes = total_read;
                if (g_mode == MODE_VOD_HTTP) {
                    uint64_t m_start = 0;
                    pthread_mutex_lock(&vod_mu);
                    m_start = vod_media_start;
                    pthread_mutex_unlock(&vod_mu);
                    fprintf(stderr, "[FUSE_READ_VOD] total=%lluMB off=%llu sz=%u head_len=%zu media_start=%llu\n",
                        (unsigned long long)(total_read / (1024*1024)),
                        (unsigned long long)offset, size,
                        (g_num_vod_files > 0 ? g_vod_files[0].head_len : 0), (unsigned long long)m_start);
                } else {
                    fprintf(stderr, "[FUSE_READ_LIVE] total=%lluMB off=%llu sz=%u S_write=%lluMB base=%llu\n",
                        (unsigned long long)(total_read / (1024*1024)),
                        (unsigned long long)offset, size,
                        (unsigned long long)(S_write / (1024*1024)),
                        (unsigned long long)base);
                }
            }
            int sret;
            if (g_mode == MODE_LIVE_FIFO) {
                pthread_mutex_lock(&mu);
                sret = serve_disk((uint8_t *)rdata, offset, size, now_ms() + BLOCK_S * 1000);
                pthread_mutex_unlock(&mu);
            } else {
                sret = serve_disk((uint8_t *)rdata, offset, size, now_ms() + BLOCK_S * 1000);
            }
            if (sret < 0) {
                if (sret == -ETIMEDOUT || sret == -EIO) {
                    /* Nunca retorna EIO para o decodificador MStar: preenche com pacotes nulos */
                    fill_null((uint8_t *)rdata, size);
                    oh->len = sizeof(struct fuse_out_header) + size;
                    oh->error = 0;
                    oh->unique = inh->unique;
                } else {
                    oh->len = sizeof(struct fuse_out_header);
                    oh->error = sret;
                    oh->unique = inh->unique;
                }
            } else {
                oh->len = sizeof(struct fuse_out_header) + size;
                oh->error = 0;
                oh->unique = inh->unique;
            }
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        } else {
            struct fuse_out_header *oh = (struct fuse_out_header *)(void *)out_buf;
            oh->len = sizeof(struct fuse_out_header);
            oh->error = -ENOSYS;
            oh->unique = inh->unique;
            if (fuse_reply(fuse_fd, out_buf, oh->len) < 0) break;
        }
    }
    printf("[!] encerrando fuse_direct_v2...\n");
    umount2(mnt, MNT_DETACH);
    close(fuse_fd);
    if (vod_fg_sock >= 0) {
        close(vod_fg_sock);
        vod_fg_sock = -1;
    }
    if (g_local_fd >= 0) {
        close(g_local_fd);
        g_local_fd = -1;
    }
    if (vod_prefetch_thread_started) {
        pthread_mutex_lock(&vod_mu);
        running = 0;
        vod_prefetch_requested = 1;
        pthread_mutex_unlock(&vod_mu);
        atomic_store_explicit(&vod_prefetch_abort, true, memory_order_release);
        vod_bg_sock_shutdown();
        pthread_mutex_lock(&vod_mu);
        pthread_cond_broadcast(&vod_prefetch_cv);
        pthread_cond_broadcast(&vod_done_cv);
        pthread_mutex_unlock(&vod_mu);
        pthread_join(vod_prefetch_tid, NULL);
    }
    return 0;
}
