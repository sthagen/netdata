#include "aclk_common.h"
#include "aclk_query.h"
#include "aclk_stats.h"
#include "aclk_rx_msgs.h"

#define WEB_HDR_ACCEPT_ENC "Accept-Encoding:"

pthread_cond_t query_cond_wait = PTHREAD_COND_INITIALIZER;
pthread_mutex_t query_lock_wait = PTHREAD_MUTEX_INITIALIZER;
#define QUERY_THREAD_LOCK pthread_mutex_lock(&query_lock_wait)
#define QUERY_THREAD_UNLOCK pthread_mutex_unlock(&query_lock_wait)

volatile int aclk_connected = 0;

#ifndef __GNUC__
#pragma region ACLK_QUEUE
#endif

static netdata_mutex_t queue_mutex = NETDATA_MUTEX_INITIALIZER;
#define ACLK_QUEUE_LOCK netdata_mutex_lock(&queue_mutex)
#define ACLK_QUEUE_UNLOCK netdata_mutex_unlock(&queue_mutex)

struct aclk_query {
    usec_t created;
    struct timeval tv_in;
    usec_t created_boot_time;
    time_t run_after; // Delay run until after this time
    ACLK_CMD cmd;     // What command is this
    char *topic;      // Topic to respond to
    char *data;       // Internal data (NULL if request from the cloud)
    char *msg_id;     // msg_id generated by the cloud (NULL if internal)
    char *query;      // The actual query
    u_char deleted;   // Mark deleted for garbage collect
    int idx;          // index of query thread
    struct aclk_query *next;
};

struct aclk_query_queue {
    struct aclk_query *aclk_query_head;
    struct aclk_query *aclk_query_tail;
    unsigned int count;
} aclk_queue = { .aclk_query_head = NULL, .aclk_query_tail = NULL, .count = 0 };


unsigned int aclk_query_size()
{
    int r;
    ACLK_QUEUE_LOCK;
    r = aclk_queue.count;
    ACLK_QUEUE_UNLOCK;
    return r;
}

/*
 * Free a query structure when done
 */
static void aclk_query_free(struct aclk_query *this_query)
{
    if (unlikely(!this_query))
        return;

    freez(this_query->topic);
    if (likely(this_query->query))
        freez(this_query->query);
    if(this_query->data && this_query->cmd == ACLK_CMD_CLOUD_QUERY_2) {
        struct aclk_cloud_req_v2 *del = (struct aclk_cloud_req_v2 *)this_query->data;
        freez(del->query_endpoint);
        freez(del->data);
        freez(del);
    }
    if (likely(this_query->msg_id))
        freez(this_query->msg_id);
    freez(this_query);
}

/*
 * Get the next query to process - NULL if nothing there
 * The caller needs to free memory by calling aclk_query_free()
 *
 *      topic
 *      query
 *      The structure itself
 *
 */
static struct aclk_query *aclk_queue_pop()
{
    struct aclk_query *this_query;

    ACLK_QUEUE_LOCK;

    if (likely(!aclk_queue.aclk_query_head)) {
        ACLK_QUEUE_UNLOCK;
        return NULL;
    }

    this_query = aclk_queue.aclk_query_head;

    // Get rid of the deleted entries
    while (this_query && this_query->deleted) {
        aclk_queue.count--;

        aclk_queue.aclk_query_head = aclk_queue.aclk_query_head->next;

        if (likely(!aclk_queue.aclk_query_head)) {
            aclk_queue.aclk_query_tail = NULL;
        }

        aclk_query_free(this_query);

        this_query = aclk_queue.aclk_query_head;
    }

    if (likely(!this_query)) {
        ACLK_QUEUE_UNLOCK;
        return NULL;
    }

    if (!this_query->deleted && this_query->run_after > now_realtime_sec()) {
        info("Query %s will run in %ld seconds", this_query->query, this_query->run_after - now_realtime_sec());
        ACLK_QUEUE_UNLOCK;
        return NULL;
    }

