#pragma once
//
// Adapted Simple HLS Client - HLS Tag Parser for Tardsplaya
// Original from https://github.com/bytems/simple_hls_client
//

#ifndef SIMPLE_HLS_CLIENT_HLS_TAG_PARSER_H
#define SIMPLE_HLS_CLIENT_HLS_TAG_PARSER_H

#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <sstream>
#include <regex>

/**
 * @brief Abstract base class for parsing HLS tags.
 *
 * This class defines the common interface that all HLS tag parsers must implement.
 * It declares methods for parsing content and sorting parsed elements based on various
 * attributes. Derived classes are expected to provide concrete implementations.
 */
class HLSTagParser {
public:
    /**
     * @brief Attributes available for sorting HLS tags.
     *
     * Each enumerator represents a specific attribute that may be used to sort
     * tags extracted from an HLS playlist.
     */
    enum class SortAttribute {
        BANDWIDTH, AVERAGE_BANDWIDTH, CODECS, RESOLUTION, FRAME_RATE, VIDEO_RANGE,
        AUDIO, CLOSED_CAPTIONS, TYPE,
        ID, NAME, LANGUAGE, DEFAULT_, AUTOSELECT, CHANNELS
    };

    virtual ~HLSTagParser() = default;
    virtual void parse(const std::string& content) = 0;
    virtual void sortByAttribute(SortAttribute attr) = 0;
    virtual void sortByAttribute(SortAttribute attr1, SortAttribute attr2) = 0;

protected:
    /**
     * @brief Extracts the value of an attribute from a given line.
     *
     * This static helper function uses a regular expression to locate and extract an attribute's
     * value from the provided line. It supports both quoted and unquoted values.
     *
     * @param line The line from which to extract the attribute.
     * @param attr The attribute key to search for.
     * @return The extracted attribute value, or an empty string if the attribute is not found.
     */
    static std::string extractAttribute(const std::string& line, const std::string& attr) {
        std::regex pattern(attr + "=(?:\"([^\"]*)\"|([^,\\s]*))");
        std::smatch matches;
        if (std::regex_search(line, matches, pattern)) {
            return matches[1].str().empty() ? matches[2].str() : matches[1].str();
        }
        return "";
    }
};

/**
 * @brief CRTP-based helper class template for sorting HLS tags.
 *
 * This template uses the Curiously Recurring Template Pattern (CRTP) to provide a generic
 * implementation of sorting methods based on one or two attributes. The derived class must
 * provide:
 *   - A getContainer() method returning a container (e.g., std::vector) of elements.
 *   - A getComparisons() method returning a mapping from SortAttribute to a comparator.
 *
 * @tparam Derived  The derived class that implements the specific HLS tag parser.
 * @tparam Element  The type of element contained in the parser's container.
 */
template<typename Derived, typename Element>
class HLSTagParserSorter : public HLSTagParser {
public:
    /**
     * Retrieves the container and the associated comparator for the specified attribute,
     * then applies std::sort to arrange the elements.
     */
    void sortByAttribute(SortAttribute attr) override {
        // Cast 'this' to the derived class to access its specific methods
        Derived* derived = static_cast<Derived*>(this);
        auto& container = derived->getContainer();
        auto& comparisons = derived->getComparisons();
        // Use the comparison functor corresponding to attr
        auto it = comparisons.find(attr);
        if (it != comparisons.end()) {
            std::sort(container.begin(), container.end(), it->second);
        }
    }

    /**
     * First, the primary comparator is used. If two elements are deemed equal (i.e., neither is
     * strictly less than the other with respect to the primary attribute), the secondary comparator
     * is then used to determine the order.
     */
    void sortByAttribute(SortAttribute primary, SortAttribute secondary) override {
        Derived* derived = static_cast<Derived*>(this);
        auto& container = derived->getContainer();
        auto& comparisons = derived->getComparisons();

        // Retrieve comparators for the primary and secondary attributes.
        auto primary_it = comparisons.find(primary);
        auto secondary_it = comparisons.find(secondary);
        
        if (primary_it == comparisons.end() || secondary_it == comparisons.end()) {
            return; // Invalid attributes
        }

        auto primary_comp = primary_it->second;
        auto secondary_comp = secondary_it->second;

        // Sort using a lambda that applies the primary comparator and falls back to the secondary if needed.
        std::sort(container.begin(), container.end(),
            [primary_comp, secondary_comp](const Element& a, const Element& b) {
                // Get comparison result for primary attribute
                bool primary_a_less_b = primary_comp(a, b);
                bool primary_b_less_a = primary_comp(b, a);

                // if primary attributes doesn't determine order, use secondary attribute
                if (!primary_a_less_b && !primary_b_less_a) {
                    return secondary_comp(a, b);
                }

                // Otherwise, use primary attribute's ordering
                return primary_a_less_b;
            }
        );
    }
};

#endif // SIMPLE_HLS_CLIENT_HLS_TAG_PARSER_H