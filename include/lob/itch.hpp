#pragma once
// Nasdaq TotalView-ITCH 5.0: just the messages that build an order book.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace lob::itch {

inline std::uint64_t be(const std::uint8_t* p, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}
inline std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(be(p, 2)); }
inline std::uint32_t be32(const std::uint8_t* p) { return static_cast<std::uint32_t>(be(p, 4)); }

struct Header {
  char type;
  std::uint16_t locate;  // per-day instrument id
  std::uint16_t tracking;
  std::uint64_t timestamp;  // ns since midnight
};
struct StockDirectory {  // 'R'
  Header h;
  char stock[8];
};
struct AddOrder {  // 'A', and 'F' with attribution
  Header h;
  std::uint64_t ref;
  char side;  // 'B' or 'S'
  std::uint32_t shares;
  char stock[8];
  std::uint32_t price;  // 4 implied decimals
  std::uint32_t attribution;
};
struct OrderExecuted {  // 'E', and 'C' with a price
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  bool has_price;
  std::uint32_t price;
};
struct OrderCancel {  // 'X' partial cancel
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
};
struct OrderDelete {  // 'D'
  Header h;
  std::uint64_t ref;
};
struct SystemEvent {  // 'S'
  Header h;
  char code;  // O start, S system open 04:00, Q market open 09:30, M market close 16:00, E system close, C end
};
struct TradingAction {  // 'H'
  Header h;
  char stock[8];
  char state;  // T trading, H halted, P paused, Q quotation only
};
struct OrderReplace {  // 'U'
  Header h;
  std::uint64_t orig_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};

// Sizes from the ITCH 5.0 spec. 0 means a type we skip.
constexpr std::size_t size_of(char type) {
  switch (type) {
    case 'R': return 39;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'S': return 12;
    case 'H': return 25;
    default: return 0;
  }
}

struct NullHandler {
  void on(const StockDirectory&) {}
  void on(const AddOrder&) {}
  void on(const OrderExecuted&) {}
  void on(const OrderCancel&) {}
  void on(const OrderDelete&) {}
  void on(const OrderReplace&) {}
  void on(const SystemEvent&) {}
  void on(const TradingAction&) {}
};

// Returns false for a message shorter than its type requires. Unused types are skipped.
template <class H>
bool dispatch(const std::uint8_t* p, std::size_t len, H& h) {
  if (len == 0) return false;
  char t = static_cast<char>(p[0]);
  std::size_t need = size_of(t);
  if (need == 0) return true;
  if (len < need) return false;
  Header hd{t, be16(p + 1), be16(p + 3), be(p + 5, 6)};
  const std::uint8_t* b = p + 11;
  switch (t) {
    case 'R': {
      StockDirectory m{hd, {}};
      std::memcpy(m.stock, b, 8);
      h.on(m);
      break;
    }
    case 'A':
    case 'F': {
      AddOrder m{hd, be(b, 8), static_cast<char>(b[8]), be32(b + 9), {}, be32(b + 21), t == 'F' ? be32(b + 25) : 0};
      std::memcpy(m.stock, b + 13, 8);
      h.on(m);
      break;
    }
    case 'E':
    case 'C': {
      OrderExecuted m{hd, be(b, 8), be32(b + 8), be(b + 12, 8), t == 'C', t == 'C' ? be32(b + 21) : 0};
      h.on(m);
      break;
    }
    case 'X': h.on(OrderCancel{hd, be(b, 8), be32(b + 8)}); break;
    case 'D': h.on(OrderDelete{hd, be(b, 8)}); break;
    case 'U': h.on(OrderReplace{hd, be(b, 8), be(b + 8, 8), be32(b + 16), be32(b + 20)}); break;
    case 'S': h.on(SystemEvent{hd, static_cast<char>(b[0])}); break;
    case 'H': {
      TradingAction m{hd, {}, static_cast<char>(b[8])};
      std::memcpy(m.stock, b, 8);
      h.on(m);
      break;
    }
  }
  return true;
}

