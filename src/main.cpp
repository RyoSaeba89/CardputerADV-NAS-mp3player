// WebDAV MP3 player for the M5Stack Cardputer ADV - stripped-down version.
// Based on CardputerADV-NAS-mp3player 1.4.0 (HardCore-Gamer). Kept: Wi-Fi, server address,
// folder browsing, playback. Removed: search, sleep timer, eco mode, resume on boot, seeking,
// clock, help screen.
// Audio: one task streams the network into a large ring buffer, another task decodes the MP3;
// drawing the screen or reading the keyboard can no longer interrupt the sound.
// One folder = one playlist: every MP3 of the folder is played in order, then playback stops.
#include <Arduino.h>
#include <M5Cardputer.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include <AudioFileSource.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int HEADER_H = 20;
constexpr int FOOTER_H = 15;
constexpr size_t MAX_ENTRIES = 1000;
constexpr uint32_t MIN_FREE_HEAP_FOR_LIST = 40 * 1024;
constexpr uint32_t NET_TIMEOUT_MS = 20000;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t C_BG = rgb565(0, 0, 0);
constexpr uint16_t C_PANEL = rgb565(28, 28, 30);
constexpr uint16_t C_PANEL_ALT = rgb565(44, 44, 46);
constexpr uint16_t C_TEXT = rgb565(245, 245, 247);
constexpr uint16_t C_DIM = rgb565(142, 142, 147);
constexpr uint16_t C_ACCENT = rgb565(10, 132, 255);
constexpr uint16_t C_SELECT = rgb565(10, 132, 255);
constexpr uint16_t C_WARN = rgb565(255, 214, 10);
constexpr uint16_t C_ERR = rgb565(255, 69, 58);
constexpr uint16_t C_SOFT = rgb565(72, 72, 74);
constexpr uint16_t C_GOOD = rgb565(48, 209, 88);
constexpr uint16_t C_EDGE = rgb565(56, 56, 58);
constexpr uint16_t C_TRACK = rgb565(44, 44, 46);
constexpr uint16_t C_ACCENT_DARK = rgb565(0, 64, 128);

struct ParsedUrl {
    bool ok = false;
    String scheme = "http";
    String user;
    String pass;
    String host;
    uint16_t port = 80;
    String path = "/";
};

// Names stored one after another in 4 KB blocks. With one String per entry
// (kept percent-encoded: 6 bytes per Cyrillic letter), folders with 500+
// entries ran out of memory and the end of the list was cut off.
class NamePool {
public:
    static constexpr size_t BLOCK = 4096;

    ~NamePool() { clear(); }

    void clear() {
        for (char *b : blocks_) {
            free(b);
        }
        blocks_.clear();
        blocks_.shrink_to_fit();
        used_ = BLOCK;
    }

    // Copies `name`; false if there is not enough memory.
    bool add(const char *name, uint32_t &ref) {
        size_t n = strlen(name) + 1;
        if (n > BLOCK) {
            return false;
        }
        if (used_ + n > BLOCK) {
            char *b = static_cast<char *>(malloc(BLOCK));
            if (!b) {
                return false;
            }
            blocks_.push_back(b);
            used_ = 0;
        }
        ref = ((blocks_.size() - 1) << 12) | used_;
        memcpy(blocks_.back() + used_, name, n);
        used_ += n;
        return true;
    }

    const char *get(uint32_t ref) const { return blocks_[ref >> 12] + (ref & 0xFFF); }

private:
    std::vector<char *> blocks_;
    size_t used_ = BLOCK;
};

// Folder entry: only the decoded name is stored; the URL is rebuilt from the
// current folder.
struct FileEntry {
    uint32_t name = 0;  // in entryNames
    bool dir = false;
    bool parent = false;
};

struct WifiItem {
    String ssid;
    int32_t rssi = 0;
    uint8_t enc = WIFI_AUTH_OPEN;
    bool saved = false;
    bool manual = false;
};

struct KeyEvent {
    bool pressed = false;
    bool enter = false;
    bool del = false;
    bool tab = false;
    bool space = false;
    bool fn = false;
    std::vector<char> chars;
};

enum class Screen { WifiList, TextInput, FileList, Player, Message };
enum class InputMode { None, WifiPassword, ManualSsid, NasUrl };

Preferences prefs;
Screen screen = Screen::Message;
Screen returnAfterMessage = Screen::WifiList;
InputMode inputMode = InputMode::None;

std::vector<WifiItem> wifiItems;
std::vector<FileEntry> entries;
NamePool entryNames;
bool listTruncated = false;

String savedSsid;
String savedPass;
String savedNas;
String pendingSsid;
String inputText;
String inputTitle;
bool inputSecret = false;

String currentUrl;  // folder being shown (ends with '/')
int selected = 0;
int scrollTop = 0;
int wifiSelected = 0;
int wifiScrollTop = 0;
int volume = 180;

uint32_t messageUntil = 0;
String messageTitle;
String messageBody;
bool needsRedraw = true;
bool bootAutoStartPending = false;

// Playlist = the folder where playback was started (names copied, the list
// can change while the music plays).
String playlistDir;
std::vector<uint32_t> playlist;
NamePool playlistNames;
int playIndex = -1;
int failedInRow = 0;

// --------------------------------------------------------------------- text / URL helpers

String toLowerCopy(String value) {
    value.toLowerCase();
    return value;
}

void removeLastUtf8(String &value) {
    if (value.isEmpty()) {
        return;
    }
    int i = value.length() - 1;
    while (i > 0 && ((static_cast<uint8_t>(value[i]) & 0xC0) == 0x80)) {
        --i;
    }
    value.remove(i);
}

