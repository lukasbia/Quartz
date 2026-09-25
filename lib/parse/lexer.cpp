#include "quartz/Parse/Lexer.h"

#include "quartz/AST/Decl.h"
#include "quartz/AST/DiagnosticsParse.h"
#include "quartz/AST/Expr.h"
#include "quartz/AST/Identifier.h"
#include "quartz/AST/Pattern.h"
#include "quartz/AST/Stmt.h"
#include "quartz/AST/Type.h"

#include "quartz/Basic/Diagnostic.h"
#include "quartz/Basic/DiagnosticEngine.h"
#include "quartz/Basic/LangOptions.h"
#include "quartz/Basic/SourceLocation.h"
#include "quartz/Basic/SourceManager.h"

#include "quartz/Bridging/ASTGen.h"

#include "quartz/Lexer/LexerDiagnostics.h"
#include "quartz/Lexer/LexerKeywords.h"
#include "quartz/Lexer/LexerLiteral.h"
#include "quartz/Lexer/LexerOptions.h"
#include "quartz/Lexer/LexerState.h"
#include "quartz/Lexer/LexerString.h"
#include "quartz/Lexer/LexerTrivia.h"
#include "quartz/Lexer/LexerUnicode.h"

#include "quartz/Parse/Confusables.h"
#include "quartz/Parse/Token.h"
#include "quartz/Parse/TokenKind.h"

#include "quartz/QIL/QILKeywords.h"
#include "quartz/QIL/QILToken.h"
#include "quartz/QIL/QILTokenKind.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace quartz {

