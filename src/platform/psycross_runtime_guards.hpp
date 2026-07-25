#pragma once

namespace sf::platform::detail {

class RelativeMouseCapture final {
public:
  ~RelativeMouseCapture();

  void set(bool enabled) noexcept;
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }

private:
  bool enabled_{};
};

} // namespace sf::platform::detail
