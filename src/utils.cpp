#include <cstring>
#include <debug.h>
#include <util.h>

#include "vk-common.h"

#include "utils.h"

string get_xml_node_prop(xmlNode* node, const char* tag, const char* default_value)
{
    char* prop = (char*)xmlGetProp(node, (xmlChar*)tag);
    if (!prop)
        return default_value;
    else
        return prop;
}

string urlencode_form(const string_map& params)
{
    string ret;
    for (const string_pair& it: params) {
        if (!ret.empty())
            ret += '&';
        ret += purple_url_encode(it.first.c_str());
        ret += '=';
        ret += purple_url_encode(it.second.c_str());
    }
    return ret;
}

namespace
{

// Helper function for parse_urlencoded_form
string urldecode(const char* s, int len)
{
    char buf[1024];
    for (int i = 0; i < len; i++)
        buf[i] = s[i];
    buf[len] = 0;
    return purple_url_decode(buf);
}

} // End anonymous namespace

string_map parse_urlencoded_form(const char* encoded)
{
    string_map params;

    const char* key = encoded;
    while (true) {
        const char* value = strchr(key, '=');
        if (!value)
            break;
        string str_key = urldecode(key, value - key);

        value++;
        const char* end = strchr(value, '&');
        if (!end)
            end = value + strlen(value);
        string str_value = urldecode(value, end - value);

        params[str_key] = str_value;

        if (!*end)
            break;
        key = end + 1;
    }

    return params;
}


namespace
{

// Helper structure for timeout_add. The two latter members are used to remove id upon
// timeout end.
struct TimeoutCbData
{
    TimeoutCb callback;
    set<uint>& timeout_ids;
    uint id;
};

// Helper callback for timeout_add
int timeout_cb(void* user_data)
{
    TimeoutCbData* data = (TimeoutCbData*)user_data;
    return data->callback();
}

// Helper callback for timeout_add
void timeout_destroy_cb(void* user_data)
{
    TimeoutCbData* data = (TimeoutCbData*)user_data;
    data->timeout_ids.erase(data->id);
    delete data;
}

} // End of anonymous namespace

void timeout_add(PurpleConnection* gc, unsigned milliseconds, const TimeoutCb& callback)
{
    VkConnData* conn_data = (VkConnData*)purple_connection_get_protocol_data(gc);
    if (conn_data->is_closing()) {
        purple_debug_error("prpl-vkcom", "Programming error: timeout_add(%d) called during logout\n", milliseconds);
        return;
    }

    TimeoutCbData* data = new TimeoutCbData{ callback, conn_data->timeout_ids(), 0};
    data->id = g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, timeout_cb, data,
                                  timeout_destroy_cb);
    conn_data->timeout_ids().insert(data->id);
}

void timeout_remove_all(PurpleConnection* gc)
{
    VkConnData* conn_data = (VkConnData*)purple_connection_get_protocol_data(gc);
    for (uint id: conn_data->timeout_ids())
        g_source_remove(id);
}


string unescape_html(const char* text)
{
    char* unescaped = purple_unescape_html(text);
    string ret = unescaped;
    // Really ugly, but works: replace the most common HTML entities.
    str_replace(ret, "&ndash;", "\xe2\x80\x93");
    str_replace(ret, "&mdash;", "\xe2\x80\x94");
    g_free(unescaped);
    return ret;
}

string unescape_html(const string& text)
{
    return unescape_html(text.c_str());
}
