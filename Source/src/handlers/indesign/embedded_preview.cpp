#include "vove/handlers/indesign/embedded_preview.hpp"

#include "vove/handlers/archive/zip_reader.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vove::handlers::indesign {
namespace {

using Status = cdr::CdrPreviewStatus;
using Result = cdr::CdrPreviewResult;

// Layout references: ExifTool InDesign.pm and Adobe XMP Part 3, InDesign files.
constexpr std::string_view kMasterGuid = "\x06\x06\xed\xf5\xd8\x1d\x46\xe5"
                                         "\xbd\x31\xef\xe7\xfe\x74\xb7\x1d";
constexpr std::string_view kHeaderGuid = "\xde\x39\x39\x79\x51\x88\x4b\x6c"
                                         "\x8e\x63\xee\xf8\xae\xe0\xdd\x38";
constexpr std::string_view kTrailerGuid = "\xfd\xce\xdb\x70\xf7\x86\x4b\x4f"
                                          "\xa4\xd3\xc7\x28\xb3\x41\x71\x06";
constexpr auto kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr auto kXmp = "http://ns.adobe.com/xap/1.0/";
constexpr auto kImage = "http://ns.adobe.com/xap/1.0/g/img/";
constexpr std::size_t kMaximumXmlDepth = 64;
constexpr std::uint32_t kMaximumXmlTokens = 262'144;
constexpr std::size_t kMaximumXmlAttributes = 64;
constexpr std::uint32_t kMaximumNamespaceWork = 4'194'304;
constexpr auto kXml = "http://www.w3.org/XML/1998/namespace";
constexpr auto kXmlns = "http://www.w3.org/2000/xmlns/";
constexpr std::string_view kIdmlMime = "application/vnd.adobe.indesign-idml-package";
constexpr std::string_view kIdmlMetadata = "META-INF/metadata.xml";

[[nodiscard]] Result result(const Status status, std::string diagnostic) {
    return {.status = status,
            .container = cdr::CdrContainer::unknown,
            .candidates = {},
            .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] cdr::CdrPreviewLimits clamped_limits(const cdr::CdrPreviewLimits &limits) {
    return {.maximum_central_entries =
                std::min(limits.maximum_central_entries, cdr::kMaximumCdrCentralEntries),
            .maximum_central_directory_bytes = std::min(limits.maximum_central_directory_bytes,
                                                        cdr::kMaximumCdrCentralDirectoryBytes),
            .maximum_compressed_preview_bytes = std::min(limits.maximum_compressed_preview_bytes,
                                                         cdr::kMaximumCdrCompressedPreviewBytes),
            .maximum_total_preview_bytes =
                std::min(limits.maximum_total_preview_bytes, cdr::kMaximumCdrPreviewBytes)};
}

[[nodiscard]] Status zip_status(const archive::ZipStatus status) noexcept {
    switch (status) {
    case archive::ZipStatus::success:
        return Status::success;
    case archive::ZipStatus::malformed:
        return Status::malformed;
    case archive::ZipStatus::resource_limit:
        return Status::resource_limit;
    case archive::ZipStatus::io_error:
        return Status::io_error;
    case archive::ZipStatus::unsupported:
        return Status::unsupported_container;
    }
    return Status::malformed;
}

[[nodiscard]] std::uint64_t integer(const std::byte *bytes, const unsigned count,
                                    const bool big_endian = false) noexcept {
    std::uint64_t value{};
    for (unsigned index = 0; index < count; ++index) {
        const auto shift = (big_endian ? count - index - 1U : index) * 8U;
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[index])} << shift;
    }
    return value;
}

[[nodiscard]] bool guid(const std::byte *bytes, const std::string_view expected) noexcept {
    return std::memcmp(bytes, expected.data(), expected.size()) == 0;
}

