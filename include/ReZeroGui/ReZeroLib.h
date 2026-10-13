#pragma once
#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <limits>
#include <cmath>
#include <ReZeroGui/Allocator.h>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4141)
#endif

#if defined(_MSC_VER)
#    define REZERO_COMPILER_MSVC 1
#    define REZERO_BREAK() __debugbreak()
#    define REZERO_INLINE inline __forceinline
#    define REZERO_NOINLINE __declspec(noinline)
#elif defined(__clang__)
#    define REZERO_COMPILER_CLANG 1
#    define REZERO_BREAK() __builtin_debugtrap()
#    define REZERO_INLINE inline __attribute__((always_inline))
#    define REZERO_NOINLINE __attribute__((noinline))
#elif defined(__GNUC__)
#    define REZERO_COMPILER_GCC 1
#    define REZERO_BREAK() __builtin_trap()
#    define REZERO_INLINE inline __attribute__((always_inline))
#    define REZERO_NOINLINE __attribute__((noinline))
#else
#    error "ReZeroGui: unsupported compiler"
#endif

#if !defined(REZERO_INLINE)
#    define REZERO_INLINE inline
#endif
#if !defined(REZERO_NOINLINE)
#    define REZERO_NOINLINE
#endif
#if !defined(REZERO_BREAK)
#    define REZERO_BREAK() ((void)0)
#endif

#define REZERO_NODISCARD [[nodiscard]]
#define REZERO_LIKELY(x) (!!(x))
#define REZERO_UNLIKELY(x) (!!(x))

#if !defined(REZERO_ENABLE_ASSERT)
#    if defined(NDEBUG)
#        define REZERO_ENABLE_ASSERT 0
#    else
#        define REZERO_ENABLE_ASSERT 1
#    endif
#endif

#if REZERO_ENABLE_ASSERT
#    define REZERO_ASSERT(expr) assert(expr)
#    define REZERO_ASSERT_MSG(expr, msg) assert((expr) && (msg))
#else
#    define REZERO_ASSERT(expr) ((void)0)
#    define REZERO_ASSERT_MSG(expr, msg) ((void)0)
#endif

#define REZERO_CHECK(expr)                                                                                             \
    do {                                                                                                               \
        auto &&checked_ = (expr);                                                                                      \
        if (!checked_)                                                                                                 \
            return z::Error{checked_.error()};                                                                         \
    } while (false)

