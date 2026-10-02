#include "openblizz/formats.hpp"
#include "openblizz/hash.hpp"
#include "openblizz/library.hpp"
#include "openblizz/tvfs.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void put_u32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 24));
    data.push_back(static_cast<std::uint8_t>(value >> 16));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void put_u32le_at(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::uint8_t>(value);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    data[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void put_u16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void put_u40(std::vector<std::uint8_t>& data, std::uint64_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 32));
    put_u32(data, static_cast<std::uint32_t>(value));
}

std::vector<std::uint8_t> single_chunk(const std::string& payload) {
    std::vector<std::uint8_t> encoded{'B', 'L', 'T', 'E', 0, 0, 0, 0, 'N'};
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

std::vector<std::uint8_t> multi_chunk(const std::string& payload) {
    std::vector<std::uint8_t> encoded{'B', 'L', 'T', 'E'};
    put_u32(encoded, 36); // 12-byte preamble + one 24-byte chunk record
    encoded.push_back(0x0f);
    encoded.push_back(0);
    encoded.push_back(0);
    encoded.push_back(1);
    put_u32(encoded, static_cast<std::uint32_t>(payload.size() + 1));
    put_u32(encoded, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> compressed{'N'};
    compressed.insert(compressed.end(), payload.begin(), payload.end());
    const auto digest = openblizz::md5_hex(compressed);
    for (std::size_t i = 0; i < digest.size(); i += 2) {
        encoded.push_back(static_cast<std::uint8_t>(std::stoul(digest.substr(i, 2), nullptr, 16)));
    }
    encoded.push_back('N');
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

// Builds a minimal TVFS manifest (9-byte EKeys, CKeys included) with the
// files "a.txt" (EKey 01..) and "dir/sub/b.bin" (EKey 02..). `first` seeds
// the EKeys so two manifests can be told apart.
std::vector<std::uint8_t> make_tvfs(std::uint8_t first) {
    std::vector<std::uint8_t> path;
    const auto fragment = [&](const std::string& name) {
        path.push_back(static_cast<std::uint8_t>(name.size()));
        path.insert(path.end(), name.begin(), name.end());
    };
    std::vector<std::uint8_t> folder;   // body of "dir"
    folder.push_back(0);                // separator before the first child
    fragment("sub");
    folder.insert(folder.end(), path.begin(), path.end());
    path.clear();
    folder.push_back(0);                // "sub/" then the file name fragment
    fragment("b.bin");
    folder.insert(folder.end(), path.begin(), path.end());
    path.clear();
    folder.push_back(0xff);
    put_u32(folder, 10);                // VFS table offset of the second entry

    std::vector<std::uint8_t> body;
    fragment("a.txt");
    body.insert(body.end(), path.begin(), path.end());
    path.clear();
    body.push_back(0xff);
    put_u32(body, 0);
    fragment("dir");
    body.insert(body.end(), path.begin(), path.end());
    path.clear();
    body.push_back(0xff);
    put_u32(body, 0x80000000u | static_cast<std::uint32_t>(folder.size() + 4));
    body.insert(body.end(), folder.begin(), folder.end());

    std::vector<std::uint8_t> path_table{0xff};
    put_u32(path_table, 0x80000000u | static_cast<std::uint32_t>(body.size() + 4));
    path_table.insert(path_table.end(), body.begin(), body.end());

    std::vector<std::uint8_t> cft;
    const auto container = [&](std::uint8_t seed, std::uint32_t encoded_size, std::uint32_t content_size) {
        for (std::uint8_t i = 0; i < 9; ++i) cft.push_back(static_cast<std::uint8_t>(seed + i));
        put_u32(cft, encoded_size);
        put_u32(cft, content_size);
        for (std::uint8_t i = 0; i < 16; ++i) cft.push_back(static_cast<std::uint8_t>(0xc0 + seed + i));
    };
    container(first, 7, 10);
    container(static_cast<std::uint8_t>(first + 1), 15, 20);

    std::vector<std::uint8_t> vfs;
    vfs.push_back(1);
    put_u32(vfs, 0);
    put_u32(vfs, 10);
    vfs.push_back(0);     // CFT offset of entry 0 (1 byte: the table is < 256 bytes)
    vfs.push_back(1);
    put_u32(vfs, 0);
    put_u32(vfs, 20);
    vfs.push_back(33);
    assert(vfs.size() == 20);

    std::vector<std::uint8_t> out{'T', 'V', 'F', 'S', 1, 38, 9, 0};
    put_u32(out, openblizz::kTvfsIncludeCKey);
    const std::uint32_t path_offset = 38;
    const auto vfs_offset = path_offset + static_cast<std::uint32_t>(path_table.size());
    const auto cft_offset = vfs_offset + static_cast<std::uint32_t>(vfs.size());
    put_u32(out, path_offset);
    put_u32(out, static_cast<std::uint32_t>(path_table.size()));
    put_u32(out, vfs_offset);
    put_u32(out, static_cast<std::uint32_t>(vfs.size()));
    put_u32(out, cft_offset);
    put_u32(out, static_cast<std::uint32_t>(cft.size()));
    put_u16(out, 3);
    out.insert(out.end(), path_table.begin(), path_table.end());
    out.insert(out.end(), vfs.begin(), vfs.end());
    out.insert(out.end(), cft.begin(), cft.end());
    return out;
}

} // namespace

int main() {
    assert(openblizz::md5_hex(std::string("")) == "d41d8cd98f00b204e9800998ecf8427e");
    assert(openblizz::sha256_hex(std::vector<std::uint8_t>{'a'}) ==
           "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");

    const auto plain = std::string("openblizz");
    assert(openblizz::BlteDecoder::decode(single_chunk(plain)) ==
           std::vector<std::uint8_t>(plain.begin(), plain.end()));
    assert(openblizz::BlteDecoder::decode(multi_chunk(plain)) ==
           std::vector<std::uint8_t>(plain.begin(), plain.end()));

    const auto config = openblizz::parse_config("root = aa bb\n# comment\nname = Warcraft\n");
    assert(config.get("root").size() == 2);
    assert(config.get("name").front() == "Warcraft");

    const std::vector<std::uint8_t> install{
        'I','N',1,16, 0,1, 0,0,0,1,
        'e','n','U','S',0, 0,1, 0x80,
        'a','.', 'b', 0,
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        0,0,0,4
    };
    const auto parsed = openblizz::parse_install_manifest(install);
    assert(parsed.entries.size() == 1);
    assert(parsed.entries.front().path == "a.b");
    assert(parsed.entries.front().file_size == 4);

    std::vector<std::uint8_t> encoding{'E','N',1,16,16};
    put_u16(encoding, 1);
    put_u16(encoding, 1);
    put_u32(encoding, 1);
    put_u32(encoding, 1);
    encoding.push_back(0); // flags
    put_u32(encoding, 0); // ESpec block size
    encoding.resize(22 + 32 + 1024 + 32 + 1024, 0);
    auto offset = static_cast<std::size_t>(22 + 32);
    encoding[offset++] = 2; // two equivalent EKeys
    put_u40(encoding, 4);
    for (std::uint8_t i = 0; i < 16; ++i) encoding[offset++] = i;
    for (std::uint8_t i = 0; i < 16; ++i) encoding[offset++] = static_cast<std::uint8_t>(0x10 + i);
    for (std::uint8_t i = 0; i < 16; ++i) encoding[offset++] = static_cast<std::uint8_t>(0x20 + i);
    const auto parsed_encoding = openblizz::EncodingIndex::parse(encoding);
    assert(parsed_encoding.mappings().size() == 1);
    assert(parsed_encoding.mappings().begin()->second.encoding_keys.size() == 2);

    std::vector<std::uint8_t> archive(4096 + 16 + 8 + 28, 0);
    for (std::size_t i = 0; i < 16; ++i) archive[i] = static_cast<std::uint8_t>(0xa0 + i);
    archive[18] = 0x12;
    archive[19] = 0x34;
    archive[20] = 1;
    archive[21] = 2;
    archive[22] = 3;
    archive[23] = 4;
    const auto footer = archive.size() - 28;
    archive[footer + 8] = 1;
    archive[footer + 11] = 4;
    archive[footer + 12] = 4;
    archive[footer + 13] = 4;
    archive[footer + 14] = 16;
    archive[footer + 15] = 8;
    put_u32le_at(archive, footer + 16, 1);
    const auto parsed_archive = openblizz::ArchiveIndex::parse(archive);
    const auto* archive_entry = parsed_archive.find("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    assert(archive_entry != nullptr);
    assert(archive_entry->offset == 0x01020304);
    assert(archive_entry->encoded_size == 0x1234);

    const std::vector<openblizz::ProductDescriptor> products{
        {"w3", "Warcraft III: Reforged", "warcraft", "w3", true},
        {"w2r", "Warcraft II: Remastered", "warcraft", "w2r", true},
        {"w1r", "Warcraft I: Remastered", "warcraft", "w1r", true},
        {"s1", "StarCraft: Remastered", "starcraft", "s1", true},
    };
    assert(openblizz::find_product(products, "s1").name == "StarCraft: Remastered");
    const auto records = openblizz::LibraryManager::parse_entitlement_response(
        R"({"products":[{"product":"w3","owned":true},{"product":"w2r","owned":false},"w1r"]})",
        products);
    assert(records.size() == 3);
    assert(records[0].product_id == "w1r");
    assert(records[0].owned);
    assert(records[1].product_id == "w2r");
    assert(!records[1].owned);
    assert(records[2].product_id == "w3");
    assert(records[2].owned);

    const auto library_path = std::filesystem::temp_directory_path() / "openblizz-library-test.json";
    openblizz::LibraryManager::save(library_path, {
        {"w3", "Warcraft III: Reforged", openblizz::OwnershipState::Manual,
         "manual", "test", 1},
    });
    const auto loaded = openblizz::LibraryManager::load(library_path);
    assert(loaded.size() == 1);
    assert(loaded.front().ownership == openblizz::OwnershipState::Manual);
    std::filesystem::remove(library_path);

    const std::vector<openblizz::ProductDescriptor> web_products{
        {"w3", "Warcraft III: Reforged", "warcraft", "w3", true},
        {"w3-legacy-tft", "Warcraft III: legacy/TFT", "warcraft", "w3", true},
        {"w2bn", "Warcraft II: Battle.net Edition", "warcraft", "w2bn", true},
        {"s1", "StarCraft: Remastered", "starcraft", "s1", true},
    };
    const auto web = openblizz::LibraryManager::parse_account_web(
        R"({"gameAccounts":[{"titleId":22323,"gameAccountStatus":"Good"},
                            {"titleId":21297,"gameAccountStatus":"Trial"},
                            {"titleId":5730135,"gameAccountStatus":"Inactive"}]})",
        "{\"classicGames\":[{\"localizedGameName\":\"Warcraft\xc2\xae II: Battle.net\xc2\xae Edition\"},"
        "{\"localizedGameName\":\"Diablo\xc2\xae II\"}]}",
        web_products);
    assert(web.records.size() == 4);
    assert(web.records[0].product_id == "w3" && web.records[0].owned);
    assert(web.records[1].product_id == "w3-legacy-tft" && web.records[1].owned);
    assert(web.records[2].product_id == "s1" && !web.records[2].owned);
    assert(web.records[3].product_id == "w2bn" && web.records[3].owned);
    assert(web.unknown_titles.size() == 2);

    const auto cookie_path = std::filesystem::temp_directory_path() / "openblizz-cookies-test.txt";
    {
        std::ofstream cookies(cookie_path);
        cookies << "# Netscape HTTP Cookie File\n"
                << "account.battle.net\tFALSE\t/\tTRUE\t0\tJSESSIONID\tabc\n"
                << "#HttpOnly_.battle.net\tTRUE\t/\tTRUE\t0\tsessionTrackingId\txyz\n"
                << "www.example.com\tFALSE\t/\tTRUE\t0\tignored\t1\n";
    }
    const auto header = openblizz::LibraryManager::cookie_header_from_netscape_file(cookie_path, "account.battle.net");
    assert(header == "JSESSIONID=abc; sessionTrackingId=xyz");
    std::filesystem::remove(cookie_path);

    const auto tvfs = openblizz::parse_tvfs(make_tvfs(0x01));
    assert(tvfs.ekey_size == 9);
    assert(tvfs.entries.size() == 2);
    assert(tvfs.entries[0].path == "a.txt");
    assert(tvfs.entries[0].spans.size() == 1);
    assert(tvfs.entries[0].spans[0].encoding_key == "010203040506070809");
    assert(tvfs.entries[0].spans[0].encoded_size == 7);
    assert(tvfs.entries[0].spans[0].content_size == 10);
    assert(tvfs.entries[0].spans[0].content_key == "c1c2c3c4c5c6c7c8c9cacbcccdcecfd0");
    assert(tvfs.entries[1].path == "dir/sub/b.bin");
    assert(tvfs.entries[1].spans[0].encoding_key == "02030405060708090a");
    assert(tvfs.entries[1].spans[0].content_size == 20);

    // vfs-1's EKey starts with the EKey of "a.txt" in vfs-root, so the
    // resolver must mount it as a nested manifest.
    const auto vfs_config = openblizz::parse_config(
        "vfs-root = 00000000000000000000000000000000 ffffffffffffffffffffffffffffffff\n"
        "vfs-root-size = 10 20\n"
        "vfs-1 = 11111111111111111111111111111111 01020304050607080900000000000000\n");
    const auto refs = openblizz::vfs_manifest_refs(vfs_config);
    assert(refs.size() == 2 && refs[0].name == "vfs-root" && refs[0].content_size == 10);
    const auto vfs_files = openblizz::VfsResolver::resolve(vfs_config, [](const openblizz::VfsManifestRef& ref) {
        return make_tvfs(ref.name == "vfs-root" ? 0x01 : 0x41);
    });
    assert(vfs_files.size() == 4);
    assert(vfs_files[0].path == "a.txt" && vfs_files[0].nested_manifest == "vfs-1");
    assert(vfs_files[1].path == "a.txt:a.txt" && vfs_files[1].manifest == "vfs-1");
    assert(vfs_files[1].spans[0].encoding_key == "414243444546474849");
    assert(vfs_files[2].path == "a.txt:dir/sub/b.bin");
    assert(vfs_files[3].path == "dir/sub/b.bin" && vfs_files[3].nested_manifest.empty());

    std::cout << "OpenBlizz core tests passed\n";
    return 0;
}
