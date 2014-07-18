// Common header, should be included in every file, contains the most basic
// data structures and algorithms.

#pragma once

// NDEBUG gets set by CMake and is so much trouble to unset, so let's unset it here :-)
// On Linux it is used only for always enabling assert().
#undef NDEBUG

#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Let's make using most popular names easier.
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

    template<typename... ParamArgTypes>
    R operator()(ParamArgTypes&&... args) const
    {
        // Amazing, that you can do it for R == void
        if (m_function)
            return m_function->operator()(std::forward<ParamArgTypes>(args)...);
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

// Miscellaneous ontainer functions

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

// I18n

#define i18n(x) (x)

// Debugging macros

#define vkcom_debug_info(fmt, ...) \
    purple_debug_info("prpl-vkcom", fmt, ##__VA_ARGS__)
#define vkcom_debug_error(fmt, ...) \
    purple_debug_error("prpl-vkcom", fmt, ##__VA_ARGS__)
