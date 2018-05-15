// Copyright 2017-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "http_parser.h"
#include "httpc.h"

static const char *TAG = "httpc";
#ifdef ESP_PLATFORM
#include <esp_log.h>
#else
#define ESP_LOGD(TAG, ...) //printf(__VA_ARGS__);
#define ESP_LOGE(TAG, ...) printf(__VA_ARGS__);
#endif

static int get_port(const char *url, struct http_parser_url *u)
{
    if (u->field_data[UF_PORT].len) {
        return strtol(&url[u->field_data[UF_PORT].off], NULL, 10);
    } else {
        if (strncmp(&url[u->field_data[UF_SCHEMA].off], "http", u->field_data[UF_SCHEMA].len) == 0) {
            return 80;
        } else if (strncmp(&url[u->field_data[UF_SCHEMA].off], "https", u->field_data[UF_SCHEMA].len) == 0) {
            return 443;
        }
    }
    return 0;
}

static bool is_tls(const char *url, struct http_parser_url *u)
{
    /* The reason it is important to check for the schema, instead of the
     * port-number is because some URLs could use a different port number.
     */
    if (strncmp(&url[u->field_data[UF_SCHEMA].off], "https", strlen("https")) == 0) {
        return true;
    }
    return false;
}

static int http_response_read_and_parse(httpc_conn_t *httpc, char *buf, size_t buf_len,
                                        bool discard_buf)
{
    if (discard_buf == true) {
        httpc->request.out_buf = NULL;
        httpc->request.out_buf_len = 0;
        httpc->request.out_buf_index = 0;
    } else {
        httpc->request.out_buf = buf;
        httpc->request.out_buf_len = buf_len;
        httpc->request.out_buf_index = 0;
    }
    /* Read data.*/
    int data_read = esp_tls_conn_read(httpc->tls, buf, buf_len);
    if (data_read < 0) {
        return -1;
    }
    /* Feed the parser */
    int parsed = http_parser_execute(&httpc->request.parser, &httpc->request.parser_settings,
                                     buf, data_read);
    if (parsed != data_read) {
        ESP_LOGE(TAG, "Error in parsing parsed:%d data_read:%d\n", parsed, data_read);
        return -1 ;
    }
    return 0;
}

static int header_parser(httpc_conn_t *httpc, char *buf, size_t buf_len)
{
    while (httpc->request.status < ESP_HTTP_RESP_HDR_RECEIVED) {
        int status = http_response_read_and_parse(httpc, buf, buf_len, false);
        if (status < 0) {
            return -1;
        }
    }
    return 0;
}

httpc_conn_t *http_connection_new(const char *url, struct esp_tls_cfg *tls_cfg)
{
    httpc_conn_t *h;

    /* Parse URI */
    struct http_parser_url u;
    http_parser_url_init(&u);
    http_parser_parse_url(url, strlen(url), 0, &u);

    /* Connect to host */
    struct esp_tls *tls;
    tls = esp_tls_conn_new(&url[u.field_data[UF_HOST].off], u.field_data[UF_HOST].len,
                           get_port(url, &u),
                           is_tls(url, &u) ? tls_cfg : NULL);
    if (!tls) {
        return NULL;
    }

    h = (httpc_conn_t *)calloc(1, sizeof(httpc_conn_t));
    if (!h) {
        goto error;
    }
    h->tls = tls;

    h->host = (char *)calloc(1, u.field_data[UF_HOST].len + 1);
    if (!h->host) {
        goto error;
    }
    strncpy((char *)h->host, &url[u.field_data[UF_HOST].off], u.field_data[UF_HOST].len);
    return h;

error:
    if (h) {
        if (h->host) {
            free(h->host);
        }
        free(h);
    }
    esp_tls_conn_delete(tls);
    return NULL;
}

