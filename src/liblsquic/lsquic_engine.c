/* Copyright (c) 2017 - 2019 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_engine.c - QUIC engine
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>
#ifndef WIN32
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#endif



#include "lsquic.h"
#include "lsquic_types.h"
#include "lsquic_int_types.h"
#include "lsquic_sizes.h"
#include "lsquic_parse_common.h"
#include "lsquic_parse.h"
#include "lsquic_packet_in.h"
#include "lsquic_packet_out.h"
#include "lsquic_senhist.h"
#include "lsquic_rtt.h"
#include "lsquic_cubic.h"
#include "lsquic_pacer.h"
#include "lsquic_send_ctl.h"
#include "lsquic_set.h"
#include "lsquic_conn_flow.h"
#include "lsquic_sfcw.h"
#include "lsquic_hash.h"
#include "lsquic_conn.h"
#include "lsquic_full_conn.h"
#include "lsquic_util.h"
#include "lsquic_qtags.h"
#include "lsquic_enc_sess.h"
#include "lsquic_mm.h"
#include "lsquic_engine_public.h"
#include "lsquic_eng_hist.h"
#include "lsquic_ev_log.h"
#include "lsquic_version.h"
#include "lsquic_attq.h"
#include "lsquic_min_heap.h"
#include "lsquic_http1x_if.h"
#include "lsquic_parse_common.h"
#include "lsquic_h3_prio.h"

#define LSQUIC_LOGGER_MODULE LSQLM_ENGINE
#include "lsquic_logger.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))


/* The batch of outgoing packets grows and shrinks dynamically */
#define MAX_OUT_BATCH_SIZE 1024
#define MIN_OUT_BATCH_SIZE 4
#define INITIAL_OUT_BATCH_SIZE 32

struct out_batch
{
    lsquic_conn_t           *conns  [MAX_OUT_BATCH_SIZE];
    lsquic_packet_out_t     *packets[MAX_OUT_BATCH_SIZE];
    struct lsquic_out_spec   outs   [MAX_OUT_BATCH_SIZE];
};

typedef struct lsquic_conn * (*conn_iter_f)(struct lsquic_engine *);

static void
process_connections (struct lsquic_engine *engine, conn_iter_f iter,
                     lsquic_time_t now);

static void
engine_incref_conn (lsquic_conn_t *conn, enum lsquic_conn_flags flag);

static lsquic_conn_t *
engine_decref_conn (lsquic_engine_t *engine, lsquic_conn_t *conn,
                                        enum lsquic_conn_flags flag);

static void
force_close_conn (lsquic_engine_t *engine, lsquic_conn_t *conn);

/* Nested calls to LSQUIC are not supported */
#define ENGINE_IN(e) do {                               \
    assert(!((e)->pub.enp_flags & ENPUB_PROC));         \
    (e)->pub.enp_flags |= ENPUB_PROC;                   \
} while (0)

#define ENGINE_OUT(e) do {                              \
    assert((e)->pub.enp_flags & ENPUB_PROC);            \
    (e)->pub.enp_flags &= ~ENPUB_PROC;                  \
} while (0)

/* A connection can be referenced from one of six places:
 *
 *   1. Connection hash: a connection starts its life in one of those.
 *
 *   2. Outgoing queue.
 *
 *   3. Tickable queue
 *
 *   4. Advisory Tick Time queue.
 *
 *   5. Closing connections queue.  This is a transient queue -- it only
 *      exists for the duration of process_connections() function call.
 *
 *   6. Ticked connections queue.  Another transient queue, similar to (5).
 *
 * The idea is to destroy the connection when it is no longer referenced.
 * For example, a connection tick may return TICK_SEND|TICK_CLOSE.  In
 * that case, the connection is referenced from two places: (2) and (5).
 * After its packets are sent, it is only referenced in (5), and at the
 * end of the function call, when it is removed from (5), reference count
 * goes to zero and the connection is destroyed.  If not all packets can
 * be sent, at the end of the function call, the connection is referenced
 * by (2) and will only be removed once all outgoing packets have been
 * sent.
 */
#define CONN_REF_FLAGS  (LSCONN_HASHED          \
                        |LSCONN_HAS_OUTGOING    \
                        |LSCONN_TICKABLE        \
                        |LSCONN_TICKED          \
                        |LSCONN_CLOSING         \
                        |LSCONN_ATTQ)






struct lsquic_engine
{
    struct lsquic_engine_public        pub;
    enum {
        ENG_SERVER      = LSENG_SERVER,
        ENG_HTTP        = LSENG_HTTP,
        ENG_COOLDOWN    = (1 <<  7),    /* Cooldown: no new connections */
        ENG_PAST_DEADLINE
                        = (1 <<  8),    /* Previous call to a processing
                                         * function went past time threshold.
                                         */
        ENG_CONNS_BY_ADDR
                        = (1 <<  9),    /* Connections are hashed by address */
#ifndef NDEBUG
        ENG_DTOR        = (1 << 26),    /* Engine destructor */
#endif
    }                                  flags;
    const struct lsquic_stream_if     *stream_if;
    void                              *stream_if_ctx;
    lsquic_packets_out_f               packets_out;
    void                              *packets_out_ctx;
    struct lsquic_hash                *conns_hash;
    struct min_heap                    conns_tickable;
    struct min_heap                    conns_out;
    struct eng_hist                    history;
    unsigned                           batch_size;
    struct attq                       *attq;
    /* Track time last time a packet was sent to give new connections
     * priority lower than that of existing connections.
     */
    lsquic_time_t                      last_sent;
    unsigned                           n_conns;
    lsquic_time_t                      deadline;
    lsquic_time_t                      resume_sending_at;
#if LSQUIC_CONN_STATS
    struct {
        unsigned                conns;
    }                                  stats;
    struct conn_stats                  conn_stats_sum;
    FILE                              *stats_fh;
#endif
    struct out_batch                   out_batch;
};


