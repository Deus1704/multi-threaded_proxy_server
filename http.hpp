// http.hpp
// Request/response parsing, host-header routing, access control, access logging.
// Header-only so both the proxy and the unit tests use the same code.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace http {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Wildcard match supporting a leading "*." and a trailing "*", plus bare "*".
// Deliberately not a full glob: these are the only shapes the config needs, and
// a smaller matcher is a smaller attack surface for an ACL.
inline bool wildcard_match(const std::string& pattern, const std::string& value) {
    if (pattern == "*") return true;
    if (pattern.size() > 2 && pattern.compare(0, 2, "*.") == 0) {
        const std::string suffix = pattern.substr(1); // ".example.com"
        if (value.size() < suffix.size()) return false;
        return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    if (!pattern.empty() && pattern.back() == '*') {
        const std::string prefix = pattern.substr(0, pattern.size() - 1);
        if (value.size() < prefix.size()) return false;
        return value.compare(0, prefix.size(), prefix) == 0;
    }
    return pattern == value;
}

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------
struct Request {
    std::string method;
    std::string target;   // origin-form "/a/b" or absolute-form "http://h/a/b"
    std::string version;  // "HTTP/1.1"
    std::string path;     // always origin-form, absolute-form is normalised into this
    std::string host;     // value of the Host header, or authority from absolute-form
    std::unordered_map<std::string, std::string> headers; // keys lower-cased
    std::string raw;      // full bytes as received (what gets forwarded upstream)
    size_t header_bytes = 0;
    long content_length = -1;
    bool keep_alive = true;
    bool valid = false;
    std::string parse_error;

    const std::string* header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? nullptr : &it->second;
    }
};

// Returns the offset just past the end of the header block, or npos.
inline size_t find_header_end(const std::string& buf) {
    size_t p = buf.find("\r\n\r\n");
    if (p != std::string::npos) return p + 4;
    p = buf.find("\n\n"); // tolerate bare-LF clients
    if (p != std::string::npos) return p + 2;
    return std::string::npos;
}

inline Request parse_request(const std::string& raw) {
    Request r;
    r.raw = raw;

    size_t hend = find_header_end(raw);
    if (hend == std::string::npos) {
        r.parse_error = "incomplete headers";
        return r;
    }
    r.header_bytes = hend;

    std::istringstream stream(raw.substr(0, hend));
    std::string line;

    if (!std::getline(stream, line)) {
        r.parse_error = "empty request";
        return r;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // request-line: METHOD SP request-target SP HTTP-version
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) {
        r.parse_error = "malformed request line";
        return r;
    }
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        r.parse_error = "malformed request line";
        return r;
    }
    r.method = line.substr(0, sp1);
    r.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    r.version = trim(line.substr(sp2 + 1));

    if (r.method.empty() || r.target.empty()) {
        r.parse_error = "malformed request line";
        return r;
    }
    if (r.version.compare(0, 5, "HTTP/") != 0) {
        r.parse_error = "unsupported version";
        return r;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue; // skip junk rather than reject
        std::string name = to_lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (name.empty()) continue;
        auto existing = r.headers.find(name);
        if (existing == r.headers.end())
            r.headers[name] = value;
        else
            existing->second += ", " + value; // fold repeats, per RFC 9110
    }

    // Absolute-form target ("GET http://host/path HTTP/1.1") is what a client
    // configured to use a forward proxy actually sends. Normalise it so routing
    // and the cache key are computed the same way for both forms.
    r.path = r.target;
    std::string lower_target = to_lower(r.target);
    if (lower_target.compare(0, 7, "http://") == 0) {
        size_t slash = r.target.find('/', 7);
        if (slash == std::string::npos) {
            r.host = r.target.substr(7);
            r.path = "/";
        } else {
            r.host = r.target.substr(7, slash - 7);
            r.path = r.target.substr(slash);
        }
    }
    if (r.host.empty()) {
        if (const std::string* h = r.header("host")) r.host = *h;
    }

    if (const std::string* cl = r.header("content-length")) {
        errno = 0;
        char* end = nullptr;
        long v = std::strtol(cl->c_str(), &end, 10);
        if (errno == 0 && end && *end == '\0' && v >= 0) r.content_length = v;
    }

    // HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close.
    r.keep_alive = (r.version == "HTTP/1.1");
    if (const std::string* c = r.header("connection")) {
        std::string v = to_lower(*c);
        if (v.find("close") != std::string::npos) r.keep_alive = false;
        else if (v.find("keep-alive") != std::string::npos) r.keep_alive = true;
    }

    // Host is mandatory in HTTP/1.1 and is what we route on, so a request
    // without it cannot be served by a forward proxy.
    if (r.host.empty()) {
        r.parse_error = "missing Host header";
        return r;
    }

    r.valid = true;
    return r;
}