struct Budget {
    const raster::NativeSource &source;
    const cdr::CdrPreviewLimits limits;
    std::uint64_t read_bytes{};
    std::uint32_t xml_tokens{};
    std::uint32_t previews{};
    std::uint32_t namespace_work{};
    Status status{Status::malformed};
    std::string diagnostic;

    bool fail(const Status code, std::string message) {
        status = code;
        diagnostic = std::move(message);
        return false;
    }

    bool read(const std::uint64_t offset, const std::span<std::byte> output) {
        if (offset > source.size() || output.size() > source.size() - offset) {
            return fail(Status::malformed,
                        "Truncated InDesign structure at offset " + std::to_string(offset));
        }
        if (output.size() > limits.maximum_central_directory_bytes - read_bytes) {
            return fail(Status::resource_limit, "InDesign aggregate metadata read budget exceeded");
        }
        read_bytes += output.size();
        const auto read_result = source.read_at(offset, output);
        if (!read_result.ok() || read_result.bytes_read != output.size()) {
            return fail(Status::io_error,
                        "InDesign read_at failed at offset " + std::to_string(offset));
        }
        return true;
    }

    [[nodiscard]] Result failure() const {
        return result(status, diagnostic);
    }
};

struct Preview {
    bool property_seen{};
    bool array_seen{};
    bool item_seen{};
    bool image_seen{};
    bool format_seen{};
    std::string base64;
    std::string format;
};

enum class Kind { other, meta, rdf, document, property, array, item, record, image, format };
struct Frame {
    Kind kind{Kind::other};
    Preview *preview{};
};

[[nodiscard]] bool xml_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] int base64_digit(const char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    return value == '+' ? 62 : value == '/' ? 63 : -1;
}

bool append_field(Budget &budget, Preview &preview, const Kind kind, const std::string_view text) {
    if (kind == Kind::format) {
        if (text.size() > 32 - preview.format.size()) {
            return budget.fail(Status::malformed, "InDesign preview format field is too long");
        }
        preview.format.append(text);
        return true;
    }
    const auto image_limit = std::min(budget.limits.maximum_compressed_preview_bytes,
                                      budget.limits.maximum_total_preview_bytes);
    const auto encoded_limit = ((image_limit + 2U) / 3U) * 4U;
    for (const char value : text) {
        if (xml_space(value)) {
            continue;
        }
        if (base64_digit(value) < 0 && value != '=') {
            return budget.fail(Status::malformed, "Invalid character in InDesign preview base64");
        }
        if (static_cast<std::uint64_t>(preview.base64.size()) >= encoded_limit) {
            return budget.fail(Status::resource_limit,
                               "InDesign preview base64 exceeds byte limit");
        }
        preview.base64.push_back(value);
    }
    return true;
}

bool begin_field(Budget &budget, Preview &preview, const Kind kind) {
    auto &seen = kind == Kind::image ? preview.image_seen : preview.format_seen;
    if (seen) {
        return budget.fail(Status::malformed, "Duplicate field in first InDesign preview");
    }
    seen = true;
    return true;
}

