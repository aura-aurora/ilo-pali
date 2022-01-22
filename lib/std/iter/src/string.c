#include "iter/string.h"

struct s_string {
    size_t len;
    size_t cap;
    char* bytes;
    alloc_t* A;
};

// empty_string(alloc_t*) -> string_t
// Allocates an empty string.
string_t empty_string(alloc_t* A) {
    return (string_t) {
        .len = 0,
        .cap = 0,
        .bytes = NULL,
        .A = A,
    };
}

// string_from_str(alloc_t*, str_t) -> string_t
// Creates a string from a str.
string_t string_from_str(alloc_t* A, str_t str) {
    string_t s = {
        .len = str.len,
        .cap = str.len,
        .bytes = alloc(A, str.len),
        .A = A,
    };
    memcpy(s.bytes, str.bytes, s.len);
    return s;
}

// str_from_string(string_t*) -> str_t
// Creates a str from a string.
str_t str_from_string(string_t* s) {
    return (str_t) {
        .len = s->len,
        .bytes = s->bytes,
    };
}

// str_slice(str_t, size_t, size_t) -> str_t
// Creates a str slice from another str.
str_t str_slice(str_t s, size_t start, size_t end) {
    if (start > end || start > s.len || end > s.len)
        return (str_t) { 0 };

    return (str_t) {
        .len = end - start,
        .bytes = s.bytes + start,
    };
}

// string_push_byte(string_t*, char) -> void
// Pushes a byte to the given string.
void string_push_byte(string_t* s, char c) {
    if (s->len >= s->cap) {
        if (s->cap == 0)
            s->cap = 8;
        else s->cap <<= 1;
        s->bytes = alloc_resize(s->A, s->bytes, s->cap);
    }
    s->bytes[s->len++] = c;
}

// string_push_str(string_t*, str_t) -> void
// Pushes a str to the given string.
void string_push_str(string_t* s, str_t str) {
    if (s->len + str.len > s->cap) {
        if (s->cap == 0)
            s->cap = str.len;
        else {
            s->cap <<= 1;
            if (s->len + str.len > s->cap)
                s->cap = s->len + str.len;
        }

        s->bytes = alloc_resize(s->A, s->bytes, s->cap);
    }

    memcpy(s->bytes + s->len, str.bytes, str.len);
    s->len += str.len;
}

// str_equals(str_t, str_t) -> bool
// Determines whether two strs are equal.
bool str_equals(str_t a, str_t b) {
    if (a.len != b.len)
        return false;
    else if (a.bytes == NULL || b.bytes == NULL)
        return false;

    for (size_t i = 0; i < a.len; i++) {
        if (a.bytes[i] != b.bytes[i])
            return false;
    }

    return true;
}

// str_is_empty(str_t) -> bool
// Returns true if the given str is empty.
bool str_is_empty(str_t s) {
    return s.len == 0 || s.bytes == NULL;
}

// str_split(str_t, str_t, str_t) -> str_t
// Splits the first argument based on the second argument, starting off on the last argument's offset.
str_t str_split(str_t s, str_t spliter, str_t last) {
    str_t cont;
    if (last.bytes != NULL) {
        if (s.bytes + s.len <= last.bytes + last.len + spliter.len)
            return STREMPTY;
        cont = (str_t) {
            .len = s.len - (last.bytes - s.bytes + last.len + spliter.len),
            .bytes = last.bytes + last.len + spliter.len,
        };
    }
    else cont = s;

    if (spliter.len > cont.len)
        return STREMPTY;

    for (size_t i = 0; i <= cont.len - spliter.len; i++) {
        if (str_equals(spliter, (str_t) {
            .len = spliter.len,
            .bytes = cont.bytes + i,
        }))
            return (str_t) {
                .len = i,
                .bytes = cont.bytes,
            };
    }

    return cont;
}

// dealloc_string(string_t*) -> void
// Deallocates a string.
void dealloc_string(string_t* s) {
    dealloc(s->A, s->bytes);
    s->len = 0;
    s->cap = 0;
    s->bytes = NULL;
}