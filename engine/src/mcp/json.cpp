// json.cpp: clean-room strict JSON parser + canonical serializer.
// Written from the JSON specification; no code copied from any existing
// implementation.
#include "litholattice/mcp/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace litholattice::mcp {
namespace {

// ---------- UTF-8 helpers ----------

// Encodes code point cp (validated) as UTF-8, appended to out.
void encodeUtf8(std::uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Validates that [begin, end) is well-formed UTF-8 (strict: no overlongs, no
// surrogates, max U+10FFFF). Used on raw string bytes and full serialized
// strings.
bool isValidUtf8(const char* begin, const char* end) {
  const auto* p = reinterpret_cast<const unsigned char*>(begin);
  const auto* e = reinterpret_cast<const unsigned char*>(end);
  while (p < e) {
    unsigned char c = *p;
    std::uint32_t cp;
    int extra;
    if (c < 0x80) {
      ++p;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
      if (cp < 0x02) return false;  // overlong
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
      if (cp > 0x04) return false;  // > U+10FFFF
    } else {
      return false;
    }
    ++p;
    for (int i = 0; i < extra; ++i) {
      if (p >= e || (*p & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (*p & 0x3F);
      ++p;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // surrogate
    if (cp > 0x10FFFF) return false;
    if (extra == 2 && cp < 0x800) return false;   // overlong
    if (extra == 3 && cp < 0x10000) return false;  // overlong
  }
  return true;
}

// ---------- strict recursive-descent parser ----------

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  ParseResult run() {
    ParseResult r;
    skipWhitespace();
    if (pos_ >= text_.size()) {
      return fail("empty input");
    }
    JsonValue v = parseValue(0, &r);
    if (!r.ok) return r;
    skipWhitespace();
    if (pos_ != text_.size()) {
      return fail("trailing characters after JSON value");
    }
    r.ok = true;
    r.value = std::move(v);
    r.error.clear();
    return r;
  }

 private:
  ParseResult fail(std::string msg) {
    ParseResult r;
    r.ok = false;
    r.error = std::move(msg);
    r.offset = pos_;
    return r;
  }

  void skipWhitespace() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++pos_;
      else
        break;
    }
  }

  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

  JsonValue parseValue(std::size_t depth, ParseResult* r) {
    char c = peek();
    switch (c) {
      case '{':
        return parseObject(depth, r);
      case '[':
        return parseArray(depth, r);
      case '"':
        return parseString(r);
      case 't':
        return parseLiteral("true", JsonValue::boolean(true), r);
      case 'f':
        return parseLiteral("false", JsonValue::boolean(false), r);
      case 'n':
        return parseLiteral("null", JsonValue::null(), r);
      case '-':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        return parseNumber(r);
      default:
        *r = fail(std::string("unexpected character '") + (c ? c : '?') + "'");
        return JsonValue();
    }
  }

  JsonValue parseLiteral(std::string_view word, JsonValue value, ParseResult* r) {
    if (text_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      r->ok = true;
      return value;
    }
    *r = fail("invalid literal");
    return JsonValue();
  }

  JsonValue parseObject(std::size_t depth, ParseResult* r) {
    if (depth >= kMaxJsonDepth) {
      *r = fail("nesting depth exceeds limit");
      return JsonValue();
    }
    ++pos_;  // '{'
    JsonValue::Object obj;
    skipWhitespace();
    if (peek() == '}') {
      ++pos_;
      r->ok = true;
      return JsonValue::object(std::move(obj));
    }
    for (;;) {
      skipWhitespace();
      if (peek() != '"') {
        *r = fail("object key must be a string");
        return JsonValue();
      }
      JsonValue key = parseString(r);
      if (!r->ok) return JsonValue();
      skipWhitespace();
      if (peek() != ':') {
        *r = fail("expected ':' after object key");
        return JsonValue();
      }
      ++pos_;
      skipWhitespace();
      JsonValue val = parseValue(depth + 1, r);
      if (!r->ok) return JsonValue();
      obj.emplace_back(key.asString(), std::move(val));
      skipWhitespace();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        r->ok = true;
        return JsonValue::object(std::move(obj));
      }
      *r = fail("expected ',' or '}' in object");
      return JsonValue();
    }
  }

  JsonValue parseArray(std::size_t depth, ParseResult* r) {
    if (depth >= kMaxJsonDepth) {
      *r = fail("nesting depth exceeds limit");
      return JsonValue();
    }
    ++pos_;  // '['
    JsonValue::Array arr;
    skipWhitespace();
    if (peek() == ']') {
      ++pos_;
      r->ok = true;
      return JsonValue::array(std::move(arr));
    }
    for (;;) {
      skipWhitespace();
      JsonValue val = parseValue(depth + 1, r);
      if (!r->ok) return JsonValue();
      arr.push_back(std::move(val));
      skipWhitespace();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        r->ok = true;
        return JsonValue::array(std::move(arr));
      }
      *r = fail("expected ',' or ']' in array");
      return JsonValue();
    }
  }

  // Parses exactly 4 hex digits at pos_; advances past them.
  bool parseHex4(std::uint32_t* out, ParseResult* r) {
    if (pos_ + 4 > text_.size()) {
      *r = fail("truncated \\u escape");
      return false;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text_[pos_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<std::uint32_t>(c - 'A' + 10);
      else {
        *r = fail("invalid hex digit in \\u escape");
        return false;
      }
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  JsonValue parseString(ParseResult* r) {
    ++pos_;  // opening quote
    std::string out;
    while (pos_ < text_.size()) {
      unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        if (!isValidUtf8(out.data(), out.data() + out.size())) {
          *r = fail("invalid UTF-8 in string");
          return JsonValue();
        }
        r->ok = true;
        return JsonValue::string(std::move(out));
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) {
          *r = fail("truncated escape sequence");
          return JsonValue();
        }
        char e = text_[pos_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            std::uint32_t cp;
            if (!parseHex4(&cp, r)) return JsonValue();
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // high surrogate: must be followed by \uDC00-\uDFFF
              if (pos_ + 6 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                *r = fail("lone high surrogate in \\u escape");
                return JsonValue();
              }
              pos_ += 2;
              std::uint32_t lo;
              if (!parseHex4(&lo, r)) return JsonValue();
              if (lo < 0xDC00 || lo > 0xDFFF) {
                *r = fail("lone high surrogate in \\u escape");
                return JsonValue();
              }
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              *r = fail("lone low surrogate in \\u escape");
              return JsonValue();
            }
            encodeUtf8(cp, out);
            break;
          }
          default:
            *r = fail(std::string("invalid escape '\\") + e + "'");
            return JsonValue();
        }
        continue;
      }
      if (c < 0x20) {
        *r = fail("unescaped control character in string");
        return JsonValue();
      }
      // Raw byte (part of a UTF-8 sequence or ASCII): copy through; the
      // whole-string UTF-8 validation at the closing quote rejects bad bytes.
      out.push_back(static_cast<char>(c));
      ++pos_;
    }
    *r = fail("unterminated string");
    return JsonValue();
  }

