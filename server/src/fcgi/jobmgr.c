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

#include "default.h"

/* temporary work-around for OS X: to be investigated.
 * 2 issues: BIND_8_COMPAT required for nameser_compat.h
 * and sth from default.h messing with the endian stuff
 */
#if __APPLE__ 
# define BIND_8_COMPAT
# ifdef WORDS_BIGENDIAN
#  undef LITTLE_ENDIAN
#  define BIG_ENDIAN 1
#  define BYTE_ORDER BIG_ENDIAN
# else
#  undef BIG_ENDIAN
#  define LITTLE_ENDIAN 1
#  define BYTE_ORDER LITTLE_ENDIAN
# endif
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>

#include "../libsxclient/src/sxproto.h"
#include "../libsxclient/src/misc.h"
#include "../libsxclient/src/curlevents.h"
#include "hashfs.h"
#include "hdist.h"
#include "job_common.h"
#include "log.h"
#include "jobmgr.h"
#include "blob.h"
#include "nodes.h"
#include "version.h"
#include "clstqry.h"

typedef enum _act_result_t {
    ACT_RESULT_UNSET = 0,
    ACT_RESULT_OK = 1,
    ACT_RESULT_TEMPFAIL = -1,
    ACT_RESULT_PERMFAIL = -2
} act_result_t;

typedef struct _job_data_t {
    void *ptr;
    unsigned int len;
    uint64_t op_expires_at; /* TODO: drop? */
} job_data_t;

typedef act_result_t (*job_action_t)(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *node, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl);

#define action_set_fail(retcode, failcode, failmsg)	    \
    do {						    \
	ret = (retcode);				    \
	*fail_code = (failcode);			    \
	sxi_strlcpy(fail_msg, (failmsg), JOB_FAIL_REASON_SIZE); \
        DEBUG("fail set to: %s\n", fail_msg); \
    } while(0)

#define action_error(retcode, failcode, failmsg)	    \
    do {						    \
	action_set_fail((retcode), (failcode), (failmsg));  \
	goto action_failed;				    \
    } while(0)

#define DEBUGHASH(MSG, X) do {				\
    char _debughash[sizeof(sx_hash_t)*2+1];		\
    if (UNLIKELY(sxi_log_is_debug(&logger))) {          \
        bin2hex((X)->b, sizeof(*X), _debughash, sizeof(_debughash));	\
        DEBUG("%s: #%s#", MSG, _debughash); \
    }\
    } while(0)
static act_result_t http2actres(int code) {
    int ch = code / 100;
    if (code < 0)
        return ACT_RESULT_PERMFAIL;
    if(ch == 2)
	return ACT_RESULT_OK;
    if(ch == 4)
	return ACT_RESULT_PERMFAIL;
    if(code == 503 || ch != 5)
	return ACT_RESULT_TEMPFAIL;
    return ACT_RESULT_PERMFAIL;
}

static act_result_t rc2actres(rc_ty rc) {
    return http2actres(rc2http(rc));
}

typedef struct {
	curlev_context_t *cbdata;
	int query_sent;
} query_list_t;

static void query_list_free(query_list_t *qrylist, unsigned nnodes)
{
    unsigned i;
    if (!qrylist)
        return;
    for (i=0;i<nnodes;i++) {
        sxi_cbdata_unref(&qrylist[i].cbdata);
    }

    free(qrylist);
}

static act_result_t force_phase_success(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    unsigned int nnode, nnodes;
    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++)
	succeeded[nnode] = 1;
    return ACT_RESULT_OK;
}

static rc_ty volonoff_common(sx_hashfs_t *hashfs, job_t job_id, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, const char *volname, int enable) {
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    unsigned int nnode, nnodes;
    char *query = NULL;
    rc_ty s;

    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);

	INFO("%s volume '%s' on %s", enable ? "Enabling" : "Disabling", volname, sx_node_uuid_str(node));

	if(!sx_node_cmp(me, node)) {
	    if(enable) {
		if((s = sx_hashfs_volume_enable(hashfs, volname))) {
		    WARN("Failed to enable volume '%s' job %lld: %s", volname, (long long)job_id, msg_get_reason());
		    action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to enable volume");
		}
	    } else {
		if((s = sx_hashfs_volume_disable(hashfs, volname))) {
		    WARN("Failed to disable volume '%s' job %lld: %s", volname, (long long)job_id, msg_get_reason());
		    action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to disable volume");
		}
	    }
	    succeeded[nnode] = 1;
	} else {
	    if(!query) {
		char *path = sxi_urlencode(sx, volname, 0);
		if(!path) {
		    WARN("Cannot encode path");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
		query = wrap_malloc(strlen(path) + sizeof("?o=disable")); /* fits "enable" and "disable" with termination */
		if(!query) {
		    free(path);
		    WARN("Cannot allocate query");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
		sprintf(query, "%s?o=%s", path, enable ? "enable" : "disable");
		free(path);

		qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate result space");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }

            qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), REQ_PUT, query, NULL, 0, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
	}
    }

 action_failed:
    if(query) {
	for(nnode=0; qrylist && nnode<nnodes; nnode++) {
	    int rc;
            long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if(http_status == 200 || http_status == 410) {
		succeeded[nnode] = 1;
	    } else {
		act_result_t newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
	}
        query_list_free(qrylist, nnodes);
	free(query);
    }

    return ret;
}


static rc_ty voldelete_common(sx_hashfs_t *hashfs, job_t job_id, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, const char *volname, int force) {
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    unsigned int nnode, nnodes;
    char *query = NULL;
    rc_ty s;


    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);

	INFO("Deleting volume %s on %s", volname, sx_node_uuid_str(node));

	if(!sx_node_cmp(me, node)) {
	    if((s = sx_hashfs_volume_delete(hashfs, volname, force)) != OK && s != ENOENT) {
		WARN("Failed to delete volume '%s' for job %lld", volname, (long long)job_id);
		action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to enable volume");
	    }
	    succeeded[nnode] = 1;
	} else {
	    if(!query) {
		query = sxi_urlencode(sx, volname, 0);
		if(!query) {
		    WARN("Cannot encode path");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
		if(force) {
		    char *nuq = malloc(strlen(query) + lenof("?force") + 1);
		    if(!nuq)
			action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		    sprintf(nuq, "%s?force", query);
		    free(query);
		    query = nuq;
		}

		qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate result space");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }

            qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), REQ_DELETE, query, NULL, 0, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
	}
    }

 action_failed:
    if(query) {
	for(nnode=0; qrylist && nnode<nnodes; nnode++) {
	    int rc;
            long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if (http_status == 200 || http_status == 410 || http_status == 404) {
		succeeded[nnode] = 1;
	    } else {
		act_result_t newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
	}
        query_list_free(qrylist, nnodes);
	free(query);
    }

    return ret;
}

static act_result_t createvol_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname, *owner;
    int64_t volsize, owner_uid;
    unsigned int nnode, nnodes;
    int i, replica, revisions, nmeta, bumpttl;
    sx_blob_t *b = NULL;
    act_result_t ret = ACT_RESULT_OK;
    sxi_query_t *proto = NULL;
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxc_meta_t *vmeta = NULL;
    query_list_t *qrylist = NULL;
    rc_ty s;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname) ||
       sx_blob_get_string(b, &owner) ||
       sx_blob_get_int64(b, &volsize) ||
       sx_blob_get_int32(b, &replica) ||
       sx_blob_get_int32(b, &revisions) ||
       sx_blob_get_int32(b, &nmeta) ||
       sx_blob_get_int32(b, &bumpttl)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if((s = sx_hashfs_get_uid(hashfs, owner, &owner_uid))) {
	WARN("Cannot find owner '%s' for job %lld", owner, (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Invalid user");
    }

    sx_blob_savepos(b);

    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);

	INFO("Making volume %s - owner: %s, size: %lld, replica: %d, revs: %u, meta: %d on %s", volname, owner, (long long)volsize, replica, revisions, nmeta, sx_node_uuid_str(node));

	if(!sx_node_cmp(me, node)) {
	    sx_hashfs_volume_new_begin(hashfs);

	    if(nmeta) {
		sx_blob_loadpos(b);
		for(i=0; i<nmeta; i++) {
		    const char *mkey;
		    const void *mval;
		    unsigned int mval_len;
		    if(sx_blob_get_string(b, &mkey) ||
		       sx_blob_get_blob(b, &mval, &mval_len)) {
			WARN("Cannot get volume metadata from blob for job %lld", (long long)job_id);
			action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
		    }
		    if((s = sx_hashfs_volume_new_addmeta(hashfs, mkey, mval, mval_len))) {
			WARN("Cannot add meta data to volume for job %lld", (long long)job_id);
			action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Invalid volume metadata");
		    }
		}
	    }

	    s = sx_hashfs_volume_new_finish(hashfs, volname, volsize, replica, revisions, owner_uid, 1);
	    if(s != OK) {
                const char *msg = (s == EINVAL || s == EEXIST) ? msg_get_reason() : rc2str(s);
		action_error(rc2actres(s), rc2http(s), msg);
            }
	    succeeded[nnode] = 1;
	    *adjust_ttl = bumpttl;
	} else {
	    if(!proto) {
		if(nmeta) {
		    sx_blob_loadpos(b);
		    if(!(vmeta = sxc_meta_new(sx))) {
			WARN("Cannot build a list of volume metadata for job %lld", (long long)job_id);
			action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		    }

		    for(i=0; i<nmeta; i++) {
			const char *mkey;
			const void *mval;
			unsigned int mval_len;
			if(sx_blob_get_string(b, &mkey) ||
			   sx_blob_get_blob(b, &mval, &mval_len)) {
			    WARN("Cannot get volume metadata from blob for job %lld", (long long)job_id);
			    action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
			}
			if(sxc_meta_setval(vmeta, mkey, mval, mval_len)) {
			    WARN("Cannot build a list of volume metadata for job %lld", (long long)job_id);
			    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
			}
		    }
		}

		proto = sxi_volumeadd_proto(sx, volname, owner, volsize, replica, revisions, vmeta);
		if(!proto) {
		    WARN("Cannot allocate proto for job %lld", (long long)job_id);
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}

		qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate result space");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }

            qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
	}
    }

 action_failed:
    sx_blob_free(b);
    if(proto) {
	for(nnode=0; qrylist && nnode<nnodes; nnode++) {
	    int rc;
            long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if(http_status == 200 || http_status == 410) {
		succeeded[nnode] = 1;
	    } else {
		act_result_t newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
	}
        query_list_free(qrylist, nnodes);
	sxi_query_free(proto);
    }
    sxc_meta_free(vmeta);
    return ret;
}

static act_result_t createvol_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname;
    sx_blob_t *b = NULL;
    act_result_t ret;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    ret = volonoff_common(hashfs, job_id, nodes, succeeded, fail_code, fail_msg, volname, 1);

 action_failed:
    sx_blob_free(b);

    return ret;
}

static act_result_t createvol_abort_and_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname;
    sx_blob_t *b = NULL;
    act_result_t ret;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    ret = voldelete_common(hashfs, job_id, nodes, succeeded, fail_code, fail_msg, volname, 1);

 action_failed:
    sx_blob_free(b);

    return ret;
}


static act_result_t deletevol_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname;
    sx_blob_t *b = NULL;
    act_result_t ret;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    ret = volonoff_common(hashfs, job_id, nodes, succeeded, fail_code, fail_msg, volname, 0);

 action_failed:
    sx_blob_free(b);

    return ret;
}

static act_result_t deletevol_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname;
    sx_blob_t *b = NULL;
    act_result_t ret;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    ret = voldelete_common(hashfs, job_id, nodes, succeeded, fail_code, fail_msg, volname, 0);

 action_failed:
    sx_blob_free(b);

    return ret;
}

static act_result_t deletevol_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname;
    sx_blob_t *b = NULL;
    act_result_t ret;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    ret = volonoff_common(hashfs, job_id, nodes, succeeded, fail_code, fail_msg, volname, 1);

 action_failed:
    sx_blob_free(b);

    return ret;
}

static act_result_t deletevol_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const char *volname;
    sx_blob_t *b = NULL;
    act_result_t ret;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname)) {
	WARN("Cannot get volume data from blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    CRIT("Volume '%s' may have been left in an inconsistent state after a failed removal attempt", volname);
    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Volume removal failed: the volume may have been left in an inconsistent state");

 action_failed:
    sx_blob_free(b);

    return ret;
}

static act_result_t job_twophase_execute(const job_2pc_t *spec, jobphase_t phase, sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    unsigned int nnode, nnodes;
    sxi_query_t *proto = NULL;
    rc_ty rc;
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sx_blob_t *b = sx_blob_from_data(job_data->ptr, job_data->len);

    if (!b) {
        WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }
    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);
        sx_blob_reset(b);

	if(!sx_node_cmp(me, node)) {
            /* execute locally */
            rc = spec->execute_blob(hashfs, b, phase, 0);
            if (rc != OK) {
                const char *msg = msg_get_reason();
                if (!msg)
                    msg = rc2str(rc);
                action_error(rc2actres(rc), rc2http(rc), msg);
            }
	    succeeded[nnode] = 1;
	    *adjust_ttl = spec->timeout(sx_hashfs_client(hashfs), nnodes);
        } else {
            /* execute remotely */
            if (!proto) {
                proto = spec->proto_from_blob(sx_hashfs_client(hashfs), b, phase);
                if (!proto) {
                    WARN("Cannot allocate proto for job %lld", (long long)job_id);
                    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
                }
                qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
                if(!qrylist) {
                    WARN("Cannot allocate result space");
                    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
                }
            }
            qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
        }
    }

 action_failed: /* or succeeded */
    sx_blob_free(b);
    if(proto) {
	for(nnode=0; nnode<nnodes; nnode++) {
	    int rc;
            long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
                *adjust_ttl = 0;
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if(http_status == 200 || http_status == 410) {
		succeeded[nnode] = 1;
	    } else {
                *adjust_ttl = 0;
		act_result_t newret;
                if (!http_status && phase == JOBPHASE_REQUEST) {
                    /* request can be safely aborted, so abort asap when
                     * a node is down */
                    http_status = 502;/* can't connect */
                    newret = ACT_RESULT_PERMFAIL;
                } else
                    newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
        }
        query_list_free(qrylist, nnodes);
	sxi_query_free(proto);
    }
    return ret;
}

