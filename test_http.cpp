// test_http.cpp
// Unit tests for the HTTP parser, host-header routing, and access control.
// These are the pieces the resume claims exist, so they get tested directly
// rather than only exercised end to end.

#include <cstdio>
#include <string>

#include "http.hpp"

using namespace std;
using namespace http;

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        g_total++;                                                                 \
        if (!(cond)) {                                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_fail++;                                                              \
        }                                                                          \
    } while (0)

static void test_request_line() {
    printf("request line + headers\n");
    Request r = parse_request("GET /a/b?c=1 HTTP/1.1\r\nHost: example.com\r\n"
                              "User-Agent: x\r\n\r\n");
    CHECK(r.valid);
    CHECK(r.method == "GET");
    CHECK(r.path == "/a/b?c=1");
    CHECK(r.version == "HTTP/1.1");
    CHECK(r.host == "example.com");
    CHECK(r.keep_alive); // HTTP/1.1 default
    CHECK(r.header("user-agent") && *r.header("user-agent") == "x");
}

static void test_header_case_and_folding() {
    printf("header names are case-insensitive, repeats fold\n");
    Request r = parse_request("GET / HTTP/1.1\r\nHOST: e.com\r\nX-F: 1\r\nx-f: 2\r\n\r\n");
    CHECK(r.valid);
    CHECK(r.host == "e.com");
    CHECK(r.header("x-f") && *r.header("x-f") == "1, 2");
}

