#include "vk-common.h"

#include "httputils.h"
#include "miscutils.h"

using std::bind;
using std::move;
using namespace std::placeholders;

namespace
{

// Returns keepalive pool for all the HTTP connections in PurpleConnection.
PurpleHttpKeepalivePool* get_keepalive_pool(PurpleConnection* gc)
{
    VkConnData* data = get_conn_data(gc);
    if (!data->keepalive_pool)
        data->keepalive_pool = purple_http_keepalive_pool_new();
    return data->keepalive_pool;
}

} // End of anonymous namespace

void destroy_keepalive_pool(PurpleConnection* gc)
{
    VkConnData* data = get_conn_data(gc);
    if (data->keepalive_pool)
        purple_http_keepalive_pool_unref(data->keepalive_pool);
}

PurpleHttpConnection* http_get(PurpleConnection* gc, const string& url, HttpCallback callback)
{
    PurpleHttpRequest* request = purple_http_request_new(url.data());
    PurpleHttpConnection* hc = http_request(gc, request, move(callback));
    purple_http_request_unref(request);
    return hc;
}

namespace
{

// Callback helper for http_request.
void http_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, void* user_data)
{
    HttpCallback* callback = (HttpCallback*)user_data;
    (*callback)(http_conn, response);
    delete callback;
}

} // End anonymous namespace

PurpleHttpConnection* http_request(PurpleConnection* gc, PurpleHttpRequest* request, HttpCallback callback)
{
    purple_http_request_set_keepalive_pool(request, get_keepalive_pool(gc));
    HttpCallback* user_data = new HttpCallback(move(callback));
    PurpleHttpConnection* hc = purple_http_request(gc, request, http_cb, user_data);
    return hc;
}

namespace
{

// A helper callback for purple_http_request_update_on_redirect. TODO: check for infinite loops.
void http_request_redirect_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, HttpCallback callback)
{
    if (purple_http_response_get_code(response) == 302) {
        PurpleConnection* gc = purple_http_conn_get_purple_connection(http_conn);
        PurpleHttpRequest* request = purple_http_conn_get_request(http_conn);
        const char* new_url = purple_http_response_get_header(response, "Location");
        purple_http_request_set_url(request, new_url);
        http_request(gc, request, bind(http_request_redirect_cb, _1, _2, move(callback)));
    } else {
        callback(http_conn, response);
    }
}

} // End anonymous namespace

PurpleHttpConnection* http_request_update_on_redirect(PurpleConnection* gc, PurpleHttpRequest* request,
                                                      HttpCallback callback)
{
    purple_http_request_set_max_redirects(request, 0);

    return http_request(gc, request, bind(http_request_redirect_cb, _1, _2, move(callback)));
}


void http_request_copy_cookie_jar(PurpleHttpRequest* target, PurpleHttpConnection* source_conn)
{
    PurpleHttpRequest* source_request = purple_http_conn_get_request(source_conn);
    purple_http_request_set_cookie_jar(target, purple_http_request_get_cookie_jar(source_request));
}