void appendUtf8(String &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Numeric references (&#1055; or &#x41F;) sent by some servers.
String decodeNumericEntities(const String &value) {
    if (value.indexOf("&#") < 0) {
        return value;
    }
    String out;
    out.reserve(value.length());
    int i = 0;
    int len = value.length();
    while (i < len) {
        int semi = value.indexOf(';', i);
        if (value[i] == '&' && i + 2 < len && value[i + 1] == '#' && semi > i + 2 && semi - i < 12) {
            bool hex = value[i + 2] == 'x' || value[i + 2] == 'X';
            String digits = value.substring(i + (hex ? 3 : 2), semi);
            uint32_t cp = strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
            if (cp > 0 && cp < 0x110000) {
                appendUtf8(out, cp);
                i = semi + 1;
                continue;
            }
        }
        out += value[i++];
    }
    return out;
}

String xmlHtmlDecode(String value) {
    value = decodeNumericEntities(value);
    value.replace("&amp;", "&");
    value.replace("&lt;", "<");
    value.replace("&gt;", ">");
    value.replace("&quot;", "\"");
    value.replace("&#39;", "'");
    value.replace("&apos;", "'");
    return value;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

String percentDecode(const String &value) {
    String out;
    out.reserve(value.length());
    for (int i = 0; i < static_cast<int>(value.length()); ++i) {
        char c = value[i];
        if (c == '%' && i + 2 < static_cast<int>(value.length())) {
            int hi = hexValue(value[i + 1]);
            int lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += c == '+' ? ' ' : c;
    }
    return out;
}

// Percent-decoding of a URL path: unlike percentDecode, '+' stays '+' (in a
// path it is a real '+'). keepSlash leaves "%2F" encoded so that a '/' inside a
// name is not mistaken for a folder separator.
String pathDecode(const String &value, bool keepSlash = false) {
    String out;
    out.reserve(value.length());
    for (int i = 0; i < static_cast<int>(value.length()); ++i) {
        char c = value[i];
        if (c == '%' && i + 2 < static_cast<int>(value.length())) {
            int hi = hexValue(value[i + 1]);
            int lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0 && !(keepSlash && ((hi << 4) | lo) == '/')) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// Name -> URL path segment: everything except letters, digits and -._~ is
// percent-encoded.
String encodeSeg(const char *name) {
    static const char *hex = "0123456789ABCDEF";
    String out;
    out.reserve(strlen(name) * 3);
    for (const char *p = name; *p; ++p) {
        uint8_t c = static_cast<uint8_t>(*p);
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

String trimCopy(String value) {
    value.trim();
    return value;
}

ParsedUrl parseUrl(String raw) {
    ParsedUrl out;
    raw = trimCopy(raw);
    if (raw.isEmpty()) {
        return out;
    }
    if (raw.indexOf("://") < 0) {
        raw = "http://" + raw;
    }
    int schemeEnd = raw.indexOf("://");
    if (schemeEnd <= 0) {
        return out;
    }
    out.scheme = raw.substring(0, schemeEnd);
    out.scheme.toLowerCase();
    if (out.scheme != "http") {
        return out;
    }
    int authorityStart = schemeEnd + 3;
    int pathStart = raw.indexOf('/', authorityStart);
    String authority = pathStart >= 0 ? raw.substring(authorityStart, pathStart) : raw.substring(authorityStart);
    out.path = pathStart >= 0 ? raw.substring(pathStart) : "/";
    if (out.path.isEmpty()) {
        out.path = "/";
    }
    int at = authority.lastIndexOf('@');
    if (at >= 0) {
        String userInfo = authority.substring(0, at);
        authority = authority.substring(at + 1);
        int colon = userInfo.indexOf(':');
        if (colon >= 0) {
            out.user = percentDecode(userInfo.substring(0, colon));
            out.pass = percentDecode(userInfo.substring(colon + 1));
        } else {
            out.user = percentDecode(userInfo);
        }
    }
    int portSep = authority.lastIndexOf(':');
    if (portSep > 0) {
        out.host = authority.substring(0, portSep);
        int parsedPort = authority.substring(portSep + 1).toInt();
        if (parsedPort > 0 && parsedPort <= 65535) {
            out.port = static_cast<uint16_t>(parsedPort);
        }
    } else {
        out.host = authority;
    }
    out.host.trim();
    out.ok = !out.host.isEmpty();
    return out;
}

String buildOrigin(const ParsedUrl &url, bool includeAuth) {
    String out = url.scheme + "://";
    if (includeAuth && !url.user.isEmpty()) {
        out += url.user;
        if (!url.pass.isEmpty()) {
            out += ":";
            out += url.pass;
        }
        out += "@";
    }
    out += url.host;
    if (url.port != 80) {
        out += ":";
        out += String(url.port);
    }
    return out;
}

String cleanUrlPath(String path) {
    int query = path.indexOf('?');
    if (query >= 0) {
        path.remove(query);
    }
    int fragment = path.indexOf('#');
    if (fragment >= 0) {
        path.remove(fragment);
    }
    return path;
}

String normalizeDirUrl(const String &raw) {
    String url = trimCopy(raw);
    if (url.indexOf("://") < 0) {
        url = "http://" + url;
    }
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return url;
    }
    String path = cleanUrlPath(parsed.path);
    if (!path.endsWith("/")) {
        path += "/";
    }
    return buildOrigin(parsed, true) + path;
}

String joinUrl(const String &baseUrl, String href) {
    href = xmlHtmlDecode(trimCopy(href));
    if (href.startsWith("//")) {
        return "http:" + href;
    }
    if (href.startsWith("http://") || href.startsWith("https://")) {
        ParsedUrl base = parseUrl(baseUrl);
        ParsedUrl target = parseUrl(href);
        if (base.ok && target.ok && !base.user.isEmpty() && base.host == target.host && base.port == target.port) {
            return buildOrigin(base, true) + target.path;
        }
        return href;
    }
    ParsedUrl base = parseUrl(baseUrl);
    if (!base.ok) {
        return href;
    }
    String path;
    if (href.startsWith("/")) {
        path = href;
    } else {
        path = cleanUrlPath(base.path);
        if (!path.endsWith("/")) {
            int slash = path.lastIndexOf('/');
            path = slash >= 0 ? path.substring(0, slash + 1) : "/";
        }
        path += href;
    }
    return buildOrigin(base, true) + path;
}

String lastSegment(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    String path = cleanUrlPath(parsed.ok ? parsed.path : url);
    if (path.endsWith("/") && path.length() > 1) {
        path.remove(path.length() - 1);
    }
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

String segToName(const String &seg) {
    return pathDecode(xmlHtmlDecode(seg));
}

String parentUrlOf(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return url;
    }
    String path = cleanUrlPath(parsed.path);
    if (path.length() <= 1) {
        return buildOrigin(parsed, true) + "/";
    }
    if (path.endsWith("/")) {
        path.remove(path.length() - 1);
    }
    int slash = path.lastIndexOf('/');
    path = slash <= 0 ? "/" : path.substring(0, slash + 1);
    return buildOrigin(parsed, true) + path;
}

bool isRootUrl(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    return !parsed.ok || cleanUrlPath(parsed.path).length() <= 1;
}

bool samePathUrl(const String &a, const String &b) {
    ParsedUrl pa = parseUrl(a);
    ParsedUrl pb = parseUrl(b);
    if (!pa.ok || !pb.ok) {
        return a == b;
    }
    String ap = pathDecode(cleanUrlPath(pa.path), true);
    String bp = pathDecode(cleanUrlPath(pb.path), true);
    if (!ap.endsWith("/")) {
        ap += "/";
    }
    if (!bp.endsWith("/")) {
        bp += "/";
    }
    return pa.host == pb.host && pa.port == pb.port && ap == bp;
}

bool isDirectChildUrl(const String &baseUrl, const String &targetUrl, bool directory) {
    ParsedUrl base = parseUrl(baseUrl);
    ParsedUrl target = parseUrl(targetUrl);
    if (!base.ok || !target.ok || base.host != target.host || base.port != target.port) {
        return false;
    }
    // decoded: the server may not encode names the way we do
    String basePath = pathDecode(cleanUrlPath(base.path), true);
    String targetPath = pathDecode(cleanUrlPath(target.path), true);
    if (!basePath.endsWith("/")) {
        basePath += "/";
    }
    if (!targetPath.startsWith(basePath) || targetPath == basePath) {
        return false;
    }
    String child = targetPath.substring(basePath.length());
    if (directory && child.endsWith("/")) {
        child.remove(child.length() - 1);
    }
    return !child.isEmpty() && child.indexOf('/') < 0;
}

bool pathIsMp3(const String &urlOrPath) {
    return toLowerCopy(cleanUrlPath(urlOrPath)).endsWith(".mp3");
}

// "Natural" sort, ignoring case: "2 - x" before "10 - x".
bool naturalLess(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = static_cast<unsigned char>(*a);
        unsigned char cb = static_cast<unsigned char>(*b);
        if (isdigit(ca) && isdigit(cb)) {
            uint32_t na = 0;
            uint32_t nb = 0;
            while (isdigit(static_cast<unsigned char>(*a))) {
                na = na * 10 + (*a++ - '0');
            }
            while (isdigit(static_cast<unsigned char>(*b))) {
                nb = nb * 10 + (*b++ - '0');
            }
            if (na != nb) {
                return na < nb;
            }
            continue;
        }
        if (ca < 0x80) {
            ca = static_cast<unsigned char>(tolower(ca));
        }
        if (cb < 0x80) {
            cb = static_cast<unsigned char>(tolower(cb));
        }
        if (ca != cb) {
            return ca < cb;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b != 0;
}

String formatTime(uint32_t ms) {
    uint32_t s = ms / 1000;
    char buf[16];
    if (s >= 3600) {
        snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
                 static_cast<unsigned long>((s / 60) % 60), static_cast<unsigned long>(s % 60));
    } else {
        snprintf(buf, sizeof(buf), "%02lu:%02lu", static_cast<unsigned long>(s / 60),
                 static_cast<unsigned long>(s % 60));
    }
    return String(buf);
}

// --------------------------------------------------------------------- audio engine

// Ring buffer: the network task writes, the decode task reads.
uint8_t *ringBuf = nullptr;
size_t ringCap = 0;
std::atomic<uint32_t> ringHead{0};
std::atomic<uint32_t> ringTail{0};

inline size_t ringAvail() {
    return ringHead.load() - ringTail.load();
}

inline size_t ringFree() {
    return ringCap - ringAvail();
}

void ringPeek(uint8_t *dst, size_t len) {
    uint32_t tail = ringTail.load();
    for (size_t i = 0; i < len; ++i) {
        dst[i] = ringBuf[(tail + i) % ringCap];
    }
}

void ringDiscard(size_t len) {
    ringTail.store(ringTail.load() + len);
}

WiFiClient netClient;
HTTPClient netHttp;
Client *netStream = nullptr;
SemaphoreHandle_t netMutex = nullptr;
std::atomic<bool> netActive{false};
std::atomic<bool> netEof{false};
std::atomic<bool> netError{false};
std::atomic<uint32_t> netTotal{0};
std::atomic<uint32_t> netReceived{0};

enum PlayState : int { PS_IDLE = 0, PS_PLAYING, PS_PAUSED, PS_DONE };
std::atomic<int> playState{PS_IDLE};
std::atomic<bool> stopReq{false};
std::atomic<uint32_t> underruns{0};
SemaphoreHandle_t decMutex = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;

String trackName;
uint32_t trackDurationMs = 0;
uint32_t trackKbps = 0;

void netTask(void *) {
    uint32_t lastData = millis();
    for (;;) {
        if (!netActive.load()) {
            lastData = millis();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        bool worked = false;
        xSemaphoreTake(netMutex, portMAX_DELAY);
        if (netActive.load() && netStream != nullptr) {
            size_t freeBytes = ringFree();
            int avail = netStream->available();
            if (avail > 0 && freeBytes > 0) {
                uint32_t head = ringHead.load();
                size_t pos = head % ringCap;
                size_t n = std::min<size_t>(freeBytes, ringCap - pos);
                n = std::min<size_t>(n, static_cast<size_t>(avail));
                n = std::min<size_t>(n, 4096);
                int got = netStream->read(ringBuf + pos, n);
                if (got > 0) {
                    ringHead.store(head + got);
                    netReceived.store(netReceived.load() + got);
                    lastData = millis();
                    worked = true;
                }
            } else if (avail <= 0) {
                uint32_t total = netTotal.load();
                bool finished = (total > 0 && netReceived.load() >= total) || !netHttp.connected();
                if (finished) {
                    netEof.store(true);
                    netActive.store(false);
                } else if (millis() - lastData > NET_TIMEOUT_MS) {
                    netError.store(true);
                    netEof.store(true);
                    netActive.store(false);
                }
            } else {
                lastData = millis();  // buffer full: wait for the decoder
            }
        }
        xSemaphoreGive(netMutex);
        vTaskDelay(worked ? 1 : pdMS_TO_TICKS(4));
    }
}

// Source read by the decoder: waits for data instead of returning short reads
// (the MP3 decoder gives up after 3 incomplete reads in a row).
class RingSource : public AudioFileSource {
public:
    uint32_t read(void *data, uint32_t len) override {
        uint8_t *out = static_cast<uint8_t *>(data);
        uint32_t done = 0;
        while (done < len && !stopReq.load()) {
            size_t avail = ringAvail();
            if (avail == 0) {
                if (netEof.load()) {
                    break;
                }
                // Underrun: let the buffer refill before resuming.
                underruns.store(underruns.load() + 1);
                size_t target = std::min<size_t>(ringCap / 2, 32 * 1024);
                while (!stopReq.load() && !netEof.load() && ringAvail() < target) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                continue;
            }
            uint32_t tail = ringTail.load();
            size_t pos = tail % ringCap;
            size_t n = std::min<size_t>(avail, len - done);
            n = std::min<size_t>(n, ringCap - pos);
            memcpy(out + done, ringBuf + pos, n);
            ringTail.store(tail + n);
            done += n;
            pos_ += n;
        }
        return done;
    }
    uint32_t readNonBlock(void *data, uint32_t len) override {
        return read(data, len);
    }
    bool seek(int32_t, int) override {
        return false;
    }
    bool close() override {
        return true;
    }
    bool isOpen() override {
        return true;
    }
    uint32_t getSize() override {
        return netTotal.load();
    }
    uint32_t getPos() override {
        return pos_;
    }
    void reset(uint32_t startPos) {
        pos_ = startPos;
    }

private:
    uint32_t pos_ = 0;
};

RingSource ringSource;

// Output to the speaker (ES8311 codec through M5Unified). playRaw() waits for a free queue
// slot: this is what paces the decode task.
class AudioOutputM5Speaker : public AudioOutput {
public:
    bool begin() override {
        return true;
    }
    bool ConsumeSample(int16_t sample[2]) override {
        if (bufPos_ + 1 >= BUF_SAMPLES) {
            flush();
            return false;
        }
        buf_[bufIndex_][bufPos_++] = sample[0];
        buf_[bufIndex_][bufPos_++] = sample[1];
        return true;
    }
    bool stop() override {
        bufPos_ = 0;
        return true;
    }
    bool SetRate(int hz) override {
        sampleRate_ = hz;
        return true;
    }
    bool SetBitsPerSample(int bits) override {
        return bits == 16;
    }
    bool SetChannels(int channels) override {
        return channels == 1 || channels == 2;
    }
    void flush() override {
        if (bufPos_ == 0) {
            return;
        }
        M5Cardputer.Speaker.playRaw(buf_[bufIndex_], bufPos_, sampleRate_, true, 1, 0);
        frames_.store(frames_.load() + bufPos_ / 2);
        bufIndex_ = (bufIndex_ + 1) % 3;
        bufPos_ = 0;
    }
    void resetCounter() {
        frames_.store(0);
        bufPos_ = 0;
    }
    uint32_t elapsedMs() const {
        return sampleRate_ > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(frames_.load()) * 1000ULL / sampleRate_) : 0;
    }

private:
    static constexpr size_t BUF_SAMPLES = 4096;  // 2048 stereo frames, ~46 ms at 44.1 kHz
    int16_t buf_[3][BUF_SAMPLES] = {};
    size_t bufPos_ = 0;
    uint8_t bufIndex_ = 0;
    int sampleRate_ = 44100;
    std::atomic<uint32_t> frames_{0};
};

AudioOutputM5Speaker out;

void decodeTask(void *) {
    for (;;) {
        if (playState.load() != PS_PLAYING || stopReq.load()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        xSemaphoreTake(decMutex, portMAX_DELAY);
        if (mp3 != nullptr && playState.load() == PS_PLAYING && !stopReq.load()) {
            if (!mp3->loop() && !stopReq.load()) {
                out.flush();
                playState.store(PS_DONE);
            }
        }
        xSemaphoreGive(decMutex);
        taskYIELD();
    }
}

uint32_t be32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Reads the first MP3 frame header: bitrate, and frame count (Xing/Info/VBRI header)
// for an exact duration, including variable bitrate files.
void parseMp3Header(const uint8_t *b, size_t n, uint32_t audioBytes) {
    static const uint16_t br1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t br2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t sr[3] = {44100, 48000, 32000};
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        int ver = (b[i + 1] >> 3) & 3;  // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
        int layer = (b[i + 1] >> 1) & 3;  // 1 = Layer III
        int brIdx = (b[i + 2] >> 4) & 0xF;
        int srIdx = (b[i + 2] >> 2) & 3;
        int chMode = (b[i + 3] >> 6) & 3;
        if (ver == 1 || layer != 1 || brIdx == 0 || brIdx == 15 || srIdx == 3) {
            continue;
        }
        uint32_t rate = sr[srIdx] >> (ver == 3 ? 0 : (ver == 2 ? 1 : 2));
        uint32_t kbps = ver == 3 ? br1[brIdx] : br2[brIdx];
        uint32_t spf = ver == 3 ? 1152 : 576;
        size_t side = ver == 3 ? (chMode == 3 ? 17 : 32) : (chMode == 3 ? 9 : 17);
        trackKbps = kbps;
        size_t x = i + 4 + side;
        if (x + 12 <= n && (memcmp(b + x, "Xing", 4) == 0 || memcmp(b + x, "Info", 4) == 0)) {
            uint32_t flags = be32(b + x + 4);
            if (flags & 1) {
                uint32_t frames = be32(b + x + 8);
                if (frames > 0) {
                    trackDurationMs = static_cast<uint32_t>(static_cast<uint64_t>(frames) * spf * 1000ULL / rate);
                    if (flags & 2 && trackDurationMs > 0) {
                        trackKbps = static_cast<uint32_t>(static_cast<uint64_t>(be32(b + x + 12)) * 8ULL / trackDurationMs);
                    }
                    return;
                }
            }
        }
        size_t v = i + 4 + 32;
        if (v + 18 <= n && memcmp(b + v, "VBRI", 4) == 0) {
            uint32_t frames = be32(b + v + 14);
            if (frames > 0) {
                trackDurationMs = static_cast<uint32_t>(static_cast<uint64_t>(frames) * spf * 1000ULL / rate);
                return;
            }
        }
        if (audioBytes > 0 && kbps > 0) {
            trackDurationMs = static_cast<uint32_t>(static_cast<uint64_t>(audioBytes) * 8ULL / kbps);
        }
        return;
    }
}

bool waitRing(size_t bytes, uint32_t timeoutMs) {
    uint32_t start = millis();
    while (ringAvail() < bytes && !netEof.load()) {
        if (millis() - start > timeoutMs) {
            return false;
        }
        M5Cardputer.update();
        delay(5);
    }
    return ringAvail() >= bytes || netEof.load();
}

void stopTrack() {
    stopReq.store(true);
    xSemaphoreTake(decMutex, portMAX_DELAY);
    if (mp3 != nullptr) {
        mp3->stop();
        delete mp3;
        mp3 = nullptr;
    }
    playState.store(PS_IDLE);
    xSemaphoreGive(decMutex);
    M5Cardputer.Speaker.stop();
    out.resetCounter();
    xSemaphoreTake(netMutex, portMAX_DELAY);
    netActive.store(false);
    netHttp.end();
    netStream = nullptr;
    ringHead.store(0);
    ringTail.store(0);
    netEof.store(false);
    netError.store(false);
    xSemaphoreGive(netMutex);
    stopReq.store(false);
}

bool openStream(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return false;
    }
    xSemaphoreTake(netMutex, portMAX_DELAY);
    netClient.setTimeout(10000);
    bool ok = netHttp.begin(netClient, buildOrigin(parsed, false) + parsed.path);
    if (ok) {
        netHttp.setTimeout(12000);
        netHttp.setReuse(false);
        netHttp.useHTTP10(true);
        netHttp.addHeader("User-Agent", "CardputerAdvMP3/2.0");
        if (!parsed.user.isEmpty()) {
            netHttp.setAuthorization(parsed.user.c_str(), parsed.pass.c_str());
        }
        int code = netHttp.GET();
        ok = code == HTTP_CODE_OK;
        if (ok) {
            netStream = netHttp.getStreamPtr();
            int size = netHttp.getSize();
            netTotal.store(size > 0 ? static_cast<uint32_t>(size) : 0);
            netReceived.store(0);
            ringHead.store(0);
            ringTail.store(0);
            netEof.store(false);
            netError.store(false);
            netActive.store(true);
        } else {
            netHttp.end();
        }
    }
    xSemaphoreGive(netMutex);
    return ok;
}

// --------------------------------------------------------------------- display

String displayFit(String value, int width) {
    if (M5Cardputer.Display.textWidth(value) <= width) {
        return value;
    }
    while (!value.isEmpty() && M5Cardputer.Display.textWidth(value + "...") > width) {
        removeLastUtf8(value);
    }
    return value + "...";
}

String tailFit(String value, int width) {
    if (M5Cardputer.Display.textWidth(value) <= width) {
        return value;
    }
    while (!value.isEmpty() && M5Cardputer.Display.textWidth("..." + value) > width) {
        int i = 1;
        while (i < static_cast<int>(value.length()) && ((static_cast<uint8_t>(value[i]) & 0xC0) == 0x80)) {
            ++i;
        }
        value.remove(0, i);
    }
    return "..." + value;
}

// Characters missing from the font would show as empty boxes: combining
// accents (names written in decomposed form) are merged with their letter,
// some Cyrillic letters get a look-alike, anything else becomes '?'.
uint32_t composeAccent(uint32_t base, uint32_t mark) {
    struct Combo {
        uint16_t mark;
        const char *bases;
        uint16_t first[12];
    };
    static const Combo combos[] = {
        {0x0300, "AEIOUaeiou", {0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9}},
        {0x0301, "AEIOUYaeiouy", {0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xDD, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFD}},
        {0x0302, "AEIOUaeiou", {0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB}},
        {0x0303, "ANOano", {0xC3, 0xD1, 0xD5, 0xE3, 0xF1, 0xF5}},
        {0x0308, "AEIOUaeiouy", {0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xFF}},
        {0x030A, "Aa", {0xC5, 0xE5}},
        {0x0327, "Cc", {0xC7, 0xE7}},
    };
    if (base < 0x80) {
        for (const auto &c : combos) {
            if (c.mark != mark) {
                continue;
            }
            const char *p = strchr(c.bases, static_cast<char>(base));
            if (p && base) {
                return c.first[p - c.bases];
            }
        }
    }
    if (mark == 0x0306) {  // breve: й Й ў Ў
        if (base == 0x0438) return 0x0439;
        if (base == 0x0418) return 0x0419;
        if (base == 0x0443) return 0x045E;
        if (base == 0x0423) return 0x040E;
    }
    if (mark == 0x0308) {  // diaeresis: ё Ё ї Ї
        if (base == 0x0435) return 0x0451;
        if (base == 0x0415) return 0x0401;
        if (base == 0x0456) return 0x0457;
        if (base == 0x0406) return 0x0407;
    }
    return 0;
}

const char *lookAlike(uint32_t cp) {
    switch (cp) {
        case 0x0400: return "\xC3\x88";  // Ѐ -> È
        case 0x0402: return "Dj";
        case 0x0403: return "\xD0\x93";  // Ѓ -> Г
        case 0x0404: return "\xD0\x95";  // Є -> Е
        case 0x0405: return "S";
        case 0x0406: return "I";
        case 0x0407: return "\xC3\x8F";  // Ї -> Ï
        case 0x0408: return "J";
        case 0x0409: return "Lj";
        case 0x040A: return "Nj";
        case 0x040B: return "C";
        case 0x040C: return "\xD0\x9A";  // Ќ -> К
        case 0x040D: return "\xD0\x98";  // Ѝ -> И
        case 0x040E: return "\xD0\xA3";  // Ў -> У
        case 0x040F: return "Dz";
        case 0x0450: return "\xC3\xA8";  // ѐ -> è
        case 0x0452: return "dj";
        case 0x0453: return "\xD0\xB3";  // ѓ -> г
        case 0x0454: return "\xD0\xB5";  // є -> е
        case 0x0455: return "s";
        case 0x0456: return "i";
        case 0x0457: return "\xC3\xAF";  // ї -> ï
        case 0x0458: return "j";
        case 0x0459: return "lj";
        case 0x045A: return "nj";
        case 0x045B: return "c";
        case 0x045C: return "\xD0\xBA";  // ќ -> к
        case 0x045D: return "\xD0\xB8";  // ѝ -> и
        case 0x045E: return "\xD1\x83";  // ў -> у
        case 0x045F: return "dz";
        case 0x0490: return "\xD0\x93";  // Ґ -> Г
        case 0x0491: return "\xD0\xB3";  // ґ -> г
        default: return nullptr;
    }
}

String displayName(const char *s) {
    const lgfx::IFont *font = M5Cardputer.Display.getFont();
    lgfx::FontMetrics m;
    std::vector<uint32_t> cps;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s);
    while (*p) {
        uint32_t cp = '?';
        int extra = 0;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            extra = 3;
        }
        ++p;
        for (int k = 0; k < extra; ++k) {
            if ((*p & 0xC0) != 0x80) {
                cp = '?';
                break;
            }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp >= 0x0300 && cp <= 0x036F) {  // combining accent
            uint32_t c = cps.empty() ? 0 : composeAccent(cps.back(), cp);
            if (c) {
                cps.back() = c;
            }
            continue;
        }
        cps.push_back(cp);
    }
    String out;
    for (uint32_t cp : cps) {
        if (cp < 0x80 || (cp <= 0xFFFF && font && font->updateFontMetric(&m, cp))) {
            appendUtf8(out, cp);
        } else if (const char *alt = lookAlike(cp)) {
            out += alt;
        } else {
            out += '?';
        }
    }
    return out;
}

bool charInEvent(const KeyEvent &key, char wanted) {
    char w = static_cast<char>(tolower(static_cast<unsigned char>(wanted)));
    for (char c : key.chars) {
        if (tolower(static_cast<unsigned char>(c)) == w) {
            return true;
        }
    }
    return false;
}

int getWifiSignalBars() {
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    long rssi = WiFi.RSSI();
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

void drawSignalGlyph(int x, int y, int bars, uint16_t active, uint16_t inactive) {
    for (int i = 0; i < 4; ++i) {
        int barH = 3 + i * 2;
        M5Cardputer.Display.fillRect(x + i * 4, y + (9 - barH), 3, barH, i < bars ? active : inactive);
    }
}

void drawHeader(const String &title) {
    auto &d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, HEADER_H, C_BG);
    d.setTextColor(C_TEXT, C_BG);
    d.setCursor(8, 4);
    d.print(displayFit(title, 150));
    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    int wifiX = SCREEN_W - 50;
    drawSignalGlyph(wifiX, 4, getWifiSignalBars(), wifiConnected ? C_TEXT : C_SOFT, C_SOFT);
    if (!wifiConnected) {
        d.drawLine(wifiX - 1, 14, wifiX + 14, 3, C_ERR);
    }
    int battery = std::max(0, std::min(100, static_cast<int>(M5Cardputer.Power.getBatteryLevel())));
    bool charging = M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
    int batX = SCREEN_W - 24;
    d.drawRoundRect(batX, 4, 17, 9, 2, C_DIM);
    d.fillRect(batX + 17, 6, 2, 5, C_DIM);
    uint16_t batColor = battery <= 10 ? C_ERR : battery > 25 ? (charging ? C_GOOD : C_DIM) : C_WARN;
    int fillW = (14 * battery) / 100;
    if (fillW > 0) {
        d.fillRect(batX + 2, 6, fillW, 6, batColor);
    }
}

void drawFooter(const String &text) {
    int y = SCREEN_H - FOOTER_H;
    M5Cardputer.Display.fillRect(0, y, SCREEN_W, FOOTER_H, C_BG);
    M5Cardputer.Display.drawFastHLine(0, y, SCREEN_W, C_EDGE);
    String shown = displayFit(text, SCREEN_W - 16);
    int textX = std::max(8, (SCREEN_W - M5Cardputer.Display.textWidth(shown)) / 2);
    M5Cardputer.Display.setTextColor(C_DIM, C_BG);
    M5Cardputer.Display.setCursor(textX, y + 3);
    M5Cardputer.Display.print(shown);
}

void drawMeter(int x, int y, int w, int h, int fillW, uint16_t fillColor) {
    M5Cardputer.Display.fillRoundRect(x, y, w, h, h / 2, C_TRACK);
    int innerW = std::max(0, std::min(fillW, w));
    if (innerW > 0) {
        M5Cardputer.Display.fillRoundRect(x, y, innerW, h, h / 2, fillColor);
    }
}

void showMessage(const String &title, const String &body, uint32_t ms, Screen next) {
    messageTitle = title;
    messageBody = body;
    messageUntil = millis() + ms;
    returnAfterMessage = next;
    screen = Screen::Message;
    needsRedraw = true;
}

void drawBusyScreen(const String &title, const String &body) {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader(title);
    M5Cardputer.Display.fillRoundRect(20, 40, 200, 50, 8, C_PANEL);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(32, 50);
    M5Cardputer.Display.print(displayFit(body, 176));
    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
    M5Cardputer.Display.setCursor(32, 68);
    M5Cardputer.Display.print("Please wait");
}

void drawMessage() {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader(messageTitle);
    M5Cardputer.Display.fillRoundRect(20, 40, 200, 50, 8, C_PANEL);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(32, 58);
    M5Cardputer.Display.print(displayFit(messageBody, 176));
    needsRedraw = false;
}

void beginInput(InputMode mode, const String &title, const String &seed, bool secret) {
    inputMode = mode;
    inputTitle = title;
    inputText = seed;
    inputSecret = secret;
    screen = Screen::TextInput;
    needsRedraw = true;
}

void drawInput() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader(inputTitle);
    String shown = inputText;
    if (inputSecret) {
        shown = "";
        for (size_t i = 0; i < inputText.length(); ++i) {
            shown += '*';
        }
    }
    d.fillRoundRect(8, 27, 224, 82, 8, C_PANEL);
    d.setTextColor(C_DIM, C_PANEL);
    d.setCursor(18, 35);
    if (inputMode == InputMode::WifiPassword) {
        d.print(displayFit(pendingSsid, 204));
    } else if (inputMode == InputMode::NasUrl) {
        d.print("HTTP / WebDAV address");
    } else {
        d.print("Network name");
    }
    d.fillRoundRect(16, 52, 208, 27, 7, C_PANEL_ALT);
    d.drawRoundRect(16, 52, 208, 27, 7, C_ACCENT);
    d.setTextColor(C_TEXT, C_PANEL_ALT);
    d.setCursor(25, 60);
    d.print(tailFit(shown, 190));
    d.setTextColor(C_DIM, C_PANEL);
    d.setCursor(18, 89);
    d.print(inputMode == InputMode::NasUrl ? displayFit("Example: http://nas:5005/music/", 204) : String("Saved after connection"));
    drawFooter("Enter Done     Tab Back");
    needsRedraw = false;
}

// --------------------------------------------------------------------- WebDAV listing

void addEntryIfUseful(const String &baseUrl, String href, bool forceDir) {
    href = xmlHtmlDecode(trimCopy(href));
    if (href.isEmpty()) {
        return;
    }
    String joined = joinUrl(baseUrl, href);
    bool dir = forceDir || cleanUrlPath(joined).endsWith("/");
    if (!dir && !pathIsMp3(joined)) {
        return;
    }
    if (!isDirectChildUrl(baseUrl, joined, dir) || (dir && samePathUrl(joined, baseUrl))) {
        return;
    }
    if (entries.size() >= MAX_ENTRIES || ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_LIST) {
        listTruncated = true;
        return;
    }
    String name = pathDecode(lastSegment(joined));
    if (name.isEmpty()) {
        return;
    }
    for (const auto &e : entries) {
        if (!e.parent && e.dir == dir && strcmp(entryNames.get(e.name), name.c_str()) == 0) {
            return;
        }
    }
    FileEntry entry;
    entry.dir = dir;
    if (!entryNames.add(name.c_str(), entry.name)) {
        listTruncated = true;
        return;
    }
    entries.push_back(entry);
}

// Text of the first opening <...href> tag of a <response> block (closing tags are skipped).
bool extractResponseHref(const String &block, const String &lower, String &hrefOut) {
    int pos = 0;
    while (true) {
        int tag = lower.indexOf("href", pos);
        if (tag < 0) {
            return false;
        }
        pos = tag + 4;
        int lt = lower.lastIndexOf('<', tag);
        int gt = lower.indexOf('>', tag);
        if (lt < 0 || gt < 0 || lower.indexOf('>', lt) < tag) {
            continue;
        }
        if (lower[lt + 1] == '/' || lower[gt - 1] == '/') {
            continue;
        }
        String tagName = lower.substring(lt + 1, gt);
        tagName.trim();
        int space = tagName.indexOf(' ');
        if (space >= 0) {
            tagName.remove(space);
        }
        if (!(tagName == "href" || tagName.endsWith(":href"))) {
            continue;
        }
        int close = lower.indexOf("</", gt + 1);
        if (close < 0) {
            return false;
        }
        hrefOut = block.substring(gt + 1, close);
        return true;
    }
}

int findResponseEnd(const String &lower) {
    int pos = 0;
    while (true) {
        int idx = lower.indexOf("response>", pos);
        if (idx < 0) {
            return -1;
        }
        int lt = lower.lastIndexOf('<', idx);
        if (lt >= 0 && lt + 1 < static_cast<int>(lower.length()) && lower[lt + 1] == '/' &&
            lower.indexOf('>', lt) == idx + 8) {
            return idx + 9;
        }
        pos = idx + 9;
    }
}

void parseWebDavStream(const String &baseUrl, HTTPClient &http) {
    Client *stream = http.getStreamPtr();
    if (!stream) {
        return;
    }
    String buf;
    buf.reserve(4096);
    char tmp[513];
    uint32_t lastData = millis();
    while (http.connected() || stream->available() > 0) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastData > 12000) {
                break;
            }
            delay(1);
            continue;
        }
        int got = stream->readBytes(reinterpret_cast<uint8_t *>(tmp), std::min(avail, sizeof(tmp) - 1));
        if (got <= 0) {
            continue;
        }
        tmp[got] = '\0';
        buf += tmp;
        lastData = millis();
        while (true) {
            String lower = toLowerCopy(buf);
            int end = findResponseEnd(lower);
            if (end < 0) {
                break;
            }
            String block = buf.substring(0, end);
            String blockLower = lower.substring(0, end);
            String href;
            if (extractResponseHref(block, blockLower, href)) {
                addEntryIfUseful(baseUrl, href, blockLower.indexOf("collection") >= 0);
            }
            buf.remove(0, end);
        }
        if (buf.length() > 16384) {
            buf.remove(0, buf.length() - 2048);
        }
    }
}

