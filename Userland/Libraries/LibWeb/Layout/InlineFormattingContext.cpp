/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineLevelIterator.h>
#include <LibWeb/Layout/LineBuilder.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

InlineFormattingContext::InlineFormattingContext(BlockContainer& containing_block, BlockFormattingContext& parent)
    : FormattingContext(Type::Inline, containing_block, &parent)
{
}

InlineFormattingContext::~InlineFormattingContext()
{
}

BlockFormattingContext& InlineFormattingContext::parent()
{
    return static_cast<BlockFormattingContext&>(*FormattingContext::parent());
}

BlockFormattingContext const& InlineFormattingContext::parent() const
{
    return static_cast<BlockFormattingContext const&>(*FormattingContext::parent());
}

InlineFormattingContext::AvailableSpaceForLineInfo InlineFormattingContext::available_space_for_line(float y) const
{
    // NOTE: Floats are relative to the BFC root box, not necessarily the containing block of this IFC.
    auto box_in_root_rect = containing_block().margin_box_rect_in_ancestor_coordinate_space(parent().root());
    float y_in_root = box_in_root_rect.y() + y;

    AvailableSpaceForLineInfo info;

    auto const& bfc = parent();

    for (ssize_t i = bfc.left_side_floats().boxes.size() - 1; i >= 0; --i) {
        auto const& floating_box = bfc.left_side_floats().boxes.at(i);
        auto rect = floating_box.margin_box_as_relative_rect();
        if (rect.contains_vertically(y_in_root)) {
            info.left = rect.right() + 1;
            break;
        }
    }

    info.right = containing_block().width();

    for (ssize_t i = bfc.right_side_floats().boxes.size() - 1; i >= 0; --i) {
        auto const& floating_box = bfc.right_side_floats().boxes.at(i);
        auto rect = floating_box.margin_box_as_relative_rect();
        if (rect.contains_vertically(y_in_root)) {
            info.right = rect.left() - 1;
            break;
        }
    }

    return info;
}

void InlineFormattingContext::run(Box&, LayoutMode layout_mode)
{
    VERIFY(containing_block().children_are_inline());

    generate_line_boxes(layout_mode);

    containing_block().for_each_child([&](auto& child) {
        VERIFY(child.is_inline());
        if (is<Box>(child) && child.is_absolutely_positioned()) {
            layout_absolutely_positioned_element(verify_cast<Box>(child));
            return;
        }
    });

    float min_line_height = containing_block().line_height();
    float max_line_width = 0;
    float content_height = 0;

    for (auto& line_box : containing_block().line_boxes()) {
        float max_height = min_line_height;
        for (auto& fragment : line_box.fragments()) {
            max_height = max(max_height, fragment.height());
        }
        max_line_width = max(max_line_width, line_box.width());
        content_height += max_height;
    }

    if (layout_mode != LayoutMode::Default) {
        containing_block().set_width(max_line_width);
    }

    containing_block().set_height(content_height);
}

void InlineFormattingContext::dimension_box_on_line(Box& box, LayoutMode layout_mode)
{
    if (is<ReplacedBox>(box)) {
        auto& replaced = verify_cast<ReplacedBox>(box);
        replaced.set_width(compute_width_for_replaced_element(replaced));
        replaced.set_height(compute_height_for_replaced_element(replaced));
        return;
    }

    if (box.is_inline_block()) {
        auto& inline_block = const_cast<BlockContainer&>(verify_cast<BlockContainer>(box));

        if (inline_block.computed_values().width().is_length() && inline_block.computed_values().width().length().is_undefined_or_auto()) {
            auto result = calculate_shrink_to_fit_widths(inline_block);
            auto width_of_containing_block = CSS::Length::make_px(containing_block().width());

            auto margin_left = inline_block.computed_values().margin().left.resolved(width_of_containing_block).resolved_or_zero(inline_block).to_px(inline_block);
            auto border_left_width = inline_block.computed_values().border_left().width;
            auto padding_left = inline_block.computed_values().padding().left.resolved(width_of_containing_block).resolved_or_zero(inline_block).to_px(inline_block);

            auto margin_right = inline_block.computed_values().margin().right.resolved(width_of_containing_block).resolved_or_zero(inline_block).to_px(inline_block);
            auto border_right_width = inline_block.computed_values().border_right().width;
            auto padding_right = inline_block.computed_values().padding().right.resolved(width_of_containing_block).resolved_or_zero(inline_block).to_px(inline_block);

            auto available_width = containing_block().width()
                - margin_left
                - border_left_width
                - padding_left
                - padding_right
                - border_right_width
                - margin_right;

            auto width = min(max(result.preferred_minimum_width, available_width), result.preferred_width);
            inline_block.set_width(width);
        } else {
            auto container_width = CSS::Length::make_px(containing_block().width());
            inline_block.set_width(inline_block.computed_values().width().resolved(container_width).resolved_or_zero(inline_block).to_px(inline_block));
        }
        (void)layout_inside(inline_block, layout_mode);

        if (inline_block.computed_values().height().is_length() && inline_block.computed_values().height().length().is_undefined_or_auto()) {
            // FIXME: (10.6.6) If 'height' is 'auto', the height depends on the element's descendants per 10.6.7.
        } else {
            auto container_height = CSS::Length::make_px(containing_block().height());
            inline_block.set_height(inline_block.computed_values().height().resolved(container_height).resolved_or_zero(inline_block).to_px(inline_block));
        }
        return;
    }

    // Non-replaced, non-inline-block, box on a line!?
    // I don't think we should be here. Dump the box tree so we can take a look at it.
    dbgln("FIXME: I've been asked to dimension a non-replaced, non-inline-block box on a line:");
    dump_tree(box);
}

void InlineFormattingContext::generate_line_boxes(LayoutMode layout_mode)
{
    containing_block().line_boxes().clear();

    InlineLevelIterator iterator(containing_block(), layout_mode);
    LineBuilder line_builder(*this);

    for (;;) {
        auto item_opt = iterator.next(line_builder.available_width_for_current_line());
        if (!item_opt.has_value())
            break;
        auto& item = item_opt.value();

        // Ignore collapsible whitespace chunks at the start of line, and if the last fragment already ends in whitespace.
        if (item.is_collapsible_whitespace && containing_block().line_boxes().last().is_empty_or_ends_in_whitespace())
            continue;

        switch (item.type) {
        case InlineLevelIterator::Item::Type::ForcedBreak:
            line_builder.break_line();
            break;
        case InlineLevelIterator::Item::Type::Element: {
            auto& box = verify_cast<Layout::Box>(*item.node);
            dimension_box_on_line(box, layout_mode);
            line_builder.break_if_needed(layout_mode, box.width(), item.should_force_break);
            line_builder.append_box(box);
            break;
        }
        case InlineLevelIterator::Item::Type::Text: {
            auto& text_node = verify_cast<Layout::TextNode>(*item.node);
            line_builder.break_if_needed(layout_mode, item.width, item.should_force_break);
            line_builder.append_text_chunk(
                text_node,
                item.offset_in_node,
                item.length_in_node,
                item.width,
                text_node.font().glyph_height());
            break;
        }
        }
    }

    for (auto& line_box : containing_block().line_boxes()) {
        line_box.trim_trailing_whitespace();
    }

    line_builder.remove_last_line_if_empty();
}

}
