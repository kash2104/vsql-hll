# AGENTS.md

Guidance for AI coding assistants working in the `vsql-hll` repository.

**Note**: Also check `AGENTS.local.md` for machine-specific instructions when present.

## What this extension is

`vsql-hll`, a VillageSQL extension that estimates how many
different values a column holds, using HyperLogLog sketches.

- Extension name (in SQL and `manifest.json`): `vsql_hll`
- Repository name: `vsql-hll`
- Column type added: `hll`
- Version: only in `manifest.json`

## Files

| File | What it contains |
|---|---|
| `src/hll_sketch.h` | Sketch constants and the `hll` namespace API. No VillageSQL headers. |
| `src/hll_sketch.cc` | The sketch logic: MurmurHash3 (`hash_bytes`, `hash_bigint`, `hash_boolean`, `hash_hex_text`), adding a hash (`add_hash`), merging (`union_payload`), the estimate (`cardinality`), and the saved format (`view_stored`, `write_header`, `encode_hex`, `import_text`). |
| `src/vsql_hll.cc` | The SQL side: the `hll` type (`HLL`), one `*_impl` function per SQL function, the aggregate state (`SketchState`), and the `VEF_GENERATE_ENTRY_POINTS()` registration. |
| `mysql-test/t/hll_hash.test` | Tests for the hash functions. |
| `mysql-test/t/hll_sketch.test` | Tests for the `hll` type and the sketch functions. |
| `mysql-test/t/hll_agg.test` | Tests for `hll_agg` and `hll_union_agg`. |
| `mysql-test/r/*.result` | Recorded expected output. Generated, never edited by hand. |
| `CMakeLists.txt` | Builds `vsql_hll.so` from the two `.cc` files and packs `vsql_hll.veb`. |
| `build.sh` | Configure, build, and install in one step. |
| `README.md`, `TESTING.md` | User documentation. Keep them in plain, simple English. |

Keep the split: sketch logic goes in `hll_sketch.*` (testable without a
server); `vsql_hll.cc` only reads arguments, checks NULL and validity, and
writes results.

## SQL functions

| Function | Returns | Notes |
|---|---|---|
| `hll_hash_bigint(BIGINT)` | BIGINT | hashes the 8 bytes, little-endian |
| `hll_hash_text(STRING)` | BIGINT | hashes the raw bytes |
| `hll_hash_boolean(INT)` | BIGINT | hashes one byte; any non-zero value is TRUE |
| `hll_hash_bytea_hex(STRING)` | BIGINT | hex text, optional `\x`; bad hex gives NULL and a warning |
| `hll_empty()` | `hll` | 3-byte empty sketch, `\x11ae7f` |
| `hll_add(hll, BIGINT)` | `hll` | |
| `hll_union(hll, hll)` | `hll` | maximum of each register |
| `hll_cardinality(hll)` | BIGINT | estimate, rounded |
| `hll_agg(BIGINT)` | `hll` | aggregate; skips NULL; only-NULL group gives an empty sketch |
| `hll_union_agg(hll)` | `hll` | aggregate; skips NULL; only-NULL group gives NULL |

All hashes use MurmurHash3 x64_128 with seed 0 and return the first 64-bit
word as a signed BIGINT.
Scalar functions return NULL if any argument is NULL. Every function is
registered with `.deterministic()`.

## The sketch format

Fixed settings: log2m = 14 (16,384 registers), regwidth = 6 bits.

- Byte 0: `(schema version 1 << 4) | type`. Byte 1: `0xAE`. Byte 2: `0x7F`.
- Stored values are always one of two forms:
  - EMPTY: the 3-byte header only (`kEmptyBytes`).
  - FULL: header plus 12,288 bytes of registers packed 6 bits each, high bit
    first (`kFullBytes` = 12,291).
- `import_text` also accepts the compact EXPLICIT and SPARSE text forms and
  converts them to EMPTY or FULL. Because every stored value is in one of the
  two stored forms, `hll_compare` can compare raw bytes.
- The text form is `\x` plus lowercase hex, at most `hll::kMaxTextBytes`
  (24,584) characters.

Do not change these bytes or the hash: that would break sketches users have
already saved.

## VEF patterns used here

