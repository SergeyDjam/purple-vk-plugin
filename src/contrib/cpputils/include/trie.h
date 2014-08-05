// Copyright 2014, Oleg Andreev. All rights reserved.
// License: http://www.opensource.org/licenses/BSD-2-Clause

// trie:
//   A simple implementation of trie (prefix tree).

#pragma once

#include <cassert>
#include <climits>
#include <memory>

#define TRIE_DISABLE_COPY(Classname) \
    Classname(const Classname&) = delete; \
    Classname& operator =(const Classname&) = delete

#define TRIE_DISABLE_MOVE(Classname) \
    Classname(Classname&&) = delete; \
    Classname& operator =(Classname&&) = delete

#define TRIE_DEFAULT_MOVE(Classname) \
    Classname(Classname&&) = default; \
    Classname& operator =(Classname&&) = default

template<typename T>
class Trie
{
public:
    Trie()
        : m_size(0)
    {
    }

    TRIE_DISABLE_COPY(Trie);
    TRIE_DEFAULT_MOVE(Trie);

    // Inserts new key-value pair into trie if the key is not present in the trie already.
    // Value is constructed from passed args. Returns true if insertion occured, false
    // otherwise.
    template<typename... ArgTypes>
    bool insert(const char* key, ArgTypes&&... args)
    {
        return insert_impl(key, std::forward<ArgTypes>(args)...);
    }

    // Returns a matching value for key, or nullptr if key has not been added.
    // If length is not null, it is set to length of the match, or zero if no match
    // has been found.
    const T* match(const char* key, size_t* length = nullptr) const
    {
        return match_impl(key, 0, m_root.get(), length);
    }

    // A non-const version of match.
    T* match(const char* key, size_t* length = nullptr)
    {
        return const_cast<T*>(match_impl(key, 0, m_root.get(), length));
    }

    // Returns true if trie is empty, false otherwise.
    bool empty() const
    {
        return m_size == 0;
    }

    // Returns the number of elements, which were added to the trie.
    size_t size() const
    {
        return m_size;
    }

private:
    static_assert(UCHAR_MAX == 255, "Are your bytes 7 bits wide or is your compiler broken?");

    class Node;
    // A structure, present in each Node, storing its children if the node is non-leaf.
    // All children are split in 16 buckets, size of each bucket is also 16. Chars [0-15] are in
    // the first bucket, chars [16-31] in the second etc.
    class NodeChildren;

    // Having root as a unique_ptr instead of inline object allows easy, fast and noexcept move
    // constructor.
    std::unique_ptr<Node> m_root;
    // Number of elements in the trie.
    size_t m_size;

    // Used for testing.
    template<typename U>
    friend class TriePrinter;

    template<typename... ArgTypes>
    bool insert_impl(const char* key, ArgTypes&&... args);

    static const T* match_impl(const char* key, size_t offset, const Node* node, size_t* length);
};


template<typename T>
class Trie<T>::NodeChildren
{
public:
    // Adds node, starting with char c or returns existing node.
    Node* add(unsigned char c);

    // Returns node, starting with char c or nullptr if such note has not been added.
    const Node* get(unsigned char c) const
    {
        return get_impl(c);
    }

    // A non-const version of get()
    Node* get(unsigned char c)
    {
        return const_cast<Node*>(get_impl(c));
    }

private:
    // Note that this compiles due to templatedness of Node. Template instantiations happen
    // only after parsing the whole file, when Node becomes a complete type.
    struct Bucket
    {
        Node children[16];
    };
    typedef std::unique_ptr<Bucket> BucketPtr;

    struct Buckets
    {
        BucketPtr buckets[16];
    };
    std::unique_ptr<Buckets> m_root;

    const Node* get_impl(unsigned char c) const;

    // Used for testing.
    template<typename U>
    friend class TriePrinter;
};

template<typename T>
typename Trie<T>::Node* Trie<T>::NodeChildren::add(unsigned char c)
{
    if (!m_root)
        m_root.reset(new Buckets);
    unsigned char upper = c >> 4;
    BucketPtr& bucket = m_root->buckets[upper];
    if (!bucket)
        bucket.reset(new Bucket);
    unsigned char lower = c & 15;
    return &bucket->children[lower];
}

