#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/fuse.h>

#define MNT_POINT "/data/local/tmp/vfat_mnt"
#define FILE_NAME "tv_stream.img"
#define FILE_INO  2

struct Channel {
    const char *name;
    const char *id;
    uint64_t start_offset;
    uint64_t end_offset;
    uint32_t start_sector;
};

static struct Channel channels[] = {
    {"01 - REDE GLOBO.ts",        "globo-morena-dourados", 1560576ULL,   62373888ULL,   3048},
    {"02 - RECORD NEWS.ts",        "record-news",          62373888ULL,  123187200ULL, 121824},
    {"03 - TV CULTURA.ts",         "tv-cultura-sp",        123187200ULL, 184000512ULL, 240600},
    {"04 - REDETV NACIONAL.ts",    "rede-tv-nacional",     184000512ULL, 244813824ULL, 359376},
    {"05 - REDE BRASIL.ts",        "rede-brasil",          244813824ULL, 305627136ULL, 478152},
    {"06 - STUDIO UNIVERSAL.ts",   "studio-universal-br",  305627136ULL, 366440448ULL, 596928},
    {"07 - SONY CHANNEL.ts",       "sony-channel-br",      366440448ULL, 427253760ULL, 715704},
    {"08 - TNT NOVELAS.ts",        "tnt-novelas-br",       427253760ULL, 488067072ULL, 834480},
    {"09 - ESPN.ts",               "espn-mirror-a07z",     488067072ULL, 548880384ULL, 953256},
    {"10 - ESPN 4.ts",             "espn4-mirror-b39z",    548880384ULL, 609693696ULL, 1072032},
    {"11 - TV BRASIL EBC.ts",      "tv-brasil-ebc",        609693696ULL, 670507008ULL, 1190808},
    {"12 - CANAL FUTURA.ts",       "canal-futura",         670507008ULL, 731320320ULL, 1309584}
};
#define NUM_CHANNELS 12

static int fuse_fd = -1;
static int backing_fd = -1;
static uint64_t file_size = 750ULL * 1024 * 1024;
static volatile int running = 1;
static int current_channel = -1;

void handle_sig(int s) {
    running = 0;
}