void
lsquic_engine_init_settings (struct lsquic_engine_settings *settings,
                             unsigned flags)
{
    memset(settings, 0, sizeof(*settings));
    settings->es_versions        = LSQUIC_DF_VERSIONS;
    if (flags & ENG_SERVER)
    {
        settings->es_cfcw        = LSQUIC_DF_CFCW_SERVER;
        settings->es_sfcw        = LSQUIC_DF_SFCW_SERVER;
        settings->es_support_srej= LSQUIC_DF_SUPPORT_SREJ_SERVER;
        settings->es_init_max_data
                                 = LSQUIC_DF_INIT_MAX_DATA_SERVER;
        settings->es_init_max_stream_data_bidi_remote
                         = LSQUIC_DF_INIT_MAX_STREAM_DATA_BIDI_REMOTE_SERVER;
        settings->es_init_max_stream_data_bidi_local
                         = LSQUIC_DF_INIT_MAX_STREAM_DATA_BIDI_LOCAL_SERVER;
        settings->es_init_max_stream_data_uni
                         = LSQUIC_DF_INIT_MAX_STREAM_DATA_UNI_SERVER;
    }
    else
    {
        settings->es_cfcw        = LSQUIC_DF_CFCW_CLIENT;
        settings->es_sfcw        = LSQUIC_DF_SFCW_CLIENT;
        settings->es_support_srej= LSQUIC_DF_SUPPORT_SREJ_CLIENT;
        settings->es_init_max_data
                                 = LSQUIC_DF_INIT_MAX_DATA_CLIENT;
        settings->es_init_max_stream_data_bidi_remote
                         = LSQUIC_DF_INIT_MAX_STREAM_DATA_BIDI_REMOTE_CLIENT;
        settings->es_init_max_stream_data_bidi_local
                         = LSQUIC_DF_INIT_MAX_STREAM_DATA_BIDI_LOCAL_CLIENT;
        settings->es_init_max_stream_data_uni
                         = LSQUIC_DF_INIT_MAX_STREAM_DATA_UNI_CLIENT;
    }
    settings->es_max_streams_in  = LSQUIC_DF_MAX_STREAMS_IN;
    settings->es_idle_conn_to    = LSQUIC_DF_IDLE_CONN_TO;
    settings->es_idle_timeout    = LSQUIC_DF_IDLE_TIMEOUT;
    settings->es_handshake_to    = LSQUIC_DF_HANDSHAKE_TO;
    settings->es_silent_close    = LSQUIC_DF_SILENT_CLOSE;
    settings->es_max_header_list_size
                                 = LSQUIC_DF_MAX_HEADER_LIST_SIZE;
    settings->es_ua              = LSQUIC_DF_UA;
    settings->es_ecn             = LSQUIC_DF_ECN;
    
    settings->es_pdmd            = QTAG_X509;
    settings->es_aead            = QTAG_AESG;
    settings->es_kexs            = QTAG_C255;
    settings->es_support_push    = LSQUIC_DF_SUPPORT_PUSH;
    settings->es_support_tcid0   = LSQUIC_DF_SUPPORT_TCID0;
    settings->es_support_nstp    = LSQUIC_DF_SUPPORT_NSTP;
    settings->es_honor_prst      = LSQUIC_DF_HONOR_PRST;
    settings->es_progress_check  = LSQUIC_DF_PROGRESS_CHECK;
    settings->es_rw_once         = LSQUIC_DF_RW_ONCE;
    settings->es_proc_time_thresh= LSQUIC_DF_PROC_TIME_THRESH;
    settings->es_pace_packets    = LSQUIC_DF_PACE_PACKETS;
    settings->es_clock_granularity = LSQUIC_DF_CLOCK_GRANULARITY;
    settings->es_init_max_streams_uni
                                 = LSQUIC_DF_INIT_MAX_STREAMS_UNI;
    settings->es_init_max_streams_bidi
                                 = LSQUIC_DF_INIT_MAX_STREAMS_BIDI;
    settings->es_scid_len        = LSQUIC_DF_SCID_LEN;
    settings->es_qpack_dec_max_size = LSQUIC_DF_QPACK_DEC_MAX_SIZE;
    settings->es_qpack_dec_max_blocked = LSQUIC_DF_QPACK_DEC_MAX_BLOCKED;
    settings->es_qpack_enc_max_size = LSQUIC_DF_QPACK_ENC_MAX_SIZE;
    settings->es_qpack_enc_max_blocked = LSQUIC_DF_QPACK_ENC_MAX_BLOCKED;
    settings->es_h3_placeholders = LSQUIC_DF_H3_PLACEHOLDERS;
}


/* Note: if returning an error, err_buf must be valid if non-NULL */
int
lsquic_engine_check_settings (const struct lsquic_engine_settings *settings,
                              unsigned flags,
                              char *err_buf, size_t err_buf_sz)
{
    unsigned sum;

    if (settings->es_cfcw < LSQUIC_MIN_FCW ||
        settings->es_sfcw < LSQUIC_MIN_FCW)
    {
        if (err_buf)
            snprintf(err_buf, err_buf_sz, "%s",
                                            "flow control window set too low");
        return -1;
    }
    if (0 == (settings->es_versions & LSQUIC_SUPPORTED_VERSIONS))
    {
        if (err_buf)
            snprintf(err_buf, err_buf_sz, "%s",
                        "No supported QUIC versions specified");
        return -1;
    }
    if (settings->es_versions & ~LSQUIC_SUPPORTED_VERSIONS)
    {
        if (err_buf)
            snprintf(err_buf, err_buf_sz, "%s",
                        "one or more unsupported QUIC version is specified");
        return -1;
    }
    if (settings->es_idle_timeout > 600)
    {
        if (err_buf)
            snprintf(err_buf, err_buf_sz, "%s",
                        "The maximum value of idle timeout is 600 seconds");
        return -1;
    }
    if (!(!(flags & ENG_SERVER) && settings->es_scid_len == 0)
            && (settings->es_scid_len < 4 || settings->es_scid_len > 18))
    {
        if (err_buf)
            snprintf(err_buf, err_buf_sz, "Source connection ID cannot be %u "
                        "bytes long; it must be between 4 and 18.",
                        settings->es_scid_len);
        return -1;
    }

    sum = settings->es_init_max_streams_bidi
        + settings->es_init_max_streams_uni
        + settings->es_h3_placeholders;
    if (sum > H3_PRIO_MAX_ELEMS)
    {
        if (err_buf)
            snprintf(err_buf, err_buf_sz, "Combined number of streams and "
                "placeholders (%u) is greater than the maximum supported "
                "number of elements in the HTTP/3 priority tree (%u)",
                sum, H3_PRIO_MAX_ELEMS);
        return -1;
    }
    return 0;
}


static void
free_packet (void *ctx, void *conn_ctx, void *packet_data, char is_ipv6)
{
    free(packet_data);
}


static void *
malloc_buf (void *ctx, void *conn_ctx, unsigned short size, char is_ipv6)
{
    return malloc(size);
}


static const struct lsquic_packout_mem_if stock_pmi =
{
    malloc_buf, free_packet, free_packet,
};


static int
hash_conns_by_addr (const struct lsquic_engine *engine)
{
    if (engine->pub.enp_settings.es_versions & LSQUIC_FORCED_TCID0_VERSIONS)
        return 1;
    if ((engine->pub.enp_settings.es_versions & LSQUIC_GQUIC_HEADER_VERSIONS)
                                && engine->pub.enp_settings.es_support_tcid0)
        return 1;
    if (engine->pub.enp_settings.es_scid_len == 0)
        return 1;
    return 0;
}


lsquic_engine_t *
lsquic_engine_new (unsigned flags,
                   const struct lsquic_engine_api *api)
{
    lsquic_engine_t *engine;
    char err_buf[100];

    if (!api->ea_packets_out)
    {
        LSQ_ERROR("packets_out callback is not specified");
        return NULL;
    }

    if (api->ea_settings &&
                0 != lsquic_engine_check_settings(api->ea_settings, flags,
                                                    err_buf, sizeof(err_buf)))
    {
        LSQ_ERROR("cannot create engine: %s", err_buf);
        return NULL;
    }

    engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    if (0 != lsquic_mm_init(&engine->pub.enp_mm))
    {
        free(engine);
        return NULL;
    }
    if (api->ea_settings)
        engine->pub.enp_settings        = *api->ea_settings;
    else
        lsquic_engine_init_settings(&engine->pub.enp_settings, flags);
    engine->pub.enp_flags = ENPUB_CAN_SEND;

    engine->flags           = flags;
    engine->stream_if       = api->ea_stream_if;
    engine->stream_if_ctx   = api->ea_stream_if_ctx;
    engine->packets_out     = api->ea_packets_out;
    engine->packets_out_ctx = api->ea_packets_out_ctx;
    if (api->ea_hsi_if)
    {
        engine->pub.enp_hsi_if  = api->ea_hsi_if;
        engine->pub.enp_hsi_ctx = api->ea_hsi_ctx;
    }
    else
    {
        engine->pub.enp_hsi_if  = lsquic_http1x_if;
        engine->pub.enp_hsi_ctx = NULL;
    }
    if (api->ea_pmi)
    {
        engine->pub.enp_pmi      = api->ea_pmi;
        engine->pub.enp_pmi_ctx  = api->ea_pmi_ctx;
    }
    else
    {
        engine->pub.enp_pmi      = &stock_pmi;
        engine->pub.enp_pmi_ctx  = NULL;
    }
    engine->pub.enp_verify_cert  = api->ea_verify_cert;
    engine->pub.enp_verify_ctx   = api->ea_verify_ctx;
    engine->pub.enp_kli          = api->ea_keylog_if;
    engine->pub.enp_kli_ctx      = api->ea_keylog_ctx;
    engine->pub.enp_engine = engine;
    if (hash_conns_by_addr(engine))
        engine->flags |= ENG_CONNS_BY_ADDR;
    engine->conns_hash = lsquic_hash_create();
    engine->attq = attq_create();
    eng_hist_init(&engine->history);
    engine->batch_size = INITIAL_OUT_BATCH_SIZE;
    if (engine->pub.enp_settings.es_honor_prst)
    {
        engine->pub.enp_srst_hash = lsquic_hash_create();
        if (!engine->pub.enp_srst_hash)
        {
            lsquic_engine_destroy(engine);
            return NULL;
        }
    }

#if LSQUIC_CONN_STATS
    engine->stats_fh = api->ea_stats_fh;
#endif

    LSQ_INFO("instantiated engine");
    return engine;
}


