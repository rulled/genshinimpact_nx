#include "json_min.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MIN_MAX_DEPTH 32u

typedef struct {
  const char *cursor;
  const char *end;
} JsonCursor;

static void skip_space(JsonCursor *cursor) {
  while (cursor->cursor < cursor->end) {
    const char c = *cursor->cursor;
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    ++cursor->cursor;
  }
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int read_hex4(const char *text, const char *end, unsigned *out) {
  if ((size_t)(end - text) < 4) return -1;
  unsigned value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    const int digit = hex_digit(text[index]);
    if (digit < 0) return -1;
    value = (value << 4) | (unsigned)digit;
  }
  *out = value;
  return 0;
}

static int read_utf8(const char **text, const char *end, unsigned *out) {
  if (!text || !*text || *text >= end || !out) return -1;
  const unsigned char *cursor = (const unsigned char *)*text;
  const unsigned char *limit = (const unsigned char *)end;
  const unsigned char first = *cursor++;
  unsigned codepoint = 0;
  unsigned continuation = 0;
  unsigned minimum = 0;
  if (first < 0x80) {
    *text = (const char *)cursor;
    *out = first;
    return 0;
  }
  if (first >= 0xc2 && first <= 0xdf) {
    codepoint = first & 0x1fu;
    continuation = 1;
    minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    codepoint = first & 0x0fu;
    continuation = 2;
    minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    codepoint = first & 0x07u;
    continuation = 3;
    minimum = 0x10000;
  } else {
    return -1;
  }
  if ((size_t)(limit - cursor) < continuation) return -1;
  for (unsigned index = 0; index < continuation; ++index) {
    if ((cursor[index] & 0xc0u) != 0x80u) return -1;
    codepoint = (codepoint << 6) | (cursor[index] & 0x3fu);
  }
  cursor += continuation;
  if (codepoint < minimum || codepoint > 0x10ffffu ||
      (codepoint >= 0xd800u && codepoint <= 0xdfffu))
    return -1;
  *text = (const char *)cursor;
  *out = codepoint;
  return 0;
}

static int skip_string(JsonCursor *cursor) {
  if (cursor->cursor >= cursor->end || *cursor->cursor++ != '"') return -1;
  while (cursor->cursor < cursor->end) {
    const unsigned char c = (unsigned char)*cursor->cursor++;
    if (c == '"') return 0;
    if (c < 0x20) return -1;
    if (c >= 0x80) {
      --cursor->cursor;
      unsigned codepoint = 0;
      if (read_utf8(&cursor->cursor, cursor->end, &codepoint) != 0)
        return -1;
      continue;
    }
    if (c != '\\') continue;
    if (cursor->cursor >= cursor->end) return -1;
    const char escaped = *cursor->cursor++;
    if (strchr("\"\\/bfnrt", escaped)) continue;
    if (escaped != 'u' || (size_t)(cursor->end - cursor->cursor) < 4)
      return -1;
    unsigned codepoint = 0;
    if (read_hex4(cursor->cursor, cursor->end, &codepoint) != 0) return -1;
    cursor->cursor += 4;
    /* Decoded strings are returned as C strings; never admit an embedded NUL. */
    if (codepoint == 0) return -1;
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
      if ((size_t)(cursor->end - cursor->cursor) < 6 ||
          cursor->cursor[0] != '\\' || cursor->cursor[1] != 'u')
        return -1;
      unsigned low = 0;
      if (read_hex4(cursor->cursor + 2, cursor->end, &low) != 0 ||
          low < 0xdc00 || low > 0xdfff)
        return -1;
      cursor->cursor += 6;
    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
      return -1;
    }
  }
  return -1;
}

static int skip_value(JsonCursor *cursor, unsigned depth, JsonMinType *type);

