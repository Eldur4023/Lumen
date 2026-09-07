#include <lumen_script/lexer.hpp>
#include <cctype>

namespace lumen_script {

namespace {

bool is_ident_start(unsigned char c) {
    // Bytes >= 0x80 are UTF-8 continuations: they are accepted so that
    // `contraseña` is a valid identifier.
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool is_ident_char(unsigned char c) {
    return is_ident_start(c) || std::isdigit(c);
}

// Translates the letter following a backslash.  Returns false if it is none of
// the known escapes.  Used by single-line strings and by triple-quoted ones, so
// that both understand exactly the same thing.
bool escape_char(char e, char& out) {
    switch (e) {
        case 'n':  out = '\n'; return true;
        case 't':  out = '\t'; return true;
        case 'r':  out = '\r'; return true;
        case '0':  out = '\0'; return true;
        case '"':  out = '"';  return true;
        case '\\': out = '\\'; return true;
        default:   return false;
    }
}

// Strips the margin from a triple-quoted string.
//
// The text is written indented inside the route body, but that indentation
// belongs to the file, not to the string: without stripping it, a SELECT would
// reach the engine log with eight spaces on every line.  There are three rules:
//
//   • a line break right after the opening does not count, it is there so the
//     text can start on its own line;
//   • if the closing sits alone on its line, that line does not count either;
//   • from the rest, the indentation common to every non-empty line is removed.
//
// Only spaces are considered: a line starting with a tab leaves the margin at
// zero and nothing is touched, which beats guessing how wide a tab is.
std::string strip_margin(std::string s) {
    if (s.rfind("\r\n", 0) == 0)         s.erase(0, 2);
    else if (!s.empty() && s[0] == '\n') s.erase(0, 1);

    const size_t last_nl = s.rfind('\n');
    if (last_nl != std::string::npos &&
        s.find_first_not_of(" \t\r", last_nl + 1) == std::string::npos)
        s.erase(last_nl);

    size_t margin = std::string::npos;
    for (size_t i = 0;;) {
        size_t end_pos = s.find('\n', i);
        const bool last_one = (end_pos == std::string::npos);
        if (last_one) end_pos = s.size();

        size_t j = i;
        while (j < end_pos && s[j] == ' ') ++j;
        if (j < end_pos && s[j] != '\r') margin = std::min(margin, j - i);

        if (last_one) break;
        i = end_pos + 1;
    }
    if (margin == std::string::npos || margin == 0) return s;

    std::string out;
    out.reserve(s.size());
    for (size_t i = 0;;) {
        size_t end_pos = s.find('\n', i);
        const bool last_one = (end_pos == std::string::npos);
        if (last_one) end_pos = s.size();

        const size_t skip = std::min(margin, end_pos - i);
        out.append(s, i + skip, end_pos - i - skip);

        if (last_one) break;
        out += '\n';
        i = end_pos + 1;
    }
    return out;
}

} // namespace

char Lexer::peek(size_t ahead) const {
    size_t i = pos_ + ahead;
    return i < file_.text.size() ? file_.text[i] : '\0';
}

char Lexer::advance() {
    char c = file_.text[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else           { ++col_; }
    return c;
}

void Lexer::push(Tok kind, SourceLoc loc, std::string text) {
    out_.push_back(Token{kind, std::move(text), loc});
}

void Lexer::skip_line_comment() {
    while (!eof() && peek() != '\n') advance();
}

// Processes the start of a logical line: measures the indentation and emits
// Indent/Dedent.  Blank lines and comment-only lines are skipped without
// touching the indentation stack.
void Lexer::handle_line_start() {
    while (!eof()) {
        size_t scan   = pos_;
        int    width  = 0;
        bool   tabs   = false;

        while (scan < file_.text.size()) {
            char c = file_.text[scan];
            if (c == ' ')       { ++width; ++scan; }
            else if (c == '\t') { tabs = true; ++width; ++scan; }
            else break;
        }

        // Empty line, comment only, or a stray carriage return: does not count.
        if (scan >= file_.text.size()) { pos_ = scan; col_ += width; return; }
        char c = file_.text[scan];
        if (c == '\n' || c == '\r' || c == '#') {
            while (pos_ < scan) advance();
            if (peek() == '#') skip_line_comment();
            if (peek() == '\r') advance();
            if (peek() == '\n') advance();
            continue;
        }

        if (tabs) {
            diags_.error({&file_.path, line_, 1},
                         "indentation with a tab: use spaces");
        }

        // A line starting with '.' continues the previous one (chaining on the
        // returned value), so it generates no Indent/Dedent.  The Newline of the
        // previous break was already emitted, because until here it was not
        // known the line was a continuation: it is withdrawn.
        if (c == '.' && !std::isdigit(static_cast<unsigned char>(
                            scan + 1 < file_.text.size() ? file_.text[scan + 1] : '\0'))) {
            while (pos_ < scan) advance();
            if (!out_.empty() && out_.back().is(Tok::Newline)) out_.pop_back();
            at_line_start_ = false;
            return;
        }

        while (pos_ < scan) advance();

        SourceLoc loc = here();
        if (width > indents_.back()) {
            indents_.push_back(width);
            push(Tok::Indent, loc);
        } else {
            while (width < indents_.back()) {
                indents_.pop_back();
                push(Tok::Dedent, loc);
            }
            if (width != indents_.back()) {
                diags_.error(loc, "indentation does not match any open level");
                indents_.push_back(width);
            }
        }
        at_line_start_ = false;
        return;
    }
}

void Lexer::lex_number(SourceLoc loc) {
    std::string text;
    while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();

    bool is_float = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        is_float = true;
        text += advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
    }
    push(is_float ? Tok::Float : Tok::Int, loc, std::move(text));
}

void Lexer::lex_string(SourceLoc loc) {
    // Three quotes open a string that can span several lines.
    if (peek(1) == '"' && peek(2) == '"') { lex_string_multi(loc); return; }

    advance();  // opening quote
    std::string value;
    while (true) {
        if (eof() || peek() == '\n') {
            diags_.error(loc, "unterminated string");
            break;
        }
        char c = advance();
        if (c == '"') break;
        if (c != '\\') { value += c; continue; }

        if (eof()) { diags_.error(loc, "unterminated string"); break; }
        char e = advance();
        char d;
        if (escape_char(e, d)) {
            value += d;
        } else {
            diags_.error({&file_.path, line_, col_ - 1},
                         std::string("unknown escape: \\") + e);
            value += e;
        }
    }
    push(Tok::String, loc, std::move(value));
}

// Triple-quoted string: good for embedding SQL or HTML without fighting the
// line breaks.
//
//     let q = """
//         SELECT id, title
//         FROM posts
//         WHERE author = ?
//         """
//
// The raw text is collected first and only then are the escapes applied: that
// way the indentation is measured over the line breaks WRITTEN in the file, and
// a \n from inside the string does not alter the margin.
void Lexer::lex_string_multi(SourceLoc loc) {
    advance(); advance(); advance();   // """

    std::string raw;
    bool        cerrada = false;
    while (!eof()) {
        if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
            advance(); advance(); advance();
            cerrada = true;
            break;
        }
        // The backslash and what follows travel together and untouched: else a \"
        // would leave a stray quote that could close the string too early.
        //
        if (peek() == '\\' && pos_ + 1 < file_.text.size()) {
            raw += advance();
            raw += advance();
            continue;
        }
        raw += advance();
    }
    if (!cerrada) diags_.error(loc, "unterminated string: the closing triple quote is missing");