static void
grow_batch_size (struct lsquic_engine *engine)
{
    engine->batch_size <<= engine->batch_size < MAX_OUT_BATCH_SIZE;
}


static void
shrink_batch_size (struct lsquic_engine *engine)
{
    engine->batch_size >>= engine->batch_size > MIN_OUT_BATCH_SIZE;
}


#if LSQUIC_CONN_STATS
void
update_stats_sum (struct lsquic_engine *engine, struct lsquic_conn *conn)
{
    unsigned long *const dst = (unsigned long *) &engine->conn_stats_sum;
    const unsigned long *src;
    const struct conn_stats *stats;
    unsigned i;

    if (conn->cn_if->ci_get_stats && (stats = conn->cn_if->ci_get_stats(conn)))
    {
        ++engine->stats.conns;
        src = (unsigned long *) stats;
        for (i = 0; i < sizeof(*stats) / sizeof(unsigned long); ++i)
            dst[i] += src[i];
    }
}


#endif


/* Wrapper to make sure important things occur before the connection is
 * really destroyed.
 */
static void
destroy_conn (struct lsquic_engine *engine, lsquic_conn_t *conn)
{
#if LSQUIC_CONN_STATS
    update_stats_sum(engine, conn);
#endif
    --engine->n_conns;
    conn->cn_flags |= LSCONN_NEVER_TICKABLE;
    conn->cn_if->ci_destroy(conn);
}


static int
maybe_grow_conn_heaps (struct lsquic_engine *engine)
{
    struct min_heap_elem *els;
    unsigned count;

    if (engine->n_conns < lsquic_mh_nalloc(&engine->conns_tickable))
        return 0;   /* Nothing to do */

    if (lsquic_mh_nalloc(&engine->conns_tickable))
        count = lsquic_mh_nalloc(&engine->conns_tickable) * 2 * 2;
    else
        count = 8;

    els = malloc(sizeof(els[0]) * count);
    if (!els)
    {
        LSQ_ERROR("%s: malloc failed", __func__);
        return -1;
    }

    LSQ_DEBUG("grew heaps to %u elements", count / 2);
    memcpy(&els[0], engine->conns_tickable.mh_elems,
                sizeof(els[0]) * lsquic_mh_count(&engine->conns_tickable));
    memcpy(&els[count / 2], engine->conns_out.mh_elems,
                sizeof(els[0]) * lsquic_mh_count(&engine->conns_out));
    free(engine->conns_tickable.mh_elems);
    engine->conns_tickable.mh_elems = els;
    engine->conns_out.mh_elems = &els[count / 2];
    engine->conns_tickable.mh_nalloc = count / 2;
    engine->conns_out.mh_nalloc = count / 2;
    return 0;
}


static void
remove_cces_from_hash (struct lsquic_hash *hash, struct lsquic_conn *conn,
                                                                unsigned todo)
{
    unsigned n;

    for (n = 0; todo; todo &= ~(1 << n++))
        if (todo & (1 << n))
            lsquic_hash_erase(hash, &conn->cn_cces[n].cce_hash_el);
}


static void
remove_all_cces_from_hash (struct lsquic_hash *hash, struct lsquic_conn *conn)
{
    remove_cces_from_hash(hash, conn, conn->cn_cces_mask);
}


static int
insert_conn_into_hash (struct lsquic_engine *engine, struct lsquic_conn *conn)
{
    struct conn_cid_elem *cce;
    unsigned todo, done, n;

    for (todo = conn->cn_cces_mask, done = 0, n = 0; todo; todo &= ~(1 << n++))
        if (todo & (1 << n))
        {
            cce = &conn->cn_cces[n];
            if (lsquic_hash_insert(engine->conns_hash, cce->cce_cid.idbuf,
                                    cce->cce_cid.len, conn, &cce->cce_hash_el))
                done |= 1 << n;
            else
                goto err;
        }

    return 0;

  err:
    remove_cces_from_hash(engine->conns_hash, conn, done);
    return -1;
}


/* The key is just the local port number */
static struct iovec
sa2key (const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        struct sockaddr_in *const sa4 = (void *) sa;
        return (struct iovec) { &sa4->sin_port, sizeof(sa4->sin_port), };
    }
    else
    {
        struct sockaddr_in6 *const sa6 = (void *) sa;
        return (struct iovec) { &sa6->sin6_port, sizeof(sa6->sin6_port), };
    }
}


static struct lsquic_hash_elem *
find_conn_by_addr (struct lsquic_hash *hash, const struct sockaddr *sa)
{
    struct iovec key;

    key = sa2key(sa);
    return lsquic_hash_find(hash, key.iov_base, key.iov_len);
}


static lsquic_conn_t *
find_conn (lsquic_engine_t *engine, lsquic_packet_in_t *packet_in,
         struct packin_parse_state *ppstate, const struct sockaddr *sa_local)
{
    struct lsquic_hash_elem *el;
    lsquic_conn_t *conn;

    if (engine->flags & ENG_CONNS_BY_ADDR)
        el = find_conn_by_addr(engine->conns_hash, sa_local);
    else if (packet_in->pi_flags & PI_CONN_ID)
        el = lsquic_hash_find(engine->conns_hash,
                    packet_in->pi_conn_id.idbuf, packet_in->pi_conn_id.len);
    else
    {
        LSQ_DEBUG("packet header does not have connection ID: discarding");
        return NULL;
    }

    if (!el)
        return NULL;

    conn = lsquic_hashelem_getdata(el);
    conn->cn_pf->pf_parse_packet_in_finish(packet_in, ppstate);
    if ((engine->flags & ENG_CONNS_BY_ADDR)
        && (packet_in->pi_flags & PI_CONN_ID)
        && !LSQUIC_CIDS_EQ(&conn->cn_cces[0].cce_cid, &packet_in->pi_conn_id))
    {
        LSQ_DEBUG("connection IDs do not match");
        return NULL;
    }

    return conn;
}


#if !defined(NDEBUG) && __GNUC__
__attribute__((weak))
#endif
void
lsquic_engine_add_conn_to_tickable (struct lsquic_engine_public *enpub,
                                    lsquic_conn_t *conn)
{
    if (0 == (enpub->enp_flags & ENPUB_PROC) &&
        0 == (conn->cn_flags & (LSCONN_TICKABLE|LSCONN_NEVER_TICKABLE)))
    {
        lsquic_engine_t *engine = (lsquic_engine_t *) enpub;
        lsquic_mh_insert(&engine->conns_tickable, conn, conn->cn_last_ticked);
        engine_incref_conn(conn, LSCONN_TICKABLE);
    }
}


void
lsquic_engine_add_conn_to_attq (struct lsquic_engine_public *enpub,
                                lsquic_conn_t *conn, lsquic_time_t tick_time)
{
    lsquic_engine_t *const engine = (lsquic_engine_t *) enpub;
    if (conn->cn_flags & LSCONN_TICKABLE)
    {
        /* Optimization: no need to add the connection to the Advisory Tick
         * Time Queue: it is about to be ticked, after which it its next tick
         * time may be queried again.
         */;
    }
    else if (conn->cn_flags & LSCONN_ATTQ)
    {
        if (lsquic_conn_adv_time(conn) != tick_time)
        {
            attq_remove(engine->attq, conn);
            if (0 != attq_add(engine->attq, conn, tick_time))
                engine_decref_conn(engine, conn, LSCONN_ATTQ);
        }
    }
    else if (0 == attq_add(engine->attq, conn, tick_time))
        engine_incref_conn(conn, LSCONN_ATTQ);
}