static int skip_container(JsonCursor *cursor, unsigned depth, char open,
                          char close, JsonMinType type, JsonMinType *out_type) {
  if (depth >= JSON_MIN_MAX_DEPTH || cursor->cursor >= cursor->end ||
      *cursor->cursor++ != open)
    return -1;
  skip_space(cursor);
  if (cursor->cursor < cursor->end && *cursor->cursor == close) {
    ++cursor->cursor;
    *out_type = type;
    return 0;
  }
  for (;;) {
    if (open == '{') {
      if (skip_string(cursor) != 0) return -1;
      skip_space(cursor);
      if (cursor->cursor >= cursor->end || *cursor->cursor++ != ':') return -1;
      skip_space(cursor);
    }
    JsonMinType child_type = JSON_MIN_INVALID;
    if (skip_value(cursor, depth + 1, &child_type) != 0) return -1;
    skip_space(cursor);
    if (cursor->cursor >= cursor->end) return -1;
    if (*cursor->cursor == close) {
      ++cursor->cursor;
      *out_type = type;
      return 0;
    }
    if (*cursor->cursor++ != ',') return -1;
    skip_space(cursor);
  }
}

static int skip_number(JsonCursor *cursor) {
  const char *p = cursor->cursor;
  if (p < cursor->end && *p == '-') ++p;
  if (p >= cursor->end) return -1;
  if (*p == '0') {
    ++p;
    if (p < cursor->end && *p >= '0' && *p <= '9') return -1;
  } else {
    if (*p < '1' || *p > '9') return -1;
    while (p < cursor->end && *p >= '0' && *p <= '9') ++p;
  }
  if (p < cursor->end && *p == '.') {
    ++p;
    if (p >= cursor->end || *p < '0' || *p > '9') return -1;
    while (p < cursor->end && *p >= '0' && *p <= '9') ++p;
  }
  if (p < cursor->end && (*p == 'e' || *p == 'E')) {
    ++p;
    if (p < cursor->end && (*p == '+' || *p == '-')) ++p;
    if (p >= cursor->end || *p < '0' || *p > '9') return -1;
    while (p < cursor->end && *p >= '0' && *p <= '9') ++p;
  }
  cursor->cursor = p;
  return 0;
}

static int consume_literal(JsonCursor *cursor, const char *literal,
                           JsonMinType type, JsonMinType *out_type) {
  const size_t size = strlen(literal);
  if ((size_t)(cursor->end - cursor->cursor) < size ||
      memcmp(cursor->cursor, literal, size))
    return -1;
  cursor->cursor += size;
  *out_type = type;
  return 0;
}

static int skip_value(JsonCursor *cursor, unsigned depth, JsonMinType *type) {
  skip_space(cursor);
  if (cursor->cursor >= cursor->end) return -1;
  switch (*cursor->cursor) {
    case '{':
      return skip_container(cursor, depth, '{', '}', JSON_MIN_OBJECT, type);
    case '[':
      return skip_container(cursor, depth, '[', ']', JSON_MIN_ARRAY, type);
    case '"':
      if (skip_string(cursor) != 0) return -1;
      *type = JSON_MIN_STRING;
      return 0;
    case 't': return consume_literal(cursor, "true", JSON_MIN_TRUE, type);
    case 'f': return consume_literal(cursor, "false", JSON_MIN_FALSE, type);
    case 'n': return consume_literal(cursor, "null", JSON_MIN_NULL, type);
    default:
      if (skip_number(cursor) != 0) return -1;
      *type = JSON_MIN_NUMBER;
      return 0;
  }
}

int json_min_parse(const char *json, size_t size, JsonMinValue *out) {
  if (!json || !out || !size || memchr(json, 0, size)) return -1;
  JsonCursor cursor = { json, json + size };
  skip_space(&cursor);
  const char *start = cursor.cursor;
  JsonMinType type = JSON_MIN_INVALID;
  if (skip_value(&cursor, 0, &type) != 0) return -1;
  const char *end = cursor.cursor;
  skip_space(&cursor);
  if (cursor.cursor != cursor.end) return -1;
  out->start = start;
  out->length = (size_t)(end - start);
  out->type = type;
  return 0;
}