#define REZERO_TRY(name, expr)                                                                                         \
    auto name##checked_ = (expr);                                                                                      \
    if (!name##checked_)                                                                                               \
        return z::Error{name##checked_.error()};                                                                       \
    auto name = *name##checked_;                                                                                       \
    (void)0

namespace ReZeroGui {

    template <typename T> constexpr const T &maximum(const T &a, const T &b) { return (a < b) ? b : a; }
    template <typename T> constexpr const T &minimum(const T &a, const T &b) { return (b < a) ? b : a; }

    constexpr auto memset(void *destination, std::uint8_t value, std::size_t count) noexcept -> void * {
        auto *bytes = static_cast<std::uint8_t *>(destination);

        for (std::size_t i = 0; i < count; ++i)
            bytes[i] = value;

        return destination;
    }

    constexpr auto memcpy(void *destination, const void *source, std::size_t count) noexcept -> void * {
        auto *dst = static_cast<std::uint8_t *>(destination);
        const auto *src = static_cast<const std::uint8_t *>(source);

        for (std::size_t i = 0; i < count; ++i)
            dst[i] = src[i];

        return destination;
    }

    [[nodiscard]] constexpr auto memcmp(const void *lhs, const void *rhs, std::size_t count) noexcept -> int {
        const auto *a = static_cast<const std::uint8_t *>(lhs);
        const auto *b = static_cast<const std::uint8_t *>(rhs);

        for (std::size_t i = 0; i < count; ++i) {
            if (a[i] < b[i])
                return -1;

            if (a[i] > b[i])
                return 1;
        }

        return 0;
    }
    constexpr float clampFloat(float value, float minimum, float maximum) noexcept {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    constexpr float lerpFloat(float a, float b, float t) noexcept { return a + (b - a) * t; }

    /// Frame rate independent approach toward `target`: `current` closes
    /// 1 - exp(-dt / tau) of the remaining distance per call, and snaps once it is
    /// closer than a tenth of a pixel so an animation can actually settle instead
    /// of redrawing sub pixel noise forever. `tau` is the time constant in
    /// seconds: about 63% of the way after one tau.
    inline float damp(float current, float target, float tau, float dt) noexcept {
        if (tau <= 0.0f || dt <= 0.0f) {
            return target;
        }
        const float next = lerpFloat(target, current, std::expf(-dt / tau));
        return std::abs(target - next) < 0.0001f ? target : next;
    }

    template <typename T> struct Span {
        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T *;
        using const_pointer = const T *;
        using reference = T &;
        using const_reference = const T &;
        using iterator = pointer;
        using const_iterator = const_pointer;

        constexpr Span() noexcept = default;
        constexpr Span(pointer ptr, size_type length) : ptr(ptr), length(length) {}

        template <size_t N> constexpr Span(T (&array)[N]) : ptr(array), length(N) {}

        template <typename U>
            requires std::convertible_to<U (*)[], element_type (*)[]>
        constexpr Span(const Span<U> &other) noexcept : ptr(other.data()), length(other.size()) {}

        constexpr Span(const Span &) noexcept = default;
        constexpr Span &operator=(const Span &) noexcept = default;

        constexpr iterator begin() const noexcept { return ptr; }
        constexpr const_iterator cbegin() const noexcept { return ptr; }
        constexpr iterator end() const noexcept { return ptr + length; }
        constexpr const_iterator cend() const noexcept { return ptr + length; }

        constexpr reference operator[](size_type idx) const noexcept {
            REZERO_ASSERT(idx < length);
            return ptr[idx];
        }

        constexpr reference front() const noexcept {
            REZERO_ASSERT(!empty());
            return ptr[0];
        }

        constexpr reference back() const noexcept {
            REZERO_ASSERT(!empty());
            return ptr[length - 1];
        }

        constexpr pointer data() const noexcept { return ptr; }

        constexpr size_type size() const noexcept { return length; }
        constexpr size_type size_bytes() const noexcept { return length * sizeof(element_type); }
        [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

        constexpr Span first(size_type size) const noexcept {
            REZERO_ASSERT(size <= length);
            return {ptr, size};
        }

        constexpr Span last(size_type size) const noexcept {
            REZERO_ASSERT(size <= length);
            return {ptr + (length - size), size};
        }

        constexpr Span subspan(size_type offset, size_type size = size_type(-1)) const noexcept {
            REZERO_ASSERT(offset <= length);
            if (size == size_type(-1)) {
                return {ptr + offset, length - offset};
            }
            REZERO_ASSERT(size <= length - offset);
            return {ptr + offset, size};
        }

      private:
        pointer ptr = nullptr;
        size_type length = 0;
    };

    template <typename T, std::size_t N> Span(T (&)[N]) -> Span<T>;

    /// T must be trivially copyable.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    struct ArrayList {
        using ValueType = T;
        using SizeType = std::size_t;

        explicit ArrayList(AllocatorView allocator) noexcept : allocator(allocator) {}

        ArrayList(AllocatorView allocator, SizeType initialCapacity) noexcept : allocator(allocator) {
            reserve(initialCapacity);
        }
        ~ArrayList() noexcept { release(); }
        ArrayList(const ArrayList &) = delete;
        ArrayList &operator=(const ArrayList &) = delete;

        ArrayList(ArrayList &&other) noexcept
            : allocator(other.allocator), items(other.items), count(other.count), capacity(other.capacity) {
            other.items = nullptr;
            other.count = 0;
            other.capacity = 0;
        }

        ArrayList &operator=(ArrayList &&other) noexcept {
            if (this == &other)
                return *this;

            release();

            allocator = other.allocator;
            items = other.items;
            count = other.count;
            capacity = other.capacity;

            other.items = nullptr;
            other.count = 0;
            other.capacity = 0;

            return *this;
        }
        [[nodiscard]] constexpr auto span() noexcept -> Span<T> { return {items, count}; }

        [[nodiscard]] constexpr auto span() const noexcept -> Span<const T> { return {items, count}; }

        [[nodiscard]] bool empty() const noexcept { return count == 0; }
        [[nodiscard]] SizeType size() const noexcept { return count; }
        [[nodiscard]] T *data() noexcept { return items; }
        [[nodiscard]] const T *data() const noexcept { return items; }
        T &operator[](SizeType index) noexcept { return items[index]; }
        const T &operator[](SizeType index) const noexcept { return items[index]; }
        T &front() noexcept { return items[0]; }
        const T &front() const noexcept { return items[0]; }
        T &back() noexcept { return items[count - 1]; }
        const T &back() const noexcept { return items[count - 1]; }

        T *begin() noexcept { return items; }

        T *end() noexcept { return items + count; }

        const T *begin() const noexcept { return items; }

        const T *end() const noexcept { return items + count; }

        bool reserve(SizeType newCapacity) noexcept {
            if (newCapacity <= capacity)
                return true;

            return grow(newCapacity);
        }

        bool push_back(const T &value) noexcept {
            if (count == capacity) {
                if (!grow(nextCapacity()))
                    return false;
            }

            items[count] = value;
            count += 1;

            return true;
        }

        bool push_back(T &&value) noexcept {
            if (count == capacity) {
                if (!grow(nextCapacity()))
                    return false;
            }

            items[count] = value;
            count += 1;

            return true;
        }

        bool resize(SizeType newSize) noexcept {
            if (newSize > capacity) {
                if (!grow(newSize))
                    return false;
            }

            if (newSize > count) {
                memset(items + count, 0, (newSize - count) * sizeof(T));
            }

            count = newSize;
            return true;
        }

        void pop_back() noexcept {
            if (count != 0)
                count -= 1;
        }

        void clear() noexcept { count = 0; }

        [[nodiscard]] SizeType nextCapacity() const noexcept {
            if (capacity == 0)
                return 8;

            constexpr SizeType Max = std::numeric_limits<SizeType>::max();

            if (capacity > Max / 2)
                return Max / sizeof(T);

            return capacity * 2;
        }

        [[nodiscard]] bool grow(SizeType newCapacity) noexcept {
            if (newCapacity <= capacity)
                return true;

            constexpr SizeType Max = std::numeric_limits<SizeType>::max();

            if (newCapacity > Max / sizeof(T))
                return false;

            const SizeType newSize = newCapacity * sizeof(T);

            void *newMemory = allocator.allocate(newSize, alignof(T));

            if (!newMemory)
                return false;

            T *newItems = static_cast<T *>(newMemory);

            if (items && count != 0) {
                memcpy(newItems, items, count * sizeof(T));
            }

            if (items) {
                allocator.deallocate(items);
            }

            items = newItems;
            capacity = newCapacity;

            return true;
        }

        void release() noexcept {
            if (!items)
                return;

            allocator.deallocate(items);

            items = nullptr;
            count = 0;
            capacity = 0;
        }

        AllocatorView allocator;
        T *items = nullptr;
        SizeType count = 0;
        SizeType capacity = 0;
    };

    class FrameArena {
        struct Block {
            std::byte *base = nullptr;
            std::size_t capacity = 0;
            std::size_t offset = 0;
            Block *next = nullptr;
        };

      public:
        static constexpr std::size_t DefaultBlockSize = 64 * 1024;
        static constexpr std::size_t BlockAlignment = 64;

        explicit FrameArena(AllocatorView allocator, std::size_t blockSize = DefaultBlockSize) noexcept
            : Allocator(allocator), BlockSize(blockSize != 0 ? blockSize : 1) {}

        ~FrameArena() noexcept { release(); }

        FrameArena(const FrameArena &) = delete;
        FrameArena &operator=(const FrameArena &) = delete;
        FrameArena(FrameArena &&) = delete;
        FrameArena &operator=(FrameArena &&) = delete;

        [[nodiscard]] void *allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept {
            if (size == 0 || !isValidAlignment(alignment)) {
                return nullptr;
            }

            while (Cur != nullptr) {
                if (void *memory = tryAllocate(*Cur, size, alignment)) {
                    return memory;
                }

                Cur = Cur->next;
            }

            Block *block = addBlock(size, alignment);
            if (block == nullptr) {
                return nullptr;
            }

            Cur = block;
            return tryAllocate(*Cur, size, alignment);
        }

        template <typename T, typename... Args>
            requires std::is_trivially_destructible_v<T> && std::is_nothrow_constructible_v<T, Args...>
        [[nodiscard]] T *create(Args &&...args) noexcept {
            void *memory = allocate(sizeof(T), alignof(T));
            if (memory == nullptr) {
                return nullptr;
            }

            return std::construct_at(static_cast<T *>(memory), std::forward<Args>(args)...);
        }

        // 结束当前帧的分配周期，保留全部内存块供下一帧复用。
        void reset() noexcept {
            for (Block *block = Head; block != nullptr; block = block->next) {
                block->offset = 0;
            }

            Cur = Head;
        }

        // 释放全部内存块。
        void release() noexcept {
            Block *block = Head;

            while (block != nullptr) {
                Block *next = block->next;

                Allocator.deallocate(block->base);
                Allocator.deallocate(block);

                block = next;
            }

            Head = nullptr;
            Tail = nullptr;
            Cur = nullptr;
        }

        [[nodiscard]] std::size_t used() const noexcept {
            std::size_t total = 0;

            for (const Block *block = Head; block != nullptr; block = block->next) {
                total += block->offset;
            }

            return total;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            std::size_t total = 0;

            for (const Block *block = Head; block != nullptr; block = block->next) {
                total += block->capacity;
            }

            return total;
        }

      private:
        static constexpr bool isValidAlignment(std::size_t alignment) noexcept {
            return alignment != 0 && (alignment & (alignment - 1)) == 0;
        }

        [[nodiscard]] static std::uintptr_t alignAddress(std::uintptr_t address, std::size_t alignment) noexcept {
            std::uintptr_t result;
            const auto mask = static_cast<std::uintptr_t>(alignment - 1);
            result = (address + mask) & ~mask;
            return result;
        }

        [[nodiscard]] void *tryAllocate(Block &block, std::size_t size, std::size_t alignment) noexcept {
            const auto base = reinterpret_cast<std::uintptr_t>(block.base);

            if (block.offset > block.capacity) {
                return nullptr;
            }

            std::uintptr_t alignedAddress = alignAddress(base + block.offset, alignment);

            if (alignedAddress < base) {
                return nullptr;
            }

            const std::size_t alignedOffset = static_cast<std::size_t>(alignedAddress - base);

            if (alignedOffset > block.capacity || size > block.capacity - alignedOffset) {
                return nullptr;
            }

            block.offset = alignedOffset + size;
            return reinterpret_cast<void *>(alignedAddress);
        }

        [[nodiscard]] Block *addBlock(std::size_t size, std::size_t alignment) noexcept {
            const std::size_t required = size + alignment - 1;
            const std::size_t capacity = maximum(BlockSize, required);

            const std::size_t blockAlignment = maximum(BlockAlignment, alignment);

            auto *base = static_cast<std::byte *>(Allocator.allocate(capacity, blockAlignment));

            if (base == nullptr) {
                return nullptr;
            }

            void *memory = Allocator.allocate(sizeof(Block), alignof(Block));

            if (memory == nullptr) {
                Allocator.deallocate(base);
                return nullptr;
            }

            auto *block = std::construct_at(static_cast<Block *>(memory));
            block->base = base;
            block->capacity = capacity;

            if (Tail != nullptr) {
                Tail->next = block;
            } else {
                Head = block;
            }

            Tail = block;
            return block;
        }

        AllocatorView Allocator;
        std::size_t BlockSize;

        Block *Head = nullptr;
        Block *Tail = nullptr;
        Block *Cur = nullptr;
    };
    template <class Char> struct BasicStringView {
      public:
        static_assert(std::is_trivial_v<Char>);

        using value_type = Char;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;

        using const_pointer = const Char *;
        using const_reference = const Char &;
        using const_iterator = const Char *;

        static constexpr size_type npos = static_cast<size_type>(-1);

      public:
        constexpr BasicStringView() noexcept = default;

        constexpr BasicStringView(const Char *chars, size_type length) noexcept : first(chars), length(length) {
            REZERO_ASSERT(chars != nullptr || length == 0);
        }

        constexpr BasicStringView(const Char *chars) noexcept : first(chars), length(chars ? cstrLength(chars) : 0) {}

        template <size_type N> constexpr BasicStringView(const Char (&str)[N]) noexcept : first(str), length(N - 1) {}

      public:
        [[nodiscard]] constexpr auto data() const noexcept -> const Char * { return first; }

        [[nodiscard]] constexpr auto size() const noexcept -> size_type { return length; }

        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return length == 0; }

        [[nodiscard]] constexpr auto front() const noexcept -> const Char & {
            REZERO_ASSERT(length > 0);
            return first[0];
        }

        [[nodiscard]] constexpr auto back() const noexcept -> const Char & {
            REZERO_ASSERT(length > 0);
            return first[length - 1];
        }

        [[nodiscard]] constexpr auto operator[](size_type index) const noexcept -> const Char & {
            REZERO_ASSERT(index < length);
            return first[index];
        }

        [[nodiscard]] constexpr auto at(size_type index) const noexcept -> const Char & {
            REZERO_ASSERT(index < length);
            return first[index];
        }

      public:
        [[nodiscard]] constexpr auto begin() const noexcept -> const_iterator { return first; }

        [[nodiscard]] constexpr auto end() const noexcept -> const_iterator { return first + length; }

      public:
        constexpr auto removePrefix(size_type count) noexcept {
            REZERO_ASSERT(count <= length);

            first += count;
            length -= count;
        }

        constexpr auto removeSuffix(size_type count) noexcept {
            REZERO_ASSERT(count <= length);

            length -= count;
        }

        [[nodiscard]] constexpr auto substr(size_type position, size_type count = npos) const noexcept
            -> BasicStringView {
            REZERO_ASSERT(position <= length);

            const size_type remaining = length - position;

            if (count > remaining) {
                count = remaining;
            }

            return BasicStringView(first + position, count);
        }

      public:
        [[nodiscard]] constexpr auto startsWith(BasicStringView value) const noexcept -> bool {
            if (value.length > length) {
                return false;
            }

            return equals(first, value.first, value.length);
        }

        [[nodiscard]] constexpr auto endsWith(BasicStringView value) const noexcept -> bool {
            if (value.length > length) {
                return false;
            }

            return equals(first + length - value.length, value.first, value.length);
        }

        [[nodiscard]] constexpr auto operator==(BasicStringView other) const noexcept -> bool {
            return length == other.length && equals(first, other.first, length);
        }

        [[nodiscard]] constexpr auto operator!=(BasicStringView other) const noexcept -> bool {
            return !(*this == other);
        }

      public:
        [[nodiscard]] constexpr auto find(Char value, size_type position = 0) const noexcept -> size_type {
            if (position >= length) {
                return npos;
            }

            for (size_type i = position; i < length; ++i) {
                if (first[i] == value) {
                    return i;
                }
            }

            return npos;
        }

        [[nodiscard]] constexpr auto find(BasicStringView value, size_type position = 0) const noexcept -> size_type {
            if (position > length) {
                return npos;
            }

            if (value.empty()) {
                return position;
            }

            if (value.length > length - position) {
                return npos;
            }

            const size_type last = length - value.length;

            for (size_type i = position; i <= last; ++i) {
                if (equals(first + i, value.first, value.length)) {
                    return i;
                }
            }

            return npos;
        }

        [[nodiscard]] constexpr auto findFirstOf(BasicStringView values, size_type position = 0) const noexcept
            -> size_type {
            if (position >= length) {
                return npos;
            }

            for (size_type i = position; i < length; ++i) {
                for (size_type j = 0; j < values.length; ++j) {
                    if (first[i] == values[j]) {
                        return i;
                    }
                }
            }

            return npos;
        }

      private:
        [[nodiscard]] static constexpr auto cstrLength(const Char *str) noexcept -> size_type {
            size_type length = 0;

            while (str[length] != Char{}) {
                ++length;
            }

            return length;
        }

        [[nodiscard]] static constexpr auto equals(const Char *lhs, const Char *rhs, size_type size) noexcept -> bool {
            for (size_type i = 0; i < size; ++i) {
                if (lhs[i] != rhs[i]) {
                    return false;
                }
            }

            return true;
        }

      private:
        const Char *first = nullptr;
        size_type length = 0;
    };
    using StringView = BasicStringView<char>;
    using u8StringView = BasicStringView<char8_t>;

    template <typename T, std::size_t N> class Array {
        static_assert(N > 0, "Array size must be greater than zero.");

      public:
        using value_type = T;
        using size_type = std::size_t;
        using reference = T &;
        using const_reference = const T &;
        using iterator = T *;
        using const_iterator = const T *;

        constexpr Array() = default;

        template <typename... U>
            requires(sizeof...(U) == N)
        constexpr Array(U &&...args) : items{std::forward<U>(args)...} {}

        constexpr auto &&at(this auto &&self, size_type index) noexcept {
            REZERO_ASSERT(index < N);
            return std::forward<decltype(self)>(self).items[index];
        }
        constexpr auto *data(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items; }
        constexpr auto size(this auto &&) noexcept -> size_type { return N; }

        constexpr auto &&front(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items[0]; }
        constexpr auto &&back(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items[N - 1]; }

        constexpr auto *begin(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items; }
        constexpr auto *end(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items + N; }

        constexpr auto &&operator[](this auto &&self, size_type index) noexcept {
            REZERO_ASSERT(index < N);
            return std::forward<decltype(self)>(self).items[index];
        }

        constexpr void fill(this auto &&self, const T &value)
            requires std::assignable_from<T &, const T &>
        {
            for (auto &item : self.items) {
                item = value;
            }
        }

      private:
        // The array member manages the object's lifetime automatically.
        T items[N];
    };

    template <typename T, std::size_t Capacity> class InplaceVector {
      public:
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = T &;
        using const_reference = const T &;
        using pointer = T *;
        using const_pointer = const T *;
        using iterator = T *;
        using const_iterator = const T *;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        static constexpr size_type capacity = Capacity;

        constexpr InplaceVector() noexcept = default;

        constexpr InplaceVector(const InplaceVector &other)
            requires std::copy_constructible<T>
        {
            for (const auto &value : other) {
                emplace_back(value);
            }
        }

        constexpr InplaceVector(InplaceVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            for (auto &value : other) {
                emplace_back(std::move(value));
            }

            other.clear();
        }

        constexpr InplaceVector &operator=(const InplaceVector &other)
            requires std::copy_constructible<T> && std::is_copy_assignable_v<T>
        {
            if (this == &other) {
                return *this;
            }

            clear();

            for (const auto &value : other) {
                emplace_back(value);
            }

            return *this;
        }

        constexpr InplaceVector &operator=(InplaceVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            if (this == &other) {
                return *this;
            }

            clear();

            for (auto &value : other) {
                emplace_back(std::move(value));
            }

            other.clear();

            return *this;
        }

        constexpr ~InplaceVector() { clear(); }

        [[nodiscard]] constexpr bool empty() const noexcept { return Size == 0; }

        [[nodiscard]] constexpr bool full() const noexcept { return Size == Capacity; }

        [[nodiscard]] constexpr size_type size() const noexcept { return Size; }

        [[nodiscard]] constexpr size_type max_size() const noexcept { return Capacity; }

        [[nodiscard]] constexpr size_type capacity_size() const noexcept { return Capacity; }

        constexpr reference operator[](size_type index) noexcept {
            REZERO_ASSERT(index < Size);
            return data()[index];
        }

        constexpr const_reference operator[](size_type index) const noexcept {
            REZERO_ASSERT(index < Size);
            return data()[index];
        }

        constexpr reference at(size_type index) {
            REZERO_ASSERT(index < Size);
            return data()[index];
        }

        constexpr const_reference at(size_type index) const {
            REZERO_ASSERT(index < Size);
            return data()[index];
        }

        constexpr reference front() noexcept {
            REZERO_ASSERT(!empty());
            return data()[0];
        }

        constexpr const_reference front() const noexcept {
            REZERO_ASSERT(!empty());
            return data()[0];
        }

        constexpr reference back() noexcept {
            REZERO_ASSERT(!empty());
            return data()[Size - 1];
        }

        constexpr const_reference back() const noexcept {
            REZERO_ASSERT(!empty());
            return data()[Size - 1];
        }

        [[nodiscard]] constexpr auto asSpan() noexcept -> Span<T> { return {data(), size()}; }

        [[nodiscard]] constexpr auto asSpan() const noexcept -> Span<const T> { return {data(), size()}; }

        constexpr pointer data() noexcept { return reinterpret_cast<pointer>(Items); }

        constexpr const_pointer data() const noexcept { return reinterpret_cast<const_pointer>(Items); }

        constexpr void push_back(const T &value)
            requires std::copy_constructible<T>
        {
            assert(!full());

            std::construct_at(data() + Size, value);

            ++Size;
        }

        constexpr void push_back(T &&value)
            requires std::move_constructible<T>
        {
            REZERO_ASSERT(!full());

            std::construct_at(data() + Size, std::move(value));

            ++Size;
        }

        template <typename... Args>
        constexpr reference emplace_back(Args &&...args)
            requires std::constructible_from<T, Args...>
        {
            REZERO_ASSERT(!full());

            std::construct_at(data() + Size, std::forward<Args>(args)...);

            ++Size;

            return back();
        }

        constexpr void pop_back() noexcept {
            REZERO_ASSERT(!empty());

            --Size;

            std::destroy_at(data() + Size);
        }

        constexpr void clear() noexcept {
            while (!empty()) {
                pop_back();
            }
        }

        constexpr iterator begin() noexcept { return data(); }

        constexpr const_iterator begin() const noexcept { return data(); }

        constexpr const_iterator cbegin() const noexcept { return data(); }

        constexpr iterator end() noexcept { return data() + Size; }

        constexpr const_iterator end() const noexcept { return data() + Size; }

        constexpr const_iterator cend() const noexcept { return data() + Size; }

        constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }

        constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }

        constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }

        constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

        constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

        constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

      private:
        // Capacity 为 0 时也要留一个字节：零长数组本身不合法（C2229），而"容量为 0 的 vector"
        // 是个合法用途（push_back 会断言拒绝）。这里不用 std::conditional 是为了让 alignas 也能跟着走。
        static constexpr size_type StorageSize = sizeof(T) * (Capacity == 0 ? 1 : Capacity);
        alignas(T) std::byte Items[StorageSize];

        size_type Size = 0;
    };

    template <typename F> class Function;
    template <typename R, typename... Args> class Function<R(Args...)> {
        static constexpr size_t SBOSize = 32;
        static constexpr size_t SBOAlign = alignof(std::max_align_t);

        struct VTable {
            R (*invoke)(void *self, Args &&...args);
            void (*destroy)(void *self) noexcept;
            void (*moveConstruct)(void *dest, void *src) noexcept;
        };

        template <typename F> static const VTable &vtableFor() noexcept {
            static VTable vt = {.invoke = [](void *self, Args &&...args) -> R {
                                    return std::invoke(*static_cast<F *>(self), std::forward<Args>(args)...);
                                },
                                .destroy = [](void *self) noexcept { std::destroy_at(static_cast<F *>(self)); },
                                .moveConstruct =
                                    [](void *dest, void *src) noexcept {
                                        std::construct_at(static_cast<F *>(dest), std::move(*static_cast<F *>(src)));
                                    }};
            return vt;
        };

      public:
        Function() = default;

        template <typename F>
            requires(!std::same_as<std::remove_cvref_t<F>, Function>) && std::invocable<F, Args...> &&
                    std::convertible_to<std::invoke_result_t<F, Args...>, R>
        Function(F &&fn) {
            using Decayed = std::decay_t<F>;
            // 只有放进内联缓冲这一条路：Function 不带分配器，所以拿不下就给一个说得清的编译错误，
            // 而不是留一条"未实现"的堆路径在这里（原来的 else 分支既有 static_assert(false)，
            // 又调用了根本不存在的 vtable_for，报错信息把人往错的方向引）。
            static_assert(sizeof(Decayed) <= SBOSize && alignof(Decayed) <= SBOAlign &&
                              std::is_nothrow_move_constructible_v<Decayed>,
                          "Function: the callable does not fit the 32 byte inline buffer. Capture a pointer (or a "
                          "small value) instead of a large object, or take the data through the arguments.");
            std::construct_at(reinterpret_cast<Decayed *>(SmallBuffer), std::forward<F>(fn));
            Vt = &vtableFor<Decayed>();
            IsSBO = true;
        }

        Function(Function &&other) noexcept { moveFrom(std::move(other)); }
        Function &operator=(Function &&other) noexcept {
            if (this != &other) {
                reset();
                moveFrom(std::move(other));
            }
            return *this;
        }

        Function(const Function &other) = delete;
        Function &operator=(const Function &other) = delete;
        explicit operator bool() const noexcept { return Vt != nullptr; }
        ~Function() { reset(); }

        R operator()(Args &&...args) noexcept {
            REZERO_ASSERT(Vt != nullptr);
            return Vt->invoke(ptr(), std::forward<Args>(args)...);
        }

        void reset() noexcept {
            if (!Vt) {
                return;
            }
            if (IsSBO) {
                Vt->destroy(SmallBuffer);
            } else {
            }
            Vt = nullptr;
        }

      private:
        void moveFrom(Function &&other) noexcept {
            if (!other.Vt) {
                Vt = nullptr;
                return;
            }

            Vt = other.Vt;
            IsSBO = other.IsSBO;

            if (IsSBO) {
                Vt->moveConstruct(SmallBuffer, other.SmallBuffer);
                other.Vt->destroy(other.SmallBuffer);
            } else {
                *reinterpret_cast<void **>(SmallBuffer) = *reinterpret_cast<void **>(other.SmallBuffer);
            }
            other.Vt = nullptr;
        }

        void *ptr() const noexcept {
            if (IsSBO) {
                return const_cast<char *>(SmallBuffer);
            }
            return *reinterpret_cast<void *const *>(SmallBuffer);
        }

        alignas(SBOAlign) char SmallBuffer[SBOSize];
        const VTable *Vt = nullptr;
        bool IsSBO = false;
    };

    struct Vector2 {
        float x = 0.0f;
        float y = 0.0f;

        constexpr Vector2() noexcept = default;
        constexpr Vector2(float xValue, float yValue) noexcept : x(xValue), y(yValue) {}
        constexpr explicit Vector2(float scalar) noexcept : x(scalar), y(scalar) {}

        constexpr Vector2 operator+(Vector2 other) const noexcept { return Vector2(x + other.x, y + other.y); }
        constexpr Vector2 operator-(Vector2 other) const noexcept { return Vector2(x - other.x, y - other.y); }
        constexpr Vector2 operator*(Vector2 other) const noexcept { return Vector2(x * other.x, y * other.y); }
        constexpr Vector2 operator/(Vector2 other) const noexcept { return Vector2(x / other.x, y / other.y); }
        constexpr Vector2 operator*(float scalar) const noexcept { return Vector2(x * scalar, y * scalar); }
        constexpr Vector2 operator/(float scalar) const noexcept { return Vector2(x / scalar, y / scalar); }
        constexpr Vector2 operator-() const noexcept { return Vector2(-x, -y); }

        constexpr Vector2 &operator+=(Vector2 other) noexcept {
            x += other.x;
            y += other.y;
            return *this;
        }
        constexpr Vector2 &operator-=(Vector2 other) noexcept {
            x -= other.x;
            y -= other.y;
            return *this;
        }
        constexpr Vector2 &operator*=(float scalar) noexcept {
            x *= scalar;
            y *= scalar;
            return *this;
        }
        constexpr Vector2 &operator/=(float scalar) noexcept {
            x /= scalar;
            y /= scalar;
            return *this;
        }

        constexpr bool operator==(const Vector2 &) const noexcept = default;
    };
    struct Rect {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        constexpr Rect() noexcept = default;
        constexpr Rect(float xValue, float yValue, float widthValue, float heightValue) noexcept
            : x(xValue), y(yValue), width(widthValue), height(heightValue) {}
        constexpr Rect(Vector2 position, Vector2 size) noexcept
            : x(position.x), y(position.y), width(size.x), height(size.y) {}

        static constexpr Rect fromPoints(Vector2 minimum, Vector2 maximum) noexcept {
            return Rect(minimum.x, minimum.y, maximum.x - minimum.x, maximum.y - minimum.y);
        }

        constexpr Vector2 position() const noexcept { return Vector2(x, y); }
        constexpr Vector2 size() const noexcept { return Vector2(width, height); }
        constexpr Vector2 minPoint() const noexcept { return Vector2(x, y); }
        constexpr Vector2 maxPoint() const noexcept { return Vector2(x + width, y + height); }
        constexpr Vector2 center() const noexcept { return Vector2(x + width * 0.5f, y + height * 0.5f); }

        constexpr float minX() const noexcept { return x; }
        constexpr float minY() const noexcept { return y; }
        constexpr float maxX() const noexcept { return x + width; }
        constexpr float maxY() const noexcept { return y + height; }

        constexpr bool isEmpty() const noexcept { return width <= 0.0f || height <= 0.0f; }

        constexpr bool contains(Vector2 point) const noexcept {
            return point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
        }

        constexpr bool contains(const Rect &other) const noexcept {
            return other.x >= x && other.y >= y && other.maxX() <= maxX() && other.maxY() <= maxY();
        }

        constexpr bool overlaps(const Rect &other) const noexcept {
            return other.x < maxX() && other.y < maxY() && other.maxX() > x && other.maxY() > y;
        }

        constexpr Rect withPosition(Vector2 position) const noexcept {
            return Rect(position.x, position.y, width, height);
        }

        constexpr Rect withSize(Vector2 size) const noexcept { return Rect(x, y, size.x, size.y); }

        constexpr Rect withOffset(Vector2 offset) const noexcept {
            return Rect(x + offset.x, y + offset.y, width, height);
        }

        constexpr Rect expanded(float amount) const noexcept {
            return Rect(x - amount, y - amount, width + amount * 2.0f, height + amount * 2.0f);
        }

        /// Intersection of this rectangle with \p other. May be empty.
        constexpr Rect intersected(const Rect &other) const noexcept {
            const float minimumX = x > other.x ? x : other.x;
            const float minimumY = y > other.y ? y : other.y;
            const float maximumX = maxX() < other.maxX() ? maxX() : other.maxX();
            const float maximumY = maxY() < other.maxY() ? maxY() : other.maxY();
            return fromPoints(Vector2(minimumX, minimumY), Vector2(maximumX > minimumX ? maximumX : minimumX,
                                                                   maximumY > minimumY ? maximumY : minimumY));
        }
    };

    using Pixel32 = std::uint32_t;

    struct Pixel {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;

        constexpr Pixel() noexcept = default;
        constexpr Pixel(float red, float green, float blue, float alpha = 1.0f) noexcept
            : r(red), g(green), b(blue), a(alpha) {}
        // 这里原来还有一个 constexpr Pixel(const Pixel &, float alpha) 的声明，全库没有定义：
        // 用了就是 LNK2019。要改透明度用 withAlpha()。

        static constexpr Pixel fromBytes(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                                         std::uint8_t alpha = 255) noexcept {
            return Pixel(static_cast<float>(red) / 255.0f, static_cast<float>(green) / 255.0f,
                         static_cast<float>(blue) / 255.0f, static_cast<float>(alpha) / 255.0f);
        }

        /// Builds a color from a 0xRRGGBBAA literal, i.e. the most significant byte
        /// is the red channel and the least significant byte is alpha. Note that
        /// toColor32() uses the opposite (little endian memory) order on purpose: it
        /// has to produce R, G, B, A bytes for the GPU.
        static constexpr Pixel fromRgba(std::uint32_t value, float alphaOverride = -1.0f) noexcept {
            return Pixel(static_cast<float>((value >> 24) & 0xFF) / 255.0f,
                         static_cast<float>((value >> 16) & 0xFF) / 255.0f,
                         static_cast<float>((value >> 8) & 0xFF) / 255.0f,
                         alphaOverride >= 0.0f ? alphaOverride : static_cast<float>(value & 0xFF) / 255.0f);
        }

        constexpr Pixel withAlpha(float alpha) const noexcept { return Pixel(r, g, b, alpha); }

        constexpr Pixel operator*(float scalar) const noexcept { return Pixel(r * scalar, g * scalar, b * scalar, a); }

        constexpr Pixel operator+(const Pixel &other) const noexcept {
            return Pixel(r + other.r, g + other.g, b + other.b, a + other.a);
        }

        constexpr bool operator==(const Pixel &) const noexcept = default;

        constexpr Pixel32 toPixel32() const noexcept {
            const auto channel = [](float value) -> std::uint32_t {
                const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
                return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
            };
            return (channel(r) << 0) | (channel(g) << 8) | (channel(b) << 16) | (channel(a) << 24);
        }

        constexpr auto lerp(const Pixel &other, float t) const {
            return Pixel(lerpFloat(r, other.r, t), lerpFloat(g, other.g, t), lerpFloat(b, other.b, t),
                         lerpFloat(a, other.a, t));
        }
    };

    constexpr std::uint32_t hashCombine(std::uint32_t lhs, std::uint32_t rhs) noexcept {
        constexpr std::uint32_t Prime = 16777619u;
        constexpr std::uint32_t Reserved = 0xFFFFFFFFu; // Id.h 的 InvalidId；见 hashBytes()
        std::uint32_t result = lhs == 0 ? 2166136261u : lhs;
        for (int byteIndex = 0; byteIndex < 4; byteIndex += 1) {
            result ^= (rhs >> (byteIndex * 8)) & 0xFFu;
            result *= Prime;
        }
        return result == 0 || result == Reserved ? 1u : result;
    }
    // FNV-1a
    //
    // 结果既避开 0 也避开 0xFFFFFFFF：前者是被 0 当哨兵的地方（`result == 0 ? 1u` 的老规矩），
    // 后者是 Id.h 的 InvalidId——全库用它表示"没有"（没有焦点、没有弹窗、没有鼠标属主）。
    // 一个控件标签要是哈希到 InvalidId，它就会永远被当成"没有"，既不能聚焦也拖不动，
    // 而这点代价（一次比较）换掉一整类 2^-32 的怪事很划算。
    constexpr std::uint32_t hashBytes(const char8_t *data, std::size_t size,
                                      std::uint32_t seed = 2166136261u) noexcept {
        constexpr std::uint32_t Prime = 16777619u;
        constexpr std::uint32_t Reserved = 0xFFFFFFFFu; // Id.h 的 InvalidId
        std::uint32_t result = seed;
        for (std::size_t index = 0; index < size; ++index) {
            result ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[index]));
            result *= Prime;
        }
        return result == 0 || result == Reserved ? 1u : result;
    }

    constexpr std::uint32_t hashString(u8StringView text, std::uint32_t seed = 2166136261u) noexcept {
        return hashBytes(text.data(), text.size(), seed);
    }

    struct NoneType {
        explicit constexpr NoneType() noexcept = default;
    };

    inline constexpr NoneType None{};

    template <typename T> struct Option {
        union Storage {
            T value;

            constexpr Storage() noexcept {}
            ~Storage() noexcept {};
        };

        using ValueType = T;

        constexpr Option() noexcept : storage{}, hasValue(false) {}

        constexpr Option(NoneType) noexcept : storage{}, hasValue(false) {}

        template <typename... Args>
        constexpr explicit Option(std::in_place_t, Args &&...args) : storage{}, hasValue(true) {
            std::construct_at(std::addressof(storage.value), std::forward<Args>(args)...);
        }

        constexpr Option(const T &val) : storage{}, hasValue(true) {
            std::construct_at(std::addressof(storage.value), val);
        }

        constexpr Option(T &&val) noexcept(std::is_nothrow_move_constructible_v<T>) : storage{}, hasValue(true) {
            std::construct_at(std::addressof(storage.value), std::move(val));
        }
        constexpr ~Option() { reset(); }

        constexpr Option(const Option &other) : storage{}, hasValue(other.hasValue) {
            if (hasValue) {
                std::construct_at(std::addressof(storage.value), other.storage.value);
            }
        }

        constexpr Option(Option &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
            : storage{}, hasValue(other.hasValue) {
            if (hasValue) {
                std::construct_at(std::addressof(storage.value), std::move(other.storage.value));
                other.reset();
            }
        }
        Option &operator=(NoneType) noexcept {
            reset();
            return *this;
        }

        constexpr Option &operator=(const Option &other) {
            if (this != std::addressof(other)) {
                if (other.hasValue) {
                    if (hasValue) {
                        storage.value = other.storage.value; // 已持有值时直接赋值
                    } else {
                        std::construct_at(std::addressof(storage.value), other.storage.value);
                        hasValue = true;
                    }
                } else {
                    reset();
                }
            }
            return *this;
        }

        constexpr Option &operator=(Option &&other) noexcept(std::is_nothrow_move_assignable_v<T> &&
                                                             std::is_nothrow_move_constructible_v<T>) {
            if (this != std::addressof(other)) {
                if (other.hasValue) {
                    if (hasValue) {
                        storage.value = std::move(other.storage.value);
                    } else {
                        std::construct_at(std::addressof(storage.value), std::move(other.storage.value));
                        hasValue = true;
                    }
                    other.reset();
                } else {
                    reset();
                }
            }
            return *this;
        }

        constexpr void reset() noexcept {
            if (hasValue) {
                std::destroy_at(std::addressof(storage.value)); // 替换显式析构调用 storage.value.~T()
                hasValue = false;
            }
        }

        [[nodiscard]] constexpr T &&value() && noexcept {
            REZERO_ASSERT(hasValue);
            return std::move(storage.value);
        }

        [[nodiscard]] constexpr T &value() & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }

        [[nodiscard]] constexpr const T &value() const & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }

        [[nodiscard]] constexpr bool has_value() const noexcept { return hasValue; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue; }

        [[nodiscard]] constexpr T &operator*() & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }
        [[nodiscard]] constexpr const T &operator*() const & noexcept {
            REZERO_ASSERT(hasValue);
            return storage.value;
        }
        [[nodiscard]] constexpr T &&operator*() && noexcept {
            REZERO_ASSERT(hasValue);
            return std::move(storage.value);
        }
        [[nodiscard]] constexpr T *operator->() noexcept {
            REZERO_ASSERT(hasValue);
            return std::addressof(storage.value);
        }
        [[nodiscard]] constexpr const T *operator->() const noexcept {
            REZERO_ASSERT(hasValue);
            return std::addressof(storage.value);
        }

        Storage storage;
        bool hasValue{false};
    };

    namespace unicode {

        /// 从 `offset` 处解出一个 UTF-8 码点，并把 `offset` 推到下一个字符。
        ///
        /// UTF-8 的编码方式（第一个字节的高位决定长度）：
        ///
        ///     0xxxxxxx                              1 字节，码点 7 位
        ///     110xxxxx 10xxxxxx                     2 字节，码点 11 位
        ///     1110xxxx 10xxxxxx 10xxxxxx            3 字节，码点 16 位
        ///     11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   4 字节，码点 21 位
        ///
        /// 解不出（越界或续字节格式不对）就返回空的 Option，offset 不动。
        ///
        /// @brief Decode one UTF-8 code point and update the offset
        ///
        /// @param text UTF-8 string
        /// @param offset current offset
        /// @return z::Option<uint32_t>
        inline auto decodeUtf8(u8StringView text, std::size_t &offset) -> Option<uint32_t> {
            if (offset >= text.size())
                return None;

            uint8_t b0 = static_cast<uint8_t>(text[offset]);
            if (b0 < 0x80) {
                offset += 1;
                return static_cast<uint32_t>(b0);
            }

            std::size_t length{0};
            uint32_t codepoint{0};
            if ((b0 & 0xE0) == 0xC0) {
                length = 2;
                codepoint = b0 & 0x1Fu;
            } else if ((b0 & 0xF0) == 0xE0) {
                length = 3;
                codepoint = b0 & 0x0Fu;
            } else if ((b0 & 0xF8) == 0xF0) {
                length = 4;
                codepoint = b0 & 0x07u;
            } else {
                // invalid lead byte
                return None;
            }

            if (offset + length > text.size())
                return None;

            for (std::size_t i = 1; i < length; ++i) {
                uint8_t b = static_cast<uint8_t>(text[offset + i]);
                if ((b & 0xC0) != 0x80)
                    // invalid continuation byte
                    return None;
                codepoint = (codepoint << 6) | (b & 0x3Fu);
            }

            // Reject invalid code points(overlong encoding)
            // - 0x80: minimum value for a 2-byte UTF-8 sequence.
            // - 0x800: minimum value for a 3-byte UTF-8 sequence.
            // - 0x10000: minimum value for a 4-byte UTF-8 sequence.
            // - 0x10FFFF: maximum valid Unicode code point.
            // - 0xD800-0xDFFF: UTF-16 surrogate range.
            bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                            (length == 4 && codepoint < 0x10000);
            if (overlong || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                // invalid code point
                return None;

            offset += length;
            return codepoint;
        }

        inline auto encodeUtf8(std::uint32_t codepoint, char *out) noexcept -> std::size_t {
            auto value = codepoint;
            if (value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
                value = 0xFFFDu;
            }

            if (value < 0x80u) {
                out[0] = static_cast<char>(value);
                return 1;
            }
            if (value < 0x800u) {
                out[0] = static_cast<char>(0xC0u | (value >> 6));
                out[1] = static_cast<char>(0x80u | (value & 0x3Fu));
                return 2;
            }
            if (value < 0x10000u) {
                out[0] = static_cast<char>(0xE0u | (value >> 12));
                out[1] = static_cast<char>(0x80u | ((value >> 6) & 0x3Fu));
                out[2] = static_cast<char>(0x80u | (value & 0x3Fu));
                return 3;
            }
            out[0] = static_cast<char>(0xF0u | (value >> 18));
            out[1] = static_cast<char>(0x80u | ((value >> 12) & 0x3Fu));
            out[2] = static_cast<char>(0x80u | ((value >> 6) & 0x3Fu));
            out[3] = static_cast<char>(0x80u | (value & 0x3Fu));
            return 4;
        }

        // UTF-8 code points can use multiple bytes:
        // [1110xxxx] [10xxxxxx] [10xxxxxx]
        //     ↑           ↑           ↑
        //  first byte  continuation  continuation

        /// Offset of the code point that ends at `offset`, i.e. one caret step to
        /// the left. Continuation bytes are skipped, so a caret never lands in the
        /// middle of a code point.
        inline auto previousCodepoint(u8StringView text, std::size_t offset) noexcept -> std::size_t {
            if (offset == 0) {
                return 0;
            }
            std::size_t index = offset > text.size() ? text.size() : offset;
            index -= 1;
            while (index > 0 && (static_cast<std::uint8_t>(text[index]) & 0xC0u) == 0x80u) {
                index -= 1;
            }
            return index;
        }
        // UTF-8 code points can use multiple bytes:
        // [1110xxxx] [10xxxxxx] [10xxxxxx]
        //     ↑           ↑           ↑
        //  first byte  continuation  continuation
        /// Offset of the code point starting at `offset`, i.e. one caret step to
        /// the right.
        inline auto nextCodepoint(u8StringView text, std::size_t offset) noexcept -> std::size_t {
            if (offset >= text.size()) {
                return text.size();
            }
            std::size_t index = offset + 1;
            while (index < text.size() && (static_cast<std::uint8_t>(text[index]) & 0xC0u) == 0x80u) {
                index += 1;
            }
            return index;
        }
    } // namespace unicode

    /// Length of the NUL terminated string inside a fixed size buffer. The
    /// buffer's own size is the hard limit, so an unterminated buffer cannot
    /// run away.
    inline auto textLength(Span<char8_t> buffer) noexcept -> std::size_t {
        std::size_t length = 0;
        while (length < buffer.size() && buffer[length] != '\0') {
            length += 1;
        }
        return length;
    }

    /// Makes room for `count` bytes at `offset` and copies `text` in. The tail
    /// is dropped when the buffer runs out, and the terminating NUL is kept.
    /// Returns true when anything was written.
    inline auto insertText(Span<char8_t> buffer, std::size_t offset, const char *text, std::size_t count) noexcept
        -> bool {
        if (buffer.size() < 2 || count == 0) {
            return false;
        }
        const std::size_t capacity = buffer.size() - 1;
        const std::size_t length = textLength(buffer);
        if (offset > length) {
            offset = length;
        }
        if (length + count > capacity) {
            count = capacity - length;
        }
        if (count == 0) {
            return false;
        }

        memmove(buffer.data() + offset + count, buffer.data() + offset, length - offset);
        memcpy(buffer.data() + offset, text, count);
        buffer[length + count] = '\0';
        return true;
    }

    /// Removes [from, to) from a NUL terminated buffer. Returns true when
    /// anything was removed.
    inline auto eraseText(Span<char8_t> buffer, std::size_t from, std::size_t to) noexcept -> bool {
        const std::size_t length = textLength(buffer);
        if (from >= to || from >= length) {
            return false;
        }
        if (to > length) {
            to = length;
        }
        memmove(buffer.data() + from, buffer.data() + to, length - to);
        buffer[length - (to - from)] = '\0';
        return true;
    }
} // namespace ReZeroGui
