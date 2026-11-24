# Testing vsql-hll

The tests for this extension are SQL scripts in `mysql-test/t/`. They run with
the MySQL test runner, `mysql-test-run.pl`, which comes with your VillageSQL
build. The runner starts its own temporary server, so you do not need to
start one yourself.

## The test scripts

| Script | What it checks |
|---|---|
| `mysql-test/t/hll_hash.test` | The four hash functions (`hll_hash_bigint`, `hll_hash_boolean`, `hll_hash_text`, `hll_hash_bytea_hex`): that bad hex returns NULL with a warning, that NULL in gives NULL out, and that they work on table columns. |
| `mysql-test/t/hll_sketch.test` | `hll_empty`, `hll_add`, `hll_union`, and `hll_cardinality`: duplicates, small counts, NULL inputs, saving sketches in a table and updating them, loading sketches from text, and that invalid text is refused. |
| `mysql-test/t/hll_agg.test` | `hll_agg` and `hll_union_agg`: estimates for a large number of values, grouping with `GROUP BY`, groups with no rows or only NULL rows, and that combining daily sketches gives the same result as building one sketch from all the rows. |

For each script, `mysql-test/r/` holds the expected output (a `.result` file).
A test passes when the script's output matches that file exactly.

Each script installs the extension at the start, and drops its tables and
uninstalls the extension at the end.

## Step 1: Build and install the extension

From the root of this repository:

```bash
export VillageSQL_BUILD_DIR="$HOME/build/villagesql"   # your VillageSQL build folder
./build.sh
```

This builds `build/vsql_hll.veb` and copies it to
`$VillageSQL_BUILD_DIR/veb_output_directory`, where the test runner looks
for extensions.

## Step 2: Run the tests

Still in the root of this repository, save its path, then move to the
build's test folder and start the runner:

```bash
export VSQL_HLL_DIR="$(pwd)"

cd "$VillageSQL_BUILD_DIR/mysql-test" && \
perl mysql-test-run.pl --suite="$VSQL_HLL_DIR/mysql-test"
```

The runner prints `[ pass ]` or `[ fail ]` for each script and a summary at
the end.

Add `--parallel=auto` to run the scripts at the same time.

## Updating the expected output

If you change a test script, or change what a function returns, record the
new expected output:

```bash
cd "$VillageSQL_BUILD_DIR/mysql-test" && \
perl mysql-test-run.pl --suite="$VSQL_HLL_DIR/mysql-test" --record
```

This rewrites the files in `mysql-test/r/`. Never edit those files by hand.
Before committing them, read the changes with `git diff mysql-test/r/` and
make sure every change is one you meant to make.

## Adding a test

- Put new checks in the script for that area (`hll_hash`, `hll_sketch`, or
  `hll_agg`), or add a new `.test` file in `mysql-test/t/`.
- When a statement is supposed to fail, put
  `--error ER_VILLAGESQL_GENERIC_ERROR` on the line before it. That is the
  error (number 3219) this extension raises, for example for invalid sketch
  text.
- Warnings, such as the one `hll_hash_bytea_hex` gives for bad hex, are
  written into the `.result` file automatically.
- Drop every table that has an `hll` column before
  `UNINSTALL EXTENSION vsql_hll;`, otherwise the uninstall fails.
- When a check allows some error in an estimate, remember that just above
  40,000 different values the estimate can be up to about 3% too high (see
  "How accurate is it?" in the README). Allow for that, or test other sizes.
- Then record the expected output as shown above, and check it.

## If something goes wrong

**`Can't open perl script "mysql-test-run.pl": No such file or directory`**
The runner is not in this repository; it is in your VillageSQL build. Run
the `cd "$VillageSQL_BUILD_DIR/mysql-test"` step first. Joining the two
commands with `&&`, as shown above, stops the run if the `cd` fails.


**The tests use an old version of the extension**
The runner takes `vsql_hll.veb` from `veb_output_directory` first. After you
change the code, run `./build.sh` again so that copy is replaced.


## Quick manual check

To try the extension by hand in a VillageSQL client:

```sql
INSTALL EXTENSION vsql_hll;
SELECT hll_cardinality(hll_add(hll_empty(), hll_hash_text('a')));
UNINSTALL EXTENSION vsql_hll;
```