bool fetchWebDavListing(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return false;
    }
    WiFiClient client;
    HTTPClient http;
    client.setTimeout(10000);
    if (!http.begin(client, buildOrigin(parsed, false) + parsed.path)) {
        return false;
    }
    http.setTimeout(15000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.addHeader("User-Agent", "CardputerAdvMP3/2.0");
    if (!parsed.user.isEmpty()) {
        http.setAuthorization(parsed.user.c_str(), parsed.pass.c_str());
    }
    http.addHeader("Depth", "1");
    http.addHeader("Content-Type", "application/xml; charset=utf-8");
    const char *payload =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<propfind xmlns=\"DAV:\"><prop><resourcetype/></prop></propfind>";
    int code = http.sendRequest("PROPFIND", reinterpret_cast<uint8_t *>(const_cast<char *>(payload)), strlen(payload));
    bool ok = code == 207 || code == HTTP_CODE_OK;
    if (ok) {
        parseWebDavStream(url, http);
    }
    http.end();
    return ok;
}

void sortEntries() {
    std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.parent != b.parent) {
            return a.parent;
        }
        if (a.dir != b.dir) {
            return a.dir;
        }
        return naturalLess(entryNames.get(a.name), entryNames.get(b.name));
    });
}

const char *entryName(const FileEntry &e) {
    return e.parent ? "" : entryNames.get(e.name);
}

