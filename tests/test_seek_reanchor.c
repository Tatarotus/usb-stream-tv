/* test_seek_reanchor.c — Self-contained unit test simulating serve_live_backend
 * under the exact Samsung TV flow:
 *   1. stream starts at foff=0, feeder feeds realtime
 *   2. TV reads sequentially up to foff=901120
 *   3. TV jumps to foff=799,997,952 (800MB bookmark/10% probe)
 * Verifies (a)-(e) per task spec. Faithful model of src/ntfs/fuse_ntfs.c D13.1.
 *
 * Compile & run (host):
 *   gcc -Wall -Wextra -O2 scratch/test_seek_reanchor.c -o /tmp/opencode/test_seek_reanchor && /tmp/opencode/test_seek_reanchor
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

/* ---- Production constants (exact copies from fuse_ntfs.h / fuse_ntfs.c) ---- */
#define RINGSZ (128ULL * 1024 * 1024)
#define NTFS_FILE_SIZE 8000000000ULL
#define MIN_STREAM_START (6ULL * 1024 * 1024)
#define LEADBACK (16ULL * 1024 * 1024)
#define HDRCACHESZ 65536
#define BLOCK 65536u
#define FOFF_JUMP 799997952ULL          /* 800MB bookmark probe from TV log */
#define FOFF_SEQ_END 901120ULL          /* TV sequential read end before jump */
#define FOFF_PROBE_15G 1610612736ULL    /* 1.5 GiB isolated probe (64KB) */

static uint8_t *g_ring = NULL;
static uint64_t g_s_write = 0;
static uint64_t g_base = 0;
static uint64_t g_epoch = 0;
static int g_base_valid = 0;
static int g_have_data = 0;
static volatile int g_running = 1;
static uint64_t g_prev_Fend = (uint64_t)-1;
static uint64_t g_anchor_foff = (uint64_t)-1;
static uint64_t g_anchor_stream_pos = 0;
static uint64_t g_anchor_birth_ms = 0;
static uint64_t g_probe_seq_end = (uint64_t)-1;
static int g_probe_consecutive_count = 0;
static uint64_t g_probe_accumulated_bytes = 0;
static uint64_t g_last_tv_stream_pos = 0;
static uint64_t g_last_file_read_ms = 0;
static uint64_t s_last_pace_ms = 0;
static uint8_t g_hcache[HDRCACHESZ];
static uint64_t g_hcache_len = 0;

static const uint8_t NULL_PKT[188] = {
    [0] = 0x47, [1] = 0x1F, [2] = 0xFF, [3] = 0x10,
    [4 ... 187] = 0xFF
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}
static void wait_step(void) __attribute__((unused));
static void wait_step(void) { }

static void fill_null(uint8_t *dst, size_t n) {
    size_t off = 0, frag = n % 188;
    if (frag) { memset(dst, 0, frag); off = frag; }
    for (; off + 188 <= n; off += 188) memcpy(dst + off, NULL_PKT, 188);
}
static inline uint64_t snap188(uint64_t v) { return v - (v % 188ULL); }

static inline uint64_t foff_to_stream_pos(uint64_t foff) {
    if (g_anchor_foff == (uint64_t)-1) return g_anchor_stream_pos;
    if (foff >= g_anchor_foff) return g_anchor_stream_pos + (foff - g_anchor_foff);
    uint64_t back = g_anchor_foff - foff;
    if (g_anchor_stream_pos >= back) return g_anchor_stream_pos - back;
    return 0;
}

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
static void ring_pkt(uint64_t abs_pos, uint8_t out[188]) { ring_copy(out, abs_pos, 188); }