    aclk_queue.count--;
    aclk_queue.aclk_query_head = aclk_queue.aclk_query_head->next;

    if (likely(!aclk_queue.aclk_query_head)) {
        aclk_queue.aclk_query_tail = NULL;
    }

    ACLK_QUEUE_UNLOCK;
    return this_query;
}

// Returns the entry after which we need to create a new entry to run at the specified time
// If NULL is returned we need to add to HEAD
// Need to have a QUERY lock before calling this

static struct aclk_query *aclk_query_find_position(time_t time_to_run)
{
    struct aclk_query *tmp_query, *last_query;

    // Quick check if we will add to the end
    if (likely(aclk_queue.aclk_query_tail)) {
        if (aclk_queue.aclk_query_tail->run_after <= time_to_run)
            return aclk_queue.aclk_query_tail;
    }

    last_query = NULL;
    tmp_query = aclk_queue.aclk_query_head;

    while (tmp_query) {
        if (tmp_query->run_after > time_to_run)
            return last_query;
        last_query = tmp_query;
        tmp_query = tmp_query->next;
    }
    return last_query;
}

// Need to have a QUERY lock before calling this
static struct aclk_query *
aclk_query_find(char *topic, void *data, char *msg_id, char *query, ACLK_CMD cmd, struct aclk_query **last_query)
{
    struct aclk_query *tmp_query, *prev_query;
    UNUSED(cmd);

    tmp_query = aclk_queue.aclk_query_head;
    prev_query = NULL;
    while (tmp_query) {
        if (likely(!tmp_query->deleted)) {
            if (strcmp(tmp_query->topic, topic) == 0 && (!query || strcmp(tmp_query->query, query) == 0)) {
                if ((!data || data == tmp_query->data) &&
                    (!msg_id || (msg_id && strcmp(msg_id, tmp_query->msg_id) == 0))) {
                    if (likely(last_query))
                        *last_query = prev_query;
                    return tmp_query;
                }
            }
        }
        prev_query = tmp_query;
        tmp_query = tmp_query->next;
    }
    return NULL;
}

/*
 * Add a query to execute, the result will be send to the specified topic
 */

int aclk_queue_query(char *topic, void *data, char *msg_id, char *query, int run_after, int internal, ACLK_CMD aclk_cmd)
{
    struct aclk_query *new_query, *tmp_query;

    // Ignore all commands while we wait for the agent to initialize
    if (unlikely(!aclk_connected))
        return 1;

    run_after = now_realtime_sec() + run_after;

    ACLK_QUEUE_LOCK;
    struct aclk_query *last_query = NULL;

    tmp_query = aclk_query_find(topic, data, msg_id, query, aclk_cmd, &last_query);
    if (unlikely(tmp_query)) {
        if (tmp_query->run_after == run_after) {
            ACLK_QUEUE_UNLOCK;
            QUERY_THREAD_WAKEUP;
            return 0;
        }

        if (last_query)
            last_query->next = tmp_query->next;
        else
            aclk_queue.aclk_query_head = tmp_query->next;

        debug(D_ACLK, "Removing double entry");
        aclk_query_free(tmp_query);
        aclk_queue.count--;
    }

    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.queries_queued++;
        ACLK_STATS_UNLOCK;
    }

    new_query = callocz(1, sizeof(struct aclk_query));
    new_query->cmd = aclk_cmd;
    if (internal) {
        new_query->topic = strdupz(topic);
        if (likely(query))
            new_query->query = strdupz(query);
    } else {
        new_query->topic = topic;
        new_query->query = query;
        new_query->msg_id = msg_id;
    }

    new_query->data = data;
    new_query->next = NULL;
    now_realtime_timeval(&new_query->tv_in);
    new_query->created = (new_query->tv_in.tv_sec * USEC_PER_SEC) + new_query->tv_in.tv_usec;
    new_query->created_boot_time = now_boottime_usec();
    new_query->run_after = run_after;

    debug(D_ACLK, "Added query (%s) (%s)", topic, query ? query : "");

    tmp_query = aclk_query_find_position(run_after);

    if (tmp_query) {
        new_query->next = tmp_query->next;
        tmp_query->next = new_query;
        if (tmp_query == aclk_queue.aclk_query_tail)
            aclk_queue.aclk_query_tail = new_query;
        aclk_queue.count++;
        ACLK_QUEUE_UNLOCK;
        QUERY_THREAD_WAKEUP;
        return 0;
    }

    new_query->next = aclk_queue.aclk_query_head;
    aclk_queue.aclk_query_head = new_query;
    aclk_queue.count++;

    ACLK_QUEUE_UNLOCK;
    QUERY_THREAD_WAKEUP;
    return 0;
}

