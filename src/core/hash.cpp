#include "core/hash.h"

#include <blake3.h>

#include <random>

namespace hs {

static std::string base32(const unsigned char* data, size_t n) {
  static const char* alphabet = "abcdefghijklmnopqrstuvwxyz234567";
  std::string out;
  out.reserve((n * 8 + 4) / 5);
  unsigned buffer = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    buffer = (buffer << 8) | data[i];
    bits += 8;
    while (bits >= 5) {
      out.push_back(alphabet[(buffer >> (bits - 5)) & 31]);
      bits -= 5;
    }
  }
  if (bits > 0) out.push_back(alphabet[(buffer << (5 - bits)) & 31]);
  return out;
}

Hash hash_bytes(std::string_view kind, std::string_view body) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, kind.data(), kind.size());
  const char zero = 0;
  blake3_hasher_update(&hasher, &zero, 1);
  blake3_hasher_update(&hasher, body.data(), body.size());
  unsigned char out[BLAKE3_OUT_LEN];
  blake3_hasher_finalize(&hasher, out, BLAKE3_OUT_LEN);
  return base32(out, BLAKE3_OUT_LEN);
}

std::string short_hash(const Hash& h, size_t n) { return h.substr(0, n); }

std::string new_uuid() {
  static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                          (uint64_t)std::random_device{}() << 32};
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 2; ++i) {
    uint64_t v = rng();
    for (int j = 0; j < 16; ++j) out.push_back(hex[(v >> (j * 4)) & 15]);
  }
  return out;
}

}  // namespace hs
