#include <cstring>

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

// Helper callback for timeout_add
int timeout_add_cb(void* user_data)
{
    TimeoutCb* callback = (TimeoutCb*)user_data;
    if ((*callback)()) {
        return 1;
    } else {
        *callback = nullptr;
        delete callback;
        return 0;
    }
}

} // End of anonymous namespace

void timeout_add(unsigned milliseconds, const TimeoutCb& callback)
{
    TimeoutCb* user_data = new TimeoutCb(callback);
    g_timeout_add(milliseconds, timeout_add_cb, user_data);
}
