#pragma once
// format() 从每帧的 FrameArena 里要缓冲区：先用 maxFormattedLength() 按参数算出上界，再写进去，
// 所以永远不会写满，也就不需要"重试 + 扩容"。文本因此活在 arena 里直到本帧结束，调用方拿到的
// u8StringView 就是借来的视图，不复制（正式的使用者是 Ui::text()）。
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ReZeroGui/ReZeroLib.h>

namespace ReZeroGui {
    namespace detail {

        /// 整数输出的进制。
        enum class Radix : std::uint8_t { Decimal, Hex };

        /// 浮点输出默认最多保留的小数位。
        ///
        /// 定点小数：1/3 显示成 0.3333，末尾的 0 不输出（3.5 就是 "3.5" 而不是 "3.5000"）。
        /// 4 位对 HUD 足够；再多只是噪音，而且定点写法在很大量级上早就失去精度了。
        inline constexpr int defaultFloatDigits = 4;

        /// 支持的最多小数位数。再多的位数没有意义，也放不进内部的临时区。
        inline constexpr int maxFloatDigits = 6;

        /// 反向写十进制数字，返回起始位置。调用方给出缓冲区末尾。
        inline auto writeDecimalBackwards(char8_t *end, std::uint64_t value) noexcept -> char8_t * {
            char8_t *cursor = end;
            do {
                *--cursor = static_cast<char8_t>(u8'0' + static_cast<char8_t>(value % 10u));
                value /= 10u;
            } while (value != 0u);
            return cursor;
        }

        /// 反向写十六进制数字（小写）。
        inline auto writeHexBackwards(char8_t *end, std::uint64_t value) noexcept -> char8_t * {
            constexpr char8_t Digits[] = u8"0123456789abcdef";
            char8_t *cursor = end;
            do {
                *--cursor = Digits[value & 0xFu];
                value >>= 4u;
            } while (value != 0u);
            return cursor;
        }

        /// 正向写十进制数字。位数少，借一次临时区比反向再翻转省事。
        inline auto writeDecimalForward(char8_t *destination, std::uint64_t value) noexcept -> char8_t * {
            char8_t scratch[24];
            char8_t *begin = writeDecimalBackwards(scratch + sizeof(scratch), value);
            char8_t *out = destination;
            for (char8_t *cursor = begin; cursor != scratch + sizeof(scratch); ++cursor) {
                *out++ = *cursor;
            }
            return out;
        }

        /// 把 [begin, end) 拷进 buffer 并在其后补 NUL。放不下就截断并断言。
        /// 最后一个字节留给 NUL，所以可见文本最多 buffer.size() - 1 个字节。
        inline auto copyInto(Span<char8_t> buffer, const char8_t *begin, const char8_t *end) noexcept -> std::size_t {
            REZERO_ASSERT_MSG(buffer.size() > 0, "number formatting needs a buffer with room for the terminator");
            const std::size_t count = static_cast<std::size_t>(end - begin);
            const std::size_t capacity = buffer.size() > 0 ? buffer.size() - 1 : 0;
            REZERO_ASSERT_MSG(count <= capacity, "the number does not fit the buffer it was given");
            const std::size_t written = count < capacity ? count : capacity;
            for (std::size_t index = 0; index < written; ++index) {
                buffer[index] = begin[index];
            }
            if (buffer.size() > 0) {
                buffer[written] = u8'\0';
            }
            return written;
        }

        /// 把一段字面量（不含结尾的 NUL）拷进 buffer。
        template <std::size_t Size>
        auto copyLiteral(Span<char8_t> buffer, const char8_t (&text)[Size]) noexcept -> std::size_t {
            return copyInto(buffer, text, text + (Size - 1));
        }

        /// 把整数写成十进制或十六进制，返回写入的字节数（不含结尾的 NUL）。
        ///
        /// 十六进制的负数按它的位模式输出，和 printf 的 "%x" 一致：按类型宽度补满。
        /// 缓冲区放不下是运行期错误（断言），并截断。
        template <class T> auto intToString(Span<char8_t> buffer, T value, Radix radix) noexcept -> std::size_t {
            static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                          "intToString takes an integer, and bool is written as true/false instead");

