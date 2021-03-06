/*
 *  Copyright (C) 2012-2014 Skylable Ltd. <info-copyright@skylable.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Special exception for linking this software with OpenSSL:
 *
 *  In addition, as a special exception, Skylable Ltd. gives permission to
 *  link the code of this program with the OpenSSL library and distribute
 *  linked combinations including the two. You must obey the GNU General
 *  Public License in all respects for all of the code used other than
 *  OpenSSL. You may extend this exception to your version of the program,
 *  but you are not obligated to do so. If you do not wish to do so, delete
 *  this exception statement from your version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <utime.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <openssl/sha.h>

#include "sx.h"
#include "libsx/src/clustcfg.h"
#include "libsx/src/volops.h"
#include "libsx/src/misc.h"
#include "version.h"
#include "rgen.h"
#include "client-test-cmdline.h"

#define VOLSIZE 2*args->replica_arg*1024LL*1024LL*1024LL /* TODO: bigger precision */
#define VOLNAME "vol" /* There will be 6 random characters suffix added. There CANNOT be '..' inside! */
#define LOCAL_DIR "/tmp/.test" /* There will be 6 random characters suffix added. */
#define REMOTE_DIR ".test"
#define EMPTY_FILE_NAME "file_empty"
#define UD_FILE_NAME "file_ud"
#define REV_FILE_NAME "file_rev" /* There will be added numbers as suffixes for revision versions */
#define ATTRIBS_COUNT 10 /* Up to 100 (or you will have no enough space for it) */
#define ATTRIBS_FILE_NAME "file_attrib"
#define TRASH_NAME "/.Trash"
#define UNDELETE_FILE_NAME "file_undelete"
#define QUOTA_FILE_NAME "file_quota"
#define QUOTA_VOL_SIZE 1
#define QUOTA_FILE_SIZE 5 /* Must be more then QUOTA_VOL_SIZE */
#define COPY_FILE_NAME "file_copy"
#define ACL_USER1 "user1" /* There will be 6 random characters suffix added. */
#define ACL_USER2 "user2" /* There will be 6 random characters suffix added. */
#define ACL_USER3 "user3" /* There will be 6 random characters suffix added. */
#define ACL_VOLNAME1 "vol1" /* There will be 6 random characters suffix added. */
#define ACL_VOLNAME2 "vol2" /* There will be 6 random characters suffix added. */
#define ACL_FILE_NAME "file_acl"
#define ACL_KEY_FILE_NAME "file_acl_key"
#define CAT_FILE_NAME_IN "file_cat_in"
#define CAT_FILE_NAME_OUT "file_cat_out"
#define CAT_FILE_SIZE 5

int64_t bytes; /* FIXME: small change in libsx to avoid this to be global */

float to_human (long long int n) {
    float h = (float)n;
    while(h >= 1024)
        h /= 1024;
    return h;
}

