# Writeup: Python-to-C++ Markdown Renderer Port

## 1. Implementation Approach

`cpp_solution/mdrender.cpp` (C++17, single translation unit) is a from-scratch port of the
observable behavior of the Python reference (`markdown-it-py` with the `commonmark` preset,
invoked via `python_reference/reference.py`). It reproduces the reference pipeline
`normalize → block → inline → text_join → render` and fixes the preset options
(`html=true`, `xhtmlOut=true`, `breaks=false`, `linkify=false`, `typographer=false`,
`langPrefix="language-"`, `maxNesting=20`). No Python and no third-party Markdown library is
invoked at build or run time; the program reads stdin and writes HTML to stdout.

- **Token model.** A single `Token` struct mirrors the reference token (`type`, `tag`,
  `nesting`, ordered `attrs`, `level`, `children`, `content`, `markup`, `info`, `block`,
  `hidden`). Attributes are an insertion-ordered vector of key/value pairs to preserve emission
  order byte-for-byte.
- **Block parsing.** `StateBlock` reproduces the reference line caches
  (`bMarks/eMarks/tShift/sCount/bsCount`), tab handling, and `is_code_block`. The implemented
  and wired block rules, in dispatch order, are: **fenced code, blockquote (with laziness),
  lists (tight/loose, bullet and ordered with `start`), ATX headings, paragraph.**
