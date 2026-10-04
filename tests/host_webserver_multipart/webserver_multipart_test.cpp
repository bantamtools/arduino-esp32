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
//     HTTP_MAX_SEND_WAIT inside it.
// Also checks the pure guard (detail/MultipartLineGuard.h) directly.

#include "WebServer.h"
#include "detail/MultipartLineGuard.h"

#include <cstdio>
#include <new>
#include <string>
#include <vector>

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
    bool canHandle(HTTPMethod, String) override { return true; }
    bool canUpload(String) override { return true; }
    void upload(WebServer&, String, HTTPUpload& up) override {
        statuses.push_back(up.status);
        if (up.status == UPLOAD_FILE_START) {
            file.clear();
            timeout_at_start = g_client->getTimeout();
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (throw_on_write) throw std::bad_alloc();
            file.append((const char*)up.buf, up.currentSize);
        } else if (up.status == UPLOAD_FILE_END) {  // the core repeats the last buffer here; not data
            timeout_at_end = g_client->getTimeout();
            files.push_back(file);
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
    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
