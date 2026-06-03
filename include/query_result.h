#ifndef QUERY_RESULT_H
#define QUERY_RESULT_H

#include "tuple_serializer.h"
#include <vector>
#include <string>

struct QueryResult {
    bool is_select = false;
    std::vector<ColumnSchema> schema;
    std::vector<std::vector<TupleValue>> rows;
    std::vector<ColumnSchema> source_schema;
    std::vector<std::vector<TupleValue>> source_rows;
    std::string message;
    bool success = true;
    std::string strategy; // To store if it was B+ Tree or Linear Scan
};

#endif