void http_connection_delete(httpc_conn_t *httpc)
{
    if (httpc->host) {
        free(httpc->host);
    }
    esp_tls_conn_delete(httpc->tls);
    free(httpc);
}

int http_request_send_custom_hdr(httpc_conn_t *httpc, const char *user_hdr)
{
    char *hdr;
    char *req_template;
#define GET_DATA_TEMPLATE      "GET %s HTTP/1.1\r\n"
#define POST_DATA_TEMPLATE     "POST %s HTTP/1.1\r\n"
#define PUT_DATA_TEMPLATE      "PUT %s HTTP/1.1\r\n"
#define DELETE_DATA_TEMPLATE   "DELETE %s HTTP/1.1\r\n"
    switch (httpc->request.op) {
    case ESP_HTTP_GET:
        req_template = GET_DATA_TEMPLATE;
        break;
    case ESP_HTTP_POST:
        req_template = POST_DATA_TEMPLATE;
        break;
    case ESP_HTTP_PUT:
        req_template = PUT_DATA_TEMPLATE;
        break;
    case ESP_HTTP_DELETE:
        req_template = DELETE_DATA_TEMPLATE;
        break;
    default:
        return -1;
        break;
    }
    int hdr_len = strlen(req_template) + strlen(httpc->request.url) + 1;
    hdr = (char *)calloc(1, hdr_len);
    if (!hdr) {
        return -1;
    }
    snprintf(hdr, hdr_len, req_template, httpc->request.url);
    if (esp_tls_conn_write(httpc->tls, hdr, strlen(hdr)) < 0) {
        free(hdr);
        return -1;
    }
    free(hdr);
    /* Send the entire set of headers */
    if (esp_tls_conn_write(httpc->tls, user_hdr, strlen(user_hdr)) < 0) {
        return -1;
    }
    httpc->request.status = ESP_HTTP_REQ_HDR_SENT;
    return 0;
#undef GET_DATA_TEMPLATE
#undef POST_DATA_TEMPLATE
#undef PUT_DATA_TEMPLATE
#undef DELETE_DATA_TEMPLATE
}

static int http_request_send_our_hdr(httpc_conn_t *httpc, size_t data_len)
{
    char *hdr;
    switch (httpc->request.op) {
        const char *op;
    case ESP_HTTP_DELETE:    
    case ESP_HTTP_GET: {
#define GET_DATA_TEMPLATE                       \
"%s %s HTTP/1.1\r\n"                   \
"Host: %s\r\n\r\n"
    	if (httpc->request.op == ESP_HTTP_GET) {
            op = "GET";
    	} else {
    	    op = "DELETE";
    	}

        int hdr_len = strlen(op) + strlen(GET_DATA_TEMPLATE) + strlen(httpc->request.url) +
            strlen(httpc->host) + 1;
        hdr = (char *)calloc(1, hdr_len);
        if (!hdr) {
            return -1;
        }
        snprintf(hdr, hdr_len, GET_DATA_TEMPLATE, op, httpc->request.url,
                httpc->host);
    }
    break;
    case ESP_HTTP_PUT:
    case ESP_HTTP_POST: {
#define POST_DATA_TEMPLATE                      \
"%s %s HTTP/1.1\r\n"                  \
"Host: %s\r\n"                      \
"Content-Length: %zu\r\n"           \
"Content-Type: %s\r\n\r\n"
        if (httpc->request.op == ESP_HTTP_POST) {
    	    op = "POST";
    	} else {
            op = "PUT";
        }
        /* 10 bytes should be sufficient to encode the content-length */
        int hdr_len = strlen(op) + strlen(POST_DATA_TEMPLATE) + strlen(httpc->request.url) +
                      strlen(httpc->host) + 10 + strlen(httpc->request.content_type) + 1;
        hdr = (char *)calloc(1, hdr_len);
        if (!hdr) {
            return -1;
        }
        snprintf(hdr, hdr_len, POST_DATA_TEMPLATE, op, httpc->request.url,
                 httpc->host, data_len, httpc->request.content_type);
    }
    break;
    default:
        return -1;
    }

    /* hdr should be allocated and populated */
    ESP_LOGD(TAG, "Sending hdr: \n%s\n", hdr);

    if (esp_tls_conn_write(httpc->tls, hdr, strlen(hdr)) < 0) {
        free(hdr);
        return -1;
    }
    free(hdr);
    httpc->request.status = ESP_HTTP_REQ_HDR_SENT;
    return 0;
}

