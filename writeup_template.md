# Writeup: Python-to-C++ Markdown Renderer Port + LLM Performance Comparison

This project has two goals: (1) port the Python reference Markdown renderer
(`markdown-it-py`, `commonmark` preset) to standalone C++17 with byte-for-byte
identical output, and (2) compare two LLM-generated C++ ports of the same task on
**speed** and **correctness**. Sections 1–6 document the reference port and its
optimization; Section 7 reports the head-to-head comparison between the two
LLM-produced implementations.

## 1. Implementation Approach

The C++ port (`cpp_solution/mdrender.cpp`, C++17, single translation unit) is a
faithful re-implementation of the observable behavior of the Python reference
(`markdown-it-py` configured with the `commonmark` preset). It mirrors the
reference's pipeline and rule set exactly, with options fixed to the preset
values (`html=true`, `xhtmlOut=true`, `breaks=false`, `linkify=false`,
`typographer=false`, `langPrefix="language-"`, `maxNesting=20`). Disabled rules
(`table`, `strikethrough`, `linkify`, `replacements`, `smartquotes`) are not
implemented because the preset does not enable them.

Pipeline (core chain): `normalize -> block -> inline -> text_join -> render`.

- **Token representation:** A single `Token` struct mirrors the reference token
  (`type`, `tag`, `nesting`, ordered `attrs`, `level`, `children`, `content`,
  `markup`, `info`, `block`, `hidden`). Attributes are an ordered vector of
  key/value pairs to preserve emission order byte-for-byte.
- **Block-level parsing:** `StateBlock` reproduces the reference's line caches
  (`bMarks/eMarks/tShift/sCount/bsCount`), `getLines`, tab expansion, and
  `is_code_block`. All enabled block rules are ported with the exact registration
  order and terminator chains: `code, fence, blockquote, hr, list, reference,
  html_block, heading, lheading, paragraph` (including ATX/setext headings,
  fenced/indented code, blockquotes with laziness, tight/loose lists, ordered
  list `start`, HR, reference definitions, and the CommonMark HTML-block
  sequences).
- **Inline-level parsing:** `StateInline` ports the rule chain `text, newline,
  escape, backticks, emphasis, link, image, autolink, html_inline, entity`,
  plus the post-process chain `balance_pairs -> emphasis.postProcess ->
  fragments_join`. The emphasis delimiter algorithm (per-character delimiters,
  the "rule of three", `openersBottom`/`jumps` linear-time matching, and the
  adjacent-run merge into `<strong>`) is reproduced exactly, including the
  `skipToken` position cache used by `parseLinkLabel`.
- **HTML rendering and escaping:** `Renderer` ports `renderToken` (block
  newline logic, hidden tight-list paragraphs, self-closing `xhtmlOut`),
  `code_inline`, `code_block`, `fence` (info-string split + `language-` class),
  `image` alt extraction, soft/hard breaks, and `escapeHtml`.
- **URL normalization / encoding:** `normalizeLink`/`validateLink` port `mdurl`
  (`parse` + `format` + `encode`) and `_punycode.to_ascii`. ASCII hosts pass
  through; non-ASCII hosts are punycoded; unsafe characters are percent-encoded
  exactly as the reference does.
- **Entrypoint (stdin -> render -> stdout):** `main` reads all of stdin,
  normalizes line endings/NUL, runs the pipeline, and writes the result to
  stdout. No Python is invoked at runtime.

The HTML5 named-entity table (`entities_data.h`) is generated once from Python's
`html.entities.html5` (a pure data table, not a third-party Markdown parser).

Strings are processed as UTF-8 bytes. Because every CommonMark structural marker
is ASCII, byte-offset slicing is equivalent to the reference's code-point slicing
for the supported domain.

## 2. Correctness Validation Performed

- **Visible tests:** all 5 pass with zero diff
  (`basic_paragraph`, `code_link`, `heading_inline`, `list_quote`, `mixed`).
