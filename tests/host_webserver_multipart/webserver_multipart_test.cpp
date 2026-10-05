// Host test for the multipart parser exits in libraries/WebServer/src/Parsing.cpp.
// Build and run with run.sh in this directory.
//
// Drives the real WebServer::_parseRequest / _parseForm with a scripted client
// (stubs/WiFiClient.h) on a simulated clock. Checks:
//   - legitimate forms still parse (the FluidNC v1 and v2 upload shapes, extra part
//     headers, several files, a field value with blank lines);
//   - a client that stops sending, closes, or sends junk in the part headers or in a
//     field value makes the parser give up within a bounded simulated time, where the
//     unpatched loops never return (a run past the hang limit counts as a failure);
//   - every failed or aborted parse frees the post-argument array, and so does an
//     exception (std::bad_alloc) out of an upload callback;
//   - the read timeout is HTTP_MAX_POST_WAIT outside a file body and
//     HTTP_MAX_SEND_WAIT inside it;
//   - a failed allocation (std::bad_alloc from operator new[], made to fail on demand
//     below) leaves the argument arrays consistent, so the next request frees each
//     array exactly once, and does not leak a plain POST body;
//   - every unsuccessful multipart parse that started a file part ends with exactly one
//     UPLOAD_FILE_ABORTED, also after the part's UPLOAD_FILE_END, so a handler that keeps
//     per-request upload state (FluidNC's /api/v2/files) is reset for the next request.
// Also checks the pure guard (detail/MultipartLineGuard.h) directly.

#include "WebServer.h"
#include "detail/MultipartLineGuard.h"

#include <cstdio>
#include <malloc/malloc.h>
#include <new>
#include <set>
#include <string>
#include <vector>

// ---- operator new[] seam ------------------------------------------------------------
// The WebServer argument arrays are the only array allocations here. Every live one is
// tracked; delete[] of a pointer that is not live is counted as a double delete (and
// not passed to free, so the run continues). fail_array_new_at = n makes the n-th
// array allocation from now throw std::bad_alloc.
static std::set<void*>& live_arrays() {
    static std::set<void*>* s = new std::set<void*>();
    return *s;
}
static int array_news        = 0;
static int fail_array_new_at = 0;  // 0: never
static int double_deletes    = 0;

void* operator new[](size_t n) {
    if (fail_array_new_at != 0 && ++array_news == fail_array_new_at) {
        fail_array_new_at = 0;
        throw std::bad_alloc();
    }
    void* p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    live_arrays().insert(p);
    return p;
}
void operator delete[](void* p) noexcept {
    if (!p) return;
    if (live_arrays().erase(p) == 0) {
        ++double_deletes;
        return;
    }
    free(p);
}
void operator delete[](void* p, size_t) noexcept { operator delete[](p); }

static void fail_nth_array_new(int n) {
    array_news        = 0;
    fail_array_new_at = n;
}

static size_t heap_in_use() {
    malloc_statistics_t st;
    malloc_zone_statistics(nullptr, &st);
    return st.size_in_use;
}

unsigned long fake_millis        = 0;
unsigned long fake_hang_limit_ms = 0;
int           fake_log_errors    = 0;

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
        }                                                                   \
    } while (0)

// ---- The parts of WebServer.cpp that Parsing.cpp needs ----------------------------

WebServer::WebServer(int port)
    : _corsEnabled(false), _server(port), _currentMethod(HTTP_ANY), _currentVersion(0), _currentStatus(HC_NONE),
      _statusChange(0), _nullDelay(true), _currentHandler(nullptr), _firstHandler(nullptr), _lastHandler(nullptr),
      _currentArgCount(0), _currentArgs(nullptr), _postArgsLen(0), _postArgs(nullptr), _headerKeysCount(0),
      _currentHeaders(nullptr), _contentLength(CONTENT_LENGTH_NOT_SET), _clientContentLength(0), _chunked(false) {}
