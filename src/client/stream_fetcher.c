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
/* 21 pacotes TS = 3948 bytes <= PIPE_BUF (4096 no Linux).
 * Garante writes atômicos no kernel que nunca partem pacotes TS. */
#define TS_WR_CHUNK (TS_PKT_SZ * 21)

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

static volatile sig_atomic_t running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

/* Accumulator para alinhamento estrito MPEG-TS de 188 bytes (68 KB no BSS) */
static uint8_t g_acc[65536 + 4096];
static size_t g_acc_len = 0;
static uint64_t g_total_valid_bytes = 0;
static uint64_t g_total_resync_drops = 0;

static int64_t g_last_heartbeat = 0;

/* BSS buffers para manter a pilha estritamente < 4KB no Cortex-A7 */
static uint8_t g_raw_buf[65536];
static char g_header_buf[4096];

/* DNS cache e controle de conexão */
static struct sockaddr_in g_cached_saddr;
static int g_saddr_cached = 0;
static int g_connect_fails = 0;

static int resolve_server_address(const char *host, int port, struct sockaddr_in *out_saddr) {
    if (g_saddr_cached && g_connect_fails < 3) {
        *out_saddr = g_cached_saddr;
        return 0;
    }

    struct sockaddr_in tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.sin_family = AF_INET;
    tmp.sin_port = htons((uint16_t)port);

    /* 1. Tenta IP numérico direto (zero overhead de rede e sem dependência de DNS) */
    if (inet_pton(AF_INET, host, &tmp.sin_addr) == 1) {
        g_cached_saddr = tmp;
        g_saddr_cached = 1;
        g_connect_fails = 0;
        *out_saddr = g_cached_saddr;
        return 0;
    }

    /* 2. Resolução estruturada via getaddrinfo numa variável local */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)res->ai_addr;
        tmp.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
        g_cached_saddr = tmp;
        g_saddr_cached = 1;
        g_connect_fails = 0;
        *out_saddr = g_cached_saddr;
        return 0;
    }
    if (res) freeaddrinfo(res);

    /* 3. Se falhou o getaddrinfo mas já tínhamos um IP válido em cache, preserva o IP bom */
    if (g_saddr_cached) {
        fprintf(stderr, "[!] Falha no DNS para %s, mantendo IP em cache: %s\n",
                host, inet_ntoa(g_cached_saddr.sin_addr));
        *out_saddr = g_cached_saddr;
        return 0;
    }

    /* 4. Fallback configurável (via SERVER_IP ou default) somente se nunca houve resolução */
    const char *fallback_ip = getenv("SERVER_IP");
    if (!fallback_ip || strlen(fallback_ip) == 0) {
        fallback_ip = "129.146.5.64";
    }
    fprintf(stderr, "[!] Falha no DNS para %s sem cache prévio. Usando fallback: %s\n", host, fallback_ip);
    tmp.sin_addr.s_addr = inet_addr(fallback_ip);
    g_cached_saddr = tmp;
    g_saddr_cached = 1;
    *out_saddr = g_cached_saddr;
    return 0;
}

