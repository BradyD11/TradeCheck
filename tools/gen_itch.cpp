#include <cstdio>
#include <cstdlib>

#include "lob/itch_sim.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gen_itch <out.itch> [messages=5000000] [symbols=100] [seed=1]\n");
    return 2;
  }
  std::size_t messages = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
  std::size_t symbols = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 100;
  std::uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;
  if (symbols == 0 || symbols > 65535 || messages < symbols) {
    std::fprintf(stderr, "need 1..65535 symbols and messages >= symbols\n");
    return 2;
  }
  auto bytes = lob::itch::simulate(messages, symbols, seed);
  std::FILE* f = std::fopen(argv[1], "wb");
  if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size() || std::fclose(f) != 0) {
    std::perror(argv[1]);
    return 1;
  }
  std::printf("wrote %zu messages (%zu bytes) to %s\n", messages, bytes.size(), argv[1]);
}
