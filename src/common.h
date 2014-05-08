// Common header, should be included in every file, contains the most basic
// data structures and algorithms.

#pragma once

// NDEBUG gets set by CMake and is so much trouble to unset, so let's unset it here :-)
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

typedef std::string string;

typedef unsigned int uint;
typedef int64_t int64;
typedef uint64_t uint64;

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

// Callbacks are pervasive in the plugin and most of the times we have to copy them (moving is a hassle
// generally and especially without support for moving into lambdas). This wrapper allocates std::functions
// on heap, which should be easier to manage and faster.
template<typename Signature>
class function_ptr;

template<typename R, typename... ArgTypes>
class function_ptr<R(ArgTypes...)>
{
public:
    function_ptr()
        : m_function(new std::function<R(ArgTypes...)>(nullptr))
    {
    }

    function_ptr(std::nullptr_t)
        : m_function(new std::function<R(ArgTypes...)>(nullptr))
    {
    }

    template<typename L>
    function_ptr(L l)
        : m_function(new std::function<R(ArgTypes...)>(std::move(l)))
    {
    }

    explicit operator bool() const
    {
        return m_function->operator bool();
    }

    R operator()(ArgTypes... args) const
    {
        // Amazing, that you can do it for R == void
        if (m_function)
            return m_function->operator()(args...);
        else
            return R();
    }

private:
    shared_ptr<std::function<R(ArgTypes...)>> m_function;
};

// This function type is used for signalling success if no other information must be passed.
typedef function_ptr<void()> SuccessCb;
// This function type is used for returning errors via callback.
typedef function_ptr<void()> ErrorCb;

// A very simple object, which calls given function upon termination.
class OnExit
{
public:
    OnExit(std::function<void()> deleter)
        : m_deleter(std::move(deleter))
    {
    }

    ~OnExit()
    {
        m_deleter();
    }

private:
    std::function<void(void)> m_deleter;
};

// Prevents compiler-generated copy-constructor and assignment operator.
#define DISABLE_COPYING(CLASSNAME) \
    CLASSNAME(const CLASSNAME&) = delete; \
    CLASSNAME& operator=(const CLASSNAME&) = delete;

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

// Returns new string with removed whitespace characters at the beginning and at the end of the string.
inline string str_trimmed(const char* s)
{
    const char* start = s;
    while (isspace(*start))
        start++;
    if (*start == '\0')
        return string();
    const char* end = start;
    for (const char* p = end; *p != '\0'; p++)
        if (!isspace(*p))
            end = p;
    return string(start, end - start + 1);
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
template<typename Map, typename Key, typename Value>
inline bool map_update(Map& map, const Key& key, const Value& value)
{
    auto it = map.find(key);
    if (it == map.end())
        return false;
    it->second = value;
    return true;
}

// Returns value for key or default value without inserting into map.
template<typename Map, typename Key, typename Value = typename Map::mapped_type>
inline typename Map::mapped_type map_at(const Map& map, const Key& key, const Value& default_value = Value())
{
    auto it = map.find(key);
    if (it == map.end())
        return default_value;
    else
        return it->second;
}

// Returns pointer to value for key or nullptr.
template<typename Map, typename Key, typename Value = typename Map::mapped_type>
inline typename Map::mapped_type* map_at_ptr(Map& map, const Key& key)
{
    auto it = map.find(key);
    if (it == map.end())
        return nullptr;
    else
        return &it->second;
}

// Returns true if map or set contains key.
template<typename Cont, typename Key>
inline bool contains(const Cont& cont, const Key& key)
{
    return cont.find(key) != cont.end();
}

// Inserts contents of one container into another, used when DstCont = map or set.
template<typename DstCont, typename SrcCont>
inline void insert(DstCont& dst, const SrcCont& src)
{
    for (auto it = src.cbegin(); it != src.cend(); ++it)
        dst.insert(*it);
}

// Appends one container to another.
template<typename DstCont, typename SrcCont>
inline void append(DstCont& dst, const SrcCont& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// Inserts all elements satisfying predicate from one container to another.
template<typename DstCont, typename SrcCont, typename Pred>
inline void insert_if(DstCont& dst, const SrcCont& src, Pred pred)
{
    for (auto it = src.cbegin(); it != src.cend(); ++it)
        if (pred(*it))
            dst.insert(*it);
}

// Appends all elements satisfying predicate from one container to another.
template<typename DstCont, typename SrcCont, typename Pred>
inline void append_if(DstCont& dst, const SrcCont& src, Pred pred)
{
    for (auto it = src.cbegin(); it != src.cend(); ++it)
        if (pred(*it))
            dst.push_back(*it);
}

// Assigns contents of one container to another, used instead of operator= when DstCont != SrcCont,
template<typename DstCont, typename SrcCont>
inline void assign(DstCont& dst, const SrcCont& src)
{
    dst = DstCont(src.begin(), src.end());
}

// Removes all elements, satisfying predicate, Cont should be list/set/map.
template<typename Cont, typename Pred>
inline void erase_if(Cont& cont, Pred pred)
{
    for (auto it = cont.begin(); it != cont.end();) {
        if (pred(*it)) {
            it = cont.erase(it);
        } else {
            ++it;
        }
    }
}

// Removes all elements, satisfying predicate, overload for erase_if when Cont is vector.
// NOTE: remove_if really should be a container member function because it depends on internal
// container structure.
template<typename T, typename Alloc, typename Pred>
inline void erase_if(vector<T, Alloc>& cont, Pred pred)
{
    cont.erase(std::remove_if(cont.begin(), cont.end(), pred), cont.end());
}

// Removes sequential equal elements. DstCont should be vector/deque.
template<typename DstCont, typename Pred>
inline void unique(DstCont& cont, Pred pred)
{
    cont.erase(std::unique(cont.begin(), cont.end(), pred), cont.end());
}

// Converts container to vector.
template<typename Cont>
inline vector<typename Cont::value_type, typename Cont::allocator_type> to_vector(const Cont& cont)
{
    return vector<typename Cont::value_type, typename Cont::allocator_type>(cont.begin(), cont.end());
}

// Time functions

// Converts the given duration to milliseconds.
template<typename T>
inline std::chrono::milliseconds::rep to_milliseconds(T duration)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// Converts the given duration to seconds.
template<typename T>
inline std::chrono::seconds::rep to_seconds(T duration)
{
    return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
}


// Debugging macros

#define vkcom_debug_info(fmt, ...) \
    purple_debug_info("prpl-vkcom", fmt, ##__VA_ARGS__)
#define vkcom_debug_error(fmt, ...) \
    purple_debug_error("prpl-vkcom", fmt, ##__VA_ARGS__)
