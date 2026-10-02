#include <stdio.h>
#include <stdlib.h>
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

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

/* Accumulator for strict 188-byte MPEG-TS packet alignment */
static uint8_t g_acc[131072 + 4096];
static size_t g_acc_len = 0;
static uint64_t g_total_valid_bytes = 0;
static uint64_t g_total_resync_drops = 0;

static time_t g_last_heartbeat = 0;

static int feed_ts_bytes(int fifo_fd, const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    if (g_acc_len + len > sizeof(g_acc)) {
        /* Buffer protection: discard unaligned data if overflowing */
        g_acc_len = 0;
    }
    memcpy(g_acc + g_acc_len, data, len);
    g_acc_len += len;

    size_t processed = 0;
    while (processed + TS_PKT_SZ <= g_acc_len && running) {
        if (g_acc[processed] == 0x47) {
            /* Count contiguous valid TS packets */
            size_t pkt_end = processed;
            while (pkt_end + TS_PKT_SZ <= g_acc_len && g_acc[pkt_end] == 0x47) {
                pkt_end += TS_PKT_SZ;
            }
            size_t valid_len = pkt_end - processed;
            size_t written = 0;
            while (written < valid_len && running) {
                ssize_t w = write(fifo_fd, g_acc + processed + written, valid_len - written);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EPIPE) return -1; /* FIFO reader closed */
                    perror("write fifo");
                    return -1;
                }
                written += (size_t)w;
            }
            g_total_valid_bytes += valid_len;

            time_t now = time(NULL);
            if (now - g_last_heartbeat >= 2) {
                int hb_fd = open("/data/local/tmp/fetcher_heartbeat.ts", O_CREAT | O_WRONLY | O_TRUNC, 0666);
                if (hb_fd >= 0) close(hb_fd);
                g_last_heartbeat = now;
            }

            processed = pkt_end;
        } else {
            /* Sync lost: search forward for next 0x47 */
            size_t search = processed + 1;
            int found = 0;
            while (search < g_acc_len) {
                if (g_acc[search] == 0x47) {
                    if (search + TS_PKT_SZ <= g_acc_len) {
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
                processed = g_acc_len;
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

    /* 1. Single-instance flock protection (Zero BusyBox dependency) */
    int lock_fd = open("/data/local/tmp/stream_fetcher.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            fprintf(stderr, "[!] Another stream_fetcher is already running (locked). Exiting.\n");
            return 0;
        }
        char pid_str[32];
        int plen = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
        ftruncate(lock_fd, 0);
        pwrite(lock_fd, pid_str, (size_t)plen, 0);
    }

    /* 2. Self-sufficient FIFO creation (Zero BusyBox dependency) */
    struct stat st;
    if (stat(fifo_path, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        unlink(fifo_path);
        if (mkfifo(fifo_path, 0666) == 0) {
            printf("[✓] Self-created FIFO pipe: %s\n", fifo_path);
        }
    }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    setlinebuf(stdout);
    setlinebuf(stderr);

    printf("[*] stream_fetcher v2 starting: target %s:%d%s -> fifo %s\n", host, port, path, fifo_path);

    while (running) {
        printf("[*] Opening FIFO %s (waiting for reader)...\n", fifo_path);
        int fifo_fd = open(fifo_path, O_WRONLY);
        if (fifo_fd < 0) {
            perror("open fifo");
            sleep(1);
            continue;
        }
        printf("[✓] FIFO connected!\n");
        g_acc_len = 0;

        while (running) {
            struct hostent *he = gethostbyname(host);
            struct sockaddr_in saddr;
            memset(&saddr, 0, sizeof(saddr));
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons((uint16_t)port);

            if (he && he->h_addr_list[0]) {
                memcpy(&saddr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
            } else {
                saddr.sin_addr.s_addr = inet_addr("129.146.5.64");
            }

            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                perror("socket");
                sleep(2);
                continue;
            }

            struct timeval tv;
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            int rcvbuf = 512 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

            printf("[*] Connecting to %s:%d...\n", inet_ntoa(saddr.sin_addr), port);
            if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
                perror("connect");
                close(sock);
                sleep(2);
                continue;
            }

            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#ifdef TCP_KEEPIDLE
            int idle = 5;
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
            int intvl = 3;
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
            int cnt = 3;
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif

            /* Request HTTP/1.0 to disable Transfer-Encoding: chunked */
            char req[512];
            snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: USBStreamTV/2.0\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
            if (send(sock, req, strlen(req), 0) < 0) {
                perror("send");
                close(sock);
                sleep(2);
                continue;
            }

            printf("[✓] HTTP request sent. Reading response headers...\n");

            char header_buf[4096];
            size_t hlen = 0;
            int header_done = 0;

            while (hlen < sizeof(header_buf) - 1 && !header_done && running) {
                char c;
                ssize_t n = recv(sock, &c, 1, 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (n == 0) break;
                header_buf[hlen++] = c;
                header_buf[hlen] = '\0';
                if (hlen >= 4 && memcmp(header_buf + hlen - 4, "\r\n\r\n", 4) == 0) {
                    header_done = 1;
                }
            }

            if (!header_done) {
                fprintf(stderr, "[!] Failed to read HTTP headers. Reconnecting...\n");
                close(sock);
                sleep(1);
                continue;
            }

            int is_chunked = 0;
            if (strstr(header_buf, "Transfer-Encoding: chunked") ||
                strstr(header_buf, "transfer-encoding: chunked")) {
                is_chunked = 1;
                printf("[!] Warning: Server returned chunked transfer encoding, parsing chunks...\n");
            }

            printf("[✓] Stream connected! Forwarding aligned MPEG-TS packets into FIFO...\n");

            uint8_t raw_buf[65536];
            time_t last_log = time(NULL);
            int stream_err = 0;

            if (!is_chunked) {
                /* Fast raw stream path */
                while (running) {
                    struct pollfd pfd;
                    pfd.fd = sock;
                    pfd.events = POLLIN;
                    int pret = poll(&pfd, 1, 8000);
                    if (pret == 0 || (pret < 0 && errno != EINTR)) {
                        fprintf(stderr, "[!] Socket timeout (8s without data from server). Reconnecting...\n");
                        close(sock);
                        sock = -1;
                        break;
                    }
                    if (pret < 0) continue;

                    ssize_t n = recv(sock, raw_buf, sizeof(raw_buf), 0);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            fprintf(stderr, "[!] Socket timeout. Reconnecting...\n");
                        } else {
                            fprintf(stderr, "[!] Server connection closed (%s)\n", strerror(errno));
                        }
                        break;
                    }
                    if (n == 0) {
                        fprintf(stderr, "[!] Server connection closed cleanly. Reconnecting...\n");
                        break;
                    }

                    if (feed_ts_bytes(fifo_fd, raw_buf, (size_t)n) < 0) {
                        stream_err = 1;
                        break;
                    }

                    time_t now = time(NULL);
                    if (now - last_log >= 10) {
                        printf("[*] Streaming active: %.2f MB valid TS (resync drops: %llu bytes)\n",
                               (double)g_total_valid_bytes / (1024.0 * 1024.0),
                               (unsigned long long)g_total_resync_drops);
                        last_log = now;
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
                        struct pollfd pfd;
                        pfd.fd = sock;
                        pfd.events = POLLIN;
                        int pret = poll(&pfd, 1, 8000);
                        if (pret == 0 || (pret < 0 && errno != EINTR)) {
                            fprintf(stderr, "[!] Socket timeout (8s without data from server). Reconnecting...\n");
                            close(sock);
                            sock = -1;
                            chunked_timeout = 1;
                            break;
                        }
                        if (pret < 0) continue;

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
                        struct pollfd pfd;
                        pfd.fd = sock;
                        pfd.events = POLLIN;
                        int pret = poll(&pfd, 1, 8000);
                        if (pret == 0 || (pret < 0 && errno != EINTR)) {
                            fprintf(stderr, "[!] Socket timeout (8s without data from server). Reconnecting...\n");
                            close(sock);
                            sock = -1;
                            chunked_timeout = 1;
                            break;
                        }
                        if (pret < 0) continue;

                        size_t to_read = (size_t)chunk_sz - chunk_read;
                        if (to_read > sizeof(raw_buf)) to_read = sizeof(raw_buf);
                        ssize_t n = recv(sock, raw_buf, to_read, 0);
                        if (n <= 0) {
                            if (n < 0 && errno == EINTR) continue;
                            break;
                        }
                        if (feed_ts_bytes(fifo_fd, raw_buf, (size_t)n) < 0) {
                            stream_err = 1;
                            break;
                        }
                        chunk_read += (size_t)n;
                    }
                    if (stream_err || chunk_read < (size_t)chunk_sz || chunked_timeout || sock < 0) break;

                    /* Consume trailing \r\n */
                    struct pollfd pfd_crlf;
                    pfd_crlf.fd = sock;
                    pfd_crlf.events = POLLIN;
                    int pret = poll(&pfd_crlf, 1, 8000);
                    if (pret == 0 || (pret < 0 && errno != EINTR)) {
                        fprintf(stderr, "[!] Socket timeout (8s without data from server). Reconnecting...\n");
                        close(sock);
                        sock = -1;
                        break;
                    }
                    if (pret > 0) {
                        char crlf[2];
                        recv(sock, crlf, 2, MSG_WAITALL);
                    }

                    time_t now = time(NULL);
                    if (now - last_log >= 10) {
                        printf("[*] Chunked streaming: %.2f MB valid TS\n",
                               (double)g_total_valid_bytes / (1024.0 * 1024.0));
                        last_log = now;
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
