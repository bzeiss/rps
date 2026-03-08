#pragma once

#include <rps/coordinator/Graph.hpp>

#include <string>

namespace rps::coordinator {

// ---------------------------------------------------------------------------
// Graph serialization — JSON round-trip via Boost.JSON
// ---------------------------------------------------------------------------

class GraphSerializer {
public:
    /// Serialize a graph to a JSON string.
    static std::string toJson(const Graph& graph);

    /// Deserialize a graph from a JSON string.
    /// Throws std::runtime_error on parse or structural errors.
    static Graph fromJson(const std::string& json);
};

} // namespace rps::coordinator
