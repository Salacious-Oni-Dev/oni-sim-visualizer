// Live attach: read a RUNNING game's grid over HTTP instead of driving SimDLL offline.
//
// Milestone 3. The other mode in this program owns the simulation -- it binds SimDLL,
// replays a corpus and steps it. This one owns nothing: `oni-framework-api`'s
// `DebugInspectorServer` is already inside Oxygen Not Included, holding the sim's own
// array pointers, and its bulk routes hand those arrays out as raw binary. So the whole
// of attach mode is a socket, a decoder, and a `GameDataUpdate` pointed at the bytes
// that came back.
//
// WHY THAT IS THE ENTIRE JOB. Every view, the probe, the stats, the digest and the hover
// readout in this program are pure functions of `const GameDataUpdate*` plus the element
// table and the world size. `GameDataUpdate`'s pointer members are, one for one, the
// eighteen surfaces `/grid` publishes. Filling owned buffers and pointing a synthesized
// update at them therefore reaches every one of those consumers with no change to any of
// them. No renderer, no view and no readout knows which mode it is in.
//
// THE SYNTHESIZED ELEMENT TABLE, and why it does not contradict session.h. That file
// records -- correctly -- that a hand-built element table is not an option, because a
// synthetic one makes `PrepareGameData` fault with a read at -1. That warning is about
// DRIVING the sim. Attach mode never sends the sim a message and never calls
// `PrepareGameData`; the table built here is read-only decoration for a picture, used
// only for `Element.colour`, `state` and `id`. It must never be handed to a SimDLL.
//
// THREE FIELDS THIS NEEDS THAT `/elements` DID NOT ORIGINALLY CARRY -- `idHash`,
// `colour`, `simState` -- were added to `DebugBulkRoutes.Elements()` for this client.
// `simState` is not a duplicate of `stateRaw`: the sim's state byte is the managed one
// with Unstable (8) ORed in from a TAG rather than from the enum,
// so `stateRaw` alone reports no unstable solid anywhere, sand included. An older
// OniFramework is detected and refused rather than silently drawn in wrong colours.

#pragma once

// Before <windows.h>, always. windows.h pulls in the original winsock.h unless
// _WINSOCKAPI_ is already defined, and winsock2.h defines it -- including them the other
// way round is the classic redefinition wall. main.cpp includes this header first for
// exactly this reason.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "extattrs.h"
#include "out.h"
#include "abi/sim_abi_ext.h"  // oni-sim-replacement root, on the include path
#include "simhost.h"          // oni-sim-replacement/driver/src, on the include path

namespace simviz {
namespace attach {

// ------------------------------------------------------------------ tiny JSON reader
//
// A real parser rather than a key scanner. `/elements` is ~700 nested objects and a
// scanner that walked it by looking for `"colour":` would mis-read the first time a
// value contained a brace or a field was added -- and it would do so silently, which for
// an element table means a picture in the wrong colours rather than an error.
namespace json {

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> fields;

  const Value* Find(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& f : fields) {
      if (f.first == key) return &f.second;
    }
    return nullptr;
  }
  bool Has(const char* key) const { return Find(key) != nullptr; }
  double Number(const char* key, double dflt) const {
    const Value* v = Find(key);
    return (v && v->type == Type::Number) ? v->number : dflt;
  }
  std::string Text(const char* key, const char* dflt) const {
    const Value* v = Find(key);
    return (v && v->type == Type::String) ? v->text : std::string(dflt);
  }
};

class Parser {
 public:
  Parser(const char* p, size_t n) : p_(p), end_(p + n) {}

  bool Parse(Value* out, std::string* err) {
    Skip();
    if (!ParseValue(out, 0, err)) return false;
    Skip();
    if (p_ != end_) {
      *err = "trailing bytes after the top-level value";
      return false;
    }
    return true;
  }

 private:
  // The documents are ours and shallow (object -> array -> object), but a cap costs
  // nothing and turns a malformed reply into an error instead of a stack overflow.
  static const int kMaxDepth = 32;