#ifndef __GNUC__
#pragma endregion
#endif

#ifndef __GNUC__
#pragma region Helper Functions
#endif

/*
 * Take a buffer, encode it and rewrite it
 *
 */

static char *aclk_encode_response(char *src, size_t content_size, int keep_newlines)
{
    char *tmp_buffer = mallocz(content_size * 2);
    char *dst = tmp_buffer;
    while (content_size > 0) {
        switch (*src) {
            case '\n':
                if (keep_newlines)
                {
                    *dst++ = '\\';
                    *dst++ = 'n';
                }
                break;
            case '\t':
                break;
            case 0x01 ... 0x08:
            case 0x0b ... 0x1F:
                *dst++ = '\\';
                *dst++ = 'u';
                *dst++ = '0';
                *dst++ = '0';
                *dst++ = (*src < 0x0F) ? '0' : '1';
                *dst++ = to_hex(*src);
                break;
            case '\"':
                *dst++ = '\\';
                *dst++ = *src;
                break;
            default:
                *dst++ = *src;
        }
        src++;
        content_size--;
    }
    *dst = '\0';

    return tmp_buffer;
}

#ifndef __GNUC__
#pragma endregion
#endif

#ifndef __GNUC__
#pragma region ACLK_QUERY
#endif


static usec_t aclk_web_api_request_v1(RRDHOST *host, struct web_client *w, char *url, usec_t q_created)
{
    usec_t t = now_boottime_usec();
    aclk_metric_mat_update(&aclk_metrics_per_sample.cloud_q_recvd_to_processed, t - q_created);

    w->response.code = web_client_api_request_v1(host, w, url);
    t = now_boottime_usec() - t;

    aclk_metric_mat_update(&aclk_metrics_per_sample.cloud_q_db_query_time, t);

    return t;
}

