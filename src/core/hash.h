#pragma once
// Content hashing: BLAKE3-256, written as lowercase base32 (RFC 4648 alphabet,
// no padding). 32 bytes -> 52 characters. Short forms are prefixes of it.
#include <string>
#include <string_view>

namespace hs {

using Hash = std::string;  // 52-char base32; empty means "none"

Hash hash_bytes(std::string_view kind, std::string_view body);
std::string short_hash(const Hash& h, size_t n = 10);
std::string new_uuid();  // random 128-bit id, 32 hex chars

}  // namespace hs
