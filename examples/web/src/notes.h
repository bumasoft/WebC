// The client-only example's data: one model, one vector, nothing about a server.
#pragma once
#include <webc/web.h>

WEBC_MODEL(Note, (id, i64, .pk = true),
                 (title, Text, .required = true, .max = 200),
                 (done, bool))

Vec(Note)