static struct lsquic_conn *
find_conn_by_srst (struct lsquic_engine *engine,
                                    const struct lsquic_packet_in *packet_in)
{
    struct lsquic_hash_elem *el;
    struct lsquic_conn *conn;

    if (packet_in->pi_data_sz < IQUIC_MIN_SRST_SIZE
                            || (packet_in->pi_data[0] & 0xC0) != 0x40)
        return NULL;

    el = lsquic_hash_find(engine->pub.enp_srst_hash,
            packet_in->pi_data + packet_in->pi_data_sz - IQUIC_SRESET_TOKEN_SZ,
            IQUIC_SRESET_TOKEN_SZ);
    if (!el)
        return NULL;

    conn = lsquic_hashelem_getdata(el);
    return conn;
}


/* Return 0 if packet is being processed by a connections, otherwise return 1 */
static int
process_packet_in (lsquic_engine_t *engine, lsquic_packet_in_t *packet_in,
       struct packin_parse_state *ppstate, const struct sockaddr *sa_local,
       const struct sockaddr *sa_peer, void *peer_ctx)
{
    lsquic_conn_t *conn;

    if (lsquic_packet_in_is_gquic_prst(packet_in)
                                && !engine->pub.enp_settings.es_honor_prst)
    {
        lsquic_mm_put_packet_in(&engine->pub.enp_mm, packet_in);
        LSQ_DEBUG("public reset packet: discarding");
        return 1;
    }

    conn = find_conn(engine, packet_in, ppstate, sa_local);

    if (!conn)
    {
        if (engine->pub.enp_settings.es_honor_prst
                && !(packet_in->pi_flags & PI_GQUIC)
                && engine->pub.enp_srst_hash
                && (conn = find_conn_by_srst(engine, packet_in)))
        {
            LSQ_DEBUGC("got stateless reset for connection %"CID_FMT,
                CID_BITS(lsquic_conn_log_cid(conn)));
            conn->cn_if->ci_stateless_reset(conn);
            if (!(conn->cn_flags & LSCONN_TICKABLE)
                && conn->cn_if->ci_is_tickable(conn))
            {
                lsquic_mh_insert(&engine->conns_tickable, conn,
                                                        conn->cn_last_ticked);
                engine_incref_conn(conn, LSCONN_TICKABLE);
            }
        }
        lsquic_mm_put_packet_in(&engine->pub.enp_mm, packet_in);
        return 1;
    }

    if (0 == (conn->cn_flags & LSCONN_TICKABLE))
    {
        lsquic_mh_insert(&engine->conns_tickable, conn, conn->cn_last_ticked);
        engine_incref_conn(conn, LSCONN_TICKABLE);
    }
    lsquic_conn_record_sockaddr(conn, sa_local, sa_peer);
    lsquic_packet_in_upref(packet_in);
    conn->cn_peer_ctx = peer_ctx;
    conn->cn_if->ci_packet_in(conn, packet_in);
    lsquic_packet_in_put(&engine->pub.enp_mm, packet_in);
    return 0;
}


void
lsquic_engine_destroy (lsquic_engine_t *engine)
{
    struct lsquic_hash_elem *el;
    lsquic_conn_t *conn;

    LSQ_DEBUG("destroying engine");
#ifndef NDEBUG
    engine->flags |= ENG_DTOR;
#endif

    while ((conn = lsquic_mh_pop(&engine->conns_out)))
    {
        assert(conn->cn_flags & LSCONN_HAS_OUTGOING);
        (void) engine_decref_conn(engine, conn, LSCONN_HAS_OUTGOING);
    }

    while ((conn = lsquic_mh_pop(&engine->conns_tickable)))
    {
        assert(conn->cn_flags & LSCONN_TICKABLE);
        (void) engine_decref_conn(engine, conn, LSCONN_TICKABLE);
    }

    for (el = lsquic_hash_first(engine->conns_hash); el;
                                el = lsquic_hash_next(engine->conns_hash))
    {
        conn = lsquic_hashelem_getdata(el);
        force_close_conn(engine, conn);
    }
    lsquic_hash_destroy(engine->conns_hash);

    assert(0 == engine->n_conns);
    attq_destroy(engine->attq);

    assert(0 == lsquic_mh_count(&engine->conns_out));
    assert(0 == lsquic_mh_count(&engine->conns_tickable));
    lsquic_mm_cleanup(&engine->pub.enp_mm);
    free(engine->conns_tickable.mh_elems);
#if LSQUIC_CONN_STATS
    if (engine->stats_fh)
    {
        const struct conn_stats *const stats = &engine->conn_stats_sum;
        fprintf(engine->stats_fh, "Aggregate connection stats collected by engine:\n");
        fprintf(engine->stats_fh, "Connections: %u\n", engine->stats.conns);
        fprintf(engine->stats_fh, "Ticks: %lu\n", stats->n_ticks);
        fprintf(engine->stats_fh, "In:\n");
        fprintf(engine->stats_fh, "    Total bytes: %lu\n", stats->in.bytes);
        fprintf(engine->stats_fh, "    packets: %lu\n", stats->in.packets);
        fprintf(engine->stats_fh, "    undecryptable packets: %lu\n", stats->in.undec_packets);
        fprintf(engine->stats_fh, "    duplicate packets: %lu\n", stats->in.dup_packets);
        fprintf(engine->stats_fh, "    error packets: %lu\n", stats->in.err_packets);
        fprintf(engine->stats_fh, "    STREAM frame count: %lu\n", stats->in.stream_frames);
        fprintf(engine->stats_fh, "    STREAM payload size: %lu\n", stats->in.stream_data_sz);
        fprintf(engine->stats_fh, "    Header bytes: %lu; uncompressed: %lu; ratio %.3lf\n",
            stats->in.headers_comp, stats->in.headers_uncomp,
            stats->in.headers_uncomp ?
            (double) stats->in.headers_comp / (double) stats->in.headers_uncomp
            : 0);
        fprintf(engine->stats_fh, "    ACK frames: %lu\n", stats->in.n_acks);
        fprintf(engine->stats_fh, "    ACK frames processed: %lu\n", stats->in.n_acks_proc);
        fprintf(engine->stats_fh, "    ACK frames merged to new: %lu\n", stats->in.n_acks_merged[0]);
        fprintf(engine->stats_fh, "    ACK frames merged to old: %lu\n", stats->in.n_acks_merged[1]);
        fprintf(engine->stats_fh, "Out:\n");
        fprintf(engine->stats_fh, "    Total bytes: %lu\n", stats->out.bytes);
        fprintf(engine->stats_fh, "    packets: %lu\n", stats->out.packets);
        fprintf(engine->stats_fh, "    acked via loss record: %lu\n", stats->out.acked_via_loss);
        fprintf(engine->stats_fh, "    acks: %lu\n", stats->out.acks);
        fprintf(engine->stats_fh, "    retx packets: %lu\n", stats->out.retx_packets);
        fprintf(engine->stats_fh, "    STREAM frame count: %lu\n", stats->out.stream_frames);
        fprintf(engine->stats_fh, "    STREAM payload size: %lu\n", stats->out.stream_data_sz);
        fprintf(engine->stats_fh, "    Header bytes: %lu; uncompressed: %lu; ratio %.3lf\n",
            stats->out.headers_comp, stats->out.headers_uncomp,
            stats->out.headers_uncomp ?
            (double) stats->out.headers_comp / (double) stats->out.headers_uncomp
            : 0);
        fprintf(engine->stats_fh, "    ACKs: %lu\n", stats->out.acks);
    }
#endif
    if (engine->pub.enp_srst_hash)
        lsquic_hash_destroy(engine->pub.enp_srst_hash);
    free(engine);
}


static int
add_conn_to_hash (struct lsquic_engine *engine, struct lsquic_conn *conn)
{
    struct iovec key;

    if (engine->flags & ENG_CONNS_BY_ADDR)
    {
        key = sa2key((struct sockaddr *) conn->cn_local_addr);
        if (lsquic_hash_insert(engine->conns_hash, key.iov_base, key.iov_len,
                                        conn, &conn->cn_cces[0].cce_hash_el))
            return 0;
        else
            return -1;

    }
    else
        return insert_conn_into_hash(engine, conn);
}