WebServer::~WebServer() {
    delete[] _currentArgs;
    delete[] _postArgs;
    delete[] _currentHeaders;
}
void WebServer::begin() {}
void WebServer::begin(uint16_t) {}
void WebServer::handleClient() {}
void WebServer::close() {}
bool WebServer::hasArg(String name) {
    for (int j = 0; j < _postArgsLen; ++j)
        if (_postArgs[j].key == name) return true;
    for (int i = 0; i < _currentArgCount; ++i)
        if (_currentArgs[i].key == name) return true;
    return false;
}
String WebServer::arg(String name) {
    for (int j = 0; j < _postArgsLen; ++j)
        if (_postArgs[j].key == name) return _postArgs[j].value;
    for (int i = 0; i < _currentArgCount; ++i)
        if (_currentArgs[i].key == name) return _currentArgs[i].value;
    return "";
}

// ---- Test harness -----------------------------------------------------------------

static WiFiClient* g_client = nullptr;

struct Recorder : RequestHandler {
    std::vector<int>      statuses;
    std::string           file;  // bytes of the file being uploaded
    std::vector<std::string> files;
    unsigned long         timeout_at_start = 0, timeout_at_end = 0;
    bool                  throw_on_write = false;  // an upload callback that runs out of memory
    // A model of FluidNC's /api/v2/files upload state (ApiV2.cpp uploadFiles): START
    // with an upload already started is refused as a second file part; only ABORTED
    // (or the request handler, which runs only after a successful parse) clears it.
    bool                  v2_started  = false;
    int                   v2_rejected = 0;
    int count(int status) const {
        int n = 0;
        for (int s : statuses) n += s == status;
        return n;
    }
    bool canHandle(HTTPMethod, String) override { return true; }
    bool canUpload(String) override { return true; }
    void upload(WebServer&, String, HTTPUpload& up) override {
        statuses.push_back(up.status);
        if (up.status == UPLOAD_FILE_START) {
            if (v2_started) ++v2_rejected;
            v2_started = true;
            file.clear();
            timeout_at_start = g_client->getTimeout();
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (throw_on_write) throw std::bad_alloc();
            file.append((const char*)up.buf, up.currentSize);
        } else if (up.status == UPLOAD_FILE_END) {  // the core repeats the last buffer here; not data
            timeout_at_end = g_client->getTimeout();
            files.push_back(file);
        } else if (up.status == UPLOAD_FILE_ABORTED) {
            v2_started = false;
        }
    }
};

struct TestServer : WebServer {
    Recorder rec;
    TestServer() : WebServer(80) {
        _firstHandler = &rec;
        _lastHandler  = &rec;
    }
    ~TestServer() { _firstHandler = _lastHandler = nullptr; }
    bool parse(WiFiClient& c) { return _parseRequest(c); }
    bool postArgsFreed() const { return _postArgs == nullptr && _postArgsLen == 0; }
    int  argCount() const { return _currentArgCount; }
    // new T[n] of a type with a destructor returns the block plus an array cookie
    // (16 bytes for RequestArgument here), so look for a block that starts up to 16
    // bytes before the pointer.
    bool currentArgsLive() const {
        if (_currentArgs == nullptr) return true;
        for (size_t off = 0; off <= 16; off += 8)
            if (live_arrays().count((char*)_currentArgs - off) == 1) return true;
        return false;
    }
};

struct Outcome {
    bool          returned = false;  // the parser returned (did not hang)
    bool          ok       = false;  // its return value
    unsigned long elapsed  = 0;      // simulated ms
    bool          postArgsFreed = false;
};

static const char* BOUNDARY = "----X7bQ";

static std::string request(const std::string& body, const std::string& query = "") {
    std::string r = "POST /api/v2/files" + query + " HTTP/1.1\r\n";
    r += "Host: plotter\r\n";
    r += std::string("Content-Type: multipart/form-data; boundary=") + BOUNDARY + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    return r + body;
}

static std::string part_field(const std::string& name, const std::string& value) {
    return std::string("--") + BOUNDARY + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value +
           "\r\n";
}

