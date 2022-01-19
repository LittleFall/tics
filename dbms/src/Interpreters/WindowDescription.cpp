#include <Interpreters/WindowDescription.h>

#include <Core/Field.h>
#include <Common/FieldVisitors.h>
#include <IO/Operators.h>
#include <Parsers/ASTFunction.h>
#include <Core/SortDescription.h>


namespace DB
{
namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
}


std::string WindowFrame::toString() const
{
    WriteBufferFromOwnString buf;
    toString(buf);
    return buf.str();
}

void WindowFrame::toString(WriteBuffer & buf) const
{
    buf << getFrameTypeName(type) << " BETWEEN ";
    if (begin_type == BoundaryType::Current)
    {
        buf << "CURRENT ROW";
    }
    else if (begin_type == BoundaryType::Unbounded)
    {
        buf << "UNBOUNDED";
        buf << " "
            << (begin_preceding ? "PRECEDING" : "FOLLOWING");
    }
    else
    {
        buf << applyVisitor(FieldVisitorToString(), begin_offset);
        buf << " "
            << (begin_preceding ? "PRECEDING" : "FOLLOWING");
    }
    buf << " AND ";
    if (end_type == BoundaryType::Current)
    {
        buf << "CURRENT ROW";
    }
    else if (end_type == BoundaryType::Unbounded)
    {
        buf << "UNBOUNDED";
        buf << " "
            << (end_preceding ? "PRECEDING" : "FOLLOWING");
    }
    else
    {
        buf << applyVisitor(FieldVisitorToString(), end_offset);
        buf << " "
            << (end_preceding ? "PRECEDING" : "FOLLOWING");
    }
}

std::string WindowFrame::getFrameTypeName(FrameType type) const
{
    switch (type)
    {
    case FrameType::Rows:
        return "rows";
    case FrameType::Groups:
        return "groups";
    case FrameType::Range:
        return "range";
    default:
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid frame type", type);
    }
}

void WindowFrame::checkValid() const
{
    // Check the validity of offsets.
    if (type == WindowFrame::FrameType::Rows
        || type == WindowFrame::FrameType::Groups)
    {
        if (begin_type == BoundaryType::Offset
            && !((begin_offset.getType() == Field::Types::UInt64
                  || begin_offset.getType() == Field::Types::Int64)
                 && begin_offset.get<Int64>() >= 0
                 && begin_offset.get<Int64>() < INT_MAX))
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                            "Frame start offset for '{}' frame must be a nonnegative 32-bit integer, '{}' of type '{}' given",
                            type,
                            applyVisitor(FieldVisitorToString(), begin_offset),
                            begin_offset.getType());
        }

        if (end_type == BoundaryType::Offset
            && !((end_offset.getType() == Field::Types::UInt64
                  || end_offset.getType() == Field::Types::Int64)
                 && end_offset.get<Int64>() >= 0
                 && end_offset.get<Int64>() < INT_MAX))
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                            "Frame end offset for '{}' frame must be a nonnegative 32-bit integer, '{}' of type '{}' given",
                            type,
                            applyVisitor(FieldVisitorToString(), end_offset),
                            end_offset.getType());
        }
    }

    // Check relative positioning of offsets.
    // UNBOUNDED PRECEDING end and UNBOUNDED FOLLOWING start should have been
    // forbidden at the parsing level.
    assert(!(begin_type == BoundaryType::Unbounded && !begin_preceding));
    assert(!(end_type == BoundaryType::Unbounded && end_preceding));

    if (begin_type == BoundaryType::Unbounded
        || end_type == BoundaryType::Unbounded)
    {
        return;
    }

    if (begin_type == BoundaryType::Current
        && end_type == BoundaryType::Offset
        && !end_preceding)
    {
        return;
    }

    if (end_type == BoundaryType::Current
        && begin_type == BoundaryType::Offset
        && begin_preceding)
    {
        return;
    }

    if (end_type == BoundaryType::Current
        && begin_type == BoundaryType::Current)
    {
        // BETWEEN CURRENT ROW AND CURRENT ROW makes some sense for RANGE or
        // GROUP frames, and is technically valid for ROWS frame.
        return;
    }

    if (end_type == BoundaryType::Offset
        && begin_type == BoundaryType::Offset)
    {
        // Frame start offset must be less or equal that the frame end offset.
        bool begin_less_equal_end;
        if (begin_preceding && end_preceding)
        {
            /// we can't compare Fields using operator<= if fields have different types
            begin_less_equal_end = applyVisitor(FieldVisitorAccurateLessOrEqual(), end_offset, begin_offset);
        }
        else if (begin_preceding && !end_preceding)
        {
            begin_less_equal_end = true;
        }
        else if (!begin_preceding && end_preceding)
        {
            begin_less_equal_end = false;
        }
        else /* if (!begin_preceding && !end_preceding) */
        {
            begin_less_equal_end = applyVisitor(FieldVisitorAccurateLessOrEqual(), begin_offset, end_offset);
        }

        if (!begin_less_equal_end)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                            "Frame start offset {} {} does not precede the frame end offset {} {}",
                            begin_offset.toString(),
                            begin_preceding ? "PRECEDING" : "FOLLOWING",
                            end_offset.toString(),
                            end_preceding ? "PRECEDING" : "FOLLOWING");
        }
        return;
    }

    throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Window frame '{}' is invalid",
                    toString());
}

void WindowDescription::checkValid() const
{
    frame.checkValid();

    // RANGE OFFSET requires exactly one ORDER BY column.
    if (frame.type == WindowFrame::FrameType::Range
        && (frame.begin_type == WindowFrame::BoundaryType::Offset
            || frame.end_type == WindowFrame::BoundaryType::Offset)
        && order_by.size() != 1)
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "The RANGE OFFSET window frame requires exactly one ORDER BY column, {} given",
                        order_by.size());
    }
}

WindowFrame::BoundaryType getBoundaryTypeFromTipb(tipb::BoundaryType type)
{
    switch (type)
    {
    case tipb::BoundaryType::Current:
        return WindowFrame::BoundaryType::Current;
    case tipb::BoundaryType::Offset:
        return WindowFrame::BoundaryType::Offset;
    case tipb::BoundaryType::Unbounded:
        return WindowFrame::BoundaryType::Unbounded;
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Unknowed boundary type {}",
                    type);
}

WindowFrame::FrameType getFrameTypeFromTipb(tipb::WindowFrameMode type)
{
    switch (type)
    {
    case tipb::WindowFrameMode::Range:
        return WindowFrame::FrameType::Range;
    case tipb::WindowFrameMode::Rows:
        return WindowFrame::FrameType::Rows;
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Unknowed frame type {}",
                    type);
}

void WindowDescription::setWindowFrame(tipb::WindowFrame frame_)
{
    frame.type = getFrameTypeFromTipb(frame_.window_frame_mode());
    frame.begin_offset = frame_.begin_offset();
    frame.begin_type = getBoundaryTypeFromTipb(frame_.begin_type());
    frame.begin_preceding = frame_.begin_preceding();
    frame.end_offset = frame_.end_offset();
    frame.end_type = getBoundaryTypeFromTipb(frame_.end_type());
    frame.end_preceding = frame_.end_preceding();
    frame.is_default = false;
}
}