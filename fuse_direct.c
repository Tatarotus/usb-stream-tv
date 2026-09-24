/* fuse_direct.c — FUSE-served infinite live disk & cloud VOD for USB Stream TV.
 *
 * Supports three modes:
 *   1. MODE_LIVE_FIFO (default): Infinite live stream fed by FIFO into 32MB RAM ring
 *   2. MODE_VOD_HTTP (Cloud VOD): Virtual remote disk via HTTP Range requests (16MB RAM cache, zero flash)
 *   3. MODE_VOD_LOCAL: Direct pread from local static MP4/TS file
 *
 * Presents ONE file (whole virtual FAT32 disk, ~4.0GB virtual) to the USB
 * mass-storage gadget. FAT/boot/dir regions come from a static template or
 * are synthesized arithmetically; file-data sectors stream live from RAM or HTTP.
 *
 * Layout (must match gen_template.py):
 *   reserved=32, fats=2, spf=8192, root cluster 2, file cluster 3, spc=8.
 * Build (ARM32 / Tablet): arm-linux-gnueabihf-gcc -static -O2 -lpthread fuse_direct.c -o fuse_direct_arm32
 * Build (ARM64 / Phone):  aarch64-linux-gnu-gcc -static -O2 -lpthread fuse_direct.c -o fuse_direct_arm64
 * Usage: fuse_direct <mnt> <fifo_or_url_or_file> <template>
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
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dirent.h>
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

/* ---- VOD HTTP Range Cache ---- */
#define VOD_CACHESZ (16ULL * 1024 * 1024) /* 16 MB RAM sliding cache for Cloud VOD */
static uint8_t vod_cache[VOD_CACHESZ];
static uint64_t vod_cache_start = (uint64_t)-1;
static size_t vod_cache_len = 0;
static int vod_sock = -1;
static char g_vod_host[128] = "127.0.0.1";
static int g_vod_port = 80;
static char g_vod_path[256] = "/vod/movie.mp4";

/* ---- ring buffer for Live TV ---- */
#define RINGSZ (32ULL * 1024 * 1024)     /* ~90s @ 2.9Mbps */
#define HDRCACHESZ (512ULL * 1024)
#define LEADBACK (12ULL * 1024 * 1024)   /* TV starts ~40s behind live for network jitter immunity */
#define BLOCK_S 10                       /* max block per read */

#define MNT_POINT "/data/local/tmp/vfat_mnt"
#define FILE_NAME "tv_stream.img"
#define FILE_INO 2
#define DEF_FIFO "/data/local/tmp/live_pipe"
#define DEF_TMPL "/data/local/tmp/fat_template.bin"

static uint8_t ring[RINGSZ];
static uint64_t S_write = 0;             /* absolute stream bytes written */
static int have_data = 0;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;  /* CLOCK_REALTIME */

static uint8_t hcache[HDRCACHESZ];
static uint64_t hcache_len = 0;          /* first stream bytes ever cached */

static uint64_t base = 0;                /* file off F -> stream base+F */
static int base_valid = 0;
static uint64_t prev_Fend = (uint64_t)-1; /* end of last READ (sequential) */
static int consec_small = 0;             /* sequential stale reads <2MB */
static uint64_t last_file_read_ms = 0;    /* timestamp of last file cluster read */
static uint64_t last_tv_stream_pos = 0;   /* absolute stream pos read by TV */

static const char *g_fifo = DEF_FIFO;
static volatile int running = 1;
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
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&cv, &mu, &ts);
}