int http_request_send(httpc_conn_t *httpc, const char *data, size_t data_len)
{
    if (httpc->request.status < ESP_HTTP_REQ_HDR_SENT) {
        if (http_request_send_our_hdr(httpc, data_len) != 0) {
            return -1;
        }
    }
    if (data && data_len) {
        if (esp_tls_conn_write(httpc->tls, data, data_len) < 0) {
            return -1;
        }
    }
    return 0;
}

void http_response_set_header_cb(httpc_conn_t *httpc, httpc_response_header_cb cb, void *arg)
{
    httpc->request.parser_state.response_hdr_cb = cb;
    httpc->request.parser_state.response_hdr_cb_arg = arg;
}

static void process_hdr_value_pair(httpc_conn_t *httpc)
{
    if (httpc->request.parser_state.hdr_buf[0] == '\0') {
        /* If the header is null, this is probably the first call, ignore it */
        return;
    }

    if (httpc->request.parser_state.response_hdr_cb) {
        (* httpc->request.parser_state.response_hdr_cb)(httpc->request.parser_state.hdr_buf,
                httpc->request.parser_state.val_buf, httpc->request.parser_state.response_hdr_cb_arg);
    }

    if (!strcasecmp(httpc->request.parser_state.hdr_buf, "Content-Type")) {
        memset(httpc->request.response_content_type, 0, sizeof(httpc->request.response_content_type));
        strncpy(httpc->request.response_content_type,
                httpc->request.parser_state.val_buf, sizeof(httpc->request.response_content_type) - 1);
    }
}

static int http_get_hdr_field(http_parser *parser, const char *p, size_t len)
{
    httpc_conn_t *h = parser->data;

    if (!h->request.parser_state.last_was_hdr) {
        /* This is a new header. First process any value from the previous
         * header-value pair
         */
        process_hdr_value_pair(h);

        /* Now start the next header processing.
         */
        if (len >= MAX_HDR_VAL_LEN) {
            len = MAX_HDR_VAL_LEN - 2;
        }
        strncpy(h->request.parser_state.hdr_buf, p, len);
        h->request.parser_state.hdr_buf[len] = '\0';
    } else {
        /* This is a continuation header. Append to the previous header */
        int space_left = MAX_HDR_VAL_LEN - strlen(h->request.parser_state.hdr_buf) - 1;
        int copy_len = len > space_left ? space_left : len;
        strncat(h->request.parser_state.hdr_buf, p, copy_len);
    }

    h->request.parser_state.last_was_hdr = true;
    return 0;
}

static int http_get_hdr_value(http_parser *parser, const char *p, size_t len)
{
    httpc_conn_t *h = parser->data;

    if (h->request.parser_state.last_was_hdr) {
        /* This is a new value.
         */
        if (len >= MAX_HDR_VAL_LEN) {
            len = MAX_HDR_VAL_LEN - 2;
        }
        strncpy(h->request.parser_state.val_buf, p, len);
        h->request.parser_state.val_buf[len] = '\0';
    } else {
        /* This is a continuation value. Append to the previous value */
        int space_left = MAX_HDR_VAL_LEN - strlen(h->request.parser_state.val_buf) - 1;
        int copy_len = len > space_left ? space_left : len;
        strncat(h->request.parser_state.val_buf, p, copy_len);
    }

    h->request.parser_state.last_was_hdr = false;
    return 0;
}

