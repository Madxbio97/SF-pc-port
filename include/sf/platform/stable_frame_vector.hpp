#pragma once

#include "sf/core/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sf::platform {

// The PsyCross ordering table stores raw packet addresses. This frame-local
// vector separates allocation from packet emission and rejects an exhausted
// budget before std::vector can move packets already linked into the OT.
template <typename Value> class StableFrameVector final {
public:
  explicit StableFrameVector(std::string_view label) : label_(label) {}

  StableFrameVector(const StableFrameVector &) = delete;
  StableFrameVector &operator=(const StableFrameVector &) = delete;
  StableFrameVector(StableFrameVector &&) = delete;
  StableFrameVector &operator=(StableFrameVector &&) = delete;

  void reset() noexcept {
    values_.clear();
    locked_ = false;
    locked_storage_ = nullptr;
  }

  void reserve(std::size_t capacity) {
    if (locked_) {
      fail("cannot reserve after packet emission started");
    }
    values_.reserve(capacity);
  }

  void lockStorage() noexcept {
    locked_storage_ = values_.data();
    locked_ = true;
  }

  template <typename... Arguments>
  Value &emplace_back(Arguments &&...arguments) {
    if (!locked_) {
      fail("packet emission started before storage was locked");
    }
    if (values_.size() >= values_.capacity()) {
      fail("packet budget was exhausted");
    }
    return values_.emplace_back(std::forward<Arguments>(arguments)...);
  }

  void pop_back() noexcept { values_.pop_back(); }

  [[nodiscard]] Value *data() noexcept { return values_.data(); }
  [[nodiscard]] const Value *data() const noexcept { return values_.data(); }
  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return values_.capacity();
  }
  [[nodiscard]] bool storageStable() const noexcept {
    return locked_ && values_.data() == locked_storage_;
  }

private:
  [[noreturn]] void fail(std::string_view reason) const {
    throw core::Error{core::ErrorCode::invalid_format,
                      label_ + ": " + std::string{reason}};
  }

  std::vector<Value> values_;
  std::string label_;
  const Value *locked_storage_{};
  bool locked_{};
};

} // namespace sf::platform