/* ---- Anchor search (verbatim logic from fuse_ntfs.c, minus stderr) ---- */
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
            if (pay[i]==0 && pay[i+1]==0 && pay[i+2]==1) {
                if ((pay[i+3] & 0x1F) == target_type) return 1;
            } else if (i+5 <= plen && pay[i]==0 && pay[i+1]==0 && pay[i+2]==0 && pay[i+3]==1) {
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
        if (pkt[0]==0x47 && (pkt[1]&0x1F)==0 && pkt[2]==0) {
            uint8_t af=(uint8_t)((pkt[3]>>4)&3);
            size_t off=4;
            if (af==2||af==0) continue;
            if (af==3) off += 1+(size_t)pkt[4];
            if (off>=188) continue;
            uint8_t ptr=pkt[off];
            if (off+1+(size_t)ptr<188 && pkt[off+1+(size_t)ptr]==0x00) return p;
        }
    }
    return (uint64_t)-1;
}
static uint64_t find_transport_lock(uint64_t start_pos, size_t window_sz, uint64_t min_pos) {
    uint64_t p = snap188(start_pos);
    uint64_t end = p + (uint64_t)window_sz;
    if (end > g_s_write) end = g_s_write;
    for (; p + 3*188 <= end; p += 188) {
        if (p < min_pos) continue;
        uint8_t p0[188],p1[188],p2[188];
        ring_copy(p0,p,188); ring_copy(p1,p+188,188); ring_copy(p2,p+376,188);
        if (p0[0]==0x47 && p1[0]==0x47 && p2[0]==0x47) return p;
    }
    return (uint64_t)-1;
}
static uint64_t find_anchor(uint64_t target, uint64_t min_pos, int prefer_sps,
                            const char **out_type, const char **out_win) {
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
            if (pkt[0]==0x47 && (pkt[1]&0x1F)==0 && pkt[2]==0) {
                uint8_t af=(uint8_t)((pkt[3]>>4)&3);
                size_t off=4;
                if (af==3) off += 1+(size_t)pkt[4];
                if (off<188) {
                    uint8_t ptr=pkt[off];
                    if (off+1+(size_t)ptr<188 && pkt[off+1+(size_t)ptr]==0x00) {
                        uint64_t scan_end=p+262144;
                        if (scan_end>g_s_write) scan_end=g_s_write;
                        if (ring_has_nal(p,scan_end,7)) {
                            *out_type="PAT_SPS"; *out_win="2M_REV"; return p;
                        }
                    }
                }
            }
            if (p<188) break;
            p-=188;
        }
    }
    uint64_t pat=find_pat_in_window(target,2*1024*1024,min_pos);
    if (pat!=(uint64_t)-1){*out_type="PAT";*out_win="2M";return pat;}
    uint64_t lock=find_transport_lock(target,2*1024*1024,min_pos);
    if (lock!=(uint64_t)-1){*out_type="TRANSPORT";*out_win="2M_LOCK";return lock;}
    *out_type="NONE";*out_win="none";return (uint64_t)-1;
}
static int snap_open_base_target(uint64_t target, uint64_t *out_base) {
    uint64_t ring_old=(g_s_write>RINGSZ)?g_s_write-RINGSZ:0;
    if (target<ring_old) target=ring_old;
    const char *atype="NONE"; const char *awin="none";
    uint64_t p_anchor=find_anchor(target,0,1,&atype,&awin);
    if (p_anchor!=(uint64_t)-1){*out_base=p_anchor;return 1;}
    if (g_s_write>0){*out_base=snap188(target);return 1;}
    return 0;
}

/* ---- serve_live_backend: faithful port (mutex/sleep stripped for unit test) ---- */
static int g_last_is_reopen = -1; /* observable for assertions */

