// The three pages. Markup only: the sentence a failed post produced arrives as a
// parameter, never as a global — two people signing in at once must not read each
// other's error, and a re-render must not forget what the reader typed.
#include "accounts.h"

// ---------------------------------------------------------------- the small parts

// What a field is, so the form is written as data and drawn once.
typedef struct Input {
  const char *name;         // the form field, the input's id and the label's `for`
  Text label;               // the words beside it
  const char *type;         // "email" · "password"
  const char *autocomplete; // what a password manager is allowed to fill
  Text value;               // what the reader typed; "" on a first paint
  const char *hint;         // the rule, under the field; null for none
  const char *minlength;    // the browser's own copy of a length rule
  bool focus;               // the field a fresh page starts in
} Input;

// The model writes the sentence ("password: must be at least 8 characters") and this page
// shows it to a person: capitalised, and with a full stop. Nothing else is rewritten — the
// words are the model's, which is the whole point of validating with a declaration.
static bool filled(Text v) { return v && *v; }
static Text sentence(Text in) {
  if (!in || !*in) return "";
  size_t n = strlen(in);
  char first = (in[0] >= 'a' && in[0] <= 'z') ? (char)(in[0] - ('a' - 'A')) : in[0];
  char last = in[n - 1];
  if (last == '.' || last == '!' || last == '?') return fmt("%c%s", first, in + 1);
  return fmt("%c%s.", first, in + 1);
}

// "2026-11-13T07:42:48Z" -> "13 November 2026". The stored value is a timestamp; the page
// says what a reader asks for, which is the day they joined.
static Text long_date(Text iso) {
  if (!iso || !*iso) return "";
  str s = fmt_date_iso(iso, "%e %B %Y");
  return s.p ? fmt("%.*s", (int)s.n, s.p) : "";
}

static Text initial(Text email) {
  if (!email || !*email) return "?";
  char c = email[0];
  if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
  return fmt("%c", c);
}

static void brand_lockup(const char *where) {
  el("a") { class_("brand"); href(where);
    el("span") { class_("brand-mark"); attr("aria-hidden", "true"); text("W"); }
    el("span") { class_("brand-word"); text("WebC"); }
    el("span") { class_("brand-sep"); attr("aria-hidden", "true"); text("/"); }
    el("span") { class_("brand-app"); text("accounts"); }
  }
}

// The sentence a failed post produced, in one live region: it is announced when it appears,
// and the field it is about points at it with aria-describedby, so the two are one thing to a
// screen reader as well as to a pair of eyes.
static void error_line(Text error) {
  if (!filled(error)) return;
  el("p") { class_("error"); id_("form-error"); attr("role", "alert"); text("%s", sentence(error)); }
}

// One labelled field: `for` binds the label, `aria-describedby` binds the hint and the
// sentence above, and a field the post complained about says so with aria-invalid too.
static void input(const Input *in, bool bad) {
  bool has_hint = in->hint && *in->hint;
  el("div") { class_(bad ? "field bad" : "field");
    el("label") { attr("for", in->name); text("%s", in->label); }
    el("input") {
      id_(in->name);
      attr("name", in->name);
      type_(in->type);
      attr("autocomplete", in->autocomplete);
      if (in->minlength) attr("minlength", in->minlength);
      if (strcmp(in->type, "email") == 0) {
        attr("inputmode", "email");
        attr("autocapitalize", "none");
        attr("spellcheck", "false");
      }
      if (filled(in->value)) value_(in->value);
      if (in->focus) attr("autofocus", "");
      required();
      if (bad) attr("aria-invalid", "true");
      if (bad && has_hint) attr("aria-describedby", fmt("%s-hint form-error", in->name));
      else if (bad) attr("aria-describedby", "form-error");
      else if (has_hint) attr("aria-describedby", fmt("%s-hint", in->name));
    }
    if (has_hint) el("p") { class_("hint"); id_fmt("%s-hint", in->name); text("%s", in->hint); }
  }
}

static void fact(Text claim, Text detail) {
  el("li") {
    el("strong") { text("%s", claim); }
    el("span") { text("%s", detail); }
  }
}