// Reads Nasdaq's sample-file framing: a 2-byte big-endian length, then the message.
// Streams, so a 10 GB file can be piped in from gunzip.
class Reader {
 public:
  explicit Reader(std::FILE* f, std::size_t chunk = 1 << 20) : f_(f), chunk_(chunk), buf_(chunk + 2 + 65535) {}

  // false at end of input; error() tells a clean end from a truncated file.
  bool next(const std::uint8_t*& msg, std::uint16_t& len) {
    if (!fill(2)) {
      error_ = end_ > pos_;
      return false;
    }
    len = be16(buf_.data() + pos_);
    if (!fill(2 + std::size_t{len})) {
      error_ = true;
      return false;
    }
    msg = buf_.data() + pos_ + 2;
    pos_ += 2 + std::size_t{len};
    return true;
  }
  bool error() const { return error_; }

 private:
  bool fill(std::size_t n) {
    while (end_ - pos_ < n) {
      if (pos_ > 0) {
        std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
        end_ -= pos_;
        pos_ = 0;
      }
      std::size_t got = std::fread(buf_.data() + end_, 1, std::min(chunk_, buf_.size() - end_), f_);
      if (got == 0) return false;
      end_ += got;
    }
    return true;
  }

  std::FILE* f_;
  std::size_t chunk_;
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0, end_ = 0;
  bool error_ = false;
};

// Encoder for tests and the synthetic generator. Same framing as Reader.
class Writer {
 public:
  std::vector<std::uint8_t> bytes;

  void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock) {
    auto s = start('R', locate, ts);
    put_stock(stock);
    finish(s, 'R');  // remaining directory fields are zero
  }
  void add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t shares,
           std::string_view stock, std::uint32_t price, std::optional<std::uint32_t> attribution = {}) {
    char t = attribution ? 'F' : 'A';
    auto s = start(t, locate, ts);
    put(ref, 8);
    put(static_cast<std::uint8_t>(side), 1);
    put(shares, 4);
    put_stock(stock);
    put(price, 4);
    if (attribution) put(*attribution, 4);
    finish(s, t);
  }
  void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares, std::uint64_t match) {
    auto s = start('E', locate, ts);
    put(ref, 8);
    put(shares, 4);
    put(match, 8);
    finish(s, 'E');
  }
  void executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                           std::uint64_t match, std::uint32_t price) {
    auto s = start('C', locate, ts);
    put(ref, 8);
    put(shares, 4);
    put(match, 8);
    put('Y', 1);
    put(price, 4);
    finish(s, 'C');
  }
  void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    auto s = start('X', locate, ts);
    put(ref, 8);
    put(shares, 4);
    finish(s, 'X');
  }
  void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    auto s = start('D', locate, ts);
    put(ref, 8);
    finish(s, 'D');
  }
  void system_event(std::uint64_t ts, char code) {
    auto s = start('S', 0, ts);
    put(static_cast<std::uint8_t>(code), 1);
    finish(s, 'S');
  }
  void trading_action(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char state) {
    auto s = start('H', locate, ts);
    put_stock(stock);
    put(static_cast<std::uint8_t>(state), 1);
    finish(s, 'H');  // reserved byte and reason code are zero
  }
  void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t orig, std::uint64_t new_ref,
               std::uint32_t shares, std::uint32_t price) {
    auto s = start('U', locate, ts);
    put(orig, 8);
    put(new_ref, 8);
    put(shares, 4);
    put(price, 4);
    finish(s, 'U');
  }

 private:
  void put(std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void put_stock(std::string_view s) {
    for (std::size_t i = 0; i < 8; ++i) bytes.push_back(i < s.size() ? s[i] : ' ');
  }
  std::size_t start(char t, std::uint16_t locate, std::uint64_t ts) {
    std::size_t s = bytes.size();
    put(size_of(t), 2);
    put(static_cast<std::uint8_t>(t), 1);
    put(locate, 2);
    put(0, 2);
    put(ts, 6);
    return s;
  }
  void finish(std::size_t s, char t) { bytes.resize(s + 2 + size_of(t)); }
};

}  // namespace lob::itch