namespace {

constexpr char NullCharacter = '\0';

constexpr unsigned MaximumIdentifierLength = 4096;
constexpr unsigned MaximumCommentNestingDepth = 256;
constexpr unsigned MaximumInterpolationDepth = 256;

constexpr uint32_t InvalidCodePoint =
    0xFFFFFFFFu;

bool isASCIIWhitespace(char value) {
    return value == ' ' ||
           value == '\t' ||
           value == '\r' ||
           value == '\n' ||
           value == '\f' ||
           value == '\v';
}

bool isASCIIDigit(char value) {
    return value >= '0' && value <= '9';
}

bool isASCIIHexDigit(char value) {
    return
        (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

bool isASCIIBinaryDigit(char value) {
    return value == '0' || value == '1';
}

bool isASCIIOctalDigit(char value) {
    return value >= '0' && value <= '7';
}

bool isASCIIAlpha(char value) {
    return
        (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z');
}

bool isASCIIIdentifierStart(char value) {
    return isASCIIAlpha(value) || value == '_';
}

bool isASCIIIdentifierContinuation(char value) {
    return isASCIIIdentifierStart(value) ||
           isASCIIDigit(value);
}

bool isOperatorCharacter(char value) {
    switch (value) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '=':
        case '!':
        case '<':
        case '>':
        case '&':
        case '|':
        case '^':
        case '~':
        case '?':
            return true;

        default:
            return false;
    }
}

bool isPunctuationCharacter(char value) {
    switch (value) {
        case '(':
        case ')':
        case '{':
        case '}':
        case '[':
        case ']':
        case ',':
        case ':':
        case ';':
        case '.':
            return true;

        default:
            return false;
    }
}

bool isHexPrefix(char value) {
    return value == 'x' || value == 'X';
}

bool isBinaryPrefix(char value) {
    return value == 'b' || value == 'B';
}

bool isOctalPrefix(char value) {
    return value == 'o' || value == 'O';
}

bool isDigitSeparator(char value) {
    return value == '_';
}

bool isValidScalar(uint32_t value) {
    if (value > 0x10FFFF)
        return false;

    if (value >= 0xD800 && value <= 0xDFFF)
        return false;

    return true;
}

} // namespace

Lexer::Lexer(
    SourceManager& sourceManager,
    SourceManager::BufferID bufferID,
    DiagnosticEngine* diagnostics,
    const LangOptions& langOptions,
    const LexerOptions& options)
    : sourceManager(sourceManager),
      bufferID(bufferID),
      diagnostics(diagnostics),
      langOptions(langOptions),
      options(options),
      buffer(sourceManager.getBuffer(bufferID)) {
}

bool Lexer::isAtEnd() const {
    return currentOffset >= buffer.size();
}

char Lexer::peekChar(unsigned distance) const {
    if (currentOffset >= buffer.size())
        return NullCharacter;

    const std::size_t position =
        currentOffset + distance;

    if (position >= buffer.size())
        return NullCharacter;

    return buffer[position];
}

char Lexer::peekPreviousChar() const {
    if (currentOffset == 0)
        return NullCharacter;

    return buffer[currentOffset - 1];
}

char Lexer::advance() {
    if (isAtEnd())
        return NullCharacter;

    const char value =
        buffer[currentOffset++];

    if (value == '\n') {
        ++line;
        column = 1;
    } else {
        ++column;
    }

    return value;
}

void Lexer::advanceBy(unsigned count) {
    while (count != 0) {
        advance();
        --count;
    }
}

std::string_view Lexer::getCurrentSlice() const {
    if (tokenStartOffset > currentOffset)
        return {};

    return buffer.substr(
        tokenStartOffset,
        currentOffset - tokenStartOffset
    );
}

std::string_view Lexer::getSlice(
    std::size_t start,
    std::size_t end) const {

    if (start > end)
        return {};

    if (start >= buffer.size())
        return {};

    end = std::min(end, buffer.size());

    return buffer.substr(
        start,
        end - start
    );
}

SourceLocation Lexer::getCurrentLocation() const {
    return SourceLocation(
        bufferID,
        static_cast<unsigned>(currentOffset),
        line,
        column
    );
}

SourceLocation Lexer::getTokenStartLocation() const {
    return sourceManager.getLocation(
        bufferID,
        tokenStartOffset
    );
}

SourceRange Lexer::getCurrentRange() const {
    return SourceRange(
        getTokenStartLocation(),
        getCurrentLocation()
    );
}

LexerState Lexer::saveState() const {
    LexerState saved;

    saved.currentOffset = currentOffset;
    saved.tokenStartOffset = tokenStartOffset;

    saved.line = line;
    saved.column = column;

    saved.state = state;

    saved.interpolationDepth =
        interpolationDepth;

    saved.hadLeadingWhitespace =
        hadLeadingWhitespace;

    saved.hadLeadingNewline =
        hadLeadingNewline;

    saved.hadLeadingComment =
        hadLeadingComment;

    saved.lookahead = lookahead;

    return saved;
}

void Lexer::restoreState(
    const LexerState& saved) {

    currentOffset =
        saved.currentOffset;

    tokenStartOffset =
        saved.tokenStartOffset;

    line = saved.line;
    column = saved.column;

    state = saved.state;

    interpolationDepth =
        saved.interpolationDepth;

    hadLeadingWhitespace =
        saved.hadLeadingWhitespace;

    hadLeadingNewline =
        saved.hadLeadingNewline;

    hadLeadingComment =
        saved.hadLeadingComment;

    lookahead = saved.lookahead;
}

void Lexer::skipTrivia() {
    hadLeadingWhitespace = false;
    hadLeadingNewline = false;
    hadLeadingComment = false;

    bool progress = true;

    while (progress && !isAtEnd()) {
        progress = false;

        while (!isAtEnd()) {
            const char value =
                peekChar();

            if (!isASCIIWhitespace(value))
                break;

            if (value == '\n')
                hadLeadingNewline = true;

            hadLeadingWhitespace = true;

            advance();
            progress = true;
        }

        if (peekChar() == '/' &&
            peekChar(1) == '/') {

            lexLineComment();

            hadLeadingComment = true;
            progress = true;

            continue;
        }

        if (peekChar() == '/' &&
            peekChar(1) == '*') {

            lexBlockComment();

            hadLeadingComment = true;
            progress = true;

            continue;
        }
    }
}

void Lexer::lexLineComment() {
    advance();
    advance();

    while (!isAtEnd()) {
        if (peekChar() == '\n')
            break;

        advance();
    }
}

void Lexer::lexBlockComment() {
    advance();
    advance();

    unsigned depth = 1;

    while (!isAtEnd() && depth != 0) {
        if (peekChar() == '/' &&
            peekChar(1) == '*') {

            if (depth >= MaximumCommentNestingDepth) {
                diagnose(
                    "Quartz block comment nesting limit exceeded"
                );

                advance();
                advance();
                continue;
            }

            ++depth;

            advance();
            advance();

            continue;
        }

        if (peekChar() == '*' &&
            peekChar(1) == '/') {

            --depth;

            advance();
            advance();

            continue;
        }

        advance();
    }

    if (depth != 0) {
        diagnose(
            "unterminated block comment"
        );
    }
}

bool Lexer::isIdentifierStart() const {
    const char value = peekChar();

    if (isASCIIIdentifierStart(value))
        return true;

    if (!options.enableUnicodeIdentifiers)
        return false;

    return isUnicodeIdentifierStartAtCurrentPosition();
}

bool Lexer::isIdentifierContinuation() const {
    const char value = peekChar();

    if (isASCIIIdentifierContinuation(value))
        return true;

    if (!options.enableUnicodeIdentifiers)
        return false;

    return isUnicodeIdentifierContinuationAtCurrentPosition();
}

Token Lexer::lexIdentifier() {
    const SourceLocation start =
        getCurrentLocation();

    unsigned length = 0;
    bool first = true;

    while (!isAtEnd()) {
        if (length >= MaximumIdentifierLength) {
            diagnose(
                "Quartz identifier is too long"
            );
            break;
        }

        if (first) {
            if (!isIdentifierStart())
                break;
        } else {
            if (!isIdentifierContinuation())
                break;
        }

        if (static_cast<unsigned char>(
                peekChar()) < 0x80) {
            advance();
        } else {
            consumeUnicodeIdentifierCharacter();
        }

        first = false;
        ++length;
    }

    const SourceLocation end =
        getCurrentLocation();

    const std::string_view spelling =
        getCurrentSlice();

    TokenKind kind =
        lookupKeyword(spelling);

    if (kind == TokenKind::unknown)
        kind = TokenKind::identifier;

    if (looksLikeConfusableIdentifier(spelling)) {
        diagnoseConfusableIdentifier(
            spelling,
            start,
            end
        );
    }

    return makeToken(
        kind,
        start,
        end
    );
}

Token Lexer::lexNumber() {
    const SourceLocation start =
        getCurrentLocation();

    bool floating = false;

    if (peekChar() == '0') {
        const char prefix = peekChar(1);

        if (isBinaryPrefix(prefix)) {
            advance();
            advance();

            lexBinaryDigits();

            return makeToken(
                TokenKind::integerLiteral,
                start,
                getCurrentLocation()
            );
        }

        if (isOctalPrefix(prefix)) {
            advance();
            advance();

            lexOctalDigits();

            return makeToken(
                TokenKind::integerLiteral,
                start,
                getCurrentLocation()
            );
        }

        if (isHexPrefix(prefix)) {
            advance();
            advance();

            lexHexDigits();

            if (peekChar() == '.' &&
                isASCIIHexDigit(peekChar(1))) {

                floating = true;

                advance();

                lexHexDigits();
            }

            if (peekChar() == 'p' ||
                peekChar() == 'P') {

                floating = true;

                advance();

                if (peekChar() == '+' ||
                    peekChar() == '-') {
                    advance();
                }

                lexDecimalDigits();
            }

            return makeToken(
                floating
                    ? TokenKind::floatingLiteral
                    : TokenKind::integerLiteral,
                start,
                getCurrentLocation()
            );
        }
    }

    lexDecimalDigits();

    if (peekChar() == '.' &&
        isASCIIDigit(peekChar(1))) {

        floating = true;

        advance();

        lexDecimalDigits();
    }

    if (peekChar() == 'e' ||
        peekChar() == 'E') {

        floating = true;

        advance();

        if (peekChar() == '+' ||
            peekChar() == '-') {
            advance();
        }

        lexDecimalDigits();
    }

    return makeToken(
        floating
            ? TokenKind::floatingLiteral
            : TokenKind::integerLiteral,
        start,
        getCurrentLocation()
    );
}

void Lexer::lexDecimalDigits() {
    bool foundDigit = false;

    while (!isAtEnd()) {
        const char value = peekChar();

        if (isASCIIDigit(value)) {
            foundDigit = true;
            advance();
            continue;
        }

        if (isDigitSeparator(value)) {
            advance();
            continue;
        }

        break;
    }

    if (!foundDigit) {
        diagnose(
            "expected decimal digit"
        );
    }
}

void Lexer::lexBinaryDigits() {
    bool foundDigit = false;

    while (!isAtEnd()) {
        const char value = peekChar();

        if (isASCIIBinaryDigit(value)) {
            foundDigit = true;
            advance();
            continue;
        }

        if (isDigitSeparator(value)) {
            advance();
            continue;
        }

        break;
    }

    if (!foundDigit) {
        diagnose(
            "expected binary digit"
        );
    }
}

void Lexer::lexOctalDigits() {
    bool foundDigit = false;

    while (!isAtEnd()) {
        const char value = peekChar();

        if (isASCIIOctalDigit(value)) {
            foundDigit = true;
            advance();
            continue;
        }

        if (isDigitSeparator(value)) {
            advance();
            continue;
        }

        break;
    }

    if (!foundDigit) {
        diagnose(
            "expected octal digit"
        );
    }
}

void Lexer::lexHexDigits() {
    bool foundDigit = false;

    while (!isAtEnd()) {
        const char value = peekChar();

        if (isASCIIHexDigit(value)) {
            foundDigit = true;
            advance();
            continue;
        }

        if (isDigitSeparator(value)) {
            advance();
            continue;
        }

        break;
    }

    if (!foundDigit) {
        diagnose(
            "expected hexadecimal digit"
        );
    }
}

Token Lexer::lexString() {
    const SourceLocation start =
        getCurrentLocation();

    if (peekChar() == '"' &&
        peekChar(1) == '"' &&
        peekChar(2) == '"') {

        return lexMultilineString();
    }

    advance();

    state =
        LexerStateKind::InString;

    while (!isAtEnd()) {
        const char value =
            peekChar();

        if (value == '"') {
            advance();

            state =
                LexerStateKind::Normal;

            return makeToken(
                TokenKind::stringLiteral,
                start,
                getCurrentLocation()
            );
        }

        if (value == '\\') {
            lexEscapeSequence();
            continue;
        }

        if (value == '$' &&
            peekChar(1) == '(') {

            lexInterpolation();
            continue;
        }

        if (value == '\n') {
            diagnose(
                "newline in single-line string literal"
            );
            break;
        }

        advance();
    }

    state =
        LexerStateKind::Normal;

    diagnose(
        "unterminated string literal"
    );

    return makeToken(
        TokenKind::stringLiteral,
        start,
        getCurrentLocation()
    );
}

Token Lexer::lexMultilineString() {
    const SourceLocation start =
        getCurrentLocation();

    advance();
    advance();
    advance();

    state =
        LexerStateKind::InMultilineString;

    while (!isAtEnd()) {
        if (peekChar() == '"' &&
            peekChar(1) == '"' &&
            peekChar(2) == '"') {

            advance();
            advance();
            advance();

            state =
                LexerStateKind::Normal;

            return makeToken(
                TokenKind::multilineStringLiteral,
                start,
                getCurrentLocation()
            );
        }

        if (peekChar() == '\\') {
            lexEscapeSequence();
            continue;
        }

        if (peekChar() == '$' &&
            peekChar(1) == '(') {

            lexInterpolation();
            continue;
        }

        advance();
    }

    state =
        LexerStateKind::Normal;

    diagnose(
        "unterminated multiline string literal"
    );

    return makeToken(
        TokenKind::multilineStringLiteral,
        start,
        getCurrentLocation()
    );
}

void Lexer::lexEscapeSequence() {
    advance();

    if (isAtEnd()) {
        diagnose(
            "unterminated escape sequence"
        );
        return;
    }

    switch (peekChar()) {
        case '0':
        case 'a':
        case 'b':
        case 'e':
        case 'f':
        case 'n':
        case 'r':
        case 't':
        case 'v':
        case '\\':
        case '"':
        case '\'':
            advance();
            return;

        case 'u':
            lexUnicodeEscape();
            return;

        default:
            diagnose(
                "unknown escape sequence"
            );
            advance();
            return;
    }
}

void Lexer::lexUnicodeEscape() {
    advance();

    if (peekChar() != '{') {
        diagnose(
            "expected '{' in Unicode escape"
        );
        return;
    }

    advance();

    unsigned digits = 0;
    uint32_t value = 0;

    while (!isAtEnd() &&
           peekChar() != '}') {

        const char character =
            peekChar();

        if (!isASCIIHexDigit(character)) {
            diagnose(
                "invalid Unicode escape digit"
            );

            advance();
            continue;
        }

        unsigned digit = 0;

        if (character >= '0' &&
            character <= '9') {
            digit =
                static_cast<unsigned>(
                    character - '0'
                );
        } else if (
            character >= 'a' &&
            character <= 'f') {
            digit =
                static_cast<unsigned>(
                    character - 'a' + 10
                );
        } else {
            digit =
                static_cast<unsigned>(
                    character - 'A' + 10
                );
        }

        if (value >
            (0x10FFFFu - digit) / 16u) {
            diagnose(
                "Unicode escape is out of range"
            );
        } else {
            value =
                value * 16u + digit;
        }

        ++digits;
        advance();
    }

    if (peekChar() == '}') {
        advance();
    } else {
        diagnose(
            "unterminated Unicode escape"
        );
    }

    if (digits == 0) {
        diagnose(
            "Unicode escape requires hexadecimal digits"
        );
    }

    if (!isValidScalar(value)) {
        diagnose(
            "Unicode escape does not name a valid Unicode scalar"
        );
    }
}

void Lexer::lexInterpolation() {
    if (interpolationDepth >=
        MaximumInterpolationDepth) {

        diagnose(
            "maximum string interpolation depth exceeded"
        );

        advance();
        advance();

        return;
    }

    advance();
    advance();

    ++interpolationDepth;

    state =
        LexerStateKind::InInterpolation;
}

Token Lexer::lexOperator() {
    const SourceLocation start =
        getCurrentLocation();

    const char first = advance();
    const char second = peekChar();

    if (first == '=' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::equalEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '!' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::notEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '<' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::lessEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '>' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::greaterEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '+' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::plusEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '-' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::minusEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '*' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::starEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '/' &&
        second == '=') {

        advance();

        return makeToken(
            TokenKind::slashEqual,
            start,
            getCurrentLocation()
        );
    }

    if (first == '-' &&
        second == '>') {

        advance();

        return makeToken(
            TokenKind::arrow,
            start,
            getCurrentLocation()
        );
    }

    if (first == '&' &&
        second == '&') {

        advance();

        return makeToken(
            TokenKind::logicalAnd,
            start,
            getCurrentLocation()
        );
    }

    if (first == '|' &&
        second == '|') {

        advance();

        return makeToken(
            TokenKind::logicalOr,
            start,
            getCurrentLocation()
        );
    }

    if (first == '?' &&
        second == '?') {

        advance();

        return makeToken(
            TokenKind::questionQuestion,
            start,
            getCurrentLocation()
        );
    }

    if (first == '.' &&
        second == '.') {

        advance();

        if (peekChar() == '.') {
            advance();

            return makeToken(
                TokenKind::ellipsis,
                start,
                getCurrentLocation()
            );
        }

        return makeToken(
            TokenKind::range,
            start,
            getCurrentLocation()
        );
    }

    switch (first) {
        case '+':
            return makeToken(
                TokenKind::plus,
                start,
                getCurrentLocation()
            );

        case '-':
            return makeToken(
                TokenKind::minus,
                start,
                getCurrentLocation()
            );

        case '*':
            return makeToken(
                TokenKind::star,
                start,
                getCurrentLocation()
            );

        case '/':
            return makeToken(
                TokenKind::slash,
                start,
                getCurrentLocation()
            );

        case '%':
            return makeToken(
                TokenKind::percent,
                start,
                getCurrentLocation()
            );

        case '=':
            return makeToken(
                TokenKind::equal,
                start,
                getCurrentLocation()
            );

        case '!':
            return makeToken(
                TokenKind::exclamation,
                start,
                getCurrentLocation()
            );

        case '<':
            return makeToken(
                TokenKind::less,
                start,
                getCurrentLocation()
            );

        case '>':
            return makeToken(
                TokenKind::greater,
                start,
                getCurrentLocation()
            );

        case '?':
            return makeToken(
                TokenKind::question,
                start,
                getCurrentLocation()
            );

        default:
            break;
    }

    while (!isAtEnd() &&
           isOperatorCharacter(peekChar())) {
        advance();
    }

    return makeToken(
        TokenKind::customOperator,
        start,
        getCurrentLocation()
    );
}

Token Lexer::lexAttribute() {
    const SourceLocation start =
        getCurrentLocation();

    advance();

    if (!isIdentifierStart()) {
        return makeToken(
            TokenKind::atSign,
            start,
            getCurrentLocation()
        );
    }

    while (!isAtEnd() &&
           isIdentifierContinuation()) {

        if (static_cast<unsigned char>(
                peekChar()) < 0x80) {
            advance();
        } else {
            consumeUnicodeIdentifierCharacter();
        }
    }

    return makeToken(
        TokenKind::attribute,
        start,
        getCurrentLocation()
    );
}

Token Lexer::lexDirective() {
    const SourceLocation start =
        getCurrentLocation();

    advance();

    if (!isIdentifierStart()) {
        return makeToken(
            TokenKind::hash,
            start,
            getCurrentLocation()
        );
    }

    while (!isAtEnd() &&
           isIdentifierContinuation()) {

        if (static_cast<unsigned char>(
                peekChar()) < 0x80) {
            advance();
        } else {
            consumeUnicodeIdentifierCharacter();
        }
    }

    return makeToken(
        TokenKind::directive,
        start,
        getCurrentLocation()
    );
}

Token Lexer::lexPunctuation() {
    const SourceLocation start =
        getCurrentLocation();

    const char value = advance();

    TokenKind kind =
        TokenKind::unknown;

    switch (value) {
        case '(':
            kind = TokenKind::leftParen;
            break;

        case ')':
            kind = TokenKind::rightParen;
            break;

        case '{':
            kind = TokenKind::leftBrace;
            break;

        case '}':
            kind = TokenKind::rightBrace;
            break;

        case '[':
            kind = TokenKind::leftBracket;
            break;

        case ']':
            kind = TokenKind::rightBracket;
            break;

        case ',':
            kind = TokenKind::comma;
            break;

        case ':':
            kind = TokenKind::colon;
            break;

        case ';':
            kind = TokenKind::semicolon;
            break;

        case '.':
            kind = TokenKind::period;
            break;

        case '@':
            kind = TokenKind::atSign;
            break;

        case '#':
            kind = TokenKind::hash;
            break;

        default:
            break;
    }

    return makeToken(
        kind,
        start,
        getCurrentLocation()
    );
}

Token Lexer::lex() {
    if (lookahead.has_value()) {
        Token result =
            *lookahead;

        lookahead.reset();

        return result;
    }

    return lexImpl();
}

Token Lexer::peek() {
    if (!lookahead.has_value())
        lookahead = lexImpl();

    return *lookahead;
}

Token Lexer::lexImpl() {
    skipTrivia();

    tokenStartOffset =
        currentOffset;

    if (isAtEnd()) {
        const SourceLocation location =
            getCurrentLocation();

        return makeToken(
            TokenKind::eof,
            location,
            location
        );
    }

    const char value =
        peekChar();

    if (isIdentifierStart())
        return lexIdentifier();

    if (isASCIIDigit(value))
        return lexNumber();

    if (value == '"')
        return lexString();

    if (value == '@')
        return lexAttribute();

    if (value == '#')
        return lexDirective();

    if (isOperatorCharacter(value))
        return lexOperator();

    if (isPunctuationCharacter(value))
        return lexPunctuation();

    const SourceLocation start =
        getCurrentLocation();

    advance();

    diagnose(
        "unexpected character in Quartz source"
    );

    return makeToken(
        TokenKind::unknown,
        start,
        getCurrentLocation()
    );
}

Token Lexer::makeToken(
    TokenKind kind,
    SourceLocation start,
    SourceLocation end) const {

    return Token(
        kind,
        SourceRange(start, end),
        getSlice(
            start.getOffset(),
            end.getOffset()
        ),
        hadLeadingWhitespace,
        hadLeadingNewline,
        hadLeadingComment
    );
}

bool Lexer::isUnicodeIdentifierStartAtCurrentPosition()
    const {

    if (isAtEnd())
        return false;

    const char* begin =
        buffer.data() + currentOffset;

    const char* end =
        buffer.data() + buffer.size();

    const char* cursor = begin;

    const uint32_t codePoint =
        decodeUTF8(cursor, end);

    if (codePoint == InvalidCodePoint)
        return false;

    return quartz::isUnicodeIdentifierStart(
        codePoint
    );
}

bool Lexer::isUnicodeIdentifierContinuationAtCurrentPosition()
    const {

    if (isAtEnd())
        return false;

    const char* begin =
        buffer.data() + currentOffset;

    const char* end =
        buffer.data() + buffer.size();

    const char* cursor = begin;

    const uint32_t codePoint =
        decodeUTF8(cursor, end);

    if (codePoint == InvalidCodePoint)
        return false;

    return quartz::isUnicodeIdentifierContinuation(
        codePoint
    );
}

void Lexer::consumeUnicodeIdentifierCharacter() {
    if (isAtEnd())
        return;

    const char* begin =
        buffer.data() + currentOffset;

    const char* end =
        buffer.data() + buffer.size();

    const char* cursor = begin;

    const uint32_t codePoint =
        decodeUTF8(cursor, end);

    if (codePoint == InvalidCodePoint) {
        diagnose(
            "invalid UTF-8 sequence in identifier"
        );

        advance();
        return;
    }

    const std::size_t length =
        static_cast<std::size_t>(
            cursor - begin
        );

    for (std::size_t index = 0;
         index < length;
         ++index) {
        advance();
    }
}

void Lexer::diagnose(
    std::string_view message) {

    if (diagnostics == nullptr)
        return;

    diagnostics->diagnose(
        getCurrentLocation(),
        message
    );
}

void Lexer::diagnoseConfusableIdentifier(
    std::string_view identifier,
    SourceLocation start,
    SourceLocation end) {

    if (diagnostics == nullptr)
        return;

    diagnostics->diagnoseConfusableIdentifier(
        identifier,
        SourceRange(start, end)
    );
}

bool Lexer::looksLikeConfusableIdentifier(
    std::string_view identifier) const {

    return containsUnicodeConfusableCharacter(
        identifier
    );
}

} // namespace quartz