static std::string part_file(const std::string& name, const std::string& fname, const std::string& content) {
    return std::string("--") + BOUNDARY + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" +
           fname + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + content + "\r\n";
}

static std::string plain_request(const std::string& body, const std::string& query = "") {
    std::string r = "POST /api/v2/jobs" + query + " HTTP/1.1\r\n";
    r += "Host: plotter\r\n";
    r += "Content-Type: text/plain\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    return r + body;
}

static std::string closing() { return std::string("--") + BOUNDARY + "--\r\n"; }

// Runs one parse. The handler sees the client through g_client.
static Outcome run(TestServer& srv, WiFiClient& c, unsigned long hang_limit_ms = 600000) {
    Outcome o;
    fake_millis        = 0;
    fake_hang_limit_ms = hang_limit_ms;
    g_client           = &c;
    c.setTimeout(HTTP_MAX_POST_WAIT / 1000);  // as handleClient now does before _parseRequest
    try {
        o.ok       = srv.parse(c);
        o.returned = true;
    } catch (const FakeHang&) {
        o.returned = false;
    }
    o.elapsed       = fake_millis;
    o.postArgsFreed = srv.postArgsFreed();
    g_client        = nullptr;
    return o;
}

static std::string gcode(size_t n) {
    std::string s;
    while (s.size() < n) {
        s += "G1 X10 Y20 F3000\r\n--not-a-boundary\r\n-\r";
    }
    s.resize(n);
    return s;
}

// ---- Pure guard -------------------------------------------------------------------

static void guard_cases() {
    mp_line_guard_t g = mp_line_guard_make(true);
    CHECK(!mp_line_guard_give_up(&g, false, true, false), "a Content-Disposition line is fine");
    CHECK(mp_line_guard_give_up(&g, true, false, true), "an empty line from a gone peer gives up at once");

    g = mp_line_guard_make(true);
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "1st empty line");
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "2nd empty line");
    CHECK(mp_line_guard_give_up(&g, true, false, false), "3rd empty line in a row gives up");

    g = mp_line_guard_make(false);  // value loop: only the empty-line rules
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "value: 1st empty");
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "value: 2nd empty");
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "value: text resets the empty run");
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "value: empty after text");
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "value: 2nd empty after text");
    CHECK(mp_line_guard_give_up(&g, true, false, false), "value: 3rd empty gives up");
    for (int i = 0; i < 300; ++i) {  // non-empty value lines never trip the guard
        g = i == 0 ? mp_line_guard_make(false) : g;
        CHECK(!mp_line_guard_give_up(&g, false, false, false), "value line %d", i);
    }
    g = mp_line_guard_make(false);
    CHECK(mp_line_guard_give_up(&g, true, false, true), "value: empty from a gone peer gives up");

    g = mp_line_guard_make(true);
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "1st other header");
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "2nd other header");
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "3rd other header");
    CHECK(mp_line_guard_give_up(&g, false, false, false), "4th non-disposition line gives up");

    g = mp_line_guard_make(true);
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "other");
    CHECK(!mp_line_guard_give_up(&g, true, false, false), "empty counts as non-disposition");
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "other");
    CHECK(mp_line_guard_give_up(&g, false, false, false), "4th mixed non-disposition line gives up");

    g = mp_line_guard_make(true);
    for (int part = 0; part < 50; ++part) {  // many parts, each with up to 3 lines before it
        CHECK(!mp_line_guard_give_up(&g, false, false, false), "part %d other 1", part);
        CHECK(!mp_line_guard_give_up(&g, true, false, false), "part %d empty", part);
        CHECK(!mp_line_guard_give_up(&g, false, false, false), "part %d other 2", part);
        CHECK(!mp_line_guard_give_up(&g, false, true, false), "part %d disposition resets", part);
    }

    g = mp_line_guard_make(true);
    CHECK(!mp_line_guard_give_up(&g, true, true, false), "an empty line is never a disposition");
    CHECK(!mp_line_guard_give_up(&g, true, true, false), "2nd");
    CHECK(mp_line_guard_give_up(&g, true, true, false), "3rd empty gives up even if flagged");

    g = mp_line_guard_make(true);
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "other 1");
    CHECK(!mp_line_guard_give_up(&g, false, false, false), "other 2");
    CHECK(!mp_line_guard_give_up(&g, true, true, false), "an empty line flagged as disposition does not reset");
    CHECK(mp_line_guard_give_up(&g, false, false, false), "so the 4th non-disposition line gives up");
}