// Total bytes a complete request occupies (headers + body), if known.
inline size_t expected_request_bytes(const Request& r) {
    if (r.content_length > 0) return r.header_bytes + (size_t)r.content_length;
    return r.header_bytes;
}

// ---------------------------------------------------------------------------
// Response (parsing what upstream sends back)
// ---------------------------------------------------------------------------
struct Response {
    int status = 0;
    std::unordered_map<std::string, std::string> headers;
    size_t header_bytes = 0;
    long content_length = -1;
    bool chunked = false;
    bool close_delimited = false;
    bool valid = false;

    const std::string* header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? nullptr : &it->second;
    }
};

inline Response parse_response_headers(const std::string& raw) {
    Response resp;
    size_t hend = find_header_end(raw);
    if (hend == std::string::npos) return resp;
    resp.header_bytes = hend;

    std::istringstream stream(raw.substr(0, hend));
    std::string line;
    if (!std::getline(stream, line)) return resp;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // status-line: HTTP/1.1 SP 200 SP OK
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return resp;
    size_t sp2 = line.find(' ', sp1 + 1);
    std::string code = (sp2 == std::string::npos) ? line.substr(sp1 + 1)
                                                  : line.substr(sp1 + 1, sp2 - sp1 - 1);
    resp.status = std::atoi(code.c_str());
    if (resp.status < 100 || resp.status > 599) return resp;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = to_lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (!name.empty()) resp.headers[name] = value;
    }

    if (const std::string* te = resp.header("transfer-encoding")) {
        if (to_lower(*te).find("chunked") != std::string::npos) resp.chunked = true;
    }
    if (!resp.chunked) {
        if (const std::string* cl = resp.header("content-length")) {
            char* end = nullptr;
            long v = std::strtol(cl->c_str(), &end, 10);
            if (end && *end == '\0' && v >= 0) resp.content_length = v;
        }
    }
    // No framing information at all => the body ends when the socket closes.
    if (!resp.chunked && resp.content_length < 0) resp.close_delimited = true;

    resp.valid = true;
    return resp;
}

// Walks the chunked body properly rather than searching for the "0\r\n\r\n" byte
// sequence. Searching is tempting and wrong: those bytes can occur inside chunk
// DATA, which would truncate a response mid-body and then cache the truncation.
// Each chunk is "<hex-size>[;ext]\r\n<data>\r\n"; a zero size ends the body
// (followed by optional trailers and a final CRLF).
inline bool chunked_body_complete(const std::string& raw, size_t body_start) {
    size_t pos = body_start;
    for (;;) {
        size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) return false; // size line still arriving

        // Size is hex, optionally followed by ";chunk-extension".
        size_t size_end = raw.find(';', pos);
        if (size_end == std::string::npos || size_end > line_end) size_end = line_end;

        size_t chunk_size = 0;
        bool any_digit = false;
        for (size_t i = pos; i < size_end; i++) {
            char c = raw[i];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else if (c == ' ' || c == '\t') continue;
            else return false; // malformed: refuse rather than guess
            chunk_size = chunk_size * 16 + (size_t)v;
            any_digit = true;
            if (chunk_size > (size_t)1 << 40) return false; // absurd, treat as malformed
        }
        if (!any_digit) return false;

        if (chunk_size == 0) {
            // Terminal chunk. Consume optional trailer lines up to the blank line.
            size_t p = line_end + 2;
            for (;;) {
                size_t e = raw.find("\r\n", p);
                if (e == std::string::npos) return false;
                if (e == p) return true; // blank line: message complete
                p = e + 2;
            }
        }

        // Skip this chunk's data plus its trailing CRLF.
        pos = line_end + 2 + chunk_size + 2;
        if (pos > raw.size()) return false;
    }
}

