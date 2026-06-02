# Porting Plan (Revised, Evidence-Based): markdown-it-py CommonMark → C++17

> Goal: a `diff`-clean `cpp_solution/mdrender`, with a
> vertical-slice build that is runnable from minute ~12 and correct for all **confirmed
> required** features well before the deadline. Optimization is strictly post-correctness.

---

## 1. Task Contract (with file evidence)

| Requirement | Value | Evidence |
|---|---|---|
| Executable | `cpp_solution/mdrender`, standalone C++17, no Python at runtime | `README.md:39-51`, `:108-112` |
| stdin/stdout | read all of stdin → render → write HTML to stdout | `README.md:31-49`; `reference.py:11-14` (`sys.stdin.read()` → `MarkdownIt("commonmark").render` → `sys.stdout.write`) |
| Run form | `./cpp_solution/mdrender < input.md > cpp_output.html` | `README.md:47-49` |
| **Build command** | **Not specified in repo. No Makefile/CMake found.** Inferred single-TU build: `g++ -std=c++17 -O2 -o cpp_solution/mdrender cpp_solution/mdrender.cpp` | Only `entities_data.h` + `mdrender.cpp` in `cpp_solution/` (see §note); `README.md:108` requires C++17 |
| Correctness check | exact output match; zero `diff`: `diff -u python.out cpp.out` | `README.md:55-67` |
| Benchmark | `... reference.py < benchmark/large_document.md > /dev/null` vs `./cpp_solution/mdrender < benchmark/large_document.md > /dev/null`; repeated measurements; report baseline + optimized | `README.md:150-165` |
| Allowed deps | C++ stdlib only; **no** existing markdown library; **no** porting of non-Python markdown-it implementations; do not call Python; do not hardcode test outputs | `README.md:108-116` |
| Deliverables | source + writeup (`writeup_template.md`: needs benchmark method, per-optimization table, baseline/final runtimes) + transcripts | `README.md:189-202`; `writeup_template.md` |

**Reference config (`presets/commonmark.py`):** `html=true`, `xhtmlOut=true`, `breaks=false`,
`linkify=false`, `typographer=false`, `langPrefix="language-"`, `maxNesting=20`. Crucially,
`linkify`/`replacements`/`smartquotes` are **not** active in commonmark — they need not be ported.

**§ Note on existing C++ files:** `cpp_solution/` contained only `entities_data.h` at session
start; `mdrender.cpp` (113 KB) and a compiled `mdrender` binary appeared mid-session (env
artifact, not produced by this read-only analysis). This plan is written as a clean porting plan;
the implementation agent may reuse that scaffold but must validate it against the checkpoints below
rather than trust it.

---

## 2. Feature Matrix

### 2A. Features OBSERVED in visible tests and/or benchmark → required

Benchmark evidence: `benchmark/large_document.md` = 400 identical ~29-line sections, **0 non-ASCII
bytes**, **0** images / autolinks / `&name;` entities / reference-defs / raw-HTML tags. It exercises
exactly the set below. (`grep` counts run against the file.)

