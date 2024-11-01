#include "whisper.h"

#include <emscripten.h>
#include <emscripten/bind.h>

#include <vector>
#include <thread>

std::mutex  g_mutex;
std::thread g_worker;

std::string g_transcribed   = "";

std::vector<struct whisper_context *> g_contexts(4, nullptr);

static inline int mpow2(int n) {
    int p = 1;
    while (p <= n) p *= 2;
    return p/2;
}

const int MaxThreads = mpow2(std::thread::hardware_concurrency());

EMSCRIPTEN_BINDINGS(whisper) {
    emscripten::function("init", emscripten::optional_override([](const std::string & path_model) {
        if (g_worker.joinable()) {
            g_worker.join();
        }

        for (size_t i = 0; i < g_contexts.size(); ++i) {
            if (g_contexts[i] == nullptr) {
                g_contexts[i] = whisper_init_from_file_with_params(path_model.c_str(), whisper_context_default_params());
                if (g_contexts[i] != nullptr) {
                    return i + 1;
                } else {
                    return (size_t) 0;
                }
            }
        }

        return (size_t) 0;
    }));

    emscripten::function("free", emscripten::optional_override([](size_t index) {
        if (g_worker.joinable()) {
            g_worker.join();
        }

        --index;

        if (index < g_contexts.size()) {
            whisper_free(g_contexts[index]);
            g_contexts[index] = nullptr;
        }
    }));

    emscripten::function("full_default", emscripten::optional_override([](size_t index, const emscripten::val & audio, int nthreads) {
        if (g_worker.joinable()) {
            g_worker.join();
        }

        if (nthreads == -1) {
            nthreads = MaxThreads;
        }

        --index;

        if (index >= g_contexts.size()) {
            return -1;
        }

        if (g_contexts[index] == nullptr) {
            return -2;
        }

        struct whisper_full_params params = whisper_full_default_params(whisper_sampling_strategy::WHISPER_SAMPLING_GREEDY);

        params.print_realtime   = false;
        params.print_progress   = false;
        params.print_timestamps = false;
        params.print_special    = false;
        params.translate        = false;
        params.language         = "en";
        params.n_threads        = std::min(nthreads, std::min(16, MaxThreads));
        params.offset_ms        = 0;

        std::vector<float> pcmf32;
        const int n = audio["length"].as<int>();

        emscripten::val heap = emscripten::val::module_property("HEAPU8");
        emscripten::val memory = heap["buffer"];

        pcmf32.resize(n);

        emscripten::val memoryView = audio["constructor"].new_(memory, reinterpret_cast<uintptr_t>(pcmf32.data()), n);
        memoryView.call<void>("set", audio);

        // print system information
        // {
        //     printf("system_info: n_threads = %d / %d | %s\n",
        //             params.n_threads, std::thread::hardware_concurrency(), whisper_print_system_info());

        //     printf("%s: processing %d samples, %.1f sec, %d threads, %d processors, lang = %s, task = %s ...\n",
        //             __func__, int(pcmf32.size()), float(pcmf32.size())/WHISPER_SAMPLE_RATE,
        //             params.n_threads, 1,
        //             params.language,
        //             params.translate ? "translate" : "transcribe");

        //     printf("\n");
        // }

        // run the worker
        {
            g_transcribed = "";

            g_worker = std::thread([index, params, pcmf32 = std::move(pcmf32)]() {
                whisper_reset_timings(g_contexts[index]);
                whisper_full(g_contexts[index], params, pcmf32.data(), pcmf32.size());
                // whisper_print_timings(g_contexts[index]);

                const auto t_start = std::chrono::high_resolution_clock::now();
                auto ctx = g_contexts[index];

                float prob = 0.0f;
                int64_t t_ms = 0;
                int prob_n = 0;
                std::string result;

                const int n_segments = whisper_full_n_segments(ctx);
                for (int i = 0; i < n_segments; ++i) {
                    const char * text = whisper_full_get_segment_text(ctx, i);

                    result += text;

                    const int n_tokens = whisper_full_n_tokens(ctx, i);
                    for (int j = 0; j < n_tokens; ++j) {
                        const auto token = whisper_full_get_token_data(ctx, i, j);

                        prob += token.p;
                        ++prob_n;
                    }
                }

                if (prob_n > 0) {
                    prob /= prob_n;
                }

                const auto t_end = std::chrono::high_resolution_clock::now();
                t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

                std::lock_guard<std::mutex> lock(g_mutex);
                g_transcribed += result;

                // printf("[af] TRANSCRIBED: %s\n", result.c_str());
            });
        }

        return 0;
    }));

    emscripten::function("get_text", emscripten::optional_override([](size_t index) -> std::string {
        std::string transcribed;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            transcribed = std::move(g_transcribed);
        }

        return transcribed;
    }));
}