  void Skip() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }
  bool Eat(char c) {
    if (p_ < end_ && *p_ == c) {
      ++p_;
      return true;
    }
    return false;
  }
  bool Literal(const char* word) {
    const size_t n = strlen(word);
    if (static_cast<size_t>(end_ - p_) < n || memcmp(p_, word, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool ParseValue(Value* out, int depth, std::string* err) {
    if (depth > kMaxDepth) {
      *err = "nested deeper than this reader accepts";
      return false;
    }
    if (p_ >= end_) {
      *err = "value expected, input ended";
      return false;
    }
    switch (*p_) {
      case '{': return ParseObject(out, depth, err);
      case '[': return ParseArray(out, depth, err);
      case '"':
        out->type = Value::Type::String;
        return ParseString(&out->text, err);
      case 't':
        if (!Literal("true")) { *err = "malformed literal"; return false; }
        out->type = Value::Type::Bool;
        out->boolean = true;
        return true;
      case 'f':
        if (!Literal("false")) { *err = "malformed literal"; return false; }
        out->type = Value::Type::Bool;
        out->boolean = false;
        return true;
      case 'n':
        if (!Literal("null")) { *err = "malformed literal"; return false; }
        out->type = Value::Type::Null;
        return true;
      default: return ParseNumber(out, err);
    }
  }

  bool ParseObject(Value* out, int depth, std::string* err) {
    out->type = Value::Type::Object;
    ++p_;  // '{'
    Skip();
    if (Eat('}')) return true;
    for (;;) {
      Skip();
      std::string key;
      if (p_ >= end_ || *p_ != '"') {
        *err = "object key expected";
        return false;
      }
      if (!ParseString(&key, err)) return false;
      Skip();
      if (!Eat(':')) {
        *err = "':' expected after an object key";
        return false;
      }
      Skip();
      Value v;
      if (!ParseValue(&v, depth + 1, err)) return false;
      out->fields.emplace_back(std::move(key), std::move(v));
      Skip();
      if (Eat(',')) continue;
      if (Eat('}')) return true;
      *err = "',' or '}' expected in an object";
      return false;
    }
  }

  bool ParseArray(Value* out, int depth, std::string* err) {
    out->type = Value::Type::Array;
    ++p_;  // '['
    Skip();
    if (Eat(']')) return true;
    for (;;) {
      Skip();
      Value v;
      if (!ParseValue(&v, depth + 1, err)) return false;
      out->items.push_back(std::move(v));
      Skip();
      if (Eat(',')) continue;
      if (Eat(']')) return true;
      *err = "',' or ']' expected in an array";
      return false;
    }
  }

  bool ParseString(std::string* out, std::string* err) {
    out->clear();
    if (!Eat('"')) {
      *err = "string expected";
      return false;
    }
    while (p_ < end_) {
      const char c = *p_++;
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (p_ >= end_) break;
      const char e = *p_++;
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          if (end_ - p_ < 4) {
            *err = "truncated \\u escape";
            return false;
          }
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = p_[i];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else {
              *err = "bad hex digit in a \\u escape";
              return false;
            }
          }
          p_ += 4;
          // UTF-8, and surrogate halves are passed through as the replacement
          // character: nothing this client reads is outside the BMP, and an element
          // name is displayed, never compared byte for byte.
          if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
          if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default:
          *err = "unknown escape in a string";
          return false;
      }
    }
    *err = "string was not closed";
    return false;
  }

  bool ParseNumber(Value* out, std::string* err) {
    char* stop = nullptr;
    // strtod stops at the first byte that cannot continue a number, and every document
    // this reads is NUL-terminated by the caller (the body is copied into a std::string),
    // so it cannot run off the end.
    const double d = strtod(p_, &stop);
    if (stop == p_) {
      *err = "number expected";
      return false;
    }
    p_ = stop;
    out->type = Value::Type::Number;
    out->number = d;
    return true;
  }

  const char* p_;
  const char* end_;
};

inline bool Parse(const std::string& text, Value* out, std::string* err) {
  Parser parser(text.c_str(), text.size());
  return parser.Parse(out, err);
}

}  // namespace json

// ------------------------------------------------------------------------ HTTP client
//
// One connection per request. `DebugInspectorServer` answers with `Connection: close`
// and a `Content-Length`, so there is no keep-alive to manage and no chunked encoding to
// decode. At a poll rate measured in frames per second, the connect is not the cost --
// the body is.

// WSAStartup exactly once for the process. Winsock refcounts, but pairing every request
// with its own startup/cleanup would tear the stack down between polls.
class Winsock {
 public:
  static bool Ensure(std::string* err) {
    static Winsock instance;
    if (!instance.up_) *err = instance.err_;
    return instance.up_;
  }

 private:
  Winsock() {
    WSADATA data;
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      char buf[96];
      snprintf(buf, sizeof(buf), "WSAStartup failed (%d)", rc);
      err_ = buf;
      return;
    }
    up_ = true;
  }
  ~Winsock() {
    if (up_) WSACleanup();
  }
  Winsock(const Winsock&) = delete;
  Winsock& operator=(const Winsock&) = delete;

  bool up_ = false;
  std::string err_;
};

inline std::string SocketError(const char* what) {
  char buf[160];
  snprintf(buf, sizeof(buf), "%s failed (WSA %d)", what, WSAGetLastError());
  return buf;
}

// GET one path. `body` is replaced on success; `content_type` is the header verbatim,
// which is how a caller tells a binary reply from the JSON error the server sends in its
// place when a route refuses.
inline bool HttpGet(const std::string& host, const std::string& port,
                    const std::string& path, int timeout_ms, std::vector<uint8_t>* body,
                    std::string* content_type, int* status, std::string* err) {
  body->clear();
  content_type->clear();
  *status = 0;
  if (!Winsock::Ensure(err)) return false;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* found = nullptr;
  const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &found);
  if (rc != 0 || !found) {
    char buf[192];
    snprintf(buf, sizeof(buf), "cannot resolve %s:%s (getaddrinfo %d)", host.c_str(),
             port.c_str(), rc);
    *err = buf;
    if (found) freeaddrinfo(found);
    return false;
  }

  SOCKET s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  if (s == INVALID_SOCKET) {
    *err = SocketError("socket");
    freeaddrinfo(found);
    return false;
  }
  // A blocking connect to a host that is not listening takes ~20 s to fail on Windows,
  // which in a poll loop reads as a hang. SO_RCVTIMEO does not cover connect, so the
  // socket goes non-blocking for that one call and back to blocking for the transfer.
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
  int cr = connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen));
  if (cr == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(s, &writable);
    fd_set failed;
    FD_ZERO(&failed);
    FD_SET(s, &failed);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(0, nullptr, &writable, &failed, &tv);
    if (ready <= 0 || FD_ISSET(s, &failed)) {
      char buf[192];
      snprintf(buf, sizeof(buf), "cannot connect to %s:%s (%s)", host.c_str(),
               port.c_str(), ready == 0 ? "timed out" : "refused");
      *err = buf;
      closesocket(s);
      freeaddrinfo(found);
      return false;
    }
    cr = 0;
  }
  freeaddrinfo(found);
  if (cr == SOCKET_ERROR) {
    *err = SocketError("connect");
    closesocket(s);
    return false;
  }
  nonblocking = 0;
  ioctlsocket(s, FIONBIO, &nonblocking);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));

  std::string request = "GET " + path +
                        " HTTP/1.1\r\n"
                        "Host: " +
                        host + ":" + port +
                        "\r\n"
                        "User-Agent: oni-sim-visualizer\r\n"
                        "Connection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < request.size()) {
    const int n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
    if (n <= 0) {
      *err = SocketError("send");
      closesocket(s);
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  // Read headers and body together: the split is found in what arrives, not asked for
  // separately, because a small reply arrives in one packet with both.
  std::vector<uint8_t> raw;
  char chunk[65536];
  size_t header_end = std::string::npos;
  long long content_length = -1;
  for (;;) {
    if (header_end != std::string::npos && content_length >= 0 &&
        raw.size() >= header_end + static_cast<size_t>(content_length)) {
      break;
    }
    const int n = recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
    if (n == 0) break;  // clean close: with Connection: close this is a valid end
    if (n < 0) {
      *err = SocketError("recv");
      closesocket(s);
      return false;
    }
    raw.insert(raw.end(), chunk, chunk + n);
    if (header_end == std::string::npos) {
      for (size_t i = 3; i < raw.size(); ++i) {
        if (raw[i - 3] == '\r' && raw[i - 2] == '\n' && raw[i - 1] == '\r' &&
            raw[i] == '\n') {
          header_end = i + 1;
          break;
        }
      }
      if (header_end != std::string::npos) {
        const std::string head(reinterpret_cast<const char*>(raw.data()), header_end);
        // "HTTP/1.1 200 OK"
        if (head.size() > 12) *status = atoi(head.c_str() + 9);
        // Header names are compared case-insensitively because that is what the spec
        // says, even though this one server always writes them the same way.
        size_t line = head.find("\r\n");
        while (line != std::string::npos && line + 2 < head.size()) {
          const size_t start = line + 2;
          const size_t stop = head.find("\r\n", start);
          if (stop == std::string::npos || stop == start) break;
          std::string name = head.substr(start, head.find(':', start) - start);
          const size_t colon = head.find(':', start);
          std::string value =
              (colon == std::string::npos || colon > stop)
                  ? std::string()
                  : head.substr(colon + 1, stop - colon - 1);
          while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
          }
          for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
          }
          if (name == "content-length") content_length = atoll(value.c_str());
          else if (name == "content-type") *content_type = value;
          line = stop;
        }
      }
    }
  }
  closesocket(s);

  if (header_end == std::string::npos) {
    *err = "the reply had no header terminator";
    return false;
  }
  const size_t available = raw.size() - header_end;
  if (content_length >= 0 && available < static_cast<size_t>(content_length)) {
    char buf[160];
    snprintf(buf, sizeof(buf), "reply truncated: %zu of %lld body bytes arrived",
             available, content_length);
    *err = buf;
    return false;
  }
  const size_t take =
      content_length >= 0 ? static_cast<size_t>(content_length) : available;
  body->assign(raw.begin() + static_cast<long>(header_end),
               raw.begin() + static_cast<long>(header_end + take));
  return true;
}