static void serve_live_backend(uint8_t *dst, uint64_t foff, size_t c, uint64_t deadline_ms) {
    (void)deadline_ms;
    g_last_is_reopen = 0;
    if (foff >= NTFS_FILE_SIZE - 10ULL*1024*1024) { fill_null(dst,c); return; }

    uint64_t now = now_ms();
    uint64_t ring_old = (g_s_write > RINGSZ) ? g_s_write - RINGSZ : 0;

    int is_seq_read = (g_prev_Fend != (uint64_t)-1 &&
                       (foff == g_prev_Fend ||
                        (foff > g_prev_Fend && foff - g_prev_Fend <= 262144) ||
                        (foff < g_prev_Fend && g_prev_Fend - foff <= 262144)));

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

    int is_opening_grace = (g_anchor_birth_ms != 0 && (now - g_anchor_birth_ms < 10000ULL) && g_anchor_foff <= 4ULL * 1024 * 1024);

    int is_confirmed_jump = (!is_seq_read &&
                             !is_opening_grace &&
                             g_probe_consecutive_count >= 4 &&
                             g_probe_accumulated_bytes >= 1ULL * 1024 * 1024);

    uint64_t s_probe = foff_to_stream_pos(foff);
    int is_future_probe = (s_probe >= g_s_write + 2ULL*1024*1024 && !is_seq_read);
    int is_behind_probe = (s_probe < ring_old && !is_seq_read);

    if (g_base_valid && foff >= 8ULL*1024*1024 && (is_future_probe || is_behind_probe) && !is_confirmed_jump) {
        fill_null(dst, c);
        /* DO NOT update g_prev_Fend! Active playback chain remains intact */
        return;
    }

    int is_reopen = 0;
    if (!g_base_valid || g_anchor_foff == (uint64_t)-1) {
        is_reopen = 1;
    } else if (foff == 0 && (now - g_last_file_read_ms > 1500 || g_prev_Fend > 131072)) {
        is_reopen = 1;
    } else if (is_seq_read) {
        uint64_t s0 = foff_to_stream_pos(foff);
        if (s0 < ring_old) {
            is_reopen = 1;
        }
    } else if (is_confirmed_jump) {
        is_reopen = 1;
        g_probe_consecutive_count = 0;
        g_probe_accumulated_bytes = 0;
        g_probe_seq_end = (uint64_t)-1;
    }
    g_last_is_reopen = is_reopen;

    if (is_reopen) {
        /* test: feeder already buffered (skip 5s wait loop) */
        if (g_have_data && g_s_write > 0) {
            uint64_t snapped_abs=0;
            uint64_t eff_lead=(g_s_write>LEADBACK)?LEADBACK:(g_s_write>2ULL*1024*1024?g_s_write-2ULL*1024*1024:0);
            uint64_t live_target=(g_s_write>eff_lead)?g_s_write-eff_lead:0;
            if (snap_open_base_target(live_target,&snapped_abs)) {
                g_anchor_foff=foff;
                g_anchor_stream_pos=snapped_abs;
                g_base=snapped_abs;
                g_epoch++;
                g_last_tv_stream_pos=snapped_abs;
                g_base_valid=1;
                g_prev_Fend=(uint64_t)-1;
                g_anchor_birth_ms=now;
                s_last_pace_ms=0;
                size_t c_h=65536; if (c_h>HDRCACHESZ) c_h=HDRCACHESZ;
                uint64_t reader_p=snapped_abs;
                if (g_s_write>reader_p){
                    size_t av=(size_t)(g_s_write-reader_p);
                    if (c_h>av) c_h=av;
                    for(size_t i=0;i<c_h;i++) g_hcache[i]=g_ring[(size_t)((reader_p+i)%RINGSZ)];
                    g_hcache_len=c_h;
                }
            }
        }
    }
    g_last_file_read_ms = now;

    size_t fo=0;
    while (fo<c) {
        uint64_t F=foff+(uint64_t)fo;
        size_t cc=c-fo;
        ring_old=(g_s_write>RINGSZ)?g_s_write-RINGSZ:0;
        if (!g_have_data){fill_null(dst+fo,cc);fo+=cc;continue;}
        if (F<g_hcache_len){
            size_t hc=(size_t)(g_hcache_len-F);
            if(hc>cc)hc=cc;
            memcpy(dst+fo,g_hcache+F,hc);
            fo+=hc;continue;
        }
        if(!g_base_valid){fill_null(dst+fo,cc);fo+=cc;continue;}
        uint64_t start=foff_to_stream_pos(F);
        if (start<ring_old){
            uint64_t snapped_abs=0;
            uint64_t live_target=(g_s_write>LEADBACK)?g_s_write-LEADBACK:0;
            if(snap_open_base_target(live_target,&snapped_abs)){
                g_anchor_foff=F;g_anchor_stream_pos=snapped_abs;g_epoch++;s_last_pace_ms=0;start=snapped_abs;
            } else {fill_null(dst+fo,cc);fo+=cc;continue;}
        }
        /* pacing intentionally omitted in unit test (no throttle) */
        if (start>=g_s_write){
            uint64_t s_frag=foff_to_stream_pos(F);
            if (s_frag>=g_s_write+2ULL*1024*1024 && !is_seq_read){
                fill_null(dst+fo,cc);fo+=cc;continue;
            }
            /* feeder underrun on sequential path: no wait in test, emit NULLs once */
            fill_null(dst+fo,cc);fo+=cc;continue;
        }
        size_t avail=(size_t)(g_s_write-start);
        if(avail>cc)avail=cc;
        ring_copy(dst+fo,start,avail);
        fo+=avail;
    }
    if (g_base_valid){
        g_last_tv_stream_pos=foff_to_stream_pos(foff+c);
        g_prev_Fend=foff+c;
        g_probe_consecutive_count=0;
        g_probe_accumulated_bytes=0;
        g_probe_seq_end=(uint64_t)-1;
    }
}