/* ---- template ---- */
static void load_template(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open template"); exit(1); }
    uint8_t rec[516];
    ssize_t n;
    while ((n = read(fd, rec, sizeof rec)) == (ssize_t)sizeof rec) {
        if (ntmpl >= MAXTMPL) { fprintf(stderr, "template too big\n"); exit(1); }
        uint32_t sec;
        memcpy(&sec, rec, 4);
        tmpl_sec[ntmpl] = sec;
        memcpy(tmpl_dat[ntmpl], rec + 4, 512);

        /* Inspect root directory (cluster 2 / DATA_SEC) to extract file size dynamically */
        if (sec == DATA_SEC) {
            for (int off = 32; off <= 512 - 32; off += 32) {
                uint8_t attr = rec[4 + off + 11];
                uint16_t lowclus = *(uint16_t *)(&rec[4 + off + 26]);
                if (attr == 0x20 && lowclus == 3) {
                    uint32_t sz = *(uint32_t *)(&rec[4 + off + 28]);
                    if (sz > 0) {
                        g_file_size = sz;
                        g_nfileclus = (g_file_size + SPC * BPS - 1) / (SPC * BPS);
                        g_lastclus = FILECLUS + g_nfileclus - 1;
                        printf("[*] Template detectou arquivo: %llu bytes (%.2f MB, %llu clusters)\n",
                               (unsigned long long)g_file_size,
                               (double)g_file_size / (1024.0 * 1024.0),
                               (unsigned long long)g_nfileclus);
                    }
                    break;
                }
            }
        }

        ntmpl++;
    }
    close(fd);
    printf("[*] template: %d sectors loaded\n", ntmpl);
}

static const uint8_t *tmpl_lookup(uint64_t sec) {
    for (int i = 0; i < ntmpl; i++)
        if (tmpl_sec[i] == sec) return tmpl_dat[i];
    return NULL;
}

