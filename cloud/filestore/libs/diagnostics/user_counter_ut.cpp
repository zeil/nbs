#include "user_counter.h"

#include "request_stats.h"

#include <cloud/storage/core/libs/common/timer.h>

#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/monlib/encode/json/json.h>
#include <library/cpp/monlib/encode/spack/spack_v1.h>
#include <library/cpp/monlib/encode/text/text.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NCloud::NFileStore::NUserCounter {

namespace {

////////////////////////////////////////////////////////////////////////////////

NJson::TJsonValue GetValue(const auto& object, const auto& name)
{
    for (const auto& data: object["sensors"].GetArray()) {
        if (data["labels"]["name"] == name) {
            if (!data.Has("hist")) {
                return data["value"];
            }
        }
    }
    UNIT_ASSERT(false);
    return NJson::TJsonValue{};
};

NJson::TJsonValue GetHist(
    const auto& object,
    const auto& name,
    const auto& valueName)
{
    for (const auto& data: object["sensors"].GetArray()) {
        if (data["labels"]["name"] == name) {
            if (data.Has("hist")) {
                return data["hist"][valueName];
            }
        }
    }
    UNIT_ASSERT(false);
    return NJson::TJsonValue{};
};

void ValidateJsons(
    const NJson::TJsonValue& testJson,
    const NJson::TJsonValue& resultJson)
{
    for(const auto& jsonValue: testJson["sensors"].GetArray()) {
        const TString name = jsonValue["labels"]["name"].GetString();

        if (jsonValue.Has("hist")) {
            for (auto valueName: {"bounds", "buckets", "inf"}) {
                UNIT_ASSERT_STRINGS_EQUAL_C(
                    NJson::WriteJson(GetHist(resultJson, name, valueName)),
                    NJson::WriteJson(GetHist(testJson, name, valueName)),
                    name
                );
            }
        } else {
            UNIT_ASSERT_STRINGS_EQUAL_C(
                NJson::WriteJson(GetValue(resultJson, name)),
                NJson::WriteJson(GetValue(testJson, name)),
                name
            );
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

const TString METRIC_COMPONENT = "test";
const TString METRIC_FS_COMPONENT = METRIC_COMPONENT + "_fs";

struct TEnv
    : public NUnitTest::TBaseFixture
{
    NMonitoring::TDynamicCountersPtr Counters;
    ITimerPtr Timer;
    std::shared_ptr<NStorage::NUserStats::TUserCounterSupplier> Supplier;
    IRequestStatsRegistryPtr Registry;

    TEnv()
        : Counters(MakeIntrusive<NMonitoring::TDynamicCounters>())
        , Timer(CreateWallClockTimer())
        , Supplier(std::make_shared<NStorage::NUserStats::TUserCounterSupplier>())
        , Registry(CreateRequestStatsRegistry(
            METRIC_COMPONENT,
            nullptr,
            Counters,
            Timer,
            Supplier))
    {}

    void SetUp(NUnitTest::TTestContext& /*context*/) override
    {}

    void TearDown(NUnitTest::TTestContext& /*context*/) override
    {}
};

}   // namespace

////////////////////////////////////////////////////////////////////////////////

using namespace NMonitoring;

Y_UNIT_TEST_SUITE(TUserWrapperTest)
{
    Y_UNIT_TEST_F(ShouldMultipleRegister, TEnv)
    {
        const TString fsId = "test_fs";
        const TString clientId = "test_client";

        const TString testResult = R"--({
          "sensors":[
            {
              "kind":"GAUGE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.read_bytes_burst"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"GAUGE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.read_ops_burst"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.read_ops"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"HIST_RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.read_latency"
              },
              "ts":12,
              "hist":{
                "bounds":[1,2,5,10,20,50,100,200,500,1000,2000,5000,10000,35000],
                "buckets":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],
                "inf":0
              }
            },
            {
              "kind":"HIST_RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.write_latency"
              },
              "ts":12,
              "hist":{
                "bounds":[1,2,5,10,20,50,100,200,500,1000,2000,5000,10000,35000],
                "buckets":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],
                "inf":0
              }
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.index_errors"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"GAUGE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.write_bytes_burst"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"GAUGE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.write_ops_burst"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.read_errors"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.write_bytes"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.index_ops"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.write_ops"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.write_errors"
              },
              "ts":12,
              "value":0
            },
            {
              "kind":"RATE",
              "labels":{
                "service":"compute",
                "project":"cloud",
                "cluster":"folder",
                "filestore":"test_fs",
                "instance":"test_client",
                "name":"filestore.read_bytes"
              },
              "ts":12,
              "value":0
            }
          ]
        })--";
        auto testJson = NJson::ReadJsonFastTree(testResult, true);
        auto emptyJson = NJson::ReadJsonFastTree("{}", true);

        auto stats = Registry->GetFileSystemStats(fsId, clientId);


        // First registration
        Registry->RegisterUserStats("cloud", "folder", fsId, clientId);

        TStringStream firstOut;
        auto firstEncoder = EncoderJson(&firstOut);
        Supplier->Accept(TInstant::Seconds(12), firstEncoder.Get());

        auto firstResult = NJson::ReadJsonFastTree(firstOut.Str(), true);
        ValidateJsons(testJson, firstResult);

        // Second registration
        Registry->RegisterUserStats("cloud", "folder", fsId, clientId);

        TStringStream secondOut;
        auto secondEncoder = EncoderJson(&secondOut);
        Supplier->Accept(TInstant::Seconds(12), secondEncoder.Get());

        auto secondResult = NJson::ReadJsonFastTree(secondOut.Str(), true);

        ValidateJsons(testJson, secondResult);

        // Unregister
        Registry->Unregister(fsId, clientId);

        TStringStream thirdOut;
        auto thirdEncoder = EncoderJson(&thirdOut);
        Supplier->Accept(TInstant::Seconds(12), thirdEncoder.Get());

        auto thirdResult = NJson::ReadJsonFastTree(thirdOut.Str(), true);
        ValidateJsons(emptyJson, thirdResult);
    }
}

}   // namespace NCloud::NFileStore::NUserCounter