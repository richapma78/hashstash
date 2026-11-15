#ifdef _WIN32
#define _CRT_RAND_S  // rand_s: the OS's secure generator (RtlGenRandom)
#endif
#include "core/hash.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

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

std::string random_hex(size_t n) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(2 * n);
#ifdef _WIN32
  for (size_t i = 0; i < n; i += 4) {
    unsigned int v = 0;
    if (rand_s(&v) != 0) throw std::runtime_error("secure random generator failed");
    for (size_t j = 0; j < 4 && i + j < n; ++j) {
      unsigned char b = (unsigned char)(v >> (8 * j));
      out.push_back(hex[b >> 4]);
      out.push_back(hex[b & 15]);
    }
  }
#else
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) throw std::runtime_error("cannot open /dev/urandom");
  for (size_t i = 0; i < n; ++i) {
    int c = std::fgetc(f);
    if (c == EOF) {
      std::fclose(f);
      throw std::runtime_error("cannot read /dev/urandom");
    }
    out.push_back(hex[(c >> 4) & 15]);
    out.push_back(hex[c & 15]);
  }
  std::fclose(f);
#endif
  return out;
}

std::string new_uuid() { return random_hex(16); }

}  // namespace hs