| # | Feature | Example input | Python expected output | Source rule | Priority |
|---|---|---|---|---|---|
| 1 | Paragraph + soft line break | `a\nb` | `<p>a\nb</p>` | rules_block/paragraph + rules_inline/newline (softbreak→`\n` since breaks=false) | MUST |
| 2 | HTML escaping `< > & "` | `5 < 10 & "x"` | `5 &lt; 10 &amp; &quot;x&quot;` | common/utils.escapeHtml; renderer.text | MUST |
| 3 | ATX heading `#`/`##`/`###` | `# T` | `<h1>T</h1>` | rules_block/heading | MUST |
| 4 | Emphasis `*x*` / `_x_` | `*it*` | `<em>it</em>` | emphasis + balance_pairs + fragments_join | MUST |
| 5 | Strong `**x**` / `__x__` | `**b**` | `<strong>b</strong>` | emphasis + balance_pairs | MUST |
| 6 | Inline code `` `x` `` | `` `c` `` | `<code>c</code>` | rules_inline/backticks; renderer.code_inline | MUST |
| 7 | Fenced code (+lang info) | ```` ```python\nx\n``` ```` | `<pre><code class="language-python">x\n</code></pre>\n` | rules_block/fence; renderer.fence (`langPrefix`) | MUST |
| 8 | Bullet list `-` (tight) | `- a\n- b` | `<ul>\n<li>a</li>\n<li>b</li>\n</ul>` | rules_block/list (hidden tight paragraphs) | MUST |
| 9 | Ordered list `1.` (tight) | `1. a\n2. b` | `<ol>\n<li>a</li>\n<li>b</li>\n</ol>` | rules_block/list | MUST |
| 10 | Blockquote `>` (+recursion) | `> a\n> b` | `<blockquote>\n<p>a\nb</p>\n</blockquote>` | rules_block/blockquote (recurses block.tokenize) | MUST |
| 11 | Inline link `[t](u)` | `[t](https://example.com/1)` | `<a href="https://example.com/1">t</a>` | rules_inline/link + helpers + normalize_url | MUST |
| 12 | Backslash escape | `\*x\*` | `*x*` | rules_inline/escape | MUST |
| 13 | Core: newline normalize + text_join | (whole doc) | — | rules_core/normalize, text_join | MUST (affects all output) |

Observed in visible link/fence URLs and benchmark: link targets are **plain ASCII**
(`https://example.com/N`) → `normalizeLink` returns them unchanged. A **minimal ASCII-passthrough
`normalizeLink`** (validate scheme, percent-encode only the few chars Python would) satisfies all
confirmed inputs; full mdurl/punycode is **not** required for confirmed cases (see §5).

### 2B. Active CommonMark features NOT observed → hidden-test risk (do NOT label "out of scope" without the README quote)

| Feature | Rule | README scope evidence | Hidden-test risk | Priority |
|---|---|---|---|---|
| HTML entities `&amp;`, `&#65;` | rules_inline/entity + unescapeAll | "escaping" is an explicit hidden-test axis (`:139-145`); entities are spec escaping | **Medium-High** — if input has `&name;`, omitting entity → double-escape `&amp;amp;` | SHOULD |
| Unicode text near emphasis | state_inline.scanDelims flanking | "whitespace and newline behavior" + general domain; not excluded | **Medium** — drives byte-vs-codepoint decision (§5) | SHOULD (handle narrowly) |
| Images `![a](u)` | rules_inline/image | Explicitly out: "images" (`:96`) | Low (excluded) | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Raw HTML in/out | html_block, html_inline | Explicitly out: "raw HTML input handling" (`:99`) | Low–Med (`html=true`; a stray `<div>` would pass through in Python) | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Reference links / defs `[a]: u` | rules_block/reference | Explicitly out: "reference-style links" (`:98`) | Low–Med (a `[x]: u` line is consumed by Python, changing surrounding output) | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Autolink `<https://…>` | rules_inline/autolink | Out: "linkify/autolinking of plain URLs" (`:94`) — ambiguous re `<...>` form | Low–Med | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Setext headings `===`/`---` | rules_block/lheading | Domain lists only ATX `#`/`##`/`###` (`:75`) | Low–Med (`---` under text changes parse) | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Thematic break `***`/`---`/`___` | rules_block/hr | Not in domain list (`:71-85`) | Low–Med | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Indented code block (4 sp) | rules_block/code | Domain lists "fenced" only | Low–Med (indentation also drives lists) | DEFER-ONLY-WITH-EXPLICIT-RISK |
| Complex/nested lists | rules_block/list | Out: "complex nested lists" (`:97`); loose lists & 1-level nesting still plausible | Med | SHOULD (tight + simple loose + 1-level nest) |

**Cross-cutting correctness caveat:** block rules consult each other during *paragraph
continuation / terminator* checks. Deferring a rule's rendering is safe for confirmed inputs only
because none of those constructs appear. For hidden inputs containing a deferred construct,
paragraph boundaries could shift. Mitigation if a DEFER feature must be honored: implement its cheap
`silent`/terminator detector (so boundaries match) even before full rendering.

---

## 3. Minimal Vertical-Slice Implementation Order

Each step ends **compilable + runnable**, with an immediate `diff` checkpoint (commands in §4).

