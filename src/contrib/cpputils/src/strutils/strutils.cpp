// Copyright 2014, Oleg Andreev. All rights reserved.
// License: http://www.opensource.org/licenses/BSD-2-Clause

#include <cassert>
#include <cstdarg>
#include <cstring>

#include "trio.h"

#include "strutils.h"

std::string str_format(const char* fmt, ...)
{
    char buf[3072];
    char* large_buf = nullptr;

    va_list arg;
    va_start(arg, fmt);
    int size = trio_vsnprintf(buf, sizeof(buf), fmt, arg);
    va_end(arg);
    if (size >= (int)sizeof(buf)) {
        large_buf = new char[size + 1];
        va_start(arg, fmt);
        trio_vsnprintf(large_buf, size + 1, fmt, arg);
        va_end(arg);
    }

    std::string ret = large_buf ? large_buf : buf;
    if (large_buf)
        delete[] large_buf;
    return ret;
}

// Helper function: finds first character not in removed or not whitespace if removed is null.
static const char* find_first_non(const char* s, const char* removed)
{
    const char* start = s;
    if (removed) {
        while (strchr(removed, *start))
            start++;
    } else {
        while (ascii_isspace(*start))
            start++;
    }
    return start;
}

// Helper function: finds last character not in removed or not whitespace if removed is null.
static const char* find_last_non(const char* s, const char* removed)
{
    const char* end = s;
    if (removed) {
        for (const char* p = end; *p != '\0'; p++)
            if (!strchr(removed, *p))
                end = p;
    } else {
        for (const char* p = end; *p != '\0'; p++)
            if (!ascii_isspace(*p))
                end = p;
    }
    return end;
}

std::string str_trimmed(const char* s, const char* removed)
{
    const char* start = find_first_non(s, removed);
    if (*start == '\0')
        return std::string();
    const char* end = find_last_non(start, removed);
    return std::string(start, end - start + 1);
}

std::string str_trimmed(const std::string& s, const char* removed)
{
    return str_trimmed(s.c_str(), removed);
}

void str_trim(std::string& s, const char* removed)
{
    const char* start = find_first_non(s.c_str(), removed);
    if (*start == '\0') {
        s.clear();
        return;
    }
    if (start != s.c_str())
        s.erase(0, start - s.c_str());

    const char* end = find_last_non(s.c_str(), removed);
    end++;
    if (*end != '\0')
        s.erase(end - s.c_str());
}

static std::string str_replaced_impl(const char *s, const char *from, size_t from_len, const char *to)
{
    std::string ret;
    if (from_len == 0) {
        ret.append(s);
    } else {
        for (const char* p = s; *p != '\0';) {
            const char* from_match = strstr(p, from);
            if (!from_match) {
                ret.append(p);
                break;
            }
            ret.append(p, from_match - p);
            ret.append(to);
            p = from_match + from_len;
        }
    }
    return ret;
}

std::string str_replaced(const char *s, const char *from, const char *to)
{
    return str_replaced_impl(s, from, strlen(from), to);
}

std::string str_replaced(const std::string& s, const std::string& from, const std::string& to)
{
    return str_replaced_impl(s.c_str(), from.c_str(), from.length(), to.c_str());
}

static void str_replace_fast_impl(std::string& s, const char* from, size_t from_len,
                                  const char* to, size_t to_len)
{
    if (from_len == 0)
        return;

    // We maintain src and dst indices into s. src points to not processed part of the string, dst points to
    // the char after the last written. If from_len == to_len, dst should be always equal src, otherwise
    // src - dst == number of matches already replaced * (from_len - to_len)
    const char* begin = s.c_str();
    size_t src = 0;
    size_t dst = 0;
    while (dst < s.length()) {
        const char* from_match = strstr(begin + src, from);
        if (!from_match) {
            if (src != dst) {
                assert(src > dst);
                std::copy(s.begin() + src, s.end(), s.begin() + dst);
            }
            dst += s.length() - src;
            break;
        }
        size_t from_pos = from_match - begin;
        if (dst != src) {
            assert(src > dst);
            std::copy(s.begin() + src, s.begin() + from_pos, s.begin() + dst);
        }
        dst += from_pos - src;
        std::copy(to, to + to_len, s.begin() + dst);
        dst += to_len;
        src = from_pos + from_len;
    }
    s.resize(dst);
}

