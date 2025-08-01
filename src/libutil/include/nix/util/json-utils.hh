#pragma once
///@file

#include <nlohmann/json.hpp>
#include <list>

#include "nix/util/error.hh"
#include "nix/util/types.hh"

namespace nix {

enum struct ExperimentalFeature;

const nlohmann::json * get(const nlohmann::json & map, const std::string & key);

nlohmann::json * get(nlohmann::json & map, const std::string & key);

/**
 * Get the value of a json object at a key safely, failing with a nice
 * error if the key does not exist.
 *
 * Use instead of nlohmann::json::at() to avoid ugly exceptions.
 */
const nlohmann::json & valueAt(const nlohmann::json::object_t & map, const std::string & key);

std::optional<nlohmann::json> optionalValueAt(const nlohmann::json::object_t & value, const std::string & key);
std::optional<nlohmann::json> nullableValueAt(const nlohmann::json::object_t & value, const std::string & key);

/**
 * Downcast the json object, failing with a nice error if the conversion fails.
 * See https://json.nlohmann.me/features/types/
 */
const nlohmann::json * getNullable(const nlohmann::json & value);
const nlohmann::json::object_t & getObject(const nlohmann::json & value);
const nlohmann::json::array_t & getArray(const nlohmann::json & value);
const nlohmann::json::string_t & getString(const nlohmann::json & value);
const nlohmann::json::number_unsigned_t & getUnsigned(const nlohmann::json & value);

template<typename T>
auto getInteger(const nlohmann::json & value) -> std::enable_if_t<std::is_signed_v<T> && std::is_integral_v<T>, T>
{
    if (auto ptr = value.get_ptr<const nlohmann::json::number_unsigned_t *>()) {
        if (*ptr <= std::make_unsigned_t<T>(std::numeric_limits<T>::max())) {
            return *ptr;
        }
    } else if (auto ptr = value.get_ptr<const nlohmann::json::number_integer_t *>()) {
        if (*ptr >= std::numeric_limits<T>::min() && *ptr <= std::numeric_limits<T>::max()) {
            return *ptr;
        }
    } else {
        auto typeName = value.is_number_float() ? "floating point number" : value.type_name();
        throw Error("Expected JSON value to be an integral number but it is of type '%s': %s", typeName, value.dump());
    }
    throw Error("Out of range: JSON value '%s' cannot be casted to %d-bit integer", value.dump(), 8 * sizeof(T));
}

const nlohmann::json::boolean_t & getBoolean(const nlohmann::json & value);
Strings getStringList(const nlohmann::json & value);
StringMap getStringMap(const nlohmann::json & value);
StringSet getStringSet(const nlohmann::json & value);

/**
 * For `adl_serializer<std::optional<T>>` below, we need to track what
 * types are not already using `null`. Only for them can we use `null`
 * to represent `std::nullopt`.
 */
template<typename T>
struct json_avoids_null;

/**
 * Handle numbers in default impl
 */
template<typename T>
struct json_avoids_null : std::bool_constant<std::is_integral<T>::value>
{};

template<>
struct json_avoids_null<std::nullptr_t> : std::false_type
{};

template<>
struct json_avoids_null<bool> : std::true_type
{};

template<>
struct json_avoids_null<std::string> : std::true_type
{};

template<typename T>
struct json_avoids_null<std::vector<T>> : std::true_type
{};

template<typename T>
struct json_avoids_null<std::list<T>> : std::true_type
{};

template<typename T, typename Compare>
struct json_avoids_null<std::set<T, Compare>> : std::true_type
{};

template<typename K, typename V>
struct json_avoids_null<std::map<K, V>> : std::true_type
{};

/**
 * `ExperimentalFeature` is always rendered as a string.
 */
template<>
struct json_avoids_null<ExperimentalFeature> : std::true_type
{};

} // namespace nix

namespace nlohmann {

/**
 * This "instance" is widely requested, see
 * https://github.com/nlohmann/json/issues/1749, but momentum has stalled
 * out. Writing there here in Nix as a stop-gap.
 *
 * We need to make sure the underlying type does not use `null` for this to
 * round trip. We do that with a static assert.
 */
template<typename T>
struct adl_serializer<std::optional<T>>
{
    /**
     * @brief Convert a JSON type to an `optional<T>` treating
     *        `null` as `std::nullopt`.
     */
    static void from_json(const json & json, std::optional<T> & t)
    {
        static_assert(nix::json_avoids_null<T>::value, "null is already in use for underlying type's JSON");
        t = json.is_null() ? std::nullopt : std::make_optional(json.template get<T>());
    }

    /**
     *  @brief Convert an optional type to a JSON type  treating `std::nullopt`
     *         as `null`.
     */
    static void to_json(json & json, const std::optional<T> & t)
    {
        static_assert(nix::json_avoids_null<T>::value, "null is already in use for underlying type's JSON");
        if (t)
            json = *t;
        else
            json = nullptr;
    }
};

} // namespace nlohmann