static act_result_t acl_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&acl_spec, JOBPHASE_REQUEST, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t acl_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&acl_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t acl_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&acl_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t acl_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&acl_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t createuser_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_spec, JOBPHASE_REQUEST, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t createuser_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t createuser_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t createuser_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t deleteuser_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&userdel_spec, JOBPHASE_REQUEST, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t deleteuser_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&userdel_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t deleteuser_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&userdel_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t deleteuser_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&userdel_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t usermodify_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_modify_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t usermodify_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_modify_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t usermodify_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&user_modify_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t cluster_mode_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&cluster_mode_spec, JOBPHASE_REQUEST, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t cluster_mode_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&cluster_mode_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t cluster_setmeta_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&cluster_setmeta_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t cluster_setmeta_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&cluster_setmeta_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t cluster_setmeta_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return job_twophase_execute(&cluster_setmeta_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static int req_append(char **req, unsigned int *req_len, const char *append_me) {
    unsigned int current_len, append_len;

    if(!*req_len) {
	*req = NULL;
	current_len = 0;
    } else
	current_len = strlen(*req);
    append_len = strlen(append_me) + 1;
    if(current_len + append_len > *req_len) {
	*req_len += MAX(1024, append_len);
	*req = wrap_realloc_or_free(*req, *req_len);
    }
    if(!*req) {
        WARN("Failed to append string to request");
        return 1;
    }

    memcpy(*req + current_len, append_me, append_len);
    return 0;
}

static rc_ty filerev_from_jobdata_rev(sx_hashfs_t *hashfs, job_data_t *job_data, sx_hashfs_file_t *filerev)
{
    char revision[REV_LEN+1];

    if(job_data->len != REV_LEN) {
	CRIT("Bad job data");
        return FAIL_EINTERNAL;
    }
    memcpy(revision, job_data->ptr, REV_LEN);
    revision[REV_LEN] = 0;
    return sx_hashfs_getinfo_by_revision(hashfs, revision, filerev);
}

static rc_ty filerev_from_jobdata_tmpfileid(sx_hashfs_t *hashfs, job_data_t *job_data, sx_hashfs_file_t *filerev)
{
    int64_t tmpfile_id;
    rc_ty s;

    if(job_data->len != sizeof(tmpfile_id)) {
        CRIT("Bad job data");
        return FAIL_EINTERNAL;
    }
    sx_hashfs_tmpinfo_t *tmpinfo;
    memcpy(&tmpfile_id, job_data->ptr, sizeof(tmpfile_id));
    s = sx_hashfs_tmp_getinfo(hashfs, tmpfile_id, &tmpinfo, 0);
    if (s) {
        WARN("Failed to lookup tmpfileid: %s", rc2str(s));
        return s;
    }
    filerev->volume_id = tmpinfo->volume_id;
    filerev->block_size = tmpinfo->block_size;
    sxi_strlcpy(filerev->name, tmpinfo->name, sizeof(filerev->name));
    sxi_strlcpy(filerev->revision, tmpinfo->revision, sizeof(filerev->revision));
    memcpy(filerev->revision_id.b, tmpinfo->revision_id.b, SXI_SHA1_BIN_LEN);

    free(tmpinfo);
    return OK;
}

static act_result_t revision_job_from(sx_hashfs_t *hashfs, job_t job_id, const sx_hashfs_file_t *filerev, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl, int op, jobphase_t phase) {
    sx_revision_op_t revision_op;
    const sx_hashfs_volume_t *volume;
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s;
    sx_blob_t *blob = NULL;
    job_data_t new_job_data;
    s = sx_hashfs_volume_by_id(hashfs, filerev->volume_id, &volume);
    if (s)
        action_error(rc2actres(s), rc2http(s), "Failed to retrieve volume info");
    revision_op.lock = NULL;
    revision_op.blocksize = filerev->block_size;
    revision_op.op = op;
    memcpy(revision_op.revision_id.b, filerev->revision_id.b, SXI_SHA1_BIN_LEN);

    blob = sx_blob_new();
    if (!blob)
        action_error(ACT_RESULT_TEMPFAIL, 500, "Cannot allocate blob");
    if (revision_spec.to_blob(sx_hashfs_client(hashfs), sx_nodelist_count(nodes), &revision_op, blob)) {
        const char *msg = msg_get_reason();
        if(!msg || !*msg)
            msg_set_reason("Cannot create job blob");
        action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    sx_blob_to_data(blob, (const void**)&new_job_data.ptr, &new_job_data.len);
    new_job_data.op_expires_at = 0;
    ret = job_twophase_execute(&revision_spec, phase, hashfs, job_id, &new_job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
action_failed:
    sx_blob_free(blob);
    return ret;
}

static act_result_t revision_job_from_tmpfileid(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl, int op, jobphase_t phase) {
    sx_hashfs_file_t filerev;
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s = filerev_from_jobdata_tmpfileid(hashfs, job_data, &filerev);
    if (s)
        action_error(rc2actres(s), rc2http(s), "Failed to lookup file by tmpfile id");
    ret = revision_job_from(hashfs, job_id, &filerev, nodes, succeeded, fail_code, fail_msg, adjust_ttl, op, phase);
action_failed:
    return ret;
}

static act_result_t revision_job_from_rev(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl, int op, jobphase_t phase)
{
    sx_hashfs_file_t filerev;
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s = filerev_from_jobdata_rev(hashfs, job_data, &filerev);
    if (s)
        action_error(rc2actres(s), rc2http(s), "Failed to lookup file by revision");
    ret = revision_job_from(hashfs, job_id, &filerev, nodes, succeeded, fail_code, fail_msg, adjust_ttl, op, phase);
    CRIT("File %s (rev %s) on volume %lld was left in an inconsistent state after a failed deletion attempt", filerev.name, filerev.revision, (long long)filerev.volume_id);
    action_error(ACT_RESULT_PERMFAIL, 500, "File was left in an inconsistent state after a failed deletion attempt");
action_failed:
    return ret;
}


static act_result_t replicateblocks_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    /* bump block revision */
    return revision_job_from_tmpfileid(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl, 1, JOBPHASE_COMMIT);
}

static act_result_t replicateblocks_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    unsigned int i, j, worstcase_rpl, nqueries = 0;
    act_result_t ret = ACT_RESULT_OK;
    sx_hashfs_tmpinfo_t *mis = NULL;
    query_list_t *qrylist = NULL;
    int64_t tmpfile_id;
    rc_ty s;

    if(job_data->len != sizeof(tmpfile_id)) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    memcpy(&tmpfile_id, job_data->ptr, sizeof(tmpfile_id));
    DEBUG("replocateblocks_request for file %lld", (long long)tmpfile_id);
    s = sx_hashfs_tmp_getinfo(hashfs, tmpfile_id, &mis, 1);
    if(s == EFAULT || s == EINVAL) {
	CRIT("Error getting tmpinfo: %s", msg_get_reason());
	action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    if(s == ENOENT) {
	WARN("Token %lld could not be found", (long long)tmpfile_id);
	action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    if(s == EAGAIN)
	action_error(ACT_RESULT_TEMPFAIL, 500, "Job data temporary unavailable");

    if(s != OK)
	action_error(rc2actres(s), rc2http(s), "Failed to check missing blocks");

    worstcase_rpl = mis->replica_count;

    /* Loop through all blocks to check availability */
    for(i=0; i<mis->nuniq; i++) {
	unsigned int ndone = 0, ndone_or_pending = 0, pushingidx = 0, blockno = mis->uniq_ids[i];

	/* For DEBUG()ging purposes */
	char blockname[SXI_SHA1_TEXT_LEN + 1];
	bin2hex(&mis->all_blocks[blockno], sizeof(mis->all_blocks[0]), blockname, sizeof(blockname));

	/* Compute the current replica level for this block */
	for(j=0; j<mis->replica_count; j++) {
	    int8_t avlbl = mis->avlblty[blockno * mis->replica_count + j];
	    if(avlbl == 1) {
		ndone++;
		ndone_or_pending++;
		pushingidx = mis->nidxs[blockno * mis->replica_count + j];
		DEBUG("Block %s is available on set %u (node %u)", blockname, j, mis->nidxs[blockno * mis->replica_count + j]);
	    } else if(avlbl > 0) {
		ndone_or_pending++;
		DEBUG("Block %s pending upload on set %u (node %u)", blockname, j, mis->nidxs[blockno * mis->replica_count + j]);
	    } else {
		DEBUG("Block %s is NOT available on set %u (node %u)", blockname, j, mis->nidxs[blockno * mis->replica_count + j]);
	    }
	}

	DEBUG("Block %s has got %u replicas (%u including pending xfers) out of %u", blockname, ndone, ndone_or_pending, mis->replica_count);

	/* Update the worst case replica */
	if(ndone < worstcase_rpl)
	    worstcase_rpl = ndone;

	/* If the replica is already satisfied, then there is nothing to do for this block */
	if(ndone_or_pending == mis->replica_count)
	    continue;

	/* No node has got this block: job failed */
	if(!ndone) {
	    char missing_block[SXI_SHA1_TEXT_LEN + 1];
	    bin2hex(&mis->all_blocks[blockno], sizeof(mis->all_blocks[0]), missing_block, sizeof(missing_block));
	    WARN("Early flush on job %lld: hash %s could not be located ", (long long)tmpfile_id, missing_block);
	    action_error(ACT_RESULT_PERMFAIL, 400, "Some block is missing");
	}

	/* We land here IF at least one node has got the block AND at least one node doesn't have the block */
	/* NOTE:
	 * If the pushing node is the local node then a transfer request is added to the local block queue
	 * If the pushing node is not the local node, then we submit the request via HTTP
	 * This unfortunately makes the following code a little bit more convoluted than it could be */

	/* Variables used for both remote and local mode */
	const sx_node_t *pusher = sx_nodelist_get(mis->allnodes, pushingidx);
	int remote = (sx_node_cmp(pusher, me) != 0);
	unsigned int current_hash_idx;

	/* Variables used in remote mode only */
	unsigned int req_len = 0;
	char *req = NULL;

	if(remote) {
	    /* query format: { "hash1":["target_node_id1","target_node_id2"],"hash2":["target_node_id3","target_node_id4"] } */
	    if(req_append(&req, &req_len, "{"))
		action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to dispatch block transfer request");
	}

	DEBUG("Selected pusher is %s node %u (%s)", remote ? "remote" : "local", pushingidx, sx_node_internal_addr(pusher));

	/* Look ahead a little (not too much to avoid congesting the pusher) and collect all blocks
	 * that are available on the selected pusher and unavailable on some other replica nodes */
	for(j=0, current_hash_idx = i; j < DOWNLOAD_MAX_BLOCKS && current_hash_idx < mis->nuniq; current_hash_idx++) {
	    unsigned int current_replica, have_pusher = 0, have_junkies = 0, current_blockno = mis->uniq_ids[current_hash_idx];

	    bin2hex(&mis->all_blocks[current_blockno], sizeof(mis->all_blocks[0]), blockname, sizeof(blockname));
	    DEBUG("Considering block %s for pusher %u...", blockname, pushingidx);

	    /* Scan the replica set of the current block... */
	    for(current_replica = 0; current_replica < mis->replica_count; current_replica++) {
		/* ...checking for some node in need of this block...  */
		if(mis->avlblty[current_blockno * mis->replica_count + current_replica] <= 0) {
		    DEBUG("Followup block %s is NOT available on set %u (node %u)", blockname, current_replica, mis->nidxs[current_blockno * mis->replica_count + current_replica]);
		    have_junkies = 1;
		    continue;
		} else if(mis->avlblty[current_blockno * mis->replica_count + current_replica] == 2)
		    DEBUG("Followup block %s is pending upload on set %u (node %u)", blockname, current_replica, mis->nidxs[current_blockno * mis->replica_count + current_replica]);
		else
		    DEBUG("Followup block %s is available on set %u (node %u)", blockname, current_replica, mis->nidxs[current_blockno * mis->replica_count + current_replica]);

		/* ...and checking if the selected pusher is in possession of the block */
		if(mis->avlblty[current_blockno * mis->replica_count + current_replica] == 1 &&
		   mis->nidxs[current_blockno * mis->replica_count + current_replica] == pushingidx)
		    have_pusher = 1;
	    }

	    if(have_junkies && have_pusher)
		DEBUG("Followup block %s needs to be replicated and CAN be replicated by %s pusher %u", blockname, remote ? "remote" : "local", pushingidx);
	    else if(have_junkies)
		DEBUG("Followup block %s needs to be replicated but CANNOT be replicated by %s pusher %u", blockname, remote ? "remote" : "local", pushingidx);
	    else
		DEBUG("Followup block %s needs NOT to be replicated", blockname);

	    /* If we don't have both, then move on to the next block */
	    if(!have_pusher || !have_junkies)
		continue;

	    j++; /* This acts as a look-ahead limiter */

	    sx_hash_t *current_hash = &mis->all_blocks[current_blockno];
            DEBUGHASH("asking hash to be pushed", current_hash);
	    sx_nodelist_t *xfertargets = NULL; /* used in local mode only */

	    if(remote) {
		char key[SXI_SHA1_TEXT_LEN + sizeof("\"\":[")];
		key[0] = '"';
		bin2hex(current_hash, sizeof(mis->all_blocks[0]), key+1, SXI_SHA1_TEXT_LEN+1);
		memcpy(&key[1+SXI_SHA1_TEXT_LEN], "\":[", sizeof("\":["));
		if(req_append(&req, &req_len, key))
		    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to dispatch block transfer request");
	    } else {
		xfertargets = sx_nodelist_new();
		if(!xfertargets)
		    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory for local block transfer");
	    }

	    /* Go through the replica set again */
	    for(current_replica = 0; current_replica < mis->replica_count; current_replica++) {
		const sx_node_t *target;

		/* Skip all nodes that have the block already */
		if(mis->avlblty[current_blockno * mis->replica_count + current_replica] > 0)
		    continue;

		DEBUG("Block %s is set to be transfered to node %u", blockname, mis->nidxs[current_blockno * mis->replica_count + current_replica]);

		/* Mark the block as being transferred so it's not picked up again later */
		mis->avlblty[current_blockno * mis->replica_count + current_replica] = 2;
		target = sx_nodelist_get(mis->allnodes, mis->nidxs[current_blockno * mis->replica_count + current_replica]);
		if(!target) {
		    WARN("Target no longer exists");
		    if(remote)
			free(req);
		    else
			sx_nodelist_delete(xfertargets);
		    action_error(ACT_RESULT_TEMPFAIL, 500, "Internal error looking up target nodes");
		}

		/* Add this node as a transfer target */
		if(remote) {
		    const sx_uuid_t *target_uuid;
		    target_uuid = sx_node_uuid(target);
		    if(req_append(&req, &req_len, "\"") ||
		       req_append(&req, &req_len, target_uuid->string) ||
		       req_append(&req, &req_len, "\","))
			action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to dispatch block transfer request");
		} else {
		    if(sx_nodelist_add(xfertargets, sx_node_dup(target))) {
			sx_nodelist_delete(xfertargets);
			action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory for local block transfer");
		    }
		}
		DEBUG("Block %s added to %s push queue for target %s", blockname, remote ? "remote" : "local", sx_node_internal_addr(target));
	    }

	    if(remote) {
		req[strlen(req)-1] = '\0';
		if(req_append(&req, &req_len, "],"))
		    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to dispatch block transfer request");
	    } else {
		/* Local xfers are flushed at each block */
		s = sx_hashfs_xfer_tonodes(hashfs, current_hash, mis->block_size, xfertargets);
		sx_nodelist_delete(xfertargets);
		if(s)
		    action_error(rc2actres(s), rc2http(s), "Failed to request local block transfer");
	    }
	}

	if(remote) {
	    /* Remote xfers are flushed at each pushing node */
	    char url[sizeof(".pushto/")+64];

	    req[strlen(req)-1] = '}';

	    if(!(nqueries % 64)) {
		query_list_t *nuqlist = realloc(qrylist, sizeof(*qrylist) * (nqueries+64));
		if(!nuqlist) {
		    free(req);
		    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to dispatch block transfer request");
		}
		memset(nuqlist + nqueries, 0, sizeof(*qrylist) * 64);
		qrylist = nuqlist;
	    }
            qrylist[nqueries].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    sxi_cbdata_set_context(qrylist[nqueries].cbdata, req);

	    snprintf(url, sizeof(url), ".pushto/%u", mis->block_size);
	    if(sxi_cluster_query_ev(qrylist[nqueries].cbdata, clust, sx_node_internal_addr(pusher), REQ_PUT, url, req, strlen(req), NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(pusher), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		nqueries++;
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nqueries].query_sent = 1;
	    nqueries++;
	} else
	    sx_hashfs_xfer_trigger(hashfs);
    }

    DEBUG("Job id %lld - current replica %u out of %u", (long long)job_id, worstcase_rpl, mis->replica_count);

    if(worstcase_rpl < mis->replica_count)
	action_error(ACT_RESULT_TEMPFAIL, 500, "Replica not yet completed");

 action_failed:
    if(qrylist) {
	for(i=0; i<nqueries; i++) {
	    if(qrylist[i].query_sent) {
                long http_status = 0;
		int rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
		if(rc != -2) {
		    if(rc == -1) {
			WARN("Query failed with %ld", http_status);
			if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else if(http_status == 404) {
			/* Syntactically invalid request (bad token or block size, etc) */
			action_set_fail(ACT_RESULT_PERMFAIL, 400, "Internal error: replicate block request failed");
		    } else if(http_status != 200) {
			act_result_t newret = http2actres(http_status);
			if(newret < ret) /* Severity shall only be raised */
			    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    }
		} else {
		    CRIT("Failed to wait for query");
		    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		}
	    }
	    free(sxi_cbdata_get_context(qrylist[i].cbdata));
	}
        query_list_free(qrylist, nqueries);
    }

    free(mis);

    if(ret == ACT_RESULT_OK) {
        unsigned i;
        for (i=0;i<sx_nodelist_count(nodes);i++) {
    	    succeeded[i] = 1;
        }
    }

    return ret;
}

static act_result_t fileflush_remote(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    unsigned int i, nnodes;
    act_result_t ret = ACT_RESULT_OK;
    sx_hashfs_tmpinfo_t *mis = NULL;
    query_list_t *qrylist = NULL;
    sxi_query_t *proto = NULL;
    int64_t tmpfile_id;
    const sx_hashfs_volume_t *volume;
    rc_ty s;

    if(job_data->len != sizeof(tmpfile_id)) {
       CRIT("Bad job data");
       action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }
    memcpy(&tmpfile_id, job_data->ptr, sizeof(tmpfile_id));
    DEBUG("fileflush_remote for file %lld", (long long)tmpfile_id);
    s = sx_hashfs_tmp_getinfo(hashfs, tmpfile_id, &mis, 0);
    if(s == EFAULT || s == EINVAL) {
	CRIT("Error getting tmpinfo: %s", msg_get_reason());
	action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    if(s == ENOENT) {
	WARN("Token %lld could not be found", (long long)tmpfile_id);
	action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    if(s == EAGAIN)
	action_error(ACT_RESULT_TEMPFAIL, 500, "Job data temporary unavailable");

    if(s != OK)
	action_error(rc2actres(s), rc2http(s), "Failed to check missing blocks");

    s = sx_hashfs_volume_by_id(hashfs, mis->volume_id, &volume);
    if(s != OK)
	action_error(rc2actres(s), rc2http(s), "Failed to lookup volume");
    nnodes = sx_nodelist_count(nodes);
    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(nodes, i);
        if (!sx_hashfs_is_node_volume_owner(hashfs, NL_NEXTPREV, node, volume)) {
	    succeeded[i] = 1; /* not a volnode, only used by undo/abort */
            continue;
        }
	if(sx_node_cmp(me, node)) {
	    /* Remote only - local tmpfile will be handled in fileflush_local */
	    if(!proto) {
		const sx_hashfs_volume_t *volume;
		unsigned int blockno;
		sxc_meta_t *fmeta;

		if(!(fmeta = sxc_meta_new(sx)))
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to prepare file propagate query");

		s = sx_hashfs_volume_by_id(hashfs, mis->volume_id, &volume);
		if(s == OK)
		    s = sx_hashfs_tmp_getmeta(hashfs, tmpfile_id, fmeta);
		if(s != OK) {
		    sxc_meta_free(fmeta);
		    action_error(rc2actres(s), rc2http(s), msg_get_reason());
		}

		proto = sxi_fileadd_proto_begin(sx, volume->name, mis->name, mis->revision, 0, mis->block_size, mis->file_size);

		blockno = 0;
		while(proto && blockno < mis->nall) {
		    char hexblock[SXI_SHA1_TEXT_LEN + 1];
		    bin2hex(&mis->all_blocks[blockno], sizeof(mis->all_blocks[0]), hexblock, sizeof(hexblock));
		    blockno++;
		    proto = sxi_fileadd_proto_addhash(sx, proto, hexblock);
		}

		if(proto)
		    proto = sxi_fileadd_proto_end(sx, proto, fmeta);
		sxc_meta_free(fmeta);

		qrylist = calloc(nnodes, sizeof(*qrylist));
		if(!proto || ! qrylist)
		    action_error(rc2actres(ENOMEM), rc2http(ENOMEM), "Failed to prepare file propagate query");
	    }

            qrylist[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[i].cbdata, clust, sx_node_internal_addr(node), proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[i].query_sent = 1;
	} else
	    succeeded[i] = 1; /* Local node is handled in _local  */
    }

 action_failed:
    if(qrylist) {
	for(i=0; i<nnodes; i++) {
	    if(qrylist[i].query_sent) {
                long http_status = 0;
		int rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
		if(rc != -2) {
		    if(rc == -1) {
			WARN("Query failed with %ld", http_status);
			if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else if(http_status != 200 && http_status != 410) {
			act_result_t newret = http2actres(http_status);
			if(newret < ret) /* Severity shall only be raised */
			    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else
			succeeded[i] = 1;
		} else {
		    CRIT("Failed to wait for query");
		    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		}
	    }
	}
        query_list_free(qrylist, nnodes);
    }

    sxi_query_free(proto);
    free(mis);
    return ret;
}

static act_result_t fileflush_local(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    sx_hashfs_tmpinfo_t *mis = NULL;
    const sx_node_t *me, *node;
    unsigned int i, nnodes;
    int64_t tmpfile_id;
    rc_ty s;

    if(job_data->len != sizeof(tmpfile_id)) {
       CRIT("Bad job data");
       action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }
    memcpy(&tmpfile_id, job_data->ptr, sizeof(tmpfile_id));

    DEBUG("fileflush_local for file %lld", (long long)tmpfile_id);
    s = sx_hashfs_tmp_getinfo(hashfs, tmpfile_id, &mis, 0);
    if(s == EFAULT || s == EINVAL) {
	CRIT("Error getting tmpinfo: %s", msg_get_reason());
	action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    if(s == ENOENT) {
	WARN("Token %lld could not be found", (long long)tmpfile_id);
	action_error(ACT_RESULT_PERMFAIL, 500, msg_get_reason());
    }
    if(s != OK)
	action_error(rc2actres(s), rc2http(s), "Failed to check missing blocks");

    nnodes = sx_nodelist_count(nodes);
    me = sx_hashfs_self(hashfs);
    for(i=0; i<nnodes; i++) {
	node = sx_nodelist_get(nodes, i);
	if(!sx_node_cmp(me, node)) {
	    /* Local only - remote file created in fileflush_remote */
	    s = sx_hashfs_tmp_tofile(hashfs, mis);
	    if(s != OK) {
		CRIT("Error creating file: %s", msg_get_reason());
		action_error(rc2actres(s), rc2http(s), msg_get_reason());
	    }
	}
	succeeded[i] = 1;
    }

 action_failed:
    free(mis);
    return ret;
}

static act_result_t replicateblocks_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    /* undo the revision bump from the commit in replicateblocks_request */
    return revision_job_from_tmpfileid(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl, 1, JOBPHASE_UNDO);
}

static act_result_t fileflush_remote_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    const sx_hashfs_volume_t *volume;
    act_result_t ret = ACT_RESULT_OK;
    sx_hashfs_tmpinfo_t *tmp = NULL;
    query_list_t *qrylist = NULL;
    unsigned int nnode, nnodes;
    sxi_query_t *proto = NULL;
    int64_t tmpfile_id;
    rc_ty s;

    nnodes = sx_nodelist_count(nodes);
    if(job_data->len != sizeof(tmpfile_id)) {
       CRIT("Bad job data");
       action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }
    memcpy(&tmpfile_id, job_data->ptr, sizeof(tmpfile_id));
    DEBUG("fileflush_remote for file %lld", (long long)tmpfile_id);

    s = sx_hashfs_tmp_getinfo(hashfs, tmpfile_id, &tmp, 0);
    if(s == ENOENT)
	return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
    if(s != OK) {
	WARN("Failed to retrive file info for tempfile %lld which will not be cleanly removed", (long long)tmpfile_id);
	action_error(rc2actres(s), rc2http(s), msg_get_reason());
    }

    s = sx_hashfs_volume_by_id(hashfs, tmp->volume_id, &volume);
    if(s == ENOENT)
	return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
    if(s != OK)
	action_error(rc2actres(s), rc2http(s), "Failed to find file to delete");

    ret = replicateblocks_abort(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
    for(nnode = 0; nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);
	if(!sx_node_cmp(me, node)) {
	    /* Local node - only parent undo needed */
            succeeded[nnode]++;
	} else {
	    /* Remote node */
	    if(!proto) {
		proto = sxi_filedel_proto(sx, volume->name, tmp->name, tmp->revision);
		if(!proto) {
		    WARN("Cannot allocate proto for job %lld", (long long)job_id);
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
		qrylist = calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate querylist for job %lld", (long long)job_id);
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }
	    qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
	}
    }

 action_failed:
    if(proto) {
	for(nnode=0; qrylist && nnode<nnodes; nnode++) {
	    int rc;
	    long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
	    rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if(http_status == 200 || http_status == 404) {
		succeeded[nnode]++;
	    } else {
		act_result_t newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
	}
	query_list_free(qrylist, nnodes);
	sxi_query_free(proto);
    }
    for(nnode=0; nnode<nnodes; nnode++) {
        /* both 'parent undo' and 'child undo' must succeeded on a node for it
         * to be successful, if one fails both are attempted again */
        succeeded[nnode] = (succeeded[nnode] == 2);
    }

    free(tmp);
    return ret;
}

static act_result_t filedelete_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    const sx_hashfs_volume_t *volume;
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    unsigned int nnode, nnodes;
    sxi_query_t *proto = NULL;
    sx_hashfs_file_t filerev;
    rc_ty s;

    s = filerev_from_jobdata_rev(hashfs, job_data, &filerev);
    if(s == ENOENT) {
	DEBUG("Cannot get revision data from blob for job %lld", (long long)job_id);
	return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
    }
    if (s)
	action_error(rc2actres(s), rc2http(s), "Failed to find file to delete");

    s = sx_hashfs_volume_by_id(hashfs, filerev.volume_id, &volume);
    if(s == ENOENT)
	return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
    if(s != OK)
	action_error(rc2actres(s), rc2http(s), "Failed to find file to delete");

    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);
	if(!sx_node_cmp(me, node)) {
            /* Local node: handled in commit */
	    succeeded[nnode] += 1;
	} else {
	    /* Remote node */
	    if(!proto) {
		proto = sxi_filedel_proto(sx, volume->name, filerev.name, filerev.revision);
		if(!proto) {
		    WARN("Cannot allocate proto for job %lld", (long long)job_id);
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
		qrylist = calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate querylist for job %lld", (long long)job_id);
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }
            qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
            DEBUG("Sending file delete query");
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
	}
    }

 action_failed:
    if(proto) {
	for(nnode=0; qrylist && nnode<nnodes; nnode++) {
	    int rc;
            long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if(http_status == 200 || http_status == 404) {
		succeeded[nnode] = 1;
	    } else {
		act_result_t newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
	}
        query_list_free(qrylist, nnodes);
	sxi_query_free(proto);
    }

    return ret;
}


static act_result_t filedelete_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const sx_node_t *me = sx_hashfs_self(hashfs);
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnode, nnodes;
    rc_ty s;

    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
	if(!sx_node_cmp(me, sx_nodelist_get(nodes, nnode))) {
            sx_hashfs_file_t filerev;
            const sx_hashfs_volume_t *volume;

            s = filerev_from_jobdata_rev(hashfs, job_data, &filerev);
            if(s == ENOENT) {
                DEBUG("Cannot get revision data from blob for job %lld", (long long)job_id);
                return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
            }
            if (s)
                action_error(rc2actres(s), rc2http(s), "Failed to retrieve fileid");
            s = sx_hashfs_volume_by_id(hashfs, filerev.volume_id, &volume);
            if (s)
                action_error(rc2actres(s), rc2http(s), "Failed to retrieve volume id");
    	    s = sx_hashfs_file_delete(hashfs, volume, filerev.name, filerev.revision);
            if (s && s != ENOENT) {
                action_error(rc2actres(s), rc2http(s), "Failed to delete file");
            }
	}
	succeeded[nnode] = 1;
    }

 action_failed:
    return ret;
}

static act_result_t filedelete_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return revision_job_from_rev(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl, -1, JOBPHASE_ABORT);
}

static act_result_t filedelete_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return revision_job_from_rev(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl, -1, JOBPHASE_UNDO);
}

struct cb_challenge_ctx {
    sx_hash_challenge_t chlrsp;
    unsigned int at;
};

static int challenge_cb(curlev_context_t *cbdata, void *ctx, const void *data, size_t size) {
    struct cb_challenge_ctx *c = (struct cb_challenge_ctx *)ctx;
    if(c->at + size > sizeof(c->chlrsp.response))
	return 1;
    memcpy(&c->chlrsp.response[c->at], data, size);
    c->at += size;
    return 0;
}

struct sync_ctx {
    sx_hashfs_t *hashfs;
    const sxi_hostlist_t *hlist;
    char buffer[2*1024*1024]; /* Need to fit entirely the largest possible object */
    char *volname;
    unsigned int at;
    enum { DOING_NOTHING, SYNCING_USERS, SYNCING_VOLUMES, SYNCING_PERMS_VOLUME, SYNCING_PERMS_USERS, SYNCING_MISC } what;
    struct {
	char key[(2+SXLIMIT_META_MAX_KEY_LEN)*6+1];
	char hexvalue[SXLIMIT_META_MAX_VALUE_LEN * 2 + 1];
    } meta[SXLIMIT_META_MAX_ITEMS];
    unsigned int nmeta;
};

static int sync_flush(struct sync_ctx *ctx) {
    int qret;

    if(ctx->what == DOING_NOTHING || !ctx->at) {
	WARN("Out of seq call");
	return -1;
    }

    strcpy(&ctx->buffer[ctx->at], "}}");

    qret = sxi_cluster_query(sx_hashfs_conns(ctx->hashfs), ctx->hlist, REQ_PUT, ".sync", ctx->buffer, ctx->at+2, NULL, NULL, NULL);
    if(qret != 200)
	return -1;

    ctx->what = DOING_NOTHING;
    ctx->at = 0;
    ctx->buffer[0] = '\0';

    return 0;
}

static int syncusers_cb(sx_uid_t user_id, const char *username, const uint8_t *user, const uint8_t *key, int is_admin, const char *desc, int64_t quota, int64_t quota_used, void *ctx) {
    struct sync_ctx *sy = (struct sync_ctx *)ctx;
    unsigned int left = sizeof(sy->buffer) - sy->at;
    char *enc_name, *enc_desc, hexkey[AUTH_KEY_LEN*2+1], hexuser[AUTH_UID_LEN*2+1];

    /* Check if we fit:
       - the preliminary '{"users":' part - 10 bytes
       - a fully encoded username - 2 + length(username) * 6 bytes
       - the key - 40 bytes
       - the user ID - 40 bytes
       - a fully encoded description - 2 + length(desc) * 6 bytes
       - the quota - up to 20 bytes
       - the json skeleton ':{"user":"","key":"","admin":true,"desc":,"quota":} - 53 bytes
       - the trailing '}}\0' - 3 bytes
    */
    if(left < strlen(username) * 6 + strlen(desc ? desc : "") * 6 + 200) {
	if(sync_flush(sy))
	    return -1;
    }

    if(sy->what == DOING_NOTHING) {
	strcpy(sy->buffer, "{\"users\":{");
	sy->at = lenof("{\"users\":{");
    } else if(sy->what == SYNCING_USERS) {
	sy->buffer[sy->at++] = ',';
    } else {
	WARN("Called out of sequence");
	return -1;
    }
    enc_name = sxi_json_quote_string(username);
    if(!enc_name) {
	WARN("Cannot quote username %s", username);
	return -1;
    }
    enc_desc = sxi_json_quote_string(desc ? desc : "");
    if (!enc_desc) {
	WARN("Cannot quote desc %s", desc);
        free(enc_name);
	return -1;
    }
    bin2hex(key, AUTH_KEY_LEN, hexkey, sizeof(hexkey));
    bin2hex(user, AUTH_UID_LEN, hexuser, sizeof(hexuser));
    sprintf(&sy->buffer[sy->at], "%s:{\"user\":\"%s\",\"key\":\"%s\",\"admin\":%s,\"desc\":%s,\"quota\":%lld}", enc_name, hexuser, hexkey, is_admin ? "true" : "false", enc_desc, (long long)quota);
    free(enc_name);
    free(enc_desc);

    sy->what = SYNCING_USERS;
    sy->at = strlen(sy->buffer);

    return 0;
}

static int syncperms_cb(const char *username, int priv, int is_owner, void *ctx) {
    struct sync_ctx *sy = (struct sync_ctx *)ctx;
    unsigned int left = sizeof(sy->buffer) - sy->at;
    char userhex[AUTH_UID_LEN * 2 + 1];
    uint8_t user[AUTH_UID_LEN];

    if(!(priv & (PRIV_READ | PRIV_WRITE)))
	return 0;

    /* Check if we fit:
       - the preliminary '{"perms":' part - 9 bytes
       - the encoded volume name - length(volname) bytes
       - the encoded user - 40 bytes
       - the json skeleton ':{"":"rw",}' - 11 bytes
       - the trailing '}}\0' - 3 bytes
    */
    if(left < strlen(username) * 6 + 64) {
	if(sy->what == SYNCING_PERMS_USERS)
	    strcat(&sy->buffer[sy->at++], "}");
	if(sync_flush(sy))
	    return -1;
    }

    if(sx_hashfs_get_user_by_name(sy->hashfs, username, user, 0)) {
	WARN("Failed to lookup user %s", username);
	return -1;
    }
    bin2hex(user, sizeof(user), userhex, sizeof(userhex));
    if(sy->what == DOING_NOTHING) {
	sprintf(sy->buffer, "{\"perms\":{%s:{",sy->volname);
	sy->at = strlen(sy->buffer);
    } else if(sy->what == SYNCING_PERMS_VOLUME) {
	sprintf(&sy->buffer[sy->at], ",%s:{",sy->volname);
	sy->at = strlen(sy->buffer);
    } else if(sy->what == SYNCING_PERMS_USERS) {
	sy->buffer[sy->at++] = ',';
    } else {
	WARN("Called out of sequence");
	return -1;
    }

    sy->what = SYNCING_PERMS_USERS;
    sprintf(&sy->buffer[sy->at], "\"%s\":%d",
	    userhex, priv);
    sy->at = strlen(sy->buffer);
    return 0;
}

static int sync_global_objects(sx_hashfs_t *hashfs, const sxi_hostlist_t *hlist) {
    const sx_hashfs_volume_t *vol;
    struct sync_ctx ctx;
    rc_ty s;
    int mode = 0;

    ctx.what = DOING_NOTHING;
    ctx.at = 0;
    ctx.hashfs = hashfs;
    ctx.hlist = hlist;

    if(sx_hashfs_list_users(hashfs, NULL, syncusers_cb, 1, 1, &ctx))
	return -1;

    /* Force flush after all users */
    if(ctx.what != DOING_NOTHING && sync_flush(&ctx))
	return -1;

    s = sx_hashfs_volume_first(hashfs, &vol, 0);
    while(s == OK) {
	uint8_t user[AUTH_UID_LEN];
	char userhex[AUTH_UID_LEN * 2 + 1], *enc_name;
	unsigned int need = strlen(vol->name) * 6 + 256;
	unsigned int left = sizeof(ctx.buffer) - ctx.at;

	/* Need to fit:
	   - the preliminary '},"volumes":' part - 13 bytes
	   - the fully encoded volume name - 2 + length(name) * 6
	   - the owner - 40 bytes
	   - the meta (computed and added later)
	   - the json skeleton ':{"owner":"","size":,"replica":,"revs":,"meta":{}},' - ~100 bytes
	   - the trailing '}}\0' - 3 bytes
	*/

	if(sx_hashfs_get_user_by_uid(hashfs, vol->owner, user, 0)) {
	    WARN("Cannot find user %lld (owner of %s)", (long long)vol->owner, vol->name);
	    return -1;
	}
	bin2hex(user, AUTH_UID_LEN, userhex, sizeof(userhex));
	s = sx_hashfs_volumemeta_begin(hashfs, vol);
	if(s == OK) {
	    const char *key;
	    const void *val;
	    unsigned int val_len;

	    ctx.nmeta = 0;
	    while((s=sx_hashfs_volumemeta_next(hashfs, &key, &val, &val_len)) == OK) {
		enc_name = sxi_json_quote_string(key);
		if(!enc_name) {
		    WARN("Cannot encode key %s of volume %s", key, vol->name);
		    s = ENOMEM;
		    break;
		}
		/* encoded key and value lengths + quoting, colon and comma */
		need += strlen(enc_name) + val_len * 2 + 4;
		strcpy(ctx.meta[ctx.nmeta].key, enc_name);
		free(enc_name);
		bin2hex(val, val_len, ctx.meta[ctx.nmeta].hexvalue, sizeof(ctx.meta[0].hexvalue));
		ctx.nmeta++;
	    }
	    if(s == ITER_NO_MORE)
		s = OK;
	}
	if(s != OK) {
	    WARN("Failed to manage metadata for volume %s", vol->name);
	    break;
	}

	if(left < need) {
	    if(sync_flush(&ctx))
		return -1;
	}

	if(ctx.what == DOING_NOTHING) {
	    strcpy(ctx.buffer, "{\"volumes\":{");
	    ctx.at = lenof("{\"volumes\":{");
	} else if(ctx.what == SYNCING_VOLUMES) {
	    ctx.buffer[ctx.at++] = ',';
	} else {
	    WARN("Called out of sequence");
	    return -1;
	}

	enc_name = sxi_json_quote_string(vol->name);
	if(!enc_name) {
	    WARN("Failed to encode volume name %s", vol->name);
	    s = ENOMEM;
	    break;
	}
	sprintf(&ctx.buffer[ctx.at], "%s:{\"owner\":\"%s\",\"size\":%lld,\"replica\":%u,\"revs\":%u", enc_name, userhex, (long long)vol->size, vol->max_replica, vol->revisions);
	free(enc_name);
	if(ctx.nmeta) {
	    unsigned int i;
	    strcat(ctx.buffer, ",\"meta\":{");
	    for(i=0; i<ctx.nmeta; i++) {
		ctx.at = strlen(ctx.buffer);
		sprintf(&ctx.buffer[ctx.at], "%s%s:\"%s\"",
			i ? "," : "",
			ctx.meta[i].key,
			ctx.meta[i].hexvalue);
	    }
	    strcat(ctx.buffer, "}");
	}
	ctx.at = strlen(ctx.buffer);
	strcat(&ctx.buffer[ctx.at++], "}");
	ctx.what = SYNCING_VOLUMES;
	s = sx_hashfs_volume_next(hashfs);
    }
    if(s != ITER_NO_MORE) {
	WARN("Sending failed with %d", s);
	return -1;
    }

    /* Force flush after all volumes */
    if(ctx.what != DOING_NOTHING && sync_flush(&ctx))
	return -1;

    s = sx_hashfs_volume_first(hashfs, &vol, 0);
    while(s == OK) {
	ctx.volname = sxi_json_quote_string(vol->name);
	if(!ctx.volname) {
	    WARN("Failed to encode volume %s", vol->name);
	    return -1;
	}
	if(sx_hashfs_list_acl(hashfs, vol, 0, PRIV_ADMIN, syncperms_cb, &ctx)) {
	    WARN("Failed to list permissions for %s: %s", vol->name, msg_get_reason());
	    free(ctx.volname);
	    return -1;
	}
	free(ctx.volname);

	if(ctx.what == SYNCING_PERMS_USERS) {
	    strcat(&ctx.buffer[ctx.at++], "}");
	    ctx.what = SYNCING_PERMS_VOLUME;
	}
	s = sx_hashfs_volume_next(hashfs);
    }

    if(ctx.what != DOING_NOTHING) {
	if(ctx.what == SYNCING_PERMS_USERS)
	    strcat(&ctx.buffer[ctx.at++], "}");
	if(sync_flush(&ctx))
	    return -1;
    }

    if(sx_hashfs_cluster_get_mode(hashfs, &mode)) {
        WARN("Failed to get cluster operating mode");
        return -1;
    }

    time_t last_clustermeta_mod;
    if(sx_hashfs_clustermeta_last_change(hashfs, &last_clustermeta_mod)) {
        WARN("Failed to last cluster meta modification time");
        return -1;
    }

    /* Syncing misc globs */
    sprintf(ctx.buffer, "{\"misc\":{\"mode\":\"%s\",\"clusterMetaLastModified\":%ld", mode ? "ro" : "rw", last_clustermeta_mod);
    ctx.what = SYNCING_MISC;
    ctx.at = strlen(ctx.buffer);

    s = sx_hashfs_clustermeta_begin(hashfs);
    if(s == OK) {
        const char *key;
        const void *val;
        unsigned int val_len;
        char *enc_name;

        ctx.nmeta = 0;
        while((s=sx_hashfs_clustermeta_next(hashfs, &key, &val, &val_len)) == OK) {
            enc_name = sxi_json_quote_string(key);
            if(!enc_name) {
                WARN("Cannot encode cluster meta key %s", key);
                s = ENOMEM;
                break;
            }
            /* encoded key and value lengths + quoting, colon and comma */
            strcpy(ctx.meta[ctx.nmeta].key, enc_name);
            free(enc_name);
            bin2hex(val, val_len, ctx.meta[ctx.nmeta].hexvalue, sizeof(ctx.meta[0].hexvalue));
            ctx.nmeta++;
        }
        if(s == ITER_NO_MORE)
            s = OK;
    }
    if(s != OK) {
        WARN("Failed to get cluster meta");
        return -1;
    }

    if(ctx.nmeta) {
        unsigned int i;
        strcat(&ctx.buffer[ctx.at], ",\"clusterMeta\":{");
        ctx.at += lenof(",\"clusterMeta\":{");
        for(i=0; i<ctx.nmeta; i++) {
            ctx.at = strlen(ctx.buffer);
            sprintf(&ctx.buffer[ctx.at], "%s%s:\"%s\"",
                    i ? "," : "",
                    ctx.meta[i].key,
                    ctx.meta[i].hexvalue);
        }
        strcat(ctx.buffer, "}");
        ctx.at = strlen(ctx.buffer);
    }

    if(sync_flush(&ctx))
        return -1;

    return 0;
}

static act_result_t challenge_and_sync(sx_hashfs_t *hashfs, const sx_node_t *node, int *fail_code, char *fail_msg) {
    sx_hash_challenge_t chlrsp;
    char challenge[lenof(".challenge/") + sizeof(chlrsp.challenge) * 2 + 1];
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    act_result_t ret = ACT_RESULT_OK;
    struct cb_challenge_ctx ctx;
    sxi_query_t *initproto;
    sxi_hostlist_t hlist;
    int qret;

    sxi_hostlist_init(&hlist);
    ctx.at = 0;
    if(sx_hashfs_challenge_gen(hashfs, &chlrsp, 1))
	action_error(ACT_RESULT_TEMPFAIL, 500, "Cannot generate challenge for new node");

    if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(node)))
	action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to perform challenge request");

    strcpy(challenge, ".challenge/");
    bin2hex(chlrsp.challenge, sizeof(chlrsp.challenge), challenge + lenof(".challenge/"), sizeof(challenge) - lenof(".challenge/"));
    qret = sxi_cluster_query(clust, &hlist, REQ_GET, challenge, NULL, 0, NULL, challenge_cb, &ctx);
    if(qret != 200 || ctx.at != sizeof(chlrsp.response))
	action_error(http2actres(qret), qret, sxc_geterrmsg(sx));

    if(memcmp(chlrsp.response, ctx.chlrsp.response, sizeof(chlrsp.response)))
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad challenge response");

    initproto = sxi_nodeinit_proto(sx,
				   sx_hashfs_cluster_name(hashfs),
				   sx_node_uuid_str(node),
				   sx_hashfs_http_port(hashfs),
				   sx_hashfs_uses_secure_proto(hashfs),
				   sx_hashfs_ca_file(hashfs));
    if(!initproto)
	action_error(rc2actres(ENOMEM), rc2http(ENOMEM), "Failed to prepare query");

    qret = sxi_cluster_query(clust, &hlist, initproto->verb, initproto->path, initproto->content, initproto->content_len, NULL, NULL, NULL);
    sxi_query_free(initproto);
    if(qret != 200)
	action_error(http2actres(qret), qret, "Failed to initialize new node");

    /* MOHDIST: Create users and volumes */
    if(sync_global_objects(hashfs, &hlist))
	action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to syncronize blobal objects on new node");

    ret = ACT_RESULT_OK;

 action_failed:
    sxi_hostlist_empty(&hlist);
    return ret;
}

static act_result_t distribution_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_hdist_t *hdist;
    const sx_nodelist_t *prev, *next;
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnode, nnodes;
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxi_query_t *proto = NULL;
    sxi_hostlist_t hlist;
    int qret;
    rc_ty s;

    if(!job_data) {
	NULLARG();
	action_set_fail(ACT_RESULT_PERMFAIL, 500, "Null job");
	return ret;
    }

    hdist = sxi_hdist_from_cfg(job_data->ptr, job_data->len);
    if(!hdist) {
	WARN("Cannot load hdist config");
	s = ENOMEM;
	action_set_fail(rc2actres(s), rc2http(s), msg_get_reason());
	return ret;
    }

    sxi_hostlist_init(&hlist);

    if(sxi_hdist_buildcnt(hdist) != 2) {
	WARN("Invalid distribution found (builds = %d)", sxi_hdist_buildcnt(hdist));
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    prev = sxi_hdist_nodelist(hdist, 1);
    next = sxi_hdist_nodelist(hdist, 0);
    if(!prev || !next) {
	WARN("Invalid distribution found");
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    proto = sxi_distribution_proto_begin(sx, job_data->ptr, job_data->len);
    if(proto)
	proto = sxi_distribution_proto_end(sx, proto);
    if(!proto) {
	WARN("Cannot allocate proto for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }
    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode < nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);
	int was_in = sx_nodelist_lookup(prev, sx_node_uuid(node)) != NULL;
	int is_in = sx_nodelist_lookup(next, sx_node_uuid(node)) != NULL;

	if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(node)))
	    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to perform challenge request");

	if(!was_in) {
	    if(!is_in) {
		WARN("Node %s is not part of either the old and the new distributions", sx_node_uuid_str(node));
		action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
	    }

	    if(!sx_node_cmp(me, node)) {
		WARN("This node cannot be both a distribution change initiator and a new node");
		action_error(ACT_RESULT_PERMFAIL, 500, "Something is really out of place");
	    }

	    /* Challenge new node */
	    ret = challenge_and_sync(hashfs, node, fail_code, fail_msg);
	    if(ret != ACT_RESULT_OK)
		goto action_failed;
	}

	if(sx_node_cmp(me, node)) {
	    qret = sxi_cluster_query(clust, &hlist, proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL, NULL);
	    if(qret != 200)
		action_error(http2actres(qret), qret, sxc_geterrmsg(sx));
	} else {
	    s = sx_hashfs_hdist_change_add(hashfs, job_data->ptr, job_data->len);
	    if(s)
		action_set_fail(rc2actres(s), rc2http(s), msg_get_reason());
	}

	sxi_hostlist_empty(&hlist);
	succeeded[nnode] = 1;
    }


action_failed:
    sxi_query_free(proto);
    sxi_hostlist_empty(&hlist);
    sxi_hdist_free(hdist);

    return ret;
}


static act_result_t commit_dist_common(sx_hashfs_t *hashfs, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg) {
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnode, nnodes;
    sxi_hostlist_t hlist;
    rc_ty s;

    sxi_hostlist_init(&hlist);
    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode < nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);

	if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(node)))
	    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to perform the enable distribution request");

	if(sx_node_cmp(me, node)) {
	    int qret = sxi_cluster_query(clust, &hlist, REQ_PUT, ".dist", NULL, 0, NULL, NULL, NULL);
	    if(qret != 200)
		action_error(http2actres(qret), qret, sxc_geterrmsg(sx));
	} else {
	    s = sx_hashfs_hdist_change_commit(hashfs);
	    if(s)
		action_set_fail(rc2actres(s), rc2http(s), msg_get_reason());
	}

	sxi_hostlist_empty(&hlist);
	succeeded[nnode] = 1;
    }

