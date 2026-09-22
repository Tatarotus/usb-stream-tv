/* fuse_direct.c — FUSE-served infinite live disk for USB Stream TV.
 *
 * Presents ONE file (whole virtual FAT32 disk, ~4.3GB virtual) to the USB
 * mass-storage gadget. FAT/boot/dir regions come from a static template or
 * are synthesized arithmetically; file-data sectors stream live from a
 * 32MB RAM ring fed by writer.py over a named FIFO. Reads past the write
 * frontier BLOCK (paced to 1x broadcast rate) instead of returning stale
 * data — the TV can never overtake the writer, never hits EOF, never sees
 * a torn rewrite (all copies under mutex).
 *
 * Layout (must match gen_template.py):
 *   reserved=32, fats=2, spf=8192, root cluster 2, file cluster 3, spc=8.
 * Build: aarch64-linux-gnu-gcc -static -O2 -lpthread fuse_direct.c -o fuse_direct
 * Usage: fuse_direct <mnt> <fifo> <template>   (all paths, defaults below)
 *
 * Locking: serve_disk() and helpers run with mu HELD (taken in FUSE_READ).
 * on_open() takes mu itself. Feeder takes mu around ring updates.
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
#include <dirent.h>
#include <linux/fuse.h>

/* ---- geometry ---- */
#define BPS 512ULL
#define SPC 8ULL
#define RSV 32ULL
#define NFATS 2ULL
#define SPF 8192ULL
#define FILECLUS 3ULL
#define FILE_SIZE 1800000000ULL          /* 1.8GB (~96 min @ 305KB/s, safe for signed 32-bit) */
#define DATA_SEC (RSV + NFATS * SPF)     /* 16416: cluster 2 */
#define FILE_SEC (DATA_SEC + SPC)        /* 16424: cluster 3 */
#define NFILECLUS ((FILE_SIZE + SPC * BPS - 1) / (SPC * BPS))
#define LASTCLUS (FILECLUS + NFILECLUS - 1)
#define TOTCLUS 1048576ULL               /* 4.00GB virtual disk, matches template total_sec */
#define TOTSEC (DATA_SEC + (TOTCLUS - 2ULL) * SPC)
#define DISK_SIZE (TOTSEC * BPS)

/* ---- ring ---- */
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

static const char *g_fifo = DEF_FIFO;
static volatile int running = 1;
static int fuse_fd = -1;

/* template sectors: secno -> 512 bytes (boot, fsinfo, rootdir) */
#define MAXTMPL 16
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
        ntmpl++;
    }
    close(fd);
    printf("[*] template: %d sectors\n", ntmpl);
}

static const uint8_t *tmpl_lookup(uint64_t sec) {
    for (int i = 0; i < ntmpl; i++)
        if (tmpl_sec[i] == sec) return tmpl_dat[i];
    return NULL;
}

/* ---- ring ops (mu held) ---- */
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

/* cache stream bytes [abs_at, abs_at+n); only first 512KB ever */
static void hcache_feed(const uint8_t *p, size_t n, uint64_t abs_at) {
    if (abs_at >= HDRCACHESZ) return;
    size_t c = n;
    if ((uint64_t)c > HDRCACHESZ - abs_at) c = (size_t)(HDRCACHESZ - abs_at);
    memcpy(hcache + abs_at, p, c);
    if (abs_at + c > hcache_len) hcache_len = abs_at + c;
}

/* copy absolute range [a, a+n), caller guarantees present; mu held */
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

