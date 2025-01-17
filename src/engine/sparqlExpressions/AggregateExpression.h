// Copyright 2021, University of Freiburg,
//                  Chair of Algorithms and Data Structures.
// Author: Johannes Kalmbach <kalmbacj@cs.uni-freiburg.de>

#ifndef QLEVER_AGGREGATEEXPRESSION_H
#define QLEVER_AGGREGATEEXPRESSION_H

#include "engine/sparqlExpressions/SparqlExpression.h"
#include "engine/sparqlExpressions/SparqlExpressionGenerators.h"
#include "global/ValueIdComparators.h"

namespace sparqlExpression {

// This can be used as the `FinalOperation` parameter to an
// `AggregateExpression` if there is nothing to be done on the final result.
inline auto noop = []<typename T>(T&& result, size_t) {
  return std::forward<T>(result);
};

// An expression that aggregates its input using the `AggregateOperation` and
// then executes the `FinalOperation` (possibly the `noop` lambda from above) on
// the result.
namespace detail {
template <typename AggregateOperation, typename FinalOperation = decltype(noop)>
class AggregateExpression : public SparqlExpression {
 public:
  // __________________________________________________________________________
  AggregateExpression(bool distinct, Ptr&& child,
                      AggregateOperation aggregateOp = AggregateOperation{});

  // __________________________________________________________________________
  ExpressionResult evaluate(EvaluationContext* context) const override;

  // _________________________________________________________________________
  std::span<SparqlExpression::Ptr> children() override;

  // _________________________________________________________________________
  vector<Variable> getUnaggregatedVariables() override;

  // An `AggregateExpression` (obviously) contains an aggregate.
  bool containsAggregate() const override { return true; }

  // __________________________________________________________________________
  [[nodiscard]] string getCacheKey(
      const VariableToColumnMap& varColMap) const override;

  // __________________________________________________________________________
  [[nodiscard]] std::optional<SparqlExpressionPimpl::VariableAndDistinctness>
  getVariableForCount() const override;

  // This is the visitor for the `evaluateAggregateExpression` function below.
  // It works on a `SingleExpressionResult` rather than on the
  // `ExpressionResult` variant.
  inline static const auto evaluateOnChildOperand =
      []<SingleExpressionResult Operand>(
          const AggregateOperation& aggregateOperation,
          const FinalOperation& finalOperation, EvaluationContext* context,
          bool distinct, Operand&& operand) -> ExpressionResult {
    // Perform the more efficient calculation on `SetOfInterval`s if it is
    // possible.
    if (isAnySpecializedFunctionPossible(
            aggregateOperation._specializedFunctions, operand)) {
      auto optionalResult = evaluateOnSpecializedFunctionsIfPossible(
          aggregateOperation._specializedFunctions,
          std::forward<Operand>(operand));
      AD_CONTRACT_CHECK(optionalResult);
      return std::move(optionalResult.value());
    }

    // The number of inputs we aggregate over.
    auto inputSize = getResultSize(*context, operand);

    // Aggregates are unary expressions, therefore we have only one value getter
    // for the single operand. But since the aggregating operation is binary,
    // there are two identical value getters for technical reasons
    {
      using V = typename AggregateOperation::ValueGetters;
      static_assert(std::tuple_size_v<V> == 2);
      static_assert(std::is_same_v<std::tuple_element_t<0, V>,
                                   std::tuple_element_t<1, V>>);
    }

    const auto& valueGetter = std::get<0>(aggregateOperation._valueGetters);

    if (!distinct) {
      auto values = detail::valueGetterGenerator(
          inputSize, context, std::forward<Operand>(operand), valueGetter);
      auto it = values.begin();
      // Unevaluated operation to get the proper `ResultType`. With `auto`, we
      // would get the operand type, which is not necessarily the `ResultType`.
      // For example, in the COUNT aggregate we calculate a sum of boolean
      // values, but the result is not boolean.
      using ResultType = std::decay_t<decltype(aggregateOperation._function(
          std::move(*it), *it))>;
      ResultType result = *it;
      for (++it; it != values.end(); ++it) {
        result =
            aggregateOperation._function(std::move(result), std::move(*it));
      }
      result = finalOperation(std::move(result), inputSize);
      if constexpr (requires { makeNumericId(result); }) {
        return makeNumericId(result);
      } else {
        return result;
      }
    } else {
      // The operands *without* applying the `valueGetter`.
      auto operands =
          makeGenerator(std::forward<Operand>(operand), inputSize, context);

      // For distinct we must put the operands into the hash set before
      // applying the `valueGetter`. For example, COUNT(?x), where ?x matches
      // three different strings, the value getter always returns `1`, but
      // we still have three distinct inputs.
      auto it = operands.begin();
      // Unevaluated operation to get the proper `ResultType`. With `auto`, we
      // would get the operand type, which is not necessarily the `ResultType`.
      // For example, in the COUNT aggregate we calculate a sum of boolean
      // values, but the result is not boolean.
      using ResultType = std::decay_t<decltype(aggregateOperation._function(
          std::move(valueGetter(*it, context)), valueGetter(*it, context)))>;
      ResultType result = valueGetter(*it, context);
      ad_utility::HashSetWithMemoryLimit<
          typename decltype(operands)::value_type>
          uniqueHashSet({*it}, inputSize, context->_allocator);
      for (++it; it != operands.end(); ++it) {
        if (uniqueHashSet.insert(*it).second) {
          result = aggregateOperation._function(
              std::move(result), valueGetter(std::move(*it), context));
        }
      }
      result = finalOperation(std::move(result), uniqueHashSet.size());
      if constexpr (requires { makeNumericId(result); }) {
        return makeNumericId(result);
      } else {
        return result;
      }
    }
  };