static int aclk_execute_query(struct aclk_query *this_query)
{
    if (strncmp(this_query->query, "/api/v1/", 8) == 0) {
        struct web_client *w = (struct web_client *)callocz(1, sizeof(struct web_client));
        w->response.data = buffer_create(NETDATA_WEB_RESPONSE_INITIAL_SIZE);
        w->response.header = buffer_create(NETDATA_WEB_RESPONSE_HEADER_SIZE);
        w->response.header_output = buffer_create(NETDATA_WEB_RESPONSE_HEADER_SIZE);
        strcpy(w->origin, "*"); // Simulate web_client_create_on_fd()
        w->cookie1[0] = 0;      // Simulate web_client_create_on_fd()
        w->cookie2[0] = 0;      // Simulate web_client_create_on_fd()
        w->acl = 0x1f;

        char *mysep = strchr(this_query->query, '?');
        if (mysep) {
            strncpyz(w->decoded_query_string, mysep, NETDATA_WEB_REQUEST_URL_SIZE);
            *mysep = '\0';
        } else
            strncpyz(w->decoded_query_string, this_query->query, NETDATA_WEB_REQUEST_URL_SIZE);

        mysep = strrchr(this_query->query, '/');

        // TODO: handle bad response perhaps in a different way. For now it does to the payload
        w->tv_in = this_query->tv_in;
        now_realtime_timeval(&w->tv_ready);
        aclk_web_api_request_v1(localhost, w, mysep ? mysep + 1 : "noop", this_query->created_boot_time);
        size_t size = w->response.data->len;
        size_t sent = size;
        w->response.data->date = w->tv_ready.tv_sec;
        web_client_build_http_header(w);  // TODO: this function should offset from date, not tv_ready
        BUFFER *local_buffer = buffer_create(NETDATA_WEB_RESPONSE_INITIAL_SIZE);
        buffer_flush(local_buffer);
        local_buffer->contenttype = CT_APPLICATION_JSON;

        aclk_create_header(local_buffer, "http", this_query->msg_id, 0, 0, aclk_shared_state.version_neg);
        buffer_strcat(local_buffer, ",\n\t\"payload\": ");
        char *encoded_response = aclk_encode_response(w->response.data->buffer, w->response.data->len, 0);
        char *encoded_header = aclk_encode_response(w->response.header_output->buffer, w->response.header_output->len, 1);

        buffer_sprintf(
            local_buffer, "{\n\"code\": %d,\n\"body\": \"%s\",\n\"headers\": \"%s\"\n}", 
            w->response.code, encoded_response, encoded_header);

        buffer_sprintf(local_buffer, "\n}");

        debug(D_ACLK, "Response:%s", encoded_header);

        aclk_send_message(this_query->topic, local_buffer->buffer, this_query->msg_id);

        struct timeval tv;
        now_realtime_timeval(&tv);

        log_access("%llu: %d '[ACLK]:%d' '%s' (sent/all = %zu/%zu bytes %0.0f%%, prep/sent/total = %0.2f/%0.2f/%0.2f ms) %d '%s'",
                   w->id
            , gettid()
            , this_query->idx
            , "DATA"
            , sent
            , size
            , size > sent ? -((size > 0) ? (((size - sent) / (double) size) * 100.0) : 0.0) : ((size > 0) ? (((sent - size ) / (double) size) * 100.0) : 0.0)
            , dt_usec(&w->tv_ready, &w->tv_in) / 1000.0
            , dt_usec(&tv, &w->tv_ready) / 1000.0
            , dt_usec(&tv, &w->tv_in) / 1000.0
            , w->response.code
            , strip_control_characters(this_query->query)
        );

        buffer_free(w->response.data);
        buffer_free(w->response.header);
        buffer_free(w->response.header_output);
        freez(w);
        buffer_free(local_buffer);
        freez(encoded_response);
        freez(encoded_header);
        return 0;
    }
    return 1;
}

