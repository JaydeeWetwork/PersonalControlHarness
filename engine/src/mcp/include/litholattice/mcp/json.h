// json.h: minimal clean-room JSON value model + strict parser + canonical
// serializer for the LithoLattice MCP stack (Phase 0: JSON-RPC core).
//
// Written clean-room from the JSON / JSON-RPC specifications; no code copied
// from any existing JSON implementation.
//
// Design decisions (locked):
//   - Objects are ORDERED (vector of key/value pairs, insertion order).
//     Rationale: deterministic canonical serialization and exact round-trip
//     of protocol documents; lookup is linear but MCP messages are small.
//   - Integers that fit in int64 are int64; anything with a fraction or
//     exponent is double. Integer-form numbers outside int64 range demote to
//     double (documented, no silent truncation to a wrong integer).
//   - Parser is strict and fail-fast (engine policy): malformed input is
//     rejected with an error, never partially accepted.
//   - Nesting depth cap: kMaxJsonDepth = 64 (arrays/objects). Deeper input is
//     rejected as malformed. 64 is far beyond any legitimate MCP document and
//     bounds recursion on the parse stack.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace litholattice::mcp {

/// Maximum nesting depth (arrays/objects) accepted by the parser.
constexpr std::size_t kMaxJsonDepth = 64;

/// JSON value. Variant over null, bool, int64, double, string, array, ordered
/// object. Copyable/movable; default-constructed value is null.
class JsonValue {
 public:
  enum class Type { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

  using Array = std::vector<JsonValue>;
  using ObjectEntry = std::pair<std::string, JsonValue>;
  /// Ordered object: entries in insertion order. Duplicate keys are allowed by
  /// the value model (parser keeps them in order); find() returns the first.
  using Object = std::vector<ObjectEntry>;

  JsonValue() = default;
  JsonValue(const JsonValue&) = default;
  JsonValue(JsonValue&&) noexcept = default;
  JsonValue& operator=(const JsonValue&) = default;
  JsonValue& operator=(JsonValue&&) noexcept = default;
  ~JsonValue() = default;

  static JsonValue null() { return JsonValue(); }
  static JsonValue boolean(bool b) {
    JsonValue v;
    v.data_ = b;
    return v;
  }
  static JsonValue integer(std::int64_t i) {
    JsonValue v;
    v.data_ = i;
    return v;
  }
  static JsonValue number(double d) {
    JsonValue v;
    v.data_ = d;
    return v;
  }
  static JsonValue string(std::string_view s) {
    JsonValue v;
    v.data_ = std::string(s);
    return v;
  }
  static JsonValue array(Array a) {
    JsonValue v;
    v.data_ = std::move(a);
    return v;
  }
  static JsonValue object(Object o) {
    JsonValue v;
    v.data_ = std::move(o);
    return v;
  }

  Type type() const {
    return static_cast<Type>(data_.index());
  }
  bool isNull() const { return type() == Type::kNull; }
  bool isBool() const { return type() == Type::kBool; }
  bool isInt() const { return type() == Type::kInt; }
  bool isDouble() const { return type() == Type::kDouble; }
  bool isNumber() const { return isInt() || isDouble(); }
  bool isString() const { return type() == Type::kString; }
  bool isArray() const { return type() == Type::kArray; }
  bool isObject() const { return type() == Type::kObject; }

  bool asBool() const { return std::get<bool>(data_); }
  std::int64_t asInt() const { return std::get<std::int64_t>(data_); }
  double asDouble() const { return std::get<double>(data_); }
  const std::string& asString() const { return std::get<std::string>(data_); }
  const Array& asArray() const { return std::get<Array>(data_); }
  Array& asArray() { return std::get<Array>(data_); }
  const Object& asObject() const { return std::get<Object>(data_); }
  Object& asObject() { return std::get<Object>(data_); }

  /// Object lookup by key; returns nullptr when absent or not an object.
  const JsonValue* find(std::string_view key) const;
  /// Mutable object lookup; inserts a null entry when absent.
  JsonValue& getOrInsert(std::string_view key);

  /// Canonical serialization: minimal (no whitespace), exact round-trip for
  /// the value model (parse(serialize(v)) compares equal to v; and for
  /// canonical input text, serialize(parse(text)) == text). Doubles use the
  /// shortest round-trip representation. Throws std::invalid_argument for
  /// non-finite doubles (not representable in JSON) and for strings holding
  /// invalid UTF-8.
  std::string serialize() const;

  bool operator==(const JsonValue& other) const { return data_ == other.data_; }
  bool operator!=(const JsonValue& other) const { return !(*this == other); }

 private:
  // variant index matches Type order: null, bool, int, double, string, array, object
  std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> data_;
};

/// Result of strict parsing. ok == false means the input was rejected
/// fail-fast; value is null and error/offset describe where and why.
struct ParseResult {
  bool ok = false;
  JsonValue value;
  std::string error;
  std::size_t offset = 0;  // byte offset of the failure (or of trailing garbage)
};

/// Strict JSON parse. Rejects (fail-fast): truncated input, trailing garbage,
/// bad escapes, lone surrogates, invalid \uXXXX, raw control characters in
/// strings, invalid UTF-8, malformed numbers (leading zeros, missing digits,
/// "+1", ".5", "1.", "NaN", "Infinity"...), duplicate acceptance is not an
/// issue (kept in order), and nesting deeper than kMaxJsonDepth.
ParseResult parseJson(std::string_view text);

}  // namespace litholattice::mcp
