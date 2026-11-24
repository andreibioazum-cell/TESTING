#include <jni.h>
#include <android/log.h>
#include "llama.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ObsidianAI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ObsidianAI", __VA_ARGS__)

namespace {
std::mutex g_mutex;
llama_model *g_model = nullptr;
llama_context *g_ctx = nullptr;
llama_batch_ext *g_batch = nullptr;
std::vector<llama_token> g_kvTokens;
std::vector<llama_logit_bias> g_banThinkBias;
bool g_modelHasThinkTag = false;
int32_t g_nVocab = 0;

std::string g_loadedPath;
int g_nCtx = 2048;
int g_nThreads = 2;
std::atomic<bool> g_abort{false};
std::atomic<bool> g_backendInited{false};

std::string fromJString(JNIEnv *env, jstring js) {
    if (!js) return {};
    const char *chars = env->GetStringUTFChars(js, nullptr);
    if (!chars) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(js, chars);
    return out;
}

void ensureBackend() {
    if (!g_backendInited.exchange(true)) {
        llama_backend_init();
    }
}

void freeContextAndModelLocked() {
    if (g_batch) {
        llama_batch_ext_free(g_batch);
        g_batch = nullptr;
    }
    if (g_ctx) {
        llama_free(g_ctx);
        g_ctx = nullptr;
    }
    if (g_model) {
        llama_model_free(g_model);
        g_model = nullptr;
    }
    g_kvTokens.clear();
    g_banThinkBias.clear();
    g_modelHasThinkTag = false;
    g_nVocab = 0;
    g_loadedPath.clear();
}

void batchSetTokens(llama_batch_ext *batch, const llama_token *tokens, int32_t n_tokens, llama_pos pos0, bool logitsLast) {
    llama_batch_ext_clear(batch);
    for (int32_t i = 0; i < n_tokens; ++i) {
        const int32_t idx = llama_batch_ext_add_token(batch, 0, tokens[i]);
        const llama_pos pos = pos0 + i;
        llama_batch_ext_set_pos(batch, idx, &pos);
    }
    if (logitsLast && n_tokens > 0) {
        llama_batch_ext_set_output_logits(batch, n_tokens - 1, true);
    }
}

void collectThinkSuppression(const llama_model *model) {
    g_banThinkBias.clear();
    const llama_vocab *vocab = llama_model_get_vocab(model);
    g_nVocab = llama_vocab_n_tokens(vocab);
    const char *tmpl = llama_model_chat_template(model, nullptr);
    g_modelHasThinkTag = (tmpl && (std::strstr(tmpl, "think") != nullptr || std::strstr(tmpl, "thought") != nullptr));

    char buf[128];
    for (int32_t id = 0; id < g_nVocab; ++id) {
        int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n <= 0) continue;
        std::string piece(buf, (size_t)n);
        if (piece.find("<think>") != std::string::npos ||
            piece.find("</think>") != std::string::npos ||
            piece.find("<thought>") != std::string::npos ||
            piece.find("<|thought|>") != std::string::npos ||
            piece.find("<|begin_of_thought|>") != std::string::npos) {
            g_banThinkBias.push_back({(llama_token)id, -1e9f});
        }
    }
    LOGI("Think suppression: templateHasThink=%d, bannedTokens=%zu",
         g_modelHasThinkTag ? 1 : 0, g_banThinkBias.size());
}