const char *playlistName(int index) {
    return playlistNames.get(playlist[index]);
}

bool isPlayingEntry(const FileEntry &e) {
    return playIndex >= 0 && playlistDir == currentUrl && !e.dir && !e.parent &&
           strcmp(entryName(e), playlistName(playIndex)) == 0;
}

String entryUrl(const FileEntry &e) {
    if (e.parent) {
        return parentUrlOf(currentUrl);
    }
    return currentUrl + encodeSeg(entryName(e)) + (e.dir ? "/" : "");
}

bool loadDirectory(const String &rawUrl) {
    String url = normalizeDirUrl(rawUrl);
    entries.clear();
    entries.shrink_to_fit();
    entryNames.clear();
    listTruncated = false;
    selected = 0;
    scrollTop = 0;
    String previous = currentUrl;
    currentUrl = url;
    if (!isRootUrl(url)) {
        FileEntry up;
        up.parent = true;
        up.dir = true;
        entries.push_back(up);
    }
    if (!fetchWebDavListing(url)) {
        currentUrl = previous;
        return false;
    }
    sortEntries();
    prefs.putString("nas", currentUrl);
    savedNas = currentUrl;
    // If this is the folder being played, select the current track.
    for (size_t i = 0; i < entries.size(); ++i) {
        if (isPlayingEntry(entries[i])) {
            selected = i;
            break;
        }
    }
    return true;
}

