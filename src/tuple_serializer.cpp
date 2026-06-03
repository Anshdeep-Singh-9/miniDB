#include "tuple_serializer.h"

#include <cstring>
#include <limits>
#include <iostream>

namespace {
template <typename T>
void append_value(std::vector<char>& out, const T& value) {
    // What: append the in-memory bytes of a fixed-size value into tuple output.
    // Why: INT values are stored directly as 4 bytes inside the serialized tuple.
    // Example: integer 10 is copied as sizeof(int32_t) bytes.
    const char* start = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), start, start + sizeof(T));
}

template <typename T>
bool read_value(const std::vector<char>& bytes,
                std::size_t offset,
                T& value_out) {
    if (offset + sizeof(T) > bytes.size()) {
        return false;
    }

    std::memcpy(&value_out, &bytes[offset], sizeof(T));
    return true;
}
}  // namespace

TupleValue TupleValue::FromInt(int32_t value) {
    TupleValue tuple_value;
    tuple_value.type = STORAGE_COLUMN_INT;
    tuple_value.int_value = value;
    return tuple_value;
}

TupleValue TupleValue::FromVarchar(const std::string& value) {
    TupleValue tuple_value;
    tuple_value.type = STORAGE_COLUMN_VARCHAR;
    tuple_value.string_value = value;
    return tuple_value;
}

bool TupleSerializer::serialize(const std::vector<ColumnSchema>& schema,
                                const std::vector<TupleValue>& values,
                                std::vector<char>& tuple_out) {
    // What: convert logical row values into compact bytes for page storage.
    // Why: DiskManager/DataPage can store only bytes, not C++ strings and objects directly.
    // Example: (1, "Aryan") becomes [4-byte int][2-byte string length]["Aryan" bytes].
    tuple_out.clear();

    if (schema.size() != values.size()) {
        return false;
    }

    for (std::size_t i = 0; i < schema.size(); ++i) {
        const ColumnSchema& column = schema[i];
        const TupleValue& value = values[i];

        if (column.type != value.type) {
            return false;
        }

        if (column.type == STORAGE_COLUMN_INT) {
            append_value(tuple_out, value.int_value);
            continue;
        }

        if (column.type == STORAGE_COLUMN_VARCHAR) {
            if (value.string_value.size() >
                static_cast<std::size_t>(column.max_length)) {
                return false;
            }

            if (value.string_value.size() >
                static_cast<std::size_t>(std::numeric_limits<uint16_t>::max())) {
                return false;
            }

            const uint16_t string_length =
                static_cast<uint16_t>(value.string_value.size());
            append_value(tuple_out, string_length);
            tuple_out.insert(tuple_out.end(),
                             value.string_value.begin(),
                             value.string_value.end());
            continue;
        }

        return false;
    }

    return true;
}

bool TupleSerializer::deserialize(const std::vector<ColumnSchema>& schema,
                                  const std::vector<char>& tuple_bytes,
                                  std::vector<TupleValue>& values_out) {
    // What: convert stored tuple bytes back into typed row values.
    // Why: SELECT/WHERE/UPDATE need readable INT/VARCHAR values after fetching a page.
    // Example: [4-byte int][length=5]["Aryan"] becomes id=1 and name="Aryan".
    values_out.clear();

    std::size_t offset = 0;
    for (std::size_t i = 0; i < schema.size(); ++i) {
        const ColumnSchema& column = schema[i];

        if (column.type == STORAGE_COLUMN_INT) {
            int32_t int_value = 0;
            if (!read_value(tuple_bytes, offset, int_value)) {
                return false;
            }

            values_out.push_back(TupleValue::FromInt(int_value));
            offset += sizeof(int32_t);
            continue;
        }

        if (column.type == STORAGE_COLUMN_VARCHAR) {
            uint16_t string_length = 0;
            if (!read_value(tuple_bytes, offset, string_length)) {
                return false;
            }

            offset += sizeof(uint16_t);
            if (offset + string_length > tuple_bytes.size()) {
                return false;
            }

            if (string_length > column.max_length) {
                return false;
            }

            std::string string_value(&tuple_bytes[offset],
                                     &tuple_bytes[offset] + string_length);
            values_out.push_back(TupleValue::FromVarchar(string_value));
            offset += string_length;
            continue;
        }

        return false;
    }

    return offset == tuple_bytes.size();
}

void TupleSerializer::print_tuple (const std::vector<TupleValue>& values){
    // What: print deserialized tuple values for debugging/display paths.
    // Why: once bytes become TupleValue objects, they can be shown as normal table cells.
    // Example: values [1, "CSE"] are printed as columns in the terminal.
    int n= values.size();
    // cout<<"Called!\n";
    for(int i=0; i<n; i++){
        if(values[i].type == STORAGE_COLUMN_INT){
        std::cout<<values[i].int_value<<"\t\t";
        }
        else{
            std::cout<<values[i].string_value<<"\t\t";
        }
    }
    
    std::cout<<std::endl;
}
