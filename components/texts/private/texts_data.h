// The tables that tools/lang.py makes (texts_data.c), and the plural rules of texts.c.
#pragma once

#include "texts_ids.h"

typedef struct {
    const char *code;           // "uk"
    const char *name;           // "Українська"
    int (*plural)(long n);      // the slot of the plural form for n: 0, 1 or 2
    const char *const *slots;   // the texts; a plural takes 3 slots, a list one slot per item
    const char *js;             // the "js.*" texts as a JSON object
} texts_language_t;

// The first slot of each key. Equal for all languages.
extern const unsigned short texts_first_slot[T_COUNT];
extern const texts_language_t texts_languages[TEXTS_LANGUAGE_COUNT];

// Plural rules. uk: one (1, 21, 31...), few (2-4, 22-24...), many (the rest). en: one (1), other.
int texts_plural_uk(long n);
int texts_plural_en(long n);