/* ---- fifo feeder ---- */
static void *feeder(void *arg) {
    (void)arg;
    static uint8_t buf[65536];
    for (;;) {
        if (!running) return NULL;
        int fd = open(g_fifo, O_RDONLY);   /* blocks till writer opens */
        if (fd < 0) { sleep(1); continue; }
        printf("[*] fifo reader connected\n");
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;             /* writer died/restarted: reopen */
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

/* ---- rebase helpers (mu held) ---- */
static uint64_t snap188(uint64_t v) { return v - (v % 188ULL); }

/* PAT packet at/after absolute p (within 64KB, present); else p */
static uint64_t snap_pat(uint64_t p) {
    uint64_t end = p + 65536;
    if (end > S_write) end = S_write;
    uint64_t q = snap188(p);
    for (; q + 188 <= end; q += 188) {
        size_t idx = (size_t)(q % RINGSZ);
        uint8_t h0 = ring[idx], h1 = ring[(idx + 1) % RINGSZ], h2 = ring[(idx + 2) % RINGSZ];
        if (h0 == 0x47 && (((h1 & 0x1F) << 8) | h2) == 0) return q;
    }
    return snap188(p);
}

/* NAL type present in ring byte range [a, b)? */
static int ring_has_nal(uint64_t a, uint64_t b, int want) {
    if (b > S_write) b = S_write;
    if (b <= a + 4) return 0;
    uint8_t w0 = 0, w1 = 0, w2 = 0;
    for (uint64_t p = a; p < b; p++) {
        uint8_t by = ring[(size_t)(p % RINGSZ)];
        if (w0 == 0 && w1 == 0 && w2 == 1) {
            if ((by & 0x1F) == want) return 1;
        }
        w0 = w1; w1 = w2; w2 = by;
    }
    return 0;
}

/* best file-start: PAT packet whose following 64KB contain SPS (IDR near) */
static uint64_t snap_open_base(void) {
    uint64_t lo = (S_write > LEADBACK + 65536) ? S_write - LEADBACK - 65536 : 0;
    uint64_t hi = S_write > LEADBACK ? S_write - LEADBACK : 0;
    for (uint64_t p = snap188(lo); p + 188 <= hi; p += 188) {
        size_t idx = (size_t)(p % RINGSZ);
        uint8_t h0 = ring[idx], h1 = ring[(idx + 1) % RINGSZ], h2 = ring[(idx + 2) % RINGSZ];
        if (h0 == 0x47 && (((h1 & 0x1F) << 8) | h2) == 0) {
            if (ring_has_nal(p, p + 65536, 7)) return p;
        }
    }
    return snap_pat(hi > 188 ? hi - 188 : 0);
}

/* map file offset F near live frontier; mu held */
static void do_rebase(uint64_t F) {
    uint64_t Fs = F - (F % 188ULL);
    uint64_t nb = (S_write > LEADBACK + Fs) ? (S_write - LEADBACK - Fs) : 0;
    nb = snap188(nb);
    nb = snap_pat(nb + Fs > S_write ? S_write : nb + 0); /* PAT near target */
    if (nb > S_write) nb = 0;
    base = nb;
    base_valid = 1;
    prev_Fend = (uint64_t)-1;
    consec_small = 0;
}

static void on_open(void) {
    pthread_mutex_lock(&mu);
    /* wait for enough stream to choose a decodable start (≤5s) */
    {
        uint64_t t0 = now_ms();
        while (S_write < 512 * 1024 && now_ms() - t0 < 5000 && running) wait_step();
    }
    if (have_data && S_write > 0) {
        base = snap_open_base();
    } else {
        base = 0;
    }
    fprintf(stderr, "[FUSE] on_open: base=%llu S_write=%llu (lead: %.1fs)\n",
            (unsigned long long)base, (unsigned long long)S_write,
            (double)(S_write - base) / (305.0 * 1024.0));
    base_valid = 1;
    prev_Fend = (uint64_t)-1;
    consec_small = 0;
    pthread_mutex_unlock(&mu);
}

static void fill_null(uint8_t *dst, size_t n) {
    size_t off = 0, frag = n % 188;
    if (frag) { memset(dst, 0, frag); off = frag; }
    for (; off + 188 <= n; off += 188) memcpy(dst + off, NULL_PKT, 188);
}

/* serve absolute disk range [disk, disk+n); mu HELD by caller.
   deadline_ms is an absolute now_ms() budget SHARED by all fragments of
   one FUSE_READ (a per-fragment timeout would multiply 10s by every 512B
   sector of a 128KB read = 43min hangs). */
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
                    else if (cl < LASTCLUS) v = (uint32_t)(cl + 1);
                    else if (cl == LASTCLUS) v = 0x0FFFFFFF;
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
        if (clus >= FILECLUS && clus < FILECLUS + NFILECLUS) {
            uint64_t foff = (clus - FILECLUS) * (SPC * BPS)
                          + (sec - (DATA_SEC + (clus - 2) * SPC)) * BPS + sec_off;
            size_t fo = 0;
            while (fo < c) {
                uint64_t F = foff + fo;
                size_t cc = c - fo;
                uint64_t ring_old = (S_write > RINGSZ) ? S_write - RINGSZ : 0;

                if (!have_data) {
                    /* pre-stream: brief wait then nulls (shares deadline) */
                    while (!have_data && now_ms() < deadline_ms && running) wait_step();
                    if (!have_data) { fill_null(dst + done + fo, cc); fo += cc; continue; }
                    continue;
                }
                /* rule 1: stale probe served from header cache */
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
                        /* Stale read behind ring buffer (or seek back): serve from ring cyclically */
                        uint64_t safe_pos = snap188(ring_old + (F % (RINGSZ / 2)));
                        if (safe_pos + cc > S_write) safe_pos = snap188(S_write > cc ? S_write - cc : 0);
                        ring_copy(dst + done + fo, safe_pos, cc);
                        fo += cc;
                        continue;
                    }
                    if (start >= S_write) {
                        /* Sequential read near frontier: BLOCK and pace to 1x broadcast rate */
                        if (start < S_write + 4 * 1024 * 1024) {
                            while (start >= S_write && now_ms() < deadline_ms && running)
                                wait_step();
                        }
                        if (start >= S_write) {
                            /* Ahead of frontier: distinguish PROBE from STREAM.
                               - Probe/seek (discontinuous F far ahead: format/EOF
                                 validators): serve valid cyclic bytes NOW so the
                                 TV recognizes video (fixes "Nenhum arquivo").
                               - Sequential stream at/near frontier: BLOCK to pace
                                 1x broadcast rate (core FUSE guarantee: the TV
                                 can never overtake live or spin on repeats). */
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
            /* lazy-rebase tracking (3-clause rule) */
            if (base_valid) {
                uint64_t s0 = base + foff;
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

    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "mkdir -p %s", mnt);
        if (system(cmd) != 0) return 1;
    }
    umount2(mnt, MNT_DETACH);

    fuse_fd = open("/dev/fuse", O_RDWR);
    if (fuse_fd < 0) { perror("open /dev/fuse"); return 1; }

    char opts[256];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=040755,user_id=0,group_id=0,allow_other", fuse_fd);
    if (mount("fuse", mnt, "fuse", 0, opts) < 0) { perror("mount"); return 2; }
    printf("[✓] fuse_direct em %s (fifo %s)\n", mnt, g_fifo);
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    pthread_t th;
    if (pthread_create(&th, NULL, feeder, NULL) != 0) { perror("thread"); return 3; }

    static char in_buf[128 * 1024 + 4096];
    static char out_buf[128 * 1024 + 4096];

    while (running) {
        ssize_t n = read(fuse_fd, in_buf, sizeof(in_buf));
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n < (ssize_t)sizeof(struct fuse_in_header)) continue;
        struct fuse_in_header *inh = (struct fuse_in_header *)in_buf;
        void *payload = in_buf + sizeof(struct fuse_in_header);

        if (inh->opcode == FUSE_INIT) {
            struct fuse_init_in *ii = payload;
            struct fuse_out_header *oh = (struct fuse_out_header *)out_buf;
            struct fuse_init_out *io = (struct fuse_init_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out));
            oh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
            oh->error = 0; oh->unique = inh->unique;
            io->major = FUSE_KERNEL_VERSION;
            io->minor = FUSE_KERNEL_MINOR_VERSION;
            io->max_readahead = 128 * 1024;
            io->flags = ii->flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES);
            io->max_write = 128 * 1024;
            if (write(fuse_fd, out_buf, oh->len) < 0) break;
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
            /* entries: "." (ino 1), FILE_NAME (ino 2) */
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
    return 0;
}