 protected:
  bool _distinct;
  Ptr _child;
  AggregateOperation _aggregateOp;
};

// The Aggregate expressions.

template <typename... Ts>
using AGG_OP = Operation<2, FunctionAndValueGetters<Ts...>>;

template <typename... Ts>
using AGG_EXP =
    AggregateExpression<Operation<2, FunctionAndValueGetters<Ts...>>>;

// COUNT
/// For the count expression, we have to manually overwrite one member function
/// for the pattern trick.
inline auto count = [](const auto& a, const auto& b) -> int64_t {
  return a + b;
};
using CountExpressionBase = AGG_EXP<decltype(count), IsValidValueGetter>;
class CountExpression : public CountExpressionBase {
  using CountExpressionBase::CountExpressionBase;
  [[nodiscard]] std::optional<SparqlExpressionPimpl::VariableAndDistinctness>
  getVariableForCount() const override {
    auto optionalVariable = _child->getVariableOrNullopt();
    if (optionalVariable.has_value()) {
      return SparqlExpressionPimpl::VariableAndDistinctness{
          std::move(optionalVariable.value()), _distinct};
    } else {
      return std::nullopt;
    }
  }
};

// Take a `NumericOperation` that takes numeric arguments (integral or floating
// points) and returns a numeric result. Return a function that performs the
// same operation, but takes and returns the `NumericValue` variant.
template <typename NumericOperation>
inline auto makeNumericExpressionForAggregate() {
  return [](const std::same_as<NumericValue> auto&... args) -> NumericValue {
    auto visitor = []<typename... Ts>(const Ts&... t) -> NumericValue {
      if constexpr ((... || std::is_same_v<NotNumeric, Ts>)) {
        return NotNumeric{};
      } else {
        return (NumericOperation{}(t...));
      }
    };
    return std::visit(visitor, args...);
  };
}

// SUM
inline auto addForSum = makeNumericExpressionForAggregate<std::plus<>>();
using SumExpression = AGG_EXP<decltype(addForSum), NumericValueGetter>;

// AVG
inline auto averageFinalOp = [](const NumericValue& aggregation,
                                size_t numElements) {
  return makeNumericExpressionForAggregate<std::divides<>>()(
      aggregation, NumericValue{static_cast<double>(numElements)});
};
using AvgExpression =
    detail::AggregateExpression<AGG_OP<decltype(addForSum), NumericValueGetter>,
                                decltype(averageFinalOp)>;

// Min and Max.
template <typename comparator, valueIdComparators::Comparison comparison>
inline const auto minMaxLambdaForAllTypes = []<SingleExpressionResult T>(
                                                const T& a, const T& b) {
  if constexpr (std::is_arithmetic_v<T> ||
                ad_utility::isSimilar<T, std::string>) {
    // TODO<joka921> Also implement correct comparisons for `std::string`
    // using ICU that respect the locale
    return comparator{}(a, b);
  } else if constexpr (ad_utility::isSimilar<T, Id>) {
    if (a.getDatatype() == Datatype::Undefined ||
        b.getDatatype() == Datatype::Undefined) {
      // If one of the values is undefined, we just return the other.
      static_assert(0u == Id::makeUndefined().getBits());
      return Id::fromBits(a.getBits() | b.getBits());
    }
    return toBoolNotUndef(valueIdComparators::compareIds<
                          valueIdComparators::ComparisonForIncompatibleTypes::
                              CompareByType>(a, b, comparison))
               ? a
               : b;
  } else {
    return ad_utility::alwaysFalse<T>;
  }
};

constexpr inline auto min = [](const auto& a, const auto& b) {
  return std::min(a, b);
};
constexpr inline auto max = [](const auto& a, const auto& b) {
  return std::max(a, b);
};
constexpr inline auto minLambdaForAllTypes =
    minMaxLambdaForAllTypes<decltype(min), valueIdComparators::Comparison::LT>;
constexpr inline auto maxLambdaForAllTypes =
    minMaxLambdaForAllTypes<decltype(max), valueIdComparators::Comparison::GT>;
// MIN
using MinExpression =
    AGG_EXP<decltype(minLambdaForAllTypes), ActualValueGetter>;

// MAX
using MaxExpression =
    AGG_EXP<decltype(maxLambdaForAllTypes), ActualValueGetter>;

}  // namespace detail

using detail::AvgExpression;
using detail::CountExpression;
using detail::MaxExpression;
using detail::MinExpression;
using detail::SumExpression;
}  // namespace sparqlExpression

#endif  // QLEVER_AGGREGATEEXPRESSION_H
