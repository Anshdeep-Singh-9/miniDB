#ifndef ALTER_H
#define ALTER_H

#include "declaration.h"
#include <string>

bool execute_alter_add_column(const std::string& table_name,
                              const std::string& column_name,
                              const std::string& type_spec,
                              bool has_default,
                              const std::string& default_value);

#endif
