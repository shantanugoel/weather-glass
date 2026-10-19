#include "mini_json.h"

#include <stdlib.h>
#include <string.h>

namespace {

typedef struct {
    const char* p;
    const char* end;
} Cur;

void skip_ws(Cur* c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' ||
                             *c->p == '\r')) {
        ++c->p;
    }
}

/** c->p is on the opening quote; on success returns the unescaped span bounds. */
bool parse_string(Cur* c, const char** start, size_t* len) {
    if (c->p >= c->end || *c->p != '"') return false;
    ++c->p;
    const char* s = c->p;
    while (c->p < c->end) {
        if (*c->p == '\\') {
            c->p += 2;
            continue;
        }
        if (*c->p == '"') {
            *start = s;
            *len = static_cast<size_t>(c->p - s);
            ++c->p;
            return true;
        }
        ++c->p;
    }
    return false;
}

bool skip_value(Cur* c) {
    skip_ws(c);
    if (c->p >= c->end) return false;
    const char ch = *c->p;
    if (ch == '"') {
        const char* s = nullptr;
        size_t l = 0;
        return parse_string(c, &s, &l);
    }
    if (ch == '{' || ch == '[') {
        int depth = 0;
        bool in_string = false;
        while (c->p < c->end) {
            if (in_string) {
                if (*c->p == '\\') {
                    c->p += 2;
                    continue;
                }
                if (*c->p == '"') in_string = false;
                ++c->p;
                continue;
            }
            if (*c->p == '"') {
                in_string = true;
            } else if (*c->p == '{' || *c->p == '[') {
                ++depth;
            } else if (*c->p == '}' || *c->p == ']') {
                --depth;
                ++c->p;
                if (depth == 0) return true;
                continue;
            }
            ++c->p;
        }
        return false;
    }
    // number / true / false / null: run to the next delimiter
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
           *c->p != ' ' && *c->p != '\n' && *c->p != '\r' && *c->p != '\t') {
        ++c->p;
    }
    return true;
}

/** Read one "key"<:>value pair, leaving c->p after the value. */
bool next_member(Cur* c, const char** key, size_t* klen, const char** value,
                 size_t* vlen) {
    skip_ws(c);
    if (c->p >= c->end) return false;
    if (*c->p == '}' || *c->p == ']') return false;
    if (!parse_string(c, key, klen)) return false;
    skip_ws(c);
    if (c->p >= c->end || *c->p != ':') return false;
    ++c->p;
    skip_ws(c);
    *value = c->p;
    if (!skip_value(c)) return false;
    *vlen = static_cast<size_t>(c->p - *value);
    skip_ws(c);
    if (c->p < c->end && *c->p == ',') ++c->p;
    return true;
}

}  // namespace

extern "C" const char* json_find(const char* json, size_t len, const char* key,
                                 size_t* out_len) {
    if (json == nullptr || len == 0) return nullptr;
    Cur c{json, json + len};
    skip_ws(&c);
    if (c.p >= c.end || (*c.p != '{' && *c.p != '[')) return nullptr;
    ++c.p;
    const size_t key_len = strlen(key);
    const bool array = *(c.p - 1) == '[';
    while (c.p < c.end) {
        if (array) {
            // Arrays of objects: search each element for the key.
            skip_ws(&c);
            if (c.p >= c.end || *c.p == ']') return nullptr;
            const char* value = c.p;
            if (!skip_value(&c)) return nullptr;
            const size_t vlen = static_cast<size_t>(c.p - value);
            const char* found = json_find(value, vlen, key, out_len);
            if (found != nullptr) return found;
            skip_ws(&c);
            if (c.p < c.end && *c.p == ',') ++c.p;
            continue;
        }
        const char* k = nullptr;
        const char* v = nullptr;
        size_t kl = 0;
        size_t vl = 0;
        if (!next_member(&c, &k, &kl, &v, &vl)) return nullptr;
        if (kl == key_len && memcmp(k, key, kl) == 0) {
            if (out_len != nullptr) *out_len = vl;
            return v;
        }
    }
    return nullptr;
}

extern "C" bool json_number(const char* scope, size_t scope_len, const char* key,
                            double* out) {
    size_t len = 0;
    const char* v = json_find(scope, scope_len, key, &len);
    if (v == nullptr || len == 0) return false;
    char buf[32];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, v, n);
    buf[n] = '\0';
    if (buf[0] == '"') {  // quoted number
        memmove(buf, buf + 1, n - 1);
        buf[n - 1] = '\0';
    }
    char* stop = nullptr;
    const double value = strtod(buf, &stop);
    if (stop == buf) return false;
    *out = value;
    return true;
}

extern "C" bool json_int(const char* scope, size_t scope_len, const char* key,
                         long* out) {
    double value = 0.0;
    if (!json_number(scope, scope_len, key, &value)) return false;
    *out = static_cast<long>(value < 0.0 ? value - 0.5 : value + 0.5);
    return true;
}

extern "C" bool json_string(const char* scope, size_t scope_len, const char* key,
                            char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) return false;
    out[0] = '\0';
    size_t len = 0;
    const char* v = json_find(scope, scope_len, key, &len);
    if (v == nullptr || len < 2 || v[0] != '"') return false;
    size_t n = len - 2;
    if (n > out_size - 1) n = out_size - 1;
    memcpy(out, v + 1, n);
    out[n] = '\0';
    return true;
}

extern "C" int json_array_numbers(const char* scope, size_t scope_len,
                                  const char* key, double* out, int max) {
    size_t len = 0;
    const char* v = json_find(scope, scope_len, key, &len);
    if (v == nullptr || len < 2 || v[0] != '[') return 0;
    Cur c{v + 1, v + len - 1};
    int count = 0;
    while (c.p < c.end && count < max) {
        skip_ws(&c);
        if (c.p >= c.end) break;
        const char* start = c.p;
        if (!skip_value(&c)) break;
        const size_t n = static_cast<size_t>(c.p - start);
        char buf[32];
        const size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        memcpy(buf, start, take);
        buf[take] = '\0';
        char* clean = buf;
        if (*clean == '"') {
            clean++;
            if (take > 1) buf[take - 1] = '\0';
        }
        char* stop = nullptr;
        const double value = strtod(clean, &stop);
        if (stop != clean) out[count++] = value;
        skip_ws(&c);
        if (c.p < c.end && *c.p == ',') ++c.p;
    }
    return count;
}

extern "C" int json_array_strings(const char* scope, size_t scope_len,
                                  const char* key, char out[][32], int max) {
    size_t len = 0;
    const char* v = json_find(scope, scope_len, key, &len);
    if (v == nullptr || len < 2 || v[0] != '[') return 0;
    Cur c{v + 1, v + len - 1};
    int count = 0;
    while (c.p < c.end && count < max) {
        skip_ws(&c);
        if (c.p >= c.end) break;
        const char* s = nullptr;
        size_t sl = 0;
        if (!parse_string(&c, &s, &sl)) break;
        const size_t take = sl < 31 ? sl : 31;
        memcpy(out[count], s, take);
        out[count][take] = '\0';
        ++count;
        skip_ws(&c);
        if (c.p < c.end && *c.p == ',') ++c.p;
    }
    return count;
}