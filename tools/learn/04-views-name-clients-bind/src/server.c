// SPDX-License-Identifier: Apache-2.0
#include <webc/server.h>
#include "app.h"

GET("/") { return page(Tasks, db_all(Task)); }
