// Copyright 2014, Oleg Andreev. All rights reserved.
// License: http://www.opensource.org/licenses/BSD-2-Clause

// contutils:
//   A number of STL container-related utilities.

#pragma once

#include <algorithm>

// Check if container has T::key_type typedef, which means it is an associative container.
// We can probably use std::enable_if here, but do not want to include the whole <type_traits>
// for that.
template<typename T>
class IsAssociativeContainer
{
private:
    typedef char yes[1];
    typedef char no[2];

    template <typename C>
    static yes& test(typename C::key_type*);

    template <typename>
    static no& test(...);

public:
    enum { value = sizeof(test<T>(0)) == sizeof(yes) };
};

// A container-like class, which stores a pair of iterators. Similar to Boost range concept
// (http://www.boost.org/doc/libs/1_55_0/libs/range/doc/html/range/concepts/overview.html).
//
// Many functions in this file accept range as an input.
template<typename It>
class ItRange
{
public:
    // Let's export all the container-related typedefs.
    typedef typename std::iterator_traits<It>::value_type value_type;
    typedef typename std::iterator_traits<It>::reference reference;
    typedef It iterator;
    typedef It const_iterator;
    typedef typename std::iterator_traits<It>::difference_type difference_type;

    ItRange(It b, It e)
        : m_begin(std::move(b)),
          m_end(std::move(e))
    {
    }

    It begin() const
    {
        return m_begin;
    }

    It end() const
    {
        return m_end;
    }

    bool empty() const
    {
        return m_begin == m_end;
    }

private:
    It m_begin;
    It m_end;
};

// A helper function, which makes using ItRange less verbose ala make_pair.
template<typename It>
ItRange<It> itrange(It begin, It end)
{
    return ItRange<It>(std::move(begin), std::move(end));
}

// A helper function, which makes using ItRange less verbose ala make_pair. Used only when It is
// a random access iterator.
template<typename It>
ItRange<It> itrange_n(It begin, size_t len)
{
    return itrange(begin, begin + len);
}

// Sets the key to new value, if it already present in the map, does nothing otherwise.
// Returns true if the value has been updated, false otherwise.
template<typename Map, typename Key, typename Value>
bool map_update(Map& map, const Key& key, Value&& value)
{
    auto it = map.find(key);
    if (it == map.end())
        return false;
    it->second = std::forward<Value>(value);
    return true;
}

// Returns value for key or default value. Unlike map[] does not insert values into map,
// unlike map.at() does not throw exceptions.
template<typename Map, typename Key>
const typename Map::mapped_type& map_at(const Map& map, const Key& key)
{
    auto it = map.find(key);
    if (it == map.end()) {
        static typename Map::mapped_type default_value;
        return default_value;
    } else {
        return it->second;
    }
}

// A variant of map_at, which returns default value,
//
// WARNING: Returns a copy of existing value, not a reference to it.
template<typename Map, typename Key, typename Value = typename Map::mapped_type>
typename Map::mapped_type map_at_default(const Map& map, const Key& key, Value default_value)
{
    auto it = map.find(key);
    if (it == map.end())
        return default_value;
    else
        return it->second;
}

// Returns pointer to value for key or nullptr if key is not present.
template<typename Map, typename Key, typename Value = typename Map::mapped_type>
const typename Map::mapped_type* map_at_ptr(const Map& map, const Key& key)
{
    auto it = map.find(key);
    if (it == map.end())
        return nullptr;
    else
        return &it->second;
}

// A non-const version of map_at_ptr
template<typename Map, typename Key, typename Value = typename Map::mapped_type>
typename Map::mapped_type* map_at_ptr(Map& map, const Key& key)
{
    auto it = map.find(key);
    if (it == map.end())
        return nullptr;
    else
        return &it->second;
}

// Returns true if an associative container (map/set) contains key.
template<typename Cont, typename Key>
bool contains(const Cont& cont, const Key& key)
{
    // Check that container is associative container to prevent user from passing e.g. std::string,
    // which also has find() method, but returns string::npos, not end() in case of failure.
    static_assert(IsAssociativeContainer<Cont>::value, "Cont must be an associative container");
    return cont.find(key) != cont.cend();
}

