/* -*- C -*- */
/*
 * Copyright (c) 2020-2021 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */



#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_CLIENT
#include "lib/trace.h"

#include "lib/misc.h"               /* M0_SRC_PATH */
#include "lib/finject.h"
#include "ut/ut.h"
#include "ut/misc.h"                /* M0_UT_CONF_PROFILE */
#include "rpc/rpclib.h"             /* m0_rpc_server_ctx */
#include "fid/fid.h"
#include "motr/client.h"
#include "motr/client_internal.h"
#include "motr/idx.h"
#include "dix/layout.h"
#include "dix/client.h"
#include "dix/meta.h"
#include "fop/fom_simple.h"     /* m0_fom_simple */
#include "cas/cas_xc.h"
#include "dtm0/fop.h"
#include "dtm0/drlink.h"
#include "conf/helpers.h"
#include "dix/fid_convert.h"
#include "be/dtm0_log.h"

#define WAIT_TIMEOUT               M0_TIME_NEVER
#define SERVER_LOG_FILE_NAME       "cas_server.log"

static struct m0_client        *ut_m0c;
static struct m0_config         ut_m0_config;
static struct m0_idx_dix_config ut_dix_config;

enum {
	MAX_RPCS_IN_FLIGHT = 10,
	CNT = 10,
	BATCH_SZ = 128,
};

static char *cas_startup_cmd[] = {
	"m0d", "-T", "linux",
	"-D", "cs_sdb", "-S", "cs_stob",
	"-A", "linuxstob:cs_addb_stob",
	"-e", M0_NET_XPRT_PREFIX_DEFAULT":0@lo:12345:34:1",
	"-H", "0@lo:12345:34:1",
	"-w", "10", "-F",
	"-f", M0_UT_CONF_PROCESS,
	"-c", M0_SRC_PATH("motr/ut/dix_conf.xc")
};

static const char *local_ep_addr = "0@lo:12345:34:2";
static const char *srv_ep_addr   = { "0@lo:12345:34:1" };
static const char *process_fid   = M0_UT_CONF_PROCESS;
struct m0_fid      pver          = M0_FID_TINIT('v', 1, 100);

static struct m0_rpc_server_ctx dix_ut_sctx = {
		.rsx_argv             = cas_startup_cmd,
		.rsx_argc             = ARRAY_SIZE(cas_startup_cmd),
		.rsx_log_file_name    = SERVER_LOG_FILE_NAME
};

static void dix_config_init()
{
	int           rc;
	struct m0_ext range[] = {{ .e_start = 0, .e_end = IMASK_INF }};

	/* Create meta indices (root, layout, layout-descr). */
	rc = m0_dix_ldesc_init(&ut_dix_config.kc_layout_ldesc, range,
			       ARRAY_SIZE(range), HASH_FNC_FNV1, &pver);
	M0_UT_ASSERT(rc == 0);
	rc = m0_dix_ldesc_init(&ut_dix_config.kc_ldescr_ldesc, range,
			       ARRAY_SIZE(range), HASH_FNC_FNV1, &pver);
	M0_UT_ASSERT(rc == 0);
	/*
	 * motr/setup.c creates meta indices now. Therefore, we must not
	 * create it twice or it will fail with -EEXIST error.
	 */
	ut_dix_config.kc_create_meta = false;
}

static void dix_config_fini()
{
	m0_dix_ldesc_fini(&ut_dix_config.kc_layout_ldesc);
	m0_dix_ldesc_fini(&ut_dix_config.kc_ldescr_ldesc);
}

static void idx_dix_ut_m0_client_init()
{
	int rc;

	ut_m0c = NULL;
	ut_m0_config.mc_is_oostore            = true;
	ut_m0_config.mc_is_read_verify        = false;
	ut_m0_config.mc_local_addr            = local_ep_addr;
	ut_m0_config.mc_ha_addr               = srv_ep_addr;
	ut_m0_config.mc_profile               = M0_UT_CONF_PROFILE;
	/* Use fake fid, see initlift_resource_manager(). */
	ut_m0_config.mc_process_fid           = process_fid;
	ut_m0_config.mc_tm_recv_queue_min_len = M0_NET_TM_RECV_QUEUE_DEF_LEN;
	ut_m0_config.mc_max_rpc_msg_size      = M0_RPC_DEF_MAX_RPC_MSG_SIZE;
	ut_m0_config.mc_idx_service_id        = M0_IDX_DIX;
	ut_m0_config.mc_idx_service_conf      = &ut_dix_config;

	m0_fi_enable_once("ha_init", "skip-ha-init");
	/* Skip HA finalisation in case of failure path. */
	m0_fi_enable("ha_fini", "skip-ha-fini");
	/*
	 * We can't use m0_fi_enable_once() here, because
	 * initlift_addb2() may be called twice in case of failure path.
	 */
	m0_fi_enable("initlift_addb2", "no-addb2");
	m0_fi_enable("ha_process_event", "no-link");
	rc = m0_client_init(&ut_m0c, &ut_m0_config, false);
	M0_UT_ASSERT(rc == 0);
	m0_fi_disable("ha_process_event", "no-link");
	m0_fi_disable("initlift_addb2", "no-addb2");
	m0_fi_disable("ha_fini", "skip-ha-fini");
	ut_m0c->m0c_motr = m0_get();
}

