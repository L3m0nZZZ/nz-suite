/* 极简 JSON 解析器
 *
 * 不引入任何第三方依赖（单 exe 绿色运行的前提）。
 * 只做解析，不做序列化——序列化在 config_save_state 里手写，
 * 因为那里要输出的结构是我们自己定的固定形状。
 *
 * 支持对象/数组/字符串/数字/true/false/null，含 \uXXXX 转义。
 * 字符串里的 \u 会被转成 UTF-8 再存回 char*，
 * 因为调用方（正则/子串匹配/URL）都按 UTF-8 处理。
 */
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *p;
    int depth;
    int failed;
} JParser;

static void skip_ws(JParser *j)
{
    for (;;) {
        unsigned char c = (unsigned char)*j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { j->p++; continue; }
        /* 容错：部分站点返回里带 BOM 或裸注释块。
         * 先判 p[1] 非零再读 p[2]，避免在缓冲区末尾多读一个字节 */
        if (c == 0xEF && (unsigned char)j->p[1] != 0 &&
            (unsigned char)j->p[1] == 0xBB && (unsigned char)j->p[2] == 0xBF) {
            j->p += 3; continue;
        }
        break;
    }
}

/* 把单个 \uXXXX 转成 UTF-8，返回写入的字节数 */
static int utf8_from_codepoint(unsigned cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *parse_string(JParser *j)
{
    if (*j->p != '"') { j->failed = 1; return NULL; }
    j->p++;
    size_t cap = 32, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) { j->failed = 1; return NULL; }
    while (*j->p && *j->p != '"') {
        if (len + 8 >= cap) {
            cap *= 2;
            char *nn = (char *)realloc(out, cap);
            if (!nn) { free(out); j->failed = 1; return NULL; }
            out = nn;
        }
        char c = *j->p;
        if (c == '\\') {
            j->p++;
            char e = *j->p;
            switch (e) {
                case 'n': out[len++] = '\n'; j->p++; break;
                case 't': out[len++] = '\t'; j->p++; break;
                case 'r': out[len++] = '\r'; j->p++; break;
                case 'b': out[len++] = '\b'; j->p++; break;
                case 'f': out[len++] = '\f'; j->p++; break;
                case '/': out[len++] = '/';  j->p++; break;
                case '"': out[len++] = '"';  j->p++; break;
                case '\\': out[len++] = '\\'; j->p++; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(j->p + 1, &cp)) { free(out); j->failed = 1; return NULL; }
                    j->p += 5;
                    /* 高低代理对 */
                    if (cp >= 0xD800 && cp <= 0xDBFF && j->p[0] == '\\' && j->p[1] == 'u') {
                        unsigned lo = 0;
                        if (hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            j->p += 6;
                        }
                    }
                    len += (size_t)utf8_from_codepoint(cp, out + len);
                    break;
                }
                default: out[len++] = e; j->p++; break;
            }
        } else {
            out[len++] = c;
            j->p++;
        }
    }
    if (*j->p != '"') { free(out); j->failed = 1; return NULL; }
    j->p++;
    out[len] = '\0';
    return out;
}

static JValue *parse_value(JParser *j);

static JValue *parse_object(JParser *j)
{
    JValue *v = (JValue *)calloc(1, sizeof(JValue));
    if (!v) { j->failed = 1; return NULL; }
    v->type = JSON_OBJECT;
    v->members = NULL;
    j->p++;  /* { */
    skip_ws(j);
    if (*j->p == '}') { j->p++; return v; }
    for (;;) {
        skip_ws(j);
        char *key = parse_string(j);
        if (j->failed) { free(key); json_free(v); return NULL; }
        skip_ws(j);
        if (*j->p != ':') { free(key); json_free(v); j->failed = 1; return NULL; }
        j->p++;
        JValue *child = parse_value(j);
        if (j->failed) { free(key); json_free(child); json_free(v); return NULL; }
        JMemb *m = (JMemb *)realloc(v->members, sizeof(JMemb) * (size_t)(v->n + 1));
        if (!m) { free(key); json_free(child); json_free(v); j->failed = 1; return NULL; }
        v->members = m;
        m[v->n].key = key;
        m[v->n].val = child;
        v->n++;
        skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return v; }
        json_free(v); j->failed = 1; return NULL;
    }
}

static JValue *parse_array(JParser *j)
{
    JValue *v = (JValue *)calloc(1, sizeof(JValue));
    if (!v) { j->failed = 1; return NULL; }
    v->type = JSON_ARRAY;
    j->p++;  /* [ */
    skip_ws(j);
    if (*j->p == ']') { j->p++; return v; }
    for (;;) {
        JValue *child = parse_value(j);
        if (j->failed) { json_free(child); json_free(v); return NULL; }
        JValue *items = (JValue *)realloc(v->items, sizeof(JValue) * (size_t)(v->n + 1));
        if (!items) { json_free(child); json_free(v); j->failed = 1; return NULL; }
        v->items = items;
        /* 数组用 items 数组平铺，num 字段记录数组下标 */
        v->items[v->n] = *child;
        v->n++;
        free(child);
        skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return v; }
        json_free(v); j->failed = 1; return NULL;
    }
}

