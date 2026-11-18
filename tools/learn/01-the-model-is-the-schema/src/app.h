// the half the suite and the server share: the model
#include <webc/shared.h>

WEBC_MODEL(Task, (id, i64, .pk = true),
                 (title, Text, .required = true, .max = 200))

Vec(Task)