template<typename T>
const typename Trie<T>::Node* Trie<T>::NodeChildren::get_impl(unsigned char c) const
{
    if (m_root) {
        unsigned char upper = c >> 4;
        const BucketPtr& bucket = m_root->buckets[upper];
        if (bucket) {
            unsigned char lower = c & 15;
            Node* ret = &bucket->children[lower];
            if (ret->is_empty())
                return nullptr;
            else
                return ret;
        }
    }
    return nullptr;
}

// Unfortunately, gcc <= 4.7 does not support alignas, simulate it via maximum alignment.
#define TRIE_HAS_GCC_LE(major, minor) \
    (!defined(__clang__) && (__GNUC__ < major || (__GNUC__ == major && __GNUC_MINOR__ <= minor)))
#if TRIE_HAS_GCC_LE(4, 7)
#define TRIE_ALIGNAS(TYPE) __attribute__((aligned(__BIGGEST_ALIGNMENT__)))
#else
#define TRIE_ALIGNAS(TYPE) alignas(alignof(TYPE))
#endif

template<typename T>
class Trie<T>::Node
{
public:
    Node()
        : type(NodeType::EMPTY)
    {
    }

    ~Node()
    {
        switch(type) {
        case NodeType::EMPTY:
            break;
        case NodeType::NONLEAF:
            children()->~NodeChildren();
            break;
        case NodeType::LEAF:
            value()->~T();
            break;
        }
    }

    TRIE_DISABLE_COPY(Node);
    TRIE_DISABLE_MOVE(Node);

    // Initializes a previously empty node to non-leaf.
    void init_nonleaf()
    {
        assert(type == NodeType::EMPTY);
        type = NodeType::NONLEAF;
        new(children_storage) NodeChildren();
    }

    // Initializes a previously empty node to leaf.
    template<typename... ArgTypes>
    void init_leaf(ArgTypes&&... args)
    {
        assert(type == NodeType::EMPTY);
        type = NodeType::LEAF;
        new(value_storage) T(std::forward<ArgTypes>(args)...);
    }

    bool is_empty() const
    {
        return type == NodeType::EMPTY;
    }

    bool is_leaf() const
    {
        return type == NodeType::LEAF;
    }

    const NodeChildren* children() const
    {
        return reinterpret_cast<const NodeChildren*>(children_storage);
    }

    NodeChildren* children()
    {
        return reinterpret_cast<NodeChildren*>(children_storage);
    }

    const T* value() const
    {
        return reinterpret_cast<const T*>(value_storage);
    }

    // Sets the prefix to no more than first PREFIX_SIZE - 1 chars of new_prefix.
    // Returns the length of set prefix.
    size_t set_prefix(const char* new_prefix);

    // Returns true if given key matches prefix (i.e. prefix is the prefix for the key)
    // and sets length to the length of maximum common subprefix.
    bool matches_prefix(const char* key, size_t* length) const;

    // "Splits" node: the first part of prefix (up to new_prefix_length) stays in
    // the node, the node is converted to non-leaf (if it is not one already).
    // Node contents (either value or children) moves to new child node along
    // with the second part of the prefix.
    void split_node(size_t new_prefix_length);

private:
    enum class NodeType : char
    {
        EMPTY,
        NONLEAF,
        LEAF
    };

    NodeType type;

    // sizeof(type + prefix) = 8
    static const size_t PREFIX_SIZE = 7;

    // Used when type != EMPTY. Must be zero-terminated.
    char prefix[PREFIX_SIZE];

    union
    {
        // Used when type == NONLEAF.
        TRIE_ALIGNAS(NodeChildren) char children_storage[sizeof(NodeChildren)];
        // Used when type == LEAF
        TRIE_ALIGNAS(T) char value_storage[sizeof(T)];
    };

    T* value()
    {
        return reinterpret_cast<T*>(value_storage);
    }

    // Initializes a previously empty node to non-leaf with given children.
    void init_nonleaf(NodeChildren&& new_children)
    {
        type = NodeType::NONLEAF;
        new(children_storage) NodeChildren(std::move(new_children));
    }

