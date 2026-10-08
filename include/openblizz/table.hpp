#pragma once

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace openblizz {

// Minimal column-aligned text table for CLI output. Width is measured in
// Unicode code points so names with ® or accents do not break alignment.
class Table {
public:
    explicit Table(std::vector<std::string> header) : header_(std::move(header)) {}

    void add(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
    [[nodiscard]] bool empty() const { return rows_.empty(); }

    void print(std::ostream& out = std::cout) const {
        std::vector<std::size_t> widths(header_.size(), 0);
        measure(header_, widths);
        for (const auto& row : rows_) measure(row, widths);
        print_row(out, header_, widths);
        std::string rule;
        for (std::size_t i = 0; i < widths.size(); ++i) {
            if (i > 0) rule += "  ";
            rule += std::string(widths[i], '-');
        }
        out << rule << '\n';
        for (const auto& row : rows_) print_row(out, row, widths);
    }

    static std::size_t display_width(const std::string& text) {
        std::size_t count = 0;
        for (const unsigned char ch : text) {
            if ((ch & 0xC0) != 0x80) ++count;   // count UTF-8 lead bytes only
        }
        return count;
    }

private:
    static void measure(const std::vector<std::string>& row, std::vector<std::size_t>& widths) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], display_width(row[i]));
        }
    }
    static void print_row(std::ostream& out, const std::vector<std::string>& row,
                          const std::vector<std::size_t>& widths) {
        for (std::size_t i = 0; i < widths.size(); ++i) {
            const auto cell = i < row.size() ? row[i] : std::string{};
            if (i > 0) out << "  ";
            out << cell;
            if (i + 1 < widths.size()) out << std::string(widths[i] - display_width(cell), ' ');
        }
        out << '\n';
    }

    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> rows_;
};

// Display order of franchises: the project's focus first, then the rest alphabetically.
inline int family_rank(const std::string& family) {
    if (family == "warcraft") return 0;
    if (family == "starcraft") return 1;
    if (family == "diablo") return 2;
    if (family == "arcade") return 3;
    if (family.empty()) return 99;
    return 10;
}
inline bool family_before(const std::string& a, const std::string& b) {
    const int ra = family_rank(a), rb = family_rank(b);
    return ra != rb ? ra < rb : a < b;
}

inline std::string family_label(const std::string& family) {
    if (family == "warcraft") return "Warcraft";
    if (family == "starcraft") return "StarCraft";
    if (family == "diablo") return "Diablo";
    if (family == "arcade") return "Blizzard Arcade";
    if (family == "overwatch") return "Overwatch";
    if (family == "hearthstone") return "Hearthstone";
    if (family == "heroes") return "Heroes of the Storm";
    if (family == "callofduty") return "Call of Duty";
    if (family == "ngdp" || family.empty()) return "Other Battle.net products";
    std::string label = family;
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}

} // namespace openblizz
