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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace hll {

constexpr uint8_t kMaxRegisterValue = (1u << kRegWidth) - 1;
constexpr size_t kMaxImportBytes = kHeaderBytes + 8 * 8192;
constexpr uint8_t kSchemaVersion = 1;
constexpr uint8_t kTypeUndefined = 0;
constexpr uint8_t kTypeExplicit = 2;
constexpr uint8_t kTypeSparse = 3;
constexpr uint8_t kParamByte = ((kRegWidth - 1) << 5) | kLog2m;
constexpr uint8_t kCutoffByte = (1u << 6) | 63;
constexpr unsigned kSparseEntryBits = kLog2m + kRegWidth;

static_assert(kRegWidth == 6, "register packing assumes 6-bit registers");
static_assert(kPayloadBytes % 3 == 0, "registers are packed 4 per 3 bytes");
static_assert(kSparseEntryBits == 20, "read_sparse_entry assumes 20 bits");

static constexpr uint8_t type_byte(uint8_t type) {
  return (kSchemaVersion << 4) | type;
}

static uint64_t load_le64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

static uint64_t load_be64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
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

void encode_hex(const unsigned char *data, size_t len, char *out) {
  static constexpr char kDigits[] = "0123456789abcdef";
  *out++ = '\\';
  *out++ = 'x';
  for (size_t i = 0; i < len; ++i) {
    *out++ = kDigits[data[i] >> 4];
    *out++ = kDigits[data[i] & 0x0f];
  }
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

static uint32_t load_group(const unsigned char *p) {
  return (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
}

static void store_group(unsigned char *p, uint32_t g) {
  p[0] = static_cast<unsigned char>(g >> 16);
  p[1] = static_cast<unsigned char>(g >> 8);
  p[2] = static_cast<unsigned char>(g);
}

static constexpr unsigned group_shift(unsigned k) { return 18 - 6 * k; }

static void raise_register(unsigned char *payload, uint32_t i, uint8_t value) {
  unsigned char *p = payload + (i >> 2) * 3;
  const unsigned shift = group_shift(i & 3);
  const uint32_t g = load_group(p);
  if (value <= ((g >> shift) & kMaxRegisterValue)) return;
  store_group(p, (g & ~(uint32_t{kMaxRegisterValue} << shift)) |
                     (uint32_t{value} << shift));
}

static bool payload_is_zero(const unsigned char *payload) {
  return std::all_of(payload, payload + kPayloadBytes,
                     [](unsigned char b) { return b == 0; });
}

bool hash_sets_register(uint64_t hash) { return (hash >> kLog2m) != 0; }

void add_hash(unsigned char *payload, uint64_t hash) {
  if (!hash_sets_register(hash)) return;
  const uint32_t index = static_cast<uint32_t>(hash & (kNumRegisters - 1));
  unsigned rank = static_cast<unsigned>(__builtin_ctzll(hash >> kLog2m)) + 1;
  if (rank > kMaxRegisterValue) rank = kMaxRegisterValue;
  raise_register(payload, index, static_cast<uint8_t>(rank));
}

void union_payload(unsigned char *dst, const unsigned char *src) {
  for (size_t off = 0; off < kPayloadBytes; off += 3) {
    const uint32_t s = load_group(src + off);
    if (s == 0) continue;
    const uint32_t d = load_group(dst + off);
    uint32_t merged = 0;
    for (unsigned k = 0; k < 4; ++k) {
      const unsigned shift = group_shift(k);
      merged |= std::max((d >> shift) & kMaxRegisterValue,
                         (s >> shift) & kMaxRegisterValue)
                << shift;
    }
    store_group(dst + off, merged);
  }
}

static constexpr std::array<double, kMaxRegisterValue + 1> make_inv_pow2() {
  std::array<double, kMaxRegisterValue + 1> table{};
  double v = 1.0;
  for (size_t r = 0; r < table.size(); ++r) {
    table[r] = v;
    v /= 2.0;
  }
  return table;
}
static constexpr auto kInvPow2 = make_inv_pow2();

static double estimate(const unsigned char *payload) {
  // Summed in register order, as postgresql-hll does, so results match.
  double sum = 0.0;
  uint32_t zeros = 0;
  for (size_t off = 0; off < kPayloadBytes; off += 3) {
    const uint32_t g = load_group(payload + off);
    for (unsigned k = 0; k < 4; ++k) {
      const uint32_t r = (g >> group_shift(k)) & kMaxRegisterValue;
      sum += kInvPow2[r];
      if (r == 0) ++zeros;
    }
  }
  const double m = kNumRegisters;
  const double alpha = 0.7213 / (1.0 + 1.079 / m);
  const double e = alpha * m * m / sum;

  if (zeros != 0 && e < 5.0 * m / 2.0) return m * std::log(m / zeros);

  // postgresql-hll uses 2^76 here, which overflows its 64-bit shift; the
  // hash space is 2^64.
  const double two_to_l = std::ldexp(1.0, 64);
  if (e <= two_to_l / 30.0) return e;
  if (e >= two_to_l) return std::numeric_limits<double>::infinity();
  return -two_to_l * std::log(1.0 - e / two_to_l);
}

int64_t cardinality(const unsigned char *payload) {
  constexpr double kLimit = 9223372036854774784.0;  // largest double < 2^63
  const double e = estimate(payload);
  if (!(e < kLimit)) return std::numeric_limits<int64_t>::max();
  return std::llround(e);
}

StoredView view_stored(const unsigned char *data, size_t len) {
  if (len >= kHeaderBytes && data[1] == kParamByte && data[2] == kCutoffByte) {
    if (len == kEmptyBytes && data[0] == type_byte(kTypeEmpty)) {
      return {StoredKind::kEmpty, nullptr};
    }
    if (len == kFullBytes && data[0] == type_byte(kTypeFull)) {
      return {StoredKind::kFull, data + kHeaderBytes};
    }
  }
  return {StoredKind::kInvalid, nullptr};
}

void write_header(unsigned char *out, uint8_t type) {
  out[0] = type_byte(type);
  out[1] = kParamByte;
  out[2] = kCutoffByte;
}

static bool read_sparse_entry(const char *hex, size_t pos, uint32_t *entry) {
  unsigned char window[3];
  if (!decode_hex_bytes(hex + 2 * (pos >> 3), 3, window)) return false;
  *entry = (load_group(window) >> (24 - kSparseEntryBits - (pos & 7))) &
           ((1u << kSparseEntryBits) - 1);
  return true;
}

size_t import_text(std::string_view text, unsigned char *out,
                   std::string *error) {
  const std::string_view hex = strip_hex_prefix(text);
  if (hex.size() % 2 != 0) {
    *error = kBadHexMessage;
    return 0;
  }
  const size_t len = hex.size() / 2;
  if (len < kHeaderBytes) {
    *error = "hll value is shorter than its 3-byte header";
    return 0;
  }
  if (len > kMaxImportBytes) {
    *error = "hll value is too long";
    return 0;
  }
  unsigned char header[kHeaderBytes];
  if (!decode_hex_bytes(hex.data(), kHeaderBytes, header)) {
    *error = kBadHexMessage;
    return 0;
  }
  const unsigned version = header[0] >> 4;
  const unsigned type = header[0] & 0x0f;
  if (version != kSchemaVersion) {
    *error = "unsupported hll schema version " + std::to_string(version);
    return 0;
  }
  if (header[1] != kParamByte) {
    *error = "hll value uses log2m=" + std::to_string(header[1] & 0x1f) +
             " and regwidth=" + std::to_string((header[1] >> 5) + 1) +
             "; vsql_hll requires log2m=14 and regwidth=6";
    return 0;
  }

  const char *body = hex.data() + 2 * kHeaderBytes;
  const size_t body_len = len - kHeaderBytes;
  unsigned char *payload = out + kHeaderBytes;
  memset(payload, 0, kPayloadBytes);

  switch (type) {
    case kTypeEmpty:
      if (body_len != 0) {
        *error = "EMPTY hll value must be exactly 3 bytes";
        return 0;
      }
      break;

    case kTypeExplicit: {
      if (body_len % 8 != 0) {
        *error = "EXPLICIT hll value must hold whole 8-byte elements";
        return 0;
      }
      int64_t previous = 0;
      for (size_t n = 0; n < body_len / 8; ++n) {
        unsigned char element_bytes[8];
        if (!decode_hex_bytes(body + 16 * n, 8, element_bytes)) {
          *error = kBadHexMessage;
          return 0;
        }
        const uint64_t element = load_be64(element_bytes);
        const int64_t signed_element = to_bigint(element);
        if (n > 0 && signed_element <= previous) {
          *error = "EXPLICIT hll elements must be strictly ascending";
          return 0;
        }
        previous = signed_element;
        add_hash(payload, element);
      }
      break;
    }

    case kTypeSparse: {
      const size_t total_bits = body_len * 8;
      const size_t count = total_bits / kSparseEntryBits;
      if (total_bits - count * kSparseEntryBits >= 8) {
        *error = "SPARSE hll value has inconsistent padding";
        return 0;
      }
      for (size_t n = 0; n < count; ++n) {
        uint32_t entry = 0;
        if (!read_sparse_entry(body, n * kSparseEntryBits, &entry)) {
          *error = kBadHexMessage;
          return 0;
        }
        raise_register(payload, entry >> kRegWidth,
                       static_cast<uint8_t>(entry & kMaxRegisterValue));
      }
      break;
    }

    case kTypeFull:
      if (body_len != kPayloadBytes) {
        *error = "FULL hll value must be exactly " +
                 std::to_string(kFullBytes) + " bytes";
        return 0;
      }
      if (!decode_hex_bytes(body, kPayloadBytes, payload)) {
        *error = kBadHexMessage;
        return 0;
      }
      break;

    default:
      *error = type == kTypeUndefined
                   ? "UNDEFINED hll values are not supported"
                   : "unknown hll type " + std::to_string(type);
      return 0;
  }

  if (payload_is_zero(payload)) {
    write_header(out, kTypeEmpty);
    return kEmptyBytes;
  }
  write_header(out, kTypeFull);
  return kFullBytes;
}

}  // namespace hll