action_failed:
    sxi_hostlist_empty(&hlist);

    return ret;
}

static act_result_t distribution_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    sxi_hdist_t *hdist = NULL;

    if(!job_data) {
	NULLARG();
	action_error(ACT_RESULT_PERMFAIL, 500, "Null job");
    }

    hdist = sxi_hdist_from_cfg(job_data->ptr, job_data->len);
    if(!hdist) {
	WARN("Cannot load hdist config");
	action_error(rc2actres(ENOMEM), rc2http(ENOMEM), msg_get_reason());
    }

    if(sxi_hdist_buildcnt(hdist) != 2) {
	WARN("Invalid distribution found (builds = %d)", sxi_hdist_buildcnt(hdist));
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    ret = commit_dist_common(hashfs, nodes, succeeded, fail_code, fail_msg);

action_failed:
    sxi_hdist_free(hdist);
    return ret;
}

static act_result_t revoke_dist_common(sx_hashfs_t *hashfs, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg) {
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnode, nnodes;
    sxi_hostlist_t hlist;
    rc_ty s;

    sxi_hostlist_init(&hlist);
    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode < nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);

	if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(node)))
	    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to perform the revoke distribution request");

	if(sx_node_cmp(me, node)) {
	    int qret = sxi_cluster_query(clust, &hlist, REQ_DELETE, ".dist", NULL, 0, NULL, NULL, NULL);
	    if(qret != 200)
		action_error(http2actres(qret), qret, sxc_geterrmsg(sx));
	} else {
	    s = sx_hashfs_hdist_change_revoke(hashfs);
	    if(s)
		action_set_fail(rc2actres(s), rc2http(s), msg_get_reason());
	}

	sxi_hostlist_empty(&hlist);
	succeeded[nnode] = 1;
    }

action_failed:
    sxi_hostlist_empty(&hlist);

    return ret;
}

static act_result_t distribution_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    sxi_hdist_t *hdist = NULL;

    if(!job_data) {
	NULLARG();
	action_error(ACT_RESULT_PERMFAIL, 500, "Null job");
    }

    hdist = sxi_hdist_from_cfg(job_data->ptr, job_data->len);
    if(!hdist) {
	WARN("Cannot load hdist config");
	action_error(rc2actres(ENOMEM), rc2http(ENOMEM), msg_get_reason());
    }

    if(sxi_hdist_buildcnt(hdist) != 2) {
	WARN("Invalid distribution found (builds = %d)", sxi_hdist_buildcnt(hdist));
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    ret = revoke_dist_common(hashfs, nodes, succeeded, fail_code, fail_msg);

action_failed:
    sxi_hdist_free(hdist);
    return ret;
}

static act_result_t distribution_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret;

    CRIT("The attempt to change the cluster distribution model (i.e. nodes) resulted in a fatal failure leaving it in an inconsistent state");
    action_set_fail(ACT_RESULT_PERMFAIL, 500, "The attempt to change the cluster distribution model (i.e. nodes) resulted in a fatal failure leaving it in an inconsistent state");
    return ret;
}


static act_result_t startrebalance_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    unsigned int i, nnodes = sx_nodelist_count(nodes);
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    rc_ty s;

    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(nodes, i);
	if(sx_node_cmp(me, node)) {
	    /* Remote node */
	    if(!qrylist) {
		qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate query");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }

            qrylist[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[i].cbdata, clust, sx_node_internal_addr(node), REQ_PUT, ".rebalance", NULL, 0, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[i].query_sent = 1;
	} else {
	    /* Local node */
	    s = sx_hashfs_hdist_rebalance(hashfs);
	    if(s != OK)
		action_error(rc2actres(s), rc2http(s), msg_get_reason());
	    succeeded[i] = 1;
	}
    }

 action_failed:
    if(qrylist) {
	for(i=0; i<nnodes; i++) {
	    if(qrylist[i].query_sent) {
                long http_status = 0;
		int rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
		if(rc != -2) {
		    if(rc == -1) {
			WARN("Query failed with %ld", http_status);
			if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else if(http_status != 200) {
			act_result_t newret = http2actres(http_status);
			if(newret < ret) /* Severity shall only be raised */
			    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else
			succeeded[i] = 1;
		} else {
		    CRIT("Failed to wait for query");
		    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		}
	    }
	}
        query_list_free(qrylist, nnodes);
    }

    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }

    return ret;
}


static act_result_t jlock_common(int lock, sx_hashfs_t *hashfs, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    unsigned int i, nnodes = sx_nodelist_count(nodes);
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    char *query = NULL;
    rc_ty s;

    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(nodes, i);
	const char *owner = sx_node_uuid_str(me);
	if(sx_node_cmp(me, node)) {
	    /* Remote node */
	    if(!query) {
		query = wrap_malloc(lenof(".jlock/") + strlen(owner) + 1);
		qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
		if(!query || !qrylist) {
		    WARN("Cannot allocate query");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
		sprintf(query, ".jlock/%s", owner);
	    }

            qrylist[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[i].cbdata, clust, sx_node_internal_addr(node), lock ? REQ_PUT : REQ_DELETE, query, NULL, 0, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[i].query_sent = 1;
	} else {
	    /* Local node */
	    if(lock)
		s = sx_hashfs_job_lock(hashfs, owner);
	    else
		s = sx_hashfs_job_unlock(hashfs, owner);
	    if(s != OK)
		action_error(rc2actres(s), rc2http(s), msg_get_reason());
	    succeeded[i] = 1;
	}
    }

 action_failed:
    if(qrylist) {
	for(i=0; i<nnodes; i++) {
	    if(qrylist[i].query_sent) {
                long http_status = 0;
		int rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
		if(rc != -2) {
		    if(rc == -1) {
			WARN("Query failed with %ld", http_status);
			if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else if(http_status != 200) {
			act_result_t newret = http2actres(http_status);
			if(newret < ret) /* Severity shall only be raised */
			    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else
			succeeded[i] = 1;
		} else {
		    CRIT("Failed to wait for query");
		    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		}
	    }
	}
        query_list_free(qrylist, nnodes);
    }

    free(query);
    return ret;
}

static act_result_t jlock_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return jlock_common(1, hashfs, nodes, succeeded, fail_code, fail_msg);
}

static act_result_t jlock_abort_and_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return jlock_common(0, hashfs, nodes, succeeded, fail_code, fail_msg);
}



static const sx_node_t *blocktarget(sx_hashfs_t *hashfs, const block_meta_t *b) {
    const sx_node_t *self = sx_hashfs_self(hashfs);
    const sx_nodelist_t *odst, *ndst;
    sx_nodelist_t *oldnodes, *newnodes;
    const sx_node_t *ret = NULL;
    unsigned int i, or, nr;

    odst = sx_hashfs_all_nodes(hashfs, NL_PREV);
    ndst = sx_hashfs_all_nodes(hashfs, NL_NEXT);
    if(!odst || !ndst)
	return NULL;

    or = sx_nodelist_count(odst);
    nr = sx_nodelist_count(ndst);
    if(!or || !nr)
	return NULL;

    oldnodes = sx_hashfs_all_hashnodes(hashfs, NL_PREV, &b->hash, or);
    if(!oldnodes) {
	WARN("No old node set");
	return NULL;
    }

    newnodes = sx_hashfs_all_hashnodes(hashfs, NL_NEXT, &b->hash, nr);
    if(!newnodes) {
	WARN("No new node set");
	sx_nodelist_delete(oldnodes);
	return NULL;
    }

    for(i=0; i<or; i++) {
	const sx_node_t *target;
	if(sx_node_cmp(sx_nodelist_get(oldnodes, i), self))
	    continue;
	if(i >= nr) {
	    /* Not reached: we prevent the numer of nodes to be less than the max replica */
	    WARN("We were replica %u for block but the new model only has got %u replicas", i, nr);
	    break;
	}
	target = sx_nodelist_get(newnodes, i);

	/* Convert the target node from the allocated list into a const
	 * node from the hashfs list so the caller needs no free */
	for(i=0; i<nr; i++) {
	    const sx_node_t *ctarget = sx_nodelist_get(ndst, i);
	    if(sx_node_cmp(ctarget, target))
		continue;
	    ret = ctarget;
	    break;
	}
	break;
    }

    sx_nodelist_delete(oldnodes);
    sx_nodelist_delete(newnodes);
    return ret;
}

#define RB_MAX_NODES (2 /* FIXME: bump me ? */)
#define RB_MAX_BLOCKS (100 /* FIXME: should be a sane(!) multiple of DOWNLOAD_MAX_BLOCKS */)
#define RB_MAX_TRIES (RB_MAX_BLOCKS * RB_MAX_NODES)
static act_result_t blockrb_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const sx_node_t *self = sx_hashfs_self(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_nodelist_t *next = sx_hashfs_all_nodes(hashfs, NL_NEXT);
    act_result_t ret = ACT_RESULT_OK;
    struct {
	curlev_context_t *cbdata;
	sxi_query_t *proto;
	const sx_node_t *node;
	block_meta_t *blocks[RB_MAX_BLOCKS];
	unsigned int nblocks;
	int query_sent;
    } rbdata[RB_MAX_NODES];
    unsigned int i, j, maxnodes = 0, maxtries;
    rc_ty s;

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    memset(rbdata, 0, sizeof(rbdata));

    s = sx_hashfs_br_begin(hashfs);
    if(s == ITER_NO_MORE) {
	INFO("No more blocks to be relocated");
	succeeded[0] = 1;
	return ACT_RESULT_OK;
    } else if(s != OK)
	action_error(rc2actres(s), rc2http(s), msg_get_reason());

    maxnodes = MIN(RB_MAX_NODES, sx_nodelist_count(next) - (sx_nodelist_lookup(next, sx_node_uuid(self)) != NULL));
    maxtries = RB_MAX_TRIES; /* Maximum *consecutive* attempts to find a pushable block */
    while(maxtries) {
	const sx_node_t *target;
	block_meta_t *blockmeta;
	char hstr[sizeof(blockmeta->hash) * 2 +1];

	s = sx_hashfs_br_next(hashfs, &blockmeta);
	if(s != OK)
	    break;

	bin2hex(&blockmeta->hash, sizeof(blockmeta->hash), hstr, sizeof(hstr));

	target = blocktarget(hashfs, blockmeta);
	if(!target) {
	    /* Should never trigger */
	    WARN("Failed to identify target for %s", hstr);
	    sx_hashfs_blockmeta_free(&blockmeta);
	    s = FAIL_EINTERNAL;
	    break;
	}
	if(!sx_node_cmp(self, target)) {
	    /* Not to be moved */
	    DEBUG("Block %s is not to be moved", hstr);
            DEBUGHASH("br_ignore", &blockmeta->hash);
	    sx_hashfs_blockmeta_free(&blockmeta);
	    continue;
	}
        if ((s = sx_hashfs_br_use(hashfs, blockmeta))) {
	    sx_hashfs_blockmeta_free(&blockmeta);
            break;
        }
	for(i=0; i<maxnodes; i++) {
	    if(!rbdata[i].node) {
		rbdata[i].node = target;
		break;
	    }
	    if(!sx_node_cmp(rbdata[i].node, target))
		break;
	}
	if(i == maxnodes) {
	    /* All target slots are taken, will target again later */
	    DEBUG("Block %s is targeted for %s(%s) to which we currently do not have a channel", hstr, sx_node_uuid_str(target), sx_node_internal_addr(target));
	    sx_hashfs_blockmeta_free(&blockmeta);
	    maxtries--;
	    continue;
	}
	if(rbdata[i].nblocks >= RB_MAX_BLOCKS) {
	    /* This target is already full */
	    DEBUG("Channel to %s (%s) have all the slots full: block %s will be moved later", sx_node_uuid_str(target), sx_node_internal_addr(target), hstr);
	    sx_hashfs_blockmeta_free(&blockmeta);
	    maxtries--;
	    continue;
	}

	rbdata[i].blocks[rbdata[i].nblocks] = blockmeta;
	rbdata[i].nblocks++;
	maxtries = RB_MAX_TRIES; /* Reset tries to the max */
	if(rbdata[i].nblocks >= RB_MAX_BLOCKS) {
	    /* Target has reached capacity, check if everyone is full */
	    for(j=0; j<maxnodes; j++)
		if(rbdata[j].nblocks < RB_MAX_BLOCKS)
		    break;
	    if(j == maxnodes) {
		DEBUG("All slots on all channels are now complete");
		break; /* All slots for all targets are full */
	    }
	}
    }

    if(s == OK || s == ITER_NO_MORE) {
	unsigned int dist_version;
	const sx_uuid_t *dist_id = sx_hashfs_distinfo(hashfs, &dist_version, NULL);
	if(!dist_id) {
	    WARN("Cannot retrieve distribution version");
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Failed toretrieve distribution version");
	}
	for(i=0; i<maxnodes; i++) {
	    if(!rbdata[i].node)
		break;

            /* FIXME: proper expiration time */
	    rbdata[i].proto = sxi_hashop_proto_inuse_begin(sx, NULL);
	    for(j=0; j<rbdata[i].nblocks; j++)
		rbdata[i].proto = sxi_hashop_proto_inuse_hash(sx, rbdata[i].proto, rbdata[i].blocks[j]);
	    rbdata[i].proto = sxi_hashop_proto_inuse_end(sx, rbdata[i].proto);
	    if(!rbdata[i].proto)
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");

            rbdata[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(rbdata[i].cbdata, clust, sx_node_internal_addr(rbdata[i].node), rbdata[i].proto->verb, rbdata[i].proto->path, rbdata[i].proto->content, rbdata[i].proto->content_len, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(rbdata[i].node), sxc_geterrmsg(sx));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    rbdata[i].query_sent = 1;
	}
    } else
	action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to iterate blocks");


action_failed:

    for(i=0; i<maxnodes; i++) {
	if(!rbdata[i].node)
	    break;

	if(rbdata[i].query_sent) {
            long http_status = 0;
	    int rc = sxi_cbdata_wait(rbdata[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc != -2) {
		if(rc == -1) {
		    WARN("Query failed with %ld", http_status);
		    if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(rbdata[i].cbdata));
		} else if(http_status != 200) {
		    act_result_t newret = http2actres(http_status);
		    if(newret < ret) /* Severity shall only be raised */
			action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(rbdata[i].cbdata));
		} else {
		    for(j=0; j<rbdata[i].nblocks; j++) {
			if(sx_hashfs_blkrb_hold(hashfs, &rbdata[i].blocks[j]->hash, rbdata[i].blocks[j]->blocksize, rbdata[i].node) != OK)
			    WARN("Cannot hold block"); /* Unexpected but not critical, will retry later */
			else if(sx_hashfs_xfer_tonode(hashfs, &rbdata[i].blocks[j]->hash, rbdata[i].blocks[j]->blocksize, rbdata[i].node) != OK)
			    WARN("Cannot add block to transfer queue"); /* Unexpected but not critical, will retry later */
			else if(sx_hashfs_br_delete(hashfs, rbdata[i].blocks[j]) != OK)
			    WARN("Cannot delete block"); /* Unexpected but not critical, will retry later */
			else {
			    char h[sizeof(sx_hash_t) * 2 +1];
			    bin2hex(&rbdata[i].blocks[j]->hash, sizeof(sx_hash_t), h, sizeof(h));
			    DEBUG("Deleted block %s", h);
			}
		    }
		}
	    } else {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
	    }
	}

	for(j=0; j<rbdata[i].nblocks; j++)
	    sx_hashfs_blockmeta_free(&rbdata[i].blocks[j]);

        sxi_cbdata_unref(&rbdata[i].cbdata);
	sxi_query_free(rbdata[i].proto);

    }

    /* If some block was skipped, return tempfail so we get called again later */
    if(ret == ACT_RESULT_OK) {
	DEBUG("All blocks in this batch queued for tranfer; more to come later...");
	action_set_fail(ACT_RESULT_TEMPFAIL, 503, "Block propagation in progress");
    }

    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }

    return ret;
}

static act_result_t blockrb_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    if(sx_hashfs_blkrb_is_complete(hashfs) != OK) {
	INFO("Waiting for pending block tranfers to complete");
	action_error(ACT_RESULT_TEMPFAIL, 500, "Awaiting completion of block propagation");
    }

    INFO("All blocks were migrated successfully");
    succeeded[0] = 1;

 action_failed:
    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}


