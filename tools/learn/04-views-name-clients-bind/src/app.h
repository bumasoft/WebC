// the half both sides share: the model and the view's prototype
#include <webc/shared.h>

WEBC_MODEL(Task, (id, i64, .pk = true), (title, Text, .required = true), (done, bool))
Vec(Task)
declare_view(Tasks, VecTask);
