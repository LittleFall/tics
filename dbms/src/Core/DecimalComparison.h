#pragma once

#include <common/arithmeticOverflow.h>
#include <Core/Block.h>
#include <Core/AccurateComparison.h>
#include <Common/Decimal.h>
#include <Common/typeid_cast.h>
#include <DataTypes/DataTypeDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnConst.h>
#include <Functions/FunctionHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int DECIMAL_OVERFLOW;
}

template <size_t > struct ConstructDecInt { using Type = Int32; };
template <> struct ConstructDecInt<8> { using Type = Int64; };
template <> struct ConstructDecInt<16> { using Type = Int128; };
template <> struct ConstructDecInt<sizeof(Int256)> { using Type = Int256; };

template <typename T, typename U>
struct DecCompareInt
{
    using Type = typename ConstructDecInt<(sizeof(T) > sizeof(U)) ? sizeof(T) : sizeof(U)>::Type;
};

///
template <typename A, typename B, template <typename, typename> typename Operation, bool _check_overflow = true,
    bool _actual = IsDecimal<A> || IsDecimal<B>>
class DecimalComparison
{
public:
    using CompareInt = typename DecCompareInt<A, B>::Type;
    using ColVecA = std::conditional_t<IsDecimal<A>, ColumnDecimal<A>, ColumnVector<A>>;
    using ColVecB = std::conditional_t<IsDecimal<B>, ColumnDecimal<B>, ColumnVector<B>>;
    using ArrayA = typename ColVecA::Container;
    using ArrayB = typename ColVecB::Container;

    static constexpr bool cast_float_a = std::is_floating_point_v<B>;
    static constexpr bool cast_float_b = std::is_floating_point_v<A>;

    using Op = std::conditional_t<cast_float_a, Operation<B,B>, std::conditional_t<cast_float_b, Operation<A,A>, Operation<CompareInt, CompareInt>>>;

    DecimalComparison(Block & block, size_t result, const ColumnWithTypeAndName & col_left, const ColumnWithTypeAndName & col_right)
    {
        if (!apply(block, result, col_left, col_right))
            throw Exception("Wrong decimal comparison with " + col_left.type->getName() + " and " + col_right.type->getName(),
                            ErrorCodes::LOGICAL_ERROR);
    }

    static bool apply(Block & block, size_t result [[maybe_unused]],
                      const ColumnWithTypeAndName & col_left, const ColumnWithTypeAndName & col_right)
    {
        if constexpr (_actual)
        {
            ColumnPtr c_res;
            if constexpr(cast_float_a || cast_float_b) {
                c_res = apply<false, false>(col_left.column, col_right.column, 1);
            } else {
                Shift shift = getScales<A, B>(col_left.type, col_right.type);

                c_res = applyWithScale(col_left.column, col_right.column, shift);
            }
            if (c_res)
                block.getByPosition(result).column = std::move(c_res);

            return true;
        }
        return false;
    }

    static bool compare(A a, B b, UInt32 scale_a, UInt32 scale_b)
    {
        static const UInt32 max_scale = maxDecimalPrecision<Decimal256>();
        if (scale_a > max_scale || scale_b > max_scale)
            throw Exception("Bad scale of decimal field", ErrorCodes::DECIMAL_OVERFLOW);

        Shift shift;
        if (scale_a < scale_b)
            shift.a = static_cast<CompareInt>(getScaleMultiplier<B>(scale_b - scale_a));
        if (scale_a > scale_b)
            shift.b = static_cast<CompareInt>(getScaleMultiplier<A>(scale_a - scale_b));

        return applyWithScale(a, b, shift);
    }

private:

    struct Shift
    {
        CompareInt a = 1;
        CompareInt b = 1;

        bool none() const { return a == 1 && b == 1; }
        bool left() const { return a != 1; }
        bool right() const { return b != 1; }
    };

    template <typename T, typename U>
    static auto applyWithScale(T a, U b, const Shift & shift)
    {
        if (shift.left())
            return apply<true, false>(a, b, shift.a);
        else if (shift.right())
            return apply<false, true>(a, b, shift.b);
        return apply<false, false>(a, b, 1);
    }

    template <typename T, typename U>
    static std::enable_if_t<IsDecimal<T> && IsDecimal<U>, Shift>
    getScales(const DataTypePtr & left_type, const DataTypePtr & right_type)
    {
        const DataTypeDecimal<T> * decimal0 = checkDecimal<T>(*left_type);
        const DataTypeDecimal<U> * decimal1 = checkDecimal<U>(*right_type);

        Shift shift;
        if (decimal0 && decimal1)
        {
            auto result_type = decimalResultType(*decimal0, *decimal1);
            shift.a = result_type.scaleFactorFor(*decimal0);
            shift.b = result_type.scaleFactorFor(*decimal1);
        }
        else if (decimal0)
            shift.b = static_cast<CompareInt>(getScaleMultiplier<T>(decimal0->getScale()));
        else if (decimal1)
            shift.a = static_cast<CompareInt>(getScaleMultiplier<U>(decimal1->getScale()));

        return shift;
    }

