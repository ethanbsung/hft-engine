#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include <stdexcept>
#include <string_view>

// =============================================================================
// Core primitive types.
// =============================================================================

namespace hft {

// --- Time ------------------------------------------------------------------
// A monotonic nanosecond timestamp. Wall-clock is irrelevant on the hot path;
// you want a steady, cheap-to-read source. (Whether that's steady_clock,
// rdtsc/__builtin_readcyclecounter, or mach_absolute_time is a decision to
// revisit when you start measuring.)
using nanos_t = std::int64_t;

// --- Price -----------------------------------------------------------------
// Decision: integer ticks. price = n * tick_size, where tick_size is looked
// up per-symbol via SymbolTable::tick_size(id). Exact, fast, cache-able as
// an array index — avoids the exact-equality/fragile-keying issues that a
// double-based price would have on the order book's price levels.
using price_t = std::int64_t;

using qty_t = std::int64_t;       // quantity in smallest tradeable unit
using order_id_t = std::uint64_t;
using SymbolId = std::uint32_t;
enum class Side : std::uint8_t { Buy, Sell };
using order_ref_t = std::uint32_t;

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
};

// SymbolTable
class SymbolTable {
public:
    SymbolId intern(std::string_view name, double tick_size, double qty_increment, int scale) {
        auto it = symbolTable.find(name);
        if (it != symbolTable.end()) {
            return it->second;
        } else {
            SymbolId id = idToName.size();
            idToName.push_back(std::string(name));
            symbolTable.emplace(std::string(name), id);
            tickSizes.push_back(tick_size);
            qtyIncrements.push_back(qty_increment);
            scales.push_back(scale);
            return id;
        }
    }

    SymbolId lookup(std::string_view name) const {
        auto it = symbolTable.find(name);
        if (it == symbolTable.end()) {
            throw std::out_of_range("symbol not found");
        } else {
            return it->second;
        }
    }

    std::string_view name(SymbolId id) const {
        return idToName[id];
    }

    double tick_size(SymbolId id) const {
        return tickSizes[id];
    }

    double qty_increment(SymbolId id) const {
        return qtyIncrements[id];
    }

    int scale(SymbolId id) const {
        return scales[id];
    }

private:
    std::vector<std::string> idToName;
    std::unordered_map<std::string, SymbolId, StringHash, std::equal_to<>> symbolTable;
    std::vector<double> tickSizes;
    std::vector<double> qtyIncrements;
    std::vector<int> scales;
};

// Latency harness eventually

}  // namespace hft
