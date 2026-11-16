# vsql-hll

`vsql_hll` is a VillageSQL extension that counts how many **different** values
a column holds (for example, how many different users visited your site)
quickly and with very little storage. The answer is an estimate, but a close
one: on average it is off by less than 1%.

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

- A VillageSQL server build, or a VillageSQL install, on Linux or macOS
- CMake 3.18 or newer
- A C++17 compiler (GCC or Clang)

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

Each sketch is about 12 KB. With that size the estimate is, on average, off
by less than 1%. Small counts (a few dozen values) are usually exact.

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
[github.com/villagesql/villagesql-server/issues](https://github.com/villagesql/villagesql-server/issues).
Please include:

- What you did (the SQL you ran)
- What you expected to happen, and what happened instead
- Your VillageSQL version (`SELECT VERSION();`)

For general VillageSQL questions, see the
[VillageSQL documentation](https://villagesql.com/docs) or join the
[VillageSQL Discord](https://discord.gg/KSr6whd3Fr).

## License

GPL-2.0. See [LICENSE](LICENSE).
