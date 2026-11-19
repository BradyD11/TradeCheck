// Prints every ITCH message for one symbol, in feed order. For investigating the feed by hand.
//   gunzip -c 01302019.NASDAQ_ITCH50.gz | itch_dump - SXTC
#include <cstdio>
#include <cstring>
#include <string>

#include "lob/itch.hpp"

using namespace lob::itch;

struct Dump : NullHandler {
  std::string symbol;
  std::uint16_t locate = 0;

  static void ts(const Header& h) {
    std::uint64_t us = h.timestamp / 1000;
    std::printf("%02llu:%02llu:%02llu.%06llu %c ", (unsigned long long)(us / 3'600'000'000ULL),
                (unsigned long long)(us / 60'000'000 % 60), (unsigned long long)(us / 1'000'000 % 60),
                (unsigned long long)(us % 1'000'000), h.type);
  }
  bool mine(const Header& h) const { return locate && h.locate == locate; }

  void on(const StockDirectory& m) {
    std::string s(m.stock, 8);
    s.erase(s.find_last_not_of(' ') + 1);
    if (s == symbol) locate = m.h.locate;
  }
  void on(const SystemEvent& m) { ts(m.h), std::printf("system %c\n", m.code); }
  void on(const TradingAction& m) {
    if (mine(m.h)) ts(m.h), std::printf("state %c\n", m.state);
  }
  void on(const AddOrder& m) {
    if (mine(m.h)) ts(m.h), std::printf("ref %llu %c %u @ %.4f\n", (unsigned long long)m.ref, m.side, m.shares, m.price / 1e4);
  }
  void on(const OrderExecuted& m) {
    if (!mine(m.h)) return;
    ts(m.h), std::printf("ref %llu exec %u match %llu", (unsigned long long)m.ref, m.shares, (unsigned long long)m.match);
    if (m.has_price) std::printf(" @ %.4f", m.price / 1e4);
    std::printf("\n");
  }
  void on(const OrderCancel& m) {
    if (mine(m.h)) ts(m.h), std::printf("ref %llu cancel %u\n", (unsigned long long)m.ref, m.shares);
  }
  void on(const OrderDelete& m) {
    if (mine(m.h)) ts(m.h), std::printf("ref %llu delete\n", (unsigned long long)m.ref);
  }
  void on(const OrderReplace& m) {
    if (mine(m.h))
      ts(m.h), std::printf("ref %llu -> %llu %u @ %.4f\n", (unsigned long long)m.orig_ref, (unsigned long long)m.new_ref,
                           m.shares, m.price / 1e4);
  }
  void on(const CrossTrade& m) {
    if (mine(m.h))
      ts(m.h), std::printf("cross %c %llu @ %.4f match %llu\n", m.cross_type, (unsigned long long)m.shares,
                           m.price / 1e4, (unsigned long long)m.match);
  }
  void on(const Imbalance& m) {
    if (mine(m.h))
      ts(m.h), std::printf("noii %c paired %llu imb %llu %c far %.4f near %.4f ref %.4f\n", m.cross_type,
                           (unsigned long long)m.paired, (unsigned long long)m.imbalance, m.direction, m.far_price / 1e4,
                           m.near_price / 1e4, m.reference_price / 1e4);
  }
};

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: itch_dump <file|-> SYMBOL\n");
    return 2;
  }
  std::FILE* f = std::strcmp(argv[1], "-") ? std::fopen(argv[1], "rb") : stdin;
  if (!f) {
    std::perror(argv[1]);
    return 1;
  }
  Dump d;
  d.symbol = argv[2];
  Reader r(f);
  const std::uint8_t* msg;
  std::uint16_t len;
  while (r.next(msg, len)) dispatch(msg, len, d);
  return r.error() ? 1 : 0;
}