lsquic_conn_t *
lsquic_engine_connect (lsquic_engine_t *engine, const struct sockaddr *local_sa,
                       const struct sockaddr *peer_sa,
                       void *peer_ctx, lsquic_conn_ctx_t *conn_ctx, 
                       const char *hostname, unsigned short max_packet_size,
                       const unsigned char *zero_rtt, size_t zero_rtt_len,
                       const unsigned char *token, size_t token_sz)
{
    lsquic_conn_t *conn;
    unsigned flags;
    int is_ipv4;

    ENGINE_IN(engine);

    if (engine->flags & ENG_SERVER)
    {
        LSQ_ERROR("`%s' must only be called in client mode", __func__);
        goto err;
    }

    if (engine->flags & ENG_CONNS_BY_ADDR
                        && find_conn_by_addr(engine->conns_hash, local_sa))
    {
        LSQ_ERROR("cannot have more than one connection on the same port");
        goto err;
    }

    if (0 != maybe_grow_conn_heaps(engine))
        return NULL;
    flags = engine->flags & (ENG_SERVER|ENG_HTTP);
    is_ipv4 = peer_sa->sa_family == AF_INET;
    if (engine->pub.enp_settings.es_versions & LSQUIC_IETF_VERSIONS)
        conn = lsquic_ietf_full_conn_client_new(&engine->pub, engine->stream_if,
                    engine->stream_if_ctx, flags, hostname, max_packet_size,
                    is_ipv4, zero_rtt, zero_rtt_len, token, token_sz);
    else
        conn = lsquic_gquic_full_conn_client_new(&engine->pub,
                            engine->stream_if, engine->stream_if_ctx, flags,
                            hostname, max_packet_size, is_ipv4,
                            zero_rtt, zero_rtt_len);
    if (!conn)
        goto err;
    ++engine->n_conns;
    lsquic_conn_record_sockaddr(conn, local_sa, peer_sa);
    if (0 != add_conn_to_hash(engine, conn))
    {
        const lsquic_cid_t *cid = lsquic_conn_log_cid(conn);
        LSQ_WARNC("cannot add connection %"CID_FMT" to hash - destroy",
            CID_BITS(cid));
        destroy_conn(engine, conn);
        goto err;
    }
    assert(!(conn->cn_flags &
        (CONN_REF_FLAGS
         & ~LSCONN_TICKABLE /* This flag may be set as effect of user
                                 callbacks */
                             )));
    conn->cn_flags |= LSCONN_HASHED;
    lsquic_mh_insert(&engine->conns_tickable, conn, conn->cn_last_ticked);
    engine_incref_conn(conn, LSCONN_TICKABLE);
    conn->cn_peer_ctx = peer_ctx;
    lsquic_conn_set_ctx(conn, conn_ctx);
    conn->cn_if->ci_client_call_on_new(conn);
  end:
    ENGINE_OUT(engine);
    return conn;
  err:
    conn = NULL;
    goto end;
}


static void
remove_conn_from_hash (lsquic_engine_t *engine, lsquic_conn_t *conn)
{
    remove_all_cces_from_hash(engine->conns_hash, conn);
    (void) engine_decref_conn(engine, conn, LSCONN_HASHED);
}


static void
refflags2str (enum lsquic_conn_flags flags, char s[6])
{
    *s = 'C'; s += !!(flags & LSCONN_CLOSING);
    *s = 'H'; s += !!(flags & LSCONN_HASHED);
    *s = 'O'; s += !!(flags & LSCONN_HAS_OUTGOING);
    *s = 'T'; s += !!(flags & LSCONN_TICKABLE);
    *s = 'A'; s += !!(flags & LSCONN_ATTQ);
    *s = 'K'; s += !!(flags & LSCONN_TICKED);
    *s = '\0';
}


static void
engine_incref_conn (lsquic_conn_t *conn, enum lsquic_conn_flags flag)
{
    const lsquic_cid_t *cid;
    char str[2][7];
    assert(flag & CONN_REF_FLAGS);
    assert(!(conn->cn_flags & flag));
    conn->cn_flags |= flag;
    cid = lsquic_conn_log_cid(conn);
    LSQ_DEBUGC("incref conn %"CID_FMT", '%s' -> '%s'", CID_BITS(cid),
                    (refflags2str(conn->cn_flags & ~flag, str[0]), str[0]),
                    (refflags2str(conn->cn_flags, str[1]), str[1]));
}


static lsquic_conn_t *
engine_decref_conn (lsquic_engine_t *engine, lsquic_conn_t *conn,
                                        enum lsquic_conn_flags flags)
{
    const lsquic_cid_t *cid;
    char str[2][7];
    assert(flags & CONN_REF_FLAGS);
    assert(conn->cn_flags & flags);
#ifndef NDEBUG
    if (flags & LSCONN_CLOSING)
        assert(0 == (conn->cn_flags & LSCONN_HASHED));
#endif
    conn->cn_flags &= ~flags;
    cid = lsquic_conn_log_cid(conn);
    LSQ_DEBUGC("decref conn %"CID_FMT", '%s' -> '%s'", CID_BITS(cid),
                    (refflags2str(conn->cn_flags | flags, str[0]), str[0]),
                    (refflags2str(conn->cn_flags, str[1]), str[1]));
    if (0 == (conn->cn_flags & CONN_REF_FLAGS))
    {
        eng_hist_inc(&engine->history, 0, sl_del_full_conns);
        destroy_conn(engine, conn);
        return NULL;
    }
    else
        return conn;
}


/* This is not a general-purpose function.  Only call from engine dtor. */
static void
force_close_conn (lsquic_engine_t *engine, lsquic_conn_t *conn)
{
    assert(engine->flags & ENG_DTOR);
    const enum lsquic_conn_flags flags = conn->cn_flags;
    assert(conn->cn_flags & CONN_REF_FLAGS);
    assert(!(flags & LSCONN_HAS_OUTGOING));  /* Should be removed already */
    assert(!(flags & LSCONN_TICKABLE));    /* Should be removed already */
    assert(!(flags & LSCONN_CLOSING));  /* It is in transient queue? */
    if (flags & LSCONN_ATTQ)
    {
        attq_remove(engine->attq, conn);
        (void) engine_decref_conn(engine, conn, LSCONN_ATTQ);
    }
    if (flags & LSCONN_HASHED)
        remove_conn_from_hash(engine, conn);
}


/* Iterator for tickable connections (those on the Tickable Queue).  Before
 * a connection is returned, it is removed from the Advisory Tick Time queue
 * if necessary.
 */
static lsquic_conn_t *
conn_iter_next_tickable (struct lsquic_engine *engine)
{
    lsquic_conn_t *conn;

    conn = lsquic_mh_pop(&engine->conns_tickable);

    if (conn)
        conn = engine_decref_conn(engine, conn, LSCONN_TICKABLE);
    if (conn && (conn->cn_flags & LSCONN_ATTQ))
    {
        attq_remove(engine->attq, conn);
        conn = engine_decref_conn(engine, conn, LSCONN_ATTQ);
    }

    return conn;
}


void
lsquic_engine_process_conns (lsquic_engine_t *engine)
{
    lsquic_conn_t *conn;
    lsquic_time_t now;

    ENGINE_IN(engine);

    now = lsquic_time_now();
    while ((conn = attq_pop(engine->attq, now)))
    {
        conn = engine_decref_conn(engine, conn, LSCONN_ATTQ);
        if (conn && !(conn->cn_flags & LSCONN_TICKABLE))
        {
            lsquic_mh_insert(&engine->conns_tickable, conn, conn->cn_last_ticked);
            engine_incref_conn(conn, LSCONN_TICKABLE);
        }
    }

    process_connections(engine, conn_iter_next_tickable, now);
    ENGINE_OUT(engine);
}


