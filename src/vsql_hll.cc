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

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "hll_sketch.h"

using vsql::CustomArg;
using vsql::CustomResult;
using vsql::INT;
using vsql::IntArg;
using vsql::IntResult;
using vsql::make_aggregate_func;
using vsql::make_extension;
using vsql::make_func;
using vsql::make_type;
using vsql::STRING;
using vsql::StringArg;
using vsql::StringResult;

using hll::kEmptyBytes;
using hll::kFullBytes;
using hll::kPayloadBytes;
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

static void set_empty(CustomResult &out) {
  if (!ensure_capacity(out, kEmptyBytes)) return;
  hll::write_header(out.buffer().data(), hll::kTypeEmpty);
  out.set_length(kEmptyBytes);
}

static unsigned char *set_full(CustomResult &out) {
  if (!ensure_capacity(out, kFullBytes)) return nullptr;
  unsigned char *buf = out.buffer().data();
  hll::write_header(buf, hll::kTypeFull);
  out.set_length(kFullBytes);
  return buf + hll::kHeaderBytes;
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

static void hll_empty_impl(CustomResult out) { set_empty(out); }

static void hll_add_impl(CustomArg sketch, IntArg hashed, CustomResult out) {
  if (sketch.is_null() || hashed.is_null()) {
    out.set_null();
    return;
  }
  const StoredView in = view(sketch);
  if (in.kind == StoredKind::kInvalid) {
    out.error(kInvalidSketch);
    return;
  }
  const uint64_t hash = static_cast<uint64_t>(hashed.value());
  if (in.kind == StoredKind::kEmpty && !hll::hash_sets_register(hash)) {
    set_empty(out);
    return;
  }
  unsigned char *registers = set_full(out);
  if (registers == nullptr) return;
  if (in.kind == StoredKind::kFull) {
    memcpy(registers, in.registers, kPayloadBytes);
  } else {
    memset(registers, 0, kPayloadBytes);
  }
  hll::add_hash(registers, hash);
}

static void hll_union_impl(CustomArg a, CustomArg b, CustomResult out) {
  if (a.is_null() || b.is_null()) {
    out.set_null();
    return;
  }
  const StoredView va = view(a);
  const StoredView vb = view(b);
  if (va.kind == StoredKind::kInvalid || vb.kind == StoredKind::kInvalid) {
    out.error(kInvalidSketch);
    return;
  }
  if (va.kind == StoredKind::kEmpty && vb.kind == StoredKind::kEmpty) {
    set_empty(out);
    return;
  }
  unsigned char *registers = set_full(out);
  if (registers == nullptr) return;
  memcpy(registers, va.kind == StoredKind::kFull ? va.registers : vb.registers,
         kPayloadBytes);
  if (va.kind == StoredKind::kFull && vb.kind == StoredKind::kFull) {
    hll::union_payload(registers, vb.registers);
  }
}

static void hll_cardinality_impl(CustomArg sketch, IntResult out) {
  if (sketch.is_null()) {
    out.set_null();
    return;
  }
  const StoredView in = view(sketch);
  switch (in.kind) {
    case StoredKind::kEmpty:
      out.set(0);
      return;
    case StoredKind::kFull:
      out.set(hll::cardinality(in.registers));
      return;
    case StoredKind::kInvalid:
      out.error(kInvalidSketch);
      return;
  }
}

struct SketchState {
  bool has_input = false;
  bool has_registers = false;
  bool invalid = false;
  std::array<unsigned char, kPayloadBytes> payload{};
};

static void sketch_state_clear(SketchState &state) {
  state.has_input = false;
  state.has_registers = false;
  state.invalid = false;
}

static void sketch_state_result(const SketchState &state, CustomResult out) {
  if (state.invalid) {
    out.error(kInvalidSketch);
    return;
  }
  if (!state.has_input) {
    out.set_null();
    return;
  }
  if (!state.has_registers) {
    set_empty(out);
    return;
  }
  unsigned char *registers = set_full(out);
  if (registers == nullptr) return;
  memcpy(registers, state.payload.data(), kPayloadBytes);
}

// A group of only NULL rows gives EMPTY, not NULL.
static void hll_agg_accumulate(SketchState &state, IntArg hashed) {
  state.has_input = true;
  if (hashed.is_null()) return;
  const uint64_t hash = static_cast<uint64_t>(hashed.value());
  if (!hll::hash_sets_register(hash)) return;
  if (!state.has_registers) {
    state.payload.fill(0);
    state.has_registers = true;
  }
  hll::add_hash(state.payload.data(), hash);
}

static void hll_union_agg_accumulate(SketchState &state, CustomArg sketch) {
  if (sketch.is_null()) return;
  const StoredView in = view(sketch);
  if (in.kind == StoredKind::kInvalid) {
    state.invalid = true;
    return;
  }
  state.has_input = true;
  if (in.kind != StoredKind::kFull) return;
  if (state.has_registers) {
    hll::union_payload(state.payload.data(), in.registers);
  } else {
    memcpy(state.payload.data(), in.registers, kPayloadBytes);
    state.has_registers = true;
  }
}

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .type(HLL)
        .func(make_func<&hll_empty_impl>("hll_empty")
                  .returns(HLL)
                  .no_params()
                  .buffer_size(kFullBytes)
                  .deterministic()
                  .build())
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
                  .build())
        .func(make_func<&hll_add_impl>("hll_add")
                  .returns(HLL)
                  .param(HLL)
                  .param(INT)
                  .buffer_size(kFullBytes)
                  .deterministic()
                  .build())
        .func(make_func<&hll_union_impl>("hll_union")
                  .returns(HLL)
                  .param(HLL)
                  .param(HLL)
                  .buffer_size(kFullBytes)
                  .deterministic()
                  .build())
        .func(make_func<&hll_cardinality_impl>("hll_cardinality")
                  .returns(INT)
                  .param(HLL)
                  .deterministic()
                  .build())
        .func(make_aggregate_func<SketchState, &sketch_state_result>("hll_agg")
                  .returns(HLL)
                  .param(INT)
                  .buffer_size(kFullBytes)
                  .deterministic()
                  .clear<&sketch_state_clear>()
                  .accumulate<&hll_agg_accumulate>()
                  .build())
        .func(make_aggregate_func<SketchState, &sketch_state_result>(
                  "hll_union_agg")
                  .returns(HLL)
                  .param(HLL)
                  .buffer_size(kFullBytes)
                  .deterministic()
                  .clear<&sketch_state_clear>()
                  .accumulate<&hll_union_agg_accumulate>()
                  .build()))
