// Copyright 2023, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author: Andre Schlegel (March of 2023, schlegea@informatik.uni-freiburg.de)

#include "util/ConfigManager/ConfigOption.h"

#include <absl/strings/str_cat.h>

#include <array>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "global/ValueId.h"
#include "util/Algorithm.h"
#include "util/ConfigManager/ConfigExceptions.h"
#include "util/ConstexprUtils.h"
#include "util/Exception.h"
#include "util/Forward.h"
#include "util/StringUtils.h"
#include "util/TypeTraits.h"
#include "util/json.h"

namespace ad_utility {
// ____________________________________________________________________________
std::string ConfigOption::availableTypesToString(const AvailableTypes& value) {
  auto toStringVisitor = []<typename T>(const T&) -> std::string {
    if constexpr (std::is_same_v<T, bool>) {
      return "boolean";
    } else if constexpr (std::is_same_v<T, std::string>) {
      return "string";
    } else if constexpr (std::is_same_v<T, int>) {
      return "integer";
    } else if constexpr (std::is_same_v<T, size_t>) {
      return "unsigned integer";
    } else if constexpr (std::is_same_v<T, float>) {
      return "float";
    } else {
      // It must be a vector.
      static_assert(ad_utility::isVector<T>);
      return absl::StrCat(
          "list of ", availableTypesToString<typename T::value_type>(), "s");
    }
  };

  return std::visit(toStringVisitor, value);
}

// ____________________________________________________________________________
bool ConfigOption::wasSetAtRuntime() const {
  return configurationOptionWasSet_;
}

// ____________________________________________________________________________
bool ConfigOption::hasDefaultValue() const {
  return std::visit([](const auto& d) { return d.defaultValue_.has_value(); },
                    data_);
}

// ____________________________________________________________________________
bool ConfigOption::wasSet() const {
  return wasSetAtRuntime() || hasDefaultValue();
}

// ____________________________________________________________________________
void ConfigOption::setValueWithJson(const nlohmann::json& json) {
  // TODO<C++23> Use "deducing this" for simpler recursive lambdas.
  /*
  Manually checks, if the json represents one of the possibilites of
  `AvailableTypes`.
  */
  auto isValueTypeSubType = []<typename T>(const nlohmann::json& j,
                                           auto& isValueTypeSubType) -> bool {
    if constexpr (std::is_same_v<T, bool>) {
      return j.is_boolean();
    } else if constexpr (std::is_same_v<T, std::string>) {
      return j.is_string();
    } else if constexpr (std::is_same_v<T, int>) {
      return j.is_number_integer();
    } else if constexpr (std::is_same_v<T, size_t>) {
      return j.is_number_unsigned();
    } else if constexpr (std::is_same_v<T, float>) {
      return j.is_number_float();
    } else {
      // Only the vector type remains.
      static_assert(ad_utility::isVector<T>);

      /*
      The recursiv call needs to be a little more complicated, because the
      `std::vector<bool>` doesn't actually contain booleans, but bits. This
      causes the check to always fail for `std::vector<bool>`, if we don't pass
      the type explicitly, because the bit type is not something, that this
      function allows.
      */
      return j.is_array() && [&j, &isValueTypeSubType]<typename InnerType>(
                                 const std::vector<InnerType>&) {
        return std::ranges::all_of(j, [&isValueTypeSubType](const auto& entry) {
          return isValueTypeSubType.template operator()<InnerType>(
              entry, AD_FWD(isValueTypeSubType));
        });
      }(T{});
    }
  };

  /*
  Check: Does the json, that we got, actually represent the type of value, this
  option is meant to hold?
  */
  if (!std::visit(
          [&isValueTypeSubType, &json]<typename T>(const T&) {
            return isValueTypeSubType.operator()<typename T::Type>(
                json, isValueTypeSubType);
          },
          data_)) {
    // Does the json represent one of the types in our `AvailableTypes`? If yes,
    // we can create a better exception message.
    ad_utility::ConstexprForLoop(
        std::make_index_sequence<std::variant_size_v<AvailableTypes>>{},
        [&isValueTypeSubType, &json,
         this ]<size_t TypeIndex,
                typename AlternativeType =
                    std::variant_alternative_t<TypeIndex, AvailableTypes>>() {
          if (isValueTypeSubType.template operator()<AlternativeType>(
                  json, isValueTypeSubType)) {
            throw ConfigOptionSetWrongJsonTypeException(
                identifier_, getActualValueTypeAsString(),
                availableTypesToString<AlternativeType>());
          }
        });

    throw ConfigOptionSetWrongJsonTypeException(
        identifier_, getActualValueTypeAsString(), "unknown");
  }

  std::visit(
      [&json, this]<typename T>(const Data<T>&) { setValue(json.get<T>()); },
      data_);
  configurationOptionWasSet_ = true;
}

// ____________________________________________________________________________
std::string_view ConfigOption::getIdentifier() const {
  return static_cast<std::string_view>(identifier_);
}

// ____________________________________________________________________________
std::string ConfigOption::contentOfAvailableTypesToString(
    const std::optional<AvailableTypes>& value) {
  if (!value.has_value()) {
    return "None";
  }

  // TODO<C++23> Use "deducing this" for simpler recursive lambdas.
  // Converts a `AvailableTypes` to their string representation.
  auto availableTypesToString = []<typename T>(const T& content,
                                               auto&& variantSubTypeToString) {
    // Return the internal value of the `std::optional`.
    if constexpr (std::is_same_v<T, std::string>) {
      // Add "", so that it's more obvious, that it's a string.
      return absl::StrCat("\"", content, "\"");
    } else if constexpr (std::is_same_v<T, bool>) {
      return content ? std::string{"true"} : std::string{"false"};
    } else if constexpr (std::is_arithmetic_v<T>) {
      return std::to_string(content);
    } else {
      // Is must be a vector.
      static_assert(ad_utility::isVector<T>);

      /*
      We have to use the EXPLICIT type, because a vector of bools uses a
      special 1 bit data type for it's entry references, instead of a bool. So
      an `auto` would land the recursive call right back in this else case.
      */
      using VectorEntryType = T::value_type;

      std::ostringstream stream;
      stream << "{";
      ad_utility::lazyStrJoin(
          &stream,
          std::views::transform(
              content,
              [&variantSubTypeToString](const VectorEntryType& entry) {
                return variantSubTypeToString(entry, variantSubTypeToString);
              }),
          ", ");
      stream << "}";
      return stream.str();
    }
  };

  return std::visit(
      [&availableTypesToString](const auto& v) {
        return availableTypesToString(v, availableTypesToString);
      },
      value.value());
}
// ____________________________________________________________________________
std::string ConfigOption::getValueAsString() const {
  // Reading an uninitialized value is never a good idea.
  AD_CONTRACT_CHECK(wasSet());

  return std::visit(
      [](const auto& d) {
        return contentOfAvailableTypesToString(*d.variablePointer_);
      },
      data_);
}

// ____________________________________________________________________________
nlohmann::json ConfigOption::getValueAsJson() const {
  // Reading an uninitialized value is never a good idea.
  AD_CONTRACT_CHECK(wasSet());

  return std::visit(
      [](const auto& d) { return nlohmann::json(*d.variablePointer_); }, data_);
}

// ____________________________________________________________________________
std::string ConfigOption::getDefaultValueAsString() const {
  return std::visit(
      [](const auto& d) {
        return d.defaultValue_.has_value()
                   ? contentOfAvailableTypesToString(d.defaultValue_.value())
                   : contentOfAvailableTypesToString(std::nullopt);
      },
      data_);
}

// ____________________________________________________________________________
nlohmann::json ConfigOption::getDefaultValueAsJson() const {
  return std::visit(
      [](const auto& d) {
        return d.defaultValue_.has_value()
                   ? nlohmann::json(d.defaultValue_.value())
                   : nlohmann::json(nlohmann::json::value_t::null);
      },
      data_);
}

// The dummy values.
template <typename DummyType>
static std::decay_t<DummyType> getDummyValue() {
  if constexpr (std::is_same_v<std::decay_t<DummyType>, bool>) {
    return false;
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>, std::string>) {
    return "Example string";
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>, int>) {
    return -42;
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>, size_t>) {
    return 42uL;
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>, float>) {
    return 4.2f;
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>,
                                      std::vector<bool>>) {
    return std::vector{true, false};
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>,
                                      std::vector<std::string>>) {
    return std::vector<std::string>{"Example", "string", "list"};
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>,
                                      std::vector<int>>) {
    return std::vector{40, -41, 42};
  } else if constexpr (std::is_same_v<std::decay_t<DummyType>,
                                      std::vector<size_t>>) {
    return std::vector{40uL, 41uL, 42uL};
  } else {
    // Must be a vector of floats.
    static_assert(std::is_same_v<std::decay_t<DummyType>, std::vector<float>>);
    return {40.0f, 41.1f, 42.2f};
  }
}

// ____________________________________________________________________________
nlohmann::json ConfigOption::getDummyValueAsJson() const {
  return std::visit(
      []<typename T>(const Data<T>&) {
        return nlohmann::json(getDummyValue<T>());
      },
      data_);
}

// ____________________________________________________________________________
std::string ConfigOption::getDummyValueAsString() const {
  return std::visit(
      /*
      We could directly return a string, but by converting a value, we don't
      have to keep an eye on how the class represents it's values as strings.
      */
      []<typename T>(const Data<T>&) {
        return contentOfAvailableTypesToString(getDummyValue<T>());
      },
      data_);
}

// ____________________________________________________________________________
ConfigOption::operator std::string() const {
  return absl::StrCat(
      "Configuration option '", identifier_, "'\n",
      ad_utility::addIndentation(
          absl::StrCat(
              "Value type: ", getActualValueTypeAsString(), "\nDefault value: ",
              getDefaultValueAsString(), "\nCurrently held value: ",
              wasSet() ? getValueAsString() : "value was never initialized",
              "\nDescription: ", description_),
          "    "));
}

// ____________________________________________________________________________
std::string ConfigOption::getActualValueTypeAsString() const {
  return std::visit(
      []<typename T>(const Data<T>&) { return availableTypesToString<T>(); },
      data_);
}
}  // namespace ad_utility