static act_result_t filerb_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    sx_hashfs_set_progress_info(hashfs, INPRG_REBALANCE_RUNNING, "Relocating metadata");

    if(sx_hashfs_relocs_populate(hashfs) != OK) {
	INFO("Failed to populate the relocation queue");
	action_error(ACT_RESULT_TEMPFAIL, 500, "Failed to setup file relocation");
    }

    succeeded[0] = 1;

 action_failed:
    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}

#define RB_MAX_FILES 128
static act_result_t filerb_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    struct {
	const sx_reloc_t *reloc;
	curlev_context_t *cbdata;
	sxi_query_t *proto;
	int query_sent;
    } rbdata[RB_MAX_FILES];
    unsigned int i;
    act_result_t ret;

    memset(&rbdata, 0, sizeof(rbdata));

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    sx_hashfs_relocs_begin(hashfs);

    for(i = 0; i<RB_MAX_FILES; i++) {
	const sx_reloc_t *rlc;
	unsigned int blockno;
	rc_ty r;

	r = sx_hashfs_relocs_next(hashfs, &rlc);
	if(r == ITER_NO_MORE)
	    break;
	if(r != OK)
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to lookup file to relocate");

	rbdata[i].reloc = rlc;
	rbdata[i].proto = sxi_fileadd_proto_begin(sx,
						  rlc->volume.name,
						  rlc->file.name,
						  rlc->file.revision,
						  0,
						  rlc->file.block_size,
						  rlc->file.file_size);
	blockno = 0;
	while(rbdata[i].proto && blockno < rlc->file.nblocks) {
	    char hexblock[SXI_SHA1_TEXT_LEN + 1];
	    bin2hex(&rlc->blocks[blockno], sizeof(rlc->blocks[0]), hexblock, sizeof(hexblock));
	    blockno++;
	    if(rbdata[i].proto)
		rbdata[i].proto = sxi_fileadd_proto_addhash(sx, rbdata[i].proto, hexblock);
	}

	if(rbdata[i].proto)
	    rbdata[i].proto = sxi_fileadd_proto_end(sx, rbdata[i].proto, rlc->metadata);

	if(!rbdata[i].proto)
	    action_error(rc2actres(ENOMEM), rc2http(ENOMEM), "Failed to prepare file relocation query");

	rbdata[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	DEBUG("File query: %u %s [ %s ]", rbdata[i].proto->verb, rbdata[i].proto->path, (char *)rbdata[i].proto->content);
	if(sxi_cluster_query_ev(rbdata[i].cbdata, clust, sx_node_internal_addr(rlc->target), rbdata[i].proto->verb, rbdata[i].proto->path, rbdata[i].proto->content, rbdata[i].proto->content_len, NULL, NULL)) {
	    WARN("Failed to query node %s: %s", sx_node_uuid_str(rlc->target), sxc_geterrmsg(sx));
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	}
	rbdata[i].query_sent = 1;
    }

    if(i == RB_MAX_FILES) {
	DEBUG("Reached file limit, will resume later");
	action_error(ACT_RESULT_TEMPFAIL, 503, "File relocation in progress");
    }

    ret = ACT_RESULT_OK;

 action_failed:
    for(i = 0; i<RB_MAX_FILES; i++) {
	if(rbdata[i].query_sent) {
            long http_status = 0;
	    int rc = sxi_cbdata_wait(rbdata[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc != -2) {
		if(rc == -1) {
		    WARN("Query failed with %ld", http_status);
		    if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(rbdata[i].cbdata));
		} else if(http_status != 200) {
		    act_result_t newret = http2actres(http_status);
		    if(newret < ret) /* Severity shall only be raised */
			action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(rbdata[i].cbdata));
		} else if(sx_hashfs_relocs_delete(hashfs, rbdata[i].reloc) != OK) {
		    if(ret == ACT_RESULT_OK)
			action_set_fail(ACT_RESULT_TEMPFAIL, 503, "Failed to delete file from relocation queue");
		}
	    } else {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
	    }
	}

        sxi_cbdata_unref(&rbdata[i].cbdata);
	sxi_query_free(rbdata[i].proto);
	sx_hashfs_reloc_free(rbdata[i].reloc);
    }

    if(ret == ACT_RESULT_OK) {
	if(sx_hashfs_set_progress_info(hashfs, INPRG_REBALANCE_COMPLETE, "Relocation complete") == OK) {
	    INFO(">>>>>>>>>>>> OBJECT RELOCATION COMPLETE <<<<<<<<<<<<");
	    succeeded[0] = 1;
	} else
	    action_set_fail(ACT_RESULT_TEMPFAIL, 503, msg_get_reason());
    }

    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}


static act_result_t finishrebalance_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    unsigned int i, nnodes = sx_nodelist_count(nodes);
    act_result_t ret = ACT_RESULT_TEMPFAIL;
    sxi_hostlist_t hlist;

    sxi_hostlist_init(&hlist);

    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(nodes, i);
	DEBUG("Checking for rebalance completion on %s", sx_node_internal_addr(node));
	if(sx_node_cmp(me, node)) {
	    /* Remote node */
	    clst_t *clst;
	    clst_state qret;

	    if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(node)))
		action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to query rebalance status");
	    clst = clst_query(clust, &hlist);
	    if(!clst)
		action_error(ACT_RESULT_TEMPFAIL, 500, "Failed to query rebalance status");

	    qret = clst_rebalance_state(clst, NULL);
	    clst_destroy(clst);

	    if(qret == CLSTOP_COMPLETED)
		succeeded[i] = 1;
	    else if(qret == CLSTOP_INPROGRESS) {
		DEBUG("Relocation still running on node %s", sx_node_uuid_str(node));
		action_error(ACT_RESULT_TEMPFAIL, 500, "Relocation still running");
	    } else {
		WARN("Unexpected rebalance state on node %s", sx_node_uuid_str(node));
		action_error(ACT_RESULT_TEMPFAIL, 500, "Unexpected rebalance status");
	    }

	    sxi_hostlist_empty(&hlist);
	} else {
	    /* Local node */
	    sx_inprogress_t inprg = sx_hashfs_get_progress_info(hashfs, NULL);
	    if(inprg == INPRG_ERROR)
		action_error(ACT_RESULT_TEMPFAIL, 500, "Unexpected rebalance state on local node");
	    else if(inprg == INPRG_REBALANCE_COMPLETE)
		succeeded[i] = 1;
	    else
		action_error(ACT_RESULT_TEMPFAIL, 500, "Rebalance still running on local node");
	}
    }

    ret = ACT_RESULT_OK;

 action_failed:
    sxi_hostlist_empty(&hlist);

    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	/* NOTE: this block was put in here for consistency with other handler, even if it cannot be reached */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }

    return ret;
}


static act_result_t finishrebalance_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    unsigned int i, nnodes = sx_nodelist_count(nodes);
    act_result_t ret = ACT_RESULT_OK;
    query_list_t *qrylist = NULL;
    rc_ty s;

    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(nodes, i);
	DEBUG("Stopping rebalance on %s", sx_node_internal_addr(node));
	if(sx_node_cmp(me, node)) {
	    /* Remote node */
	    if(!qrylist) {
		qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
		if(!qrylist) {
		    WARN("Cannot allocate query");
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
		}
	    }

            qrylist[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[i].cbdata, clust, sx_node_internal_addr(node), REQ_DELETE, ".rebalance", NULL, 0, NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[i].query_sent = 1;
	} else {
	    /* Local node */
	    s = sx_hashfs_hdist_endrebalance(hashfs);
	    if(s != OK)
		action_error(rc2actres(s), rc2http(s), msg_get_reason());
	    succeeded[i] = 1;
	}
    }

 action_failed:
    if(qrylist) {
	for(i=0; i<nnodes; i++) {
	    if(qrylist[i].query_sent) {
                long http_status = 0;
		int rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
		if(rc != -2) {
		    if(rc == -1) {
			WARN("Query failed with %ld", http_status);
			if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else if(http_status != 200) {
			act_result_t newret = http2actres(http_status);
			if(newret < ret) /* Severity shall only be raised */
			    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else
			succeeded[i] = 1;
		} else {
		    CRIT("Failed to wait for query");
		    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		}
	    }
	}
        query_list_free(qrylist, nnodes);
    }

    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}


static act_result_t cleanrb_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s;

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    if((s = sx_hashfs_hdist_set_rebalanced(hashfs))) {
	WARN("Cannot set rebalanced: %s", msg_get_reason());
	action_error(rc2actres(s), rc2http(s), msg_get_reason());
    }

    sx_hashfs_set_progress_info(hashfs, INPRG_REBALANCE_COMPLETE, "Cleaning up relocated objects after successful rebalance");

    succeeded[0] = 1;

 action_failed:
    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}

static act_result_t cleanrb_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    if(sx_hashfs_rb_cleanup(hashfs) != OK)
	action_error(ACT_RESULT_TEMPFAIL, 503, "Cleanup failed");

    sx_hashfs_set_progress_info(hashfs, INPRG_IDLE, NULL);

    INFO(">>>>>>>>>>>> THIS NODE IS NOW FULLY REBALANCED <<<<<<<<<<<<");
    succeeded[0] = 1;

 action_failed:
    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}

/* Context used to push volume sizes */
struct volsizes_push_ctx {
    unsigned int idx; /* index of a node which query was sent to */
    unsigned int fail; /* Will be set to 0 if query has been successfully sent to all nodes */
    sxi_query_t *query; /* query reference used to send query */
};

/* Push volume sizes to particular node */
static curlev_context_t *push_volume_sizes(sx_hashfs_t *h, const sx_node_t *n, unsigned int node_index, sxi_query_t *query) {
    curlev_context_t *ret;
    struct volsizes_push_ctx *ctx;

    if(!h || !n || !query) {
        NULLARG();
        return NULL;
    }

    ret = sxi_cbdata_create_generic(sx_hashfs_conns(h), NULL, NULL);
    if(!ret) {
        WARN("Failed to allocate cbdata");
        sxi_query_free(query);
        return NULL;
    }

    /* Create context which will be added to cbdata */
    ctx = malloc(sizeof(*ctx));
    if(!ctx) {
        WARN("Failed to allocate push context");
        sxi_query_free(query);
        sxi_cbdata_unref(&ret);
        return NULL;
    }

    /* Assign index to distinguish nodes during polling */
    ctx->idx = node_index;
    /* Assign query to free it later (its content may be used in async callbacks) */
    ctx->query = query;
    /* Set fail flag to 1 (failed), it will be assgined to 0 later */
    ctx->fail = 1;
    /* Add context to cbdata */
    sxi_cbdata_set_context(ret, ctx);

    if(sxi_cluster_query_ev(ret, sx_hashfs_conns(h), sx_node_internal_addr(n), REQ_PUT, query->path, query->content, query->content_len, NULL, NULL)) {
        WARN("Failed to push volume size to host %s: %s", sx_node_internal_addr(n), sxc_geterrmsg(sx_hashfs_client(h)));
        sxi_query_free(query);
        free(ctx);
        sxi_cbdata_unref(&ret);
        return NULL;
    }

    return ret;
}

static act_result_t replace_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxi_hdist_t *hdist = NULL;
    sx_nodelist_t *faulty = NULL;
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnode, nnodes, cfg_len;
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    sxi_query_t *proto = NULL;
    sx_blob_t *b = NULL;
    sxi_hostlist_t hlist;
    const void *cfg;
    int qret;
    rc_ty s;

    DEBUG("IN %s", __func__);
    if(!job_data) {
	NULLARG();
	action_set_fail(ACT_RESULT_PERMFAIL, 500, "Null job");
	return ret;
    }

    sxi_hostlist_init(&hlist);
    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    faulty = sx_nodelist_from_blob(b);
    if(!faulty || sx_blob_get_blob(b, &cfg, &cfg_len)) {
	WARN("Cannot retrrieve %s from job data for job %lld", faulty ? "new distribution":"faulty nodes", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad job data");
    }

    hdist = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!hdist) {
	WARN("Cannot load hdist config");
	s = ENOMEM;
	action_error(rc2actres(s), rc2http(s), msg_get_reason());
    }

    if(sxi_hdist_buildcnt(hdist) != 1) {
	WARN("Invalid distribution found (builds = %d)", sxi_hdist_buildcnt(hdist));
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    proto = sxi_distribution_proto_begin(sx, cfg, cfg_len);
    nnodes = sx_nodelist_count(faulty);
    for(nnode = 0; proto && nnode<nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(faulty, nnode);
	proto = sxi_distribution_proto_add_faulty(sx, proto, sx_node_uuid_str(node));
    }
    if(proto)
	proto = sxi_distribution_proto_end(sx, proto);
    if(!proto) {
	WARN("Cannot allocate proto for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode < nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);
	int is_replacement = sx_nodelist_lookup(faulty, sx_node_uuid(node)) != NULL;

	if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(node)))
	    action_error(ACT_RESULT_TEMPFAIL, 500, "Not enough memory to perform challenge request");

	if(is_replacement) {
	    if(!sx_node_cmp(me, node)) {
		WARN("This node cannot be both a distribution change initiator and a new node");
		action_error(ACT_RESULT_PERMFAIL, 500, "Something is really out of place");
	    }

	    /* Challenge new node */
	    ret = challenge_and_sync(hashfs, node, fail_code, fail_msg);
	    if(ret != ACT_RESULT_OK)
		goto action_failed;
	}

	if(sx_node_cmp(me, node)) {
	    qret = sxi_cluster_query(clust, &hlist, proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL, NULL);
	    if(qret != 200)
		action_error(http2actres(qret), qret, sxc_geterrmsg(sx));
	} else {
	    s = sx_hashfs_hdist_replace_add(hashfs, cfg, cfg_len, faulty);
	    if(s)
		action_set_fail(rc2actres(s), rc2http(s), msg_get_reason());
	}

	sxi_hostlist_empty(&hlist);
	succeeded[nnode] = 1;
    }

action_failed:
    sx_nodelist_delete(faulty);
    sx_blob_free(b);
    sxi_query_free(proto);
    sxi_hostlist_empty(&hlist);
    sxi_hdist_free(hdist);

    return ret;
}

static act_result_t replace_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    sx_nodelist_t *faulty = NULL;
    sxi_hdist_t *hdist = NULL;
    unsigned int cfg_len;
    sx_blob_t *b = NULL;
    const void *cfg;

    DEBUG("IN %s", __func__);
    if(!job_data) {
	NULLARG();
	action_set_fail(ACT_RESULT_PERMFAIL, 500, "Null job");
	return ret;
    }

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    faulty = sx_nodelist_from_blob(b);
    if(!faulty || sx_blob_get_blob(b, &cfg, &cfg_len)) {
	WARN("Cannot retrrieve %s from job data for job %lld", faulty ? "new distribution":"faulty nodes", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad job data");
    }

    hdist = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!hdist) {
	WARN("Cannot load hdist config");
	action_error(rc2actres(ENOMEM), rc2http(ENOMEM), msg_get_reason());
    }

    if(sxi_hdist_buildcnt(hdist) != 1) {
	WARN("Invalid distribution found (builds = %d)", sxi_hdist_buildcnt(hdist));
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    ret = commit_dist_common(hashfs, nodes, succeeded, fail_code, fail_msg);

action_failed:
    sx_nodelist_delete(faulty);
    sxi_hdist_free(hdist);
    sx_blob_free(b);
    return ret;
}

static act_result_t replace_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    sx_nodelist_t *faulty = NULL;
    sxi_hdist_t *hdist = NULL;
    unsigned int cfg_len;
    sx_blob_t *b = NULL;
    const void *cfg;

    DEBUG("IN %s", __func__);
    if(!job_data) {
	NULLARG();
	action_set_fail(ACT_RESULT_PERMFAIL, 500, "Null job");
	return ret;
    }

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    faulty = sx_nodelist_from_blob(b);
    if(!faulty || sx_blob_get_blob(b, &cfg, &cfg_len)) {
	WARN("Cannot retrrieve %s from job data for job %lld", faulty ? "new distribution":"faulty nodes", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad job data");
    }

    hdist = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!hdist) {
	WARN("Cannot load hdist config");
	action_error(rc2actres(ENOMEM), rc2http(ENOMEM), msg_get_reason());
    }

    if(sxi_hdist_buildcnt(hdist) != 1) {
	WARN("Invalid distribution found (builds = %d)", sxi_hdist_buildcnt(hdist));
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad distribution data");
    }

    ret = revoke_dist_common(hashfs, nodes, succeeded, fail_code, fail_msg);

action_failed:
    sx_nodelist_delete(faulty);
    sxi_hdist_free(hdist);
    sx_blob_free(b);
    return ret;
}

static void check_distribution(sx_hashfs_t *h) {
    int dc;

    dc = sx_hashfs_distcheck(h);
    if(dc < 0) {
        CRIT("Failed to reload distribution");
        return;
    }
    if(dc > 0)
        INFO("Distribution reloaded");
}

static act_result_t replace_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret;

    CRIT("The attempt to change the cluster distribution model (i.e. nodes) resulted in a fatal failure leaving it in an inconsistent state");
    action_set_fail(ACT_RESULT_PERMFAIL, 500, "The attempt to change the cluster distribution model (i.e. nodes) resulted in a fatal failure leaving it in an inconsistent state");
    return ret;
}


static act_result_t replaceblocks_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;

    DEBUG("IN %s", __func__);
    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    sx_hashfs_set_progress_info(hashfs, INPRG_REPLACE_RUNNING, "Building a list of objects to heal");

    if(sx_hashfs_init_replacement(hashfs))
	action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to initialize replacement");

    succeeded[0] = 1;

 action_failed:
    return ret;
}


enum replace_state { RPL_HDRSIZE = 0, RPL_HDRDATA, RPL_DATA, RPL_END };

struct rplblocks {
    sx_hashfs_t *hashfs;
    sx_blob_t *b;
    uint8_t block[SX_BS_LARGE];
    sx_block_meta_index_t lastgood;
    unsigned int pos, itemsz, ngood;
    enum replace_state state;
};

