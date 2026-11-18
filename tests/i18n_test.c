// i18n: the missing key is loud, the plural picks the form the language needs, and a
// translated string may reorder its placeholders.
#include <webc/test.h>
#include <webc/i18n.h>

// a catalog is sorted by key — that is what makes the lookup a binary search
static const WMsg EN[] = {
    MSG("bye", "Bye, {name}"),
    MSG("greeting", "Hello, {name}"),
    MSG("items_few", "{n} items"),
    MSG("items_many", "{n} items"),
    MSG("items_one", "{n} item"),
    MSG("items_other", "{n} items"),
    MSG("thanks", "Thanks, {who} — {what}"),
};

static const WMsg DE[] = {
    MSG("bye", "Tschüss, {name}"),
    MSG("greeting", "Hallo, {name}"),
    MSG("items_one", "{n} Ding"),
    MSG("items_other", "{n} Dinge"),
};

static const WMsg PL[] = {
    MSG("items_few", "{n} rzeczy"),
    MSG("items_many", "{n} rzeczy"),
    MSG("items_one", "{n} rzecz"),
    MSG("items_other", "{n} rzeczy"),
};

static void setup(void) { i18n_init(CATS(en_catalog(EN), ltr_catalog("de", DE), slavic_catalog("pl", PL))); }

TEST(a_string_and_its_placeholder_arrive_together) {
  setup();
  ASSERT_STR_EQ(t("greeting", "name", "Ada"), "Hello, Ada");
  ASSERT_STR_EQ(t("thanks", "who", "Ada", "what", "the tip"), "Thanks, Ada — the tip");
  ASSERT_STR_EQ(t("greeting", "name", str_lit("Bo")), "Hello, Bo");
}

TEST(switching_language_switches_the_words) {
  setup();
  ASSERT_TRUE(i18n_use("de"));
  ASSERT_STR_EQ(i18n_lang(), "de");
  ASSERT_STR_EQ(t("greeting", "name", "Ada"), "Hallo, Ada");
  // a region answers to its language
  ASSERT_TRUE(i18n_use("de-AT"));
  ASSERT_STR_EQ(t("bye", "name", "Ada"), "Tschüss, Ada");
  // a language nobody has leaves the last one in place rather than going blank
  ASSERT_FALSE(i18n_use("zz"));
  ASSERT_STR_EQ(i18n_lang(), "de");
}

TEST(an_untranslated_key_falls_back_the_fallback_catalog_not_to_nothing) {
  setup();
  i18n_use("de");
  ASSERT_STR_EQ(t("thanks", "who", "A", "what", "B"), "Thanks, A — B");
}

TEST(a_missing_key_is_loud) {
  setup();
  // visible in the page, so it is caught in a test instead of shipping as an empty sentence
  ASSERT_STR_EQ(t("nope"), "??nope??");
}

TEST(english_plural_has_two_forms_polish_has_four) {
  setup();
  ASSERT_STR_EQ(tn("items", 1), "1 item");
  ASSERT_STR_EQ(tn("items", 0), "0 items");
  ASSERT_STR_EQ(tn("items", 5), "5 items");
  setup();
  i18n_use("pl");
  ASSERT_STR_EQ(tn("items", 1), "1 rzecz");
  ASSERT_STR_EQ(tn("items", 3), "3 rzeczy");   // few
  ASSERT_STR_EQ(tn("items", 5), "5 rzeczy");   // many
  ASSERT_STR_EQ(tn("items", 22), "22 rzeczy"); // few, not 2's "many"
  ASSERT_STR_EQ(tn("items", 112), "112 rzeczy"); // many: 12 is the exception
}

TEST(a_plural_can_carry_another_placeholder) {
  setup();
  i18n_use("de");
  ASSERT_STR_EQ(tn("items", 3), "3 Dinge");
}

TEST(a_translated_string_may_reorder_its_placeholders) {
  setup();
  ASSERT_STR_EQ(t("thanks", "who", "A", "what", "B"), "Thanks, A — B");
}

TEST(a_translated_string_may_reorder_the_count) {
  setup();
  // the job of `{n}` is to survive a language that puts the number last
  ASSERT_STR_EQ(tn("items", 2), "2 items");
}

TEST(direction_is_a_property_of_the_language) {
  ASSERT_STR_EQ(lang_dir_of("ar"), "rtl");
  ASSERT_STR_EQ(lang_dir_of("he-IL"), "rtl");
  ASSERT_STR_EQ(lang_dir_of("en"), "ltr");
  ASSERT_STR_EQ(lang_dir_of(""), "ltr");
  ASSERT_STR_EQ(lang_dir_of("arbitrary"), "ltr"); // a prefix that is not a tag
}

TEST(nothing_is_initialised_and_nothing_crashes) {
  i18n_reset();
  ASSERT_STR_EQ(i18n_lang(), "en");
  ASSERT_EQ(i18n_count(), 0);
  ASSERT_STR_EQ(t("greeting", "name", "Ada"), "??greeting??");
}

TEST_MAIN()
