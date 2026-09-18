#include "toolscheme_duckdb.hpp"

#include <duckdb.h>

#include <string>
#include <string_view>
#include <vector>

namespace toolscheme {
namespace duckdb {
namespace {

class DuckSql final : public SqlCapability {
public:
    explicit DuckSql(std::string root) : root_(std::move(root)) {}

    ~DuckSql() override {
        if (connection_) duckdb_disconnect(&connection_);
        if (database_) duckdb_close(&database_);
    }

    bool supports(std::string_view operation) const override { return operation == "sql-query"; }

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        (void)vm;
        if (operation != "sql-query")
            return unsupported_result(std::string(operation), "unknown sql operation");
        if (arguments.empty() || arguments[0].type() != Value::Type::String)
            return error_result("sql-query expects a query string", "invalid-argument",
                                "sql-query");

        std::string failure;
        if (!open(failure)) return error_result(failure, "host-error", "sql-query");

        const Value options = arguments.size() > 1 ? arguments[1] : Value::nil();
        const Value requested = option(options, "limit");
        const std::int64_t limit =
            requested.type() == Value::Type::Integer ? requested.as_integer() : 10000;

        duckdb_result result;
        const std::string sql(arguments[0].as_string());
        if (duckdb_query(connection_, sql.c_str(), &result) == DuckDBError) {
            const char* message = duckdb_result_error(&result);
            // A rejected query is the caller's mistake, and DuckDB's message is the
            // useful part of it -- including the refusal when a path is outside the
            // permitted directory.
            const std::string text = message ? message : "query failed";
            duckdb_destroy_result(&result);
            return error_result(text, "invalid-argument", "sql-query");
        }

        const idx_t columns = duckdb_column_count(&result);
        std::vector<Value> names;
        for (idx_t c = 0; c < columns; ++c)
            names.push_back(Value::string(duckdb_column_name(&result, c)));

        // Values come back as text through DuckDB's own conversion, so every column
        // type has one representation here instead of a partial reimplementation of
        // DuckDB's type system on this side. A caller that wants a number casts in
        // SQL, where the types actually live.
        const idx_t total = duckdb_row_count(&result);
        const idx_t wanted =
            limit > 0 && static_cast<idx_t>(limit) < total ? static_cast<idx_t>(limit) : total;
        std::vector<Value> rows;
        for (idx_t r = 0; r < wanted; ++r) {
            ListBuilder row(columns);
            for (idx_t c = 0; c < columns; ++c) {
                if (duckdb_value_is_null(&result, c, r)) {
                    row.field(duckdb_column_name(&result, c), Value::nil());
                    continue;
                }
                char* text = duckdb_value_varchar(&result, c, r);
                // Explicitly a string: a `char*` prefers the bool overload of
                // `field`, so every value in every row came back as #t.
                row.field(duckdb_column_name(&result, c),
                          std::string_view(text ? text : ""));
                if (text) duckdb_free(text);
            }
            rows.push_back(row.build());
        }
        const bool truncated = wanted < total;
        duckdb_destroy_result(&result);

        const std::int64_t returned = static_cast<std::int64_t>(rows.size());
        return ok_result({field("columns", Value::list(std::move(names))),
                          field("rows", Value::list(std::move(rows))),
                          field("count", returned),
                          field("truncated", truncated)});
    }

private:
    // Runs one statement, reporting failure rather than continuing. Every caller
    // here is establishing the sandbox, and a security setting that quietly did
    // not apply is worse than one that was never attempted.
    bool statement(const std::string& sql, std::string& failure) {
        duckdb_result result;
        const duckdb_state state = duckdb_query(connection_, sql.c_str(), &result);
        if (state == DuckDBError) {
            const char* message = duckdb_result_error(&result);
            failure = sql + ": " + (message ? message : "failed");
        }
        duckdb_destroy_result(&result);
        return state != DuckDBError;
    }

    bool reads(const std::string& sql, std::string& out) {
        duckdb_result result;
        if (duckdb_query(connection_, sql.c_str(), &result) == DuckDBError) {
            duckdb_destroy_result(&result);
            return false;
        }
        bool ok = false;
        if (duckdb_row_count(&result) > 0) {
            char* text = duckdb_value_varchar(&result, 0, 0);
            if (text) { out = text; duckdb_free(text); ok = true; }
        }
        duckdb_destroy_result(&result);
        return ok;
    }

    // Opened on first use: a capability that is installed but never queried should
    // cost nothing, and opening is the expensive part.
    //
    // The sandbox is established here and it is the whole reason this adapter is
    // more than three lines. A query can read files, so without this `sql-query`
    // would be the one hole in a boundary the rest of the API takes seriously --
    // and the first version of this had exactly that hole, because
    // `duckdb_set_config` refuses `allowed_directories` and the return value was
    // not checked. It reads /etc/passwd happily when you get this wrong.
    //
    // The working shape is not obvious: `allowed_directories` does nothing on its
    // own. It is an exception list carved out of *disabled* external access, so
    // access must be turned off for it to mean anything, and the pair is then
    // frozen so no later query can widen either.
    bool open(std::string& failure) {
        if (connection_) return true;
        if (duckdb_open(nullptr, &database_) == DuckDBError) {
            failure = "could not open the database";
            return false;
        }
        if (duckdb_connect(database_, &connection_) == DuckDBError) {
            failure = "could not connect to the database";
            return false;
        }
        const std::string quoted = "'" + root_ + "'";
        const std::vector<std::string> setup = {
            "SET allowed_directories=[" + quoted + "]",
            // Set for completeness, though DuckDB checks permission on the literal
            // path before any search path is consulted, so a relative name is
            // refused rather than resolved. SQL paths are therefore absolute; the
            // CLI binds `capability-root` so a caller can build one.
            "SET file_search_path=" + quoted,
            "SET autoinstall_known_extensions=false",
            "SET autoload_known_extensions=false",
            "SET allow_community_extensions=false",
            // Disabling access is what gives the directory list its force.
            "SET enable_external_access=false",
            // Last: nothing after this can widen any of the above.
            "SET lock_configuration=true",
        };
        for (const std::string& sql : setup)
            if (!statement(sql, failure)) { shut(); return false; }

        // Confirmed rather than assumed, because this failed silently once.
        std::string directories, external;
        const bool read_back =
            reads("SELECT value FROM duckdb_settings() WHERE name='allowed_directories'",
                  directories) &&
            reads("SELECT value FROM duckdb_settings() WHERE name='enable_external_access'",
                  external);
        if (!read_back || directories.find(root_) == std::string::npos || external != "false") {
            failure = "the query sandbox did not take effect; refusing to run unconfined";
            shut();
            return false;
        }
        return true;
    }

    void shut() {
        if (connection_) { duckdb_disconnect(&connection_); connection_ = nullptr; }
        if (database_) { duckdb_close(&database_); database_ = nullptr; }
    }

    std::string root_;
    duckdb_database database_ = nullptr;
    duckdb_connection connection_ = nullptr;
};

} // namespace

std::shared_ptr<SqlCapability> make_sql(const std::string& root) {
    return std::make_shared<DuckSql>(root);
}

} // namespace duckdb
} // namespace toolscheme
