// The texts of the screen, the page and Home Assistant in the selected language.
// The texts come from lang/*.json (see tools/lang.py); the keys are the T_* values of texts_ids.h.
#pragma once

#include "texts_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

// Return the text of id in the current language.
const char *tr(text_id_t id);

// Return the plural form of id for the number n (for example "%d день", "%d дні" or "%d днів").
const char *trn(text_id_t id, long n);

// Return item i of a list (for example the month names). An index out of the list gives "".
const char *tra(text_id_t id, int i);

// Set the language by its code ("uk", "en"). An unknown code sets the default language (the first one).
void texts_set_language(const char *code);

// Return the code of the current language.
const char *texts_language(void);

// The languages: code and name ("en", "English"), i from 0 to TEXTS_LANGUAGE_COUNT - 1.
const char *texts_language_code(int i);
const char *texts_language_name(int i);

// Return the "js.*" texts of the current language as one JSON object, without the "js." prefix.
// It is safe inside <script>.
const char *texts_js(void);

#ifdef __cplusplus
}
#endif
