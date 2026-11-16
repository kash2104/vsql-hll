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

#ifndef VILLAGESQL_HLL_SRC_HLL_SKETCH_H
#define VILLAGESQL_HLL_SRC_HLL_SKETCH_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hll {

constexpr std::string_view kBadHexMessage =
    "input must be an even number of hex digits, optionally prefixed by \\x";

// MurmurHash3 x64_128, seed 0.
int64_t hash_bytes(const void *data, size_t len);
int64_t hash_bigint(int64_t value);
int64_t hash_boolean(bool value);
bool hash_hex_text(std::string_view text, int64_t *out);

}  // namespace hll

#endif  // VILLAGESQL_HLL_SRC_HLL_SKETCH_H
