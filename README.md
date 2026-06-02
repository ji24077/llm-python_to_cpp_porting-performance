# Python → C++ Markdown Renderer Port & LLM Performance Comparison

Port a Python Markdown-to-HTML renderer (`markdown-it-py`, CommonMark preset) to a
standalone **C++17** executable with **byte-for-byte identical** output, then use it
as a testbed to compare two LLM-generated C++ ports of the same task on **speed** and
**correctness**.

This repository has two goals:

1. **Porting:** reproduce the observable behavior of the Python reference exactly in C++.
2. **LLM comparison:** measure two independently LLM-produced C++ ports — one by
   **Cursor**, one by **Claude Code** — head-to-head.

---

## Project Structure

```text
python_reference/      Python reference renderer (the behavioral authority)
  reference.py         stdin -> MarkdownIt("commonmark").render -> stdout
cpp_solution/          C++17 port (mdrender.cpp + generated entities_data.h)
visible_tests/         Sample inputs + expected HTML outputs
benchmark/             large_document.md (~256 KB) used for timing
writeup_template.md    Detailed writeup (approach, validation, optimization, comparison)
porting_plan.md        Porting plan / feature matrix
```

## Build & Run

```bash
g++ -std=c++17 -O2 -o cpp_solution/mdrender cpp_solution/mdrender.cpp
./cpp_solution/mdrender < input.md > output.html
```

The reference (used only as the correctness oracle, never at runtime):

```bash
PYTHONPATH=python_reference python3 python_reference/reference.py < input.md > ref.html
```

## Correctness Model

Correctness = **exact byte match** against the Python reference. Validation strategy:

- **Visible tests** — 5 inputs with expected HTML (verified to SHA-256).
- **Edge cases** — hand-written cases (empty input, whitespace/newlines, escaping,
  incomplete markers, feature combinations).
- **Differential fuzzing** — thousands of randomly generated documents, each rendered
  by both Python and C++ and compared byte-for-byte.
- **Large document** — the ~256 KB benchmark file.

---

## Optimization Methods

Optimization was applied **only after** the port was correct, one change at a time,
re-validating zero-diff after each. Timings are core render time on
`benchmark/large_document.md` (Apple Silicon, clang `-O2`).

| # | Optimization | Bottleneck addressed | Before → After |
|---|---|---|---|
| 1 | Renderer appends into a single output buffer (no per-token temporary strings) + reserve output | Render phase rebuilt many small `std::string` temporaries | 6.80 → 6.63 ms |
| 2 | `reserve()` block-token vector and inline-children vectors | Heavy `Token` structs reallocated as vectors grew | 6.63 → 6.16 ms |
| 3 | `std::move` pending text into its token + reserve `text_join` working vector | Pending buffer copied on every flush | 6.16 → 6.06 ms |
| – | `-O3` / `-O3 -march=native` | Compiler auto-optimization | 6.62 → 6.60 ms (negligible, not kept) |

**Net:** 6.79 ms → 6.05 ms core (~11% / 1.12×). The biggest win is the language port
itself; micro-optimizations add a single-digit-percent improvement on top.

---

## Performance Comparison (Python vs C++)

`benchmark/large_document.md` (~256 KB, ~11.6k lines), same machine:

| | Python reference | C++ port |
|---|---|---|
| Core render | ~108 ms | **6.05 ms** |
| End-to-end (process) | ~155 ms | ~10 ms |
| Speedup | 1× | **~16× core, ~14–16× end-to-end** |

The dominant gain (~16×) comes from interpreter → native execution, not from
micro-optimization.

---

## LLM Comparison: Cursor vs Claude Code

Two independently LLM-generated C++ ports of the identical task (same reference, same
preset, same stdin→HTML→stdout contract), both built with `g++ -std=c++17 -O2` and
measured against the same Python oracle.

### Correctness

| Test set | Cursor | Claude Code |
|---|---|---|
| Visible tests (5) | pass (SHA-256 match) | pass (SHA-256 match) |
| Benchmark document (~256 KB) | zero diff | zero diff |
| Hidden-like categories (46) | **46/46** | 44/46 |
| Differential fuzz (in-domain) | **0 mismatches (10,000+ docs)** | ~81% mismatch (1,615 / 2,000) |

**Defects found in the Claude Code port** (both are core CommonMark features inside the
supported domain):

- **Indented code blocks not implemented** — `    code` renders as `<p>code</p>`
  instead of `<pre><code>code\n</code></pre>` (cause of most fuzz mismatches).
- **`*` / `_` thematic breaks not implemented** — `***`, `___`, `* * *`, `- - -`
  render as paragraphs or malformed nested lists instead of `<hr />`.

The Claude Code port passes the visible tests and the benchmark only because neither
contains those constructs; the gaps surface immediately on broader inputs.

### Speed

| | Python | Cursor C++ | Claude Code C++ |
|---|---|---|---|
| End-to-end (best-of-N) | ~155 ms | ~10 ms | ~10.7 ms |
| Speedup vs Python | 1× | ~16× | ~14.5× |

### Takeaways

- **Speed:** effectively a tie — both ~14–16× faster than Python, within measurement noise.
- **Correctness:** Cursor's port is materially better (zero diff across 10,000+ inputs)
  vs the Claude Code port's missing indented-code and thematic-break handling.
- **Lesson:** passing the visible tests is *not* evidence of a correct port. Breadth of
  **differential testing against the reference** is what exposes missing in-domain
  features.

See `writeup_template.md` for the full methodology, per-optimization detail, and the
complete comparison.

---

## Supported Input Domain

Paragraphs, ATX headings (`#`/`##`/`###`), emphasis (`*`/`_`), strong (`**`/`__`),
inline code, fenced code blocks, block quotes, simple unordered (`-`) and ordered
(`1.`) lists, inline links `[text](url)`, backslash escapes, HTML-escaped characters,
and combinations of the above.

**Out of scope:** tables, strikethrough, linkify, typographer, images, reference-style
links, complex nested lists, raw HTML, plugins.