static int aclk_execute_query_v2(struct aclk_query *this_query)
{
    int retval = 0;
    usec_t t;
    BUFFER *local_buffer = NULL;
    struct aclk_cloud_req_v2 *cloud_req = (struct aclk_cloud_req_v2 *)this_query->data;

#ifdef NETDATA_WITH_ZLIB
    int z_ret;
    BUFFER *z_buffer = buffer_create(NETDATA_WEB_RESPONSE_INITIAL_SIZE);
    char *start, *end;
#endif

    struct web_client *w = (struct web_client *)callocz(1, sizeof(struct web_client));
    w->response.data = buffer_create(NETDATA_WEB_RESPONSE_INITIAL_SIZE);
    w->response.header = buffer_create(NETDATA_WEB_RESPONSE_HEADER_SIZE);
    w->response.header_output = buffer_create(NETDATA_WEB_RESPONSE_HEADER_SIZE);
    strcpy(w->origin, "*"); // Simulate web_client_create_on_fd()
    w->cookie1[0] = 0;      // Simulate web_client_create_on_fd()
    w->cookie2[0] = 0;      // Simulate web_client_create_on_fd()
    w->acl = 0x1f;

    char *mysep = strchr(this_query->query, '?');
    if (mysep) {
        url_decode_r(w->decoded_query_string, mysep, NETDATA_WEB_REQUEST_URL_SIZE + 1);
        *mysep = '\0';
    } else
        url_decode_r(w->decoded_query_string, this_query->query, NETDATA_WEB_REQUEST_URL_SIZE + 1);

    mysep = strrchr(this_query->query, '/');

    // execute the query
    w->tv_in = this_query->tv_in;
    now_realtime_timeval(&w->tv_ready);
    t = aclk_web_api_request_v1(cloud_req->host, w, mysep ? mysep + 1 : "noop", this_query->created_boot_time);
    size_t size = (w->mode == WEB_CLIENT_MODE_FILECOPY)?w->response.rlen:w->response.data->len;
    size_t sent = size;

#ifdef NETDATA_WITH_ZLIB
    // check if gzip encoding can and should be used
    if ((start = strstr(cloud_req->data, WEB_HDR_ACCEPT_ENC))) {
        start += strlen(WEB_HDR_ACCEPT_ENC);
        end = strstr(start, "\x0D\x0A");
        start = strstr(start, "gzip");

        if (start && start < end) {
            w->response.zstream.zalloc = Z_NULL;
            w->response.zstream.zfree = Z_NULL;
            w->response.zstream.opaque = Z_NULL;
            if(deflateInit2(&w->response.zstream, web_gzip_level, Z_DEFLATED, 15 + 16, 8, web_gzip_strategy) == Z_OK) {
                w->response.zinitialized = 1;
                w->response.zoutput = 1;
            } else
                error("Failed to initialize zlib. Proceeding without compression.");
        }
    }

    if (w->response.data->len && w->response.zinitialized) {
        w->response.zstream.next_in = (Bytef *)w->response.data->buffer;
        w->response.zstream.avail_in = w->response.data->len;
        do {
            w->response.zstream.avail_out = NETDATA_WEB_RESPONSE_ZLIB_CHUNK_SIZE;
            w->response.zstream.next_out = w->response.zbuffer;
            z_ret = deflate(&w->response.zstream, Z_FINISH);
            if(z_ret < 0) {
                if(w->response.zstream.msg)
                    error("Error compressing body. ZLIB error: \"%s\"", w->response.zstream.msg);
                else
                    error("Unknown error during zlib compression.");
                retval = 1;
                goto cleanup;
            }
            int bytes_to_cpy = NETDATA_WEB_RESPONSE_ZLIB_CHUNK_SIZE - w->response.zstream.avail_out;
            buffer_need_bytes(z_buffer, bytes_to_cpy);
            memcpy(&z_buffer->buffer[z_buffer->len], w->response.zbuffer, bytes_to_cpy);
            z_buffer->len += bytes_to_cpy;
        } while(z_ret != Z_STREAM_END);
        // so that web_client_build_http_header
        // puts correct content length into header
        buffer_free(w->response.data);
        w->response.data = z_buffer;
        z_buffer = NULL;
    }
#endif

    w->response.data->date = w->tv_ready.tv_sec;
    web_client_build_http_header(w);
    local_buffer = buffer_create(NETDATA_WEB_RESPONSE_INITIAL_SIZE);
    local_buffer->contenttype = CT_APPLICATION_JSON;

    aclk_create_header(local_buffer, "http", this_query->msg_id, 0, 0, aclk_shared_state.version_neg);
    buffer_sprintf(local_buffer, ",\"t-exec\": %llu,\"t-rx\": %llu,\"http-code\": %d", t, this_query->created, w->response.code);
    buffer_strcat(local_buffer, "}\x0D\x0A\x0D\x0A");
    buffer_strcat(local_buffer, w->response.header_output->buffer);

    if (w->response.data->len) {
#ifdef NETDATA_WITH_ZLIB
        if (w->response.zinitialized) {
            buffer_need_bytes(local_buffer, w->response.data->len);
            memcpy(&local_buffer->buffer[local_buffer->len], w->response.data->buffer, w->response.data->len);
            local_buffer->len += w->response.data->len;
            sent = sent - size + w->response.data->len;
        } else {
#endif
            buffer_strcat(local_buffer, w->response.data->buffer);
#ifdef NETDATA_WITH_ZLIB
        }
#endif
    }

    aclk_send_message_bin(this_query->topic, local_buffer->buffer, local_buffer->len, this_query->msg_id);

    struct timeval tv;
    now_realtime_timeval(&tv);

    log_access("%llu: %d '[ACLK]:%d' '%s' (sent/all = %zu/%zu bytes %0.0f%%, prep/sent/total = %0.2f/%0.2f/%0.2f ms) %d '%s'",
               w->id
        , gettid()
        , this_query->idx
        , "DATA"
        , sent
        , size
        , size > sent ? -((size > 0) ? (((size - sent) / (double) size) * 100.0) : 0.0) : ((size > 0) ? (((sent - size ) / (double) size) * 100.0) : 0.0)
        , dt_usec(&w->tv_ready, &w->tv_in) / 1000.0
        , dt_usec(&tv, &w->tv_ready) / 1000.0
        , dt_usec(&tv, &w->tv_in) / 1000.0
        , w->response.code
        , strip_control_characters(this_query->query)
    );
cleanup:
#ifdef NETDATA_WITH_ZLIB
    if(w->response.zinitialized)
        deflateEnd(&w->response.zstream);
    buffer_free(z_buffer);
#endif
    buffer_free(w->response.data);
    buffer_free(w->response.header);
    buffer_free(w->response.header_output);
    freez(w);
    buffer_free(local_buffer);
    return retval;
}