static int string_matches_key(const char *start, const char *end,
                              const char *key) {
  if (start >= end || *start++ != '"') return 0;
  const unsigned char *expected = (const unsigned char *)key;
  while (start < end && *start != '"') {
    unsigned codepoint = (unsigned char)*start++;
    if (codepoint == '\\') {
      if (start >= end) return 0;
      const char escaped = *start++;
      if (escaped == 'u') {
        if (read_hex4(start, end, &codepoint) != 0) return 0;
        start += 4;
      } else {
        const char *escapes = "\"\\/bfnrt";
        const char *mapped = "\"\\/\b\f\n\r\t";
        const char *at = strchr(escapes, escaped);
        if (!at) return 0;
        codepoint = (unsigned char)mapped[at - escapes];
      }
    }
    if (codepoint > 0x7f || !*expected || codepoint != *expected++) return 0;
  }
  return start < end && *start == '"' && !*expected;
}

int json_min_object_get(const JsonMinValue *object, const char *key,
                        JsonMinValue *out) {
  if (!object || object->type != JSON_MIN_OBJECT || !key || !out ||
      object->length < 2)
    return -1;
  JsonCursor cursor = { object->start + 1,
                        object->start + object->length - 1 };
  skip_space(&cursor);
  while (cursor.cursor < cursor.end) {
    const char *key_start = cursor.cursor;
    if (skip_string(&cursor) != 0) return -1;
    const char *key_end = cursor.cursor;
    skip_space(&cursor);
    if (cursor.cursor >= cursor.end || *cursor.cursor++ != ':') return -1;
    skip_space(&cursor);
    const char *value_start = cursor.cursor;
    JsonMinType type = JSON_MIN_INVALID;
    if (skip_value(&cursor, 1, &type) != 0) return -1;
    const char *value_end = cursor.cursor;
    if (string_matches_key(key_start, key_end, key)) {
      out->start = value_start;
      out->length = (size_t)(value_end - value_start);
      out->type = type;
      return 0;
    }
    skip_space(&cursor);
    if (cursor.cursor == cursor.end) break;
    if (*cursor.cursor++ != ',') return -1;
    skip_space(&cursor);
  }
  return 1;
}

static int array_walk(const JsonMinValue *array, size_t wanted,
                      JsonMinValue *out, size_t *out_count) {
  if (!array || array->type != JSON_MIN_ARRAY || array->length < 2 ||
      (!out && !out_count))
    return -1;
  JsonCursor cursor = { array->start + 1,
                        array->start + array->length - 1 };
  skip_space(&cursor);
  size_t count = 0;
  while (cursor.cursor < cursor.end) {
    const char *value_start = cursor.cursor;
    JsonMinType type = JSON_MIN_INVALID;
    if (skip_value(&cursor, 1, &type) != 0) return -1;
    const char *value_end = cursor.cursor;
    if (out && count == wanted) {
      out->start = value_start;
      out->length = (size_t)(value_end - value_start);
      out->type = type;
      return 0;
    }
    ++count;
    skip_space(&cursor);
    if (cursor.cursor == cursor.end) break;
    if (*cursor.cursor++ != ',') return -1;
    skip_space(&cursor);
  }
  if (out_count) {
    *out_count = count;
    return 0;
  }
  return 1;
}

int json_min_array_count(const JsonMinValue *array, size_t *out_count) {
  return array_walk(array, 0, NULL, out_count);
}

int json_min_array_get(const JsonMinValue *array, size_t index,
                       JsonMinValue *out) {
  return array_walk(array, index, out, NULL);
}

