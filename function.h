#pragma once

#include <memory>

struct bad_function_call : std::exception {};

using storage_t = std::aligned_storage_t<sizeof(void*), alignof(void*)>;

template <typename T>
constexpr inline bool fits_small =
    sizeof(T) < sizeof(storage_t) && alignof(storage_t) % alignof(T) == 0 &&
    std::is_nothrow_move_assignable_v<T> &&
    std::is_nothrow_move_constructible_v<T>;

template <typename T>
T* small_cast(storage_t& storage) {
  return reinterpret_cast<T*>(&storage);
}

template <typename T>
T const* small_cast(storage_t const& storage) {
  return reinterpret_cast<T const*>(&storage);
}

template <typename T>
T* big_cast(storage_t& storage) {
  return *reinterpret_cast<T**>(&storage);
}

template <typename T>
T* big_cast(storage_t const& storage) {
  return *reinterpret_cast<T* const*>(&storage);
}

template <typename T>
T* cast(storage_t& storage) {
  if constexpr (fits_small<T>) {
    return small_cast<T>(storage);
  } else {
    return big_cast<T>(storage);
  }
}

template <typename T>
T const* cast(storage_t const& storage) {
  if constexpr (fits_small<T>) {
    return small_cast<T>(storage);
  } else {
    return big_cast<T>(storage);
  }
}

template <typename R, typename... Args>
struct type_descriptor {

  void (*copy)(storage_t const& src, storage_t& dst);
  void (*move)(storage_t& src, storage_t& dst);
  void (*destroy)(storage_t& src);
  R (*invoke)(storage_t const& src, Args&&... args);

  static type_descriptor<R, Args...> const*
  get_empty_func_descriptor() noexcept {
    constexpr static type_descriptor<R, Args...> result = {
        +[](storage_t const& src, storage_t& dst) { dst = src; },
        +[](storage_t& src, storage_t& dst) { dst = src; }, +[](storage_t&) {},
        +[](storage_t const&, Args&&...) -> R { throw bad_function_call{}; }};

    return &result;
  }

  template <typename T>
  static type_descriptor<R, Args...> const* get_descriptor() noexcept {
    constexpr static type_descriptor<R, Args...> result = {
        +[](storage_t const& src, storage_t& dst) {
          if constexpr (fits_small<T>) {
            new (&dst) T(*small_cast<T>(src));
          } else {
            auto ptr = new T(*big_cast<T>(src));
            new (&dst) T*(ptr);
          }
        },
        +[](storage_t& src, storage_t& dst) {
          if constexpr (fits_small<T>) {
            new (&dst) T(std::move(*small_cast<T>(src)));
          } else {
            new (&dst) T*(big_cast<T>(src));
          }
        },
        +[](storage_t& src) {
          if constexpr (fits_small<T>) {
            small_cast<T>(src)->~T();
          } else {
            delete big_cast<T>(src);
          }
        },
        +[](storage_t const& src, Args&&... args) -> R {
          return cast<T>(src)->operator()(std::forward<Args>(args)...);
        }};

    return &result;
  }
};

template <typename F>
struct function;

template <typename R, typename... Args>
struct function<R(Args...)> {
  function() noexcept
      : desc(type_descriptor<R, Args...>::get_empty_func_descriptor()) {}

  function(function const& other) : desc(other.desc) {
    other.desc->copy(other.storage, this->storage);
  }

  function(function&& other) : desc(other.desc) {
    other.desc = type_descriptor<R, Args...>::get_empty_func_descriptor();
    desc->move(other.storage, this->storage);
  }

  template <typename T>
  function(T val)
      : desc(type_descriptor<R, Args...>::template get_descriptor<T>()) {
    if constexpr (fits_small<T>) {
      new (&storage) T(std::move(val));
    } else {
      auto ptr = new T(std::move(val));
      new (&storage) T*(ptr);
    }
  }

  function& operator=(function const& other) {
    if (this != &other) {
      storage_t buf;
      other.desc->copy(other.storage, buf);
      this->desc->destroy(this->storage);
      desc = other.desc;
      storage = buf;
    }
    return *this;
  }

  function& operator=(function&& other) noexcept {
    if (this != &other) {
      desc->destroy(storage);
      desc = other.desc;
      other.desc = type_descriptor<R, Args...>::get_empty_func_descriptor();
      desc->move(other.storage, this->storage);
    }
    return *this;
  }

  ~function() {
    desc->destroy(storage);
  }

  explicit operator bool() const noexcept {
    return type_descriptor<R, Args...>::get_empty_func_descriptor() != desc;
  }

  R operator()(Args&&... args) const {
    return desc->invoke(storage, std::forward<Args>(args)...);
  }

  template <typename T>
  T* target() noexcept {
    if (*this &&
        type_descriptor<R, Args...>::template get_descriptor<T>() == desc) {
      return cast<T>(storage);
    }

    return nullptr;
  }

  template <typename T>
  T const* target() const noexcept {
    if (*this &&
        type_descriptor<R, Args...>::template get_descriptor<T>() == desc) {
      return cast<T>(storage);
    }

    return nullptr;
  }

private:
  storage_t storage; // 8 bytes OR 4 Byt, char[8] or char[4]
  type_descriptor<R, Args...> const* desc;
};
