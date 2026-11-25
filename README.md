# vsql-hll

`vsql_hll` is a VillageSQL extension that counts how many **different** values
a column holds (for example, how many different users visited your site)
quickly and with very little storage. The answer is an estimate, but a close
one: usually within about 1% of the real count (see
[How accurate is it?](#how-accurate-is-it)).

```sql
INSTALL EXTENSION vsql_hll;

-- About how many different users are in the events table?
SELECT hll_cardinality(hll_agg(hll_hash_bigint(user_id))) FROM events;
```

## Contents

- [Why use it](#why-use-it)
- [How it works](#how-it-works)
- [Build and install](#build-and-install)
- [Example: daily and weekly unique visitors](#example-daily-and-weekly-unique-visitors)
- [Functions](#functions)
- [Saving and loading sketches](#saving-and-loading-sketches)
- [Known limitations](#known-limitations)
- [Common problems](#common-problems)
- [What is in this repository](#what-is-in-this-repository)
- [Running the tests](#running-the-tests)
- [Reporting bugs and asking for features](#reporting-bugs-and-asking-for-features)

## Why use it

`COUNT(DISTINCT user_id)` gives an exact answer, but on a big table it has to
read every row each time you ask. It also cannot combine answers: if you know
there were 1,000 unique visitors on Monday and 1,200 on Tuesday, you still do
not know how many unique visitors there were across both days, because some
people visited on both.

`vsql_hll` solves both problems:

- You save a small summary, called a **sketch**, for each day (or hour, or
  store, or any group you like).
- A sketch is never more than about 12 KB, no matter how many values went
  into it: ten values or a billion.
- Sketches can be **combined**. Merge seven daily sketches and you get the
  number of unique visitors for the whole week, with people who came on
  several days counted only once. You never have to read the original rows
  again.

Use it when a close estimate is good enough and the data is large: unique
visitors, unique search terms, unique devices, and so on. When you need an
exact count, use `COUNT(DISTINCT ...)`.

## How it works

Every count takes three steps:

1. **Hash** each value. A hash function turns a value (a number, some text)
   into a big number that looks random. The same value always gives the same
   number. Use the `hll_hash_*` function that matches your column type.
2. **Add** the hashed values to a sketch. `hll_agg` builds a sketch from many
   rows at once; `hll_add` adds one value to an existing sketch.
3. **Count.** `hll_cardinality` returns the estimated number of different
   values in a sketch.

Adding the same value twice does not change the count, which is exactly what
makes the result a count of *different* values.

## Build and install

You need:

- VillageSQL 0.0.8 (a server build or an install), on Linux or macOS
- CMake 3.18 or newer
- A C++17 compiler (GCC or Clang)

### The development SDK is required

The `hll` column type uses a feature (`variable_length_type()`) that is only
in the extension SDK's development headers, so `CMakeLists.txt` turns them on
with `set(VSQL_USE_DEV_ABI ON)`. Your VillageSQL build must include these
headers (the `include-dev/` folder of the SDK).

### Building

From the root of this repository:

```bash
# Point this at your VillageSQL build folder
# (or at "$HOME/.villagesql/prebuilt" if you used the VillageSQL installer)
export VillageSQL_BUILD_DIR="$HOME/build/villagesql"

./build.sh
```

`build.sh` compiles the extension, packs it into `build/vsql_hll.veb`, and
copies that file to the server's extension folder.

Then, in a VillageSQL client:

```sql
INSTALL EXTENSION vsql_hll;
```

To remove it later, first drop every table column of type `hll`, then run:

```sql
UNINSTALL EXTENSION vsql_hll;
```

## Example: daily and weekly unique visitors

Start with a table of page views. User 2 visits on both days.

```sql
CREATE TABLE page_views (view_date DATE, user_id BIGINT);
INSERT INTO page_views VALUES
  ('2026-11-01', 1), ('2026-11-01', 2), ('2026-11-01', 2),
  ('2026-11-02', 2), ('2026-11-02', 3),
  ('2026-11-08', 9);
```

Save one sketch per day. `hll` is the column type this extension adds:

```sql
CREATE TABLE daily_visitors (view_date DATE PRIMARY KEY, visitors hll);

INSERT INTO daily_visitors
  SELECT view_date, hll_agg(hll_hash_bigint(user_id))
  FROM page_views GROUP BY view_date;
```

Unique visitors per day:

```sql
SELECT view_date, hll_cardinality(visitors) AS unique_visitors
FROM daily_visitors ORDER BY view_date;
```

Unique visitors for the first week, from the saved sketches only. User 2
visited on both days but is counted once:

```sql
SELECT hll_cardinality(hll_union_agg(visitors)) AS unique_visitors
FROM daily_visitors WHERE view_date BETWEEN '2026-11-01' AND '2026-11-07';
```

Record one more visitor (user 42) on November 1st:

```sql
UPDATE daily_visitors SET visitors = hll_add(visitors, hll_hash_bigint(42))
WHERE view_date = '2026-11-01';
```

## Functions

If any input is NULL, a function returns NULL (aggregates skip NULL rows
instead).

### Hash functions

Turn a value into a number that can be added to a sketch. Pick the one that
matches your column type. The results are large numbers and can be negative;
that is normal.

| Function | Use it for |
|---|---|
| `hll_hash_bigint(number)` | Whole-number columns (`INT`, `BIGINT`, ...) |
| `hll_hash_text(text)` | Text columns (`VARCHAR`, `TEXT`, ...) |
| `hll_hash_boolean(value)` | True/false columns. Any non-zero value counts as true. |
| `hll_hash_bytea_hex(hex_text)` | Binary data written as hex text. Wrap a binary column with `HEX()`: `hll_hash_bytea_hex(HEX(my_blob))`. A leading `\x` is allowed. |

`hll_hash_bytea_hex` returns NULL with a warning if the text is not valid hex
(an odd number of characters, or characters other than `0-9`, `a-f`, and
`A-F`).

Always hash the same kind of value with the same function. For example,
`hll_hash_text('42')` and `hll_hash_bigint(42)` give different numbers, so
mixing them would count 42 twice.

### Sketch functions

| Function | What it does |
|---|---|
| `hll_empty()` | Returns a new, empty sketch. |
| `hll_add(sketch, hash)` | Returns the sketch with one more hashed value added. |
| `hll_union(sketch_a, sketch_b)` | Combines two sketches into one. Values that are in both are counted once. |
| `hll_cardinality(sketch)` | Returns the estimated number of different values, as a whole number. |

```sql
-- Count 'apple' and 'banana' using two separate sketches combined into one
SELECT hll_cardinality(
         hll_union(hll_add(hll_empty(), hll_hash_text('apple')),
                   hll_add(hll_empty(), hll_hash_text('banana'))));
```

### Aggregate functions

These work across many rows, like `SUM` or `COUNT`, and can be used with
`GROUP BY`.

| Function | What it does |
|---|---|
| `hll_agg(hash)` | Builds one sketch from the hashed values in all the rows. |
| `hll_union_agg(sketch)` | Combines the sketches in all the rows into one sketch. |

Combining sketches loses nothing: merging seven daily sketches gives exactly
the sketch you would get by building one from all seven days of rows.

If there are no rows at all, both return NULL. If every row is NULL,
`hll_agg` returns an empty sketch and `hll_union_agg` returns NULL.

### How accurate is it?

Each sketch is about 12 KB. With that size the estimate is usually within
about 1% of the real count, and small counts (a few dozen values) are usually
exact.

There is one range where the error is larger. The extension uses one formula
for smaller counts and another for larger counts, and switches between them
at around 40,000 different values. In that range the estimate is typically
2–3% too high, and a single sketch can be a little more than that (for
example, 40,000 different values gave an estimate of 41,216). The error then
shrinks again and is back under 1% by about 70,000 values.

## Saving and loading sketches

- **Saving:** store sketches in a column of type `hll`, as in the example
  above. An empty sketch uses 3 bytes; any other sketch uses 12,291 bytes.
- **Viewing:** selecting an `hll` column shows the sketch as text: `\x`
  followed by hex digits.
- **Loading:** insert that text back into an `hll` column. In MySQL strings a
  backslash must be doubled, so write `'\\x...'`, or leave the `\x` out.
- **Comparing:** `=` and `<>` work on `hll` columns. Two sketches are equal
  when they hold the same information.
- **Converting:** `CAST` to or from `hll` does not work. Use the insert and
  select steps above instead.

Text that is not a valid sketch is refused with error 3219, and the message
says what is wrong.

## Known limitations

### You cannot index an `hll` column

An index, `PRIMARY KEY`, or `UNIQUE` key on an `hll` column is refused:

```
ERROR 1071 (42000): Specified key was too long; max key length is 3072 bytes
```

MySQL allows at most 3,072 bytes in an index key, and a sketch with values
in it takes 12,291 bytes.

### The aggregates cannot be used as window functions

`hll_agg(...) OVER (...)` and `hll_union_agg(...) OVER (...)` give a syntax
error, because the server does not accept `OVER` after an extension's
aggregate function. Use `GROUP BY` instead, or save one sketch per group and
combine them with `hll_union_agg`.

`hll_agg(DISTINCT ...)` is also a syntax error. You do not need it: a sketch
already counts each value only once.

### The sketch size is fixed

Every sketch uses the same settings (log2m 14 and regwidth 6): about 12 KB.
You cannot choose a smaller sketch with a larger error, or a bigger one with
a smaller error. The fixed size is what lets any two sketches be combined.

### Estimates are less accurate around 40,000 values

See [How accurate is it?](#how-accurate-is-it).

### Aggregate results inside a subquery in `FROM` or `WITH`

When the result of `hll_agg` or `hll_union_agg` comes out of a subquery in
`FROM` or a `WITH` query, other `hll` functions cannot use it:

```sql
-- Fails: argument 1 must be a custom type or string constant
SELECT hll_cardinality(a)
FROM (SELECT hll_agg(hll_hash_bigint(user_id)) AS a FROM page_views) t;
```

Call the function around the aggregate directly instead, or save the sketch
in a table first:

```sql
SELECT hll_cardinality(hll_agg(hll_hash_bigint(user_id))) FROM page_views;
```

Sketches read from a table column work fine in subqueries.

### Most built-in MySQL functions do not accept `hll` values

Functions such as `LENGTH`, `HEX`, `CONCAT`, `COALESCE`, `IF`, and
`JSON_OBJECT` refuse an `hll` value with an error like
`Incorrect usage of length and hll`. `CAST` to or from `hll` does not work
either. Use the `hll_*` functions, and use a plain `SELECT` of the column when
you need the sketch as text.

### Sorting by a sketch has no meaning

`=` and `<>` tell you whether two sketches are the same. `ORDER BY`, `<`, and
`>` also work on `hll` columns, but they compare the stored bytes, so the
order tells you nothing about the counts. Sort by `hll_cardinality(...)`
instead.

### `hll_add` and `hll_agg` do not check that the value is a hash

Both accept any whole number. If you pass the original value instead of its
hash, for example `hll_add(sketch, user_id)`, there is no error, but the
count is wrong: `hll_cardinality(hll_add(hll_empty(), 5))` returns 0. Always
wrap values in an `hll_hash_*` function. Text passed where a number is
expected is turned into a number: text like `'5'` with no warning at all, and
text that is not a number (like `'abc'`) with only a warning.

### Other limits

| Limitation | What to do instead |
|---|---|
| No function takes binary data directly. | Convert it to hex text first: `hll_hash_bytea_hex(HEX(column))`. |
| There is no hash function for every column type. | Use `hll_hash_bigint` for whole numbers and `hll_hash_text` for everything else. Note that `hll_hash_bigint` rounds decimals (`hll_hash_bigint(1.9)` is the same as `hll_hash_bigint(2)`). |
| If `hll_union_agg` is given an invalid sketch, the error is reported for the whole group, not for the bad row. | Run `hll_cardinality(sketch)` on each row to find the one that fails. |

## Common problems

**`INSTALL EXTENSION vsql_hll` says the extension cannot be found.**
The file `vsql_hll.veb` is not in the server's extension folder. Run
`SHOW VARIABLES LIKE 'veb_dir';` to see where that folder is, and copy
`build/vsql_hll.veb` there. Also check the name: it is `vsql_hll` with an
underscore, not `vsql-hll`.

**`UNINSTALL EXTENSION vsql_hll` fails with error 3219.**
Some table still has an `hll` column. The error message names one of them.
Drop those columns or tables first.

**Inserting a sketch fails with error 3219.**
The text is not a valid sketch, and the error message says why. Also check
that the `\x` at the start is written as `\\x` in your SQL.

**`hll_hash_bytea_hex` returns NULL with warning 3200.**
The input is not valid hex: it has an odd number of characters or a character
other than `0-9`, `a-f`, and `A-F`.

## What is in this repository

```
vsql-hll/
├── manifest.json          # extension name, version, and description
├── CMakeLists.txt         # build settings
├── build.sh               # builds and installs the extension in one step
├── cmake/
│   └── FindVillageSQL.cmake
├── src/
│   ├── hll_sketch.h       # the counting logic: hashing, adding, combining,
│   ├── hll_sketch.cc      #   estimating, and reading/writing saved sketches
│   └── vsql_hll.cc        # the SQL side: the hll type and all SQL functions
└── mysql-test/
    ├── t/                 # test scripts: hll_hash, hll_sketch, hll_agg
    └── r/                 # the expected output of each test script
```

## Running the tests

See [TESTING.md](TESTING.md).

## Reporting bugs and asking for features

Open an issue at
[github.com/kash2104/vsql-hll/issues](https://github.com/kash2104/vsql-hll/issues).
Please include:

- What you did (the SQL you ran)
- What you expected to happen, and what happened instead
- Your VillageSQL version (`SELECT VERSION();`)

For general VillageSQL questions, see the
[VillageSQL documentation](https://villagesql.com/docs) or join the
[VillageSQL Discord](https://discord.gg/KSr6whd3Fr).

## License

GPL-2.0. See [LICENSE](LICENSE).