- **Step 0 — Wiring (compiles & runs):** `main()` reads all stdin, applies `normalize` (CRLF/CR→`\n`,
  NUL→U+FFFD), and echoes text through `escapeHtml` wrapped in nothing yet → emit a single `<p>` for
  the whole input. Token struct + `escapeHtml` + ordered-attr container. Establishes the build line.
- **Step 1 — Paragraph + escaping + softbreak:** real paragraph block rule (blank-line separation,
  multi-line → one `<p>` with `\n`), `text` + `newline`(softbreak) inline, `renderToken`/`text`
  renderer. → **diff `basic_paragraph.md`**.
- **Step 2 — ATX headings:** heading block rule (1–3 `#`, trailing-`#` trim). → diff heading lines.
- **Step 3 — Inline emphasis/strong + inline code:** `scanDelims`, delimiter stack, `balance_pairs`
  (openersBottom + jumps), `emphasis.postProcess`, `fragments_join`; `backticks` + `code_inline`.
  → **diff `heading_inline.md`** (the emphasis/code stressor).
- **Step 4 — Fenced code:** fence block rule (open/close marker run, info string), `fence` renderer
  with `langPrefix="language-"`; add the **indented-code detector** only as a terminator helper so
  boundaries match. → diff fence portions of `code_link.md`, `mixed.md`.
- **Step 5 — Lists + blockquote:** bullet + ordered list (tight-list hidden-paragraph logic),
  blockquote with **bounded recursion** into block tokenize (enforce `maxNesting=20`).
  → **diff `list_quote.md`**.
- **Step 6 — Inline links + backslash escape:** `escape` rule; `parseLinkLabel` /
  `parseLinkDestination` / `parseLinkTitle`; **minimal ASCII `normalizeLink`** (scheme validate +
  conservative percent-encode). → **diff `code_link.md`, `mixed.md`** (links).
  → **SAFE STOPPING POINT: run full visible diff (all 5) + benchmark diff.**
- **Step 7 — Entities (SHOULD):** `entity` inline rule + `unescapeAll` (used by fence info & link
  dest/title too). → diff generated entity battery.
- **Defer:** image, html_block/inline, reference, autolink, lheading, hr, indented-code rendering —
  add only if a checkpoint/hidden risk forces it, each behind its own diff.

URL/punycode/reference/HTML work is intentionally last and mostly deferred.

---

## 4. Correctness Checkpoints (exact commands)

Reusable diff (run per stage; `X` = test stem):
```bash
PYTHONPATH=python_reference python3 python_reference/reference.py < visible_tests/X.md > /tmp/py.out
./cpp_solution/mdrender < visible_tests/X.md > /tmp/cpp.out
diff -u /tmp/py.out /tmp/cpp.out            # must be empty
diff -u visible_tests/expected/X.html /tmp/cpp.out   # cross-check vs committed expected
```
Full visible sweep + benchmark (after Step 6 and after every later change):
```bash
for X in basic_paragraph heading_inline code_link list_quote mixed; do
  PYTHONPATH=python_reference python3 python_reference/reference.py < visible_tests/$X.md > /tmp/py.$X
  ./cpp_solution/mdrender < visible_tests/$X.md > /tmp/cpp.$X
  diff -u /tmp/py.$X /tmp/cpp.$X && echo "OK $X" || echo "FAIL $X"
done
PYTHONPATH=python_reference python3 python_reference/reference.py < benchmark/large_document.md > /tmp/py.bench
./cpp_solution/mdrender < benchmark/large_document.md > /tmp/cpp.bench
diff -u /tmp/py.bench /tmp/cpp.bench && echo "BENCH OK"
```
**Generated differential inputs** (create only for in-scope / hidden-risk features; pipe each
through both binaries and diff — the Python reference is the authority):
- Step 0/1: empty (`printf ''`), no-trailing-newline, trailing blank lines, CRLF/CR
  (`printf 'a\r\nb\r\n'`) — newline normalization.
- Escaping: `< > & "`, `\* \_ \\ \` `, escape before non-escapable.
- Emphasis boundaries: `*a*`, `**a**`, `a*b*c`, `foo**b**`, `_a_`, intraword `foo_bar_baz`,
  unclosed `*x`, `***a***`, mixed `**a*b***`.