// ----------------------------------------------------------------------- the surfaces
//
// The wire ids, which mirror `DebugBulkRoutes.Surfaces` exactly. They are format, not
// enumeration order: append, never renumber. The name is what goes in `/grid?arrays=`.
enum SurfaceId : uint32_t {
  kElementIdx = 1,
  kTemperature = 2,
  kMass = 3,
  kRadiation = 4,
  kProperties = 5,
  kInsulation = 6,
  kStrengthInfo = 7,
  kDiseaseIdx = 8,
  kDiseaseCount = 9,
  kBackwallElement = 10,
  kBackwallMass = 11,
  kBackwallTemperature = 12,
  kAccumulatedFlow = 13,
  kTextureFlow = 20,
  kTextureLiquid = 21,
  kTextureLiquidData = 22,
  kTextureMaterialData = 23,
  kTextureSunlight = 24,
};

struct SurfaceInfo {
  uint32_t id;
  const char* name;
  uint32_t bytes_per_cell;
};

inline const SurfaceInfo* Surfaces(size_t* count) {
  static const SurfaceInfo table[] = {
      {kElementIdx, "elementIdx", 2},
      {kTemperature, "temperature", 4},
      {kMass, "mass", 4},
      {kRadiation, "radiation", 4},
      {kProperties, "properties", 1},
      {kInsulation, "insulation", 1},
      {kStrengthInfo, "strengthInfo", 1},
      {kDiseaseIdx, "diseaseIdx", 1},
      {kDiseaseCount, "diseaseCount", 4},
      {kBackwallElement, "backwallElement", 2},
      {kBackwallMass, "backwallMass", 4},
      {kBackwallTemperature, "backwallTemperature", 4},
      {kAccumulatedFlow, "accumulatedFlow", 4},
      {kTextureFlow, "propertyTextureFlow", 8},
      {kTextureLiquid, "propertyTextureLiquid", 4},
      {kTextureLiquidData, "propertyTextureLiquidData", 4},
      {kTextureMaterialData, "propertyTextureMaterialData", 4},
      {kTextureSunlight, "propertyTextureExposedToSunlight", 1},
  };
  *count = sizeof(table) / sizeof(table[0]);
  return table;
}

inline const SurfaceInfo* SurfaceById(uint32_t id) {
  size_t n = 0;
  const SurfaceInfo* t = Surfaces(&n);
  for (size_t i = 0; i < n; ++i) {
    if (t[i].id == id) return &t[i];
  }
  return nullptr;
}

inline const SurfaceInfo* SurfaceByName(const std::string& name) {
  size_t n = 0;
  const SurfaceInfo* t = Surfaces(&n);
  for (size_t i = 0; i < n; ++i) {
    if (name == t[i].name) return &t[i];
  }
  return nullptr;
}

// An ordered, deduplicated request set. A std::vector rather than a std::set because the
// ORDER is the wire order: `/grid` fills its entry table in the order the query names,
// and reproducing a fetch means reproducing that.
class SurfaceSet {
 public:
  void Add(uint32_t id) {
    for (uint32_t have : ids_) {
      if (have == id) return;
    }
    ids_.push_back(id);
  }
  void AddAll() {
    size_t n = 0;
    const SurfaceInfo* t = Surfaces(&n);
    for (size_t i = 0; i < n; ++i) Add(t[i].id);
  }
  bool empty() const { return ids_.empty(); }
  size_t size() const { return ids_.size(); }
  const std::vector<uint32_t>& ids() const { return ids_; }
  bool Contains(uint32_t id) const {
    for (uint32_t have : ids_) {
      if (have == id) return true;
    }
    return false;
  }
  bool operator==(const SurfaceSet& other) const { return ids_ == other.ids_; }
  bool operator!=(const SurfaceSet& other) const { return ids_ != other.ids_; }