static int rplblocks_cb(curlev_context_t *cbdata, void *ctx, const void *data, size_t size) {
    struct rplblocks *c = (struct rplblocks *)ctx;
    uint8_t *input = (uint8_t *)data;
    unsigned int todo;

    while(size) {
	if(c->state == RPL_END) {
	    if(size)
		INFO("Spurious tail of %u bytes", (unsigned int)size);
	    return 0;
	}

	if(c->state == RPL_HDRSIZE) {
	    todo = MIN((sizeof(c->itemsz) - c->pos), size);
	    memcpy(c->block + c->pos, input, todo);
	    input += todo;
	    size -= todo;
	    c->pos += todo;
	    if(c->pos == sizeof(c->itemsz)) {
		memcpy(&todo, c->block, sizeof(todo));
		c->itemsz = htonl(todo);
		if(c->itemsz >= sizeof(c->block)) {
		    WARN("Invalid header size %u", c->itemsz);
		    return 1;
		}
		c->state = RPL_HDRDATA;
		c->pos = 0;
	    }
	}

	if(c->state == RPL_HDRDATA) {
	    todo = MIN((c->itemsz - c->pos), size);
	    memcpy(c->block + c->pos, input, todo);
	    input += todo;
	    size -= todo;
	    c->pos += todo;
	    if(c->pos == c->itemsz) {
		const char *signature;
		c->b = sx_blob_from_data(c->block, c->itemsz);
		if(!c->b) {
		    WARN("Cannot create blob of size %u", c->itemsz);
		    return 1;
		}
		if(sx_blob_get_string(c->b, &signature)) {
		    WARN("Cannot read create blob signature");
		    return 1;
		}
		if(!strcmp(signature, "$THEEND$")) {
		    if(size)
			INFO("Spurious tail of %u bytes", (unsigned int)size);
		    c->state = RPL_END;
		    return 0;
		}
		if(strcmp(signature, "$BLOCK$")) {
		    WARN("Invalid blob signature '%s'", signature);
		    return 1;
		}
		if(sx_blob_get_int32(c->b, &c->itemsz) ||
		   sx_hashfs_check_blocksize(c->itemsz)) {
		    WARN("Invalid block size");
		    return 1;
		}
		c->state = RPL_DATA;
		c->pos = 0;
	    }
	}

	if(c->state == RPL_DATA) {
	    todo = MIN((c->itemsz - c->pos), size);
	    memcpy(c->block + c->pos, input, todo);
	    input += todo;
	    size -= todo;
	    c->pos += todo;
	    if(c->pos == c->itemsz) {
		const sx_block_meta_index_t *bmi;
		unsigned int maxreplica = sx_nodelist_count(sx_hashfs_all_nodes(c->hashfs, NL_NEXT));
		sx_hash_t hash;
		const void *ptr;

		if(sx_blob_get_blob(c->b, &ptr, &todo) || todo != sizeof(hash)) {
		    WARN("Invalid block hash");
		    return 1;
		}
		memcpy(&hash, ptr, sizeof(hash));

		/* FIXME: do i hash the block and match it ? */

		if(sx_blob_get_blob(c->b, (const void **)&bmi, &todo) || todo != sizeof(*bmi)) {
		    WARN("Invalid block index");
		    return 1;
		}
		if(sx_blob_get_int32(c->b, &todo)) {
		    WARN("Invalid number of entries");
		    return 1;
		}

		while(todo--) {
                    sx_hash_t revision_id;
		    unsigned int replica;
                    int32_t op;
		    rc_ty s;
		    const void *ptr;
                    unsigned int blob_size;

		    if(sx_blob_get_blob(c->b, &ptr, &blob_size) ||
                       blob_size != sizeof(revision_id.b) ||
                       sx_blob_get_int32(c->b, &replica) ||
		       sx_blob_get_int32(c->b, &op)) {
			WARN("Invalid block size: %d", blob_size);
			return 1;
		    }
                    memcpy(&revision_id.b, ptr, sizeof(revision_id.b));

		    s = sx_hashfs_hashop_mod(c->hashfs, &hash, NULL, &revision_id, c->itemsz, replica, op, 0);
		    if(s != OK && s != ENOENT) {
			WARN("Failed to mod hash");
			return 1;
		    }
		}

		if(sx_hashfs_block_put(c->hashfs, c->block, c->itemsz, maxreplica, 0)) {
		    WARN("Failed to mod hash");
		    return 1;
		}
		c->lastgood = *bmi;
		sx_blob_free(c->b);
		c->b = NULL;
		c->ngood++;
		c->pos = 0;
		c->state = RPL_HDRSIZE;
	    }
	}
    }
    return 0;
}


static act_result_t replaceblocks_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    act_result_t ret = ACT_RESULT_TEMPFAIL;
    sx_block_meta_index_t bmidx;
    const sx_node_t *source;
    sxi_hostlist_t hlist;
    unsigned int dist;
    int have_blkidx;
    rc_ty s;

    DEBUG("IN %s", __func__);
    sxi_hostlist_init(&hlist);

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    sx_hashfs_set_progress_info(hashfs, INPRG_REPLACE_RUNNING, "Healing blocks");

    s = sx_hashfs_replace_getstartblock(hashfs, &dist, &source, &have_blkidx, (uint8_t *)&bmidx);
    if(s == OK) {
	sxi_conns_t *clust = sx_hashfs_conns(hashfs);
	const sx_node_t *me = sx_hashfs_self(hashfs);
	struct rplblocks *ctx = malloc(sizeof(*ctx));
	char query[256];
	int qret;

	if(!ctx)
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Out of memory");
	if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(source))) {
	    free(ctx);
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Out of memory");
	}

	if(have_blkidx) {
	    char hexidx[sizeof(bmidx)*2+1];
	    bin2hex(&bmidx, sizeof(bmidx), hexidx, sizeof(hexidx));
	    snprintf(query, sizeof(query), ".replblk?target=%s&dist=%u&idx=%s", sx_node_uuid_str(me), dist, hexidx);
	} else
	    snprintf(query, sizeof(query), ".replblk?target=%s&dist=%u", sx_node_uuid_str(me), dist);

	ctx->hashfs = hashfs;
	ctx->b = NULL;
	ctx->pos = 0;
	ctx->ngood = 0;
	ctx->state = RPL_HDRSIZE;

	qret = sxi_cluster_query(clust, &hlist, REQ_GET, query, NULL, 0, NULL, rplblocks_cb, ctx);
	sx_blob_free(ctx->b);
	if(qret != 200) {
	    free(ctx);
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Bad reply from node");
	}
	if(ctx->state == RPL_END) {
	    if(sx_hashfs_replace_setlastblock(hashfs, sx_node_uuid(source), NULL))
		WARN("Replace setnode failed");
	} else if(ctx->ngood) {
	    if(sx_hashfs_replace_setlastblock(hashfs, sx_node_uuid(source), (uint8_t *)&ctx->lastgood))
		WARN("Replace setnode failed");
	}
	free(ctx);
    } else if(s == ITER_NO_MORE) {
	succeeded[0] = 1;
	ret = ACT_RESULT_OK;
    }

 action_failed:
    sxi_hostlist_empty(&hlist);
    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}


struct rplfiles {
    sx_hashfs_t *hashfs;
    sx_blob_t *b;
    sx_hash_t hash;
    uint8_t hdr[1024 +
		  SXLIMIT_MAX_FILENAME_LEN +
		  REV_LEN +
		  ( 128 + SXLIMIT_META_MAX_KEY_LEN + SXLIMIT_META_MAX_VALUE_LEN ) * SXLIMIT_META_MAX_ITEMS];
    char volume[SXLIMIT_MAX_VOLNAME_LEN+1],
	file[SXLIMIT_MAX_FILENAME_LEN+1],
	rev[REV_LEN+1];
    unsigned int ngood, itemsz, pos, needend;
    enum replace_state state;
};

static int rplfiles_cb(curlev_context_t *cbdata, void *ctx, const void *data, size_t size) {
    struct rplfiles *c = (struct rplfiles *)ctx;
    uint8_t *input = (uint8_t *)data;
    unsigned int todo;
    rc_ty s;

    while(size) {
	if(c->state == RPL_END) {
	    if(size)
		INFO("Spurious tail of %u bytes", (unsigned int)size);
	    return 0;
	}

	if(c->state == RPL_HDRSIZE) {
	    todo = MIN((sizeof(c->itemsz) - c->pos), size);
	    memcpy(c->hdr + c->pos, input, todo);
	    input += todo;
	    size -= todo;
	    c->pos += todo;
	    if(c->pos == sizeof(c->itemsz)) {
		memcpy(&todo, c->hdr, sizeof(todo));
		c->itemsz = htonl(todo);
		if(c->itemsz >= sizeof(c->hdr)) {
		    WARN("Invalid header size %u", c->itemsz);
		    return 1;
		}
		c->state = RPL_HDRDATA;
		c->pos = 0;
	    }
	}

	if(c->state == RPL_HDRDATA) {
	    todo = MIN((c->itemsz - c->pos), size);
	    memcpy(c->hdr + c->pos, input, todo);
	    input += todo;
	    size -= todo;
	    c->pos += todo;
	    if(c->pos == c->itemsz) {
		const char *signature;
		c->b = sx_blob_from_data(c->hdr, c->itemsz);
		if(!c->b) {
		    WARN("Cannot create blob of size %u", c->itemsz);
		    return 1;
		}
		if(sx_blob_get_string(c->b, &signature)) {
		    WARN("Cannot read create blob signature");
		    return 1;
		}
		if(!strcmp(signature, "$THEEND$")) {
		    c->state = RPL_END;
		    if(size)
			INFO("Spurious tail of %u bytes", (unsigned int)size);
		    return 0;
		}
		if(strcmp(signature, "$FILE$")) {
		    WARN("Invalid blob signature '%s'", signature);
		    return 1;
		}
		if(sx_hashfs_createfile_begin(c->hashfs)) {
		    WARN("Invalid createfile_begin failed");
		    return 1;
		}
		c->needend = 1;
		if(sx_blob_get_int32(c->b, &c->itemsz)) {
		    WARN("Invalid block size");
		    return 1;
		}
		c->state = RPL_DATA;
		c->pos = 0;
	    }
	}

	if(c->state == RPL_DATA) {
	    if(c->itemsz) {
		todo = MIN((sizeof(c->hash) - c->pos), size);
		memcpy((uint8_t *)&c->hash + c->pos, input, todo);
		input += todo;
		size -= todo;
		c->pos += todo;
		if(c->pos == sizeof(c->hash)) {
		    if(sx_hashfs_putfile_putblock(c->hashfs, &c->hash)) {
			WARN("Failed to add block");
			return 1;
		    }
		    c->pos = 0;
		    c->itemsz--;
		}
	    }
	    if(!c->itemsz) {
		const char *file_name, *file_rev;
		int64_t file_size;
		if(sx_blob_get_string(c->b, &file_name) ||
		   sx_blob_get_string(c->b, &file_rev) ||
		   sx_blob_get_int64(c->b, &file_size)) {
		    WARN("Bad file characteristics");
		    return 1;
		}
		while(1) {
		    const char *signature, *key;
		    const void *val;
		    if(sx_blob_get_string(c->b, &signature)) {
			WARN("Bad file meta signature");
			return 1;
		    }
		    if(!strcmp(signature, "$ENDMETA$"))
			break;
		    if(strcmp(signature, "$META$") ||
		       sx_blob_get_string(c->b, &key) ||
		       sx_blob_get_blob(c->b, &val, &todo)) {
			WARN("Bad file meta");
			return 1;
		    }
		    if(sx_hashfs_putfile_putmeta(c->hashfs, key, val, todo)) {
			WARN("Failed to add file meta");
			return 1;
		    }
		}
		s = sx_hashfs_createfile_commit(c->hashfs, c->volume, file_name, file_rev, file_size);
		c->needend = 0;
		if(s) {
		    WARN("Failed to create file %s:%s", file_name, file_rev);
		    return 1;
		}
		c->ngood++;
		sxi_strlcpy(c->file, file_name, sizeof(c->file));
		sxi_strlcpy(c->rev, file_rev, sizeof(c->rev));
		sx_blob_free(c->b);
		c->b = NULL;
		c->state = RPL_HDRSIZE;
	    }
	}
    }
    return 0;
}

static act_result_t replacefiles_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    act_result_t ret = ACT_RESULT_TEMPFAIL;
    char maxrev[REV_LEN+1];
    sxi_hostlist_t hlist;
    struct rplfiles *ctx = NULL;
    rc_ty s;

    DEBUG("IN %s", __func__);
    sxi_hostlist_init(&hlist);

    if(job_data->len || sx_nodelist_count(nodes) != 1) {
	CRIT("Bad job data");
	action_error(ACT_RESULT_PERMFAIL, 500, "Internal job data error");
    }

    sx_hashfs_set_progress_info(hashfs, INPRG_REPLACE_RUNNING, "Healing files");

    ctx = malloc(sizeof(*ctx));
    if(!ctx)
	action_error(ACT_RESULT_TEMPFAIL, 503, "Out of memory allocating request context");

    while((s = sx_hashfs_replace_getstartfile(hashfs, maxrev, ctx->volume, ctx->file, ctx->rev)) == OK) {
	unsigned int nnode, nnodes, rndnode;
	const sx_hashfs_volume_t *vol;
	const sx_node_t *source;
	sx_nodelist_t *volnodes;

	s = sx_hashfs_volume_by_name(hashfs, ctx->volume, &vol);
	if(s == ENOENT) {
	    /* Volume is gone */
	    s = sx_hashfs_replace_setlastfile(hashfs, ctx->volume, NULL, NULL);
	    if(s == OK)
		continue;
	}
	if(s != OK)
	    action_error(rc2actres(s), rc2http(s), msg_get_reason());

	s = sx_hashfs_all_volnodes(hashfs, NL_NEXT, vol, 0, &volnodes, NULL);
	if(s != OK)
	    action_error(rc2actres(s), rc2http(s), msg_get_reason());

	nnodes = sx_nodelist_count(volnodes);
	rndnode = sxi_rand();
	for(nnode = 0; nnode < nnodes; nnode++) {
	    source = sx_nodelist_get(volnodes, (nnode + rndnode) % nnodes);
	    if(!sx_hashfs_is_node_faulty(hashfs, sx_node_uuid(source)))
		break;
	}
	if(nnode == nnodes) {
	    /* All volnodes are faulty */
	    s = sx_hashfs_replace_setlastfile(hashfs, ctx->volume, NULL, NULL);
	    sx_nodelist_delete(volnodes);
	    if(s == OK)
		continue; /* Pick next volume */
	    break; /* Retry later */
	}

	if(sxi_hostlist_add_host(sx, &hlist, sx_node_internal_addr(source))) {
	    sx_nodelist_delete(volnodes);
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Out of memory");
	}
	sx_nodelist_delete(volnodes);
	break; /* exit with s = OK and hlist set */
    }

    if(s == OK) {
	char *enc_vol = NULL, *enc_file = NULL, *enc_rev = NULL, *enc_maxrev = NULL, *query = NULL;
	sxi_conns_t *clust = sx_hashfs_conns(hashfs);
	int qret;

	enc_vol = sxi_urlencode(sx, ctx->volume, 0);
	enc_file = sxi_urlencode(sx, ctx->file, 0);
	enc_rev = sxi_urlencode(sx, ctx->rev, 0);
	enc_maxrev = sxi_urlencode(sx, maxrev, 0);

	if(enc_vol && enc_file && enc_rev && enc_maxrev) {
	    query = malloc(lenof(".replfl/") +
			   strlen(enc_vol) +
			   lenof("/") +
			   strlen(enc_file) +
			   lenof("?maxrev=") +
			   strlen(enc_maxrev) +
			   lenof("&startrev=") +
			   strlen(enc_rev) +
			   1);

	    if(query) {
		if(strlen(enc_file))
		    sprintf(query, ".replfl/%s/%s?maxrev=%s&startrev=%s", enc_vol, enc_file, enc_maxrev, enc_rev);
		else
		    sprintf(query, ".replfl/%s?maxrev=%s", enc_vol, enc_maxrev);
	    }
	}

	free(enc_vol);
	free(enc_file);
	free(enc_rev);
	free(enc_maxrev);

	if(!query)
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Out of memory allocating the request URL");

	ctx->hashfs = hashfs;
	ctx->b = NULL;
	ctx->pos = 0;
	ctx->ngood = 0;
	ctx->needend = 0;
	ctx->state = RPL_HDRSIZE;

	qret = sxi_cluster_query(clust, &hlist, REQ_GET, query, NULL, 0, NULL, rplfiles_cb, ctx);
	free(query);
	sx_blob_free(ctx->b);
	if(ctx->needend)
	    sx_hashfs_putfile_end(hashfs);
	if(qret != 200)
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Bad reply from node");
	if(ctx->state == RPL_END) {
	    if(sx_hashfs_replace_setlastfile(hashfs, ctx->volume, NULL, NULL))
		WARN("Replace setlastfile failed");
	    else
		INFO("Replacement of volume %s completed", ctx->volume);
	} else if(ctx->ngood) {
	    if(sx_hashfs_replace_setlastfile(hashfs, ctx->volume, ctx->file, ctx->rev))
		WARN("Replace setlastfile failed");
	}
    } else if(s == ITER_NO_MORE) {
	succeeded[0] = 1;
	ret = ACT_RESULT_OK;
    } else
	action_error(rc2actres(s), rc2http(s), msg_get_reason());


 action_failed:
    sxi_hostlist_empty(&hlist);
    free(ctx);
    if(ret == ACT_RESULT_PERMFAIL) {
	/* Since there is no way we can recover at this point we
	 * downgrade to temp failure and try to notify about the issue.
	 * There is no timeout anyway */
	CRIT("A critical condition has occoured (see messages above): please check the health and reachability of all cluster nodes");
	ret = ACT_RESULT_TEMPFAIL;
	*fail_code = 503;
    }
    return ret;
}

static act_result_t replacefiles_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const sx_nodelist_t *allnodes = sx_hashfs_all_nodes(hashfs, NL_NEXT);
    int64_t hdistver = sx_hashfs_hdist_getversion(hashfs);
    unsigned int i, nnodes = sx_nodelist_count(allnodes);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    const sx_uuid_t *myuuid = sx_node_uuid(me);
    query_list_t *qrylist = NULL;
    char query[128];
    rc_ty s;
    act_result_t ret = ACT_RESULT_OK;

    DEBUG("IN %s", __func__);

    if(!sx_hashfs_is_node_faulty(hashfs, myuuid)) {
	sx_hashfs_set_progress_info(hashfs, INPRG_IDLE, NULL);
	return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
    }

    sx_hashfs_set_progress_info(hashfs, INPRG_REPLACE_COMPLETE, "Healing complete");

    snprintf(query, sizeof(query), ".faulty/%s?dist=%lld", myuuid->string, (long long)hdistver); 
    qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
    if(!qrylist) {
	WARN("Cannot allocate result space");
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }
    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(allnodes, i);
	if(!sx_node_cmp(me, node))
	    continue;
	/* Remote nodes first */
	qrylist[i].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	if(sxi_cluster_query_ev(qrylist[i].cbdata, clust, sx_node_internal_addr(node), REQ_DELETE, query, NULL, 0, NULL, NULL)) {
	    WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
	    action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	}
	qrylist[i].query_sent = 1;
    }


 action_failed:
    if(qrylist) {
	for(i=0; i<nnodes; i++) {
	    if(qrylist[i].query_sent) {
                long http_status = 0;
		int rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(clust), &http_status);
		if(rc != -2) {
		    if(rc == -1) {
			WARN("Query failed with %ld", http_status);
			if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
			    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    } else if(http_status != 200 && http_status != 404) {
			act_result_t newret = http2actres(http_status);
			if(newret < ret) /* Severity shall only be raised */
			    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
		    }
		} else {
		    CRIT("Failed to wait for query");
		    action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		}
	    }
	}
        query_list_free(qrylist, nnodes);
    }

    if(ret == ACT_RESULT_OK) {
	/* Local node last */
	s = sx_hashfs_set_unfaulty(hashfs, myuuid, hdistver);
	if(s == OK || s == ENOENT) {
	    INFO(">>>>>>>>>>>> THIS NODE IS NOW A PROPER REPLACEMENT  <<<<<<<<<<<<");
	    succeeded[0] = 1;
	} else
	    action_set_fail(rc2actres(s), rc2http(s), msg_get_reason());
    }
    return ret;
}

static act_result_t ignodes_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    sx_nodelist_t *ignodes = NULL;
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnode, nnodes;
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    query_list_t *qrylist = NULL;
    char *query = NULL;
    sx_blob_t *b = NULL;
    rc_ty s;

    DEBUG("IN %s", __FUNCTION__);
    if(!job_data) {
	NULLARG();
	action_error(ACT_RESULT_PERMFAIL, 500, "Null job");
    }

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
	WARN("Cannot allocate blob for job %lld", (long long)job_id);
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    ignodes = sx_nodelist_from_blob(b);
    if(!ignodes) {
	WARN("Cannot retrrieve list of nodes from job data for job %lld", (long long)job_id);
	action_error(ACT_RESULT_PERMFAIL, 500, "Bad job data");
    }

    nnodes = sx_nodelist_count(nodes);
    qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
    if(!qrylist) {
	WARN("Cannot allocate result space");
	action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    for(nnode = 0; nnode < nnodes; nnode++) {
	const sx_node_t *node = sx_nodelist_get(nodes, nnode);
	if(!sx_node_cmp(me, node)) {
	    /* Local node */
	    if((s = sx_hashfs_setignored(hashfs, ignodes))) {
		WARN("Failed to mark faulty nodes for job %lld: %s", (long long)job_id, msg_get_reason());
		action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to enable volume");
	    }
	    succeeded[nnode] = 1;
	} else {
	    /* Remote node */
	    if(!query) {
		unsigned int i, nign = sx_nodelist_count(ignodes);
		char *eoq;
		query = malloc((UUID_STRING_SIZE+3) * nign + sizeof("{\"faultyNodes\":[]}"));
		if(!query)
		    action_error(ACT_RESULT_TEMPFAIL, 503, "Out of memory allocating the request");
		sprintf(query, "{\"faultyNodes\":[");
		eoq = query + lenof("{\"faultyNodes\":[");
		for(i=0; i<nign; i++) {
		    const sx_node_t *ignode = sx_nodelist_get(ignodes, i);
		    snprintf(eoq, UUID_STRING_SIZE+3+3, "\"%s\"%s", sx_node_uuid_str(ignode), i != nign-1 ? "," : "]}");
		    eoq += strlen(eoq);
		}
	    }
	    qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
	    if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), REQ_PUT, ".nodes?setfaulty", query, strlen(query), NULL, NULL)) {
		WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
		action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
	    }
	    qrylist[nnode].query_sent = 1;
	}
    }


 action_failed:
    sx_nodelist_delete(ignodes);
    sx_blob_free(b);
    if(query) {
	for(nnode=0; qrylist && nnode<nnodes; nnode++) {
	    int rc;
            long http_status = 0;
	    if(!qrylist[nnode].query_sent)
		continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
	    if(rc == -2) {
		CRIT("Failed to wait for query");
		action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
		continue;
	    }
	    if(rc == -1) {
		WARN("Query failed with %ld", http_status);
		if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
		    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    } else if(http_status == 200) {
		succeeded[nnode] = 1;
	    } else {
		act_result_t newret = http2actres(http_status);
		if(newret < ret) /* Severity shall only be raised */
		    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
	    }
	}
	free(query);
    }
    if (qrylist)
        query_list_free(qrylist, nnodes);

    return ret;
}

static act_result_t ignodes_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    DEBUG("IN %s", __FUNCTION__);
    if(!job_data) {
	NULLARG();
	action_set_fail(ACT_RESULT_PERMFAIL, 500, "Null job");
	return ret;
    }

    return commit_dist_common(hashfs, nodes, succeeded, fail_code, fail_msg);
}

static act_result_t dummy_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    DEBUG("IN %s", __func__);
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}
static act_result_t dummy_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    DEBUG("IN %s", __func__);
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}
static act_result_t dummy_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    DEBUG("IN %s", __func__);
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}
static act_result_t dummy_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    DEBUG("IN %s", __func__);
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

#define REVSCLEAN_ITER_LIMIT     64