#define ACLK_HOST_PTR_COMPULSORY(x) \
    if (unlikely(!host)) { \
        errno = 0; \
        error(x " needs host pointer"); \
        break; \
    }

/*
 * This function will fetch the next pending command and process it
 *
 */
static int aclk_process_query(struct aclk_query_thread *t_info)
{
    struct aclk_query *this_query;
    static long int query_count = 0;
    ACLK_METADATA_STATE meta_state;
    RRDHOST *host;

    if (!aclk_connected)
        return 0;

    this_query = aclk_queue_pop();
    if (likely(!this_query)) {
        return 0;
    }

    if (unlikely(this_query->deleted)) {
        debug(D_ACLK, "Garbage collect query %s:%s", this_query->topic, this_query->query);
        aclk_query_free(this_query);
        return 1;
    }
    query_count++;

    host = (RRDHOST*)this_query->data;
    this_query->idx = t_info->idx;

    debug(
        D_ACLK, "Query #%ld (%s) size=%zu in queue %llu ms", query_count, this_query->topic,
        this_query->query ? strlen(this_query->query) : 0, (now_realtime_usec() - this_query->created)/USEC_PER_MS);

    switch (this_query->cmd) {
        case ACLK_CMD_ONCONNECT:
            ACLK_HOST_PTR_COMPULSORY("ACLK_CMD_ONCONNECT");
#if ACLK_VERSION_MIN < ACLK_V_CHILDRENSTATE
            if (host != localhost && aclk_shared_state.version_neg < ACLK_V_CHILDRENSTATE) {
                error("We are not allowed to send connect message in ACLK version before %d", ACLK_V_CHILDRENSTATE);
                break;
            }
#else
#warning "This check became unnecessary. Remove"
#endif

            debug(D_ACLK, "EXECUTING on connect metadata command for host \"%s\" GUID \"%s\"",
                host->hostname,
                host->machine_guid);

            rrdhost_aclk_state_lock(host);
            meta_state = host->aclk_state.metadata;
            host->aclk_state.metadata = ACLK_METADATA_SENT;
            rrdhost_aclk_state_unlock(host);
            aclk_send_metadata(meta_state, host);
            break;

        case ACLK_CMD_CHART:
            ACLK_HOST_PTR_COMPULSORY("ACLK_CMD_CHART");

            debug(D_ACLK, "EXECUTING a chart update command");
            aclk_send_single_chart(host, this_query->query);
            break;

        case ACLK_CMD_CHARTDEL:
            ACLK_HOST_PTR_COMPULSORY("ACLK_CMD_CHARTDEL");

            debug(D_ACLK, "EXECUTING a chart delete command");
            //TODO: This send the info metadata for now
            aclk_send_info_metadata(ACLK_METADATA_SENT, host);
            break;

        case ACLK_CMD_ALARM:
            debug(D_ACLK, "EXECUTING an alarm update command");
            aclk_send_message(this_query->topic, this_query->query, this_query->msg_id);
            break;

        case ACLK_CMD_CLOUD:
            debug(D_ACLK, "EXECUTING a cloud command");
            aclk_execute_query(this_query);
            break;
        case ACLK_CMD_CLOUD_QUERY_2:
            debug(D_ACLK, "EXECUTING Cloud Query v2");
            aclk_execute_query_v2(this_query);
            break;

        case ACLK_CMD_CHILD_CONNECT:
        case ACLK_CMD_CHILD_DISCONNECT:
            ACLK_HOST_PTR_COMPULSORY("ACLK_CMD_CHILD_CONNECT/ACLK_CMD_CHILD_DISCONNECT");

            debug(
                D_ACLK, "Execution Child %s command",
                this_query->cmd == ACLK_CMD_CHILD_CONNECT ? "connect" : "disconnect");
            aclk_send_info_child_connection(host, this_query->cmd);
            break;

        default:
            errno = 0;
            error("Unknown ACLK Query Command");
            break;
    }
    debug(D_ACLK, "Query #%ld (%s) done", query_count, this_query->topic);

    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.queries_dispatched++;
        aclk_queries_per_thread[t_info->idx]++;
        ACLK_STATS_UNLOCK;

        if (likely(getrusage_called_this_tick[t_info->idx] < MAX_GETRUSAGE_CALLS_PER_TICK)) {
            getrusage(RUSAGE_THREAD, &rusage_per_thread[t_info->idx]);
            getrusage_called_this_tick[t_info->idx]++;
        }

    }

    aclk_query_free(this_query);

    return 1;
}