std::string formatChatPrompt(const llama_model *model, const std::vector<std::string> &roles, const std::vector<std::string> &contents) {
    std::vector<llama_chat_message> msgs;
    msgs.reserve(roles.size());
    size_t totalChars = 0;
    for (size_t i = 0; i < roles.size(); ++i) {
        msgs.push_back({roles[i].c_str(), contents[i].c_str()});
        totalChars += roles[i].size() + contents[i].size() + 32;
    }
    std::string prompt;
    const char *tmpl = llama_model_chat_template(model, nullptr);
    if (tmpl && *tmpl) {
        std::vector<char> buf(std::max<size_t>(2048, totalChars * 2 + 512));
        int32_t n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), (int32_t)buf.size());
        if (n > (int32_t)buf.size()) {
            buf.resize((size_t)n + 16);
            n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), (int32_t)buf.size());
        }
        if (n > 0) {
            prompt.assign(buf.data(), (size_t)n);
        }
    }
    if (prompt.empty()) {
        prompt.reserve(totalChars + 64);
        for (size_t i = 0; i < roles.size(); ++i) {
            prompt += "<|im_start|>";
            prompt += roles[i];
            prompt += "\n";
            prompt += contents[i];
            prompt += "<|im_end|>\n";
        }
        prompt += "<|im_start|>assistant\n";
    }
    // Disable thinking prefill if template expects thinking tags
    if (g_modelHasThinkTag && prompt.find("<think>") == std::string::npos) {
        prompt += "<think>\n</think>\n";
    }
    return prompt;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeLoadModel(
    JNIEnv *env, jclass, jstring jPath, jint nCtx, jint nThreads) {
    std::lock_guard<std::mutex> lk(g_mutex);
    ensureBackend();
    freeContextAndModelLocked();

    std::string path = fromJString(env, jPath);
    if (path.empty()) return env->NewStringUTF("ERROR: Empty model path");

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;

    llama_model *m = llama_model_load_from_file(path.c_str(), mparams);
    if (!m) {
        return env->NewStringUTF(("ERROR: Failed to load GGUF model at " + path).c_str());
    }

    int ctxSz = std::max(512, std::min(8192, (int)nCtx));
    int thr = std::max(1, std::min(8, (int)nThreads));
    int hwCores = (int)std::thread::hardware_concurrency();
    int batchThr = std::max(thr, std::min(6, hwCores > 0 ? hwCores : thr));
    const int nBatch = 512;

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t)ctxSz;
    cparams.n_batch = (uint32_t)nBatch;
    cparams.n_ubatch = (uint32_t)nBatch;
    cparams.n_threads = thr;
    cparams.n_threads_batch = batchThr;
    cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
    cparams.type_k = GGML_TYPE_Q8_0;
    cparams.type_v = GGML_TYPE_Q8_0;
    cparams.no_perf = true;

    llama_context *ctx = llama_init_from_model(m, cparams);
    if (!ctx) {
        llama_model_free(m);
        return env->NewStringUTF("ERROR: Failed to allocate context (try smaller Ctx or model)");
    }

    llama_batch_ext *batch = llama_batch_ext_init(ctx);
    if (!batch) {
        llama_free(ctx);
        llama_model_free(m);
        return env->NewStringUTF("ERROR: Failed to allocate batch");
    }

    g_model = m;
    g_ctx = ctx;
    g_batch = batch;
    g_loadedPath = path;
    g_nCtx = ctxSz;
    g_nThreads = thr;
    collectThinkSuppression(m);

    char desc[256]{};
    llama_model_desc(m, desc, sizeof(desc));
    uint64_t params = llama_model_n_params(m);
    double bParams = params / 1e9;
    char info[512];
    snprintf(info, sizeof(info), "OK:%s (%.2fB, ctx=%d, threads=%d, no-think)", desc, bParams, g_nCtx, g_nThreads);
    LOGI("Loaded model: %s", info);
    return env->NewStringUTF(info);
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeUnloadModel(JNIEnv *, jclass) {
    g_abort.store(true);
    std::lock_guard<std::mutex> lk(g_mutex);
    freeContextAndModelLocked();
}

