#ifndef TOOLSCHEME_DUCKDB_HPP
#define TOOLSCHEME_DUCKDB_HPP

#include "toolscheme.hpp"

#include <memory>
#include <string>

// An optional analytical store. The project has no external dependencies by
// design -- md5, sha1, sha256 and the diff are all in-tree -- so this is compiled
// only when TOOLSCHEME_DUCKDB is defined, and `toolscheme` builds, runs and keeps
// collecting without it.
//
// It exists because capture and analysis want opposite things. Capture is one
// tiny append inside a 2.5 ms budget on every tool call an agent makes, and must
// not depend on anything being reachable. Analysis is whole-corpus aggregation
// that takes 24 seconds as a scan. So the log stays an append-only file and this
// reads it, rather than standing between the hook and the disk.
namespace toolscheme {
namespace duckdb {

// Queries are confined to `root` the way every other path in the API is: DuckDB is
// configured to refuse file access outside it and to install nothing.
std::shared_ptr<SqlCapability> make_sql(const std::string& root);

} // namespace duckdb
} // namespace toolscheme

#endif