static act_result_t revsclean_vol_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    const sx_hashfs_volume_t *vol = NULL;
    const sx_hashfs_file_t *file = NULL;
    const char *volume = NULL, *file_threshold = NULL;
    rc_ty s, t;
    unsigned int scheduled = 0;
    sx_blob_t *b;
    act_result_t ret = ACT_RESULT_OK;
    unsigned int nnodes;
    const sx_node_t *me;
    unsigned int my_index = 0;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
        WARN("Cannot allocate blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volume) ||
       sx_blob_get_string(b, &file_threshold)) {
        WARN("Cannot get job data from blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if((s = sx_hashfs_volume_by_name(hashfs, volume, &vol)) != OK) {
        WARN("Failed to get volume %s reference: %s", volume, msg_get_reason());
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: Invalid volume");
    }

    /* All volnodes that received this commit request should iterate over files and schedule delete jobs for the outdate revs */
    for(s = sx_hashfs_list_first(hashfs, vol, NULL, &file, 1, file_threshold, 0); s == OK; s = sx_hashfs_list_next(hashfs)) {
        unsigned int scheduled_per_file = 0;

        if((t = sx_hashfs_delete_old_revs(hashfs, vol, file->name+1, &scheduled_per_file)) != OK) {
            WARN("Failed to schedule deletes for file %s", file->name);
            /* This will not break the job itself and let it retry deleting old revisions */
            action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to schedule outdated revisions deletion");
        }

        scheduled += scheduled_per_file;
        if(scheduled >= REVSCLEAN_ITER_LIMIT) {
            DEBUG("Reached revisions cleaning limit: %u", scheduled);
            break;
        }
    }

    if(s != ITER_NO_MORE && s != OK) {
        WARN("Iteration failed: %s", msg_get_reason());
        action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to iterate over files");
    }

    /* Job is scheduled only on local node */
    nnodes = sx_nodelist_count(nodes);
    if(nnodes != 1) {
        WARN("Revsclean job scheduled to more than one (local) node, or nodes list is empty");
        action_error(ACT_RESULT_PERMFAIL, 500, "Revsclean job scheduled to more than one (local) node");
    }

    me = sx_hashfs_self(hashfs);
    if(!me) {
        WARN("Failed to get self node reference");
        action_error(ACT_RESULT_PERMFAIL, 500, "Failed to get node reference");
    }

    if(s == ITER_NO_MORE) {
        succeeded[my_index] = 1;
    } else { /* s == OK */
        sx_blob_t *new_blob;
        job_t job;
        const void *new_job_data;
        unsigned int job_datalen;
        sx_nodelist_t *curnode_list;
        int job_timeout = 20;

        if(!(new_blob = sx_blob_new()))
            action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");

        /* Insert volume name and last cleaned up file name */
        if(sx_blob_add_string(new_blob, volume) || sx_blob_add_string(new_blob, file->name)) {
            sx_blob_free(new_blob);
            action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
        }

        sx_blob_to_data(new_blob, &new_job_data, &job_datalen);
        curnode_list = sx_nodelist_new();
        if(!curnode_list) {
            WARN("Failed to allocate nodeslist");
            sx_blob_free(new_blob);
            action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
        }
        if(sx_nodelist_add(curnode_list, sx_node_dup(me))) {
            WARN("Failed to add myself to nodelist");
            sx_blob_free(new_blob);
            sx_nodelist_delete(curnode_list);
            action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
        }
        s = sx_hashfs_job_new(hashfs, 0, &job, JOBTYPE_REVSCLEAN, job_timeout, volume, new_job_data, job_datalen, curnode_list);
        sx_blob_free(new_blob);
        sx_nodelist_delete(curnode_list);
        if(s != OK)
            action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to create next job");
        succeeded[my_index] = 1;
    }

action_failed:
    sx_blob_free(b);
    return ret;
}

/* At least some warnings should be printed when revsclean job fails */
static act_result_t revsclean_vol_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    WARN("Failed to finish backgroud revsclean job");
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t revsclean_vol_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    WARN("Failed to finish backgroud revsclean job");
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}


/* Update cbdata array with new context and send volsizes query to given node */
static rc_ty finalize_query(sx_hashfs_t *h, curlev_context_t ***cbdata, unsigned int *ncbdata, const sx_node_t *n, unsigned int node_index, sxi_query_t *query) {
    curlev_context_t *ctx;
    curlev_context_t **newptr;
    rc_ty ret = FAIL_EINTERNAL;

    if(!h || !cbdata || !ncbdata || !n || !query) {
        NULLARG();
        sxi_query_free(query);
        goto finalize_query_err;
    }

    if(!(query = sxi_volsizes_proto_end(sx_hashfs_client(h), query))) {
        WARN("Failed to close query proto");
        goto finalize_query_err;
    }

    newptr = realloc(*cbdata, sizeof(curlev_context_t*) * (*ncbdata + 1));
    if(!newptr) {
        WARN("Failed to allocate memory for next cbdata");
        sxi_query_free(query);
        goto finalize_query_err;
    }
    *cbdata = newptr;

    ctx = push_volume_sizes(h, n, node_index, query);
    if(!ctx) {
        WARN("Failed to push volume sizes to node %s: Failed to send query", sx_node_addr(n));
        /* Allocation of cbdata succeeded, so this pointer should be returned to handle the rest of queries,
         * but we do not want to increase a counter and set a NULL pointer */
        goto finalize_query_err;
    }

    ret = OK;
finalize_query_err:
    /* Add new cbdata context to array */
    if(ret == OK) {
        (*cbdata)[*ncbdata] = ctx;
        (*ncbdata)++;
    }
    return ret;
}

#define VOLSIZES_PUSH_INTERVAL 10.0
#define VOLSIZES_VOLS_PER_QUERY 128

static rc_ty checkpoint_volume_sizes(sx_hashfs_t *h) {
    rc_ty ret = FAIL_EINTERNAL;
    const sx_nodelist_t *nodes;
    unsigned int i;
    const sx_node_t *me;
    struct timeval now;
    sxc_client_t *sx = sx_hashfs_client(h);
    curlev_context_t **cbdata = NULL;
    unsigned int ncbdata = 0;
    unsigned int nnodes;
    unsigned int fail;

    /* Reload hashfs */
    check_distribution(h);

    me = sx_hashfs_self(h);

    /* If storage is bare, won't push volume size changes*/
    if(sx_storage_is_bare(h))
        return OK;

    /* Check if its time to push volume sizes */
    gettimeofday(&now, NULL);
    if(sxi_timediff(&now, sx_hashfs_volsizes_timestamp(h)) < VOLSIZES_PUSH_INTERVAL)
        return OK;
    memcpy(sx_hashfs_volsizes_timestamp(h), &now, sizeof(now));

    nodes = sx_hashfs_effective_nodes(h, NL_PREVNEXT);
    if(!nodes) {
        WARN("Failed to get node list");
        goto checkpoint_volume_sizes_err;
    }
    nnodes = sx_nodelist_count(nodes);

    /* Iterate over all nodes */
    for(i = 0; i < nnodes; i++) {
        int64_t last_push_time;
        int s;
        int required = 0;
        sxi_query_t *query = NULL;
        const sx_node_t *n = sx_nodelist_get(nodes, i);
        const sx_hashfs_volume_t *vol = NULL;
        int j;

        if(!n) {
            WARN("Failed to get node at index %d", i);
            goto checkpoint_volume_sizes_err;
        }

        if(!sx_node_cmp(me, n)) {
            /* Skipping myself... */
            continue;
        }

        /* Get last push time */
        last_push_time = sx_hashfs_get_node_push_time(h, n);
        if(last_push_time < 0) {
            WARN("Failed to get last push time for node %s", sx_node_addr(n));
            goto checkpoint_volume_sizes_err;
        }

        for(s = sx_hashfs_volume_first(h, &vol, 0); s == OK; s = sx_hashfs_volume_next(h)) {
            /* Check if node n is not a volnode for volume and it is this node's volume */
            if(sx_hashfs_is_volume_to_push(h, vol, n)) {
                /* Check if its about time to push current volume size */
                if((!last_push_time && vol->changed) || last_push_time <= vol->changed) {
                    if(!query) {
                        query = sxi_volsizes_proto_begin(sx);
                        if(!query) {
                            WARN("Failed to prepare query for pushing volume size");
                            goto checkpoint_volume_sizes_err;
                        }
                    }

                    if(!(query = sxi_volsizes_proto_add_volume(sx, query, vol->name, vol->cursize))) {
                        WARN("Failed to append volume to the query string");
                        goto checkpoint_volume_sizes_err;
                    }

                    /* Increase number of required volumes */
                    required++;
                    /* Check if number of volumes is not too big, we should avoid too long json */
                    if(required >= VOLSIZES_VOLS_PER_QUERY && finalize_query(h, &cbdata, &ncbdata, n, i, query)) {
                        WARN("Failed to finalize and send query");
                        goto checkpoint_volume_sizes_err;
                    }
                }
            }
        }

        if(required && finalize_query(h, &cbdata, &ncbdata, n, i, query)) {
            WARN("Failed to finalize and send query");
            goto checkpoint_volume_sizes_err;
        }

        if(s != ITER_NO_MORE) {
            WARN("Failed to list volumes");
            goto checkpoint_volume_sizes_err;
        }

        /* All volumes were checked for current node, set fail flag to 0 for it */
        for(j = ncbdata-1; j >= 0; j--) {
            struct volsizes_push_ctx *ctx = sxi_cbdata_get_context(cbdata[j]);

            if(i == ctx->idx)
                ctx->fail = 0;
            else
                break; /* Index is different, stop iteration because we reach different node */
        }
    }

    ret = OK;
checkpoint_volume_sizes_err:
    /* First wait for all queries to finish */
    for(i = 0; i < ncbdata; i++) {
        struct volsizes_push_ctx *ctx;
        long status = -1;
        ctx = sxi_cbdata_get_context(cbdata[i]);

        if(sxi_cbdata_wait(cbdata[i], sxi_conns_get_curlev(sx_hashfs_conns(h)), &status)) {
            WARN("Failed to wait for query to finish: %s", sxi_cbdata_geterrmsg(cbdata[i]));
            ctx->fail = 1;
            ret = FAIL_EINTERNAL;
        } else if(status != 200) {
            WARN("Volume size update query failed: %s", sxi_cbdata_geterrmsg(cbdata[i]));
            ctx->fail = 1;
            ret = FAIL_EINTERNAL;
        }
    }

    /* Second, Update node push time if all queries for particular node succeeded */
    fail = 0;
    for(i = 0; i < ncbdata; i++) {
        struct volsizes_push_ctx *ctx = sxi_cbdata_get_context(cbdata[i]);

        if(i > 0) {
            struct volsizes_push_ctx *prevctx = sxi_cbdata_get_context(cbdata[i-1]);

            if(ctx->idx != prevctx->idx) { /* Node has changed, check for fail and update push time */
                const sx_node_t *n = sx_nodelist_get(nodes, prevctx->idx);

                if(n && !fail && sx_hashfs_update_node_push_time(h, n)) {
                    WARN("Failed to update node push time");
                    ret = FAIL_EINTERNAL;
                    break;
                }
                fail = 0;
            }
        }

        if(ctx->fail)
            fail = 1;
    }

    /* Handle last node */
    if(ncbdata && i == ncbdata && !fail) {
        struct volsizes_push_ctx *ctx = sxi_cbdata_get_context(cbdata[ncbdata-1]);
        const sx_node_t *n = sx_nodelist_get(nodes, ctx->idx);

        if(sx_hashfs_update_node_push_time(h, n)) {
            WARN("Failed to update node push time");
            ret = FAIL_EINTERNAL;
        }
    }

    /* Third, cleanup */
    for(i = 0; i < ncbdata; i++) {
        struct volsizes_push_ctx *ctx = sxi_cbdata_get_context(cbdata[i]);
        if(ctx) {
            sxi_query_free(ctx->query);
            free(ctx);
        }
        sxi_cbdata_unref(&cbdata[i]);
    }

    free(cbdata);
    return ret;
}

