#include <cstring>
#include <debug.h>
#include <util.h>

#include "strutils.h"

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
template<typename Iter>
string urlencode_form(Iter first, Iter last)
{
    string ret;
    for (Iter it = first; it != last; it++) {
        if (!ret.empty())
            ret += '&';
        ret += purple_url_encode(it->first.data());
        ret += '=';
        ret += purple_url_encode(it->second.data());
    }
    return ret;
}

} // End anonymous namespace

string urlencode_form(const map<string, string>& params)
{
    return urlencode_form(params.begin(), params.end());
}

string urlencode_form(const vector<pair<string, string>>& params)
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
        char* large_buf = new char[len + 1];
        memcpy(large_buf, s, len);
        large_buf[len] = 0;
        string ret = purple_url_decode(large_buf);
        delete[] large_buf;
        return ret;
    }
    memcpy(buf, s, len);
    buf[len] = 0;
    return purple_url_decode(buf);
}

} // End anonymous namespace

map<string, string> parse_urlencoded_form(const char* encoded)
{
    map<string, string> params;

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


size_t max_urlencoded_int(const uint64* start, const uint64* end, size_t max_urlencoded_len)
{
    size_t len = 0;
    for (const uint64* it = start; it != end; ++it) {
        len += int(log10(*it) + 1) + 3; // Comma is URLencoded
        if (len > max_urlencoded_len)
            return it - start;
    }
    return end - start;
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
