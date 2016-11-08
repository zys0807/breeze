#include "common.h"
#include "http.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef enum _parser_state {
    PARSER_STATE_BAD_REQUEST = -1,
    PARSER_STATE_COMPLETE = 0,
    PARSER_STATE_METHOD,
    PARSER_STATE_PATH,
    PARSER_STATE_QUERY_STR,
    PARSER_STATE_VERSION,
    PARSER_STATE_HEADER_NAME,
    PARSER_STATE_HEADER_COLON,
    PARSER_STATE_HEADER_VALUE,
    PARSER_STATE_HEADER_CR,
    PARSER_STATE_HEADER_LF,
    PARSER_STATE_HEADER_COMPLETE_CR,
} parser_state_e;


typedef void (*http_header_callback)(request_t *req, http_header_t *header);

typedef struct _header_command {
    char                   *lower_name;
    http_header_callback   callback;
} header_command_t;

static struct hsearch_data std_headers_hash;
static int std_headers_hash_initialized = 0;

static http_version_e _resolve_http_version(const char* version_str);
static int init_std_headers_hash();
static void handle_common_header(request_t *req, int header_index);
static void set_common_headers(response_t *resp);
static void on_write_finished(iostream_t *stream);

inline static const char* str_http_ver(http_version_e ver) {
    switch (ver) {
    case HTTP_VERSION_1_1:
        return "1.1";

    case HTTP_VERSION_1_0:
        return "1.0";

    case HTTP_VERSION_0_9:
        return "0.9";

    default:
        return "1.1";   
    }
}

request_t* request_create(connection_t *conn) {
    request_t  *req;
    req = (request_t*) calloc(1, sizeof(request_t));
    if (req == NULL) {
        return NULL;
    }
    bzero(req, sizeof(request_t));
    if (hcreate_r(MAX_HEADER_SIZE, &req->_header_hash) == 0) {
        error("Error creating header hash table");
        free(req);
        return NULL;
    }
    req->_conn = conn;
    return req;
}

int request_reset(request_t *req) {
    connection_t *conn = req->_conn;
    hdestroy_r(&req->_header_hash);
    bzero(req, sizeof(request_t));
    if (hcreate_r(MAX_HEADER_SIZE, &req->_header_hash) == 0) {
        error("Error creating header hash table");
        return -1;
    }    
    req->_conn = conn;
    return 0;
}

int request_destroy(request_t *req) {
    hdestroy_r(&req->_header_hash);
    free(req);
    return 0;
}

const char*  request_get_header(request_t *request, const char *header_name) {
    ENTRY          item, *ret;
    char           header_name_lower[64];

    strlowercase(header_name, header_name_lower, 64);
    item.key = header_name_lower;

    if (hsearch_r(item, FIND, &ret, &request->_header_hash) == 0) {
        return NULL;
    }

    return ((http_header_t*) ret->data)->value;
}

#define START_NEW_TOKEN(tok, req)                       \
    (tok = (req)->_buffer + (req)->_buf_idx)

#define FILL_NEXT_CHAR(req, ch)                         \
    ((req)->_buffer[req->_buf_idx++] = (ch))

#define FINISH_CUR_TOKEN(req)                           \
    ((req)->_buffer[(req)->_buf_idx++] = '\0' )

#define EXPECT_CHAR(state, ch, expected_ch, next_state) \
    if ((ch) == (expected_ch)) {                        \
        state = (next_state);                           \
    } else {                                            \
        state = PARSER_STATE_BAD_REQUEST;               \
    }


