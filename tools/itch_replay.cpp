// Replays a Nasdaq ITCH 5.0 file through the engine and reports per-operation latency.
//   itch_replay 01302019.NASDAQ_ITCH50            (uncompressed sample file)
//   gunzip -c 01302019.NASDAQ_ITCH50.gz | itch_replay -
#include <cstdio>
#include <cstring>
#include <string>

#include "lob/replay.hpp"

using namespace lob;

static void print_top(const OrderBook& b, int depth) {
  std::vector<const Level*> bids, asks;
  b.for_each_level(Side::Buy, [&](const Level& l) { if (bids.size() < size_t(depth)) bids.push_back(&l); });
  b.for_each_level(Side::Sell, [&](const Level& l) { if (asks.size() < size_t(depth)) asks.push_back(&l); });
  std::printf("\n  %12s %10s | %-10s %-12s\n", "bid qty", "bid", "ask", "ask qty");
  for (int i = 0; i < depth; ++i) {
    auto cell = [](const Level* l, bool qty_first) {
      char buf[32];
      if (!l) return std::string();
      if (qty_first) std::snprintf(buf, sizeof buf, "%llu", (unsigned long long)l->total);
      else std::snprintf(buf, sizeof buf, "%.4f", l->price / 10000.0);
      return std::string(buf);
    };
    const Level* bl = i < int(bids.size()) ? bids[i] : nullptr;
    const Level* al = i < int(asks.size()) ? asks[i] : nullptr;
    std::printf("  %12s %10s | %-10s %-12s\n", cell(bl, true).c_str(), cell(bl, false).c_str(),
                cell(al, false).c_str(), cell(al, true).c_str());
  }
}

int main(int argc, char** argv) {
  const char* path = nullptr;
  std::string symbol;
  bool timing = true;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--symbol") && i + 1 < argc) symbol = argv[++i];
    else if (!std::strcmp(argv[i], "--no-timing")) timing = false;
    else path = argv[i];
  }
  if (!path) {
    std::fprintf(stderr, "usage: itch_replay <file|-> [--symbol AAPL] [--no-timing]\n");
    return 2;
  }
  std::FILE* f = std::strcmp(path, "-") ? std::fopen(path, "rb") : stdin;
  if (!f) {
    std::perror(path);
    return 1;
  }

  LatencyHistogram hist;
  Replayer replay(timing ? &hist : nullptr, symbol);
  itch::Reader reader(f);
  const std::uint8_t* msg;
  std::uint16_t len;
  std::uint64_t messages = 0, malformed = 0;
  std::uint64_t t0 = now_ns();
  while (reader.next(msg, len)) {
    if (!itch::dispatch(msg, len, replay)) ++malformed;
    if (++messages % 50'000'000 == 0) std::fprintf(stderr, "  %llu M messages...\n", (unsigned long long)(messages / 1'000'000));
  }
  double secs = (now_ns() - t0) / 1e9;
  if (reader.error()) std::fprintf(stderr, "warning: input ended mid-message (truncated file?)\n");

  const ReplayStats& s = replay.stats;
  std::uint64_t ops = s.adds + s.executions + s.cancels + s.deletes + s.replaces;
  std::printf("messages      %llu in %.2f s (%.1f M msg/s incl. parsing)\n", (unsigned long long)messages, secs,
              messages / secs / 1e6);
  std::printf("book ops      %llu  (add %llu, exec %llu, cancel %llu, delete %llu, replace %llu)\n",
              (unsigned long long)ops, (unsigned long long)s.adds, (unsigned long long)s.executions,
              (unsigned long long)s.cancels, (unsigned long long)s.deletes, (unsigned long long)s.replaces);
  std::printf("malformed     %llu\n", (unsigned long long)malformed);
  std::printf("rejected      %llu\n", (unsigned long long)s.rejected);
  std::printf("unknown refs  %llu\n", (unsigned long long)s.unknown_refs);
  std::printf("engine trades %llu   (adds that crossed our book; 0 means it mirrors Nasdaq's)\n",
              (unsigned long long)s.unexpected_trades);
  if (s.front_checks)
    std::printf("FIFO agrees   %.4f%%  (%llu of %llu Nasdaq executions hit the order our priority picks)\n",
                100.0 * s.front_agree / s.front_checks, (unsigned long long)s.front_agree,
                (unsigned long long)s.front_checks);
  std::printf("invariants    %s\n", replay.check_all_invariants() ? "OK" : "VIOLATED");
  if (timing) {
    std::printf("\nper-op latency (book op only, excludes parsing):\n");
    hist.print("all");
  }
  if (!symbol.empty()) {
    const OrderBook* b = replay.filter_locate() ? replay.book_for(replay.filter_locate()) : nullptr;
    if (b) {
      std::printf("\n%s at end of file: %zu orders", symbol.c_str(), b->order_count());
      print_top(*b, 5);
    } else {
      std::printf("\nsymbol %s not found\n", symbol.c_str());
    }
  }
  return 0;
}