static void
release_or_return_enc_data (struct lsquic_engine *engine,
                void (*pmi_rel_or_ret) (void *, void *, void *, char),
                struct lsquic_conn *conn, struct lsquic_packet_out *packet_out)
{
    pmi_rel_or_ret(engine->pub.enp_pmi_ctx, conn->cn_peer_ctx,
                packet_out->po_enc_data, lsquic_packet_out_ipv6(packet_out));
    packet_out->po_flags &= ~PO_ENCRYPTED;
    packet_out->po_enc_data = NULL;
}


static void
release_enc_data (struct lsquic_engine *engine, struct lsquic_conn *conn,
                                        struct lsquic_packet_out *packet_out)
{
    release_or_return_enc_data(engine, engine->pub.enp_pmi->pmi_release,
                                conn, packet_out);
}


static void
return_enc_data (struct lsquic_engine *engine, struct lsquic_conn *conn,
                                        struct lsquic_packet_out *packet_out)
{
    release_or_return_enc_data(engine, engine->pub.enp_pmi->pmi_return,
                                conn, packet_out);
}


STAILQ_HEAD(conns_stailq, lsquic_conn);
TAILQ_HEAD(conns_tailq, lsquic_conn);


struct conns_out_iter
{
    struct min_heap            *coi_heap;
    TAILQ_HEAD(, lsquic_conn)   coi_active_list,
                                coi_inactive_list;
    lsquic_conn_t              *coi_next;
#ifndef NDEBUG
    lsquic_time_t               coi_last_sent;
#endif
};


static void
coi_init (struct conns_out_iter *iter, struct lsquic_engine *engine)
{
    iter->coi_heap = &engine->conns_out;
    iter->coi_next = NULL;
    TAILQ_INIT(&iter->coi_active_list);
    TAILQ_INIT(&iter->coi_inactive_list);
#ifndef NDEBUG
    iter->coi_last_sent = 0;
#endif
}


static lsquic_conn_t *
coi_next (struct conns_out_iter *iter)
{
    lsquic_conn_t *conn;

    if (lsquic_mh_count(iter->coi_heap) > 0)
    {
        conn = lsquic_mh_pop(iter->coi_heap);
        TAILQ_INSERT_TAIL(&iter->coi_active_list, conn, cn_next_out);
        conn->cn_flags |= LSCONN_COI_ACTIVE;
#ifndef NDEBUG
        if (iter->coi_last_sent)
            assert(iter->coi_last_sent <= conn->cn_last_sent);
        iter->coi_last_sent = conn->cn_last_sent;
#endif
        return conn;
    }
    else if (!TAILQ_EMPTY(&iter->coi_active_list))
    {
        conn = iter->coi_next;
        if (!conn)
            conn = TAILQ_FIRST(&iter->coi_active_list);
        if (conn)
            iter->coi_next = TAILQ_NEXT(conn, cn_next_out);
        return conn;
    }
    else
        return NULL;
}


static void
coi_deactivate (struct conns_out_iter *iter, lsquic_conn_t *conn)
{
    if (!(conn->cn_flags & LSCONN_EVANESCENT))
    {
        assert(!TAILQ_EMPTY(&iter->coi_active_list));
        TAILQ_REMOVE(&iter->coi_active_list, conn, cn_next_out);
        conn->cn_flags &= ~LSCONN_COI_ACTIVE;
        TAILQ_INSERT_TAIL(&iter->coi_inactive_list, conn, cn_next_out);
        conn->cn_flags |= LSCONN_COI_INACTIVE;
    }
}


static void
coi_reactivate (struct conns_out_iter *iter, lsquic_conn_t *conn)
{
    assert(conn->cn_flags & LSCONN_COI_INACTIVE);
    TAILQ_REMOVE(&iter->coi_inactive_list, conn, cn_next_out);
    conn->cn_flags &= ~LSCONN_COI_INACTIVE;
    TAILQ_INSERT_TAIL(&iter->coi_active_list, conn, cn_next_out);
    conn->cn_flags |= LSCONN_COI_ACTIVE;
}


static void
coi_reheap (struct conns_out_iter *iter, lsquic_engine_t *engine)
{
    lsquic_conn_t *conn;
    while ((conn = TAILQ_FIRST(&iter->coi_active_list)))
    {
        TAILQ_REMOVE(&iter->coi_active_list, conn, cn_next_out);
        conn->cn_flags &= ~LSCONN_COI_ACTIVE;
        lsquic_mh_insert(iter->coi_heap, conn, conn->cn_last_sent);
    }
    while ((conn = TAILQ_FIRST(&iter->coi_inactive_list)))
    {
        TAILQ_REMOVE(&iter->coi_inactive_list, conn, cn_next_out);
        conn->cn_flags &= ~LSCONN_COI_INACTIVE;
        (void) engine_decref_conn(engine, conn, LSCONN_HAS_OUTGOING);
    }
}


static unsigned
send_batch (lsquic_engine_t *engine, struct conns_out_iter *conns_iter,
                  struct out_batch *batch, unsigned n_to_send)
{
    int n_sent, i;
    lsquic_time_t now;

    /* Set sent time before the write to avoid underestimating RTT */
    now = lsquic_time_now();
    for (i = 0; i < (int) n_to_send; ++i)
        batch->packets[i]->po_sent = now;
    n_sent = engine->packets_out(engine->packets_out_ctx, batch->outs,
                                                                n_to_send);
    if (n_sent < (int) n_to_send)
    {
        engine->pub.enp_flags &= ~ENPUB_CAN_SEND;
        engine->resume_sending_at = now + 1000000;
        LSQ_DEBUG("cannot send packets");
        EV_LOG_GENERIC_EVENT("cannot send packets");
    }
    if (n_sent >= 0)
        LSQ_DEBUG("packets out returned %d (out of %u)", n_sent, n_to_send);
    else
    {
        LSQ_DEBUG("packets out returned an error: %s", strerror(errno));
        n_sent = 0;
    }
    if (n_sent > 0)
        engine->last_sent = now + n_sent;
    for (i = 0; i < n_sent; ++i)
    {
        eng_hist_inc(&engine->history, now, sl_packets_out);
        EV_LOG_PACKET_SENT(lsquic_conn_log_cid(batch->conns[i]),
                                                    batch->packets[i]);
        batch->conns[i]->cn_if->ci_packet_sent(batch->conns[i],
                                                    batch->packets[i]);
        /* `i' is added to maintain relative order */
        batch->conns[i]->cn_last_sent = now + i;
        /* Release packet out buffer as soon as the packet is sent
         * successfully.  If not successfully sent, we hold on to
         * this buffer until the packet sending is attempted again
         * or until it times out and regenerated.
         */
        if (batch->packets[i]->po_flags & PO_ENCRYPTED)
            release_enc_data(engine, batch->conns[i], batch->packets[i]);
    }
    if (LSQ_LOG_ENABLED_EXT(LSQ_LOG_DEBUG, LSQLM_EVENT))
        for ( ; i < (int) n_to_send; ++i)
            EV_LOG_PACKET_NOT_SENT(lsquic_conn_log_cid(batch->conns[i]),
                                                        batch->packets[i]);
    /* Return packets to the connection in reverse order so that the packet
     * ordering is maintained.
     */
    for (i = (int) n_to_send - 1; i >= n_sent; --i)
    {
        batch->conns[i]->cn_if->ci_packet_not_sent(batch->conns[i],
                                                    batch->packets[i]);
        if (!(batch->conns[i]->cn_flags & (LSCONN_COI_ACTIVE|LSCONN_EVANESCENT)))
            coi_reactivate(conns_iter, batch->conns[i]);
    }
    return n_sent;
}


/* Return 1 if went past deadline, 0 otherwise */
static int
check_deadline (lsquic_engine_t *engine)
{
    if (engine->pub.enp_settings.es_proc_time_thresh &&
                                lsquic_time_now() > engine->deadline)
    {
        LSQ_INFO("went past threshold of %u usec, stop sending",
                            engine->pub.enp_settings.es_proc_time_thresh);
        engine->flags |= ENG_PAST_DEADLINE;
        return 1;
    }
    else
        return 0;
}