// Is `raw` a complete response message yet? Used to stop reading upstream at
// the right byte instead of blocking until EOF (which stalls on a keep-alive
// origin until the 5s socket timeout fires).
inline bool response_complete(const std::string& raw, const Response& resp) {
    if (!resp.valid) return false;
    if (resp.chunked) return chunked_body_complete(raw, resp.header_bytes);
    if (resp.content_length >= 0)
        return raw.size() >= resp.header_bytes + (size_t)resp.content_length;
    return false; // close-delimited: only EOF tells us
}

// Only cache what is actually safe to replay to another client.
inline bool is_cacheable(const Request& req, const Response& resp) {
    if (req.method != "GET" && req.method != "HEAD") return false;
    if (resp.status != 200) return false;
    if (req.header("authorization")) return false;
    if (const std::string* cc = resp.header("cache-control")) {
        std::string v = to_lower(*cc);
        if (v.find("no-store") != std::string::npos) return false;
        if (v.find("no-cache") != std::string::npos) return false;
        if (v.find("private") != std::string::npos) return false;
    }
    if (resp.header("set-cookie")) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Host-header routing
// ---------------------------------------------------------------------------
struct Upstream {
    std::string host;
    int port = 80;
};

// Splits "example.com:8080" into host + port. Default port 80.
inline Upstream split_authority(const std::string& authority) {
    Upstream u;
    u.host = authority;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && colon + 1 < authority.size()) {
        bool all_digits = true;
        for (size_t i = colon + 1; i < authority.size(); i++)
            if (!std::isdigit((unsigned char)authority[i])) { all_digits = false; break; }
        if (all_digits) {
            u.host = authority.substr(0, colon);
            int p = std::atoi(authority.c_str() + colon + 1);
            if (p > 0 && p < 65536) u.port = p;
        }
    }
    return u;
}

// Explicit host -> upstream overrides, so the proxy can front a named service
// on a different address. Falls back to the Host header itself.
class RouteTable {
    std::vector<std::pair<std::string, Upstream>> routes_; // pattern -> upstream

public:
    void add(const std::string& host_pattern, const Upstream& up) {
        routes_.emplace_back(to_lower(host_pattern), up);
    }

    Upstream resolve(const std::string& host_header) const {
        std::string h = to_lower(host_header);
        for (const auto& [pattern, up] : routes_)
            if (wildcard_match(pattern, h)) return up;
        return split_authority(host_header);
    }

    size_t size() const { return routes_.size(); }
};

// ---------------------------------------------------------------------------
// Access control
// ---------------------------------------------------------------------------
enum class Decision { Allow, Deny };

struct AclResult {
    Decision decision = Decision::Allow;
    std::string matched_rule; // for the access log
};

// Rules are evaluated in file order; first match wins. If nothing matches, the
// default policy applies. First-match-wins (rather than deny-overrides) is what
// makes "allow one host, deny the rest" expressible in two lines.
//
// Config format, one rule per line:
//   default allow|deny
//   allow|deny host <pattern>
//   allow|deny path <pattern>
//   allow|deny method <NAME>
//   route <host-pattern> <upstream-host> <upstream-port>
// Patterns support "*", "*.suffix", "prefix*".
class AccessControl {
    struct Rule {
        Decision decision;
        std::string field; // host | path | method
        std::string pattern;
        std::string source; // original line, for logging
    };