    template <typename T, typename U>
    static std::enable_if_t<IsDecimal<T> && !IsDecimal<U>, Shift>
    getScales(const DataTypePtr & left_type, const DataTypePtr &)
    {
        Shift shift;
        const DataTypeDecimal<T> * decimal0 = checkDecimal<T>(*left_type);
        if (decimal0)
            shift.b = static_cast<CompareInt>(getScaleMultiplier<T>(decimal0->getScale()));
        return shift;
    }

    template <typename T, typename U>
    static std::enable_if_t<!IsDecimal<T> && IsDecimal<U>, Shift>
    getScales(const DataTypePtr &, const DataTypePtr & right_type)
    {
        Shift shift;
        const DataTypeDecimal<U> * decimal1 = checkDecimal<U>(*right_type);
        if (decimal1)
            shift.a = static_cast<CompareInt>(getScaleMultiplier<U>(decimal1->getScale()));
        return shift;
    }

    template <bool scale_left, bool scale_right>
    static ColumnPtr apply(const ColumnPtr & c0, const ColumnPtr & c1, CompareInt scale[[maybe_unused]])
    {
        auto c_res = ColumnUInt8::create();

        if constexpr (_actual)
        {
            bool c0_is_const = c0->isColumnConst();
            bool c1_is_const = c1->isColumnConst();

            if (c0_is_const && c1_is_const)
            {
                const ColumnConst * c0_const = checkAndGetColumnConst<ColVecA>(c0.get());
                const ColumnConst * c1_const = checkAndGetColumnConst<ColVecB>(c1.get());

                A a = c0_const->template getValue<A>();
                B b = c1_const->template getValue<B>();
                Int8 res;
                if constexpr (cast_float_a) {
                    ScaleType scale_ = c0_const->getField().safeGet<typename NearestFieldType<A>::Type>().getScale();
                    B x = a.template toFloat<B>(scale_);
                    res = Op::apply(x, b);
                } else if constexpr (cast_float_b) {
                    ScaleType scale_ = c1_const->getField().safeGet<typename NearestFieldType<B>::Type>().getScale();
                    A y = b.template toFloat<A>(scale_);
                    res = Op::apply(a, y);
                } else
                    res = apply<scale_left, scale_right>(a, b, scale);
                return DataTypeUInt8().createColumnConst(c0->size(), toField(res));
            }

            ColumnUInt8::Container & vec_res = c_res->getData();
            vec_res.resize(c0->size());

            if (c0_is_const)
            {
                const ColumnConst * c0_const = checkAndGetColumnConst<ColVecA>(c0.get());
                A a = c0_const->template getValue<A>();
                if (const ColVecB * c1_vec = checkAndGetColumn<ColVecB>(c1.get())) {
                    if constexpr (cast_float_a) {
                        ScaleType scale_ = c0_const->getField().safeGet<typename NearestFieldType<A>::Type>().getScale();
                        B x = a.template toFloat<B>(scale_);
                        constant_vector<scale_left, scale_right>(x, c1_vec->getData(), vec_res, scale);
                    } else
                        constant_vector<scale_left, scale_right>(a, c1_vec->getData(), vec_res, scale);
                }
                else
                    throw Exception("Wrong column in Decimal comparison", ErrorCodes::LOGICAL_ERROR);
            }
            else if (c1_is_const)
            {
                const ColumnConst * c1_const = checkAndGetColumnConst<ColVecB>(c1.get());
                B b = c1_const->template getValue<B>();
                if (const ColVecA * c0_vec = checkAndGetColumn<ColVecA>(c0.get())) {
                    if constexpr (cast_float_b) {
                        ScaleType scale_ = c1_const->getField().safeGet<typename NearestFieldType<B>::Type>().getScale();
                        A y = b.template toFloat<A>(scale_);
                        vector_constant<scale_left, scale_right>(c0_vec->getData(), y, vec_res, scale);
                    } else
                        vector_constant<scale_left, scale_right>(c0_vec->getData(), b, vec_res, scale);
                }
                else
                    throw Exception("Wrong column in Decimal comparison", ErrorCodes::LOGICAL_ERROR);
            }
            else
            {
                if (const ColVecA * c0_vec = checkAndGetColumn<ColVecA>(c0.get()))
                {
                    if (const ColVecB * c1_vec = checkAndGetColumn<ColVecB>(c1.get()))
                        vector_vector<scale_left, scale_right>(c0_vec->getData(), c1_vec->getData(), vec_res, scale);
                    else
                        throw Exception("Wrong column in Decimal comparison", ErrorCodes::LOGICAL_ERROR);
                }
                else
                    throw Exception("Wrong column in Decimal comparison", ErrorCodes::LOGICAL_ERROR);
            }
        }

        return c_res;
    }

