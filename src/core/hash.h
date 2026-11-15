#pragma once
// Content hashing: BLAKE3-256, written as lowercase base32 (RFC 4648 alphabet,
// no padding). 32 bytes -> 52 characters. Short forms are prefixes of it.
#include <string>
#include <string_view>

namespace hs {

using Hash = std::string;  // 52-char base32; empty means "none"

Hash hash_bytes(std::string_view kind, std::string_view body);
std::string short_hash(const Hash& h, size_t n = 10);
std::string new_uuid();  // random 128-bit id, 32 hex chars (from the OS's secure generator)
// n bytes from the operating system's cryptographically secure generator, as
// 2n lowercase hex characters. For secrets: keys, session tokens, salts.
std::string random_hex(size_t n);

}  // namespace hs