// --------------------------------------------------------------------- playback

void finishPlaylist(const String &why) {
    stopTrack();
    playIndex = -1;
    playlist.clear();
    playlistNames.clear();
    playlistDir = "";
    showMessage("Library", why, 1500, Screen::FileList);
}

bool startTrack(int index, bool showPlayer) {
    stopTrack();
    if (index < 0 || index >= static_cast<int>(playlist.size())) {
        return false;
    }
    playIndex = index;
    trackName = displayName(playlistName(index));
    trackDurationMs = 0;
    trackKbps = 0;
    underruns.store(0);
    if (showPlayer || screen == Screen::Player) {
        drawBusyScreen("Now Playing", trackName);
    }

    if (!openStream(playlistDir + encodeSeg(playlistName(index)))) {
        return false;
    }
    // ID3v2 tag (title, cover art...): skip it.
    uint32_t audioStart = 0;
    if (waitRing(10, 10000) && ringAvail() >= 10) {
        uint8_t h[10];
        ringPeek(h, 10);
        if (h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
            uint32_t size = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
            size += 10 + ((h[5] & 0x10) ? 10 : 0);
            audioStart = size;
            uint32_t left = size;
            uint32_t start = millis();
            while (left > 0 && millis() - start < 30000) {
                size_t n = std::min<size_t>(ringAvail(), left);
                if (n == 0) {
                    if (netEof.load()) {
                        break;
                    }
                    delay(5);
                    continue;
                }
                ringDiscard(n);
                left -= n;
            }
        }
    }
    // Pre-fill the buffer before starting the sound.
    waitRing(std::min<size_t>(ringCap * 3 / 4, 48 * 1024), 15000);
    if (ringAvail() == 0) {
        stopTrack();
        return false;
    }
    {
        size_t n = std::min<size_t>(ringAvail(), 4096);
        uint8_t *peek = static_cast<uint8_t *>(malloc(n));
        if (peek) {
            ringPeek(peek, n);
            uint32_t total = netTotal.load();
            parseMp3Header(peek, n, total > audioStart ? total - audioStart : 0);
            free(peek);
        }
    }
    ringSource.reset(audioStart);
    out.resetCounter();
    xSemaphoreTake(decMutex, portMAX_DELAY);
    mp3 = new AudioGeneratorMP3();
    bool ok = mp3->begin(&ringSource, &out);
    if (ok) {
        playState.store(PS_PLAYING);
    } else {
        delete mp3;
        mp3 = nullptr;
    }
    xSemaphoreGive(decMutex);
    if (!ok) {
        stopTrack();
        return false;
    }
    Serial.printf("[play] %d/%d %s | %lu kbps, %lu ms, id3 %lu, size %lu | ring %u/%u | heap %u (max block %u)\n",
                  index + 1, static_cast<int>(playlist.size()), trackName.c_str(),
                  static_cast<unsigned long>(trackKbps), static_cast<unsigned long>(trackDurationMs),
                  static_cast<unsigned long>(audioStart), static_cast<unsigned long>(netTotal.load()),
                  static_cast<unsigned>(ringAvail()), static_cast<unsigned>(ringCap), ESP.getFreeHeap(),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    if (showPlayer) {
        screen = Screen::Player;
    }
    needsRedraw = true;  // keep the list on screen if the user is browsing
    return true;
}

// Plays track `index`; if a file cannot be played, moves on to the next one.
void playFrom(int index, bool showPlayer) {
    while (index >= 0 && index < static_cast<int>(playlist.size())) {
        if (startTrack(index, showPlayer)) {
            failedInRow = 0;
            return;
        }
        if (WiFi.status() != WL_CONNECTED || ++failedInRow >= 3) {
            failedInRow = 0;
            finishPlaylist("Cannot play files");
            return;
        }
        ++index;
    }
    finishPlaylist("End of folder");
}

void playFolderFrom(int entryIndex) {
    stopTrack();
    playIndex = -1;
    playlist.clear();
    playlistNames.clear();
    playlistDir = currentUrl;
    int start = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].dir) {
            uint32_t ref;
            if (!playlistNames.add(entryName(entries[i]), ref)) {
                break;  // not enough memory: the playlist stops here
            }
            if (static_cast<int>(i) == entryIndex) {
                start = playlist.size();
            }
            playlist.push_back(ref);
        }
    }
    playFrom(start, true);
}