static void
send_packets_out (struct lsquic_engine *engine,
                  struct conns_tailq *ticked_conns,
                  struct conns_stailq *closed_conns)
{
    const lsquic_cid_t *cid;
    unsigned n, w, n_sent, n_batches_sent;
    lsquic_packet_out_t *packet_out;
    lsquic_conn_t *conn;
    struct out_batch *const batch = &engine->out_batch;
    struct conns_out_iter conns_iter;
    int shrink, deadline_exceeded;

    coi_init(&conns_iter, engine);
    n_batches_sent = 0;
    n_sent = 0, n = 0;
    shrink = 0;
    deadline_exceeded = 0;

    while ((conn = coi_next(&conns_iter)))
    {
        cid = lsquic_conn_log_cid(conn);
        packet_out = conn->cn_if->ci_next_packet_to_send(conn);
        if (!packet_out) {
            LSQ_DEBUGC("batched all outgoing packets for conn %"CID_FMT,
                                                    CID_BITS(cid));
            coi_deactivate(&conns_iter, conn);
            continue;
        }
        if ((packet_out->po_flags & PO_ENCRYPTED)
                && lsquic_packet_out_ipv6(packet_out)
                                            != lsquic_conn_peer_ipv6(conn))
        {
            /* Peer address changed since the packet was encrypted.  Need to
             * reallocate.
             */
            return_enc_data(engine, conn, packet_out);
        }
        if (!(packet_out->po_flags & (PO_ENCRYPTED|PO_NOENCRYPT)))
        {
            switch (conn->cn_esf_c->esf_encrypt_packet(conn->cn_enc_session,
                                            &engine->pub, conn, packet_out))
            {
            case ENCPA_NOMEM:
                /* Send what we have and wait for a more opportune moment */
                conn->cn_if->ci_packet_not_sent(conn, packet_out);
                goto end_for;
            case ENCPA_BADCRYPT:
                /* This is pretty bad: close connection immediately */
                conn->cn_if->ci_packet_not_sent(conn, packet_out);
                LSQ_INFOC("conn %"CID_FMT" has unsendable packets",
                                                    CID_BITS(cid));
                if (!(conn->cn_flags & LSCONN_EVANESCENT))
                {
                    if (!(conn->cn_flags & LSCONN_CLOSING))
                    {
                        STAILQ_INSERT_TAIL(closed_conns, conn, cn_next_closed_conn);
                        engine_incref_conn(conn, LSCONN_CLOSING);
                        if (conn->cn_flags & LSCONN_HASHED)
                            remove_conn_from_hash(engine, conn);
                    }
                    coi_deactivate(&conns_iter, conn);
                    if (conn->cn_flags & LSCONN_TICKED)
                    {
                        TAILQ_REMOVE(ticked_conns, conn, cn_next_ticked);
                        engine_decref_conn(engine, conn, LSCONN_TICKED);
                    }
                }
                continue;
            case ENCPA_OK:
                break;
            }
        }
        LSQ_DEBUGC("batched packet %"PRIu64" for connection %"CID_FMT,
                                        packet_out->po_packno, CID_BITS(cid));
        assert(conn->cn_flags & LSCONN_HAS_PEER_SA);
        if (packet_out->po_flags & PO_ENCRYPTED)
        {
            batch->outs[n].buf     = packet_out->po_enc_data;
            batch->outs[n].sz      = packet_out->po_enc_data_sz;
        }
        else
        {
            batch->outs[n].buf     = packet_out->po_data;
            batch->outs[n].sz      = packet_out->po_data_sz;
        }
        batch->outs   [n].ecn      = lsquic_packet_out_ecn(packet_out);
        batch->outs   [n].peer_ctx = conn->cn_peer_ctx;
        batch->outs   [n].local_sa = (struct sockaddr *) conn->cn_local_addr;
        batch->outs   [n].dest_sa  = (struct sockaddr *) conn->cn_peer_addr;
        batch->conns  [n]          = conn;
        batch->packets[n]          = packet_out;
        ++n;
        if (n == engine->batch_size)
        {
            n = 0;
            w = send_batch(engine, &conns_iter, batch, engine->batch_size);
            ++n_batches_sent;
            n_sent += w;
            if (w < engine->batch_size)
            {
                shrink = 1;
                break;
            }
            deadline_exceeded = check_deadline(engine);
            if (deadline_exceeded)
                break;
            grow_batch_size(engine);
        }
    }
  end_for:

    if (n > 0) {
        w = send_batch(engine, &conns_iter, batch, n);
        n_sent += w;
        shrink = w < n;
        ++n_batches_sent;
        deadline_exceeded = check_deadline(engine);
    }

    if (shrink)
        shrink_batch_size(engine);
    else if (n_batches_sent > 1 && !deadline_exceeded)
        grow_batch_size(engine);

    coi_reheap(&conns_iter, engine);

    LSQ_DEBUG("%s: sent %u packet%.*s", __func__, n_sent, n_sent != 1, "s");
}


int
lsquic_engine_has_unsent_packets (lsquic_engine_t *engine)
{
    return lsquic_mh_count(&engine->conns_out) > 0
    ;
}


static void
reset_deadline (lsquic_engine_t *engine, lsquic_time_t now)
{
    engine->deadline = now + engine->pub.enp_settings.es_proc_time_thresh;
    engine->flags &= ~ENG_PAST_DEADLINE;
}


/* TODO: this is a user-facing function, account for load */
void
lsquic_engine_send_unsent_packets (lsquic_engine_t *engine)
{
    lsquic_conn_t *conn;
    struct conns_stailq closed_conns;
    struct conns_tailq ticked_conns = TAILQ_HEAD_INITIALIZER(ticked_conns);

    STAILQ_INIT(&closed_conns);
    reset_deadline(engine, lsquic_time_now());
    if (!(engine->pub.enp_flags & ENPUB_CAN_SEND))
    {
        LSQ_DEBUG("can send again");
        EV_LOG_GENERIC_EVENT("can send again");
        engine->pub.enp_flags |= ENPUB_CAN_SEND;
    }

    send_packets_out(engine, &ticked_conns, &closed_conns);

    while ((conn = STAILQ_FIRST(&closed_conns))) {
        STAILQ_REMOVE_HEAD(&closed_conns, cn_next_closed_conn);
        (void) engine_decref_conn(engine, conn, LSCONN_CLOSING);
    }

}


static void
process_connections (lsquic_engine_t *engine, conn_iter_f next_conn,
                     lsquic_time_t now)
{
    lsquic_conn_t *conn;
    enum tick_st tick_st;
    unsigned i;
    lsquic_time_t next_tick_time;
    struct conns_stailq closed_conns;
    struct conns_tailq ticked_conns;

    eng_hist_tick(&engine->history, now);

    STAILQ_INIT(&closed_conns);
    TAILQ_INIT(&ticked_conns);
    reset_deadline(engine, now);

    if (!(engine->pub.enp_flags & ENPUB_CAN_SEND)
                                        && now > engine->resume_sending_at)
    {
        LSQ_NOTICE("failsafe activated: resume sending packets again after "
                    "timeout");
        EV_LOG_GENERIC_EVENT("resume sending packets again after timeout");
        engine->pub.enp_flags |= ENPUB_CAN_SEND;
    }

    i = 0;
    while ((conn = next_conn(engine))
          )
    {
        tick_st = conn->cn_if->ci_tick(conn, now);
        conn->cn_last_ticked = now + i /* Maintain relative order */ ++;
        if (tick_st & TICK_SEND)
        {
            if (!(conn->cn_flags & LSCONN_HAS_OUTGOING))
            {
                lsquic_mh_insert(&engine->conns_out, conn, conn->cn_last_sent);
                engine_incref_conn(conn, LSCONN_HAS_OUTGOING);
            }
        }
        if (tick_st & TICK_CLOSE)
        {
            STAILQ_INSERT_TAIL(&closed_conns, conn, cn_next_closed_conn);
            engine_incref_conn(conn, LSCONN_CLOSING);
            if (conn->cn_flags & LSCONN_HASHED)
                remove_conn_from_hash(engine, conn);
        }
        else
        {
            TAILQ_INSERT_TAIL(&ticked_conns, conn, cn_next_ticked);
            engine_incref_conn(conn, LSCONN_TICKED);
        }
    }

    if ((engine->pub.enp_flags & ENPUB_CAN_SEND)
                        && lsquic_engine_has_unsent_packets(engine))
        send_packets_out(engine, &ticked_conns, &closed_conns);

    while ((conn = STAILQ_FIRST(&closed_conns))) {
        STAILQ_REMOVE_HEAD(&closed_conns, cn_next_closed_conn);
        (void) engine_decref_conn(engine, conn, LSCONN_CLOSING);
    }

    /* TODO Heapification can be optimized by switching to the Floyd method:
     * https://en.wikipedia.org/wiki/Binary_heap#Building_a_heap
     */
    while ((conn = TAILQ_FIRST(&ticked_conns)))
    {
        TAILQ_REMOVE(&ticked_conns, conn, cn_next_ticked);
        engine_decref_conn(engine, conn, LSCONN_TICKED);
        if (!(conn->cn_flags & LSCONN_TICKABLE)
            && conn->cn_if->ci_is_tickable(conn))
        {
            lsquic_mh_insert(&engine->conns_tickable, conn, conn->cn_last_ticked);
            engine_incref_conn(conn, LSCONN_TICKABLE);
        }
        else if (!(conn->cn_flags & LSCONN_ATTQ))
        {
            next_tick_time = conn->cn_if->ci_next_tick_time(conn);
            if (next_tick_time)
            {
                if (0 == attq_add(engine->attq, conn, next_tick_time))
                    engine_incref_conn(conn, LSCONN_ATTQ);
            }
            else
                assert(0);
        }
    }

}