// The left half of an auth page: what this app is, for a reader who has not signed in yet.
// Hidden on a phone (the stylesheet does it), where a sign-in page is a sign-in page.
static void pitch(void) {
  el("aside") { class_("pitch");
    brand_lockup("/");
    el("p") { class_("eyebrow"); text("SESSIONS, END TO END"); }
    el("h2") { text("One row, one cookie, no session table."); }
    el("p") { class_("lede");
      text("Sign up and the app writes the account, hashes the password and mints a signed "
           "cookie. Every step is a form post, so the whole flow works with JavaScript "
           "switched off.");
    }
    el("ul") { class_("facts");
      fact("PBKDF2-SHA256", "A per-password salt, and the cost stored beside the hash.");
      fact("HttpOnly · SameSite=Lax", "Signed, so there is no session table to look anything up in.");
      fact("SQLite and Postgres", "The same suite runs against both, unchanged.");
    }
  }
}

// ---------------------------------------------------------------- the pages

// The two fields both pages have. Which one takes the caret is decided here, once: the
// field the server complained about, or the first one still empty. A failed post must not
// drop the reader back into the field they already filled in.
static void credential_fields(AuthCtx *c, bool signup) {
  bool bad_email = c->bad && strcmp(c->bad, "email") == 0;
  bool bad_password = c->bad && strcmp(c->bad, "password") == 0;
  bool focus_email = bad_email || (!bad_password && !filled(c->email));
  input(&(Input){.name = "email", .label = "Email", .type = "email",
                 .autocomplete = "email", .value = c->email, .focus = focus_email},
        bad_email);
  input(&(Input){.name = "password", .label = "Password", .type = "password",
                 .autocomplete = signup ? "new-password" : "current-password",
                 .minlength = signup ? "8" : nullptr,
                 .hint = signup ? "8 characters or more. It is hashed before it is stored." : nullptr,
                 .focus = !focus_email},
        bad_password);
}

view(Signin, AuthCtx *c) {
  el("div") { class_("auth");
    pitch();
    el("main") { class_("card");
      el("h1") { text("Sign in"); }
      el("p") { class_("sub"); text("Welcome back. Use the email you signed up with."); }
      error_line(c->error);
      el("form") { class_("stack"); post_to("/login");
        credential_fields(c, false);
        el("button") { class_("primary"); type_("submit"); text("Sign in"); }
      }
      el("p") { class_("alt");
        text("New here? ");
        el("a") { href("/signup"); text("Create an account"); }
      }
      el("p") { class_("fineprint");
        text("No JavaScript: the form posts, the handler answers, and the cookie rides the "
             "redirect.");
      }
    }
  }
}

view(Signup, AuthCtx *c) {
  el("div") { class_("auth");
    pitch();
    el("main") { class_("card");
      el("h1") { text("Create an account"); }
      el("p") { class_("sub"); text("One model declares the row, the rules, and this form."); }
      error_line(c->error);
      el("form") { class_("stack"); post_to("/signup");
        credential_fields(c, true);
        el("button") { class_("primary"); type_("submit"); text("Create account"); }
      }
      el("p") { class_("alt");
        text("Already registered? ");
        el("a") { href("/login"); text("Sign in"); }
      }
      el("p") { class_("fineprint");
        text("The row keeps a PBKDF2-SHA256 hash and the salt it was made with — never the "
             "password.");
      }
    }
  }
}

static void meta_row(Text term, Text value) {
  el("div") {
    el("dt") { text("%s", term); }
    el("dd") { text("%s", value); }
  }
}

view(Me, MeCtx *c) {
  Account *me = c->account;
  el("div") { class_("app");
    el("header") { class_("topbar");
      brand_lockup("/me");
      el("div") { class_("who");
        el("span") { class_("avatar"); attr("aria-hidden", "true"); text("%s", initial(me->email)); }
        el("span") { class_("who-email"); text("%s", me->email); }
      }
    }
    el("main") { class_("page");
      el("p") { class_("eyebrow"); text("ACCOUNT"); }
      el("h1") { text("Your account"); }
      el("p") { class_("sub"); text("Signed in as %s.", me->email); }
      el("section") { class_("panel"); attr("aria-label", "Account details");
        el("dl") { class_("meta");
          meta_row("Email", me->email);
          meta_row("Account", fmt("#%lld", (long long)me->id));
          meta_row("Member since", long_date(me->created_at));
          meta_row("Session", "Signed cookie · HttpOnly · SameSite=Lax");
        }
      }
      el("div") { class_("row");
        el("form") { post_to("/logout");
          el("button") { class_("secondary"); type_("submit"); text("Sign out"); }
        }
        el("p") { class_("fineprint"); text("POST /logout clears the cookie and redirects to /login."); }
      }
    }
  }
}