static void idx_dix_ut_init()
{
	int rc;

	M0_SET0(&dix_ut_sctx.rsx_motr_ctx);
	dix_ut_sctx.rsx_xprts = m0_net_all_xprt_get();
	dix_ut_sctx.rsx_xprts_nr = m0_net_xprt_nr();
	rc = m0_rpc_server_start(&dix_ut_sctx);
	M0_ASSERT(rc == 0);
	dix_config_init();
	idx_dix_ut_m0_client_init();
}

static void idx_dix_ut_fini()
{
	/*
	 * Meta-indices are destroyed automatically during m0_rpc_server_stop()
	 * along with the whole BE data.
	 */
	dix_config_fini();
	m0_fi_enable_once("ha_fini", "skip-ha-fini");
	m0_fi_enable_once("initlift_addb2", "no-addb2");
	m0_fi_enable("ha_process_event", "no-link");
	m0_client_fini(ut_m0c, false);
	m0_fi_disable("ha_process_event", "no-link");
	m0_rpc_server_stop(&dix_ut_sctx);
}

static void ut_dix_init_fini(void)
{
	idx_dix_ut_init();
	idx_dix_ut_fini();
}

static int *rcs_alloc(int count)
{
	int  i;
	int *rcs;

	M0_ALLOC_ARR(rcs, count);
	M0_UT_ASSERT(rcs != NULL);
	for (i = 0; i < count; i++)
		/* Set to some value to assert that UT actually changed rc. */
		rcs[i] = 0xdb;
	return rcs;
}

static uint8_t ifid_type(bool dist)
{
	return dist ? m0_dix_fid_type.ft_id : m0_cas_index_fid_type.ft_id;
}

static void general_ifid_fill(struct m0_fid *ifid, bool dist)
{
	*ifid = M0_FID_TINIT(ifid_type(dist), 2, 1);
}

static void general_ifid_fill_batch(struct m0_fid *ifid, bool dist, int i)
{
	*ifid = M0_FID_TINIT(ifid_type(dist), 2, i);
}

static void ut_dix_namei_ops_cancel(bool dist)
{
	struct m0_container realm;
	struct m0_idx       idx[BATCH_SZ];
	struct m0_fid       ifid[BATCH_SZ];
	struct m0_op       *op[BATCH_SZ] = { NULL };
	int                 rc;
	int                 i;

	idx_dix_ut_init();
	m0_container_init(&realm, NULL,
			  &M0_UBER_REALM, ut_m0c);

	/* Create the index. */
	for (i = 0; i < BATCH_SZ; i++) {
		general_ifid_fill_batch(&ifid[i], dist, i);
		/* Create the index. */
		m0_idx_init(&idx[i], &realm.co_realm,
			   (struct m0_uint128 *)&ifid[i]);
		rc = m0_entity_create(NULL, &idx[i].in_entity, &op[i]);
		M0_UT_ASSERT(rc == 0);
	}
	m0_op_launch(op, BATCH_SZ);
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_op_wait(op[i], M0_BITS(M0_OS_LAUNCHED,
					       M0_OS_EXECUTED,
					       M0_OS_STABLE),
				WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
	}
	m0_op_cancel(op, BATCH_SZ);
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_op_wait(op[i], M0_BITS(M0_OS_STABLE,
					       M0_OS_FAILED),
				WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
		m0_op_fini(op[i]);
		m0_free0(&op[i]);
	}

	/* Check that index exists. */
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_idx_op(&idx[i], M0_IC_LOOKUP,
				NULL, NULL, NULL, 0, &op[i]);
		M0_UT_ASSERT(rc == 0);
	}
	m0_op_launch(op, BATCH_SZ);
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_op_wait(op[i], M0_BITS(M0_OS_LAUNCHED,
					       M0_OS_EXECUTED,
					       M0_OS_STABLE),
				WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
	}
	m0_op_cancel(op, BATCH_SZ);
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_op_wait(op[i], M0_BITS(M0_OS_STABLE,
					       M0_OS_FAILED),
				WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
		m0_op_fini(op[i]);
		m0_free0(&op[i]);
	}

	/* Delete the index. */
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_entity_delete(&idx[i].in_entity, &op[i]);
		M0_UT_ASSERT(rc == 0);
	}
	m0_op_launch(op, BATCH_SZ);
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_op_wait(op[i], M0_BITS(M0_OS_LAUNCHED,
					       M0_OS_EXECUTED,
					       M0_OS_STABLE),
				WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
	}
	m0_op_cancel(op, BATCH_SZ);
	for (i = 0; i < BATCH_SZ; i++) {
		rc = m0_op_wait(op[i], M0_BITS(M0_OS_STABLE,
					       M0_OS_FAILED),
				WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
		m0_op_fini(op[i]);
		m0_free0(&op[i]);
	}

	for (i = 0; i < BATCH_SZ; i++)
		m0_idx_fini(&idx[i]);
	idx_dix_ut_fini();
}

static void ut_dix_namei_ops_cancel_dist(void)
{
	ut_dix_namei_ops_cancel(true);
}

static void ut_dix_namei_ops_cancel_non_dist(void)
{
	ut_dix_namei_ops_cancel(false);
}