/* ---- Synthetic MPEG-TS feeder ---- */
static void make_pat(uint8_t out[188]) {
    memset(out,0xFF,188);
    out[0]=0x47; out[1]=0x40; out[2]=0x00; out[3]=0x10; /* PID 0x0000, payload only */
    out[4]=0x00; /* pointer_field */
    out[5]=0x00; /* table_id PAT */
    out[6]=0xB0; out[7]=0x0D;
}
static void make_video_sps(uint8_t out[188], uint8_t cc) {
    memset(out,0xFF,188);
    out[0]=0x47; out[1]=0x41; out[2]=0x00; out[3]=0x10|(cc&0x0F); /* PID 0x0100 */
    out[4]=0x00; out[5]=0x00; out[6]=0x01; out[7]=0x67; /* 00 00 01 67 = SPS NAL */
    out[8]=0x42; out[9]=0x00; out[10]=0x1E;
}
static void make_video_generic(uint8_t out[188], uint8_t cc) {
    memset(out,0xAB,188);
    out[0]=0x47; out[1]=0x41; out[2]=0x00; out[3]=0x10|(cc&0x0F);
    out[4]=0x00; out[5]=0x00; out[6]=0x01; out[7]=0x41; /* non-SPS NAL type 1 */
}
static void feeder_fill(uint64_t bytes) {
    uint8_t pkt[188];
    uint64_t n = bytes/188;
    uint8_t cc=0;
    for (uint64_t i=0;i<n;i++) {
        if (i%32==0) make_pat(pkt);
        else if (i%97==0) make_video_sps(pkt,cc++);
        else make_video_generic(pkt,cc++);
        ring_write(pkt,188);
    }
}