/* Return 0 if packet is being processed by a real connection, 1 if the
 * packet was processed, but not by a connection, and -1 on error.
 */
int
lsquic_engine_packet_in (lsquic_engine_t *engine,
    const unsigned char *packet_in_data, size_t packet_in_size,
    const struct sockaddr *sa_local, const struct sockaddr *sa_peer,
    void *peer_ctx, int ecn)
{
    const unsigned char *const packet_end = packet_in_data + packet_in_size;
    struct packin_parse_state ppstate;
    lsquic_packet_in_t *packet_in;
    int (*parse_packet_in_begin) (struct lsquic_packet_in *, size_t length,
                int is_server, unsigned cid_len, struct packin_parse_state *);
    unsigned n_zeroes;
    int s;

    if (engine->flags & ENG_CONNS_BY_ADDR)
    {
        struct lsquic_hash_elem *el;
        const struct lsquic_conn *conn;
        el = find_conn_by_addr(engine->conns_hash, sa_local);
        if (!el)
            return -1;
        conn = lsquic_hashelem_getdata(el);
        if ((1 << conn->cn_version) & LSQUIC_GQUIC_HEADER_VERSIONS)
            parse_packet_in_begin = lsquic_gquic_parse_packet_in_begin;
        else if ((1 << conn->cn_version) & LSQUIC_IETF_VERSIONS)
            parse_packet_in_begin = lsquic_ID18_parse_packet_in_begin;
        else
        {
            assert(conn->cn_version == LSQVER_044
#if LSQUIC_USE_Q098
                   || conn->cn_version == LSQVER_098
#endif

                                                    );
            parse_packet_in_begin = lsquic_Q044_parse_packet_in_begin;
        }
    }
    else
        parse_packet_in_begin = lsquic_parse_packet_in_begin;

    n_zeroes = 0;
    do
    {
        packet_in = lsquic_mm_get_packet_in(&engine->pub.enp_mm);
        if (!packet_in)
            return -1;
        /* Library does not modify packet_in_data, it is not referenced after
         * this function returns and subsequent release of pi_data is guarded
         * by PI_OWN_DATA flag.
         */
        packet_in->pi_data = (unsigned char *) packet_in_data;
        if (0 != parse_packet_in_begin(packet_in, packet_end - packet_in_data,
                                engine->flags & ENG_SERVER,
                                engine->pub.enp_settings.es_scid_len, &ppstate))
        {
            LSQ_DEBUG("Cannot parse incoming packet's header");
            lsquic_mm_put_packet_in(&engine->pub.enp_mm, packet_in);
            errno = EINVAL;
            return -1;
        }

        packet_in_data += packet_in->pi_data_sz;
        packet_in->pi_received = lsquic_time_now();
        packet_in->pi_flags |= (3 & ecn) << PIBIT_ECN_SHIFT;
        eng_hist_inc(&engine->history, packet_in->pi_received, sl_packets_in);
        s = process_packet_in(engine, packet_in, &ppstate, sa_local, sa_peer,
                                                                    peer_ctx);
        n_zeroes += s == 0;
    }
    while (0 == s && packet_in_data < packet_end);

    return n_zeroes > 0 ? 0 : s;
}


#if __GNUC__ && !defined(NDEBUG)
__attribute__((weak))
#endif
unsigned
lsquic_engine_quic_versions (const lsquic_engine_t *engine)
{
    return engine->pub.enp_settings.es_versions;
}


int
lsquic_engine_earliest_adv_tick (lsquic_engine_t *engine, int *diff)
{
    const lsquic_time_t *next_attq_time;
    lsquic_time_t now, next_time;

    if (((engine->flags & ENG_PAST_DEADLINE)
                                    && lsquic_mh_count(&engine->conns_out))
        || lsquic_mh_count(&engine->conns_tickable))
    {
        *diff = 0;
        return 1;
    }

    next_attq_time = attq_next_time(engine->attq);
    if (engine->pub.enp_flags & ENPUB_CAN_SEND)
    {
        if (next_attq_time)
            next_time = *next_attq_time;
        else
            return 0;
    }
    else
    {
        if (next_attq_time)
            next_time = MIN(*next_attq_time, engine->resume_sending_at);
        else
            next_time = engine->resume_sending_at;
    }

    now = lsquic_time_now();
    *diff = (int) ((int64_t) next_time - (int64_t) now);
    return 1;
}


unsigned
lsquic_engine_count_attq (lsquic_engine_t *engine, int from_now)
{
    lsquic_time_t now;
    now = lsquic_time_now();
    if (from_now < 0)
        now -= from_now;
    else
        now += from_now;
    return attq_count_before(engine->attq, now);
}


int
lsquic_engine_add_cid (struct lsquic_engine_public *enpub,
                              struct lsquic_conn *conn, unsigned cce_idx)
{
    struct lsquic_engine *const engine = (struct lsquic_engine *) enpub;
    struct conn_cid_elem *const cce = &conn->cn_cces[cce_idx];

    assert(cce_idx < conn->cn_n_cces);
    assert(!(cce->cce_hash_el.qhe_flags & QHE_HASHED));

    if (lsquic_hash_insert(engine->conns_hash, cce->cce_cid.idbuf,
                                    cce->cce_cid.len, conn, &cce->cce_hash_el))
    {
        LSQ_DEBUGC("add %"CID_FMT" to the list of SCIDs",
                                                    CID_BITS(&cce->cce_cid));
        return 0;
    }
    else
    {
        LSQ_WARNC("could not add new cid %"CID_FMT" to the SCID hash",
                                                    CID_BITS(&cce->cce_cid));
        return -1;
    }
}


void
lsquic_engine_retire_cid (struct lsquic_engine_public *enpub,
              struct lsquic_conn *conn, unsigned cce_idx, lsquic_time_t now)
{
    struct lsquic_engine *const engine = (struct lsquic_engine *) enpub;
    struct conn_cid_elem *const cce = &conn->cn_cces[cce_idx];

    assert(cce_idx < conn->cn_n_cces);

    if (cce->cce_hash_el.qhe_flags & QHE_HASHED)
        lsquic_hash_erase(engine->conns_hash, &cce->cce_hash_el);

    conn->cn_cces_mask &= ~(1u << cce_idx);
    LSQ_DEBUGC("retire CID %"CID_FMT, CID_BITS(&cce->cce_cid));
}


