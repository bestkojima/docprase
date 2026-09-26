#include "dococr/dococr.h"
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ":" << __LINE__ << "\n"; std::abort(); } } while (false)

namespace {
DocOcrHandle create_engine(const char* scenario) {
    DocOcrHandle engine = 0;
    CHECK(dococr_create({scenario, std::strlen(scenario)}, &engine) == DOCOCR_OK);
    return engine;
}
DocOcrJob create_job(DocOcrHandle engine) {
    DocOcrJob job = 0;
    CHECK(dococr_job_create(engine, &job) == DOCOCR_OK);
    return job;
}
std::string json_result(DocOcrJob job) {
    DocOcrResult result{sizeof(DocOcrResult), {}, {}};
    CHECK(dococr_job_result(job, &result) == DOCOCR_OK);
    std::string json(reinterpret_cast<char*>(result.json.data), result.json.size);
    CHECK(dococr_bytes_free(&result.json) == DOCOCR_OK);
    CHECK(dococr_bytes_free(&result.markdown) == DOCOCR_OK);
    return json;
}
void destroy(DocOcrHandle engine, DocOcrJob job) {
    CHECK(dococr_job_destroy(job) == DOCOCR_OK);
    CHECK(dococr_destroy(engine) == DOCOCR_OK);
}
}