// ---- Legitimate forms still parse -------------------------------------------------

static void legit_fluidnc_v1() {
    // The v1 WebUI shape: path, <name>S, then the file.
    TestServer  srv;
    std::string content = gcode(5000);
    std::string body    = part_field("path", "/") + part_field("/job.gcodeS", std::to_string(content.size())) +
                       part_file("myfile[]", "/job.gcode", content) + closing();
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && o.ok, "v1 upload parses (returned %d ok %d)", o.returned, o.ok);
    CHECK(srv.rec.files.size() == 1 && srv.rec.files[0] == content, "v1 file content intact (%zu files)",
          srv.rec.files.size());
    CHECK(srv.hasArg("path") && srv.arg("path") == "/", "path argument kept");
    CHECK(srv.arg("/job.gcodeS") == String(std::to_string(content.size()).c_str()), "size argument kept");
    CHECK(o.postArgsFreed, "post args freed on success");
    CHECK(o.elapsed < 5000, "a complete request takes no timeouts (%lu ms)", o.elapsed);
    CHECK(srv.rec.timeout_at_start == HTTP_MAX_POST_WAIT, "part headers use the short timeout (%lu)",
          srv.rec.timeout_at_start);
    CHECK(srv.rec.timeout_at_end == HTTP_MAX_SEND_WAIT, "the file body uses the long timeout (%lu)",
          srv.rec.timeout_at_end);
    CHECK(c.getTimeout() == HTTP_MAX_POST_WAIT, "back to the short timeout after the body (%lu)", c.getTimeout());
}

static void legit_v2_single_file() {
    TestServer  srv;
    std::string content = gcode(1867);
    WiFiClient  c(request(part_file("file", "job.gcode", content) + closing()), FAKE_CLOSE);
    Outcome     o = run(srv, c);
    CHECK(o.returned && o.ok, "v2 single-file upload parses");
    CHECK(srv.rec.files.size() == 1 && srv.rec.files[0] == content, "v2 file content intact");
    CHECK(srv.rec.statuses.size() >= 2 && srv.rec.statuses.front() == UPLOAD_FILE_START &&
              srv.rec.statuses.back() == UPLOAD_FILE_END,
          "START ... END");
}

static void legit_extra_part_headers() {
    // Some clients put other headers before Content-Disposition; up to three are fine.
    TestServer  srv;
    std::string content = "G0 X0\n";
    std::string body    = std::string("--") + BOUNDARY +
                       "\r\nContent-Type: text/plain\r\nX-A: 1\r\nX-B: 2\r\nContent-Disposition: form-data; "
                       "name=\"note\"\r\n\r\nhello\r\n" +
                       part_file("file", "a.nc", content) + closing();
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && o.ok, "three headers before Content-Disposition still parse");
    CHECK(srv.arg("note") == "hello", "field read after extra headers");
    CHECK(srv.rec.files.size() == 1 && srv.rec.files[0] == content, "file after extra headers");
}

static void legit_two_files_and_blank_value_lines() {
    TestServer  srv;
    std::string a = gcode(3000), b = gcode(10);
    std::string body =
        part_field("note", "line1\r\n\r\n\r\nline4") + part_file("f1", "a.nc", a) + part_file("f2", "b.nc", b) + closing();
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && o.ok, "two files and a value with two blank lines parse");
    CHECK(srv.arg("note") == "line1\n\n\nline4", "value with blank lines kept: [%s]", srv.arg("note").c_str());
    CHECK(srv.rec.files.size() == 2 && srv.rec.files[0] == a && srv.rec.files[1] == b, "both files intact");
}

