/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Small cJSON accessors for the assertions below. Most of what these tests
 * check is the exact bytes on the wire, compared as strings — the parser is
 * only needed where a field's presence or a nested value is the point.
 */
#pragma once

#include <string>

#include "cJSON.h"

namespace jsonx {

/* Owns a parsed document; the tests are short enough that scope is lifetime. */
class Doc {
 public:
  explicit Doc(const std::string& text)
      : doc_(cJSON_ParseWithLength(text.data(), text.size())) {}
  ~Doc() { cJSON_Delete(doc_); }
  Doc(const Doc&) = delete;
  Doc& operator=(const Doc&) = delete;

  bool valid() const { return cJSON_IsObject(doc_); }
  const cJSON* get() const { return doc_; }

 private:
  cJSON* doc_;
};

inline const cJSON* field(const cJSON* obj, const char* key) {
  return cJSON_GetObjectItemCaseSensitive(obj, key);
}

/* 0 when absent, null or not a number — no field these tests read has 0 as a
 * meaningful value. */
inline uint32_t num(const cJSON* obj, const char* key) {
  const cJSON* v = field(obj, key);
  return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0u;
}

/* "" when absent or not a string, so TEST_ASSERT_EQUAL_STRING can be used
 * directly on the result. */
inline const char* str(const cJSON* obj, const char* key) {
  const cJSON* v = field(obj, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

inline bool boolean(const cJSON* obj, const char* key) {
  return cJSON_IsTrue(field(obj, key));
}

/* Present AND false — distinct from absent, which is what a caller checking
 * "the server was told not to restart" actually means. */
inline bool is_false(const cJSON* obj, const char* key) {
  return cJSON_IsFalse(field(obj, key));
}

inline bool has_number(const cJSON* obj, const char* key) {
  return cJSON_IsNumber(field(obj, key));
}

inline bool is_array(const cJSON* obj, const char* key) {
  return cJSON_IsArray(field(obj, key));
}

inline int array_size(const cJSON* obj, const char* key) {
  const cJSON* v = field(obj, key);
  return cJSON_IsArray(v) ? cJSON_GetArraySize(v) : -1;
}

inline const cJSON* at(const cJSON* obj, const char* key, int index) {
  const cJSON* v = field(obj, key);
  return cJSON_IsArray(v) ? cJSON_GetArrayItem(const_cast<cJSON*>(v), index) : nullptr;
}

}  // namespace jsonx
