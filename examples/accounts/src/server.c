// The server half: signup, sign-in, a session cookie, and one page that needs it.
//
//   curl -c jar -d 'email=a@example.com&password=hunter2hunter2' localhost:8080/signup
//   curl -b jar localhost:8080/me
//
// No error is stored outside a response: what a failed post has to say is the context of
// the page it answers with, so two readers signing in at the same time cannot swap sentences
// and a reload cannot resurrect one.
#include <webc/server.h>
#include "accounts.h"

// One sentence for a wrong password and for an email nobody registered: telling them apart
// turns the form into an oracle for which addresses have accounts.
#define WRONG_CREDENTIALS "that email and password do not match"

GET("/") { return redirect("/me"); }

GET("/login") {
  head_title("Sign in — accounts");
  AuthCtx ctx = {0};
  return page(Signin, &ctx);
}

GET("/signup") {
  head_title("Create an account — accounts");
  AuthCtx ctx = {0};
  return page(Signup, &ctx);
}

GET("/me") {
  i64 uid = auth_require_session(req, auth_secret());
  if (!uid) return redirect("/login");
  Account *me = db_get(Account, uid);
  if (!me) return redirect("/login"); // a cookie for an account that is gone
  head_title("Your account — accounts");
  MeCtx ctx = {.account = me};
  return page(Me, &ctx);
}

POST("/signup") {
  Credentials in = {0}; // filled before it is validated, so a failed post can keep the email
  head_title("Create an account — accounts");
  if (!body_try(Credentials, &in)) {
    AuthCtx ctx = {.error = body_error(), .email = in.email, .bad = body_error_field()};
    return page_with(422, Signup, &ctx);
  }
  Account a = {.email = text_copy(in.email),
               .password_hash = auth_hash(in.password),
               .created_at = now()};
  if (!db_try_save(Account, &a)) {
    AuthCtx ctx = {.error = "that email is already registered", .email = in.email, .bad = "email"};
    return page_with(409, Signup, &ctx);
  }
  Response res = redirect("/me");
  auth_set_session(&res, auth_secret(), a.id);
  return res;
}

POST("/login") {
  Credentials in = {0};
  head_title("Sign in — accounts");
  if (!body_try(Credentials, &in)) {
    AuthCtx ctx = {.error = body_error(), .email = in.email, .bad = body_error_field()};
    return page_with(422, Signin, &ctx);
  }
  WQuery *qy = q("");
  q_str(qy, "em", (str){in.email, strlen(in.email)});
  Account *a = db_find(Account, "email = :em", qy);
  if (!a || !auth_verify(a->password_hash, in.password)) {
    AuthCtx ctx = {.error = WRONG_CREDENTIALS, .email = in.email};
    return page_with(401, Signin, &ctx);
  }
  Response res = redirect("/me");
  auth_set_session(&res, auth_secret(), a->id);
  return res;
}

POST("/logout") {
  Response res = redirect("/login");
  auth_clear_session(&res);
  return res;
}