void aclk_query_threads_cleanup(struct aclk_query_threads *query_threads)
{
    if (query_threads && query_threads->thread_list) {
        for (int i = 0; i < query_threads->count; i++) {
            netdata_thread_join(query_threads->thread_list[i].thread, NULL);
        }
        freez(query_threads->thread_list);
    }

    struct aclk_query *this_query;

    do {
        this_query = aclk_queue_pop();
        aclk_query_free(this_query);
    } while (this_query);
}

#define TASK_LEN_MAX 16
void aclk_query_threads_start(struct aclk_query_threads *query_threads)
{
    info("Starting %d query threads.", query_threads->count);

    char thread_name[TASK_LEN_MAX];
    query_threads->thread_list = callocz(query_threads->count, sizeof(struct aclk_query_thread));
    for (int i = 0; i < query_threads->count; i++) {
        query_threads->thread_list[i].idx = i; //thread needs to know its index for statistics

        if(unlikely(snprintf(thread_name, TASK_LEN_MAX, "%s_%d", ACLK_THREAD_NAME, i) < 0))
            error("snprintf encoding error");
        netdata_thread_create(
            &query_threads->thread_list[i].thread, thread_name, NETDATA_THREAD_OPTION_JOINABLE, aclk_query_main_thread,
            &query_threads->thread_list[i]);
    }
}

/**
 * Checks and updates popcorning state of rrdhost
 * returns actual/updated popcorning state
 */