int main(int argc, char **argv) {
    const char *backing_path = "/data/local/tmp/tv_stream.img";
    if (argc > 1) backing_path = argv[1];

    backing_fd = open(backing_path, O_RDWR);
    if (backing_fd >= 0) {
        struct stat st;
        if (fstat(backing_fd, &st) == 0) {
            file_size = st.st_size;
        }
    } else {
        perror("open backing file");
        return 1;
    }
    printf("[*] Backing file: %s (%llu MB)\n", backing_path, (unsigned long long)(file_size / (1024*1024)));

    mkdir(MNT_POINT, 0755);
    umount2(MNT_POINT, MNT_DETACH); // Clean previous mount

    fuse_fd = open("/dev/fuse", O_RDWR);
    if (fuse_fd < 0) {
        perror("open /dev/fuse");
        return 1;
    }

    char opts[128];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=040755,user_id=0,group_id=0,allow_other", fuse_fd);
    if (mount("fuse", MNT_POINT, "fuse", 0, opts) < 0) {
        perror("mount");
        close(fuse_fd);
        return 2;
    }

    printf("[✓] FUSE Sensor ativo em %s\n", MNT_POINT);
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    char in_buf[128 * 1024 + 4096];
    char out_buf[128 * 1024 + 4096];

    while (running) {
        ssize_t n = read(fuse_fd, in_buf, sizeof(in_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n < sizeof(struct fuse_in_header)) continue;

        struct fuse_in_header *inh = (struct fuse_in_header *)in_buf;
        void *payload = in_buf + sizeof(struct fuse_in_header);

        if (inh->opcode == FUSE_INIT) {
            struct fuse_init_in *init_in = (struct fuse_init_in *)payload;
            struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
            struct fuse_init_out *init_out = (struct fuse_init_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out));

            outh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
            outh->error = 0;
            outh->unique = inh->unique;

            init_out->major = FUSE_KERNEL_VERSION;
            init_out->minor = FUSE_KERNEL_MINOR_VERSION;
            init_out->max_readahead = 128 * 1024;
            init_out->flags = init_in->flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES);
            init_out->max_write = 128 * 1024;
            write(fuse_fd, out_buf, outh->len);
        } else if (inh->opcode == FUSE_GETATTR) {
            struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
            struct fuse_attr_out *attr_out = (struct fuse_attr_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out));

            outh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
            outh->error = 0;
            outh->unique = inh->unique;

            attr_out->attr_valid = 10;
            attr_out->attr.ino = inh->nodeid;
            if (inh->nodeid == 1) {
                attr_out->attr.mode = S_IFDIR | 0755;
                attr_out->attr.nlink = 2;
                attr_out->attr.size = 4096;
            } else if (inh->nodeid == FILE_INO) {
                attr_out->attr.mode = S_IFREG | 0644;
                attr_out->attr.nlink = 1;
                attr_out->attr.size = file_size;
            } else {
                outh->error = -ENOENT;
                outh->len = sizeof(struct fuse_out_header);
            }
            write(fuse_fd, out_buf, outh->len);
        } else if (inh->opcode == FUSE_LOOKUP) {
            char *name = (char *)payload;
            struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
            if (strcmp(name, FILE_NAME) == 0) {
                struct fuse_entry_out *entry_out = (struct fuse_entry_out *)(out_buf + sizeof(struct fuse_out_header));
                memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out));

                outh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
                outh->error = 0;
                outh->unique = inh->unique;

                entry_out->nodeid = FILE_INO;
                entry_out->generation = 1;
                entry_out->entry_valid = 10;
                entry_out->attr_valid = 10;
                entry_out->attr.ino = FILE_INO;
                entry_out->attr.mode = S_IFREG | 0644;
                entry_out->attr.nlink = 1;
                entry_out->attr.size = file_size;
            } else {
                outh->len = sizeof(struct fuse_out_header);
                outh->error = -ENOENT;
                outh->unique = inh->unique;
            }
            write(fuse_fd, out_buf, outh->len);
        } else if (inh->opcode == FUSE_OPEN) {
            struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
            struct fuse_open_out *open_out = (struct fuse_open_out *)(out_buf + sizeof(struct fuse_out_header));
            memset(out_buf, 0, sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out));

            outh->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
            outh->error = 0;
            outh->unique = inh->unique;
            open_out->fh = 1;
            open_out->open_flags = FOPEN_KEEP_CACHE;
            write(fuse_fd, out_buf, outh->len);
        } else if (inh->opcode == FUSE_READ) {
            struct fuse_read_in *rin = (struct fuse_read_in *)payload;
            struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
            char *rdata = out_buf + sizeof(struct fuse_out_header);

            uint64_t offset = rin->offset;
            uint32_t size = rin->size;
            if (size > 128 * 1024) size = 128 * 1024;

            // Identificação de canal com base no offset lido
            for (int i = 0; i < NUM_CHANNELS; i++) {
                if (offset >= channels[i].start_offset && offset < channels[i].end_offset) {
                    // Dispara SOMENTE quando muda para um canal diferente
                    // Nunca re-dispara enquanto a TV está assistindo o mesmo canal
                    if (current_channel != i) {
                        current_channel = i;
                        printf("\n==================================================\n");
                        printf(" [⚡ SENSOR TV] A TV ABRIU: %s\n", channels[i].name);
                        printf(" [⚡ SENSOR TV] ID: %s | Setor: %u\n",
                               channels[i].id, channels[i].start_sector);
                        printf("==================================================\n");
                        fflush(stdout);

                        char cmd[256];
                        snprintf(cmd, sizeof(cmd),
                                 "/data/local/tmp/on_channel_switch.sh %s %u >/dev/null 2>&1 &",
                                 channels[i].id, channels[i].start_sector);
                        system(cmd);
                    }
                    break;
                }
            }

            ssize_t bytes_read = pread(backing_fd, rdata, size, offset);
            if (bytes_read < 0) bytes_read = 0;
            if (bytes_read < size) {
                memset(rdata + bytes_read, 0, size - bytes_read);
                bytes_read = size;
            }

            outh->len = sizeof(struct fuse_out_header) + bytes_read;
            outh->error = 0;
            outh->unique = inh->unique;
            write(fuse_fd, out_buf, outh->len);
        } else {
            struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
            outh->len = sizeof(struct fuse_out_header);
            outh->error = -ENOSYS;
            outh->unique = inh->unique;
            write(fuse_fd, out_buf, outh->len);
        }
    }

    printf("[!] Encerrando FUSE Sensor...\n");
    umount2(MNT_POINT, MNT_DETACH);
    close(fuse_fd);
    if (backing_fd >= 0) close(backing_fd);
    return 0;
}