static int feed_ts_bytes(int fifo_fd, const uint8_t *data, size_t len) {
    if (len == 0) return 0;

    /* Compacta acumulador mantendo os últimos pacotes se necessário */
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

            /* Escreve no FIFO não-bloqueante em blocos atômicos de 3948B (21 pacotes <= PIPE_BUF) */
            while (written < valid_len && running) {
                size_t n = valid_len - written;
                if (n > TS_WR_CHUNK) n = TS_WR_CHUNK;

                struct pollfd wf = { .fd = fifo_fd, .events = POLLOUT, .revents = 0 };
                int pret = poll(&wf, 1, 2000); /* 2.0s timeout: preserva o orçamento de recuperação da TV */
                if (pret < 0) {
                    if (errno == EINTR) {
                        if (!running) break;
                        continue;
                    }
                    return -1;
                }
                if (pret == 0) {
                    fprintf(stderr, "[!] FIFO write backpressure (>2.0s sem dreno pelo FUSE)\n");
                    return -1;
                }
                if (wf.revents & (POLLERR | POLLHUP)) {
                    fprintf(stderr, "[!] FIFO leitor desconectado (POLLHUP/POLLERR)\n");
                    return -1;
                }

                ssize_t w = write(fifo_fd, g_acc + processed + written, n);
                if (w < 0) {
                    if (errno == EINTR) {
                        if (!running) break;
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    if (errno == EPIPE) return -1;
                    perror("write fifo");
                    return -1;
                }
                written += (size_t)w;
            }
            g_total_valid_bytes += written;

            /* Heartbeat atômico via futimens (zero desgaste de flash NAND eMMC) */
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
                /* Nenhum 0x47 válido encontrado: descarta múltiplos de 188B e guarda cauda para o próximo recv */
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

    const char *env_host = getenv("SERVER_HOST");
    if (env_host && strlen(env_host) > 0) host = env_host;

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
    } else {
        perror("[!] Warning: open stream_fetcher.lock failed");
    }

    /* 2. Criação autônoma do FIFO com permissões seguras (0660) */
    struct stat st;
    if (stat(fifo_path, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        unlink(fifo_path);
        if (mkfifo(fifo_path, 0660) == 0) {
            printf("[✓] Self-created FIFO pipe: %s\n", fifo_path);
        } else {
            perror("[!] mkfifo failed");
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* NO SA_RESTART: allow EINTR on poll/write to terminate cleanly */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    setlinebuf(stdout);
    setlinebuf(stderr);

    printf("[*] stream_fetcher v2 starting: target %s:%d%s -> fifo %s\n", host, port, path, fifo_path);

    int consecutive_connect_fails = 0;

    while (running) {
        printf("[*] Opening FIFO %s (waiting for reader)...\n", fifo_path);
        int fifo_fd = -1;
        while (running) {
            /* Abre com O_NONBLOCK e MANTÉM O_NONBLOCK para writes nunca bloquearem em kernel */
            fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
            if (fifo_fd >= 0) {
                /* Expande capacidade do pipe para 1MB (Linux 2.6.35+) para amortecer jitter */
                fcntl(fifo_fd, F_SETPIPE_SZ, 1024 * 1024);
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
                consecutive_connect_fails++;
                usleep(500000);
                continue;
            }

            /* Timeouts estritos de 2.0s para garantir recuperação dentro dos 3.5s do MStar */
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            int rcvbuf = 512 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

            printf("[*] Connecting to %s:%d...\n", inet_ntoa(saddr.sin_addr), port);
            if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
                perror("connect");
                close(sock);
                g_connect_fails++;
                consecutive_connect_fails++;
                if (consecutive_connect_fails > 2) {
                    usleep(500000);
                } else {
                    usleep(100000);
                }
                continue;
            }

            consecutive_connect_fails = 0;

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

            /* Request HTTP/1.0 para entrega direta de MPEG-TS puro */
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
                g_connect_fails++;
                continue;
            }

            printf("[✓] HTTP request sent. Reading response headers...\n");

            /* Leitura bulk de cabeçalhos HTTP (zero stall de syscalls) */
            size_t hlen = 0;
            char *hdr_end = NULL;

            while (hlen < sizeof(g_header_buf) - 1 && running) {
                struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
                int pret = poll(&pfd, 1, 2000);
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
                g_connect_fails++;
                continue;
            }

            *hdr_end = '\0';
            if (strncmp(g_header_buf, "HTTP/1.", 7) != 0 ||
                strncmp(g_header_buf + 8, " 200", 4) != 0) {
                fprintf(stderr, "[!] Bad HTTP response status (not 200 OK). Reconnecting...\n");
                close(sock);
                g_connect_fails++;
                usleep(250000);
                continue;
            }

            /* Tratamento estrito de chunked: rejeita e reconecta */
            if (strstr(g_header_buf, "chunked") || strstr(g_header_buf, "Chunked")) {
                fprintf(stderr, "[!] Servidor retornou Transfer-Encoding chunked para HTTP/1.0. Reconectando...\n");
                close(sock);
                g_connect_fails++;
                usleep(250000);
                continue;
            }

            /* Sucesso de conexão: zera contador de falhas */
            g_connect_fails = 0;
            g_acc_len = 0;

            /* Repassa payload MPEG-TS já recebido no buffer de cabeçalhos */
            char *body_start = hdr_end + 4;
            size_t body_len = hlen - (size_t)(body_start - g_header_buf);
            if (body_len > 0) {
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

            while (running) {
                struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
                int pret = poll(&pfd, 1, 2000);
                if (pret <= 0) {
                    if (pret < 0 && errno == EINTR) continue;
                    fprintf(stderr, "[!] Socket timeout (2.0s sem dados). Reconectando...\n");
                    close(sock);
                    sock = -1;
                    break;
                }

                ssize_t n = recv(sock, g_raw_buf, sizeof(g_raw_buf), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        fprintf(stderr, "[!] Socket recv timeout. Reconectando...\n");
                    } else {
                        fprintf(stderr, "[!] Server connection closed (%s)\n", strerror(errno));
                    }
                    break;
                }
                if (n == 0) {
                    fprintf(stderr, "[!] Server connection closed cleanly. Reconectando...\n");
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

            if (sock >= 0) close(sock);
            if (stream_err) {
                close(fifo_fd);
                fifo_fd = -1;
                break; /* Reabre FIFO */
            }
            if (!running) break;
            /* Reconexão imediata sem sleep(1) fixo */
        }
        if (fifo_fd >= 0) close(fifo_fd);
    }
    printf("[*] stream_fetcher exiting cleanly.\n");
    return 0;
}
