#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/tcp.h>

#define TS_PKT_SZ 188

static volatile sig_atomic_t running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

/* Accumulator for strict 188-byte MPEG-TS packet alignment (135 KB in BSS) */
static uint8_t g_acc[131072 + 4096];
static size_t g_acc_len = 0;
static uint64_t g_total_valid_bytes = 0;
static uint64_t g_total_resync_drops = 0;

static int64_t g_last_heartbeat = 0;

/* BSS buffers to keep stack footprint strictly < 4KB on ARM32 */
static uint8_t g_raw_buf[65536];
static char g_header_buf[4096];

/* DNS cache and resolution state */
static struct sockaddr_in g_cached_saddr;
static int g_saddr_cached = 0;
static int g_dns_fails = 0;

static int resolve_server_address(const char *host, int port, struct sockaddr_in *out_saddr) {
    if (g_saddr_cached && g_dns_fails < 5) {
        *out_saddr = g_cached_saddr;
        return 0;
    }

    memset(&g_cached_saddr, 0, sizeof(g_cached_saddr));
    g_cached_saddr.sin_family = AF_INET;
    g_cached_saddr.sin_port = htons((uint16_t)port);

    /* 1. Tenta IP numérico direto (zero overhead de rede) */
    if (inet_pton(AF_INET, host, &g_cached_saddr.sin_addr) == 1) {
        g_saddr_cached = 1;
        g_dns_fails = 0;
        *out_saddr = g_cached_saddr;
        return 0;
    }

    /* 2. Resolução estruturada via getaddrinfo */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)res->ai_addr;
        g_cached_saddr.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
        g_saddr_cached = 1;
        g_dns_fails = 0;
        *out_saddr = g_cached_saddr;
        return 0;
    }
    if (res) freeaddrinfo(res);

    /* 3. Fallback de IP seguro se DNS do Wi-Fi falhar */
    fprintf(stderr, "[!] Falha no DNS para %s, usando fallback de IP seguro\n", host);
    g_cached_saddr.sin_addr.s_addr = inet_addr("129.146.5.64");
    g_saddr_cached = 1;
    *out_saddr = g_cached_saddr;
    return 0;
}

