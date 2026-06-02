// mdrender.cpp — standalone CommonMark-subset renderer (C++17).
// Port of python_reference (MarkdownIt("commonmark")). The Python reference is the
// sole behavioral authority; output must match it byte-for-byte.
//
// Build: g++ -std=c++17 -O2 -o cpp_solution/mdrender cpp_solution/mdrender.cpp
//
// STAGE 0-1 scope: stdin -> normalize -> block(paragraph only) -> inline(text+newline)
//                  -> HTML render -> stdout.
// Unicode strategy: byte-oriented UTF-8 (all structural markers are ASCII; UTF-8 is
// self-synchronizing). Escalate to codepoint handling only if a differential test fails.

#include <array>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "entities_data.h"

// ---------------------------------------------------------------------------
// Options (fixed for the "commonmark" preset)
// ---------------------------------------------------------------------------
static constexpr bool kXhtmlOut = true;   // self-closing <br />
static constexpr bool kBreaks   = false;  // softbreak -> "\n", not <br>

// ---------------------------------------------------------------------------
// Token
// ---------------------------------------------------------------------------
struct Token {
    std::string type;
    std::string tag;
    int nesting = 0;  // +1 open, 0 self-closing, -1 close
    std::vector<std::pair<std::string, std::string>> attrs;  // insertion-ordered
    std::string content;
    std::string markup;
    std::string info;
    bool block = false;
    bool hidden = false;
    int level = 0;
    std::vector<Token> children;  // populated only for "inline" tokens
};

// ---------------------------------------------------------------------------
// common/utils — character classification + escaping
// ---------------------------------------------------------------------------
static inline bool isStrSpace(char c) { return c == ' ' || c == '\t'; }

// Exact set of code points Python str.strip() removes (verified against CPython:
// chr(c).strip()=="" for c in 0..0x10FFFF). Used ONLY by stripWs() edge trimming.
static inline bool isStripWsCp(unsigned int cp) {
    switch (cp) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
        case 0x85: case 0xA0: case 0x1680:
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
        case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A:
        case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return false;
    }
}

// Decode one UTF-8 code point starting at i (bounded by e). On success sets cp/len and
// returns true; on an invalid/truncated sequence returns false (caller stops trimming).
static inline bool decodeCp(const std::string& s, size_t i, size_t e, unsigned int& cp,
                            size_t& len) {
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    int n;
    if (c0 < 0x80) { cp = c0; len = 1; return true; }
    else if ((c0 & 0xE0) == 0xC0) { n = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { n = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { n = 4; cp = c0 & 0x07; }
    else return false;  // continuation byte or invalid lead
    if (i + static_cast<size_t>(n) > e) return false;
    for (int k = 1; k < n; ++k) {
        unsigned char ck = static_cast<unsigned char>(s[i + k]);
        if ((ck & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (ck & 0x3F);
    }
    len = static_cast<size_t>(n);
    return true;
}

// escapeHtml: escape & < > "  (NOT '). Single pass; '&' handled inline so no re-escaping.
// Clean runs (no special char) are bulk-appended instead of per-char push_back; output is
// byte-identical to the per-char version.
static std::string escapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t run = 0;  // start of the current run of ordinary characters
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c != '&' && c != '<' && c != '>' && c != '"') continue;
        if (i > run) out.append(s, run, i - run);
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
        }
        run = i + 1;
    }
    if (run < n) out.append(s, run, n - run);
    return out;
}

// Strip leading/trailing whitespace exactly like Python str.strip(), UTF-8 aware.
// Only the edge code points are decoded (no whole-string UTF-32 conversion); interior
// bytes and all delimiter-classification logic are untouched. This is the sole path that
// needs full Unicode-whitespace parity (paragraph/heading content, fence info).
static std::string stripWs(const std::string& s) {
    size_t b = 0, e = s.size();
    // leading: decode forward, trim while the code point is Python-stripped whitespace
    while (b < e) {
        unsigned int cp;
        size_t len;
        if (!decodeCp(s, b, e, cp, len) || !isStripWsCp(cp)) break;
        b += len;
    }
    // trailing: find the last code point's start (walk back over continuation bytes), trim
    while (e > b) {
        size_t p = e - 1;
        while (p > b && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
        unsigned int cp;
        size_t len;
        if (!decodeCp(s, p, e, cp, len) || p + len != e || !isStripWsCp(cp)) break;
        e = p;
    }
    return s.substr(b, e - b);
}

// Markdown ASCII punctuation: ! " # $ % & ' ( ) * + , - . / : ; < = > ? @ [ \ ] ^ _ ` { | } ~
static inline bool isMdAsciiPunct(int code) {
    switch (code) {
        case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
        case 0x2F: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        case 0x40: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60:
        case 0x7B: case 0x7C: case 0x7D: case 0x7E:
            return true;
        default:
            return false;
    }
}

// isPunctChar approximation (byte-oriented): Python uses Unicode category P*/S*.
// For ASCII bytes this == isMdAsciiPunct; non-ASCII bytes classify as non-punct.
// (Plan: escalate to codepoint classification only if a differential test fails.)
static inline bool isPunctCharByte(unsigned char c) {
    return c < 0x80 && isMdAsciiPunct(c);
}

// isWhiteSpace(code): Zs range || MD_WHITESPACE. Fed single byte values at delim boundaries.
static inline bool isWhiteSpaceCode(int code) {
    if (code >= 0x2000 && code <= 0x200A) return true;
    switch (code) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
        case 0xA0: case 0x1680: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return false;
    }
}

// Default inline text terminators (markdown-it _DEFAULT_TERMINATORS): the text rule
// stops at these, letting other inline rules fire.
static const std::array<bool, 256>& terminatorTable() {
    static const std::array<bool, 256> tbl = [] {
        std::array<bool, 256> t{};
        for (unsigned char c : std::string("\n!#$%&*+-:<=>@[\\]^_`{}~")) t[c] = true;
        return t;
    }();
    return tbl;
}

static inline bool isAsciiAlpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline bool isAsciiAlnum(unsigned char c) {
    return isAsciiAlpha(c) || (c >= '0' && c <= '9');
}