[[nodiscard]] Result decode_preview(Budget &budget, const Preview &preview, const bool page_info) {
    if (!preview.image_seen || preview.base64.empty()) {
        return result(Status::no_preview, "First stored InDesign preview has no embedded image");
    }
    std::string_view format = preview.format;
    while (!format.empty() && xml_space(format.front())) {
        format.remove_prefix(1);
    }
    while (!format.empty() && xml_space(format.back())) {
        format.remove_suffix(1);
    }
    if (format != "JPEG") {
        return result(Status::no_preview, "First stored InDesign preview is not declared JPEG");
    }

    const auto &encoded = preview.base64;
    if ((encoded.size() % 4) != 0) {
        return result(Status::malformed, "Incomplete InDesign preview base64 quartet");
    }
    const std::size_t padding = encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1 : 0;
    const auto data_size = encoded.size() - padding;
    for (std::size_t index = 0; index < data_size; ++index) {
        if (encoded[index] == '=') {
            return result(Status::malformed, "Misplaced InDesign preview base64 padding");
        }
    }
    // append_field checked the alphabet; also require canonical zero padding bits.
    const auto last_digit = base64_digit(encoded[data_size - 1]);
    if ((padding == 2 && (last_digit & 15) != 0) || (padding == 1 && (last_digit & 3) != 0)) {
        return result(Status::malformed, "Nonzero InDesign preview base64 padding bits");
    }
    const auto decoded_size = static_cast<std::uint64_t>(encoded.size() / 4 * 3 - padding);
    if (decoded_size > budget.limits.maximum_compressed_preview_bytes ||
        decoded_size > budget.limits.maximum_total_preview_bytes) {
        return result(Status::resource_limit, "InDesign JPEG byte limit exceeded");
    }
    std::vector<std::byte> jpeg(static_cast<std::size_t>(decoded_size));
    std::size_t destination{};
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        std::uint32_t bits{};
        for (std::size_t digit = 0; digit < 4; ++digit) {
            const auto value = encoded[index + digit];
            bits = (bits << 6U) | (value == '=' ? 0U : static_cast<unsigned>(base64_digit(value)));
        }
        for (unsigned byte = 0; byte < 3 && destination < jpeg.size(); ++byte) {
            jpeg[destination++] = static_cast<std::byte>((bits >> (16U - byte * 8U)) & 0xffU);
        }
    }
    if (jpeg.size() < 4 || jpeg[0] != std::byte{0xff} || jpeg[1] != std::byte{0xd8} ||
        jpeg[2] != std::byte{0xff} || jpeg[jpeg.size() - 2] != std::byte{0xff} ||
        jpeg.back() != std::byte{0xd9}) {
        return result(Status::malformed, "Declared InDesign JPEG has invalid boundary markers");
    }
    auto output = result(
        Status::success,
        page_info
            ? "First stored PageInfo JPEG; original resolution, possibly a spread; page count "
              "unknown"
            : "First stored document thumbnail JPEG; original resolution; page count unknown");
    output.candidates.push_back(
        {.format = raster::RasterFormat::Jpeg,
         .normalized_name = page_info ? "xmp/pageinfo/first.jpg" : "xmp/thumbnails/first.jpg",
         .encoded_image = std::move(jpeg),
         .page_one = true});
    return output;
}

struct ExpandedName {
    std::string_view uri;
    std::string_view local;

    [[nodiscard]] bool is(const std::string_view expected_uri,
                          const std::string_view expected_name) const noexcept {
        return uri == expected_uri && local == expected_name;
    }
    friend bool operator==(const ExpandedName &, const ExpandedName &) = default;
};

struct ExpandedAttribute {
    ExpandedName name;
    std::string_view value;
};

bool namespace_step(Budget &budget) {
    if (++budget.namespace_work > kMaximumNamespaceWork) {
        return budget.fail(Status::resource_limit, "InDesign XMP namespace work limit exceeded");
    }
    return true;
}

bool split_name(Budget &budget, const std::string_view qualified, std::string_view &prefix,
                std::string_view &local) {
    const auto colon = qualified.find(':');
    if (colon == std::string_view::npos) {
        prefix = {};
        local = qualified;
    } else {
        prefix = qualified.substr(0, colon);
        local = qualified.substr(colon + 1);
        if (prefix.empty() || local.empty() || local.find(':') != std::string_view::npos) {
            return budget.fail(Status::malformed, "Invalid qualified name in InDesign XMP");
        }
    }
    return true;
}

