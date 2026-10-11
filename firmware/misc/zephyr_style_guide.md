# Coding style — TH modules

Notes for writing application code the way `buttons/src/main.c` is written. Zephyr's own
coding guidelines (Linux kernel style, errno conventions, `LOG_*` over `printk`, devicetree
naming rules, …) are assumed and not repeated here; this lists only where we **differ** from
or **add to** them. Items marked **(?)** are inferred from a single example and need confirming.

## Formatting

- **4-space indentation**, no tabs. (Deliberately not Zephyr's 8-wide tabs.)
- Blank lines separate ideas:
  - **two** blank lines between top-level items (include block, `LOG_MODULE_REGISTER`,
    `#define`s, prototypes, globals, each function);
  - **one** blank line after the local-variable declarations at the top of a function;
  - **one** blank line between a check-and-return block and the next statement.
- Line length: **(?)** nothing so far exceeds ~90 columns; treat 100 as the ceiling.

## File layout (top to bottom)

1. Includes.
2. `LOG_MODULE_REGISTER(<MODULE>, <LEVEL>);` — module name in upper snake case, log level
   always given explicitly (`LOG_LEVEL_INF` for now).
3. `#define`s for tunables.
4. Prototypes for every `static` function in the file, unless the function is declared in a
   header. Declarations belong near the top so the reader gets the file's "table of contents"
   before any bodies.
5. File-scope data (`static const` tables such as the `gpio_dt_spec` array).
6. `main()` — the first function body in the file. Reading top-down, the entry point comes
   before the helpers it calls.
7. `static` helper functions, in the order `main()` uses them.

## Includes

- Include directly everything the file uses, even when another header pulls it in
  transitively (`<zephyr/sys/util.h>` for `ARRAY_SIZE`, `<zephyr/device.h>` for
  `DEVICE_DT_GET`).
- Grouped, one blank line between groups, alphabetical within a group:
  1. C standard library
  2. Zephyr core: `<zephyr/device.h>`, `<zephyr/kernel.h>`, `<zephyr/sys/*.h>`
  3. Devicetree and drivers: `<zephyr/devicetree.h>`, `<zephyr/drivers/*.h>`, `<zephyr/pm/*.h>`
  4. DT bindings: `<zephyr/dt-bindings/...>`
  5. Logging: `<zephyr/logging/log.h>`, `<zephyr/logging/log_core.h>`

## Naming

- Data names describe what the thing *is* from the application's point of view, not how it
  is wired: `input_keys`, not `key_input_gpios`.
- Helper functions are prefixed with the thing they operate on: `keys_configure()`,
  `keys_read_mask()`, `keys_arm_wake()`; free-standing actions are verb phrases:
  `log_reset_cause()`.
- Time constants carry their unit as a suffix: `POLL_MS`, `DEBOUNCE_MS`, `ACTIVE_TIMEOUT_MS`.

## Variables and declarations

- All locals are declared at the top of the function, each initialised
  (`int err = 0;`, `uint32_t cause = 0;`), followed by a blank line.
- Results of calls are assigned on a separate line, not in the declaration:
  ```c
  int err = 0;

  err = hwinfo_get_reset_cause(&cause);
  ```
  Exception: compile-time values such as `DEVICE_DT_GET(...)` may initialise the declaration.
- Devicetree-backed tables are `static const` arrays of `*_dt_spec`, built with
  `GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), <prop>, <idx>)`, one entry per line.
  **(?)** The initialiser currently has an empty line before the closing `};` — keep or drop?

## Error handling

- A single `int err` per function holds the latest return code; it is checked with `err < 0`
  (not `!= 0` or `if (err)`) and returned unchanged. The last statement of such a function is
  `return err;` even when it can only be `0` there.
- Readiness checks that have no errno of their own return the closest one explicitly
  (`return -ENODEV;` after `gpio_is_ready_dt()` fails).
- The check-and-return block is uniform:
  ```c
  err = gpio_pin_configure_dt(&input_keys[i], GPIO_INPUT | GPIO_PULL_UP);
  if (err < 0) {
      LOG_ERR("Failed to configure gpio %zu! (err: %d)", i, err);
      return err;
  }
  ```
- Non-fatal failures use `LOG_WRN` and continue; fatal ones use `LOG_ERR` and return.

## Log messages

- Sentence-case, no trailing full stop; failure messages end with `!`:
  `"GPIO %zu not ready!"`, `"Failed to configure gpio %zu! (err: %d)"`.
- The errno is appended in parentheses as `(err: %d)`, always last.
- Informational messages are plain statements: `"Key %zu pressed"`, `"Entering System OFF"`.

## Comments

- Full sentences with a capital and a full stop.
- Short comments directly above a function are fine for `static` helpers; no Doxygen blocks
  in application code.

## Devicetree / overlay

- Application pins live under `zephyr,user` as `*-gpios` phandle arrays
  (`key-inputs-gpios = <&gpio0 5 GPIO_ACTIVE_LOW>, ...`).
- Polarity is expressed in the overlay (`GPIO_ACTIVE_LOW`), never in C; C code uses the
  `*_dt()` GPIO API so "active" always means "pressed".
- Overlay indentation is 4 spaces as well; continuation entries of a property align under the
  first `<`.

## Kconfig (`prj.conf`)

- No comments; symbols in the order they were needed rather than sorted.
- Only enable what the code actually calls.

## Agents

- Following the file layout order (includes, prototypes, data, `main()`, helpers) is a nice
  default for the sequence of edits, but not a rule — write blocks in whatever order makes
  them easiest to review.
- After a block is accepted, re-read it before writing the next one — the user often adjusts
  style in place, and the following blocks should match the adjusted version, not the
  original proposal.
- Don't touch `CMakeLists.txt`, `prj.conf` or the overlay in the same edit as C code; those
  are separate, small edits so they can be rejected independently.

## Open questions to settle

- The stray blank lines inherited from the placeholder (`};` in the array initialiser, before
  the final `return err;` in `main()`): intentional or leftovers?
- Line-length limit.
- Whether `bool`-returning predicates are acceptable or everything stays `int`/errno.