    template <bool scale_left, bool scale_right>
    static NO_INLINE UInt8 apply(A a, B b, CompareInt scale [[maybe_unused]])
    {
        CompareInt x = static_cast<CompareInt>(a);
        CompareInt y = static_cast<CompareInt>(b);

        if constexpr (_check_overflow)
        {
            bool overflow = false;

            if constexpr (sizeof(A) > sizeof(CompareInt))
                overflow |= (A(x) != a);
            if constexpr (sizeof(B) > sizeof(CompareInt))
                overflow |= (B(y) != b);
            if constexpr (std::is_unsigned_v<A>)
                overflow |= (x < 0);
            if constexpr (std::is_unsigned_v<B>)
                overflow |= (y < 0);

            if constexpr (scale_left) {
                if constexpr (std::is_same_v<CompareInt, Int256>)
                    x = x * scale;
                else
                    overflow |= common::mulOverflow(x, scale, x);
            }
            if constexpr (scale_right) {
                if constexpr (std::is_same_v<CompareInt, Int256>)
                    y = y * scale;
                else
                    overflow |= common::mulOverflow(y, scale, y);
            }
            if (overflow)
                throw Exception("Can't compare", ErrorCodes::DECIMAL_OVERFLOW);
        }
        else
        {
            if constexpr (scale_left)
                x *= scale;
            if constexpr (scale_right)
                y *= scale;
        }

        return Op::apply(x, y);
    }

    template <bool scale_left, bool scale_right>
    static void NO_INLINE vector_vector(const ArrayA & a, const ArrayB & b, PaddedPODArray<UInt8> & c,
                                        CompareInt scale [[maybe_unused]])
    {
        size_t size = a.size();
        const A * a_pos = a.data();
        const B * b_pos = b.data();
        UInt8 * c_pos = c.data();
        const A * a_end = a_pos + size;

        while (a_pos < a_end)
        {
            if constexpr(cast_float_a) {
                B x = a_pos -> template toFloat<B>(a.getScale());
                *c_pos = Op::apply(x, *b_pos);
            } else if constexpr(cast_float_b) {
                A y = b_pos -> template toFloat<A>(b.getScale());
                *c_pos = Op::apply(*a_pos, y);
            } else
                *c_pos = apply<scale_left, scale_right>(*a_pos, *b_pos, scale);
            ++a_pos;
            ++b_pos;
            ++c_pos;
        }
    }

    template <bool scale_left, bool scale_right, typename Y>
    static void NO_INLINE vector_constant(const ArrayA & a, Y b, PaddedPODArray<UInt8> & c, CompareInt scale [[maybe_unused]])
    {
        size_t size = a.size();
        const A * a_pos = a.data();
        UInt8 * c_pos = c.data();
        const A * a_end = a_pos + size;

        while (a_pos < a_end)
        {
            if constexpr(cast_float_a) {
                B x = a_pos -> template toFloat<B>(a.getScale());
                *c_pos = Op::apply(x, b);
            } else if constexpr(cast_float_b) {
                *c_pos = Op::apply(*a_pos, b);
            } else
                *c_pos = apply<scale_left, scale_right>(*a_pos, b, scale);
            ++a_pos;
            ++c_pos;
        }
    }

    template <bool scale_left, bool scale_right, typename X>
    static void NO_INLINE constant_vector(X a, const ArrayB & b, PaddedPODArray<UInt8> & c, CompareInt scale [[maybe_unused]])
    {
        size_t size = b.size();
        const B * b_pos = b.data();
        UInt8 * c_pos = c.data();
        const B * b_end = b_pos + size;

        while (b_pos < b_end)
        {
            if constexpr(cast_float_b) {
                A y = b_pos -> template toFloat<A>(b.getScale());
                *c_pos = Op::apply(a, y);
            } else if constexpr(cast_float_a) {
                *c_pos = Op::apply(a, *b_pos);
            } else
                *c_pos = apply<scale_left, scale_right>(a, *b_pos, scale);
            ++b_pos; ++c_pos; } } };

}
