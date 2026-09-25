#pragma once

#include "quartz/Basic/LangOptions.h"
#include "quartz/Basic/SourceManager.h"
#include "quartz/Lexer/LexerOptions.h"
#include "quartz/Lexer/LexerState.h"
#include "quartz/Parse/Token.h"

#include <cstddef>
#include <string_view>

namespace quartz {

class DiagnosticEngine;

class Lexer {
public:
    Lexer(
        SourceManager& sourceManager,
        SourceManager::BufferID bufferID,
        DiagnosticEngine* diagnostics,
        const LangOptions& langOptions,
        const LexerOptions& options
    );

    Token lex();

    Token peek();

    bool isAtEnd() const;

    LexerState saveState() const;

    void restoreState(
        const LexerState& state
    );

    SourceLocation getCurrentLocation() const;

    SourceLocation getTokenStartLocation() const;

    SourceRange getCurrentRange() const;

private:
    Token lexImpl();

    Token lexIdentifier();

    Token lexNumber();

    Token lexString();

    Token lexMultilineString();

    Token lexOperator();

    Token lexPunctuation();

    Token lexAttribute();

    Token lexDirective();

    void skipTrivia();

    void lexLineComment();

    void lexBlockComment();

    void lexEscapeSequence();

    void lexUnicodeEscape();

    void lexInterpolation();

    void lexDecimalDigits();

    void lexBinaryDigits();

    void lexOctalDigits();

    void lexHexDigits();

    void advanceBy(
        unsigned count
    );

    char advance();

    char peekChar(
        unsigned distance = 0
    ) const;

    char peekPreviousChar() const;

    bool isIdentifierStart() const;

    bool isIdentifierContinuation() const;

    bool isUnicodeIdentifierStartAtCurrentPosition()
        const;

    bool isUnicodeIdentifierContinuationAtCurrentPosition()
        const;

    void consumeUnicodeIdentifierCharacter();

    std::string_view getCurrentSlice() const;

    std::string_view getSlice(
        std::size_t start,
        std::size_t end
    ) const;

    Token makeToken(
        TokenKind kind,
        SourceLocation start,
        SourceLocation end
    ) const;

    void diagnose(
        std::string_view message
    );

    void diagnoseConfusableIdentifier(
        std::string_view identifier,
        SourceLocation start,
        SourceLocation end
    );

    bool looksLikeConfusableIdentifier(
        std::string_view identifier
    ) const;

private:
    SourceManager& sourceManager;

    SourceManager::BufferID bufferID;

    DiagnosticEngine* diagnostics;

    const LangOptions& langOptions;

    LexerOptions options;

    std::string_view buffer;

    std::size_t currentOffset = 0;
    std::size_t tokenStartOffset = 0;

    unsigned line = 1;
    unsigned column = 1;

    LexerStateKind state =
        LexerStateKind::Normal;

    unsigned interpolationDepth = 0;

    bool hadLeadingWhitespace = false;
    bool hadLeadingNewline = false;
    bool hadLeadingComment = false;

    std::optional<Token> lookahead;
};

}