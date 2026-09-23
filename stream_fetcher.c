#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

int main(int argc, char *argv[]) {
    const char *host = "tv.smre.run.place";
    int port = 80;
    const char *path = "/stream";
    const char *fifo_path = "/data/local/tmp/live_pipe";

    if (argc > 1) fifo_path = argv[1];
    if (argc > 2) host = argv[2];
    if (argc > 3) port = atoi(argv[3]);

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGPIPE, SIG_IGN);

    setlinebuf(stdout);
    setlinebuf(stderr);

    printf("[*] stream_fetcher starting: target %s:%d%s -> fifo %s\n", host, port, path, fifo_path);

    while (running) {
        printf("[*] Opening FIFO %s (waiting for reader)...\n", fifo_path);
        int fifo_fd = open(fifo_path, O_WRONLY);
        if (fifo_fd < 0) {
            perror("open fifo");
            sleep(1);
            continue;
        }
        printf("[✓] FIFO connected!\n");

        while (running) {
            struct hostent *he = gethostbyname(host);
            struct sockaddr_in saddr;
            memset(&saddr, 0, sizeof(saddr));
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(port);

            if (he && he->h_addr_list[0]) {
                memcpy(&saddr.sin_addr, he->h_addr_list[0], he->h_length);
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

            printf("[*] Connecting to %s:%d...\n", inet_ntoa(saddr.sin_addr), port);
            if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
                perror("connect");
                close(sock);
                sleep(2);
                continue;
            }

            char req[512];
            snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: usb-stream-tv/1.0\r\n"
                     "Connection: keep-alive\r\n\r\n",
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
                if (n <= 0) break;
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

            printf("[✓] HTTP stream connected! Forwarding MPEG-TS into FIFO...\n");

            uint8_t buf[65536];
            uint64_t total_bytes = 0;
            time_t last_log = time(NULL);

            while (running) {
                ssize_t n = recv(sock, buf, sizeof(buf), 0);
                if (n <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        fprintf(stderr, "[!] Socket timeout. Reconnecting...\n");
                    } else {
                        fprintf(stderr, "[!] Server connection closed (%s)\n", strerror(errno));
                    }
                    break;
                }

                size_t written = 0;
                int pipe_err = 0;
                while (written < (size_t)n) {
                    ssize_t wn = write(fifo_fd, buf + written, n - written);
                    if (wn <= 0) {
                        if (errno == EPIPE) {
                            fprintf(stderr, "[!] FIFO reader closed (fuse_direct restarted?)\n");
                            pipe_err = 1;
                            break;
                        }
                        perror("write fifo");
                        pipe_err = 1;
                        break;
                    }
                    written += wn;
                }
                if (pipe_err) {
                    close(sock);
                    close(fifo_fd);
                    fifo_fd = -1;
                    break;
                }

                total_bytes += n;
                time_t now = time(NULL);
                if (now - last_log >= 10) {
                    printf("[*] Streaming active: %.2f MB streamed\n", (double)total_bytes / (1024.0 * 1024.0));
                    last_log = now;
                }
            }

            if (sock >= 0) close(sock);
            if (fifo_fd < 0) break; // Reopen FIFO
            if (!running) break;
            sleep(1);
        }
        if (fifo_fd >= 0) close(fifo_fd);
    }
    printf("[*] stream_fetcher exiting cleanly.\n");
    return 0;
}