bool expand_name(Budget &budget, pugi::xml_node context, const std::string_view qualified,
                 const bool attribute, ExpandedName &expanded) {
    std::string_view prefix;
    if (!split_name(budget, qualified, prefix, expanded.local)) {
        return false;
    }
    if (prefix == "xml") {
        expanded.uri = kXml;
        return true;
    }
    // The default namespace applies to elements, never to unprefixed attributes.
    if (attribute && prefix.empty()) {
        expanded.uri = {};
        return true;
    }
    for (; context.type() == pugi::node_element; context = context.parent()) {
        if (!namespace_step(budget)) {
            return false;
        }
        for (const auto binding : context.attributes()) {
            if (!namespace_step(budget)) {
                return false;
            }
            const std::string_view name = binding.name();
            if ((prefix.empty() && name == "xmlns") ||
                (!prefix.empty() && name.starts_with("xmlns:") && name.substr(6) == prefix)) {
                expanded.uri = binding.value();
                if (!prefix.empty() && expanded.uri.empty()) {
                    return budget.fail(Status::malformed,
                                       "Empty prefixed namespace in InDesign XMP");
                }
                return true;
            }
        }
    }
    expanded.uri = {};
    return prefix.empty() ||
           budget.fail(Status::malformed, "Unbound namespace prefix in InDesign XMP");
}

bool inspect_names(Budget &budget, const pugi::xml_node node, ExpandedName &name,
                   std::array<ExpandedAttribute, kMaximumXmlAttributes> &attributes,
                   std::size_t &count) {
    std::array<std::string_view, kMaximumXmlAttributes * 2> raw_names{};
    std::size_t raw_count{};
    std::size_t declarations{};
    // pugixml preserves QNames; validate declarations before resolving any name.
    for (const auto attribute : node.attributes()) {
        if (raw_count == raw_names.size()) {
            return budget.fail(Status::resource_limit, "InDesign XMP attribute limit exceeded");
        }
        const std::string_view raw = attribute.name();
        for (std::size_t index = 0; index < raw_count; ++index) {
            if (!namespace_step(budget)) {
                return false;
            }
            if (raw == raw_names[index]) {
                return budget.fail(Status::malformed, "Duplicate InDesign XMP attribute");
            }
        }
        raw_names[raw_count++] = raw;
        if (raw == "xmlns" || raw.starts_with("xmlns:")) {
            if (++declarations > kMaximumXmlAttributes) {
                return budget.fail(Status::resource_limit, "InDesign XMP namespace limit exceeded");
            }
            const auto prefix = raw == "xmlns" ? std::string_view{} : raw.substr(6);
            const std::string_view uri = attribute.value();
            if ((raw != "xmlns" &&
                 (prefix.empty() || prefix.find(':') != std::string_view::npos || uri.empty())) ||
                prefix == "xmlns" || uri == kXmlns ||
                (prefix == "xml" ? uri != kXml : uri == kXml)) {
                return budget.fail(Status::malformed, "Invalid InDesign XMP namespace binding");
            }
        }
    }
    if (!expand_name(budget, node, node.name(), false, name)) {
        return false;
    }
    for (const auto attribute : node.attributes()) {
        const std::string_view raw = attribute.name();
        if (raw == "xmlns" || raw.starts_with("xmlns:")) {
            continue;
        }
        if (count == attributes.size()) {
            return budget.fail(Status::resource_limit, "InDesign XMP attribute limit exceeded");
        }
        auto &expanded = attributes[count];
        if (!expand_name(budget, node, raw, true, expanded.name)) {
            return false;
        }
        expanded.value = attribute.value();
        for (std::size_t index = 0; index < count; ++index) {
            if (!namespace_step(budget)) {
                return false;
            }
            if (attributes[index].name == expanded.name) {
                return budget.fail(Status::malformed,
                                   "Duplicate expanded attribute in InDesign XMP");
            }
        }
        ++count;
    }
    return true;
}

[[nodiscard]] const ExpandedAttribute *
find_attribute(const std::span<const ExpandedAttribute> attributes, const std::string_view uri,
               const std::string_view local) {
    for (const auto &attribute : attributes) {
        if (attribute.name.is(uri, local)) {
            return &attribute;
        }
    }
    return nullptr;
}