ACLK_POPCORNING_STATE aclk_host_popcorn_check(RRDHOST *host)
{
    rrdhost_aclk_state_lock(host);
    ACLK_POPCORNING_STATE ret = host->aclk_state.state;
    if (host->aclk_state.state != ACLK_HOST_INITIALIZING){
        rrdhost_aclk_state_unlock(host);
        return ret;
    }

    if (!host->aclk_state.t_last_popcorn_update){
        rrdhost_aclk_state_unlock(host);
        return ret;
    }

    time_t t_diff = now_monotonic_sec() - host->aclk_state.t_last_popcorn_update;

    if (t_diff >= ACLK_STABLE_TIMEOUT) {
        host->aclk_state.state = ACLK_HOST_STABLE;
        host->aclk_state.t_last_popcorn_update = 0;
        rrdhost_aclk_state_unlock(host);
        info("Host \"%s\" stable, ACLK popcorning finished. Last interrupt was %ld seconds ago", host->hostname, t_diff);
        return ACLK_HOST_STABLE;
    }

    rrdhost_aclk_state_unlock(host);
    return ret;
}

/**
 * Main query processing thread
 *
 * On startup wait for the agent collectors to initialize
 * Expect at least a time of ACLK_STABLE_TIMEOUT seconds
 * of no new collectors coming in in order to mark the agent
 * as stable (set agent_state = AGENT_STABLE)
 */
void *aclk_query_main_thread(void *ptr)
{
    struct aclk_query_thread *info = ptr;

    while (!netdata_exit) {
        if(aclk_host_popcorn_check(localhost) == ACLK_HOST_STABLE) {
#ifdef ACLK_DEBUG
            _dump_collector_list();
#endif
            break;
        }
        sleep_usec(USEC_PER_SEC * 1);
    }

    while (!netdata_exit) {
        if(aclk_disable_runtime) {
            sleep(1);
            continue;
        }
        ACLK_SHARED_STATE_LOCK;
        if (unlikely(!aclk_shared_state.version_neg)) {
            if (!aclk_shared_state.version_neg_wait_till || aclk_shared_state.version_neg_wait_till > now_monotonic_usec()) {
                ACLK_SHARED_STATE_UNLOCK;
                info("Waiting for ACLK Version Negotiation message from Cloud");
                sleep(1);
                continue;
            }
            errno = 0;
            error("ACLK version negotiation failed. No reply to \"hello\" with \"version\" from cloud in time of %ds."
                " Reverting to default ACLK version of %d.", VERSION_NEG_TIMEOUT, ACLK_VERSION_MIN);
            aclk_shared_state.version_neg = ACLK_VERSION_MIN;
            aclk_set_rx_handlers(aclk_shared_state.version_neg);
        }
        ACLK_SHARED_STATE_UNLOCK;

        rrdhost_aclk_state_lock(localhost);
        if (unlikely(localhost->aclk_state.metadata == ACLK_METADATA_REQUIRED)) {
            if (unlikely(aclk_queue_query("on_connect", localhost, NULL, NULL, 0, 1, ACLK_CMD_ONCONNECT))) {
                rrdhost_aclk_state_unlock(localhost);
                errno = 0;
                error("ACLK failed to queue on_connect command");
                sleep(1);
                continue;
            }
            localhost->aclk_state.metadata = ACLK_METADATA_CMD_QUEUED;
        }
        rrdhost_aclk_state_unlock(localhost);

        ACLK_SHARED_STATE_LOCK;
        if (aclk_shared_state.next_popcorn_host && aclk_host_popcorn_check(aclk_shared_state.next_popcorn_host) == ACLK_HOST_STABLE) {
            aclk_queue_query("on_connect", aclk_shared_state.next_popcorn_host, NULL, NULL, 0, 1, ACLK_CMD_ONCONNECT);
            aclk_shared_state.next_popcorn_host = NULL;
            aclk_update_next_child_to_popcorn();
        }
        ACLK_SHARED_STATE_UNLOCK;

        while (aclk_process_query(info)) {
            // Process all commands
        };

        QUERY_THREAD_LOCK;

        // TODO: Need to check if there are queries awaiting already
        if (unlikely(pthread_cond_wait(&query_cond_wait, &query_lock_wait)))
            sleep_usec(USEC_PER_SEC * 1);

        QUERY_THREAD_UNLOCK;
    }

    return NULL;
}

#ifndef __GNUC__
#pragma endregion
#endif
