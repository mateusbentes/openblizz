#include "openblizz/casc.hpp"
#include "openblizz/browser_login.hpp"
#include "openblizz/formats.hpp"
#include "openblizz/hash.hpp"
#include "openblizz/library.hpp"
#include "openblizz/tvfs.hpp"

#include <array>
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
        {"wow", "World of Warcraft", "warcraft", "wow", true},
        {"d2-classic", "Diablo II (classic, legacy installer)", "diablo", "", false},
    };
    const auto web = openblizz::LibraryManager::parse_account_web(
        R"({"gameAccounts":[{"titleId":22323,"gameAccountStatus":"Good"},
                            {"titleId":21297,"gameAccountStatus":"Trial"},
                            {"titleId":5730135,"gameAccountStatus":"Inactive"}]})",
        "{\"classicGames\":[{\"localizedGameName\":\"Warcraft\xc2\xae II: Battle.net\xc2\xae Edition\"},"
        "{\"localizedGameName\":\"Diablo\xc2\xae II\"}]}",
        web_products);
    assert(web.records.size() == 6);
    assert(web.records[0].product_id == "w3" && web.records[0].owned);
    assert(web.records[1].product_id == "w3-legacy-tft" && web.records[1].owned);
    assert(web.records[2].product_id == "s1" && !web.records[2].owned);
    assert(web.records[3].product_id == "wow" && web.records[3].owned);
    assert(web.records[4].product_id == "w2bn" && web.records[4].owned);
    assert(web.records[5].product_id == "d2-classic" && web.records[5].owned);
    assert(web.unknown_titles.empty());
    // FourCC title ids.
    assert(openblizz::decode_title_id(22323) == "W3");
    assert(openblizz::decode_title_id(5730135) == "WoW");
    assert(openblizz::decode_title_id(1095647827) == "ANBS");
    assert(openblizz::decode_title_id(-1).empty() && openblizz::decode_title_id(1).empty());
    assert(openblizz::encode_title_id("S2") == 21298);
    assert(openblizz::expected_title_id("wow") == 5730135);
    assert(openblizz::expected_title_id("w3-legacy-tft") == 22323);
    assert(openblizz::expected_title_id("hsb") == 1465140039);
    assert(openblizz::expected_title_id("w1r") == 5714258);
    {
        // Absent curated products become not_owned; unknown FourCCs are reported
        // decoded; summary-only products are matched by code.
        const std::vector<openblizz::ProductDescriptor> all{
            {"w3", "Warcraft III: Reforged", "warcraft", "w3", true},
            {"w1r", "Warcraft I: Remastered", "warcraft", "w1r", true},
            {"d2-lod", "Diablo II: LoD", "diablo", "", false},
            {"anbs", "NGDP product anbs", "ngdp", "anbs", true},
        };
        const auto r = openblizz::LibraryManager::parse_account_web(
            R"({"gameAccounts":[{"titleId":22323,"gameAccountStatus":"Good"},
                                {"titleId":1095647827,"gameAccountStatus":"Good"},
                                {"titleId":1178684229,"gameAccountStatus":"Good"}]})",
            R"({"classicGames":[]})", all);
        assert(r.records.size() == 3);  // w1r is license-only: absence proves nothing
        assert(r.records[0].product_id == "w3" && r.records[0].owned);
        assert(r.records[1].product_id == "anbs" && r.records[1].owned);
        assert(r.records[2].product_id == "d2-lod" && !r.records[2].owned);
        assert(r.unknown_titles.size() == 1 && r.unknown_titles[0].find("\"FAKE\"") != std::string::npos);
    }
    {
        // License-only titles (w1r) are never derived as not_owned from
        // games-and-subs; purchases resolve them instead.
        const std::vector<openblizz::ProductDescriptor> all{
            {"w3", "Warcraft III: Reforged", "warcraft", "w3", true},
            {"w1r", "Warcraft I: Remastered", "warcraft", "w1r", true},
            {"w2r", "Warcraft II: Remastered", "warcraft", "w2r", true},
            {"s1", "StarCraft: Remastered", "starcraft", "s1", true},
        };
        const auto r = openblizz::LibraryManager::parse_account_web(
            R"({"gameAccounts":[{"titleId":22323,"gameAccountStatus":"Good"}]})", R"({"classicGames":[]})", all);
        assert(r.records.size() == 2);
        assert(r.records[0].product_id == "w3" && r.records[0].owned);
        assert(r.records[1].product_id == "s1" && !r.records[1].owned);
        const auto purchases = openblizz::LibraryManager::parse_purchases({
            R"({"purchases":[{"productTitle":"Warcraft\u00ae I: Remastered","status":1},
                              {"productTitle":"Warcraft\u00ae I & II: Remastered Battle Chest","localizedStatus":"Refunded"},
                              {"productTitle":"The Witcher 3: Wild Hunt - Remastered","status":1}],
                 "giftClaims":[]})"}, all);
        assert(purchases.records.size() == 1);
        assert(purchases.records[0].product_id == "w1r" && purchases.records[0].owned);
        assert(purchases.unmatched_titles.size() == 1);
        const auto cards = openblizz::LibraryManager::parse_shop_cards(
            "<script>self.__next_f.push([1,\"x\\\"productPageName\\\":\\\"The Witcher 3 \\\",\\\"slug\\\":\\\"the-witcher-3\\\",\\\"franchise\\\":\\\"Witcher\\\",\\\"appGameCode\\\":\\\"\\\"\"])</script>");
        assert(cards.size() == 1 && cards[0].name == "The Witcher 3" && cards[0].slug == "the-witcher-3");
    }


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

    // lookup3 reference vectors (Bob Jenkins' driver5 in lookup3.c).
    assert(openblizz::jenkins_hashlittle("", 0, 0) == 0xdeadbeef);
    assert(openblizz::jenkins_hashlittle("Four score and seven years ago", 30, 0) == 0x17770551);
    assert(openblizz::jenkins_hashlittle("Four score and seven years ago", 30, 1) == 0xcd628161);
    {
        std::uint32_t pc = 0;
        std::uint32_t pb = 0;
        openblizz::jenkins_hashlittle2("Four score and seven years ago", 30, pc, pb);
        assert(pc == 0x17770551 && pb == 0xce7226e6);
    }

    // CASC bucket selection and index journal round trip.
    const auto casc_key = openblizz::casc_key("0123456789abcdef0123456789abcdef");
    assert(openblizz::casc_bucket(casc_key) == (((0x01 ^ 0x23 ^ 0x45 ^ 0x67 ^ 0x89 ^ 0xab ^ 0xcd ^ 0xef ^ 0x01) & 0xf) ^
                                               ((0x01 ^ 0x23 ^ 0x45 ^ 0x67 ^ 0x89 ^ 0xab ^ 0xcd ^ 0xef ^ 0x01) >> 4)));
    {
        std::vector<openblizz::CascEntry> entries(2);
        entries[0].key = openblizz::casc_key("ff0000000000000000");
        entries[0].archive = 3;
        entries[0].offset = 0x12345;
        entries[0].size = 100;
        entries[1].key = openblizz::casc_key("000000000000000001");
        entries[1].archive = 1023;
        entries[1].offset = 0x3fffffff;
        entries[1].size = 30;
        const auto idx = openblizz::encode_casc_index(5, entries);
        assert(idx.size() >= 0x8000 && idx[8] == 7 && idx[10] == 5);
        const auto decoded = openblizz::decode_casc_index(idx, 5);
        assert(decoded.size() == 2);
        assert(decoded[0].key == entries[1].key && decoded[0].archive == 1023 && decoded[0].offset == 0x3fffffff);
        assert(decoded[1].key == entries[0].key && decoded[1].archive == 3 && decoded[1].offset == 0x12345 &&
               decoded[1].size == 100);
        bool rejected = false;
        try { (void)openblizz::decode_casc_index(idx, 6); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
    }
    {
        std::array<std::uint8_t, 16> ekey{};
        for (std::uint8_t i = 0; i < 16; ++i) ekey[i] = static_cast<std::uint8_t>(0x10 + i);
        const auto header = openblizz::encode_casc_data_header(ekey, 70, 2, 0x1000);
        assert(header[0] == 0x1f && header[15] == 0x10);
        assert(header[0x10] == 100 && header[0x11] == 0 && header[0x14] == 0 && header[0x15] == 0);
        const auto checksum_a = openblizz::jenkins_hashlittle(header.data(), 0x16, 0x3D6BE971);
        assert(header[0x16] == static_cast<std::uint8_t>(checksum_a) &&
               header[0x19] == static_cast<std::uint8_t>(checksum_a >> 24));
    }
    {
        const auto casc_root = std::filesystem::temp_directory_path() / "openblizz-casc-test";
        std::filesystem::remove_all(casc_root);
        const std::string key_a = "00112233445566778899aabbccddeeff";
        const std::string key_b = "ffeeddccbbaa99887766554433221100";
        const std::vector<std::uint8_t> blob_a{'B', 'L', 'T', 'E', 0, 0, 0, 0, 'N', 'a'};
        const std::vector<std::uint8_t> blob_b{'B', 'L', 'T', 'E', 0, 0, 0, 0, 'N', 'b', 'b'};
        {
            openblizz::CascStorage storage(casc_root);
            storage.open();
            assert(storage.append(key_a, blob_a));
            assert(!storage.append(key_a, blob_a));
            assert(storage.append(key_b, blob_b));
            storage.commit();
            assert(storage.size() == 2);
        }
        std::size_t journals = 0;
        for (const auto& item : std::filesystem::directory_iterator(casc_root / "data")) {
            if (item.path().extension() == ".idx") ++journals;
        }
        assert(journals == 16);
        {
            openblizz::CascStorage storage(casc_root);
            storage.open();
            assert(storage.size() == 2);
            assert(storage.contains(key_a) && storage.contains(key_b) && !storage.contains("01" + key_a.substr(2)));
            const auto entry = storage.find(key_b);
            assert(entry && entry->archive == 0 && entry->offset == 30 + blob_a.size() && entry->size == 30 + blob_b.size());
            assert(storage.read(*entry) == blob_b);
            assert(storage.read(*storage.find(key_a)) == blob_a);
            assert(storage.append("abcdefabcdefabcdefabcdefabcdefab", blob_a));
            storage.commit();
        }
        {
            openblizz::CascStorage storage(casc_root);
            storage.open();
            assert(storage.size() == 3);
        }
        journals = 0;
        for (const auto& item : std::filesystem::directory_iterator(casc_root / "data")) {
            if (item.path().extension() == ".idx") ++journals;
        }
        assert(journals == 16);   // superseded journals were removed
        std::filesystem::remove_all(casc_root);
    }
    const auto build_info = openblizz::format_build_info({"us", "aa", "bb", "cc", 42, "tpr/war3",
                                                          {"h1", "h2"}, {"s1"}, "T", "1.0", "w3"});
    assert(build_info.rfind("Branch!STRING:0|Active!DEC:1|Build Key!HEX:16", 0) == 0);
    assert(build_info.find("\nus|1|aa|bb|cc|42|tpr/war3|h1 h2|s1|T|||1.0||w3\n") != std::string::npos);
    assert(openblizz::build_info_tags("us", "enUS") ==
           "Windows x86_64 US? enUS speech?:Windows x86_64 US enUS text?");

    std::cout << "OpenBlizz core tests passed\n";
    {
        using openblizz::BrowserLogin;
        assert(BrowserLogin::looks_logged_in("https://account.battle.net/games", "account.battle.net"));
        assert(BrowserLogin::looks_logged_in("https://account.battle.net/", "account.battle.net"));
        assert(!BrowserLogin::looks_logged_in("https://account.battle.net/login/en-us/", "account.battle.net"));
        assert(!BrowserLogin::looks_logged_in("https://eu.account.battle.net/login/en-us/?ref=x", "account.battle.net"));
        assert(!BrowserLogin::looks_logged_in("https://account.battle.net/oauth2/authorization/account-settings", "account.battle.net"));
        assert(!BrowserLogin::looks_logged_in("https://oauth.battle.net/authorize?x=1", "account.battle.net"));
        const auto cookies = BrowserLogin::parse_devtools_cookies(R"({"cookies":[
            {"name":"login.key","value":"abc","domain":".battle.net","path":"/","expires":1900000000,"secure":true,"httpOnly":true},
            {"name":"SESSIONID","value":"s","domain":"account.battle.net","path":"/","expires":-1,"secure":true,"httpOnly":false},
            {"name":"other","value":"x","domain":".example.com","path":"/","expires":-1,"secure":false,"httpOnly":false}]})");
        assert(cookies.size() == 3);
        const auto jar = BrowserLogin::netscape_jar(cookies, "battle.net");
        assert(jar.find("#HttpOnly_.battle.net\tTRUE\t/\tTRUE\t1900000000\tlogin.key\tabc\n") != std::string::npos);
        assert(jar.find("account.battle.net\tFALSE\t/\tTRUE\t2147483647\tSESSIONID\ts\n") != std::string::npos);
        assert(jar.find("example.com") == std::string::npos);
        const auto bidi = BrowserLogin::parse_bidi_cookies(R"({"cookies":[
            {"name":"JSESSIONID","value":{"type":"string","value":"v1"},"domain":".battle.net","path":"/","expiry":1900000000,"secure":true,"httpOnly":true,"sameSite":"none","size":12},
            {"name":"XSRF-TOKEN","value":{"type":"string","value":"t"},"domain":"account.battle.net","path":"/","secure":true,"httpOnly":false,"size":3},
            {"name":"bin","value":{"type":"base64","value":"AAE="},"domain":".battle.net","path":"/","secure":true,"httpOnly":false,"size":2}],"partitionKey":{}})");
        assert(bidi.size() == 2 && bidi[0].value == "v1" && bidi[0].expires == 1900000000 && bidi[1].expires < 0);
        assert(BrowserLogin::engine_for_name("firefox") == openblizz::BrowserEngine::Firefox);
        assert(BrowserLogin::engine_for_name("org.mozilla.firefox") == openblizz::BrowserEngine::Firefox);
        assert(BrowserLogin::engine_for_name("librewolf") == openblizz::BrowserEngine::Firefox);
        assert(BrowserLogin::engine_for_name("google-chrome") == openblizz::BrowserEngine::Chromium);
        assert(BrowserLogin::engine_for_name("brave-browser") == openblizz::BrowserEngine::Chromium);
    }

    {
        // Storefront navigation menu and family cards (synthetic Next.js flight payload).
        const std::string html =
            "<script>self.__next_f.push([1,\"{\\\"text\\\":\\\"Blizzard Arcade Collection\\\",\\\"destination\\\":\\\"/product/blizzard-arcade-collection\\\",\\\"category\\\":\\\"Bundle\\\"}"
            ",{\\\"text\\\":\\\"Warcraft\\\",\\\"destination\\\":\\\"/family/warcraft-rts\\\",\\\"category\\\":\\\"Real-time Strategy\\\"}"
            ",{\\\"text\\\":\\\"Support\\\",\\\"destination\\\":\\\"https://support.example\\\",\\\"category\\\":\\\"x\\\"}"
            ",{\\\"cmsId\\\":\\\"blt1\\\",\\\"order\\\":0,\\\"slug\\\":\\\"warcraft-1-remastered\\\",\\\"productIds\\\":[1],\\\"subscriptionIds\\\":[],\\\"title\\\":\\\"Warcraft\\u00ae I: Remastered\\\",\\\"franchiseIcon\\\":{\\\"name\\\":\\\"Warcraft\\\"}}\"])</script>";
        const auto games = openblizz::LibraryManager::parse_shop_games(html);
        assert((games.size() == 2));
        assert((games[0].name == "Blizzard Arcade Collection" && games[0].franchise == "Bundle"));
        const auto cards = openblizz::LibraryManager::parse_shop_family(html);
        assert((cards.size() == 1 && cards[0].slug == "/product/warcraft-1-remastered"));
        assert((cards[0].name == "Warcraft\xC2\xAE I: Remastered"));
        assert((openblizz::LibraryManager::shop_destination_products("/product/blizzard-arcade-collection") == std::vector<std::string>{"rtro"}));
    }
    return 0;
}
