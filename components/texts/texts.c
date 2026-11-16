#include "texts.h"

#include <string.h>

#include "texts_data.h"

// The current language. A change is one pointer write: a text that a task reads at that time comes from the old
// or the new language, both are valid.
static const texts_language_t *volatile s_lang = &texts_languages[0];

int texts_plural_uk(long n)
{
    if (n < 0) {
        n = -n;
    }
    const long last = n % 10, last2 = n % 100;
    if (last == 1 && last2 != 11) {
        return 0;
    }
    if (last >= 2 && last <= 4 && (last2 < 12 || last2 > 14)) {
        return 1;
    }
    return 2;
}

int texts_plural_en(long n)
{
    return n == 1 || n == -1 ? 0 : 1;
}

const char *tr(text_id_t id)
{
    if ((unsigned)id >= T_COUNT) {
        return "";
    }
    return s_lang->slots[texts_first_slot[id]];
}

const char *trn(text_id_t id, long n)
{
    if ((unsigned)id >= T_COUNT) {
        return "";
    }
    const texts_language_t *lang = s_lang;
    return lang->slots[texts_first_slot[id] + lang->plural(n)];
}

const char *tra(text_id_t id, int i)
{
    if ((unsigned)id >= T_COUNT || i < 0) {
        return "";
    }
    const unsigned end = (unsigned)id + 1 < T_COUNT ? texts_first_slot[id + 1] : TEXTS_SLOT_COUNT;
    const unsigned slot = texts_first_slot[id] + (unsigned)i;
    if (slot >= end) {
        return "";
    }
    return s_lang->slots[slot];
}

void texts_set_language(const char *code)
{
    for (int i = 0; i < TEXTS_LANGUAGE_COUNT; i++) {
        if (code && strcmp(code, texts_languages[i].code) == 0) {
            s_lang = &texts_languages[i];
            return;
        }
    }
    s_lang = &texts_languages[0];
}

const char *texts_language(void)
{
    return s_lang->code;
}

const char *texts_language_code(int i)
{
    return i >= 0 && i < TEXTS_LANGUAGE_COUNT ? texts_languages[i].code : "";
}

const char *texts_language_name(int i)
{
    return i >= 0 && i < TEXTS_LANGUAGE_COUNT ? texts_languages[i].name : "";
}

const char *texts_js(void)
{
    return s_lang->js;
}
