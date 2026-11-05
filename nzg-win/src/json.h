/* 极简 JSON 解析器接口 */
#ifndef NZG_JSON_H
#define NZG_JSON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JType;

typedef struct JValue JValue;

typedef struct {
    char   *key;
    JValue *val;
} JMemb;

struct JValue {
    JType type;
    double num;                 /* NUMBER / BOOL */
    char  *str;                 /* STRING（UTF-8） */
    /* ARRAY：用 items 平铺，n 为长度 */
    JValue *items;
    /* OBJECT：n 对应 members */
    JMemb *members;
    int n;
};

JValue      *json_parse(const char *text);
void         json_free(JValue *v);
const JValue *json_get(const JValue *obj, const char *key);
const JValue *json_at(const JValue *arr, int i);
const char  *json_str(const JValue *obj, const char *key, const char *def);
double       json_num(const JValue *obj, const char *key, double def);
int          json_bool(const JValue *obj, const char *key, int def);

#endif /* NZG_JSON_H */