    std::vector<Rule> rules_;
    Decision default_policy_ = Decision::Allow;
    RouteTable routes_;

public:
    Decision default_policy() const { return default_policy_; }
    void set_default_policy(Decision d) { default_policy_ = d; }
    size_t rule_count() const { return rules_.size(); }
    const RouteTable& routes() const { return routes_; }

    void add_rule(Decision d, const std::string& field, const std::string& pattern,
                  const std::string& source = "") {
        rules_.push_back({d, to_lower(field), pattern,
                          source.empty() ? (field + " " + pattern) : source});
    }

    void add_route(const std::string& host_pattern, const std::string& up_host, int up_port) {
        routes_.add(host_pattern, Upstream{up_host, up_port});
    }

    AclResult check(const Request& req) const {
        const std::string host = to_lower(req.host);
        for (const auto& r : rules_) {
            bool match = false;
            if (r.field == "host")        match = wildcard_match(to_lower(r.pattern), host);
            else if (r.field == "path")   match = wildcard_match(r.pattern, req.path);
            else if (r.field == "method") match = wildcard_match(to_lower(r.pattern),
                                                                to_lower(req.method));
            if (match) return {r.decision, r.source};
        }
        return {default_policy_, "default"};
    }

    // Returns false and fills `error` on a malformed config, so a typo in the
    // ACL fails the server at startup instead of silently allowing traffic.
    bool load_string(const std::string& text, std::string* error = nullptr) {
        std::istringstream in(text);
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            lineno++;
            std::string s = trim(line);
            if (s.empty() || s[0] == '#') continue;

            std::istringstream ls(s);
            std::string verb;
            ls >> verb;
            verb = to_lower(verb);

            if (verb == "default") {
                std::string pol;
                ls >> pol;
                pol = to_lower(pol);
                if (pol == "allow") default_policy_ = Decision::Allow;
                else if (pol == "deny") default_policy_ = Decision::Deny;
                else { if (error) *error = "line " + std::to_string(lineno) +
                                          ": default must be allow|deny"; return false; }
            } else if (verb == "allow" || verb == "deny") {
                std::string field, pattern;
                ls >> field >> pattern;
                field = to_lower(field);
                if (pattern.empty()) {
                    if (error) *error = "line " + std::to_string(lineno) + ": missing pattern";
                    return false;
                }
                if (field != "host" && field != "path" && field != "method") {
                    if (error) *error = "line " + std::to_string(lineno) +
                                        ": field must be host|path|method";
                    return false;
                }
                add_rule(verb == "allow" ? Decision::Allow : Decision::Deny, field, pattern, s);
            } else if (verb == "route") {
                std::string host_pattern, up_host;
                int up_port = 0;
                ls >> host_pattern >> up_host >> up_port;
                if (host_pattern.empty() || up_host.empty() || up_port <= 0 || up_port > 65535) {
                    if (error) *error = "line " + std::to_string(lineno) +
                                        ": route needs <host-pattern> <host> <port>";
                    return false;
                }
                add_route(host_pattern, up_host, up_port);
            } else {
                if (error) *error = "line " + std::to_string(lineno) + ": unknown directive '" +
                                    verb + "'";
                return false;
            }
        }
        return true;
    }

    bool load_file(const std::string& path, std::string* error = nullptr) {
        std::ifstream f(path);
        if (!f) {
            if (error) *error = "cannot open " + path;
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return load_string(ss.str(), error);
    }
};