  std::string Query() const {
    std::string q;
    for (size_t i = 0; i < ids_.size(); ++i) {
      const SurfaceInfo* s = SurfaceById(ids_[i]);
      if (!s) continue;
      if (!q.empty()) q += ",";
      q += s->name;
    }
    return q;
  }

  // Parse a comma-separated list of surface names, or the word "all". Returns false and
  // names the offender on an unknown one rather than quietly fetching less than asked.
  bool ParseList(const std::string& list, std::string* err) {
    if (list == "all") {
      AddAll();
      return true;
    }
    size_t at = 0;
    while (at <= list.size()) {
      const size_t comma = list.find(',', at);
      const std::string name =
          list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!name.empty()) {
        const SurfaceInfo* s = SurfaceByName(name);
        if (!s) {
          *err = "unknown array '" + name + "'";
          return false;
        }
        Add(s->id);
      }
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
    return !empty();
  }

 private:
  std::vector<uint32_t> ids_;
};

// --------------------------------------------------------------------- what /world says
struct WorldInfo {
  int id = 0;
  std::string name;
  int offset_x = 0, offset_y = 0;
  int width = 0, height = 0;
};

// ------------------------------------------------------------------------- the source
//
// Owns the buffers, the synthesized `GameDataUpdate` that points at them, and the
// element table lifted from `/elements`. Its `frame()` is a drop-in for the one a
// Session gets back from `PrepareGameData`.
class Source {
 public:
  // "host:port", "host" (9788 assumed), or a bare port. WSL reaches a game on Windows
  // through the vEthernet gateway, not through 127.0.0.1, which is why this takes a host
  // at all rather than assuming loopback.
  static bool ParseEndpoint(const std::string& endpoint, std::string* host,
                            std::string* port) {
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
      // All digits means a port on loopback; anything else is a host on the default port.
      bool digits = !endpoint.empty();
      for (char c : endpoint) {
        if (c < '0' || c > '9') digits = false;
      }
      if (digits) {
        *host = "127.0.0.1";
        *port = endpoint;
      } else {
        *host = endpoint;
        *port = "9788";
      }
      return !host->empty() && !port->empty();
    }
    *host = endpoint.substr(0, colon);
    *port = endpoint.substr(colon + 1);
    if (host->empty()) *host = "127.0.0.1";
    return !port->empty();
  }

  // Fetch /world and /elements. No grid yet -- the first Poll does that, because the
  // surface set depends on the view, which main decides after this returns.
  bool Open(const std::string& endpoint, int timeout_ms, std::string* err) {
    if (!ParseEndpoint(endpoint, &host_, &port_)) {
      *err = "cannot read '" + endpoint + "' as host:port";
      return false;
    }
    timeout_ms_ = timeout_ms;
    if (!FetchWorld(err)) return false;
    if (!FetchElements(err)) return false;
    open_ = true;
    return true;
  }

  // One frame. Refetches `/grid` for `want`, decodes it, and re-points the update.
  //
  // The world size is re-checked against every reply, not just the first: a running game
  // can load a different save under the client, and every pointer here is sized from the
  // cell count. A changed size is reported as an error rather than followed, because the
  // camera, the range and the render target upstream are all sized from the old one.
  bool Poll(const SurfaceSet& want, std::string* err) {
    if (!open_) {
      *err = "not attached";
      return false;
    }
    if (want.empty()) {
      *err = "no arrays requested";
      return false;
    }
    const std::string path = "/grid?arrays=" + want.Query();
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    if (!HttpGet(host_, port_, path, timeout_ms_, &body, &content_type, &status, err)) {
      return false;
    }
    if (status != 200) {
      *err = Describe(status, content_type, body);
      return false;
    }
    if (content_type.find("application/octet-stream") == std::string::npos) {
      *err = "/grid answered " + content_type + ": " + Describe(status, content_type, body);
      return false;
    }
    return Decode(std::move(body), err);
  }

  // The per-cell extension registry, as the sim's own self-describing blob -- `/extcells`,
  // whose body is exactly what `SIM_DebugExtCellStateAll` produced with nothing wrapped
  // around it. Decoded by `extprops::Decode`, which is the sim's own decoder rather than a
  // second parser, so nothing here needs to know what the format is.
  //
  // NOT part of `Poll`, deliberately. `/grid` is a memcpy out of pointers the game already
  // holds; this one makes the game wait for the sim thread to go idle and serialise every
  // written property, which is about 13 MB on a world with an active gas mixture. It is
  // fetched when something is going to read it and not once per frame out of habit.
  //
  // An OniFramework older than the route answers 200 with a JSON error rather than a 404 --
  // `Route` has no other shape -- so this checks the content type and reports the body.
  // "unknown route: /extcells" is the message that says which half to rebuild.
  bool FetchExtCells(std::vector<uint8_t>* blob, std::string* err) const {
    blob->clear();
    if (!open_) {
      *err = "not attached";
      return false;
    }
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    if (!HttpGet(host_, port_, "/extcells", timeout_ms_, &body, &content_type, &status,
                 err)) {
      return false;
    }
    if (status != 200) {
      *err = "/extcells " + Describe(status, content_type, body);
      return false;
    }
    if (content_type.find("application/octet-stream") == std::string::npos) {
      *err = "/extcells answered " + content_type + ": " +
             Describe(status, content_type, body);
      return false;
    }
    *blob = std::move(body);
    return true;
  }

  // The same registry off the PUBLISHED path -- `/extcells?published=1`, which serves the
  // identical ONIE blob built from the per-frame descriptor table instead of from
  // `SIM_DebugExtCellStateAll`. Same bytes, same decoder, three differences that matter:
  //
  //   * no worker barrier at the far end, which is what the publish table exists for;
  //   * `props=` asks for a SUBSET, where the inspection route is all-or-nothing;
  //   * a subscribed property appears whatever its contents, so "registered but untouched"
  //     stops arriving identically to "never registered" -- the inspection blob omits a
  //     property whose every cell is still at its default.
  //
  // ASKING SUBSCRIBES. Publishing is opt-in per property because it costs the game a copy
  // every frame, so a property nobody has asked for is not in any frame yet. The route
  // answers such a request with JSON naming what it just subscribed, and the data exists one
  // tick later -- `kPublishCellProperty` is queued. That is reported through `pending` rather
  // than as an error, because it is the normal first answer to a first request and a caller
  // that treats it as a failure will never get a second one.
  //
  // `props` is a comma-separated list, or empty for "whatever is published right now", which
  // subscribes nothing.
  bool FetchPublishedExtCells(const std::string& props, std::vector<uint8_t>* blob,
                              bool* pending, std::string* err) const {
    blob->clear();
    if (pending != nullptr) *pending = false;
    if (!open_) {
      *err = "not attached";
      return false;
    }
    std::string path = "/extcells?published=1";
    if (!props.empty()) path += "&props=" + props;
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    if (!HttpGet(host_, port_, path, timeout_ms_, &body, &content_type, &status, err)) {
      return false;
    }
    if (status != 200) {
      *err = path + " " + Describe(status, content_type, body);
      return false;
    }
    if (content_type.find("application/octet-stream") == std::string::npos) {
      // JSON here is either the subscribe-and-retry answer or a real refusal, and telling
      // them apart is the caller's whole reason for passing `pending`. Matched on the key the
      // route sends rather than parsed: this client has no JSON reader, and adding one to
      // read two fields would be a worse trade than the substring.
      const std::string text(body.begin(), body.end());
      if (pending != nullptr && text.find("\"subscribed\"") != std::string::npos &&
          text.find("\"retry\"") != std::string::npos) {
        *pending = true;
        *err = "subscribed; the sim publishes it on the next tick";
        return false;
      }
      *err = path + " answered " + content_type + ": " + Describe(status, content_type, body);
      return false;
    }
    *blob = std::move(body);
    return true;
  }

  // `/extevents`. Same three-way answer as the published-cells fetch above -- bytes, a
  // subscribe-and-retry, or a refusal -- with one difference worth naming: a successful reply
  // that carries ZERO batches is the normal case, not an empty result to be reported as a
  // failure. It means the game had nothing to say since the last drain, which for a stream
  // whose subject is refused messages is the answer you want most of the time.
  bool FetchExtEvents(const std::string& streams, std::vector<uint8_t>* blob, bool* pending,
                      std::string* err) const {
    blob->clear();
    if (pending != nullptr) *pending = false;
    if (!open_) {
      *err = "not attached";
      return false;
    }
    std::string path = "/extevents";
    if (!streams.empty()) path += "?streams=" + streams;
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    if (!HttpGet(host_, port_, path, timeout_ms_, &body, &content_type, &status, err)) {
      return false;
    }
    if (status != 200) {
      *err = path + " " + Describe(status, content_type, body);
      return false;
    }
    if (content_type.find("application/octet-stream") == std::string::npos) {
      const std::string text(body.begin(), body.end());
      if (pending != nullptr && text.find("\"subscribed\"") != std::string::npos &&
          text.find("\"retry\"") != std::string::npos) {
        *pending = true;
        *err = "subscribed; the sim collects it from the next tick";
        return false;
      }
      *err = path + " answered " + content_type + ": " + Describe(status, content_type, body);
      return false;
    }
    *blob = std::move(body);
    return true;
  }

  // The discovery half, and the reason it is separate: a bare `/extevents` subscribes nothing
  // and answers JSON naming what the SimDLL declares. This client has no JSON reader, so it
  // hands the text back whole and lets the caller print it -- which is all `--events-list`
  // needs, and is honest about the fact that nothing here parsed it.
  bool FetchExtEventStreams(std::string* json, std::string* err) const {
    json->clear();
    if (!open_) {
      *err = "not attached";
      return false;
    }
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    if (!HttpGet(host_, port_, "/extevents", timeout_ms_, &body, &content_type, &status, err)) {
      return false;
    }
    if (status != 200) {
      *err = std::string("/extevents ") + Describe(status, content_type, body);
      return false;
    }
    json->assign(body.begin(), body.end());
    return true;
  }

  // `/extelements` -- registry 2, and the only one of the three that arrives as JSON rather
  // than as a blob the sim serialised. The registry is sparse and capped at 64 attributes, so
  // the whole of it is a few kilobytes of text; a binary format here would be a fourth wire
  // format to keep two writers agreeing on, bought with nothing.
  //
  // `with_values` false asks the route for descriptors only (`values=0`). Nothing in this
  // program needs that yet -- the values are a handful of floats -- but the route offers it
  // and a caller listing a registry somebody has pushed thousands of keys into will want it.
  bool FetchElementAttributes(bool with_values, extattrs::Table* out, std::string* err) const {
    *out = extattrs::Table();
    out->source = "/extelements";
    if (!open_) {
      *err = "not attached";
      return false;
    }
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    const char* path = with_values ? "/extelements" : "/extelements?values=0";
    if (!HttpGet(host_, port_, path, timeout_ms_, &body, &content_type, &status, err)) {
      return false;
    }
    if (status != 200) {
      *err = std::string("/extelements ") + Describe(status, content_type, body);
      return false;
    }
    std::string text(body.begin(), body.end());
    json::Value doc;
    if (!json::Parse(text, &doc, err)) {
      *err = "/extelements: " + *err;
      return false;
    }
    // A 200 carrying `error` is the route saying it cannot enumerate -- a stock SimDLL, one
    // older than stage 5b, or no sim yet. That is an answer, not a transport failure, so it
    // lands in the table's own error rather than failing the fetch.
    if (doc.Has("error")) {
      out->error = doc.Text("error", "");
      return true;
    }

    out->exported = true;
    const json::Value* with = doc.Find("withValues");
    out->with_values = with != nullptr && with->type == json::Value::Type::Bool
                           ? with->boolean
                           : with_values;
    const json::Value* rows = doc.Find("attributes");
    if (rows == nullptr || rows->type != json::Value::Type::Array) {
      out->error = "/extelements answered without an attributes array";
      return true;
    }
    for (const json::Value& row : rows->items) {
      extattrs::Attribute a;
      a.idx = static_cast<int32_t>(row.Number("idx", 0));
      a.name = row.Text("name", "");
      a.type = static_cast<int32_t>(row.Number("type", 0));
      a.arity = static_cast<int32_t>(row.Number("arity", 1));
      a.stride = static_cast<int32_t>(row.Number("stride", 4));
      a.value_count = static_cast<int32_t>(row.Number("valueCount", 0));
      const json::Value* fp = row.Find("firstParty");
      a.first_party = fp != nullptr && fp->type == json::Value::Type::Bool && fp->boolean;
      // Through a double, because this reader has no 64-bit JSON integer and a write counter
      // does not reach 2^53 in any run that ends.
      a.writes = static_cast<uint64_t>(row.Number("writes", 0));

      const json::Value* values = row.Find("values");
      if (values != nullptr && values->type == json::Value::Type::Array) {
        for (const json::Value& ev : values->items) {
          extattrs::Value v;
          v.id_hash = static_cast<int32_t>(ev.Number("idHash", 0));
          v.element = ev.Text("id", "");
          const json::Value* in_table = ev.Find("inTable");
          v.in_table = in_table != nullptr && in_table->type == json::Value::Type::Bool &&
                       in_table->boolean;
          const json::Value* comps = ev.Find("v");
          if (comps != nullptr && comps->type == json::Value::Type::Array) {
            for (const json::Value& c : comps->items) {
              // Back to the bits the sim stores, since the route decoded them per type on the
              // way out. Kept as bits so `extattrs::FormatValue` is the ONE place either
              // source is rendered -- two formatters would drift the first time one of them
              // learned about a new scalar type.
              v.bits.push_back(BitsOf(a.type, c));
            }
          }
          a.values.push_back(v);
        }
      }
      out->attrs.push_back(a);
    }
    return true;
  }

  const oni_sim::GameDataUpdate* frame() const { return published_ ? &update_ : nullptr; }
  const simhost::Tables& tables() const { return tables_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int cells() const { return width_ * height_; }
  uint32_t unity_frame() const { return unity_frame_; }
  size_t last_bytes() const { return body_.size(); }
  const std::vector<WorldInfo>& worlds() const { return worlds_; }
  const std::string& host() const { return host_; }
  const std::string& port() const { return port_; }
  // Which surfaces the SERVER says it can publish. A surface it lists as unpublished
  // comes back as a zero-length entry and leaves its pointer null, which is exactly what
  // `ViewAvailable` already tests for.
  bool ServerPublishes(uint32_t id) const { return published_ids_.Contains(id); }
  // Which surfaces the last decoded frame actually filled.
  bool Holds(uint32_t id) const { return held_.Contains(id); }
  // The element's SimHashes name, as `/elements` spells it. The sim table carries only
  // the hash, so this is the one place a readable name for an index exists at all.
  const char* name(int idx) const {
    return (idx >= 0 && static_cast<size_t>(idx) < names_.size())
               ? names_[static_cast<size_t>(idx)].c_str()
               : "?";
  }

 private:
  // A JSON number back into the sim's 32 bits, per the attribute's registered type. The route
  // decodes on the way out because JSON has one number type and cannot carry the tag; this
  // undoes exactly that, so the two sources reach `extattrs::FormatValue` identically.
  static uint32_t BitsOf(int32_t type, const json::Value& v) {
    if (v.type != json::Value::Type::Number) return 0;
    if (type == 0) {
      const float f = static_cast<float>(v.number);
      uint32_t bits = 0;
      memcpy(&bits, &f, sizeof(bits));
      return bits;
    }
    return static_cast<uint32_t>(static_cast<int64_t>(v.number));
  }

  static uint32_t ReadU32(const uint8_t* p) {
    // Explicit little-endian, matching PutU32 on the server. Not a memcpy of a uint32:
    // the wire order is defined by the format, not by this machine's.
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  }

  // A JSON error body, rendered for a human. The server answers a refused route with
  // `{"error": "..."}` and a non-200, and printing the raw body is the difference between
  // "poll failed" and knowing the save is not loaded yet.
  static std::string Describe(int status, const std::string& content_type,
                              const std::vector<uint8_t>& body) {
    std::string text(reinterpret_cast<const char*>(body.data()),
                     body.size() > 400 ? 400 : body.size());
    if (content_type.find("json") != std::string::npos) {
      json::Value v;
      std::string perr;
      if (json::Parse(text, &v, &perr) && v.Has("error")) text = v.Text("error", "");
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP %d: ", status);
    return std::string(buf) + text;
  }

  bool FetchJson(const char* path, json::Value* out, std::string* err) {
    std::vector<uint8_t> body;
    std::string content_type;
    int status = 0;
    if (!HttpGet(host_, port_, path, timeout_ms_, &body, &content_type, &status, err)) {
      return false;
    }
    if (status != 200) {
      *err = std::string(path) + " " + Describe(status, content_type, body);
      return false;
    }
    const std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    std::string perr;
    if (!json::Parse(text, out, &perr)) {
      *err = std::string(path) + " did not parse: " + perr;
      return false;
    }
    return true;
  }

  bool FetchWorld(std::string* err) {
    json::Value doc;
    if (!FetchJson("/world", &doc, err)) return false;
    if (doc.Has("error")) {
      *err = "/world: " + doc.Text("error", "");
      return false;
    }
    format_version_ = static_cast<uint32_t>(doc.Number("formatVersion", 0));
    if (format_version_ != kFormatVersion) {
      char buf[192];
      snprintf(buf, sizeof(buf),
               "the game speaks bulk format %u, this client speaks %u -- rebuild and "
               "redeploy whichever is older",
               format_version_, kFormatVersion);
      *err = buf;
      return false;
    }
    width_ = static_cast<int>(doc.Number("width", 0));
    height_ = static_cast<int>(doc.Number("height", 0));
    if (width_ <= 0 || height_ <= 0) {
      *err = "/world reported no grid; is a save loaded?";
      return false;
    }
    unity_frame_ = static_cast<uint32_t>(doc.Number("unityFrame", 0));

    worlds_.clear();
    if (const json::Value* ws = doc.Find("worlds")) {
      for (const json::Value& w : ws->items) {
        WorldInfo info;
        info.id = static_cast<int>(w.Number("id", 0));
        info.name = w.Text("name", "?");
        info.offset_x = static_cast<int>(w.Number("offsetX", 0));
        info.offset_y = static_cast<int>(w.Number("offsetY", 0));
        info.width = static_cast<int>(w.Number("width", 0));
        info.height = static_cast<int>(w.Number("height", 0));
        worlds_.push_back(info);
      }
    }

    published_ids_ = SurfaceSet();
    if (const json::Value* ss = doc.Find("surfaces")) {
      for (const json::Value& s : ss->items) {
        const json::Value* pub = s.Find("published");
        const bool is_published =
            pub && pub->type == json::Value::Type::Bool && pub->boolean;
        if (!is_published) continue;
        const uint32_t id = static_cast<uint32_t>(s.Number("id", 0));
        const SurfaceInfo* known = SurfaceById(id);
        if (!known) continue;  // a surface added on the game side that this build predates
        const uint32_t bpc = static_cast<uint32_t>(s.Number("bytesPerCell", 0));
        if (bpc != known->bytes_per_cell) {
          char buf[192];
          snprintf(buf, sizeof(buf),
                   "surface %u (%s) is %u bytes per cell on the game side and %u here",
                   id, known->name, bpc, known->bytes_per_cell);
          *err = buf;
          return false;
        }
        published_ids_.Add(id);
      }
    }
    return true;
  }

  bool FetchElements(std::string* err) {
    json::Value doc;
    if (!FetchJson("/elements", &doc, err)) return false;
    if (doc.Has("error")) {
      *err = "/elements: " + doc.Text("error", "");
      return false;
    }
    const json::Value* rows = doc.Find("elements");
    if (!rows || rows->type != json::Value::Type::Array || rows->items.empty()) {
      *err = "/elements carried no element array";
      return false;
    }
    if (!rows->items[0].Has("colour") || !rows->items[0].Has("idHash") ||
        !rows->items[0].Has("simState")) {
      *err =
          "/elements is missing colour/idHash/simState -- the OniFramework in the game "
          "predates live attach. Rebuild it and redeploy.";
      return false;
    }

    // The table is laid out exactly as `Elements_CreateTable` lays it out -- int32 count
    // then count Elements -- because simhost::Tables indexes it that way, and matching
    // the corpus layout is what lets the offline and attached paths share every reader.
    const int32_t count = static_cast<int32_t>(rows->items.size());
    tables_.elements.assign(4 + static_cast<size_t>(count) * sizeof(oni_sim::Element), 0);
    memcpy(tables_.elements.data(), &count, 4);
    tables_.count = count;
    // Never sent to a SimDLL, so the disease table stays empty rather than being faked.
    tables_.diseases.clear();

    for (int32_t i = 0; i < count; ++i) {
      const json::Value& row = rows->items[static_cast<size_t>(i)];
      const int idx = static_cast<int>(row.Number("idx", i));
      if (idx != i) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "/elements row %d says idx %d -- the table is not in sim index order",
                 i, idx);
        *err = buf;
        return false;
      }
      oni_sim::Element* e = const_cast<oni_sim::Element*>(tables_.At(i));
      e->id = static_cast<int32_t>(row.Number("idHash", 0));
      e->state = static_cast<uint8_t>(row.Number("simState", 0));
      e->elementsTableIdx = static_cast<uint16_t>(i);
      e->specificHeatCapacity = static_cast<float>(row.Number("specificHeatCapacity", 0));
      e->thermalConductivity = static_cast<float>(row.Number("thermalConductivity", 0));
      e->lowTemp = static_cast<float>(row.Number("lowTemp", 0));
      e->highTemp = static_cast<float>(row.Number("highTemp", 0));
      e->colour = static_cast<uint32_t>(row.Number("colour", 0));
      // Everything else stays zero. Reading this table to draw needs id, state, colour
      // and the two temperatures; the transition indices, gradients and physics defaults
      // are only meaningful to a sim that is being driven, and this one never is.
      names_.push_back(row.Text("id", "?"));
    }
    return true;
  }

  bool Decode(std::vector<uint8_t> body, std::string* err) {
    static const uint8_t kMagic[8] = {'O', 'N', 'I', 'G', 'R', 'I', 'D', 0};
    const size_t kHeaderBytes = 32, kEntryBytes = 16;
    if (body.size() < kHeaderBytes) {
      *err = "the /grid body is shorter than its header";
      return false;
    }
    if (memcmp(body.data(), kMagic, sizeof(kMagic)) != 0) {
      *err = "the /grid body does not start with ONIGRID";
      return false;
    }
    const uint32_t version = ReadU32(body.data() + 8);
    if (version != kFormatVersion) {
      char buf[128];
      snprintf(buf, sizeof(buf), "/grid body is format %u, this client reads %u", version,
               kFormatVersion);
      *err = buf;
      return false;
    }
    const uint32_t w = ReadU32(body.data() + 12);
    const uint32_t h = ReadU32(body.data() + 16);
    const uint32_t cells = ReadU32(body.data() + 20);
    const uint32_t unity_frame = ReadU32(body.data() + 24);
    const uint32_t array_count = ReadU32(body.data() + 28);
    if (static_cast<int>(w) != width_ || static_cast<int>(h) != height_) {
      char buf[192];
      snprintf(buf, sizeof(buf),
               "the game's grid changed under this client: %dx%d at attach, %ux%u now",
               width_, height_, w, h);
      *err = buf;
      return false;
    }
    if (cells != w * h) {
      *err = "the /grid header's cell count does not match its own dimensions";
      return false;
    }
    if (body.size() < kHeaderBytes + static_cast<size_t>(array_count) * kEntryBytes) {
      *err = "the /grid body is shorter than its entry table";
      return false;
    }

    // Everything is validated against the body BEFORE any pointer is published, so a
    // truncated or malformed reply leaves the previous frame standing rather than
    // pointing the renderer at bytes that are not there.
    struct Entry {
      uint32_t id, bytes_per_cell, byte_length, byte_offset;
    };
    std::vector<Entry> entries(array_count);
    for (uint32_t i = 0; i < array_count; ++i) {
      const uint8_t* p = body.data() + kHeaderBytes + static_cast<size_t>(i) * kEntryBytes;
      Entry& e = entries[i];
      e.id = ReadU32(p);
      e.bytes_per_cell = ReadU32(p + 4);
      e.byte_length = ReadU32(p + 8);
      e.byte_offset = ReadU32(p + 12);
      if (e.byte_length == 0) continue;  // the server has no pointer for this one
      const SurfaceInfo* known = SurfaceById(e.id);
      if (!known) {
        char buf[128];
        snprintf(buf, sizeof(buf), "/grid returned unknown surface id %u", e.id);
        *err = buf;
        return false;
      }
      if (e.bytes_per_cell != known->bytes_per_cell ||
          e.byte_length != cells * known->bytes_per_cell) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "surface %u (%s) claims %u bytes at %u per cell; %u cells needs %u", e.id,
                 known->name, e.byte_length, e.bytes_per_cell, cells,
                 cells * known->bytes_per_cell);
        *err = buf;
        return false;
      }
      if (e.byte_offset < kHeaderBytes + static_cast<size_t>(array_count) * kEntryBytes ||
          static_cast<size_t>(e.byte_offset) + e.byte_length > body.size()) {
        char buf[192];
        snprintf(buf, sizeof(buf), "surface %u (%s) points outside the body: %u..%u of %zu",
                 e.id, known->name, e.byte_offset, e.byte_offset + e.byte_length,
                 body.size());
        *err = buf;
        return false;
      }
      // Every array is at least 4-byte aligned on the wire and the vector's storage is
      // aligned well past that, so the pointers below are aligned for the types they are
      // cast to. Checked rather than assumed, because an unaligned float* is UB and
      // reading one on x86 succeeds quietly right up until it does not.
      if ((e.byte_offset & 3u) != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "surface %u (%s) is at unaligned offset %u", e.id,
                 known->name, e.byte_offset);
        *err = buf;
        return false;
      }
    }

    body_ = std::move(body);
    unity_frame_ = unity_frame;
    update_ = oni_sim::GameDataUpdate{};
    // The sim's own frame counter is not published to managed code at all (Game.cs reads
    // it off the update struct and stores it nowhere), so this field carries UNITY's
    // frame instead and is labelled as such everywhere it is shown.
    update_.numFramesProcessed = static_cast<int32_t>(unity_frame);
    held_ = SurfaceSet();
    for (const Entry& e : entries) {
      if (e.byte_length == 0) continue;
      uint8_t* at = body_.data() + e.byte_offset;
      switch (e.id) {
        case kElementIdx: update_.elementIdx = reinterpret_cast<uint16_t*>(at); break;
        case kTemperature: update_.temperature = reinterpret_cast<float*>(at); break;
        case kMass: update_.mass = reinterpret_cast<float*>(at); break;
        case kRadiation: update_.radiation = reinterpret_cast<float*>(at); break;
        case kProperties: update_.properties = at; break;
        case kInsulation: update_.insulation = at; break;
        case kStrengthInfo: update_.strengthInfo = at; break;
        case kDiseaseIdx: update_.diseaseIdx = at; break;
        case kDiseaseCount: update_.diseaseCount = reinterpret_cast<int32_t*>(at); break;
        case kBackwallElement:
          update_.backwallElement = reinterpret_cast<uint16_t*>(at);
          break;
        case kBackwallMass: update_.backwallMass = reinterpret_cast<float*>(at); break;
        case kBackwallTemperature:
          update_.backwallTemperature = reinterpret_cast<float*>(at);
          break;
        case kAccumulatedFlow:
          update_.accumulatedFlow = reinterpret_cast<float*>(at);
          break;
        case kTextureFlow: update_.propertyTextureFlow = at; break;
        case kTextureLiquid: update_.propertyTextureLiquid = at; break;
        case kTextureLiquidData: update_.propertyTextureLiquidData = at; break;
        case kTextureMaterialData: update_.propertyTextureMaterialData = at; break;
        case kTextureSunlight: update_.propertyTextureExposedToSunlight = at; break;
        default: break;
      }
      held_.Add(e.id);
    }

    // The three every reader assumes. `SampleCell` dereferences elementIdx, mass and
    // temperature without checking, and so does ViewScalar's default arm -- rightly, since
    // offline they are never absent. A fetch set that omitted one would crash rather than
    // draw wrong, so it is refused here instead.
    if (!update_.elementIdx || !update_.mass || !update_.temperature) {
      *err =
          "a frame without elementIdx, mass and temperature cannot be drawn; every fetch "
          "set must include all three";
      published_ = false;
      return false;
    }
    published_ = true;
    return true;
  }

  static const uint32_t kFormatVersion = 1;

  std::string host_, port_;
  int timeout_ms_ = 5000;
  bool open_ = false, published_ = false;
  uint32_t format_version_ = 0;
  int width_ = 0, height_ = 0;
  uint32_t unity_frame_ = 0;
  std::vector<WorldInfo> worlds_;
  SurfaceSet published_ids_, held_;
  std::vector<uint8_t> body_;
  oni_sim::GameDataUpdate update_{};
  simhost::Tables tables_;
  std::vector<std::string> names_;
};

}  // namespace attach
}  // namespace simviz