// fromCodePoint: encode a Unicode code point as UTF-8.
static std::string fromCodePoint(long long c) {
    std::string out;
    if (c <= 0x7F) {
        out += static_cast<char>(c);
    } else if (c <= 0x7FF) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

// isValidEntityCode: reject surrogates, noncharacters, control codes, out-of-range.
static bool isValidEntityCode(long long c) {
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    if (c >= 0xFDD0 && c <= 0xFDEF) return false;
    if ((c & 0xFFFF) == 0xFFFF || (c & 0xFFFF) == 0xFFFE) return false;
    if (c >= 0x00 && c <= 0x08) return false;
    if (c == 0x0B) return false;
    if (c >= 0x0E && c <= 0x1F) return false;
    if (c >= 0x7F && c <= 0x9F) return false;
    return !(c > 0x10FFFF);
}

// replaceEntityPattern: decode &name; / &#dd; / &#xhh; ; otherwise return the original match.
static std::string replaceEntityPattern(const std::string& match, const std::string& name) {
    const auto& ents = html5_entities();
    auto it = ents.find(name);
    if (it != ents.end()) return it->second;

    long long code = -1;
    if (name.size() >= 2 && name[0] == '#') {
        if (name[1] != 'x' && name[1] != 'X') {
            std::string digits = name.substr(1);  // #([0-9]{1,8}) fullmatch
            if (!digits.empty() && digits.size() <= 8 &&
                digits.find_first_not_of("0123456789") == std::string::npos) {
                code = std::stoll(digits, nullptr, 10);
            }
        } else {
            std::string hex = name.substr(2);  // #x([a-f0-9]{1,8}) fullmatch
            if (!hex.empty() && hex.size() <= 8 &&
                hex.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                code = std::stoll(hex, nullptr, 16);
            }
        }
    }
    if (code >= 0 && isValidEntityCode(code)) return fromCodePoint(code);
    return match;
}

// unescapeAll: resolve backslash escapes and entity references (UNESCAPE_ALL_RE).
// Used for fence info strings (and later: link dest/title). NOT the inline entity rule.
static std::string unescapeAll(const std::string& s) {
    if (s.find('\\') == std::string::npos && s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && isMdAsciiPunct(static_cast<unsigned char>(s[i + 1]))) {
            out += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '&') {
            size_t j = i + 1;
            if (j < n && (isAsciiAlpha(static_cast<unsigned char>(s[j])) || s[j] == '#')) {
                size_t k = j + 1, cnt = 0;
                while (k < n && cnt < 31 && isAsciiAlnum(static_cast<unsigned char>(s[k]))) {
                    ++k;
                    ++cnt;
                }
                if (cnt >= 1 && k < n && s[k] == ';') {
                    std::string name = s.substr(j, k - j);
                    std::string match = s.substr(i, k + 1 - i);
                    out += replaceEntityPattern(match, name);
                    i = k + 1;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// rules_core/normalize — newline + NUL normalization
//   NEWLINES_RE = \r\n? | \n  -> "\n"
//   NULL_RE     = \0          -> U+FFFD
// ---------------------------------------------------------------------------
static std::string normalizeSource(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (c == '\r') {
            out += '\n';
            if (i + 1 < src.size() && src[i + 1] == '\n') ++i;  // \r\n -> \n
        } else if (c == '\0') {
            out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
        } else {
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// rules_block/state_block — line metadata (mirrors StateBlock.__init__)
// ---------------------------------------------------------------------------
struct StateBlock {
    const std::string& src;
    std::vector<int> bMarks;  // line start byte offset
    std::vector<int> eMarks;  // line end byte offset (index of '\n' or EOF)
    std::vector<int> tShift;  // count of leading whitespace chars
    std::vector<int> sCount;  // leading indent in virtual spaces (tabs -> next mult of 4)
    std::vector<int> bsCount; // blockquote-adjusted offset (0 at top level)
    int lineMax = 0;
    int blkIndent = 0;        // required block indent (0 at top level)
    int line = 0;             // current line cursor (set by rules, like state.line)
    int level = 0;            // token nesting level (for maxNesting + tight paragraphs)
    bool tight = false;       // loose/tight mode for lists
    int listIndent = -1;      // indent of current list block (-1 if none)
    std::string parentType = "root";  // "root"|"paragraph"|"blockquote"|"list"

    explicit StateBlock(const std::string& s) : src(s) {
        bool indentFound = false;
        int start = 0, pos = 0, indent = 0, offset = 0;
        const int length = static_cast<int>(src.size());
        while (pos < length) {
            char ch = src[pos];
            if (!indentFound) {
                if (isStrSpace(ch)) {
                    ++indent;
                    if (ch == '\t') offset += 4 - offset % 4;
                    else ++offset;
                    ++pos;
                    continue;
                }
                indentFound = true;
            }
            if (ch == '\n' || pos == length - 1) {
                if (ch != '\n') ++pos;  // last line without trailing newline
                bMarks.push_back(start);
                eMarks.push_back(pos);
                tShift.push_back(indent);
                sCount.push_back(offset);
                bsCount.push_back(0);
                indentFound = false;
                indent = 0;
                offset = 0;
                start = pos + 1;
            }
            ++pos;
        }
        // sentinel line (simplifies loops)
        bMarks.push_back(length);
        eMarks.push_back(length);
        tShift.push_back(0);
        sCount.push_back(0);
        bsCount.push_back(0);
        lineMax = static_cast<int>(bMarks.size()) - 1;
    }

    bool isEmpty(int line) const { return bMarks[line] + tShift[line] >= eMarks[line]; }

    int skipEmptyLines(int line) const {
        while (line < lineMax && isEmpty(line)) ++line;
        return line;
    }

    int skipSpaces(int pos) const {
        while (pos < static_cast<int>(src.size()) && isStrSpace(src[pos])) ++pos;
        return pos;
    }

    // is_code_block: code rule enabled (commonmark) and indent >= 4 beyond blkIndent.
    bool isCodeBlock(int line) const { return (sCount[line] - blkIndent) >= 4; }

    // Skip trailing whitespace, reverse, not below `minimum`.
    int skipSpacesBack(int pos, int minimum) const {
        if (pos <= minimum) return pos;
        while (pos > minimum) {
            --pos;
            if (!isStrSpace(src[pos])) return pos + 1;
        }
        return pos;
    }

    // Skip trailing run of `ch`, reverse, not below `minimum`.
    int skipCharsStrBack(int pos, char ch, int minimum) const {
        if (pos <= minimum) return pos;
        while (pos > minimum) {
            --pos;
            if (src[pos] != ch) return pos + 1;
        }
        return pos;
    }

    // getLines: cut lines [begin,end), dedenting up to `indent` virtual columns.
    // keepLastLF controls whether the final line keeps its trailing newline.
    // Faithful port of state_block.getLines.
    std::string getLines(int begin, int end, int indent, bool keepLastLF) const {
        if (begin >= end) return "";
        const int srcLen = static_cast<int>(src.size());
        std::string out;
        int line = begin;
        while (line < end) {
            int lineIndent = 0;
            int lineStart = bMarks[line];
            int first = lineStart;
            int last = (line + 1 < end || keepLastLF) ? eMarks[line] + 1 : eMarks[line];
            if (last > srcLen) last = srcLen;
            while (first < last && lineIndent < indent) {
                char ch = src[first];
                if (isStrSpace(ch)) {
                    if (ch == '\t') lineIndent += 4 - (lineIndent + bsCount[line]) % 4;
                    else lineIndent += 1;
                } else if (first - lineStart < tShift[line]) {
                    lineIndent += 1;
                } else {
                    break;
                }
                ++first;
            }
            if (lineIndent > indent) {
                out.append(lineIndent - indent, ' ');
                out.append(src, first, last - first);
            } else {
                out.append(src, first, last - first);
            }
            ++line;
        }
        return out;
    }
};

// StateBlock.push: append a block token with correct level bookkeeping.
static Token& pushBlockToken(StateBlock& st, std::vector<Token>& tokens,
                             const std::string& type, const std::string& tag, int nesting) {
    if (nesting < 0) --st.level;
    Token t;
    t.type = type;
    t.tag = tag;
    t.nesting = nesting;
    t.block = true;
    t.level = st.level;
    if (nesting > 0) ++st.level;
    tokens.push_back(std::move(t));
    return tokens.back();
}

// ---------------------------------------------------------------------------
// rules_block/heading — ATX headings (#..######)
//   Detection mirrors heading.py:16-43 (== the silent-mode result used as a
//   paragraph terminator). Returns level 1..6, or 0 if the line is not a heading;
//   on success sets markerEnd to the byte offset just past the '#' run.
// ---------------------------------------------------------------------------
static int detectHeading(const StateBlock& st, int line, int& markerEnd) {
    if (st.isCodeBlock(line)) return 0;
    int pos = st.bMarks[line] + st.tShift[line];
    const int maximum = st.eMarks[line];
    if (pos >= maximum || st.src[pos] != '#') return 0;

    int level = 1;
    ++pos;
    while (pos < maximum && st.src[pos] == '#' && level <= 6) {
        ++level;
        ++pos;
    }
    // reject 7+ markers, or a marker run not followed by space/tab/EOL
    if (level > 6 || (pos < maximum && !isStrSpace(st.src[pos]))) return 0;

    markerEnd = pos;
    return level;
}

// ---------------------------------------------------------------------------
// rules_block/fence — fenced code blocks (``` and ~~~)
//   Detection (== silent result, fence.py up to "if silent: return True"): a marker run
//   of >= 3 backticks/tildes, not a code block, and (for backtick fences) no backtick in
//   the info string. Sets marker, runLength, and infoStart (byte just past the run).
// ---------------------------------------------------------------------------
static bool detectFence(const StateBlock& st, int line, char& marker, int& runLength,
                        int& infoStart) {
    if (st.isCodeBlock(line)) return false;
    int pos = st.bMarks[line] + st.tShift[line];
    const int maximum = st.eMarks[line];
    if (pos + 3 > maximum) return false;  // min_markers = 3
    char m = st.src[pos];
    if (m != '`' && m != '~') return false;
    int mem = pos;
    while (pos < maximum && st.src[pos] == m) ++pos;
    int len = pos - mem;
    if (len < 3) return false;
    // CommonMark: backtick fences cannot contain a backtick in the info string.
    if (m == '`') {
        for (int i = pos; i < maximum; ++i)
            if (st.src[i] == '`') return false;
    }
    marker = m;
    runLength = len;
    infoStart = pos;
    return true;
}

// Full fence parse (fence.py after the silent check). Pushes the fence token and returns
// the next line to continue from.
static int applyFence(StateBlock& st, int line, char marker, int runLength, int infoStart,
                      std::vector<Token>& tokens) {
    const int srcLen = static_cast<int>(st.src.size());
    int startMax = st.eMarks[line];
    std::string params = st.src.substr(infoStart, startMax - infoStart);
    std::string markup = st.src.substr(st.bMarks[line] + st.tShift[line], runLength);

    int nextLine = line;
    bool haveEndMarker = false;
    while (true) {
        ++nextLine;
        if (nextLine >= st.lineMax) break;  // unclosed -> autoclose at EOF

        int pos = st.bMarks[nextLine] + st.tShift[nextLine];
        int mem = pos;
        int maxN = st.eMarks[nextLine];

        if (pos < maxN && st.sCount[nextLine] < st.blkIndent) break;
        if (pos >= srcLen) break;            // Python src[pos] IndexError -> break
        if (st.src[pos] != marker) continue;
        if (st.isCodeBlock(nextLine)) continue;

        while (pos < maxN && st.src[pos] == marker) ++pos;  // skipCharsStr
        if (pos - mem < runLength) continue;                 // closer >= opener length
        while (pos < maxN && isStrSpace(st.src[pos])) ++pos; // skipSpaces (tail must be blank)
        if (pos < maxN) continue;

        haveEndMarker = true;
        break;
    }

    int indent = st.sCount[line];  // strip the fence's own indentation from the body
    int resultLine = nextLine + (haveEndMarker ? 1 : 0);

    Token& tok = pushBlockToken(st, tokens, "fence", "code", 0);
    tok.info = params;
    tok.markup = markup;
    tok.content = st.getLines(line + 1, nextLine, indent, true);

    return resultLine;
}

// ---------------------------------------------------------------------------
// Inline parsing (Stage 3: text, newline, backticks, emphasis/strong)
//   Two passes mirror markdown-it: (1) tokenize building tokens + a delimiter stack;
//   (2) balance_pairs -> emphasis.postProcess -> fragments_join.
//   Characters whose rule is not yet implemented (escape, link, entity, html_inline,
//   ...) fall through to pending text exactly as markdown-it's main loop does.
// ---------------------------------------------------------------------------
struct Delim {
    int marker;  // '*' or '_'
    int length;
    int token;   // index into tokens
    int end;     // -1, or index of matching closer delimiter
    bool open;
    bool close;
};

struct Scanned {
    bool canOpen;
    bool canClose;
    int length;
};

struct InlineState {
    const std::string& src;
    int posMax;
    int pos = 0;
    std::string pending;
    std::vector<Token>& tokens;
    std::vector<std::vector<Delim>> scopes;  // scopes[0] = root; others = link-label scopes
    std::vector<int> scopeStack;             // active scope indices; back() = current
    std::unordered_map<int, int> backticks;
    bool backticksScanned = false;
    std::unordered_map<int, int> cache;      // skipToken memoization
    int level = 0;
    int linkLevel = 0;

    InlineState(const std::string& s, std::vector<Token>& out)
        : src(s), posMax(static_cast<int>(s.size())), tokens(out) {
        scopes.push_back({});
        scopeStack.push_back(0);
    }

    std::vector<Delim>& curDelims() { return scopes[scopeStack.back()]; }

    void pushPending() {
        Token t;
        t.type = "text";
        t.content = pending;
        t.level = level;
        tokens.push_back(std::move(t));
        pending.clear();
    }

    // push: flush pending, manage nesting level + delimiter scope, append a token.
    // (Inline token levels are recomputed by fragments_join afterward.)
    Token& push(const std::string& type, const std::string& tag, int nesting) {
        if (!pending.empty()) pushPending();
        if (nesting < 0) {
            --level;
            scopeStack.pop_back();  // restore outer delimiter scope
        }
        Token t;
        t.type = type;
        t.tag = tag;
        t.nesting = nesting;
        t.level = level;
        tokens.push_back(std::move(t));
        if (nesting > 0) {
            ++level;
            scopes.push_back({});                            // new delimiter scope
            scopeStack.push_back(static_cast<int>(scopes.size()) - 1);
        }
        return tokens.back();
    }

    Scanned scanDelims(int start, bool canSplitWord) const {
        int p = start;
        char marker = src[start];
        unsigned char lastChar = start > 0 ? static_cast<unsigned char>(src[start - 1]) : ' ';
        while (p < posMax && src[p] == marker) ++p;
        int count = p - start;
        unsigned char nextChar = p < posMax ? static_cast<unsigned char>(src[p]) : ' ';

        bool isLastPunct = isMdAsciiPunct(lastChar) || isPunctCharByte(lastChar);
        bool isNextPunct = isMdAsciiPunct(nextChar) || isPunctCharByte(nextChar);
        bool isLastWs = isWhiteSpaceCode(lastChar);
        bool isNextWs = isWhiteSpaceCode(nextChar);

        bool leftFlanking =
            !(isNextWs || (isNextPunct && !(isLastWs || isLastPunct)));
        bool rightFlanking =
            !(isLastWs || (isLastPunct && !(isNextWs || isNextPunct)));

        bool canOpen = leftFlanking && (canSplitWord || !rightFlanking || isLastPunct);
        bool canClose = rightFlanking && (canSplitWord || !leftFlanking || isNextPunct);
        return {canOpen, canClose, count};
    }
};

// rules_inline/text
static bool ruleText(InlineState& st) {
    const auto& term = terminatorTable();
    int start = st.pos;
    int p = start;
    while (p < st.posMax && !term[static_cast<unsigned char>(st.src[p])]) ++p;
    if (p == start) return false;
    st.pending.append(st.src, start, p - start);
    st.pos = p;
    return true;
}

// rules_inline/newline
static bool ruleNewline(InlineState& st) {
    if (st.src[st.pos] != '\n') return false;
    int pmax = static_cast<int>(st.pending.size()) - 1;
    if (pmax >= 0 && st.pending[pmax] == ' ') {
        if (pmax >= 1 && st.pending[pmax - 1] == ' ') {
            int ws = pmax - 1;
            while (ws >= 1 && st.pending[ws - 1] == ' ') --ws;
            st.pending.resize(ws);
            st.push("hardbreak", "br", 0);
        } else {
            st.pending.resize(pmax);
            st.push("softbreak", "br", 0);
        }
    } else {
        st.push("softbreak", "br", 0);
    }
    ++st.pos;
    while (st.pos < st.posMax && isStrSpace(st.src[st.pos])) ++st.pos;
    return true;
}

// rules_inline/escape — backslash escapes + escaped hardbreak.
// _ESCAPED (escapable set) == MD ASCII punctuation, so isMdAsciiPunct covers it.
static bool ruleEscape(InlineState& st) {
    if (st.src[st.pos] != '\\') return false;
    int pos = st.pos + 1;
    const int maximum = st.posMax;
    if (pos >= maximum) return false;  // '\' at end of inline -> literal (handled by main loop)
    char ch1 = st.src[pos];
    if (ch1 == '\n') {
        st.push("hardbreak", "br", 0);
        ++pos;
        while (pos < maximum && isStrSpace(st.src[pos])) ++pos;  // skip next line's indent
        st.pos = pos;
        return true;
    }
    // (Python's surrogate-pair branch never fires on UTF-8 bytes.)
    std::string escapedStr(1, ch1);
    std::string origStr = "\\" + escapedStr;
    Token& tok = st.push("text_special", "", 0);
    tok.content = isMdAsciiPunct(static_cast<unsigned char>(ch1)) ? escapedStr : origStr;
    tok.markup = origStr;
    tok.info = "escape";
    st.pos = pos + 1;
    return true;
}

// rules_inline/backticks
static bool ruleBacktick(InlineState& st) {
    int pos = st.pos;
    if (st.src[pos] != '`') return false;
    int start = pos;
    ++pos;
    const int maximum = st.posMax;
    while (pos < maximum && st.src[pos] == '`') ++pos;
    std::string marker = st.src.substr(start, pos - start);
    int openerLength = static_cast<int>(marker.size());

    // Faithful to Python's `state.backticks.get(openerLength, 0) <= start`: a MISSING
    // cache entry defaults to 0, so (when already scanned) it short-circuits to "no closer
    // after this point" rather than rescanning the remainder.
    auto it = st.backticks.find(openerLength);
    int cached = (it != st.backticks.end()) ? it->second : 0;
    if (st.backticksScanned && cached <= start) {
        st.pending += marker;
        st.pos += openerLength;
        return true;
    }

    int matchEnd = pos;
    while (true) {
        size_t found = st.src.find('`', matchEnd);
        if (found == std::string::npos) break;
        int matchStart = static_cast<int>(found);
        matchEnd = matchStart + 1;
        while (matchEnd < maximum && st.src[matchEnd] == '`') ++matchEnd;
        int closerLength = matchEnd - matchStart;
        if (closerLength == openerLength) {
            Token& tok = st.push("code_inline", "code", 0);
            tok.markup = marker;
            std::string content = st.src.substr(pos, matchStart - pos);
            for (char& c : content) if (c == '\n') c = ' ';
            if (content.size() > 1 && content.front() == ' ' && content.back() == ' ' &&
                !stripWs(content).empty()) {
                content = content.substr(1, content.size() - 2);
            }
            tok.content = content;
            st.pos = matchEnd;
            return true;
        }
        st.backticks[closerLength] = matchStart;
    }

    st.backticksScanned = true;
    st.pending += marker;
    st.pos += openerLength;
    return true;
}

// rules_inline/emphasis (tokenize pass): one text token per marker + a delimiter each.
static bool ruleEmphasis(InlineState& st) {
    char marker = st.src[st.pos];
    if (marker != '_' && marker != '*') return false;
    Scanned scanned = st.scanDelims(st.pos, marker == '*');
    for (int k = 0; k < scanned.length; ++k) {
        Token& tok = st.push("text", "", 0);
        tok.content = std::string(1, marker);
        Delim d;
        d.marker = static_cast<unsigned char>(marker);
        d.length = scanned.length;
        d.token = static_cast<int>(st.tokens.size()) - 1;
        d.end = -1;
        d.open = scanned.canOpen;
        d.close = scanned.canClose;
        st.curDelims().push_back(d);
    }
    st.pos += scanned.length;
    return true;
}

// rules_inline/balance_pairs: match openers/closers (CommonMark "rule of 3" + jumps).
static void processDelimiters(std::vector<Delim>& delimiters) {
    if (delimiters.empty()) return;
    std::unordered_map<int, std::array<int, 6>> openersBottom;
    int maximum = static_cast<int>(delimiters.size());
    int headerIdx = 0;
    int lastTokenIdx = -2;
    std::vector<int> jumps;
    jumps.reserve(maximum);
    int closerIdx = 0;
    while (closerIdx < maximum) {
        Delim& closer = delimiters[closerIdx];
        jumps.push_back(0);

        if (delimiters[headerIdx].marker != closer.marker ||
            lastTokenIdx != closer.token - 1) {
            headerIdx = closerIdx;
        }
        lastTokenIdx = closer.token;

        if (!closer.close) {
            ++closerIdx;
            continue;
        }

        if (openersBottom.find(closer.marker) == openersBottom.end()) {
            openersBottom[closer.marker] = {-1, -1, -1, -1, -1, -1};
        }
        int minOpenerIdx =
            openersBottom[closer.marker][(closer.open ? 3 : 0) + (closer.length % 3)];

        int openerIdx = headerIdx - jumps[headerIdx] - 1;
        int newMinOpenerIdx = openerIdx;

        while (openerIdx > minOpenerIdx) {
            Delim& opener = delimiters[openerIdx];
            if (opener.marker != closer.marker) {
                openerIdx -= jumps[openerIdx] + 1;
                continue;
            }
            if (opener.open && opener.end < 0) {
                bool isOddMatch = false;
                if ((opener.close || closer.open) &&
                    ((opener.length + closer.length) % 3 == 0) &&
                    (opener.length % 3 != 0 || closer.length % 3 != 0)) {
                    isOddMatch = true;
                }
                if (!isOddMatch) {
                    int lastJump = (openerIdx > 0 && !delimiters[openerIdx - 1].open)
                                       ? jumps[openerIdx - 1] + 1
                                       : 0;
                    jumps[closerIdx] = closerIdx - openerIdx + lastJump;
                    jumps[openerIdx] = lastJump;
                    closer.open = false;
                    opener.end = closerIdx;
                    opener.close = false;
                    newMinOpenerIdx = -1;
                    lastTokenIdx = -2;
                    break;
                }
            }
            openerIdx -= jumps[openerIdx] + 1;
        }

        if (newMinOpenerIdx != -1) {
            openersBottom[closer.marker][(closer.open ? 3 : 0) + (closer.length % 3)] =
                newMinOpenerIdx;
        }
        ++closerIdx;
    }
}

// rules_inline/emphasis postProcess: turn matched delimiter text tokens into em/strong.
static void emphasisPostProcess(std::vector<Token>& tokens, std::vector<Delim>& delimiters) {
    int i = static_cast<int>(delimiters.size()) - 1;
    while (i >= 0) {
        Delim& startDelim = delimiters[i];
        if (startDelim.marker != 0x5F && startDelim.marker != 0x2A) { --i; continue; }
        if (startDelim.end == -1) { --i; continue; }
        Delim& endDelim = delimiters[startDelim.end];

        bool isStrong =
            i > 0 &&
            delimiters[i - 1].end == startDelim.end + 1 &&
            delimiters[i - 1].marker == startDelim.marker &&
            delimiters[i - 1].token == startDelim.token - 1 &&
            delimiters[startDelim.end + 1].token == endDelim.token + 1;

        char ch = static_cast<char>(startDelim.marker);
        std::string markup = isStrong ? std::string(2, ch) : std::string(1, ch);

        Token& openTok = tokens[startDelim.token];
        openTok.type = isStrong ? "strong_open" : "em_open";
        openTok.tag = isStrong ? "strong" : "em";
        openTok.nesting = 1;
        openTok.markup = markup;
        openTok.content = "";

        Token& closeTok = tokens[endDelim.token];
        closeTok.type = isStrong ? "strong_close" : "em_close";
        closeTok.tag = isStrong ? "strong" : "em";
        closeTok.nesting = -1;
        closeTok.markup = markup;
        closeTok.content = "";

        if (isStrong) {
            tokens[delimiters[i - 1].token].content = "";
            tokens[delimiters[startDelim.end + 1].token].content = "";
            --i;
        }
        --i;
    }
}

// rules_inline/fragments_join: recompute levels + merge adjacent text tokens.
static void fragmentsJoin(std::vector<Token>& tokens) {
    int level = 0;
    int maximum = static_cast<int>(tokens.size());
    int curr = 0, last = 0;
    while (curr < maximum) {
        if (tokens[curr].nesting < 0) --level;
        tokens[curr].level = level;
        if (tokens[curr].nesting > 0) ++level;

        if (tokens[curr].type == "text" && curr + 1 < maximum &&
            tokens[curr + 1].type == "text") {
            std::string merged = tokens[curr].content;
            ++curr;
            while (curr < maximum && tokens[curr].type == "text") {
                merged += tokens[curr].content;
                ++curr;
            }
            Token m = tokens[curr - 1];
            m.content = merged;
            m.level = level;
            tokens[last] = std::move(m);
            ++last;
            continue;
        }
        if (curr != last) tokens[last] = std::move(tokens[curr]);
        ++last;
        ++curr;
    }
    if (curr != last) tokens.resize(last);
}

// ---------------------------------------------------------------------------
// Link helpers (helpers/* + common/normalize_url + mdurl.encode)
// ---------------------------------------------------------------------------
static inline int ccAt(const std::string& s, int pos) {
    return (pos >= 0 && pos < static_cast<int>(s.size()))
               ? static_cast<unsigned char>(s[pos]) : -1;
}

struct LinkDest { bool ok = false; int pos = 0; std::string str; };
static LinkDest parseLinkDestination(const std::string& s, int pos, int maximum) {
    int start = pos;
    LinkDest r;
    if (ccAt(s, pos) == 0x3C) {  // '<...>' form
        ++pos;
        while (pos < maximum) {
            int code = ccAt(s, pos);
            if (code == 0x0A) return r;
            if (code == 0x3C) return r;
            if (code == 0x3E) {
                r.pos = pos + 1;
                r.str = unescapeAll(s.substr(start + 1, pos - (start + 1)));
                r.ok = true;
                return r;
            }
            if (code == 0x5C && pos + 1 < maximum) { pos += 2; continue; }
            ++pos;
        }
        return r;  // no closing '>'
    }
    int level = 0;
    while (pos < maximum) {
        int code = ccAt(s, pos);
        if (code == -1 || code == 0x20) break;
        if (code < 0x20 || code == 0x7F) break;
        if (code == 0x5C && pos + 1 < maximum) {
            if (ccAt(s, pos + 1) == 0x20) break;
            pos += 2;
            continue;
        }
        if (code == 0x28) { ++level; if (level > 32) return r; }
        if (code == 0x29) { if (level == 0) break; --level; }
        ++pos;
    }
    if (start == pos) return r;
    if (level != 0) return r;
    r.str = unescapeAll(s.substr(start, pos - start));
    r.pos = pos;
    r.ok = true;
    return r;
}

struct LinkTitle { bool ok = false; int pos = 0; std::string str; };
static LinkTitle parseLinkTitle(const std::string& s, int start, int maximum) {
    int pos = start;
    LinkTitle st;
    if (pos >= maximum) return st;
    int marker = ccAt(s, pos);
    if (marker != 0x22 && marker != 0x27 && marker != 0x28) return st;  // " ' (
    ++start;
    ++pos;
    if (marker == 0x28) marker = 0x29;  // '(' closes with ')'
    while (pos < maximum) {
        int code = ccAt(s, pos);
        if (code == marker) {
            st.pos = pos + 1;
            st.str += unescapeAll(s.substr(start, pos - start));
            st.ok = true;
            return st;
        }
        if (code == 0x28 && marker == 0x29) return st;
        if (code == 0x5C && pos + 1 < maximum) ++pos;
        ++pos;
    }
    return st;  // no closing marker (reference-continuation deferred) -> not ok
}

// mdurl.encode default-chars cache.
static const std::array<std::string, 128>& encodeCacheDefault() {
    static const std::array<std::string, 128> cache = [] {
        std::array<std::string, 128> c;
        const std::string exclude = ";/?:@&=+$,-_.!~*'()#";
        for (int i = 0; i < 128; ++i) {
            if (isAsciiAlnum(static_cast<unsigned char>(i))) {
                c[i] = std::string(1, static_cast<char>(i));
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", i);
                c[i] = buf;
            }
        }
        for (char e : exclude) c[static_cast<unsigned char>(e)] = std::string(1, e);
        return c;
    }();
    return cache;
}

// mdurl.encode (keep_escaped=true, default exclude). Byte-wise; for code>=128 each UTF-8
// byte is percent-encoded, which equals Python's per-codepoint encode_uri_component.
static std::string mdurlEncode(const std::string& s) {
    const auto& cache = encodeCacheDefault();
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::string out;
    out.reserve(s.size());
    int l = static_cast<int>(s.size()), i = 0;
    while (i < l) {
        unsigned char code = static_cast<unsigned char>(s[i]);
        if (code == 0x25 && i + 2 < l && isHex(s[i + 1]) && isHex(s[i + 2])) {
            out += s.substr(i, 3);
            i += 3;
            continue;
        }
        if (code < 128) { out += cache[code]; ++i; continue; }
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%%%02X", code);
        out += buf;
        ++i;
    }
    return out;
}

// normalizeLink: for the supported domain (ASCII http/https/relative), mdurl.parse+format
// round-trips and hostname punycode is identity, so this reduces to mdurl.encode.
static std::string normalizeLink(const std::string& url) { return mdurlEncode(url); }

// validateLink: block javascript/vbscript/file/data: (except data:image/...).
static bool validateLink(const std::string& url) {
    std::string u = stripWs(url);
    for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto starts = [&](const char* p) { return u.rfind(p, 0) == 0; };
    if (starts("vbscript:") || starts("javascript:") || starts("file:") || starts("data:")) {
        return starts("data:image/gif;") || starts("data:image/png;") ||
               starts("data:image/jpeg;") || starts("data:image/webp;");
    }
    return true;
}

// ---------------------------------------------------------------------------
// skipToken (silent single-token advance) + parseLinkLabel
//   Silent variants advance st.pos without pushing tokens. Emphasis is silent-noop
//   (returns false). Link-in-label skipping is intentionally omitted (no nested links
//   in the supported domain); bracket balancing is handled by parseLinkLabel's level.
// ---------------------------------------------------------------------------
static bool skipText(InlineState& st) {
    const auto& term = terminatorTable();
    int start = st.pos, p = start;
    while (p < st.posMax && !term[static_cast<unsigned char>(st.src[p])]) ++p;
    if (p == start) return false;
    st.pos = p;
    return true;
}
static bool skipNewline(InlineState& st) {
    if (st.src[st.pos] != '\n') return false;
    int pos = st.pos + 1;
    while (pos < st.posMax && isStrSpace(st.src[pos])) ++pos;
    st.pos = pos;
    return true;
}
static bool skipEscape(InlineState& st) {
    if (st.src[st.pos] != '\\') return false;
    int pos = st.pos + 1;
    if (pos >= st.posMax) return false;
    if (st.src[pos] == '\n') {
        ++pos;
        while (pos < st.posMax && isStrSpace(st.src[pos])) ++pos;
        st.pos = pos;
        return true;
    }
    st.pos = pos + 1;
    return true;
}
static bool skipBacktick(InlineState& st) {
    int start = st.pos;
    if (st.src[start] != '`') return false;
    int pos = start + 1;
    while (pos < st.posMax && st.src[pos] == '`') ++pos;
    int openerLength = pos - start;
    auto it = st.backticks.find(openerLength);
    int cached = (it != st.backticks.end()) ? it->second : 0;
    if (st.backticksScanned && cached <= start) { st.pos = start + openerLength; return true; }
    int matchEnd = pos;
    while (true) {
        size_t found = st.src.find('`', matchEnd);
        if (found == std::string::npos) break;
        int matchStart = static_cast<int>(found);
        matchEnd = matchStart + 1;
        while (matchEnd < st.posMax && st.src[matchEnd] == '`') ++matchEnd;
        if (matchEnd - matchStart == openerLength) { st.pos = matchEnd; return true; }
        st.backticks[matchEnd - matchStart] = matchStart;
    }
    st.backticksScanned = true;
    st.pos = start + openerLength;
    return true;
}

// Forward decl: skipToken runs link in silent mode (so a valid inner link in a label is
// skipped as one token), mirroring parser_inline.skipToken's full rule set.
static bool ruleLink(InlineState& st, bool silent);

static void skipToken(InlineState& st) {
    int pos = st.pos;
    auto it = st.cache.find(pos);
    if (it != st.cache.end()) { st.pos = it->second; return; }
    bool ok = false;
    if (st.level < 20) {
        // emphasis is a silent no-op; link IS attempted silently (nested-link handling).
        ok = skipText(st) || skipNewline(st) || skipEscape(st) || skipBacktick(st) ||
             ruleLink(st, true);
    } else {
        st.pos = st.posMax;
    }
    if (!ok) st.pos += 1;
    st.cache[pos] = st.pos;
}

static int parseLinkLabel(InlineState& st, int start, bool disableNested) {
    int labelEnd = -1;
    int oldPos = st.pos;
    bool found = false;
    st.pos = start + 1;
    int level = 1;
    while (st.pos < st.posMax) {
        char marker = st.src[st.pos];
        if (marker == ']') {
            --level;
            if (level == 0) { found = true; break; }
        }
        int prevPos = st.pos;
        skipToken(st);
        if (marker == '[') {
            if (prevPos == st.pos - 1) ++level;
            else if (disableNested) { st.pos = oldPos; return -1; }
        }
    }
    if (found) labelEnd = st.pos;
    st.pos = oldPos;
    return labelEnd;
}

// Re-entrant inline tokenizer (called recursively by ruleLink for link labels).
static void tokenizeInline(InlineState& st);

// rules_inline/link — inline links [text](url) and [text](url "title").
// Reference/shortcut links fail (no env references — deferred, as instructed).
// In silent mode (used by skipToken/parseLinkLabel) the link is parsed and st.pos advanced
// past it, but no tokens are pushed — matching link.py's `if not silent` guard.
static bool ruleLink(InlineState& st, bool silent) {
    if (st.src[st.pos] != '[') return false;
    std::string href, title;
    int maximum = st.posMax;
    int labelStart = st.pos + 1;
    int labelEnd = parseLinkLabel(st, st.pos, true);
    if (labelEnd < 0) return false;

    int pos = labelEnd + 1;
    bool parseReference = true;
    if (pos < maximum && st.src[pos] == '(') {
        parseReference = false;
        ++pos;
        while (pos < maximum) {
            char ch = st.src[pos];
            if (!isStrSpace(ch) && ch != '\n') break;
            ++pos;
        }
        if (pos >= maximum) return false;
        int start = pos;
        LinkDest res = parseLinkDestination(st.src, pos, st.posMax);
        if (res.ok) {
            href = normalizeLink(res.str);
            if (validateLink(href)) pos = res.pos;
            else href = "";
            start = pos;
            while (pos < maximum) {
                char ch = st.src[pos];
                if (!isStrSpace(ch) && ch != '\n') break;
                ++pos;
            }
            LinkTitle tres = parseLinkTitle(st.src, pos, st.posMax);
            if (pos < maximum && start != pos && tres.ok) {
                title = tres.str;
                pos = tres.pos;
                while (pos < maximum) {
                    char ch = st.src[pos];
                    if (!isStrSpace(ch) && ch != '\n') break;
                    ++pos;
                }
            }
        }
        if (pos >= maximum || st.src[pos] != ')') parseReference = true;
        ++pos;
    }

    if (parseReference) return false;  // no references configured -> not a link

    // Valid inline link. In silent mode just advance past it (no tokens) — link.py's
    // `if not silent` guard around the push/recursion block.
    if (!silent) {
        st.pos = labelStart;
        st.posMax = labelEnd;
        Token& open = st.push("link_open", "a", 1);
        open.attrs.push_back({"href", href});
        if (!title.empty()) open.attrs.push_back({"title", title});
        ++st.linkLevel;
        tokenizeInline(st);
        --st.linkLevel;
        st.push("link_close", "a", -1);
    }
    st.pos = pos;
    st.posMax = maximum;
    return true;
}

// rules_inline/entity — &name; / &#dd; / &#xhh; (emits text_special; raw HTML stays deferred).
//   DIGITAL_RE ^&#((?:x[a-f0-9]{1,6}|[0-9]{1,7}));   NAMED_RE ^&([a-z][a-z0-9]{1,31});
static bool ruleEntity(InlineState& st) {
    int pos = st.pos, maximum = st.posMax;
    if (st.src[pos] != '&') return false;
    if (pos + 1 >= maximum) return false;

    if (st.src[pos + 1] == '#') {  // numeric
        int p = pos + 2;
        bool hex = (p < maximum && (st.src[p] == 'x' || st.src[p] == 'X'));
        if (hex) ++p;
        int digitStart = p, maxDigits = hex ? 6 : 7;
        auto isDigit = [&](char c) {
            return hex ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))
                       : (c >= '0' && c <= '9');
        };
        while (p < maximum && p - digitStart < maxDigits && isDigit(st.src[p])) ++p;
        if (p - digitStart < 1 || p >= maximum || st.src[p] != ';') return false;
        long long code = std::stoll(st.src.substr(digitStart, p - digitStart), nullptr,
                                    hex ? 16 : 10);
        Token& tok = st.push("text_special", "", 0);
        tok.content = isValidEntityCode(code) ? fromCodePoint(code) : fromCodePoint(0xFFFD);
        tok.markup = st.src.substr(pos, (p + 1) - pos);
        tok.info = "entity";
        st.pos = p + 1;
        return true;
    }

    // named: first char letter, then 1..31 alnum, then ';', and name must be in the table
    int p = pos + 1;
    if (!isAsciiAlpha(static_cast<unsigned char>(st.src[p]))) return false;
    int nameStart = p;
    ++p;
    int rest = 0;
    while (p < maximum && rest < 31 && isAsciiAlnum(static_cast<unsigned char>(st.src[p]))) {
        ++p;
        ++rest;
    }
    if (rest < 1 || p >= maximum || st.src[p] != ';') return false;
    std::string name = st.src.substr(nameStart, p - nameStart);
    const auto& ents = html5_entities();
    auto it = ents.find(name);
    if (it == ents.end()) return false;
    Token& tok = st.push("text_special", "", 0);
    tok.content = it->second;
    tok.markup = st.src.substr(pos, (p + 1) - pos);
    tok.info = "entity";
    st.pos = p + 1;
    return true;
}

static void tokenizeInline(InlineState& st) {
    while (st.pos < st.posMax) {
        if (st.level < 20) {
            if (ruleText(st)) continue;
            if (ruleNewline(st)) continue;
            if (ruleEscape(st)) continue;
            if (ruleBacktick(st)) continue;
            if (ruleEmphasis(st)) continue;
            if (ruleLink(st, false)) continue;
            if (ruleEntity(st)) continue;
        }
        st.pending += st.src[st.pos];
        ++st.pos;
    }
    if (!st.pending.empty()) st.pushPending();
}

static void inlineParse(const std::string& src, std::vector<Token>& out) {
    InlineState st(src, out);
    tokenizeInline(st);
    // pass 2: balance pairs -> emphasis -> merge text fragments (per delimiter scope)
    for (auto& sc : st.scopes) processDelimiters(sc);
    for (auto& sc : st.scopes) emphasisPostProcess(out, sc);
    fragmentsJoin(out);
}

// ---------------------------------------------------------------------------
// Block parsing (Stage 1: paragraph only)
// ---------------------------------------------------------------------------
// Push the open/inline/close trio for a block whose content is parsed inline.
static void pushInlineBlock(StateBlock& st, std::vector<Token>& tokens,
                            const std::string& openType, const std::string& closeType,
                            const std::string& tag, const std::string& markup,
                            const std::string& content) {
    pushBlockToken(st, tokens, openType, tag, 1).markup = markup;
    Token& inl = pushBlockToken(st, tokens, "inline", "", 0);
    inl.content = content;
    inlineParse(content, inl.children);
    pushBlockToken(st, tokens, closeType, tag, -1).markup = markup;
}

static constexpr int kMaxNesting = 20;

// rules_block/blockquote detection (== silent result): line starts with '>' and isn't code.
static bool detectBlockquote(const StateBlock& st, int line) {
    if (st.isCodeBlock(line)) return false;
    int pos = st.bMarks[line] + st.tShift[line];
    if (pos >= static_cast<int>(st.src.size())) return false;  // Python src[pos] IndexError
    return st.src[pos] == '>';
}

// Forward declarations for recursion / terminator checks.
static void tokenizeBlock(StateBlock& st, int startLine, int endLine,
                          std::vector<Token>& tokens);
static bool listDetect(const StateBlock& st, int line, bool silent, bool& isOrdered,
                       int& posAfterMarker, char& markerChar, int& markerValue,
                       int& markerStart);
static int applyList(StateBlock& st, int startLine, int endLine, std::vector<Token>& tokens);

// rules_block/heading apply: pushes the heading trio, returns next line.
static int applyHeading(StateBlock& st, int line, int level, int markerEnd,
                        std::vector<Token>& tokens) {
    int maximum = st.eMarks[line];
    maximum = st.skipSpacesBack(maximum, markerEnd);
    int tmp = st.skipCharsStrBack(maximum, '#', markerEnd);
    if (tmp > markerEnd && isStrSpace(st.src[tmp - 1])) maximum = tmp;
    std::string content = stripWs(st.src.substr(markerEnd, maximum - markerEnd));
    std::string tag = "h" + std::to_string(level);
    std::string markup(level, '#');
    pushInlineBlock(st, tokens, "heading_open", "heading_close", tag, markup, content);
    return line + 1;
}

// rules_block/paragraph: accumulate to an empty line / terminator, push trio, set st.line.
static void applyParagraph(StateBlock& st, int startLine, std::vector<Token>& tokens) {
    int next = startLine + 1;
    int endLine = st.lineMax;  // paragraph scans to lineMax (matches paragraph.py)
    std::string oldParent = st.parentType;
    st.parentType = "paragraph";  // so list terminator's isTerminatingParagraph fires
    while (next < endLine) {
        if (st.isEmpty(next)) break;
        if (st.sCount[next] - st.blkIndent > 3) { ++next; continue; }  // lazy continuation
        if (st.sCount[next] < 0) { ++next; continue; }
        // terminator rules (silent), markdown-it order: fence, blockquote, list, heading
        char fm = 0; int fr = 0, fi = 0, me = 0;
        bool io = false; int pam = 0, mv = 0, ms = 0; char mc = 0;
        if (detectFence(st, next, fm, fr, fi)) break;
        if (detectBlockquote(st, next)) break;
        if (listDetect(st, next, true, io, pam, mc, mv, ms)) break;
        if (detectHeading(st, next, me) > 0) break;
        ++next;
    }
    st.parentType = oldParent;
    std::string content = stripWs(st.getLines(startLine, next, st.blkIndent, false));
    st.line = next;
    pushInlineBlock(st, tokens, "paragraph_open", "paragraph_close", "p", "", content);
}

// rules_block/blockquote (commonmark; alerts disabled). Faithful port: mutates the line
// arrays for the stripped '>' prefix, recurses block tokenization, then restores.
static void applyBlockquote(StateBlock& st, int startLine, int endLine,
                            std::vector<Token>& tokens) {
    const int srcLen = static_cast<int>(st.src.size());
    int oldLineMax = st.lineMax;
    std::string oldParentType = st.parentType;
    st.parentType = "blockquote";
    int pos = st.bMarks[startLine] + st.tShift[startLine];
    int maxp = st.eMarks[startLine];
    ++pos;  // past '>'

    int initial = st.sCount[startLine] + 1;
    int offset = initial;
    bool adjustTab = false;
    bool spaceAfterMarker;
    char second = (pos < srcLen) ? st.src[pos] : '\0';
    if (second == ' ') {
        ++pos; ++initial; ++offset; adjustTab = false; spaceAfterMarker = true;
    } else if (second == '\t') {
        spaceAfterMarker = true;
        if ((st.bsCount[startLine] + offset) % 4 == 3) {
            ++pos; ++initial; ++offset; adjustTab = false;
        } else {
            adjustTab = true;
        }
    } else {
        spaceAfterMarker = false;
    }

    std::vector<int> oldBMarks{st.bMarks[startLine]};
    st.bMarks[startLine] = pos;
    while (pos < maxp) {
        char ch = st.src[pos];
        if (isStrSpace(ch)) {
            if (ch == '\t')
                offset += 4 - (offset + st.bsCount[startLine] + (adjustTab ? 1 : 0)) % 4;
            else
                ++offset;
        } else {
            break;
        }
        ++pos;
    }
    std::vector<int> oldBSCount{st.bsCount[startLine]};
    st.bsCount[startLine] = st.sCount[startLine] + 1 + (spaceAfterMarker ? 1 : 0);
    bool lastLineEmpty = pos >= maxp;
    std::vector<int> oldSCount{st.sCount[startLine]};
    st.sCount[startLine] = offset - initial;
    std::vector<int> oldTShift{st.tShift[startLine]};
    st.tShift[startLine] = pos - st.bMarks[startLine];

    int nextLine = startLine + 1;
    while (nextLine < endLine) {
        bool isOutdented = st.sCount[nextLine] < st.blkIndent;
        pos = st.bMarks[nextLine] + st.tShift[nextLine];
        maxp = st.eMarks[nextLine];
        if (pos >= maxp) break;  // Case 1: empty line outside

        bool evaluatesTrue = (st.src[pos] == '>') && !isOutdented;
        ++pos;
        if (evaluatesTrue) {
            initial = offset = st.sCount[nextLine] + 1;
            char nc = (pos < srcLen) ? st.src[pos] : '\0';
            if (nc == ' ') {
                ++pos; ++initial; ++offset; adjustTab = false; spaceAfterMarker = true;
            } else if (nc == '\t') {
                spaceAfterMarker = true;
                if ((st.bsCount[nextLine] + offset) % 4 == 3) {
                    ++pos; ++initial; ++offset; adjustTab = false;
                } else {
                    adjustTab = true;
                }
            } else {
                spaceAfterMarker = false;
            }
            oldBMarks.push_back(st.bMarks[nextLine]);
            st.bMarks[nextLine] = pos;
            while (pos < maxp) {
                char ch = st.src[pos];
                if (isStrSpace(ch)) {
                    if (ch == '\t')
                        offset += 4 - (offset + st.bsCount[nextLine] + (adjustTab ? 1 : 0)) % 4;
                    else
                        ++offset;
                } else {
                    break;
                }
                ++pos;
            }
            lastLineEmpty = pos >= maxp;
            oldBSCount.push_back(st.bsCount[nextLine]);
            st.bsCount[nextLine] = st.sCount[nextLine] + 1 + (spaceAfterMarker ? 1 : 0);
            oldSCount.push_back(st.sCount[nextLine]);
            st.sCount[nextLine] = offset - initial;
            oldTShift.push_back(st.tShift[nextLine]);
            st.tShift[nextLine] = pos - st.bMarks[nextLine];
            ++nextLine;
            continue;
        }

        if (lastLineEmpty) break;  // Case 2: empty line inside

        // Case 3: another block tag terminates the quote.
        // blockquote-group terminators (implemented): fence, list, heading.
        bool terminate = false;
        { char fm = 0; int fr = 0, fi = 0, me = 0;
          bool io = false; int pam = 0, mv = 0, ms = 0; char mc = 0;
          if (detectFence(st, nextLine, fm, fr, fi)) terminate = true;
          else if (listDetect(st, nextLine, true, io, pam, mc, mv, ms)) terminate = true;
          else if (detectHeading(st, nextLine, me) > 0) terminate = true; }
        if (terminate) {
            st.lineMax = nextLine;
            if (st.blkIndent != 0) {
                oldBMarks.push_back(st.bMarks[nextLine]);
                oldBSCount.push_back(st.bsCount[nextLine]);
                oldTShift.push_back(st.tShift[nextLine]);
                oldSCount.push_back(st.sCount[nextLine]);
                st.sCount[nextLine] -= st.blkIndent;
            }
            break;
        }

        oldBMarks.push_back(st.bMarks[nextLine]);
        oldBSCount.push_back(st.bsCount[nextLine]);
        oldTShift.push_back(st.tShift[nextLine]);
        oldSCount.push_back(st.sCount[nextLine]);
        st.sCount[nextLine] = -1;  // lazy paragraph continuation
        ++nextLine;
    }

    int oldIndent = st.blkIndent;
    st.blkIndent = 0;

    pushBlockToken(st, tokens, "blockquote_open", "blockquote", 1).markup = ">";
    tokenizeBlock(st, startLine, nextLine, tokens);
    pushBlockToken(st, tokens, "blockquote_close", "blockquote", -1).markup = ">";

    st.lineMax = oldLineMax;
    st.parentType = oldParentType;

    for (size_t i = 0; i < oldTShift.size(); ++i) {
        st.bMarks[i + startLine] = oldBMarks[i];
        st.tShift[i + startLine] = oldTShift[i];
        st.sCount[i + startLine] = oldSCount[i];
        st.bsCount[i + startLine] = oldBSCount[i];
    }
    st.blkIndent = oldIndent;
    // st.line was set by the recursive tokenizeBlock.
}

// rules_block/list — marker scanners
static int skipBulletListMarker(const StateBlock& st, int line) {
    int pos = st.bMarks[line] + st.tShift[line];
    int maximum = st.eMarks[line];
    if (pos >= static_cast<int>(st.src.size())) return -1;  // IndexError
    char marker = st.src[pos];
    ++pos;
    if (marker != '*' && marker != '-' && marker != '+') return -1;
    if (pos < maximum && !isStrSpace(st.src[pos])) return -1;  // "-test" is not a list
    return pos;
}

static int skipOrderedListMarker(const StateBlock& st, int line) {
    int start = st.bMarks[line] + st.tShift[line];
    int pos = start;
    int maximum = st.eMarks[line];
    if (pos + 1 >= maximum) return -1;  // need at least digit + delimiter
    char ch = st.src[pos];
    ++pos;
    if (ch < '0' || ch > '9') return -1;
    while (true) {
        if (pos >= maximum) return -1;
        ch = st.src[pos];
        ++pos;
        if (ch >= '0' && ch <= '9') {
            if (pos - start >= 10) return -1;  // <=9 digits
            continue;
        }
        if (ch == ')' || ch == '.') break;
        return -1;
    }
    if (pos < maximum && !isStrSpace(st.src[pos])) return -1;  // "1.test" is not a list
    return pos;
}

// Hide paragraph_open/close tokens directly inside a tight list's items.
static void markTightParagraphs(StateBlock& st, std::vector<Token>& tokens, int idx) {
    int level = st.level + 2;
    int i = idx + 2;
    int length = static_cast<int>(tokens.size()) - 2;
    while (i < length) {
        if (tokens[i].level == level && tokens[i].type == "paragraph_open") {
            tokens[i + 2].hidden = true;
            tokens[i].hidden = true;
            i += 2;
        }
        ++i;
    }
}

// list detection (== silent result, list.py up to "if silent: return True").
static bool listDetect(const StateBlock& st, int line, bool silent, bool& isOrdered,
                       int& posAfterMarker, char& markerChar, int& markerValue,
                       int& markerStart) {
    if (st.isCodeBlock(line)) return false;
    // nested-list paragraph-continuation special case
    if (st.listIndent >= 0 && st.sCount[line] - st.listIndent >= 4 &&
        st.sCount[line] < st.blkIndent)
        return false;

    bool isTerminatingParagraph =
        silent && st.parentType == "paragraph" && st.sCount[line] >= st.blkIndent;

    int pam = skipOrderedListMarker(st, line);
    if (pam >= 0) {
        isOrdered = true;
        markerStart = st.bMarks[line] + st.tShift[line];
        markerValue = std::stoi(st.src.substr(markerStart, (pam - 1) - markerStart));
        if (isTerminatingParagraph && markerValue != 1) return false;
    } else {
        pam = skipBulletListMarker(st, line);
        if (pam < 0) return false;
        isOrdered = false;
        markerValue = 0;
        markerStart = st.bMarks[line] + st.tShift[line];
    }
    // interrupting a paragraph: first line must not be empty
    if (isTerminatingParagraph && st.skipSpaces(pam) >= st.eMarks[line]) return false;

    markerChar = st.src[pam - 1];
    posAfterMarker = pam;
    return true;
}

// rules_block/list_block (commonmark; tasklists/alerts disabled). Faithful port.
static int applyList(StateBlock& st, int startLine, int endLine, std::vector<Token>& tokens) {
    bool isOrdered = false;
    int posAfterMarker = 0, markerValue = 0, markerStart = 0;
    char markerChar = 0;
    listDetect(st, startLine, false, isOrdered, posAfterMarker, markerChar, markerValue, markerStart);

    bool tight = true;
    int listTokIdx = static_cast<int>(tokens.size());

    if (isOrdered) {
        Token& t = pushBlockToken(st, tokens, "ordered_list_open", "ol", 1);
        if (markerValue != 1) t.attrs.push_back({"start", std::to_string(markerValue)});
        t.markup = std::string(1, markerChar);
    } else {
        pushBlockToken(st, tokens, "bullet_list_open", "ul", 1).markup = std::string(1, markerChar);
    }

    int nextLine = startLine;
    bool prevEmptyEnd = false;
    std::string oldParentType = st.parentType;
    st.parentType = "list";

    while (nextLine < endLine) {
        int pos = posAfterMarker;
        int maximum = st.eMarks[nextLine];
        int initial = st.sCount[nextLine] + posAfterMarker -
                      (st.bMarks[startLine] + st.tShift[startLine]);
        int offset = initial;
        while (pos < maximum) {
            char ch = st.src[pos];
            if (ch == '\t') offset += 4 - (offset + st.bsCount[nextLine]) % 4;
            else if (ch == ' ') offset += 1;
            else break;
            ++pos;
        }
        int contentStart = pos;
        int indentAfterMarker = (contentStart >= maximum) ? 1 : (offset - initial);
        if (indentAfterMarker > 4) indentAfterMarker = 1;  // extra is indented code
        int indent = initial + indentAfterMarker;

        Token& liOpen = pushBlockToken(st, tokens, "list_item_open", "li", 1);
        liOpen.markup = std::string(1, markerChar);
        if (isOrdered)
            liOpen.info = st.src.substr(markerStart, (posAfterMarker - 1) - markerStart);

        bool oldTight = st.tight;
        int oldBMark = st.bMarks[startLine];
        int oldTShift = st.tShift[startLine];
        int oldSCount = st.sCount[startLine];
        int oldListIndent = st.listIndent;
        st.listIndent = st.blkIndent;
        st.blkIndent = indent;
        st.tight = true;
        st.tShift[startLine] = contentStart - st.bMarks[startLine];
        st.sCount[startLine] = offset;

        if (contentStart >= maximum && st.isEmpty(startLine + 1)) {
            st.line = std::min(st.line + 2, endLine);  // empty item workaround
        } else {
            tokenizeBlock(st, startLine, endLine, tokens);
        }

        if (!st.tight || prevEmptyEnd) tight = false;
        prevEmptyEnd = (st.line - startLine) > 1 && st.isEmpty(st.line - 1);

        st.blkIndent = st.listIndent;
        st.listIndent = oldListIndent;
        st.bMarks[startLine] = oldBMark;
        st.tShift[startLine] = oldTShift;
        st.sCount[startLine] = oldSCount;
        st.tight = oldTight;

        pushBlockToken(st, tokens, "list_item_close", "li", -1).markup = std::string(1, markerChar);

        nextLine = startLine = st.line;
        if (nextLine >= endLine) break;

        if (st.sCount[nextLine] < st.blkIndent) break;
        if (st.isCodeBlock(startLine)) break;

        // list-group terminators (implemented): fence, blockquote
        { char fm = 0; int fr = 0, fi = 0;
          if (detectFence(st, nextLine, fm, fr, fi)) break;
          if (detectBlockquote(st, nextLine)) break; }

        // fail on list-type / marker-style change
        if (isOrdered) {
            posAfterMarker = skipOrderedListMarker(st, nextLine);
            if (posAfterMarker < 0) break;
            markerStart = st.bMarks[nextLine] + st.tShift[nextLine];
        } else {
            posAfterMarker = skipBulletListMarker(st, nextLine);
            if (posAfterMarker < 0) break;
        }
        if (markerChar != st.src[posAfterMarker - 1]) break;
    }

    if (isOrdered)
        pushBlockToken(st, tokens, "ordered_list_close", "ol", -1).markup = std::string(1, markerChar);
    else
        pushBlockToken(st, tokens, "bullet_list_close", "ul", -1).markup = std::string(1, markerChar);

    st.line = nextLine;
    st.parentType = oldParentType;

    if (tight) markTightParagraphs(st, tokens, listTokIdx);
    return st.line;
}

// parser_block.tokenize: walk lines in [startLine,endLine), dispatch block rules.
static void tokenizeBlock(StateBlock& st, int startLine, int endLine,
                          std::vector<Token>& tokens) {
    int line = startLine;
    bool hasEmptyLines = false;
    while (line < endLine) {
        st.line = line = st.skipEmptyLines(line);
        if (line >= endLine) break;
        if (st.sCount[line] < st.blkIndent) break;          // nested outdent termination
        if (st.level >= kMaxNesting) { st.line = endLine; break; }

        // rule order: fence, blockquote, list, heading, paragraph (markdown-it order, minus deferred)
        char fm = 0; int fr = 0, fi = 0, me = 0;
        bool io = false; int pam = 0, mv = 0, ms = 0; char mc = 0;
        int hlevel = 0;
        if (detectFence(st, line, fm, fr, fi)) {
            st.line = applyFence(st, line, fm, fr, fi, tokens);
        } else if (detectBlockquote(st, line)) {
            applyBlockquote(st, line, endLine, tokens);
        } else if (listDetect(st, line, false, io, pam, mc, mv, ms)) {
            applyList(st, line, endLine, tokens);
        } else if ((hlevel = detectHeading(st, line, me)) > 0) {
            st.line = applyHeading(st, line, hlevel, me, tokens);
        } else {
            applyParagraph(st, line, tokens);
        }

        st.tight = !hasEmptyLines;
        line = st.line;
        if ((line - 1) >= 0 && (line - 1) < endLine && st.isEmpty(line - 1)) hasEmptyLines = true;
        if (line < endLine && st.isEmpty(line)) {  // step past a single trailing blank
            hasEmptyLines = true;
            ++line;
            st.line = line;
        }
    }
}

static std::vector<Token> blockParse(const std::string& src) {
    StateBlock st(src);
    std::vector<Token> tokens;
    tokenizeBlock(st, 0, st.lineMax, tokens);
    return tokens;
}

// ---------------------------------------------------------------------------
// renderer.py
// ---------------------------------------------------------------------------
static std::string renderAttrs(const Token& t) {
    std::string r;
    for (const auto& kv : t.attrs) {
        r += " " + escapeHtml(kv.first) + "=\"" + escapeHtml(kv.second) + "\"";
    }
    return r;
}

static std::string renderToken(const std::vector<Token>& tokens, size_t idx) {
    const Token& token = tokens[idx];
    std::string result;
    if (token.hidden) return result;
    if (token.block && token.nesting != -1 && idx > 0 && tokens[idx - 1].hidden) {
        result += "\n";
    }
    result += (token.nesting == -1 ? "</" : "<") + token.tag;
    result += renderAttrs(token);
    if (token.nesting == 0 && kXhtmlOut) result += " /";

    bool needLf = false;
    if (token.block) {
        needLf = true;
        if (token.nesting == 1 && idx + 1 < tokens.size()) {
            const Token& next = tokens[idx + 1];
            if (next.type == "inline" || next.hidden) {
                needLf = false;
            } else if (next.nesting == -1 && next.tag == token.tag) {
                needLf = false;
            }
        }
    }
    result += needLf ? ">\n" : ">";
    return result;
}

static std::string renderInline(const std::vector<Token>& children) {
    std::string result;
    for (size_t i = 0; i < children.size(); ++i) {
        const Token& t = children[i];
        if (t.type == "text" || t.type == "text_special") {
            // text_special (escapes/entities) renders like text; escapeHtml is per-char so
            // not pre-merging via text_join yields byte-identical output.
            result += escapeHtml(t.content);
        } else if (t.type == "code_inline") {
            result += "<code" + renderAttrs(t) + ">" + escapeHtml(t.content) + "</code>";
        } else if (t.type == "softbreak") {
            result += kBreaks ? (kXhtmlOut ? "<br />\n" : "<br>\n") : "\n";
        } else if (t.type == "hardbreak") {
            result += kXhtmlOut ? "<br />\n" : "<br>\n";
        } else {
            result += renderToken(children, i);
        }
    }
    return result;
}

// renderer.fence. (info-string unescapeAll is deferred with entities/escape; not exercised
// by confirmed fence cases. highlight option is disabled, so highlighted == escaped body.)
static std::string renderFence(const Token& token) {
    std::string info = stripWs(unescapeAll(token.info));
    std::string langName;
    if (!info.empty()) {
        size_t sp = info.find_first_of(" \t\f\v\r");
        langName = (sp == std::string::npos) ? info : info.substr(0, sp);
    }
    std::string highlighted = escapeHtml(token.content);
    if (!info.empty()) {
        Token tmp;  // fence carries no other attrs in this scope
        tmp.attrs.push_back({"class", "language-" + langName});
        return "<pre><code" + renderAttrs(tmp) + ">" + highlighted + "</code></pre>\n";
    }
    return "<pre><code" + renderAttrs(token) + ">" + highlighted + "</code></pre>\n";
}

static std::string render(const std::vector<Token>& tokens) {
    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == "inline") {
            result += renderInline(tokens[i].children);
        } else if (tokens[i].type == "fence") {
            result += renderFence(tokens[i]);
        } else {
            result += renderToken(tokens, i);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// main — mirror reference.py: read all stdin, render, write stdout.
// ---------------------------------------------------------------------------
int main() {
    std::ios::sync_with_stdio(false);
    std::string input;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0)
        input.append(buf, n);
    std::string normalized = normalizeSource(input);
    std::vector<Token> tokens = blockParse(normalized);
    std::string html = render(tokens);
    std::fwrite(html.data(), 1, html.size(), stdout);
    return 0;
}