- Inline code: `` `c` ``, `` ``a`b`` ``, unclosed `` `c ``, code with `< & >`.
- Fences: with/without lang, unclosed fence to EOF, fence body containing `< > &`, indented fence.
- Lists: tight vs loose (blank line between items), ordered start ≠ 1, `-` vs trailing content.
- Links: `[t](u)`, `[t]()`, `[t](u "ti")` (title→DEFER note), empty label `[](u)`.
- Nesting (bounded): blockquote→list, list→blockquote, depth near 20.
- **Hidden-risk — entities:** `&amp;`, `&lt;`, `&#65;`, `&#x41;`, `&nope;`.
- **Hidden-risk — Unicode:** emphasis around `café`, NBSP/typographic chars adjacent to `*`
  (validates §5 byte-vs-codepoint decision).
- **DEFER-risk probes:** `<div>x</div>`, `[a]: http://x`, `<https://x>`, setext `H\n===`, `---`
  — run to *document the gap* vs Python, not necessarily to fix.

**Rule:** no optimization is accepted until every applicable correctness diff above passes.

---

## 5. Data-Structure Decisions Requiring Evidence

| Question | Finding | Decision |
|---|---|---|
| `Token.meta` observable? | Only `renderer.list_item_open` reads `meta["checked"]`, set by GFM task-list detection — **not active in commonmark**. | Omit `meta` from the required path (verify with one grep before relying on it). |
| `children is None` vs empty? | Renderer uses truthy checks (`if token.children:` / `if token.children:`) — None and `[]` behave identically. | Represent "no children" as an empty `std::vector<Token>`. |
| Attribute insertion order? | `renderAttrs` iterates `attrs.items()` in insertion order. Required path emits ≤1 attr (`href` for link, `class` for fence); order is only material for multi-attr image/title (deferred). | Use an **ordered `vector<pair<string,string>>`**, never `std::map`. Low risk for MUST features. |
| Ruler dynamism? | `configure()` calls `enableOnly` once at construction (`main.py:131-140`); the render path never enables/disables. | **Hardcode** the fixed rule sequences; skip the dynamic `Ruler`. |
| Recursion state? | blockquote/list recurse into `block.tokenize`; link/image into inline tokenize; bounded by `maxNesting=20`. Visible = 1 level; hidden may nest. | Implement recursion (or explicit stack) **with the `>= maxNesting` bail replicated exactly**. |

**UTF-8 bytes vs `std::u32string` — evidence-driven (this REJECTS the earlier "decode everything"
recommendation):**
- Evidence: benchmark has **0 non-ASCII bytes**; all visible inputs are ASCII. Codepoint vs byte
  indexing only diverges where the parser classifies characters — chiefly `scanDelims` emphasis
  *flanking* (`isWhiteSpace`/`isPunctChar` on adjacent chars). `escapeHtml`, code/fence/text
  passthrough, and link bytes are all byte-safe (UTF-8 passes through untouched).
- Cost: full `u32string` adds a decode/encode pass and ~4× memory on the 256 KB benchmark for **zero
  benefit on confirmed inputs**.
- **Decision:** parse **byte-oriented UTF-8**. Add a *narrow* codepoint-classification helper used
  **only** at delimiter flanking boundaries; UTF-8 lead/continuation bytes classify as "other"
  (letter-like), so `*word*` around Unicode letters already works byte-wise. Escalate to decoding
  only the boundary char (not the whole document) **iff** a Unicode differential test (§4) fails.
  Justified by: ASCII-only confirmed corpus + isolated flanking dependency.

---

## 6. Profiling Plan (hypotheses only)

These are **hypotheses, not conclusions** for the bottleneck ranking:
- H1 `escapeHtml`/`unescapeAll` (runs on every text/code/fence token over the whole doc),
- H2 emphasis delimiter pairing (`balance_pairs.processDelimiters`),
- H3 block line scanning + terminator lookahead (`parser_block.tokenize`),
- H4 output string concatenation (`result += ...`).