void togglePause() {
    int st = playState.load();
    if (st == PS_PLAYING) {
        playState.store(PS_PAUSED);
    } else if (st == PS_PAUSED) {
        playState.store(PS_PLAYING);
    }
    needsRedraw = true;
}

void drawPlayerDynamic() {
    auto &d = M5Cardputer.Display;
    uint32_t elapsed = out.elapsedMs();
    String timeText = formatTime(elapsed) + " / " + (trackDurationMs > 0 ? formatTime(trackDurationMs) : String("--:--"));
    if (trackKbps > 0) {
        timeText += "   " + String(trackKbps) + " kbps";
    }
    d.fillRect(8, 62, 224, 12, C_BG);
    d.setTextColor(C_TEXT, C_BG);
    d.setCursor(8, 62);
    d.print(timeText);
    int fill = trackDurationMs > 0 ? static_cast<int>(static_cast<uint64_t>(224) * std::min(elapsed, trackDurationMs) / trackDurationMs) : 0;
    drawMeter(8, 77, 224, 4, fill, C_ACCENT);

    size_t avail = ringAvail();
    int pct = ringCap > 0 ? static_cast<int>(avail * 100 / ringCap) : 0;
    uint32_t cuts = underruns.load();
    String bufText = "Buffer " + String(pct) + "% of " + String(ringCap / 1024) + "K";
    if (cuts > 0) {
        bufText += "  cuts " + String(cuts);
    }
    if (playState.load() == PS_PAUSED) {
        bufText += "  PAUSE";
    }
    d.fillRect(8, 86, 224, 12, C_BG);
    d.setTextColor(cuts > 0 ? C_WARN : C_DIM, C_BG);
    d.setCursor(8, 86);
    d.print(bufText);

    d.fillRect(8, 101, 224, 12, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 101);
    d.print("Vol " + String(volume * 100 / 255) + "%");
    drawMeter(70, 105, 100, 4, volume * 100 / 255, C_SELECT);
}

void drawPlayer() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("Now Playing");
    d.setTextColor(C_TEXT, C_BG);
    d.setCursor(8, 26);
    d.print(displayFit(trackName, 224));
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 43);
    d.print(displayFit(String(playIndex + 1) + "/" + String(playlist.size()) + "  " + displayName(segToName(lastSegment(playlistDir)).c_str()), 224));
    drawPlayerDynamic();
    drawFooter("Space Pause  N/P Track  ` Library");
    needsRedraw = false;
}

void drawFileList() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("Library");
    String location = isRootUrl(currentUrl) ? String("/") : displayName(segToName(lastSegment(currentUrl)).c_str());
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(9, 24);
    d.print(displayFit(location, 180));
    String count = String(entries.size()) + (listTruncated ? "+" : "");
    d.setCursor(SCREEN_W - 8 - d.textWidth(count), 24);
    d.print(count);

    const int listTop = 38;
    const int visible = 6;
    if (selected < scrollTop) {
        scrollTop = selected;
    }
    if (selected >= scrollTop + visible) {
        scrollTop = selected - visible + 1;
    }
    for (int row = 0; row < visible; ++row) {
        int idx = scrollTop + row;
        if (idx >= static_cast<int>(entries.size())) {
            break;
        }
        const FileEntry &e = entries[idx];
        int y = listTop + row * 13;
        bool sel = idx == selected;
        bool playing = isPlayingEntry(e);
        uint16_t rowBg = sel ? C_SELECT : C_PANEL;
        d.fillRoundRect(8, y, 224, 12, 5, rowBg);
        uint16_t fg = sel ? C_TEXT : (playing ? C_GOOD : (e.dir ? C_ACCENT : C_TEXT));
        d.setTextColor(fg, rowBg);
        d.setCursor(16, y + 2);
        String label = e.parent ? String("[..]") : String(playing ? "> " : "") + displayName(entryName(e));
        d.print(displayFit(label, 205));
    }
    if (entries.empty()) {
        d.setTextColor(C_WARN, C_BG);
        d.setCursor(60, 70);
        d.print("No folder / MP3 here");
    }
    drawFooter(playIndex >= 0 ? "Enter Open  ` Back  Tab Player" : "Enter Open  ` Back  N Address");
    needsRedraw = false;
}