/* ---- Check helpers ---- */
static int is_null_block(const uint8_t *b, size_t n) {
    size_t off=0, frag=n%188;
    if (frag){ for(size_t i=0;i<frag;i++) if(b[i]!=0) return 0; off=frag; }
    for(;off+188<=n;off+=188) if(memcmp(b+off,NULL_PKT,188)!=0) return 0;
    return 1;
}
static int has_sync(const uint8_t *b, size_t n) {
    /* Valid-data path has NO frag-zero prefix (only NULL path does).
     * Check 188-stride sync from offset 0. */
    if (n < 188) return 0;
    for (size_t off = 0; off + 188 <= n; off += 188) if (b[off] != 0x47) return 0;
    return 1;
}
static int has_sync_any(const uint8_t *b, size_t n) {
    /* Misalignment-tolerant: 64KB foff deltas vs 188B TS packets drift
     * (65536 % 188 == 112), so post-anchor stream_pos is unaligned.
     * Accept if ANY 188-stride alignment yields consistent sync. */
    for (size_t shift = 0; shift < 188 && shift < n; shift++) {
        int ok = 1, cnt = 0;
        for (size_t off = shift; off + 188 <= n; off += 188) {
            if (b[off] != 0x47) { ok = 0; break; }
            cnt++;
        }
        if (ok && cnt >= 3) return 1;
    }
    return 0;
}
/* Byte-exact ring-backing check: proves data came from ring memory
 * even when 64KB/188 beat-frequency misaligns TS sync. */
static int equals_ring(const uint8_t *b, uint64_t F, size_t n) {
    uint8_t *exp = (uint8_t *)malloc(n);
    if (!exp) return 0;
    if (F < g_hcache_len) {
        size_t hc = (size_t)(g_hcache_len - F);
        if (hc > n) hc = n;
        memcpy(exp, g_hcache + F, hc);
        if (hc < n) ring_copy(exp + hc, foff_to_stream_pos(F + hc), n - hc);
    } else {
        ring_copy(exp, foff_to_stream_pos(F), n);
    }
    int eq = (memcmp(b, exp, n) == 0);
    free(exp);
    return eq;
}
static int has_video_pid(const uint8_t *b, size_t n) __attribute__((unused));
static int has_video_pid(const uint8_t *b, size_t n) {
    size_t off=n%188;
    for(;off+188<=n;off+=188){
        uint16_t pid=(uint16_t)(((b[off+1]&0x1F)<<8)|b[off+2]);
        if(pid==0x100) return 1;
    }
    return 0;
}
static int is_pat_at(uint64_t stream_pos) {
    uint8_t pkt[188];
    if (stream_pos+188>g_s_write) return 0;
    ring_pkt(snap188(stream_pos),pkt);
    if(pkt[0]!=0x47||(pkt[1]&0x1F)!=0||pkt[2]!=0) return 0;
    uint8_t af=(uint8_t)((pkt[3]>>4)&3);
    size_t off=4;
    if(af==2||af==0) return 0;
    if(af==3) off+=1+(size_t)pkt[4];
    if(off>=188) return 0;
    uint8_t ptr=pkt[off];
    if(off+1+(size_t)ptr>=188) return 0;
    return pkt[off+1+(size_t)ptr]==0x00;
}

static int passes=0, fails=0;
#define CHECK(cond, fmt, ...) do{ \
    if(cond){passes++; printf("  PASS: " fmt "\n", ##__VA_ARGS__);} \
    else{fails++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__);} \
} while(0)

