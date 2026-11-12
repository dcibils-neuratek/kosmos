/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **ICU on Kosmos** (`docs/w4.md` W4b): the things WTF and JavaScriptCore
 * ask ICU for - normalising, collating, breaking text into words, and
 * formatting a number - each in more than one locale, written as one
 * sentence of plain ASCII.
 *
 * The same file is compiled twice. Into `icutest.elf`, with ICU as
 * `tools/icu_build.py` built it for Kosmos; and for this Mac, against the
 * same ICU 79.1 and the same data built natively (`icu_build.py host`), by
 * `tools/run_icu.py`, which holds the guest's sentence to the Mac's. So
 * what is checked is that ICU on Kosmos answers as ICU does on a machine
 * nobody doubts - and the sentence is readable, so what it answers can be
 * looked at as well.
 *
 * Every answer here comes out of the data: German and Swedish put "ä" in
 * different places, Thai has no spaces and is broken by a dictionary, and
 * the thousands separator is the locale's. Data that was not found gives
 * an error's name in the sentence rather than an answer.
 */

#include <string>
#include <vector>
#include <algorithm>

#include <string.h>
#include <stdio.h>

#include <unicode/brkiter.h>
#include <unicode/coll.h>
#include <unicode/locid.h>
#include <unicode/normalizer2.h>
#include <unicode/numberformatter.h>
#include <unicode/uclean.h>
#include <unicode/unistr.h>
#include <unicode/uversion.h>

/* Text as ASCII: what is not ASCII as <U+XXXX>, so a serial line carries it. */
static std::string ascii(const icu::UnicodeString &s)
{
    std::string out;

    for (int32_t i = 0; i < s.length(); ) {
        UChar32 c = s.char32At(i);
        if (c >= 0x20 && c < 0x7f) {
            out += (char)c;
        } else {
            char hex[16];
            snprintf(hex, sizeof(hex), "<U+%04X>", (unsigned)c);
            out += hex;
        }
        i += U16_LENGTH(c);
    }
    return out;
}

static icu::UnicodeString text(const char *utf8)
{
    return icu::UnicodeString::fromUTF8(utf8);
}

static std::string failed(const char *what, UErrorCode e)
{
    return std::string(what) + " failed: " + u_errorName(e);
}

static std::string normalise(void)
{
    UErrorCode e = U_ZERO_ERROR;
    const icu::Normalizer2 *nfc = icu::Normalizer2::getNFCInstance(e);
    const icu::Normalizer2 *nfd = icu::Normalizer2::getNFDInstance(e);
    const icu::Normalizer2 *nfkc = icu::Normalizer2::getNFKCInstance(e);

    if (U_FAILURE(e))
        return failed("normalisers", e);

    icu::UnicodeString composed = nfc->normalize(text("e\xcc\x81" "cole"), e);
    icu::UnicodeString decomposed = nfd->normalize(text("\xc3\x85"), e);
    icu::UnicodeString compat = nfkc->normalize(text("\xef\xac\x81" "\xe2\x91\xa0"), e);

    if (U_FAILURE(e))
        return failed("normalising", e);

    return "NFC " + ascii(composed) + ", NFD " + ascii(decomposed)
           + ", NFKC " + ascii(compat);
}

static std::string collate(const char *locale)
{
    UErrorCode e = U_ZERO_ERROR;
    icu::Collator *c = icu::Collator::createInstance(icu::Locale(locale), e);

    if (U_FAILURE(e))
        return failed("collator", e);

    std::vector<icu::UnicodeString> words;
    for (const char *w : {"zebra", "\xc3\xa4pple", "apple", "Zoo", "able", "\xc3\xb6l"})
        words.push_back(text(w));

    std::sort(words.begin(), words.end(),
              [c](const icu::UnicodeString &a, const icu::UnicodeString &b) {
                  UErrorCode ce = U_ZERO_ERROR;
                  return c->compare(a, b, ce) == UCOL_LESS;
              });
    delete c;

    std::string out = std::string(locale) + " ";
    for (size_t i = 0; i < words.size(); i++)
        out += (i ? " " : "") + ascii(words[i]);
    return out;
}

static std::string words(const char *locale, const char *utf8)
{
    UErrorCode e = U_ZERO_ERROR;
    icu::BreakIterator *b = icu::BreakIterator::createWordInstance(icu::Locale(locale), e);

    if (U_FAILURE(e))
        return failed("word breaker", e);

    icu::UnicodeString s = text(utf8);
    b->setText(s);

    std::string out;
    int32_t from = b->first();

    for (int32_t to = b->next(); to != icu::BreakIterator::DONE; from = to, to = b->next()) {
        if (b->getRuleStatus() == UBRK_WORD_NONE)
            continue;                             /* spaces and punctuation */
        icu::UnicodeString word;
        s.extractBetween(from, to, word);
        out += (out.empty() ? "" : "|") + ascii(word);
    }
    delete b;
    return std::string(locale) + " " + out;
}

static std::string number(const char *locale)
{
    UErrorCode e = U_ZERO_ERROR;
    icu::UnicodeString s = icu::number::NumberFormatter::withLocale(icu::Locale(locale))
                               .formatDouble(1234567.891, e).toString(e);

    if (U_FAILURE(e))
        return failed("number", e);
    return std::string(locale) + " " + ascii(s);
}

extern "C" int kosmos_icutest_run(char *out, unsigned long size)
{
    std::string said = std::string("ICU ") + U_ICU_VERSION + "; "
        + normalise() + "; "
        + collate("de") + "; " + collate("sv") + "; "
        + words("en", "It's 3.5 km, isn't it?") + "; "
        + words("th", "\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\xa9\xe0\xb8\xb2\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2"
                      "\xe0\xb8\x87\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\xa2") + "; "
        + number("en_US") + "; " + number("de_DE") + "; " + number("fr_FR");

    snprintf(out, size, "%s", said.c_str());
    return (int)said.size();
}

#ifndef KOSMOS_USER
/* The Mac's half (`tools/run_icu.py`): the same sentence, printed. */
int main(void)
{
    static char said[2048];

    kosmos_icutest_run(said, sizeof(said));
    puts(said);
    return 0;
}
#endif