    // Used for testing.
    template<typename U>
    friend class TriePrinter;
};

template<typename T>
size_t Trie<T>::Node::set_prefix(const char* new_prefix)
{
    for (size_t l = 0; l < PREFIX_SIZE - 1; l++) {
        prefix[l] = new_prefix[l];
        if (prefix[l] == '\0')
            return l;
    }
    prefix[PREFIX_SIZE - 1] = '\0';
    return PREFIX_SIZE - 1;
}

template<typename T>
bool Trie<T>::Node::matches_prefix(const char* key, size_t* length) const
{
    assert(type != NodeType::EMPTY);
    size_t l = 0;
    while (prefix[l] != '\0' && key[l] != '\0' && prefix[l] == key[l])
        l++;
    *length = l;
    return prefix[l] == '\0';
}

template<typename T>
void Trie<T>::Node::split_node(size_t new_prefix_length)
{
    assert(type != NodeType::EMPTY);
    NodeChildren new_children;

    unsigned char split_char = prefix[new_prefix_length];
    Node* new_node = new_children.add(split_char);
    new_node->set_prefix(prefix + new_prefix_length);
    prefix[new_prefix_length] = '\0';

    if (type == NodeType::NONLEAF) {
        new_node->init_nonleaf(std::move(*children()));
        *children() = std::move(new_children);
    } else {
        new_node->init_leaf(std::move(*value()));
        value()->~T();
        init_nonleaf(std::move(new_children));
    }
}


template<typename T>
template<typename... ArgTypes>
bool Trie<T>::insert_impl(const char* key, ArgTypes&&... args)
{
    if (!m_root)
        m_root.reset(new Node());

    Node* node = m_root.get();
    // The offset from the beginning of the key, which has already been processed.
    size_t offset = 0;
    while (true) {
        if (node->is_empty()) {
            offset += node->set_prefix(key + offset);
            if (key[offset] == '\0') {
                // We have processed the whole key.
                node->init_leaf(std::forward<ArgTypes>(args)...);
                m_size++;
                return true;
            } else {
                node->init_nonleaf();
            }
        } else {
            size_t common_length;
            if (!node->matches_prefix(key + offset, &common_length)) {
                node->split_node(common_length);
            } else if (node->is_leaf()) {
                // We matched the whole key, therefore we already have the key present
                // in the trie.
                if (key[offset + common_length] == '\0')
                    return false;
                node->split_node(common_length);
            }
            assert(common_length > 0 || node == m_root.get());
            offset += common_length;
        }

        unsigned char next_char = key[offset];
        node = node->children()->add(next_char);
    }
}

template<typename T>
const T* Trie<T>::match_impl(const char* key, size_t offset, const Trie::Node* node, size_t* length)
{
    if (length)
        *length = 0;

    // The offset from the beginning of the key, which has already been processed.
    while (true) {
        // The last can be true only for root node.
        if (!node)
            return nullptr;
        assert(!node->is_empty());

        size_t common_length;
        if (!node->matches_prefix(key + offset, &common_length))
            return nullptr;
        offset += common_length;
        if (node->is_leaf()) {
            if (length)
                *length = offset;
            return node->value();
        }
        unsigned char next_char = key[offset];
        const Node* child_zero = node->children()->get(0);
        if (child_zero) {
            assert(child_zero->is_leaf());
            // If node has a child in zero position, this means that this is one of the possible
            // matches (but there can be longer matches). We have to branch via recursion.
            const Node* next_node = node->children()->get(next_char);
            const T* match = match_impl(key, offset, next_node, length);
            if (match) {
                // We have found a longer match, adjust length and return it.
                return match;
            } else {
                // No longer matches have been found, return the current match.
                *length = offset;
                return child_zero->value();
            }
        } else {
            node = node->children()->get(next_char);
        }
    }
}

#undef TRIE_DISABLE_COPY
#undef TRIE_DISABLE_MOVE
#undef TRIE_DEFAULT_MOVE
#undef TRIE_HAS_GCC_LE
#undef TRIE_ALIGNAS