int main() {
    CHECK(dococr_abi_version() == DOCOCR_ABI_VERSION);
    DocOcrHandle engine = 0;
    const std::string config = "fixture:sample";
    CHECK(dococr_create({config.data(), config.size()}, &engine) == DOCOCR_OK);
    DocOcrJob job = 0;
    CHECK(dococr_job_create(engine, &job) == DOCOCR_OK);
    uint8_t pixels[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    DocOcrInput input{sizeof(DocOcrInput), pixels, sizeof(pixels), DOCOCR_IMAGE_RGB8, 2, 2, 6};
    CHECK(dococr_job_run(job, &input) == DOCOCR_OK);
    DocOcrResult result{sizeof(DocOcrResult), {}, {}};
    CHECK(dococr_job_result(job, &result) == DOCOCR_OK);
    std::string json(reinterpret_cast<char*>(result.json.data), result.json.size);
    std::string md(reinterpret_cast<char*>(result.markdown.data), result.markdown.size);
    CHECK(json.find("\"schema_version\":\"1.0\"") != std::string::npos);
    CHECK(json.find("测试文字") != std::string::npos);
    CHECK(md.find("测试文字") != std::string::npos);
    size_t count = 0;
    CHECK(dococr_job_asset_count(job, &count) == DOCOCR_OK && count == 1);
    DocOcrBytes name{}, data{};
    CHECK(dococr_job_asset(job, 0, &name, &data) == DOCOCR_OK);
    std::string path(reinterpret_cast<char*>(name.data), name.size);
    CHECK(md.find(path) != std::string::npos);
    CHECK(data.size > 8 && std::memcmp(data.data, "\x89PNG\r\n\x1a\n", 8) == 0);
    std::vector<uint8_t> expected_asset(data.data, data.data + data.size);
    CHECK(dococr_bytes_free(&name) == DOCOCR_OK);
    CHECK(dococr_bytes_free(&data) == DOCOCR_OK);
    CHECK(dococr_bytes_free(&result.json) == DOCOCR_OK);
    CHECK(dococr_bytes_free(&result.markdown) == DOCOCR_OK);
    CHECK(dococr_job_destroy(job) == DOCOCR_OK);
    CHECK(dococr_destroy(engine) == DOCOCR_OK);

    // Blank is a successful, explicit state; controlled recognition failure keeps evidence.
    for (const char* scenario : {"fixture:blank", "fixture:failure", "fixture:formula_table", "fixture:invalid_utf8"}) {
        auto e = create_engine(scenario);
        auto j = create_job(e);
        CHECK(dococr_job_run(j, &input) == DOCOCR_OK);
        std::string result_json = json_result(j);
        if (std::strcmp(scenario, "fixture:blank") == 0) {
            CHECK(result_json.find("\"status\":\"blank\"") != std::string::npos);
            size_t assets = 99;
            CHECK(dococr_job_asset_count(j, &assets) == DOCOCR_OK && assets == 0);
        } else if (std::strcmp(scenario, "fixture:failure") == 0) {
            CHECK(result_json.find("\"status\":\"partial\"") != std::string::npos);
            CHECK(result_json.find("partial raw output") != std::string::npos);
            CHECK(result_json.find("\"confidence\":null") != std::string::npos);
            size_t assets = 0;
            CHECK(dococr_job_asset_count(j, &assets) == DOCOCR_OK && assets == 1);
        } else if (std::strcmp(scenario, "fixture:invalid_utf8") == 0) {
            CHECK(result_json.find("\"raw_output_base64\":\"/g==\"") != std::string::npos);
            CHECK(result_json.find("\"text_base64\":\"/w==\"") != std::string::npos);
            CHECK(result_json.find("\"error_base64\":\"/Q==\"") != std::string::npos);
        } else {
            CHECK(result_json.find("\"format\":\"latex\"") != std::string::npos);
            CHECK(result_json.find("\"format\":\"html\"") != std::string::npos);
        }
        destroy(e, j);
    }

    // Caller-owned source is not modified; invalid pixels, dimensions and encoding are rejected.
    auto e = create_engine("fixture:sample");
    auto j = create_job(e);
    uint8_t original[sizeof(pixels)]; std::memcpy(original, pixels, sizeof(pixels));
    CHECK(dococr_job_run(j, &input) == DOCOCR_OK);
    CHECK(std::memcmp(original, pixels, sizeof(pixels)) == 0);
    std::memset(pixels, 0, sizeof(pixels));
    DocOcrBytes owned_name{}, owned_data{};
    CHECK(dococr_job_asset(j, 0, &owned_name, &owned_data) == DOCOCR_OK);
    CHECK(std::vector<uint8_t>(owned_data.data, owned_data.data + owned_data.size) == expected_asset);
    CHECK(dococr_bytes_free(&owned_name) == DOCOCR_OK);
    CHECK(dococr_bytes_free(&owned_data) == DOCOCR_OK);
    std::memcpy(pixels, original, sizeof(pixels));
    CHECK(json_result(j).find("测试文字") != std::string::npos);
    destroy(e, j);
    // Padded RGB rows and grayscale are accepted with explicit pixel formats.
    uint8_t padded[] = {255,0,0,0,255,0,9,9, 0,0,255,255,255,255,9,9};
    DocOcrInput padded_input{sizeof(DocOcrInput), padded, sizeof(padded), DOCOCR_IMAGE_RGB8, 2, 2, 8};
    e = create_engine("fixture:sample"); j = create_job(e);
    CHECK(dococr_job_run(j, &padded_input) == DOCOCR_OK);
    CHECK(json_result(j) == json); // same pixels produce stable DocumentIR despite stride
    destroy(e, j);
    uint8_t gray[] = {0, 127, 200, 255};
    DocOcrInput gray_input{sizeof(DocOcrInput), gray, sizeof(gray), DOCOCR_IMAGE_GRAY8, 2, 2, 2};
    e = create_engine("fixture:sample"); j = create_job(e);
    CHECK(dococr_job_run(j, &gray_input) == DOCOCR_OK);
    CHECK(json_result(j).find("\"raster_size\":[2,2]") != std::string::npos);
    destroy(e, j);
    for (DocOcrInput invalid : {
             DocOcrInput{sizeof(DocOcrInput), pixels, sizeof(pixels), 99, 2, 2, 6},
             DocOcrInput{sizeof(DocOcrInput), pixels, sizeof(pixels), DOCOCR_IMAGE_RGB8, 0, 2, 6},
             DocOcrInput{sizeof(DocOcrInput), pixels, sizeof(pixels), DOCOCR_IMAGE_RGB8, 2, 2, 5},
             DocOcrInput{sizeof(DocOcrInput), pixels, sizeof(pixels), DOCOCR_IMAGE_RGB8, 20000, 20000, 60000},
             DocOcrInput{sizeof(DocOcrInput), pixels, sizeof(pixels), DOCOCR_IMAGE_PNG, 0, 0, 0}}) {
        e = create_engine("fixture:sample"); j = create_job(e);
        CHECK(dococr_job_run(j, &invalid) == DOCOCR_INPUT_ERROR);
        DocOcrResult no_result{sizeof(DocOcrResult), {}, {}};
        CHECK(dococr_job_result(j, &no_result) == DOCOCR_NO_RESULT);
        DocOcrBytes events{};
        CHECK(dococr_job_poll_events(j, &events) == DOCOCR_OK);
        std::string event_text(reinterpret_cast<char*>(events.data), events.size);
        CHECK(event_text.find("input_error") != std::string::npos);
        CHECK(dococr_bytes_free(&events) == DOCOCR_OK);
        destroy(e, j);
    }

    // Generation token prevents a stale copied buffer from freeing a newly allocated result.
    e = create_engine("fixture:sample");
    DocOcrBytes first{};
    CHECK(dococr_capabilities(e, &first) == DOCOCR_OK);
    DocOcrBytes stale = first;
    CHECK(dococr_bytes_free(&first) == DOCOCR_OK);
    bool reused_address = false;
    for (int i = 0; i < 100; ++i) {
        DocOcrBytes live{};
        CHECK(dococr_capabilities(e, &live) == DOCOCR_OK);
        if (live.data == stale.data) reused_address = true;
        CHECK(dococr_bytes_free(&stale) == DOCOCR_INVALID_ARGUMENT);
        CHECK(dococr_bytes_free(&live) == DOCOCR_OK);
    }
    std::cout << "stale-buffer address reuse observed: " << (reused_address ? "yes" : "no") << "\n";
    CHECK(dococr_bytes_free(&first) == DOCOCR_INVALID_ARGUMENT);
    CHECK(dococr_destroy(e) == DOCOCR_OK);

    // Registry handles are opaque and stale handles cannot be reused.
    e = create_engine("fixture:sample"); j = create_job(e);
    CHECK(dococr_destroy(e) == DOCOCR_BUSY);
    CHECK(dococr_job_destroy(j) == DOCOCR_OK);
    CHECK(dococr_job_destroy(j) == DOCOCR_INVALID_HANDLE);
    CHECK(dococr_destroy(e) == DOCOCR_OK);
    CHECK(dococr_destroy(e) == DOCOCR_INVALID_HANDLE);
    CHECK(dococr_job_cancel(j) == DOCOCR_INVALID_HANDLE);

    // A running job refuses destruction; cancellation completes before release.
    e = create_engine("fixture:slow"); j = create_job(e);
    std::atomic<DocOcrStatus> run_status{DOCOCR_FAILED};
    std::thread worker([&] { run_status = dococr_job_run(j, &input); });
    bool running = false;
    for (int i = 0; i < 100 && !running; ++i) {
        DocOcrBytes events{};
        CHECK(dococr_job_poll_events(j, &events) == DOCOCR_OK);
        std::string state(reinterpret_cast<char*>(events.data), events.size);
        running = state.find("\"running\"") != std::string::npos;
        CHECK(dococr_bytes_free(&events) == DOCOCR_OK);
        if (!running) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(running);
    CHECK(dococr_job_destroy(j) == DOCOCR_BUSY);
    CHECK(dococr_destroy(e) == DOCOCR_BUSY);
    CHECK(dococr_job_cancel(j) == DOCOCR_OK);
    worker.join();
    CHECK(run_status == DOCOCR_CANCELLED);
    destroy(e, j);
}
