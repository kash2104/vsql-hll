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

#include <string>
#include <string_view>

#include "hll_sketch.h"

using vsql::CustomArg;
using vsql::CustomResult;
using vsql::INT;
using vsql::IntArg;
using vsql::IntResult;
using vsql::make_extension;
using vsql::make_func;
using vsql::make_type;
using vsql::STRING;
using vsql::StringArg;
using vsql::StringResult;

using hll::kFullBytes;
using hll::StoredKind;
using hll::StoredView;

static constexpr std::string_view kInvalidSketch =
    "argument is not a valid hll value";

static StoredView view(CustomArg sketch) {
  const auto bytes = sketch.value();
  return hll::view_stored(bytes.data(), bytes.size());
}

template <typename Result>
static bool ensure_capacity(Result &out, size_t needed) {
  if (out.buffer().size() >= needed) return true;
  out.set_length(needed);
  return false;
}

static void hll_from_string(std::string_view text, CustomResult out) {
  try {
    if (!ensure_capacity(out, kFullBytes)) return;
    std::string error;
    const size_t len = hll::import_text(text, out.buffer().data(), &error);
    if (len == 0) {
      out.error(error);
      return;
    }
    out.set_length(len);
  } catch (...) {
    out.error("hll: internal error");
  }
}

static void hll_to_string(CustomArg sketch, StringResult out) {
  if (sketch.is_null()) {
    out.set_null();
    return;
  }
  if (view(sketch).kind == StoredKind::kInvalid) {
    out.error(kInvalidSketch);
    return;
  }
  const auto bytes = sketch.value();
  const size_t needed = hll::hex_text_length(bytes.size());
  if (!ensure_capacity(out, needed)) return;
  hll::encode_hex(bytes.data(), bytes.size(), out.buffer().data());
  out.set_length(needed);
}

// Byte-wise, as postgresql-hll's hll_eq; stored values are canonical.
static int hll_compare(CustomArg a, CustomArg b) {
  if (a.is_null() || b.is_null()) return int{b.is_null()} - int{a.is_null()};
  const auto as_view = [](CustomArg arg) {
    const auto bytes = arg.value();
    return std::string_view(reinterpret_cast<const char *>(bytes.data()),
                            bytes.size());
  };
  const int c = as_view(a).compare(as_view(b));
  return (c > 0) - (c < 0);
}

static constexpr char kHllTypeName[] = "hll";

constexpr auto HLL = make_type<kHllTypeName>()
                         .variable_length_type()
                         .max_persisted_length(kFullBytes)
                         .max_decode_buffer_length(hll::kMaxTextBytes)
                         .from_string<&hll_from_string>()
                         .to_string<&hll_to_string>()
                         .compare<&hll_compare>()
                         .intrinsic_default_str("\\x11ae7f")  // empty sketch
                         .build();

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
        .type(HLL)
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