- **Inline parsing.** `StateInline` runs the rule chain **text → newline → escape → backticks
  → emphasis → link → entity**, followed by the post-process chain `balance_pairs →
  emphasis.postProcess → fragments_join`. The emphasis algorithm reproduces per-character
  delimiters, the "rule of three", `openersBottom`/`jumps` linear-time matching, and the
  adjacent-run merge into `<strong>`. Links use a single `ruleLink(…, bool silent)`; the
  `skipToken`/`parseLinkLabel` path calls it in silent mode so a valid inner link consumes its
  label as one token and the outer bracket is left literal (CommonMark's no-nested-links rule).
- **Rendering / escaping.** `Renderer` ports `renderToken` (block-newline logic, hidden
  tight-list paragraphs, self-closing `xhtmlOut`), `code_inline`, fenced code (info-string
  split + `language-` class), soft/hard breaks, and `escapeHtml` (`& < > "`).
- **Whitespace parity.** `stripWs` (applied to paragraph/heading content and fence info) trims
  exactly the code-point set Python's `str.strip()` removes — ASCII whitespace plus the full
  Unicode-whitespace set (NBSP, U+2000–200A, U+1680, U+2028/2029, U+202F, U+205F, U+3000,
  U+0085, control 0x1C–0x1F). It is UTF-8-aware but decodes only the edge code points being
  trimmed; interior bytes and all delimiter-classification logic are untouched.
- **Entities.** The HTML5 named-entity table (`cpp_solution/entities_data.h`) is generated once
  from Python's `html.entities.html5` (a pure data table). `ruleEntity` resolves named
  (`&copy;`), decimal (`&#65;`), and hex (`&#x41;`) entities, mapping invalid code points to
  U+FFFD and leaving unknown names literal, matching the reference.

Strings are processed as UTF-8 bytes; because every CommonMark structural marker is ASCII,
byte-offset slicing equals the reference's code-point slicing for the supported domain.

## 2. Correctness Validation Performed

- **Visible tests:** all 5 (`basic_paragraph`, `code_link`, `heading_inline`, `list_quote`,
  `mixed`) produce **zero diff** — both against the committed `visible_tests/expected/` files
  and against a live run of the Python reference.
- **Benchmark document:** `benchmark/large_document.md` produces **zero diff** vs the reference.
- **Targeted differential probes** (C++ vs live Python, exact byte diff): HTML escaping of
  `& < > "`; backslash escapes; emphasis/strong (including interior runs); inline code and the
  backtick cache; headings; fenced code with info strings; tight/loose and ordered lists;
  blockquotes; inline links (titles, `<...>` destinations, nested-link literal handling);
  named/numeric/invalid/unknown entities; NBSP and the full Unicode-whitespace trim set (and
  confirmation that non-whitespace code points such as U+200B/ZWSP, U+FEFF, U+180E are
  *preserved*); CRLF/CR normalization; and NUL → U+FFFD. All zero diff.
- **Diff method:** `diff <(PYTHONPATH=python_reference python3 python_reference/reference.py <
  in) <(./cpp_solution/mdrender < in)` — exact byte comparison; any nonzero diff is a failure.
- **Behavior-preserving optimization check:** the optimized build and the pre-optimization
  baseline were compiled separately and their outputs confirmed **byte-identical on every input
  above**, proving the retained change alters no observable behavior.

## 3. Benchmark Method

- **Hardware / OS / compiler:** Apple Silicon (arm64), macOS 26.x, Apple clang 21.0.0.
- **Build (canonical):** `g++ -std=c++17 -O2 -o cpp_solution/mdrender cpp_solution/mdrender.cpp`
  (`-O3`/`-march=native` were measured and gave no reliable improvement, so `-O2` is used).
- **Timing:** end-to-end process wall time via Python's `time.perf_counter` around
  `subprocess.run`, with 2 warmup runs discarded and the **median** of the timed runs reported
  (medians over repeated batches, not a single short run).
- **Official input:** `benchmark/large_document.md` (256,704 bytes, 11,600 lines), median of 21
  runs.
- **Supporting "100×" input:** the official input concatenated 100× into a single 25,670,400-byte
  document, built with `for i in $(seq 1 100); do cat benchmark/large_document.md; done >
  /tmp/big.md`, median of 11 runs. Purpose: at this size the actual parse/render compute
  dominates and fixed process-launch/I/O overhead becomes negligible (run-to-run spread ~1–2%),
  giving a low-noise signal for whether a renderer change improves real work. This is supporting
  evidence only — **not** the official end-to-end result.

## 4. Optimizations Attempted

Profiling used the macOS `sample` time-profiler on the 100× input (Apple clang does not support
`gprof`/`-pg`, and no GCC/valgrind was available). The profile showed the remaining cost split
between `std::vector<Token>` reallocation and per-character output-string construction
(`escapeHtml` + `std::string::push_back` + `append`). Each change below was applied
individually and re-validated (zero diff vs the reference) before measuring.

| # | Optimization | Target / hypothesis | Correctness | Result | Kept? |
|---|---|---|---|---|---|
| 1 | **`escapeHtml`: bulk-append clean runs** instead of per-character `out += c`; emit an entity only at `& < > "` | Top renderer string-append cost; most bytes are ordinary text | Zero diff; output proven byte-identical to baseline | 100× compute **782.7 → 738.2 ms (~5.7%)**; official end-to-end 12.78 → 12.45 ms | **Yes** |
| 2 | `renderToken`: `result.reserve(4096)` | Pre-size per-token output string | Zero diff | **−6.7%** (regression: forces a heap allocation for SSO-sized tag strings on every call) | No (reverted) |
| 3 | `tokenizeBlock`: `tokens.reserve(256)` | Pre-size token vector to cut reallocation | Zero diff | **−0.1%** (negligible: function is recursive on a shared vector; total tokens ≫ 256) | No (reverted) |
| — | `-O3` / `-O3 -march=native` | Compiler auto-improvement | Zero diff | No reliable gain | No (kept `-O2`) |

Only optimization #1 was retained. It is renderer-only and provably output-identical; the
reverted attempts are documented here for completeness.

## 5. Baseline and Final Runtime Results

Exact medians (commands and method per §3):

**Official benchmark** (`benchmark/large_document.md`, 256,704 bytes; median of 21 runs):

| Subject | Median |
|---|---|
| Python reference | 175.09 ms |
| C++ baseline (pre-`escapeHtml`) | 12.78 ms |
| C++ final (with `escapeHtml`) | 12.45 ms |

- **Official end-to-end improvement:** 12.78 → 12.45 ms ≈ **2.6%** for this run. End-to-end time
  (~12 ms) is dominated by fixed process-launch/I/O overhead, so this figure is noisy and has
  ranged **~1–3%** across runs; it should be read as a small, partly-noise-bound improvement,
  not a headline.
- **Speedup vs Python:** ≈ **14×** end-to-end (the ratio moves with Python's own variance,
  measured 159–175 ms across sessions).

**100× compute signal** (25,670,400 bytes; median of 11 runs) — supporting evidence:

| Subject | Median |
|---|---|
| C++ baseline | 782.7 ms |
| C++ final | 738.2 ms |

- **Compute improvement:** 782.7 → 738.2 ms ≈ **5.7%** (4.8–5.7% across runs). This is the
  reliable evidence that the `escapeHtml` change genuinely speeds up parse/render work, even
  though the end-to-end headline barely moves.

**Note on stdin reading (`fread`).** The final baseline reads stdin via a bulk 64 KB `fread`
loop rather than a per-byte `std::istreambuf_iterator`. This I/O choice is already part of the
measured baseline above. No pre-`fread` source snapshot of this code lineage was preserved and
the read was later entangled with correctness changes, so the isolated contribution of that
choice **cannot be independently re-verified from the current evidence** and is not claimed as a
separately measured optimization.

## 6. Remaining Limitations

- **Deferred / not implemented** (out of scope per the task README): images, autolinks, raw HTML
  (inline and block), reference-style links and definitions, indented code blocks, thematic
  breaks (`hr`), and setext headings. The renderer escapes raw inline/block HTML rather than
  passing it through, which is the only class of differences observed against the reference, and
  all such inputs are outside the documented supported domain.
- **Emphasis flanking** uses ASCII punctuation/whitespace classification for bytes ≥ 0x80
  (treated as ordinary word characters). This can differ from the reference only when an
  emphasis marker sits directly adjacent to non-ASCII punctuation; all-ASCII inputs and UTF-8
  text not adjacent to markers match exactly. (The `stripWs` whitespace path, by contrast, is
  full-Unicode-aware.)
- **Disabled preset rules** (tables, strikethrough, linkify, typographer, smartquotes,
  replacements) are intentionally absent, matching the `commonmark` preset.
- No behavioral differences were observed within the supported domain across the visible tests,
  the benchmark document, or the targeted differential probes in §2.
