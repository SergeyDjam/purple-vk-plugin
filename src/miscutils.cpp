#include <cstring>
#include <debug.h>
#include <util.h>

#include "vk-common.h"

#include "miscutils.h"

string get_xml_node_prop(xmlNode* node, const char* tag, const char* default_value)
{
    char* prop = (char*)xmlGetProp(node, (xmlChar*)tag);
    if (!prop)
        return default_value;
    else
        return prop;
}

namespace
{

// Generic version of different urlencode_form variants.
template<typename It>
string urlencode_form(It first, It last)
{
    string ret;
    for (It it = first; it != last; it++) {
        if (!ret.empty())
            ret += '&';
        ret += purple_url_encode(it->first.data());
        ret += '=';
        ret += purple_url_encode(it->second.data());
    }
    return ret;
}

} // End anonymous namespace

string urlencode_form(const string_map& params)
{
    return urlencode_form(params.begin(), params.end());
}

string urlencode_form(const vector<string_pair>& params)
{
    return urlencode_form(params.begin(), params.end());
}

namespace
{

// Helper function for parse_urlencoded_form
string urldecode(const char* s, int len)
{
    char buf[1024];
    if (len >= (int)sizeof(buf)) {
        assert(false);
        return "";
    }
    memcpy(buf, s, len);
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

size_t max_urlencoded_prefix(const char *s, size_t max_urlencoded_len)
{
    const char* pos;
    // We preferrably split on: a) line break, b) punctuation and c) spaces.
    const char* last_break_pos = nullptr;
    const char* last_punct_pos = nullptr;
    const char* last_space_pos = nullptr;
    size_t len = 0;

    for (pos = s; *pos; pos = g_utf8_next_char(pos)) {
        int char_len = g_utf8_next_char(pos) - pos;
        if (char_len == 1) {
            char c = *pos;
            if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
                len++;
            else
                len += 3;
            if (len > max_urlencoded_len)
                break;

            // We want to include the break/space/punctuation character in the prefix.
            if (c == '\n')
                last_break_pos = pos + 1;
            else if (ispunct(c))
                last_punct_pos = pos + 1;
            else if (isspace(c))
                last_space_pos = pos + 1;
        } else {
            len += char_len * 3;
            if (len > max_urlencoded_len)
                break;
        }
    }

    if (*pos) {
        if (last_break_pos)
            return last_break_pos - s;
        else if (last_punct_pos)
            return last_punct_pos - s;
        else if (last_space_pos)
            return last_space_pos - s;
        else
            return pos - s;
    } else {
        // The whole string can be urlencoded in less than max_urlencoded_len.
        return pos - s;
    }
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

void timeout_add(PurpleConnection* gc, unsigned milliseconds, TimeoutCb callback)
{
    VkConnData* conn_data = get_conn_data(gc);
    if (conn_data->is_closing()) {
        purple_debug_error("prpl-vkcom", "Programming error: timeout_add(%d) called during logout\n", milliseconds);
        return;
    }

    TimeoutCbData* data = new TimeoutCbData({ std::move(callback), conn_data->timeout_ids, 0 });
    data->id = g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, timeout_cb, data,
                                  timeout_destroy_cb);
    conn_data->timeout_ids.insert(data->id);
}

void timeout_remove_all(PurpleConnection* gc)
{
    // g_source_remove calls timeout_destroy_cb, which modifies timeout_ids, so we make a copy before
    // calling g_source_remove. Damned mutability.
    uint_set timeout_ids = get_conn_data(gc)->timeout_ids;
    for (uint id: timeout_ids)
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
    return unescape_html(text.data());
}

string ensure_https_url(const string& url)
{
    if (strncmp(url.data(), "http:", 5) == 0)
        return "https:" + url.substr(5);
    else
        return url;
}


size_t max_urlencoded_int(uint64_vec::const_iterator start, uint64_vec::const_iterator end, size_t max_urlencoded_len)
{
    size_t len = 0;
    for (uint64_vec::const_iterator it = start; it != end; ++it) {
        len += int(log10(*it) + 1) + 3; // Comma is URLencoded
        if (len > max_urlencoded_len)
            return it - start;
    }
    return end - start;
}