    raw = strip_margin(std::move(raw));

    std::string value;
    value.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) { value += raw[i]; continue; }
        char d;
        if (escape_char(raw[++i], d)) {
            value += d;
        } else {
            diags_.error(loc, std::string("unknown escape: \\") + raw[i]);
            value += raw[i];
        }
    }
    push(Tok::String, loc, std::move(value));
}

void Lexer::lex_ident(SourceLoc loc) {
    std::string text;
    while (is_ident_char(static_cast<unsigned char>(peek()))) text += advance();
    Tok kind = keyword_or_ident(text);
    push(kind, loc, std::move(text));
}

void Lexer::lex_token() {
    char c = peek();
    SourceLoc loc = here();

    if (std::isdigit(static_cast<unsigned char>(c))) { lex_number(loc); return; }
    if (c == '"')                                    { lex_string(loc); return; }
    if (is_ident_start(static_cast<unsigned char>(c))){ lex_ident(loc);  return; }

    advance();
    switch (c) {
        case '(': ++bracket_depth_; push(Tok::LParen, loc);   return;
        case ')': if (bracket_depth_) --bracket_depth_; push(Tok::RParen, loc);   return;
        case '[': ++bracket_depth_; push(Tok::LBracket, loc); return;
        case ']': if (bracket_depth_) --bracket_depth_; push(Tok::RBracket, loc); return;
        case '{': ++bracket_depth_; push(Tok::LBrace, loc);   return;
        case '}': if (bracket_depth_) --bracket_depth_; push(Tok::RBrace, loc);   return;

        case ',': push(Tok::Comma, loc);    return;
        case ':': push(Tok::Colon, loc);    return;
        case '.': push(Tok::Dot, loc);      return;
        case '?': push(Tok::Question, loc); return;

        case '+':
            if (peek() == '+')      { advance(); push(Tok::PlusPlus, loc); }
            else if (peek() == '=') { advance(); push(Tok::PlusEq, loc); }
            else                    { push(Tok::Plus, loc); }
            return;
        case '*':
            if (peek() == '=') { advance(); push(Tok::StarEq, loc); }
            else               { push(Tok::Star, loc); }
            return;
        case '/':
            if (peek() == '=') { advance(); push(Tok::SlashEq, loc); }
            else               { push(Tok::Slash, loc); }
            return;
        case '%':
            if (peek() == '=') { advance(); push(Tok::PercentEq, loc); }
            else               { push(Tok::Percent, loc); }
            return;

        case '-':
            // '->' of the static mount, '--' and '-=' before the bare minus.
            if (peek() == '>')      { advance(); push(Tok::Arrow, loc); }
            else if (peek() == '-') { advance(); push(Tok::MinusMinus, loc); }
            else if (peek() == '=') { advance(); push(Tok::MinusEq, loc); }
            else                    { push(Tok::Minus, loc); }
            return;
        case '=':
            if (peek() == '=') { advance(); push(Tok::Eq, loc); }
            else               { push(Tok::Assign, loc); }
            return;
        case '!':
            if (peek() == '=') { advance(); push(Tok::NotEq, loc); return; }
            diags_.error(loc, "stray '!': negation is written 'not'");
            return;
        case '<':
            if (peek() == '=') { advance(); push(Tok::LtEq, loc); }
            else               { push(Tok::Lt, loc); }
            return;
        case '>':
            // '>' '>' never merge: List<Dict<string,int>> has to close with two
            // independent Gt tokens.
            if (peek() == '=') { advance(); push(Tok::GtEq, loc); }
            else               { push(Tok::Gt, loc); }
            return;
    }

    diags_.error(loc, std::string("caracter inesperado: '") + c + "'");
}

