/* Auto-generated for VOD Cinema 8GB/12GB hierarchy */
#ifndef CATALOG_HIERARCHY_H
#define CATALOG_HIERARCHY_H

#include "fuse_ntfs.h"

static inline void init_full_hierarchy_catalog(void) {
    pthread_rwlock_wrlock(&g_catalog_rwlock);
    g_num_files = 8;

    /* Entry 0: TV AO VIVO.trp (Inode 32, 8589934592 B) */
    memset(&g_files[0], 0, sizeof(struct virtual_file));
    g_files[0].file_id = 0;
    strncpy(g_files[0].id, "live_tv", sizeof(g_files[0].id) - 1);
    strncpy(g_files[0].category, "LIVE", sizeof(g_files[0].category) - 1);
    strncpy(g_files[0].path, "TV AO VIVO.trp", sizeof(g_files[0].path) - 1);
    g_files[0].file_size = 8589934592ULL;
    g_files[0].src_type = SRC_LIVE_FIFO;
    g_files[0].state = VOD_STATE_READY;
    g_files[0].bytes_available = 8589934592ULL;
    g_files[0].num_extents = 2;
    g_files[0].extents[0].id = 0;
    g_files[0].extents[0].vcn_start = 0ULL;
    g_files[0].extents[0].vcn_end = 1557134ULL;
    g_files[0].extents[0].lcn_start = 1588592ULL;
    g_files[0].extents[0].lcn_end = 3145726ULL;
    g_files[0].extents[0].file_start = 0ULL;
    g_files[0].extents[0].file_end = 6378024960ULL;
    g_files[0].extents[0].lba_start = 12708736ULL;
    g_files[0].extents[0].lba_end = 25165815ULL;
    g_files[0].extents[0].num_clusters = 1557135ULL;
    g_files[0].extents[1].id = 1;
    g_files[0].extents[1].vcn_start = 1557135ULL;
    g_files[0].extents[1].vcn_end = 2097151ULL;
    g_files[0].extents[1].lcn_start = 393416ULL;
    g_files[0].extents[1].lcn_end = 933432ULL;
    g_files[0].extents[1].file_start = 6378024960ULL;
    g_files[0].extents[1].file_end = 8589934592ULL;
    g_files[0].extents[1].lba_start = 3147328ULL;
    g_files[0].extents[1].lba_end = 7467463ULL;
    g_files[0].extents[1].num_clusters = 540017ULL;
    g_files[0].active = 1;
    g_files[0].http_sock = -1;
    pthread_mutex_init(&g_files[0].vf_mu, NULL);
    pthread_cond_init(&g_files[0].vf_cv, NULL);

    /* Entry 1: FILMES/Interestelar.mp4 (Inode 33, 268958514 B) */
    memset(&g_files[1], 0, sizeof(struct virtual_file));
    g_files[1].file_id = 1;
    strncpy(g_files[1].id, "vod_interestelar", sizeof(g_files[1].id) - 1);
    strncpy(g_files[1].category, "FILMES", sizeof(g_files[1].category) - 1);
    strncpy(g_files[1].path, "FILMES/Interestelar.mp4", sizeof(g_files[1].path) - 1);
    g_files[1].file_size = 268958514ULL;
    g_files[1].src_type = SRC_VOD_HTTP_RANGE;
    g_files[1].state = VOD_STATE_READY;
    g_files[1].bytes_available = 268958514ULL;
    g_files[1].num_extents = 1;
    g_files[1].extents[0].id = 0;
    g_files[1].extents[0].vcn_start = 0ULL;
    g_files[1].extents[0].vcn_end = 65663ULL;
    g_files[1].extents[0].lcn_start = 933433ULL;
    g_files[1].extents[0].lcn_end = 999096ULL;
    g_files[1].extents[0].file_start = 0ULL;
    g_files[1].extents[0].file_end = 268958514ULL;
    g_files[1].extents[0].lba_start = 7467464ULL;
    g_files[1].extents[0].lba_end = 7992775ULL;
    g_files[1].extents[0].num_clusters = 65664ULL;
    strncpy(g_files[1].http_host, "tv.smre.run.place", sizeof(g_files[1].http_host) - 1);
    g_files[1].http_port = 80;
    strncpy(g_files[1].http_path, "/vod/1c504eb5/movie.mp4", sizeof(g_files[1].http_path) - 1);
    g_files[1].active = 1;
    g_files[1].http_sock = -1;
    pthread_mutex_init(&g_files[1].vf_mu, NULL);
    pthread_cond_init(&g_files[1].vf_cv, NULL);

    /* Entry 2: FILMES/Matrix.mp4 (Inode 34, 26214400 B) */
    memset(&g_files[2], 0, sizeof(struct virtual_file));
    g_files[2].file_id = 2;
    strncpy(g_files[2].id, "vod_matrix", sizeof(g_files[2].id) - 1);
    strncpy(g_files[2].category, "FILMES", sizeof(g_files[2].category) - 1);
    strncpy(g_files[2].path, "FILMES/Matrix.mp4", sizeof(g_files[2].path) - 1);
    g_files[2].file_size = 26214400ULL;
    g_files[2].src_type = SRC_VOD_HTTP_RANGE;
    g_files[2].state = VOD_STATE_READY;
    g_files[2].bytes_available = 26214400ULL;
    g_files[2].num_extents = 1;
    g_files[2].extents[0].id = 0;
    g_files[2].extents[0].vcn_start = 0ULL;
    g_files[2].extents[0].vcn_end = 6399ULL;
    g_files[2].extents[0].lcn_start = 999097ULL;
    g_files[2].extents[0].lcn_end = 1005496ULL;
    g_files[2].extents[0].file_start = 0ULL;
    g_files[2].extents[0].file_end = 26214400ULL;
    g_files[2].extents[0].lba_start = 7992776ULL;
    g_files[2].extents[0].lba_end = 8043975ULL;
    g_files[2].extents[0].num_clusters = 6400ULL;
    strncpy(g_files[2].http_host, "tv.smre.run.place", sizeof(g_files[2].http_host) - 1);
    g_files[2].http_port = 80;
    strncpy(g_files[2].http_path, "/vod/6f9ebdc0/movie.mp4", sizeof(g_files[2].http_path) - 1);
    g_files[2].active = 1;
    g_files[2].http_sock = -1;
    pthread_mutex_init(&g_files[2].vf_mu, NULL);
    pthread_cond_init(&g_files[2].vf_cv, NULL);

    /* Entry 3: FILMES/Oppenheimer.mp4 (Inode 35, 20971520 B) */
    memset(&g_files[3], 0, sizeof(struct virtual_file));
    g_files[3].file_id = 3;
    strncpy(g_files[3].id, "vod_oppenheimer", sizeof(g_files[3].id) - 1);
    strncpy(g_files[3].category, "FILMES", sizeof(g_files[3].category) - 1);
    strncpy(g_files[3].path, "FILMES/Oppenheimer.mp4", sizeof(g_files[3].path) - 1);
    g_files[3].file_size = 20971520ULL;
    g_files[3].src_type = SRC_VOD_HTTP_RANGE;
    g_files[3].state = VOD_STATE_READY;
    g_files[3].bytes_available = 20971520ULL;
    g_files[3].num_extents = 1;
    g_files[3].extents[0].id = 0;
    g_files[3].extents[0].vcn_start = 0ULL;
    g_files[3].extents[0].vcn_end = 5119ULL;
    g_files[3].extents[0].lcn_start = 1005497ULL;
    g_files[3].extents[0].lcn_end = 1010616ULL;
    g_files[3].extents[0].file_start = 0ULL;
    g_files[3].extents[0].file_end = 20971520ULL;
    g_files[3].extents[0].lba_start = 8043976ULL;
    g_files[3].extents[0].lba_end = 8084935ULL;
    g_files[3].extents[0].num_clusters = 5120ULL;
    strncpy(g_files[3].http_host, "tv.smre.run.place", sizeof(g_files[3].http_host) - 1);
    g_files[3].http_port = 80;
    strncpy(g_files[3].http_path, "/vod/92ac9491/movie.mp4", sizeof(g_files[3].http_path) - 1);
    g_files[3].active = 1;
    g_files[3].http_sock = -1;
    pthread_mutex_init(&g_files[3].vf_mu, NULL);
    pthread_cond_init(&g_files[3].vf_cv, NULL);

    /* Entry 4: SERIES/Breaking_Bad/S01E01.mp4 (Inode 36, 15728640 B) */
    memset(&g_files[4], 0, sizeof(struct virtual_file));
    g_files[4].file_id = 4;
    strncpy(g_files[4].id, "vod_bb_s01e01", sizeof(g_files[4].id) - 1);
    strncpy(g_files[4].category, "SERIES", sizeof(g_files[4].category) - 1);
    strncpy(g_files[4].path, "SERIES/Breaking_Bad/S01E01.mp4", sizeof(g_files[4].path) - 1);
    g_files[4].file_size = 15728640ULL;
    g_files[4].src_type = SRC_VOD_HTTP_RANGE;
    g_files[4].state = VOD_STATE_READY;
    g_files[4].bytes_available = 15728640ULL;
    g_files[4].num_extents = 1;
    g_files[4].extents[0].id = 0;
    g_files[4].extents[0].vcn_start = 0ULL;
    g_files[4].extents[0].vcn_end = 3839ULL;
    g_files[4].extents[0].lcn_start = 1010617ULL;
    g_files[4].extents[0].lcn_end = 1014456ULL;
    g_files[4].extents[0].file_start = 0ULL;
    g_files[4].extents[0].file_end = 15728640ULL;
    g_files[4].extents[0].lba_start = 8084936ULL;
    g_files[4].extents[0].lba_end = 8115655ULL;
    g_files[4].extents[0].num_clusters = 3840ULL;
    strncpy(g_files[4].http_host, "tv.smre.run.place", sizeof(g_files[4].http_host) - 1);
    g_files[4].http_port = 80;
    strncpy(g_files[4].http_path, "/vod/1c504eb5/movie.mp4", sizeof(g_files[4].http_path) - 1);
    g_files[4].active = 1;
    g_files[4].http_sock = -1;
    pthread_mutex_init(&g_files[4].vf_mu, NULL);
    pthread_cond_init(&g_files[4].vf_cv, NULL);

    /* Entry 5: SERIES/Breaking_Bad/S01E02.mp4 (Inode 37, 12582912 B) */
    memset(&g_files[5], 0, sizeof(struct virtual_file));
    g_files[5].file_id = 5;
    strncpy(g_files[5].id, "vod_bb_s01e02", sizeof(g_files[5].id) - 1);
    strncpy(g_files[5].category, "SERIES", sizeof(g_files[5].category) - 1);
    strncpy(g_files[5].path, "SERIES/Breaking_Bad/S01E02.mp4", sizeof(g_files[5].path) - 1);
    g_files[5].file_size = 12582912ULL;
    g_files[5].src_type = SRC_VOD_HTTP_RANGE;
    g_files[5].state = VOD_STATE_READY;
    g_files[5].bytes_available = 12582912ULL;
    g_files[5].num_extents = 1;
    g_files[5].extents[0].id = 0;
    g_files[5].extents[0].vcn_start = 0ULL;
    g_files[5].extents[0].vcn_end = 3071ULL;
    g_files[5].extents[0].lcn_start = 1014457ULL;
    g_files[5].extents[0].lcn_end = 1017528ULL;
    g_files[5].extents[0].file_start = 0ULL;
    g_files[5].extents[0].file_end = 12582912ULL;
    g_files[5].extents[0].lba_start = 8115656ULL;
    g_files[5].extents[0].lba_end = 8140231ULL;
    g_files[5].extents[0].num_clusters = 3072ULL;
    strncpy(g_files[5].http_host, "tv.smre.run.place", sizeof(g_files[5].http_host) - 1);
    g_files[5].http_port = 80;
    strncpy(g_files[5].http_path, "/vod/6f9ebdc0/movie.mp4", sizeof(g_files[5].http_path) - 1);
    g_files[5].active = 1;
    g_files[5].http_sock = -1;
    pthread_mutex_init(&g_files[5].vf_mu, NULL);
    pthread_cond_init(&g_files[5].vf_cv, NULL);

    /* Entry 6: ANIMES/Death_Note/S01E01.mp4 (Inode 38, 8388608 B) */
    memset(&g_files[6], 0, sizeof(struct virtual_file));
    g_files[6].file_id = 6;
    strncpy(g_files[6].id, "vod_dn_s01e01", sizeof(g_files[6].id) - 1);
    strncpy(g_files[6].category, "ANIMES", sizeof(g_files[6].category) - 1);
    strncpy(g_files[6].path, "ANIMES/Death_Note/S01E01.mp4", sizeof(g_files[6].path) - 1);
    g_files[6].file_size = 8388608ULL;
    g_files[6].src_type = SRC_VOD_HTTP_RANGE;
    g_files[6].state = VOD_STATE_READY;
    g_files[6].bytes_available = 8388608ULL;
    g_files[6].num_extents = 1;
    g_files[6].extents[0].id = 0;
    g_files[6].extents[0].vcn_start = 0ULL;
    g_files[6].extents[0].vcn_end = 2047ULL;
    g_files[6].extents[0].lcn_start = 1017529ULL;
    g_files[6].extents[0].lcn_end = 1019576ULL;
    g_files[6].extents[0].file_start = 0ULL;
    g_files[6].extents[0].file_end = 8388608ULL;
    g_files[6].extents[0].lba_start = 8140232ULL;
    g_files[6].extents[0].lba_end = 8156615ULL;
    g_files[6].extents[0].num_clusters = 2048ULL;
    strncpy(g_files[6].http_host, "tv.smre.run.place", sizeof(g_files[6].http_host) - 1);
    g_files[6].http_port = 80;
    strncpy(g_files[6].http_path, "/vod/1c504eb5/movie.mp4", sizeof(g_files[6].http_path) - 1);
    g_files[6].active = 1;
    g_files[6].http_sock = -1;
    pthread_mutex_init(&g_files[6].vf_mu, NULL);
    pthread_cond_init(&g_files[6].vf_cv, NULL);

    /* Entry 7: ANIMES/Death_Note/S01E02.mp4 (Inode 39, 7340032 B) */
    memset(&g_files[7], 0, sizeof(struct virtual_file));
    g_files[7].file_id = 7;
    strncpy(g_files[7].id, "vod_dn_s01e02", sizeof(g_files[7].id) - 1);
    strncpy(g_files[7].category, "ANIMES", sizeof(g_files[7].category) - 1);
    strncpy(g_files[7].path, "ANIMES/Death_Note/S01E02.mp4", sizeof(g_files[7].path) - 1);
    g_files[7].file_size = 7340032ULL;
    g_files[7].src_type = SRC_VOD_HTTP_RANGE;
    g_files[7].state = VOD_STATE_READY;
    g_files[7].bytes_available = 7340032ULL;
    g_files[7].num_extents = 1;
    g_files[7].extents[0].id = 0;
    g_files[7].extents[0].vcn_start = 0ULL;
    g_files[7].extents[0].vcn_end = 1791ULL;
    g_files[7].extents[0].lcn_start = 1019577ULL;
    g_files[7].extents[0].lcn_end = 1021368ULL;
    g_files[7].extents[0].file_start = 0ULL;
    g_files[7].extents[0].file_end = 7340032ULL;
    g_files[7].extents[0].lba_start = 8156616ULL;
    g_files[7].extents[0].lba_end = 8170951ULL;
    g_files[7].extents[0].num_clusters = 1792ULL;
    strncpy(g_files[7].http_host, "tv.smre.run.place", sizeof(g_files[7].http_host) - 1);
    g_files[7].http_port = 80;
    strncpy(g_files[7].http_path, "/vod/92ac9491/movie.mp4", sizeof(g_files[7].http_path) - 1);
    g_files[7].active = 1;
    g_files[7].http_sock = -1;
    pthread_mutex_init(&g_files[7].vf_mu, NULL);
    pthread_cond_init(&g_files[7].vf_cv, NULL);

    pthread_rwlock_unlock(&g_catalog_rwlock);
}

#endif /* CATALOG_HIERARCHY_H */
