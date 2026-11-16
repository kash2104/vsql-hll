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

#include <villagesql/vsql.h>

#include <string_view>

#include "hll_sketch.h"

using vsql::INT;
using vsql::IntArg;
using vsql::IntResult;
using vsql::make_extension;
using vsql::make_func;
using vsql::STRING;
using vsql::StringArg;

static void hll_hash_text_impl(StringArg value, IntResult out) {
  if (value.is_null()) {
    out.set_null();
    return;
  }
  const std::string_view v = value.value();
  out.set(hll::hash_bytes(v.data(), v.size()));
}

static void hll_hash_bigint_impl(IntArg value, IntResult out) {
  if (value.is_null()) {
    out.set_null();
    return;
  }
  out.set(hll::hash_bigint(value.value()));
}

static void hll_hash_bytea_hex_impl(StringArg hex, IntResult out) {
  if (hex.is_null()) {
    out.set_null();
    return;
  }
  int64_t hash = 0;
  if (!hll::hash_hex_text(hex.value(), &hash)) {
    out.warning(hll::kBadHexMessage);
    return;
  }
  out.set(hash);
}

static void hll_hash_boolean_impl(IntArg value, IntResult out) {
  if (value.is_null()) {
    out.set_null();
    return;
  }
  out.set(hll::hash_boolean(value.value() != 0));
}

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .func(make_func<&hll_hash_text_impl>("hll_hash_text")
                  .returns(INT)
                  .param(STRING)
                  .deterministic()
                  .build())
        .func(make_func<&hll_hash_bigint_impl>("hll_hash_bigint")
                  .returns(INT)
                  .param(INT)
                  .deterministic()
                  .build())
        .func(make_func<&hll_hash_bytea_hex_impl>("hll_hash_bytea_hex")
                  .returns(INT)
                  .param(STRING)
                  .deterministic()
                  .build())
        .func(make_func<&hll_hash_boolean_impl>("hll_hash_boolean")
                  .returns(INT)
                  .param(INT)
                  .deterministic()
                  .build()))
