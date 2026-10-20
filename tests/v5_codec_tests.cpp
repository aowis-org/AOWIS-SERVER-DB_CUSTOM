#include "aowis/native/model_codec_adapter.h"

#include <QCoreApplication>
#include <QDate>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using aowis::native::Bytes;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::Repository;
namespace v5 = aowis::native::v5;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QUuid fixedUuid(std::uint8_t kind, std::uint64_t ordinal) {
    Key key{};
    key[0] = kind;
    key[6] = 0x70;
    key[8] = 0x80;
    for (unsigned i = 0; i < 6; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(ordinal >> (8U * i));
    }
    return v5::uuidFromKey(key);
}

std::filesystem::path uniqueDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "aowis-native-v5-test-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    char* directory = ::mkdtemp(name.data());
    if (!directory) throw std::runtime_error("mkdtemp failed");
    return std::filesystem::path(directory);
}

void testCanonicalCodecAndRepository() {
    const std::filesystem::path path = uniqueDirectory();
    try {
        const QUuid idA = fixedUuid(1, 1);
        const QUuid idB = fixedUuid(1, 2);
        HydraulicNodeJunction a;
        a.uuid = idA;
        a.id = QStringLiteral("water–α");
        a.elevation_m = 109.625;
        a.coordinate_wgs84.latitude_deg = 11.987654321;
        a.metadata.description = QStringLiteral("Test junction with UTF-8 É 水");
        a.metadata.date_installed = QDate(2021, 4, 2);
        HydraulicNodeJunction b;
        b.uuid = idB;
        b.id = QStringLiteral("J-2");
        HydraulicLinkPipe pipe;
        pipe.uuid = fixedUuid(2, 99);
        pipe.node_uuid_from = idA;
        pipe.node_uuid_to = idB;
        pipe.length_measured_m = 123.125;
        pipe.diameter_mm = 90.625;
        pipe.roughness_darcy_weisbach_mm = 0.025;
        const Bytes expectedA = v5::bytesFromQByteArray(
            aowis::native::ModelCodec<HydraulicNodeJunction>::encode(a));
        expect(!expectedA.empty(), "native V5 codec returned empty junction");
        expect(v5::qByteArrayFromBytes(expectedA) ==
               aowis::native::ModelCodec<HydraulicNodeJunction>::encode(a),
               "canonical codec bytes changed on roundtrip");
        expect(v5::uuidFromKey(v5::keyFromUuid(idA)) == idA,
               "UUID key transform inconsistent");
        {
            Repository store(path);
            store.createBranch("main");
            const std::vector<ModelChange> changes{v5::changeFor(a), v5::changeFor(b), v5::changeFor(pipe)};
            store.commitModel("main", changes);
            const std::optional<aowis::native::ModelRecord> stored = store.getModel("main", v5::keyFromUuid(idA));
            expect(stored.has_value() && stored->kind == v5::junctionKind &&
                   stored->payload == expectedA, "native store modified canonical native V5 bytes");
            QString error;
            const std::optional<HydraulicNodeJunction> readA =
                v5::readJunction(store, "main", v5::keyFromUuid(idA), &error);
            expect(readA.has_value() && readA->id == a.id && readA->elevation_m == a.elevation_m,
                   "native-to-V5 junction roundtrip failed");
            const std::optional<HydraulicLinkPipe> readPipe =
                v5::readPipe(store, "main", v5::keyFromUuid(pipe.uuid), &error);
            expect(readPipe.has_value() && readPipe->node_uuid_from == idA &&
                   readPipe->node_uuid_to == idB && readPipe->diameter_mm == pipe.diameter_mm,
                   "native-to-V5 pipe roundtrip failed");
            expect(!v5::readJunction(store, "main", v5::keyFromUuid(pipe.uuid), &error),
                   "reading a pipe as a junction must fail");
            store.createBranch("other", "main");
            pipe.diameter_mm = 112.75;
            store.commitModel("other", {v5::changeFor(pipe)});
            expect(v5::readPipe(store, "main", v5::keyFromUuid(pipe.uuid))->diameter_mm == 90.625,
                   "branch isolation failed");
            expect(v5::readPipe(store, "other", v5::keyFromUuid(pipe.uuid))->diameter_mm == 112.75,
                   "branch update failed");
            expect(store.diff("main", "other").size() == 1,
                   "codec-level branch diff failed");
            bool rejected = false;
            try {
                store.commitModel("other", {{v5::keyFromUuid(idA), std::nullopt}});
            } catch (const std::exception&) { rejected = true; }
            expect(rejected, "referenced junction deletion was accepted");
        }
        {
            Repository reopened(path);
            const std::optional<HydraulicLinkPipe> stored =
                v5::readPipe(reopened, "other", v5::keyFromUuid(pipe.uuid));
            expect(stored.has_value() && stored->diameter_mm == 112.75,
                   "reopened canonical V5 pipe missing");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testCanonicalCodecAndRepository();
        std::cout << "Canonical native V5 model codec/native repository test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Canonical native V5 model codec/native repository test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
