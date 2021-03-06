/*
 *  Copyright (C) 2012-2014 Skylable Ltd. <info-copyright@skylable.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#ifndef _SXPROTO_H
#define _SXPROTO_H

#include "sx.h"
#define EXPIRE_TEXT_LEN 18

enum sxi_cluster_verb { REQ_GET = 0, REQ_PUT, REQ_HEAD, REQ_DELETE };
typedef struct _sxi_query_t {
    enum sxi_cluster_verb verb;
    char *path;
    void *content;
    unsigned int content_len;
    unsigned int content_allocated;
    int comma;
} sxi_query_t;

enum sxi_hashop_kind {
    HASHOP_NONE = 0,
    HASHOP_RESERVE,
    HASHOP_CHECK,
    HASHOP_INUSE,
    HASHOP_DELETE
};

sxi_query_t *sxi_useradd_proto(sxc_client_t *sx, const char *username, const uint8_t *key, int admin);
sxi_query_t *sxi_useronoff_proto(sxc_client_t *sx, const char *username, int enable);
sxi_query_t *sxi_userdel_proto(sxc_client_t *sx, const char *username, const char *newowner);
sxi_query_t *sxi_usernewkey_proto(sxc_client_t *sx, const char *username, const uint8_t *key);
sxi_query_t *sxi_volumeadd_proto(sxc_client_t *sx, const char *volname, const char *owner, int64_t size, unsigned int replica, unsigned int revisions, sxc_meta_t *metadata);
sxi_query_t *sxi_flushfile_proto(sxc_client_t *sx, const char *token);
sxi_query_t *sxi_fileadd_proto_begin(sxc_client_t *sx, const char *volname, const char *path, const char *revision, int64_t pos, int64_t blocksize, int64_t size);
sxi_query_t *sxi_fileadd_proto_addhash(sxc_client_t *sx, sxi_query_t *query, const char *hexhash);
sxi_query_t *sxi_fileadd_proto_end(sxc_client_t *sx, sxi_query_t *query, sxc_meta_t *metadata);
sxi_query_t *sxi_filedel_proto(sxc_client_t *sx, const char *volname, const char *path, const char *revision);

typedef struct {
    unsigned replica;
    int count;
} block_meta_entry_t;

typedef struct {
    uint8_t b[SXI_SHA1_BIN_LEN];
} sx_hash_t;

typedef struct {
    uint8_t b[1+SXI_SHA1_BIN_LEN];
} sx_block_meta_index_t;

typedef struct {
    sx_hash_t hash;
    sx_block_meta_index_t cursor;
    unsigned int blocksize;
    block_meta_entry_t *entries;
    unsigned int count;
    int64_t blockid;
} block_meta_t;

typedef enum {
    SX_ID_TOKEN=1,
    SX_ID_REBALANCE,
    SX_ID_REPAIR
} hashop_kind_t;

int sxi_hashop_generate_id(sxc_client_t *sx, hashop_kind_t kind,
                           const void *global, unsigned global_size,
                           const void *local, unsigned local_size, sx_hash_t *id);

sxi_query_t *sxi_hashop_proto_check(sxc_client_t *sx, unsigned blocksize, const char *hashes, unsigned hashes_len);
sxi_query_t *sxi_hashop_proto_reserve(sxc_client_t *sx, unsigned blocksize, const char *hashes, unsigned hashes_len, const char *id, uint64_t op_expires_at);
sxi_query_t *sxi_hashop_proto_inuse_begin(sxc_client_t *sx, hashop_kind_t kind, const char *id, uint64_t op_expires_at);
sxi_query_t *sxi_hashop_proto_inuse_begin_bin(sxc_client_t *sx, hashop_kind_t kind, const void *id, unsigned int id_size, uint64_t op_expires_at);
sxi_query_t *sxi_hashop_proto_inuse_hash(sxc_client_t *sx, sxi_query_t *query, const block_meta_t *blockmeta);
sxi_query_t *sxi_hashop_proto_decuse_hash(sxc_client_t *sx, sxi_query_t *query, const block_meta_t *blockmeta);
sxi_query_t *sxi_hashop_proto_inuse_end(sxc_client_t *sx, sxi_query_t *query);

sxi_query_t *sxi_nodeinit_proto(sxc_client_t *sx, const char *cluster_name, const char *node_uuid, uint16_t http_port, int ssl_flag, const char *ssl_file);
sxi_query_t *sxi_distribution_proto_begin(sxc_client_t *sx, const void *cfg, unsigned int cfg_len);
sxi_query_t *sxi_distribution_proto_add_faulty(sxc_client_t *sx, sxi_query_t *query, const char *node_uuid);
sxi_query_t *sxi_distribution_proto_end(sxc_client_t *sx, sxi_query_t *query);

sxi_query_t *sxi_volsizes_proto_begin(sxc_client_t *sx);
sxi_query_t *sxi_volsizes_proto_add_volume(sxc_client_t *sx, sxi_query_t *query, const char *volname, int64_t size);
sxi_query_t *sxi_volsizes_proto_end(sxc_client_t *sx, sxi_query_t *query);

sxi_query_t *sxi_volume_mod_proto(sxc_client_t *sx, const char *volume, const char *newowner, int64_t newsize);

typedef const char* (*acl_cb_t)(void *ctx);
sxi_query_t *sxi_volumeacl_proto(sxc_client_t *sx, const char *volname,
                                 acl_cb_t grant_read, acl_cb_t grant_write,
                                 acl_cb_t revoke_read, acl_cb_t revoke_write,
                                 void *ctx);

void sxi_query_free(sxi_query_t *query);

#endif