Use the typed C++ API: `#include <villagesql/vsql.h>` with one
`using vsql::...` line per name. Check names in the SDK headers, not from
memory.

### The `hll` type

```cpp
constexpr auto HLL = make_type<kHllTypeName>()
                         .variable_length_type()
                         .max_persisted_length(kFullBytes)
                         .max_decode_buffer_length(hll::kMaxTextBytes)
                         .from_string<&hll_from_string>()
                         .to_string<&hll_to_string>()
                         .compare<&hll_compare>()
                         .intrinsic_default_str("\\x11ae7f")  // empty sketch
                         .build();
```

- `variable_length_type()` exists only in the dev SDK headers, which is why
  `CMakeLists.txt` sets `VSQL_USE_DEV_ABI ON`. Do not remove it.
- `hll_from_string` is the only entry point with a `try`/`catch`, because it
  builds `std::string` error messages.

### Returning an `hll`

- Every function or aggregate that returns `hll` must set
  `.buffer_size(kFullBytes)`; the default buffer is too small for a FULL
  sketch.
- Write results with `set_empty(out)` or `set_full(out)` (which returns the
  register area to fill). Both check the buffer size first.

### Aggregates

Both aggregates share `SketchState`, `sketch_state_clear`, and
`sketch_state_result`, and differ only in their `accumulate` function:

```cpp
.func(make_aggregate_func<SketchState, &sketch_state_result>("hll_agg")
          .returns(HLL).param(INT).buffer_size(kFullBytes).deterministic()
          .clear<&sketch_state_clear>()
          .accumulate<&hll_agg_accumulate>()
          .build())
```

- `has_input` decides between NULL and an empty sketch: `hll_agg_accumulate`
  sets it for every row, `hll_union_agg_accumulate` only for non-NULL
  sketches.
- `accumulate` cannot raise an error. An invalid sketch sets `state.invalid`,
  and `sketch_state_result` raises the error for the whole group.

### Errors and warnings

- `out.error(msg)` fails the statement with `ER_VILLAGESQL_GENERIC_ERROR`
  (3219). Invalid text inserted into an `hll` column fails the same way.
- `out.warning(msg)` returns NULL with warning 3200. Only
  `hll_hash_bytea_hex` uses it, for bad hex.
- Invalid `hll` arguments raise `kInvalidSketch`
  (`"argument is not a valid hll value"`).

## Adding a SQL function

1. If it needs new sketch logic, add it to `hll_sketch.h` / `hll_sketch.cc`
   in the `hll` namespace.
2. Add a `static void <name>_impl(...)` in `src/vsql_hll.cc`: arguments first,
   result last; return NULL for NULL arguments; check `view(sketch).kind` for
   `StoredKind::kInvalid`.
3. Register it in `VEF_GENERATE_ENTRY_POINTS()` with `.deterministic()`, and
   `.buffer_size(kFullBytes)` if it returns `hll`.
4. Add checks to the matching `mysql-test/t/*.test` file and record the result.
5. Update the function tables in `README.md` and in this file.

## Build and test

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./build.sh                                   # builds and installs build/vsql_hll.veb

export VSQL_HLL_DIR="$(pwd)"
cd "$VillageSQL_BUILD_DIR/mysql-test" && \
perl mysql-test-run.pl --suite="$VSQL_HLL_DIR/mysql-test"            # run
perl mysql-test-run.pl --suite="$VSQL_HLL_DIR/mysql-test" --record   # re-record
```

- `mysql-test-run.pl` is in the VillageSQL build, not in this repository.
- `--suite` needs an absolute path; use `$HOME` or a variable, never `~`
  after `=`.
- Expect extension errors in tests with `--error ER_VILLAGESQL_GENERIC_ERROR`.
- After recording, review `git diff mysql-test/r/`; a changed hash or estimate
  means saved sketches would no longer match.

## Conventions

- C++17, lowercase_with_underscores, GPL v2 header on every .cc, .h and CMakeLists.txt.
- The typed SDK API only: include <villagesql/vsql.h>. Never read or use anything under an abi/ directory.
- Extension name uses underscores (vsql_hll); the repository uses a hyphen (vsql-hll).
No PR or issue numbers in source or test files.
