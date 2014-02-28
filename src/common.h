// Common header, should be included in every file, contains the most basic
// data structures and algorithms.

#pragma once

// It gets set by CMake and is so much trouble to unset, so let's unset it here :-)
// On Linux it is used only for always enabling assert().
#undef NDEBUG

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Let's make using most popular names easier.
using std::map;
using std::pair;
using std::set;
using std::shared_ptr;
using std::vector;

// NOTE: Should change string alias to __gnu_cxx::vstring;
typedef std::string string;
typedef map<string, string> string_map;
typedef pair<string, string> string_pair;
typedef set<string> string_set;
typedef vector<string> string_vec;

typedef unsigned int uint;
typedef vector<int> int_vec;
typedef set<uint> uint_set;
typedef int64_t int64;
typedef uint64_t uint64;
typedef vector<uint64> uint64_vec;
typedef set<uint64> uint64_set;


// GCC 4.6 supported only pre-C++11 monotonic_clock
#if __GNUC__ != 4 || __GNUC_MINOR__ != 6
using std::chrono::steady_clock;
typedef std::chrono::steady_clock::time_point steady_time_point;
typedef std::chrono::steady_clock::duration steady_duration;
#else
typedef std::chrono::monotonic_clock steady_clock;
typedef std::chrono::monotonic_clock::time_point steady_time_point;
typedef std::chrono::monotonic_clock::duration steady_duration;
#endif

// This function type is used for signalling success if no other information must be passed.
typedef std::function<void()> SuccessCb;
// This function type is used for returning errors via callback.
typedef std::function<void()> ErrorCb;

// A very simple object, which calls given function upon termination.
class OnExit
{
public:
    OnExit(std::function<void()> deleter)
        : m_deleter(move(deleter))
    {
    }

    ~OnExit()
    {
        m_deleter();
    }

private:
    std::function<void(void)> m_deleter;
};

// Miscellaneous string functions

// Creates a new string, analogous to sprintf
inline string str_format(const char* fmt, ...)
{
    char tmp[3072];

    va_list arg;
    va_start(arg, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, arg);
    va_end(arg);

    return string(tmp);
}

// Replaces all occurences of from to to in src string. Extremely inefficient, but who cares.
// Probably should've used replace_all or regex from boost.
inline void str_replace(string& s, const char* from, const char* to)
{
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from_len, to);
        pos += to_len;
    }
}

inline void str_replace(string& s, const string& from, const string& to)
{
    str_replace(s, from.data(), to.data());
}

// Concatenates strings into one string, separating them with given separator like "smth".join() in Python.
template<typename Sep, typename It>
inline string str_concat(Sep sep, It first, It last)
{
    string s;
    for (It it = first; it != last; it++) {
        if (!s.empty())
            s += sep;
        s += *it;
    }
    return s;
}

// Version of str_concat, which accepts container.
template<typename Sep, typename C>
inline string str_concat(Sep sep, const C& c)
{
    return str_concat(sep, c.cbegin(), c.cend());
}

// Creates string of integers, separated by sep.
template<typename Sep, typename It>
string str_concat_int(Sep sep, It first, It last)
{
    string s;
    for (It it = first; it != last; it++) {
        if (!s.empty())
            s += sep;
        char buf[128];
        sprintf(buf, "%" PRId64, *it);
        s += buf;
    }
    return s;
}

// Version of str_concat_int, which accepts container.
template<typename Sep, typename C>
string str_concat_int(Sep sep, const C& c)
{
    return str_concat_int(sep, c.cbegin(), c.cend());
}

// Converts string to upper-case
inline void to_upper(string& s)
{
    std::transform(s.begin(), s.end(), s.begin(), toupper);
}

// Returns the portion of the string after the rightmost sep.
inline string str_rsplit(const string& s, char sep)
{
    string::size_type last = s.find_last_of(sep);
    if (last != string::npos)
        return s.substr(last + 1);
    else
        return s;
}

inline string str_rsplit(const char* s, char sep)
{
    const char* last = strrchr(s, sep);
    if (last)
        return string(last + 1);
    else
        return string(s);
}

// Unfortunately, MinGW does not support std::to_string, so we have to emulate it.
inline string to_string(int i)
{
    char buf[128];
    sprintf(buf, "%d", i);
    return buf;
}

inline string to_string(uint i)
{
    char buf[128];
    sprintf(buf, "%u", i);
    return buf;
}

inline string to_string(int64 i)
{
    char buf[128];
    sprintf(buf, "%" PRId64, i);
    return buf;
}

inline string to_string(uint64 i)
{
    char buf[128];
    sprintf(buf, "%" PRIu64, i);
    return buf;
}

// Miscellaneous container functions

// Checks if map already contains key and sets it to new value.
template<typename M, typename K, typename V>
inline bool map_update(M& map, const K& key, const V& value)
{
    typename M::iterator it = map.find(key);
    if (it == map.end())
        return false;
    it->second = value;
    return true;
}

// Returns value for key or default value without inserting into map.
template<typename M, typename K, typename V = typename M::value_type>
inline V map_at_default(const M& map, const K& key, const V& default_value = V())
{
    typename M::iterator it = map.find(key);
    if (it == map.end())
        return default_value;
    else
        return it->second;
}

// Returns true if map or set contains key.
template<typename C, typename K>
inline bool contains_key(const C& cont, const K& key)
{
    return cont.find(key) != cont.end();
}

// Appends one vector, deque or list container to another.
template<typename D, typename S>
inline void append(D& dst, const S& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

template<typename D, typename S, typename Pred>
inline void append_if(D& dst, const S& src, Pred pred)
{
    std::copy_if(src.begin(), src.end(), std::back_inserter(dst), pred);
}

// A simple wrapper over std::remove_if and erase, usable for std::vector/std::string/std::deque.
template<typename C, typename Pred>
inline void remove_all(C& cont, Pred pred)
{
    cont.erase(std::remove_if(cont.begin(), cont.end(), pred), cont.end());
}

// Time functions

// Converts the given duration to milliseconds.
template<typename T>
inline std::chrono::milliseconds::rep to_milliseconds(T duration)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