static void ut_dix_namei_ops(bool dist, uint32_t flags)
{
	struct m0_container  realm;
	struct m0_idx        idx;
	struct m0_idx        idup;
	struct m0_fid        ifid;
	struct m0_op        *op = NULL;
	struct m0_bufvec     keys;
	int                 *rcs;
	int                  rc;
	struct m0_op_common *oc;
	struct m0_op_idx    *oi;
	
	idx_dix_ut_init();
	m0_container_init(&realm, NULL, &M0_UBER_REALM, ut_m0c);
	general_ifid_fill(&ifid, dist);
	/* Create the index. */
	m0_idx_init(&idx, &realm.co_realm, (struct m0_uint128 *)&ifid);

	if (flags & M0_OIF_SKIP_LAYOUT)
		idx.in_entity.en_flags |= M0_ENF_META;
	rc = m0_entity_create(NULL, &idx.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	oc = M0_AMB(oc, op, oc_op);
	oi = M0_AMB(oi, oc, oi_oc);
	oi->oi_flags = flags;
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_free0(&op);

	/* Create an index with the same fid once more => -EEXIST. */
	m0_idx_init(&idup, &realm.co_realm, (struct m0_uint128 *)&ifid);
	if (flags & M0_OIF_SKIP_LAYOUT)
		idx.in_entity.en_flags |= M0_ENF_META;
	rc = m0_entity_create(NULL, &idup.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	oc = M0_AMB(oc, op, oc_op);
	oi = M0_AMB(oi, oc, oi_oc);
	oi->oi_flags = flags;
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(op->op_rc == -EEXIST);
	m0_op_fini(op);
	m0_free0(&op);
	m0_idx_fini(&idup);

	/* Check that index exists. */
	rc = m0_idx_op(&idx, M0_IC_LOOKUP, NULL, NULL, NULL, 0,
			      &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_free0(&op);

	/* List all indices (only one exists). */
	rcs = rcs_alloc(2);
	rc = m0_bufvec_alloc(&keys, 2, sizeof(struct m0_fid));
	M0_UT_ASSERT(rc == 0);
	rc = m0_idx_op(&idx, M0_IC_LIST, &keys, NULL, rcs, flags,
			      &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	if (flags & M0_OIF_SKIP_LAYOUT) {
		M0_UT_ASSERT(rcs[0] == -ENOENT);
		M0_UT_ASSERT(!m0_fid_eq(keys.ov_buf[0], &ifid));
	} else {
		M0_UT_ASSERT(rcs[0] == 0);
		M0_UT_ASSERT(m0_fid_eq(keys.ov_buf[0], &ifid));
	}
	M0_UT_ASSERT(rcs[1] == -ENOENT);
	M0_UT_ASSERT(keys.ov_vec.v_nr == 2);
	M0_UT_ASSERT(keys.ov_vec.v_count[0] == sizeof(struct m0_fid));
	M0_UT_ASSERT(keys.ov_vec.v_count[1] == sizeof(struct m0_fid));
	M0_UT_ASSERT(!m0_fid_is_set(keys.ov_buf[1]));
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);
	m0_bufvec_free(&keys);

	/* Delete the index. */
	rc = m0_entity_delete(&idx.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_free0(&op);

	/* Delete an index with the same fid once more => -ENOENT. */
	m0_idx_init(&idup, &realm.co_realm, (struct m0_uint128 *)&ifid);
	rc = m0_entity_open(&idup.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	rc = m0_entity_delete(&idup.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(op->op_rc == -ENOENT);
	m0_op_fini(op);
	m0_free0(&op);

	m0_idx_fini(&idx);
	idx_dix_ut_fini();
}

static void ut_dix_namei_ops_dist(void)
{
	ut_dix_namei_ops(true, 0);
}

static void ut_dix_namei_ops_dist_skip_layout(void)
{
	uint32_t flags = M0_OIF_SKIP_LAYOUT;
	ut_dix_namei_ops(true, flags);
}

static void ut_dix_namei_ops_dist_skip_layout_enable_crow(void)
{
	uint32_t flags = M0_OIF_SKIP_LAYOUT | M0_OIF_CROW;
	ut_dix_namei_ops(true, flags);
}

static void ut_dix_namei_ops_non_dist(void)
{
	ut_dix_namei_ops(false, 0);
}

static uint64_t dix_key(uint64_t i)
{
	return 100 + i;
}

static uint64_t dix_val(uint64_t i)
{
	return 100 + i * i;
}

static void ut_dix_record_ops(bool dist, uint32_t flags)
{
	struct m0_container  realm;
	struct m0_idx        idx;
	struct m0_fid        ifid;
	struct m0_op        *op = NULL;
	struct m0_bufvec     keys;
	struct m0_bufvec     vals;
	uint64_t             i;
	bool                 eof;
	uint64_t             accum;
	uint64_t             recs_nr;
	uint64_t             cur_key;
	int                  rc;
	int                 *rcs;
	struct m0_op_common *oc;
	struct m0_op_idx    *oi;

	idx_dix_ut_init();
	general_ifid_fill(&ifid, dist);
	m0_container_init(&realm, NULL, &M0_UBER_REALM, ut_m0c);
	m0_idx_init(&idx, &realm.co_realm, (struct m0_uint128 *)&ifid);
	if (flags & M0_OIF_SKIP_LAYOUT)
		idx.in_entity.en_flags |= M0_ENF_META;

	/* Create index. */
	rc = m0_entity_create(NULL, &idx.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	oc = M0_AMB(oc, op, oc_op);
	oi = M0_AMB(oi, oc, oi_oc);
	oi->oi_flags = flags;
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_free0(&op);

	/* Get non-existing key. */
	rcs = rcs_alloc(1);
	rc = m0_bufvec_alloc(&keys, 1, sizeof(uint64_t)) ?:
	     m0_bufvec_empty_alloc(&vals, 1);
	M0_UT_ASSERT(rc == 0);
	*(uint64_t*)keys.ov_buf[0] = dix_key(10);
	rc = m0_idx_op(&idx, M0_IC_GET, &keys, &vals, rcs, flags,
			      &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(vals.ov_buf[0] == NULL);
	M0_UT_ASSERT(vals.ov_vec.v_count[0] == 0);
	M0_UT_ASSERT(rcs[0] == -ENOENT);
	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);


	/* Add records to the index. */
	rcs = rcs_alloc(CNT);
	rc = m0_bufvec_alloc(&keys, CNT, sizeof(uint64_t)) ?:
	     m0_bufvec_alloc(&vals, CNT, sizeof(uint64_t));
	M0_UT_ASSERT(rc == 0);
	for (i = 0; i < keys.ov_vec.v_nr; i++) {
		*(uint64_t *)keys.ov_buf[i] = dix_key(i);
		*(uint64_t *)vals.ov_buf[i] = dix_val(i);
	}
	rc = m0_idx_op(&idx, M0_IC_PUT, &keys, &vals, rcs, 0,
		       &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc  == 0);
	M0_UT_ASSERT(m0_forall(i, CNT,
			       rcs[i] == 0 &&
		               *(uint64_t *)vals.ov_buf[i] == dix_val(i)));
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Try to add recs again without OVERWRITE flag. */
	rcs = rcs_alloc(CNT);
	rc = m0_idx_op(&idx, M0_IC_PUT, &keys, &vals, rcs, 0,
			      &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(op->op_sm.sm_rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT, rcs[i] == -EEXIST));
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Try to add recs again with OVERWRITE flag. */
	rcs = rcs_alloc(CNT);
	for (i = 0; i < keys.ov_vec.v_nr; i++)
		*(uint64_t *)vals.ov_buf[i] = dix_val(i * 10);

	rc = m0_idx_op(&idx, M0_IC_PUT, &keys, &vals, rcs,
		       M0_OIF_OVERWRITE, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(op->op_sm.sm_rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT, rcs[i] == 0));
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);

	/* Get records from the index by keys. */
	rcs = rcs_alloc(CNT);
	rc = m0_bufvec_alloc(&keys, CNT, sizeof(uint64_t)) ?:
	     m0_bufvec_empty_alloc(&vals, CNT);
	M0_UT_ASSERT(rc == 0);
	for (i = 0; i < keys.ov_vec.v_nr; i++)
		*(uint64_t*)keys.ov_buf[i] = dix_key(i);
	rc = m0_idx_op(&idx, M0_IC_GET, &keys, &vals, rcs, 0,
			      &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT,
			       rcs[i] == 0 &&
		               *(uint64_t *)vals.ov_buf[i] == dix_val(i * 10)));
	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Get records with all existing keys, except the one. */
	rcs = rcs_alloc(CNT);
	rc = m0_bufvec_alloc(&keys, CNT, sizeof(uint64_t)) ?:
	     m0_bufvec_empty_alloc(&vals, CNT);
	M0_UT_ASSERT(rc == 0);
	for (i = 0; i < keys.ov_vec.v_nr; i++)
		*(uint64_t*)keys.ov_buf[i] = dix_key(i);
	*(uint64_t *)keys.ov_buf[5] = dix_key(999);
	rc = m0_idx_op(&idx, M0_IC_GET, &keys, &vals, rcs, 0,
			      &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	for (i = 0; i < CNT; i++) {
		if (i != 5) {
			M0_UT_ASSERT(rcs[i] == 0);
			M0_UT_ASSERT(*(uint64_t *)vals.ov_buf[i] ==
							dix_val(i * 10));
		} else {
			M0_UT_ASSERT(rcs[i] == -ENOENT);
			M0_UT_ASSERT(vals.ov_buf[5] == NULL);
		}
	}
	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Iterate over all records in the index. */
	rcs = rcs_alloc(CNT + 1);
	rc = m0_bufvec_empty_alloc(&keys, CNT + 1) ?:
	     m0_bufvec_empty_alloc(&vals, CNT + 1);
	M0_UT_ASSERT(rc == 0);
	cur_key = dix_key(0);
	keys.ov_buf[0] = &cur_key;
	keys.ov_vec.v_count[0] = sizeof(uint64_t);
	rc = m0_idx_op(&idx, M0_IC_NEXT, &keys, &vals, rcs, 0, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT,
			       rcs[i] == 0 &&
			       *(uint64_t*)keys.ov_buf[i] == dix_key(i) &&
			       *(uint64_t*)vals.ov_buf[i] == dix_val(i * 10)));
	M0_UT_ASSERT(rcs[CNT] == -ENOENT);
	M0_UT_ASSERT(keys.ov_buf[CNT] == NULL);
	M0_UT_ASSERT(vals.ov_buf[CNT] == NULL);
	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Iterate over all records in the index excluding the start key. */
	rcs = rcs_alloc(CNT + 1);
	rc = m0_bufvec_empty_alloc(&keys, CNT + 1) ?:
	     m0_bufvec_empty_alloc(&vals, CNT + 1);
	M0_UT_ASSERT(rc == 0);
	cur_key = dix_key(0);
	keys.ov_buf[0] = &cur_key;
	keys.ov_vec.v_count[0] = sizeof(uint64_t);
	rc = m0_idx_op(&idx, M0_IC_NEXT, &keys, &vals, rcs,
		       M0_OIF_EXCLUDE_START_KEY, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT - 1,
			       rcs[i] == 0 &&
			       *(uint64_t*)keys.ov_buf[i] == dix_key(i + 1) &&
			       *(uint64_t*)vals.ov_buf[i] ==
			       dix_val((i + 1) * 10)));
	M0_UT_ASSERT(rcs[CNT - 1] == -ENOENT);
	M0_UT_ASSERT(keys.ov_buf[CNT - 1] == NULL);
	M0_UT_ASSERT(vals.ov_buf[CNT - 1] == NULL);
	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Try to add recs again with OVERWRITE flag. */
	rcs = rcs_alloc(CNT + 1);
	rc = m0_bufvec_alloc(&keys, CNT, sizeof(uint64_t)) ?:
	     m0_bufvec_alloc(&vals, CNT, sizeof(uint64_t));
	M0_UT_ASSERT(rc == 0);
	for (i = 0; i < keys.ov_vec.v_nr; i++) {
		*(uint64_t *)vals.ov_buf[i] = dix_val(i);
		*(uint64_t *)keys.ov_buf[i] = dix_key(i);
	}

	rc = m0_idx_op(&idx, M0_IC_PUT, &keys, &vals, rcs,
		       M0_OIF_OVERWRITE, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(op->op_sm.sm_rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT, rcs[i] == 0));
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);
	m0_bufvec_free(&vals);
	m0_bufvec_free(&keys);
	/*
	 * Iterate over all records in the index, starting from the beginning
	 * and requesting two records at a time.
	 */
	accum = 0;
	cur_key = 0;
	do {
		rcs = rcs_alloc(2);
		rc = m0_bufvec_empty_alloc(&keys, 2) ?:
		     m0_bufvec_empty_alloc(&vals, 2);
		M0_UT_ASSERT(rc == 0);
		if (cur_key != 0) {
			keys.ov_buf[0] = &cur_key;
			keys.ov_vec.v_count[0] = sizeof(uint64_t);
		} else {
			/*
			 * Pass NULL in order to request records starting from
			 * the smallest key.
			 */
			keys.ov_buf[0] = NULL;
			keys.ov_vec.v_count[0] = 0;
		}
		rc = m0_idx_op(&idx, M0_IC_NEXT, &keys, &vals,
				      rcs, 0, &op);
		M0_UT_ASSERT(rc == 0);
		m0_op_launch(&op, 1);
		rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE),
				       WAIT_TIMEOUT);
		M0_UT_ASSERT(rc == 0);
		for (i = 0; i < vals.ov_vec.v_nr && rcs[i] == 0; i++)
			;
		recs_nr = i;
		eof = recs_nr < keys.ov_vec.v_nr;
		for (i = 0; i < recs_nr; i++) {
			M0_UT_ASSERT(*(uint64_t *)keys.ov_buf[i] ==
				     dix_key(accum + i));
			M0_UT_ASSERT(*(uint64_t *)vals.ov_buf[i] ==
				     dix_val(accum + i));
			cur_key = *(uint64_t *)keys.ov_buf[i];
		}
		m0_bufvec_free(&keys);
		m0_bufvec_free(&vals);
		m0_op_fini(op);
		m0_free0(&op);
		m0_free0(&rcs);
		/*
		 * Starting key is also included in returned number of records,
		 * so extract 1. The only exception is the first request, when
		 * starting key is unknown. It is accounted before accum check
		 * after eof is reached.
		 */
		accum += recs_nr - 1;
	} while (!eof);
	accum++;
	M0_UT_ASSERT(accum == CNT);

	/* Remove the records from the index. */
	rcs = rcs_alloc(CNT);
	rc = m0_bufvec_alloc(&keys, CNT, sizeof(uint64_t));
	M0_UT_ASSERT(rc == 0);
	for (i = 0; i < keys.ov_vec.v_nr; i++)
		*(uint64_t *)keys.ov_buf[i] = dix_key(i);
	rc = m0_idx_op(&idx, M0_IC_DEL, &keys, NULL, rcs, 0, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(m0_forall(i, CNT, rcs[i] == 0));
	m0_bufvec_free(&keys);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);

	/* Remove the index. */
	rc = m0_entity_delete(&idx.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_free0(&op);

	m0_idx_fini(&idx);
	idx_dix_ut_fini();
}

static void ut_dix_record_ops_dist_skip_layout(void)
{
	uint32_t flags = M0_OIF_SKIP_LAYOUT;
	ut_dix_record_ops(true, flags);
}

static void ut_dix_record_ops_dist_skip_layout_enable_crow(void)
{
	uint32_t flags = M0_OIF_SKIP_LAYOUT | M0_OIF_CROW;
	ut_dix_record_ops(true, flags);
}

static void ut_dix_record_ops_dist(void)
{
	ut_dix_record_ops(true, 0);
}

static void ut_dix_record_ops_non_dist(void)
{
	ut_dix_record_ops(false, 0);
}

struct m0_ut_suite ut_suite_idx_dix = {
	.ts_name   = "idx-dix",
	.ts_owners = "Egor",
	.ts_init   = NULL,
	.ts_fini   = NULL,
	.ts_tests  = {
		{ "init-fini",            ut_dix_init_fini,           "Egor" },
		{ "namei-ops-dist",       ut_dix_namei_ops_dist,      "Egor" },
		{ "namei-ops-non-dist",   ut_dix_namei_ops_non_dist,  "Egor" },
		{ "record-ops-dist",      ut_dix_record_ops_dist,     "Egor" },
		{ "record-ops-non-dist",  ut_dix_record_ops_non_dist, "Egor" },
		{ "namei-ops-cancel-dist",     ut_dix_namei_ops_cancel_dist,
		  "Vikram" },
		{ "namei-ops-cancel-non-dist", ut_dix_namei_ops_cancel_non_dist,
		  "Vikram" },
		{ "namei-ops-dist-skip-layout",
		  ut_dix_namei_ops_dist_skip_layout,
		  "Venky" },
		{ "record-ops-dist-skip-layout",
		  ut_dix_record_ops_dist_skip_layout,
		  "Venky" },
		{ "namei-ops-dist-skip-layout-enable-crow",
		  ut_dix_namei_ops_dist_skip_layout_enable_crow,
		  "Venky" },
		{ "record-ops-dist-skip-layout-enable-crow",
		  ut_dix_record_ops_dist_skip_layout_enable_crow,
		  "Venky" },
		{ NULL, NULL }
	}
};

static int ut_suite_mt_idx_dix_init(void)
{
	idx_dix_ut_init();
	return 0;
}

static int ut_suite_mt_idx_dix_fini(void)
{
	idx_dix_ut_fini();
	return 0;
}

extern void st_mt(void);
extern void st_lsfid(void);
extern void st_lsfid_cancel(void);

struct m0_client* st_get_instance()
{
	return ut_m0c;
}

#include "dtm0/helper.h"
#include "dtm0/service.h"

struct dtm0_ut_ctx {
	struct m0_idx           duc_idx;
	struct m0_container     duc_realm;
	struct m0_fid           duc_ifid;
};

static struct dtm0_ut_ctx duc = {};
static struct m0_fid cli_dtm0_fid = M0_FID_INIT(0x7300000000000001, 0x1a);

static int duc_setup(void)
{
	int                      rc;
	struct m0_container     *realm = &duc.duc_realm;

	m0_fi_enable("m0_dtm0_in_ut", "ut");
	rc = ut_suite_mt_idx_dix_init();
	M0_UT_ASSERT(rc == 0);

	m0_container_init(realm, NULL, &M0_UBER_REALM, ut_m0c);

	if (ENABLE_DTM0) {
		M0_UT_ASSERT(ut_m0c->m0c_dtms != NULL);
	}

	general_ifid_fill(&duc.duc_ifid, true);

	return 0;
}

static void idx_setup(void)
{
	struct m0_op            *op = NULL;
	struct m0_idx           *idx = &duc.duc_idx;
	struct m0_container     *realm = &duc.duc_realm;
	int                      rc;
	struct m0_fid           *ifid = &duc.duc_ifid;

	m0_idx_init(idx, &realm->co_realm, (struct m0_uint128 *) ifid);

	/* Create the index */
	rc = m0_entity_create(NULL, &idx->in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_op_free(op);
	op = NULL;
}

static void idx_teardown(void)
{
	struct m0_op            *op = NULL;
	int                      rc;

	/* Delete the index */
	rc = m0_entity_delete(&duc.duc_idx.in_entity, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	m0_op_fini(op);
	m0_op_free(op);
	m0_idx_fini(&duc.duc_idx);
	M0_SET0(&duc.duc_idx);
}


static int duc_teardown(void)
{
	int rc;

	rc = ut_suite_mt_idx_dix_fini();
	m0_fi_disable("m0_dtm0_in_ut", "ut");
	return rc;
}

/* Submits multiple M0 client (PUT|DEL) operations and then waits on "phase1"
 * states, and then waits on "phase2" states.
 */
static void run_m0ops(uint64_t nr, enum m0_idx_opcode opcode,
		      uint64_t phase1wait,
		      uint64_t phase2wait)
{
	struct m0_idx      *idx = &duc.duc_idx;
	struct m0_op      **ops;
	int                *rcs;
	struct m0_bufvec   *key_vecs;
	char               *val = NULL;
	struct m0_bufvec    vals = {};
	m0_bcount_t         len = 1;
	int                 flags = 0;
	uint64_t            i;
	int                 rc;

	M0_PRE(M0_IN(opcode, (M0_IC_PUT, M0_IC_DEL)));
	M0_ALLOC_ARR(ops, nr);
	M0_UT_ASSERT(ops != NULL);
	M0_ALLOC_ARR(rcs, nr);
	M0_UT_ASSERT(rcs != NULL);
	M0_ALLOC_ARR(key_vecs, nr);
	M0_UT_ASSERT(key_vecs != NULL);

	if (opcode == M0_IC_PUT) {
		val = m0_strdup("ItIsAValue");
		M0_UT_ASSERT(val != NULL);
		vals = M0_BUFVEC_INIT_BUF((void **) &val, &len);
	}

	/* Execute the ops */
	for (i = 0; i < nr; ++i) {
		rc = m0_bufvec_alloc(&key_vecs[i], 1, sizeof(i));
		M0_UT_ASSERT(rc == 0);
		M0_UT_ASSERT(key_vecs[i].ov_vec.v_count[0] == sizeof(i));
		memcpy(key_vecs[i].ov_buf[0], &i, sizeof(i));

		rc = m0_idx_op(idx, opcode, &key_vecs[i],
			       opcode == M0_IC_DEL ? NULL : &vals,
			       &rcs[i], flags, &ops[i]);
		M0_UT_ASSERT(rc == 0);
		m0_op_launch(&ops[i], 1);

		if (phase1wait != 0)
			rc = m0_op_wait(ops[i], phase1wait, WAIT_TIMEOUT);
		else
			rc = 0;
		M0_LOG(M0_DEBUG, "Got phase1 %" PRIu64, i);
		if (rc == -ESRCH)
			M0_UT_ASSERT(ops[i]->op_sm.sm_state == M0_OS_STABLE);
	}

	/* Wait until they get stable */
	for (i = 0; i < nr; ++i) {
		if (phase2wait != 0)
			rc = m0_op_wait(ops[i], phase2wait, WAIT_TIMEOUT);
		else
			rc = 0;
		M0_LOG(M0_DEBUG, "Got phase2 %" PRIu64, i);
		M0_UT_ASSERT(rc == 0);
		M0_UT_ASSERT(ops[i]->op_rc == 0);
		M0_UT_ASSERT(rcs[0] == 0);
		m0_op_fini(ops[i]);
		m0_op_free(ops[i]);
		ops[i] = NULL;
		m0_bufvec_free(&key_vecs[i]);
	}

	M0_UT_ASSERT(M0_IS0(ops));
	m0_free(key_vecs);
	m0_free(ops);
	m0_free(val);
}

/** Launch an operation, wait until it gets executed and launch another one.
 * When all operations are executed, wait until all of them get stable.
 */
static void exec_then_stable(uint64_t nr, enum m0_idx_opcode opcode)
{
	run_m0ops(nr, opcode, M0_BITS(M0_OS_EXECUTED), M0_BITS(M0_OS_STABLE));
}

/** Launch an operation and wait until it gets stable. Then launch another one.
 */
static void exec_one_by_one(uint64_t nr, enum m0_idx_opcode opcode)
{
	run_m0ops(nr, opcode, M0_BITS(M0_OS_STABLE), 0);
}

/** Launch an operation then launch another one. When all opearations are
 * launched, wait until they get stable.
 */
static void exec_concurrent(uint64_t nr, enum m0_idx_opcode opcode)
{
	run_m0ops(nr, opcode, 0, M0_BITS(M0_OS_STABLE));
}

static void st_dtm0(void)
{
	idx_setup();
	exec_one_by_one(1, M0_IC_PUT);
	idx_teardown();
}

static void st_dtm0_putdel(void)
{
	idx_setup();
	exec_one_by_one(1, M0_IC_PUT);
	exec_one_by_one(1, M0_IC_DEL);
	idx_teardown();

	idx_setup();
	exec_one_by_one(100, M0_IC_PUT);
	exec_one_by_one(100, M0_IC_DEL);
	idx_teardown();
}

static void st_dtm0_e_then_s(void)
{
	idx_setup();
	exec_then_stable(100, M0_IC_PUT);
	exec_then_stable(100, M0_IC_DEL);
	idx_teardown();
}

static void st_dtm0_c(void)
{
	idx_setup();
	exec_concurrent(100, M0_IC_PUT);
	exec_concurrent(100, M0_IC_DEL);
	idx_teardown();
}

static void dtm0_ut_cas_op_prepare(const struct m0_fid    *cfid,
				   struct m0_cas_op       *op,
				   struct m0_cas_rec      *rec,
				   uint64_t               *key,
				   uint64_t               *val,
				   struct m0_dtm0_tx_desc *txr)
{
	int                  rc;
	struct m0_buf        buf_key = { .b_nob = sizeof(uint64_t),
					 .b_addr = key };
	struct m0_buf        buf_val = { .b_nob = sizeof(uint64_t),
					 .b_addr = val };
	struct m0_rpc_at_buf at_buf_key = { .u.ab_buf = buf_key,
					    .ab_type  = M0_RPC_AT_INLINE };
	struct m0_rpc_at_buf at_buf_val = { .u.ab_buf = buf_val,
					    .ab_type  = M0_RPC_AT_INLINE };

	rec->cr_key = at_buf_key;
	rec->cr_val = at_buf_val;

	op->cg_id.ci_layout.dl_type = DIX_LTYPE_DESCR;
	rc = m0_dix_ldesc_init(&op->cg_id.ci_layout.u.dl_desc,
			       &(struct m0_ext) { .e_start = 0,
			       .e_end = IMASK_INF }, 1, HASH_FNC_CITY,
			       &pver);
	M0_UT_ASSERT(rc == 0);

	op->cg_id.ci_fid = *cfid;
	op->cg_rec.cr_nr = 1;
	op->cg_rec.cr_rec = rec;
	if (txr != NULL) {
		rc = m0_dtm0_tx_desc_copy(txr, &op->cg_txd);
		M0_UT_ASSERT(rc == 0);
	}
}

static void dtm0_ut_send_redo(const struct m0_fid *ifid,
			      uint64_t *key, uint64_t *val)
{
	int                     rc;
	struct dtm0_req_fop     req = { .dtr_msg = DTM_REDO };
	struct m0_dtm0_tx_desc  txr = {};
	struct m0_dtm0_clk_src  dcs;
	struct m0_dtm0_ts       now;
	struct m0_dtm0_service *dtm0 = ut_m0c->m0c_dtms;
	struct m0_buf           payload;
	struct m0_cas_op        cas_op = {};
	struct m0_cas_rec       cas_rec = {};
	struct m0_fid           srv_dtm0_fid;
	struct m0_fid           srv_proc_fid;
	struct m0_fid           cctg_fid;
	/*
	 * FIXME: this zeroed fom is added by DTM0 team mates' request
	 * to make the merge easier as there is a massive parallel work.
	 * This fom is passed to m0_dtm0_req_post() to get sm_id without
	 * checks and errors.
	 * This fom must be and will be deleted in the next patch by
	 * Ivan Alekhin.
	 */
	struct m0_fom           zero_fom_to_be_deleted = {};
	/* Extreme hack to convert index fid to component catalogue fid. */
	uint32_t                sdev_idx = 10;

	m0_dtm0_clk_src_init(&dcs, M0_DTM0_CS_PHYS);
	m0_dtm0_clk_src_now(&dcs, &now);

	rc = m0_dtm0_tx_desc_init(&txr, 1);
	M0_UT_ASSERT(rc == 0);

	/*
	 * Use zero fid here intentionally to skip triggering of the
	 * pmsg send logic on the client side as we check REDOs only.
	 */
	txr.dtd_ps.dtp_pa[0].p_fid = M0_FID0;
	txr.dtd_ps.dtp_pa[0].p_state = M0_DTPS_PERSISTENT;
	txr.dtd_id = (struct m0_dtm0_tid) {
		.dti_ts = now,
		.dti_fid = cli_dtm0_fid
	};

	m0_dix_fid_convert_dix2cctg(ifid, &cctg_fid, sdev_idx);

	dtm0_ut_cas_op_prepare(&cctg_fid, &cas_op, &cas_rec, key, val, &txr);

	rc = m0_xcode_obj_enc_to_buf(&M0_XCODE_OBJ(m0_cas_op_xc, &cas_op),
				     &payload.b_addr, &payload.b_nob);
	M0_UT_ASSERT(rc == 0);

	req.dtr_txr = txr;
	req.dtr_payload = payload;

	rc = m0_fid_sscanf(ut_m0_config.mc_process_fid, &srv_proc_fid);
	M0_UT_ASSERT(rc == 0);
	rc = m0_conf_process2service_get(m0_reqh2confc(&ut_m0c->m0c_reqh),
					 &srv_proc_fid,
					 M0_CST_DTM0, &srv_dtm0_fid);
	M0_UT_ASSERT(rc == 0);

	rc = m0_dtm0_req_post(dtm0, NULL, &req, &srv_dtm0_fid,
			      &zero_fom_to_be_deleted, false);
	M0_UT_ASSERT(rc == 0);
}

static void dtm0_ut_read_and_check(uint64_t key, uint64_t val)
{
	struct m0_idx      *idx = &duc.duc_idx;
	struct m0_op       *op = NULL;
	struct m0_bufvec    keys;
	struct m0_bufvec    vals;
	int                 rc;
	int                *rcs;

	rcs = rcs_alloc(1);
	rc = m0_bufvec_alloc(&keys, 1, sizeof(uint64_t)) ?:
	     m0_bufvec_empty_alloc(&vals, 1);
	M0_UT_ASSERT(rc == 0);
	*(uint64_t*)keys.ov_buf[0] = key;
	rc = m0_idx_op(idx, M0_IC_GET, &keys, &vals, rcs, 0, &op);
	M0_UT_ASSERT(rc == 0);
	m0_op_launch(&op, 1);
	rc = m0_op_wait(op, M0_BITS(M0_OS_STABLE), WAIT_TIMEOUT);
	M0_UT_ASSERT(rc == 0);
	M0_UT_ASSERT(rcs[0] == 0);
	M0_UT_ASSERT(vals.ov_vec.v_nr == 1);
	M0_UT_ASSERT(vals.ov_vec.v_count[0] == sizeof(val));
	M0_UT_ASSERT(vals.ov_buf[0] != NULL);
	M0_UT_ASSERT(*(uint64_t *)vals.ov_buf[0] == val);
	m0_bufvec_free(&keys);
	m0_bufvec_free(&vals);
	m0_op_fini(op);
	m0_free0(&op);
	m0_free0(&rcs);
}

static void st_dtm0_r(void)
{
	m0_time_t rem;
	uint64_t  key = 111;
	uint64_t  val = 222;

	if (!ENABLE_DTM0)
		return;

	idx_setup();
	exec_one_by_one(1, M0_IC_PUT);
	dtm0_ut_send_redo(&duc.duc_ifid, &key, &val);

	/* XXX dirty hack, but now we don't have completion notification */
	rem = 2ULL * M0_TIME_ONE_SECOND;
        while (rem != 0)
                m0_nanosleep(rem, &rem);

	dtm0_ut_read_and_check(key, val);
	idx_teardown();
}

struct m0_ut_suite ut_suite_mt_idx_dix = {
	.ts_name   = "idx-dix-mt",
	.ts_owners = "Anatoliy",
	.ts_init   = duc_setup,
	.ts_fini   = duc_teardown,
	.ts_tests  = {
		{ "fom",            st_mt,            "Anatoliy" },
		{ "lsf",            st_lsfid,         "Anatoliy" },
		{ "lsfc",           st_lsfid_cancel,  "Vikram"   },
		{ "dtm0",           st_dtm0,          "Anatoliy" },
		{ "dtm0_putdel",    st_dtm0_putdel,   "Ivan"     },
		{ "dtm0_e_then_s",  st_dtm0_e_then_s, "Ivan"     },
		{ "dtm0_c",         st_dtm0_c,        "Ivan"     },
		{ "dtm0_r",         st_dtm0_r,        "Sergey"   },
		{ NULL, NULL }
	}
};

#undef M0_TRACE_SUBSYSTEM

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