static void value_with_three_blank_lines_is_refused() {
    // Documented limitation of the guard.
    TestServer srv;
    std::string body = part_field("note", "x\r\n\r\n\r\n\r\ny") + closing();
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "three blank lines in a value are refused");
    CHECK(o.postArgsFreed, "post args freed");
}

// ---- Stalled, closed and junk clients give up --------------------------------------

static std::string head_only() { return std::string("--") + BOUNDARY + "\r\n"; }

static void stall_in_part_headers_silent() {
    // Headers and the first boundary, then nothing; the connection stays open.
    TestServer srv;
    WiFiClient c(request(head_only() + std::string()), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned, "silent client in part headers: parser returns (elapsed %lu ms)", o.elapsed);
    CHECK(!o.ok, "and fails");
    // Three empty lines, two 5 s reads each.
    CHECK(o.elapsed <= 3 * 2 * HTTP_MAX_POST_WAIT + 100, "within 30 s (took %lu ms)", o.elapsed);
    CHECK(o.postArgsFreed, "post args freed after the stall");
    CHECK(srv.rec.statuses.empty(), "no upload started");
}

static void stall_in_part_headers_closed() {
    TestServer srv;
    WiFiClient c(request(head_only()), FAKE_CLOSE);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "closed client in part headers: parser fails (returned %d)", o.returned);
    // One empty line (two reads) and the peer is gone.
    CHECK(o.elapsed <= 2 * HTTP_MAX_POST_WAIT + 100, "within one empty line (took %lu ms)", o.elapsed);
    CHECK(o.postArgsFreed, "post args freed after the close");
}

static void junk_part_headers() {
    // Non-empty lines that are never Content-Disposition, then silence.
    TestServer  srv;
    std::string junk = head_only();
    for (int i = 0; i < 4; ++i) junk += "X-Junk: " + std::to_string(i) + "\r\n";
    WiFiClient c(request(junk), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "junk part headers: parser fails (returned %d)", o.returned);
    CHECK(o.elapsed < 1000, "on the 4th junk line, with no timeout (took %lu ms)", o.elapsed);
    CHECK(o.postArgsFreed, "post args freed");
}

static void stall_in_field_value() {
    TestServer  srv;
    std::string body = std::string("--") + BOUNDARY + "\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n/";
    WiFiClient  c(request(body), FAKE_SILENT);
    Outcome     o = run(srv, c);
    CHECK(o.returned && !o.ok, "silent client in a field value: parser fails (returned %d)", o.returned);
    CHECK(o.elapsed <= 4 * 2 * HTTP_MAX_POST_WAIT + 100, "bounded (took %lu ms)", o.elapsed);
    CHECK(o.postArgsFreed, "post args freed");
}

static void closed_in_field_value() {
    TestServer  srv;
    std::string body = std::string("--") + BOUNDARY + "\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n/";
    WiFiClient  c(request(body), FAKE_CLOSE);
    Outcome     o = run(srv, c);
    CHECK(o.returned && !o.ok, "closed client in a field value: parser fails (returned %d)", o.returned);
    CHECK(o.elapsed <= 2 * 2 * HTTP_MAX_POST_WAIT + 100, "bounded (took %lu ms)", o.elapsed);
}

static void stall_after_file_part() {
    // A complete file part, then the client goes quiet instead of sending the closing boundary.
    TestServer  srv;
    std::string body = part_file("file", "a.nc", gcode(100)) + std::string("--") + BOUNDARY + "\r\n";
    WiFiClient  c(request(body), FAKE_SILENT);
    Outcome     o = run(srv, c);
    CHECK(o.returned && !o.ok, "silent after a file part: parser fails (returned %d)", o.returned);
    CHECK(o.elapsed <= 3 * 2 * HTTP_MAX_POST_WAIT + 100, "with the short timeout (took %lu ms)", o.elapsed);
    CHECK(o.postArgsFreed, "post args freed");
}

