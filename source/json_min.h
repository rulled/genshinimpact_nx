#ifndef GENSHIN_JSON_MIN_H
#define GENSHIN_JSON_MIN_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  JSON_MIN_INVALID = 0,
  JSON_MIN_OBJECT,
  JSON_MIN_ARRAY,
  JSON_MIN_STRING,
  JSON_MIN_NUMBER,
  JSON_MIN_TRUE,
  JSON_MIN_FALSE,
  JSON_MIN_NULL,
} JsonMinType;

typedef struct {
  const char *start;
  size_t length;
  JsonMinType type;
} JsonMinValue;

/* Validate exactly one complete JSON value and return its bounded span. */
int json_min_parse(const char *json, size_t size, JsonMinValue *out);

/* Look up a direct member of an already validated object. */
int json_min_object_get(const JsonMinValue *object, const char *key,
                        JsonMinValue *out);

/* Count or retrieve direct elements of an already validated array. */
int json_min_array_count(const JsonMinValue *array, size_t *out_count);
int json_min_array_get(const JsonMinValue *array, size_t index,
                       JsonMinValue *out);

int json_min_int64(const JsonMinValue *value, int64_t *out);
int json_min_bool(const JsonMinValue *value, int *out);

/* Decode a JSON string, including UTF-16 surrogate pairs, into UTF-8. */
int json_min_string(const JsonMinValue *value, char *out,
                    size_t out_capacity);

#endif
