#include "nix/util/json-utils.hh"
#include "nix/util/error.hh"
#include "nix/util/types.hh"
#include <nlohmann/json_fwd.hpp>
#include <iostream>
#include <optional>

namespace nix {

const nlohmann::json * get(const nlohmann::json & map, const std::string & key)
{
    auto i = map.find(key);
    if (i == map.end())
        return nullptr;
    return &*i;
}

nlohmann::json * get(nlohmann::json & map, const std::string & key)
{
    auto i = map.find(key);
    if (i == map.end())
        return nullptr;
    return &*i;
}

const nlohmann::json & valueAt(const nlohmann::json::object_t & map, const std::string & key)
{
    if (!map.contains(key))
        throw Error("Expected JSON object to contain key '%s' but it doesn't: %s", key, nlohmann::json(map).dump());

    return map.at(key);
}

std::optional<nlohmann::json> optionalValueAt(const nlohmann::json::object_t & map, const std::string & key)
{
    if (!map.contains(key))
        return std::nullopt;

    return std::optional{map.at(key)};
}

std::optional<nlohmann::json> nullableValueAt(const nlohmann::json::object_t & map, const std::string & key)
{
    auto value = valueAt(map, key);

    if (value.is_null())
        return std::nullopt;

    return std::optional{std::move(value)};
}

const nlohmann::json * getNullable(const nlohmann::json & value)
{
    return value.is_null() ? nullptr : &value;
}

/**
 * Ensure the type of a JSON object is what you expect, failing with a
 * ensure type if it isn't.
 *
 * Use before type conversions and element access to avoid ugly
 * exceptions, but only part of this module to define the other `get*`
 * functions. It is too cumbersome and easy to forget to expect regular
 * JSON code to use it directly.
 */
static const nlohmann::json & ensureType(const nlohmann::json & value, nlohmann::json::value_type expectedType)
{
    if (value.type() != expectedType)
        throw Error(
            "Expected JSON value to be of type '%s' but it is of type '%s': %s",
            nlohmann::json(expectedType).type_name(),
            value.type_name(),
            value.dump());

    return value;
}

const nlohmann::json::object_t & getObject(const nlohmann::json & value)
{
    return ensureType(value, nlohmann::json::value_t::object).get_ref<const nlohmann::json::object_t &>();
}

const nlohmann::json::array_t & getArray(const nlohmann::json & value)
{
    return ensureType(value, nlohmann::json::value_t::array).get_ref<const nlohmann::json::array_t &>();
}

const nlohmann::json::string_t & getString(const nlohmann::json & value)
{
    return ensureType(value, nlohmann::json::value_t::string).get_ref<const nlohmann::json::string_t &>();
}

const nlohmann::json::number_unsigned_t & getUnsigned(const nlohmann::json & value)
{
    if (auto ptr = value.get<const nlohmann::json::number_unsigned_t *>()) {
        return *ptr;
    }
    const char * typeName = value.type_name();
    if (typeName == nlohmann::json(0).type_name()) {
        typeName = value.is_number_float() ? "floating point number" : "signed integral number";
    }
    throw Error(
        "Expected JSON value to be an unsigned integral number but it is of type '%s': %s", typeName, value.dump());
}

const nlohmann::json::boolean_t & getBoolean(const nlohmann::json & value)
{
    return ensureType(value, nlohmann::json::value_t::boolean).get_ref<const nlohmann::json::boolean_t &>();
}

Strings getStringList(const nlohmann::json & value)
{
    auto & jsonArray = getArray(value);

    Strings stringList;

    for (const auto & elem : jsonArray)
        stringList.push_back(getString(elem));

    return stringList;
}

StringMap getStringMap(const nlohmann::json & value)
{
    auto & jsonObject = getObject(value);

    StringMap stringMap;

    for (const auto & [key, value] : jsonObject)
        stringMap[getString(key)] = getString(value);

    return stringMap;
}

StringSet getStringSet(const nlohmann::json & value)
{
    auto & jsonArray = getArray(value);

    StringSet stringSet;

    for (const auto & elem : jsonArray)
        stringSet.insert(getString(elem));

    return stringSet;
}
} // namespace nix