/* ---- VOD HTTP Range Engine ---- */
static int vod_connect(void) {
    if (vod_sock >= 0) {
        close(vod_sock);
        vod_sock = -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 6;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct hostent *he = gethostbyname(g_vod_host);
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(g_vod_port);
    if (he && he->h_addr_list[0]) {
        memcpy(&saddr.sin_addr, he->h_addr_list[0], he->h_length);
    } else {
        saddr.sin_addr.s_addr = inet_addr(g_vod_host);
    }

    if (connect(s, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(s);
        return -1;
    }
    vod_sock = s;
    return 0;
}

static int vod_fetch_range(uint64_t start_off, size_t needed_len) {
    (void)needed_len;
    if (start_off >= g_file_size) return -1;

    size_t chunk_size = 2 * 1024 * 1024; /* 2MB chunk per fetch */
    if (chunk_size > VOD_CACHESZ) chunk_size = VOD_CACHESZ;
    if (start_off + chunk_size > g_file_size) {
        chunk_size = (size_t)(g_file_size - start_off);
    }
    uint64_t fetch_end = start_off + chunk_size - 1;

    for (int retry = 0; retry < 2; retry++) {
        if (vod_sock < 0) {
            if (vod_connect() < 0) {
                usleep(500000);
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
            g_vod_path, g_vod_host,
            (unsigned long long)start_off,
            (unsigned long long)fetch_end);

        if (write(vod_sock, req, req_len) != req_len) {
            close(vod_sock);
            vod_sock = -1;
            continue;
        }

        /* Read response headers byte by byte until \r\n\r\n */
        char hbuf[2048];
        size_t hlen = 0;
        int header_done = 0;
        while (hlen < sizeof(hbuf) - 1) {
            char c;
            ssize_t r = read(vod_sock, &c, 1);
            if (r <= 0) break;
            hbuf[hlen++] = c;
            hbuf[hlen] = '\0';
            if (hlen >= 4 && memcmp(hbuf + hlen - 4, "\r\n\r\n", 4) == 0) {
                header_done = 1;
                break;
            }
        }

        if (!header_done) {
            close(vod_sock);
            vod_sock = -1;
            continue;
        }

        int status_code = 0;
        if (sscanf(hbuf, "HTTP/1.%*d %d", &status_code) != 1 || (status_code != 200 && status_code != 206)) {
            fprintf(stderr, "[VOD] HTTP status inesperado: %d\n", status_code);
            close(vod_sock);
            vod_sock = -1;
            continue;
        }

        /* Parse Content-Length if available */
        size_t expected_len = chunk_size;
        char *cl_str = strcasestr(hbuf, "Content-Length:");
        if (cl_str) {
            size_t val = 0;
            if (sscanf(cl_str + 15, "%zu", &val) == 1 && val > 0 && val <= VOD_CACHESZ) {
                expected_len = val;
            }
        }

        size_t total_body = 0;
        while (total_body < expected_len) {
            ssize_t rd = read(vod_sock, vod_cache + total_body, expected_len - total_body);
            if (rd <= 0) break;
            total_body += (size_t)rd;
        }

        if (total_body > 0) {
            vod_cache_start = start_off;
            vod_cache_len = total_body;
            return 0;
        }

        close(vod_sock);
        vod_sock = -1;
    }
    return -1;
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
    if (g_mode != MODE_LIVE_FIFO) {
        printf("[FUSE] on_open: modo VOD (tamanho %llu bytes)\n", (unsigned long long)g_file_size);
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
static void serve_disk(uint8_t *dst, uint64_t disk, size_t n, uint64_t deadline_ms) {
    size_t done = 0;
    while (done < n) {
        uint64_t sec = (disk + done) / BPS;
        size_t sec_off = (size_t)((disk + done) % BPS);
        size_t c = BPS - sec_off;
        if (c > n - done) c = n - done;
        uint8_t sbuf[BPS];

        const uint8_t *t = tmpl_lookup(sec);
        if (t) {
            memcpy(sbuf, t, BPS);
            memcpy(dst + done, sbuf + sec_off, c);
            done += c;
            continue;
        }

        if (sec < FILE_SEC) {
            if (sec >= RSV && sec < RSV + NFATS * SPF) {
                /* synthesized FAT mirror: 128 entries/sector */
                uint64_t sno = sec - RSV;
                if (sno >= SPF) sno -= SPF;
                uint32_t *e = (uint32_t *)sbuf;
                for (int k = 0; k < 128; k++) {
                    uint64_t cl = sno * 128 + (uint64_t)k;
                    uint32_t v;
                    if (cl < 2) v = (cl == 0) ? 0x0FFFFFF8 : 0x0FFFFFFF;
                    else if (cl == 2) v = 0x0FFFFFFF;
                    else if (cl < g_lastclus) v = (uint32_t)(cl + 1);
                    else if (cl == g_lastclus) v = 0x0FFFFFFF;
                    else v = 0;
                    e[k] = v;
                }
            } else {
                memset(sbuf, 0, BPS);
            }
            memcpy(dst + done, sbuf + sec_off, c);
            done += c;
            continue;
        }

        uint64_t clus = 2 + (sec - DATA_SEC) / SPC;
        if (clus >= FILECLUS && clus < FILECLUS + g_nfileclus) {
            uint64_t foff = (clus - FILECLUS) * (SPC * BPS)
                          + (sec - (DATA_SEC + (clus - 2) * SPC)) * BPS + sec_off;

            /* ---- MODE_VOD_HTTP (Cloud VOD) ---- */
            if (g_mode == MODE_VOD_HTTP) {
                if (foff < g_file_size) {
                    size_t to_read = c;
                    if (foff + to_read > g_file_size) to_read = (size_t)(g_file_size - foff);
                    if (foff >= vod_cache_start && foff + to_read <= vod_cache_start + vod_cache_len) {
                        memcpy(dst + done, vod_cache + (foff - vod_cache_start), to_read);
                    } else {
                        if (vod_fetch_range(foff, to_read) == 0 && foff >= vod_cache_start && foff < vod_cache_start + vod_cache_len) {
                            size_t avail = vod_cache_len - (size_t)(foff - vod_cache_start);
                            if (avail > to_read) avail = to_read;
                            memcpy(dst + done, vod_cache + (foff - vod_cache_start), avail);
                            if (to_read > avail) memset(dst + done + avail, 0, to_read - avail);
                        } else {
                            memset(dst + done, 0, to_read);
                        }
                    }
                    if (c > to_read) memset(dst + done + to_read, 0, c - to_read);
                } else {
                    memset(dst + done, 0, c);
                }
                done += c;
                continue;
            }

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
}

/* ---- FUSE ---- */
static void handle_sig(int s) { (void)s; running = 0; }

int main(int argc, char **argv) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    const char *mnt = MNT_POINT;
    if (argc > 1) mnt = argv[1];
    if (argc > 2) g_fifo = argv[2];
    if (argc > 3) load_template(argv[3]);
    else load_template(DEF_TMPL);

    /* Detect mode from target data source */
    if (strncmp(g_fifo, "http://", 7) == 0 || strncmp(g_fifo, "https://", 8) == 0) {
        g_mode = MODE_VOD_HTTP;
        const char *p = (strncmp(g_fifo, "https://", 8) == 0) ? g_fifo + 8 : g_fifo + 7;
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
            strncpy(g_vod_host, hostport, sizeof(g_vod_host) - 1);
            strncpy(g_vod_path, slash, sizeof(g_vod_path) - 1);
        }
        printf("[✓] FUSE iniciado em MODO VOD CLOUD: host=%s port=%d path=%s\n",
               g_vod_host, g_vod_port, g_vod_path);
    } else if (access(g_fifo, F_OK) == 0 && (strstr(g_fifo, ".mp4") || strstr(g_fifo, ".ts")) && !strstr(g_fifo, "pipe")) {
        struct stat st;
        if (stat(g_fifo, &st) == 0 && S_ISREG(st.st_mode)) {
            g_mode = MODE_VOD_LOCAL;
            g_local_fd = open(g_fifo, O_RDONLY);
            printf("[✓] FUSE iniciado em MODO VOD LOCAL: %s\n", g_fifo);
        }
    } else {
        g_mode = MODE_LIVE_FIFO;
        printf("[✓] FUSE iniciado em MODO TV AO VIVO (FIFO: %s)\n", g_fifo);
    }

    mkdir(mnt, 0755);
    umount2(mnt, MNT_DETACH);

    fuse_fd = open("/dev/fuse", O_RDWR);
    if (fuse_fd < 0) { perror("open /dev/fuse"); return 1; }

    char opts[256];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=040755,user_id=0,group_id=0,allow_other", fuse_fd);
    if (mount("fuse", mnt, "fuse", 0, opts) < 0) { perror("mount"); return 2; }
    printf("[✓] fuse_direct montado em %s\n", mnt);
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    if (g_mode == MODE_LIVE_FIFO) {
        pthread_t th;
        if (pthread_create(&th, NULL, feeder, NULL) != 0) { perror("thread"); return 3; }
    }

    static char in_buf[128 * 1024 + 4096];
    static char out_buf[128 * 1024 + 4096];

    while (running) {
        ssize_t n = read(fuse_fd, in_buf, sizeof(in_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != ENODEV) perror("fuse read error");
            break;
        }
        if (n < (ssize_t)sizeof(struct fuse_in_header)) continue;
        struct fuse_in_header *inh = (struct fuse_in_header *)in_buf;
        void *payload = in_buf + sizeof(struct fuse_in_header);

        if (inh->opcode == FUSE_INIT) {
            struct fuse_init_in *ii = payload;
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            struct fuse_init_out *io = (struct fuse_init_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out));
            oh->error = 0; oh->unique = inh->unique;
            io->major = FUSE_KERNEL_VERSION;
            io->minor = ii->minor < FUSE_KERNEL_MINOR_VERSION ? ii->minor : FUSE_KERNEL_MINOR_VERSION;
            io->max_readahead = 128 * 1024;
            io->flags = ii->flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES);
            io->max_write = 128 * 1024;
            if (ii->minor < 23) {
                /* FUSE_COMPAT_22_INIT_OUT_SIZE = 24 bytes for Linux <= 3.13 */
                oh->len = sizeof(struct fuse_out_header) + 24;
            } else {
                oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
            }
            if (write(fuse_fd, out_buf, oh->len) < 0) {
                perror("write FUSE_INIT reply");
                break;
            }
        } else if (inh->opcode == FUSE_GETATTR) {
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            struct fuse_attr_out *ao = (struct fuse_attr_out *)(out_buf + sizeof(struct fuse_out_header));
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
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_LOOKUP) {
            char *name = payload;
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            if (strncmp(name, FILE_NAME, sizeof(FILE_NAME)) == 0) {
                struct fuse_entry_out *eo = (struct fuse_entry_out *)(out_buf + sizeof(struct fuse_out_header));
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
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_OPEN) {
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            struct fuse_open_out *oo = (struct fuse_open_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out));
            on_open();
            oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
            oh->error = 0; oh->unique = inh->unique;
            oo->fh = 1;
            oo->open_flags = FOPEN_KEEP_CACHE;
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_OPENDIR) {
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            struct fuse_open_out *oo = (struct fuse_open_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out));
            oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
            oh->error = 0; oh->unique = inh->unique;
            oo->fh = 1;
            oo->open_flags = FOPEN_KEEP_CACHE;
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_READDIR) {
            struct fuse_read_in *rr = payload;
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            char *dd = out_buf + sizeof(struct fuse_out_header);
            uint64_t off = rr->offset;
            size_t used = 0;
            struct { uint64_t ino; uint64_t next; const char *nm; } ents[] = {
                { 1, 1, "." }, { FILE_INO, 2, FILE_NAME },
            };
            for (int k = 0; k < 2; k++) {
                if ((uint64_t)(k + 1) <= off) continue;
                size_t nl = strlen(ents[k].nm);
                size_t esz = sizeof(struct fuse_dirent) + nl;
                esz = (esz + 7) & ~7ULL;
                if (used + esz > rr->size) break;
                struct fuse_dirent *de = (struct fuse_dirent *)(dd + used);
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
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_RELEASEDIR) {
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            oh->len = sizeof(struct fuse_out_header);
            oh->error = 0; oh->unique = inh->unique;
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else if (inh->opcode == FUSE_READ) {
            struct fuse_read_in *rin = payload;
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            char *rdata = out_buf + sizeof(struct fuse_out_header);
            uint64_t offset = rin->offset;
            uint32_t size = rin->size;
            if (size > 128 * 1024) size = 128 * 1024;
            static uint64_t total_read = 0;
            static uint64_t last_log_bytes = 0;
            total_read += size;
            if (total_read - last_log_bytes >= 2 * 1024 * 1024 || offset < FILE_SEC * BPS) {
                last_log_bytes = total_read;
                fprintf(stderr, "[FUSE_READ] total=%lluMB off=%llu sz=%u S_write=%lluMB base=%llu\n",
                    (unsigned long long)(total_read / (1024*1024)),
                    (unsigned long long)offset, size,
                    (unsigned long long)(S_write / (1024*1024)),
                    (unsigned long long)base);
            }
            pthread_mutex_lock(&mu);
            serve_disk((uint8_t *)rdata, offset, size, now_ms() + BLOCK_S * 1000);
            pthread_mutex_unlock(&mu);
            oh->len = sizeof(struct fuse_out_header) + size;
            oh->error = 0; oh->unique = inh->unique;
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        } else {
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            oh->len = sizeof(struct fuse_out_header);
            oh->error = -ENOSYS;
            oh->unique = inh->unique;
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
        }
    }
    printf("[!] encerrando fuse_direct...\n");
    umount2(mnt, MNT_DETACH);
    close(fuse_fd);
    if (vod_sock >= 0) close(vod_sock);
    if (g_local_fd >= 0) close(g_local_fd);
    return 0;
}