**Agent 3 measurement protocol (after all correctness diffs pass):**
1. Build a baseline with `-O2 -g`; record runtime: `for i in $(seq 1 9); do /usr/bin/time -p ./cpp_solution/mdrender < benchmark/large_document.md > /dev/null; done`, take the **median** of ≥9 runs (also record Python baseline the same way).
2. Profile on macOS (darwin): `sample <pid>` during a long run, or Instruments **Time Profiler**
   (`xcrun xctrace record --template 'Time Profiler' --launch -- ./cpp_solution/mdrender …`).
   Fallback: add manual high-resolution `steady_clock` timers around the four phases
   (normalize / block-tokenize / inline-tokenize / render) and print to stderr under a build flag.
3. Rank functions by self-time; **confirm the empirical #1** before changing code. Apply **one**
   optimization at a time, re-run the full §4 diff sweep, keep only correctness-preserving + measured
   wins (`README.md:167-187`). Record each in the `writeup_template.md` table.

---

## 7. Implementation Phases (ordered, correctness-first)

| Phase | Step (§3) | Est. min | Cumulative |
|---|---|---|---|
| Wiring | Step 0 (build line + stdin/out + Token + escapeHtml + normalize) | 12 | 12 |
| Slice | Step 1 paragraph/escape/softbreak + diff | 13 | 25 |
| Slice | Step 2 ATX headings + diff | 8 | 33 |
| Slice | Step 3 emphasis/strong + inline code + diff | 22 | 55 |
| Slice | Step 4 fenced code (+lang, +indent terminator) + diff | 12 | 67 |
| Slice | Step 5 lists + blockquote (recursion) + diff | 22 | 89 |
| Slice | Step 6 links + backslash escape + **full visible+bench diff** | 16 | 105 |
| Hardening | edge-case differential battery (§4) | 8 | 113 |
| Optional | Step 7 entities (SHOULD) | 8 | 121 |
| **Active total** | | **121** | |
| **Reserve** | profiling + 1 measured optimization + benchmark reruns + final full re-diff + writeup fill-in | **29** | **150** |

**Safe stopping point:** end of Step 6 + full visible/bench diff (~105 min). At that point
`mdrender` is correct for **every confirmed required feature** (all 5 visible tests + the benchmark).
Entities (Step 7) and all optimization are improvements beyond the safe point and may be dropped
without failing the confirmed contract. If time is short, skip optimization, not correctness.

---

## Assumption Audit (confirmed / rejected / unverified)

**CONFIRMED**
- Active CommonMark rule set & options (`presets/commonmark.py`); linkify/replacements/smartquotes inactive.
- Visible tests + benchmark are **ASCII-only** and use only the 13 MUST features (grep counts: 0 images/autolinks/entities/ref-defs/raw-HTML; 0 non-ASCII lines; 400 `#` sections).
- Link targets are plain ASCII → `normalizeLink` is effectively identity → minimal ASCII normalizer suffices for confirmed inputs.
- `entities_data.h` is provided; no Makefile/CMake exists; `mdrender.cpp` + binary appeared mid-session (env artifact).
- Children-none↔empty equivalence, ruler is static on the render path, `maxNesting=20` recursion bound.

**REJECTED (from the earlier draft)**
- "Decode the whole document to `std::u32string`" — over-engineered; evidence shows ASCII-only inputs and an isolated flanking dependency → byte-oriented parsing with narrow codepoint handling.
- "Port all 30+ active rules; ~161 min / 16 modules" — not executable in budget and not warranted; README explicitly scopes out images/raw-HTML/reference links, and the benchmark confirms their absence. Replaced with a 13-feature vertical slice + explicit DEFER list.
- Treating every active rule as MUST — rejected in favor of MUST/SHOULD/DEFER tied to test evidence.

**UNVERIFIED (risk-driving open questions)**
- Whether hidden tests contain `&name;` entities, Unicode adjacent to emphasis, raw HTML, reference defs, setext, hr, or indented code — governs the SHOULD/DEFER calls and the byte-vs-codepoint escalation.
- Exact intended build command (inferred `g++ -std=c++17 -O2 …`; no Makefile to confirm).
- Whether `Token.meta` is ever set on the commonmark path (high-confidence "no"; confirm with one grep before omitting).
- The empirical #1 bottleneck — to be measured by Agent 3, not assumed.