static void test_absolute_form() {
    printf("absolute-form target (what a proxy client actually sends)\n");
    Request r = parse_request("GET http://origin.test:8080/p/q HTTP/1.1\r\n"
                              "Host: ignored.example\r\n\r\n");
    CHECK(r.valid);
    // authority in the target wins over the Host header
    CHECK(r.host == "origin.test:8080");
    CHECK(r.path == "/p/q");

    Request r2 = parse_request("GET http://origin.test HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK(r2.valid);
    CHECK(r2.host == "origin.test");
    CHECK(r2.path == "/");
}

static void test_incomplete_and_malformed() {
    printf("incomplete / malformed requests are rejected, not guessed\n");
    CHECK(!parse_request("GET / HTTP/1.1\r\nHost: a.com\r\n").valid);   // no blank line
    CHECK(!parse_request("GET /\r\n\r\n").valid);                        // no version
    CHECK(!parse_request("\r\n\r\n").valid);                             // empty
    CHECK(!parse_request("GET / HTTP/1.1\r\n\r\n").valid);               // no Host
    CHECK(!parse_request("GET / SPDY/9\r\nHost: a\r\n\r\n").valid);      // not HTTP
    Request r = parse_request("GET / HTTP/1.1\r\nHost: a\r\ngarbage-no-colon\r\n\r\n");
    CHECK(r.valid); // junk header lines are skipped, request is still usable
}

static void test_keep_alive_semantics() {
    printf("keep-alive defaults per version, Connection overrides\n");
    CHECK(parse_request("GET / HTTP/1.1\r\nHost: a\r\n\r\n").keep_alive);
    CHECK(!parse_request("GET / HTTP/1.0\r\nHost: a\r\n\r\n").keep_alive);
    CHECK(!parse_request("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n").keep_alive);
    CHECK(parse_request("GET / HTTP/1.0\r\nHost: a\r\nConnection: keep-alive\r\n\r\n").keep_alive);
    // capitalisation of the value must not matter
    CHECK(!parse_request("GET / HTTP/1.1\r\nHost: a\r\nConnection: CLOSE\r\n\r\n").keep_alive);
}

static void test_body_framing() {
    printf("request body length\n");
    string raw = "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    Request r = parse_request(raw);
    CHECK(r.valid);
    CHECK(r.content_length == 5);
    CHECK(expected_request_bytes(r) == raw.size());

    // A partial body must report that more bytes are needed.
    Request p = parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhe");
    CHECK(p.valid);
    CHECK(expected_request_bytes(p) > string("POST /x HTTP/1.1\r\nHost: a\r\n"
                                             "Content-Length: 5\r\n\r\nhe").size());
}

static void test_response_framing() {
    printf("response framing: content-length, chunked, close-delimited\n");
    Response a = parse_response_headers("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    CHECK(a.valid && a.status == 200 && a.content_length == 3 && !a.chunked &&
          !a.close_delimited);
    CHECK(response_complete("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", a));
    CHECK(!response_complete("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nab", a));

    Response b = parse_response_headers("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    CHECK(b.valid && b.chunked && b.content_length == -1);
    const std::string chunked_head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(response_complete(chunked_head + "3\r\nabc\r\n0\r\n\r\n", b));
    CHECK(response_complete(chunked_head + "0\r\n\r\n", b));                 // empty body
    CHECK(response_complete(chunked_head + "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", b)); // multi-chunk
    CHECK(response_complete(chunked_head + "a\r\n0123456789\r\n0\r\n\r\n", b));      // hex size
    CHECK(response_complete(chunked_head + "3;name=v\r\nabc\r\n0\r\n\r\n", b));      // chunk-ext
    CHECK(response_complete(chunked_head + "0\r\nX-T: 1\r\n\r\n", b));               // trailer

    // Incomplete forms must NOT be reported complete.
    CHECK(!response_complete(chunked_head + "3\r\nab", b));                  // short data
    CHECK(!response_complete(chunked_head + "3\r\nabc\r\n", b));             // no terminal chunk
    CHECK(!response_complete(chunked_head + "3\r\nabc\r\n0\r\n", b));        // trailers unterminated
    CHECK(!response_complete(chunked_head + "5\r\n", b));                    // size only

    // The bug a naive find("\r\n0\r\n\r\n") search would have: those exact bytes
    // appearing INSIDE chunk data must not be mistaken for the end of the message.
    // Chunk of 7 bytes whose payload is literally "0\r\n\r\n" preceded by CRLF.
    const std::string trap = chunked_head + std::string("7\r\n") + "\r\n0\r\n\r\n" + "\r\n";
    CHECK(!response_complete(trap, b));                       // truncating here would corrupt it
    CHECK(response_complete(trap + "0\r\n\r\n", b));           // only now is it really done

    Response c = parse_response_headers("HTTP/1.1 200 OK\r\nServer: x\r\n\r\nbody");
    CHECK(c.valid && c.close_delimited); // no framing info => EOF terminates

    CHECK(!parse_response_headers("garbage\r\n\r\n").valid);
    CHECK(!parse_response_headers("HTTP/1.1 999 Nope\r\n\r\n").valid);
}

static void test_cacheability() {
    printf("cacheability rules\n");
    auto req = [](const string& raw) { return parse_request(raw); };
    Request get = req("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    Request post = req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n");
    Request authed = req("GET / HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer t\r\n\r\n");

    Response ok = parse_response_headers("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    Response notfound = parse_response_headers("HTTP/1.1 404 NF\r\nContent-Length: 0\r\n\r\n");
    Response nostore = parse_response_headers(
        "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: 0\r\n\r\n");
    Response cookie = parse_response_headers(
        "HTTP/1.1 200 OK\r\nSet-Cookie: s=1\r\nContent-Length: 0\r\n\r\n");
    Response priv = parse_response_headers(
        "HTTP/1.1 200 OK\r\nCache-Control: private\r\nContent-Length: 0\r\n\r\n");

    CHECK(is_cacheable(get, ok));
    CHECK(!is_cacheable(post, ok));      // only GET/HEAD
    CHECK(!is_cacheable(get, notfound)); // only 200
    CHECK(!is_cacheable(get, nostore));
    CHECK(!is_cacheable(get, cookie));   // would leak one user's session
    CHECK(!is_cacheable(get, priv));
    CHECK(!is_cacheable(authed, ok));    // would leak an authorised response
}

static void test_wildcards() {
    printf("wildcard matcher\n");
    CHECK(wildcard_match("*", "anything"));
    CHECK(wildcard_match("exact.com", "exact.com"));
    CHECK(!wildcard_match("exact.com", "notexact.com"));
    CHECK(wildcard_match("*.ads.com", "x.ads.com"));
    CHECK(wildcard_match("*.ads.com", "a.b.ads.com"));
    CHECK(!wildcard_match("*.ads.com", "ads.com"));       // bare parent is not a subdomain
    CHECK(!wildcard_match("*.ads.com", "evilads.com"));   // must match on the dot
    CHECK(wildcard_match("/admin/*", "/admin/users"));
    CHECK(!wildcard_match("/admin/*", "/public/admin/"));
}

static void test_split_authority() {
    printf("authority splitting\n");
    CHECK(split_authority("h.com").host == "h.com");
    CHECK(split_authority("h.com").port == 80);
    CHECK(split_authority("h.com:8080").host == "h.com");
    CHECK(split_authority("h.com:8080").port == 8080);
    // a trailing non-numeric colon part is part of the host, not a port
    CHECK(split_authority("h.com:abc").host == "h.com:abc");
    CHECK(split_authority("h.com:99999").port == 80); // out of range, ignored
}

static void test_routing() {
    printf("host-header routing\n");
    RouteTable rt;
    rt.add("origin.test", Upstream{"127.0.0.1", 19000});
    rt.add("*.internal", Upstream{"10.0.0.5", 8080});

    CHECK(rt.resolve("origin.test").host == "127.0.0.1");
    CHECK(rt.resolve("origin.test").port == 19000);
    CHECK(rt.resolve("ORIGIN.TEST").port == 19000); // host matching is case-insensitive
    CHECK(rt.resolve("svc.internal").host == "10.0.0.5");
    // no route => route to the Host header itself, which is the fallback
    CHECK(rt.resolve("other.com:8081").host == "other.com");
    CHECK(rt.resolve("other.com:8081").port == 8081);
}

static void test_acl() {
    printf("access control: order, defaults, fail-closed parsing\n");
    AccessControl acl;
    string err;
    bool loaded = acl.load_string(
        "# comment\n"
        "deny  method connect\n"
        "deny  host blocked.example.com\n"
        "deny  host *.doubleclick.net\n"
        "deny  path /admin/*\n"
        "route origin.test 127.0.0.1 19000\n"
        "default allow\n",
        &err);
    CHECK(loaded);
    if (!loaded) printf("    load error: %s\n", err.c_str());

    auto decide = [&](const string& raw) { return acl.check(parse_request(raw)); };

    CHECK(decide("GET / HTTP/1.1\r\nHost: ok.com\r\n\r\n").decision == Decision::Allow);
    CHECK(decide("GET / HTTP/1.1\r\nHost: blocked.example.com\r\n\r\n").decision == Decision::Deny);
    CHECK(decide("GET / HTTP/1.1\r\nHost: ad.doubleclick.net\r\n\r\n").decision == Decision::Deny);
    CHECK(decide("GET /admin/users HTTP/1.1\r\nHost: ok.com\r\n\r\n").decision == Decision::Deny);
    CHECK(decide("CONNECT x:443 HTTP/1.1\r\nHost: x:443\r\n\r\n").decision == Decision::Deny);
    // host match must survive a port and odd casing
    CHECK(decide("GET / HTTP/1.1\r\nHost: BLOCKED.EXAMPLE.COM\r\n\r\n").decision == Decision::Deny);
    // the matched rule is reported, so the access log can say why
    CHECK(decide("GET / HTTP/1.1\r\nHost: blocked.example.com\r\n\r\n").matched_rule.find(
              "blocked.example.com") != string::npos);
    CHECK(decide("GET / HTTP/1.1\r\nHost: ok.com\r\n\r\n").matched_rule == "default");
    CHECK(acl.routes().resolve("origin.test").port == 19000);

    printf("access control: default deny (allowlist mode)\n");
    AccessControl allowlist;
    CHECK(allowlist.load_string("allow host good.com\nallow host *.good.net\ndefault deny\n"));
    CHECK(allowlist.check(parse_request("GET / HTTP/1.1\r\nHost: good.com\r\n\r\n")).decision ==
          Decision::Allow);
    CHECK(allowlist.check(parse_request("GET / HTTP/1.1\r\nHost: a.good.net\r\n\r\n")).decision ==
          Decision::Allow);
    CHECK(allowlist.check(parse_request("GET / HTTP/1.1\r\nHost: evil.com\r\n\r\n")).decision ==
          Decision::Deny);

    printf("access control: first match wins\n");
    AccessControl order;
    CHECK(order.load_string("allow host safe.ads.example.com\ndeny host *.ads.example.com\n"
                            "default allow\n"));
    CHECK(order.check(parse_request("GET / HTTP/1.1\r\nHost: safe.ads.example.com\r\n\r\n"))
              .decision == Decision::Allow);
    CHECK(order.check(parse_request("GET / HTTP/1.1\r\nHost: other.ads.example.com\r\n\r\n"))
              .decision == Decision::Deny);

    printf("access control: bad config is an error, never silently allow-all\n");
    AccessControl bad;
    CHECK(!bad.load_string("allow\n", &err));            // missing field/pattern
    CHECK(!bad.load_string("permit host x\n", &err));     // unknown directive
    CHECK(!bad.load_string("allow scheme https\n", &err)); // unknown field
    CHECK(!bad.load_string("default maybe\n", &err));
    CHECK(!bad.load_string("route h\n", &err));           // incomplete route
    CHECK(!bad.load_string("route h 1.2.3.4 0\n", &err)); // invalid port
}

static void test_acl_file_matches_repo_config() {
    printf("the shipped access_control.conf parses\n");
    AccessControl acl;
    string err;
    if (!acl.load_file("access_control.conf", &err)) {
        printf("  (skipped: %s)\n", err.c_str());
        return;
    }
    CHECK(acl.rule_count() > 0);
    CHECK(acl.check(parse_request("GET / HTTP/1.1\r\nHost: blocked.example.com\r\n\r\n"))
              .decision == Decision::Deny);
    CHECK(acl.check(parse_request("GET /admin/x HTTP/1.1\r\nHost: any.com\r\n\r\n")).decision ==
          Decision::Deny);
    CHECK(acl.check(parse_request("GET /ok HTTP/1.1\r\nHost: any.com\r\n\r\n")).decision ==
          Decision::Allow);
}

int main() {
    printf("=== http.hpp: parser / routing / access control ===\n\n");
    test_request_line();
    test_header_case_and_folding();
    test_absolute_form();
    test_incomplete_and_malformed();
    test_keep_alive_semantics();
    test_body_framing();
    test_response_framing();
    test_cacheability();
    test_wildcards();
    test_split_authority();
    test_routing();
    test_acl();
    test_acl_file_matches_repo_config();

    printf("\n%d checks, %d failures\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
