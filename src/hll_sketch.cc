/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "hll_sketch.h"

#include <cstring>

namespace hll {

static uint64_t load_le64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

static int64_t to_bigint(uint64_t h) {
  int64_t out;
  memcpy(&out, &h, sizeof(out));
  return out;
}

static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

static uint64_t fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

template <typename ReadBytes>
static bool murmur3_low(size_t len, ReadBytes read, uint64_t *result) {
  constexpr uint64_t c1 = 0x87c37b91114253d5ULL;
  constexpr uint64_t c2 = 0x4cf5ad432745937fULL;
  uint64_t h1 = 0;
  uint64_t h2 = 0;

  unsigned char block[16];
  const size_t nblocks = len / 16;
  for (size_t i = 0; i < nblocks; ++i) {
    if (!read(i * 16, 16, block)) return false;
    uint64_t k1 = load_le64(block);
    uint64_t k2 = load_le64(block + 8);
    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;
    h1 = rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;
    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;
    h2 = rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  const size_t tail_len = len & 15;
  if (tail_len > 0 && !read(nblocks * 16, tail_len, block)) return false;
  uint64_t k1 = 0;
  uint64_t k2 = 0;
  for (size_t i = 0; i < tail_len; ++i) {
    if (i < 8) {
      k1 ^= static_cast<uint64_t>(block[i]) << (8 * i);
    } else {
      k2 ^= static_cast<uint64_t>(block[i]) << (8 * (i - 8));
    }
  }
  if (tail_len > 8) {
    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;
  }
  if (tail_len > 0) {
    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;
  }

  h1 ^= len;
  h2 ^= len;
  h1 += h2;
  h2 += h1;
  h1 = fmix64(h1);
  h2 = fmix64(h2);
  h1 += h2;
  *result = h1;
  return true;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static std::string_view strip_hex_prefix(std::string_view text) {
  if (text.size() >= 2 && text[0] == '\\' &&
      (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  return text;
}

static bool decode_hex_bytes(const char *hex, size_t n, unsigned char *out) {
  for (size_t i = 0; i < n; ++i) {
    const int hi = hex_value(hex[2 * i]);
    const int lo = hex_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return true;
}

int64_t hash_bytes(const void *data, size_t len) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  uint64_t h = 0;
  murmur3_low(
      len,
      [bytes](size_t offset, size_t count, unsigned char *dst) {
        memcpy(dst, bytes + offset, count);
        return true;
      },
      &h);
  return to_bigint(h);
}

// postgresql-hll hashes the 8 bytes of the int8 in memory (little-endian).
int64_t hash_bigint(int64_t value) {
  const uint64_t v = static_cast<uint64_t>(value);
  unsigned char bytes[8];
  for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(v >> (8 * i));
  return hash_bytes(bytes, sizeof(bytes));
}

int64_t hash_boolean(bool value) {
  const unsigned char byte = value ? 1 : 0;
  return hash_bytes(&byte, 1);
}

bool hash_hex_text(std::string_view text, int64_t *out) {
  const std::string_view hex = strip_hex_prefix(text);
  if (hex.size() % 2 != 0) return false;
  uint64_t h = 0;
  const bool ok = murmur3_low(
      hex.size() / 2,
      [hex](size_t offset, size_t count, unsigned char *dst) {
        return decode_hex_bytes(hex.data() + 2 * offset, count, dst);
      },
      &h);
  if (ok) *out = to_bigint(h);
  return ok;
}

}  // namespace hll