// Returns true if a range contains the specified element. This method is named differently
// from contains() to highlight the semantics (and potentially performance in case of set)
// difference.
template<typename Range, typename Value>
bool seq_contains(const Range& range, const Value& value)
{
    static_assert(!IsAssociativeContainer<Range>::value, "Range must be a sequence container");
    for (const auto& v: range)
        if (v == value)
            return true;
    return false;
}

// Assigns contents of range to container, used instead of operator= when DstCont != SrcCont,
template<typename DstCont, typename SrcRange>
void assign(DstCont& dst, const SrcRange& src)
{
    dst = DstCont(src.begin(), src.end());
}

// Inserts contents of range into container. DstCont must be an associative container (set/map),
// use append for sequence containers.
template<typename DstCont, typename SrcRange>
void insert(DstCont& dst, const SrcRange& src)
{
    static_assert(IsAssociativeContainer<DstCont>::value,
                  "DstCont must be an associative container");
    for (const auto& v: src)
        dst.insert(v);
}

// Appends range to container. DstCont mmust be a sequence coontainer (vector/deque/list),
// use insert for associative ones.
template<typename DstCont, typename SrcRange>
void append(DstCont& dst, const SrcRange& src)
{
    static_assert(!IsAssociativeContainer<DstCont>::value, "DstCont must be a sequence container");
    dst.insert(dst.end(), src.cbegin(), src.cend());
}

// Inserts all elements satisfying predicate from range to container. DstCont must be
// an associative container (set/map), use append_if for sequence containers.
template<typename DstCont, typename SrcRange, typename Pred>
void insert_if(DstCont& dst, const SrcRange& src, const Pred& pred)
{
    static_assert(IsAssociativeContainer<DstCont>::value,
                  "DstCont must be an associative container");
    for (const auto& v: src)
        if (pred(v))
            dst.insert(v);
}

// Appends all elements satisfying predicate from range to contianer. DstCont must be a sequence
// container (vector/deque/list), use insert_if for associative containers.
template<typename DstCont, typename SrcRange, typename Pred>
void append_if(DstCont& dst, const SrcRange& src, const Pred& pred)
{
    static_assert(!IsAssociativeContainer<DstCont>::value, "DstCont must be a sequence container");
    for (const auto& v: src)
        if (pred(v))
            dst.push_back(v);
}

// Implementation of erase_if for associative containers.
//
// Description of this method of template dispatch can be found on:
// http://stackoverflow.com/questions/6917079/tag-dispatch-versus-static-methods-on-partially-specialised-classes
//
// Boy, I hate template metaprogramming.
template<typename Cont, typename Pred>
static void __erase_if_impl(Cont& cont, const Pred& pred,
                            char(*)[IsAssociativeContainer<Cont>::value] = nullptr)
{
    for (auto it = cont.begin(), end = cont.end(); it != end;) {
        if (pred(*it)) {
            it = cont.erase(it);
        } else {
            ++it;
        }
    }
}

// Implementation of erase_if for sequence containers.
template<typename Cont, typename Pred>
static void __erase_if_impl(Cont& cont, const Pred& pred,
                            char(*)[!IsAssociativeContainer<Cont>::value] = nullptr)
{
    cont.erase(std::remove_if(cont.begin(), cont.end(), pred), cont.end());
}

// Removes all elements, satisfying predicate. Cont may be either sequence or associative container.
template<typename Cont, typename Pred>
void erase_if(Cont& cont, const Pred& pred)
{
    __erase_if_impl(cont, pred);
}

// Removes sequential equal elements. Cont must be a sequence container (vector/deque/list),
// not an associative container (set/map).
template<typename Cont>
void unique(Cont& cont)
{
    static_assert(!IsAssociativeContainer<Cont>::value, "Cont must be a sequence container");
    cont.erase(std::unique(cont.begin(), cont.end()), cont.end());
}

// Version of unique, which accepts a predicate.
template<typename Cont, typename Pred>
void unique(Cont& cont, const Pred& pred)
{
    static_assert(!IsAssociativeContainer<Cont>::value, "Cont must be a sequence container");
    cont.erase(std::unique(cont.begin(), cont.end(), pred), cont.end());
}