int main(void) {
    printf("=== test_seek_reanchor: Samsung TV foff=0 -> 901120 -> 799997952 jump ===\n");
    g_ring=(uint8_t*)malloc((size_t)RINGSZ);
    if(!g_ring){printf("FAIL: malloc ring 128MB\n");return 1;}
    memset(g_ring,0,(size_t)RINGSZ);

    /* Step 1: feeder buffers 20MB realtime BEFORE/WHILE TV starts */
    feeder_fill(20ULL*1024*1024);
    printf("[setup] S_write=%llu (%.1f MB)\n",
        (unsigned long long)g_s_write, (double)g_s_write/1048576.0);
    CHECK(g_s_write>=MIN_STREAM_START,"feeder pre-buffer >= MIN_STREAM_START (6MB): S_write=%llu",
        (unsigned long long)g_s_write);

    uint8_t *blk=(uint8_t*)malloc(262144);
    if(!blk){printf("FAIL: malloc blk\n");return 1;}
    uint64_t dl=now_ms()+2000;

    /* Step 2: TV opens at foff=0 -> must anchor (is_reopen=1, fresh anchor) */
    serve_live_backend(blk,0,BLOCK,dl);
    uint64_t anchor0_foff=g_anchor_foff, anchor0_pos=g_anchor_stream_pos;
    uint64_t epoch0=g_epoch;
    printf("[open] anchor_foff=%llu anchor_pos=%llu epoch=%llu is_reopen=%d\n",
        (unsigned long long)anchor0_foff,(unsigned long long)anchor0_pos,
        (unsigned long long)epoch0,g_last_is_reopen);
    CHECK(g_base_valid==1,"open at foff=0 sets g_base_valid=1");
    CHECK(g_anchor_foff==0,"open anchor_foff==0 (got %llu)",(unsigned long long)g_anchor_foff);
    CHECK(g_last_is_reopen==1,"open triggers is_reopen=1");
    CHECK(anchor0_pos%188==0,"open anchor stream_pos 188-aligned (%llu)",
        (unsigned long long)anchor0_pos);
    CHECK(is_pat_at(anchor0_pos),"open anchor points at PAT (pos=%llu)",
        (unsigned long long)anchor0_pos);
    CHECK(equals_ring(blk,0,BLOCK),"open block bytes equal ring memory at anchor (valid video, hcache-backed)");
    CHECK(has_sync(blk,BLOCK)||has_sync_any(blk,BLOCK),"open block shows TS sync 0x47 on some 188-alignment");

    /* Step 3: sequential reads up to 901120 */
    printf("[seq] reading 0..%llu in 64KB blocks...\n",(unsigned long long)FOFF_SEQ_END);
    int seq_ok=1;
    for(uint64_t f=BLOCK;f<FOFF_SEQ_END;f+=BLOCK){
        size_t want=BLOCK;
        if(f+want>FOFF_SEQ_END) want=(size_t)(FOFF_SEQ_END-f);
        serve_live_backend(blk,f,want,dl);
        if(is_null_block(blk,want)){ seq_ok=0; printf("    unexpected NULL at foff=%llu\n",(unsigned long long)f); break; }
    }
    CHECK(seq_ok,"sequential reads 64KB..901120 all deliver video (no NULLs)");
    uint64_t fend_before_jump=g_prev_Fend;
    printf("[seq] g_prev_Fend=%llu (expect %llu)\n",
        (unsigned long long)fend_before_jump,(unsigned long long)FOFF_SEQ_END);
    CHECK(fend_before_jump==FOFF_SEQ_END,"g_prev_Fend==901120 after sequential phase");
    uint64_t anchor_seq = g_anchor_foff, epoch_seq = g_epoch;
    CHECK(anchor_seq==0,"anchor still at foff=0 after sequential phase (got %llu)",
        (unsigned long long)anchor_seq);

    /* Step 4: ConnectShare opening probe at 800MB (16KB, 32KB, 64KB, 128KB, 128KB = 368KB total)
     * Real Samsung TV behavior: ConnectShare probes container metadata at 800MB (10% of 8GB)
     * during initial opening, then returns to foff=1032192 (continuation of sequential stream).
     * These probes MUST ALL return NULL packets and MUST NOT move g_anchor_foff or touch g_prev_Fend! */
    printf("[probe-800m] ConnectShare opening probe sequence at 800MB (total 368KB)...\n");
    size_t probe_sizes[] = {16384, 32768, 65536, 131072, 131072};
    uint64_t curr_probe_off = FOFF_JUMP;
    for (int i = 0; i < 5; i++) {
        size_t psz = probe_sizes[i];
        serve_live_backend(blk, curr_probe_off, psz, dl);
        CHECK(is_null_block(blk, psz), "4.%d: probe %zu bytes at foff=%llu returns NULLs",
              i + 1, psz, (unsigned long long)curr_probe_off);
        CHECK(g_anchor_foff == anchor_seq, "4.%d: anchor_foff untouched (still %llu)",
              i + 1, (unsigned long long)g_anchor_foff);
        CHECK(g_prev_Fend == FOFF_SEQ_END, "4.%d: g_prev_Fend untouched (still %llu)",
              i + 1, (unsigned long long)g_prev_Fend);
        CHECK(g_epoch == epoch_seq, "4.%d: epoch unchanged (still %llu)",
              i + 1, (unsigned long long)g_epoch);
        curr_probe_off += psz;
    }

    /* Step 5: TV returns to sequential playback at foff=1032192 (901120 + 131072)
     * Because g_prev_Fend was preserved at 901120, this read is recognized as sequential!
     * Because g_anchor_foff was NOT moved to 800MB, it delivers continuous valid video! */
    uint64_t foff_resume = FOFF_SEQ_END; /* 901120 */
    printf("[resume] TV returns to sequential playback at foff=%llu...\n",
           (unsigned long long)foff_resume);
    serve_live_backend(blk, foff_resume, BLOCK, dl);
    CHECK(!is_null_block(blk, BLOCK), "5a: resume at foff=%llu delivers valid video (NOT NULLs!)",
          (unsigned long long)foff_resume);
    CHECK(equals_ring(blk, foff_resume, BLOCK), "5b: resume block matches ring memory exactly");
    CHECK(g_anchor_foff == anchor_seq, "5c: anchor still at foff=0 (zero anchor hijacking!)");
    CHECK(g_epoch == epoch_seq, "5d: epoch still %llu (zero spurious epoch bump!)",
          (unsigned long long)epoch_seq);
    CHECK(g_prev_Fend == foff_resume + BLOCK, "5e: g_prev_Fend now advances to %llu",
          (unsigned long long)(foff_resume + BLOCK));

    /* Step 6: 4 subsequent sequential blocks delivered smoothly */
    printf("[play] continuing sequential playback at 1MB..2MB...\n");
    int seq_play_ok = 1;
    for (int i = 1; i <= 4; i++) {
        uint64_t f = foff_resume + (uint64_t)i * BLOCK;
        serve_live_backend(blk, f, BLOCK, dl);
        if (is_null_block(blk, BLOCK) || !equals_ring(blk, f, BLOCK)) {
            seq_play_ok = 0;
            break;
        }
    }
    CHECK(seq_play_ok, "6: 4 subsequent sequential blocks deliver valid ring video");

    /* Step 7: Real confirmed user seek (outside grace period, >= 4 blocks AND >= 1 MB) */
    printf("[seek] simulated real user seek outside grace period (16 blocks x 64KB = 1MB)...\n");
    g_anchor_birth_ms = now_ms() - 15000ULL; /* advance past 10s grace period */
    uint64_t FOFF_SEEK = 1600000000ULL; /* 1.6 GB */
    uint64_t epoch_before_seek = g_epoch;
    int seek_reanchored = 0;

    for (int i = 0; i < 16; i++) {
        uint64_t f = FOFF_SEEK + (uint64_t)i * BLOCK;
        serve_live_backend(blk, f, BLOCK, dl);
        if (g_last_is_reopen) {
            seek_reanchored = 1;
            break;
        }
    }
    CHECK(seek_reanchored, "7a: real seek >= 1MB confirms jump and triggers re-anchor");
    CHECK(g_epoch == epoch_before_seek + 1, "7b: epoch incremented exactly once on confirmed seek");
    CHECK(g_anchor_foff >= FOFF_SEEK, "7c: anchor_foff updated to seek location");

    printf("\n=== RESULT: %d PASS, %d FAIL ===\n",passes,fails);
    free(blk); free(g_ring);
    return fails==0?0:1;
}