static rc_ty volmod_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return job_twophase_execute(&volmod_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty volmod_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return job_twophase_execute(&volmod_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty volmod_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return job_twophase_execute(&volmod_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t distlock_common(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, jobphase_t phase, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    unsigned int nnode, nnodes;
    sx_blob_t *b = NULL;
    act_result_t ret = ACT_RESULT_OK;
    sxi_query_t *proto = NULL;
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    query_list_t *qrylist = NULL;
    const char *lockid = NULL;
    int32_t op = 0;
    rc_ty s;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
        WARN("Cannot allocate blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &lockid) || sx_blob_get_int32(b, &op)) {
        WARN("Cannot get data from blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if(!lockid) {
        WARN("Cannot get data from blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if(phase == JOBPHASE_ABORT)
        op = !op; /* Revert operation in case of abort */

    nnodes = sx_nodelist_count(nodes);
    for(nnode = 0; nnode<nnodes; nnode++) {
        const sx_node_t *node = sx_nodelist_get(nodes, nnode);

        if(sx_node_cmp(me, node)) {
            if(!proto) {
                proto = sxi_distlock_proto(sx, op, lockid);
                if(!proto) {
                    WARN("Cannot allocate proto for job %lld", (long long)job_id);
                    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
                }

                qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
                if(!qrylist) {
                    WARN("Cannot allocate result space");
                    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
                }
            }

            qrylist[nnode].cbdata = sxi_cbdata_create_generic(clust, NULL, NULL);
            if(sxi_cluster_query_ev(qrylist[nnode].cbdata, clust, sx_node_internal_addr(node), proto->verb, proto->path, proto->content, proto->content_len, NULL, NULL)) {
                WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
                action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
            }
            qrylist[nnode].query_sent = 1;
        } else if(phase == JOBPHASE_REQUEST) {
            succeeded[nnode] = 1; /* Locally mark as succeeded */
        } else { /* ABORT phase on local node, revert previously set distlock */
            /* op variable was previously inverted */
            if(op) { /* Lock operation */
                s = sx_hashfs_distlock_acquire(hashfs, lockid);
                if(s != OK && s != EEXIST) { /* EEXIST is not an error when we want to revert the lock */
                    WARN("Failed to acquire lock %s", lockid);
                    action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to acquire distribution lock");
                }
            } else { /* Unlock operation */
                s = sx_hashfs_distlock_release(hashfs);
                if(s != OK) {
                    WARN("Failed to release lock %s", lockid);
                    action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to release distribution lock");
                }
            }
        }
    }

 action_failed:
    sx_blob_free(b);
    if(proto) {
        for(nnode=0; qrylist && nnode<nnodes; nnode++) {
            int rc;
            long http_status = 0;
            if(!qrylist[nnode].query_sent)
                continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
            if(rc == -2) {
                CRIT("Failed to wait for query");
                action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
                continue;
            }
            if(rc == -1) {
                WARN("Query failed with %ld", http_status);
                if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
                    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
            } else if(http_status == 200) {
                succeeded[nnode] = 1;
            } else {
                act_result_t newret = http2actres(http_status);
                if(newret < ret) /* Severity shall only be raised */
                    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
            }
        }
        query_list_free(qrylist, nnodes);
        sxi_query_free(proto);
    }
    return ret;
}

static act_result_t distlock_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return distlock_common(hashfs, job_id, job_data, nodes, JOBPHASE_REQUEST, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t distlock_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
   return distlock_common(hashfs, job_id, job_data, nodes, JOBPHASE_ABORT, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty revision_commit(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return job_twophase_execute(&revision_spec, JOBPHASE_COMMIT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty revision_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return job_twophase_execute(&revision_spec, JOBPHASE_UNDO, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty revision_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    return job_twophase_execute(&revision_spec, JOBPHASE_ABORT, hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static act_result_t upgrade_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    INFO("Preparing to upgrade node");
    if (sx_hashfs_upgrade_1_0_or_1_1_prepare(hashfs) ||
        sx_hashfs_upgrade_1_0_or_1_1_local(hashfs))
        action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to upgrade local node");
    ret = force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
 action_failed:
    return ret;
}

static rc_ty jobpoll_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s;
    sx_blob_t *b = NULL;
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sxi_conns_t *clust = sx_hashfs_conns(hashfs);
    query_list_t *qrylist = NULL;
    unsigned int nnode, nnodes;
    sxi_job_t **jobs = NULL;
    nnodes = sx_nodelist_count(nodes);
    const sx_node_t *me = sx_hashfs_self(hashfs);
    int r;
    sx_uuid_t uuid;
    const char *jobid = NULL;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
        WARN("Cannot allocate blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    /* This is a master job, poll status of children jobs */
    jobs = wrap_calloc(nnodes, sizeof(*jobs));
    if(!jobs) {
        WARN("Cannot allocate memory for jobs to poll");
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    qrylist = wrap_calloc(nnodes, sizeof(*qrylist));
    if(!qrylist) {
        WARN("Cannot allocate result space");
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    while(!(r = sx_blob_get_string(b, &jobid))) {
        job_status_t status;
        const char *message = NULL;
        job_t job;
        const sx_node_t *node = NULL;
        unsigned int uuid_len;
        char uuid_str[UUID_STRING_SIZE+1];
        const char *p;
        char *enumb;

        if(!jobid || !(p = strchr(jobid, ':'))) {
            WARN("Corrupted node list for job %s", jobid);
            action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: invalid node list");
        }

        uuid_len = p - jobid;
        if(uuid_len != UUID_STRING_SIZE) {
            WARN("Corrupted node list for job %s", jobid);
            action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: invalid node list");
        }
        memcpy(uuid_str, jobid, UUID_STRING_SIZE);
        uuid_str[UUID_STRING_SIZE] = '\0';
        uuid_from_string(&uuid, uuid_str);
        job = strtoll(p + 1, &enumb, 10);
        if(enumb && *enumb) {
            WARN("Corrupted node list for job %s", jobid);
            action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: invalid node list");
        }

        node = sx_nodelist_lookup_index(nodes, &uuid, &nnode);
        if(!node)
            continue; /* Node has been removed because it has failed or succeeded */

        if(!sx_node_cmp(node, me)) {
            if((s = sx_hashfs_job_result(hashfs, job, 0, &status, &message)) != OK) {
                WARN("Failed to check job %lld status: %s", (long long)job, msg_get_reason());
                action_error(ACT_RESULT_PERMFAIL, rc2http(s), "Failed to check job status");
            }
            if(status == JOB_OK)
                succeeded[nnode] = 1;
            else if(status == JOB_PENDING)
                action_error(ACT_RESULT_TEMPFAIL, 503, "Local job is pending");
            else {
                WARN("Local job has failed: status: %d, message: %s", status, message);
                action_error(ACT_RESULT_PERMFAIL, rc2http(s), message);
            }
        } else {
            jobs[nnode] = sxi_job_new(sx_hashfs_conns(hashfs), jobid, REQ_DELETE, sx_node_internal_addr(node));
            if(!jobs[nnode]) {
                WARN("Cannot allocate memory for job %lld to poll", (long long)job);
                action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
            }

            qrylist[nnode].cbdata = sxi_job_cbdata(jobs[nnode]);
            if(sxi_job_query_ev(sx_hashfs_conns(hashfs), jobs[nnode], NULL)) {
                WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx));
                action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
            }
            qrylist[nnode].query_sent = 1;
        }
    }

    if(r < 0) {
        WARN("Cannot get data from blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    ret = ACT_RESULT_OK;
action_failed:
    if(qrylist && jobs) {
        for(nnode=0; nnode<nnodes; nnode++) {
            int rc;
            long http_status = 0;
            if(!qrylist[nnode].query_sent)
                continue;
            rc = sxi_cbdata_wait(qrylist[nnode].cbdata, sxi_conns_get_curlev(clust), &http_status);
            if(rc == -2) {
                CRIT("Failed to wait for query");
                action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
                continue;
            }
            if(rc == -1) {
                WARN("Query failed with %ld", http_status);
                if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
                    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
            } else if (http_status == 200) {
                sxi_job_status_t status = sxi_job_status(jobs[nnode]);

                if(status == JOBST_OK)
                    succeeded[nnode] = 1;
                else if(status == JOBST_PENDING)
                    action_set_fail(ACT_RESULT_TEMPFAIL, 503, "Remote job is pending");
                else
                    action_set_fail(ACT_RESULT_PERMFAIL, 500, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
            } else {
                act_result_t newret = http2actres(http_status);
                if(newret < ret) /* Severity shall only be raised */
                    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[nnode].cbdata));
            }
        }
        /* cbdata stored in qrylist is owned by jobs array, therefore the query_list_free() is not used here */
        free(qrylist);
    }
    if(jobs) {
        for(nnode = 0; nnode < nnodes; nnode++)
            sxi_job_free(jobs[nnode]);
        free(jobs);
    }
    sx_blob_free(b);
    return ret;
}

static rc_ty jobpoll_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    CRIT("Some files could be left in an inconsistent state after a failed polling job %lld", (long long)job_id);
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty jobpoll_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    CRIT("Some files could be left in an inconsistent state after a failed polling job %lld", (long long)job_id);
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static int timeval2str(const struct timeval *tv, char *buff) {
    struct tm *t;

    if(!tv || !buff)
        return -1;

    t = gmtime(&tv->tv_sec);
    if(strftime(buff, REV_TIME_LEN, "%Y-%m-%d %H:%M:%S", t) != REV_TIME_LEN - 4)
        return -1;
    snprintf(buff + REV_TIME_LEN - 4, 5, ".%03lld", (long long)tv->tv_usec / 1000);
    return 0;
}

static rc_ty effective_non_volnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hashfs_volume_t *volume, sx_nodelist_t **nodes) {
    rc_ty s;
    sx_nodelist_t *volnodes = NULL;
    const sx_nodelist_t *allnodes;
    sx_nodelist_t *ret;
    unsigned int nnode, nnodes;

    if(!volume || !nodes) {
        msg_set_reason("NULL argument");
        return FAIL_EINTERNAL;
    }

    if((s = sx_hashfs_effective_volnodes(h, which, volume, 0, &volnodes, NULL)) != OK)
        return s;
    allnodes = sx_hashfs_effective_nodes(h, which);
    if(!allnodes) {
        sx_nodelist_delete(volnodes);
        return s;
    }
    
    ret = sx_nodelist_new();
    if(!ret) {
        sx_nodelist_delete(volnodes);
        return FAIL_EINTERNAL;
    }

    nnodes = sx_nodelist_count(allnodes);
    for(nnode = 0; nnode < nnodes; nnode++) {
        const sx_node_t *n = sx_nodelist_get(allnodes, nnode);
        if(!n) {
            sx_nodelist_delete(volnodes);
            sx_nodelist_delete(ret);
            return FAIL_EINTERNAL;
        }
        if(!sx_nodelist_lookup(volnodes, sx_node_uuid(n))) {
            if(sx_nodelist_add(ret, sx_node_dup(n))) {
                sx_nodelist_delete(volnodes);
                sx_nodelist_delete(ret);
                return FAIL_EINTERNAL;
            }
        }
    }

    sx_nodelist_delete(volnodes);
    *nodes = ret;
    return OK;
}

#define MAX_BATCH_ITER  2048
static rc_ty massdelete_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s;
    struct timeval timestamp;
    sx_blob_t *b;
    const char *volname = NULL, *pattern = NULL;
    const sx_hashfs_volume_t *vol = NULL;
    const sx_hashfs_file_t *file = NULL;
    unsigned int i = 0;
    int recursive = 0;
    char timestamp_str[REV_TIME_LEN+1];
    sxi_query_t *query = NULL;
    query_list_t *qrylist = NULL;
    unsigned int qrylist_len = 0, queries_sent = 0;
    sx_nodelist_t *nonvolnodes = NULL;
    unsigned int nnodes, nnode;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
        WARN("Cannot allocate blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname) || sx_blob_get_int32(b, &recursive) ||
       sx_blob_get_string(b, &pattern) || sx_blob_get_datetime(b, &timestamp)) {
        WARN("Cannot get data from blob for job %lld: %s %s", (long long)job_id, volname, pattern);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if(!volname || !pattern) {
        WARN("Cannot get data from blob for job %lld: NULL volname or pattern", (long long)job_id);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if(timeval2str(&timestamp, timestamp_str)) {
        WARN("Cannot parse timestamp");
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if((s = sx_hashfs_volume_by_name(hashfs, volname, &vol))) {
        WARN("Failed to load volume %s", volname);
        action_error(rc2actres(s), rc2http(s), msg_get_reason());
    }

    if((s = effective_non_volnodes(hashfs, NL_NEXTPREV, vol, &nonvolnodes)) != OK) {
        WARN("Failed to get non-volnodes list");
        action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to get non-volnodes list");
    }
    nnodes = sx_nodelist_count(nonvolnodes);

    if(nnodes) {   
        /* When non file has more revisions than volume limit, then this should be enouch to supply worst case */
        qrylist_len = MAX_BATCH_ITER * nnodes + vol->effective_replica - 1;
        qrylist = wrap_calloc(qrylist_len, sizeof(*qrylist));
        if(!qrylist) {
            WARN("Cannot allocate result space");
            action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
        }
    }

    /* Perform operations */
    for(s = sx_hashfs_list_first(hashfs, vol, pattern, &file, recursive, NULL, 0); s == OK && i < MAX_BATCH_ITER; s = sx_hashfs_list_next(hashfs)) {
        rc_ty t;
        const sx_hashfs_file_t *filerev = NULL;
        char name[SXLIMIT_MAX_FILENAME_LEN+2];
        sxi_strlcpy(name, file->name, sizeof(name));

        for(t = sx_hashfs_revision_first(hashfs, vol, name+1, &filerev, 0); t == OK; t = sx_hashfs_revision_next(hashfs, 0)) {
            rc_ty u;
            if(strncmp(filerev->revision, timestamp_str, REV_TIME_LEN) > 0) {
                DEBUG("Skipping %s: %.*s > %s", filerev->name, (int)REV_TIME_LEN, filerev->revision, timestamp_str);
                continue;
            }

            if(nnodes) {           
                /* We can reach memory limit when temporary over-replica situation happens, need to reallocate qrylist array */
                if(queries_sent + nnodes >= qrylist_len) {
                    query_list_t *oldptr = qrylist;
                    qrylist_len *= 2;
                    DEBUG("Reallocating queries list");
                    qrylist = wrap_realloc(oldptr, qrylist_len * sizeof(*oldptr));
                    if(!qrylist) {
                        WARN("Failed to reallocate queries list array");
                        qrylist = oldptr;
                        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
                    }

                    /* Reset newly allocated entries to avoid using uninits in query_list_free() */
                    memset(qrylist + queries_sent, 0, (qrylist_len - queries_sent) * sizeof(*qrylist));
                }
            }

            /* Delete the revision */
            if((u = sx_hashfs_file_delete(hashfs, vol, filerev->name, filerev->revision)) != OK) {
                WARN("Failed to delete file revision %s: %s", filerev->revision, msg_get_reason());
                t = u;
                break;
            }

            /* File is deleted, we can unbump the revision */
            if((u = sx_hashfs_revision_op(hashfs, filerev->block_size, &filerev->revision_id, -1)) != OK) {
                WARN("Failed to unbump file revision");
                t = u;
                break;
            }

            for(nnode = 0; nnode < nnodes; nnode++) {
                const sx_node_t *node = sx_nodelist_get(nonvolnodes, nnode);
                sxi_query_free(query);
                query = sxi_hashop_proto_revision(sx_hashfs_client(hashfs), filerev->block_size, &filerev->revision_id, -1);
                if(!query) {
                    WARN("Cannot allocate query");
                    action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
                }

                qrylist[queries_sent].cbdata = sxi_cbdata_create_generic(sx_hashfs_conns(hashfs), NULL, NULL);
                if(sxi_cluster_query_ev(qrylist[queries_sent].cbdata, sx_hashfs_conns(hashfs), sx_node_internal_addr(node), query->verb, query->path, NULL, 0, NULL, NULL)) {
                    WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(hashfs)));
                    action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to setup cluster communication");
                }
                qrylist[queries_sent].query_sent = 1;
                queries_sent++;
            }

            i++;
        }

        if(t != ITER_NO_MORE && t != ENOENT) {
            WARN("Failed to delete all revisions of file %s: %s", file->name, msg_get_reason());
            s = t;
            break;
        }
    }
    if(s != ITER_NO_MORE) {
        if(i >= MAX_BATCH_ITER) {
            DEBUG("Sleeping job due to exceeded deletions limit");
            action_error(ACT_RESULT_TEMPFAIL, 503, "Exceeded limit");
        } else {
            WARN("Failed to finish batch job: %s", rc2str(s));
            action_error(rc2actres(s), rc2http(s), rc2str(s));
        }
    }
    /* Job is done */
    ret = ACT_RESULT_OK;
action_failed:
    sx_blob_free(b);
    sx_nodelist_delete(nonvolnodes);
    if(qrylist) {
        for(i = 0; i < queries_sent; i++) {
            int rc;
            long http_status = 0;
            if(!qrylist[i].query_sent)
                continue;
            rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(sx_hashfs_conns(hashfs)), &http_status);
            if(rc == -2) {
                CRIT("Failed to wait for query");
                action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
                continue;
            }
            if(rc == -1) {
                WARN("Query failed with %ld", http_status);
                if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
                    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
            } else if (http_status != 200 && http_status != 410 && http_status != 404) {
                act_result_t newret = http2actres(http_status);
                if(newret < ret) /* Severity shall only be raised */
                    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
            }
        }
        query_list_free(qrylist, qrylist_len);
    }
    sxi_query_free(query);

    if(ret == ACT_RESULT_OK)
        succeeded[0] = 1;
    return ret;
}

static rc_ty massdelete_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    CRIT("Some files were left in an inconsistent state after a failed deletion attempt");
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty massdelete_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    CRIT("Some files were left in an inconsistent state after a failed deletion attempt");
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

/* Drop all stale source revisions */
static rc_ty massrename_drop_old_src_revs(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const char *filename, sx_nodelist_t *nodes, unsigned int *queries_sent, unsigned int *list_len, query_list_t **list) {
    rc_ty ret = FAIL_EINTERNAL, t;
    const sx_hashfs_file_t *filerev = NULL;
    query_list_t *qrylist;
    unsigned int nnodes, nnode;
    sxi_query_t *query = NULL;

    if(!vol || !filename || !nodes || !list || !queries_sent || !list_len) {
        NULLARG();
        return EINVAL;
    }

    qrylist = *list;
    nnodes = sx_nodelist_count(nodes);
    if(nnodes && !qrylist) {
        /* When file has no more revisions than volume limit, then this should be enough to supply worst case */
        *list_len = MAX_BATCH_ITER * nnodes + vol->effective_replica - 1;
        qrylist = wrap_calloc(*list_len, sizeof(*qrylist));
        if(!qrylist) {
            WARN("Cannot allocate result space");
            msg_set_reason("Not enough memory to perform the requested action");
            ret = ENOMEM;
            goto massrename_drop_old_src_revs_err;
        }
    }

    /* Iterate through all older revisions of the file and drop them */
    for(t = sx_hashfs_revision_first(h, vol, filename, &filerev, 0); t == OK; t = sx_hashfs_revision_next(h, 0)) {
        rc_ty u;

        if(nnodes) {
            /* We can reach memory limit when temporary over-replica situation happens, need to reallocate qrylist array */
            if(*queries_sent + nnodes >= *list_len) {
                INFO("Reallocating queries list %d -> %d", *list_len, *list_len * 2);
                query_list_t *oldptr = qrylist;
                DEBUG("Reallocating queries list");
                qrylist = wrap_realloc(oldptr, 2 * (*list_len) * sizeof(*oldptr));
                if(!qrylist) {
                    WARN("Failed to reallocate queries list array");
                    qrylist = oldptr;
                    msg_set_reason("Not enough memory to perform the requested action");
                    ret = ENOMEM;
                    goto massrename_drop_old_src_revs_err;
                }
                *list_len *= 2;

                /* Reset newly allocated entries to avoid using uninits in query_list_free() */
                memset(qrylist + *queries_sent, 0, (*list_len - *queries_sent) * sizeof(*qrylist));
            }
        }

        /* Delete the revision */
        if((u = sx_hashfs_file_delete(h, vol, filerev->name, filerev->revision)) != OK) {
            WARN("Failed to delete file revision %s: %s", filerev->revision, msg_get_reason());
            t = u;
            break;
        }

        /* File is deleted, we can unbump the revision */
        if((u = sx_hashfs_revision_op(h, filerev->block_size, &filerev->revision_id, -1)) != OK) {
            WARN("Failed to unbump file revision");
            t = u;
            break;
        }

        for(nnode = 0; nnode < nnodes; nnode++) {
            const sx_node_t *node = sx_nodelist_get(nodes, nnode);
            sxi_query_free(query);
            query = sxi_hashop_proto_revision(sx_hashfs_client(h), filerev->block_size, &filerev->revision_id, -1);
            if(!query) {
                WARN("Cannot allocate query");
                msg_set_reason("Not enough memory to perform the requested action");
                ret = ENOMEM;
                goto massrename_drop_old_src_revs_err;
            }

            qrylist[*queries_sent].cbdata = sxi_cbdata_create_generic(sx_hashfs_conns(h), NULL, NULL);
            if(sxi_cluster_query_ev(qrylist[*queries_sent].cbdata, sx_hashfs_conns(h), sx_node_internal_addr(node), query->verb, query->path, NULL, 0, NULL, NULL)) {
                WARN("Failed to query node %s: %s", sx_node_uuid_str(node), sxc_geterrmsg(sx_hashfs_client(h)));
                msg_set_reason("Failed to setup cluster communication");
                ret = EAGAIN;
                goto massrename_drop_old_src_revs_err;
            }
            qrylist[*queries_sent].query_sent = 1;
            (*queries_sent)++;
        }
    }

    if(t != ITER_NO_MORE && t != ENOENT) {
        WARN("Failed to delete all revisions of file %s: %s", filename, msg_get_reason());
        ret = t;
        goto massrename_drop_old_src_revs_err;
    }

    ret = OK;
massrename_drop_old_src_revs_err:
    sxi_query_free(query);
    *list = qrylist;
    return ret;
}

static rc_ty massrename_request(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    act_result_t ret = ACT_RESULT_OK;
    rc_ty s;
    struct timeval timestamp;
    sx_blob_t *b;
    const char *volname = NULL, *source = NULL, *dest = NULL;
    const sx_hashfs_volume_t *vol = NULL;
    const sx_hashfs_file_t *file = NULL;
    unsigned int dlen, slen, plen;
    int recursive = 0; /* TODO: Not used, but passed by a generic mass jobs scheduler */
    char timestamp_str[REV_TIME_LEN+1];
    char newname[SXLIMIT_MAX_FILENAME_LEN+1];
    const char *suffix;
    query_list_t *qrylist = NULL;
    unsigned int qrylist_len = 0, queries_sent = 0;
    sx_nodelist_t *nonvolnodes = NULL;

    b = sx_blob_from_data(job_data->ptr, job_data->len);
    if(!b) {
        WARN("Cannot allocate blob for job %lld", (long long)job_id);
        action_error(ACT_RESULT_TEMPFAIL, 503, "Not enough memory to perform the requested action");
    }

    if(sx_blob_get_string(b, &volname) || sx_blob_get_int32(b, &recursive) ||
       sx_blob_get_string(b, &source) || sx_blob_get_datetime(b, &timestamp) ||
       sx_blob_get_string(b, &dest)) {
        WARN("Cannot get data from blob for job %lld: %s %s", (long long)job_id, volname, source);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if(!volname || !source || !dest) {
        WARN("Cannot get data from blob for job %lld: NULL volname or pattern", (long long)job_id);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if(timeval2str(&timestamp, timestamp_str)) {
        WARN("Cannot parse timestamp");
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: data corruption detected");
    }

    if((s = sx_hashfs_volume_by_name(hashfs, volname, &vol))) {
        WARN("Failed to load volume %s", volname);
        action_error(rc2actres(s), rc2http(s), msg_get_reason());
    }

    if((s = effective_non_volnodes(hashfs, NL_NEXTPREV, vol, &nonvolnodes)) != OK) {
        WARN("Failed to get non-volnodes list");
        action_error(ACT_RESULT_TEMPFAIL, 503, "Failed to get non-volnodes list");
    }

    slen = strlen(source);
    /* Check if source points to the root of the volume and handle it */
    if(slen == 1 && *source == '/') {
        slen = 0;
        source = "";
    }
    dlen = strlen(dest);

    if(!dlen) {
        WARN("Destination %s is empty", dest);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: Invalid argument");
    }

    if((!slen || source[slen-1] == '/') && dest[dlen-1] != '/') {
        WARN("Destination %s is not a directory", dest);
        action_error(ACT_RESULT_PERMFAIL, 500, "Internal error: Invalid argument");
    }

    sxi_strlcpy(newname, dest, sizeof(newname));

    suffix = strrchr(source, '/');
    if(suffix) /* prefix len is until suffix only */
        plen = suffix - source + 1;
    else /* Source does not contain slashes, whole source will be prefixed with dest */
        plen = 0;

    if(slen && source[slen-1] != '/') {
        sx_hashfs_file_t f;

        /* F2F, only rename source to dest */
        if(dest[dlen-1] == '/') {
            /* Check if appending new name suffix to the destination prefix won't exceed filename limit */
            if(strlen(source) - plen + dlen > SXLIMIT_MAX_FILENAME_LEN)
                action_error(ACT_RESULT_PERMFAIL, 500, "Filename too long");

            /* Destination is a directory, append source without the prefix */
            sxi_strlcpy(newname + dlen, source + plen, sizeof(newname) - dlen);
        }

        DEBUG("[F2F] Renaming file %s to %s", source, newname);
        if((s = sx_hashfs_getfile_begin(hashfs, vol->name, source, NULL, &f, NULL))) {
            sx_hashfs_getfile_end(hashfs);
            if(s == ENOENT)
                action_error(ACT_RESULT_PERMFAIL, 404, "Not found");
            else
                action_error(rc2actres(s), rc2http(s), rc2str(s));
        }

        sx_hashfs_getfile_end(hashfs);
        if((s = sx_hashfs_file_rename(hashfs, vol, source, f.revision, newname)) != OK) {
            if(s != ENOENT)
                WARN("Failed to rename file %s to %s: %s", source, newname, msg_get_reason());
            action_error(rc2actres(s), rc2http(s), msg_get_reason());
        }

        if((s = massrename_drop_old_src_revs(hashfs, vol, source, nonvolnodes, &queries_sent, &qrylist_len, &qrylist)) != OK) {
            INFO("Failed to drop old %s revisions: %s", source, msg_get_reason());
            if(s == ENOMEM || s == EAGAIN)
                action_error(ACT_RESULT_TEMPFAIL, 503, msg_get_reason());
            else
                action_error(rc2actres(s), rc2http(s), "Failed to remove old source revisions");
        }

        /* Success */
        goto action_failed;
    }

    /* Iterate over files - happens only when source is a directory (ends with slash) */
    for(s = sx_hashfs_list_first(hashfs, vol, source, &file, 1, NULL, 1); s == OK && queries_sent < MAX_BATCH_ITER; s = sx_hashfs_list_next(hashfs)) {
        rc_ty t;
        const sx_hashfs_file_t *filerev = NULL;
        char name[SXLIMIT_MAX_FILENAME_LEN+1];

        /* +1 because of preceding slash */
        sxi_strlcpy(name, file->name + 1, sizeof(name));

        /* Check if appending new name suffix to the destination prefix won't exceed filename limit */
        if(strlen(name) - plen + dlen > SXLIMIT_MAX_FILENAME_LEN) {
            msg_set_reason("Filename too long");
            s = ENAMETOOLONG;
            break;
        }

        /* Destination is a directory, append source without the prefix */
        sxi_strlcpy(newname + dlen, name + plen, sizeof(newname) - dlen);

        DEBUG("[D2D] Renaming file %s to %s", name, newname);
        if(strncmp(file->revision, timestamp_str, REV_TIME_LEN) > 0) {
            DEBUG("Skipping %s: %.*s > %s", name, (int)REV_TIME_LEN, filerev->revision, timestamp_str);
            continue;
        }

        /* Rename the youngest revision */
        if((t = sx_hashfs_file_rename(hashfs, vol, name, file->revision, newname)) != OK) {
            WARN("Failed to rename file revision %s: %s", file->revision, msg_get_reason());
            s = t;
            break;
        }

        if((t = massrename_drop_old_src_revs(hashfs, vol, name, nonvolnodes, &queries_sent, &qrylist_len, &qrylist)) != OK) {
            WARN("Failed to drop old %s revisions: %s", name, msg_get_reason());
            if(t == ENOMEM || t == EAGAIN)
                action_error(ACT_RESULT_TEMPFAIL, 503, msg_get_reason());
            else
                action_error(rc2actres(t), rc2http(t), "Failed to remove old source revisions");
        }
    }

    if(s != ITER_NO_MORE) {
        if(queries_sent >= MAX_BATCH_ITER) {
            DEBUG("Sleeping job due to exceeded deletions limit");
            action_error(ACT_RESULT_TEMPFAIL, 503, "Exceeded limit");
        } else {
            WARN("Failed to finish batch job: %s", rc2str(s));
            action_error(rc2actres(s), rc2http(s), rc2str(s));
        }
    }
    /* Job is done */
    ret = ACT_RESULT_OK;
action_failed:
    sx_blob_free(b);
    sx_nodelist_delete(nonvolnodes);
    if(qrylist) {
        unsigned int i;
        for(i = 0; i < queries_sent; i++) {
            int rc;
            long http_status = 0;
            if(!qrylist[i].query_sent)
                continue;
            rc = sxi_cbdata_wait(qrylist[i].cbdata, sxi_conns_get_curlev(sx_hashfs_conns(hashfs)), &http_status);
            if(rc == -2) {
                CRIT("Failed to wait for query");
                action_set_fail(ACT_RESULT_PERMFAIL, 500, "Internal error in cluster communication");
                continue;
            }
            if(rc == -1) {
                WARN("Query failed with %ld", http_status);
                if(ret > ACT_RESULT_TEMPFAIL) /* Only raise OK to TEMP */
                    action_set_fail(ACT_RESULT_TEMPFAIL, 503, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
            } else if (http_status != 200 && http_status != 410 && http_status != 404) {
                act_result_t newret = http2actres(http_status);
                if(newret < ret) /* Severity shall only be raised */
                    action_set_fail(newret, http_status, sxi_cbdata_geterrmsg(qrylist[i].cbdata));
            }
        }
        query_list_free(qrylist, qrylist_len);
    }

    if(ret == ACT_RESULT_OK)
        succeeded[0] = 1;
    return ret;
}

static rc_ty massrename_abort(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    CRIT("Some files were left in an inconsistent state after a failed rename attempt");
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

static rc_ty massrename_undo(sx_hashfs_t *hashfs, job_t job_id, job_data_t *job_data, const sx_nodelist_t *nodes, int *succeeded, int *fail_code, char *fail_msg, int *adjust_ttl) {
    CRIT("Some files were left in an inconsistent state after a failed rename attempt");
    return force_phase_success(hashfs, job_id, job_data, nodes, succeeded, fail_code, fail_msg, adjust_ttl);
}

/* TODO: upgrade from 1.0-style flush and delete jobs */
static struct {
    job_action_t fn_request;
    job_action_t fn_commit;
    job_action_t fn_abort;
    job_action_t fn_undo;
} actions[] = {
    { createvol_request, createvol_commit, createvol_abort_and_undo, createvol_abort_and_undo }, /* JOBTYPE_CREATE_VOLUME */
    { createuser_request, createuser_commit, createuser_abort, createuser_undo }, /* JOBTYPE_CREATE_USER */
    { acl_request, acl_commit, acl_abort, acl_undo }, /* JOBTYPE_VOLUME_ACL */
    { replicateblocks_request, replicateblocks_commit, replicateblocks_abort, replicateblocks_abort }, /* JOBTYPE_REPLICATE_BLOCKS */
    { force_phase_success, fileflush_remote, replicateblocks_abort, fileflush_remote_undo }, /* JOBTYPE_FLUSH_FILE_REMOTE */
    { filedelete_request, filedelete_commit, filedelete_abort, filedelete_undo }, /* JOBTYPE_DELETE_FILE */
    { distribution_request, distribution_commit, distribution_abort, distribution_undo }, /* JOBTYPE_DISTRIBUTION */
    { startrebalance_request, force_phase_success, force_phase_success, force_phase_success }, /* JOBTYPE_STARTREBALANCE */
    { finishrebalance_request, finishrebalance_commit, force_phase_success, force_phase_success }, /* JOBTYPE_FINISHREBALANCE */
    { jlock_request, force_phase_success, jlock_abort_and_undo, jlock_abort_and_undo }, /* JOBTYPE_JLOCK */
    { blockrb_request, blockrb_commit, force_phase_success, force_phase_success }, /* JOBTYPE_REBALANCE_BLOCKS */
    { filerb_request, filerb_commit, force_phase_success, force_phase_success }, /* JOBTYPE_REBALANCE_FILES */
    { cleanrb_request, cleanrb_commit, force_phase_success, force_phase_success }, /* JOBTYPE_REBALANCE_CLEANUP */
    { deleteuser_request, deleteuser_commit, deleteuser_abort, deleteuser_undo }, /* JOBTYPE_DELETE_USER */
    { deletevol_request, deletevol_commit, deletevol_abort, deletevol_undo }, /* JOBTYPE_DELETE_VOLUME */
    { force_phase_success, usermodify_commit, usermodify_abort, usermodify_undo }, /* JOBTYPE_MODIFY_USER */
    { force_phase_success, volmod_commit, volmod_abort, volmod_undo }, /* JOBTYPE_MODIFY_VOLUME */
    { replace_request, replace_commit, replace_abort, replace_undo }, /* JOBTYPE_REPLACE */
    { replaceblocks_request, replaceblocks_commit, force_phase_success, force_phase_success }, /* JOBTYPE_REPLACE_BLOCKS */
    { replacefiles_request, replacefiles_commit, force_phase_success, force_phase_success }, /* JOBTYPE_REPLACE_FILES */
    { dummy_request, dummy_commit, dummy_abort, dummy_undo }, /* JOBTYPE_DUMMY */
    { force_phase_success, revsclean_vol_commit, revsclean_vol_abort, revsclean_vol_undo }, /* JOBTYPE_REVSCLEAN */
    { distlock_request, force_phase_success, distlock_abort, force_phase_success }, /* JOBTYPE_DISTLOCK */
    { cluster_mode_request, force_phase_success, cluster_mode_abort, force_phase_success }, /* JOBTYPE_CLUSTER_MODE */
    { ignodes_request, ignodes_commit, force_phase_success, force_phase_success }, /* JOBTYPE_IGNODES */
    { force_phase_success, revision_commit, revision_abort, revision_undo }, /* JOBTYPE_BLOCKS_REVISION */
    { force_phase_success, fileflush_local, fileflush_remote_undo, force_phase_success }, /* JOBTYPE_FLUSH_FILE_LOCAL  - 1 node */
    { upgrade_request, force_phase_success, force_phase_success, force_phase_success }, /* JOBTYPE_UPGRADE_FROM_1_0_OR_1_1 */
    { jobpoll_request, force_phase_success, jobpoll_abort, jobpoll_undo }, /* JOBTYPE_JOBPOLL */
    { massdelete_request, force_phase_success, massdelete_abort, massdelete_undo }, /* JOBTYPE_MASSDELETE */
    { massrename_request, force_phase_success, massrename_abort, massrename_undo }, /* JOBTYPE_MASSRENAME */
    { force_phase_success, cluster_setmeta_commit, cluster_setmeta_abort, cluster_setmeta_undo }, /* JOBTYPE_CLUSTER_SETMETA */
};


static job_data_t *make_jobdata(const void *data, unsigned int data_len, uint64_t op_expires_at) {
    job_data_t *ret;

    if(!data && data_len)
	return NULL;
    if(!(ret = wrap_malloc(sizeof(*ret) + data_len)))
	return NULL;
    ret->ptr = (void *)(ret+1);
    ret->len = data_len;
    ret->op_expires_at = op_expires_at;
    if(data_len)
	memcpy(ret->ptr, data, data_len);
    return ret;
}

static int terminate = 0;
static void sighandler(int signum) {
    if (signum == SIGHUP || signum == SIGUSR1) {
	log_reopen();
	return;
    }
    terminate = 1;
}

#define JOB_PHASE_REQUEST 0
#define JOB_PHASE_COMMIT 1
#define JOB_PHASE_DONE 2
#define JOB_PHASE_FAIL 3

#define BATCH_ACT_NUM 64

struct jobmgr_data_t {
    /* The following items are filled in once by jobmgr() */
    sx_hashfs_t *hashfs;
    sxi_db_t *eventdb;
    sqlite3_stmt *qjob;
    sqlite3_stmt *qact;
    sqlite3_stmt *qfail_children;
    sqlite3_stmt *qfail_parent;
    sqlite3_stmt *qcpl;
    sqlite3_stmt *qphs;
    sqlite3_stmt *qdly;
    sqlite3_stmt *qlfe;
    sqlite3_stmt *qvbump;
    time_t next_vcheck;

    /* The following items are filled in by:
     * jobmgr_process_queue(): sets job_id, job_type, job_expired, job_failed, job_data (from the db)
     * jobmgr_run_job(): job_failed gets updated if job fails or expires
     * jobmgr_get_actions_batch(): sets act_phase and nacts then fills the targets and act_ids arrays
     * jobmgr_execute_actions_batch(): fail_reason in case of failure
     */
    sx_nodelist_t *targets;
    job_data_t *job_data;
    int64_t act_ids[BATCH_ACT_NUM];
    int job_expired, job_failed;
    job_t job_id;
    jobtype_t job_type;
    unsigned int nacts;
    int act_phase;
    int adjust_ttl;
    char fail_reason[JOB_FAIL_REASON_SIZE];
};


static act_result_t jobmgr_execute_actions_batch(int *http_status, struct jobmgr_data_t *q) {
    act_result_t act_res = ACT_RESULT_UNSET;
    unsigned int nacts = q->nacts;
    int act_succeeded[BATCH_ACT_NUM];

    memset(act_succeeded, 0, sizeof(act_succeeded));
    *http_status = 0;
    q->fail_reason[0] = '\0';

    if(q->job_failed) {
	if(q->act_phase == JOB_PHASE_REQUEST) {
	    /* Nothing to (un)do */
	    act_res = ACT_RESULT_OK;
	} else if(q->act_phase == JOB_PHASE_COMMIT) {
	    act_res = actions[q->job_type].fn_abort(q->hashfs, q->job_id, q->job_data, q->targets, act_succeeded, http_status, q->fail_reason, &q->adjust_ttl);
	} else { /* act_phase == JOB_PHASE_DONE */
	    act_res = actions[q->job_type].fn_undo(q->hashfs, q->job_id, q->job_data, q->targets, act_succeeded, http_status, q->fail_reason, &q->adjust_ttl);
	}
        if(act_res == ACT_RESULT_TEMPFAIL && q->job_expired) {
            CRIT("Some undo action expired for job %lld.", (long long)q->job_id);
            act_res = ACT_RESULT_OK;
        } else if(act_res == ACT_RESULT_PERMFAIL) {
	    CRIT("Some undo action permanently failed for job %lld.", (long long)q->job_id);
	    act_res = ACT_RESULT_OK;
	}
    } else {
	if(q->act_phase == JOB_PHASE_REQUEST) {
	    act_res = actions[q->job_type].fn_request(q->hashfs, q->job_id, q->job_data, q->targets, act_succeeded, http_status, q->fail_reason, &q->adjust_ttl);
	} else { /* act_phase == JOB_PHASE_COMMIT */
	    act_res = actions[q->job_type].fn_commit(q->hashfs, q->job_id, q->job_data, q->targets, act_succeeded, http_status, q->fail_reason, &q->adjust_ttl);
	}
    }
    if(act_res != ACT_RESULT_OK && act_res != ACT_RESULT_TEMPFAIL && act_res != ACT_RESULT_PERMFAIL) {
	WARN("Unknown action return code %d: changing to PERMFAIL", act_res);
	act_res = ACT_RESULT_PERMFAIL;
    }

    while(nacts--) { /* Bump phase of successful actions */
	if(act_succeeded[nacts] || (q->job_failed && act_res == ACT_RESULT_OK)) {
	    if(qbind_int64(q->qphs, ":act", q->act_ids[nacts]) ||
	       qbind_int(q->qphs, ":phase", q->job_failed ? JOB_PHASE_FAIL : q->act_phase + 1) ||
	       qstep_noret(q->qphs))
		WARN("Cannot advance action phase for %lld.%lld", (long long)q->job_id, (long long)q->act_ids[nacts]);
	    else
		DEBUG("Action %lld advanced to phase %d", (long long)q->act_ids[nacts], q->job_failed ? JOB_PHASE_FAIL : q->act_phase + 1);
	}
    }

    return act_res;
}

static int jobmgr_get_actions_batch(struct jobmgr_data_t *q) {
    const sx_node_t *me = sx_hashfs_self(q->hashfs);
    unsigned int nacts;
    int r;

    q->nacts = 0;

    if(qbind_int64(q->qact, ":job", q->job_id) ||
       qbind_int(q->qact, ":maxphase", q->job_failed ? JOB_PHASE_FAIL : JOB_PHASE_DONE)) {
	WARN("Cannot lookup actions for job %lld", (long long)q->job_id);
	return -1;
    }
    r = qstep(q->qact);
    if(r == SQLITE_DONE) {
	if(qbind_int64(q->qcpl, ":job", q->job_id) ||
	   qstep_noret(q->qcpl))
	    WARN("Cannot set job %lld to complete", (long long)q->job_id);
	else
	    DEBUG("No actions for job %lld", (long long)q->job_id);
	return 1; /* Job completed */
    } else if(r == SQLITE_ROW)
	q->act_phase = sqlite3_column_int(q->qact, 1); /* Define the current batch phase */

    for(nacts=0; nacts<BATCH_ACT_NUM; nacts++) {
	sx_node_t *target;
	int64_t act_id;
	sx_uuid_t uuid;
	const void *ptr;
	unsigned int plen;
	rc_ty rc;

	if(r == SQLITE_DONE)
	    break; /* set batch_size and return success */

	if(r != SQLITE_ROW) {
	    WARN("Failed to retrieve actions for job %lld", (long long)q->job_id);
	    return -1;
	}
	if(sqlite3_column_int(q->qact, 1) != q->act_phase)
	    break; /* set batch_size and return success */

	act_id = sqlite3_column_int64(q->qact, 0);
	ptr = sqlite3_column_blob(q->qact, 2);
	plen = sqlite3_column_bytes(q->qact, 2);
	if(plen != sizeof(uuid.binary)) {
	    WARN("Bad action target for job %lld.%lld", (long long)q->job_id, (long long)act_id);
	    sqlite3_reset(q->qact);
	    return -1;
	}
	uuid_from_binary(&uuid, ptr);
	/* node = sx_nodelist_lookup(sx_hashfs_nodelist(q->hashfs, NL_NEXTPREV), &uuid); */
	/* if(!node) */
	    target = sx_node_new(&uuid, sqlite3_column_text(q->qact, 3), sqlite3_column_text(q->qact, 4), sqlite3_column_int64(q->qact, 5));
	/* else */
	/*     target = sx_node_dup(node); */
	if(!sx_node_cmp(me, target)) {
	    rc = sx_nodelist_prepend(q->targets, target);
	    if(nacts)
		memmove(&q->act_ids[1], &q->act_ids[0], nacts * sizeof(act_id));
	    q->act_ids[0] = act_id;
	} else {
	    rc = sx_nodelist_add(q->targets, target);
	    q->act_ids[nacts] = act_id;
	}
	if(rc != OK) {
	    WARN("Cannot add action target");
	    sqlite3_reset(q->qact);
	    return -1;
	}
	DEBUG("Action %lld (phase %d, target %s) loaded", (long long)act_id, q->act_phase, uuid.string);
	r = qstep(q->qact);
    }

    sqlite3_reset(q->qact);
    q->nacts = nacts;
    return 0;
}


static int set_job_failed(struct jobmgr_data_t *q, int result, const char *reason) {
    if(qbegin(q->eventdb)) {
	CRIT("Cannot set job %lld to failed: cannot start transaction", (long long)q->job_id);
	return -1;
    }

    if(qbind_int64(q->qfail_children, ":job", q->job_id) ||
       qbind_int(q->qfail_children, ":res", result) ||
       qbind_text(q->qfail_children, ":reason", reason) ||
       qstep_noret(q->qfail_children))
	goto setfailed_error;


    if(qbind_int64(q->qfail_parent, ":job", q->job_id) ||
       qbind_int(q->qfail_parent, ":res", result) ||
       qbind_text(q->qfail_parent, ":reason", reason) ||
       qstep_noret(q->qfail_parent))
	goto setfailed_error;

    if(qcommit(q->eventdb))
	goto setfailed_error;

    return 0;

 setfailed_error:
    CRIT("Cannot mark job %lld (and children) as failed", (long long)q->job_id);
    qrollback(q->eventdb);
    return -1;
}

static rc_ty adjust_job_ttl(struct jobmgr_data_t *q) {
    if(!q)
        return EINVAL;
    if(q->adjust_ttl) {
        char lifeadj[24];

        sqlite3_reset(q->qlfe);
        snprintf(lifeadj, sizeof(lifeadj), "%d seconds", q->adjust_ttl);
        if(qbind_int64(q->qlfe, ":job", q->job_id) ||
           qbind_text(q->qlfe, ":ttldiff", lifeadj) ||
           qstep_noret(q->qlfe)) {
            return FAIL_EINTERNAL;
        } else
            DEBUG("Lifetime of job %lld adjusted by %s", (long long)q->job_id, lifeadj);
    }
    return OK;
}

static rc_ty get_failed_job_expiration_ttl(struct jobmgr_data_t *q) {
    int64_t fsize;
    if(!q)
        return EINVAL;

    /* Handle blocks replication and file delete jobs using sx_hashfs_job_file_timeout() */
    if(q->job_type == JOBTYPE_REPLICATE_BLOCKS) {
	sx_hashfs_tmpinfo_t *tmpinfo;
        int64_t tmpfile_id;
        rc_ty s;

        if(!q->job_data || !q->job_data->ptr || q->job_data->len != sizeof(tmpfile_id))
            return FAIL_EINTERNAL;

        memcpy(&tmpfile_id, q->job_data->ptr, q->job_data->len);

        /* JOBTYPE_REPLICATE_BLOCKS contains tempfile ID as job data. Use it to get tempfile entry. */
        if((s = sx_hashfs_tmp_getinfo(q->hashfs, tmpfile_id, &tmpinfo, 0)) != OK)
            return s;
	fsize = tmpinfo->file_size;
        free(tmpinfo);
    } else if(q->job_type == JOBTYPE_DELETE_FILE) {
	sx_hashfs_file_t revinfo;
        rc_ty s;
        char rev[REV_LEN+1];

        if(!q->job_data || !q->job_data->ptr || q->job_data->len != REV_LEN)
            return FAIL_EINTERNAL;

        /* Need to nul terminate string */
        memcpy(rev, q->job_data->ptr, REV_LEN);
        rev[REV_LEN] = '\0';
        /* JOBTYPE_DELETE_FILE contains revision as job data. Use it to get tempfile entry. */
        if((s = sx_hashfs_getinfo_by_revision(q->hashfs, rev, &revinfo)) != OK) {
            /* File could be deleted already, set size to 0 but do not fail and let job manager to finish */
            if(s == ENOENT)
                fsize = 0;
            else
                return s;
        }
	fsize = revinfo.file_size;
    }

    if(q->job_type == JOBTYPE_REPLICATE_BLOCKS || q->job_type == JOBTYPE_DELETE_FILE) {
        q->adjust_ttl = sx_hashfs_job_file_timeout(q->hashfs, sx_nodelist_count(q->targets), fsize);
        return OK;
    }

    /* Default timeout, common for all jobs besides the two above */
    q->adjust_ttl = JOBMGR_UNDO_TIMEOUT * sx_nodelist_count(q->targets);
    if(!q->adjust_ttl) /* in case sx_nodelist_count() returns 0 */
        q->adjust_ttl = JOBMGR_UNDO_TIMEOUT;

    return OK;
}

static void jobmgr_run_job(struct jobmgr_data_t *q) {
    int r;

    /* Reload distribution */
    check_distribution(q->hashfs);
    if(q->job_expired && !q->job_failed) {
	/* FIXME: we could keep a trace of the reason of the last delay
	 * which is stored in db in case of tempfail.
	 * Of limited use but maybe nice to have */
	if(set_job_failed(q, 500, "Cluster timeout"))
	    return;
	q->job_failed = 1;
        /* Bump expiration time for abort/undo actions */
        if(get_failed_job_expiration_ttl(q) != OK) {
            WARN("Failed to determine expiration time for failed job %lld", (long long)q->job_id);
            q->adjust_ttl = JOBMGR_UNDO_TIMEOUT;
        }
        DEBUG("Job %lld is now expired, bumping expiration time with %d seconds", (long long)q->job_id, q->adjust_ttl);
        q->job_expired = 0;

        if(adjust_job_ttl(q) != OK)
            WARN("Cannot adjust lifetime of expired job %lld", (long long)q->job_id);
    }

    while(!terminate) {
	act_result_t act_res;
	int http_status;

	/* Collect a batch of actions but just for the current phase */
	sx_nodelist_empty(q->targets);
	r = jobmgr_get_actions_batch(q);
	if(r > 0) /* Job complete */
	    break;
	if(r < 0) { /* Error getting actions */
	    WARN("Failed to collect actions for job %lld", (long long)q->job_id);
	    break;
	}

	/* Execute actions */
	q->adjust_ttl = 0;
	act_res = jobmgr_execute_actions_batch(&http_status, q);

        if(adjust_job_ttl(q) != OK)
            WARN("Cannot adjust lifetime of job %lld", (long long)q->job_id);

	/* Temporary failure: mark job as to-be-retried and stop processing it for now */
	if(act_res == ACT_RESULT_TEMPFAIL) {
	    if(qbind_int64(q->qdly, ":job", q->job_id) ||
	       qbind_text(q->qdly, ":reason", q->fail_reason[0] ? q->fail_reason : "Unknown delay reason") ||
	       qbind_text(q->qdly, ":delay",
                          (q->job_type == JOBTYPE_FLUSH_FILE_REMOTE ||
                           q->job_type == JOBTYPE_FLUSH_FILE_LOCAL ||
                           q->job_type == JOBTYPE_REPLICATE_BLOCKS) ?
                          STRIFY(JOBMGR_DELAY_MIN) " seconds" : STRIFY(JOBMGR_DELAY_MAX) " seconds") ||
	       qstep_noret(q->qdly))
		CRIT("Cannot reschedule job %lld (you are gonna see this again!)", (long long)q->job_id);
	    else
		DEBUG("Job %lld will be retried later", (long long)q->job_id);
	    break;
	}

	/* Permanent failure: mark job as failed and go on (with cleanup actions) */
	if(act_res == ACT_RESULT_PERMFAIL) {
	    const char *fail_reason = q->fail_reason[0] ? q->fail_reason : "Unknown failure";
            if (!http_status)
                WARN("Job failed but didn't set fail code, missing action_set_fail/action_error call?");
	    if(set_job_failed(q, http_status, fail_reason))
		break;
	    DEBUG("Job %lld failed: %s", (long long)q->job_id, fail_reason);
	    q->job_failed = 1;
	}

	/* Success: go on with the next batch */
    }

    sx_nodelist_empty(q->targets); /* Explicit free, just in case */
}

static uint16_t to_u16(const uint8_t *ptr) {
    return ((uint16_t)ptr[0] << 8) | ptr[1];
}

#define DNS_QUESTION_SECT 0
#define DNS_ANSWER_SECT 1
#define DNS_SERVERS_SECT 2
#define DNS_EXTRA_SECT 3
#define DNS_MAX_SECTS 4


static void check_version(struct jobmgr_data_t *q) {
    char buf[1024], resbuf[1024], *p1, *p2;
    uint16_t rrcount[DNS_MAX_SECTS];
    const uint8_t *rd, *eom;
    time_t now = time(NULL);
    int i, vmaj, vmin, newver, secflag, len;

    if(q->next_vcheck > now)
	return;
    q->next_vcheck += 24 * 60 * 60 + (sxi_rand() % (60 * 60)) - 30 * 60;
    if(q->next_vcheck <= now)
	q->next_vcheck = now + 24 * 60 * 60 + (sxi_rand() % (60 * 60)) - 30 * 60;
    if(qbind_int(q->qvbump, ":next", q->next_vcheck) ||
       qstep_noret(q->qvbump))
	WARN("Cannot update check time");

    if(res_init()) {
	WARN("Failed to initialize resolver");
	return;
    }

    snprintf(buf, sizeof(buf), "%lld.%s.sxver.skylable.com", (long long)q->job_id, sx_hashfs_self_unique(q->hashfs));
    len = res_query(buf, C_IN, T_TXT, resbuf, sizeof(resbuf));
    if(len < 0) {
	WARN("Failed to check version: query failed");
	return;
    }

    rd = resbuf;
    eom = resbuf + len;
    do {
	if(len < sizeof(uint16_t) + sizeof(uint16_t) + DNS_MAX_SECTS * sizeof(uint16_t))
	    break;
	rd += sizeof(uint16_t) + sizeof(uint16_t); /* id + flags */
	for(i=0; i<DNS_MAX_SECTS; i++) {
	    rrcount[i] = to_u16(rd);
	    rd += 2;
	}
	if(rrcount[DNS_QUESTION_SECT] != 1 || rrcount[DNS_ANSWER_SECT] != 1)
	    break;
	/* At question section: name + type + class */
	i = dn_skipname(rd, eom);
	if(i < 0 || rd + i + sizeof(uint16_t) + sizeof(uint16_t) > eom)
	    break;
	rd += i + sizeof(uint16_t) + sizeof(uint16_t);
	/* At answer section: name + type + class + ttl + rdlen (+rdata) */
	i = dn_skipname(rd, eom);
	if(i < 0 || rd + i + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint16_t) > eom)
	    break;
	rd += i;
	if(to_u16(rd) != T_TXT || to_u16(rd+2) != C_IN)
	    break;
	rd += sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t);
	len = to_u16(rd);
	rd += sizeof(uint16_t);
	if(len < 1 || rd + len > eom)
	    break;
	/* At rdata of the first record of the answer section: string_lenght + string */
	if(*rd != len - 1)
	    break;
	rd++;
	len = MIN(len, sizeof(buf)) - 1;
	memcpy(buf, rd, len);
	buf[len] = '\0';
	eom = NULL;
    } while(0);
    if(eom) {
	WARN("Failed to check version: bad DNS reply");
	return;
    }

    vmaj = strtol(buf, &p1, 10);
    if(p1 == buf || *p1 != '.') {
	WARN("Failed to check version: bad version received");
	return;
    }
    p1++;
    vmin = strtol(p1, &p2, 10);
    if(p2 == p1 || *p2 != '.') {
	WARN("Failed to check version: bad version received");
	return;
    }
    p2++;
    secflag = strtol(p2, &p1, 10);
    if(p2 == p1 || (*p1 != '.' && *p1 != '\0')) {
	WARN("Failed to check version: bad version received");
	return;
    }

    if(vmaj > SRC_MAJOR_VERSION)
	newver = 2;
    else if(vmaj == SRC_MAJOR_VERSION && vmin > SRC_MINOR_VERSION) {
	if(secflag || vmin > SRC_MINOR_VERSION + 1)
	    newver = 2;
	else
	    newver = 1;
    } else
	newver=0;

    if(newver) {
	if(newver > 1) {
	    CRIT("CRITICAL update found! Skylable SX %d.%d is available (this node is running version %d.%d)", vmaj, vmin, SRC_MAJOR_VERSION, SRC_MINOR_VERSION);
	    CRIT("See http://www.skylable.com/products/sx/release/%d.%d for upgrade instructions", vmaj, vmin);
	} else {
	    INFO("Skylable SX %d.%d is available (this node is running version %d.%d)", vmaj, vmin, SRC_MAJOR_VERSION, SRC_MINOR_VERSION);
	    INFO("See http://www.skylable.com/products/sx/release/%d.%d for more info", vmaj, vmin);
	}
    }

}

static void jobmgr_process_queue(struct jobmgr_data_t *q, int forced) {
    while(!terminate) {
	const void *ptr;
	unsigned int plen;
	int r;

	r = qstep(q->qjob);
	if(r == SQLITE_DONE && forced) {
	    unsigned int waitus = 200000;
	    do {
		usleep(waitus);
		waitus *= 2;
		r = qstep(q->qjob);
	    } while(r == SQLITE_DONE && waitus <= 800000);
	    if(r == SQLITE_DONE)
		DEBUG("Triggered run without jobs");
	}
        forced = 0;
	if(r == SQLITE_DONE) {
	    DEBUG("No more pending jobs");
	    break; /* Stop processing jobs */
	}
	if(r != SQLITE_ROW) {
	    WARN("Failed to retrieve the next job to execute");
	    break; /* Stop processing jobs */
	}

	q->job_id = sqlite3_column_int64(q->qjob, 0);
	q->job_type = sqlite3_column_int(q->qjob, 1);
	ptr = sqlite3_column_blob(q->qjob, 2);
	plen = sqlite3_column_bytes(q->qjob, 2);
	q->job_expired = sqlite3_column_int(q->qjob, 3);
	q->job_failed = (sqlite3_column_int(q->qjob, 4) != 0);
	q->job_data = make_jobdata(ptr, plen, sqlite3_column_int(q->qjob, 5));
	sqlite3_reset(q->qjob);

	if(!q->job_data) {
	    WARN("Job %lld has got invalid data", (long long)q->job_id);
	    continue; /* Process next job */
	}

	DEBUG("Running job %lld (type %d, %s, %s)", (long long)q->job_id, q->job_type, q->job_expired?"expired":"not expired", q->job_failed?"failed":"not failed");
	jobmgr_run_job(q);
	free(q->job_data);
	DEBUG("Finished running job %lld", (long long)q->job_id);
	/* Process next job */
        sx_hashfs_checkpoint_passive(q->hashfs);
    }

    if(!terminate)
	check_version(q);
}


int jobmgr(sxc_client_t *sx, const char *self, const char *dir, int pipe) {
    sqlite3_stmt *q_vcheck = NULL;
    struct jobmgr_data_t q;
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sighandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGUSR2, &act, NULL);
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);
    signal(SIGPIPE, SIG_IGN);

    act.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &act, NULL);
    sigaction(SIGHUP, &act, NULL);

    memset(&q, 0, sizeof(q));
    q.hashfs = sx_hashfs_open(dir, sx);
    if(!q.hashfs) {
	CRIT("Failed to initialize the hash server interface");
	goto jobmgr_err;
    }

    q.targets = sx_nodelist_new();
    if(!q.targets) {
	WARN("Cannot create target nodelist");
	goto jobmgr_err;
    }

    q.eventdb = sx_hashfs_eventdb(q.hashfs);

    if(qprep(q.eventdb, &q.qjob, "SELECT job, type, data, expiry_time < datetime('now'), result, strftime('%s',expiry_time) FROM jobs WHERE complete = 0 AND sched_time <= strftime('%Y-%m-%d %H:%M:%f') AND NOT EXISTS (SELECT 1 FROM jobs AS subjobs WHERE subjobs.job = jobs.parent AND subjobs.complete = 0) ORDER BY sched_time ASC LIMIT 1") ||
       qprep(q.eventdb, &q.qact, "SELECT id, phase, target, addr, internaladdr, capacity FROM actions WHERE job_id = :job AND phase < :maxphase ORDER BY phase") ||
       qprep(q.eventdb, &q.qfail_children, "WITH RECURSIVE descendents_of(jb) AS (SELECT job FROM jobs WHERE parent = :job UNION SELECT job FROM jobs, descendents_of WHERE jobs.parent = descendents_of.jb) UPDATE jobs SET result = :res, reason = :reason, complete = 1, lock = NULL WHERE job IN (SELECT * FROM descendents_of) AND result = 0") ||
       qprep(q.eventdb, &q.qfail_parent, "UPDATE jobs SET result = :res, reason = :reason WHERE job = :job AND result = 0") ||
       qprep(q.eventdb, &q.qcpl, "UPDATE jobs SET complete = 1, lock = NULL WHERE job = :job") ||
       qprep(q.eventdb, &q.qphs, "UPDATE actions SET phase = :phase WHERE id = :act") ||
       qprep(q.eventdb, &q.qdly, "UPDATE jobs SET sched_time = strftime('%Y-%m-%d %H:%M:%f', 'now', :delay), reason = :reason WHERE job = :job") ||
       qprep(q.eventdb, &q.qlfe, "WITH RECURSIVE descendents_of(jb) AS (VALUES(:job) UNION SELECT job FROM jobs, descendents_of WHERE jobs.parent = descendents_of.jb) UPDATE jobs SET expiry_time = datetime(expiry_time, :ttldiff)  WHERE job IN (SELECT * FROM descendents_of)") ||
       qprep(q.eventdb, &q.qvbump, "INSERT OR REPLACE INTO hashfs (key, value) VALUES ('next_version_check', datetime(:next, 'unixepoch'))") ||
       qprep(q.eventdb, &q_vcheck, "SELECT strftime('%s', value) FROM hashfs WHERE key = 'next_version_check'"))
	goto jobmgr_err;

    if(qstep(q_vcheck) == SQLITE_ROW)
	q.next_vcheck = sqlite3_column_int(q_vcheck, 0);
    else
	q.next_vcheck = time(NULL);
    qnullify(q_vcheck);

    while(!terminate) {
	int forced_awake = 0;

        if (wait_trigger(pipe, JOBMGR_DELAY_MIN, &forced_awake))
            break;

	DEBUG("Start processing job queue");
	jobmgr_process_queue(&q, forced_awake);
	DEBUG("Done processing job queue");
        sx_hashfs_checkpoint_eventdb(q.hashfs);
        sx_hashfs_checkpoint_gc(q.hashfs);
        sx_hashfs_checkpoint_passive(q.hashfs);
        checkpoint_volume_sizes(q.hashfs);
    }

 jobmgr_err:
    sqlite3_finalize(q.qjob);
    sqlite3_finalize(q.qact);
    sqlite3_finalize(q.qfail_children);
    sqlite3_finalize(q.qfail_parent);
    sqlite3_finalize(q.qcpl);
    sqlite3_finalize(q.qphs);
    sqlite3_finalize(q.qdly);
    sqlite3_finalize(q.qlfe);
    sqlite3_finalize(q.qvbump);
    sqlite3_finalize(q_vcheck);
    sx_nodelist_delete(q.targets);
    sx_hashfs_close(q.hashfs);
    close(pipe);
    return terminate ? EXIT_SUCCESS : EXIT_FAILURE;
}