static int http_get_body(http_parser *parser, const char *p, size_t len)
{
    httpc_conn_t *h = parser->data;
    if (!h->request.out_buf) {
        /* We just have to flush the data out, the user isn't interested in getting
         * a copy of the data. So do nothing...
         */
        return 0;
    }

    if (len + h->request.out_buf_index > h->request.out_buf_len) {
        /* How is this possible, we ensured that we will never read more than
         * the user's buffer length.
         */
        ESP_LOGE(TAG, "ASSERT: This shouldn't happen\n");
        return -1;
    }
    memcpy(h->request.out_buf + h->request.out_buf_index, p, len);
    h->request.out_buf_index += len;

    return 0;
}

static int http_hdr_complete(http_parser *parser)
{
    httpc_conn_t *h = parser->data;

    h->request.status = ESP_HTTP_RESP_HDR_RECEIVED;
    ESP_LOGD(TAG, "on-hdr-complete: resp_code %d  resp_len %zu\n",
             http_response_get_code(h),
             http_response_get_content_len(h));
    process_hdr_value_pair(parser->data);

    return 0;
}

static int http_message_complete(http_parser *parser)
{
    httpc_conn_t *h = parser->data;
    h->request.status = ESP_HTTP_RESP_BDY_RECEIVED;
    return 0;
}

static char *http_get_correct_path(const char *url)
{
    if (url[0] == '/') {
        /* This is the correct path */
        return strdup(url);
    } else {
        /* It appears that the user passed the entire URL including the
         * hostname. Get the path component.
         */
        struct http_parser_url u;
        http_parser_url_init(&u);
        http_parser_parse_url(url, strlen(url), 0, &u);
        if (u.field_data[UF_PATH].len) {
	    /* The path may be followed by some URL parameters which
	     * also need to be sent out
	     */
            char *path = calloc(1, strlen(url) - u.field_data[UF_PATH].off + 1);
            if (!path) {
                return NULL;
            }
            strncpy(path, &url[u.field_data[UF_PATH].off],
                    strlen(url) - u.field_data[UF_PATH].off + 1);
            return path;
        } else {
            /* No path, implies '/' */
            return strdup("/");
        }
    }
}

int http_request_new(httpc_conn_t *httpc, httpc_ops_t op, const char *url)
{
    if (httpc->request.status != ESP_HTTP_REQ_NEW) {
        /* There may be left-over data in here from the previous request on
         * the same socket, flush it out so that the next request works
         * properly.
         */
        char buf[50];
        while (httpc->request.status < ESP_HTTP_RESP_BDY_RECEIVED) {
            int status = http_response_read_and_parse(httpc, buf, sizeof(buf), true);
            if (status < 0) {
                return -1;
            }
        }
    }
    memset(&httpc->request, 0, sizeof(httpc->request));
    httpc->request.op = op;
    httpc->request.url = http_get_correct_path(url);
    if (! httpc->request.url) {
        return -1;
    }
    httpc->request.content_type = DEFAULT_CONTENT_TYPE;
    httpc->request.status = ESP_HTTP_REQ_NEW;
    http_parser_init(&httpc->request.parser, HTTP_RESPONSE);
    httpc->request.parser.data = httpc;
    httpc->request.parser_settings.on_headers_complete = http_hdr_complete;
    httpc->request.parser_settings.on_body = http_get_body;
    httpc->request.parser_settings.on_message_complete = http_message_complete;
    httpc->request.parser_settings.on_header_field = http_get_hdr_field;
    httpc->request.parser_settings.on_header_value = http_get_hdr_value;
    return 0;
}

void http_request_delete(httpc_conn_t *httpc)
{
    if (httpc->request.url) {
        free((void *)httpc->request.url);
    }
    /* We don't memset to 0 in here, since there could be data not fetched from
     * the buffer. We fetch and discard that lazily in the next new()
     */
}