// --------------------------------------------------------------------- Wi-Fi

void drawWifiList() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("Wi-Fi");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(9, 25);
    d.print(savedSsid.isEmpty() ? String("Choose a network") : displayFit("Saved: " + savedSsid, 176));
    const int visible = 4;
    if (wifiSelected < wifiScrollTop) {
        wifiScrollTop = wifiSelected;
    }
    if (wifiSelected >= wifiScrollTop + visible) {
        wifiScrollTop = wifiSelected - visible + 1;
    }
    for (int row = 0; row < visible; ++row) {
        int idx = wifiScrollTop + row;
        if (idx >= static_cast<int>(wifiItems.size())) {
            break;
        }
        int y = 40 + row * 19;
        bool sel = idx == wifiSelected;
        uint16_t rowBg = sel ? C_SELECT : C_PANEL;
        d.fillRoundRect(8, y, 224, 17, 7, rowBg);
        d.setTextColor(C_TEXT, rowBg);
        d.setCursor(17, y + 4);
        d.print(displayFit(wifiItems[idx].manual ? String("Manual SSID") : wifiItems[idx].ssid, 150));
        if (wifiItems[idx].saved) {
            d.fillCircle(209, y + 8, 3, sel ? C_TEXT : C_GOOD);
        } else if (!wifiItems[idx].manual) {
            int r = wifiItems[idx].rssi;
            int bars = r >= -55 ? 4 : r >= -67 ? 3 : r >= -75 ? 2 : r >= -85 ? 1 : 0;
            drawSignalGlyph(201, y + 4, bars, sel ? C_TEXT : C_DIM, sel ? C_ACCENT_DARK : C_TRACK);
        }
    }
    drawFooter("Enter Connect  R Rescan  ` Back");
    needsRedraw = false;
}

void scanWifi() {
    drawBusyScreen("Wi-Fi", "Searching for networks");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);
    wifiItems.clear();
    if (!savedSsid.isEmpty()) {
        WifiItem item;
        item.ssid = savedSsid;
        item.saved = true;
        item.enc = WIFI_AUTH_WPA2_PSK;
        wifiItems.push_back(item);
    }
    int count = WiFi.scanNetworks(false, true);
    for (int i = 0; i < count; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) {
            continue;
        }
        bool exists = false;
        for (const auto &item : wifiItems) {
            if (item.ssid == ssid) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        WifiItem item;
        item.ssid = ssid;
        item.rssi = WiFi.RSSI(i);
        item.enc = WiFi.encryptionType(i);
        wifiItems.push_back(item);
    }
    WiFi.scanDelete();
    std::sort(wifiItems.begin(), wifiItems.end(), [](const WifiItem &a, const WifiItem &b) {
        if (a.saved != b.saved) {
            return a.saved;
        }
        return a.rssi > b.rssi;
    });
    WifiItem manual;
    manual.manual = true;
    wifiItems.push_back(manual);
    wifiSelected = 0;
    wifiScrollTop = 0;
    screen = Screen::WifiList;
    needsRedraw = true;
}

bool connectWifi(const String &ssid, const String &pass) {
    drawBusyScreen("Wi-Fi", "Connecting to " + ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 22000) {
        M5Cardputer.update();
        delay(20);
    }
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);  // no Wi-Fi power saving: steadier throughput
        return true;
    }
    WiFi.disconnect(false, false);
    return false;
}

void openSavedLibrary() {
    if (!savedNas.isEmpty()) {
        drawBusyScreen("Library", "Loading files");
        if (loadDirectory(savedNas)) {
            screen = Screen::FileList;
            needsRedraw = true;
            return;
        }
    }
    beginInput(InputMode::NasUrl, "NAS address", savedNas.isEmpty() ? String("http://") : savedNas, false);
}

void afterWifiConnected() {
    savedSsid = pendingSsid;
    savedPass = inputText;
    prefs.putString("ssid", savedSsid);
    prefs.putString("pass", savedPass);
    openSavedLibrary();
}

// --------------------------------------------------------------------- keyboard

KeyEvent readKeys() {
    KeyEvent out;
    static char heldNav = 0;
    static uint32_t nextRepeat = 0;
    auto &kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
        auto status = kb.keysState();
        out.pressed = true;
        out.enter = status.enter;
        out.del = status.del;
        out.tab = status.tab;
        out.fn = status.fn;
        heldNav = 0;
        for (char c : status.word) {
            if (c == ' ') {
                out.space = true;
            }
            if (c >= 32 && c <= 126) {
                out.chars.push_back(c);
            }
            if (c == ';' || c == '.' || c == 'w' || c == 's') {
                heldNav = c;
            }
        }
        nextRepeat = millis() + 400;
        return out;
    }
    // Navigation key held down: auto-repeat (long lists).
    if (heldNav != 0 && kb.isPressed() && millis() >= nextRepeat &&
        (screen == Screen::FileList || screen == Screen::WifiList)) {
        nextRepeat = millis() + 60;
        out.pressed = true;
        out.chars.push_back(heldNav);
        return out;
    }
    if (!kb.isPressed()) {
        heldNav = 0;
    }
    return out;
}

bool isUp(const KeyEvent &k) {
    return charInEvent(k, ';') || charInEvent(k, 'w');
}

bool isDown(const KeyEvent &k) {
    return charInEvent(k, '.') || charInEvent(k, 's');
}

bool isBack(const KeyEvent &k) {
    return charInEvent(k, '`') || k.del;
}

void handleInput(const KeyEvent &key) {
    if (key.del) {
        removeLastUtf8(inputText);
        needsRedraw = true;
    }
    for (char c : key.chars) {
        if (c >= 32 && c <= 126 && inputText.length() < 180) {
            inputText += c;
            needsRedraw = true;
        }
    }
    if (key.tab) {
        screen = (inputMode == InputMode::NasUrl && !entries.empty()) ? Screen::FileList : Screen::WifiList;
        needsRedraw = true;
        return;
    }
    if (!key.enter) {
        return;
    }
    if (inputMode == InputMode::ManualSsid) {
        pendingSsid = trimCopy(inputText);
        if (!pendingSsid.isEmpty()) {
            beginInput(InputMode::WifiPassword, "WiFi password", "", true);
        }
    } else if (inputMode == InputMode::WifiPassword) {
        if (connectWifi(pendingSsid, inputText)) {
            afterWifiConnected();
        } else {
            showMessage("WiFi failed", "Check password or signal", 1600, Screen::WifiList);
        }
    } else if (inputMode == InputMode::NasUrl) {
        drawBusyScreen("Library", "Loading files");
        if (loadDirectory(inputText)) {
            screen = Screen::FileList;
            needsRedraw = true;
        } else {
            showMessage("NAS failed", "Use HTTP/WebDAV URL", 1800, Screen::TextInput);
        }
    }
}

void handleWifiList(const KeyEvent &key) {
    if ((isBack(key) || key.tab) && WiFi.status() == WL_CONNECTED && !currentUrl.isEmpty()) {
        screen = Screen::FileList;
        needsRedraw = true;
        return;
    }
    if (charInEvent(key, 'r')) {
        scanWifi();
        return;
    }
    if (isUp(key) && wifiSelected > 0) {
        --wifiSelected;
        needsRedraw = true;
    }
    if (isDown(key) && wifiSelected + 1 < static_cast<int>(wifiItems.size())) {
        ++wifiSelected;
        needsRedraw = true;
    }
    if (!key.enter || wifiSelected < 0 || wifiSelected >= static_cast<int>(wifiItems.size())) {
        return;
    }
    WifiItem &item = wifiItems[wifiSelected];
    if (item.manual) {
        beginInput(InputMode::ManualSsid, "WiFi SSID", "", false);
        return;
    }
    pendingSsid = item.ssid;
    if (item.saved) {
        inputText = savedPass;
        if (connectWifi(pendingSsid, savedPass)) {
            openSavedLibrary();
        } else {
            showMessage("WiFi failed", "Saved password failed", 1600, Screen::WifiList);
        }
        return;
    }
    if (item.enc == WIFI_AUTH_OPEN) {
        inputText = "";
        if (connectWifi(pendingSsid, "")) {
            afterWifiConnected();
        } else {
            showMessage("WiFi failed", "Cannot connect", 1500, Screen::WifiList);
        }
        return;
    }
    beginInput(InputMode::WifiPassword, "WiFi password", "", true);
}

