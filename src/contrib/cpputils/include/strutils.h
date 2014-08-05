// Copyright 2014, Oleg Andreev. All rights reserved.
// License: http://www.opensource.org/licenses/BSD-2-Clause

// strutils:
//   A number of string-related utilities: splitting, concatenating, replacing in strings.
//
//   WARNING: All functions assume that std::string contains only one null character (the terminator).

#pragma once

#include <cinttypes>
#include <cstring>
#include <string>

// A small function, which determines if char is an ASCII space (this is independent of locale unlike isspace from stdlib).
inline bool ascii_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Format function

#ifdef __GNUC__
#ifdef __MINGW32__
#define STR_FORMAT_CHECK(fmt_pos, arg_pos) __attribute__((format (gnu_printf, fmt_pos, arg_pos)))
#else
#define STR_FORMAT_CHECK(fmt_pos, arg_pos) __attribute__((format (printf, fmt_pos, arg_pos)))
#endif
#else
#define STR_FORMAT_CHECK
#endif

// Creates a new string, analogous to sprintf
//
// NOTE: str_format does not use a system printf, but a printf from trio library.
//  * Both %lld and %I64d specifiers are supported, no need for PRId64 crap.
//  * All positional arguments must be specified only once.
//  * %zu for size_t, %td for ptrdiff_t are supported.
STR_FORMAT_CHECK(1, 2)
std::string str_format(const char* fmt, ...);

#undef STR_FORMAT_CHECK

// Trim functions

// Returns new string with removed characters at the beginning and at the end of the string. removed must be
// a string of characters, each of which must be removed. If removed is nullptr, ' ', '\t', '\r' and '\n'
// will be removed.
std::string str_trimmed(const char* s, const char* removed = nullptr);
std::string str_trimmed(const std::string& s, const char* removed = nullptr);

// An inplace version of str_trimmed, modifies input string.
void str_trim(std::string& s, const char* removed = nullptr);

// Replace functions

// Returns new string with all occurences of from replaced with to.
std::string str_replaced(const char* s, const char* from, const char* to);
std::string str_replaced(const std::string& s, const std::string& from, const std::string& to);

// An inplace version of str_replaced.
void str_replace(std::string& s, const char* from, const char* to);
void str_replace(std::string& s, const std::string& from, const std::string& to);

// Concat functions

// Concatenates strings into one string, separating them with given separator like "smth".join() in Python.
// ForwardIt must be a forward iterator, pointing to std::string (or an object which has length() method and
// operator += with std::string).
template<typename Sep, typename ForwardIt>
inline std::string str_concat(const Sep& sep, ForwardIt first, ForwardIt last)
{
    std::string ret;
    for (ForwardIt it = first; it != last; ++it) {
        if (!ret.empty())
            ret += sep;
        ret += *it;
    }
    return ret;
}

template<typename Sep, typename Cont>
inline std::string str_concat(const Sep& sep, const Cont& cont)
{
    return str_concat(sep, cont.cbegin(), cont.cend());
}

// Split functions

// Returns the first and last part of the string before and after first separator in input string. If separator
// is not present in the string, last is set to empty. first and/or last can be null. Separator is not included
// in either of the output strings.
void str_lsplit(const char* s, char sep, std::string* first, std::string* last);
void str_lsplit(const std::string& s, char sep, std::string* first, std::string* last);

// Returns the first and last part of the string before and after last separator in input string. If separator
// is not present in the string, last is set to empty. first and/or last can be null. Separator is not included
// in either of the output strings.
void str_rsplit(const char* s, char sep, std::string* first, std::string* last);
void str_rsplit(const std::string& s, char sep, std::string* first, std::string* last);

// Generic split function, calls passed function on std::string of each split. func must accept
// either std::string or const std::string&
template<typename Func>
void str_split_func(const char* s, char sep, const Func& func)
{
    // We use one buf for the whole process so that we do not constantly allocate-reallocate memory.
    std::string buf;
    const char* p = s;
    while (*p != '\0') {
        const char* next = strchr(p, sep);
        if (!next)
            break;
        buf.append(p, next - p);
        func(buf);
        buf.clear();
        p = next + 1;
    }
    buf.append(p);
    func(buf);
}

template<typename Func>
void str_split_func(const std::string& s, char sep, const Func& func)
{
    str_split_func(s.c_str(), sep, func);
}

// A helper function for str_split_func, which appends all the strings from the split string
// to the vector or vector-like container.
template<typename Cont>
void str_split_append(const char* s, char sep, Cont& cont)
{
    str_split_func(s, sep, [&](std::string v) { cont.push_back(std::move(v)); });
}

template<typename Cont>
void str_split_append(const std::string& s, char sep, Cont& cont)
{
    str_split_append(s.c_str(), sep, cont);
}

// A helper function for str_split_func, which inserts all the strings from the split string
// to the set or set-like container.
template<typename Cont>
void str_split_insert(const char* s, char sep, Cont& cont)
{
    str_split_func(s, sep, [&](std::string v) { cont.insert(std::move(v)); });
}

template<typename Cont>
void str_split_insert(const std::string& s, char sep, Cont& cont)
{
    str_split_insert(s.c_str(), sep, cont);
}

// Case conversions

// Converts new string, which has ASCII chars in string converted to lowercase. Locale-independent.
std::string str_lowered(const char* s);
std::string str_lowered(const std::string& s);

// Converts new string, which has ASCII chars in string converted to uppercase. Locale-independent.
std::string str_uppered(const char* s);
std::string str_uppered(const std::string& s);

// Converts ASCII chars in string to lowercase. Locale-independent.
void str_tolower(std::string& s);
// Converts ASCII chars in string to uppercase. Locale-independent.
void str_toupper(std::string& s);

// Emulation of to_string for MinGW.
std::string to_string(int i);
std::string to_string(unsigned int i);
std::string to_string(long i);
std::string to_string(unsigned long i);
std::string to_string(long long i);
std::string to_string(unsigned long long i);