int request_parse_headers(request_t *req,
                          const char *data,
                          const size_t data_len,
                          size_t *consumed) {
    http_version_e      ver;
    int                 i, rc;
    char                ch;
    char                *cur_token = req->_buffer;
    parser_state_e      state = PARSER_STATE_METHOD;

    req->_buffer[0] = '\0';
    req->_buf_idx = 0;
    req->header_count = 0;

    if (std_headers_hash_initialized == 0) {
        init_std_headers_hash();
    }

    for (i = 0; i < data_len;){

        if (state == PARSER_STATE_COMPLETE ||
             state == PARSER_STATE_BAD_REQUEST) {
            break;
        } 
        ch = data[i++];

        switch(state) {

            case PARSER_STATE_METHOD:
                if (ch == ' ') {
                    FINISH_CUR_TOKEN(req);
                    req->method = cur_token;
                    state = PARSER_STATE_PATH;
                    START_NEW_TOKEN(cur_token, req);
                } else if (ch < 'A' || ch > 'Z') {
                    state = PARSER_STATE_BAD_REQUEST;
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_PATH:
                if (ch == '?') {
                    FINISH_CUR_TOKEN(req);
                    req->path = cur_token;
                    state = PARSER_STATE_QUERY_STR;
                    START_NEW_TOKEN(cur_token, req);
                } else if (ch == ' ') {
                    FINISH_CUR_TOKEN(req);
                    req->path = cur_token;
                    state = PARSER_STATE_VERSION;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_QUERY_STR:
                if (ch == ' ') {
                    FINISH_CUR_TOKEN(req);
                    req->query_str = cur_token;
                    state = PARSER_STATE_VERSION;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_VERSION:
                switch (ch) {
                    // For HTTP part in the request line, e.g. GET / HTTP/1.1
                    case 'H':
                    case 'T':
                    case 'P':

                    // Currently only 0.9, 1.0 and 1.1 are supported.
                    case '0':
                    case '1':
                    case '9':
                    case '.':
                        FILL_NEXT_CHAR(req, ch);
                        break;

                    case '/':
                        FINISH_CUR_TOKEN(req);
                        if (strcmp("HTTP", cur_token) != 0) {
                            state = PARSER_STATE_BAD_REQUEST;
                            break;
                        }
                        START_NEW_TOKEN(cur_token, req);
                        break;

                    case '\r':
                        FINISH_CUR_TOKEN(req);
                        ver = _resolve_http_version(cur_token);
                        if (ver == HTTP_VERSION_UNKNOW) {
                            state = PARSER_STATE_BAD_REQUEST;
                            break;
                        }
                        req->version = ver;
                        state = PARSER_STATE_HEADER_CR;
                        START_NEW_TOKEN(cur_token, req);
                        break;
                }
                break;

            case PARSER_STATE_HEADER_NAME:
                if (ch == ':') {
                    FINISH_CUR_TOKEN(req);
                    req->headers[req->header_count].name = cur_token;
                    state = PARSER_STATE_HEADER_COLON;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_HEADER_COLON:
                EXPECT_CHAR(state, ch, ' ', PARSER_STATE_HEADER_VALUE);
                break;

            case PARSER_STATE_HEADER_VALUE:
                if (ch == '\r') {
                    FINISH_CUR_TOKEN(req);
                    req->headers[req->header_count].value = cur_token;
                    handle_common_header(req, req->header_count);
                    req->header_count++;
                    state = PARSER_STATE_HEADER_CR;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_HEADER_CR:
                EXPECT_CHAR(state, ch, '\n', PARSER_STATE_HEADER_LF);
                break;

            case PARSER_STATE_HEADER_LF:
                // Another CR after a header LF, meanning the header end is met.
                if (ch == '\r') { 
                    state = PARSER_STATE_HEADER_COMPLETE_CR; 
                } else { 
                    state = PARSER_STATE_HEADER_NAME; 
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_HEADER_COMPLETE_CR:
                EXPECT_CHAR(state, ch, '\n', PARSER_STATE_COMPLETE);
                break;

            default:
                error("Unexpected state: %d", state);
                break;

        }
    }

    *consumed = i;

    switch (state) {
        case PARSER_STATE_COMPLETE:
            rc = STATUS_COMPLETE;
            break;

        case PARSER_STATE_BAD_REQUEST:
            rc = STATUS_ERROR;
            break;

        default:
            rc = STATUS_INCOMPLETE;
            break;
    }

    return rc;
}

static http_version_e _resolve_http_version(const char* version_str) {
    if (strcmp(version_str, "1.1") == 0) {
        return HTTP_VERSION_1_1;
    } else if (strcmp(version_str, "1.0") == 0) {
        return HTTP_VERSION_1_0;
    } else if (strcmp(version_str, "0.9") == 0) {
        return HTTP_VERSION_0_9;
    }

    return HTTP_VERSION_UNKNOW;
}

static void _handle_content_len(request_t *req, http_header_t *header) {
    size_t   content_length;

    content_length = (size_t) atol(header->value);
    req->content_length = content_length;
}

static void _handle_host(request_t *req, http_header_t *header) {
    req->host = header->value;
}

static void _handle_connection(request_t *req, http_header_t *header) {
    if (strcasecmp("keep-alive", header->value) == 0) {
        req->connection = CONN_KEEP_ALIVE;
    } else {
        req->connection = CONN_CLOSE;
    }
}

static header_command_t std_headers[] = {
    { "accept", NULL },
    { "accept-charset", NULL },
    { "accept-datetime", NULL },
    { "accept-encoding", NULL },
    { "accept-language", NULL },
    { "accept-ranges", NULL },
    { "access-control-allow-origin", NULL },
    { "age", NULL },
    { "allow", NULL },
    { "authorization", NULL },
    { "cache-control", NULL },
    { "connection", _handle_connection },
    { "content-disposition", NULL },
    { "content-encoding", NULL },
    { "content-language", NULL },
    { "content-length", _handle_content_len },
    { "content-location", NULL },
    { "content-md5", NULL },
    { "content-range", NULL },
    { "content-security-policy", NULL },
    { "content-type", NULL },
    { "cookie", NULL },
    { "dnt", NULL },
    { "date", NULL },
    { "etag", NULL },
    { "expect", NULL },
    { "expires", NULL },
    { "from", NULL },
    { "front-end-https", NULL },
    { "host", _handle_host },
    { "if-match", NULL },
    { "if-modified-since", NULL },
    { "if-none-match", NULL },
    { "if-range", NULL },
    { "if-unmodified-since", NULL },
    { "last-modified", NULL },
    { "link", NULL },
    { "location", NULL },
    { "max-forwards", NULL },
    { "origin", NULL },
    { "p3p", NULL },
    { "pragma", NULL },
    { "proxy-authenticate", NULL },
    { "proxy-authorization", NULL },
    { "proxy-connection", NULL },
    { "range", NULL },
    { "referer", NULL },
    { "refresh", NULL },
    { "retry-after", NULL },
    { "server", NULL },
    { "set-cookie", NULL },
    { "status", NULL },
    { "strict-transport-security", NULL },
    { "te", NULL },
    { "trailer", NULL },
    { "transfer-encoding", NULL },
    { "upgrade", NULL },
    { "user-agent", NULL },
    { "vary", NULL },
    { "via", NULL },
    { "www-authenticate", NULL },
    { "warning", NULL },
    { "x-att-deviceid", NULL },
    { "x-content-security-policy", NULL },
    { "x-content-type-options", NULL },
    { "x-forwarded-for", NULL },
    { "x-forwarded-proto", NULL },
    { "x-frame-options", NULL },
    { "x-powered-by", NULL },
    { "x-requested-with", NULL },
    { "x-wap-profile", NULL },
    { "x-webkit-csp", NULL },
    { "x-xss-protection", NULL },
    { "x-ua-compatible", NULL },
};

static int init_std_headers_hash() {
    int i;
    size_t size;
    ENTRY item, *ret;

    size = sizeof(std_headers)/sizeof(std_headers[0]);

    bzero(&std_headers_hash, sizeof(struct hsearch_data));
    if (hcreate_r(sizeof(std_headers) * 2, &std_headers_hash) == 0) {
        error("Error creating standard headers hash");
        return -1;
    }
    for (i = 0; i < size; i++) {
        item.key = std_headers[i].lower_name;
        item.data = std_headers[i].callback;
        if (hsearch_r(item, ENTER, &ret, &std_headers_hash) == 0) {
            error("Error entering standard header %s to hash", item.key);
        }
    }
    std_headers_hash_initialized = 1;
    return 0;
}

static void handle_common_header(request_t *req, int header_index) {
    ENTRY           ent, *ret;
    http_header_t  *header;
    char            header_lowercase[64];
    http_header_callback callback = NULL;

    header = req->headers + header_index;
    strlowercase(header->name, header_lowercase, 64);
    ent.key = header_lowercase;
    if (hsearch_r(ent, FIND, &ret, &std_headers_hash) == 0) {
        return;
    }

    callback = (http_header_callback) ret->data;
    if (callback != NULL) {
        callback(req, header);
    }

    ent.key = ret->key;
    ent.data = header;

    hsearch_r(ent, ENTER, &ret, &req->_header_hash);
}


// HTTP common status codes

// 1xx informational
http_status_t STATUS_CONTINUE = {100, "Continue"};

// 2xx success
http_status_t STATUS_OK = {200, "OK"};
http_status_t STATUS_CREATED = {201, "Created"};
http_status_t STATUS_ACCEPTED = {202, "Accepted"};
http_status_t STATUS_NO_CONTENT = {204, "No Content"};
http_status_t STATUS_PARTIAL_CONTENT = {206, "Partial Content"};

// 3xx redirection
http_status_t STATUS_MOVED = {301, "Moved Permanently"};
http_status_t STATUS_FOUND = {302, "Found"};
http_status_t STATUS_SEE_OTHER = {303, "See Other"};
http_status_t STATUS_NOT_MODIFIED = {304, "Not Modified"};

// 4xx client errors
http_status_t STATUS_BAD_REQUEST = {400, "Bad Request"};
http_status_t STATUS_UNAUTHORIZED = {401, "Unauthorized"};
http_status_t STATUS_FORBIDDEN = {403, "Forbidden"};
http_status_t STATUS_NOT_FOUND = {404, "Not Found"};
http_status_t STATUS_METHOD_NOT_ALLOWED = {405, "Method Not Allowed"};
http_status_t STATUS_RANGE_NOT_SATISFIABLE = {416, "Range Not Satisfiable"};

// 5xx server errors
http_status_t STATUS_INTERNAL_ERROR = {500, "Internal Server Error"};
http_status_t STATUS_NOT_IMPLEMENTED = {501, "Not Implemented"};
http_status_t STATUS_BAD_GATEWAY = {502, "Bad Gateway"};
http_status_t STATUS_SERVICE_UNAVAILABLE = {503, "Service Unavailable"};
http_status_t STATUS_GATEWAY_TIMEOUT = {504, "Gateway Timeout"};


response_t* response_create(connection_t *conn) {
    response_t   *resp;

    resp = (response_t*) calloc(1, sizeof(response_t));
    if (resp == NULL) {
        return NULL;
    }

    bzero(resp, sizeof(response_t));
    if (hcreate_r(MAX_HEADER_SIZE, &resp->_header_hash) == 0) {
        error("Error creating header hash table");
        free(resp);
        return NULL;
    }

    resp->_conn = conn;
    // Content Length of -1 means we do not handle content length
    resp->content_length = -1;
    resp->connection = CONN_KEEP_ALIVE;
    return resp;
}

int response_reset(response_t *resp) {
    connection_t  *conn = resp->_conn;
    hdestroy_r(&resp->_header_hash);
    bzero(resp, sizeof(response_t));
    if (hcreate_r(MAX_HEADER_SIZE, &resp->_header_hash) == 0) {
        error("Error creating header hash table");
        return -1;
    }
    resp->_conn = conn;
    resp->content_length = -1;
    resp->connection = CONN_KEEP_ALIVE;
    return 0;
}

int response_destroy(response_t *resp) {
    hdestroy_r(&resp->_header_hash);
    free(resp);
    return 0;
}

const char* response_get_header(response_t *resp, const char *header_name) {
    ENTRY          item, *ret;
    char           header_name_lower[64];

    strlowercase(header_name, header_name_lower, 64);
    item.key = header_name_lower;

    if (hsearch_r(item, FIND, &ret, &resp->_header_hash) == 0) {
        return NULL;
    }

    return ((http_header_t*) ret->data)->value;
}

char* response_alloc(response_t *resp, size_t n) {
    char *res;
    if (resp->_buf_idx + n > RESPONSE_BUFFER_SIZE) {
        return NULL;
    }
    res = resp->_buffer + resp->_buf_idx;
    resp->_buf_idx += n;
    return res;
}

int response_set_header(response_t *resp, char *header_name, char *header_value) {
    ENTRY          ent, *ret;
    http_header_t  *header;
    char           header_lowercase[64];
    char           *keybuf;
    size_t         n;

    if (resp->_header_sent) {
        return -1;
    }
    strlowercase(header_name, header_lowercase, 64);
    ent.key = header_lowercase;
    if (hsearch_r(ent, FIND, &ret, &resp->_header_hash) != 0) {
        header = (http_header_t*) ret->data;
        header->value = header_value;
        return 0;
    }

    header = resp->headers + resp->header_count++;
    header->name = header_name;
    header->value = header_value;

    if (hsearch_r(ent, FIND, &ret, &std_headers_hash) != 0) {
        ent.key = ret->key;
    } else {
        n = strlen(header_lowercase) + 1;
        keybuf = response_alloc(resp, n);
        if (keybuf == NULL) {
            return -1;
        }
        ent.key = strncpy(keybuf, header_lowercase, n);
    }
    ent.data = header;
    if (hsearch_r(ent, ENTER, &ret, &resp->_header_hash) == 0) {
        return -1;
    }
    return 0;
}

int response_set_header_printf(response_t *resp, char* name,
                               const char *fmt, ...) {
    char *buf = resp->_buffer + resp->_buf_idx;
    size_t max_len = RESPONSE_BUFFER_SIZE - resp->_buf_idx;
    int len = 0, res;
    va_list ap;
    va_start(ap, fmt);
    len = vsnprintf(buf, max_len, fmt, ap);
    va_end(ap);
    res = response_set_header(resp, name, buf);
    if (res == 0) {
        // Only truly allocate the buffer when the operation succeeds.
        resp->_buf_idx += (len + 1);
    }
    return res;
}

static void set_common_headers(response_t *resp) {
    char *keybuf, *tmbuf;
    size_t len = resp->content_length;
    int n;
    
    if (len >= 0) {
        keybuf = resp->_buffer + resp->_buf_idx;
        n = snprintf(keybuf,
                     RESPONSE_BUFFER_SIZE - resp->_buf_idx,
                     "%ld",
                     resp->content_length);

        resp->_buf_idx += (n + 1);
        response_set_header(resp, "Content-Length", keybuf);
    } else {
        // No Content-Length header set, set connection header
        // to close
        // TODO Support chunked transfer encoding
        resp->connection = CONN_CLOSE;
    }

    switch(resp->connection) {
    case CONN_KEEP_ALIVE:
        //TODO Handle keep-alive time
        response_set_header(resp, "Connection", "keep-alive");
        break;

    default:
        response_set_header(resp, "Connection", "close");
        break;
    }

    response_set_header(resp, "Server", _BREEZE_NAME);
    tmbuf = response_alloc(resp, 32);
    if (current_http_date(tmbuf, 32) == 0) {
        response_set_header(resp, "Date", tmbuf);
    }
}

int response_send_headers(response_t *resp, handler_func next_handler) {
    char           buffer[RESPONSE_BUFFER_SIZE];
    int            i, n, buf_len = 0;
    http_header_t  *header;

    if (resp->_header_sent) {
        return -1;
    }
    
    set_common_headers(resp);
    // Write status line
    n = snprintf(buffer,
                 RESPONSE_BUFFER_SIZE,
                 "HTTP/%s %d %s\r\n",
                 str_http_ver(resp->version),
                 resp->status.code,
                 resp->status.msg);
    buf_len += n;

    // Write headers
    for (i = 0; i < resp->header_count; i++) {
        header = resp->headers + i;
        n = snprintf(buffer + buf_len,
                     RESPONSE_BUFFER_SIZE - buf_len,
                     "%s: %s\r\n",
                     header->name,
                     header->value);
        buf_len += n;
    }

    // Empty line between headers and body
    buffer[buf_len++] = '\r';
    buffer[buf_len++] = '\n';

    resp->_next_handler = next_handler;
    if (iostream_write(resp->_conn->stream, buffer, buf_len, on_write_finished) < 0) {
        return -1;
    }
    resp->_header_sent = 1;
    return 0;
}

int response_write(response_t *resp,
                   char *data, size_t data_len,
                   handler_func next_handler) {
    if (!resp->_header_sent) {
        return -1;
    }
    resp->_next_handler = next_handler;
    if (iostream_write(resp->_conn->stream,
                       data, data_len, on_write_finished) < 0) {
        connection_close(resp->_conn);
        return -1;
    }
    return 0;
}

int response_send_file(response_t *resp,
                       int fd,
                       size_t offset,
                       size_t size,
                       handler_func next_handler) {
    if (!resp->_header_sent) {
        return -1;
    }
    resp->_next_handler = next_handler;
    if (iostream_sendfile(resp->_conn->stream, fd,
                          offset, size, on_write_finished) < 0) {
        connection_close(resp->_conn);
        return -1;
    }
    return 0;
}

const char *status_msg_template = "<html>"
    "<head><title>%d %s</title></head>"
    "<body>"
    "<center><h2>%d %s</h2></center>"
    "<center>Please contact website administrator to report the problem.</center>"
    "<hr/>"
    "<center>"
    "Powered by <a href=\"https://github.com/moonranger/breeze\" target=\"_blank\">%s</a>"
    "</center>"
    "</body>"
    "</html>";

int response_send_status(response_t *resp, http_status_t status) {
    size_t len = 0;
    char   buf[1024];
    
    resp->status = status;
    response_set_header(resp, "Content-Type", "text/html");
    snprintf(buf, 1024, status_msg_template,
             status.code, status.msg,
             status.code, status.msg,
             _BREEZE_NAME);
    len = strlen(buf);

    resp->content_length = len;
    response_send_headers(resp, NULL);
    response_write(resp, buf, len, NULL);
    return HANDLER_DONE;
}

static void on_write_finished(iostream_t *stream) {
    connection_t  *conn;
    handler_func  handler;
    response_t    *resp;
    
    conn = (connection_t*) stream->user_data;
    resp = conn->response;
    handler = resp->_next_handler;

    if (handler != NULL) {
        connection_run_handler(conn, handler);
    }
    if (resp->_done) {
        switch (resp->connection) {
        case CONN_CLOSE:
            connection_close(conn);
            break;

        case CONN_KEEP_ALIVE:
        default:
            if (request_reset(conn->request) < 0) {
                connection_close(conn);
                break;
            }
            if (response_reset(conn->response) < 0) {
                connection_close(conn);
                break;
            }
            if (context_reset(conn->context) < 0) {
                connection_close(conn);
                break;
            }
            conn->context->conf = conn->server->handler_conf;
            connection_run(conn);
            break;
        }
    }
}

handler_ctx_t* context_create() {
    handler_ctx_t *ctx = (handler_ctx_t*) calloc(1, sizeof(handler_ctx_t));
    return ctx;
}

int context_reset(handler_ctx_t *ctx) {
    ctx->_stat_top = 0;
    ctx->conf = NULL;
    return 0;
}

int context_destroy(handler_ctx_t *ctx) {
    free(ctx);
    return 0;
}

int context_push(handler_ctx_t *ctx, ctx_state_t stat) {
    if (ctx->_stat_top >= MAX_STATE_STACK_SIZE) {
        return -1;
    }
    ctx->_stat_stack[ctx->_stat_top] = stat;
    ctx->_stat_top++;
    return 0;
}

ctx_state_t* context_pop(handler_ctx_t *ctx) {
    if (ctx->_stat_top <= 0) {
        return NULL;
    }
    ctx->_stat_top--;
    return &ctx->_stat_stack[ctx->_stat_top];
}

ctx_state_t* context_peek(handler_ctx_t *ctx) {
    if (ctx->_stat_top <= 0) {
        return NULL;
    }
    return &ctx->_stat_stack[ctx->_stat_top - 1];
}
