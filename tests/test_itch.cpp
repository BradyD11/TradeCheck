#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "lob/itch_sim.hpp"
#include "lob/replay.hpp"

using namespace lob;
using namespace lob::itch;

struct Recorder : NullHandler {
  std::vector<char> types;
  std::vector<AddOrder> adds;
  std::vector<OrderExecuted> execs;
  std::vector<OrderCancel> cancels;
  std::vector<OrderDelete> deletes;
  std::vector<OrderReplace> replaces;
  std::string stock;
  void on(const StockDirectory& m) { types.push_back('R'); stock.assign(m.stock, 8); }
  void on(const AddOrder& m) { types.push_back(m.h.type); adds.push_back(m); }
  void on(const OrderExecuted& m) { types.push_back(m.h.type); execs.push_back(m); }
  void on(const OrderCancel& m) { types.push_back('X'); cancels.push_back(m); }
  void on(const OrderDelete& m) { types.push_back('D'); deletes.push_back(m); }
  void on(const OrderReplace& m) { types.push_back('U'); replaces.push_back(m); }
};

static void read_all(std::vector<std::uint8_t>& bytes, std::size_t chunk, Recorder& rec, bool& error) {
  std::FILE* f = fmemopen(bytes.data(), bytes.size(), "rb");
  CHECK(f);
  Reader r(f, chunk);
  const std::uint8_t* msg;
  std::uint16_t len;
  while (r.next(msg, len)) CHECK(dispatch(msg, len, rec));
  error = r.error();
  std::fclose(f);
}

static void round_trip_every_type() {
  Writer w;
  w.stock_directory(7, 1, "AAPL");
  w.add(7, 2, 1001, 'B', 300, "AAPL", 1'500'250);
  w.add(7, 3, 1002, 'S', 200, "AAPL", 1'500'300, 0x4E534454);
  w.executed(7, 4, 1001, 100, 55);
  w.executed_with_price(7, 5, 1002, 50, 56, 1'500'290);
  w.cancel(7, 6, 1001, 25);
  w.replace(7, 7, 1002, 1003, 400, 1'500'400);
  w.del(7, 0xFFFFFFFFFFFF, 1003);  // max 48-bit timestamp
  w.bytes.insert(w.bytes.end(), {0, 3, 'S', 0, 1});  // unknown type: skipped, not an error

  for (std::size_t chunk : {std::size_t{1} << 20, std::size_t{7}, std::size_t{1}}) {  // small chunks split messages
    Recorder rec;
    bool error;
    read_all(w.bytes, chunk, rec, error);
    CHECK(!error);
    CHECK((rec.types == std::vector<char>{'R', 'A', 'F', 'E', 'C', 'X', 'U', 'D'}));
    CHECK(rec.stock == "AAPL    ");
    CHECK(rec.adds[0].h.locate == 7 && rec.adds[0].h.timestamp == 2);
    CHECK(rec.adds[0].ref == 1001 && rec.adds[0].side == 'B' && rec.adds[0].shares == 300);
    CHECK(rec.adds[0].price == 1'500'250 && std::memcmp(rec.adds[0].stock, "AAPL    ", 8) == 0);
    CHECK(rec.adds[1].attribution == 0x4E534454 && rec.adds[1].side == 'S');
    CHECK(rec.execs[0].ref == 1001 && rec.execs[0].shares == 100 && rec.execs[0].match == 55 && !rec.execs[0].has_price);
    CHECK(rec.execs[1].has_price && rec.execs[1].price == 1'500'290);
    CHECK(rec.cancels[0].ref == 1001 && rec.cancels[0].shares == 25);
    CHECK(rec.replaces[0].orig_ref == 1002 && rec.replaces[0].new_ref == 1003);
    CHECK(rec.replaces[0].shares == 400 && rec.replaces[0].price == 1'500'400);
    CHECK(rec.deletes[0].ref == 1003 && rec.deletes[0].h.timestamp == 0xFFFFFFFFFFFF);
  }
}

static void malformed_input_is_reported() {
  Writer w;
  w.add(1, 1, 1, 'B', 1, "X", 1);
  std::vector<std::uint8_t> truncated(w.bytes.begin(), w.bytes.end() - 3);
  Recorder rec;
  bool error;
  read_all(truncated, 1 << 20, rec, error);
  CHECK(error && rec.adds.empty());

  // A length prefix shorter than the type needs must not read past the message.
  const std::uint8_t short_add[] = {'A', 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(!dispatch(short_add, sizeof short_add, rec));
  CHECK(!dispatch(short_add, 0, rec));
}

static void synthetic_day_replays_cleanly() {
  auto bytes = simulate(200'000, 20, 7);
  std::FILE* f = fmemopen(bytes.data(), bytes.size(), "rb");
  CHECK(f);
  Replayer replay;
  Reader r(f);
  const std::uint8_t* msg;
  std::uint16_t len;
  std::uint64_t n = 0;
  while (r.next(msg, len)) {
    CHECK(dispatch(msg, len, replay));
    ++n;
  }
  std::fclose(f);
  const ReplayStats& s = replay.stats;
  CHECK(n == 200'000 && !r.error());
  CHECK(s.adds > 0 && s.executions > 0 && s.cancels > 0 && s.deletes > 0 && s.replaces > 0);
  CHECK(s.rejected == 0 && s.unknown_refs == 0 && s.unexpected_trades == 0);
  CHECK(s.front_checks > 0 && s.front_agree == s.front_checks);
  CHECK(replay.check_all_invariants());
}

int main() {
  round_trip_every_type();
  malformed_input_is_reported();
  synthetic_day_replays_cleanly();
  std::puts("itch: OK");
}