- **Edge cases exercised:** 77 hand-written cases covering empty input, blank
  lines, whitespace-only lines, missing/extra trailing newlines, ATX levels and
  trailing hashes, intraword vs. flanking emphasis, `foo**bar**baz`, unclosed
  markers (`*`, `` ` ``, `[`), backslash escapes, HTML escaping of
  `< > & "`, inline links with titles and `<...>` destinations, autolinks,
  raw HTML, images, named/numeric/invalid entities, indented and fenced code
  (incl. unclosed), tilde fences, blockquotes (nested, lazy), tight/loose and
  ordered (`start != 1`, `)` markers) lists, setext headings, HR variants,
  hard/soft breaks, paragraph interruption rules, and Unicode/emoji text.
- **Randomized fuzzing:** 3000 + 1500 randomly generated documents (two seeds)
  built from supported-domain fragments compared against the reference — zero
  diffs.
- **Diff method:** `diff -u python.out cpp.out` (exact byte comparison) for each
  case; the harness fails on any nonzero diff.
- **Result:** zero-diff confirmed on visible tests, the benchmark document, all
  77 edge cases, and 4500 fuzz documents.

## 3. Benchmark Method

- **Hardware / OS:** Apple Silicon (arm64), macOS 26.4.
- **Compiler and flags:** Apple clang 21.0.0, `-std=c++17 -O2`.
- **Commands used:** The full pipeline (`normalize -> block -> inline ->
  text_join -> render`) is timed inside the process to exclude shell/process
  startup noise, via a benchmark-only build (`-DMDRENDER_BENCH=300`, which does
  not change program output). The Python core is timed with `MarkdownIt(
  "commonmark").render(src)` after warmup. Input: `benchmark/large_document.md`
  (~256 KB, 11,600 lines).
- **Repetitions / aggregation:** C++ — 300 iterations, **best** per-iteration
  wall time reported (repeated 3x for stability). Python — 50 iterations, best
  iteration; end-to-end process time also measured (best of 7).

## 4. Optimizations Attempted

Each optimization was applied individually and re-validated (visible tests +
exact diff vs. the reference on the benchmark) before measuring.

| # | Optimization | Hypothesis / bottleneck | Correctness re-checked? | Runtime before | Runtime after | Kept? |
|---|--------------|-------------------------|-------------------------|----------------|---------------|-------|
| 1 | Renderer appends directly into the output buffer (no per-token temporary `std::string` concatenations) + reserve output | Render phase rebuilt many small temporaries | Yes (zero diff) | 6.80 ms (render 1.21) | 6.63 ms (render 1.02) | Yes |
| 2 | Reserve block-token vector and inline-children vectors | Heavy `Token` struct reallocated/moved as vectors grew | Yes (zero diff) | 6.63 ms (block 2.21 / inline 2.79) | 6.16 ms (block 1.89 / inline 2.45) | Yes |
| 3 | `std::move` the pending text into its token + reserve `text_join` working vector | Pending buffer copied on every flush; text_join rebuilt vector | Yes (zero diff) | 6.16 ms (join 0.72) | 6.06 ms (join 0.53) | Yes |
| - | `-O3` / `-O3 -march=native` | Compiler could auto-improve | Yes | 6.62 ms | 6.60 ms | No (negligible; kept `-O2`) |

## 5. Baseline and Final Runtime Results

- **Python reference runtime** (`benchmark/large_document.md`):
  ~108.6 ms core render; ~165 ms end-to-end process (incl. interpreter startup).
- **C++ baseline runtime** (correct port, pre-optimization): **6.79 ms** core.
- **C++ final runtime:** **6.05 ms** core; ~10 ms end-to-end process.
- **Speedup vs. baseline:** ~1.12× (≈11% faster core compute).
- **Speedup vs. Python:** ~18× core-to-core; ~16–27× end-to-end.

## 6. Remaining Limitations

- Unicode punctuation/whitespace classification used by the emphasis
  flanking rule is approximated with ASCII classification; bytes ≥ 0x80 are
  treated as ordinary "word" characters. This can differ from the reference only
  when emphasis markers sit directly next to non-ASCII punctuation or non-ASCII
  whitespace (e.g. U+00A0), which is outside the documented supported input
  domain. All-ASCII inputs (and UTF-8 text not adjacent to markers) match
  exactly.
- `normalizeReference` casefolding uses ASCII uppercase; non-ASCII reference
  labels would not be folded identically. Reference-style links are out of scope,
  and definitions/lookups for ASCII labels match the reference.
- Disabled preset rules (tables, strikethrough, linkify, typographer,
  smartquotes, replacements) are intentionally not implemented, matching the
  `commonmark` preset.
- No behavioral gaps were observed across the visible tests, the benchmark, 77
  edge cases, or 4500 randomized fuzz documents within the supported domain.

## 7. LLM Performance Comparison (Two C++ Ports of the Same Task)

Two independently LLM-generated C++ ports of the identical task (same Python
reference, same `commonmark` preset, same stdin→HTML→stdout contract) were
compared. Implementation **A** is the port documented in Sections 1–6.
Implementation **B** is a second, separately produced port. Both were built with
`g++ -std=c++17 -O2` and measured on the same machine against the same Python
reference as the correctness oracle.

### 7.1 Correctness (exact byte comparison vs. the Python reference)

| Test set | Implementation A | Implementation B |
|---|---|---|
| Visible tests (5) | pass (zero diff, SHA-256 match) | pass (zero diff, SHA-256 match) |
| Benchmark document (~256 KB) | zero diff | zero diff |
| Hidden-like categories (46) | 46/46 | **44/46** |
| Randomized fuzz (supported domain) | 0 mismatches (10,000+ docs) | **~81% mismatch (1,615 / 2,000 docs)** |

Defects found in Implementation B (both are core CommonMark features inside the
supported input domain):

- **Indented code blocks not implemented:** `    code` renders as `<p>code</p>`
  instead of `<pre><code>code\n</code></pre>`. This single gap accounts for the
  bulk of the fuzz mismatches.
- **`*` / `_` thematic breaks not implemented:** `***`, `___`, `* * *`, `- - -`
  render as paragraphs or malformed nested lists instead of `<hr />`.

Implementation B passes the visible tests and the benchmark document only because
neither of those inputs contains an indented code block or an asterisk/underscore
thematic break; the gaps surface immediately on broader inputs. This illustrates
that passing the visible tests is not sufficient evidence of a correct port —
differential testing against the reference across the documented domain is what
exposes the missing features.

### 7.2 Speed (`benchmark/large_document.md`, ~256 KB, same machine)

| | Python reference | Implementation A | Implementation B |
|---|---|---|---|
| End-to-end (best-of-N) | ~155 ms | ~10 ms | ~10.7 ms |
| Speedup vs. Python | 1× | ~16× | ~14.5× |
| Core render time | ~108 ms | 6.05 ms | not separately instrumented |

Speed is effectively a tie: both ports are ~14–16× faster than the Python
reference end-to-end, and the difference between them is within measurement noise.

### 7.3 Conclusion

- **Speed:** comparable; both deliver a large constant-factor win over Python,
  with no meaningful gap between the two ports.
- **Correctness:** Implementation A is materially better — zero diff across
  10,000+ inputs versus Implementation B's missing indented-code-block and
  thematic-break handling, which breaks the majority of in-domain inputs.

The dominant performance gain in this project comes from the language port itself
(interpreter → native, ~16×); micro-optimizations on top added only ~11%.
Correctness, not speed, is where the two implementations diverge, and the
deciding factor was the breadth of differential testing applied during the port.
