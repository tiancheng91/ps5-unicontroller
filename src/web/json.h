/* Small strict JSON token parser, original implementation, no dependencies. */
#ifndef UC_JSON_H
#define UC_JSON_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifndef UC_JSON_TOKEN_MAX
#define UC_JSON_TOKEN_MAX 192
#endif
typedef struct {
    int start, end, next;
    char kind;
} JToken;
typedef struct {
    const char *s;
    size_t len, pos;
    int count;
    JToken t[UC_JSON_TOKEN_MAX];
} Json;
static void jspace(Json *j)
{
    while (j->pos < j->len && strchr(" \r\n\t", j->s[j->pos]))
        j->pos++;
}
static int jvalue(Json *j, int depth)
{
    jspace(j);
    if (depth > 8 || j->count >= UC_JSON_TOKEN_MAX || j->pos >= j->len)
        return -1;
    int n = j->count++;
    JToken *t = &j->t[n];
    t->start = (int)j->pos;
    t->kind = j->s[j->pos++];
    if (t->kind == '{' || t->kind == '[') {
        char end = t->kind == '{' ? '}' : ']';
        jspace(j);
        if (j->pos < j->len && j->s[j->pos] == end)
            j->pos++;
        else
            for (;;) {
                if (t->kind == '{' && (j->pos >= j->len || j->s[j->pos] != '"'))
                    return -1;
                if (jvalue(j, depth + 1) < 0)
                    return -1;
                if (t->kind == '{') {
                    jspace(j);
                    if (j->pos >= j->len || j->s[j->pos++] != ':')
                        return -1;
                    if (jvalue(j, depth + 1) < 0)
                        return -1;
                }
                jspace(j);
                if (j->pos >= j->len)
                    return -1;
                char c = j->s[j->pos++];
                if (c == end)
                    break;
                if (c != ',')
                    return -1;
                jspace(j);
            }
    } else if (t->kind == '"') {
        int done = 0;
        while (j->pos < j->len) {
            unsigned char c = j->s[j->pos++];
            if (c == '"') {
                done = 1;
                break;
            }
            if (c < 32)
                return -1;
            if (c == '\\') {
                if (j->pos >= j->len)
                    return -1;
                char e = j->s[j->pos++];
                if (e == 'u') {
                    for (int z = 0; z < 4; z++) {
                        if (j->pos >= j->len || !strchr("0123456789abcdefABCDEF", j->s[j->pos++]))
                            return -1;
                    }
                } else if (!strchr("\"\\/bfnrt", e))
                    return -1;
            }
        }
        if (!done)
            return -1;
    } else {
        while (j->pos < j->len && !strchr(" ,]}\r\n\t", j->s[j->pos]))
            j->pos++;
        size_t len = j->pos - (size_t)t->start;
        const char *p = j->s + t->start;
        if ((len == 4 && !memcmp(p, "true", 4)) || (len == 5 && !memcmp(p, "false", 5)) ||
            (len == 4 && !memcmp(p, "null", 4))) {
        } else {
            size_t k = 0;
            if (p[k] == '-')
                k++;
            if (k >= len)
                return -1;
            if (p[k] == '0')
                k++;
            else {
                if (p[k] < '1' || p[k] > '9')
                    return -1;
                while (k < len && p[k] >= '0' && p[k] <= '9')
                    k++;
            }
            if (k < len && p[k] == '.') {
                k++;
                size_t st = k;
                while (k < len && p[k] >= '0' && p[k] <= '9')
                    k++;
                if (k == st)
                    return -1;
            }
            if (k < len && (p[k] == 'e' || p[k] == 'E')) {
                k++;
                if (k < len && (p[k] == '+' || p[k] == '-'))
                    k++;
                size_t st = k;
                while (k < len && p[k] >= '0' && p[k] <= '9')
                    k++;
                if (k == st)
                    return -1;
            }
            if (k != len)
                return -1;
        }
    }
    t->end = (int)j->pos;
    t->next = j->count;
    return n;
}
static int jparse(Json *j, const char *s, size_t len)
{
    memset(j, 0, sizeof(*j));
    j->s = s;
    j->len = len;
    if (jvalue(j, 0) != 0)
        return -1;
    jspace(j);
    return j->pos == len ? 0 : -1;
}
static int jeq(Json *j, int n, const char *s)
{
    JToken *t = &j->t[n];
    return t->kind == '"' && t->end - t->start == (int)strlen(s) + 2 &&
           !memcmp(j->s + t->start + 1, s, strlen(s));
}
/* Reject duplicate keys and unknown fields, including escaped aliases. */
static int jobject(Json *j, const char *const *keys, int count)
{
    if (j->t[0].kind != '{')
        return -1;
    uint64_t seen = 0;
    for (int n = 1; n < j->t[0].next;) {
        int k;
        for (k = 0; k < count; k++)
            if (jeq(j, n, keys[k]))
                break;
        if (k == count || (seen & (1ULL << k)))
            return -1;
        seen |= 1ULL << k;
        n = j->t[n + 1].next;
    }
    return 0;
}
static int jget(Json *j, const char *key)
{
    for (int n = 1; n < j->t[0].next;) {
        if (jeq(j, n, key))
            return n + 1;
        n = j->t[n + 1].next;
    }
    return -1;
}
static int jint(Json *j, int n, int64_t min, int64_t max, int64_t *out)
{
    if (n < 0)
        return -1;
    JToken *t = &j->t[n];
    int len = t->end - t->start;
    if (len <= 0 || len > 20)
        return -1;
    char b[24];
    memcpy(b, j->s + t->start, (size_t)len);
    b[len] = 0;
    for (int i = b[0] == '-' ? 1 : 0; i < len; i++)
        if (b[i] < '0' || b[i] > '9')
            return -1; /* Parse magnitude with overflow checks. */
    uint64_t v = 0;
    int neg = b[0] == '-';
    for (int i = neg; i < len; i++) {
        unsigned d = (unsigned)(b[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    if (v > INT64_MAX)
        return -1;
    int64_t value = neg ? -(int64_t)v : (int64_t)v;
    if (value < min || value > max)
        return -1;
    *out = value;
    return 0;
}
static int jbool(Json *j, int n, int *out)
{
    if (n < 0)
        return -1;
    JToken *t = &j->t[n];
    int l = t->end - t->start;
    if (l == 4 && !memcmp(j->s + t->start, "true", 4)) {
        *out = 1;
        return 0;
    }
    if (l == 5 && !memcmp(j->s + t->start, "false", 5)) {
        *out = 0;
        return 0;
    }
    return -1;
}
#endif