static void closed_mid_file_body() {
    TestServer  srv;
    std::string body = std::string("--") + BOUNDARY +
                       "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.nc\"\r\n\r\n" + gcode(2000);
    WiFiClient c(request(body), FAKE_CLOSE);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "closed mid-body: parser fails");
    CHECK(!srv.rec.statuses.empty() && srv.rec.statuses.back() == UPLOAD_FILE_ABORTED, "handler told ABORTED");
    CHECK(srv.rec.count(UPLOAD_FILE_ABORTED) == 1, "exactly once (%d)", srv.rec.count(UPLOAD_FILE_ABORTED));
    CHECK(o.postArgsFreed, "post args freed after an aborted body (leaked before)");
}

static void stall_mid_file_body_keeps_long_timeout() {
    TestServer  srv;
    std::string body = std::string("--") + BOUNDARY +
                       "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.nc\"\r\n\r\n" + gcode(2000);
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "silent mid-body: parser fails");
    CHECK(o.elapsed >= HTTP_MAX_SEND_WAIT, "the body waits the full long timeout (took %lu ms)", o.elapsed);
    CHECK(!srv.rec.statuses.empty() && srv.rec.statuses.back() == UPLOAD_FILE_ABORTED, "handler told ABORTED");
    CHECK(o.postArgsFreed, "post args freed");
}

static void too_many_fields() {
    TestServer  srv;
    std::string body;
    for (int i = 0; i < 40; ++i) body += part_field("k" + std::to_string(i), "v");
    body += closing();
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "too many fields: parser fails");
    CHECK(o.postArgsFreed, "post args freed on the too-many-fields exit (leaked before)");
}

static void bad_first_boundary() {
    TestServer srv;
    WiFiClient c(request("--wrong\r\n"), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "wrong first boundary fails");
    CHECK(o.postArgsFreed, "post args freed");
}

static void stale_post_args_freed_by_next_failure() {
    // A request that fails leaves nothing behind even if the previous one left args.
    TestServer srv;
    {
        WiFiClient c(request(part_field("a", "1") + head_only()), FAKE_CLOSE);
        Outcome    o = run(srv, c);
        CHECK(o.returned && !o.ok && o.postArgsFreed, "first failing request frees its args");
    }
    {
        WiFiClient c(request(part_field("a", "1") + closing()), FAKE_SILENT);
        Outcome    o = run(srv, c);
        CHECK(o.returned && o.ok && o.postArgsFreed, "a following good request still works");
    }
}

