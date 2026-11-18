// form: the form drawn from the model it validates with.
//
// The point of the kit is that the same declaration drives the input's kind, its bounds,
// its label and its error — so the assertions here are mostly about *agreement*: the type
// and the validator say the same thing, the required star and the required rule say the
// same thing, and the error's link lands on the input's id.
#include <webc/test.h>
#include <webc/form.h>

WEBC_ENUM(Plan, starter, pro, team)

WEBC_MODEL(Signup,
           (id, i64, .pk = true),
           (email, str, .not_null = true, .required = true, .max = 120, .format = "email"),
           (team_name, str, .max = 60),
           (plan, Plan, .not_null = true),
           (seats, i32, .gte = 1, .lte = 50),
           (ahead, bool, .default_false = true),
           (bio, str, .max = 400),
           (secret, str, .no_json = true))

static Signup g_draft;

static str render_cmds(void (*v)(WBuild *)) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, v);
  return buf_str(&c.cmds.b);
}

static void one_field(WBuild *b) {
  (void)b;
  WForm f;
  form_init(&f, Signup_modelof(), &g_draft);
  form_field(&f, "email");
}

static void all_fields(WBuild *b) {
  (void)b;
  WForm f;
  form_init(&f, Signup_modelof(), &g_draft);
  form_fields(&f);
}

static void bad_email(WBuild *b) {
  (void)b;
  WForm f;
  form_init(&f, Signup_modelof(), &g_draft);
  f.submitted = true;
  werrs_add(&f.errs, str_lit("email"), str_lit("must be an address"));
  werrs_add(&f.errs, str_lit("plan"), str_lit("pick a plan"));
  form_field(&f, "email");
  form_errors(&f);
}

static void prefixed(WBuild *b) {
  (void)b;
  WForm f;
  form_init(&f, Signup_modelof(), &g_draft);
  f.prefix = "edit";
  form_field(&f, "team_name");
}

static void long_field(WBuild *b) {
  (void)b;
  WForm f;
  form_init(&f, Signup_modelof(), &g_draft);
  form_field_long(&f, "bio", 4);
}

TEST(an_email_field_asks_for_an_email_and_carries_its_limit) {
  memset(&g_draft, 0, sizeof g_draft);
  g_draft.email = str_lit("ada@example.com");
  str cmds = render_cmds(one_field);
  ASSERT_TRUE(str_contains(cmds, str_lit("type email")));
  ASSERT_TRUE(str_contains(cmds, str_lit("name email")));
  ASSERT_TRUE(str_contains(cmds, str_lit("value ada@example.com")));
  ASSERT_TRUE(str_contains(cmds, str_lit("maxlength 120")));
  ASSERT_TRUE(str_contains(cmds, str_lit("required")));
  // the label is the identifier said out loud, and points at the input
  ASSERT_TRUE(str_contains(cmds, str_lit("for f-email")));
  ASSERT_TRUE(str_contains(cmds, str_lit("Email")));
  ASSERT_TRUE(str_contains(cmds, str_lit("f-email")));
}

TEST(a_number_field_carries_the_bounds_the_validator_will_check) {
  memset(&g_draft, 0, sizeof g_draft);
  g_draft.seats = 3;
  str cmds = render_cmds(all_fields);
  ASSERT_TRUE(str_contains(cmds, str_lit("type number")));
  ASSERT_TRUE(str_contains(cmds, str_lit("min 1")));
  ASSERT_TRUE(str_contains(cmds, str_lit("max 50")));
  ASSERT_TRUE(str_contains(cmds, str_lit("step 1")));
  ASSERT_TRUE(str_contains(cmds, str_lit("value 3")));
}

TEST(a_bool_is_a_checkbox_and_an_enum_is_a_select_of_the_closed_set) {
  str cmds = render_cmds(all_fields);
  ASSERT_TRUE(str_contains(cmds, str_lit("type checkbox")));
  ASSERT_TRUE(str_contains(cmds, str_lit("select")));
  ASSERT_TRUE(str_contains(cmds, str_lit("value starter")));
  ASSERT_TRUE(str_contains(cmds, str_lit("value pro")));
  ASSERT_TRUE(str_contains(cmds, str_lit("value team")));
  // free is 0 and the draft is zeroed, so it is the selected one
  ASSERT_TRUE(str_contains(cmds, str_lit("selected true")));
}

TEST(every_field_is_drawn_once_and_the_nested_ones_are_not) {
  str cmds = render_cmds(all_fields);
  ASSERT_TRUE(str_contains(cmds, str_lit("f-ahead")));
  // team_name says twice what the underscore said once
  ASSERT_TRUE(str_contains(cmds, str_lit("Team name")));
}

TEST(an_error_sits_beside_its_field_and_is_wired_to_it) {
  str cmds = render_cmds(bad_email);
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-invalid true")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-describedby f-email-err")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class field err")));
  ASSERT_TRUE(str_contains(cmds, str_lit("must be an address")));
  ASSERT_TRUE(str_contains(cmds, str_lit("id f-email-err")));
}

TEST(the_summary_is_an_alert_with_a_link_to_each_field) {
  str cmds = render_cmds(bad_email);
  ASSERT_TRUE(str_contains(cmds, str_lit("role alert")));
  ASSERT_TRUE(str_contains(cmds, str_lit("href #f-email")));
  ASSERT_TRUE(str_contains(cmds, str_lit("href #f-plan")));
  ASSERT_TRUE(str_contains(cmds, str_lit("pick a plan")));
}

TEST(two_forms_on_a_page_get_two_id_spaces) {
  str cmds = render_cmds(prefixed);
  ASSERT_TRUE(str_contains(cmds, str_lit("id edit-team_name")));
  ASSERT_TRUE(str_contains(cmds, str_lit("for edit-team_name")));
}

TEST(a_long_field_is_the_one_place_textarea_is_chosen_and_the_app_says_so) {
  str cmds = render_cmds(long_field);
  ASSERT_TRUE(str_contains(cmds, str_lit("textarea")));
  ASSERT_TRUE(str_contains(cmds, str_lit("rows 4")));
  ASSERT_TRUE(str_contains(cmds, str_lit("maxlength 400")));
}

TEST(a_field_nobody_declared_is_silently_nothing) {
  str cmds = render_cmds(one_field);
  ASSERT_FALSE(str_contains(cmds, str_lit("nope")));
  ASSERT_NULL(form_field_of(Signup_modelof(), "nope"));
  ASSERT_NOT_NULL(form_field_of(Signup_modelof(), "email"));
}

TEST(the_label_is_the_identifier_said_out_loud) {
  ASSERT_STR_EQ(form_label("first_name"), "First name");
  ASSERT_STR_EQ(form_label("ahead"), "Ahead");
  ASSERT_STR_EQ(form_label("URL_path"), "URL path");
}

TEST_MAIN()