static JValue *parse_value(JParser *j)
{
    skip_ws(j);
    if (++j->depth > 64) { j->failed = 1; return NULL; }
    JValue *r = NULL;
    char c = *j->p;
    if (c == '{') {
        r = parse_object(j);
    } else if (c == '[') {
        r = parse_array(j);
    } else if (c == '"') {
        r = (JValue *)calloc(1, sizeof(JValue));
        if (r) {
            r->type = JSON_STRING;
            r->str = parse_string(j);
            if (j->failed) { free(r->str); free(r); r = NULL; }
        }
    } else if (strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        r = (JValue *)calloc(1, sizeof(JValue));
        if (r) { r->type = JSON_BOOL; r->num = 1; }
    } else if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        r = (JValue *)calloc(1, sizeof(JValue));
        if (r) { r->type = JSON_BOOL; r->num = 0; }
    } else if (strncmp(j->p, "null", 4) == 0) {
        j->p += 4;
        r = (JValue *)calloc(1, sizeof(JValue));
        if (r) r->type = JSON_NULL;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        double d = strtod(j->p, &end);
        if (end == j->p) { j->failed = 1; return NULL; }
        j->p = end;
        r = (JValue *)calloc(1, sizeof(JValue));
        if (r) { r->type = JSON_NUMBER; r->num = d; }
    } else {
        j->failed = 1;
    }
    j->depth--;
    return r;
}

JValue *json_parse(const char *text)
{
    if (!text) return NULL;
    JParser j;
    j.p = text;
    j.depth = 0;
    j.failed = 0;
    JValue *v = parse_value(&j);
    if (j.failed) return NULL;
    return v;
}

/* 释放「内容」，但不释放节点本身。
 *
 * 为什么必须有这个函数：数组元素是按值内联存放在 items[] 数组里的
 * （parse_array 里 `v->items[v->n] = *child; free(child);`），
 * 所以数组元素根本不是独立的堆块。如果对它调 free()，
 * 就是对「堆块中间的一个地址」做 free —— 立刻破坏堆，
 * 而且要等到后面某次无关的分配才炸（表现为乱码地址崩溃、时间随机）。
 */
static void json_free_inner(JValue *v)
{
    if (!v) return;
    if (v->type == JSON_OBJECT) {
        for (int i = 0; i < v->n; i++) {
            free(v->members[i].key);
            json_free(v->members[i].val);
        }
        free(v->members);
        v->members = NULL;
    } else if (v->type == JSON_ARRAY) {
        /* 元素内联，只递归清内容 */
        for (int i = 0; i < v->n; i++) json_free_inner(&v->items[i]);
        free(v->items);
        v->items = NULL;
    } else if (v->type == JSON_STRING) {
        free(v->str);
        v->str = NULL;
    }
    v->n = 0;
}

void json_free(JValue *v)
{
    if (!v) return;
    json_free_inner(v);
    free(v);          /* 只有真正 calloc 出来的节点才能走到这里 */
}

const JValue *json_get(const JValue *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->n; i++) {
        if (strcmp(obj->members[i].key, key) == 0) return obj->members[i].val;
    }
    return NULL;
}

const JValue *json_at(const JValue *arr, int i)
{
    if (!arr || arr->type != JSON_ARRAY || i < 0 || i >= arr->n) return NULL;
    return &arr->items[i];
}

/* 取字符串字段；缺失或非字符串时返回 def */
const char *json_str(const JValue *obj, const char *key, const char *def)
{
    const JValue *v = json_get(obj, key);
    if (!v || v->type != JSON_STRING) return def;
    return v->str;
}

/* 取数字字段；字符串数字也接受（很多接口把 id 写成字符串） */
double json_num(const JValue *obj, const char *key, double def)
{
    const JValue *v = json_get(obj, key);
    if (!v) return def;
    if (v->type == JSON_NUMBER) return v->num;
    if (v->type == JSON_BOOL) return v->num;
    if (v->type == JSON_STRING && v->str && v->str[0]) return strtod(v->str, NULL);
    return def;
}

int json_bool(const JValue *obj, const char *key, int def)
{
    const JValue *v = json_get(obj, key);
    if (!v) return def;
    if (v->type == JSON_BOOL) return v->num != 0;
    if (v->type == JSON_NUMBER) return v->num != 0;
    return def;
}