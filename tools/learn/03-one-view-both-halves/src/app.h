// the half the view and the route share: the model and the context's shape
#include <webc/shared.h>

WEBC_MODEL(Item, (id, i64, .pk = true), (name, Text, .required = true))
Vec(Item)

typedef struct HomeCtx {
  const VecItem *items;
  Text note;
} HomeCtx;
declare_view(Home, HomeCtx);