  JsonValue parseNumber(ParseResult* r) {
    std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    // integer part
    if (pos_ >= text_.size()) {
      *r = fail("truncated number");
      return JsonValue();
    }
    char c = text_[pos_];
    if (c == '0') {
      ++pos_;
    } else if (c >= '1' && c <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    } else {
      *r = fail("invalid number");
      return JsonValue();
    }
    bool isDouble = false;
    // fraction
    if (pos_ < text_.size() && text_[pos_] == '.') {
      isDouble = true;
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        *r = fail("missing digits after decimal point");
        return JsonValue();
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    // exponent
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      isDouble = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        *r = fail("missing digits in exponent");
        return JsonValue();
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    std::string tok(text_.substr(start, pos_ - start));
    if (!isDouble) {
      errno = 0;
      char* endp = nullptr;
      long long v = std::strtoll(tok.c_str(), &endp, 10);
      if (errno == 0 && endp && *endp == '\0') {
        r->ok = true;
        return JsonValue::integer(static_cast<std::int64_t>(v));
      }
      // Integer-form number outside int64 range: demote to double
      // (documented; never silently wraps to a wrong integer).
    }
    char* endp = nullptr;
    errno = 0;
    double d = std::strtod(tok.c_str(), &endp);
    if (!endp || *endp != '\0' || errno == ERANGE) {
      *r = fail("number out of range");
      return JsonValue();
    }
    r->ok = true;
    return JsonValue::number(d);
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

// ---------- canonical serializer ----------

void serializeInto(const JsonValue& v, std::string& out);

void serializeString(std::string_view s, std::string& out) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void serializeDouble(double d, std::string& out) {
  if (!std::isfinite(d)) {
    throw std::invalid_argument("json: non-finite double is not JSON-serializable");
  }
  // %.17g guarantees round-trip for IEEE-754 doubles. Append ".0" to
  // integral spellings so a double always reparses as a double (type-stable
  // round-trip through the value model).
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  out += buf;
  if (std::strchr(buf, '.') == nullptr && std::strchr(buf, 'e') == nullptr &&
      std::strchr(buf, 'E') == nullptr) {
    out += ".0";
  }
}

void serializeInto(const JsonValue& v, std::string& out) {
  switch (v.type()) {
    case JsonValue::Type::kNull:
      out += "null";
      break;
    case JsonValue::Type::kBool:
      out += v.asBool() ? "true" : "false";
      break;
    case JsonValue::Type::kInt:
      out += std::to_string(v.asInt());
      break;
    case JsonValue::Type::kDouble:
      serializeDouble(v.asDouble(), out);
      break;
    case JsonValue::Type::kString:
      if (!isValidUtf8(v.asString().data(), v.asString().data() + v.asString().size())) {
        throw std::invalid_argument("json: string holds invalid UTF-8");
      }
      serializeString(v.asString(), out);
      break;
    case JsonValue::Type::kArray: {
      out.push_back('[');
      bool first = true;
      for (const auto& e : v.asArray()) {
        if (!first) out.push_back(',');
        first = false;
        serializeInto(e, out);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::Type::kObject: {
      out.push_back('{');
      bool first = true;
      for (const auto& [k, val] : v.asObject()) {
        if (!first) out.push_back(',');
        first = false;
        serializeString(k, out);
        out.push_back(':');
        serializeInto(val, out);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const {
  if (!isObject()) return nullptr;
  for (const auto& [k, v] : asObject()) {
    if (k == key) return &v;
  }
  return nullptr;
}

JsonValue& JsonValue::getOrInsert(std::string_view key) {
  if (!isObject()) {
    data_ = Object{};
  }
  for (auto& [k, v] : asObject()) {
    if (k == key) return v;
  }
  asObject().emplace_back(std::string(key), JsonValue::null());
  return asObject().back().second;
}

std::string JsonValue::serialize() const {
  std::string out;
  serializeInto(*this, out);
  return out;
}

ParseResult parseJson(std::string_view text) {
  return Parser(text).run();
}

}  // namespace litholattice::mcp
