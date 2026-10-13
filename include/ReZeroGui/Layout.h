#pragma once
#include <ReZeroGui/ReZeroLib.h>

namespace ReZeroGui {

    struct LayoutContext {
        Vector2 min;
        Vector2 max;
        // 下一个控件的放置位置。
        Vector2 cursor;
        // 所有已放置控件所达到的最远内容边界
        Vector2 cursorMax;
        // 当前行的起始 X 坐标。
        float lineStartX = 0.0f;
        // 当前行中最高控件的高度。
        float lineHeight = 0.0f;
        // 每行起始位置额外增加的缩进量。
        float indent = 0.0f;
        // 已请求换行；下一个控件放置前会先换到新的一行。
        bool pendingWrap = false;

        // 重置布局区域，使其覆盖 [minPoint, maxPoint]，
        // 并将布局光标及行状态初始化。
        void reset(Vector2 minPoint, Vector2 maxPoint) noexcept {
            min = minPoint;
            max = maxPoint;
            cursor = minPoint;
            cursorMax = minPoint;
            lineStartX = minPoint.x;
            lineHeight = 0.0f;
            indent = 0.0f;
            pendingWrap = false;
        }

        // 放置一个控件，并返回该控件占用的矩形区域。
        //
        // 默认每个控件独占一行：放置完成后，会将换行标记设为 true，
        // 使下一个控件在放置前先换到新的一行。
        // 只有调用 sameLine()，才能让下一个控件接着当前控件排列。
        auto place(Vector2 size, Vector2 itemSpacing) noexcept -> Rect {
            if (pendingWrap) {
                wrap(itemSpacing);
            }
            if (cursor.x == lineStartX) {
                // 行首：补上缩进。wrap() 已经加过一次（所以通常这行是空操作），但 reset() 直接开的
                // 第一行不会经过 wrap()，少了这一步"缩进"就只对第二行起的内容生效。
                cursor.x = lineStartX + indent;
            }
            const Rect rect(cursor, size);
            cursor.x += size.x + itemSpacing.x;
            lineHeight = maximum(lineHeight, size.y);
            cursorMax.x = maximum(cursorMax.x, rect.maxX());
            cursorMax.y = maximum(cursorMax.y, rect.maxY());
            pendingWrap = true;
            return rect;
        }

        /// 让下一个控件接着当前这一行放，而不是另起一行。
        ///
        /// 这是 place() 结尾那次 pendingWrap = true 的唯一开关：默认一个控件独占一行，
        /// 在这里把它清掉，下一个控件就落在前一个的右边，间距是 itemSpacing.x。
        /// 这一行的行高取行内最高控件，换行时按它推进 y。
        ///
        /// 在这一行还没有任何控件时调用它是无害的：游标本来就在行首。
        void sameLine() noexcept { pendingWrap = false; }

        // 结束当前行，并将光标移动到下一行。
        void wrap(Vector2 itemSpacing) noexcept {
            cursor.x = lineStartX + indent;
            cursor.y += lineHeight + itemSpacing.y;
            lineHeight = 0.0f;
            pendingWrap = false;
        }

        // 返回下一个控件在它将要落到的那一行上还有多少宽度可用。
        //
        // 这里必须和 place() 的落点一致，否则"占满整行"的控件会算出比实际可用宽度更大的尺寸。
        // 两种情况都要按缩进后的行首算：已经请求换行，或者这一行还什么都没放（游标就在行首，
        // 而 place() 会把行首推到 lineStartX + indent）。
        //
        // contentRegionWidth() 就是这个值。
        float availableWidth() const noexcept {
            const bool atLineStart = cursor.x == lineStartX;
            const float startX = (pendingWrap || atLineStart) ? lineStartX + indent : cursor.x;
            return ReZeroGui::maximum(0.0f, max.x - startX);
        }

        // 返回截至目前，所有已放置控件所占内容区域的尺寸。
        auto contentSize() const noexcept -> Vector2 {
            return Vector2(maximum(0.0f, cursorMax.x - min.x), maximum(0.0f, cursorMax.y - min.y));
        }
    };
} // namespace ReZeroGui