extern "C" JNIEXPORT void JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeStopGeneration(JNIEnv *, jclass) {
    g_abort.store(true);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeIsLoaded(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return (g_model != nullptr && g_ctx != nullptr) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_obsidiancore_studio_LlamaEngine_nativeGenerate(
    JNIEnv *env, jclass, jobjectArray jRoles, jobjectArray jContents,
    jint maxTokens, jfloat temp, jint coolDelayMs, jobject jCallback) {

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_model || !g_ctx || !g_batch) return env->NewStringUTF("ERROR: No model loaded");
    g_abort.store(false);

    jsize msgCount = env->GetArrayLength(jRoles);
    std::vector<std::string> roles(msgCount), contents(msgCount);
    for (jsize i = 0; i < msgCount; ++i) {
        auto r = (jstring)env->GetObjectArrayElement(jRoles, i);
        auto c = (jstring)env->GetObjectArrayElement(jContents, i);
        roles[i] = fromJString(env, r);
        contents[i] = fromJString(env, c);
        if (r) env->DeleteLocalRef(r);
        if (c) env->DeleteLocalRef(c);
    }

    std::string prompt = formatChatPrompt(g_model, roles, contents);
    const llama_vocab *vocab = llama_model_get_vocab(g_model);

    int nPrompt = -llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(), nullptr, 0, true, true);
    if (nPrompt <= 0) return env->NewStringUTF("ERROR: Tokenization failed");
    std::vector<llama_token> tokens((size_t)nPrompt);
    if (llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(), tokens.data(), (int32_t)tokens.size(), true, true) < 0) {
        return env->NewStringUTF("ERROR: Tokenization failed");
    }

    int reserveGen = std::max(96, std::min((int)maxTokens, g_nCtx / 2));
    if ((int)tokens.size() > g_nCtx - reserveGen) {
        int keepHead = std::min<int>(160, (int)tokens.size());
        int keepTail = std::max(64, g_nCtx - reserveGen - keepHead);
        std::vector<llama_token> trimmed;
        trimmed.reserve((size_t)(keepHead + keepTail));
        trimmed.insert(trimmed.end(), tokens.begin(), tokens.begin() + keepHead);
        trimmed.insert(trimmed.end(), tokens.end() - std::min<int>(keepTail, (int)tokens.size() - keepHead), tokens.end());
        tokens.swap(trimmed);
    }

    // Incremental KV-cache reuse: find longest common prefix with cached tokens
    int lcp = 0;
    int maxCommon = std::min((int)g_kvTokens.size(), (int)tokens.size());
    while (lcp < maxCommon && g_kvTokens[lcp] == tokens[lcp]) {
        lcp++;
    }
    if (lcp == (int)tokens.size() && lcp > 0) {
        lcp--; // Keep at least 1 token to evaluate logits
    }

    if (lcp < (int)g_kvTokens.size()) {
        if (lcp == 0) {
            llama_memory_clear(llama_get_memory(g_ctx), true);
        } else {
            llama_memory_seq_rm(llama_get_memory(g_ctx), 0, lcp, -1);
        }
        g_kvTokens.resize((size_t)lcp);
    }

    auto sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
    llama_sampler *smpl = llama_sampler_chain_init(sparams);
    if (!g_banThinkBias.empty()) {
        llama_sampler_chain_add(smpl, llama_sampler_init_logit_bias(
            g_nVocab, (int32_t)g_banThinkBias.size(), g_banThinkBias.data()));
    }
    if (temp <= 0.02f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(32));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.90f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.06f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    }

    jclass cbCls = jCallback ? env->GetObjectClass(jCallback) : nullptr;
    jmethodID cbTokMid = cbCls ? env->GetMethodID(cbCls, "onToken", "(Ljava/lang/String;)V") : nullptr;
    jmethodID cbProgMid = cbCls ? env->GetMethodID(cbCls, "onPrefillProgress", "(II)V") : nullptr;

    const int nBatch = 512;
    const int nPromptTokens = (int)tokens.size();
    const int toProcess = nPromptTokens - lcp;
    int processed = 0;
    int nPos = lcp;
    bool failed = false;

    for (int off = lcp; off < nPromptTokens; off += nBatch) {
        if (g_abort.load()) break;
        int chunk = std::min(nBatch, nPromptTokens - off);
        bool isLastChunk = (off + chunk == nPromptTokens);
        batchSetTokens(g_batch, tokens.data() + off, chunk, nPos, isLastChunk);
        if (llama_process(g_ctx, LLAMA_PROCESS_TYPE_DECODE, g_batch) != 0) {
            failed = true;
            break;
        }
        g_kvTokens.insert(g_kvTokens.end(), tokens.begin() + off, tokens.begin() + off + chunk);
        nPos += chunk;
        processed += chunk;
        if (jCallback && cbProgMid && toProcess > nBatch) {
            env->CallVoidMethod(jCallback, cbProgMid, processed, toProcess);
        }
        if (coolDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(15, (int)coolDelayMs)));
        }
    }

    std::string generated;
    bool suppressingThink = false;
    bool firstVisibleSignaled = false;
    int thinkTokensSkipped = 0;
    const int limit = std::min((int)maxTokens, std::max(64, g_nCtx - nPos));

    for (int step = 0; !failed && !g_abort.load() && step < limit; ++step) {
        llama_token tok = llama_sampler_sample(smpl, g_ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;

        char piece[256];
        int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
        if (n > 0) {
            generated.append(piece, (size_t)n);

            // Safety fallback: if a model still emitted "<think>" split across tokens, fast-skip or suppress visible output
            if (!suppressingThink && generated.size() <= 32 && generated.find("<think>") != std::string::npos) {
                suppressingThink = true;
            }
            if (suppressingThink) {
                thinkTokensSkipped++;
                size_t closePos = generated.find("</think>");
                if (closePos != std::string::npos) {
                    suppressingThink = false;
                    generated = generated.substr(closePos + 8);
                } else if (thinkTokensSkipped >= 48) {
                    // Cut off runaway thinking early
                    break;
                }
            }

            if (!suppressingThink && !firstVisibleSignaled && jCallback && cbTokMid) {
                firstVisibleSignaled = true;
                jstring jFirst = env->NewStringUTF("");
                env->CallVoidMethod(jCallback, cbTokMid, jFirst);
                env->DeleteLocalRef(jFirst);
            }
        }

        batchSetTokens(g_batch, &tok, 1, nPos, true);
        if (llama_process(g_ctx, LLAMA_PROCESS_TYPE_DECODE, g_batch) != 0) {
            failed = true;
            break;
        }
        g_kvTokens.push_back(tok);
        nPos += 1;

        if (coolDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(15, (int)coolDelayMs)));
        }
    }

    if (failed) {
        g_kvTokens.clear();
        llama_memory_clear(llama_get_memory(g_ctx), true);
    }

    if (cbCls) env->DeleteLocalRef(cbCls);
    llama_sampler_free(smpl);

    if (failed) return env->NewStringUTF("ERROR: Decode failed during generation");
    return env->NewStringUTF(generated.c_str());
}