// ---------------------------------------------------------------------------
// Access log
// ---------------------------------------------------------------------------
// One line per request, emitted for every request the proxy touches, served
// from cache, forwarded, denied, or rejected as malformed. Format is
// space-separated key=value so it greps cleanly and parses without a schema.
class AccessLog {
    std::mutex mtx_;
    std::ofstream file_;
    bool to_stdout_ = true;
    uint64_t lines_ = 0;

    // Lines are batched in memory and flushed when the buffer fills or when the
    // periodic flusher ticks. fflush-per-request would put a disk write inside
    // the critical section of every single request: measured, that alone caps
    // the proxy well below its actual request-handling rate. nginx buffers its
    // access log for the same reason. `immediate` restores fsync-ish behaviour
    // for the case where the log is an audit trail that must survive a crash.
    std::string buf_;
    size_t flush_bytes_ = 64 * 1024;
    bool immediate_ = false;

    // strftime + gmtime_r per line is pure overhead when thousands of requests
    // share the same second. Cache the second-granularity prefix and only
    // reformat the milliseconds.
    time_t cached_sec_ = 0;
    char cached_prefix_[32] = {0};

    void flush_locked() {
        if (!buf_.empty() && file_.is_open()) {
            file_.write(buf_.data(), (std::streamsize)buf_.size());
            file_.flush();
        }
        buf_.clear();
    }

public:
    // Empty path = stdout only.
    bool open(const std::string& path, bool also_stdout = false, bool immediate = false) {
        std::lock_guard<std::mutex> lock(mtx_);
        to_stdout_ = also_stdout || path.empty();
        immediate_ = immediate;
        if (path.empty()) return true;
        file_.open(path, std::ios::app);
        if (file_.is_open()) buf_.reserve(flush_bytes_ + 1024);
        return file_.is_open();
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mtx_);
        flush_locked();
    }

    ~AccessLog() {
        std::lock_guard<std::mutex> lock(mtx_);
        flush_locked();
    }

    struct Entry {
        std::string client_ip;
        std::string method;
        std::string path;
        std::string version;
        std::string host;
        std::string upstream;   // "host:port" or "-"
        int status = 0;
        size_t bytes = 0;
        const char* cache = "-";   // HIT | MISS | BYPASS | -
        const char* acl = "-";     // ALLOW | DENY
        std::string rule;          // matched ACL rule
        long long duration_us = 0;
    };

    void write(const Entry& e) {
        timespec now{};
        clock_gettime(CLOCK_REALTIME, &now);
        char ts[64];
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (now.tv_sec != cached_sec_) {
                tm tmv{};
                gmtime_r(&now.tv_sec, &tmv);
                strftime(cached_prefix_, sizeof(cached_prefix_), "%Y-%m-%dT%H:%M:%S", &tmv);
                cached_sec_ = now.tv_sec;
            }
            snprintf(ts, sizeof(ts), "%s.%03ldZ", cached_prefix_, now.tv_nsec / 1000000);
        }

        std::ostringstream out;
        out << ts << ' ' << (e.client_ip.empty() ? "-" : e.client_ip)
            << " \"" << e.method << ' ' << e.path << ' ' << e.version << '"'
            << " host=" << (e.host.empty() ? "-" : e.host)
            << " upstream=" << (e.upstream.empty() ? "-" : e.upstream)
            << " status=" << e.status
            << " bytes=" << e.bytes
            << " cache=" << e.cache
            << " acl=" << e.acl
            << " rule=\"" << e.rule << '"'
            << " dur_us=" << e.duration_us;
        std::string line = out.str();

        std::lock_guard<std::mutex> lock(mtx_);
        lines_++;
        if (file_.is_open()) {
            buf_ += line;
            buf_ += '\n';
            if (immediate_ || buf_.size() >= flush_bytes_) flush_locked();
        }
        if (to_stdout_) {
            fputs(line.c_str(), stdout);
            fputc('\n', stdout);
        }
    }

    uint64_t lines() {
        std::lock_guard<std::mutex> lock(mtx_);
        return lines_;
    }
};

} // namespace http
