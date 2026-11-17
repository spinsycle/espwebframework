#pragma once
// A JSON reply produced a few bytes at a time instead of being built whole first.
//
// AsyncResponseStream keeps the entire body in one growing buffer and, when the heap is fragmented and
// the buffer cannot grow, silently cuts the reply short. On the ESP8266 that happens once a reply is
// bigger than about 1.4 KB. A chunked response asks for bytes as the TCP window has room, so the most it
// ever holds is one item of a list.
//
//   auto c = std::make_shared<Chunker>("{\"enabled\":true");               // the start of the object
//   c->add(",\"rules\":[", [](size_t i, String& out) { ...; return true; }); // one section: ',"name":[' items ']'
//   sendChunked(r, c);                                                      // the closing '}' is added for you
#include <ESPAsyncWebServer.h>

#include <functional>
#include <memory>

namespace espwf {

class Chunker {
 public:
  // Fills `out` with item i as JSON text and returns true; false = no item i (the list is over).
  using ItemFn = std::function<bool(size_t i, String& out)>;

  explicit Chunker(const String& head) : head_(head) {}

  Chunker& add(const char* open, ItemFn fn) {
    secs_.push_back(Section{String(open), fn});
    return *this;
  }

  // Up to `max` bytes of the reply; 0 means it is finished.
  size_t fill(uint8_t* buf, size_t max) {
    size_t n = 0;
    while (n < max) {
      if (pos_ >= cur_.length()) {
        cur_ = "";
        pos_ = 0;
        if (!next()) break;
      }
      size_t take = cur_.length() - pos_;
      if (take > max - n) take = max - n;
      memcpy(buf + n, cur_.c_str() + pos_, take);
      n += take;
      pos_ += take;
    }
    return n;
  }

 private:
  struct Section {
    String open;
    ItemFn item;
  };

  // The next piece of text goes in cur_. False = nothing left.
  bool next() {
    switch (stage_) {
      case 0: cur_ = head_; stage_ = 1; sec_ = 0; return true;
      case 1:  // start of a section, or the end of the object
        if (sec_ >= secs_.size()) { cur_ = "}"; stage_ = 4; return true; }
        cur_ = secs_[sec_].open;
        i_ = 0;
        stage_ = 2;
        return true;
      case 2: {
        String it;
        if (secs_[sec_].item(i_, it)) {
          cur_ = i_ ? String(",") + it : it;
          i_++;
          return true;
        }
        cur_ = "]";
        stage_ = 3;
        return true;
      }
      case 3: sec_++; stage_ = 1; return next();
      default: return false;
    }
  }

  String head_;
  std::vector<Section> secs_;
  String cur_;
  size_t pos_ = 0, sec_ = 0, i_ = 0;
  int stage_ = 0;
};

// Sends the chunker as a JSON reply. The response owns it and frees it when the reply ends or the client leaves.
inline void sendChunked(AsyncWebServerRequest* r, std::shared_ptr<Chunker> c, const char* extraHeader = nullptr, const char* extraValue = nullptr) {
  AsyncWebServerResponse* resp = r->beginChunkedResponse("application/json", [c](uint8_t* buf, size_t max, size_t) -> size_t { return c->fill(buf, max); });
  resp->addHeader("Cache-Control", "no-store");
  if (extraHeader) resp->addHeader(extraHeader, extraValue);
  r->send(resp);
}

}  // namespace espwf