bool read_attributes(Budget &budget, Preview &preview,
                     const std::span<const ExpandedAttribute> attributes) {
    for (const auto &attribute : attributes) {
        const auto kind = attribute.name.is(kImage, "image")    ? Kind::image
                          : attribute.name.is(kImage, "format") ? Kind::format
                                                                : Kind::other;
        if (kind != Kind::other && (!begin_field(budget, preview, kind) ||
                                    !append_field(budget, preview, kind, attribute.value))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result parse_xmp(Budget &budget, std::vector<char> &packet) {
    if (std::find(packet.begin(), packet.end(), '\0') != packet.end()) {
        return result(Status::malformed, "NUL byte in InDesign XMP");
    }
    // In-place DOM storage is bounded by the <=8 MiB input. No global allocator
    // hooks, DTD expansion, external entity loading, XPath, or recursive traversal.
    pugi::xml_document document;
    const auto parsed =
        document.load_buffer_inplace(packet.data(), packet.size(),
                                     pugi::parse_full | pugi::parse_ws_pcdata, pugi::encoding_utf8);
    if (!parsed) {
        return result(parsed.status == pugi::status_out_of_memory ? Status::resource_limit
                                                                  : Status::malformed,
                      "InDesign XMP parse error at byte " + std::to_string(parsed.offset) + ": " +
                          parsed.description());
    }
    std::array<Frame, kMaximumXmlDepth> stack{};
    std::size_t depth{};
    std::size_t roots{};
    Preview pages;
    Preview thumbnails;
    bool found_rdf{};
    auto node = document.first_child();
    while (node) {
        const auto cost = node.type() == pugi::node_element ? 2U : 1U;
        if (cost > kMaximumXmlTokens - budget.xml_tokens) {
            return result(Status::resource_limit, "InDesign XMP token limit exceeded");
        }
        budget.xml_tokens += cost;
        if (node.type() == pugi::node_doctype) {
            return result(Status::malformed, "DTD/entities are not supported in InDesign XMP");
        }
        if (node.type() == pugi::node_element) {
            if (depth == stack.size()) {
                return result(Status::resource_limit, "InDesign XMP depth limit exceeded");
            }
            if (depth == 0 && ++roots > 1) {
                return result(Status::malformed, "Multiple InDesign XMP document roots");
            }
            ExpandedName name;
            std::array<ExpandedAttribute, kMaximumXmlAttributes> attribute_storage{};
            std::size_t attribute_count{};
            if (!inspect_names(budget, node, name, attribute_storage, attribute_count)) {
                return budget.failure();
            }
            const std::span<const ExpandedAttribute> attributes(attribute_storage.data(),
                                                                attribute_count);
            const auto *about = find_attribute(attributes, kRdf, "about");
            const Frame parent = depth ? stack[depth - 1] : Frame{};
            Frame frame;
            if (depth == 0 && name.is("adobe:ns:meta/", "xmpmeta")) {
                frame.kind = Kind::meta;
            } else if ((depth == 0 || parent.kind == Kind::meta) && name.is(kRdf, "RDF")) {
                frame.kind = Kind::rdf;
                found_rdf = true;
            } else if (parent.kind == Kind::rdf && name.is(kRdf, "Description") &&
                       (!about || about->value.empty()) &&
                       !find_attribute(attributes, kRdf, "nodeID")) {
                frame.kind = Kind::document;
            } else if (parent.kind == Kind::document &&
                       (name.is(kXmp, "PageInfo") || name.is(kXmp, "Thumbnails"))) {
                auto *preview = name.is(kXmp, "PageInfo") ? &pages : &thumbnails;
                if (preview->property_seen) {
                    return result(Status::malformed,
                                  "Duplicate InDesign document preview property");
                }
                preview->property_seen = true;
                frame = {Kind::property, preview};
            } else if (parent.kind == Kind::property) {
                if (!name.is(kRdf, parent.preview == &pages ? "Seq" : "Alt") ||
                    parent.preview->array_seen) {
                    return result(Status::malformed, "Invalid InDesign document preview array");
                }
                parent.preview->array_seen = true;
                frame = {Kind::array, parent.preview};
            } else if (parent.kind == Kind::array) {
                if (!name.is(kRdf, "li")) {
                    return result(Status::malformed, "Invalid InDesign preview array item");
                }
                if (++budget.previews > budget.limits.maximum_central_entries) {
                    return result(Status::resource_limit,
                                  "InDesign saved-preview count limit exceeded");
                }
                if (!parent.preview->item_seen) {
                    parent.preview->item_seen = true;
                    frame = {Kind::item, parent.preview};
                }
            } else if (parent.kind == Kind::item && name.is(kRdf, "Description")) {
                frame = {Kind::record, parent.preview};
            } else if ((parent.kind == Kind::item || parent.kind == Kind::record) &&
                       (name.is(kImage, "image") || name.is(kImage, "format"))) {
                frame = {name.is(kImage, "image") ? Kind::image : Kind::format, parent.preview};
                if (!begin_field(budget, *frame.preview, frame.kind)) {
                    return budget.failure();
                }
            } else if (parent.kind == Kind::image || parent.kind == Kind::format) {
                return result(Status::malformed, "Nested element in InDesign preview scalar field");
            }
            if ((frame.kind == Kind::item || frame.kind == Kind::record) &&
                !read_attributes(budget, *frame.preview, attributes)) {
                return budget.failure();
            }
            stack[depth] = frame;
        } else if ((node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) && depth) {
            const auto frame = stack[depth - 1];
            if ((frame.kind == Kind::image || frame.kind == Kind::format) &&
                !append_field(budget, *frame.preview, frame.kind, node.value())) {
                return budget.failure();
            }
        }
        if (node.first_child()) {
            node = node.first_child();
            ++depth;
            continue;
        }
        while (node && !node.next_sibling()) {
            node = node.parent();
            if (node == document) {
                node = {};
                break;
            }
            --depth;
        }
        if (node) {
            node = node.next_sibling();
        }
    }
    if (!found_rdf) {
        return result(Status::malformed, "InDesign XMP has no document RDF root");
    }
    if (!pages.property_seen && !thumbnails.property_seen) {
        return result(Status::no_preview, "InDesign XMP has no embedded document preview");
    }
    return decode_preview(budget, pages.property_seen ? pages : thumbnails, pages.property_seen);
}

[[nodiscard]] Result extract(Budget &budget) {
    std::array<std::byte, 8192> masters{};
    if (budget.source.size() < 16) {
        return result(Status::malformed, "Truncated InDesign signature");
    }
    if (!budget.read(0, std::span(masters).first(16))) {
        return budget.failure();
    }
    if (!guid(masters.data(), kMasterGuid)) {
        return result(Status::unsupported_container, "Not an InDesign database");
    }
    if (!budget.read(16, std::span(masters).subspan(16))) {
        return budget.failure();
    }
    if (!guid(masters.data() + 4096, kMasterGuid)) {
        return result(Status::malformed, "Invalid second InDesign master page GUID");
    }
    // Master/header integers stay LE even when stream integers use BE.
    const auto *master =
        masters.data() +
        (integer(masters.data() + 4096 + 264, 8) > integer(masters.data() + 264, 8) ? 4096 : 0);
    const auto byte_order = std::to_integer<unsigned char>(master[24]);
    if (byte_order != 1 && byte_order != 2) {
        return result(Status::malformed, "Invalid InDesign stream byte order");
    }
    const auto database_pages = integer(master + 280, 4);
    if (database_pages < 2) {
        return result(Status::malformed, "Invalid InDesign database block count");
    }
    std::uint64_t offset = database_pages * 4096ULL;
    if (offset > budget.source.size()) {
        return result(Status::malformed, "InDesign object region is outside the source");
    }
    for (std::uint32_t objects = 0; offset < budget.source.size(); ++objects) {
        if (objects >= budget.limits.maximum_central_entries) {
            return result(Status::resource_limit,
                          "InDesign contiguous-object count limit exceeded");
        }
        std::array<std::byte, 32> header{};
        const auto available =
            std::min<std::uint64_t>(header.size(), budget.source.size() - offset);
        if (!budget.read(offset, std::span(header).first(static_cast<std::size_t>(available)))) {
            return budget.failure();
        }
        if (std::all_of(header.begin(), header.end(),
                        [](const auto b) { return b == std::byte{}; })) {
            return result(Status::no_preview, "No InDesign document preview before object padding");
        }
        if (available != header.size() || !guid(header.data(), kHeaderGuid)) {
            return result(Status::malformed,
                          "Invalid InDesign contiguous-object header at offset " +
                              std::to_string(offset));
        }
        const auto length = integer(header.data() + 24, 4);
        const auto payload_offset = offset + header.size();
        if (length > budget.source.size() - payload_offset ||
            budget.source.size() - payload_offset - length < 32) {
            return result(Status::malformed, "Truncated InDesign contiguous object/trailer");
        }
        std::array<std::byte, 32> trailer{};
        if (!budget.read(payload_offset + length, trailer)) {
            return budget.failure();
        }
        if (!guid(trailer.data(), kTrailerGuid) ||
            !std::equal(header.begin() + 16, header.begin() + 28, trailer.begin() + 16)) {
            return result(Status::malformed, "Invalid or nonmatching InDesign object trailer");
        }
        // Inspect only the defined stream start, never search database/image bytes.
        std::array<std::byte, 16> prefix{};
        if (length >= prefix.size()) {
            if (!budget.read(payload_offset, prefix)) {
                return budget.failure();
            }
            const std::string_view start(reinterpret_cast<const char *>(prefix.data() + 4), 12);
            if (start.starts_with("<?xpacket ")) {
                const auto xmp_length = integer(prefix.data(), 4, byte_order == 2);
                if (xmp_length > length - 4 || xmp_length < 12) {
                    return result(Status::malformed, "Invalid/truncated InDesign XMP length word");
                }
                if (xmp_length > budget.limits.maximum_compressed_preview_bytes ||
                    xmp_length >
                        budget.limits.maximum_central_directory_bytes - budget.read_bytes) {
                    return result(Status::resource_limit,
                                  "InDesign XMP packet/read byte limit exceeded");
                }
                std::vector<char> packet(static_cast<std::size_t>(xmp_length));
                if (!budget.read(payload_offset + 4, {reinterpret_cast<std::byte *>(packet.data()),
                                                      static_cast<std::size_t>(packet.size())})) {
                    return budget.failure();
                }
                // The first document XMP packet is authoritative, including absence.
                return parse_xmp(budget, packet);
            }
        }
        offset = payload_offset + length + trailer.size();
    }
    return result(Status::no_preview, "No InDesign document XMP preview stream");
}

[[nodiscard]] Result extract_idml(Budget &budget) {
    std::array<std::byte, 4> prefix{};
    if (budget.source.size() < prefix.size()) {
        return result(Status::unsupported_container, "IDML package signature is absent");
    }
    const auto signature = budget.source.read_at(0, prefix);
    constexpr std::array zip_local{std::byte{'P'}, std::byte{'K'}, std::byte{3}, std::byte{4}};
    if (!signature.ok() || signature.bytes_read != prefix.size()) {
        return result(Status::io_error, "IDML package signature could not be read");
    }
    if (prefix != zip_local) {
        return result(Status::unsupported_container, "Document is not an IDML ZIP package");
    }

    archive::ZipDirectory directory;
    std::string diagnostic;
    const auto directory_status = archive::read_zip_directory(
        budget.source,
        {budget.limits.maximum_central_entries, budget.limits.maximum_central_directory_bytes},
        directory, diagnostic);
    if (directory_status != archive::ZipStatus::success) {
        return result(zip_status(directory_status), std::move(diagnostic));
    }

    bool duplicate{};
    const auto find_unique = [&](const std::string_view name) -> const archive::ZipEntry * {
        const archive::ZipEntry *found{};
        for (const auto &entry : directory.entries) {
            if (entry.name != name) {
                continue;
            }
            duplicate = duplicate || found != nullptr;
            found = &entry;
        }
        return found;
    };
    const auto *mime = find_unique("mimetype");
    const auto *metadata = find_unique(kIdmlMetadata);
    if (duplicate) {
        return result(Status::malformed, "IDML package has duplicate identity or metadata entries");
    }
    if (mime == nullptr) {
        return result(Status::unsupported_container, "IDML package mimetype is absent");
    }

    const auto extract_entry = [&](const archive::ZipEntry &entry,
                                   const archive::ZipEntryLimits &limits,
                                   std::vector<std::byte> &output) {
        bool payload_failure{};
        archive::ZipEntryRange range;
        return archive::extract_zip_entry(budget.source, directory, entry, limits, output,
                                          diagnostic, payload_failure, range);
    };
    std::vector<std::byte> mime_bytes;
    auto status = extract_entry(*mime, {256, 256}, mime_bytes);
    if (status != archive::ZipStatus::success) {
        return result(zip_status(status), std::move(diagnostic));
    }
    if (mime_bytes.size() != kIdmlMime.size() ||
        !std::equal(mime_bytes.begin(), mime_bytes.end(),
                    reinterpret_cast<const std::byte *>(kIdmlMime.data()))) {
        return result(Status::unsupported_container, "ZIP mimetype is not Adobe IDML");
    }
    if (metadata == nullptr) {
        return result(Status::no_preview, "IDML package has no metadata.xml preview source");
    }

    std::vector<std::byte> metadata_bytes;
    const archive::ZipEntryLimits metadata_limits{
        std::min(budget.limits.maximum_compressed_preview_bytes,
                 budget.limits.maximum_central_directory_bytes),
        budget.limits.maximum_central_directory_bytes};
    status = extract_entry(*metadata, metadata_limits, metadata_bytes);
    if (status != archive::ZipStatus::success) {
        return result(zip_status(status), std::move(diagnostic));
    }
    if (metadata_bytes.empty()) {
        return result(Status::no_preview, "IDML metadata.xml is empty");
    }
    std::vector<char> packet(metadata_bytes.size());
    std::memcpy(packet.data(), metadata_bytes.data(), metadata_bytes.size());
    auto parsed = parse_xmp(budget, packet);
    if (parsed.ok()) {
        for (auto &candidate : parsed.candidates) {
            candidate.normalized_name = "idml/metadata/" + candidate.normalized_name;
        }
        parsed.diagnostic = "IDML " + parsed.diagnostic;
    }
    return parsed;
}

} // namespace

cdr::CdrPreviewResult extract_indesign_embedded_previews(const raster::NativeSource &source,
                                                         const cdr::CdrPreviewLimits &limits) {
    try {
        Budget budget{.source = source, .limits = clamped_limits(limits), .diagnostic = {}};
        return extract(budget);
    } catch (const std::bad_alloc &) {
        return result(Status::resource_limit, "InDesign preview allocation failed");
    } catch (const std::length_error &) {
        return result(Status::resource_limit, "InDesign preview allocation length exceeded");
    }
}

cdr::CdrPreviewResult extract_idml_embedded_previews(const raster::NativeSource &source,
                                                     const cdr::CdrPreviewLimits &limits) {
    try {
        Budget budget{.source = source, .limits = clamped_limits(limits), .diagnostic = {}};
        return extract_idml(budget);
    } catch (const std::bad_alloc &) {
        return result(Status::resource_limit, "IDML preview allocation failed");
    } catch (const std::length_error &) {
        return result(Status::resource_limit, "IDML preview allocation length exceeded");
    }
}

} // namespace vove::handlers::indesign