static int feed_ts_bytes(int fifo_fd, const uint8_t *data, size_t len) {
    if (len == 0) return 0;

    /* CRÍTICO 4: Se o acumulador for estourar, compacta mantendo os últimos pacotes
     * para re-sincronismo em vez de descartar cegamente tudo (o que causa perda de PCR). */
    if (g_acc_len + len > sizeof(g_acc)) {
        size_t keep = (g_acc_len > TS_PKT_SZ * 4) ? (TS_PKT_SZ * 4) : g_acc_len;
        if (keep > 0) {
            memmove(g_acc, g_acc + (g_acc_len - keep), keep);
        }
        g_acc_len = keep;
        if (g_acc_len + len > sizeof(g_acc)) {
            len = sizeof(g_acc) - g_acc_len;
        }
    }
    memcpy(g_acc + g_acc_len, data, len);
    g_acc_len += len;

    size_t processed = 0;
    while (processed + TS_PKT_SZ <= g_acc_len && running) {
        if (g_acc[processed] == 0x47) {
            /* Conta pacotes TS contíguos válidos */
            size_t pkt_end = processed;
            while (pkt_end + TS_PKT_SZ <= g_acc_len && g_acc[pkt_end] == 0x47) {
                pkt_end += TS_PKT_SZ;
            }
            size_t valid_len = pkt_end - processed;
            size_t written = 0;
            while (written < valid_len && running) {
                /* ALTO 2: Poll com 2.5s no FIFO antes de escrever para evitar travar o socket TCP */
                struct pollfd wf = { .fd = fifo_fd, .events = POLLOUT, .revents = 0 };
                int pret = poll(&wf, 1, 2500);
                if (pret <= 0) {
                    if (pret < 0 && errno == EINTR) {
                        if (!running) break;
                        continue;
                    }
                    /* Backpressure do leitor FUSE: pipe cheio > 2.5s */
                    return -1;
                }

                ssize_t w = write(fifo_fd, g_acc + processed + written, valid_len - written);
                if (w < 0) {
                    if (errno == EINTR) {
                        if (!running) break;
                        continue;
                    }
                    if (errno == EPIPE) return -1; /* Leitor FUSE encerrou */
                    perror("write fifo");
                    return -1;
                }
                written += (size_t)w;
            }
            g_total_valid_bytes += written;

            /* ALTO 3: Heartbeat via futimens (zero desgaste de flash NAND eMMC) */
            struct timespec ts_hb;
            clock_gettime(CLOCK_MONOTONIC, &ts_hb);
            if (ts_hb.tv_sec - g_last_heartbeat >= 2) {
                int hb_fd = open("/data/local/tmp/fetcher_heartbeat.ts", O_CREAT | O_WRONLY, 0644);
                if (hb_fd >= 0) {
                    futimens(hb_fd, NULL);
                    close(hb_fd);
                }
                g_last_heartbeat = ts_hb.tv_sec;
            }

            processed = pkt_end;
        } else {
            /* Sincronismo perdido: procura próximo 0x47 */
            size_t search = processed + 1;
            int found = 0;
            while (search < g_acc_len) {
                if (g_acc[search] == 0x47) {
                    if (search + TS_PKT_SZ < g_acc_len) {
                        if (g_acc[search + TS_PKT_SZ] == 0x47) {
                            found = 1;
                            break;
                        }
                    } else {
                        found = 1;
                        break;
                    }
                }
                search++;
            }
            if (found) {
                size_t dropped = search - processed;
                g_total_resync_drops += dropped;
                fprintf(stderr, "[!] Resynced TS stream: dropped %zu misaligned bytes\n", dropped);
                processed = search;
            } else {
                /* CRÍTICO 4: Se nenhum 0x47 foi encontrado no buffer restante,
                 * descarta os múltiplos de 188B desalinhados mas PRESERVA o fragmento de cauda (<188B)
                 * para que o próximo recv complete o pacote sem perder a continuidade de PTS/PCR! */
                if (g_acc_len - processed >= TS_PKT_SZ) {
                    size_t keep_tail = (g_acc_len - processed) % TS_PKT_SZ;
                    size_t dropped = (g_acc_len - processed) - keep_tail;
                    g_total_resync_drops += dropped;
                    processed += dropped;
                }
                break;
            }
        }
    }

    if (processed > 0) {
        size_t rem = g_acc_len - processed;
        if (rem > 0) {
            memmove(g_acc, g_acc + processed, rem);
        }
        g_acc_len = rem;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *host = "tv.smre.run.place";
    int port = 80;
    const char *path = "/stream";
    const char *fifo_path = "/data/local/tmp/live_pipe";

    if (argc > 1) fifo_path = argv[1];
    if (argc > 2) host = argv[2];
    if (argc > 3) port = atoi(argv[3]);
    if (argc > 4) path = argv[4];

    /* 1. Single-instance flock protection com FD_CLOEXEC */
    int lock_fd = open("/data/local/tmp/stream_fetcher.lock", O_CREAT | O_RDWR, 0660);
    if (lock_fd >= 0) {
        fcntl(lock_fd, F_SETFD, FD_CLOEXEC);
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            fprintf(stderr, "[!] Another stream_fetcher is already running (locked). Exiting.\n");
            return 0;
        }
        char pid_str[32];
        int plen = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
        if (ftruncate(lock_fd, 0) == 0) {
            ssize_t pw = pwrite(lock_fd, pid_str, (size_t)plen, 0);
            (void)pw;
        }
    }

    /* 2. Self-sufficient FIFO creation com permissões seguras (0660) */
    struct stat st;
    if (stat(fifo_path, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        unlink(fifo_path);
        if (mkfifo(fifo_path, 0660) == 0) {
            printf("[✓] Self-created FIFO pipe: %s\n", fifo_path);
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* NO SA_RESTART: allow EINTR on blocking open/write to terminate cleanly */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    setlinebuf(stdout);
    setlinebuf(stderr);

    printf("[*] stream_fetcher v2 starting: target %s:%d%s -> fifo %s\n", host, port, path, fifo_path);

    while (running) {
        printf("[*] Opening FIFO %s (waiting for reader)...\n", fifo_path);
        int fifo_fd = -1;
        while (running) {
            /* ALTO 1: Open com O_NONBLOCK para não prender processo em D-state no boot */
            fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
            if (fifo_fd >= 0) {
                int fl = fcntl(fifo_fd, F_GETFL, 0);
                if (fl >= 0) {
                    fcntl(fifo_fd, F_SETFL, fl & ~O_NONBLOCK);
                }
                break;
            }
            if (errno == ENXIO) {
                /* Leitor FUSE ainda não abriu o pipe */
                usleep(250000);
                continue;
            }
            if (errno == EINTR) break;
            perror("open fifo");
            sleep(1);
        }
        if (!running || fifo_fd < 0) break;

        printf("[✓] FIFO connected!\n");
        g_acc_len = 0;

        while (running) {
            struct sockaddr_in saddr;
            resolve_server_address(host, port, &saddr);

            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                perror("socket");
                sleep(2);
                continue;
            }

            /* CRÍTICO 2: Timeouts estritos de 3s para garantir recuperação antes dos 3.5s do MStar */
            struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            struct timeval snd_tv = { .tv_sec = 3, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

            int rcvbuf = 512 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

            printf("[*] Connecting to %s:%d...\n", inet_ntoa(saddr.sin_addr), port);
            if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
                perror("connect");
                close(sock);
                g_dns_fails++;
                sleep(2);
                continue;
            }

            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#ifdef TCP_KEEPIDLE
            int idle = 3;
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
            int intvl = 2;
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
            int cnt = 3;
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif

            /* Request HTTP/1.0 to disable Transfer-Encoding: chunked */
            char req[512];
            if (port == 80) {
                snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "User-Agent: USBStreamTV/2.0\r\n"
                         "Connection: close\r\n\r\n",
                         path, host);
            } else {
                snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s:%d\r\n"
                         "User-Agent: USBStreamTV/2.0\r\n"
                         "Connection: close\r\n\r\n",
                         path, host, port);
            }
            if (send(sock, req, strlen(req), 0) < 0) {
                perror("send");
                close(sock);
                g_dns_fails++;
                sleep(2);
                continue;
            }

            printf("[✓] HTTP request sent. Reading response headers...\n");

            /* CRÍTICO 1: Leitura bulk de cabeçalhos HTTP (elimina ~4000 syscalls) */
            size_t hlen = 0;
            char *hdr_end = NULL;

            while (hlen < sizeof(g_header_buf) - 1 && running) {
                struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
                int pret = poll(&pfd, 1, 2500);
                if (pret <= 0) {
                    if (pret < 0 && errno == EINTR) continue;
                    break;
                }
                ssize_t r = recv(sock, g_header_buf + hlen, sizeof(g_header_buf) - 1 - hlen, 0);
                if (r <= 0) {
                    if (r < 0 && errno == EINTR) continue;
                    break;
                }
                size_t scan = (hlen >= 3) ? (hlen - 3) : 0;
                hlen += (size_t)r;
                g_header_buf[hlen] = '\0';
                hdr_end = strstr(g_header_buf + scan, "\r\n\r\n");
                if (hdr_end) break;
            }

            if (!hdr_end) {
                fprintf(stderr, "[!] Failed to read HTTP headers (timeout/EOF). Reconnecting...\n");
                close(sock);
                g_dns_fails++;
                sleep(1);
                continue;
            }

            *hdr_end = '\0';
            if (strncmp(g_header_buf, "HTTP/1.", 7) != 0 ||
                (strstr(g_header_buf, " 200 ") == NULL && strstr(g_header_buf, " 200\r\n") == NULL)) {
                fprintf(stderr, "[!] Bad HTTP response status (not 200 OK). Reconnecting...\n");
                close(sock);
                g_dns_fails++;
                sleep(2);
                continue;
            }

            /* Conexão confirmada com sucesso: zera falhas */
            g_dns_fails = 0;
            g_acc_len = 0;

            int is_chunked = 0;
            if (strstr(g_header_buf, "Transfer-Encoding: chunked") ||
                strstr(g_header_buf, "transfer-encoding: chunked")) {
                is_chunked = 1;
                printf("[!] Warning: Server returned chunked transfer encoding, parsing chunks...\n");
            }

            /* CRÍTICO 1: Repassa dados do payload MPEG-TS já recebidos no buffer de cabeçalho! */
            char *body_start = hdr_end + 4;
            size_t body_len = hlen - (size_t)(body_start - g_header_buf);
            if (body_len > 0 && !is_chunked) {
                if (feed_ts_bytes(fifo_fd, (const uint8_t *)body_start, body_len) < 0) {
                    close(sock);
                    close(fifo_fd);
                    fifo_fd = -1;
                    break;
                }
            }

            printf("[✓] Stream connected! Forwarding aligned MPEG-TS packets into FIFO...\n");

            struct timespec ts_log;
            clock_gettime(CLOCK_MONOTONIC, &ts_log);
            int64_t last_log = ts_log.tv_sec;
            int stream_err = 0;

            if (!is_chunked) {
                /* Fast raw stream path */
                while (running) {
                    struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
                    int pret = poll(&pfd, 1, 2500);
                    if (pret <= 0) {
                        if (pret < 0 && errno == EINTR) continue;
                        fprintf(stderr, "[!] Socket timeout (2.5s without data from server). Reconnecting...\n");
                        close(sock);
                        sock = -1;
                        g_dns_fails++;
                        break;
                    }

                    ssize_t n = recv(sock, g_raw_buf, sizeof(g_raw_buf), 0);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            fprintf(stderr, "[!] Socket timeout. Reconnecting...\n");
                        } else {
                            fprintf(stderr, "[!] Server connection closed (%s)\n", strerror(errno));
                        }
                        g_dns_fails++;
                        break;
                    }
                    if (n == 0) {
                        fprintf(stderr, "[!] Server connection closed cleanly. Reconnecting...\n");
                        break;
                    }

                    if (feed_ts_bytes(fifo_fd, g_raw_buf, (size_t)n) < 0) {
                        stream_err = 1;
                        break;
                    }

                    struct timespec ts_now;
                    clock_gettime(CLOCK_MONOTONIC, &ts_now);
                    if (ts_now.tv_sec - last_log >= 10) {
                        printf("[*] Streaming active: %.2f MB valid TS (resync drops: %llu bytes)\n",
                               (double)g_total_valid_bytes / (1024.0 * 1024.0),
                               (unsigned long long)g_total_resync_drops);
                        last_log = ts_now.tv_sec;
                    }
                }
            } else {
                /* Chunked stream decoder path */
                while (running) {
                    /* Read chunk size in hex */
                    char chunk_sz_str[32];
                    size_t csi = 0;
                    int chunked_timeout = 0;
                    while (csi < sizeof(chunk_sz_str) - 1 && running) {
                        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
                        int pret = poll(&pfd, 1, 2500);
                        if (pret <= 0) {
                            if (pret < 0 && errno == EINTR) continue;
                            fprintf(stderr, "[!] Socket timeout (2.5s without data from server). Reconnecting...\n");
                            close(sock);
                            sock = -1;
                            chunked_timeout = 1;
                            break;
                        }

                        char c;
                        ssize_t n = recv(sock, &c, 1, 0);
                        if (n <= 0) break;
                        if (c == '\n') break;
                        if (c != '\r') chunk_sz_str[csi++] = c;
                    }
                    if (chunked_timeout || sock < 0) break;

                    chunk_sz_str[csi] = '\0';
                    long chunk_sz = strtol(chunk_sz_str, NULL, 16);
                    if (chunk_sz <= 0) {
                        fprintf(stderr, "[!] Chunked stream end (%ld). Reconnecting...\n", chunk_sz);
                        break;
                    }

                    /* Read exactly chunk_sz bytes */
                    size_t chunk_read = 0;
                    while (chunk_read < (size_t)chunk_sz && running) {
                        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
                        int pret = poll(&pfd, 1, 2500);
                        if (pret <= 0) {
                            if (pret < 0 && errno == EINTR) continue;
                            fprintf(stderr, "[!] Socket timeout (2.5s without data from server). Reconnecting...\n");
                            close(sock);
                            sock = -1;
                            chunked_timeout = 1;
                            break;
                        }

                        size_t to_read = (size_t)chunk_sz - chunk_read;
                        if (to_read > sizeof(g_raw_buf)) to_read = sizeof(g_raw_buf);
                        ssize_t n = recv(sock, g_raw_buf, to_read, 0);
                        if (n <= 0) {
                            if (n < 0 && errno == EINTR) continue;
                            break;
                        }
                        if (feed_ts_bytes(fifo_fd, g_raw_buf, (size_t)n) < 0) {
                            stream_err = 1;
                            break;
                        }
                        chunk_read += (size_t)n;
                    }
                    if (stream_err || chunk_read < (size_t)chunk_sz || chunked_timeout || sock < 0) break;

                    /* Consume trailing \r\n */
                    struct pollfd pfd_crlf = { .fd = sock, .events = POLLIN, .revents = 0 };
                    int pret = poll(&pfd_crlf, 1, 2500);
                    if (pret <= 0) {
                        if (pret < 0 && errno == EINTR) continue;
                        fprintf(stderr, "[!] Socket timeout (2.5s without data from server). Reconnecting...\n");
                        close(sock);
                        sock = -1;
                        break;
                    }
                    if (pret > 0) {
                        char crlf[2];
                        recv(sock, crlf, 2, MSG_WAITALL);
                    }

                    struct timespec ts_now;
                    clock_gettime(CLOCK_MONOTONIC, &ts_now);
                    if (ts_now.tv_sec - last_log >= 10) {
                        printf("[*] Chunked streaming: %.2f MB valid TS\n",
                               (double)g_total_valid_bytes / (1024.0 * 1024.0));
                        last_log = ts_now.tv_sec;
                    }
                }
            }

            if (sock >= 0) close(sock);
            if (stream_err) {
                close(fifo_fd);
                fifo_fd = -1;
                break; /* Reopen FIFO */
            }
            if (!running) break;
            sleep(1);
        }
        if (fifo_fd >= 0) close(fifo_fd);
    }
    printf("[*] stream_fetcher exiting cleanly.\n");
    return 0;
}