static void bad_alloc_from_upload_callback_frees_post_args() {
    // FluidNC catches std::bad_alloc around handleClient and carries on, so an exception
    // out of an upload callback must not leave the post-argument array (about 1 KB)
    // allocated until the next multipart POST.
    TestServer srv;
    srv.rec.throw_on_write = true;
    std::string body = part_field("path", "/") + part_file("file", "a.nc", gcode(3000)) + closing();
    WiFiClient  c(request(body), FAKE_SILENT);
    fake_millis        = 0;
    fake_hang_limit_ms = 600000;
    g_client           = &c;
    c.setTimeout(HTTP_MAX_POST_WAIT / 1000);
    bool threw = false;
    try {
        srv.parse(c);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    g_client = nullptr;
    CHECK(threw, "the callback's bad_alloc reaches the caller");
    CHECK(srv.postArgsFreed(), "post args freed when an upload callback throws (leaked before)");
}

// Runs one parse expected to throw std::bad_alloc; true if it did.
static bool run_expect_bad_alloc(TestServer& srv, WiFiClient& c) {
    fake_millis        = 0;
    fake_hang_limit_ms = 600000;
    g_client           = &c;
    c.setTimeout(HTTP_MAX_POST_WAIT / 1000);
    bool threw = false;
    try {
        srv.parse(c);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_array_new_at = 0;
    g_client          = nullptr;
    return threw;
}

static void merged_args_allocation_failure_then_next_request() {
    // _parseForm merges the post arguments into a new _currentArgs. Array allocations in
    // this request: _parseArguments (1), the post arguments (2), the merged array (3).
    TestServer srv;
    int        before = double_deletes;
    {
        WiFiClient c(request(part_field("path", "/") + closing()), FAKE_SILENT);
        fail_nth_array_new(3);
        CHECK(run_expect_bad_alloc(srv, c), "the merged-array allocation failure reaches the caller");
        CHECK(srv.postArgsFreed(), "post args freed");
        CHECK(srv.currentArgsLive(), "_currentArgs is not left pointing at a freed array");
    }
    {
        WiFiClient c(request(part_field("path", "/") + closing()), FAKE_SILENT);
        Outcome    o = run(srv, c);
        CHECK(o.returned && o.ok, "the next request parses");
        CHECK(srv.arg("path") == "/", "and has its argument");
    }
    CHECK(double_deletes == before, "no array freed twice (%d)", double_deletes - before);
}

static void query_args_allocation_failure_leaves_no_count() {
    // _parseArguments: a failed allocation must not leave a count over a null array.
    TestServer srv;
    WiFiClient c(request(part_field("path", "/") + closing(), "?a=1&b=2"), FAKE_SILENT);
    fail_nth_array_new(1);
    CHECK(run_expect_bad_alloc(srv, c), "the argument-array allocation failure reaches the caller");
    CHECK(srv.argCount() == 0, "no argument count without an array (%d)", srv.argCount());
    if (srv.argCount() == 0) {  // else hasArg would index a null array
        CHECK(!srv.hasArg("a"), "and hasArg is safe");
    }
}

static void plain_body_freed_when_args_allocation_fails() {
    // A text/plain POST: the body buffer (malloc) is held while _parseArguments
    // allocates. A failure there must not leak it.
    TestServer  srv;
    std::string body(100000, 'x');
    size_t      before = heap_in_use();
    {
        WiFiClient c(plain_request(body), FAKE_SILENT);
        fail_nth_array_new(1);
        CHECK(run_expect_bad_alloc(srv, c), "the allocation failure reaches the caller");
    }
    size_t after = heap_in_use();
    CHECK(after < before + body.size() / 2, "the 100 KB body is freed (heap grew by %zu bytes)",
          after > before ? after - before : 0);
}

static void plain_body_still_parses() {
    TestServer srv;
    WiFiClient c(plain_request("{\"a\":1}", "?x=1"), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && o.ok, "a plain POST parses");
    CHECK(srv.arg("plain") == "{\"a\":1}" && srv.arg("x") == "1", "with its body and query");
}

static void aborted_after_completed_file_part() {
    // A file part completes (END), then the part headers never come.
    TestServer  srv;
    std::string body = part_file("file", "a.nc", gcode(100)) + std::string("--") + BOUNDARY + "\r\n";
    WiFiClient  c(request(body), FAKE_SILENT);
    Outcome     o = run(srv, c);
    CHECK(o.returned && !o.ok, "fails");
    CHECK(srv.rec.count(UPLOAD_FILE_END) == 1, "the file part ended");
    CHECK(srv.rec.count(UPLOAD_FILE_ABORTED) == 1 && srv.rec.statuses.back() == UPLOAD_FILE_ABORTED,
          "then exactly one ABORTED (%d)", srv.rec.count(UPLOAD_FILE_ABORTED));
    CHECK(o.postArgsFreed, "post args freed");
}

static void aborted_after_file_then_stalled_field() {
    TestServer  srv;
    std::string body = part_file("file", "a.nc", gcode(100)) + std::string("--") + BOUNDARY +
                       "\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n/";
    WiFiClient  c(request(body), FAKE_CLOSE);
    Outcome     o = run(srv, c);
    CHECK(o.returned && !o.ok, "fails");
    CHECK(srv.rec.count(UPLOAD_FILE_ABORTED) == 1 && srv.rec.statuses.back() == UPLOAD_FILE_ABORTED,
          "a field-value exit after a file part sends one ABORTED (%d)", srv.rec.count(UPLOAD_FILE_ABORTED));
}

static void aborted_after_file_then_too_many_fields() {
    TestServer  srv;
    std::string body = part_file("file", "a.nc", gcode(100));
    for (int i = 0; i < 40; ++i) body += part_field("k" + std::to_string(i), "v");
    body += closing();
    WiFiClient c(request(body), FAKE_SILENT);
    Outcome    o = run(srv, c);
    CHECK(o.returned && !o.ok, "fails");
    CHECK(srv.rec.count(UPLOAD_FILE_ABORTED) == 1 && srv.rec.statuses.back() == UPLOAD_FILE_ABORTED,
          "the too-many-fields exit after a file part sends one ABORTED (%d)", srv.rec.count(UPLOAD_FILE_ABORTED));
}

static void failed_request_then_valid_upload_v2_model() {
    // Codex F4: after a file part's END, a failed parse used to tell the handler nothing,
    // so FluidNC's v2 upload state stayed "started" and the next valid upload was refused
    // as a second file part.
    TestServer srv;
    {
        std::string body = part_file("file", "a.nc", gcode(100)) + std::string("--") + BOUNDARY + "\r\n";
        WiFiClient  c(request(body), FAKE_CLOSE);
        Outcome     o = run(srv, c);
        CHECK(o.returned && !o.ok, "the first request fails after its file part");
    }
    {
        std::string content = gcode(500);
        WiFiClient  c(request(part_file("file", "b.nc", content) + closing()), FAKE_SILENT);
        Outcome     o = run(srv, c);
        CHECK(o.returned && o.ok, "the next valid upload parses");
        CHECK(srv.rec.v2_rejected == 0, "and is not refused as a second file part (%d)", srv.rec.v2_rejected);
        CHECK(srv.rec.files.size() == 2 && srv.rec.files[1] == content, "its file is intact");
    }
}

static void no_aborted_for_a_previous_requests_upload() {
    // The upload object of an earlier request (still set when an exception skipped
    // handleClient's reset; the harness never resets it) must not draw an ABORTED for a
    // later request that has no file part.
    TestServer srv;
    {
        WiFiClient c(request(part_file("file", "a.nc", gcode(100)) + closing()), FAKE_SILENT);
        Outcome    o = run(srv, c);
        CHECK(o.returned && o.ok, "a good upload");
    }
    size_t n = srv.rec.statuses.size();
    {
        WiFiClient c(request(head_only()), FAKE_CLOSE);
        Outcome    o = run(srv, c);
        CHECK(o.returned && !o.ok, "a later request with no file part fails");
    }
    CHECK(srv.rec.statuses.size() == n, "and the handler hears nothing about the earlier upload (%zu new)",
          srv.rec.statuses.size() - n);
}

int main() {
    guard_cases();
    legit_fluidnc_v1();
    legit_v2_single_file();
    legit_extra_part_headers();
    legit_two_files_and_blank_value_lines();
    value_with_three_blank_lines_is_refused();
    stall_in_part_headers_silent();
    stall_in_part_headers_closed();
    junk_part_headers();
    stall_in_field_value();
    closed_in_field_value();
    stall_after_file_part();
    closed_mid_file_body();
    stall_mid_file_body_keeps_long_timeout();
    too_many_fields();
    bad_first_boundary();
    stale_post_args_freed_by_next_failure();
    bad_alloc_from_upload_callback_frees_post_args();
    merged_args_allocation_failure_then_next_request();
    query_args_allocation_failure_leaves_no_count();
    plain_body_freed_when_args_allocation_fails();
    plain_body_still_parses();
    aborted_after_completed_file_part();
    aborted_after_file_then_stalled_field();
    aborted_after_file_then_too_many_fields();
    failed_request_then_valid_upload_v2_model();
    no_aborted_for_a_previous_requests_upload();
    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