char to_human_suffix (long long int n) {
    int count = 0;
    char suf[] = {'B', 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y'};
    while(n > 1023) {
        n /= 1024;
        count++;
    }
    return count < sizeof(suf) ? suf[count] : suf[sizeof(suf)-1];
}

int test_input_fn(sxc_client_t *sx, sxc_input_t type, const char *prompt, const char *def, char *in, unsigned int insize, void *ctx) {
    if(!sx || !prompt || !in || !insize) {
        if(sx)
            fprintf(stderr, "test_input_fn: ERROR: NULL argument.\n");
        return -1;
    }
    switch(type) {
        case SXC_INPUT_SENSITIVE:
            snprintf(in, insize, "yacWetheas9");
            break;
        default:
            return -1;
    }
    return 0;
}

static int test_callback(const sxc_xfer_stat_t *xfer_stat) {
    if(!xfer_stat) {
        fprintf(stderr, "Callback failure.\n");
        return SXE_NOERROR;
    }
    if(xfer_stat->status == SXC_XFER_STATUS_PART_FINISHED || xfer_stat->status == SXC_XFER_STATUS_WAITING)
        *((int64_t*)xfer_stat->ctx) = xfer_stat->current_xfer.sent;
    return SXE_NOERROR;
}

int randomize_name(char *name) {
    int fd;
    mode_t mask = umask(0);
    umask(077); /* shut up warnings */
    fd = mkstemp(name);
    umask(mask);
    if(fd < 0) {
        fprintf(stderr, "randomize_name: ERROR: Cannot generate temporary name.\n");
        return 1;
    }
    if(close(fd)) {
        fprintf(stderr, "randomize_name: ERROR: Cannot close file descriptor: %s\n", strerror(errno));
        if(unlink(name))
            fprintf(stderr, "randomize_name: ERROR: Cannot delete '%s' file: %s\n", name, strerror(errno));
        return 1;
    }
    if(unlink(name)) {
        fprintf(stderr, "randomize_name: ERROR: Cannot delete '%s' file: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int create_volume(sxc_client_t *sx, sxc_cluster_t *cluster, const char *volname, const char *owner, const char *filter_dir, const char *filter_name, const char *filter_cfg, const struct gengetopt_args_info *args, int max_revisions, int hide_errors) {
    void *cfgdata = NULL;
    int i, fcount, filter_idx, ret = 1;
    unsigned int cfgdata_len = 0;
    char uuidcfg[41];
    uint8_t uuid[16];
    char *voldir = NULL;
    const char *confdir;
    sxc_meta_t *meta = NULL;
    const sxc_filter_t *filter = NULL;
    const sxf_handle_t *filters = NULL;
    
    if(filter_name) {
        sxc_filter_loadall(sx, filter_dir);
        confdir = sxi_cluster_get_confdir(cluster);
        voldir = (char*)malloc(strlen(confdir) + strlen("/volumes/") + strlen(volname) + 1);
        if(!voldir) {
            if(!hide_errors)
                fprintf(stderr, "create_volume: ERROR: Cannot allocate memory for voldir.\n");
            goto create_volume_err;
        }
        sprintf(voldir, "%s/volumes/%s", confdir, volname);
        /* Wipe existing local config */
        /* There is no check for '..' in the path since the path is fully based on the code, not on arguments. */
        if(!access(voldir, F_OK) && sxi_rmdirs(voldir)) {
            if(!hide_errors)
                fprintf(stderr, "create_volume: ERROR: Cannot wipe old volume configuration directory: %s\n", voldir);
            goto create_volume_err;
        }
        filters = sxc_filter_list(sx, &fcount);
        if(!filters) {
            if(!hide_errors)
                fprintf(stderr, "create_volume: ERROR: No filters available.\n");
            goto create_volume_err;
        }
        meta = sxc_meta_new(sx);
        if(!meta) {
            if(!hide_errors)
                fprintf(stderr, "create_volume: ERROR: Cannot initiate meta.\n");
            goto create_volume_err;
        }
        for(i=0; i<fcount; i++) {
            const sxc_filter_t *f = sxc_get_filter(&filters[i]);
            if(!strcmp(f->shortname, filter_name))
                if(!filter || (f->version[0] > filter->version[0]) || (f->version[0] == filter->version[0] && f->version[1] > filter->version[1])) {
                    filter = f;
                    filter_idx = i;
                }
        }
        if(!filter) {
            if(!hide_errors)
                fprintf(stderr, "create_volume: ERROR: Filter not found.\n");
            goto create_volume_err;
        }
        sxi_uuid_parse(filter->uuid, uuid);
        if(sxc_meta_setval(meta, "filterActive", uuid, 16)) {
            if(!hide_errors)
                fprintf(stderr, "create_volume: ERROR: Metadata error.\n");
            goto create_volume_err;
        }
        snprintf(uuidcfg, sizeof(uuidcfg), "%s-cfg", filter->uuid);
        if(filter->configure) {
            char *fdir;
	    int rc = 0;
            fdir = (char*)malloc(strlen(voldir) + 1 + strlen(filter->uuid) + 1); /* The 1 inside is for '/' character. */
            if(!fdir) {
                if(!hide_errors)
                    fprintf(stderr, "create_volume: ERROR: Cannot allocate memory for fdir.\n");
                goto create_volume_err;
            }
            if(access(voldir, F_OK))
                rc = mkdir(voldir, 0700);
            sprintf(fdir, "%s/%s", voldir, filter->uuid);
            if(access(fdir, F_OK)) {
                if(rc == -1 || mkdir(fdir, 0700) == -1) {
                    if(!hide_errors)
                        fprintf(stderr, "create_volume: ERROR: Cannot create filter configuration directory: %s\n", fdir);
                    free(fdir);
                    goto create_volume_err;
                }
	    }
	    if(filter->configure(&filters[filter_idx], filter_cfg, fdir, &cfgdata, &cfgdata_len)) {
		if(!hide_errors)
		    fprintf(stderr, "create_volume: ERROR: Cannot configure filter.\n");
		free(fdir);
		goto create_volume_err;
	    }
	    free(fdir);
	    if(cfgdata && sxc_meta_setval(meta, uuidcfg, cfgdata, cfgdata_len)) {
		if(!hide_errors)
		    fprintf(stderr, "create_volume: ERROR: Cannot store filter configuration.\n");
		goto create_volume_err;
	    }
	}
    }
    if(sxc_volume_add(cluster, volname, VOLSIZE, args->replica_arg, max_revisions, meta, owner)) {
        if(!hide_errors)
            fprintf(stderr, "create_volume: ERROR: %s\n", sxc_geterrmsg(sx));
        goto create_volume_err;
    }
    if(sxi_volume_cfg_store(sx, cluster, volname, filter ? filter->uuid : NULL, cfgdata, cfgdata_len)) { /* TODO: check error message */
        if(!hide_errors)
            fprintf(stderr, "create_volume: ERROR: Configuration problem: %s\n", sxc_geterrmsg(sx));
        goto create_volume_err;
    }
    if(args->human_flag)
        printf("create_volume: Volume '%s' (replica: %d, size: %0.f%c) created.\n", volname, args->replica_arg, to_human(VOLSIZE), to_human_suffix(VOLSIZE));
    else
        printf("create_volume: Volume '%s' (replica: %d, size: %lld) created.\n", volname, args->replica_arg, (long long int)VOLSIZE);

    ret = 0;
create_volume_err:
    free(voldir);
    free(cfgdata);
    sxc_meta_free(meta);
    return ret;
} /* create_volume */

int remove_volume(sxc_client_t *sx, sxc_cluster_t *cluster, const char *volname, int hide_errors) {
    int ret = 1;
    char *voldir = NULL;
    const char *confdir;

    if(sxc_volume_remove(cluster, volname)) {
        if(!hide_errors)
            fprintf(stderr, "remove_volume: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    printf("remove_volume: Volume '%s' removed.\n", volname);
    confdir = sxi_cluster_get_confdir(cluster);
    voldir = (char*)malloc(strlen(confdir) + strlen("/volumes/") + strlen(volname) + 1);
    if(!voldir) {
        if(!hide_errors)
            fprintf(stderr, "remove_volume: ERROR: Cannot allocate memory for voldir.\n");
        return ret;
    }
    sprintf(voldir, "%s/volumes/%s", confdir, volname);
    if(!access(voldir, F_OK) && sxi_rmdirs(voldir)) {
        if(!hide_errors)
            fprintf(stderr, "remove_volume: ERROR: Cannot wipe volume configuration directory: %s\n", voldir);
        goto remove_volume_err;
    }
    
    ret = 0;
remove_volume_err:
    free(voldir);
    return ret;
} /* remove_volume */

int upload_file(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_path, const char *remote_path, int hide_errors) {
    int ret = 1;
    sxc_uri_t *uri;
    sxc_file_t *src, *dest = NULL;
     
    uri = sxc_parse_uri(sx, remote_path);
    if(!uri) {
        if(!hide_errors)
            fprintf(stderr, "upload_file: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    src = sxc_file_local(sx, local_path);
    if(!src) {
        if(!hide_errors)
            fprintf(stderr, "upload_file: ERROR: Cannot open '%s': %s\n", local_path, sxc_geterrmsg(sx));
        goto upload_file_err;
    }
    dest = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
    if(!dest) {
        if(!hide_errors)
            fprintf(stderr, "upload_file: ERROR: Cannot open '%s': %s\n", remote_path, sxc_geterrmsg(sx));
        goto upload_file_err;
    }
    if(sxc_copy(src, dest, local_path[strlen(local_path) - 1] == '/', 0, 0, NULL)) {
        if(!hide_errors)
            fprintf(stderr, "upload_file: ERROR: Cannot upload file: %s\n", sxc_geterrmsg(sx));
        goto upload_file_err;
    }

    ret = 0;
upload_file_err:
    sxc_free_uri(uri);
    sxc_file_free(src);
    sxc_file_free(dest);
    return ret;
} /* upload_file */

/* This function gives you an opened file. Remember to fclose() it. */
FILE* download_file(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_file_path, const char *remote_file_path, int revision) {
    char *rev_char = NULL;
    FILE *ret = NULL;
    sxc_uri_t *uri;
    sxc_file_t *src, *dest = NULL;
    sxc_revlist_t *revs = NULL;
     
    uri = sxc_parse_uri(sx, remote_file_path);
    if(!uri) {
        fprintf(stderr, "download_file: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    src = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
    if(!src) {
        fprintf(stderr, "download_file: ERROR: Cannot open '%s' file.\n", remote_file_path);
        goto download_file_err;
    }
    dest = sxc_file_local(sx, local_file_path);
    if(!dest) {
        fprintf(stderr, "download_file: ERROR: Cannot open '%s'.\n", local_file_path);
        goto download_file_err;
    }
    if(revision > 0) {
        revs = sxc_revisions(src); /* TODO: check error message */
        if(!revs) {
            fprintf(stderr, "download_file: ERROR: Failed to retrieve file revisions: %s\n", sxc_geterrmsg(sx));
            goto download_file_err;
        }
        if(revision > revs->count) {
            fprintf(stderr, "download_file: ERROR: No such a revision number.\n");
            goto download_file_err;
        }
        rev_char = revs->revisions[revision-1]->revision;
        sxc_file_free(src);
        src = sxc_file_remote(cluster, uri->volume, uri->path, rev_char);
        if(!src) {
            fprintf(stderr, "download_file: ERROR: Cannot open '%s' file. (%s)\n", remote_file_path, rev_char);
            goto download_file_err;
        }
        if(sxc_copy_sxfile(src, dest)) {
            fprintf(stderr, "download_file: ERROR: Cannot download file: %s\n", sxc_geterrmsg(sx));
            goto download_file_err;
        }
    } else {
        if(sxc_copy(src, dest, 0, 0, 0, NULL)) {
            fprintf(stderr, "download_file: ERROR: Cannot download file: %s\n", sxc_geterrmsg(sx));
            goto download_file_err;
        }
    }
    ret = fopen(local_file_path, "r");
    if(!ret) {
        fprintf(stderr, "download_file: ERROR: Cannot open '%s' file: %s\n", local_file_path, strerror(errno));
        goto download_file_err;
    }

download_file_err:
    sxc_free_uri(uri);
    sxc_file_free(src);
    sxc_file_free(dest);
    sxc_revisions_free(revs);
    return ret;
} /* download_file */

int download_files(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path) {
    int ret = 1;
    sxc_uri_t *uri;
    sxc_file_t *src, *dest = NULL;
     
    uri = sxc_parse_uri(sx, remote_dir_path);
    if(!uri) {
        fprintf(stderr, "download_files: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    src = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
    if(!src) {
        fprintf(stderr, "download_files: ERROR: Cannot open '%s' file.\n", remote_dir_path);
        goto download_files_err;
    }
    dest = sxc_file_local(sx, local_dir_path);
    if(!dest) {
        fprintf(stderr, "download_files: ERROR: Cannot open '%s' file.\n", local_dir_path);
        goto download_files_err;
    }
    if(sxc_copy(src, dest, 1, 0, 0, NULL)) {
        fprintf(stderr, "download_files: ERROR: Cannot download files: %s\n", sxc_geterrmsg(sx));
        goto download_files_err;
    }

    ret = 0;
download_files_err:
    sxc_free_uri(uri);
    sxc_file_free(src);
    sxc_file_free(dest);
    return ret;
} /* download_files */

int delete_files(sxc_client_t *sx, sxc_cluster_t *cluster, const char *remote_path, int hide_errors) {
    int ret = 1, n;
    char *file_name;
    sxc_uri_t *uri;
    sxc_file_t *file = NULL;
    sxc_file_list_t *lst = NULL;
    sxc_cluster_lf_t *file_list = NULL;

    uri = sxc_parse_uri(sx, remote_path);
    if(!uri) {
        if(!hide_errors)
            fprintf(stderr, "delete_files: ERROR: Bad uri %s: %s\n", remote_path, sxc_geterrmsg(sx));
        return ret;
    }
    lst = sxc_file_list_new(sx, 0);
    if(!lst) {
        if(!hide_errors)
            fprintf(stderr, "delete_files: ERROR: Failed to create new file list.\n");
        sxc_file_free(file);
        goto delete_files_err;
    }
    if(remote_path[strlen(remote_path) - 1] == '/') {
        file_list = sxc_cluster_listfiles(cluster, uri->volume, uri->path, 0, NULL, NULL, NULL, NULL, 0);
        if(!file_list) {
            if(!hide_errors)
                fprintf(stderr, "delete_files: ERROR: Cannot get volume files list.\n");
            goto delete_files_err;
        }
        while(1) {
            n = sxc_cluster_listfiles_next(file_list, &file_name, NULL, NULL, NULL);
            if(n <= 0) {
                if(n) {
                    if(!hide_errors)
                        fprintf(stderr, "delete_files: ERROR: Failed to retrieve file name for '%s' directory.\n", remote_path);
                    goto delete_files_err;
                }
                break;
            }
            if(!file_name) {
                if(!hide_errors)
                    fprintf(stderr, "delete_files: ERROR: NULL file name pointer received.\n");
                goto delete_files_err;
            }
            file = sxc_file_remote(cluster, uri->volume, file_name, NULL);
            if(!file) {
                if(!hide_errors)
                    fprintf(stderr, "delete_files: ERROR: Cannot process file: %s\n", file_name);
                free(file_name);
                goto delete_files_err;
            }
            if(sxc_file_list_add(lst, file, 0)) {
                if(!hide_errors)
                    fprintf(stderr, "delete_files: ERROR: Cannot add file list entry '%s': %s\n", file_name, sxc_geterrmsg(sx));
                sxc_file_free(file);
                free(file_name);
                goto delete_files_err;
            }
            free(file_name);
        }
    } else {
        file = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
        if(!file) {
            if(!hide_errors)
                fprintf(stderr, "delete_files: ERROR: Cannot open destination directory.\n");
            goto delete_files_err;
        }
        if(sxc_file_list_add(lst, file, 0)) {
            if(!hide_errors)
                fprintf(stderr, "delete_files: ERROR: Cannot add file list entry '%s': %s\n", remote_path, sxc_geterrmsg(sx));
            sxc_file_free(file);
            goto delete_files_err;
        }
    }
    if(sxc_rm(lst)) {
        if(!hide_errors)
            fprintf(stderr, "delete_files: ERROR: Failed to remove file list: %s\n", sxc_geterrmsg(sx));
        goto delete_files_err;
    }

    ret = 0;
delete_files_err:
    sxc_free_uri(uri);
/*  sxc_file_free(file); */         /* done in sx_file_files_free(lst); */
    sxc_file_list_free(lst);
    sxc_cluster_listfiles_free(file_list);
    return ret;
} /* delete_files */

/* -1 - error
 *  0 - file not found
 *  1 - file found*/
int find_file(sxc_client_t *sx, sxc_cluster_t *cluster, const char *remote_file_path, int hide_errors) {
    int ret = -1, n;
    char *file_name = NULL;
    sxc_uri_t *uri;
    sxc_file_list_t *lst = NULL;
    sxc_cluster_lf_t *file_list;

    uri = sxc_parse_uri(sx, remote_file_path);
    if(!uri) {
        if(!hide_errors)
            fprintf(stderr, "find_file: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    file_list = sxc_cluster_listfiles(cluster, uri->volume, uri->path, 0, NULL, NULL, NULL, NULL, 0);
    if(!file_list) {
        if(!hide_errors)
            fprintf(stderr, "find_file: ERROR: Cannot get volume files list.\n");
        goto find_file_err;
    }
    n = sxc_cluster_listfiles_next(file_list, &file_name, NULL, NULL, NULL);
    if(n < 0) {
        if(!hide_errors)
            fprintf(stderr, "find_file: ERROR: Failed to retrieve file name for '%s'.\n", remote_file_path);
        goto find_file_err;
    }
    if(n > 0 && !file_name) {
        if(!hide_errors)
            fprintf(stderr, "find_file: ERROR: NULL file name pointer received.\n");
        goto find_file_err;
    }

    ret = n?1:0;
find_file_err:
    free(file_name);
    sxc_free_uri(uri);
    sxc_file_list_free(lst);
    sxc_cluster_listfiles_free(file_list);
    return ret;
} /* find_file */

void create_block(rnd_state_t *state, unsigned char *block, uint64_t block_size)
{
    uint64_t i;
    for(i=0; i<block_size; i++)
        block[i] = rand_2cmres(state);
} /* create_block */

int create_file(const char* local_file_path, uint64_t block_size, uint64_t block_count, unsigned char sha_hash[SHA_DIGEST_LENGTH], int force_size) {
    int i, ret = 1;
    uint64_t seed;
    unsigned char *block;
    FILE *file = NULL;
    rnd_state_t state;
    SHA_CTX ctx;

    block = (unsigned char*)malloc(block_size);
    if(!block) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot allocate memory for block.\n");
        return ret;
    }
    seed = make_seed();
    printf("create_file: Seed: %012lx\n", seed);
    rnd_seed(&state, seed);
    if(!force_size)
        switch(block_size) {
            case SX_BS_SMALL:
                if(block_count > 31)
                    fprintf(stderr, "create_file: WARNING: File size out of set block size bounds.\n");
                break;
            case SX_BS_MEDIUM:
                if(block_count < 32 || block_count > 8192)
                    fprintf(stderr, "create_file: WARNING: File size out of set block size bounds.\n");
                break;
            case SX_BS_LARGE:
                if(block_count < 129)
                    fprintf(stderr, "create_file: WARNING: File size out of set block size bounds.\n");
                break;
            default:
                fprintf(stderr, "create_file: ERROR: Unknown block size.\n");
                goto create_file_err;;
        }
    create_block(&state, block, block_size);
    file = fopen(local_file_path, "wb");
    if(!file) {
        fprintf(stderr, "create_file: ERROR: Cannot create '%s' file: %s\n", local_file_path, strerror(errno));
        goto create_file_err;
    }
    if(sha_hash && !SHA1_Init(&ctx)) {
        fprintf(stderr, "create_file: ERROR: SHA1_Init() failure.\n");
        goto create_file_err;
    }
    for(i=0; i<block_count; i++) {
        if(fwrite(block, sizeof(unsigned char), block_size, file) != block_size) {
            fprintf(stderr, "create_file: ERROR: Error while writing to '%s' file. (%d)\n", local_file_path, i);
            goto create_file_err;
        }
        if(sha_hash && !SHA1_Update(&ctx, block, block_size)) {
            fprintf(stderr, "create_file: ERROR: SHA1_Update() failure. (%d).\n", i);
            goto create_file_err;
        }
    }
    if(sha_hash && !SHA1_Final(sha_hash, &ctx)) {
        fprintf(stderr, "create_file: ERROR: SHA1_Final() failure.\n");
        goto create_file_err;
    }
    if(fclose(file) == EOF) {
        fprintf(stderr, "create_file: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
        if(unlink(local_file_path))
            fprintf(stderr, "create_file: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        file = NULL;
        goto create_file_err;
    }

    ret = 0;
create_file_err:
    free(block);
    if(ret && file) {
        if(fclose(file) == EOF)
            fprintf(stderr, "create_file: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
        if(unlink(local_file_path))
            fprintf(stderr, "create_file: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
    }
    return ret;
} /* create_file */

int test_empty_file(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path) {
    int ret = 1;
    char *local_file_path, *remote_file_path = NULL;
    FILE *file = NULL;
    sxc_uri_t *uri;

    printf("test_empty_file: Started\n");
    uri = sxc_parse_uri(sx, remote_dir_path);
    if(!uri) {
        fprintf(stderr, "test_empty_file: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(EMPTY_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_empty_file_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, EMPTY_FILE_NAME);
    remote_file_path = (char*)malloc(strlen(remote_dir_path) + strlen(EMPTY_FILE_NAME) + 1);
    if(!remote_file_path) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot allocate memory for remote_file_path.\n");
        goto test_empty_file_err;
    }
    sprintf(remote_file_path, "%s%s", remote_dir_path, EMPTY_FILE_NAME);
    file = fopen(local_file_path, "w");
    if(!file) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot create '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_empty_file_err;
    }
    if(fclose(file) == EOF) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_empty_file_err;
    }
    if(upload_file(sx, cluster, local_file_path, remote_dir_path, 0)) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot upload '%s' file.\n", local_file_path);
        goto test_empty_file_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 0)) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot delete '%s' file.\n", remote_file_path);
        goto test_empty_file_err;
    }

    ret = 0;
    printf("test_empty_file: Succeeded\n");
test_empty_file_err:
    if(file && unlink(local_file_path)) {
        fprintf(stderr, "test_empty_file: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        ret = 1;
    }
    free(remote_file_path);
    free(local_file_path);
    sxc_free_uri(uri);
    return ret;
} /* test_empty_file */

int test_upload_and_download(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path, uint64_t block_size, uint64_t count, int human, int size_flag) {
    int tmp, ret = 1;
    char *local_file_path = NULL, *remote_file_path = NULL;
    unsigned char *block = NULL, hash1[SHA_DIGEST_LENGTH], hash2[SHA_DIGEST_LENGTH];
    FILE *file = NULL;
    SHA_CTX ctx;
    
    printf("test_upload_and_download: Started\n");
    if(sxc_cluster_set_progress_cb(sx, cluster, test_callback, (void*)&bytes)) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot set callback.\n");
        return ret;
    }
    block = (unsigned char*)malloc(block_size);
    if(!block) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot allocate memory for block.\n");
        goto test_upload_and_download_err;
    }
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(UD_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_upload_and_download_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, UD_FILE_NAME);
    remote_file_path = (char*)malloc(strlen(remote_dir_path) + strlen(UD_FILE_NAME) + 1);
    if(!remote_file_path) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot allocate memory for remote_file_path.\n");
        goto test_upload_and_download_err;
    }
    sprintf(remote_file_path, "%s%s", remote_dir_path, UD_FILE_NAME);
    if(human)
        printf("test_upload_and_download: Creating file of size: %.2f%c (%" PRIu64 "*%.0f%c)\n", to_human(block_size*count), to_human_suffix(block_size*count), count, to_human(block_size), to_human_suffix(block_size));
    else
        printf("test_upload_and_download: Creating file of size: %" PRIu64 " (%" PRIu64 "*%" PRIu64 ")\n", block_size*count, count, block_size);
    if(create_file(local_file_path, block_size, count, hash1, 0)) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot create '%s' file.\n", local_file_path);
        goto test_upload_and_download_err;
    }
    printf("test_upload_and_download: Uploading\n");
    if(upload_file(sx, cluster, local_file_path, remote_dir_path, 0)) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot upload '%s' file.\n", local_file_path);
        if(unlink(local_file_path))
            fprintf(stderr, "test_upload_and_download: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_upload_and_download_err;
    }
    if(unlink(local_file_path)) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_upload_and_download_err;
    }
    if(size_flag && (uint64_t)block_size != bytes) {
        fprintf(stderr, "test_upload_and_download: ERROR: Uploaded wrong number of data.\n");
        goto test_upload_and_download_err;
    }
    printf("test_upload_and_download: Downloading\n");
    file = download_file(sx, cluster, local_file_path, remote_file_path, 0);
    if(!file) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot download '%s' file.\n", remote_file_path);
        goto test_upload_and_download_err;
    }
    if(!SHA1_Init(&ctx)) {
        fprintf(stderr, "test_upload_and_download: ERROR: SHA1_Init() failure.\n");
        goto test_upload_and_download_err;
    }
    while((tmp = fread(block, sizeof(unsigned char), block_size, file))) {
        if(!SHA1_Update(&ctx, block, tmp)) {
            fprintf(stderr, "test_upload_and_download: ERROR: SHA1_Update() failure.\n");
            goto test_upload_and_download_err;
        }
        if(tmp < block_size) {
            fprintf(stderr, "test_upload_and_download: ERROR: Downloaded only a part of file.\n");
            goto test_upload_and_download_err;
        }
    }
    if(!SHA1_Final(hash2, &ctx)) {
        fprintf(stderr, "test_upload_and_download: ERROR: SHA1_Final() failure.\n");
        goto test_upload_and_download_err;
    }
    if(memcmp(hash1, hash2, SHA_DIGEST_LENGTH)) {
        fprintf(stderr, "test_upload_and_download: ERROR: Uploaded and downloaded file differs.\n");
        goto test_upload_and_download_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 0)) {
        fprintf(stderr, "test_upload_and_download: ERROR: Cannot delete '%s' file.\n", remote_file_path);
        goto test_upload_and_download_err;
    }
    switch(find_file(sx, cluster, remote_file_path, 0)) {
        case -1:
            fprintf(stderr, "test_upload_and_download: ERROR: Looking for '%s' file in %s failed.\n", UD_FILE_NAME, remote_file_path);
            goto test_upload_and_download_err;
        case 0: break;
        case 1:
            fprintf(stderr, "test_upload_and_download: ERROR: '%s' file has not been deleted correctly.\n", UD_FILE_NAME);
            goto test_upload_and_download_err;
    }
    
    ret = 0;
    printf("test_upload_and_download: Succeeded\n");
test_upload_and_download_err:
    free(block);
    if(file) {
        if(fclose(file) == EOF) {
            fprintf(stderr, "test_upload_and_download: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
            ret = 1;
        } else if(unlink(local_file_path)) {
            fprintf(stderr, "test_upload_and_download: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            ret = 1;
        }
    }
    free(local_file_path);
    free(remote_file_path);
    return ret;
} /* test_upload_and_download */

int test_revision(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path, uint64_t block_size, uint64_t count, int human, int max_revisions) {
    int i, tmp, ret = 1;
    char *local_file_path = NULL, *remote_file_path = NULL;
    unsigned char *block, hash[SHA_DIGEST_LENGTH], **hashes = NULL;
    FILE *file = NULL;
    SHA_CTX ctx;

    printf("test_revision: Started (revision: %d)\n", max_revisions);
    block = (unsigned char*)malloc(block_size);
    if(!block) {
        fprintf(stderr, "test_revision: ERROR: Cannot allocate memory for block.\n");
        return ret;
    }
    hashes = (unsigned char**)calloc(max_revisions, sizeof(unsigned char*));
    if(!hashes) {
        fprintf(stderr, "test_revision: ERROR: Cannot allocate memory for hashes.\n");
        goto test_revision_err;
    }
    for(i=0; i<max_revisions; i++) {
        hashes[i] = (unsigned char*)malloc(SHA_DIGEST_LENGTH);
        if(!hashes[i]) {
            fprintf(stderr, "test_revision: ERROR: Cannot allocate memory for hashes[%d].)\n", i);
            goto test_revision_err;
        }
    }
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(REV_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_revision: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_revision_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, REV_FILE_NAME);
    remote_file_path = (char*)malloc(strlen(remote_dir_path) + strlen(REV_FILE_NAME) + 1);
    if(!remote_file_path) {
        fprintf(stderr, "test_revision: ERROR: Cannot allocate memory for remote_file_path.\n");
        goto test_revision_err;
    }
    sprintf(remote_file_path, "%s%s", remote_dir_path, REV_FILE_NAME);
    if(human)
        printf("test_revision: Creating and uploading files of size: %.2f%c (%" PRIu64 "*%.0f%c)\n", to_human(block_size*count), to_human_suffix(block_size*count), count, to_human(block_size), to_human_suffix(block_size));
    else
        printf("test_revision: Creating and uploading files of size: %" PRIu64 " (%" PRIu64 "*%" PRIu64 ")\n", block_size*count, count, block_size);
    for(i=0; i<max_revisions; i++) {
        if(create_file(local_file_path, block_size, count, hashes[max_revisions-1-i], !i)) {
            fprintf(stderr, "test_revision: ERROR: Cannot create '%s' file.\n", local_file_path);
            goto test_revision_err;
        }
        if(upload_file(sx, cluster, local_file_path, remote_dir_path, 0)) {
            fprintf(stderr, "test_revision: ERROR: Cannot upload '%s' file.\n", local_file_path);
            if(unlink(local_file_path))
                fprintf(stderr, "test_revision: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            goto test_revision_err;
        }
        if(unlink(local_file_path)) {
            fprintf(stderr, "test_revision: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            goto test_revision_err;
        }
    }
    printf("test_revision: Downloading and checking file versions.\n");
    for(i=0; i<max_revisions; i++) {
        file = download_file(sx, cluster, local_file_path, remote_file_path, i+1);
        if(!file) {
            fprintf(stderr, "test_revision: ERROR: Cannot download '%s' file.\n", remote_file_path);
            goto test_revision_err;
        }
        if(!SHA1_Init(&ctx)) {
            fprintf(stderr, "test_revision: ERROR: SHA1_Init() failure while downloading file. (%d)\n", i);
            goto test_revision_err;
        }
        while((tmp = fread(block, sizeof(unsigned char), block_size, file))) {
            if(!SHA1_Update(&ctx, block, tmp)) {
                fprintf(stderr, "test_revision: ERROR: SHA1_Update() failure while downloading file. (%d)\n", i);
                goto test_revision_err;
            }
            if(tmp < block_size) {
                fprintf(stderr, "test_revision: ERROR: Downloaded only a part of file.\n");
                goto test_revision_err;
            }
        }
        if(fclose(file) == EOF) {
            fprintf(stderr, "test_revision: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
            if(unlink(local_file_path))
                fprintf(stderr, "test_revision: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            file = NULL;
            goto test_revision_err;
        }
        if(unlink(local_file_path)) {
            fprintf(stderr, "test_revision: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            file = NULL;
            goto test_revision_err;
        }
        file = NULL;
        if(!SHA1_Final(hash, &ctx)) {
            fprintf(stderr, "test_revision: ERROR: SHA1_Final() failure while downloading file. (%d)\n", i);
            goto test_revision_err;
        }
        if(memcmp(hash, hashes[i], SHA_DIGEST_LENGTH)) {
            fprintf(stderr, "test_revision: ERROR: Uploaded and downloaded file differs. (%d)\n", i);
            goto test_revision_err;
        }
    }
    if(delete_files(sx, cluster, remote_file_path, 0)) {
        fprintf(stderr, "test_revision: ERROR: Cannot delete '%s' file.\n", remote_file_path);
        goto test_revision_err;
    }
    switch(find_file(sx, cluster, remote_file_path, 0)) {
        case -1:
            fprintf(stderr, "test_revision: ERROR: Looking for '%s' file in %s failed.\n", REV_FILE_NAME, remote_file_path);
            goto test_revision_err;
        case 0: break;
        case 1:
            fprintf(stderr, "test_revision: ERROR: '%s' file has not been deleted correctly.\n", REV_FILE_NAME);
            goto test_revision_err;
    }

    ret = 0;
    printf("test_revision: Succeeded\n");
test_revision_err:
    if(file) {
        if(fclose(file) == EOF) {
            fprintf(stderr, "test_revision: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
            ret = 1;
        } else if(unlink(local_file_path)) {
            fprintf(stderr, "test_revision: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            ret = 1;
        }
    }
    free(local_file_path);
    free(remote_file_path);
    if(hashes)
        for(i=0; i<max_revisions; i++)
            free(hashes[i]);
    free(hashes);
    free(block);
    return ret;
} /* test_revision */

int test_cat(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path) {
    int fd = 0, ret = 1, tmp;
    char *local_file_path = NULL, *cat_file_path = NULL, *remote_file_path = NULL;
    unsigned char *block = NULL, hash_in[SHA_DIGEST_LENGTH], hash_out[SHA_DIGEST_LENGTH];
    FILE *file = NULL;
    sxc_uri_t *uri = NULL;
    sxc_file_t *src = NULL;
    SHA_CTX ctx;

    printf("test_cat: Started\n");
    block = (unsigned char*)malloc(SX_BS_LARGE);
    if(!block) {
        fprintf(stderr, "test_cat: ERROR: Cannot allocate memory for block.\n");
        return ret;
    }
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(CAT_FILE_NAME_IN) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_cat: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_cat_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, CAT_FILE_NAME_IN);
    cat_file_path = (char*)malloc(strlen(local_dir_path) + strlen(CAT_FILE_NAME_OUT) + 1);
    if(!cat_file_path) {
        fprintf(stderr, "test_cat: ERROR: Cannot allocate memory for cat_file_path.\n");
        goto test_cat_err;
    }
    sprintf(cat_file_path, "%s%s", local_dir_path, CAT_FILE_NAME_OUT);
    remote_file_path = (char*)malloc(strlen(remote_dir_path) + strlen(CAT_FILE_NAME_IN) + 1);
    if(!remote_file_path) {
        fprintf(stderr, "test_cat: ERROR: Cannot allocate memory for remote_file_path.\n");
        goto test_cat_err;
    }
    sprintf(remote_file_path, "%s%s", remote_dir_path, CAT_FILE_NAME_IN);
    if(create_file(local_file_path, SX_BS_LARGE, CAT_FILE_SIZE, hash_in, 1)) {
        fprintf(stderr, "test_cat: ERROR: Cannot create new file.\n");
        goto test_cat_err;
    }
    if(upload_file(sx, cluster, local_file_path, remote_file_path, 0)) {
        fprintf(stderr, "test_cat: ERROR: Cannot upload '%s' file.\n", local_file_path);
        if(unlink(local_file_path))
            fprintf(stderr, "test_cat: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_cat_err;
    }
    if(unlink(local_file_path)) {
        fprintf(stderr, "test_cat: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_cat_err;
    }
    uri = sxc_parse_uri(sx, remote_file_path);
    if(!uri) {
	fprintf(stderr, "test_cat: ERROR: %s\n", sxc_geterrmsg(sx));
        goto test_cat_err;
    }
    src = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
    if(!src) {
        fprintf(stderr, "test_cat: ERROR: Cannot open '%s': %s\n", remote_file_path, sxc_geterrmsg(sx));
        goto test_cat_err;
    }
    fd = open(cat_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0) {
        fprintf(stderr, "test_cat: ERROR: Cannot create new file.\n");
        goto test_cat_err;
    }
    printf("test_cat: Processing the file.\n"); /* It shows (on stdout) that program works */
    if(sxc_cat(src, fd)) {
        fprintf(stderr, "test_cat: ERROR: %s\n", sxc_geterrmsg(sx));
        goto test_cat_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 0)) {
        fprintf(stderr, "test_cat: ERROR: Cannot delete '%s' file.\n", remote_file_path);
        goto test_cat_err;
    }
    file = fopen(cat_file_path, "rb");
    if(!file) {
        fprintf(stderr, "test_cat: ERROR: Cannot open '%s' file: %s\n", cat_file_path, strerror(errno));
        goto test_cat_err;
    }
    if(!SHA1_Init(&ctx)) {
        fprintf(stderr, "test_cat: ERROR: SHA1_Init() failure.\n");
        if(fclose(file))
            fprintf(stderr, "test_cat: ERROR: Cannot close '%s' file: %s\n", cat_file_path, strerror(errno));
        goto test_cat_err;
    }
    while((tmp = fread(block, sizeof(unsigned char), SX_BS_LARGE, file))) {
        if(!SHA1_Update(&ctx, block, tmp)) {
            fprintf(stderr, "test_cat: ERROR: SHA1_Update() failure.\n");
            if(fclose(file))
                fprintf(stderr, "test_cat: ERROR: Cannot close '%s' file: %s\n", cat_file_path, strerror(errno));
            goto test_cat_err;
        }
        if(tmp < SX_BS_LARGE) {
            if(fclose(file))
                fprintf(stderr, "test_cat: ERROR: Cannot close '%s' file: %s\n", cat_file_path, strerror(errno));
            fclose(file);
            goto test_cat_err;
        }
    }
    if(fclose(file))
        fprintf(stderr, "test_cat: ERROR: Cannot close '%s' file: %s\n", cat_file_path, strerror(errno));
    if(!SHA1_Final(hash_out, &ctx)) {
        fprintf(stderr, "test_cat_file: ERROR: SHA1_Final() failure.\n");
        goto test_cat_err;
    }
    if(memcmp(hash_in, hash_out, SHA_DIGEST_LENGTH)) {
        fprintf(stderr, "test_cat: ERROR: File from cat differs.\n");
        goto test_cat_err;
    }
    printf("test_cat: Succeeded\n");

    ret = 0;
test_cat_err:
    if(fd && close(fd)) {
        fprintf(stderr, "test_cat: ERROR: Cannot close '%s' file: %s\n", cat_file_path, strerror(errno));
        ret = 1;
    }
    if(file && unlink(cat_file_path)) {
        fprintf(stderr, "test_cat: ERROR: Cannot delete '%s' file: %s\n", cat_file_path, strerror(errno));
        ret = 1;
    }
    free(block);
    free(local_file_path);
    free(cat_file_path);
    free(remote_file_path);
    sxc_free_uri(uri);
    sxc_file_free(src);
    return ret;
} /* test_cat */

/* For test_upload_and_download:
 *       Block size | Available number of blocks
 *    SX_BS_SMALL   |  0 - 31
 *    SX_BS_MEDIUM  |  8 - 8192
 *    SX_BS_LARGE   |  129+
 * REMEMBER TO CHECK WHETHER THE VOLUME SIZE IS BIG ENOUGH!! */
int run_tests(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path, const struct gengetopt_args_info *args, int max_revisions, int size_flag) {
    if(test_empty_file(sx, cluster, local_dir_path, remote_dir_path)) {
        fprintf(stderr, "run_tests: ERROR: test_empty_file() failed.\n");
        return 1;
    }
    if(test_upload_and_download(sx, cluster, local_dir_path, remote_dir_path, SX_BS_SMALL, 26, args->human_flag, size_flag)) {
        fprintf(stderr, "run_tests: ERROR: test_upload_and_download failed.\n");
        return 1;
    }
    if(test_upload_and_download(sx, cluster, local_dir_path, remote_dir_path, SX_BS_MEDIUM, 2314, args->human_flag, size_flag)) {
        fprintf(stderr, "run_tests: ERROR: test_upload_and_download failed.\n");
        return 1;
    }
    if(args->all_flag && test_upload_and_download(sx, cluster, local_dir_path, remote_dir_path, SX_BS_LARGE, 285, args->human_flag, size_flag)) {
        fprintf(stderr, "run_tests: ERROR: test_upload_and_download failed.\n");
        return 1;
    }
    if(test_revision(sx, cluster, local_dir_path, remote_dir_path, SX_BS_SMALL, 29, args->human_flag, max_revisions)) {
        fprintf(stderr, "run_tests: ERROR: test_revision failed.\n");
        return 1;
    }
    if(test_revision(sx, cluster, local_dir_path, remote_dir_path, SX_BS_MEDIUM, 649, args->human_flag, max_revisions)) {
        fprintf(stderr, "run_tests: ERROR: test_revision failed.\n");
        return 1;
    }
    if(args->all_flag && test_revision(sx, cluster, local_dir_path, remote_dir_path, SX_BS_LARGE, 131, args->human_flag, max_revisions)) {
        fprintf(stderr, "run_tests: ERROR: test_revision failed.\n");
        return 1;
    }
    if(test_cat(sx, cluster, local_dir_path, remote_dir_path))
        return 1;
    return 0;
} /* run_tests */

int test_attribs(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path) {
    int i, ret = 1, owner, group, other;
    long int tmp_time;
    char *local_files_paths[ATTRIBS_COUNT], *remote_files_paths[ATTRIBS_COUNT];
    uint64_t seed;
    mode_t attribs;
    rnd_state_t state;
    struct utimbuf time;
    struct timeval tv;
    struct stat st, t_st[ATTRIBS_COUNT];
    FILE *files[ATTRIBS_COUNT];

    printf("test_attribs: Started\n");
    memset(files, 0, sizeof(files));
    memset(local_files_paths, 0, sizeof(local_files_paths));
    memset(remote_files_paths, 0, sizeof(remote_files_paths));
    seed = make_seed();
    printf("test_attribs: Seed: %012lx\n", seed);
    rnd_seed(&state, seed);
    for(i=0; i<ATTRIBS_COUNT; i++) {
        local_files_paths[i] = (char*)malloc(strlen(local_dir_path) + strlen(ATTRIBS_FILE_NAME) + 2 + 1);
        if(!local_files_paths[i]) {
            fprintf(stderr, "test_attribs: ERROR: Cannot allocate memory for local_files_paths[%d].\n", i);
            goto test_attribs_err;
        }
        sprintf(local_files_paths[i], "%s%s%d", local_dir_path, ATTRIBS_FILE_NAME, i);
        remote_files_paths[i] = (char*)malloc(strlen(remote_dir_path) + strlen(ATTRIBS_FILE_NAME) + 2 + 1);
        if(!remote_files_paths[i]) {
            fprintf(stderr, "test_attribs: ERROR: Cannot allocate memory for remote_files_paths[%d].\n", i);
            goto test_attribs_err;
        }
        sprintf(remote_files_paths[i], "%s%s%d", remote_dir_path, ATTRIBS_FILE_NAME, i);
        files[i] = fopen(local_files_paths[i], "w");
        if(!files[i]) {
            fprintf(stderr, "test_attribs: ERROR: Cannot open '%s' file: %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
        }
        if(fclose(files[i]) == EOF) {
            fprintf(stderr, "test_attribs: ERROR: Cannot close '%s' file: %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
        }
        owner = (rand_2cmres(&state)%8)|4;
        group = rand_2cmres(&state)%8;
        other = rand_2cmres(&state)%8;
        printf("test_attribs: rights being tested: %c%c%c%c%c%c%c%c%c\n", owner&4?'r':'-', owner&2?'w':'-', owner&1?'x':'-', group&4?'r':'-', group&2?'w':'-', group&1?'x':'-', other&4?'r':'-', other&2?'w':'-', other&1?'x':'-');
        attribs = (owner<<6) | (group<<3) | other;
        if(chmod(local_files_paths[i], attribs)) {
            fprintf(stderr, "test_attribs: ERROR: Cannot set attributes for '%s' file: %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
        }
        if(gettimeofday(&tv, NULL)) {
            fprintf(stderr, "test_attribs: ERROR: Cannot get current time: %s\n", strerror(errno));
            goto test_attribs_err;
        }
        tmp_time = (long int)rand_2cmres(&state) % 100000000;
        if((owner + group + other)&1)
            tmp_time *= -1;
        time.actime = 0;
        time.modtime = tv.tv_sec + tmp_time;
        if(utime(local_files_paths[i], &time)) {
            fprintf(stderr, "test_attribs: ERROR: Cannot set modification time for '%s' file: %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
        }
        if(stat(local_files_paths[i], &t_st[i]) == -1) {
            fprintf(stderr, "test_attribs: ERROR: stat() failed for '%s': %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
	}
    }
    if(upload_file(sx, cluster, local_dir_path, remote_dir_path, 0)) {
        fprintf(stderr, "test_attribs: ERROR: Cannot upload files from '%s'.\n", local_dir_path);
        goto test_attribs_err;
    }
    for(i=0; i<ATTRIBS_COUNT; i++) {
       files[i] = NULL;
       if(unlink(local_files_paths[i])) {
            fprintf(stderr, "test_attribs: ERROR: Cannot delete '%s' file: %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
       }
    }
    if(download_files(sx, cluster, local_dir_path, remote_dir_path)) {
        fprintf(stderr, "test_attribs: ERROR: Cannot download files from '%s'.\n", remote_dir_path);
        goto test_attribs_err;
    }
    memset(files, 1, sizeof(files));
    for(i=0; i<ATTRIBS_COUNT; i++) {
        if(stat(local_files_paths[i], &st) == -1) {
            fprintf(stderr, "test_attribs: ERROR: stat() failed for '%s': %s\n", local_files_paths[i], strerror(errno));
            goto test_attribs_err;
	}
        if(st.st_mode != t_st[i].st_mode) {
            fprintf(stderr, "test_attribs: ERROR: File attributes differ.\n");
            goto test_attribs_err;
        }
        if(st.st_mtime != t_st[i].st_mtime) {
            fprintf(stderr, "test_attribs: ERROR: File modification time differs.\n");
            goto test_attribs_err;
        }
    }
    if(delete_files(sx, cluster, remote_dir_path, 0)) {
        fprintf(stderr, "test_attribs: ERROR: Cannot delete files from '%s'.\n", remote_dir_path);
        goto test_attribs_err;
    }

    printf("test_attribs: Succeeded\n");
    ret = 0;
test_attribs_err:
    for(i=0; i<ATTRIBS_COUNT; i++) {
        if(local_files_paths[i]) {
            unlink(local_files_paths[i]);
            free(local_files_paths[i]);
        }
        free(remote_files_paths[i]);
    }
    return ret;
} /* test_attribs */

int test_undelete(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *remote_dir_path) {
    int ret = 1;
    char *local_file_path, *remote_file_path = NULL;
    FILE *file = NULL;
    sxc_uri_t *uri;

    printf("test_undelete: Started\n");
    uri = sxc_parse_uri(sx, remote_dir_path);
    if(!uri) {
        fprintf(stderr, "test_undelete: ERROR: %s\n", sxc_geterrmsg(sx));
        return ret;
    }
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(UNDELETE_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_undelete: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_undelete_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, UNDELETE_FILE_NAME);
    remote_file_path = (char*)malloc(strlen(remote_dir_path) + strlen(TRASH_NAME) + strlen(UNDELETE_FILE_NAME) + 1);
    if(!remote_file_path) {
        fprintf(stderr, "test_undelete: ERROR: Cannot allocate memory for remote_file_path.\n");
        goto test_undelete_err;
    }
    sprintf(remote_file_path, "%s%s", remote_dir_path, UNDELETE_FILE_NAME);
    file = fopen(local_file_path, "w");
    if(!file) {
        fprintf(stderr, "test_undelete: ERROR: Cannot open '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_undelete_err;
    }
    if(fclose(file) == EOF) {
        fprintf(stderr, "test_undelete: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_undelete_err;
    }
    if(upload_file(sx, cluster, local_file_path, remote_file_path, 0)) {
        fprintf(stderr, "test_undelete: ERROR: Cannot upload '%s' file.\n", local_file_path);
        goto test_undelete_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 0)) {
        fprintf(stderr, "test_undelete: ERROR: Cannot delete '%s' file.\n", remote_file_path);
        goto test_undelete_err;
    }
    sprintf(remote_file_path, "sx://%s%s%s/%s%s/%s/%s", uri->profile ? uri->profile : "", uri->profile ? "@" : "", uri->host, uri->volume, TRASH_NAME, REMOTE_DIR, UNDELETE_FILE_NAME);
    switch(find_file(sx, cluster, remote_file_path, 0)) {
        case -1:
            fprintf(stderr, "test_undelete: ERROR: Looking for '%s' file in %s failed.\n", UNDELETE_FILE_NAME, remote_file_path);
            goto test_undelete_err;
        case 0:
            fprintf(stderr, "test_undelete: ERROR: '%s' file has not been deleted correctly.\n", UNDELETE_FILE_NAME);
            goto test_undelete_err;
        case 1: break;
    }
    remote_file_path[strlen(remote_file_path) - strlen(UNDELETE_FILE_NAME)] = 0;
    if(delete_files(sx, cluster, remote_file_path, 0)) {
        fprintf(stderr, "test_undelete: ERROR: Cannot delete files from '%s'.\n", remote_file_path);
        goto test_undelete_err;
    }

    printf("test_undelete: Succeeded\n");
    ret = 0;
test_undelete_err:
    if(file && unlink(local_file_path)) {
        fprintf(stderr, "test_undelete: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        ret = 1;
    }
    free(local_file_path);
    free(remote_file_path);
    sxc_free_uri(uri);
    return ret;
} /* test_undelete */

int volume_test(sxc_client_t *sx, sxc_cluster_t *cluster, const char *volname, const char *filter_dir, const char *filter_name, const char *filter_cfg, const char *local_dir_path, const char *remote_dir_path, const struct gengetopt_args_info *args, int max_revisions) {
    int size_flag;
    if(filter_name && (!strcmp(filter_name, "zcomp") || !strcmp(filter_name, "aes256")))
        size_flag = 0;
    else
        size_flag = 1;
    printf("\nVolume test - filter: %s; filter configuration: %s\n", filter_name?filter_name:"<no filter>", filter_cfg?filter_cfg:"<none>");
    if(create_volume(sx, cluster, volname, args->owner_arg, filter_dir, filter_name, filter_cfg, args, max_revisions, 0)) {
        fprintf(stderr, "volume_test: ERROR: Cannot create new volume.\n");
        return 1;
    }
    if(run_tests(sx, cluster, local_dir_path, remote_dir_path, args, max_revisions, size_flag))
        return 1;
    if(filter_name) {
        if(!strcmp(filter_name, "attribs") && test_attribs(sx, cluster, local_dir_path, remote_dir_path)) {
            fprintf(stderr, "volume_test: ERROR: Attributes test failed.\n");
            return 1;
        }
        /* This has to be executed at the very end because it removes all the files from trash. */
        if(!strcmp(filter_name, "undelete") && test_undelete(sx, cluster, local_dir_path, remote_dir_path)) {
            fprintf(stderr, "volume_test: ERROR: Undelete test failed.\n");
            return 1;
        }
    }
    if(remove_volume(sx, cluster, volname, 0)) {
        fprintf(stderr, "volume_test: ERROR: Cannot remove '%s' volume.\n", volname);
        return 1;
    }
    return 0;
} /* volume_test */

int test_quota(sxc_client_t *sx, sxc_cluster_t *cluster, const char *local_dir_path, const char *host, const struct gengetopt_args_info *args) {
    int ret = 1, file = 0;
    char *volname, *local_file_path = NULL, *remote_path = NULL;
    sxc_file_t *src = NULL, *dest = NULL;

    printf("\ntest_quota: Started\n");
    volname = (char*)malloc(strlen(VOLNAME) + strlen("XXXXXX") + 1);
    if(!volname) {
        fprintf(stderr, "test_quota: ERROR: Cannot allocate memory for volname.\n");
        return ret;
    }
    sprintf(volname, "%sXXXXXX", VOLNAME);
    if(randomize_name(volname))
        goto test_quota_err;
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(QUOTA_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_quota: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_quota_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, QUOTA_FILE_NAME);
    remote_path = (char*)malloc(strlen("sx://") + strlen(args->owner_arg) + 1 + strlen(host) + 1 + strlen(volname) + 1 + strlen(REMOTE_DIR) + 1 + strlen(QUOTA_FILE_NAME) + 1); /* The 1's inside are for '@' and '/' characters. */
    if(!remote_path) {
        fprintf(stderr, "test_quota: ERROR: Cannot allocate memory for remote_path.\n");
        goto test_quota_err;
    }
    sprintf(remote_path, "%s/%s", REMOTE_DIR, QUOTA_FILE_NAME);
    if(sxc_volume_add(cluster, volname, QUOTA_VOL_SIZE*SX_BS_LARGE, 1, 1, NULL, args->owner_arg)) {
        fprintf(stderr, "test_quota: ERROR: Cannot create '%s' volume: %s\n", volname, sxc_geterrmsg(sx));
        goto test_quota_err;
    }
    if(args->human_flag) {
        printf("test_quota: Volume '%s' (replica: 1, size: %dM) created.\n", volname, QUOTA_VOL_SIZE);
        printf("test_quota: Creating file of size: %dM\n", QUOTA_FILE_SIZE);
    } else {
        printf("test_quota: Volume '%s' (replica: 1, size: %lld) created.\n", volname, QUOTA_VOL_SIZE*1024LL*1024LL);
        printf("test_quota: Creating file of size: %" PRIu64 "\n", (uint64_t)QUOTA_FILE_SIZE*1024*1024);
    }
    src = sxc_file_local(sx, local_file_path);
    if(!src) {
        fprintf(stderr, "test_quota: ERROR: Cannot open '%s' file.\n", local_file_path);
        goto test_quota_err;
    }
    dest = sxc_file_remote(cluster, volname, remote_path, NULL);
    if(!dest) {
        fprintf(stderr, "test_quota: ERROR: Cannot open '%s' directory.\n", remote_path);
        goto test_quota_err;
    }
    if(create_file(local_file_path, SX_BS_LARGE, QUOTA_FILE_SIZE, NULL, 1)) {
        fprintf(stderr, "test_quota: ERROR: Cannot create '%s' file.\n", local_file_path);
        goto test_quota_err;
    }
    file = 1;
    switch(sxc_copy(src, dest, 0, 0, 0, NULL)) {
        case 0:
            fprintf(stderr, "test_quota: ERROR: Volume size limit not enforced.\n");
            goto test_quota_err;
        case 413:
            printf("test_quota: Volume size limit enforced correctly.\n");
            break;
        default:
            fprintf(stderr, "test_quota: ERROR: Cannot upload '%s' file: %s\n", local_file_path, sxc_geterrmsg(sx));
            goto test_quota_err;
    }
    if(sxc_volume_modify(cluster, volname, NULL, 2 * QUOTA_FILE_SIZE * SX_BS_LARGE)) {
        fprintf(stderr, "test_quota: ERROR: Cannot change volume size: %s\n", sxc_geterrmsg(sx));
        goto test_quota_err;
    }
    if(sxc_copy(src, dest, 0, 0, 0, NULL)) {
        fprintf(stderr, "test_quota: ERROR: Cannot upload '%s' file: %s\n", local_file_path, sxc_geterrmsg(sx));
        goto test_quota_err;
    }
    printf("test_quota: Volume size changed correctly.\n");
    sprintf(remote_path, "sx://%s@%s/%s/%s/%s", args->owner_arg, host, volname, REMOTE_DIR, QUOTA_FILE_NAME);
    if(delete_files(sx, cluster, remote_path, 0)) {
        fprintf(stderr, "test_quota: ERROR: Cannot delete '%s' file: %s\n", remote_path, sxc_geterrmsg(sx));
        goto test_quota_err;
    }
    if(remove_volume(sx, cluster, volname, 0)) {
        fprintf(stderr, "test_quota: ERROR: Cannot remove '%s' volume: %s\n", volname, sxc_geterrmsg(sx));
        goto test_quota_err;
    }

    printf("test_quota: Succeeded\n");
    ret = 0;
test_quota_err:
    if(file && unlink(local_file_path)) {
        fprintf(stderr, "test_quota: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_quota_err;
    }
    free(volname);
    free(local_file_path);
    free(remote_path);
    sxc_file_free(src);
    sxc_file_free(dest);
    return ret;
} /* test_quota */

int test_copy(sxc_client_t *sx, sxc_cluster_t *cluster, const char *cluster_name, const char *profile_name, const char *filter_dir, const char *filter1_name, const char *filter1_cfg, const char *filter2_name, const char *filter2_cfg, const char *local_dir_path, const struct gengetopt_args_info *args) {
    int ret = 1, tmp;
    char *volname1, *volname2 = NULL, *local_file_path = NULL, *remote_file1_path = NULL, *remote_file2_path = NULL;
    unsigned char block[SX_BS_MEDIUM], hash1[SHA_DIGEST_LENGTH], hash2[SHA_DIGEST_LENGTH];
    FILE *file = NULL;
    sxc_uri_t *uri = NULL;
    sxc_file_t *src = NULL, *dest = NULL;
    SHA_CTX ctx;

    printf("\ntest_copy: Started\n");
    volname1 = (char*)malloc(strlen(VOLNAME) + strlen("XXXXXX") + 1);
    if(!volname1) {
        fprintf(stderr, "test_copy: ERROR: Cannot allocate memory for volname1.\n");
        return ret;
    }
    sprintf(volname1, "%sXXXXXX", VOLNAME);
    if(randomize_name(volname1))
        goto test_copy_err;
    volname2 = (char*)malloc(strlen(VOLNAME) + strlen("XXXXXX") + 1);
    if(!volname2) {
        fprintf(stderr, "test_copy: ERROR: Cannot allocate memory for volname2.\n");
        goto test_copy_err;
    }
    sprintf(volname2, "%sXXXXXX", VOLNAME);
    if(randomize_name(volname2))
        goto test_copy_err;
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(COPY_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_copy: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_copy_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, COPY_FILE_NAME);
    remote_file1_path = (char*)malloc(strlen("sx://") + (profile_name ? strlen(profile_name) : -1) + 1 + strlen(cluster_name) + 1 + strlen(volname1) + 1 + strlen(REMOTE_DIR) + 1 + strlen(COPY_FILE_NAME) + 1); /* The 1's inside are for '@' and '/' characters. */
    if(!remote_file1_path) {
        fprintf(stderr, "test_copy: ERROR: Cannot allocate memory for remote_file1_path.\n");
        goto test_copy_err;
    }
    sprintf(remote_file1_path, "sx://%s%s%s/%s/%s/%s", profile_name ? profile_name : "", profile_name ? "@" : "", cluster_name, volname1, REMOTE_DIR, COPY_FILE_NAME);
    remote_file2_path = (char*)malloc(strlen("sx://") + (profile_name ? strlen(profile_name) : -1) + 1 + strlen(cluster_name) + 1 + strlen(volname2) + 1 + strlen(REMOTE_DIR) + 1 + strlen(COPY_FILE_NAME) + 1); /* The 1's inside are for '@' and '/' characters. */
    if(!remote_file2_path) {
        fprintf(stderr, "test_copy: ERROR: Cannot allocate memory for remote_file2_path.\n");
        goto test_copy_err;
    }
    sprintf(remote_file2_path, "sx://%s%s%s/%s/%s/%s", profile_name ? profile_name : "", profile_name ? "@" : "", cluster_name, volname2, REMOTE_DIR, COPY_FILE_NAME);
    printf("test_copy: Filters: %s (%s) and %s (%s).\n", filter1_name, filter1_cfg, filter2_name, filter2_cfg);
    if(create_volume(sx, cluster, volname1, args->owner_arg, filter_dir, filter1_name, filter1_cfg, args, 1, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot create new volume.\n");
        goto test_copy_err;
    }
    if(create_volume(sx, cluster, volname2, args->owner_arg, filter_dir, filter2_name, filter2_cfg, args, 1, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot create new volume.\n");
        goto test_copy_err;
    }
    if(create_file(local_file_path, SX_BS_MEDIUM, 10, hash1, 1)) {
        fprintf(stderr, "test_copy: ERROR: Cannot create '%s' file.\n", local_file_path);
        goto test_copy_err;
    }
    printf("test_copy: Uploading file.\n");
    if(upload_file(sx, cluster, local_file_path, remote_file1_path, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot upload '%s' file.\n", local_file_path);
        if(unlink(local_file_path))
            fprintf(stderr, "test_copy: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_copy_err;
    }
    if(unlink(local_file_path)) {
        fprintf(stderr, "test_copy: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_copy_err;
    }
    uri = sxc_parse_uri(sx, remote_file1_path);
    if(!uri) {
        fprintf(stderr, "test_copy: ERROR: %s\n", sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    src = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
    if(!src) {
        fprintf(stderr, "test_copy: ERROR: Cannot open '%s'.\n", remote_file1_path);
        sxc_free_uri(uri);
        uri = NULL;
        goto test_copy_err;
    }
    sxc_free_uri(uri);
    printf("test_copy: Copying file between volumes.\n");
    uri = sxc_parse_uri(sx, remote_file2_path);
    if(!uri) {
        fprintf(stderr, "test_copy: ERROR: %s\n", sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    dest = sxc_file_remote(cluster, uri->volume, uri->path, NULL);
    if(!dest) {
        fprintf(stderr, "test_copy: ERROR: Cannot open '%s'.\n", remote_file2_path);
        goto test_copy_err;
    }
    if(sxc_copy(src, dest, 0, 0, 0, NULL)) {
        fprintf(stderr, "test_copy: ERROR: Cannot upload '%s' file: %s\n", remote_file2_path, sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    printf("test_copy: Downloading file.\n");
    file = download_file(sx, cluster, local_file_path, remote_file2_path, 0);
    if(!file) {
        fprintf(stderr, "test_copy: ERROR: Cannot download '%s' file: %s\n", remote_file2_path, sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    if(!SHA1_Init(&ctx)) {
        fprintf(stderr, "test_copy: ERROR: SHA1_Init() failure.\n");
        goto test_copy_err;
    }
    while((tmp = fread(block, sizeof(unsigned char), SX_BS_MEDIUM, file))) {
        if(!SHA1_Update(&ctx, block, tmp)) {
            fprintf(stderr, "test_copy: ERROR: SHA1_Update() failure.\n");
            goto test_copy_err;
        }
        if(tmp < SX_BS_MEDIUM) {
            fprintf(stderr, "test_copy: ERROR: Downloaded only a part of file.\n");
            goto test_copy_err;
        }
    }
    if(!SHA1_Final(hash2, &ctx)) {
        fprintf(stderr, "test_copy: ERROR: SHA1_Final() failure.\n");
        goto test_copy_err;
    }
    if(memcmp(hash1, hash2, SHA_DIGEST_LENGTH)) {
        fprintf(stderr, "test_copy: ERROR: Uploaded and downloaded file differs.\n");
        goto test_copy_err;
    }
    if(delete_files(sx, cluster, remote_file1_path, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot delete '%s' file: %s\n", remote_file1_path, sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    if(delete_files(sx, cluster, remote_file2_path, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot delete '%s' file: %s\n", remote_file2_path, sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    if(remove_volume(sx, cluster, volname1, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot remove '%s' volume: %s\n", volname1, sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    if(remove_volume(sx, cluster, volname2, 0)) {
        fprintf(stderr, "test_copy: ERROR: Cannot remove '%s' volume: %s\n", volname2, sxc_geterrmsg(sx));
        goto test_copy_err;
    }
    
    printf("test_copy: Succeeded\n");
    ret = 0;
test_copy_err:
    if(file) {
        if(fclose(file) == EOF) {
            fprintf(stderr, "test_copy: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
            goto test_copy_err;
        }
        if(unlink(local_file_path)) {
            fprintf(stderr, "test_copy: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
            ret = 1;
        }
    }
    free(volname1);
    free(volname2);
    free(local_file_path);
    free(remote_file1_path);
    free(remote_file2_path);
    sxc_free_uri(uri);
    sxc_file_free(src);
    sxc_file_free(dest);
    return ret;
} /* test_copy */

/* Both users without 'read' permission.
 * Path to already existing file. 
 *  -1 - error
 *   0 - permissions forced
 *   1 - files uploaded */
int cross_copy(sxc_client_t *sx, sxc_cluster_t *cluster, const char *cluster_name, const char *volname1, const char* volname2, const char *user1, const char *user2, const char *profile_name, const char *local_file_path) {
    int ret = -1;
    char *remote_file_path;

    remote_file_path = (char*)malloc(strlen("sx://") + strlen(user1) + strlen(user2) + 1 + strlen(cluster_name) + 1 + strlen(volname1) + strlen(volname2) + 2); /* The 1's inside are for '@' and '/' characters. */
    if(!remote_file_path) {
        fprintf(stderr, "cross_copy: ERROR: Cannot allocate memory for remote_file_path.\n");
        return ret;
    }
    if(sxc_cluster_set_access(cluster, user1)) {
        fprintf(stderr, "cross_copy: ERROR: Failed to set '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto cross_copy_err;
    }
    sprintf(remote_file_path, "sx://%s@%s/%s/", user1, cluster_name, volname2);
    if(upload_file(sx, cluster, local_file_path, remote_file_path, 1)) {
        if(sxc_geterrnum(sx) != SXE_EAUTH) {
            fprintf(stderr, "cross_copy: ERROR: Cannot upload '%s' file.\n", local_file_path);
            goto cross_copy_err;
        }
    } else {
        fprintf(stderr, "cross_copy: ERROR: File upload succeeded without permission.\n");
        ret = 1;
        goto cross_copy_err;
    }
    if(sxc_cluster_set_access(cluster, user2)) {
        fprintf(stderr, "cross_copy: ERROR: Failed to set '%s' profile authentication: %s\n", user2, sxc_geterrmsg(sx));
        goto cross_copy_err;
    }
    sprintf(remote_file_path, "sx://%s@%s/%s/", user2, cluster_name, volname1);
    if(upload_file(sx, cluster, local_file_path, remote_file_path, 1)) {
        if(sxc_geterrnum(sx) != SXE_EAUTH) {
            fprintf(stderr, "cross_copy: ERROR: Cannot upload '%s' file.\n", local_file_path);
            goto cross_copy_err;
        }
    } else {
        fprintf(stderr, "cross_copy: ERROR: File upload succeeded without permission.\n");
        ret = 1;
        goto cross_copy_err;
    }
    if(sxc_cluster_set_access(cluster, profile_name)) {
        fprintf(stderr, "cross_copy: ERROR: Failed to set default profile: %s\n", sxc_geterrmsg(sx));
        goto cross_copy_err;
    }

    ret = 0;
cross_copy_err:
    free(remote_file_path); 
    return ret;
} /* cross_copy */

int check_users(sxc_cluster_t *cluster, const char **users, const int users_num) {
    int i, ret = -1, is_admin, next = 1, num = 0;
    char *user = NULL;
    sxc_cluster_lu_t *lstu;

    lstu = sxc_cluster_listusers(cluster);
    if(!lstu)
        return ret;
    while(next > 0) {
        next = sxc_cluster_listusers_next(lstu, &user, &is_admin);
        switch(next) {
            case -1:
                if(user)
                    free(user);
                goto check_users_err;
            case 0: break;
            case 1:
                for(i=0; i<users_num; i++)
                    if(users[i] && !strcmp(users[i], user)) {
                        users[i] = NULL;
                        break;
                    }
                free(user);
                user = NULL;
                num++;
                break;
        }
    }
    if(num - 1 != users_num) /* There is always admin profile */
        return 1;
    for(i=0; i<users_num; i++)
        if(users[i])
            return 2;

    ret = 0;
check_users_err:
    sxc_cluster_listusers_free(lstu);
    return ret;
} /* check_users */

int check_user(sxc_cluster_t *cluster, const char *volname, const char *user, int rights) {
    int ret = -1, next = 1, tmp_rights, read = 1, write = 2, owner = 4, can_read, can_write, is_owner;
    char *get_user = NULL;
    sxc_cluster_la_t *lstu;

    lstu = sxc_cluster_listaclusers(cluster, volname);
    if(!lstu)
        return ret;
    while(next > 0) {
        next = sxc_cluster_listaclusers_next(lstu, &get_user, &can_read, &can_write, &is_owner);
        switch(next) {
            case -1:
                if(get_user)
                    free(get_user);
                goto check_user_err;
            case 0: break;
            case 1:
                if(!strcmp(user, get_user)) {
                    free(get_user);
                    tmp_rights = 0;
                    tmp_rights |= can_read ? read : 0;
                    tmp_rights |= can_write ? write : 0;
                    tmp_rights |= is_owner ? owner : 0;
                    if(rights != tmp_rights) {
                        ret = 1;
                        goto check_user_err;
                    }
                    next = 0;
                    break;
                }
                free(get_user);
                get_user = NULL;
                break;
        }
    }

    ret = 0;
check_user_err:
    sxc_cluster_listaclusers_free(lstu);
    return ret;
} /* check_user */

int check_admin(sxc_cluster_t *cluster) {
    int ret = -1, is_admin, next = 1;
    char *get_user = NULL;
    char *user;
    sxc_cluster_lu_t *lstu;

    lstu = sxc_cluster_listusers(cluster);
    if(!lstu)
        return ret;

    user = sxc_cluster_whoami(cluster);
    if(!user)
	goto check_admin_err;

    while(next > 0) {
        next = sxc_cluster_listusers_next(lstu, &get_user, &is_admin);
        switch(next) {
            case -1:
                if(get_user)
                    free(get_user);
                goto check_admin_err;
            case 0: break;
            case 1:
                if(!strcmp(user, get_user))
                    next = 0;
                free(get_user);
                get_user = NULL;
                break;
        }
    }

    ret = is_admin;
check_admin_err:
    sxc_cluster_listusers_free(lstu);
    free(user);
    return ret;
} /* check_users */

int test_acl(const char *program_name, sxc_client_t *sx, sxc_cluster_t *cluster, const char *cluster_name, const char *profile_name, const char *local_dir_path, const struct gengetopt_args_info *args) {
    int ret = 1;
    char *user1, *user2 = NULL, *user3 = NULL, *users[3], *key1 = NULL, *key2 = NULL, *key3 = NULL, key_tmp[AUTHTOK_ASCII_LEN], *volname1 = NULL, *volname2 = NULL, *local_file_path = NULL, *remote_file_path = NULL, *key_file_path = NULL;
    FILE *file = NULL, *file_key = NULL;

    printf("\ntest_acl: Started\n");
    switch(check_admin(cluster)) {
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            return ret;
        case 0:
            fprintf(stderr, "test_acl: ERROR: current user is not an admin.\n");
            return ret;
    }
    switch(check_users(cluster, (const char**)users, 0)) {
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            return ret;
        case 0: break;
        case 1: 
            fprintf(stderr, "test_acl: ERROR: Wrong number of users.\n");
            return ret;
        case 2:
            fprintf(stderr, "test_acl: ERROR: Different user list.\n");
            return ret;
    }
    user1 = (char*)malloc(strlen(ACL_USER1) + strlen("XXXXXX") + 1);
    if(!user1) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for user1.\n");
        return ret;
    }
    sprintf(user1, "%sXXXXXX", ACL_USER1);
    if(randomize_name(user1))
        goto test_acl_err;
    key1 = sxc_user_add(cluster, user1, 0, NULL);
    if(!key1) {
        fprintf(stderr, "test_acl: ERROR: Cannot create '%s' user: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_add_access(cluster, user1, key1)) {
        fprintf(stderr, "test_acl: ERROR: Failed to add '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    user2 = (char*)malloc(strlen(ACL_USER2) + strlen("XXXXXX") + 1);
    if(!user2) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for user2.\n");
        goto test_acl_err;
    }
    sprintf(user2, "%sXXXXXX", ACL_USER2);
    if(randomize_name(user2))
        goto test_acl_err;
    key2 = sxc_user_add(cluster, user2, 0, NULL);
    if(!key2) {
        fprintf(stderr, "test_acl: ERROR: Cannot create '%s' user: %s", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_add_access(cluster, user2, key2)) {
        fprintf(stderr, "test_acl: ERROR: Failed to add '%s' profile authentication: %s\n", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    volname1 = (char*)malloc(strlen(ACL_VOLNAME1) + strlen("XXXXXX") + 1);
    if(!volname1) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for volname1.\n");
        goto test_acl_err;
    }
    sprintf(volname1, "%sXXXXXX", ACL_VOLNAME1);
    if(randomize_name(volname1))
        goto test_acl_err;
    volname2 = (char*)malloc(strlen(ACL_VOLNAME2) + strlen("XXXXXX") + 1);
    if(!volname2) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for volname2.\n");
        goto test_acl_err;
    }
    sprintf(volname2, "%sXXXXXX", ACL_VOLNAME2);
    if(randomize_name(volname2))
        goto test_acl_err;
    remote_file_path = (char*)malloc(strlen("sx://") + strlen(user1) + strlen(user2) + 1 + strlen(cluster_name) + 1 + strlen(volname1) + strlen(volname2) + 2); /* The 1's inside are for '@' and '/' characters. */
    if(!remote_file_path) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for remote_file_path.\n");
        goto test_acl_err;
    }
    local_file_path = (char*)malloc(strlen(local_dir_path) + strlen(ACL_FILE_NAME) + 1);
    if(!local_file_path) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for local_file_path.\n");
        goto test_acl_err;
    }
    sprintf(local_file_path, "%s%s", local_dir_path, ACL_FILE_NAME);
    file = fopen(local_file_path, "w");
    if(!file) {
        fprintf(stderr, "test_acl: ERROR: Cannot create '%s' file: %s\n", local_file_path, strerror(errno));
        goto test_acl_err;
    }
    if(fclose(file) == EOF) {
        fprintf(stderr, "test_acl: ERROR: Cannot close '%s' file: %s\n", local_file_path, strerror(errno));
        if(unlink(local_file_path))
            fprintf(stderr, "test_acl: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        file = NULL;
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user1)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(create_volume(sx, cluster, volname1, user1, NULL, NULL, NULL, args, 1, 1)) {
        if(sxc_geterrnum(sx) == SXE_EAUTH)
            printf("test_acl: Volume creation permission enforced correctly.\n");
        else {
            fprintf(stderr, "test_acl: ERROR: Cannot create new volume.\n");
            goto test_acl_err;
        }
    } else {
        fprintf(stderr, "test_acl: ERROR: Volume created without permission.\n");
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, profile_name)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set default profile: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(create_volume(sx, cluster, volname1, user1, NULL, NULL, NULL, args, 1, 0)) {
        fprintf(stderr, "test_acl: ERROR: Cannot create new volume.\n");
        goto test_acl_err;
    }
    if(create_volume(sx, cluster, volname2, user2, NULL, NULL, NULL, args, 1, 0)) {
        fprintf(stderr, "test_acl: ERROR: Cannot create new volume.\n");
        goto test_acl_err;
    }
    switch(check_user(cluster, volname1, user1, 1 + 2 + 4)) { /* read + write + owner */
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            goto test_acl_err;
        case 1:
            fprintf(stderr, "test_acl: ERROR: '%s' has diferent rights on '%s'.\n", user1, volname1);
            goto test_acl_err;
    }
    switch(check_user(cluster, volname1, user2, 0)) { /* no rights yet */
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            goto test_acl_err;
        case 1:
            fprintf(stderr, "test_acl: ERROR: '%s' has diferent rights on '%s'.\n", user2, volname1);
            goto test_acl_err;
    }
    switch(cross_copy(sx, cluster, cluster_name, volname1, volname2, user1, user2, profile_name, local_file_path)) {
        case -1:
            fprintf(stderr, "test_acl: ERROR: Files uploading failure.\n");
            goto test_acl_err;
        case 0:
            printf("test_acl: Users permissions enforced correctly.\n");
            break;
        case 1:
            fprintf(stderr, "test_acl: ERROR: Files uploaded without permission.\n");
            goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user2)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    sprintf(remote_file_path, "sx://%s@%s/%s/%s", user1, cluster_name, volname2, ACL_FILE_NAME);
    if(upload_file(sx, cluster, local_file_path, remote_file_path, 0)) { /* user1 in remote_file_path have no impact here */
        fprintf(stderr, "test_acl: ERROR: Cannot upload '%s' file.\n", local_file_path);
        goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname2, user1, "read", NULL)) {
        fprintf(stderr, "test_acl: ERROR: Cannot add 'read' permission to '%s': %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user1)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    switch(find_file(sx, cluster, remote_file_path, 0)) {
        case -1:
            fprintf(stderr, "test_acl: ERROR: Looking for '%s' file failed.\n", remote_file_path);
            goto test_acl_err;
        case 0:
            fprintf(stderr, "test_acl: ERROR: '%s' file not found.\n", remote_file_path);
            goto test_acl_err;
        case 1:
            printf("test_acl: 'read' permission granted correctly.\n");
            break;
    }
    switch(check_user(cluster, volname2, user1, 1)) { /* read */
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            goto test_acl_err;
        case 1:
            fprintf(stderr, "test_acl: ERROR: '%s' has diferent rights on '%s'.\n", user1, volname2);
            goto test_acl_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 1)) {
        if(sxc_geterrnum(sx) == SXE_EAUTH)
            printf("test_acl: 'write' permission enforced correctly.\n");
        else {
            fprintf(stderr, "test_acl: ERROR: Cannot delete '%s' file: %s\n", remote_file_path, sxc_geterrmsg(sx));
            goto test_acl_err;
        }
    } else {
        fprintf(stderr, "test_acl: ERROR: File has been deleted without permission.\n");
        goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname1, user2, "write", NULL)) {
        fprintf(stderr, "test_acl: ERROR: Cannot add 'write' permission to '%s': %s\n", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user2)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 1)) {
        fprintf(stderr, "test_acl: ERROR: Cannot delete '%s' file: %s\n", remote_file_path, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    sprintf(remote_file_path, "sx://%s@%s/%s/%s", user2, cluster_name, volname1, ACL_FILE_NAME);
    if(upload_file(sx, cluster, local_file_path, remote_file_path, 1)) {
        fprintf(stderr, "test_acl: ERROR: Cannot upload '%s' file.\n", local_file_path);
        goto test_acl_err;
    } else
        printf("test_acl: 'write' permission granted correctly.\n");
    if(find_file(sx, cluster, remote_file_path, 1) == -1) {
        if(sxc_geterrnum(sx) == SXE_EAUTH)
            printf("test_acl: 'read' permission enforced correctly.\n");
        else {
            fprintf(stderr, "test_acl: ERROR: Looking for '%s' file in %s failed: %s\n", ACL_FILE_NAME, remote_file_path, sxc_geterrmsg(sx));
            goto test_acl_err;
        }
    } else {
        fprintf(stderr, "test_acl: ERROR: Searching for a file done without permission.\n");
        goto test_acl_err;
    }
    if(delete_files(sx, cluster, remote_file_path, 1)) {
        fprintf(stderr, "test_acl: ERROR: Cannot delete '%s' file: %s\n", remote_file_path, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname2, user1, NULL, "read")) {
        fprintf(stderr, "test_acl: ERROR: Cannot revoke 'read' permission from '%s': %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user1)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname1, user2, NULL, "write")) {
        fprintf(stderr, "test_acl: ERROR: Cannot revoke 'write' permission from '%s': %s\n", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    switch(cross_copy(sx, cluster, cluster_name, volname1, volname2, user1, user2, profile_name, local_file_path)) {
        case -1:
            fprintf(stderr, "test_acl: ERROR: Cannot upload file.\n");
            goto test_acl_err;
        case 0:
            printf("test_acl: User permissions revoked correctly.\n");
            break;
        case 1:
            fprintf(stderr, "test_acl: ERROR: File uploaded without permission.\n");
            goto test_acl_err;
    }
    user3 = (char*)malloc(strlen(ACL_USER3) + strlen("XXXXXX") + 1);
    if(!user3) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for user3.\n");
        goto test_acl_err;
    }
    sprintf(user3, "%sXXXXXX", ACL_USER1);
    if(randomize_name(user3))
        goto test_acl_err;
    key3 = sxc_user_add(cluster, user3, 0, NULL);
    if(!key3) {
        fprintf(stderr, "test_acl: ERROR: Cannot create '%s' user: %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_add_access(cluster, user3, key3)) {
        fprintf(stderr, "test_acl: ERROR: Failed to add '%s' profile authentication: %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    users[0] = user1;
    users[1] = user2;
    users[2] = user3;
    switch(check_users(cluster, (const char**)users, 3)) {
        case -1:
            goto test_acl_err;
        case 0:
            break;
        case 1: 
            fprintf(stderr, "test_acl: ERROR: Wrong number of users.\n");
            goto test_acl_err;
        case 2:
            fprintf(stderr, "test_acl: ERROR: Different user list.\n");
            goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname1, user3, "read,write", NULL)) {
        fprintf(stderr, "test_acl: ERROR: Cannot add 'read,write' permission to %s: %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user3)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname1, user2, "read", NULL)) {
        if(sxc_geterrnum(sx) == SXE_EAUTH)
            printf("test_acl: User permissions enforced correctly.\n");
        else {
            fprintf(stderr, "test_acl: ERROR: Cannot add 'read' permission to '%s': %s\n", user3, sxc_geterrmsg(sx));
            goto test_acl_err;
        }
    } else {
        fprintf(stderr, "test_acl: ERROR: Permissions granted without permission.\n");
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, profile_name)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set default profile: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_volume_acl(cluster, volname1, user3, NULL, "read,write")) {
        fprintf(stderr, "test_acl: ERROR: Cannot revoke 'read,write' permission from '%s': %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_volume_modify(cluster, volname1, user3, 0)) {
        fprintf(stderr, "test_quota: ERROR: Cannot change volume owner: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user3)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    switch(check_user(cluster, volname1, user1, 1 + 2)) { /* read + write */
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            goto test_acl_err;
        case 1:
            fprintf(stderr, "test_acl: ERROR: '%s' has diferent rights on '%s'.\n", user1, volname1);
            goto test_acl_err;
    }
    switch(check_user(cluster, volname1, user3, 1 + 2 + 4)) { /* read + write + owner */
        case -1:
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            goto test_acl_err;
        case 1:
            fprintf(stderr, "test_acl: ERROR: '%s' has diferent rights on '%s'.\n", user1, volname1);
            goto test_acl_err;
    }
    if(remove_volume(sx, cluster, volname1, 1)) {
        if(sxc_geterrnum(sx) == SXE_EAUTH)
            printf("test_acl: Volume removal permission enforced correctly.\n");
        else {
            fprintf(stderr, "test_acl: ERROR: Cannot remove '%s' volume: %s\n", volname1, sxc_geterrmsg(sx));
            goto test_acl_err;
        }
    } else {
        fprintf(stderr, "test_acl: ERROR: Volume removed without permission.\n");
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, profile_name)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set default profile: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    key_file_path = (char*)malloc(strlen(local_dir_path) + 1 + strlen(ACL_KEY_FILE_NAME) + 1);
    if(!key_file_path) {
        fprintf(stderr, "test_acl: ERROR: Cannot allocate memory for key_file_path.\n");
        goto test_acl_err;
    }
    sprintf(key_file_path, "%s/%s", local_dir_path, ACL_KEY_FILE_NAME);
    file_key = fopen(key_file_path, "w+");
    if(!file_key) {
        fprintf(stderr, "test_acl: ERROR: Cannot open '%s' file: %s\n", key_file_path, strerror(errno));
        goto test_acl_err;
    }
    if(sxc_user_getkey(cluster, user1, file_key)) {
        fprintf(stderr, "test_acl: ERROR: Cannot get '%s' key: %s\n", user1, sxc_geterrmsg(sx));
        fclose(file_key);
        goto test_acl_err;
    }
    if(fflush(file_key) == EOF) {
        fprintf(stderr, "test_acl: ERROR: Cannot flush '%s' file: %s\n", key_file_path, strerror(errno));
        fclose(file_key);
        goto test_acl_err;
    }
    rewind(file_key);
    if(ftell(file_key) == -1) {
        fprintf(stderr, "test_acl: ERROR: Cannot rewind '%s' file: %s\n", key_file_path, strerror(errno));
        fclose(file_key);
        goto test_acl_err;
    }
    if(fread(key_tmp, 1, AUTHTOK_ASCII_LEN, file_key) != AUTHTOK_ASCII_LEN) {
        fprintf(stderr, "test_acl: ERROR: Cannot get '%s' key.\n", user1);
        fclose(file_key);
        goto test_acl_err;
    }
    fclose(file_key);
    if(memcmp(key1, key_tmp, AUTHTOK_ASCII_LEN)) {
        fprintf(stderr, "test_acl: ERROR: User keys differs.\n");
        goto test_acl_err;
    }
    free(key2);
    key2 = sxc_user_newkey(cluster, user1, NULL);
    if(!key2) {
        fprintf(stderr, "test_acl: ERROR: Cannot generate new key for '%s': %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user1)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    free(key3);
    key3 = sxc_cluster_whoami(cluster);
    if(!key3) {
        if(sxc_geterrnum(sx) == 7) {
            printf("test_acl: User permissions after key change enforced correctly.\n");
        } else {
            fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
            goto test_acl_err;
        }
    } else {
        fprintf(stderr, "test_acl: ERROR: Name checked without permission.\n");
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, profile_name)) {
        fprintf(stderr, "cross_copy: ERROR: Failed to set default profile: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_add_access(cluster, user1, key2)) {
        fprintf(stderr, "test_acl: ERROR: Failed to add '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, user1)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set '%s' profile authentication: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    free(key3);
    key3 = sxc_cluster_whoami(cluster);
    if(!key3) {
        fprintf(stderr, "test_acl: ERROR: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(strcmp(user1, key3)) {
        fprintf(stderr, "test_acl: ERROR: Got wrong user name.\n");
        goto test_acl_err;
    }
    if(sxc_cluster_set_access(cluster, profile_name)) {
        fprintf(stderr, "test_acl: ERROR: Failed to set default profile: %s\n", sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_user_remove(cluster, user1)) {
        fprintf(stderr, "test_acl: ERROR: Cannot remove '%s' user: %s\n", user1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_user_remove(cluster, user2)) {
        fprintf(stderr, "test_acl: ERROR: Cannot remove '%s' user: %s\n", user2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(sxc_user_remove(cluster, user3)) {
        fprintf(stderr, "test_acl: ERROR: Cannot remove '%s' user: %s\n", user3, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(remove_volume(sx, cluster, volname1, 0)) {
        fprintf(stderr, "test_acl: ERROR: Cannot remove '%s' volume: %s\n", volname1, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    if(remove_volume(sx, cluster, volname2, 0)) {
        fprintf(stderr, "test_acl: ERROR: Cannot remove '%s' volume: %s\n", volname2, sxc_geterrmsg(sx));
        goto test_acl_err;
    }
    
    printf("test_acl: Succeeded\n");
    ret = 0;
test_acl_err:
    if(file && unlink(local_file_path)) {
        fprintf(stderr, "test_acl: ERROR: Cannot delete '%s' file: %s\n", local_file_path, strerror(errno));
        if(!ret)
            ret = -1;
    }
    if(file_key && unlink(key_file_path)) {
        fprintf(stderr, "test_acl: ERROR: Cannot delete '%s' file: %s\n", key_file_path, strerror(errno));
        if(!ret)
            ret = -1;
    }
    free(user1);
    free(user2);
    free(user3);
    free(key1);
    free(key2);
    free(key3);
    free(volname1);
    free(volname2);
    free(local_file_path);
    free(remote_file_path);
    free(key_file_path);
    return ret;
} /* test_acl */

int main(int argc, char **argv) {
    int ret = 1;
    char *local_dir_path = NULL, *remote_dir_path = NULL, *volname = NULL, *filter_dir = NULL;
    sxc_client_t *sx = NULL;
    sxc_logger_t log;
    sxc_cluster_t *cluster = NULL;
    sxc_uri_t *uri = NULL;
    struct gengetopt_args_info args;

    if(cmdline_parser(argc, argv, &args)) {
        cmdline_parser_print_help();
        printf("\n");
        return ret;
    }
    if(args.inputs_num != 1) {
        cmdline_parser_print_help();
        printf("\n");
        fprintf(stderr, "main: ERROR: Wrong number of arguments.\n");
        goto main_err;
    }
    sx = sxc_init(SRC_VERSION, sxc_default_logger(&log, argv[0]), test_input_fn, NULL);
    if(!sx) {
        fprintf(stderr, "main: ERROR: Cannot initiate SX.\n");
        goto main_err;
    }
    if(args.config_dir_given && sxc_set_confdir(sx, args.config_dir_arg)) {
        fprintf(stderr, "main: ERROR: Could not set configuration directory to '%s': %s\n", args.config_dir_arg, sxc_geterrmsg(sx));
        goto main_err;
    }
    sxc_set_debug(sx, args.debug_flag);

    uri = sxc_parse_uri(sx, args.inputs[0]);
    if(!uri) {
	fprintf(stderr, "main: ERROR: %s\n", sxc_geterrmsg(sx));
        goto main_err;
    }
    if(uri->volume) {
        fprintf(stderr, "main: ERROR: Volume name not expected.\n");
        goto main_err;
    }
    cluster = sxc_cluster_load_and_update(sx, uri->host, uri->profile);
    if(!cluster) {
        fprintf(stderr, "main: ERROR: Cannot load cluster: %s\n", sxc_geterrmsg(sx));
        goto main_err;
    }
    volname = (char*)malloc(strlen(VOLNAME) + strlen("XXXXXX") + 1);
    if(!volname) {
        fprintf(stderr, "main: ERROR: Cannot allocate memory for volname.\n");
        goto main_err;
    }
    sprintf(volname, "%sXXXXXX", VOLNAME);
    if(randomize_name(volname))
        goto main_err;
    remote_dir_path = (char*)malloc(strlen("sx://") + (uri->profile ? strlen(uri->profile) : -1) + 1 + strlen(uri->host) + 1 + strlen(volname) + 1 + strlen(REMOTE_DIR) + 1 + 1); /* The 1's inside are for '@' and '/' characters. */
    if(!remote_dir_path) {
        fprintf(stderr, "main: ERROR: Cannot allocate memory for remote_dir_path.\n");
        goto main_err;
    }
    sprintf(remote_dir_path, "sx://%s%s%s/%s/%s/", uri->profile ? uri->profile : "", uri->profile ? "@" : "", uri->host, volname, REMOTE_DIR);

    local_dir_path = (char*)malloc(strlen(LOCAL_DIR) + strlen("XXXXXX") + 1 + 1); /* There is '/' character at the end */
    if(!local_dir_path) {
        fprintf(stderr, "main: ERROR: Cannot allocate memory for local_dir_path.\n");
        goto main_err;
    }
    sprintf(local_dir_path, "%sXXXXXX", LOCAL_DIR);
    if(!mkdtemp(local_dir_path)) {
        fprintf(stderr, "main: ERROR: Cannot create '%s' temporary directory: %s\n", local_dir_path, strerror(errno));
        goto main_err;
    }
    strcat(local_dir_path, "/");
    if(args.filter_dir_given) {
        filter_dir = strdup(args.filter_dir_arg);
    } else {
        const char *pt = sxi_getenv("SX_FILTER_DIR");
        if(pt)
            filter_dir = strdup(pt);
    }
    if(!filter_dir) {
        fprintf(stderr, "main: ERROR: Cannot get filter directory. Use --filter-dir or 'export SX_FILTER_DIR=<src_dir>/client/src/filters/'\n");
        goto main_err;
    }
    
    /* The beginning of tests */
    if(volume_test(sx, cluster, volname, filter_dir, NULL, NULL, local_dir_path, remote_dir_path, &args, 1))
        goto main_err;
    if(volume_test(sx, cluster, volname, filter_dir, "aes256", NULL, local_dir_path, remote_dir_path, &args, 2))
        goto main_err;
    if(volume_test(sx, cluster, volname, filter_dir, "zcomp", "level:1", local_dir_path, remote_dir_path, &args, 3))
        goto main_err;
    if(volume_test(sx, cluster, volname, filter_dir, "attribs", NULL, local_dir_path, remote_dir_path, &args, 4))
        goto main_err;
    if(volume_test(sx, cluster, volname, filter_dir, "undelete", TRASH_NAME, local_dir_path, remote_dir_path, &args, 5))
        goto main_err;
    if(test_quota(sx, cluster, local_dir_path, uri->host, &args))
        goto main_err;
    if(test_copy(sx, cluster, uri->host, uri->profile, NULL, NULL, NULL, NULL, NULL, local_dir_path, &args))
        goto main_err;
    if(test_copy(sx, cluster, uri->host, uri->profile, filter_dir, "aes256", NULL, "zcomp", "level:1", local_dir_path, &args))
        goto main_err;
    if(test_acl(argv[0], sx, cluster, uri->host, uri->profile, local_dir_path, &args))
        goto main_err;
    /* The end of tests */

    ret = 0;
    printf("\nmain: All tests succeeded.\n");
main_err:
    if(local_dir_path && rmdir(local_dir_path)) {
        fprintf(stderr, "main: ERROR: Cannot delete '%s' directory: %s\n", local_dir_path, strerror(errno));
        ret = 1;
    }
    free(local_dir_path);
    free(remote_dir_path);
    free(volname);
    free(filter_dir);
    sxc_cluster_free(cluster);
    sxc_free_uri(uri);
    sxc_shutdown(sx, 0);
    cmdline_parser_free(&args);
    return ret;
} /* main */

