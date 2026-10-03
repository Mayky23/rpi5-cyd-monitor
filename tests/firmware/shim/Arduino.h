// Minimal Arduino replacement to unit-test firmware headers on the PC.
// Only implements what panel_data.h and runtime_config.h use.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using std::max;
using std::min;

#define constrain(value, low, high) ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)))
#define PROGMEM

class String {
 public:
  String() = default;
  String(const char *text) : data_(text ? text : "") {}
  String(const std::string &text) : data_(text) {}
  String(char c) : data_(1, c) {}
  String(int value) : data_(std::to_string(value)) {}
  String(unsigned value) : data_(std::to_string(value)) {}
  String(long value) : data_(std::to_string(value)) {}
  String(unsigned long value) : data_(std::to_string(value)) {}

  size_t length() const { return data_.size(); }
  const char *c_str() const { return data_.c_str(); }
  char operator[](size_t index) const { return index < data_.size() ? data_[index] : '\0'; }

  bool startsWith(const String &prefix) const { return data_.rfind(prefix.data_, 0) == 0; }
  bool endsWith(const String &suffix) const {
    return data_.size() >= suffix.data_.size() &&
           data_.compare(data_.size() - suffix.data_.size(), suffix.data_.size(), suffix.data_) == 0;
  }
  int indexOf(char c) const {
    const auto at = data_.find(c);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  int lastIndexOf(char c) const {
    const auto at = data_.rfind(c);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  String substring(size_t from) const { return from < data_.size() ? String(data_.substr(from)) : String(); }
  String substring(size_t from, size_t to) const {
    if (from > to) std::swap(from, to);
    if (from >= data_.size()) return String();
    return String(data_.substr(from, to - from));
  }
  void toLowerCase() {
    for (auto &c : data_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  void toUpperCase() {
    for (auto &c : data_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  void replace(const String &find, const String &with) {
    if (find.data_.empty()) return;
    size_t at = 0;
    while ((at = data_.find(find.data_, at)) != std::string::npos) {
      data_.replace(at, find.data_.size(), with.data_);
      at += with.data_.size();
    }
  }
  void remove(size_t index) {
    if (index < data_.size()) data_.erase(index);
  }
  void remove(size_t index, size_t count) {
    if (index < data_.size()) data_.erase(index, count);
  }

  String &operator+=(const String &other) { data_ += other.data_; return *this; }
  String &operator+=(const char *other) { data_ += other ? other : ""; return *this; }
  String &operator+=(char c) { data_ += c; return *this; }

  friend String operator+(const String &a, const String &b) { return String(a.data_ + b.data_); }
  friend String operator+(const String &a, const char *b) { return String(a.data_ + (b ? b : "")); }
  friend String operator+(const char *a, const String &b) { return String((a ? a : "") + b.data_); }
  friend bool operator==(const String &a, const String &b) { return a.data_ == b.data_; }
  friend bool operator==(const String &a, const char *b) { return a.data_ == (b ? b : ""); }
  friend bool operator!=(const String &a, const String &b) { return a.data_ != b.data_; }
  friend bool operator<(const String &a, const String &b) { return a.data_ < b.data_; }

 private:
  std::string data_;
};