static void str_replace_slow_impl(std::string& s, const char* from, size_t from_len, const char* to, size_t to_len)
{
    if (from_len == 0)
        return;

    // String can be reallocated, do not use pointers.
    for (size_t i = 0; i < s.length(); ) {
        const char* begin = s.c_str();
        const char* from_match = strstr(begin + i, from);
        if (!from_match)
            return;
        s.replace(from_match - begin, from_len, to);
        i = from_match - begin + to_len;
    }
}

static void str_replace_impl(std::string& s, const char* from, size_t from_len, const char* to, size_t to_len)
{
    // We have two basic cases: when from_len >= to_len and when from_len < to_len. The first
    // case does not require reallocation of the rest of the string and can be implemented via
    // "fast" path.
    if (from_len >= to_len) {
        str_replace_fast_impl(s, from, from_len, to, to_len);
    } else {
        str_replace_slow_impl(s, from, from_len, to, to_len);
    }
}

void str_replace(std::string& s, const char* from, const char* to)
{
    str_replace_impl(s, from, strlen(from), to, strlen(to));
}

void str_replace(std::string& s, const std::string& from, const std::string& to)
{
    str_replace_impl(s, from.c_str(), from.length(), to.c_str(), to.length());
}


static void str_split_impl(const char* s, const char* split_pos, std::string* first, std::string* last)
{
    if (split_pos) {
        if (first) {
            first->clear();
            first->append(s, split_pos - s);
        }
        if (last) {
            last->clear();
            last->append(split_pos + 1);
        }
    } else {
        if (first)
            *first = s;
        if (last)
            last->clear();
    }
}


void str_lsplit(const char* s, char sep, std::string* first, std::string* last)
{
    const char* first_sep = strchr(s, sep);
    str_split_impl(s, first_sep, first, last);
}


void str_lsplit(const std::string& s, char sep, std::string* first, std::string* last)
{
    str_lsplit(s.c_str(), sep, first, last);
}


void str_rsplit(const char* s, char sep, std::string* first, std::string* last)
{
    const char* last_sep = strrchr(s, sep);
    str_split_impl(s, last_sep, first, last);
}


void str_rsplit(const std::string& s, char sep, std::string* first, std::string* last)
{
    str_rsplit(s.c_str(), sep, first, last);
}


static char ascii_tolower(char c)
{
    if (c < 'A' || c > 'Z')
        return c;
    else
        return c + 'a' - 'A';
}

static char ascii_toupper(char c)
{
    if (c < 'a' || c > 'z')
        return c;
    else
        return c + 'A' - 'a';
}

std::string str_lowered(const char* s)
{
    // Two passes over string seems to be faster than push_backing chars one-by-one.
    std::string ret = s;
    str_tolower(ret);
    return ret;
}

std::string str_lowered(const std::string& s)
{
    std::string ret = s;
    str_tolower(ret);
    return ret;
}

std::string str_uppered(const char* s)
{
    std::string ret = s;
    str_toupper(ret);
    return ret;
}

std::string str_uppered(const std::string& s)
{
    std::string ret = s;
    str_toupper(ret);
    return ret;
}

void str_tolower(std::string& s)
{
    for (std::string::iterator it = s.begin(); it != s.end(); ++it)
        *it = ascii_tolower(*it);
}

void str_toupper(std::string& s)
{
    for (std::string::iterator it = s.begin(); it != s.end(); ++it)
        *it = ascii_toupper(*it);
}


std::string to_string(int i)
{
    char buf[128];
    trio_sprintf(buf, "%d", i);
    return buf;
}

std::string to_string(unsigned int i)
{
    char buf[128];
    trio_sprintf(buf, "%u", i);
    return buf;
}

std::string to_string(long i)
{
    char buf[128];
    trio_sprintf(buf, "%ld", i);
    return buf;
}

std::string to_string(unsigned long i)
{
    char buf[128];
    trio_sprintf(buf, "%lu", i);
    return buf;
}

std::string to_string(long long i)
{
    char buf[128];
    trio_sprintf(buf, "%lld", i);
    return buf;
}

std::string to_string(unsigned long long i)
{
    char buf[128];
    trio_sprintf(buf, "%llu", i);
    return buf;
}