int http_response_recv(httpc_conn_t *httpc, char *buf, size_t buf_len)
{
    /* This could either be called AFTER a header parsing API, or immediately
     * after sending the request. If the headers aren't parsed, we need to parse
     * them in here.
     */
    if (httpc->request.status < ESP_HTTP_RESP_STARTED) {
        httpc->request.status = ESP_HTTP_RESP_STARTED;
    }
    if ( httpc->request.status < ESP_HTTP_RESP_HDR_RECEIVED ) {
        int status = header_parser(httpc, buf, buf_len);
        if (status < 0) {
            return -1;
        }
        /* At this point it is likely that the on-body received was already called
         * and some data was copied into the user buffers. If that is the case,
         * just return the data right from here.
         */
        if (httpc->request.out_buf_index) {
            return httpc->request.out_buf_index;
        }
    }

    /* If httpc->request.body_read_in_header is set as true it implies that
     * http_header_fetch() API call was made before http_response_recv() API call
     * and there is dynamically allocated buffer which contains some bits of the
     * body which needs to be emptied.
     */
    if (httpc->request.status < ESP_HTTP_RESP_BDY_RECEIVED &&
            httpc->request.hdr_overflow_buf) {
        int total_len = httpc->request.hdr_overflow_buf_len -
                        httpc->request.hdr_overflow_buf_index;
        int  copy_len = buf_len <= total_len ? buf_len : total_len;
        memcpy(buf, httpc->request.hdr_overflow_buf +
               httpc->request.hdr_overflow_buf_index,
               copy_len);
        if (copy_len == total_len) {
            free(httpc->request.hdr_overflow_buf);
            httpc->request.hdr_overflow_buf = NULL;
        } else {
            httpc->request.hdr_overflow_buf_index += copy_len;
        }
        return copy_len;
    }
    if (httpc->request.status < ESP_HTTP_RESP_BDY_RECEIVED) {
        int status  = http_response_read_and_parse(httpc, buf, buf_len, false);
        if (status < 0) {
            return -1;
        }
        return httpc->request.out_buf_index;
    }
    /* End of data */
    return 0;
}

void http_header_fetch(httpc_conn_t *httpc)
{
    if (httpc->request.status < ESP_HTTP_RESP_STARTED) {
        httpc->request.status = ESP_HTTP_RESP_STARTED;
        httpc->request.hdr_overflow_buf = (char *)malloc(BUF_SIZE);
        int status =  header_parser(httpc, httpc->request.hdr_overflow_buf, BUF_SIZE);
        if (status < 0) {
            return;
        }
        if (httpc->request.out_buf_index) {
            httpc->request.hdr_overflow_buf_len = httpc->request.out_buf_index;
            httpc->request.hdr_overflow_buf_index = 0;
            return;
        } else {
            free(httpc->request.hdr_overflow_buf);
            httpc->request.hdr_overflow_buf = NULL;
        }
    } else {
        return;
    }
}

httpc_conn_t *http_simple_get_new(const char *url, struct esp_tls_cfg *tls_cfg)
{
    httpc_conn_t *h = http_connection_new(url, tls_cfg);
    if (!h) {
        ESP_LOGE(TAG, "Couldn't open connection\n");
        goto connection_failed;
    }

    int req_err = http_request_new(h, ESP_HTTP_GET, url);
    if (req_err < 0) {
        ESP_LOGE(TAG, "http new request failed");
        goto request_failed;
    }

    int send_err = http_request_send(h, NULL, 0);
    if (send_err < 0) {
        ESP_LOGE(TAG, "http send request failed");
        goto send_failed;
    }

    return h;

send_failed:
    http_request_delete(h);

request_failed:
    http_connection_delete(h);

connection_failed:
    return NULL;
}

void http_simple_get_delete(httpc_conn_t *h)
{
    http_request_delete(h);
    http_connection_delete(h);
}