int json_min_int64(const JsonMinValue *value, int64_t *out) {
  if (!value || value->type != JSON_MIN_NUMBER || !out ||
      !value->length || value->length >= 64)
    return -1;
  char buffer[64];
  memcpy(buffer, value->start, value->length);
  buffer[value->length] = 0;
  if (strchr(buffer, '.') || strchr(buffer, 'e') || strchr(buffer, 'E'))
    return -1;
  errno = 0;
  char *end = NULL;
  const long long parsed = strtoll(buffer, &end, 10);
  if (errno == ERANGE || !end || *end) return -1;
  *out = (int64_t)parsed;
  return 0;
}

int json_min_bool(const JsonMinValue *value, int *out) {
  if (!value || !out) return -1;
  if (value->type == JSON_MIN_TRUE) *out = 1;
  else if (value->type == JSON_MIN_FALSE) *out = 0;
  else return -1;
  return 0;
}

static int append_utf8(char **output, size_t *remaining, unsigned codepoint) {
  unsigned char encoded[4];
  size_t count = 0;
  if (codepoint <= 0x7f) {
    encoded[count++] = (unsigned char)codepoint;
  } else if (codepoint <= 0x7ff) {
    encoded[count++] = 0xc0u | (unsigned char)(codepoint >> 6);
    encoded[count++] = 0x80u | (unsigned char)(codepoint & 0x3f);
  } else if (codepoint <= 0xffff) {
    encoded[count++] = 0xe0u | (unsigned char)(codepoint >> 12);
    encoded[count++] = 0x80u | (unsigned char)((codepoint >> 6) & 0x3f);
    encoded[count++] = 0x80u | (unsigned char)(codepoint & 0x3f);
  } else if (codepoint <= 0x10ffff) {
    encoded[count++] = 0xf0u | (unsigned char)(codepoint >> 18);
    encoded[count++] = 0x80u | (unsigned char)((codepoint >> 12) & 0x3f);
    encoded[count++] = 0x80u | (unsigned char)((codepoint >> 6) & 0x3f);
    encoded[count++] = 0x80u | (unsigned char)(codepoint & 0x3f);
  } else {
    return -1;
  }
  if (*remaining <= count) return -1;
  memcpy(*output, encoded, count);
  *output += count;
  *remaining -= count;
  return 0;
}

int json_min_string(const JsonMinValue *value, char *out,
                    size_t out_capacity) {
  if (!value || value->type != JSON_MIN_STRING || !out || !out_capacity ||
      value->length < 2 || value->start[0] != '"' ||
      value->start[value->length - 1] != '"')
    return -1;
  const char *cursor = value->start + 1;
  const char *end = value->start + value->length - 1;
  char *output = out;
  size_t remaining = out_capacity;
  while (cursor < end) {
    unsigned codepoint = (unsigned char)*cursor++;
    if (codepoint >= 0x80) {
      --cursor;
      if (read_utf8(&cursor, end, &codepoint) != 0) return -1;
    }
    if (codepoint == '\\') {
      if (cursor >= end) return -1;
      const char escaped = *cursor++;
      if (escaped == 'u') {
        if (read_hex4(cursor, end, &codepoint) != 0) return -1;
        cursor += 4;
        if (codepoint == 0) return -1;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if ((size_t)(end - cursor) < 6 || cursor[0] != '\\' ||
              cursor[1] != 'u') return -1;
          unsigned low = 0;
          if (read_hex4(cursor + 2, end, &low) != 0 ||
              low < 0xdc00 || low > 0xdfff) return -1;
          cursor += 6;
          codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) +
                      (low - 0xdc00u);
        }
      } else {
        const char *escapes = "\"\\/bfnrt";
        const char *mapped = "\"\\/\b\f\n\r\t";
        const char *at = strchr(escapes, escaped);
        if (!at) return -1;
        codepoint = (unsigned char)mapped[at - escapes];
      }
    }
    if (append_utf8(&output, &remaining, codepoint) != 0) return -1;
  }
  *output = 0;
  return 0;
}