void openFolder(const String &url) {
    drawBusyScreen("Library", "Opening folder");
    if (!loadDirectory(url)) {
        showMessage("NAS failed", "Cannot open folder", 1400, Screen::FileList);
    } else {
        needsRedraw = true;
    }
}

void handleFileList(const KeyEvent &key) {
    if (key.tab && playIndex >= 0) {
        screen = Screen::Player;
        needsRedraw = true;
        return;
    }
    if (isBack(key)) {
        if (!isRootUrl(currentUrl)) {
            openFolder(parentUrlOf(currentUrl));
        }
        return;
    }
    if (charInEvent(key, 'q')) {
        scanWifi();
        return;
    }
    if (charInEvent(key, 'n')) {
        beginInput(InputMode::NasUrl, "NAS address", currentUrl.isEmpty() ? savedNas : currentUrl, false);
        return;
    }
    if (charInEvent(key, 'r')) {
        openFolder(currentUrl);
        return;
    }
    if (isUp(key) && selected > 0) {
        --selected;
        needsRedraw = true;
    }
    if (isDown(key) && selected + 1 < static_cast<int>(entries.size())) {
        ++selected;
        needsRedraw = true;
    }
    if (key.enter && selected >= 0 && selected < static_cast<int>(entries.size())) {
        const FileEntry &e = entries[selected];
        if (e.dir) {
            openFolder(entryUrl(e));
        } else if (isPlayingEntry(e)) {
            screen = Screen::Player;  // already playing: go back to the player screen
            needsRedraw = true;
        } else {
            playFolderFrom(selected);
        }
    }
}

void handlePlayer(const KeyEvent &key) {
    if (isBack(key) || key.tab) {
        screen = Screen::FileList;  // music keeps playing
        needsRedraw = true;
        return;
    }
    if (key.space) {
        togglePause();
    }
    if (charInEvent(key, 'n')) {
        playFrom(playIndex + 1, true);
        return;
    }
    if (charInEvent(key, 'p')) {
        playFrom(std::max(0, playIndex - (out.elapsedMs() > 3000 ? 0 : 1)), true);
        return;
    }
    if (charInEvent(key, '+') || charInEvent(key, '=')) {
        volume = std::min(255, volume + 15);
        M5Cardputer.Speaker.setVolume(volume);
        prefs.putUInt("volume", volume);
        needsRedraw = true;
    }
    if (charInEvent(key, '-') || charInEvent(key, '_')) {
        volume = std::max(0, volume - 15);
        M5Cardputer.Speaker.setVolume(volume);
        prefs.putUInt("volume", volume);
        needsRedraw = true;
    }
}

void dispatchKeys(const KeyEvent &key) {
    if (!key.pressed) {
        return;
    }
    switch (screen) {
        case Screen::WifiList:
            handleWifiList(key);
            break;
        case Screen::TextInput:
            handleInput(key);
            break;
        case Screen::FileList:
            handleFileList(key);
            break;
        case Screen::Player:
            handlePlayer(key);
            break;
        case Screen::Message:
            break;
    }
}

void drawCurrentScreen() {
    if (!needsRedraw) {
        return;
    }
    switch (screen) {
        case Screen::WifiList:
            drawWifiList();
            break;
        case Screen::TextInput:
            drawInput();
            break;
        case Screen::FileList:
            drawFileList();
            break;
        case Screen::Player:
            drawPlayer();
            break;
        case Screen::Message:
            drawMessage();
            break;
    }
}

void allocateRing() {
    static const size_t sizes[] = {96 * 1024, 80 * 1024, 64 * 1024, 48 * 1024, 32 * 1024};
    for (size_t sz : sizes) {
        Serial.printf("[ring] try %u: heap %u, max block %u\n", static_cast<unsigned>(sz), ESP.getFreeHeap(),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        // keep enough memory for Wi-Fi, the decoder and the folder list
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < sz || ESP.getFreeHeap() < sz + 150 * 1024) {
            continue;
        }
        ringBuf = static_cast<uint8_t *>(heap_caps_malloc(sz, MALLOC_CAP_8BIT));
        if (ringBuf) {
            ringCap = sz;
            return;
        }
    }
    ringBuf = static_cast<uint8_t *>(malloc(16 * 1024));
    ringCap = ringBuf ? 16 * 1024 : 0;
}

// Cardputer ADV keyboard read on every loop. The M5Cardputer library reader
// waits for the TCA8418 interrupt: if a key arrives at the wrong moment, the
// interrupt is lost and the keyboard stops responding (the app keeps running).
class PolledKeyboardReader : public KeyboardReader {
public:
    void begin() override {
        _ok = _tca.begin();
        if (_ok) {
            _tca.matrix(7, 8);
            _tca.flush();
        }
    }

    void update() override {
        if (!_ok) {
            return;
        }
        for (uint8_t ev = _tca.getEvent(); ev != 0; ev = _tca.getEvent()) {
            int code = (ev & 0x7F) - 1;
            int r = code / 10;
            int c = code % 10;
            if (code < 0 || r >= 7 || c >= 8) {
                continue;
            }
            Point2D_t p;  // same layout as the library
            p.x = r * 2 + (c > 3 ? 1 : 0);
            p.y = (c + 4) % 4;
            auto it = std::find(_key_list.begin(), _key_list.end(), p);
            if (ev & 0x80) {
                if (it == _key_list.end()) {
                    _key_list.push_back(p);
                }
            } else if (it != _key_list.end()) {
                _key_list.erase(it);
            }
        }
    }

private:
    Adafruit_TCA8418 _tca;
    bool _ok = false;
};

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    if (M5.getBoard() == m5::board_t::board_M5CardputerADV) {
        // Replace the library keyboard reader (see PolledKeyboardReader)
        detachInterrupt(digitalPinToInterrupt(11));
        M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PolledKeyboardReader()));
    }
    auto &d = M5Cardputer.Display;
    d.setRotation(1);
    d.setTextDatum(top_left);
    d.setTextWrap(false);
    d.setFont(&fonts::efontJA_12);  // also has É, Ç, À... (efontCN_12 did not)
    d.setTextSize(1);
    d.setBrightness(128);
    d.fillScreen(C_BG);

    auto spk = M5Cardputer.Speaker.config();
    spk.task_pinned_core = APP_CPU_NUM;
    spk.task_priority = 3;
    M5Cardputer.Speaker.config(spk);
    M5Cardputer.Speaker.begin();

    prefs.begin("nasmp3", false);
    savedSsid = prefs.getString("ssid", "");
    savedPass = prefs.getString("pass", "");
    savedNas = prefs.getString("nas", "");
    volume = static_cast<int>(prefs.getUInt("volume", volume));
    M5Cardputer.Speaker.setVolume(volume);

    Serial.begin(115200);
    allocateRing();
    Serial.printf("[boot] ring %u bytes, heap %u\n", static_cast<unsigned>(ringCap), ESP.getFreeHeap());
    netMutex = xSemaphoreCreateMutex();
    decMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(netTask, "net", 4096, nullptr, 2, nullptr, PRO_CPU_NUM);
    xTaskCreatePinnedToCore(decodeTask, "mp3", 16384, nullptr, 3, nullptr, PRO_CPU_NUM);

    if (!savedSsid.isEmpty()) {
        bootAutoStartPending = true;
        messageTitle = "Boot";
        messageBody = "Starting...";
        messageUntil = millis() + 600000UL;
        screen = Screen::Message;
        needsRedraw = true;
    } else {
        scanWifi();
    }
}

void loop() {
    M5Cardputer.update();

    if (bootAutoStartPending) {
        bootAutoStartPending = false;
        pendingSsid = savedSsid;
        inputText = savedPass;
        if (connectWifi(savedSsid, savedPass)) {
            openSavedLibrary();
        } else {
            scanWifi();
        }
    }

    if (screen == Screen::Message && static_cast<int32_t>(millis() - messageUntil) >= 0) {
        screen = returnAfterMessage;
        needsRedraw = true;
    }

    // End of track: next one in the folder, or stop at the end of the folder.
    if (playState.load() == PS_DONE) {
        Serial.printf("[end] cuts %lu, net error %d, heap %u\n", static_cast<unsigned long>(underruns.load()),
                      netError.load() ? 1 : 0, ESP.getFreeHeap());
        if (netError.load()) {
            finishPlaylist("Network error");
        } else {
            playFrom(playIndex + 1, false);
        }
    }

    static uint32_t lastDyn = 0;
    if (screen == Screen::Player && !needsRedraw && millis() - lastDyn > 500) {
        lastDyn = millis();
        drawPlayerDynamic();
    }

    KeyEvent key = readKeys();
    dispatchKeys(key);
    drawCurrentScreen();
    delay(5);
}