            char8_t scratch[24];
            char8_t *begin = scratch + sizeof(scratch);
            if (radix == Radix::Hex) {
                begin =
                    writeHexBackwards(begin, static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)));
            } else if constexpr (std::is_signed_v<T>) {
                const bool negative = value < 0;
                // 先转 64 位有符号、再转无符号，取负在无符号里做：这样最小的负数也不会溢出，
                // 而且不会像"先转本类型的无符号、再零扩展到 64 位"那样丢掉符号扩展。
                const std::uint64_t bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
                begin = writeDecimalBackwards(begin, negative ? std::uint64_t{0} - bits : bits);
                if (negative) {
                    *--begin = u8'-';
                }
            } else {
                begin = writeDecimalBackwards(begin, static_cast<std::uint64_t>(value));
            }
            return copyInto(buffer, begin, scratch + sizeof(scratch));
        }

        /// 10 的 digits 次幂。
        inline constexpr auto powerOfTen(int digits) noexcept -> std::uint64_t {
            std::uint64_t result = 1;
            for (int index = 0; index < digits; ++index) {
                result *= 10u;
            }
            return result;
        }

        /// 定点写法能覆盖的最大量级。
        ///
        /// 定点是靠"先缩放成整数再四舍五入"做的，量级乘上 10^digits 不能撑出 uint64，
        /// 所以这个上限随小数位数变化。超过它就改用指数写法。
        inline constexpr auto floatFixedRange(int digits) noexcept -> double {
            return 1.0e18 / static_cast<double>(powerOfTen(digits));
        }

        /// 正向写一个非负、有限的浮点：整数部分 + 最多 digits 位小数，末尾的 0 与孤立的小数点
        /// 都不写。返回写完的位置。调用方保证 value < floatFixedRange(digits)。
        inline auto writeFixed(char8_t *destination, double value, int digits) noexcept -> char8_t * {
            const std::uint64_t scale = powerOfTen(digits);
            const std::uint64_t rounded = static_cast<std::uint64_t>(value * static_cast<double>(scale) + 0.5);
            const std::uint64_t whole = rounded / scale;
            const std::uint64_t fraction = rounded % scale;

            char8_t *out = writeDecimalForward(destination, whole);
            if (digits == 0 || fraction == 0) {
                return out; // 小数部分是 0，连小数点都不写
            }

            // 小数位逐位算出来（高位在前），再去掉末尾的 0。
            char8_t fractionDigits[maxFloatDigits] = {};
            std::uint64_t divisor = scale / 10u;
            for (int index = 0; index < digits; ++index) {
                fractionDigits[index] = static_cast<char8_t>(u8'0' + static_cast<char8_t>((fraction / divisor) % 10u));
                divisor /= 10u;
            }
            int count = digits;
            while (count > 0 && fractionDigits[count - 1] == u8'0') {
                count -= 1;
            }
            *out++ = u8'.';
            for (int index = 0; index < count; ++index) {
                *out++ = fractionDigits[index];
            }
            return out;
        }

        /// 指数写法：把尾数规范到 [1, 10) 再写成 "1.2345e+30"。只在定点写法会丢掉全部有效数字、
        /// 或者量级超出定点范围时才用。
        inline auto writeExponential(char8_t *destination, double magnitude, int digits) noexcept -> char8_t * {
            int exponent = 0;
            double mantissa = magnitude;
            while (mantissa >= 10.0) {
                mantissa /= 10.0;
                exponent += 1;
            }
            while (mantissa < 1.0) {
                mantissa *= 10.0;
                exponent -= 1;
            }

            char8_t *out = writeFixed(destination, mantissa, digits);
            *out++ = u8'e';
            *out++ = exponent < 0 ? u8'-' : u8'+';
            const int absolute = exponent < 0 ? -exponent : exponent;
            if (absolute < 10) {
                *out++ = u8'0'; // 和 printf 的 %e 一样至少两位，读起来整齐
            }
            return writeDecimalForward(out, static_cast<std::uint64_t>(absolute));
        }

        /// 把浮点写成十进制，返回写入的字节数（不含结尾的 NUL）。
        ///
        /// 定点输出，最多 \p digits 位小数，末尾的 0 不写。两种情况下改用指数写法：
        ///   - 量级超出定点的整数范围（value * 10^digits 会撑出 uint64）；
        ///   - 量级小到定点会把全部有效数字都抹成 0（例如 1e-5 在 4 位小数下）。
        /// 非有限值写成 nan / inf / -inf。缓冲区放不下是运行期错误（断言），并截断。
        template <class T>
        auto floatToString(Span<char8_t> buffer, T value, int digits = defaultFloatDigits) noexcept -> std::size_t {
            static_assert(std::is_floating_point_v<T>, "floatToString takes a floating point value");
            REZERO_ASSERT_MSG(digits >= 0 && digits <= maxFloatDigits, "floatToString: digits out of range");

            const double number = static_cast<double>(value);
            if (number != number) {
                return copyLiteral(buffer, u8"nan");
            }

            char8_t scratch[32];
            char8_t *out = scratch;
            if (number < 0.0) {
                *out++ = u8'-';
            }
            const double magnitude = number < 0.0 ? -number : number;
            if (std::isinf(magnitude)) {
                for (const char8_t *letter = u8"inf"; *letter != u8'\0'; ++letter) {
                    *out++ = *letter;
                }
                return copyInto(buffer, scratch, out);
            }
            if (magnitude == 0.0) {
                *out++ = u8'0';
                return copyInto(buffer, scratch, out);
            }

            const double fixedRange = floatFixedRange(digits);
            const double smallest = 0.5 / static_cast<double>(powerOfTen(digits));
            if (magnitude < fixedRange && magnitude >= smallest) {
                out = writeFixed(out, magnitude, digits);
            } else {
                out = writeExponential(out, magnitude, digits);
            }
            return copyInto(buffer, scratch, out);
        }

        // ---- 参数 → 文本 ---------------------------------------------------------

        /// 输出光标：调用方的缓冲区加上已写入的长度。最后一个字节留给结尾的 NUL。
        struct Output {
            Span<char8_t> buffer;
            std::size_t length = 0;
            bool truncated = false;

            auto view() const noexcept -> u8StringView { return u8StringView(buffer.data(), length); }

            void append(char8_t value) noexcept {
                if (length + 1 >= buffer.size()) {
                    truncated = true;
                    return;
                }
                buffer[length] = value;
                length += 1;
                buffer[length] = u8'\0';
            }

            void append(const char8_t *data, std::size_t size) noexcept {
                for (std::size_t index = 0; index < size; ++index) {
                    append(data[index]);
                }
            }

            /// 以 NUL 结尾的字符串，长度未知。
            void appendCString(const char8_t *text) noexcept {
                if (text == nullptr) {
                    return;
                }
                for (std::size_t index = 0; text[index] != u8'\0'; ++index) {
                    append(text[index]);
                }
            }
        };

        template <class T> struct IsBasicStringView : std::false_type {};
        template <class Char> struct IsBasicStringView<BasicStringView<Char>> : std::true_type {};

        template <class T> inline constexpr bool isCharType = std::is_same_v<T, char> || std::is_same_v<T, char8_t>;

        template <class T> struct IsCharPointer : std::false_type {};
        template <> struct IsCharPointer<char *> : std::true_type {};
        template <> struct IsCharPointer<const char *> : std::true_type {};
        template <> struct IsCharPointer<char8_t *> : std::true_type {};
        template <> struct IsCharPointer<const char8_t *> : std::true_type {};

        template <class T> inline constexpr bool alwaysFalse = false;

        /// 把一个参数写进输出。
        ///
        /// 分支顺序是有意的：char 也是整型，字符指针也是指针，所以窄类型必须先判。
        /// 不支持的类型是编译期错误。
        template <class T> void appendValue(Output &out, const T &value) noexcept {
            using Value = std::remove_cvref_t<T>;

            if constexpr (std::is_same_v<Value, bool>) {
                out.appendCString(value ? u8"true" : u8"false");
            } else if constexpr (isCharType<Value>) {
                // 单个字符按字符输出，不按数字。
                out.append(static_cast<char8_t>(value));
            } else if constexpr (IsCharPointer<Value>::value) {
                out.appendCString(reinterpret_cast<const char8_t *>(value));
            } else if constexpr (IsBasicStringView<Value>::value) {
                // 长度取自视图本身，所以不以 NUL 结尾的视图也是安全的。
                out.append(reinterpret_cast<const char8_t *>(value.data()), value.size());
            } else if constexpr (std::is_array_v<Value> && isCharType<std::remove_extent_t<Value>>) {
                // 字符串字面量传进来是数组类型，它本来就是以 NUL 结尾的。
                out.appendCString(reinterpret_cast<const char8_t *>(value));
            } else if constexpr (std::is_integral_v<Value>) {
                char8_t scratch[32];
                const std::size_t written = intToString(Span<char8_t>(scratch, sizeof(scratch)), value, Radix::Decimal);
                out.append(scratch, written);
            } else if constexpr (std::is_floating_point_v<Value>) {
                char8_t scratch[32];
                const std::size_t written = floatToString(Span<char8_t>(scratch, sizeof(scratch)), value);
                out.append(scratch, written);
            } else if constexpr (std::is_pointer_v<Value>) {
                if (value == nullptr) {
                    out.appendCString(u8"null");
                } else {
                    char8_t scratch[32];
                    scratch[0] = u8'0';
                    scratch[1] = u8'x';
                    const std::size_t written = intToString(Span<char8_t>(scratch + 2, sizeof(scratch) - 2),
                                                            reinterpret_cast<std::uintptr_t>(value), Radix::Hex);
                    out.append(scratch, written + 2);
                }
            } else {
                static_assert(alwaysFalse<Value>,
                              "text: unsupported argument type. Format it yourself first, or add an appendValue "
                              "branch in Format.h.");
            }
        }

        // ---- 扫描与替换 ---------------------------------------------------------

        /// 扫描停在哪里
        struct Scan {
            std::size_t offset = 0;
            /// 为真表示停在了一个 "{}" 上，且 offset 已经指向它之后。
            bool foundPlaceholder = false;
        };

        /// 把 format[offset..] 追加到 out，并把 "{{" / "}}" 还原成单个花括号。
        ///
        /// stopAtPlaceholder 为真时，扫到第一个 "{}" 就停下并报告它——参数就是这样一个个
        /// 被消费掉的。为假时（参数已经用完了）会一直扫到底，此时遇到的 "{}" 按字面输出：
        /// 在界面上看见一个 "{}" 比文本悄悄少一截容易发现得多。
        ///
        /// 落单的 "{" 或 "}" 一律按字面输出（std::format 会报错，这里选择宽容）。
        inline auto copySegment(Output &out, u8StringView format, std::size_t offset, bool stopAtPlaceholder,
                                bool allowExcess = false) noexcept -> Scan {
            while (offset < format.size()) {
                const char8_t current = format[offset];
                const bool hasNext = offset + 1 < format.size();
                const char8_t next = hasNext ? format[offset + 1] : char8_t{};

                if (current == u8'{' && next == u8'{') {
                    out.append(u8'{');
                    offset += 2;
                    continue;
                }
                if (current == u8'{' && next == u8'}') {
                    if (stopAtPlaceholder) {
                        return Scan{offset + 2, true};
                    }
                    // 参数比占位符少才算用错。一个参数都没传是"这串就是纯文本"：
                    // ui.text(u8"按 {} 格式化") 这样的内容文本不该在 Debug 下把进程带走，
                    // 所以那时 "{}" 按字面留着（和 Release 行为一致）。
                    REZERO_ASSERT_MSG(allowExcess || false, "text: more placeholders than arguments");
                    out.append(u8'{');
                    out.append(u8'}');
                    offset += 2;
                    continue;
                }
                if (current == u8'}' && next == u8'}') {
                    out.append(u8'}');
                    offset += 2;
                    continue;
                }
                out.append(current);
                offset += 1;
            }
            return Scan{offset, false};
        }

        /// 递归出口：参数用完了，剩下的格式串全是字面文本。
        /// \p allowExcess 为真表示"本来就没传参数"，此时多余的 "{}" 不算错误（见 copySegment）。
        inline void formatArgs(Output &out, u8StringView format, std::size_t offset, bool allowExcess) noexcept {
            copySegment(out, format, offset, false, allowExcess);
        }

        /// 每递归一层消费一个参数：先抄到下一个 "{}" 之前，写出这个参数，再继续。
        template <class First, class... Rest>
        void formatArgs(Output &out, u8StringView format, std::size_t offset, bool allowExcess, const First &first,
                        const Rest &...rest) noexcept {
            const Scan scan = copySegment(out, format, offset, true);
            if (!scan.foundPlaceholder) {
                REZERO_ASSERT_MSG(false, "text: more arguments than placeholders");
                return;
            }
            appendValue(out, first);
            formatArgs(out, format, scan.offset, allowExcess, rest...);
        }

        /// 非字符串参数按多少字节估算上界。
        ///
        /// 故意取得宽松：int64 十进制最多 20 位、指针 "0x" + 16、浮点最长约 19、bool 5。
        /// 上界不足是 bug，会被 format() 里的断言抓住，所以这里只要保证够大，不必精确。
        inline constexpr std::size_t maxNumberLength = 32;

        /// 以 NUL 结尾的字节串长度。
        template <class Char> auto cstrLengthOf(const Char *text) noexcept -> std::size_t {
            std::size_t length = 0;
            while (text[length] != Char{}) {
                length += 1;
            }
            return length;
        }

        /// 参数真正会写出多少字节。分支与 appendValue() 一一对应，所以上界不会漏算：
        /// 字符串类给出真实长度，其余类型返回 0（它们由 maxNumberLength 覆盖）。
        template <class T> auto stringLength(const T &value) noexcept -> std::size_t {
            using Value = std::remove_cvref_t<T>;

            if constexpr (IsBasicStringView<Value>::value) {
                // 长度取自视图本身，所以不以 NUL 结尾的视图也算得准。
                return value.size();
            } else if constexpr (IsCharPointer<Value>::value) {
                return value == nullptr ? 0 : cstrLengthOf(value);
            } else if constexpr (std::is_array_v<Value> && isCharType<std::remove_extent_t<Value>>) {
                return std::extent_v<Value> - 1;
            } else {
                return 0;
            }
        }

        /// 格式化这段文本最多需要多少字节的缓冲区，**含结尾的 NUL**。
        ///
        /// 交给 arena 一次性要到足够的内存，format() 就不可能写溢出，也就不需要"写满了再扩容"
        /// 那套机制（块满了原地长不大，只能重来一遍）。代价是这个上界，而它只要是安全富余就够了。
        ///
        /// 各段为什么够：格式串本身一个字节最多产出一个字节（"{{" 只会变短）；每个非字符串参数
        /// 由 maxNumberLength 覆盖；字符串参数走 stringLength()，精确到字节。字符串因此被读了
        /// 两遍（一遍量长度、一遍写出），HUD 这种短文本上不值得为省一遍引入状态。
        template <class... Args>
        auto maxFormattedLength(u8StringView format, const Args &...args) noexcept -> std::size_t {
            return format.size() + (maxNumberLength * sizeof...(Args)) + (stringLength(args) + ... + std::size_t{0}) +
                   1;
        }

        /// 把 format 里的每个 "{}" 依次换成一个参数，"{{" / "}}" 换成单个花括号，返回结果视图。
        ///
        /// 没有 format spec：占位符就是 "{}"，别的一律按字面。
        /// 结果写在 \p arena 里，寿命到本帧结束（见 Context::arena()）；因为先按
        /// maxFormattedLength() 把上界要足，这里不可能写溢出，也就不需要"写满再扩容"那套机制。
        ///
        /// 参数类型不支持是编译期错误（static_assert）；占位符与参数个数不匹配是运行期错误
        /// （断言），多出来的 "{}" 会按字面留在文本里——在界面上看见一个 "{}"，
        /// 比文本悄悄少一截容易发现。
        ///
        /// arena 要不到内存时返回空串，本帧这一条文本就不画：断言在 Debug 下会指出问题，但
        /// Release 下不能因为一次分配失败就把整个进程带走（界面少一行字远好过崩溃）。
        template <class... Args>
        auto format(FrameArena &arena, u8StringView format, const Args &...args) noexcept -> u8StringView {
            const std::size_t needed = maxFormattedLength(format, args...);
            char8_t *buffer = static_cast<char8_t *>(arena.allocate(needed));
            if (buffer == nullptr) {
                REZERO_ASSERT_MSG(false, "format: the frame arena is out of memory");
                return u8StringView();
            }
            Output out{{buffer, needed}, 0, false};
            // 一个参数都没有时，"{}" 是内容文本的一部分，不是漏传的参数。
            formatArgs(out, format, std::size_t{0}, sizeof...(Args) == 0, args...);
            // 上界若是不足，会先在这里炸，而不是让界面显示半句话。
            REZERO_ASSERT_MSG(!out.truncated, "format: the text did not fit maxFormattedLength()");
            return out.view();
        }

    } // namespace detail
} // namespace ReZeroGui