std::vector<Token> Lexer::tokenize() {
    out_.clear();

    while (true) {
        if (at_line_start_ && bracket_depth_ == 0) handle_line_start();
        if (eof()) break;

        char c = peek();

        if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
        if (c == '#') { skip_line_comment(); continue; }

        if (c == '\n') {
            // The position is taken BEFORE consuming the break: an error of the
            // "expected ':'" kind has to point at the end of the line that
            // caused it, not at column 1 of the next one.
            SourceLoc eol = here();
            advance();
            if (bracket_depth_ == 0) {
                // A Newline only makes sense if the line produced tokens.
                if (!out_.empty() && !out_.back().is(Tok::Newline) &&
                    !out_.back().is(Tok::Indent) && !out_.back().is(Tok::Dedent))
                    push(Tok::Newline, eol);
                at_line_start_ = true;
            }
            continue;
        }

        lex_token();
    }

    SourceLoc end{&file_.path, line_, col_};
    if (!out_.empty() && !out_.back().is(Tok::Newline)) push(Tok::Newline, end);
    while (indents_.size() > 1) { indents_.pop_back(); push(Tok::Dedent, end); }
    push(Tok::EndOfFile, end);

    if (bracket_depth_ != 0)
        diags_.error(end, "unclosed parenthesis or bracket");

    return std::move(out_);
}

} // namespace lumen_script
