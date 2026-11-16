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

// HyperLogLog sketch compatible with citusdata/postgresql-hll hll(14, 6).
//
// Storage (postgresql-hll storage spec v1):
//   byte 0  (schema version 1 << 4) | type
//   byte 1  ((regwidth - 1) << 5) | log2m = 0xAE
//   byte 2  (sparseon << 6) | expthresh  = 0x7F
// EMPTY is the 3-byte header alone. FULL adds 16384 6-bit registers packed
// MSB-first (12288 bytes). A FULL value always has a nonzero register.

#ifndef VILLAGESQL_HLL_SRC_HLL_SKETCH_H
#define VILLAGESQL_HLL_SRC_HLL_SKETCH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hll {

constexpr unsigned kLog2m = 14;
constexpr unsigned kRegWidth = 6;
constexpr uint32_t kNumRegisters = 1u << kLog2m;

constexpr size_t kHeaderBytes = 3;
constexpr size_t kPayloadBytes = kNumRegisters * kRegWidth / 8;
constexpr size_t kEmptyBytes = kHeaderBytes;
constexpr size_t kFullBytes = kHeaderBytes + kPayloadBytes;

constexpr size_t hex_text_length(size_t bytes) { return 2 + 2 * bytes; }
constexpr size_t kMaxTextBytes = hex_text_length(kFullBytes);

constexpr uint8_t kTypeEmpty = 1;
constexpr uint8_t kTypeFull = 4;

constexpr std::string_view kBadHexMessage =
    "input must be an even number of hex digits, optionally prefixed by \\x";

// MurmurHash3 x64_128, seed 0.
int64_t hash_bytes(const void *data, size_t len);
int64_t hash_bigint(int64_t value);
int64_t hash_boolean(bool value);
bool hash_hex_text(std::string_view text, int64_t *out);

bool hash_sets_register(uint64_t hash);
void add_hash(unsigned char *payload, uint64_t hash);
void union_payload(unsigned char *dst, const unsigned char *src);
// Estimated distinct count, rounded and capped at the BIGINT maximum.
int64_t cardinality(const unsigned char *payload);

enum class StoredKind { kEmpty, kFull, kInvalid };

struct StoredView {
  StoredKind kind;
  const unsigned char *registers;  // set only for kFull
};

StoredView view_stored(const unsigned char *data, size_t len);
void write_header(unsigned char *out, uint8_t type);

void encode_hex(const unsigned char *data, size_t len, char *out);

// Accepts EMPTY, EXPLICIT, SPARSE or FULL text and stores it as EMPTY or FULL.
// Returns the length written, or 0 with *error set.
size_t import_text(std::string_view text, unsigned char *out,
                   std::string *error);

}  // namespace hll

#endif  // VILLAGESQL_HLL_SRC_HLL_SKETCH_H
