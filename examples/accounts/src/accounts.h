// The accounts app's model, shared by the views and the routes.
#pragma once
#include <webc/shared.h>

WEBC_MODEL(Account, (id, i64, .pk = true),
                    (email, Text, .required = true, .format = "email", .unique = true),
                    (password_hash, Text, .required = true, .sensitive = true, .no_json = true),
                    (created_at, Text))

// What a login form carries. A struct of its own, because a form is not an Account:
// no id, no hash, and a password instead of one. INTERNAL says so to the schema check:
// this model is a request body, not a table nobody migrated.
WEBC_MODEL(Credentials, (email, Text, .required = true, .format = "email"),
                        (password, Text, .required = true, .min = 8, .format = "password"))
WEBC_MODEL_INTERNAL(Credentials)

Vec(Account)

// What the two auth pages are given. A struct, not a bare sentence, because a page that
// failed has more to say than "no": the field the sentence is about, and the email the
// reader typed, which a re-render must not throw away.
typedef struct AuthCtx {
  Text error; // the sentence to show, "" when there is nothing to say
  Text email; // what the reader typed, so a failed post does not empty the field
  Text bad;   // the field the sentence is about ("email", "password", "") for aria-invalid
} AuthCtx;

typedef struct MeCtx {
  Account *account;
} MeCtx;

declare_view(Signin, AuthCtx);
declare_view(Signup, AuthCtx);
declare_view(Me, MeCtx);